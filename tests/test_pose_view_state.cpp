// M3-06 viewer 3D 位姿视图状态语义测试（独立验证；apps/viewer/pose_view.hpp
// PoseViewState、DEC-011 风险 3 视图重置语义、里程碑 M3-06"视图重置 + 空态"）。
//
// 覆盖（实现自述契约，pose_view.hpp:32-59）：
// - 空态初值：available == false、baseline 恒等；
// - update()：置可用、逐字段保存快照（orientation/sequence/sources），且不动
//   baseline（重锚定参考跨快照持续，直到 clear）；
// - 显示姿态公式（compose 使用，pose_view.hpp:254-257）：
//   display = quatMultiply(quatConjugate(baseline), snapshot.orientation)。
//   baseline 恒等时 display == 原始姿态（逐位）；resetView() 后 baseline ==
//   当前姿态 → display == 恒等（抑制六轴 yaw 漂移的视觉积累，DEC-011 风险 3）；
// - resetView() 幂等：重复点击 baseline 逐位不变、显示仍恒等；后续新快照以
//   重锚定参考为基准显示相对运动（conj(baseline) ⊗ q2，与 double 参考一致）；
// - 空态 resetView() 是 no-op（不产生陈旧重锚定）；clear() 回空态并复位 baseline
//   恒等，之后 update 重新按原始姿态显示（restream/换设备语义）。
//
// 范围与限制（如实说明）：compose 绘制路径（固定网格/视锥/相机轴 polygon 组装、
// ≤37 多边形有界提交、空态文案）需要活动 EUI-NEO 运行时与窗口，无法 headless
// 单测，真机视觉验收归 M3-08；其依赖的 Core 投影数学由 pose_math 测试独立覆盖，
// gyro→color 外参换算所用 Core 函数（matrix3FromColumnMajor/transposeMatrix3/
// multiplyMatrix3 的列主序契约）亦在 pose_math 测试中覆盖。app.cpp pump() 的
// Streaming/Restreaming 门控属应用装配（M3-07 关闭回归范围），本测试不重复。
//
// DOD-02 适用性说明：PoseViewState 是 compose/UI 线程独占的普通值状态（无
// Executor 任务/队列/取消/超时/shutdown 语义），并发矩阵不适用；采集 worker →
// LatestMailbox → pump 的跨上下文通路不在本单元（既有 motion_ingest 覆盖发布/
// 读取与 shutdown 收敛并已入档 tsan 复跑）。
//
// 构建注记：本测试包含 apps/viewer/pose_view.hpp（经 viewer_theme.hpp 引入
// EUI-NEO 公开头）；仅使用其状态类型与 Core 四元数函数，不链接 EUI-NEO 库——
// 未引用的 inline 组装函数不产生外部符号依赖。头文件包含路径经 eui_neo 目标的
// 用法属性注入（见 tests/CMakeLists.txt）。
#include "test_util.hpp"

#include <array>
#include <cmath>
#include <cstring>

#include "pose_view.hpp"
#include "rin/pose_math.hpp"

namespace {

using viewer::PoseViewState;
using rin::quatConjugate;
using rin::quatMultiply;
using rin::quatNormalize;

// --- double 参考四元数（独立于被测复合表达式验证显示公式数值）---

using RefQuat = std::array<double, 4>;

RefQuat refMul(const RefQuat& a, const RefQuat& b) {
    return {a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
            a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
            a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
            a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]};
}

RefQuat refConj(const RefQuat& q) { return {q[0], -q[1], -q[2], -q[3]}; }

RefQuat toRef(const std::array<float, 4>& q) {
    return {static_cast<double>(q[0]), static_cast<double>(q[1]),
            static_cast<double>(q[2]), static_cast<double>(q[3])};
}

/// display = conj(baseline) ⊗ current（pose_view.hpp 文档契约）的 double 参考。
std::array<float, 4> expectedDisplay(const std::array<float, 4>& baseline,
                                     const std::array<float, 4>& current) {
    const RefQuat r = refMul(refConj(toRef(baseline)), toRef(current));
    return {static_cast<float>(r[0]), static_cast<float>(r[1]), static_cast<float>(r[2]),
            static_cast<float>(r[3])};
}

bool quatNear(const std::array<float, 4>& actual, const std::array<float, 4>& expected,
              double tolerance) {
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(static_cast<double>(actual[i]) - expected[i]) > tolerance) {
            return false;
        }
    }
    return true;
}

bool isIdentity(const std::array<float, 4>& q) {
    return q == std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f};
}

/// 构造带完整元数据的快照。
rin::ImuSnapshot makeSnapshot(const std::array<float, 4>& orientation, std::uint64_t sequence,
                              float gyroHz, float accelHz) {
    rin::ImuSnapshot snapshot;
    snapshot.orientation = orientation;
    snapshot.sequence = sequence;
    snapshot.sources.gyroHz = gyroHz;
    snapshot.sources.accelHz = accelHz;
    snapshot.sources.gyroSamples = 400;
    snapshot.sources.accelSamples = 250;
    return snapshot;
}

}  // namespace

int main() {
    // DEC-011 原型姿态（非平凡、非对称）与第二条姿态，经 quatNormalize 保证单位。
    const std::array<float, 4> poseA =
        quatNormalize({0.904f, 0.215f, 0.306f, -0.206f});
    const std::array<float, 4> poseB = quatNormalize({0.8f, 0.2f, -0.5f, 0.22f});

    // --- 1) 空态初值：不可用 + 恒等参考 ---
    {
        PoseViewState state;
        RIN_CHECK(!state.available);
        RIN_CHECK(isIdentity(state.baseline));
    }

    // --- 2) update()：置可用、逐字段保存、不动 baseline ---
    {
        PoseViewState state;
        const rin::ImuSnapshot snapshot = makeSnapshot(poseA, 7, 199.5f, 249.5f);
        state.update(snapshot);
        RIN_CHECK(state.available);
        RIN_CHECK(state.snapshot.valid());
        RIN_CHECK(state.snapshot.orientation == poseA);
        RIN_CHECK_EQ(state.snapshot.sequence, 7u);
        RIN_CHECK(state.snapshot.sources.gyroHz == 199.5f);
        RIN_CHECK(state.snapshot.sources.accelHz == 249.5f);
        RIN_CHECK_EQ(state.snapshot.sources.gyroSamples, 400u);
        RIN_CHECK_EQ(state.snapshot.sources.accelSamples, 250u);
        RIN_CHECK(isIdentity(state.baseline));  // 重锚定参考跨快照持续

        // baseline 恒等时显示姿态 == 原始姿态（conj(I)⊗q == q 逐位）。
        const std::array<float, 4> display =
            quatMultiply(quatConjugate(state.baseline), state.snapshot.orientation);
        RIN_CHECK(display == poseA);
    }

    // --- 3) resetView()：重锚定显示参考（DEC-011 风险 3），显示回恒等、幂等 ---
    {
        PoseViewState state;
        state.update(makeSnapshot(poseA, 1, 200.0f, 250.0f));
        state.resetView();
        RIN_CHECK(state.available);
        RIN_CHECK(state.baseline == poseA);  // 锚定到当前物理姿态

        std::array<float, 4> display =
            quatMultiply(quatConjugate(state.baseline), state.snapshot.orientation);
        RIN_CHECK(quatNear(display, {1.0f, 0.0f, 0.0f, 0.0f}, 1e-6));

        // 幂等：重复 Reset 后 baseline 逐位不变、显示仍恒等。
        const std::array<float, 4> before = state.baseline;
        state.resetView();
        state.resetView();
        RIN_CHECK(std::memcmp(state.baseline.data(), before.data(), sizeof(before)) == 0);
        display = quatMultiply(quatConjugate(state.baseline), state.snapshot.orientation);
        RIN_CHECK(quatNear(display, {1.0f, 0.0f, 0.0f, 0.0f}, 1e-6));

        // 重锚定后的新快照：显示 = conj(baseline) ⊗ q2（相对运动），与 double
        // 参考一致，且不等于原始 q2（参考确实改变了）。
        state.update(makeSnapshot(poseB, 2, 200.0f, 250.0f));
        display = quatMultiply(quatConjugate(state.baseline), state.snapshot.orientation);
        const std::array<float, 4> reference = expectedDisplay(poseA, poseB);
        RIN_CHECK(quatNear(display, reference, 1e-6));
        RIN_CHECK(!(display == poseB));
        // 纯 yaw 漂移场景的语义抽查：漂移在显示中被相对化（此处以一般旋转验证公式）。
    }

    // --- 4) 空态 resetView() 是 no-op（不产生陈旧重锚定）---
    {
        PoseViewState state;
        state.resetView();
        RIN_CHECK(!state.available);
        RIN_CHECK(isIdentity(state.baseline));
    }

    // --- 5) clear()：回空态 + baseline 复位恒等；clear 后 Reset 仍 no-op；
    //        再次 update 重新按原始姿态显示（restream/换设备语义）---
    {
        PoseViewState state;
        state.update(makeSnapshot(poseA, 1, 200.0f, 250.0f));
        state.resetView();
        RIN_CHECK(state.baseline == poseA);

        state.clear();
        RIN_CHECK(!state.available);
        RIN_CHECK(isIdentity(state.baseline));

        state.resetView();  // 空态下 Reset 无效果
        RIN_CHECK(isIdentity(state.baseline));
        RIN_CHECK(!state.available);

        state.update(makeSnapshot(poseB, 9, 198.0f, 251.0f));
        RIN_CHECK(state.available);
        RIN_CHECK(state.snapshot.orientation == poseB);
        RIN_CHECK_EQ(state.snapshot.sequence, 9u);
        const std::array<float, 4> display =
            quatMultiply(quatConjugate(state.baseline), state.snapshot.orientation);
        RIN_CHECK(display == poseB);  // 新会话从原始姿态重新显示
    }

    // --- 6) 显示公式与 Core 复合在一般姿态下逐数值一致（double 参考交叉验证）---
    {
        const std::array<float, 4> baseline = poseA;
        const std::array<float, 4> current = poseB;
        const std::array<float, 4> display = quatMultiply(quatConjugate(baseline), current);
        RIN_CHECK(quatNear(display, expectedDisplay(baseline, current), 1e-6));
        // 相对姿态仍是合法旋转（单位范数）。
        double normSq = 0.0;
        for (const float c : display) {
            normSq += static_cast<double>(c) * c;
        }
        RIN_CHECK(std::fabs(normSq - 1.0) <= 1e-5);
    }

    return rin_test::exitStatus();
}
