#include "imu_motion_ingest.hpp"

#include <cmath>

namespace rin::detail {
namespace {

/// 滑动频率 EMA 系数：新样本 dt 权重（约 10 个样本收敛到稳态，400Hz 下 ~25ms）。
constexpr double kMotionRateEmaAlpha = 0.1;
/// 超过该 dt（毫秒）视为流中断/重开，重建频率估计窗口而非继续平滑旧值。
constexpr double kMotionRateGapResetMs = 1000.0;

}  // namespace

MotionIngest::MotionIngest(executor::comm::LatestMailbox<MotionSample>& motionMailbox,
                           executor::comm::LatestMailbox<ImuSnapshot>& poseMailbox,
                           std::unique_ptr<ImuFuser> fuser) noexcept
    : motionMailbox_(motionMailbox), poseMailbox_(poseMailbox), fuser_(std::move(fuser)) {}

void MotionIngest::resetStreamState() noexcept {
    fuser_->reset();
    gyroLastTsMs_ = -1.0;
    accelLastTsMs_ = -1.0;
    gyroEmaDtMs_ = 0.0;
    accelEmaDtMs_ = 0.0;
}

MotionSourceStats MotionIngest::stats() const noexcept {
    MotionSourceStats out;
    out.gyroHz = gyroEmaDtMs_ > 0.0 ? static_cast<float>(1000.0 / gyroEmaDtMs_) : 0.0f;
    out.accelHz = accelEmaDtMs_ > 0.0 ? static_cast<float>(1000.0 / accelEmaDtMs_) : 0.0f;
    out.gyroSamples = gyroSamples_;
    out.accelSamples = accelSamples_;
    return out;
}

void MotionIngest::updateRate(bool gyro, double timestampMs) noexcept {
    double& lastTsMs = gyro ? gyroLastTsMs_ : accelLastTsMs_;
    double& emaDtMs = gyro ? gyroEmaDtMs_ : accelEmaDtMs_;
    if (!std::isfinite(timestampMs) || timestampMs < 0.0) {
        return;  // 设备时间戳异常：只计数，不污染频率估计。
    }
    if (lastTsMs < 0.0) {
        lastTsMs = timestampMs;  // 该源首个样本：建立参考点。
        return;
    }
    const double dt = timestampMs - lastTsMs;
    if (dt <= 0.0) {
        return;  // 重复/乱序时间戳：保持参考点与估计不变。
    }
    lastTsMs = timestampMs;
    if (dt >= kMotionRateGapResetMs) {
        emaDtMs = 0.0;  // 长间隙：旧频率作废，下个样本重建窗口。
        return;
    }
    emaDtMs = emaDtMs > 0.0
                  ? kMotionRateEmaAlpha * dt + (1.0 - kMotionRateEmaAlpha) * emaDtMs
                  : dt;
}

void MotionIngest::ingest(const MotionSample& sample) {
    // EXEC-06 有界校验：损坏的采样在分支入口丢弃，不进入融合器与邮箱。
    if (!sample.valid()) {
        return;
    }

    const bool gyro = sample.kind == MotionStreamKind::Gyro;
    updateRate(gyro, sample.deviceTimestampMs);
    if (gyro) {
        ++gyroSamples_;
    } else {
        ++accelSamples_;
    }

    // 原始采样投递（ACCEL/GYRO 共用一条运动通道，最新态语义；槽被读者钉住时
    // 丢弃当前样本——与邮箱覆盖同属有界丢弃策略，经 comm 统计可观察）。
    MotionSample published = sample;
    published.sequence = ++motionSequence_;
    (void)motionMailbox_.try_publish(published);

    // 融合推进（Core 纯逻辑，单 worker 独占；无堆分配）。
    fuser_->advance(sample);

    // 姿态可用即随样本发布快照（姿态 + 实测统计 + 姿态通道序号；消费方按最新态取用）。
    if (fuser_->hasPose()) {
        ImuSnapshot snapshot;
        snapshot.orientation = fuser_->orientation();
        snapshot.sources = stats();
        snapshot.sequence = ++poseSequence_;
        (void)poseMailbox_.try_publish(snapshot);
    }
}

}  // namespace rin::detail
