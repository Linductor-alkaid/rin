#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rin {

/// 相机服务显式状态集（AGENTS.md "Runtime 与状态模型"；热插拔语义见 DEC-006）。
enum class CameraServiceState {
    Idle,
    Opening,
    Streaming,
    Restreaming,
    /// 等待相机接入/用户选择设备（设计稳态，非错误）。
    Waiting,
    Stopping,
    Failed,
};

[[nodiscard]] const char* toString(CameraServiceState state) noexcept;

enum class FrameKind {
    Rgb,
    Depth,
};

/// 深度图输出配色（DEC-007）：Z16 → RGBA8 的视觉映射风格。
enum class DepthColorScheme {
    /// jet 伪彩（DEC-003 默认）。
    Jet,
    /// 灰度黑白：近处白、远处黑；无效深度（0）与 jet 一致输出不透明黑。
    Grayscale,
};

/// 一次流配置请求：彩色与深度成对（M1 不支持单流）；`enableMotion` 请求附加
/// ACCEL/GYRO 运动流（M3）。设备无 IMU 时运动通道保持空，不视为错误。
struct StreamRequest {
    std::uint32_t colorWidth = 848;
    std::uint32_t colorHeight = 480;
    std::uint32_t colorFps = 30;
    std::uint32_t depthWidth = 848;
    std::uint32_t depthHeight = 480;
    std::uint32_t depthFps = 30;
    /// 是否同时采集 IMU 运动流（M3-03 使能位）；跨分辨率切换请求粘性保持。
    bool enableMotion = false;
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

/// 两个流坐标系之间的刚体外参（M3-04，SDK rs2_extrinsics 语义）。
struct Extrinsics {
    /// 3x3 旋转，列主序（v_target = rotation * v_source + translation）。
    std::array<float, 9> rotation{};
    /// 平移（米，目标坐标系下）。
    std::array<float, 3> translation{};

    /// 有效性：rotation/translation 全部有限，且 rotation 非全零——全零旋转是
    /// "未填充/读取失败"的可观察哨兵（真实标定旋转不可能全零）；平移允许全零
    /// （同址/共面安装）。
    [[nodiscard]] bool valid() const noexcept;
};

/// 单个运动传感器（ACCEL/GYRO）的出厂运动内参（M3-04，SDK motion intrinsics；
/// scale/bias 为 SDK 出厂标定单位，与运动流采样的 m/s²、rad/s 输出单位不同源）。
struct MotionIntrinsics {
    /// 三轴刻度/轴间耦合矩阵（行主序 3x3：对角为刻度，非对角为轴间耦合；
    /// SDK data[3][4] 前 3 列）。
    std::array<float, 9> scale{};
    /// 三轴零偏（SDK data[3][4] 第 4 列）。
    std::array<float, 3> bias{};
    /// 三轴噪声方差。
    std::array<float, 3> noiseVariances{};
    /// 三轴零偏方差。
    std::array<float, 3> biasVariances{};

    /// 有效性：全部字段有限，且 scale 非全零——全零刻度是"未填充/读取失败"的
    /// 可观察哨兵（真实出厂刻度对角元 ~1）；bias/方差允许全零（标定值可为 0）。
    [[nodiscard]] bool valid() const noexcept;
};

/// 当前流配置下的彩色+深度内参与 IMU 运动内参/外参快照。
struct IntrinsicsSnapshot {
    StreamIntrinsics color;
    StreamIntrinsics depth;
    /// gyro → color 外参（M3-04）：把 IMU 传感器系融合姿态换算到彩色相机系所需；
    /// 运动流未使能、设备无 IMU 或读取失败时保持全零无效值（valid() == false）。
    Extrinsics gyroToColor;
    /// ACCEL 出厂运动内参（M3-04）；无效条件同 gyroToColor。
    MotionIntrinsics accelIntrinsics;
    /// GYRO 出厂运动内参（M3-04）；无效条件同 gyroToColor。
    MotionIntrinsics gyroIntrinsics;
    std::uint64_t sequence = 0;
};

/// IMU 运动流类别（对应设备 ACCEL / GYRO 两个运动传感器流）。
enum class MotionStreamKind {
    Accel,
    Gyro,
};

/// 单个 IMU 三轴采样（MOTION_XYZ32F 语义；运动通道的最小发布单元）。
struct MotionSample {
    /// 采样来源；决定 axes 携带的物理量与单位。
    MotionStreamKind kind = MotionStreamKind::Accel;
    /// kind == Accel：三轴比力，单位 m/s²（含重力；静止时上指轴读 +1g）；
    /// kind == Gyro：三轴角速度，单位 rad/s。均为设备（传感器）坐标系。
    std::array<float, 3> axes{};
    /// 设备时间戳（毫秒；与 Frame::deviceTimestampMs 同源，供融合 dt 与单调校验）。
    double deviceTimestampMs = 0.0;
    /// 运动通道内单调递增序号（ACCEL/GYRO 共用一条通道）。
    std::uint64_t sequence = 0;

    [[nodiscard]] bool valid() const noexcept;
};

/// IMU 源频率统计（采集侧实测滑动频率 + 会话累计样本数，随姿态快照发布）。
struct MotionSourceStats {
    /// 实测陀螺采样频率（Hz）。
    float gyroHz = 0.0f;
    /// 实测加速计采样频率（Hz）。
    float accelHz = 0.0f;
    /// 会话（上次 start）以来累计陀螺样本数。
    std::uint64_t gyroSamples = 0;
    /// 会话（上次 start）以来累计加速计样本数。
    std::uint64_t accelSamples = 0;
};

/// 融合姿态快照（DEC-010）：姿态四元数 + 源频率统计 + 序号，经姿态通道发布。
struct ImuSnapshot {
    /// 姿态单位四元数，标量在前（w, x, y, z）：传感器系 → 世界系 的旋转
    /// （v_world = q ⊗ v_sensor ⊗ q*）。世界系 Z 轴向上（重力沿 −Z），yaw 初值
    /// 为 0——六轴无磁力计，绕重力轴旋转不可观（DEC-010 披露，不宣称绝对航向）。
    /// 本方向约定即 DEC-010 交由 M3-03 锁定的契约。
    std::array<float, 4> orientation{1.0f, 0.0f, 0.0f, 0.0f};
    MotionSourceStats sources;
    /// 姿态通道内单调递增序号。
    std::uint64_t sequence = 0;

    [[nodiscard]] bool valid() const noexcept;
};

struct ResolutionOption {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fps = 0;
};

/// 单台在线设备的静态信息与能力。
struct DeviceInfo {
    std::string name;
    std::string serial;
    std::string firmwareVersion;
    std::vector<ResolutionOption> colorOptions;
    std::vector<ResolutionOption> depthOptions;
    /// 设备是否内置 IMU（存在 ACCEL/GYRO 运动传感器）。
    bool imuSupported = false;
    /// IMU 输出速率档位（Hz，升序去重；如加速计 63/250、陀螺 200/400）；
    /// 仅 imuSupported 时填充。
    std::vector<std::uint32_t> imuAccelRatesHz;
    std::vector<std::uint32_t> imuGyroRatesHz;
};

/// 在线设备目录（热插拔时由 worker 重新枚举并发布；DEC-006）。
struct DeviceCatalog {
    std::vector<DeviceInfo> devices;
    /// 当前活动设备序列号；空 = 尚未选择（多台设备时等待用户选择）。
    std::string activeSerial;
    /// activeSerial 为自动选择（唯一设备）时为 true；false = 用户指定。
    bool activeIsAuto = false;
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

}  // namespace rin
