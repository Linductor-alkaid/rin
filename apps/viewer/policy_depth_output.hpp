#pragma once

// 策略深度单帧输出组件（DEC-019 决策 3 的"无历史分片"变体）：ICameraService
// 米制深度通道 → 冻结管线 O6 单帧组合（preprocessPolicyDepthFrame：填充 →
// 降采样到 raw 网格 → 裁切 → 模糊 → 归一化，PolicyDepthConfig 默认值即
// roboparty E3-Parkour 部署参考的完整单帧预处理语义）→ 最近邻放大 + [0,1]×255
// 灰度 RGBA8 显示帧 → LatestMailbox。应用层接缝（workflow_frame_source.hpp
// 同款边界）：无 EUI 类型，可 headless 单测（脚本化假服务直接驱动 tick）。
//
// 与 M9 PolicyDepthPreview（已随 DEC-020 决策 4 移除）的差异：不经 O7 历史
// 环与抽样，无跨帧状态——历史分片不进预览可选输出（本轮需求边界）；流重启
// 检测/复位随之不适用。
//
// 语义要点（DEC-019 同源纪律）：
// - tick 由 Executor 周期任务驱动（20 ms ≈ 策略 50 Hz，软调度允许抖动）；
//   tick 闭包持弱引用（掉队 tick 生命周期闭合），busy 原子跳过防重叠；
// - UI 线程仅经 LatestMailbox "上次已见序号"非阻塞消费（RULE-05 渲染线程
//   有界消费）；
// - 失败显式化：无效源帧 / O6 拒绝（如源帧小于 raw 网格的降采样放大拒绝）
//   计入 droppedFrames，不静默不排队（AGENTS 规则 10）；
// - 生命周期 owner 为 ViewerContext（AppRuntime）：服务 start 准入通过后
//   start，onShutdown 在服务 stop 之后、executor shutdown 之前析构停止。

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include <kairo/comm/mailbox.hpp>
#include <kairo/executor.hpp>

#include <rin/camera_service.hpp>
#include <rin/camera_types.hpp>
#include <rin/depth_preproc.hpp>

namespace viewer {

/// 显示帧最长边上限（有界预算：512×288×4 ≈ 0.6 MiB，邮箱单槽换新）。
inline constexpr std::uint32_t kPolicyDepthOutputMaxDim = 512;

/// [0,1] 归一化值 → 显示灰度（×255 量化，DEC-019 决策 4 同源显示语义：
/// near 黑 → far 白，farValue 填充的无效像素为白）。
[[nodiscard]] inline std::uint8_t policyDepthDisplayGray(double value) noexcept {
    const double clamped = value < 0.0 ? 0.0 : value > 1.0 ? 1.0 : value;
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0));
}

/// 策略帧 → 最近邻放大的灰度 RGBA8 显示帧（最长边 ≤
/// kPolicyDepthOutputMaxDim 的整数倍率，至少 1）。帧无效抛 std::invalid_argument。
[[nodiscard]] inline rin::Frame policyDepthDisplayFrame(const rin::DepthFrameF32& frame) {
    if (!frame.valid()) {
        throw std::invalid_argument("policy depth frame is invalid");
    }
    const std::uint32_t width = frame.width();
    const std::uint32_t height = frame.height();
    const std::uint32_t scale =
        std::max(1u, kPolicyDepthOutputMaxDim / std::max(width, height));
    const std::uint32_t outWidth = width * scale;
    const std::uint32_t outHeight = height * scale;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(outWidth) * outHeight * 4u);
    for (std::uint32_t y = 0; y < outHeight; ++y) {
        const float* sourceRow = frame.row(y / scale);
        std::uint8_t* targetRow = pixels.data() + static_cast<std::size_t>(y) * outWidth * 4u;
        for (std::uint32_t x = 0; x < outWidth; ++x) {
            const std::uint8_t gray =
                policyDepthDisplayGray(sourceRow[x / scale]);
            std::uint8_t* target = targetRow + static_cast<std::size_t>(x) * 4u;
            target[0] = gray;
            target[1] = gray;
            target[2] = gray;
            target[3] = 255;
        }
    }
    rin::Frame out;
    out.kind = rin::FrameKind::Depth;
    out.width = outWidth;
    out.height = outHeight;
    out.stride = outWidth * 4u;
    out.pixels = std::make_shared<const std::vector<std::uint8_t>>(std::move(pixels));
    return out;
}

class PolicyDepthOutput : public std::enable_shared_from_this<PolicyDepthOutput> {
public:
    explicit PolicyDepthOutput(std::shared_ptr<rin::ICameraService> service,
                               rin::PolicyDepthConfig config = {})
        : service_(std::move(service)), config_(config) {
        if (!config_.valid()) {
            throw std::invalid_argument("policy depth config is invalid");
        }
    }

    ~PolicyDepthOutput() { stop(); }

    PolicyDepthOutput(const PolicyDepthOutput&) = delete;
    PolicyDepthOutput& operator=(const PolicyDepthOutput&) = delete;

    /// 启动周期 tick（20 ms）；重复 start 无效果（幂等）。组件必须由
    /// shared_ptr 持有（tick 闭包经 weak_from_this 捕获，enable_shared_from_this
    /// 契约）——unique/栈持有会使弱引用恒空、周期回调静默空转，此处显式拒绝
    /// 返回 false（不静默，IVS 复验发现的缺陷类）。
    [[nodiscard]] bool start(kairo::Executor& executor) {
        if (weak_from_this().expired()) {
            return false;
        }
        if (started_.exchange(true, std::memory_order_relaxed)) {
            return true;
        }
        handle_ = executor.submit_periodic_cancellable(
            20, [weak = weak_from_this()](kairo::StopToken) {
                if (const auto self = weak.lock()) {
                    self->tick();
                }
            });
        return true;
    }

    /// 取消周期 tick（析构同路径）；幂等。已取消后不再有新周期 tick；在途
    /// tick 完成当前帧后自然终止（弱引用闭合生命周期）。start 可重新提交。
    void stop() noexcept {
        if (handle_.valid()) {
            handle_.cancel();
        }
    }

    /// 单步推进（Executor 回调；公开供 headless 时序测试直接驱动，无运行态
    /// 前置）：取最新米制帧 → O6 冻结组合 → 显示帧发布。无新帧为空转快路径；
    /// 无效源帧与 O6 拒绝计入丢弃计数（显式，不静默）。
    void tick() noexcept {
        if (busy_.exchange(true, std::memory_order_relaxed)) {
            return;
        }
        const struct BusyReset {
            std::atomic<bool>& busy;
            ~BusyReset() { busy.store(false, std::memory_order_relaxed); }
        } reset{busy_};
        if (service_ == nullptr) {
            return;
        }
        rin::DepthMetricSample sample;
        if (!service_->tryLoadDepthMetric(metricLastSeen_, sample)) {
            return;
        }
        if (!sample.valid()) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        try {
            const rin::DepthFrameF32 policy =
                rin::preprocessPolicyDepthFrame(sample.frame, config_);
            rin::Frame display = policyDepthDisplayFrame(policy);
            display.sequence = sample.sequence;
            display.deviceTimestampMs = sample.deviceTimestampMs;
            frames_.publish(std::move(display));
        } catch (const std::exception&) {
            dropped_.fetch_add(1, std::memory_order_relaxed);  // 显式丢弃，不静默。
        }
    }

    /// UI 线程最新态消费：mailbox 序号 > lastSeen 的新显示帧写入 out 并推进
    /// lastSeen。无新帧返回 false（RULE-05 非阻塞）。
    [[nodiscard]] bool tryLoadFrame(std::uint64_t& lastSeen, rin::Frame& out) {
        return frames_.try_load_newer_than(lastSeen, out, lastSeen);
    }

    /// 无效源帧/O6 拒绝累计（显式丢弃的可观察面，EXEC-07）。
    [[nodiscard]] std::uint64_t droppedFrames() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    std::shared_ptr<rin::ICameraService> service_;
    rin::PolicyDepthConfig config_;
    kairo::comm::LatestMailbox<rin::Frame> frames_{"viewer.policydepth"};

    std::uint64_t metricLastSeen_ = 0;  /// 服务米制通道"上次已见序号"（tick 侧）。
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<bool> busy_{false};
    std::atomic<bool> started_{false};
    kairo::TimerHandle handle_{};
};

}  // namespace viewer
