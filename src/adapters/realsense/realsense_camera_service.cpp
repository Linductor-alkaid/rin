#include "realsense_camera_service.hpp"

#include <librealsense2/rs.hpp>

#include <executor/comm/mailbox.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include "camera_state_machine.hpp"
#include "pixel_format.hpp"
#include "rs_vision/camera_types.hpp"

namespace rsv {
namespace {

using executor::comm::LatestMailbox;

constexpr auto kFrameWaitTimeout = std::chrono::milliseconds(1000);
/// StopToken 无法中断 wait_for_frames；连续超时达到该值才判 Failed（约 3 秒流失联）。
constexpr int kMaxConsecutiveFrameFailures = 3;
constexpr float kDepthNearMeters = 0.2f;
constexpr float kDepthFarMeters = 6.5f;  // DEC-003 暂定视觉区间

double steadyMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct ControlCommand {
    enum class Kind { Restream } kind = Kind::Restream;
    StreamRequest request;
};

}  // namespace

class RealSenseCamera;

/// Executor blocking worker：承载 librealsense 阻塞采集循环（EXEC-01）。
class CaptureLoop final : public executor::IBlockingIoWorker {
public:
    CaptureLoop(RealSenseCamera& owner, StreamRequest initialRequest)
        : owner_(owner), request_(initialRequest) {}

    void run(executor::StopToken stopToken) override;
    void wakeup() noexcept override {
        // wait_for_frames 带超时返回即到达取消/命令检查点；无需额外解除阻塞手段。
    }

private:
    void drainCommands(rs2::pipeline& pipeline);

    RealSenseCamera& owner_;
    StreamRequest request_;
    std::uint64_t lastCommandSequence_ = 0;
};

class RealSenseCamera final : public ICameraService {
public:
    explicit RealSenseCamera(executor::Executor& executor) : executor_(executor) {}

    RealSenseCamera(const RealSenseCamera&) = delete;
    RealSenseCamera& operator=(const RealSenseCamera&) = delete;

    StartOutcome start(const StreamRequest& request) override;
    bool requestResolution(const StreamRequest& request, std::string* error) override;
    void stop() override;

    [[nodiscard]] CameraServiceState state() const override { return machine_.state(); }
    [[nodiscard]] std::string lastError() const override {
        std::lock_guard<std::mutex> lock(errorMutex_);
        return lastError_;
    }

    [[nodiscard]] bool tryLoadFrame(FrameKind kind,
                                    std::uint64_t& lastSeenSequence,
                                    Frame& out) override {
        return frameMailbox(kind).try_load_newer_than(lastSeenSequence, out, lastSeenSequence);
    }

    [[nodiscard]] bool tryLoadIntrinsics(std::uint64_t& lastSeenSequence,
                                         IntrinsicsSnapshot& out) override {
        return intrinsics_.try_load_newer_than(lastSeenSequence, out, lastSeenSequence);
    }

    [[nodiscard]] bool tryLoadCapabilities(std::uint64_t& lastSeenSequence,
                                           StreamCapabilities& out) override {
        return capabilities_.try_load_newer_than(lastSeenSequence, out, lastSeenSequence);
    }

    [[nodiscard]] bool tryLoadEvent(ServiceEvent& out) override { return events_.try_load(out); }

    // --- 以下成员供 CaptureLoop 在 worker 线程使用（单 worker，无并发） ---
    [[nodiscard]] LatestMailbox<Frame>& rgbMailbox() { return rgbFrames_; }
    [[nodiscard]] LatestMailbox<Frame>& depthMailbox() { return depthFrames_; }
    [[nodiscard]] LatestMailbox<IntrinsicsSnapshot>& intrinsicsMailbox() { return intrinsics_; }
    [[nodiscard]] LatestMailbox<ServiceEvent>& eventMailbox() { return events_; }
    [[nodiscard]] LatestMailbox<ControlCommand>& commandMailbox() { return commands_; }

    [[nodiscard]] std::uint64_t nextSnapshotSequence() noexcept {
        return snapshotSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    void publishCapabilities(StreamCapabilities caps) { capabilities_.publish(std::move(caps)); }

    void publishEvent(ServiceEventKind kind, const std::string& message) {
        ServiceEvent event;
        event.kind = kind;
        event.state = machine_.state();
        event.message = message;
        event.timestampMs = steadyMs();
        events_.publish(std::move(event));
    }

    void setLastError(const std::string& message) {
        std::lock_guard<std::mutex> lock(errorMutex_);
        lastError_ = message;
    }

    /// worker 内推进状态。
    bool workerTransition(CameraServiceState next, const std::string& errorContext) {
        std::string reason;
        if (!machine_.transitionTo(next, &reason)) {
            setLastError(errorContext + ": " + reason);
            return false;
        }
        return true;
    }

    /// worker 退出时收敛终态；与 stop() 的消费标志互斥，只有一个方向生效。
    void finishWorker(bool failed, const std::string& message) {
        if (!workerRunning_.exchange(false)) {
            return;
        }
        if (failed) {
            setLastError(message);
            machine_.transitionTo(CameraServiceState::Failed);
            publishEvent(ServiceEventKind::Failed, message);
            return;
        }
        if (machine_.state() == CameraServiceState::Stopping) {
            machine_.transitionTo(CameraServiceState::Idle);
            publishEvent(ServiceEventKind::Stopped, message);
        }
    }

private:
    [[nodiscard]] LatestMailbox<Frame>& frameMailbox(FrameKind kind) {
        return kind == FrameKind::Rgb ? rgbFrames_ : depthFrames_;
    }

    void failFromCaller(const std::string& message) {
        setLastError(message);
        machine_.transitionTo(CameraServiceState::Failed);
        publishEvent(ServiceEventKind::Failed, message);
    }

    executor::Executor& executor_;
    detail::CameraStateMachine machine_;

    LatestMailbox<Frame> rgbFrames_{"rsv.frames.rgb"};
    LatestMailbox<Frame> depthFrames_{"rsv.frames.depth"};
    LatestMailbox<IntrinsicsSnapshot> intrinsics_{"rsv.intrinsics"};
    LatestMailbox<StreamCapabilities> capabilities_{"rsv.capabilities"};
    LatestMailbox<ServiceEvent> events_{"rsv.events"};
    LatestMailbox<ControlCommand> commands_{"rsv.commands"};

    executor::WorkerHandle worker_{};
    std::atomic<bool> workerRunning_{false};
    std::atomic<std::uint64_t> snapshotSequence_{0};

    mutable std::mutex errorMutex_;
    std::string lastError_;

    /// start()/stop() 生命周期决策互斥；worker 线程不持有该锁（无死锁路径）。
    std::mutex lifecycleMutex_;
};

namespace {

StreamCapabilities enumerateCapabilities(const rs2::device& device) {
    StreamCapabilities caps;
    caps.devicePresent = true;
    caps.deviceName = device.get_info(RS2_CAMERA_INFO_NAME);
    caps.serial = device.get_info(RS2_CAMERA_INFO_SERIAL_NUMBER);
    caps.firmwareVersion = device.get_info(RS2_CAMERA_INFO_FIRMWARE_VERSION);

    for (const rs2::sensor& sensor : device.query_sensors()) {
        std::vector<rs2::stream_profile> profiles;
        try {
            profiles = sensor.get_stream_profiles();
        } catch (const rs2::error&) {
            continue;  // 传感器不支持流枚举时跳过。
        }
        for (const rs2::stream_profile& profile : profiles) {
            if (profile.stream_type() == RS2_STREAM_COLOR &&
                profile.format() == RS2_FORMAT_RGB8) {
                const auto video = profile.as<rs2::video_stream_profile>();
                caps.colorOptions.push_back(
                    {static_cast<std::uint32_t>(video.width()),
                     static_cast<std::uint32_t>(video.height()),
                     static_cast<std::uint32_t>(video.fps())});
            } else if (profile.stream_type() == RS2_STREAM_DEPTH &&
                       profile.format() == RS2_FORMAT_Z16) {
                const auto video = profile.as<rs2::video_stream_profile>();
                caps.depthOptions.push_back(
                    {static_cast<std::uint32_t>(video.width()),
                     static_cast<std::uint32_t>(video.height()),
                     static_cast<std::uint32_t>(video.fps())});
            }
        }
    }
    return caps;
}

StreamIntrinsics readIntrinsics(const rs2::video_stream_profile& profile) {
    StreamIntrinsics intrinsics;
    if (!profile) {
        return intrinsics;
    }
    const rs2_intrinsics raw = profile.get_intrinsics();
    intrinsics.width = static_cast<std::uint32_t>(raw.width);
    intrinsics.height = static_cast<std::uint32_t>(raw.height);
    intrinsics.fx = raw.fx;
    intrinsics.fy = raw.fy;
    intrinsics.cx = raw.ppx;
    intrinsics.cy = raw.ppy;
    for (int index = 0; index < 5; ++index) {
        intrinsics.distortion[static_cast<std::size_t>(index)] = raw.coeffs[index];
    }
    switch (raw.model) {
        case RS2_DISTORTION_NONE:
            intrinsics.model = DistortionModel::None;
            break;
        case RS2_DISTORTION_MODIFIED_BROWN_CONRADY:
            intrinsics.model = DistortionModel::ModifiedBrownConrady;
            break;
        case RS2_DISTORTION_INVERSE_BROWN_CONRADY:
            intrinsics.model = DistortionModel::InverseBrownConrady;
            break;
        case RS2_DISTORTION_BROWN_CONRADY:
            intrinsics.model = DistortionModel::BrownConrady;
            break;
        case RS2_DISTORTION_FTHETA:
            intrinsics.model = DistortionModel::FTheta;
            break;
        case RS2_DISTORTION_KANNALA_BRANDT4:
            intrinsics.model = DistortionModel::KannalaBrandt4;
            break;
        default:
            intrinsics.model = DistortionModel::Unknown;
            break;
    }
    return intrinsics;
}

void publishFrame(LatestMailbox<Frame>& mailbox,
                  FrameKind kind,
                  std::uint32_t width,
                  std::uint32_t height,
                  std::uint32_t stride,
                  std::uint64_t sequence,
                  double timestampMs,
                  std::vector<std::uint8_t>&& pixels) {
    Frame frame;
    frame.kind = kind;
    frame.width = width;
    frame.height = height;
    frame.stride = stride;
    frame.sequence = sequence;
    frame.deviceTimestampMs = timestampMs;
    frame.pixels = std::make_shared<const std::vector<std::uint8_t>>(std::move(pixels));
    mailbox.publish(std::move(frame));
}

rs2::config buildConfig(const StreamRequest& request) {
    rs2::config config;
    config.enable_stream(RS2_STREAM_COLOR, -1, static_cast<int>(request.colorWidth),
                         static_cast<int>(request.colorHeight), RS2_FORMAT_RGB8,
                         static_cast<int>(request.colorFps));
    config.enable_stream(RS2_STREAM_DEPTH, -1, static_cast<int>(request.depthWidth),
                         static_cast<int>(request.depthHeight), RS2_FORMAT_Z16,
                         static_cast<int>(request.depthFps));
    return config;
}

IntrinsicsSnapshot snapshotIntrinsics(const rs2::pipeline_profile& profile,
                                      std::uint64_t sequence) {
    IntrinsicsSnapshot snapshot;
    snapshot.sequence = sequence;
    snapshot.color =
        readIntrinsics(profile.get_stream(RS2_STREAM_COLOR).as<rs2::video_stream_profile>());
    snapshot.depth =
        readIntrinsics(profile.get_stream(RS2_STREAM_DEPTH).as<rs2::video_stream_profile>());
    return snapshot;
}

}  // namespace

void CaptureLoop::run(executor::StopToken stopToken) {
    rs2::pipeline pipeline;
    bool pipelineRunning = false;
    bool failed = false;
    std::string failureMessage;

    try {
        rs2::context context;
        if (context.query_devices().size() == 0) {
            throw std::runtime_error("no RealSense device found");
        }

        const rs2::pipeline_profile profile = pipeline.start(buildConfig(request_));
        pipelineRunning = true;

        // rs2 get_depth_scale() 语义：每个深度单位对应的米数。
        float depthScale = 0.001f;
        try {
            depthScale = profile.get_device().first<rs2::depth_sensor>().get_depth_scale();
        } catch (const rs2::error&) {
            // 无深度传感器时兜底；深度帧将不可读，但不影响彩色流。
        }

        owner_.intrinsicsMailbox().publish(snapshotIntrinsics(profile, owner_.nextSnapshotSequence()));
        owner_.workerTransition(CameraServiceState::Streaming, "start");
        owner_.publishEvent(ServiceEventKind::Started, "streaming");

        std::uint64_t sequence = 0;
        int consecutiveFailures = 0;
        std::vector<std::uint8_t> rgba;

        while (!stopToken.stop_requested()) {
            drainCommands(pipeline);
            if (stopToken.stop_requested()) {
                break;
            }

            rs2::frameset frameset;
            try {
                frameset = pipeline.wait_for_frames(static_cast<unsigned int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(kFrameWaitTimeout)
                        .count()));
                consecutiveFailures = 0;
            } catch (const rs2::error&) {
                if (++consecutiveFailures >= kMaxConsecutiveFrameFailures) {
                    throw;
                }
                continue;
            }

            const rs2::video_frame color = frameset.get_color_frame();
            const rs2::depth_frame depth = frameset.get_depth_frame();
            if (!color || !depth) {
                continue;
            }
            ++sequence;

            const auto colorWidth = static_cast<std::uint32_t>(color.get_width());
            const auto colorHeight = static_cast<std::uint32_t>(color.get_height());
            const auto colorStride = static_cast<std::uint32_t>(color.get_stride_in_bytes());
            if (convertRgb8ToRgba8(reinterpret_cast<const std::uint8_t*>(color.get_data()),
                                   colorWidth, colorHeight, colorStride, rgba)) {
                publishFrame(owner_.rgbMailbox(), FrameKind::Rgb, colorWidth, colorHeight,
                             colorWidth * 4u, sequence, color.get_timestamp(), std::move(rgba));
            }

            const auto depthWidth = static_cast<std::uint32_t>(depth.get_width());
            const auto depthHeight = static_cast<std::uint32_t>(depth.get_height());
            const auto depthStrideUnits =
                static_cast<std::uint32_t>(depth.get_stride_in_bytes() / sizeof(std::uint16_t));
            if (convertDepth16ToRgba8Jet(reinterpret_cast<const std::uint16_t*>(depth.get_data()),
                                         depthWidth, depthHeight, depthStrideUnits, depthScale,
                                         kDepthNearMeters, kDepthFarMeters, rgba)) {
                publishFrame(owner_.depthMailbox(), FrameKind::Depth, depthWidth, depthHeight,
                             depthWidth * 4u, sequence, depth.get_timestamp(), std::move(rgba));
            }
        }
    } catch (const std::exception& error) {
        failed = true;
        failureMessage = error.what();
    }

    if (pipelineRunning) {
        try {
            pipeline.stop();
        } catch (const rs2::error&) {
            // 停止失败不掩盖主失败原因；资源由 rs2 RAII 兜底。
        }
    }
    owner_.finishWorker(failed,
                        failed ? failureMessage : std::string("capture loop stopped"));
}

void CaptureLoop::drainCommands(rs2::pipeline& pipeline) {
    ControlCommand command;
    std::uint64_t newSequence = lastCommandSequence_;
    if (!owner_.commandMailbox().try_load_newer_than(lastCommandSequence_, command,
                                                     newSequence)) {
        return;
    }
    lastCommandSequence_ = newSequence;
    if (command.request == request_) {
        return;  // 同配置请求（含 UI 重复下发）直接忽略。
    }

    owner_.workerTransition(CameraServiceState::Restreaming, "restream");
    owner_.publishEvent(ServiceEventKind::Info, "restreaming");
    pipeline.stop();
    const rs2::pipeline_profile profile = pipeline.start(buildConfig(command.request));
    request_ = command.request;
    owner_.intrinsicsMailbox().publish(snapshotIntrinsics(profile, owner_.nextSnapshotSequence()));
    owner_.workerTransition(CameraServiceState::Streaming, "restream finish");
    owner_.publishEvent(ServiceEventKind::ResolutionChanged, "resolution applied");
}

StartOutcome RealSenseCamera::start(const StreamRequest& request) {
    StartOutcome outcome;
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    if (machine_.state() != CameraServiceState::Idle) {
        outcome.error = "cannot start from state " + std::string(toString(machine_.state()));
        return outcome;
    }
    std::string reason;
    if (!machine_.transitionTo(CameraServiceState::Opening, &reason)) {
        outcome.error = reason;
        return outcome;
    }

    try {
        rs2::context context;
        const rs2::device_list devices = context.query_devices();
        if (devices.size() == 0) {
            throw std::runtime_error("no RealSense device found");
        }
        publishCapabilities(enumerateCapabilities(devices.front()));
    } catch (const std::exception& error) {
        const std::string message = error.what();
        failFromCaller(message);
        outcome.error = message;
        return outcome;
    }

    executor::BlockingWorkerSpec spec;
    spec.name = "rsv-capture";
    spec.config.thread_name = "rsv-capture";
    spec.config.startup_timeout = std::chrono::milliseconds(2000);
    spec.worker = std::make_unique<CaptureLoop>(*this, request);
    worker_ = executor_.start_worker(std::move(spec));
    if (!worker_.started()) {
        const std::string message = "capture worker admission failed: " +
                                    worker_.start_result().message;
        failFromCaller(message);
        outcome.error = message;
        return outcome;
    }
    workerRunning_.store(true);

    outcome.admitted = true;
    return outcome;
}

bool RealSenseCamera::requestResolution(const StreamRequest& request, std::string* error) {
    const CameraServiceState current = machine_.state();
    if (current != CameraServiceState::Streaming && current != CameraServiceState::Restreaming) {
        if (error != nullptr) {
            *error = "cannot change resolution from state " + std::string(toString(current));
        }
        return false;
    }
    ControlCommand command;
    command.kind = ControlCommand::Kind::Restream;
    command.request = request;
    if (!commands_.try_publish(command)) {
        if (error != nullptr) {
            *error = "command mailbox rejected request";
        }
        return false;
    }
    // WorkerHandle 无非停止性外部唤醒通道（wakeup 仅绑定 request_stop，见
    // docs/executor_feedback/ledger.md 已核对非缺口事项）；命令在 wait_for_frames
    // 有界超时（≤1s）后的检查点被拾取。
    return true;
}

void RealSenseCamera::stop() {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    if (machine_.state() == CameraServiceState::Idle) {
        return;  // 幂等快路径。
    }
    machine_.transitionTo(CameraServiceState::Stopping);
    if (workerRunning_.exchange(false)) {
        worker_.request_stop();
        worker_.stop();  // join：消费退出标志，终态由此处收敛（EXEC-01/EXEC-04）。
    }
    if (machine_.state() == CameraServiceState::Stopping) {
        machine_.transitionTo(CameraServiceState::Idle);
        publishEvent(ServiceEventKind::Stopped, "service stopped");
    }
}

std::shared_ptr<ICameraService> createRealSenseCameraService(executor::Executor& executor) {
    return std::make_shared<RealSenseCamera>(executor);
}

}  // namespace rsv
