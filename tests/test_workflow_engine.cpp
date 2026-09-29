// M4-07 工作流真引擎独立验证（DEC-013 执行模型，Independent-Verification-Agent）。
//
// 被测面：src/workflow/engine.hpp/.cpp（rin::createWorkflowEngine /
// WorkflowEngineConfig / WorkflowFrameSource / WorkflowFrameInput）实现的
// IWorkflowEngine 公开契约（include/rin/workflow_engine.hpp），以及真引擎特有语义：
// - 工厂校验：frameSource 为空、maxInFlight==0、pumpInterval<=0 抛
//   std::invalid_argument；paramQueueCapacity==0 按 1 处理（构造不抛）；
// - 默认目录单一事实源：createWorkflowEngine 引擎的 catalog() 与
//   makeDefaultFakeCatalog() 逐字段相等（typeId/displayName/inputs/outputs 与
//   每个 ParamDescriptor 的 id/label/kind/defaultValue/hasRange/min/max/enumOptions）；
// - 真实算子数值链：source→grayify 的 BT.601 定点亮度
//   Y=(77R+150G+29B+128)>>8 逐像素 golden（独立手推，设计文档 §7 冻结公式）；
// - 参数"下一帧生效"（真实算子）：downscale scale 0.5→1.0 热更后输出
//   64×48→128×96（有界轮询）；
// - EXEC-07 过载显式丢弃：maxInFlight=1 + 20ms 慢算子包装 + 2ms 泵 →
//   droppedFrames>0 且 processedFrames 仍增长且 inFlight 不越上界；
// - 帧源空闲：帧源门控关闭 → 静默窗内 processedFrames==0 且保持 Running；
//   打开后恢复推进（无新帧的 tick 为廉价探测，不提交）；
// - Running 图替换消费式语义（假引擎 peek 缺陷探针）：GraphApplied 仅发布一次，
//   其后 ParamUpdated 在 ≥300ms 静默窗内保持为最新事件（逐帧重发 GraphApplied
//   会翻转最新事件）；统计持续推进且参数真实生效（floor(128×0.9)=115、
//   floor(96×0.9)=86）；
// - sourceSequence 传播：同一帧的源/下游产物 sourceSequence 相等（发布按帧
//   提交序有序化），运行期观察非递减；
// - 运行期算子失败归因：crop 越界 ROI（x=1000，声明范围 [0,4096] 内合法、
//   运行期 x+width>帧宽 invalid_argument）→ Failed + NodeFailed(node==2) +
//   lastError 非空（最新态事件通道经多会话采样，与共用套件同纪律）；
// - 关停竞态防御：Running 中 executor.shutdown(false) 后 stop() 有界收敛 Idle；
//   owner 纪律正常路径 stop+reset 后 shutdown(true) Completed；
// - 会话复位：stop→start 后 processedFrames 从小值重新增长而统计通道 sequence
//   实例内单调不复位；
// - 参数预编译校验：使图不可构建的修改（fft_bandpass lowCut ≥ highCut）受理时
//   同步拒绝，合法组合受理；
// - 源节点执行记账回归（2026-09-29 复验修复项）：注入型源节点计入逐节点统计
//   （executedFrames 随帧递增），且不伪造耗时值（lastCostMs/avgCostMs 保持 0）；
// - 帧源返回无效图像按"无输入"处理（不进入执行、不计丢弃、保持 Running）；
// - 待生效图新源节点尚无捕获输入：不缺失输入执行（不 Failed）、不计丢弃、
//   新图不产出、恢复供帧后正常执行。GraphApplied 相对输入可用的时序与旧图
//   是否继续运行为观察项（printf，不作断言）——DEC-013 §1.3 字面为"推迟换代
//   （图保持待生效，不发布事件）"，实现为立即换代 + 覆盖检查推迟提交（见报告）。
//
// 契约面（引擎无关）由 tests/workflow_engine_contract_suite.hpp 共用套件覆盖
// （本文件先以真引擎 fixture 运行它）；DOD-02 适用性说明见套件文件头。所有等待
// 均为有界轮询（rin_test::pollUntil 默认 5s 死限、静默窗 300ms），不悬挂；
// 套件 + 特有检查整体设计 < 60s。
#include "test_util.hpp"

#include <executor/executor.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "engine.hpp"
#include "fake_engine.hpp"
#include "rin/workflow_types.hpp"
#include "workflow_engine_contract_suite.hpp"

namespace {

using executor::ShutdownResult;
using rin::IImageNode;
using rin::IWorkflowEngine;
using rin::ImageU8;
using rin::NodeCatalog;
using rin::NodeDescriptor;
using rin::NodeId;
using rin::NodeOutputSnapshot;
using rin::ParamAssignment;
using rin::ParamDescriptor;
using rin::ParamValue;
using rin::WorkflowEngineConfig;
using rin::WorkflowEngineState;
using rin::WorkflowEvent;
using rin::WorkflowEventKind;
using rin::WorkflowFrameInput;
using rin::WorkflowGraph;
using rin::WorkflowStats;

/// 标准 fixture 帧尺寸（≥64×64：标准链式图消费节点 crop 默认 ROI (0,0,64,64)）。
constexpr std::uint32_t kFixtureWidth = 128;
constexpr std::uint32_t kFixtureHeight = 96;

/// 确定性 128×96 Rgba8 图案（共享不可变缓冲；测试断言不依赖具体像素值）。
const std::shared_ptr<const std::vector<std::uint8_t>>& fixturePatternPixels() {
    static const std::shared_ptr<const std::vector<std::uint8_t>> pixels = [] {
        auto buffer = std::make_shared<std::vector<std::uint8_t>>(
            static_cast<std::size_t>(kFixtureWidth) * kFixtureHeight * 4);
        for (std::uint32_t y = 0; y < kFixtureHeight; ++y) {
            for (std::uint32_t x = 0; x < kFixtureWidth; ++x) {
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * kFixtureWidth + x) * 4;
                (*buffer)[offset + 0] = static_cast<std::uint8_t>((x * 2 + y) & 0xFF);
                (*buffer)[offset + 1] = static_cast<std::uint8_t>((x + y * 3) & 0xFF);
                (*buffer)[offset + 2] = static_cast<std::uint8_t>((x * 5 + y * 7) & 0xFF);
                (*buffer)[offset + 3] = 0xFF;
            }
        }
        return buffer;
    }();
    return pixels;
}

/// 标准 fixture 帧（每次调用包一层零拷贝视图）。
ImageU8 fixtureFrame() {
    return ImageU8::wrap(rin::PortType::Rgba8, kFixtureWidth, kFixtureHeight,
                         kFixtureWidth * 4, fixturePatternPixels());
}

/// 共用基线配置：2ms 帧泵 + maxInFlight=2，其余默认。
WorkflowEngineConfig baseConfig() {
    WorkflowEngineConfig config;
    config.pumpInterval = std::chrono::milliseconds{2};
    config.maxInFlight = 2;
    return config;
}

/// 按需生成帧源（fixture 标准语义）：引擎侧已消费序号 +1，固定图案帧。
/// 无状态（每源节点序号由引擎传入的 lastSeen 驱动），可被多引擎实例共享。
rin::WorkflowFrameSource alwaysSupplySource() {
    return [](NodeId, std::uint64_t& lastSeen, WorkflowFrameInput& out) {
        out.sourceSequence = lastSeen + 1;
        out.image = fixtureFrame();
        lastSeen = out.sourceSequence;
        return true;
    };
}

/// 可门控帧源（帧源空闲 / 发布有序化检查用）：open=false 时按"无新帧"处理。
struct GatedSource {
    std::shared_ptr<std::atomic<bool>> open{std::make_shared<std::atomic<bool>>(false)};
    rin::WorkflowFrameSource fn;

    GatedSource() {
        fn = [open = open](NodeId, std::uint64_t& lastSeen, WorkflowFrameInput& out) {
            if (!open->load(std::memory_order_relaxed)) {
                return false;
            }
            out.sourceSequence = lastSeen + 1;
            out.image = fixtureFrame();
            lastSeen = out.sourceSequence;
            return true;
        };
    }
};

/// source(sourceId) -> consumerType(consumerId) 标准链（末端悬空输出）。
WorkflowGraph makeChainIds(NodeId sourceId, NodeId consumerId,
                           const std::string& consumerType,
                           std::vector<ParamAssignment> consumerParams = {}) {
    WorkflowGraph graph;
    rin::NodeInstance source;
    source.id = sourceId;
    source.typeId = "source";
    rin::NodeInstance consumer;
    consumer.id = consumerId;
    consumer.typeId = consumerType;
    consumer.params = std::move(consumerParams);
    graph.nodes = {source, consumer};
    rin::Connection connection;
    connection.from = rin::PortRef{sourceId, rin::PortDirection::Output, 0};
    connection.to = rin::PortRef{consumerId, rin::PortDirection::Input, 0};
    graph.connections = {connection};
    return graph;
}

/// source(1) -> consumerType(2) 标准链。
WorkflowGraph makeChain(const std::string& consumerType,
                        std::vector<ParamAssignment> consumerParams = {}) {
    return makeChainIds(1, 2, consumerType, std::move(consumerParams));
}

/// 计数故障包装节点：全局第 failOnApply 次 apply（跨代/跨会话连续）抛
/// std::runtime_error，其余转发内部真实节点实例（套件 makeFailing 注入语义）。
class CountingFailureNode final : public IImageNode {
public:
    CountingFailureNode(std::unique_ptr<IImageNode> inner,
                        std::shared_ptr<std::atomic<std::uint64_t>> counter,
                        std::uint64_t failOnApply)
        : inner_(std::move(inner)), counter_(std::move(counter)), failOnApply_(failOnApply) {}

    [[nodiscard]] const NodeDescriptor& descriptor() const noexcept override {
        return inner_->descriptor();
    }

    [[nodiscard]] std::vector<ImageU8> apply(
        const std::vector<ImageU8>& inputs) const override {
        const std::uint64_t n = counter_->fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == failOnApply_) {
            throw std::runtime_error("injected crop failure at global apply #" +
                                     std::to_string(n));
        }
        return inner_->apply(inputs);
    }

private:
    std::unique_ptr<IImageNode> inner_;
    std::shared_ptr<std::atomic<std::uint64_t>> counter_;
    std::uint64_t failOnApply_;
};

/// 慢算子包装节点：apply 内固定睡眠后转发（过载显式丢弃检查用）。
class SlowNode final : public IImageNode {
public:
    SlowNode(std::unique_ptr<IImageNode> inner, std::chrono::milliseconds delay)
        : inner_(std::move(inner)), delay_(delay) {}

    [[nodiscard]] const NodeDescriptor& descriptor() const noexcept override {
        return inner_->descriptor();
    }

    [[nodiscard]] std::vector<ImageU8> apply(
        const std::vector<ImageU8>& inputs) const override {
        std::this_thread::sleep_for(delay_);
        return inner_->apply(inputs);
    }

private:
    std::unique_ptr<IImageNode> inner_;
    std::chrono::milliseconds delay_;
};

/// 契约套件 fixture：标准引擎 + 跨代/跨会话计数故障注入（消费节点 crop）。
rin_test::WorkflowEngineFixture makeFixture() {
    rin_test::WorkflowEngineFixture fixture;
    fixture.make = [](executor::Executor& executor) -> std::shared_ptr<IWorkflowEngine> {
        WorkflowEngineConfig config = baseConfig();
        config.frameSource = alwaysSupplySource();
        return rin::createWorkflowEngine(executor, std::move(config));
    };
    fixture.makeFailing =
        [](executor::Executor& executor, NodeId failNode,
           std::uint64_t failOnFrame) -> std::shared_ptr<IWorkflowEngine> {
        WorkflowEngineConfig config = baseConfig();
        // 计数器在本次 makeFailing 调用内创建并被工厂闭包捕获：同一引擎实例
        // 跨代（图/参数重建）/跨会话（stop→start）连续，注入恰好触发一次。
        auto counter = std::make_shared<std::atomic<std::uint64_t>>(0);
        config.frameSource = alwaysSupplySource();
        config.nodeFactory =
            [counter, failNode, failOnFrame](const NodeDescriptor& descriptor,
                                             const rin::NodeInstance& instance)
            -> std::unique_ptr<IImageNode> {
            if (instance.typeId == "source") {
                return nullptr;  // 注入型源节点语义（M4-07 源节点）。
            }
            std::unique_ptr<IImageNode> inner =
                rin::makeDefaultImageNode(descriptor, instance);
            if (instance.id == failNode && instance.typeId == "crop") {
                inner = std::make_unique<CountingFailureNode>(std::move(inner), counter,
                                                              failOnFrame);
            }
            return inner;
        };
        return rin::createWorkflowEngine(executor, std::move(config));
    };
    return fixture;
}

}  // namespace

int main() {
    // ---- 1) 共用契约套件（真引擎 fixture：128×96 帧、2ms 泵、maxInFlight=2）----
    {
        rin_test::runWorkflowEngineContractChecks(makeFixture());
    }

    // ---- 2) 工厂校验：非法配置抛 std::invalid_argument；paramQueueCapacity=0 按 1 处理 ----
    {
        executor::Executor executor;
        executor::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        const auto rejectsInvalid = [&executor](WorkflowEngineConfig bad) {
            try {
                (void)rin::createWorkflowEngine(executor, std::move(bad));  // 预期在此抛出。
            } catch (const std::invalid_argument&) {
                return true;
            } catch (...) {
                return false;
            }
            return false;
        };
        RIN_CHECK(rejectsInvalid(baseConfig()));  // frameSource 为空。
        {
            WorkflowEngineConfig zeroInFlight = baseConfig();
            zeroInFlight.frameSource = alwaysSupplySource();
            zeroInFlight.maxInFlight = 0;
            RIN_CHECK(rejectsInvalid(std::move(zeroInFlight)));
        }
        {
            WorkflowEngineConfig zeroPump = baseConfig();
            zeroPump.frameSource = alwaysSupplySource();
            zeroPump.pumpInterval = std::chrono::milliseconds{0};
            RIN_CHECK(rejectsInvalid(std::move(zeroPump)));
        }
        {
            WorkflowEngineConfig negativePump = baseConfig();
            negativePump.frameSource = alwaysSupplySource();
            negativePump.pumpInterval = std::chrono::milliseconds{-5};
            RIN_CHECK(rejectsInvalid(std::move(negativePump)));
        }
        // paramQueueCapacity == 0 按 1 处理：构造不抛（文档化行为）。
        bool zeroQueueConstructed = false;
        try {
            WorkflowEngineConfig zeroQueue = baseConfig();
            zeroQueue.frameSource = alwaysSupplySource();
            zeroQueue.paramQueueCapacity = 0;
            std::shared_ptr<IWorkflowEngine> engine =
                rin::createWorkflowEngine(executor, std::move(zeroQueue));
            zeroQueueConstructed = static_cast<bool>(engine);
        } catch (...) {
            zeroQueueConstructed = false;
        }
        RIN_CHECK(zeroQueueConstructed);
        RIN_CHECK(executor.shutdown(true) == ShutdownResult::Completed);
    }

    // ---- 3) 默认目录一致性：真引擎 catalog() 与 makeDefaultFakeCatalog() 逐字段相等 ----
    {
        executor::Executor executor;
        executor::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        WorkflowEngineConfig config = baseConfig();
        config.frameSource = alwaysSupplySource();
        std::shared_ptr<IWorkflowEngine> engine =
            rin::createWorkflowEngine(executor, std::move(config));
        RIN_CHECK(static_cast<bool>(engine));
        if (engine) {
            const NodeCatalog& real = engine->catalog();
            const NodeCatalog fake = rin::makeDefaultFakeCatalog();
            RIN_CHECK(real.valid());
            RIN_CHECK(fake.valid());
            RIN_CHECK_EQ(real.nodes.size(), fake.nodes.size());
            for (const NodeDescriptor& realNode : real.nodes) {
                const NodeDescriptor* fakeNode =
                    rin::findNodeDescriptor(fake, realNode.typeId);
                RIN_CHECK_MSG(fakeNode != nullptr,
                              ("fake catalog must contain typeId: " + realNode.typeId)
                                  .c_str());
                if (fakeNode == nullptr) {
                    continue;
                }
                RIN_CHECK_EQ(realNode.typeId, fakeNode->typeId);
                RIN_CHECK_EQ(realNode.displayName, fakeNode->displayName);
                RIN_CHECK(realNode.inputs == fakeNode->inputs);
                RIN_CHECK(realNode.outputs == fakeNode->outputs);
                RIN_CHECK_EQ(realNode.params.size(), fakeNode->params.size());
                for (const ParamDescriptor& realParam : realNode.params) {
                    const ParamDescriptor* fakeParam = nullptr;
                    for (const ParamDescriptor& candidate : fakeNode->params) {
                        if (candidate.id == realParam.id) {
                            fakeParam = &candidate;
                            break;
                        }
                    }
                    RIN_CHECK_MSG(fakeParam != nullptr,
                                  ("fake catalog must contain param: " + realParam.id)
                                      .c_str());
                    if (fakeParam == nullptr) {
                        continue;
                    }
                    RIN_CHECK_EQ(realParam.id, fakeParam->id);
                    RIN_CHECK_EQ(realParam.label, fakeParam->label);
                    RIN_CHECK(realParam.kind == fakeParam->kind);
                    RIN_CHECK(realParam.defaultValue == fakeParam->defaultValue);
                    RIN_CHECK_EQ(realParam.hasRange, fakeParam->hasRange);
                    RIN_CHECK_EQ(realParam.minValue, fakeParam->minValue);
                    RIN_CHECK_EQ(realParam.maxValue, fakeParam->maxValue);
                    RIN_CHECK(realParam.enumOptions == fakeParam->enumOptions);
                }
            }
        }
        engine.reset();
        RIN_CHECK(executor.shutdown(true) == ShutdownResult::Completed);
    }

    // ---- 4) 真实算子数值链：source(1)→grayify(2)，BT.601 定点亮度逐像素 golden ----
    // 已知 4×2 Rgba8 输入（行主序，alpha 不参与）：
    //   行 0：红(255,0,0) 绿(0,255,0) 蓝(0,0,255) 白(255,255,255)
    //   行 1：灰(128,128,128) (10,20,30) (1,2,3) (200,100,50)
    // Y=(77R+150G+29B+128)>>8 独立手推：
    //   红   (77·255+128)>>8   = 19763>>8  = 77
    //   绿   (150·255+128)>>8  = 38378>>8  = 149
    //   蓝   (29·255+128)>>8   = 7523>>8   = 29
    //   白   (256·255+128)>>8  = 65408>>8  = 255
    //   灰   (256·128+128)>>8  = 32896>>8  = 128
    //   10/20/30 (770+3000+870+128)>>8 = 4768>>8 = 18
    //   1/2/3   (77+300+87+128)>>8    = 592>>8  = 2
    //   200/100/50 (15400+15000+1450+128)>>8 = 31978>>8 = 124
    {
        executor::Executor executor;
        executor::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        auto knownPixels = std::make_shared<const std::vector<std::uint8_t>>(
            std::vector<std::uint8_t>{255, 0, 0, 255,    0, 255, 0, 255,
                                      0, 0, 255, 255,    255, 255, 255, 255,
                                      128, 128, 128, 255, 10, 20, 30, 255,
                                      1, 2, 3, 255,      200, 100, 50, 255});
        WorkflowEngineConfig config = baseConfig();
        config.frameSource = [knownPixels](NodeId, std::uint64_t& lastSeen,
                                           WorkflowFrameInput& out) {
            out.sourceSequence = lastSeen + 1;
            out.image = ImageU8::wrap(rin::PortType::Rgba8, 4, 2, 16, knownPixels);
            lastSeen = out.sourceSequence;
            return true;
        };
        std::shared_ptr<IWorkflowEngine> engine =
            rin::createWorkflowEngine(executor, std::move(config));
        RIN_CHECK(engine->applyGraph(makeChain("grayify")).ok);
        RIN_CHECK(engine->start().admitted);

        std::uint64_t seen = 0;
        NodeOutputSnapshot out;
        RIN_CHECK(rin_test::pollUntil([&] {
            return engine->tryLoadNodeOutput(2, seen, out) && out.valid();
        }));
        RIN_CHECK(out.format == rin::PortType::Gray8);
        RIN_CHECK_EQ(out.width, 4u);
        RIN_CHECK_EQ(out.height, 2u);
        RIN_CHECK_EQ(out.stride, 4u);  // 紧凑缓冲：stride = 宽 × elementSize。
        static constexpr std::uint8_t kGoldenGray[8] = {77, 149, 29, 255, 128, 18, 2, 124};
        RIN_CHECK(static_cast<bool>(out.pixels) && out.pixels->size() == 8);
        if (out.pixels && out.pixels->size() == 8) {
            for (std::size_t i = 0; i < 8; ++i) {
                RIN_CHECK_EQ(out.pixels->at(i), kGoldenGray[i]);
            }
        }

        // 源节点执行记账回归检查（2026-09-29 复验修复项）：注入型源节点计入
        // 逐节点统计（executedFrames 随帧递增），且不伪造耗时值（无 apply 可测，
        // lastCostMs/avgCostMs 保持 0）——与共用套件"统计覆盖全部图节点"基线一致。
        {
            std::uint64_t statsSeen = 0;
            WorkflowStats stats;
            RIN_CHECK_MSG(rin_test::pollUntil([&] {
                return engine->tryLoadStats(statsSeen, stats) &&
                       rin_test::findNodeStats(stats, 1) != nullptr &&
                       rin_test::findNodeStats(stats, 2) != nullptr &&
                       rin_test::findNodeStats(stats, 1)->executedFrames >= 1 &&
                       rin_test::findNodeStats(stats, 2)->executedFrames >= 1;
            }), "injected source node must be counted in per-node stats");
            const std::uint64_t sourceExecutedBaseline =
                rin_test::findNodeStats(stats, 1)->executedFrames;
            RIN_CHECK_MSG(rin_test::pollUntil([&] {
                return engine->tryLoadStats(statsSeen, stats) &&
                       rin_test::findNodeStats(stats, 1) != nullptr &&
                       rin_test::findNodeStats(stats, 1)->executedFrames >
                           sourceExecutedBaseline;
            }), "source node executedFrames must increase with frames");
            RIN_CHECK_MSG(rin_test::findNodeStats(stats, 1)->lastCostMs == 0.0 &&
                              rin_test::findNodeStats(stats, 1)->avgCostMs == 0.0,
                          "source node stats must not carry fabricated cost values");
        }

        engine->stop();
        engine.reset();
        RIN_CHECK(executor.shutdown(true) == ShutdownResult::Completed);
    }

    // ---- 5) 参数"下一帧生效"（真实算子）：downscale scale 0.5→1.0，64×48→128×96 ----
    {
        executor::Executor executor;
        executor::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        WorkflowEngineConfig config = baseConfig();
        config.frameSource = alwaysSupplySource();
        std::shared_ptr<IWorkflowEngine> engine =
            rin::createWorkflowEngine(executor, std::move(config));
        RIN_CHECK(engine->applyGraph(makeChain("downscale")).ok);
        RIN_CHECK(engine->start().admitted);

        std::uint64_t seen = 0;
        NodeOutputSnapshot out;
        RIN_CHECK(rin_test::pollUntil([&] {
            return engine->tryLoadNodeOutput(2, seen, out) && out.valid() &&
                   out.width == 64 && out.height == 48;  // 默认 scale=0.5。
        }));

        std::string error;
        RIN_CHECK(engine->requestParamUpdate(2, "scale", ParamValue{1.0}, &error));
        RIN_CHECK(error.empty());
        RIN_CHECK(rin_test::pollUntil([&] {
            return engine->tryLoadNodeOutput(2, seen, out) && out.valid() &&
                   out.width == 128 && out.height == 96;  // 下一帧生效。
        }));

        engine->stop();
        engine.reset();
        RIN_CHECK(executor.shutdown(true) == ShutdownResult::Completed);
    }

    // ---- 6) 过载显式丢弃：maxInFlight=1 + 20ms 慢算子 + 2ms 泵 ----
    {
        executor::Executor executor;
        executor::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        WorkflowEngineConfig config = baseConfig();
        config.maxInFlight = 1;
        config.frameSource = alwaysSupplySource();
        config.nodeFactory = [](const NodeDescriptor& descriptor,
                                const rin::NodeInstance& instance)
            -> std::unique_ptr<IImageNode> {
            if (instance.typeId == "source") {
                return nullptr;
            }
            std::unique_ptr<IImageNode> inner =
                rin::makeDefaultImageNode(descriptor, instance);
            if (instance.typeId == "downscale") {
                inner = std::make_unique<SlowNode>(std::move(inner),
                                                   std::chrono::milliseconds{20});
            }
            return inner;
        };
        std::shared_ptr<IWorkflowEngine> engine =
            rin::createWorkflowEngine(executor, std::move(config));
        RIN_CHECK(engine->applyGraph(makeChain("downscale")).ok);
        RIN_CHECK(engine->start().admitted);

        std::uint64_t seen = 0;
        WorkflowStats stats;
        bool inFlightBounded = true;
        RIN_CHECK(rin_test::pollUntil([&] {
            if (engine->tryLoadStats(seen, stats)) {
                if (stats.inFlight > 1) {
                    inFlightBounded = false;
                }
                return stats.droppedFrames > 0;
            }
            return false;
        }));
        RIN_CHECK(inFlightBounded);
        RIN_CHECK(stats.inFlight <= 1);  // 在飞计数不越过 maxInFlight=1 上界。

        // 丢弃发生的同时 processedFrames 仍增长（显式丢弃而非停摆/静默排队）。
        const std::uint64_t processedAtDrop = stats.processedFrames;
        RIN_CHECK(rin_test::pollUntil([&] {
            return engine->tryLoadStats(seen, stats) &&
                   stats.processedFrames > processedAtDrop;
        }));

        engine->stop();
        engine.reset();
        RIN_CHECK(executor.shutdown(true) == ShutdownResult::Completed);
    }

    // ---- 7) 帧源空闲：门控关闭 → 静默窗无帧推进且保持 Running；打开后恢复 ----
    {
        executor::Executor executor;
        executor::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        GatedSource gated;
        WorkflowEngineConfig config = baseConfig();
        config.frameSource = gated.fn;
        std::shared_ptr<IWorkflowEngine> engine =
            rin::createWorkflowEngine(executor, std::move(config));
        RIN_CHECK(engine->applyGraph(makeChain("grayify")).ok);
        RIN_CHECK(engine->start().admitted);

        bool stayedRunning = true;
        RIN_CHECK_MSG(rin_test::quietFor(
                          [&] {
                              if (engine->state() != WorkflowEngineState::Running) {
                                  stayedRunning = false;
                                  return true;  // 状态漂移即刻终止观察（断言判失败）。
                              }
                              std::uint64_t probe = 0;
                              WorkflowStats stats;
                              if (engine->tryLoadStats(probe, stats)) {
                                  return stats.processedFrames > 0;
                              }
                              return false;
                          },
                          rin_test::kContractQuietWindow),
                      "idle frame source must not produce frames");
        RIN_CHECK(stayedRunning);
        RIN_CHECK(engine->state() == WorkflowEngineState::Running);

        gated.open->store(true, std::memory_order_relaxed);
        std::uint64_t seen = 0;
        WorkflowStats stats;
        RIN_CHECK(rin_test::pollUntil([&] {
            return engine->tryLoadStats(seen, stats) && stats.processedFrames >= 3;
        }));

        engine->stop();
        engine.reset();
        RIN_CHECK(executor.shutdown(true) == ShutdownResult::Completed);
    }

    // ---- 8) Running 图替换消费式语义：GraphApplied 不逐帧重发（假引擎 peek 缺陷探针）----
    {
        executor::Executor executor;
        executor::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        WorkflowEngineConfig config = baseConfig();
        config.frameSource = alwaysSupplySource();
        std::shared_ptr<IWorkflowEngine> engine =
            rin::createWorkflowEngine(executor, std::move(config));
        RIN_CHECK(engine->applyGraph(makeChain("crop")).ok);
        RIN_CHECK(engine->start().admitted);

        WorkflowEvent event;
        RIN_CHECK(rin_test::pollUntil([&] {
            return engine->tryLoadEvent(event) &&
                   event.kind == WorkflowEventKind::Started;
        }));

        // 第二张图：source(7)→downscale(8)。
        RIN_CHECK(engine->applyGraph(makeChainIds(7, 8, "downscale")).ok);
        RIN_CHECK(rin_test::pollUntil([&] {
            return engine->tryLoadEvent(event) &&
                   event.kind == WorkflowEventKind::GraphApplied;
        }));

        // 参数命令在帧边界应用 → ParamUpdated（此时新图已发布且已消费）。
        std::string error;
        RIN_CHECK(engine->requestParamUpdate(8, "scale", ParamValue{0.9}, &error));
        RIN_CHECK(error.empty());
        RIN_CHECK(rin_test::pollUntil([&] {
            return engine->tryLoadEvent(event) &&
                   event.kind == WorkflowEventKind::ParamUpdated;
        }));

        // 缺陷探针：≥300ms 静默窗内最新事件必须保持 ParamUpdated——若引擎逐帧
        // 重建换代并重发 GraphApplied（假引擎 peek 不消费缺陷），最新事件会被
        // 翻转回 GraphApplied。
        RIN_CHECK_MSG(rin_test::quietFor(
                          [&] {
                              return engine->tryLoadEvent(event) &&
                                     event.kind == WorkflowEventKind::GraphApplied;
                          },
                          rin_test::kContractQuietWindow),
                      "GraphApplied must not be republished per frame after consumption");

        // 统计继续推进。
        std::uint64_t seen = 0;
        WorkflowStats stats;
        RIN_CHECK(rin_test::pollUntil([&] {
            return engine->tryLoadStats(seen, stats) && stats.processedFrames >= 3;
        }));
        const std::uint64_t processedAtParam = stats.processedFrames;
        RIN_CHECK(rin_test::pollUntil([&] {
            return engine->tryLoadStats(seen, stats) &&
                   stats.processedFrames > processedAtParam;
        }));

        // 参数真实生效（真实算子）：floor(128×0.9)=115、floor(96×0.9)=86。
        std::uint64_t outSeen = 0;
        NodeOutputSnapshot out;
        RIN_CHECK(rin_test::pollUntil([&] {
            return engine->tryLoadNodeOutput(8, outSeen, out) && out.valid() &&
                   out.width == 115 && out.height == 86;
        }));

        engine->stop();
        engine.reset();
        RIN_CHECK(executor.shutdown(true) == ShutdownResult::Completed);
    }

    // ---- 9) sourceSequence 传播：同帧两节点相等 + 运行期非递减 ----
    {
        executor::Executor executor;
        executor::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        GatedSource gated;
        gated.open->store(true, std::memory_order_relaxed);
        WorkflowEngineConfig config = baseConfig();
        config.frameSource = gated.fn;
        std::shared_ptr<IWorkflowEngine> engine =
            rin::createWorkflowEngine(executor, std::move(config));
        RIN_CHECK(engine->applyGraph(makeChain("crop")).ok);
        RIN_CHECK(engine->start().admitted);

        std::uint64_t srcSeen = 0;
        NodeOutputSnapshot srcOut;
        RIN_CHECK(rin_test::pollUntil([&] {
            return engine->tryLoadNodeOutput(1, srcSeen, srcOut) && srcOut.valid();
        }));

        // 运行期采样：下游（crop）产物 sourceSequence 非递减（发布按帧提交序
        // 有序化——maxInFlight=2 下帧任务乱序完成不得造成回退）。
        bool monotonic = true;
        bool haveSample = false;
        std::uint64_t previous = 0;
        for (int i = 0; i < 60; ++i) {
            std::uint64_t probeSeen = 0;
            NodeOutputSnapshot probe;
            if (engine->tryLoadNodeOutput(2, probeSeen, probe) && probe.valid()) {
                if (haveSample && probe.sourceSequence < previous) {
                    monotonic = false;
                }
                previous = probe.sourceSequence;
                haveSample = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        RIN_CHECK(haveSample);
        RIN_CHECK_MSG(monotonic, "downstream sourceSequence must be non-decreasing");

        // 关帧源 → 先等在飞帧收敛（统计序号稳定，与共用套件 phase 3 同纪律）
        // → 两节点最新产物来自同一帧 → sourceSequence 相等。
        gated.open->store(false, std::memory_order_relaxed);
        RIN_CHECK(rin_test::pollUntil([&] {
            std::uint64_t settleSeen = 0;
            WorkflowStats settle1;
            if (!engine->tryLoadStats(settleSeen, settle1)) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{8});
            std::uint64_t settleSeen2 = 0;
            WorkflowStats settle2;
            return !engine->tryLoadStats(settleSeen2, settle2) ||
                   settle2.sequence == settle1.sequence;
        }));
        std::uint64_t statsSeen = 0;
        WorkflowStats last;
        RIN_CHECK(engine->tryLoadStats(statsSeen, last));
        const std::uint64_t sequenceAtGate = last.sequence;
        RIN_CHECK(rin_test::quietFor(
            [&] {
                std::uint64_t probe = sequenceAtGate;
                WorkflowStats discarded;
                return engine->tryLoadStats(probe, discarded);
            },
            std::chrono::milliseconds{300}));
        std::uint64_t srcSeen2 = 0;
        NodeOutputSnapshot srcFinal;
        std::uint64_t dstSeen2 = 0;
        NodeOutputSnapshot dstFinal;
        const bool haveSrc = engine->tryLoadNodeOutput(1, srcSeen2, srcFinal);
        const bool haveDst = engine->tryLoadNodeOutput(2, dstSeen2, dstFinal);
        RIN_CHECK(haveSrc && srcFinal.valid());
        RIN_CHECK(haveDst && dstFinal.valid());
        if (haveSrc && haveDst) {
            RIN_CHECK_MSG(srcFinal.sourceSequence == dstFinal.sourceSequence,
                          "source and downstream outputs of the same frame must carry "
                          "equal sourceSequence");
            RIN_CHECK(dstFinal.sourceSequence >= previous);
        }

        engine->stop();
        engine.reset();
        RIN_CHECK(executor.shutdown(true) == ShutdownResult::Completed);
    }

    // ---- 10) 运行期算子失败归因：crop 越界 ROI → Failed + NodeFailed(node=2) ----
    // x=1000 在声明范围 [0,4096] 内合法（applyGraph/预编译校验均受理）；运行期
    // 1000+64 > 128 帧宽由真实 crop 算子抛 std::invalid_argument → 引擎转
    // Failed。NodeFailed 与 Failed 背靠背发布（最新态通道单会话可能只见
    // Failed），以多个独立失败会话采样捕获（与共用套件同纪律）。
    {
        constexpr int kFailureAttempts = 8;
        bool sawNodeFailed = false;
        NodeId nodeFailedId = rin::kInvalidNode;
        bool reachedFailedOnce = false;
        for (int attempt = 0; attempt < kFailureAttempts && !sawNodeFailed; ++attempt) {
            executor::Executor executor;
            executor::ExecutorConfig executorConfig;
            RIN_CHECK(executor.initialize(executorConfig));
            WorkflowEngineConfig config = baseConfig();
            config.frameSource = alwaysSupplySource();
            std::shared_ptr<IWorkflowEngine> engine =
                rin::createWorkflowEngine(executor, std::move(config));
            WorkflowGraph graph = makeChain("crop");
            for (rin::NodeInstance& instance : graph.nodes) {
                if (instance.id == 2) {
                    instance.params.push_back(
                        ParamAssignment{"x", ParamValue{static_cast<std::int64_t>(1000)}});
                }
            }
            RIN_CHECK(engine->applyGraph(graph).ok);
            RIN_CHECK(engine->start().admitted);

            bool reachedFailed = false;
            const auto deadline =
                std::chrono::steady_clock::now() + rin_test::kContractPollDeadline;
            while (std::chrono::steady_clock::now() < deadline) {
                WorkflowEvent sampled;
                if (engine->tryLoadEvent(sampled)) {
                    if (sampled.kind == WorkflowEventKind::NodeFailed) {
                        sawNodeFailed = true;
                        nodeFailedId = sampled.node;
                    }
                }
                if (engine->state() == WorkflowEngineState::Failed) {
                    reachedFailed = true;
                    break;
                }
            }
            RIN_CHECK(reachedFailed);
            reachedFailedOnce = reachedFailedOnce || reachedFailed;
            RIN_CHECK(!engine->lastError().empty());

            engine.reset();  // 析构内幂等 stop。
            RIN_CHECK(executor.shutdown(true) == ShutdownResult::Completed);
        }
        RIN_CHECK(reachedFailedOnce);
        RIN_CHECK_MSG(sawNodeFailed,
                      "NodeFailed event must surface on the event channel "
                      "(latest-wins mailbox; sampled across repeated failure sessions)");
        if (sawNodeFailed) {
            RIN_CHECK_EQ(nodeFailedId, static_cast<NodeId>(2));
        }
    }

    // ---- 11) 关停竞态防御：Running 中 executor.shutdown(false) 后 stop() 有界收敛 ----
    {
        executor::Executor executor;
        executor::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        WorkflowEngineConfig config = baseConfig();
        config.frameSource = alwaysSupplySource();
        std::shared_ptr<IWorkflowEngine> engine =
            rin::createWorkflowEngine(executor, std::move(config));
        RIN_CHECK(engine->applyGraph(makeChain("grayify")).ok);
        RIN_CHECK(engine->start().admitted);
        {
            std::uint64_t seen = 0;
            WorkflowStats stats;
            RIN_CHECK(rin_test::pollUntil([&] {
                return engine->tryLoadStats(seen, stats) && stats.processedFrames >= 3;
            }));
        }

        executor.shutdown(false);  // 对抗路径：引擎仍 Running。
        engine->stop();            // 必须有界返回，不悬挂。
        RIN_CHECK(engine->state() == WorkflowEngineState::Idle);
        engine.reset();  // 析构幂等 stop（executor 已 shutdown，不再调 shutdown）。
    }

    // ---- 12) owner 纪律正常路径收尾：stop + reset 后 shutdown(true) 干净 ----
    {
        executor::Executor executor;
        executor::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        WorkflowEngineConfig config = baseConfig();
        config.frameSource = alwaysSupplySource();
        std::shared_ptr<IWorkflowEngine> engine =
            rin::createWorkflowEngine(executor, std::move(config));
        RIN_CHECK(engine->applyGraph(makeChain("grayify")).ok);
        RIN_CHECK(engine->start().admitted);
        {
            std::uint64_t seen = 0;
            WorkflowStats stats;
            RIN_CHECK(rin_test::pollUntil([&] {
                return engine->tryLoadStats(seen, stats) && stats.processedFrames >= 2;
            }));
        }
        engine->stop();
        RIN_CHECK(engine->state() == WorkflowEngineState::Idle);
        engine.reset();
        RIN_CHECK(executor.shutdown(true) == ShutdownResult::Completed);
    }

    // ---- 13) 会话复位：processedFrames 复位从小值增长，统计通道序号实例内单调 ----
    {
        executor::Executor executor;
        executor::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        WorkflowEngineConfig config = baseConfig();
        config.frameSource = alwaysSupplySource();
        std::shared_ptr<IWorkflowEngine> engine =
            rin::createWorkflowEngine(executor, std::move(config));
        RIN_CHECK(engine->applyGraph(makeChain("grayify")).ok);
        RIN_CHECK(engine->start().admitted);

        // 会话 1：累计足够帧（余量吸收轮询器调度抖动）。
        std::uint64_t statsSeen = 0;
        WorkflowStats stats;
        RIN_CHECK(rin_test::pollUntil([&] {
            return engine->tryLoadStats(statsSeen, stats) && stats.processedFrames >= 50;
        }));
        const std::uint64_t processedSession1 = stats.processedFrames;
        const std::uint64_t sequenceSession1 = stats.sequence;

        engine->stop();
        RIN_CHECK(engine->state() == WorkflowEngineState::Idle);

        // 会话 2：processedFrames 复位从小值重新增长；统计通道序号不回退。
        RIN_CHECK(engine->start().admitted);
        std::uint64_t firstProcessedSession2 = 0;
        bool haveFirstSession2 = false;
        {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds{2000};
            while (std::chrono::steady_clock::now() < deadline) {
                if (engine->tryLoadStats(statsSeen, stats)) {
                    firstProcessedSession2 = stats.processedFrames;
                    haveFirstSession2 = true;
                    break;
                }
            }
        }
        RIN_CHECK(haveFirstSession2);
        RIN_CHECK_MSG(firstProcessedSession2 < processedSession1,
                      "session-2 processedFrames must restart below session-1 total");
        RIN_CHECK_MSG(stats.sequence > sequenceSession1,
                      "stats channel sequence must not regress across sessions");
        RIN_CHECK(rin_test::pollUntil([&] {
            return engine->tryLoadStats(statsSeen, stats) && stats.processedFrames >= 5;
        }));

        engine->stop();
        engine.reset();
        RIN_CHECK(executor.shutdown(true) == ShutdownResult::Completed);
    }

    // ---- 14) 参数预编译校验（DEC-013）：使图不可构建的修改同步拒绝 ----
    // source(1)→grayify(2)→fft_bandpass(3)：lowCut 默认 0.2、highCut 默认 0.6。
    // lowCut=0.8 落在声明范围 [0,1] 内（值校验放行），但 0.8 ≥ 0.6 使工厂期
    // 构造拒绝——受理前预编译校验必须同步拒绝（false + error），而非等帧边界
    // 重建失败。合法组合（highCut=0.9）受理。
    {
        executor::Executor executor;
        executor::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        WorkflowEngineConfig config = baseConfig();
        config.frameSource = alwaysSupplySource();
        std::shared_ptr<IWorkflowEngine> engine =
            rin::createWorkflowEngine(executor, std::move(config));
        WorkflowGraph graph;
        rin::NodeInstance source;
        source.id = 1;
        source.typeId = "source";
        rin::NodeInstance gray;
        gray.id = 2;
        gray.typeId = "grayify";
        rin::NodeInstance band;
        band.id = 3;
        band.typeId = "fft_bandpass";
        graph.nodes = {source, gray, band};
        graph.connections = {
            rin::Connection{rin::PortRef{1, rin::PortDirection::Output, 0},
                            rin::PortRef{2, rin::PortDirection::Input, 0}},
            rin::Connection{rin::PortRef{2, rin::PortDirection::Output, 0},
                            rin::PortRef{3, rin::PortDirection::Input, 0}},
        };
        RIN_CHECK(engine->applyGraph(graph).ok);

        std::string error;
        RIN_CHECK_MSG(!engine->requestParamUpdate(3, "lowCut", ParamValue{0.8}, &error),
                      "param that makes the graph unbuildable must be rejected");
        RIN_CHECK(!error.empty());
        error.clear();
        RIN_CHECK(engine->requestParamUpdate(3, "highCut", ParamValue{0.9}, &error));
        RIN_CHECK(error.empty());

        engine.reset();
        RIN_CHECK(executor.shutdown(true) == ShutdownResult::Completed);
    }

    // ---- 15) 帧源返回无效图像按"无输入"处理（不进入执行、不计丢弃）----
    // engine.hpp 契约：无效图像由帧泵按"无输入"处理，不进入执行。帧源返回
    // true 携带无效图像时：无帧推进、无过载丢弃、保持 Running；恢复有效图像
    // 后正常推进。
    {
        executor::Executor executor;
        executor::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        auto supplyValid = std::make_shared<std::atomic<bool>>(false);
        WorkflowEngineConfig config = baseConfig();
        config.frameSource = [supplyValid](NodeId, std::uint64_t& lastSeen,
                                           WorkflowFrameInput& out) {
            out.sourceSequence = lastSeen + 1;
            out.image = supplyValid->load(std::memory_order_relaxed)
                            ? fixtureFrame()
                            : rin::ImageU8{};  // 无效图像 + 返回 true。
            lastSeen = out.sourceSequence;
            return true;
        };
        std::shared_ptr<IWorkflowEngine> engine =
            rin::createWorkflowEngine(executor, std::move(config));
        RIN_CHECK(engine->applyGraph(makeChain("grayify")).ok);
        RIN_CHECK(engine->start().admitted);

        bool stayedRunning = true;
        RIN_CHECK_MSG(rin_test::quietFor(
                          [&] {
                              if (engine->state() != WorkflowEngineState::Running) {
                                  stayedRunning = false;
                                  return true;  // 状态漂移即刻终止观察（断言判失败）。
                              }
                              std::uint64_t probe = 0;
                              WorkflowStats probeStats;
                              if (engine->tryLoadStats(probe, probeStats)) {
                                  return probeStats.processedFrames > 0 ||
                                         probeStats.droppedFrames > 0;
                              }
                              return false;
                          },
                          rin_test::kContractQuietWindow),
                      "invalid frame image must be treated as no input");
        RIN_CHECK(stayedRunning);
        RIN_CHECK(engine->state() == WorkflowEngineState::Running);

        supplyValid->store(true, std::memory_order_relaxed);
        std::uint64_t statsSeen = 0;
        WorkflowStats stats;
        RIN_CHECK(rin_test::pollUntil([&] {
            return engine->tryLoadStats(statsSeen, stats) && stats.processedFrames >= 3;
        }));

        engine->stop();
        engine.reset();
        RIN_CHECK(executor.shutdown(true) == ShutdownResult::Completed);
    }

    // ---- 16) 待生效图新源节点尚无捕获输入：不缺失输入执行、不计丢弃 ----
    // 帧源门控关闭（新图源节点 7 无任何捕获输入）时在 Running 下换图：
    // 安全不变量（断言）：不缺失输入执行（引擎不 Failed）、不计过载丢弃、
    // 新图不产出、恢复供帧后新图正常执行且统计恢复推进。
    // 观察项（printf，不作断言）：GraphApplied 相对输入可用的时序与统计是否
    // 在关源期间推进——DEC-013 §1.3 字面为"推迟换代（图保持待生效，不发布
    // 事件）"（两观察值应为 0）；实现为立即换代 + 覆盖检查推迟提交（见报告）。
    {
        executor::Executor executor;
        executor::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        GatedSource gated;
        gated.open->store(true, std::memory_order_relaxed);
        WorkflowEngineConfig config = baseConfig();
        config.frameSource = gated.fn;
        std::shared_ptr<IWorkflowEngine> engine =
            rin::createWorkflowEngine(executor, std::move(config));
        RIN_CHECK(engine->applyGraph(makeChain("grayify")).ok);
        RIN_CHECK(engine->start().admitted);
        std::uint64_t statsSeen = 0;
        WorkflowStats stats;
        RIN_CHECK(rin_test::pollUntil([&] {
            return engine->tryLoadStats(statsSeen, stats) && stats.processedFrames >= 3;
        }));

        // 关源（新图源节点 7 无输入）→ 等在飞帧收敛 → 换图。
        gated.open->store(false, std::memory_order_relaxed);
        RIN_CHECK(rin_test::pollUntil([&] {
            std::uint64_t settleSeen = 0;
            WorkflowStats settle1;
            if (!engine->tryLoadStats(settleSeen, settle1)) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{8});
            std::uint64_t settleSeen2 = 0;
            WorkflowStats settle2;
            return !engine->tryLoadStats(settleSeen2, settle2) ||
                   settle2.sequence == settle1.sequence;
        }));
        std::uint64_t baselineSeen = 0;
        WorkflowStats baseline;
        RIN_CHECK(engine->tryLoadStats(baselineSeen, baseline));
        const std::uint64_t processedAtClose = baseline.processedFrames;

        RIN_CHECK(engine->applyGraph(makeChainIds(7, 8, "downscale")).ok);

        bool sawGraphAppliedWhileClosed = false;
        bool statsAdvancedWhileClosed = false;
        {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds{300};
            while (std::chrono::steady_clock::now() < deadline) {
                WorkflowEvent sampled;
                if (engine->tryLoadEvent(sampled) &&
                    sampled.kind == WorkflowEventKind::GraphApplied) {
                    sawGraphAppliedWhileClosed = true;
                }
                std::uint64_t probeSeen = 0;
                WorkflowStats probe;
                if (engine->tryLoadStats(probeSeen, probe) &&
                    probe.processedFrames > processedAtClose) {
                    statsAdvancedWhileClosed = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
        }
        RIN_CHECK_MSG(engine->state() == WorkflowEngineState::Running,
                      "graph swap without captured input must not fail the engine");
        std::uint64_t dropSeen = 0;
        WorkflowStats dropStats;
        RIN_CHECK(engine->tryLoadStats(dropSeen, dropStats));
        RIN_CHECK_EQ(dropStats.droppedFrames, 0u);  // 不计过载丢弃。
        std::uint64_t out8Seen = 0;
        NodeOutputSnapshot out8;
        RIN_CHECK(!engine->tryLoadNodeOutput(8, out8Seen, out8));  // 新图未执行。
        std::printf(
            "observation: GraphApplied while pending-source has no input=%d; stats "
            "advanced while closed=%d (DEC-013 §1.3 literal wording: both 0)\n",
            sawGraphAppliedWhileClosed ? 1 : 0, statsAdvancedWhileClosed ? 1 : 0);

        // 恢复供帧：新图正常执行、统计恢复推进。
        gated.open->store(true, std::memory_order_relaxed);
        RIN_CHECK(rin_test::pollUntil([&] {
            std::uint64_t seen8 = 0;
            NodeOutputSnapshot o8;
            return engine->tryLoadNodeOutput(8, seen8, o8) && o8.valid();
        }));
        RIN_CHECK(rin_test::pollUntil([&] {
            std::uint64_t probeSeen = 0;
            WorkflowStats probe;
            return engine->tryLoadStats(probeSeen, probe) &&
                   probe.processedFrames > processedAtClose;
        }));

        engine->stop();
        engine.reset();
        RIN_CHECK(executor.shutdown(true) == ShutdownResult::Completed);
    }

    return rin_test::exitStatus();
}
