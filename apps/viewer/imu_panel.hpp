#pragma once

// IMU 状态面板（M3-07）：源频率（实测 EMA Hz + 会话累计样本）与姿态数值
// （ZYX 欧拉角 + 单位四元数）。数据源与 3D 位姿视图（pose_view.hpp）同为 pump()
// 经 tryLoadPose() 消费、按流状态门控/排空后的最新 ImuSnapshot——本单元不新增
// 通道消费（原始运动通道 tryLoadMotion() 仅作诊断，不进面板）。
//
// 边界（RULE-01/05/07）：只读快照做无堆分配的数值换算与有界文本组装；不做融合、
// 不做阻塞等待、不在渲染线程执行 CPU 密集处理。欧拉分解为展示用纯函数（视角
// 读数，非契约类型），位于 viewer 层与 PoseViewState 同模式（可 headless 单测）。
//
// 姿态约定（DEC-010/M3-03 契约）：四元数标量在前 (w,x,y,z)，传感器系→世界系，
// 世界系 Z 轴向上；六轴 yaw 初值为 0 且绕重力轴漂移不可观，读数如实显示。

#include "pose_view.hpp"
#include "viewer_components.hpp"
#include "viewer_theme.hpp"

#include <eui_neo.h>

#include <rin/camera_types.hpp>
#include <rin/pose_math.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace viewer {

/// ZYX 欧拉角读数（度）：把 传感器系→世界系 姿态分解为绕 X（roll）、Y（pitch）、
/// 世界 Z（yaw）的旋转。pitch 经 asin 表示域天然截断 ±90°（万向锁附近欧拉读数
/// 仅参考，3D 视图不受影响——视图直接消费四元数）；yaw ∈ (−180°, 180°]。
struct ImuAttitudeDegrees {
    float roll = 0.0f;
    float pitch = 0.0f;
    float yaw = 0.0f;
};

/// 姿态四元数 → ZYX 欧拉角（度）。输入先经 quatNormalize 消毒（与 Core 契约
/// 一致，退化输入回恒等而非传播 NaN）。
[[nodiscard]] inline ImuAttitudeDegrees attitudeDegreesFromOrientation(
    const std::array<float, 4>& orientation) noexcept {
    // world←sensor 行主序 R = Rz(yaw)·Ry(pitch)·Rx(roll)：
    // sinθ = −r20、roll = atan2(r21, r22)、yaw = atan2(r10, r00)。
    const rin::PoseMat3 r = rin::orientationToMatrix(rin::quatNormalize(orientation));
    constexpr float kRadToDeg = 57.29577951308232f;
    ImuAttitudeDegrees out;
    out.roll = std::atan2(r[7], r[8]) * kRadToDeg;
    out.pitch = std::asin(std::clamp(-r[6], -1.0f, 1.0f)) * kRadToDeg;
    out.yaw = std::atan2(r[3], r[0]) * kRadToDeg;
    return out;
}

/// 单源频率行文本："399.5 Hz · 123456 smp"（实测 EMA 频率 + 会话累计样本）。
[[nodiscard]] inline std::string formatSourceRate(float hz, std::uint64_t samples) {
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.1f Hz · %llu smp",
                  static_cast<double>(hz), static_cast<unsigned long long>(samples));
    return buffer;
}

/// 姿态数值行文本："+1.2° · −0.4° · 359.8°"（roll · pitch · yaw，度）。
[[nodiscard]] inline std::string formatAttitudeDegrees(const ImuAttitudeDegrees& attitude) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%+.1f° · %+.1f° · %+.1f°",
                  static_cast<double>(attitude.roll), static_cast<double>(attitude.pitch),
                  static_cast<double>(attitude.yaw));
    return buffer;
}

/// 单位四元数行文本："w 0.999 · x 0.012 · y −0.005 · z 0.003"（标量在前契约）。
[[nodiscard]] inline std::string formatOrientationQuaternion(
    const std::array<float, 4>& orientation) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "w %.3f · x %.3f · y %.3f · z %.3f",
                  static_cast<double>(orientation[0]), static_cast<double>(orientation[1]),
                  static_cast<double>(orientation[2]), static_cast<double>(orientation[3]));
    return buffer;
}

/// 合成 IMU 状态面板卡片（与 Intrinsics 卡同卡片语汇，DEC-005 令牌）。快照不可用
/// （未使能运动流/设备无 IMU/流重建窗口/已排空）时各行为 "n/a"，通道恢复后随
/// 最新快照自动刷新。行集合：Gyro/Accel 源频率、R·P·Y 姿态数值、四元数。
inline void composeImuPanelCard(eui::Ui& ui, const PoseViewState& pose, float width,
                                float height, float x, float y) {
    using namespace viewer::theme;  // 语义令牌（DEC-005）；函数内引入，不泄漏头文件作用域
    const ThemeTokens& tokens = dark();

    ui.stack("imu")
        .position(x, y)
        .size(width, height)
        .content([&] {
            // 卡片底 + 标题 + 名称/值行列表：共享脚手架（M12/CR-33、CR-38）。
            const CardGeometry card = composeCardShell(ui, tokens, "imu", "IMU", width, height);

            const bool hasSnapshot = pose.available && pose.snapshot.valid();
            const ImuAttitudeDegrees attitude =
                hasSnapshot ? attitudeDegreesFromOrientation(pose.snapshot.orientation)
                            : ImuAttitudeDegrees{};
            const char* names[] = {"Gyro", "Accel", "R·P·Y", "Quat"};
            const char* rowIds[] = {"row0", "row1", "row2", "row3"};
            const std::string values[] = {
                hasSnapshot ? formatSourceRate(pose.snapshot.sources.gyroHz,
                                               pose.snapshot.sources.gyroSamples)
                            : std::string("n/a"),
                hasSnapshot ? formatSourceRate(pose.snapshot.sources.accelHz,
                                               pose.snapshot.sources.accelSamples)
                            : std::string("n/a"),
                hasSnapshot ? formatAttitudeDegrees(attitude) : std::string("n/a"),
                hasSnapshot ? formatOrientationQuaternion(pose.snapshot.orientation)
                            : std::string("n/a"),
            };
            composeLabeledRows(ui, tokens, "imu", card.pad, card.contentTop, width,
                               kLabeledRowsValueX, 4, rowIds, names, values);

            if (hasSnapshot) {
                // 快照序号（姿态通道会话单调递增）：通道活性的可观察读数。
                char sequence[24];
                std::snprintf(sequence, sizeof(sequence), "#%llu",
                              static_cast<unsigned long long>(pose.snapshot.sequence));
                ui.text("imu.sequence")
                    .position(width - card.pad - kLabeledRowsMetaWidth, card.contentTop)
                    .size(kLabeledRowsMetaWidth, kFontBase + kSpace2)
                    .text(sequence)
                    .fontSize(kFontXs)
                    .color(tokens.fgSubtlest)
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .build();
            }
        })
        .build();
}

}  // namespace viewer
