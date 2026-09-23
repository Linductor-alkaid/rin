// M3-06 3D 位姿视图投影数学测试（独立验证；include/rin/pose_math.hpp、
// src/core/pose_math.cpp、DEC-011 冻结验证方式 1"投影数学单测"）。
//
// 完成判据（里程碑 M3-06：投影数学单测通过）与 DEC-011 验证方式 1 对照：
// - 已知位姿四元数 + 固定观察相机 → 期望屏幕点集（golden，固定容差）：本测以
//   double 精度、与实现不同算法路径的测试侧参考（Hamilton 三积 q⊗v⊗q* 旋转、
//   标准 look-at、透视除法）端到端对照 Core 链路
//   orientationToMatrix → rotateVector/worldToView → projectToScreen；
// - 退化输入全定义（实现自述契约，头文件 pose_math.hpp:39-41/71-74/86-101）：
//   非有限/零范数四元数回退恒等、orbit distance<=0/非有限按 1、azimuth/elevation
//   非有限按 0、仰角 ±89° 截断（up=+Z look-at 的 ±90° 奇异防护）、近平面后端点
//   与退化线段剔除且不写出参、widthPx 非有限/<=0 拒绝；
// - 四元数规范化、共轭、Hamilton 乘积（含结合方向契约"先施 b 后施 a"）、
//   四元数→矩阵（sensor→world，正交性 det=+1、与参考旋转逐点一致、pitch≈±90°
//   无表示奇异——DEC-011 关切）、矩阵转置/乘法/列主序外参布局
//   （camera_types.hpp Extrinsics::rotation 契约）、orbit look-at 语义
//   （原点深度 = distance、世界上方 = 视图上方、方位/仰角轴）、透视投影 y 向下
//   屏幕约定与 <=nearZ 剔除边界、线段→等宽四边形（法向半宽、点序 a+n,b+n,b−n,a−n）。
//
// 方法（独立验证裁定）：解析用例全部以二元精确值/宽裕量容差断言（不锁定私有
// 常量：零范数 epsilon 与最小段长只验证"存在合理阈值"，两侧留 ≥6 个数量级
// 裕量；仰角截断按公开契约 ±89° 验证而非私有常量 1.5533）；golden 用例容差
// 0.05 px（float32 链路 vs double 参考在 ~600 px 量级上的噪声上界 ~1e-2 px，
// 5 倍裕量且远低于可感知阈）。姿态驱动旋转与固定世界系以不变量锁定：同一姿态
// 下世界参考点投影与位姿无关，相机指点随位姿显著移动。
//
// DOD-02 适用性说明：pose_math 全部为无堆分配的值语义纯函数（头文件
// pose_math.hpp:18-19），单线程、无线程/队列/任务提交/取消/超时/shutdown 语义，
// 并发矩阵不适用；姿态快照的跨上下文发布（采集 worker → LatestMailbox → pump）
// 不在本单元，由 motion_ingest 覆盖（其 tsan 复跑在 M3-04 已入档，M3-07 承接
// 关闭回归）。
#include "test_util.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

#include "rin/pose_math.hpp"

namespace {

using rin::PoseMat3;
using rin::PoseProjector;
using rin::PoseVec2;
using rin::PoseVec3;

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

// --- 近似比较辅助（float 实际值 vs double 期望值）---

bool nearF(float actual, double expected, double tolerance) {
    return std::fabs(static_cast<double>(actual) - expected) <= tolerance;
}

bool quatNear(const std::array<float, 4>& actual, const std::array<double, 4>& expected,
              double tolerance) {
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(static_cast<double>(actual[i]) - expected[i]) > tolerance) {
            return false;
        }
    }
    return true;
}

bool matNear(const PoseMat3& actual, const std::array<double, 9>& expected,
             double tolerance) {
    for (int i = 0; i < 9; ++i) {
        if (std::fabs(static_cast<double>(actual[i]) - expected[i]) > tolerance) {
            return false;
        }
    }
    return true;
}

bool vec3Near(const PoseVec3& actual, double x, double y, double z, double tolerance) {
    return std::fabs(static_cast<double>(actual.x) - x) <= tolerance &&
           std::fabs(static_cast<double>(actual.y) - y) <= tolerance &&
           std::fabs(static_cast<double>(actual.z) - z) <= tolerance;
}

bool vec2Near(const PoseVec2& actual, double x, double y, double tolerance) {
    return std::fabs(static_cast<double>(actual.x) - x) <= tolerance &&
           std::fabs(static_cast<double>(actual.y) - y) <= tolerance;
}

/// 四边形逐点精确相等（PoseVec2 无 operator==；用于"失败不写出参"哨兵比对）。
bool quadExact(const std::array<PoseVec2, 4>& actual,
               const std::array<PoseVec2, 4>& expected) {
    for (int i = 0; i < 4; ++i) {
        if (actual[i].x != expected[i].x || actual[i].y != expected[i].y) {
            return false;
        }
    }
    return true;
}

// --- 测试侧独立参考数学（double 精度；不调用被测函数。
//     Hamilton 三积旋转与矩阵公式是两条独立路径，交叉验证实现）---

using RefQuat = std::array<double, 4>;
using RefMat3 = std::array<double, 9>;

RefQuat refQuatMul(const RefQuat& a, const RefQuat& b) {
    return {a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
            a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
            a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
            a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]};
}

RefQuat refQuatConj(const RefQuat& q) { return {q[0], -q[1], -q[2], -q[3]}; }

/// v_world = q ⊗ v ⊗ q*（sensor→world，DEC-010/M3-03 锁定语义）。
std::array<double, 3> refRotate(const RefQuat& q, const std::array<double, 3>& v) {
    const RefQuat p{0.0, v[0], v[1], v[2]};
    const RefQuat r = refQuatMul(refQuatMul(q, p), refQuatConj(q));
    return {r[1], r[2], r[3]};
}

/// ZYX 欧拉（度）：q = qz ⊗ qy ⊗ qx。
RefQuat refQuatEulerZYX(double rollDeg, double pitchDeg, double yawDeg) {
    const double cr = std::cos(0.5 * rollDeg * kDeg), sr = std::sin(0.5 * rollDeg * kDeg);
    const double cp = std::cos(0.5 * pitchDeg * kDeg), sp = std::sin(0.5 * pitchDeg * kDeg);
    const double cy = std::cos(0.5 * yawDeg * kDeg), sy = std::sin(0.5 * yawDeg * kDeg);
    return {cy * cp * cr + sy * sp * sr,
            cy * cp * sr - sy * sp * cr,
            cy * sp * cr + sy * cp * sr,
            sy * cp * cr - cy * sp * sr};
}

/// 标准四元数→旋转矩阵（unit q，行主序；与 Hamilton 旋转定义等价的教科书式）。
RefMat3 refQuatMatrix(const RefQuat& q) {
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    return {1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - w * z), 2.0 * (x * z + w * y),
            2.0 * (x * y + w * z), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z - w * x),
            2.0 * (x * z - w * y), 2.0 * (y * z + w * x), 1.0 - 2.0 * (x * x + y * y)};
}

std::array<double, 3> refMatVec(const RefMat3& m, const std::array<double, 3>& v) {
    return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
            m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
            m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}

/// 标准 orbit look-at（DEC-011 约定：注视原点，up = 世界 +Z，view z 指向深度为正）。
struct RefOrbit {
    std::array<double, 3> eye{};
    RefMat3 rotation{};
};

RefOrbit refOrbitView(double azimuthRad, double elevationRad, double distance) {
    const double ce = std::cos(elevationRad), se = std::sin(elevationRad);
    const double ca = std::cos(azimuthRad), sa = std::sin(azimuthRad);
    const std::array<double, 3> eye{distance * ce * ca, distance * ce * sa,
                                    distance * se};
    const double n = std::sqrt(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]);
    const std::array<double, 3> fwd{-eye[0] / n, -eye[1] / n, -eye[2] / n};
    // right = normalize(fwd × up)，up = ẑ：(fwd × ẑ) = (fwd.y, −fwd.x, 0)。
    std::array<double, 3> right{fwd[1], -fwd[0], 0.0};
    const double rn = std::sqrt(right[0] * right[0] + right[1] * right[1]);
    right = {right[0] / rn, right[1] / rn, 0.0};
    const std::array<double, 3> up{right[1] * fwd[2] - right[2] * fwd[1],
                                   right[2] * fwd[0] - right[0] * fwd[2],
                                   right[0] * fwd[1] - right[1] * fwd[0]};
    return {eye, {right[0], right[1], right[2], up[0], up[1], up[2],
                  fwd[0], fwd[1], fwd[2]}};
}

std::array<double, 3> refWorldToView(const RefOrbit& view,
                                     const std::array<double, 3>& point) {
    const std::array<double, 3> d{point[0] - view.eye[0], point[1] - view.eye[1],
                                  point[2] - view.eye[2]};
    return refMatVec(view.rotation, d);
}

/// 透视投影（屏幕像素，y 向下为正——DEC-011/头文件契约）。
std::array<double, 2> refProject(double cx, double cy, double focal,
                                 const std::array<double, 3>& viewPoint) {
    return {cx + focal * viewPoint[0] / viewPoint[2],
            cy - focal * viewPoint[1] / viewPoint[2]};
}

/// 正交性 + 行列式（纯测试侧不变量）：姿态旋转 det = +1；视图基（x 右、y 上、
/// z 深度为正的 look-at 行基）按文档约定为左手系，det = −1。
bool orthogonalWithDet(const RefMat3& m, double expectedDet, double tolerance) {
    for (int colA = 0; colA < 3; ++colA) {
        for (int colB = colA; colB < 3; ++colB) {
            double dot = 0.0;
            for (int row = 0; row < 3; ++row) {
                dot += m[static_cast<std::size_t>(row) * 3 + colA] *
                       m[static_cast<std::size_t>(row) * 3 + colB];
            }
            const double expected = colA == colB ? 1.0 : 0.0;
            if (std::fabs(dot - expected) > tolerance) {
                return false;
            }
        }
    }
    const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) -
                       m[1] * (m[3] * m[8] - m[5] * m[6]) +
                       m[2] * (m[3] * m[7] - m[4] * m[6]);
    return std::fabs(det - expectedDet) <= tolerance;
}

bool orthonormalF(const PoseMat3& m, double tolerance) {
    RefMat3 asDouble{};
    for (int i = 0; i < 9; ++i) {
        asDouble[i] = static_cast<double>(m[i]);
    }
    return orthogonalWithDet(asDouble, 1.0, tolerance);
}

/// 视图基正交且左手（det = −1）：OrbitView.rotation 的文档约定不变量
///（x 右、y 上、z 指向场景深度为正，头文件 pose_math.hpp:65-69）。
bool viewBasisF(const PoseMat3& m, double tolerance) {
    RefMat3 asDouble{};
    for (int i = 0; i < 9; ++i) {
        asDouble[i] = static_cast<double>(m[i]);
    }
    return orthogonalWithDet(asDouble, -1.0, tolerance);
}

/// 把 float 四元数提升为 double 参考（供参考数学使用）。
RefQuat toRefQuat(const std::array<float, 4>& q) {
    return {static_cast<double>(q[0]), static_cast<double>(q[1]),
            static_cast<double>(q[2]), static_cast<double>(q[3])};
}

}  // namespace

int main() {
    // --- 1) quatNormalize：单位保持、归一化、非有限/零范数回退恒等 ---
    {
        const std::array<float, 4> unit = rin::quatNormalize({1.0f, 0.0f, 0.0f, 0.0f});
        RIN_CHECK((unit == std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f}));

        const std::array<float, 4> half = rin::quatNormalize({0.5f, 0.5f, 0.5f, 0.5f});
        RIN_CHECK(quatNear(half, {0.5, 0.5, 0.5, 0.5}, 1e-7));

        const std::array<float, 4> scaled = rin::quatNormalize({2.0f, 0.0f, 0.0f, 0.0f});
        RIN_CHECK((scaled == std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f}));

        const std::array<float, 4> general = rin::quatNormalize({0.0f, 0.0f, 3.0f, 4.0f});
        RIN_CHECK(quatNear(general, {0.0, 0.0, 0.6, 0.8}, 1e-6));

        // 任一分量非有限 → 恒等（头文件 pose_math.hpp:39-41）。
        RIN_CHECK((rin::quatNormalize({kNaN, 1.0f, 0.0f, 0.0f}) ==
                   std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f}));
        RIN_CHECK((rin::quatNormalize({1.0f, 0.0f, kInf, 0.0f}) ==
                   std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f}));
        RIN_CHECK((rin::quatNormalize({1.0f, 0.0f, 0.0f, -kInf}) ==
                   std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f}));

        // 零范数与"接近零"范数 → 恒等；阈值之上正常归一化（宽裕量边界，不锁定
        // 私有 epsilon：两侧相差 ≥6 个数量级）。
        RIN_CHECK((rin::quatNormalize({0.0f, 0.0f, 0.0f, 0.0f}) ==
                   std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f}));
        RIN_CHECK((rin::quatNormalize({1e-7f, 1e-7f, 0.0f, 0.0f}) ==
                   std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f}));
        const std::array<float, 4> above =
            rin::quatNormalize({1e-3f, 1e-3f, 0.0f, 0.0f});
        RIN_CHECK(quatNear(above, {std::sqrt(0.5), std::sqrt(0.5), 0.0, 0.0}, 1e-6));

        // 任意输入结果必为单位范数。
        const std::array<float, 4> generic =
            rin::quatNormalize({0.9f, 0.2f, 0.3f, -0.2f});
        double normSq = 0.0;
        for (const float c : generic) {
            normSq += static_cast<double>(c) * c;
        }
        RIN_CHECK(std::fabs(normSq - 1.0) <= 1e-6);
        // 与 double 参考逐分量一致。
        const double n = std::sqrt(0.81 + 0.04 + 0.09 + 0.04);
        RIN_CHECK(quatNear(generic, {0.9 / n, 0.2 / n, 0.3 / n, -0.2 / n}, 1e-6));
    }

    // --- 2) quatConjugate：逐分量取反虚部；双共轭还原 ---
    {
        const std::array<float, 4> q{0.9f, 0.2f, 0.3f, -0.2f};
        RIN_CHECK((rin::quatConjugate(q) == std::array<float, 4>{0.9f, -0.2f, -0.3f, 0.2f}));
        RIN_CHECK((rin::quatConjugate(rin::quatConjugate(q)) == q));
        RIN_CHECK((rin::quatConjugate({1.0f, 0.0f, 0.0f, 0.0f}) ==
                   std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f}));
    }

    // --- 3) quatMultiply：单位元、四元数代数、复合方向契约、与 double 参考一致 ---
    {
        const std::array<float, 4> identity{1.0f, 0.0f, 0.0f, 0.0f};
        const std::array<float, 4> q{0.9f, 0.2f, 0.3f, -0.2f};
        RIN_CHECK(rin::quatMultiply(identity, q) == q);
        RIN_CHECK(rin::quatMultiply(q, identity) == q);

        // Hamilton 代数：i⊗i = −1、j⊗k = i。
        const std::array<float, 4> i{0.0f, 1.0f, 0.0f, 0.0f};
        const std::array<float, 4> j{0.0f, 0.0f, 1.0f, 0.0f};
        const std::array<float, 4> k{0.0f, 0.0f, 0.0f, 1.0f};
        RIN_CHECK((rin::quatMultiply(i, i) == std::array<float, 4>{-1.0f, 0.0f, 0.0f, 0.0f}));
        RIN_CHECK(rin::quatMultiply(j, k) == i);

        // 复合方向（头文件 pose_math.hpp:46-48"先施 b 后施 a"）：
        // a = 绕 x +90°，b = 绕 z +90°。(a⊗b)·ẑ = a·(b·ẑ) = a·ẑ = −ŷ；
        // (a⊗b)·x̂ = a·(b·x̂) = a·x̂ = x̂。以 double 参考旋转独立验证。
        const RefQuat ax = refQuatEulerZYX(90.0, 0.0, 0.0);
        const RefQuat bz = refQuatEulerZYX(0.0, 0.0, 90.0);
        const std::array<float, 4> composed = rin::quatMultiply(
            {static_cast<float>(ax[0]), static_cast<float>(ax[1]),
             static_cast<float>(ax[2]), static_cast<float>(ax[3])},
            {static_cast<float>(bz[0]), static_cast<float>(bz[1]),
             static_cast<float>(bz[2]), static_cast<float>(bz[3])});
        const std::array<double, 3> worldZ = refRotate(toRefQuat(composed), {0.0, 0.0, 1.0});
        const std::array<double, 3> expectZ = refRotate(ax, refRotate(bz, {0.0, 0.0, 1.0}));
        const std::array<double, 3> worldX = refRotate(toRefQuat(composed), {1.0, 0.0, 0.0});
        const std::array<double, 3> expectX = refRotate(ax, refRotate(bz, {1.0, 0.0, 0.0}));
        for (int c = 0; c < 3; ++c) {
            RIN_CHECK(std::fabs(worldZ[c] - expectZ[c]) <= 1e-6);
            RIN_CHECK(std::fabs(worldX[c] - expectX[c]) <= 1e-6);
        }
        RIN_CHECK(std::fabs(worldZ[1] + 1.0) <= 1e-6);  // ẑ → −ŷ（b 绕 z 不动 ẑ，a 绕 x 转 −ŷ）
        RIN_CHECK(std::fabs(worldX[2] - 1.0) <= 1e-6);  // x̂ → ẑ（b 绕 z 转 ŷ，a 绕 x 转 ẑ）

        // 一般四元数：与 double 参考 Hamilton 乘积逐分量一致。
        const RefQuat p = toRefQuat(q);
        const RefQuat r = toRefQuat({0.5f, -0.5f, 0.5f, 0.5f});
        const RefQuat expected = refQuatMul(p, r);
        RIN_CHECK(quatNear(rin::quatMultiply(q, {0.5f, -0.5f, 0.5f, 0.5f}), expected, 1e-6));
    }

    // --- 4) orientationToMatrix：恒等、已知旋转、消毒回退、正交性、与参考旋转一致、
    //        pitch≈±90° 无奇异（DEC-011）---
    {
        const PoseMat3 identityMat = rin::orientationToMatrix({1.0f, 0.0f, 0.0f, 0.0f});
        RIN_CHECK((identityMat == PoseMat3{1, 0, 0, 0, 1, 0, 0, 0, 1}));

        // 绕 z +90°（非单位输入，先归一化）：x̂→ŷ。
        constexpr float kS = 0.70710678118654752f;  // cos45° = sin45°
        const PoseMat3 rotZ = rin::orientationToMatrix({2.0f * kS, 0.0f, 0.0f, 2.0f * kS});
        RIN_CHECK(matNear(rotZ, {0, -1, 0, 1, 0, 0, 0, 0, 1}, 1e-6));

        // 退化输入回退恒等矩阵（经 quatNormalize 消毒）。
        RIN_CHECK((rin::orientationToMatrix({0.0f, 0.0f, 0.0f, 0.0f}) ==
                   PoseMat3{1, 0, 0, 0, 1, 0, 0, 0, 1}));
        RIN_CHECK((rin::orientationToMatrix({kNaN, 1.0f, 0.0f, 0.0f}) ==
                   PoseMat3{1, 0, 0, 0, 1, 0, 0, 0, 1}));
        RIN_CHECK((rin::orientationToMatrix({1.0f, 0.0f, kInf, 0.0f}) ==
                   PoseMat3{1, 0, 0, 0, 1, 0, 0, 0, 1}));

        // 一般姿态（DEC-011 原型固定姿态 q=(0.904, 0.215, 0.306, −0.206)）：正交、
        // det=+1、矩阵作用 == Hamilton 旋转（两条独立路径交叉验证）。
        const RefQuat pose = toRefQuat(rin::quatNormalize({0.904f, 0.215f, 0.306f, -0.206f}));
        const PoseMat3 R = rin::orientationToMatrix({0.904f, 0.215f, 0.306f, -0.206f});
        RIN_CHECK(orthonormalF(R, 1e-5));
        RIN_CHECK(matNear(R, refQuatMatrix(pose), 1e-5));
        for (const std::array<double, 3>& v : {std::array<double, 3>{1.0, 0.0, 0.0},
                                               std::array<double, 3>{0.0, 1.0, 0.0},
                                               std::array<double, 3>{0.0, 0.0, 1.0},
                                               std::array<double, 3>{0.3, -0.4, 0.5}}) {
            const PoseVec3 rotated = rin::rotateVector(R, {static_cast<float>(v[0]),
                                                           static_cast<float>(v[1]),
                                                           static_cast<float>(v[2])});
            const std::array<double, 3> expected = refRotate(pose, v);
            RIN_CHECK(vec3Near(rotated, expected[0], expected[1], expected[2], 1e-5));
        }

        // pitch ≈ ±90°（相机俯视/仰视为常规姿态）：表示不退化（DEC-011"无欧拉奇异"）。
        for (const double pitchDeg : {89.9, -89.9}) {
            const RefQuat q = refQuatEulerZYX(0.0, pitchDeg, 30.0);
            const PoseMat3 m = rin::orientationToMatrix(
                {static_cast<float>(q[0]), static_cast<float>(q[1]),
                 static_cast<float>(q[2]), static_cast<float>(q[3])});
            RIN_CHECK(orthonormalF(m, 1e-5));
            const PoseVec3 rotated = rin::rotateVector(m, {1.0f, 0.0f, 0.0f});
            const std::array<double, 3> expected = refRotate(q, {1.0, 0.0, 0.0});
            RIN_CHECK(vec3Near(rotated, expected[0], expected[1], expected[2], 1e-5));
        }
    }

    // --- 5) transposeMatrix3 / multiplyMatrix3：排列、恒等元、R·Rᵀ=I、结合律 ---
    {
        const PoseMat3 m{1, 2, 3, 4, 5, 6, 7, 8, 9};
        RIN_CHECK((rin::transposeMatrix3(m) == PoseMat3{1, 4, 7, 2, 5, 8, 3, 6, 9}));

        const PoseMat3 identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
        RIN_CHECK(rin::multiplyMatrix3(identity, m) == m);
        RIN_CHECK(rin::multiplyMatrix3(m, identity) == m);

        // 一般矩阵乘法：手算已知乘积 a·aᵀ（行·列内积，a = [[1,2,3],[0,1,0],[0,0,1]]）。
        const PoseMat3 a{1, 2, 3, 0, 1, 0, 0, 0, 1};
        const PoseMat3 at = rin::transposeMatrix3(a);
        const PoseMat3 aat = rin::multiplyMatrix3(a, at);
        RIN_CHECK(matNear(aat, {14, 2, 3, 2, 1, 0, 3, 0, 1}, 1e-6));

        // 旋转矩阵的 R·Rᵀ = I（正交性仅对真旋转成立）。
        const PoseMat3 rot = rin::orientationToMatrix({0.904f, 0.215f, 0.306f, -0.206f});
        const PoseMat3 rotRotT = rin::multiplyMatrix3(rot, rin::transposeMatrix3(rot));
        RIN_CHECK(matNear(rotRotT, {1, 0, 0, 0, 1, 0, 0, 0, 1}, 1e-5));

        // 结合律：(a·rot)·c == a·(rot·c)。
        const PoseMat3 c{1, 0, 0, 0, 0, -1, 0, 1, 0};
        const PoseMat3 abC = rin::multiplyMatrix3(rin::multiplyMatrix3(a, rot), c);
        const PoseMat3 aBC = rin::multiplyMatrix3(a, rin::multiplyMatrix3(rot, c));
        RIN_CHECK(matNear(abC, {static_cast<double>(aBC[0]), static_cast<double>(aBC[1]),
                                static_cast<double>(aBC[2]), static_cast<double>(aBC[3]),
                                static_cast<double>(aBC[4]), static_cast<double>(aBC[5]),
                                static_cast<double>(aBC[6]), static_cast<double>(aBC[7]),
                                static_cast<double>(aBC[8])},
                          1e-5));
    }

    // --- 6) matrix3FromColumnMajor：Extrinsics::rotation 列主序契约
    //        （camera_types.hpp:98-100：v_target = rotation * v_source）---
    {
        RIN_CHECK((rin::matrix3FromColumnMajor({1, 0, 0, 0, 1, 0, 0, 0, 1}) ==
                   PoseMat3{1, 0, 0, 0, 1, 0, 0, 0, 1}));

        // 绕 x +90°（y→z、z→−y）以列主序存储：col0=(1,0,0) col1=(0,0,1) col2=(0,−1,0)。
        const PoseMat3 rx = rin::matrix3FromColumnMajor({1, 0, 0, 0, 0, 1, 0, -1, 0});
        RIN_CHECK(matNear(rx, {1, 0, 0, 0, 0, -1, 0, 1, 0}, 1e-7));
        const PoseVec3 ty = rin::rotateVector(rx, {0.0f, 1.0f, 0.0f});
        const PoseVec3 tz = rin::rotateVector(rx, {0.0f, 0.0f, 1.0f});
        RIN_CHECK(vec3Near(ty, 0, 0, 1, 1e-7));   // ŷ → ẑ
        RIN_CHECK(vec3Near(tz, 0, -1, 0, 1e-7));  // ẑ → −ŷ
    }

    // --- 7) rotateVector：恒等、已知映射、线性 ---
    {
        const PoseVec3 v{0.3f, -0.4f, 0.5f};
        const PoseMat3 identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
        // 容差 ≥ float→double 表示间隙（|0.3f − 0.3| ≈ 1.2e-8）。
        RIN_CHECK(vec3Near(rin::rotateVector(identity, v), 0.3, -0.4, 0.5, 1e-7));

        const PoseMat3 rotZ{0, -1, 0, 1, 0, 0, 0, 0, 1};
        RIN_CHECK(vec3Near(rin::rotateVector(rotZ, {1, 0, 0}), 0, 1, 0, 1e-7));  // x̂→ŷ

        const PoseMat3 r{0.5f, -1.25f, 2.0f, 0.25f, 0.75f, -0.5f, 1.5f, 0.0f, 1.0f};
        const PoseVec3 sum = rin::rotateVector(r, {0.8f, -0.4f, 0.5f});
        const PoseVec3 partA = rin::rotateVector(r, {0.3f, -0.4f, 0.5f});
        const PoseVec3 partB = rin::rotateVector(r, {0.5f, 0.0f, 0.0f});
        RIN_CHECK(vec3Near(sum, partA.x + partB.x, partA.y + partB.y, partA.z + partB.z,
                           1e-6));
    }

    // --- 8) makeOrbitView / worldToView：orbit look-at 语义（DEC-011 固定世界系）---
    {
        // 方位 0、仰角 0：eye 在世界 +x，前向 −x̂，世界上方 = 视图上方，世界 +y = 视图右。
        const rin::OrbitView view = rin::makeOrbitView(0.0f, 0.0f, 2.0f);
        RIN_CHECK(vec3Near(view.eye, 2, 0, 0, 0.0));
        RIN_CHECK((view.rotation == PoseMat3{0, 1, 0, 0, 0, 1, -1, 0, 0}));
        RIN_CHECK(vec3Near(rin::worldToView(view, {0, 0, 0}), 0, 0, 2, 0.0));  // 深度 = distance
        RIN_CHECK(vec3Near(rin::worldToView(view, {0, 0, 1}), 0, 1, 2, 0.0));  // 世界上方 = 视图上方
        RIN_CHECK(vec3Near(rin::worldToView(view, {0, 1, 0}), 1, 0, 2, 0.0));  // 世界 +y = 视图右
        RIN_CHECK(vec3Near(rin::worldToView(view, {2, 0, 0}), 0, 0, 0, 0.0));  // 注视点即 eye

        // viewer 常用视角（DEC-011 原型构图）：视图基正交且左手（det=−1，x 右/
        // y 上/z 深度为正的文档约定）、fwd 行 = −eye/|eye|、eye 模长 = distance、
        // 世界原点深度 = distance。
        const rin::OrbitView orbit = rin::makeOrbitView(0.62f, 0.50f, 3.8f);
        const double eyeNorm = std::sqrt(static_cast<double>(orbit.eye.x) * orbit.eye.x +
                                         static_cast<double>(orbit.eye.y) * orbit.eye.y +
                                         static_cast<double>(orbit.eye.z) * orbit.eye.z);
        RIN_CHECK(std::fabs(eyeNorm - 3.8) <= 1e-5);
        RIN_CHECK(viewBasisF(orbit.rotation, 1e-5));
        const PoseVec3 originView = rin::worldToView(orbit, {0, 0, 0});
        RIN_CHECK(vec3Near(originView, 0, 0, 3.8, 1e-4));
        const double fwdRow[3] = {orbit.rotation[6], orbit.rotation[7], orbit.rotation[8]};
        RIN_CHECK(nearF(fwdRow[0], -static_cast<double>(orbit.eye.x) / eyeNorm, 1e-5));
        RIN_CHECK(nearF(fwdRow[1], -static_cast<double>(orbit.eye.y) / eyeNorm, 1e-5));
        RIN_CHECK(nearF(fwdRow[2], -static_cast<double>(orbit.eye.z) / eyeNorm, 1e-5));
        // right 行（row 0）恒在世界水平面内（up = +Z look-at 的构造不变量）。
        RIN_CHECK(nearF(orbit.rotation[2], 0.0, 1e-7));
        const double rightNorm = std::sqrt(static_cast<double>(orbit.rotation[0]) *
                                               orbit.rotation[0] +
                                           static_cast<double>(orbit.rotation[1]) *
                                               orbit.rotation[1]);
        RIN_CHECK(std::fabs(rightNorm - 1.0) <= 1e-6);

        // 仰角 ±90° 截断（公开契约 ±89°；up=+Z look-at 在 ±90° right 零向量退化）：
        // 不达极点（水平分量非零）、right 仍单位、视图基仍正交左手。
        for (const double extreme : {kPi / 2.0, -kPi / 2.0, 1.60, -1.60}) {
            const rin::OrbitView clamped = rin::makeOrbitView(0.3f, extreme, 2.0f);
            const double horizontalSq = static_cast<double>(clamped.eye.x) * clamped.eye.x +
                                        static_cast<double>(clamped.eye.y) * clamped.eye.y;
            RIN_CHECK(horizontalSq > 1e-4);  // 89° 截断后 cos²(89°) ≈ 3e-4 上界之内
            RIN_CHECK(viewBasisF(clamped.rotation, 1e-5));
            const double rn = std::sqrt(static_cast<double>(clamped.rotation[0]) *
                                            clamped.rotation[0] +
                                        static_cast<double>(clamped.rotation[1]) *
                                            clamped.rotation[1]);
            RIN_CHECK(std::fabs(rn - 1.0) <= 1e-6);
        }

        // 非有限输入消毒：azimuth/elevation 非有限按 0；distance 非有限/<=0 按 1。
        const rin::OrbitView nanAz = rin::makeOrbitView(kNaN, 0.0f, 2.0f);
        RIN_CHECK(vec3Near(nanAz.eye, 2, 0, 0, 1e-6));
        const rin::OrbitView nanEl = rin::makeOrbitView(0.0f, kNaN, 2.0f);
        RIN_CHECK(vec3Near(nanEl.eye, 2, 0, 0, 1e-6));
        for (const float badDistance : {kNaN, kInf, -kInf, 0.0f, -5.0f}) {
            const rin::OrbitView unit = rin::makeOrbitView(0.0f, 0.0f, badDistance);
            const double norm = std::sqrt(static_cast<double>(unit.eye.x) * unit.eye.x +
                                          static_cast<double>(unit.eye.y) * unit.eye.y +
                                          static_cast<double>(unit.eye.z) * unit.eye.z);
            RIN_CHECK(std::fabs(norm - 1.0) <= 1e-6);
        }
        // 非有限仰角按 0 后仍受截断语义约束（有限大仰角被钳制、不产生 NaN）。
        const rin::OrbitView hugeEl = rin::makeOrbitView(0.0f, 1e30f, 2.0f);
        RIN_CHECK(std::isfinite(hugeEl.eye.z) && std::fabs(hugeEl.eye.z) < 2.0f);
    }

    // --- 9) projectToScreen：屏幕约定（y 向下）、<=nearZ 剔除边界、失败不写出参 ---
    {
        const PoseProjector projector{{50.0f, 50.0f}, 100.0f, 0.05f};
        PoseVec2 out{123.0f, 456.0f};  // 哨兵：失败路径必须保持不变

        RIN_CHECK(rin::projectToScreen(projector, {0.0f, 0.0f, 2.0f}, out));
        RIN_CHECK(vec2Near(out, 50, 50, 0.0));
        RIN_CHECK(rin::projectToScreen(projector, {0.0f, 1.0f, 2.0f}, out));  // 视图上 → 屏幕 y 更小
        RIN_CHECK(vec2Near(out, 50, 0, 0.0));
        RIN_CHECK(rin::projectToScreen(projector, {1.0f, 0.0f, 2.0f}, out));  // 视图右 → 屏幕 x 更大
        RIN_CHECK(vec2Near(out, 100, 50, 0.0));
        // 契约仅近平面/非有限拒绝，无视口裁剪：屏幕外仍成功。
        RIN_CHECK(rin::projectToScreen(projector, {-1.0f, 0.0f, 1.0f}, out));
        RIN_CHECK(vec2Near(out, -50, 50, 0.0));

        // 近平面边界：z == nearZ 即剔除（<= 语义），严格在前成功。
        out = {123.0f, 456.0f};
        RIN_CHECK(!rin::projectToScreen(projector, {0.0f, 0.0f, 0.05f}, out));
        RIN_CHECK(vec2Near(out, 123, 456, 0.0));
        RIN_CHECK(!rin::projectToScreen(projector, {0.0f, 0.0f, 0.049f}, out));
        RIN_CHECK(vec2Near(out, 123, 456, 0.0));
        RIN_CHECK(rin::projectToScreen(projector, {0.0f, 0.0f, 0.050001f}, out));

        // 非有限视图点拒绝且不写出参。
        out = {123.0f, 456.0f};
        RIN_CHECK(!rin::projectToScreen(projector, {0.0f, 0.0f, kNaN}, out));
        RIN_CHECK(vec2Near(out, 123, 456, 0.0));
        RIN_CHECK(!rin::projectToScreen(projector, {0.0f, 0.0f, kInf}, out));
        RIN_CHECK(vec2Near(out, 123, 456, 0.0));
        RIN_CHECK(!rin::projectToScreen(projector, {kNaN, 0.0f, 1.0f}, out));
        RIN_CHECK(vec2Near(out, 123, 456, 0.0));

        // 投影结果溢出为非有限 → 拒绝（头文件 pose_math.hpp:86-91）。
        const PoseProjector huge{{50.0f, 50.0f}, 1e30f, 0.05f};
        out = {123.0f, 456.0f};
        RIN_CHECK(!rin::projectToScreen(huge, {1e30f, 0.0f, 0.5f}, out));
        RIN_CHECK(vec2Near(out, 123, 456, 0.0));
    }

    // --- 10) segmentToQuad：等宽四边形几何与点序、退化剔除、失败不写出参 ---
    {
        const PoseProjector projector{{0.0f, 0.0f}, 1.0f, 0.05f};
        const std::array<PoseVec2, 4> sentinel{
            PoseVec2{91.0f, 92.0f}, PoseVec2{93.0f, 94.0f}, PoseVec2{95.0f, 96.0f},
            PoseVec2{97.0f, 98.0f}};
        std::array<PoseVec2, 4> quad = sentinel;

        // 水平段（focal=1、center=0：屏幕坐标 == 视图坐标）：点序 a+n, b+n, b−n, a−n。
        RIN_CHECK(rin::segmentToQuad(projector, {0.0f, 0.0f, 1.0f}, {2.0f, 0.0f, 1.0f},
                                     1.0f, quad));
        RIN_CHECK(vec2Near(quad[0], 0, 0.5, 1e-6));
        RIN_CHECK(vec2Near(quad[1], 2, 0.5, 1e-6));
        RIN_CHECK(vec2Near(quad[2], 2, -0.5, 1e-6));
        RIN_CHECK(vec2Near(quad[3], 0, -0.5, 1e-6));

        // 垂直段：视图 +y 投影后朝屏幕上方（y 向下翻转），屏幕段 (0,0)→(0,−2)，
        // 法向翻转到 x。
        quad = sentinel;
        RIN_CHECK(rin::segmentToQuad(projector, {0.0f, 0.0f, 1.0f}, {0.0f, 2.0f, 1.0f},
                                     1.0f, quad));
        RIN_CHECK(vec2Near(quad[0], 0.5, 0, 1e-6));
        RIN_CHECK(vec2Near(quad[1], 0.5, -2, 1e-6));
        RIN_CHECK(vec2Near(quad[2], -0.5, -2, 1e-6));
        RIN_CHECK(vec2Near(quad[3], -0.5, 0, 1e-6));

        // 斜段（屏幕 3-4-5，屏幕段 (0,0)→(3,−4)）：n ⊥ (sb−sa)、|n| = width/2、
        // 平行四边形不变量。
        quad = sentinel;
        RIN_CHECK(rin::segmentToQuad(projector, {0.0f, 0.0f, 1.0f}, {3.0f, 4.0f, 1.0f},
                                     2.0f, quad));
        const double nx = 4.0 / 5.0 * 1.0;   // n = (−dy, dx)/len·width/2，屏幕 dy = −4
        const double ny = 3.0 / 5.0 * 1.0;
        RIN_CHECK(vec2Near(quad[0], nx, ny, 1e-6));
        RIN_CHECK(vec2Near(quad[1], 3 + nx, -4 + ny, 1e-6));
        RIN_CHECK(vec2Near(quad[2], 3 - nx, -4 - ny, 1e-6));
        RIN_CHECK(vec2Near(quad[3], -nx, -ny, 1e-6));
        RIN_CHECK(std::fabs(nx * 3.0 + ny * -4.0) <= 1e-9);      // 垂直（屏幕空间）
        RIN_CHECK(std::fabs(std::hypot(nx, ny) - 1.0) <= 1e-9);  // 半宽 = width/2

        // 宽度按 widthPx 线性缩放。
        quad = sentinel;
        RIN_CHECK(rin::segmentToQuad(projector, {0.0f, 0.0f, 1.0f}, {2.0f, 0.0f, 1.0f},
                                     10.0f, quad));
        RIN_CHECK(vec2Near(quad[0], 0, 5, 1e-6));
        RIN_CHECK(vec2Near(quad[2], 2, -5, 1e-6));

        // 退化：零长度与近平零屏幕长度（kSegmentMinLengthPx 量级以下）。
        quad = sentinel;
        RIN_CHECK(!rin::segmentToQuad(projector, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f},
                                      1.0f, quad));
        RIN_CHECK(quadExact(quad, sentinel));
        RIN_CHECK(!rin::segmentToQuad(projector, {0.0f, 0.0f, 1.0f}, {1e-6f, 0.0f, 1.0f},
                                      1.0f, quad));
        RIN_CHECK(quadExact(quad, sentinel));

        // widthPx 非有限/<=0 拒绝。
        for (const float badWidth : {0.0f, -1.0f, kNaN, kInf}) {
            quad = sentinel;
            RIN_CHECK(!rin::segmentToQuad(projector, {0.0f, 0.0f, 1.0f}, {2.0f, 0.0f, 1.0f},
                                          badWidth, quad));
            RIN_CHECK(quadExact(quad, sentinel));
        }

        // 任一端点近平面后 / 恰在近平面 / 非有限 → 整段剔除且不写出参
        //（DEC-011/头文件 pose_math.hpp:93-101"端点剔除即够用"）。
        quad = sentinel;
        RIN_CHECK(!rin::segmentToQuad(projector, {0.0f, 0.0f, 0.01f}, {2.0f, 0.0f, 1.0f},
                                      1.0f, quad));
        RIN_CHECK(quadExact(quad, sentinel));
        RIN_CHECK(!rin::segmentToQuad(projector, {0.0f, 0.0f, 1.0f}, {2.0f, 0.0f, 0.05f},
                                      1.0f, quad));
        RIN_CHECK(quadExact(quad, sentinel));
        RIN_CHECK(!rin::segmentToQuad(projector, {0.0f, 0.0f, 1.0f}, {kNaN, 0.0f, 1.0f},
                                      1.0f, quad));
        RIN_CHECK(quadExact(quad, sentinel));
    }

    // --- 11) golden 端到端：已知位姿 + 固定观察相机 → 期望屏幕点集（DEC-011
    //          验证方式 1，double 参考独立推导，容差 0.05 px）---
    double goldenMaxDevPx = 0.0;
    {
        const rin::OrbitView view = rin::makeOrbitView(0.62f, 0.50f, 3.8f);
        const RefOrbit refView = refOrbitView(0.62, 0.50, 3.8);
        const PoseProjector projector{{320.0f, 180.0f}, 200.0f, 0.05f};
        constexpr double kCx = 320.0, kCy = 180.0, kFocal = 200.0;

        // 位姿 A：ZYX(30°, 20°, 40°)；位姿 B = A 绕世界 Z 再转 90°（姿态驱动对照）。
        const RefQuat poseA = refQuatEulerZYX(30.0, 20.0, 40.0);
        const RefQuat yaw90 = refQuatEulerZYX(0.0, 0.0, 90.0);
        const RefQuat poseB = refQuatMul(yaw90, poseA);
        const std::array<RefQuat, 2> poses{poseA, poseB};

        // 场景点集：相机系（视锥顶点/像平面角/相机轴端，仿 pose_view 构图）与
        // 世界固定参考（网格角/世界轴端）。
        constexpr float kHalf = 0.85f * 0.42f;
        const std::array<PoseVec3, 8> camPoints{
            PoseVec3{0, 0, 0},
            PoseVec3{-kHalf, -kHalf, 0.85f}, PoseVec3{kHalf, -kHalf, 0.85f},
            PoseVec3{kHalf, kHalf, 0.85f}, PoseVec3{-kHalf, kHalf, 0.85f},
            PoseVec3{0.5f, 0, 0}, PoseVec3{0, 0.5f, 0}, PoseVec3{0, 0, 0.5f}};
        const std::array<PoseVec3, 4> worldPoints{
            PoseVec3{-1, -1, 0}, PoseVec3{1, 1, 0},
            PoseVec3{1.25f, 0, 0}, PoseVec3{0, 0, 1.25f}};

        std::array<std::array<double, 2>, 8> camScreenA{};
        std::array<std::array<double, 2>, 8> camScreenB{};
        for (int p = 0; p < 2; ++p) {
            const PoseMat3 R = rin::orientationToMatrix(
                {static_cast<float>(poses[p][0]), static_cast<float>(poses[p][1]),
                 static_cast<float>(poses[p][2]), static_cast<float>(poses[p][3])});
            for (int i = 0; i < static_cast<int>(camPoints.size()); ++i) {
                const PoseVec3 world = rin::rotateVector(R, camPoints[i]);
                const PoseVec3 viewPoint = rin::worldToView(view, world);
                PoseVec2 screen{0.0f, 0.0f};
                RIN_CHECK(rin::projectToScreen(projector, viewPoint, screen));
                const std::array<double, 3> refWorld = refRotate(poses[p],
                                                                 {camPoints[i].x, camPoints[i].y,
                                                                  camPoints[i].z});
                const std::array<double, 3> refPoint = refWorldToView(refView, refWorld);
                const std::array<double, 2> refScreen = refProject(kCx, kCy, kFocal, refPoint);
                const double dev = std::max(
                    std::fabs(static_cast<double>(screen.x) - refScreen[0]),
                    std::fabs(static_cast<double>(screen.y) - refScreen[1]));
                goldenMaxDevPx = std::max(goldenMaxDevPx, dev);
                RIN_CHECK(dev <= 0.05);
                (p == 0 ? camScreenA : camScreenB)[i] = {screen.x, screen.y};
            }
        }

        // 世界固定系：网格角与轴端投影与位姿无关（固定世界坐标系契约）。
        for (const PoseVec3& worldFixed : worldPoints) {
            PoseVec2 screenA{0.0f, 0.0f};
            PoseVec2 screenB{0.0f, 0.0f};
            RIN_CHECK(rin::projectToScreen(
                projector, rin::worldToView(view, worldFixed), screenA));
            RIN_CHECK(rin::projectToScreen(
                projector, rin::worldToView(view, worldFixed), screenB));
            RIN_CHECK(std::fabs(static_cast<double>(screenA.x) - screenB.x) <= 1e-5);
            RIN_CHECK(std::fabs(static_cast<double>(screenA.y) - screenB.y) <= 1e-5);
        }

        // 姿态驱动旋转：相机光轴端点（点 7）投影随 90° yaw 显著移动。
        const double axisShift = std::hypot(
            static_cast<double>(camScreenA[7][0]) - camScreenB[7][0],
            static_cast<double>(camScreenA[7][1]) - camScreenB[7][1]);
        RIN_CHECK(axisShift > 5.0);

        // 可读场景方向：世界上方轴端投影在世界原点之上（屏幕 y 更小，y 向下为正）。
        const PoseVec3 zTipView = rin::worldToView(view, {0, 0, 1.25f});
        const PoseVec3 originViewPoint = rin::worldToView(view, {0, 0, 0});
        PoseVec2 zTipScreen{0.0f, 0.0f};
        PoseVec2 originScreen{0.0f, 0.0f};
        RIN_CHECK(rin::projectToScreen(projector, zTipView, zTipScreen));
        RIN_CHECK(rin::projectToScreen(projector, originViewPoint, originScreen));
        RIN_CHECK(zTipScreen.y < originScreen.y - 1.0f);

        std::printf(
            "golden: max |core - reference| = %.3e px (<= 0.05), camera axis shift "
            "over 90 deg yaw = %.1f px\n",
            goldenMaxDevPx, axisShift);
    }

    std::printf("pose-math summary: golden max deviation %.3e px\n", goldenMaxDevPx);
    return rin_test::exitStatus();
}
