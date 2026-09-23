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
/// M3-04 以假实现（IdentityImuFuser）接入适配器发布链路，覆盖离线分支/命令语义；
/// M3-05 按 DEC-010 实现 Mahony 显式互补滤波真身并切换 createImuFuser() 的返回实现。
/// 边界刻意保持可替换（POST-05：不阻碍替换为外部位姿源）。
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
/// 仅用于发布链路接通与离线（无设备）分支/命令语义测试；真实姿态由 M3-05 提供。
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

/// 融合器工厂：当前返回 M3-04 假实现；M3-05 实现真身后在此切换默认返回值。
[[nodiscard]] std::unique_ptr<ImuFuser> createImuFuser();

}  // namespace rin::detail
