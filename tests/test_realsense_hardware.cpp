// 真机冒烟测试（hardware 标签）：D435if 在位时执行完整链路
// start(640x480@30, enableMotion) -> Started 事件 -> RGB/Depth 帧 -> motion 通道
// （M3-04：ACCEL/GYRO 原始采样 + IdentityImuFuser 恒等姿态快照 + 源频率收敛）->
// 内参（含 gyro→color 外参与 ACCEL/GYRO 出厂运动内参，内容级非全零防线）-> 设备目录
// （DEC-006 + IMU 能力字段）-> requestDevice(活动序列号) 幂等（不中断流）-> 深度配色
// 运行时切换（DEC-007：Grayscale 生效不重流、消息精确、帧内容灰度性质；切回 Jet 伪彩
// 性质；同值幂等）-> M3-08 IMU 出流频率验收（EMA 稳态 × 墙钟窗口计数交叉验证，交付
// 频率对设备档位只记录不断言）-> requestResolution(848x480@30)（M3-04：restream 重建
// 含 IMU，运动/姿态会话序号不回退、累计样本只增；M3-08：restream 后源频率重新收敛
// 到 restream 前同一量级）-> ResolutionChanged + 新分辨率帧 -> stop() 收敛
// Idle -> 二次 stop() 幂等。
// DEC-006：start 不再因无设备拒绝（worker 进 Waiting 稳态）；无设备时测试经
// Waiting + 空目录判定打印 SKIP 并返回 77（ctest SKIP_RETURN_CODE 记为跳过）。
// M3-04 降级语义的测试侧对应：适配器在含运动流的 pipeline.start 失败（典型：
// 无 root 时 HID/IIO scan_element 权限前置）时一次性降级为纯视频并照常 Started，
// 服务不再进入 Failed——旧"等 Failed 事件"的 SKIP 判据不可达。改用持久降级
// 签名 SKIP 77：imuSupported==true 且 motion 通道有界窗口（5s）零采样且最新
// IntrinsicsSnapshot 的 gyroToColor/gyroIntrinsics 均无效（camera_types.hpp 契约
// "运动流未使能时保持全零无效值"；降级 Info 事件会被 Started 在 latest-state
// 事件邮箱覆盖，不可作判据）。补跑条件：安装 librealsense udev 规则
//（upstream scripts/setup_udev_rules.sh）或等价 HID/IIO 授权后复跑。
// Waiting 态的设备移除/恢复语义无法在真机上程序化模拟（无免密 sudo 拔 USB），
// 由真机人工验收覆盖；本测试只覆盖单设备自动选择路径（无 IMU 设备的退化路径与
// 多设备切换路径无第二台设备，由 M3-08 真机验收按规范补充）。
// 全部等待为有界轮询（100ms 间隔），不使用裸 sleep 等待；退出前保证 stop() 收尾。
#include "test_util.hpp"

#include <executor/executor.hpp>

#include <chrono>
#include <cmath>
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

// --- M3-08 IMU 出流频率验收参数（真机 D435IF 实测，2026-09-24）---
// 实测依据：librealsense syncer 把 ACCEL/GYRO 与视频帧按帧率配对交付
//（137/137 frameset 形态 "AG"，~30 Hz/源），交付速率 ≠ 设备 ODR 档位
//（本机目录上报 accel 100/200/400、gyro 200/400）。DEC-010 明确不对真机提出
// 数值阈值（"M3-08 按 D435if 冒烟记录 IMU 出流频率"），故本测试对交付频率
// 只做"测量完整性"断言并打印记录，档位对照只打印不断言。
// EMA 稳态 gate：ingest 侧 kMotionRateEmaAlpha=0.1（时间常数 ~10 样本），
// 30 个新样本后收敛 >95%，随后再跨 90 样本窗口计数。
constexpr std::uint64_t kFreqSettleGyroSamples = 30;
constexpr std::uint64_t kFreqWindowGyroSamples = 90;
// 活流下限：窗口计数速率低于该值视为运动流未有效流动（实测 ~30 Hz/源）。
constexpr double kMinDeliveredSourceHz = 5.0;
// EMA（设备时间戳域相邻 dt 的滑动均值）与墙钟窗口计数速率的一致性相对容差
// （两者统计口径不同——调和 vs 算术平均、100ms 轮询量化——35% 只拦截量级错误）。
constexpr double kFreqConsistencyRelTol = 0.35;

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

// --- M3-08：IMU 出流频率测量（M3-08 真机记录的可编程载体）---
/// 单次频率测量结果：EMA（发布侧统计）+ 墙钟窗口计数均值（独立口径）。
struct SourceRateMeasurement {
    double gyroEmaHz = 0.0;
    double accelEmaHz = 0.0;
    double gyroCounterHz = 0.0;
    double accelCounterHz = 0.0;
    std::uint64_t gyroSamples = 0;
    std::uint64_t accelSamples = 0;
    std::uint64_t poseSequence = 0;
};

/// 跨样本窗口的频率测量：等待该源自 `lastGyroSamples` 起再前进
/// kFreqSettleGyroSamples 个陀螺样本（EMA 稳态 gate），随后以墙钟窗口跨
/// kFreqWindowGyroSamples 个陀螺样本计数，读窗口末快照的 EMA 与累计计数。
/// 窗口未在预算内完成（流停滞）返回 false。轮询经姿态通道最新态语义，
/// 计数器为 worker 侧累计值，不受轮询节奏影响。
bool measureSourceRates(ICameraService& service, std::uint64_t& lastSeenPoseSeq,
                        std::uint64_t lastGyroSamples, SourceRateMeasurement& out) {
    rin::ImuSnapshot snapshot;
    // 预算与活流下限自洽：kFreqSettleGyroSamples/kFreqWindowGyroSamples 个样本
    // 在 5 Hz 下限需 6s/18s，预算留裕量（实测 ~30 Hz 时为 1s/3s）。
    const bool settled = pollUntil(10s, [&] {
        return service.tryLoadPose(lastSeenPoseSeq, snapshot) &&
               snapshot.sources.gyroSamples >= lastGyroSamples + kFreqSettleGyroSamples;
    });
    if (!settled) {
        return false;
    }
    const rin::ImuSnapshot begin = snapshot;
    const auto tBegin = std::chrono::steady_clock::now();
    const bool windowed = pollUntil(20s, [&] {
        return service.tryLoadPose(lastSeenPoseSeq, snapshot) &&
               snapshot.sources.gyroSamples >=
                   begin.sources.gyroSamples + kFreqWindowGyroSamples;
    });
    if (!windowed) {
        return false;
    }
    const auto tEnd = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(tEnd - tBegin).count();
    if (seconds <= 0.0) {
        return false;
    }
    out.gyroEmaHz = snapshot.sources.gyroHz;
    out.accelEmaHz = snapshot.sources.accelHz;
    out.gyroSamples = snapshot.sources.gyroSamples;
    out.accelSamples = snapshot.sources.accelSamples;
    out.poseSequence = snapshot.sequence;
    out.gyroCounterHz =
        static_cast<double>(snapshot.sources.gyroSamples - begin.sources.gyroSamples) / seconds;
    out.accelCounterHz =
        static_cast<double>(snapshot.sources.accelSamples - begin.sources.accelSamples) / seconds;
    return true;
}

/// M3-08 频率记录断言（发布侧统计契约的完整性，不对交付频率设阈值）：
/// EMA 与计数速率均有限、为正、达到活流下限，且两口径在容差内一致。
void checkRateIntegrity(const SourceRateMeasurement& rates, const char* label) {
    RIN_CHECK(std::isfinite(rates.gyroEmaHz) && rates.gyroEmaHz > 0.0);
    RIN_CHECK(std::isfinite(rates.accelEmaHz) && rates.accelEmaHz > 0.0);
    RIN_CHECK(rates.gyroCounterHz >= kMinDeliveredSourceHz);
    RIN_CHECK(rates.accelCounterHz >= kMinDeliveredSourceHz);
    RIN_CHECK(std::fabs(rates.gyroEmaHz - rates.gyroCounterHz) <=
              kFreqConsistencyRelTol * rates.gyroCounterHz);
    RIN_CHECK(std::fabs(rates.accelEmaHz - rates.accelCounterHz) <=
              kFreqConsistencyRelTol * rates.accelCounterHz);
    std::printf("hardware: M3-08 imu rates %s: gyro ema=%.1fHz counter=%.1fHz, accel "
                "ema=%.1fHz counter=%.1fHz (pose seq %llu)\n",
                label, rates.gyroEmaHz, rates.gyroCounterHz, rates.accelEmaHz,
                rates.accelCounterHz, static_cast<unsigned long long>(rates.poseSequence));
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

/// 灰度性质检查（DEC-007）：全帧所有像素 R==G==B（无效深度黑像素天然满足），
/// 且至少存在一个非黑像素（避免"全黑帧也满足"的空判通过）。
struct GrayscaleCheck {
    bool allGray = false;
    bool hasNonBlack = false;
};

GrayscaleCheck grayscaleProperty(const Frame& frame) {
    GrayscaleCheck result;
    if (!frame.pixels || frame.width == 0 || frame.height == 0) {
        return result;
    }
    bool allGray = true;
    std::uint64_t nonBlack = 0;
    const std::uint8_t* data = frame.pixels->data();
    for (std::uint32_t row = 0; row < frame.height; ++row) {
        const std::uint8_t* rowPtr = data + static_cast<std::size_t>(row) * frame.stride;
        for (std::uint32_t col = 0; col < frame.width; ++col) {
            const std::uint8_t* px = rowPtr + static_cast<std::size_t>(col) * 4;
            if (px[0] != px[1] || px[1] != px[2]) {
                allGray = false;
            }
            if (px[0] != 0) {
                ++nonBlack;
            }
        }
    }
    result.allGray = allGray;
    result.hasNonBlack = nonBlack > 0;
    return result;
}

/// 伪彩性质（DEC-007）：存在 R!=G 或 G!=B 的像素。
/// jet 三角波在任何 t 都不输出 R==G==B（r/g、g/b、r/b 两两相等的 t 互不相同），
/// 因此"存在彩色像素"是区分 jet 帧与灰度帧（含无效黑像素）的充分判据。
bool hasChromaticPixel(const Frame& frame) {
    if (!frame.pixels) {
        return false;
    }
    const std::uint8_t* data = frame.pixels->data();
    for (std::uint32_t row = 0; row < frame.height; ++row) {
        const std::uint8_t* rowPtr = data + static_cast<std::size_t>(row) * frame.stride;
        for (std::uint32_t col = 0; col < frame.width; ++col) {
            const std::uint8_t* px = rowPtr + static_cast<std::size_t>(col) * 4;
            if (px[0] != px[1] || px[1] != px[2]) {
                return true;
            }
        }
    }
    return false;
}

/// 冒烟主体。返回 0 = 通过（RIN_CHECK 结果见 exitStatus），77 = 无设备或 IMU 前置
/// 缺失（运动流被适配器降级为纯视频，见文件头降级签名）跳过，1 = 失败。
int runSmokeTest(ICameraService& service) {
    // M3-04：全程 enableMotion——D435if 具备 IMU，混合 pipeline（video + ACCEL/GYRO）
    // 从首次 pipeline.start 起生效，motion 分支与视频链路并行验证。
    const rin::StreamRequest baseRequest{640, 480, 30, 640, 480, 30, true};  // RGB8/Z16 均支持
    const rin::StreamRequest altRequest{848, 480, 30, 848, 480, 30, true};

    // M3-04 跨 restream 连续性记账（2c 采样、5c 复核）：会话序号与累计样本数。
    std::uint64_t preMotionSessionSeq = 0;
    std::uint64_t prePoseSessionSeq = 0;
    std::uint64_t preGyroSamples = 0;
    std::uint64_t preAccelSamples = 0;

    const auto t0 = std::chrono::steady_clock::now();
    const auto elapsedMs = [&t0] {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
            .count();
    };

    // --- 0) Idle 态 requestDevice 守卫（DEC-006：Waiting/Opening/Streaming/Restreaming
    //     之外的状态必须拒绝）---
    {
        std::string selectError = "<untouched>";
        RIN_CHECK(!service.requestDevice("nonexistent-serial", &selectError));
        RIN_CHECK(selectError.find("Idle") != std::string::npos);
    }

    // --- 0b) Idle 态 requestDepthColorScheme 守卫（DEC-007：Idle/Failed/Stopping 拒绝
    //     并回填 error；start 之前不允许预设配色）---
    {
        std::string schemeError = "<untouched>";
        RIN_CHECK(!service.requestDepthColorScheme(rin::DepthColorScheme::Grayscale,
                                                   &schemeError));
        RIN_CHECK(!schemeError.empty());
        RIN_CHECK(schemeError.find("Idle") != std::string::npos);
        // Jet 同样拒绝（Idle 态不区分取值）。
        std::string jetError = "<untouched>";
        RIN_CHECK(!service.requestDepthColorScheme(rin::DepthColorScheme::Jet, &jetError));
        RIN_CHECK(!jetError.empty());
        RIN_CHECK(jetError.find("Idle") != std::string::npos);
    }

    // --- start 准入（DEC-006：不再因无设备拒绝）---
    const rin::StartOutcome outcome = service.start(baseRequest);
    if (!outcome.admitted) {
        std::printf("FAIL: start rejected: %s\n", outcome.error.c_str());
        return 1;
    }

    // --- 1) <=5s 收到 Started 事件；超时且 state==Waiting => 无设备 SKIP 77 ---
    ServiceEvent event;
    bool startedEvent = false;
    std::string startedMessage;
    std::string failureMessage;
    pollUntil(5s, [&] {
        if (!service.tryLoadEvent(event)) {
            return false;
        }
        if (event.kind == ServiceEventKind::Started) {
            startedEvent = true;
            startedMessage = event.message;
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
    if (!startedEvent && failureMessage.find("scan_element") != std::string::npos &&
        failureMessage.find("Permission denied") != std::string::npos) {
        // M3-04 真机前置缺失：运动流启用需写 IMU 的 HID/IIO scan_elements sysfs
        //（如 in_accel_y_en，root:root 0644），须安装 librealsense udev 规则或等价
        // 授权。按工程规范显式 SKIP 并记录补跑条件：安装 librealsense udev 规则
        //（上游 scripts/setup_udev_rules.sh）后复跑本测试；在此之前 motion 真机
        // 验证保持未执行状态。
        // 防御性保留：M3-04 降级语义下服务不再因运动流权限 Failed，本分支常态
        // 不可达（降级路径有 Started 事件，且降级 Info 文本被 latest-state 事件
        // 邮箱的 Started 覆盖）；仅当适配器回归为"打开失败即 Failed"时兜底。
        // 常态 IMU 前置缺失的 SKIP 判据见 2c 的持久降级签名。
        std::printf("SKIP: IMU motion stream blocked by iio scan_element permissions "
                    "(install librealsense udev rules, then rerun); last error: %s\n",
                    failureMessage.c_str());
        return 77;
    }
    if (!startedEvent && service.state() == CameraServiceState::Waiting) {
        // DEC-006：无设备时 worker 停在 Waiting 稳态（真机人工验收覆盖热插拔恢复）。
        std::printf("SKIP: no RealSense device (state=Waiting after 5s, last event '%s')\n",
                    event.message.c_str());
        return 77;
    }
    RIN_CHECK(startedEvent);
    if (!startedEvent) {
        std::printf(
            "FAIL: Started event not observed within 5s (state=%s, last event kind=%d "
            "message='%s', lastError='%s')\n",
            rin::toString(service.state()), static_cast<int>(event.kind), event.message.c_str(),
            service.lastError().c_str());
        return 1;
    }
    std::printf("hardware: Started event after %.0f ms (message='%s')\n", elapsedMs(),
                startedMessage.c_str());

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
    RIN_CHECK(rgbArrived);
    RIN_CHECK(depthArrived);
    if (rgbArrived) {
        RIN_CHECK(rgbFrame.valid());
        RIN_CHECK(rgbFrame.kind == FrameKind::Rgb);
        RIN_CHECK(rgbFrame.width > 0);
        RIN_CHECK(rgbFrame.height > 0);
        RIN_CHECK(rgbFrame.sequence > 0);
        RIN_CHECK_EQ(rgbFrame.width, 640u);
        RIN_CHECK_EQ(rgbFrame.height, 480u);
    }
    if (depthArrived) {
        RIN_CHECK(depthFrame.valid());
        RIN_CHECK(depthFrame.kind == FrameKind::Depth);
        RIN_CHECK(depthFrame.width > 0);
        RIN_CHECK(depthFrame.height > 0);
        RIN_CHECK(depthFrame.sequence > 0);
        RIN_CHECK_EQ(depthFrame.width, 640u);
        RIN_CHECK_EQ(depthFrame.height, 480u);
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
            RIN_CHECK(frame.valid());
            RIN_CHECK_EQ(frame.width, 640u);   // 采样期档位未变，尺寸断言逐帧维持
            RIN_CHECK_EQ(frame.height, 480u);
            if (haveSequence) {
                RIN_CHECK(frame.sequence > previousSequence);  // 邮箱序号严格单调
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
        RIN_CHECK(framesSeen >= 2);  // 变化检测至少需要两帧
        RIN_CHECK(checksumVariation);
        RIN_CHECK(maxRatio >= threshold);
    };
    sampleContent(FrameKind::Rgb, rgbSequence, kRgbMinNonZeroRatio, "rgb");
    sampleContent(FrameKind::Depth, depthSequence, kDepthMinNonZeroRatio, "depth");

    // --- 2c) M3-04 motion 通道：ACCEL/GYRO 原始采样与恒等姿态快照经通道发布 ---
    // enableMotion 且设备具备 IMU（D435if）：采集 worker 的 motion 分支把原始采样
    // 与 IdentityImuFuser 姿态快照投递到 motion/pose 通道；源频率 EMA 在数百毫秒内
    // 收敛为正，会话累计样本数为正。
    {
        rin::MotionSample motion;
        std::uint64_t motionSeq = 0;
        // D435IF 实测（2026-09-24 frameset 组成探针）：ACCEL/GYRO 在同一 frameset 内
        // 按固定顺序（ACCEL→GYRO）成对到达（137/137 帧形态 "AG"，~27-30 Hz/源），
        // 单值诊断通道的最新态恒为 GYRO——ACCEL 保持最新态仅到同帧组内下一条采样
        // （微秒级），轮询节奏无法可靠观察。故通道到达判据为"诊断通道有采样流动
        // 且样本有效"；ACCEL/GYRO 双源活性由姿态快照的分源会话计数与实测频率承载
        // （gyroSamples/accelSamples 仅在 ingest 分源累加，下方断言强制双源为正）。
        const bool motionArrived = pollUntil(5s, [&] {
            return service.tryLoadMotion(motionSeq, motion);
        });

        // IMU 前置缺失降级签名（M3-04 降级语义的测试侧对应，见文件头）：适配器在
        // 含运动流的 pipeline.start 失败（典型：无 root 时 HID/IIO scan_element
        // 权限前置）时一次性降级为纯视频并照常 Started，服务不再进入 Failed——
        // 旧"等 Failed 事件"的 SKIP 判据不可达，且降级 Info 事件被 Started 在
        // latest-state 事件邮箱覆盖，不可作判据。改用持久可观察签名，三者同时
        // 成立才判为环境前置缺失（SKIP 77）：
        //   (a) 目录 imuSupported==true（设备确有 IMU；枚举只查传感器能力，与
        //       pipeline.start 成败无关）；
        //   (b) 有界窗口内 motion 通道零采样（motionActive_==false 的直接证据）；
        //   (c) 最新 IntrinsicsSnapshot 的 gyroToColor/gyroIntrinsics 均无效
        //      （无运动流快照签名，camera_types.hpp 契约"全零无效可观察"）。
        // 只要不满足其一即按既有失败路径处理：真机运动流真实断裂（此时快照由
        // motion=true 路径发布、外参有效）或无 IMU 设备（imuSupported==false，
        // 既有退化语义：目录断言按原样暴露）都不被误判为 SKIP。
        if (!motionArrived) {
            rin::DeviceCatalog catalog;
            std::uint64_t catalogSeq = 0;
            rin::IntrinsicsSnapshot intrinsics;
            std::uint64_t intrinsicsSeq = 0;
            // 目录与快照先于 Started 发布（resolveTarget 枚举 + streamLoop 出流前
            // 快照）；此处仅读取不作断言，取不到按签名不成立处理（保守走失败路径）。
            const bool catalogSeen =
                pollUntil(1s, [&] { return service.tryLoadCatalog(catalogSeq, catalog); });
            const bool snapshotSeen = pollUntil(1s, [&] {
                return service.tryLoadIntrinsics(intrinsicsSeq, intrinsics);
            });
            bool activeImuSupported = false;
            if (catalogSeen) {
                for (const rin::DeviceInfo& device : catalog.devices) {
                    if (device.serial == catalog.activeSerial) {
                        activeImuSupported = device.imuSupported;
                    }
                }
            }
            if (catalogSeen && snapshotSeen && activeImuSupported &&
                !intrinsics.gyroToColor.valid() && !intrinsics.gyroIntrinsics.valid()) {
                std::printf(
                    "SKIP: IMU motion stream unavailable (adapter degraded to video only: "
                    "imuSupported=1 but no motion samples within 5s and no motion "
                    "intrinsics in latest snapshot) — install librealsense udev rules "
                    "(upstream scripts/setup_udev_rules.sh) or equivalent HID/IIO "
                    "scan_element authorization, then rerun\n");
                return 77;
            }
        }
        RIN_CHECK(motionArrived);
        if (motionArrived) {
            RIN_CHECK(motion.valid());
            RIN_CHECK(motion.deviceTimestampMs >= 0.0);
            std::printf("hardware: motion sample kind=%s ts=%.1f ms (channel flowing)\n",
                        motion.kind == rin::MotionStreamKind::Gyro ? "gyro" : "accel",
                        motion.deviceTimestampMs);
        }

        rin::ImuSnapshot pose;
        std::uint64_t poseSeq = 0;
        const bool poseArrived =
            pollUntil(5s, [&] { return service.tryLoadPose(poseSeq, pose); });
        RIN_CHECK(poseArrived);
        if (poseArrived) {
            RIN_CHECK(pose.valid());
            // M3-05 起 createImuFuser() 返回 Mahony 真身（DEC-010）：姿态随真实
            // 运动变化，恒等断言不再成立；真机噪声下契约约束为单位四元数
            // （ImuSnapshot::valid() 已含 norm² 容差 1e-3，此处显式复核数值面）。
            const double poseNormSq =
                static_cast<double>(pose.orientation[0]) * pose.orientation[0] +
                static_cast<double>(pose.orientation[1]) * pose.orientation[1] +
                static_cast<double>(pose.orientation[2]) * pose.orientation[2] +
                static_cast<double>(pose.orientation[3]) * pose.orientation[3];
            RIN_CHECK(std::fabs(poseNormSq - 1.0) <= 1e-3);
            RIN_CHECK(pose.sources.gyroHz > 0.0f);
            RIN_CHECK(pose.sources.accelHz > 0.0f);
            RIN_CHECK(pose.sources.gyroSamples > 0);
            RIN_CHECK(pose.sources.accelSamples > 0);
            preMotionSessionSeq = motion.sequence;
            prePoseSessionSeq = pose.sequence;
            preGyroSamples = pose.sources.gyroSamples;
            preAccelSamples = pose.sources.accelSamples;
            std::printf(
                "hardware: pose seq %llu unit-norm (gyro %.1f Hz / %llu samples, accel "
                "%.1f Hz / %llu samples)\n",
                static_cast<unsigned long long>(pose.sequence), pose.sources.gyroHz,
                static_cast<unsigned long long>(pose.sources.gyroSamples),
                pose.sources.accelHz, static_cast<unsigned long long>(pose.sources.accelSamples));
        }
    }

    // --- 3) 内参快照 color/depth valid()，且与当前档位一致 ---
    rin::IntrinsicsSnapshot intrinsics;
    std::uint64_t intrinsicsSequence = 0;
    const bool intrinsicsArrived = pollUntil(5s, [&] {
        return service.tryLoadIntrinsics(intrinsicsSequence, intrinsics);
    });
    RIN_CHECK(intrinsicsArrived);
    if (intrinsicsArrived) {
        RIN_CHECK(intrinsics.color.valid());
        RIN_CHECK(intrinsics.depth.valid());
        RIN_CHECK_EQ(intrinsics.color.width, 640u);
        RIN_CHECK_EQ(intrinsics.color.height, 480u);
        RIN_CHECK_EQ(intrinsics.depth.width, 640u);
        RIN_CHECK_EQ(intrinsics.depth.height, 480u);
        std::printf("hardware: intrinsics color %ux%u (fx=%.1f), depth %ux%u (fx=%.1f)\n",
                    intrinsics.color.width, intrinsics.color.height, intrinsics.color.fx,
                    intrinsics.depth.width, intrinsics.depth.height, intrinsics.depth.fx);

        // --- M3-04：motion intrinsics 与 gyro→color 外参链（D435if 读取成功形态）。
        // 内容级防线：全零旋转不可能是真实外参、全零刻度不可能是真实出厂内参——
        // 即使 valid() 放行也能拦住"读取失败被静默当成功"（全零应可观察为无效，
        // 契约面由 test_motion_intrinsics.cpp 锁定）。
        RIN_CHECK(intrinsics.gyroToColor.valid());
        RIN_CHECK(intrinsics.accelIntrinsics.valid());
        RIN_CHECK(intrinsics.gyroIntrinsics.valid());
        bool gyroRotationNonZero = false;
        for (const float value : intrinsics.gyroToColor.rotation) {
            gyroRotationNonZero = gyroRotationNonZero || value != 0.0f;
        }
        RIN_CHECK(gyroRotationNonZero);
        RIN_CHECK(intrinsics.accelIntrinsics.scale[0] != 0.0f);
        RIN_CHECK(intrinsics.accelIntrinsics.scale[8] != 0.0f);
        RIN_CHECK(intrinsics.gyroIntrinsics.scale[0] != 0.0f);
        RIN_CHECK(intrinsics.gyroIntrinsics.scale[8] != 0.0f);
        std::printf(
            "hardware: gyro->color t=(%.4f, %.4f, %.4f); scale diag accel (%.4f, %.4f, "
            "%.4f) gyro (%.4f, %.4f, %.4f)\n",
            intrinsics.gyroToColor.translation[0], intrinsics.gyroToColor.translation[1],
            intrinsics.gyroToColor.translation[2], intrinsics.accelIntrinsics.scale[0],
            intrinsics.accelIntrinsics.scale[4], intrinsics.accelIntrinsics.scale[8],
            intrinsics.gyroIntrinsics.scale[0], intrinsics.gyroIntrinsics.scale[4],
            intrinsics.gyroIntrinsics.scale[8]);
    }

    // --- 4) 设备目录（DEC-006）：单设备自动选择、目录项完整、与帧流来源一致 ---
    rin::DeviceCatalog caps;
    std::uint64_t capsSequence = 0;
    const bool capsArrived = pollUntil(5s, [&] {
        return service.tryLoadCatalog(capsSequence, caps);
    });
    RIN_CHECK(capsArrived);
    std::string activeSerial;
    if (capsArrived) {
        RIN_CHECK_EQ(caps.devices.size(), std::size_t{1});  // 本台架单设备；多设备由人工验收
        RIN_CHECK(caps.activeIsAuto);  // 唯一设备 → 自动选择（DEC-006）
        const rin::DeviceInfo& device = caps.devices.front();
        activeSerial = device.serial;
        RIN_CHECK(!activeSerial.empty());
        RIN_CHECK_EQ(caps.activeSerial, activeSerial);
        RIN_CHECK(!device.name.empty());
        RIN_CHECK(!device.firmwareVersion.empty());
        RIN_CHECK(!device.colorOptions.empty());
        RIN_CHECK(!device.depthOptions.empty());
        // M3-04：D435if 内置 IMU——目录上报 imuSupported 与升序速率档位（混合
        // pipeline 的设备能力依据，采集侧 deviceHasImu 查询该目录）。
        RIN_CHECK(device.imuSupported);
        RIN_CHECK(!device.imuAccelRatesHz.empty());
        RIN_CHECK(!device.imuGyroRatesHz.empty());
        for (std::size_t index = 1; index < device.imuAccelRatesHz.size(); ++index) {
            RIN_CHECK(device.imuAccelRatesHz[index] > device.imuAccelRatesHz[index - 1]);
        }
        for (std::size_t index = 1; index < device.imuGyroRatesHz.size(); ++index) {
            RIN_CHECK(device.imuGyroRatesHz[index] > device.imuGyroRatesHz[index - 1]);
        }
        std::printf("hardware: catalog imuSupported=%d accel rates:", device.imuSupported ? 1 : 0);
        for (const std::uint32_t rate : device.imuAccelRatesHz) {
            std::printf(" %u", rate);
        }
        std::printf(", gyro rates:");
        for (const std::uint32_t rate : device.imuGyroRatesHz) {
            std::printf(" %u", rate);
        }
        std::printf("\n");
        // 帧流来源一致性：Started 事件携带 "streaming <serial>"（适配器契约）。
        RIN_CHECK(startedMessage.find(activeSerial) != std::string::npos);
        std::printf(
            "hardware: catalog device='%s' serial='%s' firmware='%s' auto=%d "
            "(color options=%zu, depth options=%zu)\n",
            device.name.c_str(), device.serial.c_str(), device.firmwareVersion.c_str(),
            caps.activeIsAuto ? 1 : 0, device.colorOptions.size(), device.depthOptions.size());
    }

    // --- 4b) requestDevice(活动序列号) 幂等：不中断流、不重开管线（DEC-006）---
    {
        // 选择当前活动设备：粘性意图生效（目录 activeIsAuto 应翻转为 false），
        // 但同设备选择不得触发 restream——帧序号持续前移即流未被中断的直接证据。
        std::string selectError = "<untouched>";
        RIN_CHECK(service.requestDevice(activeSerial, &selectError));
        Frame frame;
        std::uint64_t selectRgbSeq = rgbSequence;
        const bool rgbContinued = pollUntil(3s, [&] {
            return service.tryLoadFrame(FrameKind::Rgb, selectRgbSeq, frame) &&
                   frame.sequence > rgbSequence;
        });
        RIN_CHECK(rgbContinued);  // 选择命令消费后帧流继续（无重开窗口）
        rin::DeviceCatalog afterSelect;
        std::uint64_t selectCatalogSeq = 0;
        const bool catalogUpdated = pollUntil(3s, [&] {
            if (!service.tryLoadCatalog(selectCatalogSeq, afterSelect)) {
                return false;
            }
            return !afterSelect.activeSerial.empty() && !afterSelect.activeIsAuto;
        });
        RIN_CHECK(catalogUpdated);
        if (catalogUpdated) {
            RIN_CHECK_EQ(afterSelect.devices.size(), std::size_t{1});
            RIN_CHECK_EQ(afterSelect.activeSerial, activeSerial);  // 活动设备未变
            RIN_CHECK(!afterSelect.activeIsAuto);  // 用户指定（粘性）
            std::printf("hardware: requestDevice(%s) sticky, frames continued, auto=%d\n",
                        activeSerial.c_str(), afterSelect.activeIsAuto ? 1 : 0);
        }
        if (rgbContinued) {
            RIN_CHECK(service.tryLoadEvent(event));
            RIN_CHECK(event.kind == ServiceEventKind::Info);  // 同设备选择仅发 Info
            RIN_CHECK_EQ(service.state(), CameraServiceState::Streaming);
        }
        // 深度流同样保持存活（双流契约）。
        std::uint64_t selectDepthSeq = depthSequence;
        const bool depthContinued = pollUntil(3s, [&] {
            return service.tryLoadFrame(FrameKind::Depth, selectDepthSeq, frame) &&
                   frame.sequence > depthSequence;
        });
        RIN_CHECK(depthContinued);
    }

    // --- 4c) 深度配色运行时切换（DEC-007）：Grayscale 生效不重流、事件消息精确、
    //     帧内容满足灰度性质；切回 Jet 恢复伪彩性质；同值幂等不打断流 ---
    {
        RIN_CHECK_EQ(service.state(), CameraServiceState::Streaming);

        // (a) Streaming 下切到 Grayscale：请求被接受（返回 true；error 仅在拒绝时回填，
        //     接受路径对 error 不作约定，与 requestDevice 测试口径一致）。
        std::string grayError = "<untouched>";
        RIN_CHECK(service.requestDepthColorScheme(rin::DepthColorScheme::Grayscale, &grayError));

        // (b) 有界窗口（3s）等到 "depth palette: grayscale" Info 事件（消息精确匹配）；
        //     期间状态持续采样保持 Streaming——配色切换不得进入 Restreaming 停启管线。
        bool grayEventSeen = false;
        bool grayStateDeviated = false;
        pollUntil(3s, [&] {
            if (service.state() != CameraServiceState::Streaming) {
                grayStateDeviated = true;
                return true;
            }
            if (service.tryLoadEvent(event) && event.kind == ServiceEventKind::Info &&
                event.message == "depth palette: grayscale") {
                grayEventSeen = true;
                return true;
            }
            return false;
        });
        RIN_CHECK(!grayStateDeviated);
        RIN_CHECK(grayEventSeen);
        RIN_CHECK_EQ(service.state(), CameraServiceState::Streaming);

        // (c) 事件后逐帧采样（切换边界前发布的最后一帧可能是残余 jet 帧）：
        //     找到首个整帧满足灰度性质的深度帧——所有像素 R==G==B 且至少一个非黑像素。
        bool grayFrameVerified = false;
        {
            Frame paletteFrame;
            std::uint64_t paletteSeq = depthSequence;
            const bool gotGrayFrame = pollUntil(3s, [&] {
                if (service.state() != CameraServiceState::Streaming) {
                    return true;  // 状态偏移交给下方断言取证
                }
                if (!service.tryLoadFrame(FrameKind::Depth, paletteSeq, paletteFrame)) {
                    return false;
                }
                RIN_CHECK(paletteFrame.valid());
                if (const GrayscaleCheck check = grayscaleProperty(paletteFrame);
                    check.allGray && check.hasNonBlack) {
                    grayFrameVerified = true;
                    return true;
                }
                return false;  // 切换边界残余帧，继续等下一帧
            });
            RIN_CHECK_EQ(service.state(), CameraServiceState::Streaming);
            RIN_CHECK(gotGrayFrame);
            RIN_CHECK(grayFrameVerified);
            depthSequence = paletteSeq;
        }
        std::printf(
            "hardware: depth palette grayscale applied at %.0f ms (frame content verified)\n",
            elapsedMs());

        // (d) 同值幂等：重复 Grayscale 被接受、不打断流；至少两个新帧（保证重复命令
        //     已被 worker 在帧检查点消费）后无新 palette 事件产生。
        {
            std::string idempotentError = "<untouched>";
            RIN_CHECK(service.requestDepthColorScheme(rin::DepthColorScheme::Grayscale,
                                                      &idempotentError));
            Frame frame;
            std::uint64_t idempotentSeq = depthSequence;
            const bool framesContinued = pollUntil(3s, [&] {
                RIN_CHECK_EQ(service.state(), CameraServiceState::Streaming);
                return service.tryLoadFrame(FrameKind::Depth, idempotentSeq, frame) &&
                       frame.sequence > depthSequence + 1;
            });
            RIN_CHECK(framesContinued);
            depthSequence = idempotentSeq;
            // 同值命令消费后不得重复发事件：最近事件仍是 (b) 的 palette Info。
            RIN_CHECK(service.tryLoadEvent(event));
            RIN_CHECK_EQ(event.message, std::string("depth palette: grayscale"));
            std::printf("hardware: duplicate grayscale request idempotent at %.0f ms\n",
                        elapsedMs());
        }

        // (e) 切回 Jet：接受、不重流；事件消息精确 "depth palette: jet"；
        //     其后深度帧存在彩色像素（伪彩性质恢复；jet 任何像素都不满足 R==G==B）。
        {
            std::string jetError = "<untouched>";
            RIN_CHECK(service.requestDepthColorScheme(rin::DepthColorScheme::Jet, &jetError));
            bool jetEventSeen = false;
            pollUntil(3s, [&] {
                if (service.state() != CameraServiceState::Streaming) {
                    return true;  // 状态偏移交给下方断言取证
                }
                if (service.tryLoadEvent(event) && event.kind == ServiceEventKind::Info &&
                    event.message == "depth palette: jet") {
                    jetEventSeen = true;
                    return true;
                }
                return false;
            });
            RIN_CHECK_EQ(service.state(), CameraServiceState::Streaming);
            RIN_CHECK(jetEventSeen);
            RIN_CHECK_EQ(event.message, std::string("depth palette: jet"));

            bool chromaticVerified = false;
            Frame paletteFrame;
            std::uint64_t jetSeq = depthSequence;
            const bool gotChromatic = pollUntil(3s, [&] {
                if (service.state() != CameraServiceState::Streaming) {
                    return true;
                }
                if (!service.tryLoadFrame(FrameKind::Depth, jetSeq, paletteFrame)) {
                    return false;
                }
                RIN_CHECK(paletteFrame.valid());
                if (hasChromaticPixel(paletteFrame)) {
                    chromaticVerified = true;
                    return true;
                }
                return false;  // 切换边界残余灰度帧，继续等下一帧
            });
            RIN_CHECK_EQ(service.state(), CameraServiceState::Streaming);
            RIN_CHECK(gotChromatic);
            RIN_CHECK(chromaticVerified);
            depthSequence = jetSeq;
            std::printf(
                "hardware: depth palette jet restored at %.0f ms (chromatic pixel verified)\n",
                elapsedMs());
        }
    }

    // --- 4d) M3-08 真机记录：IMU 出流频率（EMA 稳态 × 墙钟窗口交叉验证）---
    // DEC-010 明确不对真机提出数值阈值；交付频率与设备档位的对照只打印记录
    // 不断言（本机实测：syncer 按视频帧率配对交付 ~30 Hz/源，≠ ODR 档位；
    // 交付速率与 DEC-010 假设的偏差交 owner 按 DEC-010 复核裁决）。
    SourceRateMeasurement preRestreamRates;
    bool preRatesMeasured = false;
    if (preGyroSamples > 0) {
        std::uint64_t freqPoseSeq = prePoseSessionSeq;
        preRatesMeasured =
            measureSourceRates(service, freqPoseSeq, preGyroSamples, preRestreamRates);
        RIN_CHECK(preRatesMeasured);
        if (preRatesMeasured) {
            checkRateIntegrity(preRestreamRates, "pre-restream");
            // 档位对照记录（只记录；对照结论由验收记录承载）。
            if (capsArrived && !caps.devices.empty()) {
                std::printf("hardware: M3-08 device ODR gears accel:");
                for (const std::uint32_t rate : caps.devices.front().imuAccelRatesHz) {
                    std::printf(" %u", rate);
                }
                std::printf(", gyro:");
                for (const std::uint32_t rate : caps.devices.front().imuGyroRatesHz) {
                    std::printf(" %u", rate);
                }
                std::printf(" | delivered rate is syncer video-paced pairing; gear match "
                            "is a record, not an assertion\n");
            }
        }
    }

    // --- 5) requestResolution 848x480：统一 6s 期限等 ResolutionChanged + 新帧 ---
    std::string requestError = "<untouched>";
    const std::uint64_t rgbSeqBeforeChange = rgbSequence;
    const std::uint64_t depthSeqBeforeChange = depthSequence;
    RIN_CHECK(service.requestResolution(altRequest, &requestError));
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
    RIN_CHECK(changedEvent);
    RIN_CHECK(rgb848Arrived);
    RIN_CHECK(depth848Arrived);

    // --- 5b) restream 后占比断言：有界窗口取最大（容忍切档后孤立瞬态坏帧）---
    // 实测：管线重开后个别深度帧有效率瞬时塌陷（如 1.6%），1-2 帧内恢复；
    // 单帧断言会假阳性，故与 640 段同策略：最多 kContentSampleFrames 帧 / 3s 取最大。
    const auto sampleMaxRatioAfterRestream = [&](FrameKind kind, std::uint64_t& mailboxSequence,
                                                 const char* label) {
        double maxRatio = 0.0;
        int framesSeen = 0;
        Frame frame;
        pollUntil(3s, [&] {
            if (framesSeen >= kContentSampleFrames) {
                return true;
            }
            if (!service.tryLoadFrame(kind, mailboxSequence, frame)) {
                return false;
            }
            ++framesSeen;
            RIN_CHECK(frame.valid());
            RIN_CHECK_EQ(frame.width, 848u);
            RIN_CHECK_EQ(frame.height, 480u);
            if (const double ratio = contentNonZeroRatio(frame); ratio > maxRatio) {
                maxRatio = ratio;
            }
            return false;  // 采满窗口才结束
        });
        std::printf("hardware: %s post-restream ratio over %d frames: max %.1f%%\n", label,
                    framesSeen, maxRatio * 100.0);
        return maxRatio;
    };

    if (rgb848Arrived) {
        RIN_CHECK(rgb848.valid());
        RIN_CHECK_EQ(rgb848.width, 848u);
        RIN_CHECK_EQ(rgb848.height, 480u);
        RIN_CHECK(rgb848.sequence > rgbSeqBeforeChange);  // 跨 restream 序号前移
        const double ratio = sampleMaxRatioAfterRestream(FrameKind::Rgb, rgbSequence, "rgb");
        RIN_CHECK(ratio >= kRgbMinNonZeroRatio);
    }
    if (depth848Arrived) {
        RIN_CHECK(depth848.valid());
        RIN_CHECK_EQ(depth848.width, 848u);
        RIN_CHECK_EQ(depth848.height, 480u);
        RIN_CHECK(depth848.sequence > depthSeqBeforeChange);
        const double ratio = sampleMaxRatioAfterRestream(FrameKind::Depth, depthSequence, "depth");
        RIN_CHECK(ratio >= kDepthMinNonZeroRatio);
    }
    std::printf("hardware: ResolutionChanged after %.0f ms, 848x480 rgb=%s depth=%s\n", changedMs,
                rgb848Arrived ? "ok" : "missing", depth848Arrived ? "ok" : "missing");

    // --- 5c) M3-04 restream 重建含 IMU：运动/姿态通道恢复发布，会话序号不回退，
    //     会话累计样本数只增（resetStreamState 只复位融合器与频率窗口，序号与统计
    //     跨重建连续——消费方 lastSeen 序号不回退的契约）。
    {
        rin::MotionSample motion;
        std::uint64_t motionSeq = 0;
        const bool motionResumed = pollUntil(5s, [&] {
            return service.tryLoadMotion(motionSeq, motion) &&
                   motion.sequence > preMotionSessionSeq;
        });
        RIN_CHECK(motionResumed);

        rin::ImuSnapshot pose;
        std::uint64_t poseSeq = 0;
        const bool poseResumed = pollUntil(5s, [&] {
            return service.tryLoadPose(poseSeq, pose) && pose.sequence > prePoseSessionSeq;
        });
        RIN_CHECK(poseResumed);
        if (poseResumed) {
            RIN_CHECK(pose.valid());
            // M3-05（DEC-010）起 Mahony 真身复位后重新收敛，姿态随真实运动变化，
            // 恒等断言不再成立；与 2c 首检同契约：单位四元数（norm² 容差 1e-3）。
            const double poseNormSq =
                static_cast<double>(pose.orientation[0]) * pose.orientation[0] +
                static_cast<double>(pose.orientation[1]) * pose.orientation[1] +
                static_cast<double>(pose.orientation[2]) * pose.orientation[2] +
                static_cast<double>(pose.orientation[3]) * pose.orientation[3];
            RIN_CHECK(std::fabs(poseNormSq - 1.0) <= 1e-3);
            RIN_CHECK(pose.sources.gyroSamples >= preGyroSamples);
            RIN_CHECK(pose.sources.accelSamples >= preAccelSamples);
            std::printf(
                "hardware: motion resumed post-restream (motion seq %llu > %llu, pose seq "
                "%llu > %llu, gyro samples %llu >= %llu, accel samples %llu >= %llu)\n",
                static_cast<unsigned long long>(motion.sequence),
                static_cast<unsigned long long>(preMotionSessionSeq),
                static_cast<unsigned long long>(pose.sequence),
                static_cast<unsigned long long>(prePoseSessionSeq),
                static_cast<unsigned long long>(pose.sources.gyroSamples),
                static_cast<unsigned long long>(preGyroSamples),
                static_cast<unsigned long long>(pose.sources.accelSamples),
                static_cast<unsigned long long>(preAccelSamples));

            // --- M3-08：restream 后源频率恢复——EMA（resetStreamState 清零重建）
            // 重新收敛，且回到 restream 前同一量级（量级漂移 >2x 视为恢复异常；
            // 精确值仍只记录不设阈值，DEC-010 真机无数值阈值）。
            SourceRateMeasurement postRestreamRates;
            const bool postRatesMeasured = measureSourceRates(
                service, poseSeq, pose.sources.gyroSamples, postRestreamRates);
            RIN_CHECK(postRatesMeasured);
            if (postRatesMeasured) {
                checkRateIntegrity(postRestreamRates, "post-restream");
                if (preRatesMeasured) {
                    RIN_CHECK(postRestreamRates.gyroEmaHz >= 0.5 * preRestreamRates.gyroEmaHz &&
                              postRestreamRates.gyroEmaHz <= 2.0 * preRestreamRates.gyroEmaHz);
                    RIN_CHECK(postRestreamRates.accelEmaHz >=
                                  0.5 * preRestreamRates.accelEmaHz &&
                              postRestreamRates.accelEmaHz <= 2.0 * preRestreamRates.accelEmaHz);
                    std::printf(
                        "hardware: M3-08 post-restream rate recovery: gyro %.1fHz vs pre "
                        "%.1fHz, accel %.1fHz vs pre %.1fHz\n",
                        postRestreamRates.gyroEmaHz, preRestreamRates.gyroEmaHz,
                        postRestreamRates.accelEmaHz, preRestreamRates.accelEmaHz);
                }
            }
        }
    }

    // --- 6) stop() 收敛 Idle；二次 stop() 幂等不崩溃 ---
    service.stop();
    RIN_CHECK(service.state() == CameraServiceState::Idle);
    service.stop();
    RIN_CHECK(service.state() == CameraServiceState::Idle);
    ServiceEvent finalEvent;
    if (service.tryLoadEvent(finalEvent)) {
        RIN_CHECK(finalEvent.kind == ServiceEventKind::Stopped);
    }

    // --- 6b) 终态守卫：stop 后回到 Idle，配色请求再次被拒绝（DEC-007 状态门禁）---
    {
        std::string stoppedSchemeError = "<untouched>";
        RIN_CHECK(!service.requestDepthColorScheme(rin::DepthColorScheme::Grayscale,
                                                   &stoppedSchemeError));
        RIN_CHECK(!stoppedSchemeError.empty());
        RIN_CHECK(stoppedSchemeError.find("Idle") != std::string::npos);
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
        std::shared_ptr<ICameraService> service = rin::createRealSenseCameraService(executor);
        if (!service) {
            std::printf("FAIL: createRealSenseCameraService returned null\n");
            return 1;
        }
        status = runSmokeTest(*service);
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
