#pragma once

// 脚本化假相机服务唯一实现（M12/CR-34，替代 test_run_control /
// test_workflow_depth_source 的两份手抄副本）：
// - 脚本面：publish()/publishGray()/publishMetric() 由测试线程同步发布；
//   rgbReads() 见证"未知节点回退 RGB"的读取次数。
// - 消费契约：tryLoad* 为"比 lastSeen 新"的最新态语义；stop() 返回后全部
//   数据通道不再有新发布（ICameraService::stop 排空契约的最小等价）。
// - 帧读取与引擎帧泵线程之间的共享经 mutex（帧源契约"非阻塞"指不等待帧
//   到达，mutex 短临界区满足）；不创建线程。
// - start()/request* 为契约桩：start 进入 Streaming（admitted），request*
//   受理返回 true（本 fake 不实现控制面语义）。

#include <rin/camera_service.hpp>
#include <rin/camera_types.hpp>

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace rin_test {

// 可继承（非 final）：个别用例需要特化单条通道语义（如
// test_policy_depth_output 的"直返无效米制采样"桩），其余方法保持继承复用。
class FakeCameraService : public rin::ICameraService {
public:
    // --- 测试脚本面（测试主线程同步调用） ---
    void publish(rin::Frame frame) {
        const std::scoped_lock lock(mutex_);
        latestRgba_[frame.kind] = std::move(frame);
    }

    void publishGray(rin::GrayFrameKind kind, rin::GrayFrame frame) {
        const std::scoped_lock lock(mutex_);
        latestGray_[kind] = std::move(frame);
    }

    void publishMetric(rin::DepthMetricSample sample) {
        const std::scoped_lock lock(mutex_);
        latestMetric_ = std::move(sample);
    }

    [[nodiscard]] int rgbReads() const {
        const std::scoped_lock lock(mutex_);
        return rgbReads_;
    }

    // --- ICameraService（相机源接缝只经 tryLoad*；其余为契约桩） ---
    rin::StartOutcome start(const rin::StreamRequest&) override {
        const std::scoped_lock lock(mutex_);
        state_ = rin::CameraServiceState::Streaming;
        rin::StartOutcome outcome;
        outcome.admitted = true;
        return outcome;
    }

    bool requestResolution(const rin::StreamRequest&, std::string*) override { return true; }

    bool requestDevice(const std::string&, std::string*) override { return true; }

    bool requestDepthColorScheme(rin::DepthColorScheme, std::string*) override { return true; }

    void stop() override {
        const std::scoped_lock lock(mutex_);
        stopped_ = true;
        state_ = rin::CameraServiceState::Idle;
        // 缓冲内容一并清空：stopped_ 已使 tryLoad* 恒 false，清空仅为不滞留
        // 陈旧缓冲（对消费方不可观察）。
        latestRgba_.clear();
        latestGray_.clear();
        latestMetric_.reset();
    }

    [[nodiscard]] rin::CameraServiceState state() const override {
        const std::scoped_lock lock(mutex_);
        return state_;
    }

    [[nodiscard]] std::string lastError() const override { return {}; }

    [[nodiscard]] bool tryLoadFrame(rin::FrameKind kind,
                                    std::uint64_t& lastSeenSequence,
                                    rin::Frame& out) override {
        const std::scoped_lock lock(mutex_);
        if (kind == rin::FrameKind::Rgb) {
            ++rgbReads_;
        }
        if (stopped_) {
            return false;  // 排空契约：stop 返回后不再有新发布。
        }
        const auto it = latestRgba_.find(kind);
        if (it == latestRgba_.end() || !it->second.valid() ||
            it->second.sequence <= lastSeenSequence) {
            return false;
        }
        out = it->second;
        lastSeenSequence = it->second.sequence;
        return true;
    }

    [[nodiscard]] bool tryLoadGrayFrame(rin::GrayFrameKind kind,
                                        std::uint64_t& lastSeenSequence,
                                        rin::GrayFrame& out) override {
        const std::scoped_lock lock(mutex_);
        if (stopped_) {
            return false;  // 排空契约：stop 返回后不再有新发布。
        }
        const auto it = latestGray_.find(kind);
        if (it == latestGray_.end() || !it->second.valid() ||
            it->second.sequence <= lastSeenSequence) {
            return false;
        }
        out = it->second;
        lastSeenSequence = it->second.sequence;
        return true;
    }

    [[nodiscard]] bool tryLoadDepthMetric(std::uint64_t& lastSeenSequence,
                                          rin::DepthMetricSample& out) override {
        const std::scoped_lock lock(mutex_);
        if (stopped_ || !latestMetric_.has_value() || !latestMetric_->valid() ||
            latestMetric_->sequence <= lastSeenSequence) {
            return false;
        }
        out = *latestMetric_;
        lastSeenSequence = out.sequence;
        return true;
    }

    [[nodiscard]] bool tryLoadIntrinsics(std::uint64_t&,
                                         rin::IntrinsicsSnapshot&) override {
        return false;
    }

    [[nodiscard]] bool tryLoadMotion(std::uint64_t&, rin::MotionSample&) override {
        return false;
    }

    [[nodiscard]] bool tryLoadPose(std::uint64_t&, rin::ImuSnapshot&) override {
        return false;
    }

    [[nodiscard]] bool tryLoadCatalog(std::uint64_t&, rin::DeviceCatalog&) override {
        return false;
    }

    [[nodiscard]] bool tryLoadEvent(rin::ServiceEvent&) override { return false; }

private:
    mutable std::mutex mutex_;
    std::map<rin::FrameKind, rin::Frame> latestRgba_;
    std::map<rin::GrayFrameKind, rin::GrayFrame> latestGray_;
    std::optional<rin::DepthMetricSample> latestMetric_;
    int rgbReads_ = 0;
    bool stopped_ = false;
    rin::CameraServiceState state_ = rin::CameraServiceState::Idle;
};

}  // namespace rin_test
