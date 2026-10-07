// M10 引擎有状态节点执行模型独立验证（DEC-020 决策 3，
// Independent-Verification-Agent）：src/workflow/engine.cpp 的
// IStatefulImageNode 串行在飞门 + src/core/image_ops.cpp DepthHistoryNode
// 的 staged 重提交去重，对照 DEC-013 帧泵执行模型既有契约。
//
// 被测面与范围：
// 1) 节点级去重（确定性）：factory 构造 depth_history，同像素缓冲对象重提交
//    不重复入环（apply 输出逐位不变），不同缓冲对象照常入环；
// 2) 帧序保证（串行门）：source_depth_metric → depth_history 图 + 慢算子 +
//    持续供帧（maxInFlight=2 泵 2ms，提交速率 ≫ 处理速率）：历史平面按应用序
//    严格递增堆叠（串行门正确则无乱序应用——staged 合帧只可能跳帧，不可能
//    逆序），droppedFrames == 0（跳过提交不计过载丢弃，DEC-020 决策 3），
//    stats.inFlight 全程采样 ≤ 1（串行在飞观察面）；
// 3) staged 旧帧重提交去重（引擎级，多源图）：source1 供 A/B/C 后静默、
//    source2 持续供新帧驱动 staged {S1: C} 反复重提交——去重使历史保持
//    [A,B,C]（输出平面 = 1,2,3）；无去重将被重复入环推成 [C,C,C]（值域可
//    区分）；sourceSequence 传播（历史节点 = min 生产者 = 3）附带核对；
// 4) 生成代复位：参数热更新（sample_count 2→3）重建生成代即清空历史——
//    复位后首帧欠帧填充全为首帧值（[3,3,3]，非复位续接 [2,3,3]）；
// 5) 串行门不影响无状态图（对照）：stateless 慢算子图（source→fill，包装
//    不含 IStatefulImageNode）在 maxInFlight=2 下 droppedFrames > 0（并发
//    准入与过载丢弃照常，门不得误触发）；
// 6) 输入尺寸中途变化 → 节点抛 invalid_argument → 引擎 NodeFailed(node) +
//    Failed 终态路径。
//
// 等待纪律：全部有界轮询（5s 死限），不悬挂；每块独立 Executor，先 engine
// stop 后 executor shutdown（owner 顺序，AGENTS.md Executor 规则 7）。
// golden 独立性：期望值由 M8 O7 冻结下标公式独立推导（欠帧首帧填充、
// oldest 顶最新底、idx = length − (count−1−i)·skip − 1 − delay），不参考
// 实现代码。
#include "test_util.hpp"

#include <kairo/executor.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rin/image_node.hpp>
#include <rin/image_types.hpp>
#include <rin/workflow_engine.hpp>
#include <rin/workflow_types.hpp>

#include "default_catalog.hpp"
#include "engine.hpp"

namespace {

using kairo::ShutdownResult;
using rin::IImageNode;
using rin::ImageNodeFactory;
using rin::ImageU8;
using rin::IStatefulImageNode;
using rin::NodeCatalog;
using rin::NodeDescriptor;
using rin::NodeId;
using rin::NodeInstance;
using rin::ParamAssignment;
using rin::PortDirection;
using rin::PortRef;
using rin::PortType;
using rin::WorkflowEngineConfig;
using rin::WorkflowEngineState;
using rin::WorkflowEvent;
using rin::WorkflowEventKind;
using rin::WorkflowFrameInput;
using rin::WorkflowGraph;
using rin::WorkflowStats;

// kPollDeadline/pollUntil 由 tests/test_util.hpp 唯一提供（M12/CR-23）。
using rin_test::kPollDeadline;
using rin_test::pollUntil;

/// 常量米制深度帧（每次调用独立缓冲对象：指针同一性测试需要可区分缓冲）。
ImageU8 depthConstantFrame(std::uint32_t width, std::uint32_t height, float value) {
    std::vector<float> values(static_cast<std::size_t>(width) * height, value);
    std::vector<std::uint8_t> bytes(values.size() * 4u);
    std::memcpy(bytes.data(), values.data(), bytes.size());
    return ImageU8::wrap(PortType::Depth32F, width, height, width * 4u,
                         std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes)));
}

/// 从共享缓冲构造 Depth32F 视图（staged 重提交同一缓冲对象的测试面）。
ImageU8 depthFrameFromShared(std::shared_ptr<const std::vector<std::uint8_t>> pixels,
                             std::uint32_t width, std::uint32_t height) {
    return ImageU8::wrap(PortType::Depth32F, width, height, width * 4u, std::move(pixels));
}

std::shared_ptr<const std::vector<std::uint8_t>> sharedDepthBytes(std::uint32_t width,
                                                                  std::uint32_t height,
                                                                  float value) {
    std::vector<float> values(static_cast<std::size_t>(width) * height, value);
    std::vector<std::uint8_t> bytes(values.size() * 4u);
    std::memcpy(bytes.data(), values.data(), bytes.size());
    return std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
}

/// 脚本化多源帧源：每节点独立 FIFO 队列（显式推入，每次调用弹一帧）+ 可选
/// 持续计数供帧源（帧值 = 供帧计数，新鲜缓冲；永不枯竭，规避 staged 无新帧
/// 滞留面）；总闸 open=false 时按"无新帧"处理（帧源空闲语义）。
class ScriptedSource {
public:
    void push(NodeId node, ImageU8 frame, std::uint64_t sequence) {
        const std::scoped_lock lock(mutex_);
        queues_[node].frames.push_back(std::move(frame));
        queues_[node].sequences.push_back(sequence);
    }

    void setContinuous(NodeId node, std::uint32_t width, std::uint32_t height) {
        const std::scoped_lock lock(mutex_);
        continuous_[node] = Continuous{width, height, 0};
    }

    void setOpen(bool open) { open_.store(open, std::memory_order_relaxed); }

    rin::WorkflowFrameSource fn() {
        return [this](NodeId node, std::uint64_t& lastSeen, WorkflowFrameInput& out) -> bool {
            if (!open_.load(std::memory_order_relaxed)) {
                return false;
            }
            const std::scoped_lock lock(mutex_);
            const auto queueIt = queues_.find(node);
            if (queueIt != queues_.end() && !queueIt->second.frames.empty()) {
                out.image = std::move(queueIt->second.frames.front());
                out.sourceSequence = queueIt->second.sequences.front();
                queueIt->second.frames.pop_front();
                queueIt->second.sequences.pop_front();
                lastSeen = out.sourceSequence;
                return true;
            }
            const auto contIt = continuous_.find(node);
            if (contIt != continuous_.end()) {
                ++contIt->second.supplied;
                out.image = depthConstantFrame(contIt->second.width, contIt->second.height,
                                               static_cast<float>(contIt->second.supplied));
                out.sourceSequence = contIt->second.supplied;
                lastSeen = out.sourceSequence;
                return true;
            }
            return false;
        };
    }

private:
    struct Queue {
        std::deque<ImageU8> frames;
        std::deque<std::uint64_t> sequences;
    };
    struct Continuous {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint64_t supplied = 0;
    };
    std::mutex mutex_;
    std::atomic<bool> open_{true};
    std::map<NodeId, Queue> queues_;
    std::map<NodeId, Continuous> continuous_;
};

/// 无状态慢算子包装（不含 IStatefulImageNode：对照组串行门不得误触发）。
class SlowNode final : public IImageNode {
public:
    SlowNode(std::unique_ptr<IImageNode> inner, std::chrono::milliseconds delay)
        : inner_(std::move(inner)), delay_(delay) {}

    [[nodiscard]] const NodeDescriptor& descriptor() const noexcept override {
        return inner_->descriptor();
    }
    [[nodiscard]] std::vector<ImageU8> apply(const std::vector<ImageU8>& inputs) const override {
        std::this_thread::sleep_for(delay_);
        return inner_->apply(inputs);
    }

private:
    std::unique_ptr<IImageNode> inner_;
    std::chrono::milliseconds delay_;
};

/// 有状态慢算子包装（携带 IStatefulImageNode 标记：buildGeneration 探测面）。
class SlowStatefulNode final : public IImageNode, public IStatefulImageNode {
public:
    SlowStatefulNode(std::unique_ptr<IImageNode> inner, std::chrono::milliseconds delay)
        : inner_(std::move(inner)), delay_(delay) {}

    [[nodiscard]] const NodeDescriptor& descriptor() const noexcept override {
        return inner_->descriptor();
    }
    [[nodiscard]] std::vector<ImageU8> apply(const std::vector<ImageU8>& inputs) const override {
        std::this_thread::sleep_for(delay_);
        return inner_->apply(inputs);
    }

private:
    std::unique_ptr<IImageNode> inner_;
    std::chrono::milliseconds delay_;
};

/// 工厂：depth_history 加慢有状态包装，depth_fill_invalid 加慢无状态包装，
/// 其余转发默认工厂（nullptr 注入语义透传）。
ImageNodeFactory slowDepthFactory(std::chrono::milliseconds statefulDelay,
                                  std::chrono::milliseconds statelessDelay) {
    return [statefulDelay, statelessDelay](
               const NodeDescriptor& descriptor,
               const NodeInstance& instance) -> std::unique_ptr<IImageNode> {
        std::unique_ptr<IImageNode> inner = rin::makeDefaultImageNode(descriptor, instance);
        if (inner == nullptr) {
            return nullptr;
        }
        if (descriptor.typeId == "depth_history") {
            return std::make_unique<SlowStatefulNode>(std::move(inner), statefulDelay);
        }
        if (descriptor.typeId == "depth_fill_invalid") {
            return std::make_unique<SlowNode>(std::move(inner), statelessDelay);
        }
        return inner;
    };
}

WorkflowGraph chainGraph(NodeId sourceId, NodeId consumerId, const std::string& consumerType,
                         std::vector<ParamAssignment> consumerParams = {}) {
    WorkflowGraph graph;
    NodeInstance source;
    source.id = sourceId;
    source.typeId = "source_depth_metric";
    NodeInstance consumer;
    consumer.id = consumerId;
    consumer.typeId = consumerType;
    consumer.params = std::move(consumerParams);
    graph.nodes = {source, consumer};
    rin::Connection connection;
    connection.from = PortRef{sourceId, PortDirection::Output, 0};
    connection.to = PortRef{consumerId, PortDirection::Input, 0};
    graph.connections = {connection};
    return graph;
}

/// 小参数集（M8 O7 手推友好）：length=4、skip=1、delay=0、count 可变。
std::vector<ParamAssignment> historyParams(std::int64_t count) {
    return {{"history_length", std::int64_t{4}},
            {"sample_count", count},
            {"sample_skip", std::int64_t{1}},
            {"sample_delay", std::int64_t{0}}};
}

/// 快照 → 逐平面首像素值（平面 = 竖直堆叠的 planeHeight 行块，M8 O7 布局）。
std::vector<float> planeLeadValues(const rin::NodeOutputSnapshot& snapshot,
                                   std::uint32_t planeHeight) {
    std::vector<float> leads;
    const ImageU8 view = ImageU8::wrap(snapshot.format, snapshot.width, snapshot.height,
                                       snapshot.stride, snapshot.pixels);
    if (!view.valid() || planeHeight == 0 || snapshot.height % planeHeight != 0) {
        return leads;
    }
    const std::uint32_t planes = snapshot.height / planeHeight;
    leads.reserve(planes);
    for (std::uint32_t p = 0; p < planes; ++p) {
        const float* row = rin::depthF32Row(view, p * planeHeight);
        if (row == nullptr) {
            return {};
        }
        leads.push_back(row[0]);
    }
    return leads;
}

/// 读取节点最新产物（水印 0 起拉最新一幅；邮箱跨代共享，序号单调）。
bool readLatestOutput(rin::IWorkflowEngine& engine, NodeId node, rin::NodeOutputSnapshot& out) {
    std::uint64_t watermark = 0;
    return engine.tryLoadNodeOutput(node, watermark, out);
}

/// 引擎运行骨架：独立 Executor + 引擎；析构按 owner 顺序回收（engine stop →
/// executor shutdown）。
struct EngineRun {
    kairo::Executor executor;
    std::shared_ptr<rin::IWorkflowEngine> engine;

    ~EngineRun() {
        if (engine != nullptr) {
            engine->stop();
        }
        (void)executor.shutdown(true);
    }
};

std::unique_ptr<EngineRun> startEngine(
    WorkflowEngineConfig config, const WorkflowGraph& graph, ScriptedSource& source,
    std::chrono::milliseconds statefulDelay = std::chrono::milliseconds{0},
    std::chrono::milliseconds statelessDelay = std::chrono::milliseconds{0}) {
    auto run = std::make_unique<EngineRun>();
    kairo::ExecutorConfig executorConfig;
    RIN_CHECK(run->executor.initialize(executorConfig));
    config.pumpInterval = std::chrono::milliseconds{2};
    config.frameSource = source.fn();
    config.nodeFactory = slowDepthFactory(statefulDelay, statelessDelay);
    run->engine = rin::createWorkflowEngine(run->executor, std::move(config));
    RIN_CHECK(run->engine != nullptr);
    if (run->engine == nullptr) {
        return run;
    }
    RIN_CHECK(run->engine->applyGraph(graph).ok);
    RIN_CHECK(run->engine->start().admitted);
    return run;
}

}  // namespace

int main() {
    // ---- 1) 节点级 staged 重提交去重（确定性，无引擎） -----------------------
    {
        NodeInstance instance;
        instance.id = 1;
        instance.typeId = "depth_history";
        instance.params = historyParams(2);
        // 目录运行期不变；静态缓存避免描述符悬垂（临时目录在满表达式末销毁）。
        static const NodeCatalog catalog = rin::workflow_catalog::makeDefaultImageNodeCatalog();
        const NodeDescriptor& descriptor = *rin::findNodeDescriptor(catalog, "depth_history");
        std::unique_ptr<IImageNode> node = rin::makeDefaultImageNode(descriptor, instance);
        RIN_CHECK(node != nullptr);
        RIN_CHECK(dynamic_cast<const IStatefulImageNode*>(node.get()) != nullptr);
        if (node == nullptr) {
            return rin_test::exitStatus();
        }

        // A、B 各自独立缓冲；bOld 与 b 共享同一像素缓冲对象（指针同一）——
        // staged 重提交语义 = 最新帧缓冲重交付（去重判据：与最后入环帧同缓冲）。
        ImageU8 a = depthConstantFrame(4, 1, 1.0f);
        ImageU8 b = depthConstantFrame(4, 1, 2.0f);
        ImageU8 bOld = depthFrameFromShared(b.pixels(), 4, 1);
        ImageU8 bNewBuffer = depthConstantFrame(4, 1, 3.0f);  // 新缓冲（必须照常入环）。

        std::vector<ImageU8> out1 = node->apply({a});
        std::vector<ImageU8> out2 = node->apply({b});
        std::vector<ImageU8> out3 = node->apply({bOld});  // staged 旧帧（B 缓冲）重提交。
        RIN_CHECK_EQ(out1[0].height(), std::uint32_t{2});  // 欠帧首帧填充 [A, A]。
        {                                                  // 欠帧填充 sanity（O7）。
            const float* row0of1 = rin::depthF32Row(out1[0], 0);
            const float* row1of1 = rin::depthF32Row(out1[0], 1);
            RIN_CHECK(row0of1 != nullptr && row1of1 != nullptr);
            if (row0of1 != nullptr && row1of1 != nullptr) {
                RIN_CHECK_EQ(row0of1[0], 1.0f);
                RIN_CHECK_EQ(row1of1[0], 1.0f);
            }
        }

        // 输出平面（count=2，skip=1）：去重 → [A, B]；若重复入环 → [B, A]。
        RIN_CHECK_EQ(out3[0].width(), std::uint32_t{4});
        RIN_CHECK_EQ(out3[0].height(), std::uint32_t{2});
        {
            const float* row0of2 = rin::depthF32Row(out2[0], 0);
            const float* row1of2 = rin::depthF32Row(out2[0], 1);
            const float* row0of3 = rin::depthF32Row(out3[0], 0);
            const float* row1of3 = rin::depthF32Row(out3[0], 1);
            RIN_CHECK(row0of2 != nullptr && row1of2 != nullptr);
            RIN_CHECK(row0of3 != nullptr && row1of3 != nullptr);
            if (row0of2 != nullptr && row0of3 != nullptr) {
                RIN_CHECK_EQ(row0of3[0], row0of2[0]);  // 与上一帧输出逐位不变。
                RIN_CHECK_EQ(row0of3[0], 1.0f);        // oldest 顶 = A。
            }
            if (row1of2 != nullptr && row1of3 != nullptr) {
                RIN_CHECK_EQ(row1of3[0], row1of2[0]);
                RIN_CHECK_EQ(row1of3[0], 2.0f);  // 最新底 = B。
            }
        }

        // 对照：不同缓冲对象必须照常入环——喂 B'（新缓冲，值 3）后平面 = [B, B']。
        std::vector<ImageU8> out4 = node->apply({bNewBuffer});
        const float* row0of4 = rin::depthF32Row(out4[0], 0);
        const float* row1of4 = rin::depthF32Row(out4[0], 1);
        RIN_CHECK(row0of4 != nullptr && row1of4 != nullptr);
        if (row0of4 != nullptr && row1of4 != nullptr) {
            RIN_CHECK_EQ(row0of4[0], 2.0f);
            RIN_CHECK_EQ(row1of4[0], 3.0f);
        }
    }

    // ---- 2) 帧序保证（串行门 + 持续供帧压力，maxInFlight=2） -----------------
    {
        ScriptedSource source;
        source.setContinuous(1, 32, 18);
        WorkflowGraph graph = chainGraph(1, 2, "depth_history",
                                         {{"history_length", std::int64_t{16}},
                                          {"sample_count", std::int64_t{8}},
                                          {"sample_skip", std::int64_t{1}},
                                          {"sample_delay", std::int64_t{0}}});
        WorkflowEngineConfig config;
        config.maxInFlight = 2;
        std::unique_ptr<EngineRun> run =
            startEngine(std::move(config), graph, source, std::chrono::milliseconds{5});
        RIN_CHECK(run->engine != nullptr);
        if (run->engine == nullptr) {
            return rin_test::exitStatus();
        }

        std::uint64_t statsSeq = 0;
        WorkflowStats stats;
        std::uint64_t maxInFlightSeen = 0;
        const bool reached = pollUntil([&] {
            if (run->engine->tryLoadStats(statsSeq, stats)) {
                maxInFlightSeen = std::max<std::uint64_t>(maxInFlightSeen, stats.inFlight);
            }
            return stats.processedFrames >= 12;
        });
        RIN_CHECK(reached);

        run->engine->stop();  // 冻结统计与产物（stale 语义保留）。
        RIN_CHECK_EQ(run->engine->state(), WorkflowEngineState::Idle);

        std::uint64_t finalSeq = 0;
        WorkflowStats finalStats;
        RIN_CHECK(run->engine->tryLoadStats(finalSeq, finalStats));
        RIN_CHECK(finalStats.processedFrames >= 12);
        // 串行门核心契约：跳过提交不计过载丢弃。
        RIN_CHECK_EQ(finalStats.droppedFrames, std::uint64_t{0});
        // 串行在飞观察面：含状态图全程至多 1 个在飞任务（采样上界）。
        RIN_CHECK(maxInFlightSeen <= 1);

        rin::NodeOutputSnapshot snapshot;
        RIN_CHECK(readLatestOutput(*run->engine, 2, snapshot));
        RIN_CHECK(snapshot.format == PortType::Depth32F);
        RIN_CHECK_EQ(snapshot.width, std::uint32_t{32});
        RIN_CHECK_EQ(snapshot.height, std::uint32_t{8u * 18u});
        const std::vector<float> leads = planeLeadValues(snapshot, 18);
        RIN_CHECK_EQ(leads.size(), std::size_t{8});
        // 帧值 = 供帧计数（1,2,3,…）；应用序严格递增（乱序应用必然产生逆序对；
        // staged 合帧只减少帧数，processedFrames ≥ 12 > 8 保证无欠帧填充）。
        bool ordered = true;
        for (std::size_t i = 1; i < leads.size(); ++i) {
            if (leads[i - 1] >= leads[i]) {
                ordered = false;
            }
        }
        RIN_CHECK(ordered);
        RIN_CHECK(leads.front() >= 1.0f);
    }

    // ---- 3) staged 旧帧重提交去重（引擎级多源：S1 静默后 staged 反复重提交） --
    {
        ScriptedSource source;
        // source1：A、B、C 后静默（缓冲对象互异）；source2 持续供帧驱动提交。
        source.push(1, depthConstantFrame(4, 1, 1.0f), 1);
        source.push(1, depthConstantFrame(4, 1, 2.0f), 2);
        source.push(1, depthConstantFrame(4, 1, 3.0f), 3);
        source.setContinuous(3, 8, 8);

        WorkflowGraph graph;
        NodeInstance s1;
        s1.id = 1;
        s1.typeId = "source_depth_metric";
        NodeInstance history;
        history.id = 2;
        history.typeId = "depth_history";
        history.params = historyParams(3);  // count=3：重复入环会推移抽样窗。
        NodeInstance s2;
        s2.id = 3;
        s2.typeId = "source_depth_metric";
        NodeInstance fill;
        fill.id = 4;
        fill.typeId = "depth_fill_invalid";
        graph.nodes = {s1, history, s2, fill};
        graph.connections = {rin::Connection{PortRef{1, PortDirection::Output, 0},
                                             PortRef{2, PortDirection::Input, 0}},
                             rin::Connection{PortRef{3, PortDirection::Output, 0},
                                             PortRef{4, PortDirection::Input, 0}}};

        WorkflowEngineConfig config;
        config.maxInFlight = 2;
        // 无慢算子：任务在 tick 间隔内完成，A/B/C 逐 tick 按序提交（无合帧），
        // 此后 S1 staged 保持 C、由 S2 新帧逐 tick 驱动重提交（去重面）。
        std::unique_ptr<EngineRun> run = startEngine(std::move(config), graph, source);
        RIN_CHECK(run->engine != nullptr);
        if (run->engine == nullptr) {
            return rin_test::exitStatus();
        }

        std::uint64_t statsSeq = 0;
        WorkflowStats stats;
        RIN_CHECK(pollUntil([&] {
            return run->engine->tryLoadStats(statsSeq, stats) && stats.processedFrames >= 6;
        }));

        run->engine->stop();
        std::uint64_t finalSeq = 0;
        WorkflowStats finalStats;
        RIN_CHECK(run->engine->tryLoadStats(finalSeq, finalStats));
        RIN_CHECK(finalStats.processedFrames >= 6);
        RIN_CHECK_EQ(finalStats.droppedFrames, std::uint64_t{0});

        rin::NodeOutputSnapshot snapshot;
        RIN_CHECK(readLatestOutput(*run->engine, 2, snapshot));
        // 去重 → 历史 [A,B,C] 平面 [1,2,3]；无去重 → [C,C,C] 平面 [3,3,3]。
        const std::vector<float> leads = planeLeadValues(snapshot, 1);
        RIN_CHECK_EQ(leads.size(), std::size_t{3});
        if (leads.size() == 3) {
            RIN_CHECK_EQ(leads[0], 1.0f);
            RIN_CHECK_EQ(leads[1], 2.0f);
            RIN_CHECK_EQ(leads[2], 3.0f);
        }
        // sourceSequence 传播：历史节点 = min 生产者 = source1 最新序号 3。
        RIN_CHECK_EQ(snapshot.sourceSequence, std::uint64_t{3});
    }

    // ---- 4) 生成代复位：参数热更新清空历史（欠帧首帧填充可观测） -------------
    {
        ScriptedSource source;
        source.push(1, depthConstantFrame(4, 1, 1.0f), 1);
        source.push(1, depthConstantFrame(4, 1, 2.0f), 2);
        WorkflowGraph graph = chainGraph(1, 2, "depth_history", historyParams(2));

        WorkflowEngineConfig config;
        config.maxInFlight = 2;
        std::unique_ptr<EngineRun> run = startEngine(std::move(config), graph, source);
        RIN_CHECK(run->engine != nullptr);
        if (run->engine == nullptr) {
            return rin_test::exitStatus();
        }

        std::uint64_t statsSeq = 0;
        WorkflowStats stats;
        RIN_CHECK(pollUntil([&] {
            return run->engine->tryLoadStats(statsSeq, stats) && stats.processedFrames >= 2;
        }));

        // 停供 + 参数热更新（下一帧生效，帧边界重建生成代）。
        source.setOpen(false);
        std::string paramError;
        RIN_CHECK(run->engine->requestParamUpdate(2, "sample_count",
                                                  rin::ParamValue{std::int64_t{3}}, &paramError));
        WorkflowEvent event;
        RIN_CHECK(pollUntil([&] {
            return run->engine->tryLoadEvent(event) &&
                   event.kind == WorkflowEventKind::ParamUpdated;
        }));

        // 复位后首帧 C：新历史欠帧填充 → 三平面全 3.0（未复位则为 [2,3,3]）。
        source.push(1, depthConstantFrame(4, 1, 3.0f), 3);
        source.setOpen(true);
        RIN_CHECK(pollUntil([&] {
            return run->engine->tryLoadStats(statsSeq, stats) && stats.processedFrames >= 3;
        }));
        run->engine->stop();

        rin::NodeOutputSnapshot snapshot;
        RIN_CHECK(readLatestOutput(*run->engine, 2, snapshot));
        RIN_CHECK_EQ(snapshot.height, std::uint32_t{3u * 1u});
        const std::vector<float> leads = planeLeadValues(snapshot, 1);
        RIN_CHECK_EQ(leads.size(), std::size_t{3});
        if (leads.size() == 3) {
            RIN_CHECK_EQ(leads[0], 3.0f);
            RIN_CHECK_EQ(leads[1], 3.0f);
            RIN_CHECK_EQ(leads[2], 3.0f);
        }
    }

    // ---- 5) 对照：无状态图不受串行门影响（并发准入 + 过载丢弃照常） ----------
    {
        ScriptedSource source;
        source.setContinuous(1, 32, 18);
        WorkflowGraph graph = chainGraph(1, 2, "depth_fill_invalid");

        WorkflowEngineConfig config;
        config.maxInFlight = 2;
        std::unique_ptr<EngineRun> run =
            startEngine(std::move(config), graph, source, std::chrono::milliseconds{0},
                        std::chrono::milliseconds{30});  // 仅 fill 慢（无状态包装）。
        RIN_CHECK(run->engine != nullptr);
        if (run->engine == nullptr) {
            return rin_test::exitStatus();
        }

        std::uint64_t statsSeq = 0;
        WorkflowStats stats;
        // 过载丢弃出现 = 并发在飞达上限 = 串行门未误触发（包装无状态标记）。
        RIN_CHECK(pollUntil([&] {
            return run->engine->tryLoadStats(statsSeq, stats) && stats.droppedFrames >= 1;
        }));
        RIN_CHECK_EQ(run->engine->state(), WorkflowEngineState::Running);
    }

    // ---- 6) 输入尺寸中途变化 → 节点抛 → NodeFailed/Failed --------------------
    // NodeFailed 与 Failed 背靠背发布（最新态通道单会话可能只见 Failed）：
    // 等待 Failed 期间高频采样事件通道捕获 NodeFailed；未捕获则换新会话重试
    // （test_workflow_engine.cpp 失败归因同纪律）。
    {
        constexpr int kFailureAttempts = 8;
        bool sawNodeFailed = false;
        NodeId nodeFailedId = rin::kInvalidNode;
        bool reachedFailedOnce = false;
        for (int attempt = 0; attempt < kFailureAttempts && !sawNodeFailed; ++attempt) {
            ScriptedSource source;
            // 首帧 4×1 定型，其后全部 4×2（20 帧冗余：供帧不中断，规避 staged 滞留）。
            source.push(1, depthConstantFrame(4, 1, 1.0f), 1);
            for (std::uint64_t k = 2; k <= 21; ++k) {
                source.push(1, depthConstantFrame(4, 2, static_cast<float>(k)), k);
            }
            WorkflowGraph graph = chainGraph(1, 2, "depth_history", historyParams(2));

            WorkflowEngineConfig config;
            config.maxInFlight = 2;
            std::unique_ptr<EngineRun> run = startEngine(std::move(config), graph, source);
            RIN_CHECK(run->engine != nullptr);
            if (run->engine == nullptr) {
                return rin_test::exitStatus();
            }

            const auto deadline = std::chrono::steady_clock::now() + kPollDeadline;
            bool reachedFailed = false;
            while (std::chrono::steady_clock::now() < deadline) {
                WorkflowEvent sampled;
                if (run->engine->tryLoadEvent(sampled) &&
                    sampled.kind == WorkflowEventKind::NodeFailed) {
                    sawNodeFailed = true;
                    nodeFailedId = sampled.node;
                    RIN_CHECK(!sampled.message.empty());
                }
                if (run->engine->state() == WorkflowEngineState::Failed) {
                    reachedFailed = true;
                    break;
                }
                // 无睡眠热采样：NodeFailed 与 Failed 背靠背发布（同线程、亚微秒
                // 窗口），睡眠采样必然错过（test_workflow_engine.cpp 同纪律）。
            }
            RIN_CHECK(reachedFailed);
            reachedFailedOnce = reachedFailedOnce || reachedFailed;
            RIN_CHECK(!run->engine->lastError().empty());
        }
        RIN_CHECK(reachedFailedOnce);
        RIN_CHECK_MSG(sawNodeFailed,
                      "NodeFailed event must surface on the event channel "
                      "(latest-wins mailbox; sampled across repeated failure sessions)");
        if (sawNodeFailed) {
            RIN_CHECK_EQ(nodeFailedId, std::uint64_t{2});
        }
    }

    return rin_test::exitStatus();
}
