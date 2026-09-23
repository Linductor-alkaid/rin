#include "imu_fuser.hpp"

namespace rin::detail {

void IdentityImuFuser::advance(const MotionSample& sample) {
    (void)sample;  // 假实现不消费采样内容；真实融合见 M3-05（DEC-010 Mahony）。
    hasPose_ = true;
}

std::array<float, 4> IdentityImuFuser::orientation() const noexcept {
    // 恒等姿态：契约默认值 {1,0,0,0}（DEC-010 严格恒等约定，M3-03 契约测试锁定）。
    return {1.0f, 0.0f, 0.0f, 0.0f};
}

void IdentityImuFuser::reset() noexcept {
    hasPose_ = false;  // 幂等：重复复位到同一构造初态。
}

std::unique_ptr<ImuFuser> createImuFuser() {
    return std::make_unique<IdentityImuFuser>();
}

}  // namespace rin::detail
