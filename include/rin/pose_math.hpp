#pragma once

#include <array>

namespace rin {

/// 3D 位姿视图投影数学（M3-06，[DEC-011]）：固定世界坐标系下 CPU 投影的纯逻辑，
/// 全部为无第三方类型的纯函数（RULE-01；沿 DEC-003 "呈现类纯逻辑落 Core"先例），
/// 公式与 DEC-011 最小原型一致（四元数→旋转矩阵、orbit look-at 世界→视图、透视
/// 投影、线段→屏幕空间等宽四边形），支撑投影数学单测。
///
/// 坐标约定（与 M3-03 锁定契约一致）：
/// - 世界系 Z 轴向上（重力沿 −Z）；2D 输出为屏幕坐标（y 向下为正，与 UI 一致）。
/// - 姿态四元数标量在前 (w, x, y, z)，表示 传感器系 → 世界系
///   （`ImuSnapshot::orientation` 语义，DEC-010）。
/// - 相机系（彩色相机，librealsense 惯例）光轴 +Z 前向、+Y 向下、+X 向右。
///
/// 热路径约束：全部为无堆分配的值语义纯函数（EUI 渲染线程 compose 期调用，
/// RULE-05 允许的有界工作；位姿融合不在此处，见 EXEC-06）。
///
/// [DEC-011]: ../../docs/decisions/DEC-011-pose-view-rendering.md

/// 2D 点（屏幕空间，像素；y 向下为正）。
struct PoseVec2 {
    float x = 0.0f;
    float y = 0.0f;
};

/// 3D 点（世界系或相机系）。
struct PoseVec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

/// 3x3 旋转/变换矩阵，行主序（m[row * 3 + col]）。
using PoseMat3 = std::array<float, 9>;

/// 四元数归一化（标量在前 w,x,y,z）。任一分量非有限或范数接近零时返回恒等
/// {1,0,0,0}（与 DEC-010 融合器初值消毒约定一致，不传播退化输入）。
[[nodiscard]] std::array<float, 4> quatNormalize(const std::array<float, 4>& q) noexcept;

/// 四元数共轭（conj(q) 表示逆旋转；逐分量取反虚部，不做有效性消毒）。
[[nodiscard]] std::array<float, 4> quatConjugate(const std::array<float, 4>& q) noexcept;

/// 四元数 Hamilton 乘积 a ⊗ b（标量在前；先施 b 后施 a 的旋转复合）。
[[nodiscard]] std::array<float, 4> quatMultiply(const std::array<float, 4>& a,
                                                const std::array<float, 4>& b) noexcept;

/// 姿态四元数 → 行主序旋转矩阵（world = R * sensor；输入先经 quatNormalize）。
[[nodiscard]] PoseMat3 orientationToMatrix(const std::array<float, 4>& orientation) noexcept;

/// 行主序矩阵转置（旋转矩阵的转置即逆旋转）。
[[nodiscard]] PoseMat3 transposeMatrix3(const PoseMat3& m) noexcept;

/// 行主序矩阵乘法（结果 = a * b；组合先施 b 后施 a）。
[[nodiscard]] PoseMat3 multiplyMatrix3(const PoseMat3& a, const PoseMat3& b) noexcept;

/// 列主序 3x3 存储（`Extrinsics::rotation` 的契约布局）→ 行主序矩阵。
[[nodiscard]] PoseMat3 matrix3FromColumnMajor(const std::array<float, 9>& m) noexcept;

/// 行主序矩阵作用于 3D 点（不含平移的旋转/线性变换）。
[[nodiscard]] PoseVec3 rotateVector(const PoseMat3& m, const PoseVec3& v) noexcept;

/// 固定世界坐标系的观察相机：orbit look-at（注视原点，up = 世界 +Z）。
struct OrbitView {
    PoseVec3 eye{};      /// 观察点（世界系）
    PoseMat3 rotation{}; /// 行主序 world → view（view 的 z 轴指向场景深度为正）
};

/// 构造 orbit 观察相机。elevation 绝对值按 ±89° 截断（up=+Z 的 look-at 在
/// ±90° 退化）；distance 非有限或 <= 0 时按 1 处理；azimuth 非有限按 0 处理。
[[nodiscard]] OrbitView makeOrbitView(float azimuthRad, float elevationRad,
                                      float distance) noexcept;

/// 世界系点 → 视图系点。
[[nodiscard]] PoseVec3 worldToView(const OrbitView& view, const PoseVec3& point) noexcept;

/// 透视投影参数（屏幕像素空间）。
struct PoseProjector {
    PoseVec2 center{};  /// 屏幕中心（投影原点）
    float focal = 0.0f; /// 像素焦距
    float nearZ = 0.05f; /// 近平面（view z <= nearZ 的点不可投影）
};

/// 视图系点 → 屏幕点（y 向下为正）。z <= nearZ、z 非有限或投影结果非有限时
/// 返回 false 且不写 out（近平面后端点剔除，不做裁剪插值——位姿视图场景几何
/// 小型且有界，端点剔除即够用，DEC-011 原型同语义）。
[[nodiscard]] bool projectToScreen(const PoseProjector& projector,
                                   const PoseVec3& viewPoint,
                                   PoseVec2& out) noexcept;

/// 视图系线段 → 屏幕空间等宽四边形（polygon 4 点，序：a+n, b+n, b−n, a−n，
/// n 为左法向半宽偏移）。任一端点不可投影（近平面后）、屏幕长度退化或
/// widthPx 非有限/<= 0 时返回 false 且不写 out。widthPx 可按深度缩放
/// （DEC-011 深度提示预留，调用方自行决定）。
[[nodiscard]] bool segmentToQuad(const PoseProjector& projector,
                                 const PoseVec3& aView,
                                 const PoseVec3& bView,
                                 float widthPx,
                                 std::array<PoseVec2, 4>& out) noexcept;

}  // namespace rin
