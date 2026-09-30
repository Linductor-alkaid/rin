#pragma once

#include <cstdint>

#include "rin/depth_preproc.hpp"
#include "rin/image_types.hpp"

namespace rin {

/// M9 策略深度预览模型（DEC-019）：把 M8 冻结管线（O6 组合 + O7 时序抽样）
/// 的输出渲染为可显示的历史网格快照。Core 纯逻辑、零第三方依赖、可 headless
/// 单测；执行上下文归调用方（viewer tick 组件，Executor 周期任务内单写者）。
///
/// - 网格布局冻结：2 列 × 4 行，每帧 32×18 最近邻 ×8 → 256×144，画布
///   512×576 Rgba8（灰度复制 RGB，alpha 恒 255）；8 帧按 sample() 顺序
///   （oldest→newest）阅读序排布，最新帧在右下。值域 [0,1] → uint8
///   round(v×255)。
/// - `process` 逐帧推进（无帧时不调用）；`reset` 供流重启复位（幂等，
///   复位计数进快照可观测）。

/// 历史网格快照（共享不可变像素；未处理任何帧时 grid 无效）。
struct PolicyDepthPreviewSnapshot {
    std::uint64_t sourceSequence = 0;  /// 最新处理的源帧序号（DepthMetricSample::sequence）。
    std::uint64_t processedFrames = 0;  /// 会话累计处理帧数。
    std::uint64_t historyResets = 0;  /// 会话累计历史复位次数（流重启检测驱动）。
    ImageU8 grid;                     /// Rgba8 历史网格（512×576；无效 = 尚无帧）。

    [[nodiscard]] bool valid() const noexcept { return grid.valid(); }
};

class PolicyDepthPreviewModel {
public:
    /// config 无效抛 std::invalid_argument（构造期定型）。
    explicit PolicyDepthPreviewModel(const PolicyDepthConfig& config = PolicyDepthConfig{});

    /// 推进一帧：O6 冻结组合 → O7 append → 抽样 → 渲染网格快照。帧无效抛
    /// std::invalid_argument；快照整体换新（sourceSequence/processedFrames
    /// 推进，grid 为新共享缓冲）。
    void process(const DepthFrameF32& metricFrame, std::uint64_t sourceSequence);

    /// 复位时序历史（流重启）；快照计数保留，grid 保留为最后状态直到下一帧。
    void reset() noexcept;

    [[nodiscard]] const PolicyDepthPreviewSnapshot& snapshot() const noexcept { return snapshot_; }
    [[nodiscard]] const PolicyDepthConfig& config() const noexcept { return config_; }

private:
    PolicyDepthConfig config_;
    PolicyDepthHistory history_;
    PolicyDepthPreviewSnapshot snapshot_;
};

}  // namespace rin
