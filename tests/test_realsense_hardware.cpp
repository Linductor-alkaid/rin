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

// --- 内容级断言参数（真机 D435IF 暗室实测，2026-09-23）---
// 缺陷背景：渲染侧 eui::ImageStream 输出"全黑帧"（仅左缘彩条），帧元数据 valid()
// 全部通过，暴露内容级盲区；下列断言对采集帧的像素内容把关。
// 实测依据：librealsense 原始帧落盘 RGB 100% 像素非零、Z16 57% 非零；
// 阈值取实测量 ~1/3 保守值。
constexpr double kRgbMinNonZeroRatio = 0.30;    // 实测 100% -> 保守 30%
constexpr double kDepthMinNonZeroRatio = 0.05;  // 实测 57%  -> 保守 5%
// 偶发过暗帧重试窗口：最多连续采样 N 帧，取窗口内最大占比判定。
constexpr int kContentSampleFrames = 10;

/// 非零像素占比：R/G/B 任一内容通道非零的像素比例。
/// 统计口径排除 alpha——转换器将 A 恒置 255，计入 A 会把全黑帧也判为非零
/// （正是本次缺陷形态），故只看内容通道。
double contentNonZeroRatio(const Frame& frame) {
    if (!frame.pixels || frame.width == 0 || frame.height == 0) {
        return 0.0;
    }
    const std::uint8_t* data = frame.pixels->data();
    std::uint64_t nonZero = 0;
    for (std::uint32_t row = 0; row < frame.height; ++row) {
        const std::uint8_t* rowPtr = data + static_cast<std::size_t>(row) * frame.stride;
        for (std::uint32_t col = 0; col < frame.width; ++col) {
            const std::uint8_t* px = rowPtr + static_cast<std::size_t>(col) * 4;
            if (px[0] != 0 || px[1] != 0 || px[2] != 0) {
                ++nonZero;
            }
        }
    }
    return static_cast<double>(nonZero) /
           (static_cast<std::uint64_t>(frame.width) * frame.height);
}

/// 全帧像素字节 FNV-1a 64 校验和（帧间变化检测）。
std::uint64_t frameChecksum(const Frame& frame) {
    if (!frame.pixels) {
        return 0;
    }
    std::uint64_t hash = 1469598103934665603ull;
    for (const std::uint8_t byte : *frame.pixels) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
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
        RSV_CHECK(rgbFrame.sequence > 0);
        RSV_CHECK_EQ(rgbFrame.width, 640u);
        RSV_CHECK_EQ(rgbFrame.height, 480u);
    }
    if (depthArrived) {
        RSV_CHECK(depthFrame.valid());
        RSV_CHECK(depthFrame.kind == FrameKind::Depth);
        RSV_CHECK(depthFrame.width > 0);
        RSV_CHECK(depthFrame.height > 0);
        RSV_CHECK(depthFrame.sequence > 0);
        RSV_CHECK_EQ(depthFrame.width, 640u);
        RSV_CHECK_EQ(depthFrame.height, 480u);
    }
    std::printf("hardware: first rgb frame %.0f ms, first depth frame %.0f ms\n", firstRgbMs,
                firstDepthMs);

    // --- 2b) 内容级断言：非零像素占比阈值 + 帧间校验和变化（防"静止纹理"假阳性）---
    // 最多连续采样 kContentSampleFrames 帧，占比取窗口最大（容忍偶发过暗帧）；
    // 变化检测要求窗口内存在至少一对相邻帧校验和不同（逐帧全字节 FNV-1a）。
    const auto sampleContent = [&](FrameKind kind, std::uint64_t& mailboxSequence,
                                   double threshold, const char* label) {
        int framesSeen = 0;
        double maxRatio = 0.0;
        double ratios[kContentSampleFrames] = {};
        bool checksumVariation = false;
        bool havePreviousChecksum = false;
        bool haveSequence = false;
        std::uint64_t previousChecksum = 0;
        std::uint64_t previousSequence = 0;
        Frame frame;
        pollUntil(5s, [&] {
            if (framesSeen >= kContentSampleFrames) {
                return true;
            }
            if (!service.tryLoadFrame(kind, mailboxSequence, frame)) {
                return false;  // 尚无新帧，轮询重试
            }
            ++framesSeen;
            RSV_CHECK(frame.valid());
            RSV_CHECK_EQ(frame.width, 640u);   // 采样期档位未变，尺寸断言逐帧维持
            RSV_CHECK_EQ(frame.height, 480u);
            if (haveSequence) {
                RSV_CHECK(frame.sequence > previousSequence);  // 邮箱序号严格单调
            }
            previousSequence = frame.sequence;
            haveSequence = true;

            ratios[framesSeen - 1] = contentNonZeroRatio(frame);
            if (ratios[framesSeen - 1] > maxRatio) {
                maxRatio = ratios[framesSeen - 1];
            }
            const std::uint64_t checksum = frameChecksum(frame);
            if (havePreviousChecksum && checksum != previousChecksum) {
                checksumVariation = true;  // 任一相邻帧对内容不同即通过
            }
            previousChecksum = checksum;
            havePreviousChecksum = true;
            return maxRatio >= threshold && checksumVariation;  // 双条件满足可提前结束
        });
        std::printf("hardware: %s content over %d frames: max non-zero ratio %.1f%% "
                    "(threshold >= %.0f%%), checksum variation=%s, ratios:",
                    label, framesSeen, maxRatio * 100.0, threshold * 100.0,
                    checksumVariation ? "yes" : "no");
        for (int i = 0; i < framesSeen; ++i) {
            std::printf(" %.1f%%", ratios[i] * 100.0);
        }
        std::printf("\n");
        RSV_CHECK(framesSeen >= 2);  // 变化检测至少需要两帧
        RSV_CHECK(checksumVariation);
        RSV_CHECK(maxRatio >= threshold);
    };
    sampleContent(FrameKind::Rgb, rgbSequence, kRgbMinNonZeroRatio, "rgb");
    sampleContent(FrameKind::Depth, depthSequence, kDepthMinNonZeroRatio, "depth");

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
    const std::uint64_t rgbSeqBeforeChange = rgbSequence;
    const std::uint64_t depthSeqBeforeChange = depthSequence;
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
        RSV_CHECK(rgb848.sequence > rgbSeqBeforeChange);  // 跨 restream 序号前移
        const double ratio = contentNonZeroRatio(rgb848);
        std::printf("hardware: 848x480 rgb non-zero ratio %.1f%%\n", ratio * 100.0);
        RSV_CHECK(ratio >= kRgbMinNonZeroRatio);
    }
    if (depth848Arrived) {
        RSV_CHECK(depth848.valid());
        RSV_CHECK_EQ(depth848.width, 848u);
        RSV_CHECK_EQ(depth848.height, 480u);
        RSV_CHECK(depth848.sequence > depthSeqBeforeChange);
        const double ratio = contentNonZeroRatio(depth848);
        std::printf("hardware: 848x480 depth non-zero ratio %.1f%%\n", ratio * 100.0);
        RSV_CHECK(ratio >= kDepthMinNonZeroRatio);
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
