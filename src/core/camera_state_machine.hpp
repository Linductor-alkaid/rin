#pragma once

#include <atomic>
#include <string>

#include "rin/camera_types.hpp"

namespace rin::detail {

/// 相机服务显式状态机。转换规则集中于此，非法转换被拒绝并给出原因；
/// 终态（Idle/Failed 上的 stop 幂等）由调用方配合 isAllowed() 保证。
class CameraStateMachine {
public:
    /// 纯函数转换表；单测覆盖全矩阵。
    [[nodiscard]] static bool isAllowed(CameraServiceState from, CameraServiceState to) noexcept;

    /// 尝试转换；失败时返回 false 并填写 error。
    bool transitionTo(CameraServiceState next, std::string* error = nullptr) noexcept;

    [[nodiscard]] CameraServiceState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

private:
    std::atomic<CameraServiceState> state_{CameraServiceState::Idle};
};

}  // namespace rin::detail
