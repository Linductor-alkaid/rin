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
    for (const float value : rotation) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    for (const float value : translation) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return true;
}

bool MotionIntrinsics::valid() const noexcept {
    for (const float value : scale) {
        if (!std::isfinite(value)) {
            return false;
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
    return true;
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
