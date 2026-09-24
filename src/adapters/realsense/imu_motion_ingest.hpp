#pragma once

#include <cstdint>
#include <memory>

#include <executor/comm/mailbox.hpp>

#include "imu_fuser.hpp"
#include "rin/camera_types.hpp"

namespace rin::detail {

/// 采集 blocking worker 内的 motion 帧轻量分支（总计划 EXEC-06）：
/// 有界校验（MotionSample::valid）→ 原始采样最新态投递 → ImuFuser 按样本推进 →
/// 姿态可用时组装 ImuSnapshot（姿态 + 实测源频率统计 + 通道序号）投递。
/// 热路径无堆分配；投递使用 LatestMailbox::try_publish（无锁、不等待，最新态语义
/// 下覆盖/丢弃即既有背压策略，经 comm 统计可观察）。
///
/// 线程模型：单采集 worker 独占调用 ingest/resetStreamState；tryLoad* 由 UI 线程
/// 经邮箱"上次已见序号"语义读取，与 worker 无共享可变状态。
///
/// 序号与统计语义（M3-03 契约）：motion/pose 通道序号自会话（上次 start）起单调
/// 递增，跨 restream / 设备切换保持连续——resetStreamState() 只复位融合器与频率
/// 估计窗口，不动序号与会话累计样本数（消费方 lastSeen 序号不回退，邮箱 stale 读
/// 语义不失效）。无 IMU 设备上运动流退化为不配置（本类不被驱动），通道保持空。
class MotionIngest {
public:
    /// 不持有邮箱所有权（邮箱由服务对象持有，寿命覆盖本类）；融合器独占。
    MotionIngest(executor::comm::LatestMailbox<MotionSample>& motionMailbox,
                 executor::comm::LatestMailbox<ImuSnapshot>& poseMailbox,
                 std::unique_ptr<ImuFuser> fuser) noexcept;

    /// 推进单个已到达的运动采样；无效采样（含损坏枚举/非有限轴值）直接丢弃，
    /// 不投递、不推进融合器、不计入统计。
    void ingest(const MotionSample& sample);

    /// 流重建（首次打开 / restream / 设备切换 / 热插拔重开）时调用：融合器复位
    /// （姿态回到不可用，重新收敛）+ 各源频率估计窗口清零。幂等。
    void resetStreamState() noexcept;

    /// 当前实测源频率统计（EMA 滑动频率 + 会话累计样本数）。
    [[nodiscard]] MotionSourceStats stats() const noexcept;

    /// 姿态是否可用（透传融合器；假实现为"见过首个有效采样"）。
    [[nodiscard]] bool hasPose() const noexcept { return fuser_->hasPose(); }

private:
    /// 更新单一源的 EMA 频率估计（dt 取相邻设备时间戳差；dt 非正不污染估计，
    /// 超过窗口阈值视为流中断重建窗口）。
    void updateRate(bool gyro, double timestampMs) noexcept;

    executor::comm::LatestMailbox<MotionSample>& motionMailbox_;
    executor::comm::LatestMailbox<ImuSnapshot>& poseMailbox_;
    std::unique_ptr<ImuFuser> fuser_;

    std::uint64_t motionSequence_ = 0;  /// 运动通道会话序号（单调递增）
    std::uint64_t poseSequence_ = 0;    /// 姿态通道会话序号（单调递增）

    /// EMA 频率估计状态：lastTs < 0 表示该源尚未见样本；emaDt <= 0 表示窗口
    /// 重建中（频率记 0）。
    double gyroLastTsMs_ = -1.0;
    double accelLastTsMs_ = -1.0;
    double gyroEmaDtMs_ = 0.0;
    double accelEmaDtMs_ = 0.0;

    /// 会话累计样本数（跨 resetStreamState 保留，MotionSourceStats 契约）。
    std::uint64_t gyroSamples_ = 0;
    std::uint64_t accelSamples_ = 0;
};

}  // namespace rin::detail
