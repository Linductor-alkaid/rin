#include "policy_depth_preview.hpp"

#include <exception>
#include <utility>

namespace viewer {

namespace {
/// 策略时钟周期（毫秒）：50 Hz 策略 tick（DEC-019 决策 3）；软调度允许抖动，
/// 无帧 tick 为空转快路径。
inline constexpr std::int64_t kTickPeriodMs = 20;
/// 处理速率 EMA 权重（新样本 0.1，与引擎统计滚动窗口同量级）。
inline constexpr double kFpsAlpha = 0.1;
}  // namespace

PolicyDepthPreview::PolicyDepthPreview(std::shared_ptr<rin::ICameraService> service,
                                       const rin::PolicyDepthConfig& config)
    : service_(std::move(service)), model_(config) {}

PolicyDepthPreview::~PolicyDepthPreview() {
    stop();  // 防御：实例析构先于 stop 时取消周期任务（掉队 tick 经弱引用短路）。
}

bool PolicyDepthPreview::start(executor::Executor& executor) {
    if (running_.load(std::memory_order_relaxed)) {
        return true;  // 幂等。
    }
    // 弱引用闭包（引擎同款，M5-08 掉队 tick 生命周期闭合）：实例先于 executor
    // shutdown 停止或析构（onShutdown 顺序），排队 tick 锁不住已析构实例。
    // Executor 在未初始化/定时器已停止等状态下以异常报告准入失败（而非无效
    // 句柄），按组件契约转译为返回 false（D1，独立验证报告）。
    try {
        executor::TimerHandle handle = executor.submit_periodic_cancellable_with_handle(
            kTickPeriodMs, [weak = weak_from_this()](executor::StopToken) {
                if (std::shared_ptr<PolicyDepthPreview> self = weak.lock()) {
                    self->tick();
                }
            });
        if (!handle.valid()) {
            return false;  // 准入显式失败，保持非运行态。
        }
        handle_ = std::move(handle);
    } catch (const std::exception&) {
        busy_.store(false, std::memory_order_relaxed);
        return false;  // 准入异常同样转译为显式 false，保持非运行态。
    }
    running_.store(true, std::memory_order_relaxed);
    return true;
}

void PolicyDepthPreview::stop() noexcept {
    if (!running_.exchange(false, std::memory_order_relaxed)) {
        return;  // 幂等。
    }
    if (handle_.valid()) {
        handle_.cancel();  // 阻止后续 tick；已派发 tick 由 running 短路（weak 锁失败同效）。
    }
    handle_ = {};
}

void PolicyDepthPreview::tick() noexcept {
    if (!running_.load(std::memory_order_relaxed)) {
        return;  // 停止后已派发 tick 短路。
    }
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true)) {
        return;  // 重叠防御跳过（周期 ≫ 单帧成本，触发即计数可观测）。
    }
    // busy_ 复位守卫（异常安全）：tick 内任何异常不得卡死重叠防御门
    // （独立验证报告加固项）；tick 本身契约无异常（process 仅在无效输入抛，
    // 已由 sample.valid() 门控），此为防御闭环。
    struct BusyGuard {
        std::atomic<bool>& busy;
        ~BusyGuard() { busy.store(false, std::memory_order_relaxed); }
    } guard{busy_};

    try {
        rin::DepthMetricSample sample;
        const bool hasNew =
            service_ != nullptr && service_->tryLoadDepthMetric(lastMailboxSequence_, sample);
        if (hasNew) {
            if (hasSourceSequence_ && sample.sequence < lastSourceSequence_) {
                model_.reset();  // 流重启：帧序号回绕 → 历史环复位（DEC-019 决策 3）。
            }
            lastSourceSequence_ = sample.sequence;
            hasSourceSequence_ = true;

            if (sample.valid()) {
                model_.process(sample.frame, sample.sequence);
                const auto now = std::chrono::steady_clock::now();
                if (hasFrameTime_) {
                    const double dt = std::chrono::duration<double>(now - lastFrameTime_).count();
                    if (dt > 0.0) {
                        const double instant = 1.0 / dt;
                        fps_ = fps_ > 0.0 ? fps_ + kFpsAlpha * (instant - fps_) : instant;
                    }
                }
                lastFrameTime_ = now;
                hasFrameTime_ = true;
            } else {
                ++droppedFrames_;  // 无效源帧：显式计数，不静默吞掉。
            }

            PolicyDepthPreviewState state;
            state.snapshot = model_.snapshot();
            state.fps = fps_;
            state.droppedFrames = droppedFrames_;
            states_.publish(std::move(state));
        }
    } catch (const std::exception&) {
        ++droppedFrames_;  // 处理异常：显式计数丢弃本帧，不静默（AGENTS 规则 10）。
    }
}

bool PolicyDepthPreview::tryLoadState(std::uint64_t& lastSeen, PolicyDepthPreviewState& out) {
    return states_.try_load_newer_than(lastSeen, out, lastSeen);
}

}  // namespace viewer
