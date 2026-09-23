#include "rs_vision/camera_types.hpp"

#include <chrono>

namespace rsv {

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
           lhs.depthHeight == rhs.depthHeight && lhs.depthFps == rhs.depthFps;
}

bool operator!=(const StreamRequest& lhs, const StreamRequest& rhs) noexcept {
    return !(lhs == rhs);
}

}  // namespace rsv
