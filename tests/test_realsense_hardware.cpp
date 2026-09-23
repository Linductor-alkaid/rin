// 真机冒烟测试（hardware 标签）：D435if 在位时执行完整链路
// start(640x480@30) -> Started 事件 -> RGB/Depth 帧 -> 内参 -> 能力 ->
// requestResolution(848x480@30) -> ResolutionChanged + 新分辨率帧 ->
// stop() 收敛 Idle -> 二次 stop() 幂等。
// 无设备时打印 SKIP 并返回 77（ctest SKIP_RETURN_CODE 记为跳过）。
// 全部等待为有界轮询（100ms 间隔），不使用裸 sleep 等待；退出前保证 stop() 收尾。
#include "test_util.hpp"

#include <executor/executor.hpp>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "realsense_camera_service.hpp"
#include "rs_vision/camera_service.hpp"

namespace {

using namespace std::chrono_literals;

using rsv::CameraServiceState;
using rsv::Frame;
using rsv::FrameKind;
using rsv::ICameraService;
using rsv::ServiceEvent;
using rsv::ServiceEventKind;

/// 有界轮询：每 100ms 谓词一次，budget 内为真返回 true，超时返回 false。
template <typename Pred>
bool pollUntil(std::chrono::steady_clock::duration budget, Pred&& pred) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
        if (pred()) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(100ms);
    }
}

/// 冒烟主体。返回 0 = 通过（RSV_CHECK 结果见 exitStatus），77 = 无设备跳过，1 = 失败。
int runSmokeTest(ICameraService& service) {
    const rsv::StreamRequest baseRequest{640, 480, 30, 640, 480, 30};  // D435if RGB8/Z16 均支持
    const rsv::StreamRequest altRequest{848, 480, 30, 848, 480, 30};

    const auto t0 = std::chrono::steady_clock::now();
    const auto elapsedMs = [&t0] {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
            .count();
    };

    // --- start 准入；无设备 -> SKIP 77 ---
    const rsv::StartOutcome outcome = service.start(baseRequest);
    if (!outcome.admitted) {
        if (outcome.error.find("no RealSense device") != std::string::npos) {
            std::printf("SKIP: no RealSense device (%s)\n", outcome.error.c_str());
            return 77;
        }
        std::printf("FAIL: start rejected: %s\n", outcome.error.c_str());
        return 1;
    }

    // --- 1) <=5s 收到 Started 事件（Failed + no device -> SKIP 77）---
    ServiceEvent event;
    bool startedEvent = false;
    std::string failureMessage;
    pollUntil(5s, [&] {
        if (!service.tryLoadEvent(event)) {
            return false;
        }
        if (event.kind == ServiceEventKind::Started) {
            startedEvent = true;
            return true;
        }
        if (event.kind == ServiceEventKind::Failed) {
            failureMessage = event.message;
            return true;
        }
        return false;
    });
    if (!startedEvent && failureMessage.find("no RealSense device") != std::string::npos) {
        std::printf("SKIP: no RealSense device (%s)\n", failureMessage.c_str());
        return 77;
    }
    RSV_CHECK(startedEvent);
    if (!startedEvent) {
        std::printf(
            "FAIL: Started event not observed within 5s (state=%s, last event kind=%d "
            "message='%s', lastError='%s')\n",
            rsv::toString(service.state()), static_cast<int>(event.kind), event.message.c_str(),
            service.lastError().c_str());
        return 1;
    }
    std::printf("hardware: Started event after %.0f ms\n", elapsedMs());

    // --- 2) <=5s 各收到 >=1 帧 RGB 与 Depth；valid()、宽高匹配 640x480 ---
    double firstRgbMs = -1.0;
    double firstDepthMs = -1.0;
    Frame rgbFrame;
    Frame depthFrame;
    std::uint64_t rgbSequence = 0;
    std::uint64_t depthSequence = 0;
    const bool rgbArrived =
        pollUntil(5s, [&] {
            if (service.tryLoadFrame(FrameKind::Rgb, rgbSequence, rgbFrame)) {
                firstRgbMs = elapsedMs();
                return true;
            }
            return false;
        });
    const bool depthArrived =
        pollUntil(5s, [&] {
            if (service.tryLoadFrame(FrameKind::Depth, depthSequence, depthFrame)) {
                firstDepthMs = elapsedMs();
                return true;
            }
            return false;
        });
    RSV_CHECK(rgbArrived);
    RSV_CHECK(depthArrived);
    if (rgbArrived) {
        RSV_CHECK(rgbFrame.valid());
        RSV_CHECK(rgbFrame.kind == FrameKind::Rgb);
        RSV_CHECK(rgbFrame.width > 0);
        RSV_CHECK(rgbFrame.height > 0);
        RSV_CHECK_EQ(rgbFrame.width, 640u);
        RSV_CHECK_EQ(rgbFrame.height, 480u);
    }
    if (depthArrived) {
        RSV_CHECK(depthFrame.valid());
        RSV_CHECK(depthFrame.kind == FrameKind::Depth);
        RSV_CHECK(depthFrame.width > 0);
        RSV_CHECK(depthFrame.height > 0);
        RSV_CHECK_EQ(depthFrame.width, 640u);
        RSV_CHECK_EQ(depthFrame.height, 480u);
    }
    std::printf("hardware: first rgb frame %.0f ms, first depth frame %.0f ms\n", firstRgbMs,
                firstDepthMs);

    // --- 3) 内参快照 color/depth valid()，且与当前档位一致 ---
    rsv::IntrinsicsSnapshot intrinsics;
    std::uint64_t intrinsicsSequence = 0;
    const bool intrinsicsArrived = pollUntil(5s, [&] {
        return service.tryLoadIntrinsics(intrinsicsSequence, intrinsics);
    });
    RSV_CHECK(intrinsicsArrived);
    if (intrinsicsArrived) {
        RSV_CHECK(intrinsics.color.valid());
        RSV_CHECK(intrinsics.depth.valid());
        RSV_CHECK_EQ(intrinsics.color.width, 640u);
        RSV_CHECK_EQ(intrinsics.color.height, 480u);
        RSV_CHECK_EQ(intrinsics.depth.width, 640u);
        RSV_CHECK_EQ(intrinsics.depth.height, 480u);
        std::printf("hardware: intrinsics color %ux%u (fx=%.1f), depth %ux%u (fx=%.1f)\n",
                    intrinsics.color.width, intrinsics.color.height, intrinsics.color.fx,
                    intrinsics.depth.width, intrinsics.depth.height, intrinsics.depth.fx);
    }

    // --- 4) 能力快照：设备在位、序列号与档位列表非空 ---
    rsv::StreamCapabilities caps;
    std::uint64_t capsSequence = 0;
    const bool capsArrived = pollUntil(5s, [&] {
        return service.tryLoadCapabilities(capsSequence, caps);
    });
    RSV_CHECK(capsArrived);
    if (capsArrived) {
        RSV_CHECK(caps.devicePresent);
        RSV_CHECK(!caps.serial.empty());
        RSV_CHECK(!caps.colorOptions.empty());
        RSV_CHECK(!caps.depthOptions.empty());
        std::printf(
            "hardware: device='%s' serial='%s' firmware='%s' (color options=%zu, depth "
            "options=%zu)\n",
            caps.deviceName.c_str(), caps.serial.c_str(), caps.firmwareVersion.c_str(),
            caps.colorOptions.size(), caps.depthOptions.size());
    }

    // --- 5) requestResolution 848x480：统一 6s 期限等 ResolutionChanged + 新帧 ---
    std::string requestError = "<untouched>";
    RSV_CHECK(service.requestResolution(altRequest, &requestError));
    bool changedEvent = false;
    bool rgb848Arrived = false;
    bool depth848Arrived = false;
    double changedMs = -1.0;
    Frame rgb848;
    Frame depth848;
    const auto changeDeadline = std::chrono::steady_clock::now() + 6s;
    while (std::chrono::steady_clock::now() < changeDeadline &&
           !(changedEvent && rgb848Arrived && depth848Arrived)) {
        if (!changedEvent && service.tryLoadEvent(event) &&
            event.kind == ServiceEventKind::ResolutionChanged) {
            changedEvent = true;
            changedMs = elapsedMs();
        }
        Frame frame;
        if (!rgb848Arrived && service.tryLoadFrame(FrameKind::Rgb, rgbSequence, frame) &&
            frame.width == 848u && frame.height == 480u) {
            rgb848 = std::move(frame);
            rgb848Arrived = true;
        }
        if (!depth848Arrived && service.tryLoadFrame(FrameKind::Depth, depthSequence, frame) &&
            frame.width == 848u && frame.height == 480u) {
            depth848 = std::move(frame);
            depth848Arrived = true;
        }
        std::this_thread::sleep_for(100ms);
    }
    RSV_CHECK(changedEvent);
    RSV_CHECK(rgb848Arrived);
    RSV_CHECK(depth848Arrived);
    if (rgb848Arrived) {
        RSV_CHECK(rgb848.valid());
        RSV_CHECK_EQ(rgb848.width, 848u);
        RSV_CHECK_EQ(rgb848.height, 480u);
    }
    if (depth848Arrived) {
        RSV_CHECK(depth848.valid());
        RSV_CHECK_EQ(depth848.width, 848u);
        RSV_CHECK_EQ(depth848.height, 480u);
    }
    std::printf("hardware: ResolutionChanged after %.0f ms, 848x480 rgb=%s depth=%s\n", changedMs,
                rgb848Arrived ? "ok" : "missing", depth848Arrived ? "ok" : "missing");

    // --- 6) stop() 收敛 Idle；二次 stop() 幂等不崩溃 ---
    service.stop();
    RSV_CHECK(service.state() == CameraServiceState::Idle);
    service.stop();
    RSV_CHECK(service.state() == CameraServiceState::Idle);
    ServiceEvent finalEvent;
    if (service.tryLoadEvent(finalEvent)) {
        RSV_CHECK(finalEvent.kind == ServiceEventKind::Stopped);
    }
    std::printf("hardware: stopped cleanly at %.0f ms\n", elapsedMs());
    return 0;
}

}  // namespace

int main() {
    executor::Executor executor;
    if (!executor.initialize_ex({})) {
        std::printf("FAIL: executor initialize_ex failed\n");
        return 1;
    }

    int status = 1;
    {
        // service 持有 executor 引用：先于 executor 收尾析构（工厂契约）。
        std::shared_ptr<ICameraService> service = rsv::createRealSenseCameraService(executor);
        if (!service) {
            std::printf("FAIL: createRealSenseCameraService returned null\n");
            return 1;
        }
        status = runSmokeTest(*service);
        service->stop();  // 所有早退路径的兜底收尾（幂等）。
        RSV_CHECK(service->state() == CameraServiceState::Idle);
        service.reset();
    }
    executor.shutdown();

    const int failures = rsv_test::exitStatus();
    if (failures > 0) {
        return 1;
    }
    return status;  // 0 = 通过；77 = 无设备跳过
}
