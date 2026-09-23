#include "rin/pose_math.hpp"

#include <algorithm>
#include <cmath>

namespace rin {
namespace {

/// 四元数范数平方的消毒下限（低于视为零范数，回退恒等）。
constexpr float kQuatNormEpsilon = 1e-12f;
/// 线段屏幕长度低于该值视为退化（宽度四边形无意义）。
constexpr float kSegmentMinLengthPx = 1e-4f;
/// orbit 仰角截断（~±89°）：up = +Z 的 look-at 在 ±90° 退化（right 零向量）。
constexpr float kOrbitMaxElevation = 1.5533f;

}  // namespace

std::array<float, 4> quatNormalize(const std::array<float, 4>& q) noexcept {
    float normSq = 0.0f;
    for (const float c : q) {
        if (!std::isfinite(c)) {
            return {1.0f, 0.0f, 0.0f, 0.0f};
        }
        normSq += c * c;
    }
    if (normSq <= kQuatNormEpsilon) {
        return {1.0f, 0.0f, 0.0f, 0.0f};
    }
    const float inv = 1.0f / std::sqrt(normSq);
    return {q[0] * inv, q[1] * inv, q[2] * inv, q[3] * inv};
}

std::array<float, 4> quatConjugate(const std::array<float, 4>& q) noexcept {
    return {q[0], -q[1], -q[2], -q[3]};
}

std::array<float, 4> quatMultiply(const std::array<float, 4>& a,
                                  const std::array<float, 4>& b) noexcept {
    const float w = a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3];
    const float x = a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2];
    const float y = a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
    const float z = a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
    return {w, x, y, z};
}

PoseMat3 orientationToMatrix(const std::array<float, 4>& orientation) noexcept {
    const std::array<float, 4> q = quatNormalize(orientation);
    const float xx = q[1] * q[1], yy = q[2] * q[2], zz = q[3] * q[3];
    const float xy = q[1] * q[2], xz = q[1] * q[3], yz = q[2] * q[3];
    const float wx = q[0] * q[1], wy = q[0] * q[2], wz = q[0] * q[3];
    return {1.0f - 2.0f * (yy + zz), 2.0f * (xy - wz), 2.0f * (xz + wy),
            2.0f * (xy + wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz - wx),
            2.0f * (xz - wy), 2.0f * (yz + wx), 1.0f - 2.0f * (xx + yy)};
}

PoseMat3 transposeMatrix3(const PoseMat3& m) noexcept {
    return {m[0], m[3], m[6],
            m[1], m[4], m[7],
            m[2], m[5], m[8]};
}

PoseMat3 multiplyMatrix3(const PoseMat3& a, const PoseMat3& b) noexcept {
    PoseMat3 out{};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k) {
                sum += a[static_cast<std::size_t>(row) * 3 + k] *
                       b[static_cast<std::size_t>(k) * 3 + col];
            }
            out[static_cast<std::size_t>(row) * 3 + col] = sum;
        }
    }
    return out;
}

PoseMat3 matrix3FromColumnMajor(const std::array<float, 9>& m) noexcept {
    return {m[0], m[3], m[6],
            m[1], m[4], m[7],
            m[2], m[5], m[8]};
}

PoseVec3 rotateVector(const PoseMat3& m, const PoseVec3& v) noexcept {
    return {m[0] * v.x + m[1] * v.y + m[2] * v.z,
            m[3] * v.x + m[4] * v.y + m[5] * v.z,
            m[6] * v.x + m[7] * v.y + m[8] * v.z};
}

OrbitView makeOrbitView(float azimuthRad, float elevationRad, float distance) noexcept {
    if (!std::isfinite(azimuthRad)) {
        azimuthRad = 0.0f;
    }
    if (!std::isfinite(elevationRad)) {
        elevationRad = 0.0f;
    }
    elevationRad = std::clamp(elevationRad, -kOrbitMaxElevation, kOrbitMaxElevation);
    if (!std::isfinite(distance) || distance <= 0.0f) {
        distance = 1.0f;
    }

    const float ce = std::cos(elevationRad), se = std::sin(elevationRad);
    const float ca = std::cos(azimuthRad), sa = std::sin(azimuthRad);
    const PoseVec3 eye{distance * ce * ca, distance * ce * sa, distance * se};
    // forward = normalize(origin - eye)。
    const float n = std::sqrt(eye.x * eye.x + eye.y * eye.y + eye.z * eye.z);
    const PoseVec3 fwd{-eye.x / n, -eye.y / n, -eye.z / n};
    // right = normalize(fwd x up)，up = 世界 +Z；仰角已截断，水平分量恒非零。
    PoseVec3 right{fwd.y, -fwd.x, 0.0f};
    const float rn = std::sqrt(right.x * right.x + right.y * right.y);
    right = {right.x / rn, right.y / rn, 0.0f};
    const PoseVec3 up{right.y * fwd.z - right.z * fwd.y,
                      right.z * fwd.x - right.x * fwd.z,
                      right.x * fwd.y - right.y * fwd.x};
    // 行主序：view = [right; up; fwd]（view z 轴指向场景深度为正）。
    return OrbitView{eye,
                     {right.x, right.y, right.z,
                      up.x, up.y, up.z,
                      fwd.x, fwd.y, fwd.z}};
}

PoseVec3 worldToView(const OrbitView& view, const PoseVec3& point) noexcept {
    const PoseVec3 d{point.x - view.eye.x, point.y - view.eye.y, point.z - view.eye.z};
    return rotateVector(view.rotation, d);
}

bool projectToScreen(const PoseProjector& projector, const PoseVec3& viewPoint,
                     PoseVec2& out) noexcept {
    if (!std::isfinite(viewPoint.z) || viewPoint.z <= projector.nearZ) {
        return false;
    }
    const PoseVec2 projected{projector.center.x + projector.focal * viewPoint.x / viewPoint.z,
                             projector.center.y - projector.focal * viewPoint.y / viewPoint.z};
    if (!std::isfinite(projected.x) || !std::isfinite(projected.y)) {
        return false;
    }
    out = projected;
    return true;
}

bool segmentToQuad(const PoseProjector& projector, const PoseVec3& aView,
                   const PoseVec3& bView, float widthPx,
                   std::array<PoseVec2, 4>& out) noexcept {
    PoseVec2 a{};
    PoseVec2 b{};
    if (!projectToScreen(projector, aView, a) || !projectToScreen(projector, bView, b)) {
        return false;
    }
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (!std::isfinite(widthPx) || widthPx <= 0.0f || len < kSegmentMinLengthPx) {
        return false;
    }
    const float nx = -dy / len * widthPx * 0.5f;
    const float ny = dx / len * widthPx * 0.5f;
    out = {PoseVec2{a.x + nx, a.y + ny},
           PoseVec2{b.x + nx, b.y + ny},
           PoseVec2{b.x - nx, b.y - ny},
           PoseVec2{a.x - nx, a.y - ny}};
    return true;
}

}  // namespace rin
