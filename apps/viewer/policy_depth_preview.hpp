#pragma once

// 策略深度预览组件（M9，DEC-019）：ICameraService 米制深度通道 → 冻结管线
// （depth_preproc O6 组合 + O7 时序抽样，PolicyDepthPreviewModel）→ 预览
// 快照邮箱。应用层接缝（workflow_frame_source.hpp 同款边界）：无 EUI 类型，
// 可 headless 单测（脚本化假服务直接驱动 tick）。
//
// 语义要点（DEC-019 决策 3/5）：
// - tick 由 Executor 周期任务驱动（submit_periodic_cancellable_with_handle，
//   20 ms ≈ 策略 50 Hz，软调度允许抖动）；tick 闭包持弱引用（引擎同款掉队
//   tick 生命周期闭合），busy 原子跳过防重叠（周期 ≫ 单帧成本，防御性）；
// - 单写者：模型与历史环只在 tick 上下文推进；UI 线程仅经 LatestMailbox
//   "上次已见序号"非阻塞消费快照（RULE-05 渲染线程有界消费）；
// - 流重启复位：tick 内检测 sample.sequence 回绕（每次流启动重置）→
//   model.reset()，历史环不跨流存续；
// - 失败显式化：无效帧计数进快照（droppedFrames），不静默不排队；
// - 生命周期 owner 为 ViewerContext（AppRuntime）：服务创建后 start，
//   onShutdown 在服务 stop 之后、executor shutdown 之前 stop。

#include <atomic>
#include <chrono>
#include <memory>

#include <executor/comm/mailbox.hpp>
#include <executor/executor.hpp>

#include <rin/camera_service.hpp>
#include <rin/depth_policy_preview.hpp>
#include <rin/depth_preproc.hpp>

namespace viewer {

/// tick 侧实测状态（快照 + 处理速率 EMA + 显式丢弃计数）。
struct PolicyDepthPreviewState {
    rin::PolicyDepthPreviewSnapshot snapshot;
    double fps = 0.0;                 /// 处理速率 EMA（tick 侧，帧间隔倒数）。
    std::uint64_t droppedFrames = 0;  /// 无效源帧/防御跳过累计（显式丢弃，不静默）。
};

class PolicyDepthPreview : public std::enable_shared_from_this<PolicyDepthPreview> {
public:
    /// config 无效（PolicyDepthHistory 构造）抛 std::invalid_argument。
    explicit PolicyDepthPreview(std::shared_ptr<rin::ICameraService> service,
                                const rin::PolicyDepthConfig& config = rin::PolicyDepthConfig{});

    ~PolicyDepthPreview();

    PolicyDepthPreview(const PolicyDepthPreview&) = delete;
    PolicyDepthPreview& operator=(const PolicyDepthPreview&) = delete;

    /// 启动周期 tick（20 ms）；重复 start 无效果（幂等）。准入失败返回 false
    /// 并保持非运行态（显式失败，调用方可上报）。
    [[nodiscard]] bool start(executor::Executor& executor);

    /// 取消周期 tick；幂等。已派发 tick 经弱引用短路（引擎同款）。
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_relaxed); }

    /// 单步推进（Executor 回调；公开供 headless 时序测试直接驱动）：取最新
    /// 米制帧 → 流重启检测/复位 → 模型推进 → 状态发布。无新帧为空转快路径。
    void tick() noexcept;

    /// UI 线程最新态消费：sequence > lastSeen 的新状态写入 out 并推进 lastSeen。
    [[nodiscard]] bool tryLoadState(std::uint64_t& lastSeen, PolicyDepthPreviewState& out);

private:
    std::shared_ptr<rin::ICameraService> service_;
    rin::PolicyDepthPreviewModel model_;
    executor::comm::LatestMailbox<PolicyDepthPreviewState> states_{"viewer.policydepth"};

    // --- 仅 tick 上下文访问（单写者） ---
    std::uint64_t lastMailboxSequence_ = 0;  /// 服务米制通道"上次已见序号"。
    std::uint64_t lastSourceSequence_ = 0;  /// 流重启检测（帧序号每次流启动重置）。
    bool hasSourceSequence_ = false;
    std::uint64_t droppedFrames_ = 0;
    std::chrono::steady_clock::time_point lastFrameTime_{};
    bool hasFrameTime_ = false;
    double fps_ = 0.0;

    std::atomic<bool> busy_{false};
    std::atomic<bool> running_{false};
    executor::TimerHandle handle_{};
};

}  // namespace viewer
