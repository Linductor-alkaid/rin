#include "rin/camera_types.hpp"

#include <cmath>

#include <chrono>

namespace rin {

const char* toString(CameraServiceState state) noexcept {
    switch (state) {
        case CameraServiceState::Idle:
            return "Idle";
        case CameraServiceState::Opening:
            return "Opening";
        case CameraServiceState::Streaming:
            return "Streaming";
        case CameraServiceState::Restreaming:
            return "Restreaming";
        case CameraServiceState::Waiting:
            return "Waiting";
        case CameraServiceState::Stopping:
            return "Stopping";
        case CameraServiceState::Failed:
            return "Failed";
    }
    return "Unknown";
}

bool operator==(const StreamRequest& lhs, const StreamRequest& rhs) noexcept {
    return lhs.colorWidth == rhs.colorWidth && lhs.colorHeight == rhs.colorHeight &&
           lhs.colorFps == rhs.colorFps && lhs.depthWidth == rhs.depthWidth &&
           lhs.depthHeight == rhs.depthHeight && lhs.depthFps == rhs.depthFps &&
           lhs.enableMotion == rhs.enableMotion;
}

bool operator!=(const StreamRequest& lhs, const StreamRequest& rhs) noexcept {
    return !(lhs == rhs);
}

bool Extrinsics::valid() const noexcept {
    // 全零旋转矩阵是"未填充/读取失败"的可观察哨兵（真实标定的旋转不可能全零），
    // 判无效——消费方据此区分"没有外参"与"外参可用"；非有限值同样拒绝。
    // 平移允许全零（共面/同址安装的平移可为 0，旋转不可为全零）。
    bool rotationPopulated = false;
    for (const float value : rotation) {
        if (!std::isfinite(value)) {
            return false;
        }
        if (value != 0.0f) {
            rotationPopulated = true;
        }
    }
    for (const float value : translation) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return rotationPopulated;
}

bool MotionIntrinsics::valid() const noexcept {
    // 全零刻度矩阵是"未填充/读取失败"的可观察哨兵（真实出厂刻度对角元 ~1，
    // 不可能全零），判无效；bias/方差允许全零（出厂标定零偏与方差可为 0）。
    // 任意字段非有限值拒绝。
    bool scalePopulated = false;
    for (const float value : scale) {
        if (!std::isfinite(value)) {
            return false;
        }
        if (value != 0.0f) {
            scalePopulated = true;
        }
    }
    for (const float value : bias) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    for (const float value : noiseVariances) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    for (const float value : biasVariances) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return scalePopulated;
}

bool MotionSample::valid() const noexcept {
    if (kind != MotionStreamKind::Accel && kind != MotionStreamKind::Gyro) {
        return false;
    }
    for (const float axis : axes) {
        if (!std::isfinite(axis)) {
            return false;
        }
    }
    return true;
}

bool ImuSnapshot::valid() const noexcept {
    double sumSquares = 0.0;
    for (const float component : orientation) {
        if (!std::isfinite(component)) {
            return false;
        }
        sumSquares += static_cast<double>(component) * component;
    }
    // 发布前已归一化的单位四元数；容差覆盖 float 归一化舍入并拦截未初始化/损坏值。
    if (std::fabs(sumSquares - 1.0) > 1e-3) {
        return false;
    }
    return std::isfinite(sources.gyroHz) && sources.gyroHz >= 0.0f &&
           std::isfinite(sources.accelHz) && sources.accelHz >= 0.0f;
}

}  // namespace rin
