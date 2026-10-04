#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace rin {

/// M8 米制深度策略预处理算子（DEC-018；冻结公式见
/// docs/design/depth_policy_preproc_design.md）：
///
/// 把 RealSense 米制深度帧（float32，米，distance_to_image_plane 语义）
/// 确定性预处理为视觉策略 depth_encoder 的输入时序张量，与 roboparty
/// E3-Parkour 仿真部署参考实现逐位对齐。Core 纯函数层：零第三方依赖、
/// 单线程、不进入 M4 工作流节点面（时序历史为跨帧有状态，与
/// IImageNode 无状态契约冲突，DEC-018 决策 1）。
///
/// 失败纪律：输入帧无效或参数越界一律抛 std::invalid_argument（显式
/// 报告，不静默）；输出一律为提交后不可变的紧凑新缓冲（stride = 宽）。
///
/// - O1 "fillDepthInvalid"：无效深度填充。!isfinite(v) 或 v ≤ invalidBelow
///   （默认 0，Z16×depth_scale 的 0 = 无效）→ farValue（默认语义
///   "无信息 = 远距"）。
/// - O2 "resizeDepthArea"：面积加权箱式降采样（= cv2 INTER_AREA 64F
///   数学定义）；仅允许缩小，放大抛（语义回退分叉显式拒绝）。
/// - O3 "cropDepth"：裁切 (up, down, left, right)，rows [up, H−down)、
///   cols [left, W−right)，与 noise_model.crop_and_resize 索引逐位一致；
///   退化/越界抛。
/// - O4 "gaussianBlurDepth"：可分离两趟 + reflect-101 边界 + double 中间
///   量；核式与 M4-04 gaussian_blur/cv2 getGaussianKernel 同式，σ=0 δ 核
///   恒等；radius ∈ [1,10]、sigma ∈ [0,10] 越界抛。
/// - O5 "clipNormalizeDepth"：clip [near, far] 后 (v−near)/(far−near)；
///   near ≥ far 抛。
/// - O6 "preprocessPolicyDepthFrame"：冻结顺序组合（填充 → 降采样到
///   raw 网格 → 裁切 → 模糊 → 归一化），输出 policyHeight × policyWidth
///   （默认 18×32）。
/// - O7 "PolicyDepthHistory"：historyLength（默认 37）有界环形历史，
///   sample() 输出 CHW float32（默认 8×18×32，oldest→newest，抽样下标
///   {1,6,11,16,21,26,31,36}）；欠帧首帧填充、空历史全 0（对齐部署
///   参考 sample_depth_history 两分支）。单写者约定：append/sample 由
///   同一执行上下文调用（50 Hz 策略 tick 内），类不加内部锁；跨上下文
///   使用由上层经 kairo::comm 交付语义保证。

/// 单帧米制深度字节预算（镜像 kMaxImageBytes；848×480 F32 ≈ 1.6 MiB）。
inline constexpr std::uint64_t kMaxDepthFrameBytes = 16u * 1024u * 1024u;

/// 米制深度帧（float32 单通道，米；M8-02）：共享不可变像素 + 容量有界。
///
/// 值语义与 ImageU8 一致：默认构造无效；make/wrap 不抛异常，失败返回
/// 无效帧；stride 为元素数（≥ width）；wrap 后以 const 视图共享。
class DepthFrameF32 {
public:
    /// 无效空帧。
    DepthFrameF32() = default;

    /// 分配并构造（值初始化为零）。宽高为 0、stride 小于宽或字节预算
    /// 超限返回无效帧。
    [[nodiscard]] static DepthFrameF32 make(std::uint32_t width, std::uint32_t height,
                                            std::uint32_t stride = 0);

    /// 接管既有缓冲。缓冲为空、尺寸非法或小于 stride×height 返回无效帧；
    /// 接管后调用方不得再改写内容。
    [[nodiscard]] static DepthFrameF32 wrap(std::uint32_t width, std::uint32_t height,
                                            std::uint32_t stride,
                                            std::shared_ptr<const std::vector<float>> pixels);

    /// 有效性：非默认构造成功（宽高非零、stride 达到最小行宽、缓冲非空
    /// 且不小于 stride×height）。
    [[nodiscard]] bool valid() const noexcept;

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::uint32_t stride() const noexcept { return stride_; }

    /// 缓冲字节数（stride×height×4；无效帧为 0）。
    [[nodiscard]] std::uint64_t byteSize() const noexcept;

    /// 共享像素缓冲；无效帧返回空指针。
    [[nodiscard]] const std::shared_ptr<const std::vector<float>>& pixels() const noexcept {
        return pixels_;
    }

    /// 行首指针；帧无效或 y ≥ height 返回 nullptr。
    [[nodiscard]] const float* row(std::uint32_t y) const noexcept;

private:
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t stride_ = 0;
    std::shared_ptr<const std::vector<float>> pixels_;
};

/// 冻结配置（默认值 = E3-Parkour 部署值；字段语义与来源见设计文档 §4）。
struct PolicyDepthConfig {
    /// raw 射线网格（传感器对齐目标分辨率）。
    std::uint32_t gridWidth = 64;
    std::uint32_t gridHeight = 36;
    /// 裁切 (up, down, left, right)，语义同 noise_model.crop_and_resize。
    std::uint32_t cropUp = 18;
    std::uint32_t cropDown = 0;
    std::uint32_t cropLeft = 16;
    std::uint32_t cropRight = 16;
    /// 高斯模糊（K = 2·radius+1）。
    std::uint32_t blurRadius = 1;
    double blurSigma = 1.0;
    /// 深度裁切归一化区间（米）。
    double depthNear = 0.0;
    double depthFar = 2.5;
    /// 时序历史与抽样（delay=0 = 每策略 tick 取最新帧）。
    std::size_t historyLength = 37;
    std::size_t sampleCount = 8;
    std::size_t sampleSkip = 5;
    std::size_t sampleDelay = 0;

    /// 有效性（设计文档 §4 全部约束；越界配置在构造 PolicyDepthHistory
    /// 与组合函数入口同样校验）。
    [[nodiscard]] bool valid() const noexcept;

    /// 派生：策略张量宽 = gridWidth − cropLeft − cropRight（默认 32）。
    [[nodiscard]] std::uint32_t policyWidth() const noexcept;
    /// 派生：策略张量高 = gridHeight − cropUp − cropDown（默认 18）。
    [[nodiscard]] std::uint32_t policyHeight() const noexcept;
};

/// O1 无效深度填充。!isfinite(v) 或 v ≤ invalidBelow 的像素置 farValue。
[[nodiscard]] DepthFrameF32 fillDepthInvalid(const DepthFrameF32& in, double farValue,
                                             double invalidBelow = 0.0);

/// O2 面积加权箱式降采样到 dstWidth × dstHeight（仅缩小；放大或零尺寸
/// 目标抛）。同尺寸逐像素恒等。
[[nodiscard]] DepthFrameF32 resizeDepthArea(const DepthFrameF32& in, std::uint32_t dstWidth,
                                            std::uint32_t dstHeight);

/// O3 裁切。up + down ≥ 输入高 或 left + right ≥ 输入宽 抛。
[[nodiscard]] DepthFrameF32 cropDepth(const DepthFrameF32& in, std::uint32_t up, std::uint32_t down,
                                      std::uint32_t left, std::uint32_t right);

/// O4 可分离高斯模糊（reflect-101 边界，double 中间量，σ=0 恒等）。
[[nodiscard]] DepthFrameF32 gaussianBlurDepth(const DepthFrameF32& in, std::uint32_t radius,
                                              double sigma);

/// O5 裁切归一化到 [0,1]。
[[nodiscard]] DepthFrameF32 clipNormalizeDepth(const DepthFrameF32& in, double near, double far);

/// O6 冻结顺序组合：填充（far = depthFar）→ 降采样到 raw 网格 → 裁切 →
/// 模糊 → 归一化。config 无效或输入帧无效抛；输出
/// policyHeight × policyWidth。
[[nodiscard]] DepthFrameF32 preprocessPolicyDepthFrame(const DepthFrameF32& rawMetricDepth,
                                                       const PolicyDepthConfig& config);

/// O7 时序环形历史（有界 historyLength 帧，单写者约定见文件头注）。
class PolicyDepthHistory {
public:
    /// 构造期定型：config 无效抛 std::invalid_argument。
    explicit PolicyDepthHistory(const PolicyDepthConfig& config);

    /// 清空历史。
    void reset() noexcept;

    /// 追加一帧（环满覆盖最旧）；帧尺寸 ≠ policyHeight × policyWidth 或
    /// 帧无效抛。
    void append(const DepthFrameF32& frame);

    /// 抽样输出 CHW float32（sampleCount × policyHeight × policyWidth，
    /// oldest→newest 连续存储）；空历史全 0，欠帧首帧填充（设计文档
    /// §5 O7）。无异常抛出（帧尺寸由 append 保证）。
    [[nodiscard]] std::vector<float> sample() const;

    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] std::uint32_t frameWidth() const noexcept { return config_.policyWidth(); }
    [[nodiscard]] std::uint32_t frameHeight() const noexcept { return config_.policyHeight(); }

private:
    PolicyDepthConfig config_;
    std::vector<float> ring_;  // historyLength × policyHeight × policyWidth
    std::size_t head_ = 0;     // 下一写入位置
    std::size_t count_ = 0;    // 有效帧数（≤ historyLength）
};

}  // namespace rin
