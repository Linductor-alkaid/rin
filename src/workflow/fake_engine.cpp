#include "fake_engine.hpp"

#include <executor/comm/channel.hpp>
#include <executor/comm/mailbox.hpp>
#include <executor/task_cancellation.hpp>
#include <executor/timer.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <future>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rin {
namespace {

using executor::comm::LatestMailbox;

/// 统计滚动窗口长度（NodeStats.avgCostMs / endToEndFps；实现定义，契约只冻结语义）。
constexpr std::size_t kStatsWindow = 32;

double steadyMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// 参数值与声明的种类/约束匹配（规则与 validateWorkflowGraph 参数面一致）。
bool paramValueMatches(const ParamDescriptor& d, const ParamValue& value) {
    switch (d.kind) {
    case ParamKind::Boolean:
        return std::holds_alternative<bool>(value);
    case ParamKind::Integer: {
        const auto* i = std::get_if<std::int64_t>(&value);
        if (i == nullptr) {
            return false;
        }
        if (!d.hasRange) {
            return true;
        }
        // long double 比较：int64 边界不被 double 量化吞掉。
        const long double x = static_cast<long double>(*i);
        return x >= static_cast<long double>(d.minValue) &&
               x <= static_cast<long double>(d.maxValue);
    }
    case ParamKind::Real: {
        const auto* r = std::get_if<double>(&value);
        if (r == nullptr) {
            return false;
        }
        if (!d.hasRange) {
            return true;
        }
        // NaN 与任何比较均为 false，自然拒绝（hasRange 时）。
        return *r >= d.minValue && *r <= d.maxValue;
    }
    case ParamKind::Enumeration: {
        const auto* s = std::get_if<std::string>(&value);
        if (s == nullptr) {
            return false;
        }
        return std::find(d.enumOptions.begin(), d.enumOptions.end(), *s) !=
               d.enumOptions.end();
    }
    case ParamKind::RealArray: {
        const auto* a = std::get_if<std::vector<double>>(&value);
        if (a == nullptr) {
            return false;
        }
        return std::all_of(a->begin(), a->end(),
                           [](double x) { return std::isfinite(x); });
    }
    }
    return false;
}

const ParamDescriptor* findParam(const NodeDescriptor& descriptor,
                                 const std::string& paramId) {
    for (const ParamDescriptor& p : descriptor.params) {
        if (p.id == paramId) {
            return &p;
        }
    }
    return nullptr;
}

/// 节点执行失败（注入）：携带节点 id 逃逸到 future，reaper 转化为 NodeFailed/Failed。
class FakeNodeFailure : public std::runtime_error {
public:
    FakeNodeFailure(NodeId node, const std::string& message)
        : std::runtime_error(message), node_(node) {}

    [[nodiscard]] NodeId node() const noexcept { return node_; }

private:
    NodeId node_;
};

/// Running 下 applyGraph 入队的待生效图（最新态：只保留最新一张）。
using PendingGraph = std::shared_ptr<const WorkflowGraph>;

/// 参数热更新命令（逐条 FIFO：不同参数的更新不得互相覆盖）。
struct ParamCommand {
    NodeId node = kInvalidNode;
    std::string paramId;
    ParamValue value;
};

executor::comm::ChannelOptions paramChannelOptions(std::size_t capacity) {
    executor::comm::ChannelOptions options;
    options.capacity = capacity == 0 ? 1 : capacity;
    options.drop_policy = executor::comm::DropPolicy::RejectNewest;
    options.enable_stats = true;
    options.name = "rin.workflow.params";
    return options;
}

/// 仿真逐节点耗时的确定性缺省表（毫秒）：按类型基数 + 帧序/节点派生抖动。
double defaultSimulatedCost(const NodeInstance& node, std::uint64_t frameIndex) {
    double base = 1.0;
    const std::string& type = node.typeId;
    if (type == "source") {
        base = 0.5;
    } else if (type == "crop") {
        base = 0.4;
    } else if (type == "downscale") {
        base = 0.9;
    } else if (type == "grayify") {
        base = 0.3;
    } else if (type == "gaussian_blur") {
        base = 1.4;
    } else if (type == "conv_kernel") {
        base = 2.2;
    } else if (type == "hist_eq") {
        base = 1.1;
    } else if (type == "fft_lowpass" || type == "fft_highpass" ||
               type == "fft_bandpass") {
        base = 4.5;
    }
    return base + static_cast<double>((frameIndex * 37u + node.id * 17u) % 11u) * 0.05;
}

}  // namespace

/// M4 调色板假目录：签名与参数 schema 按 M4 范围冻结（DEC-012 未冻结前的 UI
/// 开发基线；算子数值语义归 M4 真引擎，假引擎只产出合成图案）。
NodeCatalog makeDefaultFakeCatalog() {
    NodeCatalog catalog;

    NodeDescriptor source;
    source.typeId = "source";
    source.displayName = "相机源";
    source.outputs = {PortType::Rgba8};
    catalog.nodes.push_back(std::move(source));

    NodeDescriptor crop;
    crop.typeId = "crop";
    crop.displayName = "裁切";
    crop.inputs = {PortType::Rgba8};
    crop.outputs = {PortType::Rgba8};
    ParamDescriptor cropX;
    cropX.id = "x";
    cropX.label = "X";
    cropX.kind = ParamKind::Integer;
    cropX.defaultValue = static_cast<std::int64_t>(0);
    cropX.hasRange = true;
    cropX.minValue = 0.0;
    cropX.maxValue = 4096.0;
    ParamDescriptor cropY = cropX;
    cropY.id = "y";
    cropY.label = "Y";
    ParamDescriptor cropW = cropX;
    cropW.id = "width";
    cropW.label = "宽";
    cropW.defaultValue = static_cast<std::int64_t>(64);
    ParamDescriptor cropH = cropW;
    cropH.id = "height";
    cropH.label = "高";
    crop.params = {cropX, cropY, cropW, cropH};
    catalog.nodes.push_back(std::move(crop));

    NodeDescriptor downscale;
    downscale.typeId = "downscale";
    downscale.displayName = "降分辨率";
    downscale.inputs = {PortType::Rgba8};
    downscale.outputs = {PortType::Rgba8};
    ParamDescriptor interpolation;
    interpolation.id = "interpolation";
    interpolation.label = "插值";
    interpolation.kind = ParamKind::Enumeration;
    interpolation.defaultValue = std::string("nearest");
    interpolation.enumOptions = {"nearest", "bilinear"};
    ParamDescriptor scale;
    scale.id = "scale";
    scale.label = "缩放";
    scale.kind = ParamKind::Real;
    scale.defaultValue = 0.5;
    scale.hasRange = true;
    scale.minValue = 0.1;
    scale.maxValue = 1.0;
    downscale.params = {interpolation, scale};
    catalog.nodes.push_back(std::move(downscale));

    NodeDescriptor grayify;
    grayify.typeId = "grayify";
    grayify.displayName = "灰度化";
    grayify.inputs = {PortType::Rgba8};
    grayify.outputs = {PortType::Gray8};
    catalog.nodes.push_back(std::move(grayify));

    NodeDescriptor blur;
    blur.typeId = "gaussian_blur";
    blur.displayName = "高斯模糊";
    blur.inputs = {PortType::Gray8};
    blur.outputs = {PortType::Gray8};
    ParamDescriptor radius;
    radius.id = "radius";
    radius.label = "半径";
    radius.kind = ParamKind::Integer;
    radius.defaultValue = static_cast<std::int64_t>(3);
    radius.hasRange = true;
    radius.minValue = 1.0;
    radius.maxValue = 10.0;
    ParamDescriptor sigma;
    sigma.id = "sigma";
    sigma.label = "sigma";
    sigma.kind = ParamKind::Real;
    sigma.defaultValue = 1.5;
    sigma.hasRange = true;
    sigma.minValue = 0.0;
    sigma.maxValue = 10.0;
    blur.params = {radius, sigma};
    catalog.nodes.push_back(std::move(blur));

    NodeDescriptor conv;
    conv.typeId = "conv_kernel";
    conv.displayName = "自定义卷积";
    conv.inputs = {PortType::Gray8};
    conv.outputs = {PortType::Gray8};
    ParamDescriptor kernelSize;
    kernelSize.id = "size";
    kernelSize.label = "核尺寸";
    kernelSize.kind = ParamKind::Enumeration;
    kernelSize.defaultValue = std::string("3");
    kernelSize.enumOptions = {"1", "3", "5"};
    ParamDescriptor kernel;
    kernel.id = "kernel";
    kernel.label = "卷积核";
    kernel.kind = ParamKind::RealArray;
    // 3x3 单位核（行主序），中心为 1。
    kernel.defaultValue = std::vector<double>{0, 0, 0, 0, 1, 0, 0, 0, 0};
    conv.params = {kernelSize, kernel};
    catalog.nodes.push_back(std::move(conv));

    NodeDescriptor histEq;
    histEq.typeId = "hist_eq";
    histEq.displayName = "直方图均衡";
    histEq.inputs = {PortType::Gray8};
    histEq.outputs = {PortType::Gray8};
    catalog.nodes.push_back(std::move(histEq));

    NodeDescriptor fftLow;
    fftLow.typeId = "fft_lowpass";
    fftLow.displayName = "FFT 低通";
    fftLow.inputs = {PortType::Gray8};
    fftLow.outputs = {PortType::Gray8};
    ParamDescriptor lowCutoff;
    lowCutoff.id = "cutoff";
    lowCutoff.label = "截止";
    lowCutoff.kind = ParamKind::Real;
    lowCutoff.defaultValue = 0.2;
    lowCutoff.hasRange = true;
    lowCutoff.minValue = 0.0;
    lowCutoff.maxValue = 1.0;
    fftLow.params = {lowCutoff};
    catalog.nodes.push_back(std::move(fftLow));

    NodeDescriptor fftHigh;
    fftHigh.typeId = "fft_highpass";
    fftHigh.displayName = "FFT 高通";
    fftHigh.inputs = {PortType::Gray8};
    fftHigh.outputs = {PortType::Gray8};
    fftHigh.params = {lowCutoff};
    catalog.nodes.push_back(std::move(fftHigh));

    NodeDescriptor fftBand;
    fftBand.typeId = "fft_bandpass";
    fftBand.displayName = "FFT 带通";
    fftBand.inputs = {PortType::Gray8};
    fftBand.outputs = {PortType::Gray8};
    ParamDescriptor bandLow;
    bandLow.id = "lowCut";
    bandLow.label = "下限截止";
    bandLow.kind = ParamKind::Real;
    bandLow.defaultValue = 0.2;
    bandLow.hasRange = true;
    bandLow.minValue = 0.0;
    bandLow.maxValue = 1.0;
    ParamDescriptor bandHigh = bandLow;
    bandHigh.id = "highCut";
    bandHigh.label = "上限截止";
    bandHigh.defaultValue = 0.6;
    fftBand.params = {bandLow, bandHigh};
    catalog.nodes.push_back(std::move(fftBand));

    return catalog;
}

class FakeWorkflowEngine final : public IWorkflowEngine {
public:
    FakeWorkflowEngine(executor::Executor& executor, FakeWorkflowEngineConfig config)
        : executor_(executor),
          config_(std::move(config)),
          paramQueue_(paramChannelOptions(config_.paramQueueCapacity)) {
        if (config_.catalog.nodes.empty()) {
            config_.catalog = makeDefaultFakeCatalog();
        }
    }

    ~FakeWorkflowEngine() override {
        // 防御：owner 未显式停止时收敛在飞任务（实例必须先于 executor shutdown 消亡）。
        stop();
    }

    FakeWorkflowEngine(const FakeWorkflowEngine&) = delete;
    FakeWorkflowEngine& operator=(const FakeWorkflowEngine&) = delete;

    // --- 控制面（owner 线程） ---

    [[nodiscard]] const NodeCatalog& catalog() const override { return config_.catalog; }

    [[nodiscard]] WorkflowValidation applyGraph(const WorkflowGraph& graph) override {
        WorkflowValidation validation = validateWorkflowGraph(graph, config_.catalog);
        if (validation.ok && graph.nodes.size() > config_.maxNodes) {
            validation.ok = false;
            validation.issues.push_back(
                {ValidationIssueKind::BadParam, kInvalidNode,
                 "graph exceeds node admission limit (" +
                     std::to_string(config_.maxNodes) + ")"});
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

    bool requestParamUpdate(NodeId node, const std::string& paramId,
                            const ParamValue& value, std::string* error) override {
        std::scoped_lock lock(lifecycleMutex_);
        const WorkflowEngineState current = state_.load(std::memory_order_relaxed);
        if (current == WorkflowEngineState::Stopping) {
            if (error != nullptr) {
                *error = "engine is stopping";
            }
            return false;
        }

        // 校验目标图：Running 取最新待生效图（其次生效图）；Idle/Failed 取待运行图。
        const WorkflowGraph* target = nullptr;
        if (current == WorkflowEngineState::Running) {
            PendingGraph queued;
            if (graphQueue_.try_load(queued)) {
                target = queued.get();
            } else if (const GenerationPtr effective = effective_.load()) {
                target = &effective->graph;
            }
        } else if (pendingStart_ != nullptr) {
            target = pendingStart_.get();
        }
        if (target == nullptr) {
            if (error != nullptr) {
                *error = "no graph applied";
            }
            return false;
        }
        const NodeInstance* instance = findNode(*target, node);
        if (instance == nullptr) {
            if (error != nullptr) {
                *error = "unknown node in target graph";
            }
            return false;
        }
        const NodeDescriptor* descriptor =
            findNodeDescriptor(config_.catalog, instance->typeId);
        const ParamDescriptor* param =
            descriptor != nullptr ? findParam(*descriptor, paramId) : nullptr;
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
            // Idle/Failed：无执行上下文，直接改待运行图并即时报告（同步生效）。
            WorkflowGraph updated = *pendingStart_;
            applyParamToGraph(updated, node, paramId, value);
            pendingStart_ = std::make_shared<const WorkflowGraph>(std::move(updated));
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

        // 新会话统计复位（Idle 下无任务/tick，无并发写者）。
        {
            std::scoped_lock statsLock(statsMutex_);
            nodeStats_.clear();
            frameTimes_.clear();
            processedFrames_ = 0;
        }
        droppedFrames_.store(0, std::memory_order_relaxed);
        effective_.store(buildGeneration(*pendingStart_));

        state_.store(WorkflowEngineState::Running, std::memory_order_relaxed);
        timerHandle_ = executor_.submit_periodic_cancellable_with_handle(
            static_cast<std::int64_t>(config_.frameInterval.count()),
            [this](executor::StopToken tickToken) { tick(tickToken); });
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

    [[nodiscard]] bool tryLoadStats(std::uint64_t& lastSeenSequence,
                                    WorkflowStats& out) override {
        return stats_.try_load_newer_than(lastSeenSequence, out, lastSeenSequence);
    }

    [[nodiscard]] bool tryLoadNodeOutput(NodeId node,
                                         std::uint64_t& lastSeenSequence,
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

    [[nodiscard]] bool tryLoadEvent(WorkflowEvent& out) override {
        return events_.try_load(out);
    }

private:
    /// 生效图一代：图 + 拓扑序 + 每节点最新产物邮箱（有界：每节点仅最新一幅）。
    /// 图替换时整体换代，任务持有旧代 shared_ptr 完成当帧（迟到的旧代发布不进新代）。
    struct Generation {
        WorkflowGraph graph;
        std::vector<NodeId> topo;
        std::unordered_map<NodeId,
                           std::unique_ptr<LatestMailbox<NodeOutputSnapshot>>>
            outputs;

        [[nodiscard]] const NodeInstance* node(NodeId id) const {
            for (const NodeInstance& instance : graph.nodes) {
                if (instance.id == id) {
                    return &instance;
                }
            }
            return nullptr;
        }
    };
    using GenerationPtr = std::shared_ptr<const Generation>;

    struct InFlight {
        executor::TaskHandle handle;
        std::future<void> future;
    };

    struct NodeRunStats {
        std::deque<double> window;
        double lastCostMs = 0.0;
        std::uint64_t executedFrames = 0;
    };

    // --- 帧泵（tick 在 Executor 周期任务上下文；持 lifecycleMutex_ 贯穿提交，
    //        与 stop() 互斥，杜绝"stop 返回后仍有新提交/新发布"） ---

    void tick(executor::StopToken /*tickToken*/) {
        std::scoped_lock lock(lifecycleMutex_);
        if (state_.load(std::memory_order_relaxed) != WorkflowEngineState::Running) {
            return;
        }
        reapFinished();
        if (state_.load(std::memory_order_relaxed) != WorkflowEngineState::Running) {
            return;  // reap 触发 Failed。
        }
        if (inflight_.size() >= config_.maxInFlight) {
            droppedFrames_.fetch_add(1, std::memory_order_relaxed);  // 显式丢弃（EXEC-07）。
            return;
        }
        const GenerationPtr generation = effective_.load();
        if (generation == nullptr) {
            return;
        }
        const std::uint64_t frameIndex = frameCounter_.fetch_add(1) + 1;
        executor::TaskSubmission<void> submission = executor_.submit_cancellable(
            [this, generation, frameIndex](executor::StopToken token) {
                executeFrame(generation, frameIndex, token);
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

    /// 消费已完成任务的 future（异常经 future 保持可见，不被吞掉）。
    /// 调用方持 lifecycleMutex_；只消费已就绪 future，不阻塞。
    void reapFinished() {
        for (auto it = inflight_.begin(); it != inflight_.end();) {
            if (it->get()->future.wait_for(std::chrono::seconds(0)) !=
                std::future_status::ready) {
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
        } catch (const executor::TaskCancelled&) {
            return;  // 取消不是失败（stop 排空/关停清理）。
        } catch (const FakeNodeFailure& failure) {
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
        } catch (const executor::TaskCancelled&) {
            return;
        } catch (const FakeNodeFailure& failure) {
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
            publishEvent(WorkflowEventKind::Info, kInvalidNode,
                         "unknown task failure during stop");
        }
    }

    // --- 帧执行（Executor 有限任务上下文） ---

    void executeFrame(const GenerationPtr& scheduled, std::uint64_t frameIndex,
                      executor::StopToken token) {
        // 帧边界：排空待生效图与参数命令（"下一帧生效"语义），可能换代。
        const GenerationPtr effective = drainBoundary(scheduled);
        if (token.stop_requested()) {
            return;  // 协作取消：静默退出，无产物发布。
        }

        std::unordered_map<NodeId, NodeOutputSnapshot> produced;
        for (const NodeId id : effective->topo) {
            if (token.stop_requested()) {
                return;
            }
            const NodeInstance* instance = effective->node(id);
            if (instance == nullptr) {
                continue;  // 代内一致，防御。
            }
            if (config_.injectNodeFailure != nullptr &&
                config_.injectNodeFailure(*instance, frameIndex)) {
                throw FakeNodeFailure(instance->id, "node failed: " + instance->typeId);
            }
            const NodeOutputSnapshot snapshot =
                synthesize(*instance, *effective, produced, frameIndex);
            produced.emplace(instance->id, snapshot);
            const auto it = effective->outputs.find(instance->id);
            if (it != effective->outputs.end()) {
                it->second->publish(snapshot);
            }
            recordNodeCost(instance->id, simulatedCost(*instance, frameIndex));
        }
        recordFrameCompleted(*effective);
    }

    /// 帧边界排空：图替换（最新态，只取最新）+ 参数命令（逐条 FIFO，全部应用）。
    /// 返回当帧继续使用的代（可能为新代）。
    [[nodiscard]] GenerationPtr drainBoundary(const GenerationPtr& current) {
        PendingGraph queued;
        const bool hasGraph = graphQueue_.try_load(queued);
        std::vector<ParamCommand> commands;
        ParamCommand command;
        while (paramQueue_.try_receive(command)) {
            commands.push_back(std::move(command));
        }
        if (!hasGraph && commands.empty()) {
            return current;
        }

        WorkflowGraph next = hasGraph ? *queued : current->graph;
        for (ParamCommand& cmd : commands) {
            applyParamToGraph(next, cmd.node, cmd.paramId, cmd.value);
        }
        const GenerationPtr rebuilt = buildGeneration(next);
        effective_.store(rebuilt);
        if (hasGraph) {
            publishEvent(WorkflowEventKind::GraphApplied, kInvalidNode,
                         "graph applied at frame boundary");
        }
        {
            std::scoped_lock statsLock(statsMutex_);
            for (auto it = nodeStats_.begin(); it != nodeStats_.end();) {
                if (std::find(rebuilt->topo.begin(), rebuilt->topo.end(), it->first) ==
                    rebuilt->topo.end()) {
                    it = nodeStats_.erase(it);  // 已移除节点的窗口随之释放（有界）。
                } else {
                    ++it;
                }
            }
        }
        return rebuilt;
    }

    /// 参数赋值（requestParamUpdate 已校验；图换代后失配时显式丢弃并发布事件）。
    void applyParamToGraph(WorkflowGraph& graph, NodeId node,
                           const std::string& paramId, const ParamValue& value) {
        NodeInstance* instance = findNode(graph, node);
        const NodeDescriptor* descriptor =
            instance != nullptr
                ? findNodeDescriptor(config_.catalog, instance->typeId)
                : nullptr;
        const ParamDescriptor* param =
            descriptor != nullptr ? findParam(*descriptor, paramId) : nullptr;
        if (instance == nullptr || param == nullptr || !paramValueMatches(*param, value)) {
            publishEvent(WorkflowEventKind::Info, node,
                         "param update discarded (not applicable): " + paramId);
            return;
        }
        for (ParamAssignment& assignment : instance->params) {
            if (assignment.paramId == paramId) {
                assignment.value = value;
                publishEvent(WorkflowEventKind::ParamUpdated, node,
                             "param updated: " + paramId);
                return;
            }
        }
        instance->params.push_back(ParamAssignment{paramId, value});
        publishEvent(WorkflowEventKind::ParamUpdated, node, "param updated: " + paramId);
    }

    [[nodiscard]] double simulatedCost(const NodeInstance& node,
                                       std::uint64_t frameIndex) const {
        if (config_.simulatedCostMs != nullptr) {
            return config_.simulatedCostMs(node, frameIndex);
        }
        return defaultSimulatedCost(node, frameIndex);
    }

    /// 合成节点输出：确定性图案（节点 id + 帧序驱动，随时间可见推进），有输入时
    /// 与上游输出混合；crop/downscale 参数缩放输出宽高（参数面板调试可见）。
    [[nodiscard]] NodeOutputSnapshot synthesize(
        const NodeInstance& node, const Generation& generation,
        const std::unordered_map<NodeId, NodeOutputSnapshot>& produced,
        std::uint64_t frameIndex) const {
        const NodeDescriptor* descriptor =
            findNodeDescriptor(config_.catalog, node.typeId);
        const PortType format = (descriptor != nullptr && !descriptor->outputs.empty())
                                    ? descriptor->outputs.front()
                                    : PortType::Gray8;
        std::uint32_t width = config_.frameWidth;
        std::uint32_t height = config_.frameHeight;
        applyFakeGeometry(node, width, height);

        const NodeOutputSnapshot* upstream = nullptr;
        for (const Connection& connection : generation.graph.connections) {
            if (connection.to.node != node.id) {
                continue;
            }
            const auto it = produced.find(connection.from.node);
            if (it != produced.end()) {
                upstream = &it->second;
                break;
            }
        }

        NodeOutputSnapshot snapshot;
        snapshot.node = node.id;
        snapshot.format = format;
        snapshot.width = width;
        snapshot.height = height;
        snapshot.stride = format == PortType::Rgba8 ? width * 4u : width;
        snapshot.sourceSequence = frameIndex;
        snapshot.pixels = std::make_shared<const std::vector<std::uint8_t>>(
            synthesizePixels(node, format, width, height, frameIndex, upstream));
        return snapshot;
    }

    static std::vector<std::uint8_t> synthesizePixels(const NodeInstance& node,
                                                      PortType format,
                                                      std::uint32_t width,
                                                      std::uint32_t height,
                                                      std::uint64_t frameIndex,
                                                      const NodeOutputSnapshot* upstream) {
        const std::uint32_t channels = format == PortType::Rgba8 ? 4u : 1u;
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height *
                                         channels);
        const std::uint32_t seed = static_cast<std::uint32_t>(node.id) * 29u;
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * width + x) * channels;
                if (channels == 1u) {
                    pixels[offset] = static_cast<std::uint8_t>(x * 3u + y * 5u +
                                                               frameIndex * 11u + seed);
                } else {
                    pixels[offset + 0u] =
                        static_cast<std::uint8_t>(x * 7u + frameIndex * 13u + seed);
                    pixels[offset + 1u] =
                        static_cast<std::uint8_t>(y * 9u + frameIndex * 17u + seed);
                    pixels[offset + 2u] = static_cast<std::uint8_t>(
                        (x ^ y) * 5u + frameIndex * 7u + seed);
                    pixels[offset + 3u] = 255u;
                }
            }
        }
        if (upstream != nullptr && upstream->pixels != nullptr) {
            const std::vector<std::uint8_t>& input = *upstream->pixels;
            const std::size_t count = std::min(pixels.size(), input.size());
            for (std::size_t i = 0; i < count; ++i) {
                pixels[i] = static_cast<std::uint8_t>((pixels[i] + input[i] + 1u) / 2u);
            }
        }
        return pixels;
    }

    void applyFakeGeometry(const NodeInstance& node, std::uint32_t& width,
                           std::uint32_t& height) const {
        if (node.typeId == "crop") {
            const std::int64_t w =
                intParam(node, "width", static_cast<std::int64_t>(config_.frameWidth));
            const std::int64_t h =
                intParam(node, "height", static_cast<std::int64_t>(config_.frameHeight));
            width = static_cast<std::uint32_t>(std::clamp<std::int64_t>(w, 1, 4096));
            height = static_cast<std::uint32_t>(std::clamp<std::int64_t>(h, 1, 4096));
        } else if (node.typeId == "downscale") {
            double scale = realParam(node, "scale", 1.0);
            if (!std::isfinite(scale) || scale <= 0.0) {
                scale = 1.0;
            }
            scale = std::min(scale, 1.0);
            width = std::max<std::uint32_t>(
                1u, static_cast<std::uint32_t>(static_cast<double>(width) * scale));
            height = std::max<std::uint32_t>(
                1u, static_cast<std::uint32_t>(static_cast<double>(height) * scale));
        }
    }

    [[nodiscard]] std::int64_t intParam(const NodeInstance& node,
                                        const std::string& paramId,
                                        std::int64_t fallback) const {
        for (const ParamAssignment& assignment : node.params) {
            if (assignment.paramId == paramId) {
                if (const auto* value = std::get_if<std::int64_t>(&assignment.value)) {
                    return *value;
                }
            }
        }
        return fallback;
    }

    [[nodiscard]] double realParam(const NodeInstance& node, const std::string& paramId,
                                   double fallback) const {
        for (const ParamAssignment& assignment : node.params) {
            if (assignment.paramId == paramId) {
                if (const auto* value = std::get_if<double>(&assignment.value)) {
                    return *value;
                }
            }
        }
        return fallback;
    }

    // --- 统计（任务线程写，start/stop 复位） ---

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

    void recordFrameCompleted(const Generation& generation) {
        std::scoped_lock lock(statsMutex_);
        frameTimes_.push_back(steadyMs());
        if (frameTimes_.size() > kStatsWindow) {
            frameTimes_.pop_front();
        }
        ++processedFrames_;

        WorkflowStats snapshot;
        snapshot.sequence = ++statsSequence_;
        snapshot.endToEndFps =
            frameTimes_.size() >= 2
                ? static_cast<double>(frameTimes_.size() - 1) * 1000.0 /
                      (frameTimes_.back() - frameTimes_.front())
                : 0.0;
        snapshot.processedFrames = processedFrames_;
        snapshot.droppedFrames = droppedFrames_.load(std::memory_order_relaxed);
        snapshot.inFlight = static_cast<std::uint32_t>(inFlightCount_.load());
        for (const NodeId id : generation.topo) {
            NodeStats nodeStats;
            nodeStats.node = id;
            const auto it = nodeStats_.find(id);
            if (it != nodeStats_.end()) {
                const NodeRunStats& run = it->second;
                nodeStats.lastCostMs = run.lastCostMs;
                const double sum =
                    std::accumulate(run.window.begin(), run.window.end(), 0.0);
                nodeStats.avgCostMs =
                    run.window.empty() ? 0.0
                                       : sum / static_cast<double>(run.window.size());
                nodeStats.executedFrames = run.executedFrames;
            }
            snapshot.nodes.push_back(nodeStats);
        }
        stats_.publish(std::move(snapshot));
    }

    // --- 图代构建与查找 ---

    [[nodiscard]] GenerationPtr buildGeneration(const WorkflowGraph& graph) const {
        auto generation = std::make_shared<Generation>();
        generation->graph = graph;

        const std::size_t count = graph.nodes.size();
        std::unordered_map<NodeId, std::size_t> indexOf;
        indexOf.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            indexOf.emplace(graph.nodes[i].id, i);
        }
        std::vector<std::size_t> inDegree(count, 0);
        std::vector<std::vector<std::size_t>> adjacency(count);
        for (const Connection& connection : graph.connections) {
            const std::size_t from = indexOf[connection.from.node];
            const std::size_t to = indexOf[connection.to.node];
            adjacency[from].push_back(to);
            ++inDegree[to];
        }
        // Kahn（图已经 validateWorkflowGraph 判定 DAG；就绪栈保证确定性遍历）。
        std::vector<std::size_t> ready;
        for (std::size_t i = 0; i < count; ++i) {
            if (inDegree[i] == 0) {
                ready.push_back(i);
            }
        }
        generation->topo.reserve(count);
        while (!ready.empty()) {
            const std::size_t current = ready.back();
            ready.pop_back();
            generation->topo.push_back(graph.nodes[current].id);
            for (const std::size_t next : adjacency[current]) {
                if (--inDegree[next] == 0) {
                    ready.push_back(next);
                }
            }
        }
        // 防御：DAG 校验已保证全覆盖；若未来校验被绕过，按声明序补齐避免丢节点。
        for (const NodeInstance& instance : graph.nodes) {
            if (std::find(generation->topo.begin(), generation->topo.end(),
                          instance.id) == generation->topo.end()) {
                generation->topo.push_back(instance.id);
            }
        }

        for (const NodeInstance& instance : graph.nodes) {
            generation->outputs[instance.id] =
                std::make_unique<LatestMailbox<NodeOutputSnapshot>>(
                    "rin.workflow.node." + std::to_string(instance.id));
        }
        return generation;
    }

    static NodeInstance* findNode(WorkflowGraph& graph, NodeId node) {
        for (NodeInstance& instance : graph.nodes) {
            if (instance.id == node) {
                return &instance;
            }
        }
        return nullptr;
    }

    static const NodeInstance* findNode(const WorkflowGraph& graph, NodeId node) {
        for (const NodeInstance& instance : graph.nodes) {
            if (instance.id == node) {
                return &instance;
            }
        }
        return nullptr;
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

    executor::Executor& executor_;
    FakeWorkflowEngineConfig config_;

    LatestMailbox<WorkflowStats> stats_{"rin.workflow.stats"};
    LatestMailbox<WorkflowEvent> events_{"rin.workflow.events"};
    LatestMailbox<PendingGraph> graphQueue_{"rin.workflow.graph"};
    executor::comm::MpscChannel<ParamCommand> paramQueue_;

    std::atomic<GenerationPtr> effective_{nullptr};
    std::shared_ptr<const WorkflowGraph> pendingStart_;  // owner 线程（lifecycleMutex_ 下）
    std::vector<std::unique_ptr<InFlight>> inflight_;    // lifecycleMutex_ 下
    std::atomic<std::size_t> inFlightCount_{0};
    std::atomic<std::uint64_t> frameCounter_{0};
    std::atomic<std::uint64_t> droppedFrames_{0};
    std::atomic<WorkflowEngineState> state_{WorkflowEngineState::Idle};
    executor::TimerHandle timerHandle_{};

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

std::shared_ptr<IWorkflowEngine> createFakeWorkflowEngine(
    executor::Executor& executor, FakeWorkflowEngineConfig config) {
    if (config.frameWidth == 0 || config.frameHeight == 0) {
        throw std::invalid_argument("fake workflow engine: frame size must be positive");
    }
    if (config.maxInFlight == 0) {
        throw std::invalid_argument("fake workflow engine: maxInFlight must be positive");
    }
    if (config.frameInterval.count() <= 0) {
        throw std::invalid_argument(
            "fake workflow engine: frameInterval must be positive");
    }
    return std::make_shared<FakeWorkflowEngine>(executor, std::move(config));
}

}  // namespace rin
