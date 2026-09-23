#include "realsense_camera_service.hpp"

#include <librealsense2/rs.hpp>

#include <executor/comm/mailbox.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "camera_state_machine.hpp"
#include "pixel_format.hpp"
#include "rs_vision/camera_types.hpp"

namespace rsv {
namespace {

using executor::comm::LatestMailbox;

constexpr auto kFrameWaitTimeout = std::chrono::milliseconds(1000);
/// StopToken 无法中断 wait_for_frames；连续超时达到该值才判设备丢失/失败（约 3 秒）。
constexpr int kMaxConsecutiveFrameFailures = 3;
/// 设备在线但打开连续失败达到该值才判 Failed（DEC-006：真实错误保持可见）。
constexpr int kMaxConsecutiveOpenFailures = 3;
/// Waiting 态与打开失败退避的轮询周期（热插拔信号/命令/停止的检查点）。
constexpr auto kWaitingPollInterval = std::chrono::milliseconds(300);
constexpr float kDepthNearMeters = 0.2f;
constexpr float kDepthFarMeters = 6.5f;  // DEC-003 暂定视觉区间

double steadyMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct ControlCommand {
    enum class Kind { Restream, SelectDevice } kind = Kind::Restream;
    StreamRequest request;
    std::string serial;
};

/// 热插拔监视桥（DEC-006）：librealsense 回调线程只做 alive 检查 + 有界投递
/// （AGENTS.md 规则 11）；析构方在互斥下置 alive=false，杜绝析构窗口悬挂。
struct HotPlugBridge {
    explicit HotPlugBridge(LatestMailbox<int>& mailboxRef) : mailbox(mailboxRef) {}

    LatestMailbox<int>& mailbox;
    std::mutex mutex;
    bool alive = true;
};

}  // namespace

class RealSenseCamera;

/// Executor blocking worker：设备解析（Waiting）、打开（有界重试）与采集循环（EXEC-01）。
class CaptureLoop final : public executor::IBlockingIoWorker {
public:
    CaptureLoop(RealSenseCamera& owner, StreamRequest initialRequest)
        : owner_(owner), request_(initialRequest) {}

    void run(executor::StopToken stopToken) override;
    void wakeup() noexcept override {
        // wait_for_frames / Waiting 轮询均带界返回即到达取消/命令检查点。
    }

private:
    enum class StreamExit { Stopped, DeviceLost, Fatal };

    /// 解析目标设备（requested 优先 > 唯一自动 > 未选即 Waiting）；空 = 停止或失败。
    std::string resolveTarget(rs2::context& context, executor::StopToken stopToken);
    /// Streaming 域：绑定 serial 出流，处理命令/热插拔/设备移除。
    StreamExit streamLoop(rs2::context& context,
                          rs2::pipeline& pipeline,
                          rs2::pipeline_profile& profile,
                          const std::string& serial,
                          executor::StopToken stopToken);
    /// 枚举 + 选择策略发布目录（requested 在线优先 > 唯一自动 > 未选）。
    void refreshCatalog(rs2::context& context);
    [[nodiscard]] bool serialOnline(const rs2::context& context, const std::string& serial);
    [[nodiscard]] bool sleepPoll(executor::StopToken stopToken);

    RealSenseCamera& owner_;
    StreamRequest request_;
    std::string requestedSerial_;  // 用户选择意图（粘性，跨插拔保留）
    std::string activeSerial_;     // 当前流送设备
    StreamRequest pendingResolution_;  // 最近一次待应用的分辨率请求
    std::string errorMessage_;     // worker 内最近一次失败描述
    std::uint64_t lastCommandSequence_ = 0;
    std::uint64_t lastHotPlugSequence_ = 0;
    int openFailures_ = 0;
    DeviceCatalog lastCatalog_;
};

class RealSenseCamera final : public ICameraService {
public:
    explicit RealSenseCamera(executor::Executor& executor) : executor_(executor) {
        // 热插拔监视（DEC-006）：长驻 context；回调（librealsense 线程）只做 alive
        // 检查 + 邮箱投递（AGENTS.md 规则 11），枚举与状态推进在采集 worker 完成。
        context_ = std::make_unique<rs2::context>();
        bridge_ = std::make_shared<HotPlugBridge>(hotPlug_);
        std::weak_ptr<HotPlugBridge> weak = bridge_;
        context_->set_devices_changed_callback([weak](const rs2::event_information&) {
            if (auto bridge = weak.lock()) {
                std::lock_guard<std::mutex> lock(bridge->mutex);
                if (bridge->alive) {
                    (void)bridge->mailbox.try_publish(0);
                }
            }
        });
    }

    ~RealSenseCamera() override {
        if (bridge_ != nullptr) {
            std::lock_guard<std::mutex> lock(bridge_->mutex);
            bridge_->alive = false;  // 在互斥下停用回调投递，之后成员方可析构。
        }
        // context_ 析构时注销回调；worker 生命周期已由 stop() 先行收敛。
    }

    RealSenseCamera(const RealSenseCamera&) = delete;
    RealSenseCamera& operator=(const RealSenseCamera&) = delete;

    StartOutcome start(const StreamRequest& request) override;
    bool requestResolution(const StreamRequest& request, std::string* error) override;
    bool requestDevice(const std::string& serial, std::string* error) override;
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

    [[nodiscard]] bool tryLoadCatalog(std::uint64_t& lastSeenSequence,
                                      DeviceCatalog& out) override {
        return catalog_.try_load_newer_than(lastSeenSequence, out, lastSeenSequence);
    }

    [[nodiscard]] bool tryLoadEvent(ServiceEvent& out) override { return events_.try_load(out); }

    // --- 以下成员供 CaptureLoop 在 worker 线程使用（单 worker，无并发） ---
    [[nodiscard]] LatestMailbox<Frame>& rgbMailbox() { return rgbFrames_; }
    [[nodiscard]] LatestMailbox<Frame>& depthMailbox() { return depthFrames_; }
    [[nodiscard]] LatestMailbox<IntrinsicsSnapshot>& intrinsicsMailbox() { return intrinsics_; }
    [[nodiscard]] LatestMailbox<DeviceCatalog>& catalogMailbox() { return catalog_; }
    [[nodiscard]] LatestMailbox<ServiceEvent>& eventMailbox() { return events_; }
    [[nodiscard]] LatestMailbox<ControlCommand>& commandMailbox() { return commands_; }
    [[nodiscard]] LatestMailbox<int>& hotPlugMailbox() { return hotPlug_; }

    [[nodiscard]] std::uint64_t nextSnapshotSequence() noexcept {
        return snapshotSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    void publishCatalog(DeviceCatalog catalog) { catalog_.publish(std::move(catalog)); }

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
    LatestMailbox<DeviceCatalog> catalog_{"rsv.catalog"};
    LatestMailbox<ServiceEvent> events_{"rsv.events"};
    LatestMailbox<ControlCommand> commands_{"rsv.commands"};
    LatestMailbox<int> hotPlug_{"rsv.hotplug"};

    executor::WorkerHandle worker_{};
    std::atomic<bool> workerRunning_{false};
    std::atomic<std::uint64_t> snapshotSequence_{0};

    mutable std::mutex errorMutex_;
    std::string lastError_;

    /// start()/stop() 生命周期决策互斥；worker 线程不持有该锁（无死锁路径）。
    std::mutex lifecycleMutex_;

    /// 热插拔监视桥：shared_ptr 与回调解耦生命周期；context_ 析构注销回调。
    std::shared_ptr<HotPlugBridge> bridge_;
    std::unique_ptr<rs2::context> context_;
};

namespace {

/// 枚举单台设备的名称/序列号/固件与 RGB8/Z16 分辨率档位。
DeviceInfo enumerateDevice(const rs2::device& device) {
    DeviceInfo info;
    info.name = device.get_info(RS2_CAMERA_INFO_NAME);
    info.serial = device.get_info(RS2_CAMERA_INFO_SERIAL_NUMBER);
    info.firmwareVersion = device.get_info(RS2_CAMERA_INFO_FIRMWARE_VERSION);

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
                info.colorOptions.push_back(
                    {static_cast<std::uint32_t>(video.width()),
                     static_cast<std::uint32_t>(video.height()),
                     static_cast<std::uint32_t>(video.fps())});
            } else if (profile.stream_type() == RS2_STREAM_DEPTH &&
                       profile.format() == RS2_FORMAT_Z16) {
                const auto video = profile.as<rs2::video_stream_profile>();
                info.depthOptions.push_back(
                    {static_cast<std::uint32_t>(video.width()),
                     static_cast<std::uint32_t>(video.height()),
                     static_cast<std::uint32_t>(video.fps())});
            }
        }
    }
    return info;
}

/// 枚举全部在线设备（仅含同时支持 RGB8 与 Z16 的设备，与 M1 双流契约一致）。
std::vector<DeviceInfo> enumerateDevices(const rs2::context& context) {
    std::vector<DeviceInfo> devices;
    for (const rs2::device& device : context.query_devices()) {
        DeviceInfo info = enumerateDevice(device);
        if (!info.colorOptions.empty() && !info.depthOptions.empty()) {
            devices.push_back(std::move(info));
        }
    }
    return devices;
}

[[nodiscard]] bool serialInDevices(const std::vector<DeviceInfo>& devices,
                                   const std::string& serial) {
    for (const DeviceInfo& info : devices) {
        if (info.serial == serial) {
            return true;
        }
    }
    return false;
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

rs2::config buildConfig(const StreamRequest& request, const std::string& serial) {
    rs2::config config;
    if (!serial.empty()) {
        config.enable_device(serial);
    }
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

/// 枚举 + 选择策略发布目录：requested 在线优先 > 唯一设备自动 > 未选（DEC-006）。
void CaptureLoop::refreshCatalog(rs2::context& context) {
    DeviceCatalog catalog;
    catalog.devices = enumerateDevices(context);
    if (!requestedSerial_.empty() && serialInDevices(catalog.devices, requestedSerial_)) {
        catalog.activeSerial = requestedSerial_;
        catalog.activeIsAuto = false;
    } else if (catalog.devices.size() == 1) {
        catalog.activeSerial = catalog.devices.front().serial;
        catalog.activeIsAuto = true;
    }
    lastCatalog_ = catalog;
    owner_.publishCatalog(std::move(catalog));
}

/// 解析目标设备（Waiting 域）：空 = 停止或致命失败；非空 = 已选定可尝试打开。
std::string CaptureLoop::resolveTarget(rs2::context& context, executor::StopToken stopToken) {
    std::string serial;
    for (;;) {
        refreshCatalog(context);
        const DeviceCatalog& catalog = lastCatalog_;
        serial = catalog.activeSerial;

        if (!serial.empty()) {
            if (!owner_.workerTransition(CameraServiceState::Opening, "device resolved")) {
                return {};
            }
            return serial;
        }

        owner_.workerTransition(CameraServiceState::Waiting, "waiting");
        owner_.publishEvent(ServiceEventKind::Info,
                            requestedSerial_.empty()
                                ? (catalog.devices.empty() ? "waiting for device"
                                                           : "multiple devices, select one")
                                : "selected device offline: " + requestedSerial_);

        // 有界轮询：命令（用户选择）+ 热插拔信号 + 停止。
        while (!stopToken.stop_requested()) {
            ControlCommand command;
            std::uint64_t newSequence = lastCommandSequence_;
            if (owner_.commandMailbox().try_load_newer_than(lastCommandSequence_, command,
                                                            newSequence)) {
                lastCommandSequence_ = newSequence;
                if (command.kind == ControlCommand::Kind::SelectDevice &&
                    command.serial != requestedSerial_) {
                    requestedSerial_ = command.serial;
                    owner_.publishEvent(ServiceEventKind::Info,
                                        "device selected: " + command.serial);
                    break;  // 立即重新解析
                }
            }
            int hotPlug = 0;
            std::uint64_t newHotPlug = lastHotPlugSequence_;
            if (owner_.hotPlugMailbox().try_load_newer_than(lastHotPlugSequence_, hotPlug,
                                                            newHotPlug)) {
                lastHotPlugSequence_ = newHotPlug;
                break;  // 总线变化，重新枚举
            }
            std::this_thread::sleep_for(kWaitingPollInterval);
        }
        if (stopToken.stop_requested()) {
            return {};
        }
    }
}

/// 消费命令（分辨率切换 / 设备选择）；设备选择仅记录意图与事件。
CaptureLoop::StreamExit CaptureLoop::streamLoop(rs2::context& context,
                                                rs2::pipeline& pipeline,
                                                rs2::pipeline_profile& profile,
                                                const std::string& serial,
                                                executor::StopToken stopToken) {
    int consecutiveFailures = 0;
    std::uint64_t sequence = 0;
    std::vector<std::uint8_t> rgba;
    activeSerial_ = serial;

    float depthScale = 0.001f;
    try {
        depthScale = profile.get_device().first<rs2::depth_sensor>().get_depth_scale();
    } catch (const rs2::error&) {
        // 无深度传感器时兜底；深度帧将不可读，但不影响彩色流。
    }

    owner_.intrinsicsMailbox().publish(
        snapshotIntrinsics(profile, owner_.nextSnapshotSequence()));
    owner_.workerTransition(CameraServiceState::Streaming, "start");
    owner_.publishEvent(ServiceEventKind::Started, "streaming " + serial);

    while (!stopToken.stop_requested()) {
        // 命令与热插拔检查点（wait_for_frames 界内每秒至少到达一次）。
        int hotPlug = 0;
        std::uint64_t newHotPlug = lastHotPlugSequence_;
        const bool hotPlugSeen = owner_.hotPlugMailbox().try_load_newer_than(
            lastHotPlugSequence_, hotPlug, newHotPlug);
        if (hotPlugSeen) {
            lastHotPlugSequence_ = newHotPlug;
        }

        ControlCommand command;
        std::uint64_t newSequence = lastCommandSequence_;
        bool restreamResolution = false;
        bool selectChanged = false;
        while (owner_.commandMailbox().try_load_newer_than(lastCommandSequence_, command,
                                                           newSequence)) {
            lastCommandSequence_ = newSequence;
            if (command.kind == ControlCommand::Kind::Restream &&
                !(command.request == request_)) {
                pendingResolution_ = command.request;
                restreamResolution = true;
            } else if (command.kind == ControlCommand::Kind::SelectDevice &&
                       command.serial != requestedSerial_) {
                requestedSerial_ = command.serial;
                selectChanged = true;
                owner_.publishEvent(ServiceEventKind::Info,
                                    "device selected: " + command.serial);
            }
        }
        if (stopToken.stop_requested()) {
            return StreamExit::Stopped;
        }
        if (restreamResolution || hotPlugSeen || selectChanged) {
            // 流送中的枚举/重启可能瞬时失败（如设备重枚举竞态）：捕获后下一检查点
            // 重试；restreamResolution 未消费即自动重试。
            try {
            // 活动设备被移除 → Waiting（DeviceLost）；否则刷新目录并按需重配。
            if (!serialOnline(context, activeSerial_)) {
                owner_.workerTransition(CameraServiceState::Waiting, "device removed");
                owner_.publishEvent(ServiceEventKind::Info,
                                    "device removed: " + activeSerial_);
                activeSerial_.clear();
                refreshCatalog(context);
                return StreamExit::DeviceLost;
            }
            refreshCatalog(context);

            // 分辨率切换（同设备）：Streaming→Restreaming→Streaming。
            if (restreamResolution) {
                request_ = pendingResolution_;
                owner_.workerTransition(CameraServiceState::Restreaming,
                                        "resolution switch");
                owner_.publishEvent(ServiceEventKind::Info, "switching resolution");
                pipeline.stop();
                profile = pipeline.start(buildConfig(request_, activeSerial_));
                owner_.intrinsicsMailbox().publish(
                    snapshotIntrinsics(profile, owner_.nextSnapshotSequence()));
                owner_.workerTransition(CameraServiceState::Streaming,
                                        "resolution switch finish");
                owner_.publishEvent(ServiceEventKind::ResolutionChanged,
                                    "resolution applied");
            }

            // 设备切换（用户指定在线设备）：Streaming→Restreaming→Streaming。
            if (selectChanged && !requestedSerial_.empty() &&
                requestedSerial_ != activeSerial_ &&
                serialOnline(context, requestedSerial_)) {
                owner_.workerTransition(CameraServiceState::Restreaming, "device switch");
                owner_.publishEvent(ServiceEventKind::Info, "switching device");
                pipeline.stop();
                profile = pipeline.start(buildConfig(request_, requestedSerial_));
                activeSerial_ = requestedSerial_;
                owner_.intrinsicsMailbox().publish(
                    snapshotIntrinsics(profile, owner_.nextSnapshotSequence()));
                refreshCatalog(context);
                owner_.workerTransition(CameraServiceState::Streaming,
                                        "device switch finish");
                owner_.publishEvent(ServiceEventKind::ResolutionChanged,
                                    "device active: " + activeSerial_);
            } else if (selectChanged && !requestedSerial_.empty()) {
                owner_.publishEvent(ServiceEventKind::Info,
                                    "selected device offline: " + requestedSerial_);
            }
            } catch (const rs2::error& error) {
                    errorMessage_ = error.what();
            }
        }

        rs2::frameset frameset;
        try {
            frameset = pipeline.wait_for_frames(static_cast<unsigned int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(kFrameWaitTimeout)
                    .count()));
            consecutiveFailures = 0;
        } catch (const rs2::error&) {
            if (++consecutiveFailures >= kMaxConsecutiveFrameFailures) {
                // 设备已不在总线上 → 按热插拔移除处理（Waiting），否则按真实错误上抛。
                if (!serialOnline(context, activeSerial_)) {
                    owner_.workerTransition(CameraServiceState::Waiting, "device removed");
                    owner_.publishEvent(ServiceEventKind::Info,
                                        "device removed: " + activeSerial_);
                    activeSerial_.clear();
                    refreshCatalog(context);
                    return StreamExit::DeviceLost;
                }
                errorMessage_ = "stream interrupted while device present";
                return StreamExit::Fatal;
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
    return StreamExit::Stopped;
}

[[nodiscard]] bool CaptureLoop::serialOnline(const rs2::context& context,
                                             const std::string& serial) {
    if (serial.empty()) {
        return false;
    }
    return serialInDevices(enumerateDevices(context), serial);
}

/// 分辨率切换命令（Streaming/Restreaming 有效）。
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
    return true;
}

/// 设备选择命令（Waiting/Opening/Streaming/Restreaming 有效；粘性，跨插拔保留）。
bool RealSenseCamera::requestDevice(const std::string& serial, std::string* error) {
    const CameraServiceState current = machine_.state();
    if (current == CameraServiceState::Idle || current == CameraServiceState::Failed ||
        current == CameraServiceState::Stopping) {
        if (error != nullptr) {
            *error = "cannot select device from state " + std::string(toString(current));
        }
        return false;
    }
    ControlCommand command;
    command.kind = ControlCommand::Kind::SelectDevice;
    command.serial = serial;
    if (!commands_.try_publish(command)) {
        if (error != nullptr) {
            *error = "command mailbox rejected request";
        }
        return false;
    }
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

/// 采集 worker 主循环：设备解析（Waiting，DEC-006）→ 打开（有界重试）→ 流送
/// （Streaming）→ 设备丢失/切换回到解析域；Stop/致命错误退出。
void CaptureLoop::run(executor::StopToken stopToken) {
    bool failed = false;
    rs2::context context;
    try {
        while (!stopToken.stop_requested()) {
            const std::string serial = resolveTarget(context, stopToken);
            if (serial.empty() || stopToken.stop_requested()) {
                break;
            }

            // 打开（设备在线但有界重试；设备消失则回 Waiting 重解析）。
            rs2::pipeline pipeline;
            rs2::pipeline_profile profile;
            bool opened = false;
            while (!stopToken.stop_requested()) {
                try {
                    profile = pipeline.start(buildConfig(request_, serial));
                    opened = true;
                    break;
                } catch (const std::exception& error) {
                    errorMessage_ = error.what();
                    if (!serialOnline(context, serial)) {
                        break;  // 设备又消失 → Waiting 重解析
                    }
                    if (++openFailures_ >= kMaxConsecutiveOpenFailures) {
                        failed = true;
                        break;
                    }
                    sleepPoll(stopToken);
                }
            }
            if (!opened) {
                if (failed) {
                    break;
                }
                continue;
            }
            openFailures_ = 0;

            const StreamExit exit =
                streamLoop(context, pipeline, profile, serial, stopToken);
            if (exit == StreamExit::Stopped || exit == StreamExit::Fatal) {
                break;
            }
            // DeviceLost：外层重新解析（Waiting 自动恢复/等待用户选择）。
        }
    } catch (const std::exception& error) {
        failed = true;
        errorMessage_ = error.what();
    }
    owner_.finishWorker(failed,
                        failed ? errorMessage_ : std::string("capture loop stopped"));
}

[[nodiscard]] bool CaptureLoop::sleepPoll(executor::StopToken stopToken) {
    for (int i = 0; i < 10 && !stopToken.stop_requested(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    return !stopToken.stop_requested();
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

    // 启动不依赖相机连接（DEC-006）：无设备时 worker 进入 Waiting 等待接入。
    executor::BlockingWorkerSpec spec;
    spec.name = "rsv-capture";
    spec.config.thread_name = "rsv-capture";
    spec.config.startup_timeout = std::chrono::milliseconds(2000);
    spec.worker = std::make_unique<CaptureLoop>(*this, request);
    worker_ = executor_.start_worker(std::move(spec));
    if (!worker_.started()) {
        const std::string message =
            "capture worker admission failed: " + worker_.start_result().message;
        failFromCaller(message);
        outcome.error = message;
        return outcome;
    }
    workerRunning_.store(true);

    outcome.admitted = true;
    return outcome;
}

std::shared_ptr<ICameraService> createRealSenseCameraService(executor::Executor& executor) {
    return std::make_shared<RealSenseCamera>(executor);
}

}  // namespace rsv
