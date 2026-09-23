#include "camera_state_machine.hpp"

namespace rin::detail {

bool CameraStateMachine::isAllowed(CameraServiceState from, CameraServiceState to) noexcept {
    if (from == to) {
        // 幂等：同状态转换视为成功（重复 stop、重复进入同状态）。
        return true;
    }
    switch (from) {
        case CameraServiceState::Idle:
            return to == CameraServiceState::Opening;
        case CameraServiceState::Opening:
            // 设备未接入时进入 Waiting（设计稳态，DEC-006）。
            return to == CameraServiceState::Streaming || to == CameraServiceState::Waiting ||
                   to == CameraServiceState::Failed || to == CameraServiceState::Stopping;
        case CameraServiceState::Streaming:
            return to == CameraServiceState::Restreaming ||
                   to == CameraServiceState::Waiting ||  // 活动设备被移除
                   to == CameraServiceState::Stopping || to == CameraServiceState::Failed;
        case CameraServiceState::Restreaming:
            return to == CameraServiceState::Streaming ||
                   to == CameraServiceState::Waiting ||  // 切换目标设备被移除
                   to == CameraServiceState::Failed || to == CameraServiceState::Stopping;
        case CameraServiceState::Waiting:
            // 设备到达或用户选定后重新尝试打开；等待中亦可直接停止。
            return to == CameraServiceState::Opening || to == CameraServiceState::Stopping;
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

}  // namespace rin::detail
