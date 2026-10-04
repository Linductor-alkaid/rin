#include "engine.hpp"

#include "rin/image_node.hpp"

#include <kairo/comm/channel.hpp>
#include <kairo/comm/mailbox.hpp>
#include <kairo/task_cancellation.hpp>
#include <kairo/timer.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "default_catalog.hpp"
#include "param_check.hpp"

namespace rin {
namespace {

using kairo::comm::LatestMailbox;
using workflow_detail::findNodeInstance;
using workflow_detail::findParamDescriptor;
using workflow_detail::paramValueMatches;

/// 统计滚动窗口长度（NodeStats.avgCostMs / endToEndFps；与假引擎一致，契约只
/// 冻结语义）。
constexpr std::size_t kStatsWindow = 32;

double steadyMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// 节点执行失败：携带节点 id 逃逸到 future，reaper 转化为 NodeFailed/Failed
/// （归因来自 runNodeGraph 观测接缝，DEC-013）。
class WorkflowNodeFailure : public std::runtime_error {
public:
    WorkflowNodeFailure(NodeId node, const std::string& message)
        : std::runtime_error(message), node_(node) {}

    [[nodiscard]] NodeId node() const noexcept { return node_; }

private:
    NodeId node_;
};

/// Running 下 applyGraph 入队的待生效图（LatestMailbox 最新态承载；消费语义
/// 由引擎以图对象同一性判定——见 WorkflowEngine::drainBoundary）。
using PendingGraph = std::shared_ptr<const WorkflowGraph>;

/// 参数热更新命令（逐条 FIFO：不同参数的更新不得互相覆盖）。
struct ParamCommand {
    NodeId node = kInvalidNode;
    std::string paramId;
    ParamValue value;
};

kairo::comm::ChannelOptions paramChannelOptions(std::size_t capacity) {
    kairo::comm::ChannelOptions options;
    options.capacity = capacity == 0 ? 1 : capacity;
    options.drop_policy = kairo::comm::DropPolicy::RejectNewest;
    options.enable_stats = true;
    options.name = "rin.workflow.params";
    return options;
}

}  // namespace

/// M4-07 工作流真引擎（DEC-013 执行模型：帧泵采样 + 最新帧快照 + 有界在飞
/// 显式丢弃）。线程模型与假引擎一致：控制面 owner 线程、观测面 UI 线程、
/// 帧泵 tick 与帧任务在 Executor 上下文；全部跨上下文交接经 kairo::comm。
class WorkflowEngine final : public std::enable_shared_from_this<WorkflowEngine>,
                             public IWorkflowEngine {
public:
    WorkflowEngine(kairo::Executor& executor, WorkflowEngineConfig config)
        : executor_(executor),
          config_(std::move(config)),
          paramQueue_(paramChannelOptions(config_.paramQueueCapacity)) {
        if (config_.catalog.nodes.empty()) {
            config_.catalog = workflow_catalog::makeDefaultImageNodeCatalog();
        }
        if (!config_.nodeFactory) {
            config_.nodeFactory = makeDefaultImageNode;
        }
    }

    ~WorkflowEngine() override {
        // 防御：owner 未显式停止时收敛在飞任务（实例必须先于 executor shutdown
        // 消亡）。帧泵闭包只持弱引用且体内提升强引用（见 start()），与析构互斥
        // 由引用计数保证（假引擎同款纪律）。
        stop();
    }

    WorkflowEngine(const WorkflowEngine&) = delete;
    WorkflowEngine& operator=(const WorkflowEngine&) = delete;

    // --- 控制面（owner 线程） ---

    [[nodiscard]] const NodeCatalog& catalog() const override { return config_.catalog; }

    [[nodiscard]] WorkflowValidation applyGraph(const WorkflowGraph& graph) override {
        WorkflowValidation validation = validateWorkflowGraph(graph, config_.catalog);
        if (validation.ok && graph.nodes.size() > config_.maxNodes) {
            validation.ok = false;
            validation.issues.push_back(
                {ValidationIssueKind::BadParam, kInvalidNode,
                 "graph exceeds node admission limit (" + std::to_string(config_.maxNodes) + ")"});
        }
        if (!validation.ok) {
            return validation;
        }
        std::scoped_lock lock(lifecycleMutex_);
        switch (state_.load(std::memory_order_relaxed)) {
            case WorkflowEngineState::Idle:
            case WorkflowEngineState::Failed:
                pendingStart_ = std::make_shared<const WorkflowGraph>(graph);
                break;
            case WorkflowEngineState::Running:
                graphQueue_.publish(std::make_shared<const WorkflowGraph>(graph));
                break;
            case WorkflowEngineState::Stopping:
                // 同线程模型下不可达（stop() 为 owner 线程同步调用）；防御性显式拒绝。
                validation.ok = false;
                validation.issues.push_back(
                    {ValidationIssueKind::BadParam, kInvalidNode, "engine is stopping"});
                break;
        }
        return validation;
    }

    bool requestParamUpdate(NodeId node, const std::string& paramId, const ParamValue& value,
                            std::string* error) override {
        std::scoped_lock lock(lifecycleMutex_);
        const WorkflowEngineState current = state_.load(std::memory_order_relaxed);
        if (current == WorkflowEngineState::Stopping) {
            if (error != nullptr) {
                *error = "engine is stopping";
            }
            return false;
        }

        // 校验目标图：Running 取最新待生效图（其次生效图基底）；Idle/Failed 取
        // 待运行图。
        std::shared_ptr<const WorkflowGraph> target;
        if (current == WorkflowEngineState::Running) {
            PendingGraph queued;
            if (graphQueue_.try_load(queued)) {
                target = std::move(queued);
            } else if (const GenerationPtr effective = effective_.load()) {
                target = effective->sourceGraph;
            }
        } else {
            target = pendingStart_;
        }
        if (target == nullptr) {
            if (error != nullptr) {
                *error = "no graph applied";
            }
            return false;
        }
        const NodeInstance* instance = findNodeInstance(*target, node);
        if (instance == nullptr) {
            if (error != nullptr) {
                *error = "unknown node in target graph";
            }
            return false;
        }
        const NodeDescriptor* descriptor = findNodeDescriptor(config_.catalog, instance->typeId);
        const ParamDescriptor* param =
            descriptor != nullptr ? findParamDescriptor(*descriptor, paramId) : nullptr;
        if (param == nullptr) {
            if (error != nullptr) {
                *error = "unknown param: " + paramId;
            }
            return false;
        }
        if (!paramValueMatches(*param, value)) {
            if (error != nullptr) {
                *error = "param value mismatches declaration: " + paramId;
            }
            return false;
        }

        if (current == WorkflowEngineState::Running) {
            // 预编译校验（DEC-013）：参数落在目标图后图必须仍可构建（如带通
            // lowCut ≥ highCut 的工厂期拒绝在受理时同步暴露），避免帧边界重建
            // 失败进入重试风暴。
            WorkflowGraph candidate = *target;
            const bool applied = applyParamToGraph(candidate, node, paramId, value, false);
            if (!applied || !compileOrMessage(candidate, error)) {
                if (error != nullptr && error->empty()) {
                    *error = "param update rejected: graph build failed";
                }
                return false;
            }
            ParamCommand command;
            command.node = node;
            command.paramId = paramId;
            command.value = value;
            if (!paramQueue_.try_send(std::move(command))) {
                if (error != nullptr) {
                    *error = "param command queue full";
                }
                return false;
            }
            // ParamUpdated 事件由帧边界应用时发布（"下一帧生效"）。
        } else {
            // Idle/Failed：无执行上下文，直接改待运行图并即时报告（同步生效）；
            // 不可构建的修改同步拒绝（start 期才会重建，缺陷提前到受理时暴露）。
            WorkflowGraph updated = *pendingStart_;
            const bool applied = applyParamToGraph(updated, node, paramId, value, false);
            if (!applied || !compileOrMessage(updated, error)) {
                if (error != nullptr && error->empty()) {
                    *error = "param update rejected: graph build failed";
                }
                return false;
            }
            pendingStart_ = std::make_shared<const WorkflowGraph>(std::move(updated));
            publishEvent(WorkflowEventKind::ParamUpdated, node, "param updated: " + paramId);
        }
        return true;
    }

    [[nodiscard]] AdmissionResult start() override {
        std::scoped_lock lock(lifecycleMutex_);
        AdmissionResult result;
        if (state_.load(std::memory_order_relaxed) != WorkflowEngineState::Idle) {
            result.error = "cannot start from state " +
                           std::string(toString(state_.load(std::memory_order_relaxed)));
            return result;
        }
        if (pendingStart_ == nullptr) {
            result.error = "no graph applied";
            return result;
        }

        // 新会话统计复位（Idle 下无任务/tick，无并发写者）；帧源序号、会话
        // 产物与发布序复位（统计通道序号保持实例内单调）。
        {
            std::scoped_lock statsLock(statsMutex_);
            nodeStats_.clear();
            frameTimes_.clear();
            processedFrames_ = 0;
        }
        {
            std::scoped_lock publishLock(publishMutex_);
            pendingPublish_.clear();
            nextPublishSeq_ = 1;
        }
        droppedFrames_.store(0, std::memory_order_relaxed);
        submitSeq_ = 0;
        staged_.clear();
        committedSeq_.clear();
        consumedQueued_.reset();

        GenerationPtr generation = buildGeneration(*pendingStart_);
        if (generation == nullptr) {
            // 工厂期失败（参数依赖的构造拒绝等）：准入显式失败，保持 Idle。
            result.error =
                buildFailureMessage_.empty() ? "graph build failed" : buildFailureMessage_;
            buildFailureMessage_.clear();
            return result;
        }
        effective_.store(std::move(generation));

        state_.store(WorkflowEngineState::Running, std::memory_order_relaxed);
        timerHandle_ = executor_.submit_periodic_cancellable(
            static_cast<std::int64_t>(config_.pumpInterval.count()),
            // 掉队 tick 生命周期闭合：闭包只持弱引用（假引擎同款，见 M5-08）。
            [weak =
                 std::weak_ptr<WorkflowEngine>(shared_from_this())](kairo::StopToken tickToken) {
                if (std::shared_ptr<WorkflowEngine> self = weak.lock()) {
                    self->tick(tickToken);
                }
            });
        if (!timerHandle_.valid()) {
            // 帧泵准入失败：显式回滚，不进入 Running。
            state_.store(WorkflowEngineState::Idle, std::memory_order_relaxed);
            effective_.store(nullptr);
            result.error = "frame pump admission failed";
            return result;
        }
        publishEvent(WorkflowEventKind::Started, kInvalidNode, "workflow started");
        result.admitted = true;
        return result;
    }

    void stop() override {
        std::scoped_lock lock(lifecycleMutex_);
        if (state_.load(std::memory_order_relaxed) == WorkflowEngineState::Idle) {
            return;  // 幂等快路径。
        }
        state_.store(WorkflowEngineState::Stopping, std::memory_order_relaxed);
        if (timerHandle_.valid()) {
            timerHandle_.cancel();  // 阻止后续 tick；已派发 tick 由状态检查短路。
        }
        // 排空在飞：排队/协作取消，然后消费全部 future（阻塞至回收完成，EXEC-04）。
        for (auto& entry : inflight_) {
            (void)executor_.request_task_cancel(entry->handle);
        }
        std::vector<std::unique_ptr<InFlight>> draining = std::move(inflight_);
        inflight_.clear();
        inFlightCount_.store(0, std::memory_order_relaxed);
        for (auto& entry : draining) {
            std::exception_ptr error;
            try {
                entry->future.get();
            } catch (...) {
                error = std::current_exception();
            }
            if (error != nullptr) {
                consumeFailureDuringStop(std::move(error));
            }
        }
        {
            // 排空后清理未及发布的有序化缓冲（如失败帧阻塞的早帧）。
            std::scoped_lock publishLock(publishMutex_);
            pendingPublish_.clear();
            nextPublishSeq_ = 1;
        }
        state_.store(WorkflowEngineState::Idle, std::memory_order_relaxed);
        publishEvent(WorkflowEventKind::Stopped, kInvalidNode, "workflow stopped");
    }

    // --- 观测面（UI 线程，非阻塞） ---

    [[nodiscard]] WorkflowEngineState state() const override {
        return state_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::string lastError() const override {
        std::scoped_lock lock(errorMutex_);
        return lastError_;
    }

    [[nodiscard]] bool tryLoadStats(std::uint64_t& lastSeenSequence, WorkflowStats& out) override {
        return stats_.try_load_newer_than(lastSeenSequence, out, lastSeenSequence);
    }

    [[nodiscard]] bool tryLoadNodeOutput(NodeId node, std::uint64_t& lastSeenSequence,
                                         NodeOutputSnapshot& out) override {
        const GenerationPtr generation = effective_.load();
        if (generation == nullptr) {
            return false;
        }
        const auto it = generation->outputs.find(node);
        if (it == generation->outputs.end()) {
            return false;  // 不在当前生效图（图重建后移除/未运行）。
        }
        return it->second->try_load_newer_than(lastSeenSequence, out, lastSeenSequence);
    }

    [[nodiscard]] bool tryLoadEvent(WorkflowEvent& out) override { return events_.try_load(out); }

private:
    /// 生效图一代：图基底（参数应用后的图值，参数边界重建的基底）+ 编译图 +
    /// 每节点最新产物邮箱（有界：每节点仅最新一幅，M4-09 保留语义）。图替换
    /// 时整体换代；在飞任务持有旧代 shared_ptr 完成当帧（迟到的旧代发布不进
    /// 新代）。M7-02：产物邮箱跨代共享——同一节点 id 在换代（图替换/参数重建）
    /// 时沿用上一代邮箱（shared_ptr 共享同一对象），发布序号跨代连续，UI 的
    /// "上次已见序号"水位语义跨代成立（此前每代新建邮箱使序号从头计，运行中
    /// 改图/热更新参数后 UI 水位过滤掉全部新快照，面板永久显示旧代陈旧产物）；
    /// 被移除节点的邮箱随旧代释放，新节点取全新邮箱。
    struct Generation {
        std::shared_ptr<const WorkflowGraph> sourceGraph;  /// 构建来源图值。
        std::unique_ptr<NodeGraph> compiled;               /// 稳定拓扑序编译图。
        /// 含有状态节点（M10/DEC-020）：串行在飞——上一帧任务未完成时跳过
        /// 本帧提交（staged 最新帧语义，不计过载丢弃），保证 apply 按帧序。
        bool serialExecution = false;
        std::vector<NodeId> injectableNodes;  /// 注入型源节点（拓扑序）。
        std::unordered_map<NodeId, std::shared_ptr<LatestMailbox<NodeOutputSnapshot>>> outputs;
    };
    using GenerationPtr = std::shared_ptr<const Generation>;

    struct InFlight {
        kairo::TaskHandle handle;
        std::future<void> future;
    };

    /// 已完成待按提交序发布的帧（缓冲 ≤ maxInFlight，executeFrame 有界持有）。
    struct PendingPublish {
        GenerationPtr generation;
        std::shared_ptr<const std::unordered_map<NodeId, WorkflowFrameInput>> inputs;
        std::vector<std::vector<ImageU8>> outputs;
    };

    struct NodeRunStats {
        std::deque<double> window;
        double lastCostMs = 0.0;
        std::uint64_t executedFrames = 0;
    };

    /// 编译图（工厂期校验）；失败时向 error 填充首个问题并返回 false。
    [[nodiscard]] bool compileOrMessage(const WorkflowGraph& graph, std::string* error) {
        NodeGraphBuild build =
            buildNodeGraph(graph, config_.catalog, config_.nodeFactory, config_.maxNodes);
        if (build.graph != nullptr) {
            return true;
        }
        if (error != nullptr) {
            std::string message = "graph build failed";
            for (const ValidationIssue& issue : build.validation.issues) {
                message += ": ";
                message += issue.message;
                break;  // 首个问题足够定位；全部问题在 applyGraph 校验路径可见。
            }
            *error = message;
        }
        return false;
    }

    /// 编译并装配一代；失败返回 nullptr（buildFailureMessage_ 临时携带原因）。
    /// previous 为上一代（start 期为空）：同 id 节点沿用其产物邮箱（跨代序号
    /// 连续，见 Generation 注释），新节点取全新邮箱。调用方持有 lifecycleMutex_
    /// 且传递的 previous 即 effective_ 当前值——共享（不移除）语义对并发读取
    /// 者（tryLoadNodeOutput 持旧代 shared_ptr）无数据竞争。
    [[nodiscard]] GenerationPtr buildGeneration(const WorkflowGraph& graph,
                                                const GenerationPtr& previous = {}) {
        NodeGraphBuild build =
            buildNodeGraph(graph, config_.catalog, config_.nodeFactory, config_.maxNodes);
        if (build.graph == nullptr) {
            std::string message = "graph build failed";
            for (const ValidationIssue& issue : build.validation.issues) {
                message += ": ";
                message += issue.message;
                break;
            }
            buildFailureMessage_ = std::move(message);
            return nullptr;
        }
        auto generation = std::make_shared<Generation>();
        generation->sourceGraph = std::make_shared<const WorkflowGraph>(graph);
        generation->compiled = std::move(build.graph);
        for (const NodeGraph::Node& node : generation->compiled->nodes()) {
            if (node.impl != nullptr &&
                dynamic_cast<const IStatefulImageNode*>(node.impl.get()) != nullptr) {
                generation->serialExecution = true;
            }
            if (node.impl == nullptr && node.descriptor->outputs.size() == 1) {
                generation->injectableNodes.push_back(node.id);
            }
            if (previous != nullptr) {
                if (const auto carried = previous->outputs.find(node.id);
                    carried != previous->outputs.end()) {
                    generation->outputs[node.id] = carried->second;  // 跨代共享同一邮箱。
                    continue;
                }
            }
            generation->outputs[node.id] = std::make_shared<LatestMailbox<NodeOutputSnapshot>>(
                "rin.workflow.node." + std::to_string(node.id));
        }
        return generation;
    }

    // --- 帧泵（tick 在 Executor 周期任务上下文；持 lifecycleMutex_ 贯穿，
    //        与 stop()/applyGraph()/requestParamUpdate() 互斥） ---

    void tick(kairo::StopToken /*tickToken*/) {
        std::scoped_lock lock(lifecycleMutex_);
        if (state_.load(std::memory_order_relaxed) != WorkflowEngineState::Running) {
            return;
        }
        reapFinished();
        if (state_.load(std::memory_order_relaxed) != WorkflowEngineState::Running) {
            return;  // reap 触发 Failed。
        }

        drainBoundary();

        const GenerationPtr current = effective_.load();
        if (current == nullptr) {
            return;
        }

        // 捕获新帧（帧泵是帧源唯一调用方：探测即消费，单线程串行）。帧源
        // 契约为非阻塞、不抛出（转换错误应在实现内消化为返回 false）；抛出
        // 属源实现缺陷，显式转引擎失败路径，不让异常逃逸进 timer 任务体。
        bool hasNew = false;
        try {
            for (const NodeId id : current->injectableNodes) {
                std::uint64_t lastSeen = 0;
                if (const auto it = committedSeq_.find(id); it != committedSeq_.end()) {
                    lastSeen = it->second;
                }
                WorkflowFrameInput input;
                if (config_.frameSource(id, lastSeen, input) && input.image.valid()) {
                    hasNew = true;
                    committedSeq_[id] = input.sourceSequence;
                    staged_[id] = std::move(input);
                }
            }
        } catch (const std::exception& e) {
            reportFailure(kInvalidNode, std::string("frame source failed: ") + e.what());
            return;
        } catch (...) {
            reportFailure(kInvalidNode, "frame source failed with unknown exception");
            return;
        }
        if (!hasNew) {
            return;  // 无新帧：廉价探测，不提交（帧源空闲语义）。
        }
        // 串行在飞门（M10/DEC-020，IStatefulImageNode）：上一帧任务未完成时
        // 跳过本帧提交——staged 保留最新帧，下一 tick 重试；不计过载丢弃
        // （帧未被消费能力接受，非过载），保证状态节点 apply 按帧序。
        if (current->serialExecution && !inflight_.empty()) {
            return;
        }
        // 覆盖检查：生效图必须全部源节点有捕获输入（待生效图的新源节点尚未
        // 产出时图不可运行；此时不提交也不计入过载丢弃——帧未被消费能力
        // 接受，非过载）。
        for (const NodeId id : current->injectableNodes) {
            if (staged_.find(id) == staged_.end()) {
                return;
            }
        }
        // 有界在飞准入（EXEC-07）：满载对捕获的新帧显式丢弃并计数。
        if (inflight_.size() >= config_.maxInFlight) {
            droppedFrames_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto inputs =
            std::make_shared<const std::unordered_map<NodeId, WorkflowFrameInput>>(staged_);
        const std::uint64_t frameSeq = ++submitSeq_;
        kairo::TaskSubmission<void> submission = executor_.submit_cancellable(
            [this, current, inputs, frameSeq](kairo::StopToken token) {
                executeFrame(current, inputs, frameSeq, token);
            });
        if (!submission.handle.valid()) {
            // 提交拒绝显式化：计数 + 事件，不静默重试。
            droppedFrames_.fetch_add(1, std::memory_order_relaxed);
            publishEvent(WorkflowEventKind::Info, kInvalidNode,
                         "frame submit rejected by executor");
            return;
        }
        auto entry = std::make_unique<InFlight>();
        entry->handle = std::move(submission.handle);
        entry->future = std::move(submission.future);
        inflight_.push_back(std::move(entry));
        inFlightCount_.store(inflight_.size(), std::memory_order_relaxed);
    }

    /// 帧边界排空（DEC-013）：先图替换（消费式——按图对象同一性判定，仅实际
    /// 变化时重建换代并发布一次 GraphApplied），后参数命令（逐条 FIFO 全部
    /// 应用，重建生效）。工厂期构建失败显式发布 Info 并消费该图（不重试风暴）。
    void drainBoundary() {
        PendingGraph queued;
        const bool hasQueued = graphQueue_.try_load(queued);
        const bool graphNew = hasQueued && queued != consumedQueued_;
        if (graphNew) {
            GenerationPtr rebuilt = buildGeneration(*queued, effective_.load());
            if (rebuilt != nullptr) {
                promote(std::move(rebuilt));
                publishEvent(WorkflowEventKind::GraphApplied, kInvalidNode,
                             "graph applied at frame boundary");
            } else {
                publishEvent(WorkflowEventKind::Info, kInvalidNode,
                             buildFailureMessage_.empty()
                                 ? "graph rejected at frame boundary"
                                 : "graph rejected at frame boundary: " + buildFailureMessage_);
                buildFailureMessage_.clear();
            }
            consumedQueued_ = std::move(queued);  // 成功或显式拒绝都消费。
        }

        const GenerationPtr current = effective_.load();
        if (current == nullptr) {
            return;
        }
        std::vector<ParamCommand> commands;
        ParamCommand command;
        while (paramQueue_.try_receive(command)) {
            commands.push_back(std::move(command));
        }
        if (commands.empty()) {
            return;
        }
        WorkflowGraph next = *current->sourceGraph;
        std::size_t applied = 0;
        for (ParamCommand& cmd : commands) {
            if (applyParamToGraph(next, cmd.node, cmd.paramId, cmd.value, true)) {
                ++applied;
            }
        }
        if (applied == 0) {
            return;  // 全部失配已按条发布 Info；图不变不重建。
        }
        GenerationPtr rebuilt = buildGeneration(next, effective_.load());
        if (rebuilt == nullptr) {
            // 防御路径（requestParamUpdate 已预编译校验，批量组合失败才会到
            // 这里）：保持旧代显式报告，不进入重试风暴。
            publishEvent(WorkflowEventKind::Info, kInvalidNode,
                         buildFailureMessage_.empty()
                             ? "param update rejected at frame boundary"
                             : "param update rejected at frame boundary: " + buildFailureMessage_);
            buildFailureMessage_.clear();
            return;
        }
        promote(std::move(rebuilt));
    }

    /// 换代并裁剪不再存在的源节点捕获状态（有界卫生，假引擎统计擦除同款）。
    void promote(GenerationPtr generation) {
        pruneCaptureState(*generation, staged_);
        pruneCaptureState(*generation, committedSeq_);
        effective_.store(std::move(generation));
    }

    template <typename Map>
    static void pruneCaptureState(const Generation& generation, Map& map) {
        const std::vector<NodeId>& injectables = generation.injectableNodes;
        for (auto it = map.begin(); it != map.end();) {
            if (std::find(injectables.begin(), injectables.end(), it->first) == injectables.end()) {
                it = map.erase(it);
            } else {
                ++it;
            }
        }
    }

    /// 参数赋值到图（值合法性已由 requestParamUpdate 校验；帧边界应用时失配
    /// 显式丢弃并发布 Info，假引擎同款）。publishEvents=false 用于
    /// requestParamUpdate 预编译校验路径（事件在帧边界或受理点另行发布）。
    bool applyParamToGraph(WorkflowGraph& graph, NodeId node, const std::string& paramId,
                           const ParamValue& value, bool publishEvents) {
        NodeInstance* instance = findNodeInstance(graph, node);
        const NodeDescriptor* descriptor =
            instance != nullptr ? findNodeDescriptor(config_.catalog, instance->typeId) : nullptr;
        const ParamDescriptor* param =
            descriptor != nullptr ? findParamDescriptor(*descriptor, paramId) : nullptr;
        if (instance == nullptr || param == nullptr || !paramValueMatches(*param, value)) {
            publishEvent(WorkflowEventKind::Info, node,
                         "param update discarded (not applicable): " + paramId);
            return false;
        }
        for (ParamAssignment& assignment : instance->params) {
            if (assignment.paramId == paramId) {
                assignment.value = value;
                if (publishEvents) {
                    publishEvent(WorkflowEventKind::ParamUpdated, node,
                                 "param updated: " + paramId);
                }
                return true;
            }
        }
        instance->params.push_back(ParamAssignment{paramId, value});
        if (publishEvents) {
            publishEvent(WorkflowEventKind::ParamUpdated, node, "param updated: " + paramId);
        }
        return true;
    }

    // --- 帧执行（Executor 有限任务上下文） ---

    void executeFrame(
        const GenerationPtr& generation,
        const std::shared_ptr<const std::unordered_map<NodeId, WorkflowFrameInput>>& inputs,
        std::uint64_t frameSeq, kairo::StopToken token) {
        // 帧边界取消检查（DEC-013：取消在帧边界生效；已开始的帧为有界工作
        // 单元，完整完成并发布，不在节点间中断）。
        if (token.stop_requested()) {
            return;
        }

        // 逐节点观测（runNodeGraph 接缝）：耗时入统计；失败归因节点。
        NodeId failedNode = kInvalidNode;
        std::string failureMessage;
        NodeExecutionObserver observer = [this, &failedNode, &failureMessage](
                                             const NodeGraph::Node& node, double costMs,
                                             const std::exception_ptr& error) {
            if (error != nullptr) {
                try {
                    std::rethrow_exception(error);
                } catch (const std::exception& e) {
                    failedNode = node.id;
                    failureMessage = e.what();
                } catch (...) {
                    failedNode = node.id;
                    failureMessage = "unknown node failure";
                }
                return;
            }
            recordNodeCost(node.id, costMs);
        };

        SourceInjector injector = [this, inputs](const NodeGraph::Node& node) -> ImageU8 {
            // 源节点执行记账：注入点即执行点（runNodeGraph 对每个注入型节点
            // 恰调用一次注入器）。无耗时值可测（DEC-013：源节点不经耗时观测）
            // —— NodeStats.lastCostMs/avgCostMs 保持 0，executedFrames 随注入
            // 递增，与假引擎"统计覆盖全部图节点"基线一致。
            recordNodeExecuted(node.id);
            const auto it = inputs->find(node.id);
            if (it == inputs->end()) {
                // 帧泵已保证生效图源节点全覆盖；防御路径显式失败。
                throw std::runtime_error("no staged input for source node " +
                                         std::to_string(node.id));
            }
            return it->second.image;
        };

        std::vector<std::vector<ImageU8>> outputs;
        try {
            outputs = runNodeGraph(*generation->compiled, injector, observer);
        } catch (...) {
            if (failedNode != kInvalidNode) {
                throw WorkflowNodeFailure(failedNode, failureMessage);
            }
            throw;  // 系统失败（注入/防御核对）原样传播，reaper 泛化报告。
        }

        // 发布按提交序有序化（maxInFlight > 1 时帧任务可能乱序完成；缓冲有界
        // ≤ maxInFlight 帧）。失败帧不进入发布通道（引擎转 Failed，无后续帧）。
        {
            std::scoped_lock publishLock(publishMutex_);
            pendingPublish_.emplace(frameSeq,
                                    PendingPublish{generation, inputs, std::move(outputs)});
            for (auto it = pendingPublish_.find(nextPublishSeq_); it != pendingPublish_.end();
                 it = pendingPublish_.find(nextPublishSeq_)) {
                PendingPublish& frame = it->second;
                publishOutputs(frame.generation, frame.outputs, *frame.inputs);
                recordFrameCompleted(*frame.generation);
                pendingPublish_.erase(it);
                ++nextPublishSeq_;
            }
        }
    }

    /// 产物发布（执行序）：每节点最新一幅（LatestMailbox）；sourceSequence
    /// 传播语义（DEC-013）：源节点 = 注入帧序号，下游 = 各生产者的最小值。
    /// M7-02：generation 非当前生效代（提交后在飞期间发生换代）时跳过邮箱
    /// 发布、统计照记——邮箱跨代共享后，迟到的旧代帧不得以更高序号把旧图
    /// 产物写回共享邮箱（"迟到的旧代发布不进新代"语义在共享邮箱下的等价
    /// 保障；提交序有序化保证新代帧不被跳过）。
    void publishOutputs(const GenerationPtr& generation,
                        const std::vector<std::vector<ImageU8>>& outputs,
                        const std::unordered_map<NodeId, WorkflowFrameInput>& inputs) {
        if (generation != effective_.load()) {
            return;  // 过代帧：跳过发布（recordFrameCompleted 由调用方照记）。
        }
        const std::vector<NodeGraph::Node>& nodes = generation->compiled->nodes();
        std::unordered_map<NodeId, std::uint64_t> seqOf;
        seqOf.reserve(nodes.size());
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const NodeGraph::Node& node = nodes[i];
            const std::vector<ImageU8>& nodeOutputs = outputs[i];
            if (nodeOutputs.empty()) {
                continue;  // 无输出（零输出声明）：无产物可发布。
            }
            std::uint64_t sequence = 0;
            if (node.impl == nullptr) {
                const auto it = inputs.find(node.id);
                sequence = it != inputs.end() ? it->second.sourceSequence : 0;
            } else {
                bool first = true;
                for (const NodeGraph::InputEdge& edge : node.inputs) {
                    const auto it = seqOf.find(nodes[edge.producerIndex].id);
                    if (it == seqOf.end()) {
                        continue;  // 拓扑序保证生产者先发布；防御。
                    }
                    sequence = first ? it->second : std::min(sequence, it->second);
                    first = false;
                }
            }
            seqOf[node.id] = sequence;

            const auto mailbox = generation->outputs.find(node.id);
            if (mailbox == generation->outputs.end()) {
                continue;  // 代内一致，防御。
            }
            for (std::size_t port = 0; port < nodeOutputs.size(); ++port) {
                const ImageU8& image = nodeOutputs[port];
                NodeOutputSnapshot snapshot;
                snapshot.node = node.id;
                snapshot.format = image.format();
                snapshot.width = image.width();
                snapshot.height = image.height();
                snapshot.stride = image.stride();
                snapshot.sourceSequence = sequence;
                snapshot.pixels = image.pixels();
                mailbox->second->publish(std::move(snapshot));
            }
        }
    }

    // --- 帧泵任务回收与失败分类（假引擎同款语义） ---

    /// 消费已完成任务的 future（异常经 future 保持可见，不被吞掉）。
    /// 调用方持 lifecycleMutex_；只消费已就绪 future，不阻塞。
    void reapFinished() {
        for (auto it = inflight_.begin(); it != inflight_.end();) {
            if (it->get()->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++it;
                continue;
            }
            std::exception_ptr error;
            try {
                it->get()->future.get();
            } catch (...) {
                error = std::current_exception();
            }
            it = inflight_.erase(it);
            inFlightCount_.store(inflight_.size(), std::memory_order_relaxed);
            if (error != nullptr) {
                handleTaskFailure(std::move(error));
            }
        }
    }

    void handleTaskFailure(std::exception_ptr error) {
        try {
            std::rethrow_exception(std::move(error));
        } catch (const kairo::TaskCancelled&) {
            return;  // 取消不是失败（stop 排空/关停清理）。
        } catch (const WorkflowNodeFailure& failure) {
            reportFailure(failure.node(), failure.what());
            return;
        } catch (const std::exception& e) {
            reportFailure(kInvalidNode, e.what());
            return;
        }
        reportFailure(kInvalidNode, "unknown task failure");
    }

    void reportFailure(NodeId node, const std::string& message) {
        setLastError(message);
        if (node != kInvalidNode) {
            publishEvent(WorkflowEventKind::NodeFailed, node, message);
        }
        WorkflowEngineState expected = WorkflowEngineState::Running;
        if (state_.compare_exchange_strong(expected, WorkflowEngineState::Failed,
                                           std::memory_order_relaxed)) {
            if (timerHandle_.valid()) {
                timerHandle_.cancel();
            }
            publishEvent(WorkflowEventKind::Failed, kInvalidNode, message);
        }
    }

    /// 停止排空中消费异常：保持可见（lastError + 事件），但终态按 stop() 语义
    /// 收敛到 Idle，不改判 Failed。
    void consumeFailureDuringStop(std::exception_ptr error) {
        try {
            std::rethrow_exception(std::move(error));
        } catch (const kairo::TaskCancelled&) {
            return;
        } catch (const WorkflowNodeFailure& failure) {
            setLastError(failure.what());
            publishEvent(WorkflowEventKind::NodeFailed, failure.node(), failure.what());
            return;
        } catch (const std::exception& e) {
            setLastError(e.what());
            publishEvent(WorkflowEventKind::Info, kInvalidNode,
                         std::string("task failed during stop: ") + e.what());
            return;
        } catch (...) {
            // 非 std 异常不得逃出 stop()（否则 std::terminate）。
            setLastError("unknown task failure during stop");
            publishEvent(WorkflowEventKind::Info, kInvalidNode, "unknown task failure during stop");
        }
    }

    // --- 统计（帧任务线程写，start/stop 复位） ---

    void recordNodeCost(NodeId node, double costMs) {
        std::scoped_lock lock(statsMutex_);
        NodeRunStats& stats = nodeStats_[node];
        stats.lastCostMs = costMs;
        stats.window.push_back(costMs);
        if (stats.window.size() > kStatsWindow) {
            stats.window.pop_front();
        }
        ++stats.executedFrames;
    }

    /// 源节点执行计数（无耗时值；注入点记账，见 executeFrame 的注入器）。
    void recordNodeExecuted(NodeId node) {
        std::scoped_lock lock(statsMutex_);
        ++nodeStats_[node].executedFrames;
    }

    void recordFrameCompleted(const Generation& generation) {
        std::scoped_lock lock(statsMutex_);
        frameTimes_.push_back(steadyMs());
        if (frameTimes_.size() > kStatsWindow) {
            frameTimes_.pop_front();
        }
        ++processedFrames_;

        WorkflowStats snapshot;
        snapshot.sequence = ++statsSequence_;
        snapshot.endToEndFps = frameTimes_.size() >= 2
                                   ? static_cast<double>(frameTimes_.size() - 1) * 1000.0 /
                                         (frameTimes_.back() - frameTimes_.front())
                                   : 0.0;
        snapshot.processedFrames = processedFrames_;
        snapshot.droppedFrames = droppedFrames_.load(std::memory_order_relaxed);
        snapshot.inFlight = static_cast<std::uint32_t>(inFlightCount_.load());
        for (const NodeGraph::Node& node : generation.compiled->nodes()) {
            NodeStats nodeStats;
            nodeStats.node = node.id;
            const auto it = nodeStats_.find(node.id);
            if (it != nodeStats_.end()) {
                const NodeRunStats& run = it->second;
                nodeStats.lastCostMs = run.lastCostMs;
                const double sum = std::accumulate(run.window.begin(), run.window.end(), 0.0);
                nodeStats.avgCostMs =
                    run.window.empty() ? 0.0 : sum / static_cast<double>(run.window.size());
                nodeStats.executedFrames = run.executedFrames;
            }
            snapshot.nodes.push_back(nodeStats);
        }
        stats_.publish(std::move(snapshot));
    }

    // --- 基础设施 ---

    void publishEvent(WorkflowEventKind kind, NodeId node, const std::string& message) {
        WorkflowEvent event;
        event.kind = kind;
        event.node = node;
        event.message = message;
        event.timestampMs = steadyMs();
        events_.publish(std::move(event));
    }

    void setLastError(const std::string& message) {
        std::scoped_lock lock(errorMutex_);
        lastError_ = message;
    }

    kairo::Executor& executor_;
    WorkflowEngineConfig config_;

    LatestMailbox<WorkflowStats> stats_{"rin.workflow.stats"};
    LatestMailbox<WorkflowEvent> events_{"rin.workflow.events"};
    LatestMailbox<PendingGraph> graphQueue_{"rin.workflow.graph"};
    kairo::comm::MpscChannel<ParamCommand> paramQueue_;

    std::atomic<GenerationPtr> effective_{nullptr};
    std::shared_ptr<const WorkflowGraph> pendingStart_;  // owner 线程（lifecycleMutex_ 下）
    PendingGraph consumedQueued_;  // 已消费待生效图标记（lifecycleMutex_ 下）
    std::unordered_map<NodeId, WorkflowFrameInput> staged_;  // 捕获帧快照（lifecycleMutex_ 下）
    std::unordered_map<NodeId, std::uint64_t>
        committedSeq_;  // 引擎侧已消费源序号（lifecycleMutex_ 下）
    std::vector<std::unique_ptr<InFlight>> inflight_;  // lifecycleMutex_ 下
    std::atomic<std::size_t> inFlightCount_{0};
    std::atomic<std::uint64_t> droppedFrames_{0};
    std::atomic<WorkflowEngineState> state_{WorkflowEngineState::Idle};
    kairo::TimerHandle timerHandle_{};
    std::string buildFailureMessage_;  // buildGeneration 失败原因（lifecycleMutex_ 下）
    std::uint64_t submitSeq_ = 0;      // 帧提交序（lifecycleMutex_ 下）

    // 发布有序化（帧提交序 = 发布序；publishMutex_ 下）。lifecycleMutex_ 持有
    // 期间可短促获取本锁（start/stop 复位），帧任务只持本锁，无反向嵌套。
    std::mutex publishMutex_;
    std::uint64_t nextPublishSeq_ = 1;
    std::unordered_map<std::uint64_t, PendingPublish> pendingPublish_;

    mutable std::mutex errorMutex_;
    std::string lastError_;
    std::mutex lifecycleMutex_;
    std::mutex statsMutex_;
    std::unordered_map<NodeId, NodeRunStats> nodeStats_;  // statsMutex_ 下
    std::deque<double> frameTimes_;                       // statsMutex_ 下
    std::uint64_t processedFrames_ = 0;                   // statsMutex_ 下
    std::uint64_t statsSequence_ = 0;                     // statsMutex_ 下
};

}  // namespace rin

namespace rin {

std::shared_ptr<IWorkflowEngine> createWorkflowEngine(kairo::Executor& executor,
                                                      WorkflowEngineConfig config) {
    if (!config.frameSource) {
        throw std::invalid_argument("workflow engine: frameSource must be set");
    }
    if (config.maxInFlight == 0) {
        throw std::invalid_argument("workflow engine: maxInFlight must be positive");
    }
    if (config.pumpInterval.count() <= 0) {
        throw std::invalid_argument("workflow engine: pumpInterval must be positive");
    }
    return std::make_shared<WorkflowEngine>(executor, std::move(config));
}

}  // namespace rin
