#include "camera_state_machine.hpp"

namespace rsv::detail {

bool CameraStateMachine::isAllowed(CameraServiceState from, CameraServiceState to) noexcept {
    if (from == to) {
        // 幂等：同状态转换视为成功（重复 stop、重复进入同状态）。
        return true;
    }
    switch (from) {
        case CameraServiceState::Idle:
            return to == CameraServiceState::Opening;
        case CameraServiceState::Opening:
            return to == CameraServiceState::Streaming || to == CameraServiceState::Failed ||
                   to == CameraServiceState::Stopping;
        case CameraServiceState::Streaming:
            return to == CameraServiceState::Restreaming || to == CameraServiceState::Stopping ||
                   to == CameraServiceState::Failed;
        case CameraServiceState::Restreaming:
            return to == CameraServiceState::Streaming || to == CameraServiceState::Failed ||
                   to == CameraServiceState::Stopping;
        case CameraServiceState::Stopping:
            return to == CameraServiceState::Idle;
        case CameraServiceState::Failed:
            return to == CameraServiceState::Stopping;
    }
    return false;
}

bool CameraStateMachine::transitionTo(CameraServiceState next, std::string* error) noexcept {
    const CameraServiceState current = state();
    if (!isAllowed(current, next)) {
        if (error != nullptr) {
            *error = "illegal transition " + std::string(toString(current)) + " -> " +
                     toString(next);
        }
        return false;
    }
    if (current == next) {
        return true;
    }
    state_.store(next, std::memory_order_acq_rel);
    return true;
}

}  // namespace rsv::detail
