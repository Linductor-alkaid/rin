#pragma once

#include <array>
#include <memory>

#include "rin/camera_types.hpp"

namespace rin::detail {

/// 六轴 IMU 姿态融合器契约（DEC-010）：Core 纯逻辑，按样本推进，热路径无堆分配
/// （总计划 EXEC-06）；公开面只使用 std 契约类型（RULE-01）。
///
/// 方向约定沿用 M3-03 锁定的契约：姿态四元数为 传感器系 → 世界系（标量在前 w,x,y,z，
/// 单位化；世界系 Z 轴向上，yaw 初值 0 且六轴不可观）。实现内部维护各源时间戳单调性
/// 与 dt（DEC-010：GYRO 传播取相邻设备时间戳差）；调用方保证传入 valid() 的采样。
///
/// M3-05 按 DEC-010 交付 Mahony 显式互补滤波真身（MahonyImuFuser）并经
/// createImuFuser() 工厂提供默认实例。边界刻意保持可替换（POST-05：不阻碍替换为
/// 外部位姿源）。
class ImuFuser {
public:
    virtual ~ImuFuser() = default;
    ImuFuser(const ImuFuser&) = delete;
    ImuFuser& operator=(const ImuFuser&) = delete;

    /// 推进一个采样（调用方保证 sample.valid()）；kind 决定该样本驱动陀螺传播还是
    /// 重力修正。非阻塞、无堆分配。
    virtual void advance(const MotionSample& sample) = 0;

    /// 姿态是否已可用（首个有效采样推进后为 true；reset() 后回到 false）。
    [[nodiscard]] virtual bool hasPose() const noexcept = 0;

    /// 当前姿态单位四元数（w,x,y,z 标量在前，传感器系→世界系）。
    [[nodiscard]] virtual std::array<float, 4> orientation() const noexcept = 0;

    /// 复位到构造初态；幂等。restream / 设备切换重建流时由适配器调用。
    virtual void reset() noexcept = 0;

protected:
    ImuFuser() = default;
};

/// M3-04 假实现：不做真正融合——首个 advance 后姿态可用且恒为恒等四元数 {1,0,0,0}。
/// 保留用于离线（无设备）接缝/分支语义对照与融合数值测试的恒等参照；生产默认
/// 实现为 MahonyImuFuser。
class IdentityImuFuser final : public ImuFuser {
public:
    IdentityImuFuser() = default;

    void advance(const MotionSample& sample) override;
    [[nodiscard]] bool hasPose() const noexcept override { return hasPose_; }
    [[nodiscard]] std::array<float, 4> orientation() const noexcept override;
    void reset() noexcept override;

private:
    bool hasPose_ = false;
};

/// Mahony 显式互补滤波（DEC-010 冻结算法，Mahony et al. 2008）：四元数姿态 + 陀螺
/// 零偏在线估计（PI 反馈），按样本推进，双精度内部状态，热路径无堆分配。
///
/// 推进语义（DEC-010 交由本实现锁定的离散化，经典 Mahony 显式互补滤波约定）：
/// - GYRO 样本：四元数传播。dt 取相邻 GYRO 设备时间戳差；首个样本与 dt <= 0
///   （重复/乱序）不传播，只维持单调性守卫；时间戳非有限（valid() 不约束时间戳域）
///   的样本整体忽略。传播用 SO(3) 指数映射（任意 dt 下保持单位范数），
///   角速度先扣除零偏估计。
/// - ACCEL 样本：重力方向修正（P）与零偏更新（I）。首个通过 1g 门限的有效 ACCEL
///   样本做一次性重力对齐（roll/pitch 对齐测量方向，yaw 置 0，DEC-010 初值语义；
///   门限外的首个样本视为污染，不参与对齐）；其后每个 1g 门限内
///   （|‖a‖ − 1g| <= 10%，g = 9.80665 m/s²）的样本执行一步修正：
///   误差 e = â × ĝ（测量/估计重力方向叉积），零偏估计 b̂ -= 2·Ki·e·dt，
///   姿态按 e 以角速率 2·Kp·e 右乘指数映射（2 倍系数为经典 Mahony 离散化约定，
///   默认 Kp=1.0 / Ki=0.1 即 DEC-010 冻结组合）。门限外样本跳过修正与零偏更新
///   （污染防护，roll/pitch 由零偏估计兜底）；修正 dt 取相邻 ACCEL 设备时间戳差，
///   与门限跳过无关地按样本推进参考点。
/// - 状态在单采集 worker 内独占推进（EXEC-06），无内部同步；reset() 恢复构造
///   初态且幂等。
class MahonyImuFuser final : public ImuFuser {
public:
    /// 可注入初值与参数（DEC-010）：默认值即冻结的验收组合。负增益按调用方语义
    /// 原样使用（纯逻辑不做隐藏钳制）；非有限/零范数初值消毒为恒等/零。
    struct Params {
        /// 比例修正增益（1/s）；修正角速率 = 2·Kp·e。
        double kp = 1.0;
        /// 积分（零偏跟踪）增益（1/s）；零偏更新 = -2·Ki·e·dt。
        double ki = 0.1;
        /// 初始姿态四元数（w,x,y,z 标量在前，传感器系→世界系；非单位则归一化，
        /// 零范数/非有限回退恒等）。
        std::array<double, 4> initialOrientation{1.0, 0.0, 0.0, 0.0};
        /// 初始陀螺零偏估计（rad/s；非有限分量归零）。
        std::array<double, 3> initialGyroBias{0.0, 0.0, 0.0};
    };

    MahonyImuFuser() : MahonyImuFuser(Params{}) {}
    explicit MahonyImuFuser(const Params& params);

    void advance(const MotionSample& sample) override;
    [[nodiscard]] bool hasPose() const noexcept override { return hasPose_; }
    [[nodiscard]] std::array<float, 4> orientation() const noexcept override;
    void reset() noexcept override;

    /// 当前陀螺零偏估计（rad/s）。DEC-010 数值验收（零偏收敛判据）的观测面；
    /// 仅具体类型暴露，多态接缝保持 POST-05 可替换面最小。
    [[nodiscard]] std::array<double, 3> gyroBiasEstimate() const noexcept;

private:
    /// 恢复构造初态（构造与 reset() 共用；对注入初值做一次性消毒）。
    void restoreInitialState() noexcept;
    /// 推进单个有效 GYRO 采样：零偏补偿后的指数映射传播（含单调性守卫）。
    void advanceGyro(const MotionSample& sample) noexcept;
    /// 推进单个有效 ACCEL 采样：一次性对齐或一步 PI 修正。
    void advanceAccel(const MotionSample& sample) noexcept;

    Params params_;
    std::array<double, 4> q_;      /// 姿态（w,x,y,z，传感器系→世界系），始终单位化
    std::array<double, 3> bias_;   /// 陀螺零偏估计（rad/s）
    bool hasPose_ = false;
    bool aligned_ = false;         /// 已完成首帧 ACCEL 重力对齐
    bool hasGyroTs_ = false;       /// GYRO 时间戳参考点是否建立
    double gyroTsMs_ = 0.0;
    bool hasAccelTs_ = false;      /// ACCEL 修正 dt 参考点是否建立
    double accelTsMs_ = 0.0;
};

/// 融合器工厂：返回 DEC-010 默认参数的 MahonyImuFuser（M3-05 起的生产实现；
/// IdentityImuFuser 假实现保留用于离线对照）。
[[nodiscard]] std::unique_ptr<ImuFuser> createImuFuser();

}  // namespace rin::detail
