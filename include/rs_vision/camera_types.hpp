#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rsv {

/// 相机服务显式状态集（AGENTS.md "Runtime 与状态模型"）。
enum class CameraServiceState {
    Idle,
    Opening,
    Streaming,
    Restreaming,
    Stopping,
    Failed,
};

[[nodiscard]] const char* toString(CameraServiceState state) noexcept;

enum class FrameKind {
    Rgb,
    Depth,
};

/// 一次流配置请求：彩色与深度成对（M1 不支持单流）。
struct StreamRequest {
    std::uint32_t colorWidth = 848;
    std::uint32_t colorHeight = 480;
    std::uint32_t colorFps = 30;
    std::uint32_t depthWidth = 848;
    std::uint32_t depthHeight = 480;
    std::uint32_t depthFps = 30;
};

[[nodiscard]] bool operator==(const StreamRequest& lhs, const StreamRequest& rhs) noexcept;
[[nodiscard]] bool operator!=(const StreamRequest& lhs, const StreamRequest& rhs) noexcept;

/// 一帧已转换为 RGBA8（打包、行对齐 stride）的图像。
struct Frame {
    FrameKind kind = FrameKind::Rgb;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;  // 字节
    std::uint64_t sequence = 0;
    double deviceTimestampMs = 0.0;
    /// RGBA8 打包像素；提交后内容不可变（消费方共享所有权）。
    std::shared_ptr<const std::vector<std::uint8_t>> pixels;

    [[nodiscard]] bool valid() const noexcept {
        return pixels != nullptr && width > 0 && height > 0 &&
               stride >= width * 4u && pixels->size() >= static_cast<std::size_t>(stride) * height;
    }
};

enum class DistortionModel {
    Unknown,
    None,
    ModifiedBrownConrady,
    InverseBrownConrady,
    BrownConrady,
    FTheta,
    KannalaBrandt4,
};

/// 单个流的针孔内参。
struct StreamIntrinsics {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float fx = 0.0f;
    float fy = 0.0f;
    float cx = 0.0f;
    float cy = 0.0f;
    std::array<float, 5> distortion{};
    DistortionModel model = DistortionModel::Unknown;

    [[nodiscard]] bool valid() const noexcept {
        return width > 0 && height > 0 && fx > 0.0f && fy > 0.0f;
    }
};

/// 当前流配置下的彩色+深度内参快照。
struct IntrinsicsSnapshot {
    StreamIntrinsics color;
    StreamIntrinsics depth;
    std::uint64_t sequence = 0;
};

struct ResolutionOption {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fps = 0;
};

/// 设备静态能力（在 start() 准入阶段同步枚举，经邮箱发布）。
struct StreamCapabilities {
    std::string deviceName;
    std::string serial;
    std::string firmwareVersion;
    std::vector<ResolutionOption> colorOptions;
    std::vector<ResolutionOption> depthOptions;
    bool devicePresent = false;
};

enum class ServiceEventKind {
    Started,
    ResolutionChanged,
    Info,
    Stopped,
    Failed,
};

struct ServiceEvent {
    ServiceEventKind kind = ServiceEventKind::Info;
    CameraServiceState state = CameraServiceState::Idle;
    std::string message;
    double timestampMs = 0.0;
};

/// start() 的准入结果；admitted 仅表示进入 Opening，后续失败经事件邮箱报告。
struct StartOutcome {
    bool admitted = false;
    std::string error;
};

}  // namespace rsv
