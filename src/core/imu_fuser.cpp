#include "imu_fuser.hpp"

#include <cmath>

namespace rin::detail {
namespace {

/// 标准重力（m/s²）：1g 门限基准（DEC-010；运动流比力输出单位 m/s²，静止上指轴 +1g）。
constexpr double kStandardGravity = 9.80665;
/// 重力参考门限：|‖a‖ − 1g| 超过 1g 的 10% 时跳过修正与零偏更新（DEC-010 冻结值）。
constexpr double kGravityGateRatio = 0.10;
/// SO(3) 指数映射的小角度分支阈值（rad）：低于该值一阶展开与精确式不可区分。
constexpr double kSmallAngleRad = 1e-8;
/// 两向量对齐的近平行判定余量（点积距离）：对应对齐角误差 ≤ ~4.5e-5 rad。
constexpr double kParallelEpsilon = 1e-9;

using Quatd = std::array<double, 4>;  /// (w, x, y, z)，标量在前
using Vec3d = std::array<double, 3>;

bool allFinite(const Quatd& q) noexcept {
    for (const double component : q) {
        if (!std::isfinite(component)) {
            return false;
        }
    }
    return true;
}

bool allFinite(const Vec3d& v) noexcept {
    for (const double component : v) {
        if (!std::isfinite(component)) {
            return false;
        }
    }
    return true;
}

/// 原地单位化四元数；零范数/非有限回退恒等（防御性，正常推进路径不会触发）。
void normalizeQuaternion(Quatd& q) noexcept {
    const double normSq = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (!(normSq > 0.0) || !std::isfinite(normSq)) {
        q = Quatd{1.0, 0.0, 0.0, 0.0};
        return;
    }
    const double invNorm = 1.0 / std::sqrt(normSq);
    for (double& component : q) {
        component *= invNorm;
    }
}

/// 四元数乘积 lhs ⊗ rhs（Hamilton 约定，与契约 v_world = q ⊗ v_sensor ⊗ q* 一致）。
Quatd quaternionMultiply(const Quatd& lhs, const Quatd& rhs) noexcept {
    const double w = lhs[0];
    const double x = lhs[1];
    const double y = lhs[2];
    const double z = lhs[3];
    return {w * rhs[0] - x * rhs[1] - y * rhs[2] - z * rhs[3],
            w * rhs[1] + x * rhs[0] + y * rhs[3] - z * rhs[2],
            w * rhs[2] - x * rhs[3] + y * rhs[0] + z * rhs[1],
            w * rhs[3] + x * rhs[2] - y * rhs[1] + z * rhs[0]};
}

/// 传感器系角速度 ω（rad/s）在 dt 秒内的精确旋转增量 exp(½·ω·dt)（右乘因子）。
/// 精确指数映射保证任意 dt（含流间隙的单步大 dt）下结果仍为单位四元数。
Quatd rotationIncrement(const Vec3d& omegaRadS, double dtSeconds) noexcept {
    const Vec3d theta{omegaRadS[0] * dtSeconds, omegaRadS[1] * dtSeconds,
                      omegaRadS[2] * dtSeconds};
    const double angle =
        std::sqrt(theta[0] * theta[0] + theta[1] * theta[1] + theta[2] * theta[2]);
    if (!(angle > kSmallAngleRad)) {
        return {1.0, 0.5 * theta[0], 0.5 * theta[1], 0.5 * theta[2]};  // 一阶展开
    }
    const double halfAngle = 0.5 * angle;
    const double sinc = std::sin(halfAngle) / angle;
    return {std::cos(halfAngle), sinc * theta[0], sinc * theta[1], sinc * theta[2]};
}

/// 估计的世界上方向在传感器系的表达：ĝ = R(q)ᵀ·ẑ（R 为 q 对应的旋转）。
Vec3d estimatedUpInSensor(const Quatd& q) noexcept {
    const double w = q[0];
    const double x = q[1];
    const double y = q[2];
    const double z = q[3];
    return {2.0 * (x * z - w * y), 2.0 * (w * x + y * z), w * w - x * x - y * y + z * z};
}

/// 单位向量 u → v 的最小旋转四元数（结果满足 R(q)·u = v）。u ≈ v 时精确返回恒等
/// （M3-04 接缝契约依赖 +1g 对齐的精确恒等）；u ≈ −v（反平行）取含固定辅助轴的
/// 180° 旋转。返回值未单位化，调用方归一化。
Quatd minimalRotation(const Vec3d& from, const Vec3d& to) noexcept {
    const double dot = from[0] * to[0] + from[1] * to[1] + from[2] * to[2];
    if (dot >= 1.0 - kParallelEpsilon) {
        return Quatd{1.0, 0.0, 0.0, 0.0};
    }
    if (dot <= -1.0 + kParallelEpsilon) {
        const Vec3d auxiliary = std::fabs(from[0]) < 0.9 ? Vec3d{1.0, 0.0, 0.0}
                                                         : Vec3d{0.0, 1.0, 0.0};
        Vec3d axis{from[1] * auxiliary[2] - from[2] * auxiliary[1],
                   from[2] * auxiliary[0] - from[0] * auxiliary[2],
                   from[0] * auxiliary[1] - from[1] * auxiliary[0]};
        const double norm = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] +
                                      axis[2] * axis[2]);
        axis[0] /= norm;
        axis[1] /= norm;
        axis[2] /= norm;
        return Quatd{0.0, axis[0], axis[1], axis[2]};
    }
    // 半角公式：q = normalize((1 + u·v, u×v))。
    return {1.0 + dot,
            from[1] * to[2] - from[2] * to[1],
            from[2] * to[0] - from[0] * to[2],
            from[0] * to[1] - from[1] * to[0]};
}

}  // namespace

MahonyImuFuser::MahonyImuFuser(const Params& params) : params_(params) {
    restoreInitialState();
}

void MahonyImuFuser::advance(const MotionSample& sample) {
    // 接缝契约：首个有效采样推进后姿态可用（与样本类别无关）。
    hasPose_ = true;
    if (sample.kind == MotionStreamKind::Gyro) {
        advanceGyro(sample);
    } else {
        advanceAccel(sample);
    }
}

std::array<float, 4> MahonyImuFuser::orientation() const noexcept {
    return {static_cast<float>(q_[0]), static_cast<float>(q_[1]),
            static_cast<float>(q_[2]), static_cast<float>(q_[3])};
}

void MahonyImuFuser::reset() noexcept {
    restoreInitialState();  // 幂等：恢复到同一构造初态。
}

std::array<double, 3> MahonyImuFuser::gyroBiasEstimate() const noexcept {
    return bias_;
}

void MahonyImuFuser::advanceGyro(const MotionSample& sample) noexcept {
    const double timestampMs = sample.deviceTimestampMs;
    if (!std::isfinite(timestampMs)) {
        return;  // valid() 不约束时间戳域：损坏时间戳不进入积分，状态不变。
    }
    if (!hasGyroTs_) {
        gyroTsMs_ = timestampMs;  // 首个样本只建立参考点（无 dt，不传播）。
        hasGyroTs_ = true;
        return;
    }
    const double dtMs = timestampMs - gyroTsMs_;
    if (dtMs <= 0.0) {
        return;  // 时间戳单调守卫：重复/乱序样本不传播，参考点保持不动。
    }
    gyroTsMs_ = timestampMs;
    const Vec3d biasCompensatedRate{static_cast<double>(sample.axes[0]) - bias_[0],
                                    static_cast<double>(sample.axes[1]) - bias_[1],
                                    static_cast<double>(sample.axes[2]) - bias_[2]};
    q_ = quaternionMultiply(q_, rotationIncrement(biasCompensatedRate, dtMs * 1e-3));
    normalizeQuaternion(q_);
}

void MahonyImuFuser::advanceAccel(const MotionSample& sample) noexcept {
    const double timestampMs = sample.deviceTimestampMs;
    if (!std::isfinite(timestampMs)) {
        return;
    }
    const Vec3d axes{static_cast<double>(sample.axes[0]), static_cast<double>(sample.axes[1]),
                     static_cast<double>(sample.axes[2])};
    const double norm = std::sqrt(axes[0] * axes[0] + axes[1] * axes[1] + axes[2] * axes[2]);
    if (!(norm > 0.0) || !std::isfinite(norm)) {
        return;  // 零范数/溢出：无重力方向可言，状态与参考点不动。
    }
    const double dtMs = hasAccelTs_ ? timestampMs - accelTsMs_ : 0.0;
    accelTsMs_ = timestampMs;  // 修正 dt 参考点按样本推进（对齐/门限跳过不冻结窗口）。
    hasAccelTs_ = true;

    // 1g 门限（DEC-010）：线加速度/振动污染防护——对齐、修正与零偏更新一并跳过，
    // 门限期间 roll/pitch 由零偏估计兜底（陀螺传播继续在 GYRO 分支进行）。
    if (std::fabs(norm - kStandardGravity) > kGravityGateRatio * kStandardGravity) {
        return;
    }

    if (!aligned_) {
        // DEC-010 初值语义：首个通过门限的有效 ACCEL 样本一次性重力对齐——
        // roll/pitch 对齐测量方向、yaw 置 0（最小旋转天然不含绕重力轴分量）。
        const Vec3d measuredUp{axes[0] / norm, axes[1] / norm, axes[2] / norm};
        q_ = minimalRotation(measuredUp, Vec3d{0.0, 0.0, 1.0});
        normalizeQuaternion(q_);
        aligned_ = true;
        return;
    }
    if (dtMs <= 0.0) {
        return;  // 重复/乱序样本不修正。
    }

    const Vec3d measuredUp{axes[0] / norm, axes[1] / norm, axes[2] / norm};
    const Vec3d estimatedUp = estimatedUpInSensor(q_);
    // 误差 e = â × ĝ（测量重力方向 × 估计重力方向，传感器系）。
    const Vec3d error{measuredUp[1] * estimatedUp[2] - measuredUp[2] * estimatedUp[1],
                      measuredUp[2] * estimatedUp[0] - measuredUp[0] * estimatedUp[2],
                      measuredUp[0] * estimatedUp[1] - measuredUp[1] * estimatedUp[0]};
    const double dtSeconds = dtMs * 1e-3;
    // 积分反馈（I）：零偏估计跟踪，b̂ -= 2·Ki·e·dt（经典离散化的 2 倍系数）。
    for (int axis = 0; axis < 3; ++axis) {
        bias_[axis] -= 2.0 * params_.ki * error[axis] * dtSeconds;
    }
    // 比例反馈（P）：按修正角速率 2·Kp·e 右乘指数映射。
    const Vec3d correctionRate{2.0 * params_.kp * error[0], 2.0 * params_.kp * error[1],
                               2.0 * params_.kp * error[2]};
    q_ = quaternionMultiply(q_, rotationIncrement(correctionRate, dtSeconds));
    normalizeQuaternion(q_);
}

void MahonyImuFuser::restoreInitialState() noexcept {
    q_ = params_.initialOrientation;
    if (!allFinite(q_)) {
        q_ = Quatd{1.0, 0.0, 0.0, 0.0};
    }
    normalizeQuaternion(q_);  // 非单位初值归一化，零范数回退恒等。
    bias_ = params_.initialGyroBias;
    if (!allFinite(bias_)) {
        for (double& component : bias_) {
            if (!std::isfinite(component)) {
                component = 0.0;
            }
        }
    }
    hasPose_ = false;
    aligned_ = false;
    hasGyroTs_ = false;
    gyroTsMs_ = 0.0;
    hasAccelTs_ = false;
    accelTsMs_ = 0.0;
}

void IdentityImuFuser::advance(const MotionSample& sample) {
    (void)sample;  // 假实现不消费采样内容；保留用于离线接缝对照。
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
    return std::make_unique<MahonyImuFuser>();
}

}  // namespace rin::detail
