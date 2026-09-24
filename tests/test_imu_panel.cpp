// M3-07 IMU 状态面板纯函数测试（独立验证；apps/viewer/imu_panel.hpp）。
//
// 覆盖（实现自述契约，imu_panel.hpp:35-81）：
// - attitudeDegreesFromOrientation（四元数 → ZYX 欧拉角，度）：
//   * 独立 double 参考路径：四元数直接分解（不经实现所用的 orientationToMatrix
//     矩阵下标路径），roll = atan2(2(wx+yz), 1−2(x²+y²))、
//     pitch = asin(clamp(2(wy−zx)))、yaw = atan2(2(wz+xy), 1−2(y²+z²))；
//   * 恒等 → (0,0,0)；单轴 roll/pitch/yaw；DEC-011 golden ZYX 30°/20°/40°；
//   * 固定角度网格往返（前向参考 qz⊗qy⊗qx → 分解回原角度）；
//   * yaw 折叠契约 yaw ∈ (−180°, 180°]：200° → −160°、±180° → +180°；
//   * 万向锁附近（pitch ±89.9°）有限且 pitch 正确；恰 ±90° 时输出有限
//     （读数退化仅供参考、无 NaN，3D 视图直接消费四元数不受影响）；
//   * 输入消毒（quatNormalize 契约）：NaN/Inf/零范数 → 恒等 → 全零读数；
//     非单位输入（×3.7）与单位输入读数一致。
// - formatSourceRate / formatAttitudeDegrees / formatOrientationQuaternion：
//   精确文本（含正负号、ASCII '-'、UTF-8 "°"/"·" 分隔、%.1f/%.3f 舍入、
//   uint64 大样本数）。
// - 面板空态门控输入（imu_panel.hpp:114 hasSnapshot = available && valid()）：
//   空态、invalid 快照（NaN 姿态）、排空后（clear 留 stale 快照但 available=false，
//   M3-07 排空语义在面板输入侧可观察）三种组合的门控取值。
//
// 范围与限制（如实说明）：composeImuPanelCard（卡片合成、行集合与 "n/a" 文案、
// 快照序号角标）需活动 EUI-NEO 运行时，headless 不可单测，真机视觉验收归 M3-08；
// 本测试覆盖其全部数值输入（欧拉读数与三行文本组装）与门控输入。
//
// DOD-02 适用性说明：被测函数全部为无堆分配值语义纯函数（RULE-05/07 允许的
// 渲染线程有界工作），单线程、无 Executor 任务/队列/取消/超时/shutdown 语义，
// 并发矩阵不适用；跨上下文姿态通路与关闭排空回归由 shutdown_drain 测试覆盖。
#include "test_util.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "imu_panel.hpp"

#include "rin/pose_math.hpp"

namespace {

using viewer::ImuAttitudeDegrees;
using viewer::attitudeDegreesFromOrientation;
using viewer::formatAttitudeDegrees;
using viewer::formatOrientationQuaternion;
using viewer::formatSourceRate;
using rin::quatNormalize;

constexpr double kRadToDeg = 57.2957795130823229;

// --- 独立 double 参考（不使用被测实现的矩阵路径）---

using RefQuat = std::array<double, 4>;  // 标量在前 w,x,y,z

RefQuat refMul(const RefQuat& a, const RefQuat& b) {
    return {a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
            a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
            a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
            a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]};
}

RefQuat refAxisAngle(double angleRad, double x, double y, double z) {
    const double half = angleRad * 0.5;
    return {std::cos(half), std::sin(half) * x, std::sin(half) * y, std::sin(half) * z};
}

/// ZYX 角度（度）→ 四元数：q = qz(yaw) ⊗ qy(pitch) ⊗ qx(roll)（world←sensor，
/// 与 imu_panel.hpp 声明的分解序互逆）。
RefQuat refFromZyxDegrees(double rollDeg, double pitchDeg, double yawDeg) {
    const RefQuat qx = refAxisAngle(rollDeg / kRadToDeg, 1.0, 0.0, 0.0);
    const RefQuat qy = refAxisAngle(pitchDeg / kRadToDeg, 0.0, 1.0, 0.0);
    const RefQuat qz = refAxisAngle(yawDeg / kRadToDeg, 0.0, 0.0, 1.0);
    return refMul(qz, refMul(qy, qx));
}

/// 四元数直接 ZYX 分解（double 参考；独立于 orientationToMatrix 下标提取）。
ImuAttitudeDegrees refEulerDegrees(const RefQuat& q) {
    const double w = q[0];
    const double x = q[1];
    const double y = q[2];
    const double z = q[3];
    ImuAttitudeDegrees out;
    out.roll = static_cast<float>(std::atan2(2.0 * (w * x + y * z), 1.0 - 2.0 * (x * x + y * y)) *
                                  kRadToDeg);
    const double sinPitch = std::clamp(2.0 * (w * y - z * x), -1.0, 1.0);
    out.pitch = static_cast<float>(std::asin(sinPitch) * kRadToDeg);
    out.yaw = static_cast<float>(std::atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (y * y + z * z)) *
                                 kRadToDeg);
    return out;
}

std::array<float, 4> toFloatQuat(const RefQuat& q) {
    return {static_cast<float>(q[0]), static_cast<float>(q[1]), static_cast<float>(q[2]),
            static_cast<float>(q[3])};
}

bool nearDeg(float actual, double expected, double toleranceDeg) {
    return std::fabs(static_cast<double>(actual) - expected) <= toleranceDeg;
}

bool finiteAttitude(const ImuAttitudeDegrees& a) {
    return std::isfinite(a.roll) && std::isfinite(a.pitch) && std::isfinite(a.yaw);
}

bool attitudeEquals(const ImuAttitudeDegrees& a, const ImuAttitudeDegrees& b, double tolDeg) {
    return nearDeg(a.roll, b.roll, tolDeg) && nearDeg(a.pitch, b.pitch, tolDeg) &&
           nearDeg(a.yaw, b.yaw, tolDeg);
}

rin::ImuSnapshot makeSnapshot(const std::array<float, 4>& orientation) {
    rin::ImuSnapshot snapshot;
    snapshot.orientation = orientation;
    snapshot.sequence = 42;
    snapshot.sources.gyroHz = 399.5f;
    snapshot.sources.accelHz = 249.5f;
    snapshot.sources.gyroSamples = 1000;
    snapshot.sources.accelSamples = 800;
    return snapshot;
}

}  // namespace

int main() {
    // --- 1) 恒等 → (0,0,0) ---
    {
        const ImuAttitudeDegrees a =
            attitudeDegreesFromOrientation({1.0f, 0.0f, 0.0f, 0.0f});
        RIN_CHECK(nearDeg(a.roll, 0.0, 1e-6));
        RIN_CHECK(nearDeg(a.pitch, 0.0, 1e-6));
        RIN_CHECK(nearDeg(a.yaw, 0.0, 1e-6));
    }

    // --- 2) 单轴旋转：读数落在对应轴、其余为零 ---
    {
        const ImuAttitudeDegrees rollOnly = attitudeDegreesFromOrientation(
            toFloatQuat(refFromZyxDegrees(30.0, 0.0, 0.0)));
        RIN_CHECK(nearDeg(rollOnly.roll, 30.0, 5e-3));
        RIN_CHECK(nearDeg(rollOnly.pitch, 0.0, 5e-3));
        RIN_CHECK(nearDeg(rollOnly.yaw, 0.0, 5e-3));

        const ImuAttitudeDegrees pitchOnly = attitudeDegreesFromOrientation(
            toFloatQuat(refFromZyxDegrees(0.0, -20.0, 0.0)));
        RIN_CHECK(nearDeg(pitchOnly.pitch, -20.0, 5e-3));
        RIN_CHECK(nearDeg(pitchOnly.roll, 0.0, 5e-3));
        RIN_CHECK(nearDeg(pitchOnly.yaw, 0.0, 5e-3));

        const ImuAttitudeDegrees yawOnly = attitudeDegreesFromOrientation(
            toFloatQuat(refFromZyxDegrees(0.0, 0.0, 40.0)));
        RIN_CHECK(nearDeg(yawOnly.yaw, 40.0, 5e-3));
        RIN_CHECK(nearDeg(yawOnly.roll, 0.0, 5e-3));
        RIN_CHECK(nearDeg(yawOnly.pitch, 0.0, 5e-3));
    }

    // --- 3) DEC-011 golden ZYX 30/20/40 复合姿态：与 double 参考逐角一致 ---
    {
        const std::array<float, 4> golden =
            toFloatQuat(refFromZyxDegrees(30.0, 20.0, 40.0));
        const ImuAttitudeDegrees a = attitudeDegreesFromOrientation(golden);
        RIN_CHECK(nearDeg(a.roll, 30.0, 5e-3));
        RIN_CHECK(nearDeg(a.pitch, 20.0, 5e-3));
        RIN_CHECK(nearDeg(a.yaw, 40.0, 5e-3));
        const ImuAttitudeDegrees reference =
            refEulerDegrees({static_cast<double>(golden[0]), static_cast<double>(golden[1]),
                             static_cast<double>(golden[2]), static_cast<double>(golden[3])});
        RIN_CHECK(attitudeEquals(a, reference, 5e-3));
    }

    // --- 4) 角度网格往返：前向参考（double）→ float 四元数 → 分解回原角度 ---
    {
        struct Case {
            double roll;
            double pitch;
            double yaw;
        };
        constexpr Case kCases[] = {
            {0.0, 0.0, 0.0},      {15.5, -10.25, 5.0},   {-45.0, 30.0, 120.0},
            {10.0, -35.0, -170.0}, {80.0, 60.0, -95.0}, {-12.0, 88.0, 0.5},
            {0.0, -88.5, 179.0},  {33.0, 0.0, -160.0},  {-89.0, -1.0, 1.0},
        };
        for (const Case& c : kCases) {
            const RefQuat q = refFromZyxDegrees(c.roll, c.pitch, c.yaw);
            const ImuAttitudeDegrees a = attitudeDegreesFromOrientation(toFloatQuat(q));
            RIN_CHECK_MSG(nearDeg(a.roll, c.roll, 5e-3) && nearDeg(a.pitch, c.pitch, 5e-3) &&
                              nearDeg(a.yaw, c.yaw, 5e-3),
                          ("roundtrip " + std::to_string(c.roll) + "/" +
                           std::to_string(c.pitch) + "/" + std::to_string(c.yaw))
                              .c_str());
        }
    }

    // --- 5) yaw 折叠契约（imu_panel.hpp:34 声明 yaw ∈ (−180°, 180°]）---
    {
        // yaw 200° 折叠到 −160°（实测精确）。
        const ImuAttitudeDegrees wrapped =
            attitudeDegreesFromOrientation(toFloatQuat(refFromZyxDegrees(0.0, 0.0, 200.0)));
        RIN_CHECK(nearDeg(wrapped.yaw, -160.0, 5e-3));
        // yaw ±180° 边界：两者是同一物理姿态。+180 输出 +180（区间闭端成立）；
        // 恰 −180 的四元数经 atan2 实测输出 −180.0（区间开端的 measure-zero 端点，
        // atan2(−ε, −1) = −π；任何连续融合流都不会精确产生该输入）。此处锁定物理
        // 不变量 |yaw| == 180；区间措辞的端点细化留给 owner 酌情同步文档。
        const ImuAttitudeDegrees plus180 =
            attitudeDegreesFromOrientation(toFloatQuat(refFromZyxDegrees(0.0, 0.0, 180.0)));
        RIN_CHECK(nearDeg(plus180.yaw, 180.0, 5e-3));
        const ImuAttitudeDegrees minus180 =
            attitudeDegreesFromOrientation(toFloatQuat(refFromZyxDegrees(0.0, 0.0, -180.0)));
        RIN_CHECK(nearDeg(std::fabs(minus180.yaw), 180.0, 5e-3));
    }

    // --- 6) 万向锁附近与恰 ±90°：输出有限、pitch 正确（读数退化仅供参考）---
    // 距奇异 1° 处 float32 舍入经 1/cos(pitch) ≈ 573 倍放大，实测 pitch 偏差
    // ~5e-3°；容差按病态程度放大（0.02°），不存在符号/域错误即通过。
    {
        for (const double pitchDeg : {-89.9, 89.9}) {
            const RefQuat q = refFromZyxDegrees(17.0, pitchDeg, -43.0);
            const ImuAttitudeDegrees a = attitudeDegreesFromOrientation(toFloatQuat(q));
            RIN_CHECK_MSG(finiteAttitude(a),
                          ("gimbal finite pitch=" + std::to_string(pitchDeg)).c_str());
            RIN_CHECK_MSG(nearDeg(a.pitch, pitchDeg, 0.02),
                          ("gimbal pitch=" + std::to_string(pitchDeg)).c_str());
            RIN_CHECK_MSG(nearDeg(a.roll, 17.0, 0.02) && nearDeg(a.yaw, -43.0, 0.02),
                          ("gimbal roll/yaw pitch=" + std::to_string(pitchDeg)).c_str());
        }
        // 恰 ±90°（奇异点）：读数退化仅供参考——只需有限且 pitch 落在 ±90 域内。
        for (const double pitchDeg : {90.0, -90.0}) {
            const RefQuat q = refFromZyxDegrees(17.0, pitchDeg, -43.0);
            const ImuAttitudeDegrees a = attitudeDegreesFromOrientation(toFloatQuat(q));
            RIN_CHECK_MSG(finiteAttitude(a),
                          ("singular finite pitch=" + std::to_string(pitchDeg)).c_str());
            RIN_CHECK_MSG(std::fabs(static_cast<double>(a.pitch)) >= 89.9 &&
                              std::fabs(static_cast<double>(a.pitch)) <= 90.0,
                          ("singular pitch=" + std::to_string(pitchDeg)).c_str());
        }
    }

    // --- 7) 输入消毒（quatNormalize：退化回恒等而非传播 NaN）---
    {
        constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
        constexpr float kInf = std::numeric_limits<float>::infinity();

        const ImuAttitudeDegrees nanInput = attitudeDegreesFromOrientation({kNan, kNan, 0.f, 1.f});
        RIN_CHECK(finiteAttitude(nanInput));
        RIN_CHECK(nearDeg(nanInput.roll, 0.0, 1e-6));
        RIN_CHECK(nearDeg(nanInput.pitch, 0.0, 1e-6));
        RIN_CHECK(nearDeg(nanInput.yaw, 0.0, 1e-6));

        const ImuAttitudeDegrees infInput =
            attitudeDegreesFromOrientation({kInf, 0.0f, 0.0f, 0.0f});
        RIN_CHECK(finiteAttitude(infInput));
        RIN_CHECK(nearDeg(infInput.yaw, 0.0, 1e-6));

        const ImuAttitudeDegrees zeroInput = attitudeDegreesFromOrientation({0.0f, 0.0f, 0.0f, 0.0f});
        RIN_CHECK(finiteAttitude(zeroInput));
        RIN_CHECK(nearDeg(zeroInput.roll, 0.0, 1e-6));

        // 非单位输入（×3.7）与单位输入读数一致（先归一化再分解）。
        const std::array<float, 4> unit =
            quatNormalize(toFloatQuat(refFromZyxDegrees(25.0, -15.0, 65.0)));
        const std::array<float, 4> scaled = {unit[0] * 3.7f, unit[1] * 3.7f, unit[2] * 3.7f,
                                             unit[3] * 3.7f};
        const ImuAttitudeDegrees fromUnit = attitudeDegreesFromOrientation(unit);
        const ImuAttitudeDegrees fromScaled = attitudeDegreesFromOrientation(scaled);
        RIN_CHECK(attitudeEquals(fromUnit, fromScaled, 1e-4));
        RIN_CHECK(nearDeg(fromScaled.roll, 25.0, 5e-3));
        RIN_CHECK(nearDeg(fromScaled.pitch, -15.0, 5e-3));
        RIN_CHECK(nearDeg(fromScaled.yaw, 65.0, 5e-3));
    }

    // --- 8) 与 double 参考在任意姿态上逐角一致（DEC-011 原型姿态）---
    {
        const std::array<float, 4> protoA = quatNormalize({0.904f, 0.215f, 0.306f, -0.206f});
        const std::array<float, 4> protoB = quatNormalize({0.8f, 0.2f, -0.5f, 0.22f});
        for (const std::array<float, 4>& q : {protoA, protoB}) {
            const ImuAttitudeDegrees a = attitudeDegreesFromOrientation(q);
            const ImuAttitudeDegrees reference = refEulerDegrees(
                {static_cast<double>(q[0]), static_cast<double>(q[1]),
                 static_cast<double>(q[2]), static_cast<double>(q[3])});
            RIN_CHECK(attitudeEquals(a, reference, 5e-3));
        }
    }

    // --- 9) formatSourceRate 精确文本 ---
    {
        RIN_CHECK(formatSourceRate(0.0f, 0) == "0.0 Hz · 0 smp");
        RIN_CHECK(formatSourceRate(399.5f, 123456) == "399.5 Hz · 123456 smp");
        RIN_CHECK(formatSourceRate(200.0f, 1) == "200.0 Hz · 1 smp");
        RIN_CHECK(formatSourceRate(12.34f, 7) == "12.3 Hz · 7 smp");
        RIN_CHECK(formatSourceRate(400.0f, 1234567890123ULL) ==
                  "400.0 Hz · 1234567890123 smp");
    }

    // --- 10) formatAttitudeDegrees 精确文本（%+.1f；负号为 ASCII '-'）---
    {
        RIN_CHECK(formatAttitudeDegrees({0.0f, 0.0f, 0.0f}) == "+0.0° · +0.0° · +0.0°");
        RIN_CHECK(formatAttitudeDegrees({12.3f, -4.5f, 179.9f}) ==
                  "+12.3° · -4.5° · +179.9°");
        // 端到端：golden 姿态分解 → 姿态行文本。
        const ImuAttitudeDegrees golden = attitudeDegreesFromOrientation(
            toFloatQuat(refFromZyxDegrees(30.0, 20.0, 40.0)));
        RIN_CHECK(formatAttitudeDegrees(golden) == "+30.0° · +20.0° · +40.0°");
    }

    // --- 11) formatOrientationQuaternion 精确文本（%.3f，标量在前）---
    {
        RIN_CHECK(formatOrientationQuaternion({1.0f, 0.0f, 0.0f, 0.0f}) ==
                  "w 1.000 · x 0.000 · y 0.000 · z 0.000");
        RIN_CHECK(formatOrientationQuaternion({0.999f, 0.012f, -0.005f, 0.003f}) ==
                  "w 0.999 · x 0.012 · y -0.005 · z 0.003");
    }

    // --- 12) 面板空态门控输入（hasSnapshot = pose.available && pose.snapshot.valid()，
    //         imu_panel.hpp:114）：空态 / invalid 快照 / 排空后 stale 快照 ---
    {
        const std::array<float, 4> pose = quatNormalize({0.9f, 0.1f, 0.2f, -0.3f});
        constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

        // 空态：不可用。
        {
            viewer::PoseViewState view;
            RIN_CHECK(!(view.available && view.snapshot.valid()));
        }
        // 有效快照：门控放行（面板显示数值行）。
        {
            viewer::PoseViewState view;
            view.update(makeSnapshot(pose));
            RIN_CHECK(view.available && view.snapshot.valid());
        }
        // invalid 快照（NaN 姿态）：available 但 valid() 拒绝 → 门控不放行。
        {
            viewer::PoseViewState view;
            rin::ImuSnapshot corrupted = makeSnapshot(pose);
            corrupted.orientation = {kNan, 0.0f, 0.0f, 1.0f};
            view.update(corrupted);
            RIN_CHECK(view.available);
            RIN_CHECK(!view.snapshot.valid());
            RIN_CHECK(!(view.available && view.snapshot.valid()));
        }
        // 排空后（M3-07）：clear 只清 available/baseline，stale 快照残留字段但
        // 门控不放行——面板回到 "n/a"，陈旧姿态不跨 shutdown 存活。
        {
            viewer::PoseViewState view;
            view.update(makeSnapshot(pose));
            view.resetView();
            view.clear();
            RIN_CHECK(!view.available);
            RIN_CHECK(!(view.available && view.snapshot.valid()));
            RIN_CHECK((view.baseline ==
                       std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f}));
        }
    }

    return rin_test::exitStatus();
}
