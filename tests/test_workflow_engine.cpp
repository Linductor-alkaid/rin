// M4-07 工作流真引擎独立验证（DEC-013 执行模型，Independent-Verification-Agent）。
//
// 被测面：src/workflow/engine.hpp/.cpp（rin::createWorkflowEngine /
// WorkflowEngineConfig / WorkflowFrameSource / WorkflowFrameInput）实现的
// IWorkflowEngine 公开契约（include/rin/workflow_engine.hpp），以及真引擎特有语义：
// - 工厂校验：frameSource 为空、maxInFlight==0、pumpInterval<=0 抛
//   std::invalid_argument；paramQueueCapacity==0 按 1 处理（构造不抛）；
// - 默认目录单一事实源：createWorkflowEngine 引擎的 catalog() 与
//   makeDefaultImageNodeCatalog()（src/workflow/default_catalog.hpp，两引擎共同
//   单一事实源；假引擎已随 M5-06 假换真集成移除）逐字段相等
//   （typeId/displayName/inputs/outputs 与
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
// - M7-02 跨代水位回归（2026-09-29 真机 Bug B，产物邮箱跨代共享化）：17）
//   Running 图替换后同 id 节点以既有"上次已见序号"水位继续可拉新尺寸快照且
//   邮箱序号跨代严格递增（判别判据：大水位 + 窄判别窗口——旧实现每代新建
//   邮箱、序号从 1 重计，窗口内无法越过大水位，永久拉不到）；18）参数热更新
//   换代（无 GraphApplied 事件路径）同样成立；19）过代帧不回写共享邮箱
//   （maxInFlight=1 + 门控挂起算子确定性构造"交付 → 挂起执行期间换代 → 释放"
//   时序：释放后挂起旧代帧完成且统计照记，但共享邮箱保持冻结）。
//
// 契约面（引擎无关）由 tests/workflow_engine_contract_suite.hpp 共用套件覆盖
// （本文件先以真引擎 fixture 运行它）；DOD-02 适用性说明见套件文件头。所有等待
// 均为有界轮询（rin_test::pollUntil 默认 5s 死限、静默窗 300ms），不悬挂；
// 套件 + 特有检查整体设计 < 60s。
#include "test_util.hpp"

#include <kairo/executor.hpp>

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

#include "default_catalog.hpp"
#include "engine.hpp"
#include "rin/workflow_types.hpp"
#include "workflow_engine_contract_suite.hpp"

namespace {

using kairo::ShutdownResult;
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

/// 任意尺寸确定性 Rgba8 帧（每次调用独立常量缓冲；M7-02 跨代尺寸回归用，
/// 断言只依赖尺寸/格式，不依赖像素值）。
ImageU8 sizedFrame(std::uint32_t width, std::uint32_t height) {
    auto buffer = std::make_shared<const std::vector<std::uint8_t>>(
        static_cast<std::size_t>(width) * height * 4, std::uint8_t{128});
    return ImageU8::wrap(rin::PortType::Rgba8, width, height, width * 4,
                         std::move(buffer));
}

/// 可控帧源：supply 置 false 后按"无新帧"处理（与 GatedSource 相同，但帧尺寸
/// 可指定——M7-02 跨代尺寸回归用）。
struct SizedGatedSource {
    std::shared_ptr<std::atomic<bool>> supply{
        std::make_shared<std::atomic<bool>>(true)};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    rin::WorkflowFrameSource fn;

    SizedGatedSource(std::uint32_t frameWidth, std::uint32_t frameHeight)
        : width(frameWidth), height(frameHeight) {
        fn = [supply = supply, w = width, h = height](
                 NodeId, std::uint64_t& lastSeen, WorkflowFrameInput& out) {
            if (!supply->load(std::memory_order_relaxed)) {
                return false;
            }
            out.sourceSequence = lastSeen + 1;
            out.image = sizedFrame(w, h);
            lastSeen = out.sourceSequence;
            return true;
        };
    }
};

/// 一次性武装的门控挂起算子：apply 入口计数（entered，测试线程可观察）；
/// 被武装（arm 一次性消费）的下一次 apply 在入口挂起，直至 release 置位
/// （有界 10s 防悬挂）。M7-02 过代帧回归用：测试线程以 entered 前进而完成数
/// 停滞定位"已提交且挂起中"的帧，从而确定性地构造"帧源交付 → 慢算子执行
/// 期间换代 → 释放"时序，无须依赖统计邮箱（发布按提交序有序化，挂起帧会
/// 冻结全部后续发布，统计邮箱不可用）。
class GatedHoldNode final : public IImageNode {
public:
    GatedHoldNode(std::unique_ptr<IImageNode> inner,
                  std::shared_ptr<std::atomic<std::uint64_t>> entered,
                  std::shared_ptr<std::atomic<bool>> arm,
                  std::shared_ptr<std::atomic<bool>> release)
        : inner_(std::move(inner)), entered_(std::move(entered)),
          arm_(std::move(arm)), release_(std::move(release)) {}

    [[nodiscard]] const NodeDescriptor& descriptor() const noexcept override {
        return inner_->descriptor();
    }

    [[nodiscard]] std::vector<ImageU8> apply(
        const std::vector<ImageU8>& inputs) const override {
        entered_->fetch_add(1, std::memory_order_relaxed);
        if (arm_->exchange(false, std::memory_order_acq_rel)) {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds{10};
            while (!release_->load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::microseconds{200});
            }
        }
        return inner_->apply(inputs);
    }

private:
    std::unique_ptr<IImageNode> inner_;
    std::shared_ptr<std::atomic<std::uint64_t>> entered_;
    std::shared_ptr<std::atomic<bool>> arm_;
    std::shared_ptr<std::atomic<bool>> release_;
};

/// 契约套件 fixture：标准引擎 + 跨代/跨会话计数故障注入（消费节点 crop）。
rin_test::WorkflowEngineFixture makeFixture() {
    rin_test::WorkflowEngineFixture fixture;
    fixture.make = [](kairo::Executor& executor) -> std::shared_ptr<IWorkflowEngine> {
        WorkflowEngineConfig config = baseConfig();
        config.frameSource = alwaysSupplySource();
        return rin::createWorkflowEngine(executor, std::move(config));
    };
    fixture.makeFailing =
        [](kairo::Executor& executor, NodeId failNode,
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
        kairo::Executor executor;
        kairo::ExecutorConfig executorConfig;
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

    // ---- 3) 默认目录一致性：真引擎 catalog() 与 makeDefaultImageNodeCatalog()
    // 逐字段相等（两引擎共同单一事实源；假引擎已随 M5-06 移除，基线即本函数）----
    {
        kairo::Executor executor;
        kairo::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        WorkflowEngineConfig config = baseConfig();
        config.frameSource = alwaysSupplySource();
        std::shared_ptr<IWorkflowEngine> engine =
            rin::createWorkflowEngine(executor, std::move(config));
        RIN_CHECK(static_cast<bool>(engine));
        if (engine) {
            const NodeCatalog& real = engine->catalog();
            const NodeCatalog baseline =
                rin::workflow_catalog::makeDefaultImageNodeCatalog();
            RIN_CHECK(real.valid());
            RIN_CHECK(baseline.valid());
            RIN_CHECK_EQ(real.nodes.size(), baseline.nodes.size());
            for (const NodeDescriptor& realNode : real.nodes) {
                const NodeDescriptor* expectedNode =
                    rin::findNodeDescriptor(baseline, realNode.typeId);
                RIN_CHECK_MSG(expectedNode != nullptr,
                              ("baseline catalog must contain typeId: " + realNode.typeId)
                                  .c_str());
                if (expectedNode == nullptr) {
                    continue;
                }
                RIN_CHECK_EQ(realNode.typeId, expectedNode->typeId);
                RIN_CHECK_EQ(realNode.displayName, expectedNode->displayName);
                RIN_CHECK(realNode.inputs == expectedNode->inputs);
                RIN_CHECK(realNode.outputs == expectedNode->outputs);
                RIN_CHECK_EQ(realNode.params.size(), expectedNode->params.size());
                for (const ParamDescriptor& realParam : realNode.params) {
                    const ParamDescriptor* expectedParam = nullptr;
                    for (const ParamDescriptor& candidate : expectedNode->params) {
                        if (candidate.id == realParam.id) {
                            expectedParam = &candidate;
                            break;
                        }
                    }
                    RIN_CHECK_MSG(expectedParam != nullptr,
                                  ("baseline catalog must contain param: " + realParam.id)
                                      .c_str());
                    if (expectedParam == nullptr) {
                        continue;
                    }
                    RIN_CHECK_EQ(realParam.id, expectedParam->id);
                    RIN_CHECK_EQ(realParam.label, expectedParam->label);
                    RIN_CHECK(realParam.kind == expectedParam->kind);
                    RIN_CHECK(realParam.defaultValue == expectedParam->defaultValue);
                    RIN_CHECK_EQ(realParam.hasRange, expectedParam->hasRange);
                    RIN_CHECK_EQ(realParam.minValue, expectedParam->minValue);
                    RIN_CHECK_EQ(realParam.maxValue, expectedParam->maxValue);
                    RIN_CHECK(realParam.enumOptions == expectedParam->enumOptions);
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
        kairo::Executor executor;
        kairo::ExecutorConfig executorConfig;
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
        kairo::Executor executor;
        kairo::ExecutorConfig executorConfig;
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
        kairo::Executor executor;
        kairo::ExecutorConfig executorConfig;
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
        kairo::Executor executor;
        kairo::ExecutorConfig executorConfig;
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
        kairo::Executor executor;
        kairo::ExecutorConfig executorConfig;
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
        kairo::Executor executor;
        kairo::ExecutorConfig executorConfig;
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
            kairo::Executor executor;
            kairo::ExecutorConfig executorConfig;
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
        kairo::Executor executor;
        kairo::ExecutorConfig executorConfig;
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
        kairo::Executor executor;
        kairo::ExecutorConfig executorConfig;
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
        kairo::Executor executor;
        kairo::ExecutorConfig executorConfig;
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
        // 观测窗内取最小值见证复位（对调度鲁棒）：tsan 高负载下测试线程可能
        // 被延迟数百 ms，"首个快照"已越过会话 1 累计值（IVA 实测 ~1/13 次）；
        // 复位语义下窗口内必然观测到 < 会话 1 总量的小值，若复位缺失则全部
        // 观测值 ≥ 会话 1 总量、最小值同样判失败——语义等价且无调度盲区。
        RIN_CHECK(engine->start().admitted);
        std::uint64_t minProcessedSession2 = 0;
        bool haveSession2Sample = false;
        {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds{2000};
            while (std::chrono::steady_clock::now() < deadline) {
                if (engine->tryLoadStats(statsSeen, stats)) {
                    if (!haveSession2Sample ||
                        stats.processedFrames < minProcessedSession2) {
                        minProcessedSession2 = stats.processedFrames;
                    }
                    haveSession2Sample = true;
                    if (minProcessedSession2 < processedSession1) {
                        break;  // 复位见证已达成，提前收窗。
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }
        RIN_CHECK(haveSession2Sample);
        RIN_CHECK_MSG(minProcessedSession2 < processedSession1,
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
        kairo::Executor executor;
        kairo::ExecutorConfig executorConfig;
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
        kairo::Executor executor;
        kairo::ExecutorConfig executorConfig;
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
        kairo::Executor executor;
        kairo::ExecutorConfig executorConfig;
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

    // ---- 17) M7-02 跨代水位回归（图替换）：同 id 节点产物邮箱跨代共享 ----
    // 真机缺陷（M7 Bug B）：M4-07 产物邮箱为代内设施（每代逐节点新建、内部序
    // 号从 1 重计），UI 的"上次已见序号"水位跨代比较无意义——运行中改图后新代
    // 快照全部被旧水位过滤，面板永久显示旧代陈旧产物。修复后同 id 节点跨代共
    // 享同一邮箱（发布序号连续），以既有水位继续拉取必须立刻得到新代产物。
    // 判别判据：先积累水位 ≥ kWm（远大于判别窗口内新建邮箱可追平的帧数），
    // 再换代并在 kSwitchWindow 内以既有水位拉取——旧实现需 ≥kWm 帧才能以更高
    // 序号越过水位（kWm×泵周期 >> kSwitchWindow），窗口内拉不到 → 回归失败；
    // 新实现共享邮箱 → 首个新代帧即命中。
    {
        kairo::Executor executor;
        kairo::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        SizedGatedSource source(64, 48);  // grayify 保持输入尺寸：图 A 输出 64x48。
        WorkflowEngineConfig config = baseConfig();
        config.frameSource = source.fn;
        std::shared_ptr<IWorkflowEngine> engine =
            rin::createWorkflowEngine(executor, std::move(config));

        // 图 A：source(1) → grayify(2)。
        RIN_CHECK(engine->applyGraph(makeChain("grayify")).ok);
        RIN_CHECK(engine->start().admitted);

        // 积累节点 2 的消费水位（有界 30s：2ms 泵 × 300 帧 ≈ 1s，余量吸收
        // sanitizer 下的调度放大）。
        constexpr std::uint64_t kWm = 300;
        std::uint64_t seen = 0;
        NodeOutputSnapshot out;
        RIN_CHECK_MSG(rin_test::pollUntil(
                          [&] {
                              return engine->state() == WorkflowEngineState::Running &&
                                     engine->tryLoadNodeOutput(2, seen, out) &&
                                     out.valid() && out.width == 64 &&
                                     out.height == 48 && seen >= kWm;
                          },
                          std::chrono::milliseconds{30000}),
                      "cross-gen: node 2 watermark accumulates while Running "
                      "(generation A, 64x48 gray)");

        // 运行中 applyGraph 图 B：source(1) → downscale(3) → grayify(2)
        // （grayify 同 id；downscale 默认 scale=0.5 → 输入减半，grayify 输出
        // 32x24 Gray8）。
        WorkflowGraph graphB;
        {
            rin::NodeInstance src;
            src.id = 1;
            src.typeId = "source";
            rin::NodeInstance down;
            down.id = 3;
            down.typeId = "downscale";
            rin::NodeInstance gray;
            gray.id = 2;
            gray.typeId = "grayify";
            graphB.nodes = {src, down, gray};
            graphB.connections = {
                rin::Connection{rin::PortRef{1, rin::PortDirection::Output, 0},
                                rin::PortRef{3, rin::PortDirection::Input, 0}},
                rin::Connection{rin::PortRef{3, rin::PortDirection::Output, 0},
                                rin::PortRef{2, rin::PortDirection::Input, 0}},
            };
        }
        RIN_CHECK(engine->applyGraph(graphB).ok);
        WorkflowEvent event;
        RIN_CHECK_MSG(rin_test::pollUntil([&] {
                          return engine->tryLoadEvent(event) &&
                                 event.kind == WorkflowEventKind::GraphApplied;
                      }),
                      "cross-gen: graph B applies at a frame boundary");

        // 核心判据：以既有水位继续拉节点 2，判别窗口内拉到新尺寸快照。窗口内
        // 的中间加载允许命中换代前最后发布的旧尺寸帧（水位积累退出后、换代前
        // 在飞/已发布的 gen-A 帧合法存在于邮箱中，发布序领先水位），判据是窗口
        // 内出现新尺寸快照——旧实现（换代新建邮箱、序号从 1 重计）在新尺寸出现
        // 前必须先以 ≥kWm 帧越过大水位（kWm×泵周期 >> kSwitchWindow），窗口内
        // 拉不到 → 回归失败；新实现共享邮箱 → 首个新代帧即命中。
        const std::uint64_t watermarkAtSwitch = seen;
        constexpr auto kSwitchWindow = std::chrono::milliseconds{300};
        NodeOutputSnapshot out2;
        bool resized = false;
        {
            const auto deadline = std::chrono::steady_clock::now() + kSwitchWindow;
            while (std::chrono::steady_clock::now() < deadline) {
                if (engine->tryLoadNodeOutput(2, seen, out2) && out2.valid() &&
                    out2.width == 32 && out2.height == 24) {
                    resized = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }
        RIN_CHECK_MSG(resized,
                      "cross-gen: existing watermark must keep working across the "
                      "generation switch (stale-thumbnail regression: a per-"
                      "generation mailbox restarting at 1 cannot overtake the "
                      "watermark within the bounded window)");
        if (resized) {
            RIN_CHECK_EQ(out2.width, 32u);
            RIN_CHECK_EQ(out2.height, 24u);
            RIN_CHECK(out2.format == rin::PortType::Gray8);
            RIN_CHECK_MSG(seen > watermarkAtSwitch,
                          "cross-gen: mailbox publish sequence must strictly "
                          "increase across the generation switch");
        }

        // 序号跨代单调（修复强化的语义）：继续采样，后续每幅新快照序号严格递增。
        {
            bool strictlyIncreasing = true;
            std::uint64_t previous = seen;
            int loads = 0;
            rin_test::pollUntil(
                [&] {
                    NodeOutputSnapshot next;
                    if (engine->tryLoadNodeOutput(2, seen, next) && next.valid()) {
                        if (seen <= previous) {
                            strictlyIncreasing = false;
                        }
                        previous = seen;
                        ++loads;
                    }
                    return loads >= 5 || !strictlyIncreasing;
                },
                std::chrono::milliseconds{5000});
            RIN_CHECK_MSG(loads >= 5,
                          "cross-gen: node 2 keeps publishing in generation B");
            RIN_CHECK_MSG(strictlyIncreasing,
                          "cross-gen: node 2 sequence stays strictly increasing "
                          "after the switch");
        }

        // 新节点 downscale(3) 从水位 0 可拉（新节点取全新邮箱）。
        {
            std::uint64_t seen3 = 0;
            NodeOutputSnapshot out3;
            RIN_CHECK_MSG(rin_test::pollUntil([&] {
                              return engine->tryLoadNodeOutput(3, seen3, out3) &&
                                     out3.valid() && out3.width == 32 &&
                                     out3.height == 24;
                          }),
                          "cross-gen: new node 3 is pullable from a zero watermark");
            RIN_CHECK(out3.format == rin::PortType::Rgba8);
        }

        RIN_CHECK_MSG(engine->state() == WorkflowEngineState::Running,
                      "cross-gen: engine stays Running across the graph switch");
        engine->stop();
        engine.reset();
        RIN_CHECK(executor.shutdown(true) == ShutdownResult::Completed);
    }

    // ---- 18) M7-02 参数热更新跨代回归：参数换代（不走 GraphApplied 事件）----
    // 参数命令在帧边界应用并重建一代（drainBoundary 参数路径），与图替换同一
    // "换代"机制但不发布 GraphApplied——本回归确保该路径的邮箱共享同样成立：
    // 同 id 节点以既有水位可拉到参数生效后的新尺寸快照。判别判据同 17）。
    {
        kairo::Executor executor;
        kairo::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        SizedGatedSource source(128, 96);
        WorkflowEngineConfig config = baseConfig();
        config.frameSource = source.fn;
        std::shared_ptr<IWorkflowEngine> engine =
            rin::createWorkflowEngine(executor, std::move(config));

        // source(1) → downscale(2)：默认 scale=0.5 → 输出 64x48。
        RIN_CHECK(engine->applyGraph(makeChain("downscale")).ok);
        RIN_CHECK(engine->start().admitted);

        constexpr std::uint64_t kWm = 300;
        std::uint64_t seen = 0;
        NodeOutputSnapshot out;
        RIN_CHECK_MSG(rin_test::pollUntil(
                          [&] {
                              return engine->state() == WorkflowEngineState::Running &&
                                     engine->tryLoadNodeOutput(2, seen, out) &&
                                     out.valid() && out.width == 64 &&
                                     out.height == 48 && seen >= kWm;
                          },
                          std::chrono::milliseconds{30000}),
                      "param-gen: node 2 watermark accumulates at 64x48");

        std::string error;
        RIN_CHECK_MSG(
            engine->requestParamUpdate(2, "scale", ParamValue{1.0}, &error),
            "param-gen: scale hot update accepted while running");
        RIN_CHECK(error.empty());

        // 参数换代无 GraphApplied 事件：以判别窗口轮询既有水位。注意窗口内的
        // 中间加载允许命中更新命令前提交的在飞旧尺寸帧（"下一帧生效"语义），
        // 判据是窗口内出现新尺寸快照——旧实现（参数换代新建邮箱、序号从 1 重
        // 计）在新尺寸出现前必须先以 ≥kWm 帧越过大水位，窗口内不可达。
        const std::uint64_t watermarkAtUpdate = seen;
        constexpr auto kSwitchWindow = std::chrono::milliseconds{300};
        NodeOutputSnapshot out2;
        bool resized = false;
        {
            const auto deadline = std::chrono::steady_clock::now() + kSwitchWindow;
            while (std::chrono::steady_clock::now() < deadline) {
                if (engine->tryLoadNodeOutput(2, seen, out2) && out2.valid() &&
                    out2.width == 128 && out2.height == 96) {
                    resized = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }
        RIN_CHECK_MSG(resized,
                      "param-gen: existing watermark must keep working across the "
                      "param rebuild (64x48 -> 128x96 at the next frame boundary)");
        if (resized) {
            RIN_CHECK_EQ(out2.width, 128u);
            RIN_CHECK_EQ(out2.height, 96u);
            RIN_CHECK_MSG(seen > watermarkAtUpdate,
                          "param-gen: mailbox sequence strictly increases across "
                          "the param rebuild");
        }

        RIN_CHECK_MSG(engine->state() == WorkflowEngineState::Running,
                      "param-gen: engine stays Running across the param rebuild");
        engine->stop();
        engine.reset();
        RIN_CHECK(executor.shutdown(true) == ShutdownResult::Completed);
    }

    // ---- 19) M7-02 过代帧不回写共享邮箱：换代前提交的旧代帧发布被跳过 ----
    // 邮箱跨代共享后，换代前提交的在飞旧代帧若照常发布，会以更高邮箱序号把
    // 旧图产物写回共享邮箱（"迟到的旧代发布不进新代"语义在共享邮箱下的保障，
    // publishOutputs 对非生效代跳过、统计照记）。确定性构造：maxInFlight=1 +
    // 门控挂起算子挂起一帧（帧源在图 B 应用前交付、挂起执行期间应用图 B），
    // 停源后释放挂起帧——此后不存在任何新代帧，共享邮箱必须保持冻结（静默窗
    // 内无新快照）。判别判据：若跳过语义缺失，挂起帧完成时会以更高的邮箱序号
    // 发布旧图产物，静默窗检查读到新快照而失败。（注：本判据只在邮箱共享前提
    // 下判别"跳过缺失"；共享语义本身由 17）/18）锁定。）
    {
        kairo::Executor executor;
        kairo::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        SizedGatedSource source(128, 96);
        auto entered = std::make_shared<std::atomic<std::uint64_t>>(0);
        auto arm = std::make_shared<std::atomic<bool>>(false);
        auto release = std::make_shared<std::atomic<bool>>(false);
        WorkflowEngineConfig config = baseConfig();
        config.maxInFlight = 1;
        config.frameSource = source.fn;
        config.nodeFactory = [entered, arm, release](
                                 const NodeDescriptor& descriptor,
                                 const rin::NodeInstance& instance)
            -> std::unique_ptr<IImageNode> {
            if (instance.typeId == "source") {
                return nullptr;  // 注入型源节点语义。
            }
            std::unique_ptr<IImageNode> inner =
                rin::makeDefaultImageNode(descriptor, instance);
            if (instance.typeId == "grayify") {
                inner = std::make_unique<GatedHoldNode>(std::move(inner), entered,
                                                        arm, release);
            }
            return inner;
        };
        std::shared_ptr<IWorkflowEngine> engine =
            rin::createWorkflowEngine(executor, std::move(config));

        // 图 A：source(1) → grayify(2)（输出 128x96）。帧流动若干幅。
        RIN_CHECK(engine->applyGraph(makeChain("grayify")).ok);
        RIN_CHECK(engine->start().admitted);
        std::uint64_t statsSeen = 0;
        WorkflowStats stats;
        RIN_CHECK_MSG(rin_test::pollUntil(
                          [&] {
                              return engine->tryLoadStats(statsSeen, stats) &&
                                     stats.processedFrames >= 40;
                          },
                          std::chrono::milliseconds{30000}),
                      "stale-frame: generation A flows before the hold");

        // 武装挂起：下一次 grayify apply（即下一帧）在入口挂起。apply 计数前进
        // 而完成数停滞 = "已提交且挂起中"的确定性证据（发布按提交序有序化，
        // 挂起帧冻结全部后续发布，故以节点入口计数定位）。
        arm->store(true, std::memory_order_release);
        const std::uint64_t appliedAtArm = entered->load(std::memory_order_relaxed);
        RIN_CHECK_MSG(rin_test::pollUntil(
                          [&] {
                              return entered->load(std::memory_order_relaxed) >
                                     appliedAtArm;
                          },
                          std::chrono::milliseconds{10000}),
                      "stale-frame: a frame is held inside the gated operator");
        // 统计邮箱自挂起帧后冻结（发布按提交序有序化），既有水位已消费到最新
        // 快照——以全新水位 0 探针读取当前累计值。
        RIN_CHECK(engine->tryLoadStats(statsSeen = 0, stats));
        RIN_CHECK_MSG(stats.processedFrames ==
                          entered->load(std::memory_order_relaxed) - 1,
                      "stale-frame: held frame started but has not completed");

        // 停源（挂起帧之后不再有任何新代帧）→ 运行中应用图 B：
        // source(1) → downscale(3) → grayify(2)（grayify 同 id → 邮箱共享）。
        source.supply->store(false, std::memory_order_relaxed);
        WorkflowGraph graphB;
        {
            rin::NodeInstance src;
            src.id = 1;
            src.typeId = "source";
            rin::NodeInstance down;
            down.id = 3;
            down.typeId = "downscale";
            rin::NodeInstance gray;
            gray.id = 2;
            gray.typeId = "grayify";
            graphB.nodes = {src, down, gray};
            graphB.connections = {
                rin::Connection{rin::PortRef{1, rin::PortDirection::Output, 0},
                                rin::PortRef{3, rin::PortDirection::Input, 0}},
                rin::Connection{rin::PortRef{3, rin::PortDirection::Output, 0},
                                rin::PortRef{2, rin::PortDirection::Input, 0}},
            };
        }
        RIN_CHECK(engine->applyGraph(graphB).ok);
        WorkflowEvent event;
        RIN_CHECK_MSG(rin_test::pollUntil(
                          [&] {
                              return engine->tryLoadEvent(event) &&
                                     event.kind == WorkflowEventKind::GraphApplied;
                          },
                          std::chrono::milliseconds{10000}),
                      "stale-frame: graph B applies while the frame is held");

        // 释放挂起帧：完成且统计照记（过代帧仍是完成的一帧）。
        release->store(true, std::memory_order_release);
        const std::uint64_t heldApplies = entered->load(std::memory_order_relaxed);
        RIN_CHECK_MSG(rin_test::pollUntil(
                          [&] {
                              std::uint64_t probeSeen = 0;
                              WorkflowStats probe;
                              return engine->tryLoadStats(probeSeen, probe) &&
                                     probe.processedFrames >= heldApplies;
                          },
                          std::chrono::milliseconds{10000}),
                      "stale-frame: stale frame completes and is still counted");

        // 共享邮箱未被回写：末次产物仍是旧代末帧（128x96），此后静默窗内不得
        // 出现任何新快照（挂起帧是停源后唯一可能的发布者，其发布必须被跳过）。
        {
            std::uint64_t probeSeen = 0;
            NodeOutputSnapshot probe;
            RIN_CHECK_MSG(engine->tryLoadNodeOutput(2, probeSeen, probe) &&
                              probe.valid() && probe.width == 128 &&
                              probe.height == 96,
                          "stale-frame: mailbox keeps the last generation-A output");
            const std::uint64_t frozen = probeSeen;
            RIN_CHECK_MSG(rin_test::quietFor(
                              [&] {
                                  std::uint64_t probeSeen2 = frozen;
                                  NodeOutputSnapshot discarded;
                                  return engine->tryLoadNodeOutput(2, probeSeen2,
                                                                   discarded);
                              },
                              rin_test::kContractQuietWindow),
                          "stale-frame: late generation-A frame must not publish "
                          "into the shared mailbox");
        }

        RIN_CHECK_MSG(engine->state() == WorkflowEngineState::Running,
                      "stale-frame: engine stays Running across the held-frame "
                      "switch");
        engine->stop();
        engine.reset();
        RIN_CHECK(executor.shutdown(true) == ShutdownResult::Completed);
    }

    return rin_test::exitStatus();
}
