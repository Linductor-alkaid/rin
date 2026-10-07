// M13-03 真机帧率档位切换验收（hardware 标签，Independent-Verification-Agent；
// docs/plans/m13-fps-selection.md）：视频-only 双流 848x480@30 → requestResolution
// 848x480@60 → 回 30 的实测帧率闭环。独立于 test_realsense_hardware（其 enableMotion
// 路径在本宿主因 LRS-20261007-001 运动流饿死而 FAIL，台账在案）：本测试全程
// enableMotion=false（视频-only），不触碰运动通道。
//
// 流程（有界轮询纪律、无裸 sleep 等待、所有路径 stop() 收尾）：
//   1) 无设备 → SKIP 77（与 realsense_hardware 同语义：Started 5s 未达且
//      Failed 文本含 "no RealSense device"，或 state==Waiting）。
//   2) start(848x480@30, enableMotion=false) → Started → 首帧 RGB/Depth。
//   3) ≥2s 墙钟窗口实测 RGB 帧率（按服务生命周期帧序号差 ÷ 墙钟时长；
//      LatestMailbox 只保留最新帧，轮询采样下以序号差还原真实发布速率；
//      设备时间戳口径交叉打印），断言 ≥20（30fps 档容忍调度/USB 抖动）。
//   4) requestResolution(848x480@60) → 6s 内 ResolutionChanged + 新序号 848x480
//      双流帧 → ≥2s 实测断言 ≥45（60fps 容忍同步/USB 抖动）。
//   5) requestResolution 回 30 → 同上实测恢复 ≤40 量级（断言 20 < rate < 45，
//      避免贴设备档位的脆弱断言）。
//   6) 全程采样帧分辨率恒 848x480；stop() 收敛 Idle；二次 stop() 幂等。
// 中途失败打印已实测数据再退出（RIN_CHECK 记录失败 + main 兜底 stop()）。
#include "test_util.hpp"

#include <kairo/executor.hpp>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "realsense_camera_service.hpp"
#include "rin/camera_service.hpp"

namespace {

using namespace std::chrono_literals;

using rin::CameraServiceState;
using rin::Frame;
using rin::FrameKind;
using rin::ICameraService;
using rin::ServiceEvent;
using rin::ServiceEventKind;
using rin::StreamRequest;

/// 有界轮询：每 20ms 谓词一次，budget 内为真返回 true，超时返回 false。
/// 硬件变体（预算前置、固定步进、超时末次复核；帧率测量窗口以 20ms 步进
/// 降低窗口边界量化误差，100ms 步进在 2s 窗口引入 ~5% 速率误差）。
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
        std::this_thread::sleep_for(20ms);
    }
}

/// 单窗口帧率测量结果：墙钟口径（序号差 ÷ 墙钟时长）与设备时间戳口径交叉。
struct RateMeasurement {
    double wallHz = 0.0;
    double deviceTsHz = 0.0;
    std::uint64_t frames = 0;
    double windowSeconds = 0.0;
};

/// ≥window 的墙钟窗口内 RGB 帧率测量。跨窗口序号差还原发布速率：
/// tryLoadFrame 的最新态邮箱语义下每次轮询只见到最新帧，服务生命周期帧序号
/// 逐帧递增（2026-10-07 热插拔修复后的契约），序号差即窗口内真实发布帧数。
/// 帧率采样全程校验分辨率恒 width x height（档位漂移当场暴露）。窗口未在
/// window + 5s 内跨过（流停滞）返回 false；已测得的部分数据保留在 out。
bool measureRgbRate(ICameraService& service, std::uint64_t& lastSeenSequence,
                    std::uint32_t width, std::uint32_t height,
                    std::chrono::steady_clock::duration window, RateMeasurement& out) {
    Frame first;
    Frame frame;
    std::uint64_t probe = lastSeenSequence;
    if (!pollUntil(5s, [&] { return service.tryLoadFrame(FrameKind::Rgb, probe, first); })) {
        std::printf("fps-hw: no rgb frame within 5s (state=%s, lastError='%s')\n",
                    rin::toString(service.state()), service.lastError().c_str());
        return false;
    }
    RIN_CHECK(first.valid());
    RIN_CHECK_EQ(first.width, width);
    RIN_CHECK_EQ(first.height, height);
    const auto t0 = std::chrono::steady_clock::now();
    const std::uint64_t firstSequence = first.sequence;
    const double firstTimestampMs = first.deviceTimestampMs;
    const auto hardDeadline = t0 + window + 5s;
    for (;;) {
        if (service.tryLoadFrame(FrameKind::Rgb, probe, frame)) {
            RIN_CHECK(frame.valid());
            RIN_CHECK(frame.sequence > firstSequence);  // 序号严格单调
            RIN_CHECK_EQ(frame.width, width);           // 窗口内档位不得漂移
            RIN_CHECK_EQ(frame.height, height);
            const auto now = std::chrono::steady_clock::now();
            if (now - t0 >= window) {
                const double seconds = std::chrono::duration<double>(now - t0).count();
                out.frames = frame.sequence - firstSequence;
                out.windowSeconds = seconds;
                out.wallHz = static_cast<double>(out.frames) / seconds;
                if (frame.deviceTimestampMs > firstTimestampMs) {
                    out.deviceTsHz = static_cast<double>(out.frames) * 1000.0 /
                                     (frame.deviceTimestampMs - firstTimestampMs);
                }
                lastSeenSequence = probe;
                return true;
            }
        }
        if (std::chrono::steady_clock::now() >= hardDeadline) {
            lastSeenSequence = probe;
            std::printf("fps-hw: rate window stalled (frames so far %llu over %.2fs)\n",
                        static_cast<unsigned long long>(out.frames), out.windowSeconds);
            return false;
        }
        std::this_thread::sleep_for(20ms);
    }
}

void printRate(const RateMeasurement& rate, const char* label) {
    std::printf("fps-hw: %s measured rgb %.2f Hz (device-ts %.2f Hz, %llu frames over %.2fs)\n",
                label, rate.wallHz, rate.deviceTsHz, static_cast<unsigned long long>(rate.frames),
                rate.windowSeconds);
}

/// 等 ResolutionChanged 事件 + 新序号的双流 848x480 帧（统一 6s 期限；
/// 返回是否三条件齐备，事件消息与帧序号经出参取证）。
bool awaitRestream(ICameraService& service, std::uint64_t& rgbSequence,
                   std::uint64_t& depthSequence, const std::uint64_t rgbSeqBefore,
                   const std::uint64_t depthSeqBefore, ServiceEvent& changedEvent) {
    const auto deadline = std::chrono::steady_clock::now() + 6s;
    bool changed = false;
    bool rgbArrived = false;
    bool depthArrived = false;
    Frame frame;
    while (std::chrono::steady_clock::now() < deadline &&
           !(changed && rgbArrived && depthArrived)) {
        if (!changed && service.tryLoadEvent(changedEvent) &&
            changedEvent.kind == ServiceEventKind::ResolutionChanged) {
            changed = true;
        }
        if (!rgbArrived && service.tryLoadFrame(FrameKind::Rgb, rgbSequence, frame) &&
            frame.sequence > rgbSeqBefore && frame.width == 848u && frame.height == 480u) {
            rgbArrived = true;
        }
        if (!depthArrived && service.tryLoadFrame(FrameKind::Depth, depthSequence, frame) &&
            frame.sequence > depthSeqBefore && frame.width == 848u && frame.height == 480u) {
            depthArrived = true;
        }
        std::this_thread::sleep_for(20ms);
    }
    return changed && rgbArrived && depthArrived;
}

/// 冒烟主体。返回 0 = 通过，77 = 无设备跳过，1 = 失败。
int runFpsSwitchTest(ICameraService& service) {
    // 全程视频-only（LRS-20261007-001：本宿主运动流使能即整条 pipeline 饿死）。
    const StreamRequest gear30{848, 480, 30, 848, 480, 30, false};
    const StreamRequest gear60{848, 480, 60, 848, 480, 60, false};

    // --- 1) start 准入 + Started（无设备 SKIP 77，判据与 realsense_hardware 一致）---
    const rin::StartOutcome outcome = service.start(gear30);
    if (!outcome.admitted) {
        std::printf("FAIL: start rejected: %s\n", outcome.error.c_str());
        return 1;
    }
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
    if (!startedEvent && service.state() == CameraServiceState::Waiting) {
        std::printf("SKIP: no RealSense device (state=Waiting after 5s, last event '%s')\n",
                    event.message.c_str());
        return 77;
    }
    RIN_CHECK(startedEvent);
    if (!startedEvent) {
        std::printf("FAIL: Started event not observed within 5s (state=%s, last event kind=%d "
                    "message='%s', lastError='%s')\n",
                    rin::toString(service.state()), static_cast<int>(event.kind),
                    event.message.c_str(), service.lastError().c_str());
        return 1;
    }
    RIN_CHECK_EQ(service.state(), CameraServiceState::Streaming);

    // --- 2) 首帧 RGB/Depth 到达（≤5s），分辨率 848x480 ---
    std::uint64_t rgbSequence = 0;
    std::uint64_t depthSequence = 0;
    Frame rgbFrame;
    Frame depthFrame;
    const bool rgbArrived =
        pollUntil(5s, [&] { return service.tryLoadFrame(FrameKind::Rgb, rgbSequence, rgbFrame); });
    const bool depthArrived = pollUntil(5s, [&] {
        return service.tryLoadFrame(FrameKind::Depth, depthSequence, depthFrame);
    });
    RIN_CHECK(rgbArrived);
    RIN_CHECK(depthArrived);
    if (rgbArrived) {
        RIN_CHECK(rgbFrame.valid());
        RIN_CHECK_EQ(rgbFrame.width, 848u);
        RIN_CHECK_EQ(rgbFrame.height, 480u);
    }
    if (depthArrived) {
        RIN_CHECK(depthFrame.valid());
        RIN_CHECK_EQ(depthFrame.width, 848u);
        RIN_CHECK_EQ(depthFrame.height, 480u);
    }
    if (!rgbArrived || !depthArrived) {
        return 1;
    }
    RIN_CHECK_EQ(service.state(), CameraServiceState::Streaming);

    // --- 3) 30fps 档实测：≥2s 窗口 RGB 帧率 ≥20（30fps 档容忍抖动）---
    RateMeasurement rate30;
    RIN_CHECK(measureRgbRate(service, rgbSequence, 848u, 480u, 2s, rate30));
    printRate(rate30, "848x480@30 (initial)");
    RIN_CHECK(rate30.wallHz >= 20.0);

    // --- 4) 切 60fps 档：ResolutionChanged + 双流新帧 → 实测 ≥45 ---
    const std::uint64_t rgbSeqBefore60 = rgbSequence;
    const std::uint64_t depthSeqBefore60 = depthSequence;
    std::string requestError = "<untouched>";
    RIN_CHECK(service.requestResolution(gear60, &requestError));
    ServiceEvent changedEvent;
    const bool restreamed60 =
        awaitRestream(service, rgbSequence, depthSequence, rgbSeqBefore60, depthSeqBefore60,
                      changedEvent);
    RIN_CHECK(restreamed60);
    if (!restreamed60) {
        std::printf("FAIL: 60fps restream incomplete within 6s (state=%s, lastError='%s')\n",
                    rin::toString(service.state()), service.lastError().c_str());
        return 1;
    }
    RIN_CHECK_EQ(changedEvent.message, std::string("resolution applied"));
    RIN_CHECK_EQ(service.state(), CameraServiceState::Streaming);
    RateMeasurement rate60;
    RIN_CHECK(measureRgbRate(service, rgbSequence, 848u, 480u, 2s, rate60));
    printRate(rate60, "848x480@60 (after switch)");
    RIN_CHECK(rate60.wallHz >= 45.0);

    // --- 5) 切回 30fps 档：实测恢复 20 < rate < 45 区间 ---
    const std::uint64_t rgbSeqBefore30 = rgbSequence;
    const std::uint64_t depthSeqBefore30 = depthSequence;
    std::string backError = "<untouched>";
    RIN_CHECK(service.requestResolution(gear30, &backError));
    ServiceEvent backEvent;
    const bool restreamed30 =
        awaitRestream(service, rgbSequence, depthSequence, rgbSeqBefore30, depthSeqBefore30,
                      backEvent);
    RIN_CHECK(restreamed30);
    if (!restreamed30) {
        std::printf("FAIL: 30fps restore restream incomplete within 6s (state=%s, "
                    "lastError='%s')\n",
                    rin::toString(service.state()), service.lastError().c_str());
        return 1;
    }
    RIN_CHECK_EQ(service.state(), CameraServiceState::Streaming);
    RateMeasurement rate30Restored;
    RIN_CHECK(measureRgbRate(service, rgbSequence, 848u, 480u, 2s, rate30Restored));
    printRate(rate30Restored, "848x480@30 (restored)");
    RIN_CHECK(rate30Restored.wallHz > 20.0);
    RIN_CHECK(rate30Restored.wallHz < 45.0);

    // --- 6) stop() 收敛 Idle；二次 stop() 幂等 ---
    service.stop();
    RIN_CHECK(service.state() == CameraServiceState::Idle);
    service.stop();
    RIN_CHECK(service.state() == CameraServiceState::Idle);
    ServiceEvent finalEvent;
    if (service.tryLoadEvent(finalEvent)) {
        RIN_CHECK(finalEvent.kind == ServiceEventKind::Stopped);
    }
    return 0;
}

}  // namespace

int main() {
    kairo::Executor executor;
    if (!executor.initialize({})) {
        std::printf("FAIL: kairo initialize failed\n");
        return 1;
    }

    int status = 1;
    {
        // service 持有 executor 引用：先于 executor 收尾析构（工厂契约）。
        std::shared_ptr<ICameraService> service = rin::createRealSenseCameraService(executor);
        if (!service) {
            std::printf("FAIL: createRealSenseCameraService returned null\n");
            return 1;
        }
        status = runFpsSwitchTest(*service);
        service->stop();  // 所有早退路径的兜底收尾（幂等）。
        RIN_CHECK(service->state() == CameraServiceState::Idle);
        service.reset();
    }
    executor.shutdown();

    const int failures = rin_test::exitStatus();
    if (failures > 0) {
        return 1;
    }
    return status;  // 0 = 通过；77 = 无设备跳过
}
