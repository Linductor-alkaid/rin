// M3-05 MahonyImuFuser 数值与语义测试（独立验证；src/core/imu_fuser.hpp:59-127、
// DEC-010 冻结算法与验证方式 1-8）。
//
// 完成判据（里程碑 M3-05）与 DEC-010 验证方式对照（误差度量见下）：
// 1) 静态重力对齐收敛：真值 30°/20° 倾斜（初始误差 ~35.5°），默认参数 ≤4s 内
//    roll/pitch 误差 <0.5°（本测另锁对齐后即 <1° 的一次性对齐语义）；
// 2) 已知旋转序列跟踪：60s 多轴正弦（roll ±30°@0.2Hz、pitch ±20°@0.35Hz、
//    yaw 5°/s + ±15°@0.1Hz）roll/pitch RMS ≤0.5°、峰值 ≤2°；
// 3) yaw 漂移披露性：60s 末 yaw 误差 ≤ |b_z|·60s = 30°，且 Ki 开 < Ki 关；
// 4) 零偏估计：静态 60s 后 |b̂_x|、|b̂_y| 残差 ≤0.2°/s（b_z 不可观仅记录）；
// 5) 线加速度扰动：±2 m/s² 竖直平移突发（5s 通/5s 断）门限关闭期间
//    RMS ≤0.5°、峰值 ≤2°；
// 6) 复位幂等：reset() 后同序列重放与全新实例逐位（memcmp）一致；连续两次
//    reset() 重放一致；
// 7) 确定性：同进程内全新实例重放逐位一致（debug/asan/ubsan/tsan 跨预设一致性
//    由门禁脚本全量复跑覆盖，本测试不重复）；
// 8) 热路径无堆分配（EXEC-06，DEC-010"实现以测试断言"）：advance 循环内全局
//    operator new 计数为零；每样本耗时仅打印（1µs 阈值为 DEC-010 的 -O2 release
//    实测口径，debug/消毒剂构建下不作硬断言，避免假失败）。
//
// 误差度量口径（独立验证裁定，依据 DEC-010 原型方法论）：roll/pitch 误差 =
// 重力参考"上"方向夹角 angle(R_estᵀẑ, R_trueᵀẑ)，对绕重力轴的 yaw 规范自由度
// 不敏感——DEC-010 原型在含 300° yaw 的 60s 序列上实测 RMS 0.118°，只有 yaw 无关
// 口径可能成立；且六轴 yaw 不可观（DEC-010 披露）。已知观察（不构成本项判据失
// 败，报告留档）：一次性对齐用最小旋转（旋转轴水平、不含绕 ẑ 分量），对倾斜
// 开机的设备其 ZYX 欧拉 yaw 并非 0（30°/20° 倾斜下实测 +5.41° 世界 yaw 规范
// 偏移，重力参考 roll/pitch 不受影响且不被后续修正改变——e=â×ĝ 在该状态为 0）；
// 与 DEC-010"yaw 置 0"/M3-03"yaw 初值为 0"的字面欧拉读数存在解释分歧，是否
// 调整对齐构造或文档措辞交由实现者/计划 owner 裁定。
//
// 语义/边界锁定（实现自述行为，均为头文件契约或 DEC-010 冻结语义）：
// - GYRO：首样本仅建参考不传播；dt<=0（重复/乱序）跳过且参考点保持；非有限时间
//   戳样本整体忽略（valid() 不约束时间戳域）但接缝契约 hasPose 仍置位；传播用
//   扣除零偏后的 SO(3) 精确指数映射（单步大 dt 仍单位范数）；
// - ACCEL：首个过 1g 门限样本一次性对齐（+1g Z 精确恒等、+1g X 为最小旋转、
//   门限外首样本不参与对齐、零范数不对齐）；门限外跳过修正与零偏更新但推进
//   ACCEL dt 参考点；修正步 e=â×ĝ、b̂-=2·Ki·e·dt、姿态按 2·Kp·e 修正（单步解析
//   对照 + 负增益不钳制）；
// - Params 注入与消毒：非单位初值归一化、零范数/非有限回退恒等、非有限零偏分量
//   归零；默认 Kp=1.0/Ki=0.1（DEC-010 冻结）；工厂返回 MahonyImuFuser 真身。
//
// DOD-02 适用性说明：ImuFuser 为单采集 worker 独占推进的纯逻辑状态（EXEC-06，
// 无线程/队列/任务提交/取消/超时/shutdown 语义），并发矩阵不适用；融合器参与的
// 跨上下文发布/读取与 shutdown 收敛由 motion_ingest 测试覆盖（本轮加跑 tsan）。
// 合成数据按 DEC-010 生成器默认参数（dt_gyro=1/400s、dt_accel=1/250s、
// σ_g=0.002 rad/s、σ_a=0.02 m/s²、注入零偏 [0.3,-0.2,0.5]°/s、固定种子），
// 阈值均取 DEC-010 冻结值，对 libm/编译器浮点差异保有 ≥4 倍裕量。
#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <random>
#include <vector>

#include "imu_fuser.hpp"
#include "rin/camera_types.hpp"

namespace {

using rin::detail::createImuFuser;
using rin::detail::MahonyImuFuser;
using rin::MotionSample;
using rin::MotionStreamKind;

constexpr double kGravity = 9.80665;                 // DEC-010 标准重力（m/s²）
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr std::array<double, 3> kInjectedBiasDegS{0.3, -0.2, 0.5};  // DEC-010

// --- 测试侧独立四元数数学（Hamilton，w,x,y,z 标量在前，传感器系→世界系；
// 不引用实现内部函数，单步解析对照才构成独立验证）---

using Quat = std::array<double, 4>;
using Vec3 = std::array<double, 3>;

Quat qMul(const Quat& a, const Quat& b) {
    return {a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
            a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
            a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
            a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]};
}

Quat qConj(const Quat& q) { return {q[0], -q[1], -q[2], -q[3]}; }

/// v_world = q ⊗ v_sensor ⊗ q*
Vec3 qRotate(const Quat& q, const Vec3& v) {
    const Quat p{0.0, v[0], v[1], v[2]};
    const Quat r = qMul(qMul(q, p), qConj(q));
    return {r[1], r[2], r[3]};
}

/// SO(3) 指数映射：旋转向量 θ（rad）→ 单位四元数。
Quat qExp(const Vec3& theta) {
    const double angle = std::sqrt(theta[0] * theta[0] + theta[1] * theta[1] +
                                   theta[2] * theta[2]);
    if (angle < 1e-12) {
        return {1.0, 0.5 * theta[0], 0.5 * theta[1], 0.5 * theta[2]};
    }
    const double half = 0.5 * angle;
    const double sinc = std::sin(half) / angle;
    return {std::cos(half), sinc * theta[0], sinc * theta[1], sinc * theta[2]};
}

/// ZYX 欧拉（先绕 x roll、再 y pitch、后 z yaw）→ 四元数：q = qz ⊗ qy ⊗ qx。
Quat qEulerZYX(double rollRad, double pitchRad, double yawRad) {
    const Quat qx{std::cos(0.5 * rollRad), std::sin(0.5 * rollRad), 0.0, 0.0};
    const Quat qy{std::cos(0.5 * pitchRad), 0.0, std::sin(0.5 * pitchRad), 0.0};
    const Quat qz{std::cos(0.5 * yawRad), 0.0, 0.0, std::sin(0.5 * yawRad)};
    return qMul(qz, qMul(qy, qx));
}

/// 姿态四元数 → 传感器系"上"方向（世界上方向在传感器系的表达，R(q)ᵀ·ẑ）。
Vec3 upInSensor(const Quat& q) { return qRotate(qConj(q), Vec3{0.0, 0.0, 1.0}); }

double dot3(const Vec3& a, const Vec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/// roll/pitch 误差（DEC-010"roll/pitch 误差"语义）：重力参考的"上"方向夹角
/// angle(R_estᵀẑ, R_trueᵀẑ)——对绕世界重力轴的 yaw 分量不敏感（六轴 yaw 不可观，
/// DEC-010 披露；原型 60s 序列含 300° yaw 仍实测 RMS 0.118°，证明验收指标为
/// yaw 无关口径）。注意不可用 angle(R·ẑ) 之类世界系轴夹角：对倾斜设备，纯 yaw
/// 偏移（绕 ẑ 的规范自由度）会以 ~sin(倾角)·Δψ 泄漏进该口径。
double tiltErrorDeg(const Quat& estimate, const Quat& truth) {
    const double c =
        std::clamp(dot3(upInSensor(estimate), upInSensor(truth)), -1.0, 1.0);
    return std::acos(c) / kDeg;
}

/// yaw 误差：相对旋转 q_rel = q_est ⊗ q_true* 的 ZYX 偏航角。
double yawErrorDeg(const Quat& estimate, const Quat& truth) {
    const Quat rel = qMul(estimate, qConj(truth));
    const double yaw =
        std::atan2(2.0 * (rel[0] * rel[3] + rel[1] * rel[2]),
                   1.0 - 2.0 * (rel[2] * rel[2] + rel[3] * rel[3]));
    return yaw / kDeg;
}

double normSq(const std::array<float, 4>& q) {
    double sum = 0.0;
    for (const float component : q) {
        sum += static_cast<double>(component) * component;
    }
    return sum;
}

Quat toDouble(const std::array<float, 4>& q) {
    return {static_cast<double>(q[0]), static_cast<double>(q[1]),
            static_cast<double>(q[2]), static_cast<double>(q[3])};
}

bool approx(float value, double expected, double tolerance) {
    return std::fabs(static_cast<double>(value) - expected) <= tolerance;
}

bool approxArray(const std::array<float, 4>& actual, const Quat& expected,
                 double tolerance) {
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(static_cast<double>(actual[i]) - expected[i]) > tolerance) {
            return false;
        }
    }
    return true;
}

// --- DEC-010 合成数据生成器（固定种子；dt_gyro=1/400s、dt_accel=1/250s、
// σ_g=0.002 rad/s、σ_a=0.02 m/s²、注入零偏 [0.3,-0.2,0.5]°/s）---

struct ScenarioConfig {
    double durationS = 60.0;
    bool sineTrajectory = true;  // false：固定 30°/20° 静态倾斜
    bool burst = false;          // ±2 m/s² 竖直平移突发（5s 通/5s 断）
    bool noise = true;
    unsigned seed = 20260924u;
};

double rollDegAt(double t) { return 30.0 * std::sin(2.0 * kPi * 0.2 * t); }

double pitchDegAt(double t) {
    return 20.0 * std::sin(2.0 * kPi * 0.35 * t);
}

double yawDegAt(double t) {
    return 5.0 * t + 15.0 * std::sin(2.0 * kPi * 0.1 * t);
}

Quat truthAt(const ScenarioConfig& config, double tSeconds) {
    if (config.sineTrajectory) {
        return qEulerZYX(rollDegAt(tSeconds) * kDeg, pitchDegAt(tSeconds) * kDeg,
                         yawDegAt(tSeconds) * kDeg);
    }
    return qEulerZYX(30.0 * kDeg, 20.0 * kDeg, 0.0);
}

/// 竖直平移加速度（m/s²，世界系 +Z）：突发窗口内 ±2，否则 0。
double burstAzMs2(double tSeconds) {
    const bool window = (tSeconds >= 10.0 && tSeconds < 15.0) ||
                        (tSeconds >= 30.0 && tSeconds < 35.0) ||
                        (tSeconds >= 50.0 && tSeconds < 55.0);
    if (!window) {
        return 0.0;
    }
    const long period = static_cast<long>(tSeconds / 10.0);
    return (period % 2) == 1 ? -2.0 : 2.0;
}

std::vector<MotionSample> buildScenario(const ScenarioConfig& config) {
    constexpr double kDtGyroMs = 1000.0 / 400.0;   // 2.5 ms
    constexpr double kDtAccelMs = 1000.0 / 250.0;  // 4.0 ms
    constexpr double kSigmaGyro = 0.002;           // rad/s
    constexpr double kSigmaAccel = 0.02;           // m/s²
    const double durationMs = config.durationS * 1000.0;

    std::mt19937 generator(config.seed);
    std::normal_distribution<double> gyroNoise(0.0, kSigmaGyro);
    std::normal_distribution<double> accelNoise(0.0, kSigmaAccel);

    Vec3 injectedBias{};
    for (int i = 0; i < 3; ++i) {
        injectedBias[i] = kInjectedBiasDegS[i] * kDeg;
    }

    std::vector<MotionSample> gyro;
    std::vector<MotionSample> accel;
    const int gyroCount = static_cast<int>(durationMs / kDtGyroMs);
    const int accelCount = static_cast<int>(durationMs / kDtAccelMs);
    gyro.reserve(static_cast<std::size_t>(gyroCount));
    accel.reserve(static_cast<std::size_t>(accelCount));

    const double kDeltaS = 1e-4;  // 真值角速度的数值微分步长
    for (int i = 0; i < gyroCount; ++i) {
        const double tSeconds = i * kDtGyroMs * 1e-3;
        // 真值机体角速度：ω = 2·vec(q(t)* ⊗ q(t+δ))/δ（Ṙ = R[ω_s]× 的一阶 log）。
        const Quat q0 = truthAt(config, tSeconds);
        const Quat q1 = truthAt(config, tSeconds + kDeltaS);
        Quat delta = qMul(qConj(q0), q1);
        if (delta[0] < 0.0) {
            for (double& component : delta) {
                component = -component;
            }
        }
        MotionSample sample;
        sample.kind = MotionStreamKind::Gyro;
        for (int axis = 0; axis < 3; ++axis) {
            const double omega = 2.0 * delta[axis + 1] / kDeltaS;
            sample.axes[axis] = static_cast<float>(
                omega + injectedBias[axis] +
                (config.noise ? gyroNoise(generator) : 0.0));
        }
        sample.deviceTimestampMs = i * kDtGyroMs;
        gyro.push_back(sample);
    }

    for (int i = 0; i < accelCount; ++i) {
        const double tSeconds = i * kDtAccelMs * 1e-3;
        // 比力：f_sensor = Rᵀ·(g·ẑ − a_world)，静态时沿"传感器系上方向"读 +1g。
        const double az = config.burst ? burstAzMs2(tSeconds) : 0.0;
        const Vec3 specificWorld{0.0, 0.0, kGravity - az};
        const Vec3 ideal = qRotate(qConj(truthAt(config, tSeconds)), specificWorld);
        MotionSample sample;
        sample.kind = MotionStreamKind::Accel;
        for (int axis = 0; axis < 3; ++axis) {
            sample.axes[axis] = static_cast<float>(
                ideal[axis] + (config.noise ? accelNoise(generator) : 0.0));
        }
        sample.deviceTimestampMs = i * kDtAccelMs;
        accel.push_back(sample);
    }

    // 单流合并（时间戳升序，同刻 ACCEL 在前：先对齐后传播，贴近设备启动次序）。
    std::vector<MotionSample> merged;
    merged.reserve(gyro.size() + accel.size());
    std::size_t gi = 0;
    std::size_t ai = 0;
    while (gi < gyro.size() || ai < accel.size()) {
        const bool takeGyro =
            ai >= accel.size() ||
            (gi < gyro.size() &&
             (gyro[gi].deviceTimestampMs < accel[ai].deviceTimestampMs));
        merged.push_back(takeGyro ? gyro[gi++] : accel[ai++]);
    }
    return merged;
}

// --- 场景驱动与误差统计 ---

struct ScenarioRun {
    std::vector<std::array<float, 4>> trace;  // 每个采样推进后的姿态
    std::array<double, 3> finalBias{};
    bool hasPoseAfterFirstSample = false;
};

ScenarioRun runScenario(const std::vector<MotionSample>& samples,
                        const MahonyImuFuser::Params& params) {
    MahonyImuFuser fuser(params);
    ScenarioRun run;
    run.trace.reserve(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        fuser.advance(samples[i]);
        if (i == 0) {
            run.hasPoseAfterFirstSample = fuser.hasPose();
        }
        run.trace.push_back(fuser.orientation());
    }
    run.finalBias = fuser.gyroBiasEstimate();
    return run;
}

struct ErrorStats {
    double rmsDeg = 0.0;
    double peakDeg = 0.0;
    double maxNormSqDev = 0.0;
    std::size_t count = 0;
};

/// 逐样本 roll/pitch 误差统计（跳过首样本对齐点；burstOnly 限突发窗口）。
ErrorStats tiltStats(const std::vector<MotionSample>& samples, const ScenarioRun& run,
                     const ScenarioConfig& config, bool burstOnly) {
    ErrorStats stats;
    double sumSq = 0.0;
    double peak = 0.0;
    bool aligned = false;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        stats.maxNormSqDev =
            std::max(stats.maxNormSqDev, std::fabs(normSq(run.trace[i]) - 1.0));
        if (!aligned) {
            if (samples[i].kind == MotionStreamKind::Accel) {
                aligned = true;  // 首个 ACCEL 即对齐点，不计入统计
            }
            continue;
        }
        if (burstOnly &&
            burstAzMs2(samples[i].deviceTimestampMs * 1e-3) == 0.0) {
            continue;
        }
        const Quat truth =
            truthAt(config, samples[i].deviceTimestampMs * 1e-3);
        const double errorDeg = tiltErrorDeg(toDouble(run.trace[i]), truth);
        sumSq += errorDeg * errorDeg;
        peak = std::max(peak, errorDeg);
        ++stats.count;
    }
    stats.rmsDeg = stats.count > 0 ? std::sqrt(sumSq / static_cast<double>(stats.count))
                                   : 0.0;
    stats.peakDeg = peak;
    return stats;
}

// --- EXEC-06 热路径无堆分配断言：全局 operator new 计数（仅测试进程内生效；
//     分配/释放函数必须位于全局作用域，见全局区定义）---

std::size_t g_newCount = 0;
std::size_t g_newBytes = 0;
bool g_counting = false;

void* countedNew(std::size_t size) {
    if (g_counting) {
        ++g_newCount;
        g_newBytes += size;
    }
    if (void* p = std::malloc(size)) {
        return p;
    }
    throw std::bad_alloc{};
}

// --- 便捷采样构造（解析用例，无噪声）---
constexpr double kTiltAx = 0.196133;  // ~2° 的 +x 倾斜比力分量（1g 门限内）

/// 修正单步解析对照用的 â_x：经 float 往返与 MotionSample.axes 携带值逐位一致。
double tiltAxUnit() {
    const double ax = static_cast<double>(static_cast<float>(kTiltAx));
    const double az = static_cast<double>(static_cast<float>(kGravity));
    return ax / std::sqrt(ax * ax + az * az);
}

MotionSample gyroSample(double timestampMs, double wx, double wy, double wz) {
    MotionSample sample;
    sample.kind = MotionStreamKind::Gyro;
    sample.axes = {static_cast<float>(wx), static_cast<float>(wy),
                   static_cast<float>(wz)};
    sample.deviceTimestampMs = timestampMs;
    return sample;
}

MotionSample accelSample(double timestampMs, double ax, double ay, double az) {
    MotionSample sample;
    sample.kind = MotionStreamKind::Accel;
    sample.axes = {static_cast<float>(ax), static_cast<float>(ay),
                   static_cast<float>(az)};
    sample.deviceTimestampMs = timestampMs;
    return sample;
}

}  // namespace

// 全局作用域分配函数（C++ 标准要求：分配/释放函数不得声明于命名空间内）。
void* operator new(std::size_t size) { return countedNew(size); }
void* operator new[](std::size_t size) { return countedNew(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return countedNew(size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return countedNew(size);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

int main() {
    // --- 0) 冻结默认参数与工厂真身（DEC-010 决策 5；头文件工厂契约 M3-05 起）---
    {
        const MahonyImuFuser::Params defaults{};
        RIN_CHECK_EQ(defaults.kp, 1.0);
        RIN_CHECK_EQ(defaults.ki, 0.1);

        const std::unique_ptr<rin::detail::ImuFuser> product = createImuFuser();
        RIN_CHECK(product != nullptr);
        // M3-05 起工厂契约即 MahonyImuFuser（头文件 imu_fuser.hpp:125-127）。
        RIN_CHECK(dynamic_cast<MahonyImuFuser*>(product.get()) != nullptr);
        // 工厂产品是真融合：过门限的倾斜 ACCEL 对齐后姿态偏离恒等（+x 倾斜 →
        // 绕 −y 的最小旋转，z 分量为负、标量接近 1）。
        product->advance(accelSample(0.0, 0.196133, 0.0, kGravity));
        RIN_CHECK(product->hasPose());
        RIN_CHECK(product->orientation()[2] < 0.0f);
        RIN_CHECK(product->orientation()[0] > 0.999f);  // ~1.15° 倾斜：w=cos(θ/2)
        RIN_CHECK(std::fabs(static_cast<double>(product->orientation()[1])) <= 1e-6);
        RIN_CHECK(std::fabs(static_cast<double>(product->orientation()[3])) <= 1e-6);
    }

    // --- 1) Params 注入与消毒（头文件 imu_fuser.hpp:80-91、107-108）---
    {
        // 非单位初值归一化：{2,0,0,0} → {1,0,0,0}。
        MahonyImuFuser::Params params;
        params.initialOrientation = {2.0, 0.0, 0.0, 0.0};
        const MahonyImuFuser normalized(params);
        RIN_CHECK_EQ(normalized.orientation()[0], 1.0f);
        RIN_CHECK_EQ(normalized.orientation()[1], 0.0f);
        RIN_CHECK_EQ(normalized.orientation()[3], 0.0f);

        // 零范数初值回退恒等；非有限初值回退恒等。
        params.initialOrientation = {0.0, 0.0, 0.0, 0.0};
        const MahonyImuFuser zeroNorm(params);
        RIN_CHECK_EQ(zeroNorm.orientation()[0], 1.0f);
        params.initialOrientation =
            {std::numeric_limits<double>::quiet_NaN(), 1.0, 0.0, 0.0};
        const MahonyImuFuser nonFinite(params);
        RIN_CHECK_EQ(nonFinite.orientation()[0], 1.0f);
        RIN_CHECK_EQ(nonFinite.orientation()[2], 0.0f);

        // 非单位但可归一化的注入初值被真实采用（推进前可见），零偏有限分量保留、
        // 非有限分量归零。
        params.initialOrientation = {0.0, 1.0, 0.0, 0.0};  // 单位：绕 x 90°
        params.initialGyroBias = {0.5, std::numeric_limits<double>::quiet_NaN(), -0.25};
        const MahonyImuFuser injected(params);
        RIN_CHECK_EQ(injected.orientation()[0], 0.0f);
        RIN_CHECK_EQ(injected.orientation()[1], 1.0f);
        RIN_CHECK_EQ(injected.orientation()[2], 0.0f);
        const std::array<double, 3> bias = injected.gyroBiasEstimate();
        RIN_CHECK_EQ(bias[0], 0.5);
        RIN_CHECK_EQ(bias[1], 0.0);
        RIN_CHECK_EQ(bias[2], -0.25);
        RIN_CHECK(!injected.hasPose());
    }

    // --- 2) GYRO 传播语义：首样本参考点、单调守卫、非有限时间戳、零偏补偿、
    //        精确指数映射（解析对照）---
    {
        // 2a) 首样本只建参考不传播；重复/乱序样本跳过且参考点保持。
        MahonyImuFuser fuser;
        fuser.advance(gyroSample(0.0, 0.0, 0.0, kPi));  // 仅建参考
        RIN_CHECK(fuser.hasPose());
        RIN_CHECK_EQ(fuser.orientation()[0], 1.0f);
        RIN_CHECK_EQ(fuser.orientation()[3], 0.0f);

        fuser.advance(gyroSample(100.0, 0.0, 0.0, kPi));  // dt=100ms → 绕 z 转 0.1π rad
        Quat expected = qExp(Vec3{0.0, 0.0, kPi * 0.1});
        RIN_CHECK(approxArray(fuser.orientation(), expected, 1e-6));

        const std::array<float, 4> before = fuser.orientation();
        fuser.advance(gyroSample(100.0, 0.0, 0.0, kPi));  // dt=0：跳过
        RIN_CHECK(fuser.orientation() == before);
        fuser.advance(gyroSample(50.0, 0.0, 0.0, kPi));  // dt<0：跳过，参考点保持
        RIN_CHECK(fuser.orientation() == before);
        fuser.advance(gyroSample(200.0, 0.0, 0.0, kPi));  // dt=100ms（自 100ms 参考）
        expected = qExp(Vec3{0.0, 0.0, kPi * 0.2});
        RIN_CHECK(approxArray(fuser.orientation(), expected, 1e-6));
    }
    {
        // 2b) 非有限时间戳样本整体忽略（不传播、参考点不被覆盖），但接缝契约
        //     hasPose 仍置位（valid() 不约束时间戳域）。
        MahonyImuFuser fuser;
        fuser.advance(gyroSample(0.0, 0.0, 0.0, kPi));
        fuser.advance(gyroSample(100.0, 0.0, 0.0, kPi));
        const std::array<float, 4> before = fuser.orientation();
        fuser.advance(gyroSample(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0,
                                 kPi));
        RIN_CHECK(fuser.hasPose());  // 首个有效采样推进后姿态可用（接缝契约）
        RIN_CHECK(fuser.orientation() == before);
        fuser.advance(
            gyroSample(std::numeric_limits<double>::infinity(), 0.0, 0.0, kPi));
        RIN_CHECK(fuser.orientation() == before);
        fuser.advance(gyroSample(200.0, 0.0, 0.0, kPi));  // dt=100ms（自保留参考）
        const Quat expected = qExp(Vec3{0.0, 0.0, kPi * 0.2});
        RIN_CHECK(approxArray(fuser.orientation(), expected, 1e-6));
    }
    {
        // 2c) 传播扣除注入零偏：ω=(0.3,0,0)，b̂=(0.1,0,0)，dt=0.5s → 绕 x 转 0.1 rad。
        MahonyImuFuser::Params params;
        params.initialGyroBias = {0.1, 0.0, 0.0};
        MahonyImuFuser fuser(params);
        fuser.advance(gyroSample(0.0, 0.3, 0.0, 0.0));
        fuser.advance(gyroSample(500.0, 0.3, 0.0, 0.0));
        const Quat expected = qExp(Vec3{0.1, 0.0, 0.0});
        RIN_CHECK(approxArray(fuser.orientation(), expected, 1e-6));
    }
    {
        // 2d) 单步大 dt 精确指数映射保持单位范数（流间隙语义）。
        MahonyImuFuser fuser;
        fuser.advance(gyroSample(0.0, 1.0, 2.0, 3.0));
        fuser.advance(gyroSample(10000.0, 1.0, 2.0, 3.0));  // dt=10s，旋转角 ~37.4 rad
        const double dev = std::fabs(normSq(fuser.orientation()) - 1.0);
        RIN_CHECK(dev <= 1e-5);
        const Quat expected = qExp(Vec3{10.0, 20.0, 30.0});
        RIN_CHECK(approxArray(fuser.orientation(), expected, 1e-5));
    }

    // --- 3) ACCEL 对齐语义：+1g Z 精确恒等、+1g X 最小旋转、污染首样本不对齐、
    //        零范数不对齐（相机契约：静止时上指轴读 +1g）---
    {
        MahonyImuFuser fuser;
        fuser.advance(accelSample(0.0, 0.0, 0.0, kGravity));  // +1g Z → 精确恒等
        RIN_CHECK(fuser.hasPose());
        RIN_CHECK((fuser.orientation() == std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f}));
    }
    {
        MahonyImuFuser fuser;
        fuser.advance(accelSample(0.0, kGravity, 0.0, 0.0));  // +1g X → x̂→ẑ 最小旋转
        // 期望 q = (√2/2, 0, −√2/2, 0)：R(q)·x̂ = ẑ 且 yaw 分量为 0。
        RIN_CHECK(approx(fuser.orientation()[0], std::sqrt(0.5), 1e-6));
        RIN_CHECK(std::fabs(static_cast<double>(fuser.orientation()[1])) <= 1e-6);
        RIN_CHECK(approx(fuser.orientation()[2], -std::sqrt(0.5), 1e-6));
        RIN_CHECK(std::fabs(static_cast<double>(fuser.orientation()[3])) <= 1e-6);
        const Vec3 worldX = qRotate(toDouble(fuser.orientation()), Vec3{1.0, 0.0, 0.0});
        RIN_CHECK(std::fabs(worldX[0]) <= 1e-6 && std::fabs(worldX[1]) <= 1e-6 &&
                  std::fabs(worldX[2] - 1.0) <= 1e-6);
    }
    {
        // 门限外首个样本视为污染：不对齐（姿态保持初值），后续过门限样本才对齐。
        MahonyImuFuser fuser;
        fuser.advance(accelSample(0.0, 2.0 * kGravity, 0.0, 0.0));  // 2g：门限外
        RIN_CHECK(fuser.hasPose());
        RIN_CHECK((fuser.orientation() == std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f}));
        fuser.advance(accelSample(4.0, kGravity, 0.0, 0.0));  // 过门限 → 对齐
        RIN_CHECK(approx(fuser.orientation()[2], -std::sqrt(0.5), 1e-6));
    }
    {
        // 零范数 ACCEL（valid() 不排除）：不对齐、状态不动，后续样本正常对齐。
        MahonyImuFuser fuser;
        fuser.advance(accelSample(0.0, 0.0, 0.0, 0.0));
        RIN_CHECK(fuser.hasPose());
        RIN_CHECK((fuser.orientation() == std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f}));
        fuser.advance(accelSample(4.0, kGravity, 0.0, 0.0));
        RIN_CHECK(approx(fuser.orientation()[2], -std::sqrt(0.5), 1e-6));
    }
    {
        // 非有限时间戳 ACCEL 整体忽略：不对齐、不推进参考点。
        MahonyImuFuser fuser;
        fuser.advance(
            accelSample(std::numeric_limits<double>::quiet_NaN(), kGravity, 0.0, 0.0));
        RIN_CHECK(fuser.hasPose());
        RIN_CHECK((fuser.orientation() == std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f}));
    }

    // --- 4) PI 修正单步解析对照：e=â×ĝ、b̂-=2·Ki·e·dt、姿态按 2·Kp·e 修正、
    //        ACCEL dt 参考点与门限跳过无关地推进（头文件 imu_fuser.hpp:70-75）---
    {
        MahonyImuFuser fuser;  // Kp=1.0, Ki=0.1, 初始恒等、零偏零
        fuser.advance(accelSample(0.0, 0.0, 0.0, kGravity));  // 对齐恒等
        // 修正样本：â_x ≈ 0.02（~2° 倾斜），dt=100ms。
        fuser.advance(accelSample(100.0, kTiltAx, 0.0, kGravity));
        const double axUnit = tiltAxUnit();
        // e = â×ĝ = (0, −â_x, 0)（ĝ=ẑ）；b̂_y = −2·Ki·e_y·dt = +0.02·â_x。
        const std::array<double, 3> bias = fuser.gyroBiasEstimate();
        RIN_CHECK(std::fabs(bias[0]) <= 1e-12);
        RIN_CHECK(std::fabs(bias[1] - 2.0 * 0.1 * axUnit * 0.1) <= 1e-10);
        RIN_CHECK(std::fabs(bias[2]) <= 1e-12);
        // q_new = exp(0.5·(2·Kp·e)·dt)（q=恒等时右乘即本体）：绕 −y 转 0.2·â_x rad。
        const Quat expected = qExp(Vec3{0.0, -2.0 * 1.0 * axUnit * 0.1, 0.0});
        RIN_CHECK(approxArray(fuser.orientation(), expected, 1e-6));
    }
    {
        // 门限外样本推进 ACCEL dt 参考点：修正 dt 跨门限样本计为 100ms（非 200ms）。
        MahonyImuFuser fuser;
        fuser.advance(accelSample(0.0, 0.0, 0.0, kGravity));          // 对齐，参考 0ms
        fuser.advance(accelSample(100.0, 0.0, 0.0, 2.0 * kGravity));  // 门限外，参考→100ms
        fuser.advance(accelSample(200.0, kTiltAx, 0.0, kGravity));    // dt=100ms
        const std::array<double, 3> bias = fuser.gyroBiasEstimate();
        RIN_CHECK(std::fabs(bias[1] - 2.0 * 0.1 * tiltAxUnit() * 0.1) <= 1e-10);
    }
    {
        // ACCEL 乱序样本不修正；ACCEL dt 参考点按样本推进到乱序时间戳（与 GYRO 的
        // "参考点保持不动"不同：头文件 imu_fuser.hpp:74-75"按样本推进参考点"，实现
        // 在门限/修正守卫之前更新参考），后续修正 dt = 相邻设备时间戳差 150ms。
        MahonyImuFuser fuser;
        fuser.advance(accelSample(0.0, 0.0, 0.0, kGravity));       // 对齐
        fuser.advance(accelSample(100.0, 0.0, 0.0, kGravity));     // e=0：零偏/姿态不动
        fuser.advance(accelSample(50.0, kTiltAx, 0.0, kGravity));  // dt<0：不修正
        std::array<double, 3> bias = fuser.gyroBiasEstimate();
        RIN_CHECK(std::fabs(bias[1]) <= 1e-12);
        fuser.advance(accelSample(200.0, kTiltAx, 0.0, kGravity));  // dt=150ms（自 50ms）
        bias = fuser.gyroBiasEstimate();
        RIN_CHECK(std::fabs(bias[1] - 2.0 * 0.1 * tiltAxUnit() * 0.15) <= 1e-10);
    }
    {
        // 负增益不隐藏钳制：Ki<0 时零偏更新反号；Kp=0 时姿态修正归零但零偏仍更新。
        MahonyImuFuser::Params params;
        params.ki = -0.5;
        MahonyImuFuser fuser(params);
        fuser.advance(accelSample(0.0, 0.0, 0.0, kGravity));
        fuser.advance(accelSample(100.0, kTiltAx, 0.0, kGravity));
        const std::array<double, 3> bias = fuser.gyroBiasEstimate();
        // b̂_y = −2·Ki·e_y·dt，Ki=−0.5、e_y=−â_x → −0.1·â_x（与默认 Ki 反号）。
        RIN_CHECK(std::fabs(bias[1] + 2.0 * 0.5 * tiltAxUnit() * 0.1) <= 1e-10);

        MahonyImuFuser::Params kp0;
        kp0.kp = 0.0;
        MahonyImuFuser noCorrection(kp0);
        noCorrection.advance(accelSample(0.0, 0.0, 0.0, kGravity));
        noCorrection.advance(accelSample(100.0, kTiltAx, 0.0, kGravity));
        RIN_CHECK((noCorrection.orientation() ==
                   std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f}));
        RIN_CHECK(std::fabs(noCorrection.gyroBiasEstimate()[1] -
                            2.0 * 0.1 * tiltAxUnit() * 0.1) <= 1e-10);
    }

    // --- 5) DEC-010 判据 1：静态重力对齐收敛（真值 30°/20°，≤4s <0.5°）---
    double staticTiltAfterAlignDeg = 0.0;
    double staticTiltEndDeg = 0.0;
    double initialTiltErrorDeg = 0.0;
    {
        const Quat truthStatic = qEulerZYX(30.0 * kDeg, 20.0 * kDeg, 0.0);
        // 初始误差可观：恒等初值下 roll/pitch 误差 ≈35.5°（DEC-010 记 35.9° 量级）。
        MahonyImuFuser probe;
        probe.advance(gyroSample(0.0, 0.0, 0.0, 0.0));
        initialTiltErrorDeg = tiltErrorDeg(toDouble(probe.orientation()), truthStatic);
        RIN_CHECK(initialTiltErrorDeg > 30.0);

        ScenarioConfig config;
        config.sineTrajectory = false;
        config.durationS = 4.0;
        const std::vector<MotionSample> samples = buildScenario(config);
        const ScenarioRun run = runScenario(samples, MahonyImuFuser::Params{});
        RIN_CHECK(run.hasPoseAfterFirstSample);
        staticTiltAfterAlignDeg =
            tiltErrorDeg(toDouble(run.trace[0]), truthStatic);  // 首样本即对齐点
        RIN_CHECK(staticTiltAfterAlignDeg < 1.0);  // 一次性对齐（测量噪声上界 ~0.35°）
        const ErrorStats stats = tiltStats(samples, run, config, false);
        staticTiltEndDeg = tiltErrorDeg(toDouble(run.trace.back()), truthStatic);
        RIN_CHECK(staticTiltEndDeg < 0.5);  // DEC-010 判据（4s 末）
        RIN_CHECK(stats.peakDeg < 1.0);     // 对齐后无大幅回弹
        std::printf(
            "static-align: initial %.2f deg -> after-align %.3f deg -> 4s %.3f deg "
            "(peak %.3f deg)\n",
            initialTiltErrorDeg, staticTiltAfterAlignDeg, staticTiltEndDeg,
            stats.peakDeg);
    }

    // --- 6) DEC-010 判据 2/3：60s 已知旋转序列跟踪与 yaw 漂移披露 ---
    double trackingRmsDeg = 0.0;
    double trackingPeakDeg = 0.0;
    double yawKiOnDeg = 0.0;
    double yawKiOffDeg = 0.0;
    double traceNormDev = 0.0;
    {
        const ScenarioConfig config;  // 60s 正弦序列，固定种子噪声
        const std::vector<MotionSample> samples = buildScenario(config);

        // 生成器自检：t=0 处真值机体角速度 ≈ (φ̇, θ̇, ψ̇)（零位欧拉率），防生成器
        // 轴序/符号/单位错误使全部数值断言失义。
        {
            const double rollRate = 30.0 * 2.0 * kPi * 0.2 * kDeg;              // rad/s
            const double pitchRate = 20.0 * 2.0 * kPi * 0.35 * kDeg;            // rad/s
            const double yawRate = (5.0 + 15.0 * 2.0 * kPi * 0.1) * kDeg;       // rad/s
            const Quat q0 = truthAt(config, 0.0);
            const Quat q1 = truthAt(config, 1e-4);
            Quat delta = qMul(qConj(q0), q1);
            if (delta[0] < 0.0) {
                for (double& component : delta) {
                    component = -component;
                }
            }
            RIN_CHECK(std::fabs(2.0 * delta[1] / 1e-4 - rollRate) <= 1e-3);
            RIN_CHECK(std::fabs(2.0 * delta[2] / 1e-4 - pitchRate) <= 1e-3);
            RIN_CHECK(std::fabs(2.0 * delta[3] / 1e-4 - yawRate) <= 1e-3);
        }

        const ScenarioRun run = runScenario(samples, MahonyImuFuser::Params{});
        RIN_CHECK(run.hasPoseAfterFirstSample);
        const ErrorStats stats = tiltStats(samples, run, config, false);
        trackingRmsDeg = stats.rmsDeg;
        trackingPeakDeg = stats.peakDeg;
        traceNormDev = stats.maxNormSqDev;
        RIN_CHECK(trackingRmsDeg <= 0.5);  // DEC-010：RMS ≤0.5°
        RIN_CHECK(trackingPeakDeg <= 2.0);  // DEC-010：峰值 ≤2°
        RIN_CHECK(traceNormDev <= 1e-5);    // 全程单位范数

        const Quat truthEnd = truthAt(config, 60.0);
        yawKiOnDeg = yawErrorDeg(toDouble(run.trace.back()), truthEnd);
        RIN_CHECK(std::fabs(yawKiOnDeg) <=
                  std::fabs(kInjectedBiasDegS[2]) * 60.0);  // ≤ |b_z|·60s = 30°

        // Ki 关闭对照（DEC-010 决策 5 可注入参数）：Ki 开 < Ki 关。
        MahonyImuFuser::Params kiOff;
        kiOff.ki = 0.0;
        const ScenarioRun runKiOff = runScenario(samples, kiOff);
        yawKiOffDeg = yawErrorDeg(toDouble(runKiOff.trace.back()), truthEnd);
        RIN_CHECK(std::fabs(yawKiOnDeg) < std::fabs(yawKiOffDeg));
        std::printf(
            "tracking-60s: roll/pitch rms %.3f deg, peak %.3f deg, norm dev %.2e; "
            "yaw(60s) ki-on %.2f deg < ki-off %.2f deg (bound 30 deg)\n",
            trackingRmsDeg, trackingPeakDeg, traceNormDev, yawKiOnDeg, yawKiOffDeg);
    }

    // --- 7) DEC-010 判据 4：静态 60s 零偏估计残差（|b̂_x|、|b̂_y| ≤0.2°/s）---
    {
        ScenarioConfig config;
        config.sineTrajectory = false;
        const std::vector<MotionSample> samples = buildScenario(config);
        const ScenarioRun run = runScenario(samples, MahonyImuFuser::Params{});
        const double residualX = std::fabs(run.finalBias[0] - kInjectedBiasDegS[0] * kDeg);
        const double residualY = std::fabs(run.finalBias[1] - kInjectedBiasDegS[1] * kDeg);
        const double residualZ = std::fabs(run.finalBias[2] - kInjectedBiasDegS[2] * kDeg);
        RIN_CHECK(residualX <= 0.2 * kDeg);
        RIN_CHECK(residualY <= 0.2 * kDeg);
        // b_z 不可观（DEC-010）：仅记录，不设阈值。
        std::printf(
            "static-bias-60s: residual x %.5f deg/s, y %.5f deg/s, z(unobservable) "
            "%.5f deg/s\n",
            residualX / kDeg, residualY / kDeg, residualZ / kDeg);
    }

    // --- 8) DEC-010 判据 5：±2 m/s² 竖直平移突发（5s 通/5s 断）门限期间精度 ---
    {
        ScenarioConfig config;
        config.burst = true;
        const std::vector<MotionSample> samples = buildScenario(config);
        const ScenarioRun run = runScenario(samples, MahonyImuFuser::Params{});
        const ErrorStats gated = tiltStats(samples, run, config, true);
        RIN_CHECK(gated.count > 1000);  // 突发窗口确实被评估
        RIN_CHECK(gated.rmsDeg <= 0.5);  // DEC-010：门限期间 RMS ≤0.5°
        RIN_CHECK(gated.peakDeg <= 2.0);
        std::printf("gated-burst: %zu samples, rms %.3f deg, peak %.3f deg\n",
                    gated.count, gated.rmsDeg, gated.peakDeg);
    }

    // --- 9) DEC-010 判据 6/7：复位幂等（memcmp 逐位）与确定性 ---
    {
        const ScenarioConfig config;  // 60s 正弦 + 噪声（固定种子）
        const std::vector<MotionSample> samples = buildScenario(config);
        const std::size_t traceBytes =
            samples.size() * sizeof(std::array<float, 4>);

        const ScenarioRun fresh = runScenario(samples, MahonyImuFuser::Params{});
        const ScenarioRun fresh2 = runScenario(samples, MahonyImuFuser::Params{});
        RIN_CHECK(std::memcmp(fresh.trace.data(), fresh2.trace.data(), traceBytes) == 0);
        RIN_CHECK(std::memcmp(fresh.finalBias.data(), fresh2.finalBias.data(),
                              sizeof(double) * 3) == 0);

        // reset() 后重放与全新实例逐位一致。
        MahonyImuFuser fuser;
        for (const MotionSample& sample : samples) {
            fuser.advance(sample);
        }
        fuser.reset();
        RIN_CHECK(!fuser.hasPose());
        ScenarioRun replayed;
        replayed.trace.reserve(samples.size());
        for (const MotionSample& sample : samples) {
            fuser.advance(sample);
            replayed.trace.push_back(fuser.orientation());
        }
        replayed.finalBias = fuser.gyroBiasEstimate();
        RIN_CHECK(std::memcmp(fresh.trace.data(), replayed.trace.data(), traceBytes) ==
                  0);
        RIN_CHECK(std::memcmp(fresh.finalBias.data(), replayed.finalBias.data(),
                              sizeof(double) * 3) == 0);

        // 连续两次 reset() 后重放仍逐位一致。
        fuser.reset();
        fuser.reset();
        RIN_CHECK(!fuser.hasPose());
        ScenarioRun replayed2;
        replayed2.trace.reserve(samples.size());
        for (const MotionSample& sample : samples) {
            fuser.advance(sample);
            replayed2.trace.push_back(fuser.orientation());
        }
        RIN_CHECK(std::memcmp(fresh.trace.data(), replayed2.trace.data(),
                              traceBytes) == 0);

        // reset() 恢复注入初值（经基类引用多态路径）。
        MahonyImuFuser::Params params;
        params.initialOrientation = {0.0, 1.0, 0.0, 0.0};
        params.initialGyroBias = {0.5, 0.0, 0.0};
        MahonyImuFuser injectedFuser(params);
        rin::detail::ImuFuser& seam = injectedFuser;
        seam.advance(gyroSample(0.0, 1.0, 1.0, 1.0));
        seam.advance(accelSample(4.0, 0.0, 0.0, kGravity));
        RIN_CHECK(seam.hasPose());
        seam.reset();
        seam.reset();
        RIN_CHECK(!seam.hasPose());
        RIN_CHECK((injectedFuser.orientation() ==
                   std::array<float, 4>{0.0f, 1.0f, 0.0f, 0.0f}));
        const std::array<double, 3> bias = injectedFuser.gyroBiasEstimate();
        RIN_CHECK_EQ(bias[0], 0.5);
        RIN_CHECK_EQ(bias[1], 0.0);
        RIN_CHECK_EQ(bias[2], 0.0);
    }

    // --- 10) EXEC-06 热路径：advance 无堆分配（DEC-010 验证方式 8"实现以测试
    //          断言"）；耗时仅打印（1µs 阈值为 -O2 release 口径）---
    {
        MahonyImuFuser::Params params;
        MahonyImuFuser fuser(params);
        // 预热：完成对齐与参考点建立，使测量窗口覆盖传播/修正/守卫全部分支。
        fuser.advance(accelSample(0.0, 0.0, 0.0, kGravity));

        constexpr int kLoopSamples = 100000;
        long allocationCount = 0;
        double elapsedNsPerSample = 0.0;
        for (int pass = 0; pass < 2; ++pass) {
            const bool counting = pass == 0;
            g_counting = counting;
            g_newCount = 0;
            g_newBytes = 0;
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < kLoopSamples; ++i) {
                MotionSample sample;
                const double tMs = 1000.0 + static_cast<double>(i) * 2.5;
                if (i % 4 == 3) {
                    sample.kind = MotionStreamKind::Accel;
                    // 交替门限内/外：覆盖修正与跳过两分支。
                    sample.axes = (i % 8 == 7)
                                      ? std::array<float, 3>{0.0f, 0.0f,
                                                             2.0f * static_cast<float>(kGravity)}
                                      : std::array<float, 3>{0.196133f, 0.0f,
                                                             static_cast<float>(kGravity)};
                } else {
                    sample.kind = MotionStreamKind::Gyro;
                    sample.axes = {0.01f, -0.02f, 0.03f};
                }
                sample.deviceTimestampMs = tMs;
                fuser.advance(sample);
            }
            const auto stop = std::chrono::steady_clock::now();
            g_counting = false;
            if (counting) {
                allocationCount = static_cast<long>(g_newCount);
            } else {
                const double ns =
                    static_cast<double>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
                            .count()) /
                    static_cast<double>(kLoopSamples);
                elapsedNsPerSample = ns;
            }
        }
        RIN_CHECK_EQ(allocationCount, 0L);
        RIN_CHECK(std::fabs(normSq(fuser.orientation()) - 1.0) <= 1e-5);  // 热循环后仍稳定
        std::printf("hot-path: %d samples, heap allocations %ld, %.1f ns/sample "
                    "(debug build, informational; DEC-010 1us bound is a -O2 metric)\n",
                    kLoopSamples, allocationCount, elapsedNsPerSample);
    }

    // 数值证据汇总（人可读，供验证记录引用）。
    std::printf(
        "summary: static-align %.3f deg(4s, <0.5) after %.3f deg; tracking rms "
        "%.3f/peak %.3f deg (<=0.5/2); yaw ki-on %.2f < ki-off %.2f (<=30); "
        "initial error %.2f deg\n",
        staticTiltEndDeg, staticTiltAfterAlignDeg, trackingRmsDeg, trackingPeakDeg,
        yawKiOnDeg, yawKiOffDeg, initialTiltErrorDeg);

    return rin_test::exitStatus();
}
