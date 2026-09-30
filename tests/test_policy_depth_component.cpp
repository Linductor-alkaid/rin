// M9 viewer 策略深度预览 tick 组件时序测试（独立验证，Independent-Verification-Agent）：
// apps/viewer/policy_depth_preview.hpp/.cpp（viewer::PolicyDepthPreview，无 EUI 类型，
// 可 headless 单测）。
//
// 语义判据（DEC-019 决策 3/5 + 组件头文件契约）：
//   - tick 链：tryLoadDepthMetric（"上次已见序号"最新态；水位为**邮箱发布序号**
//     ——每次 publish 单调 +1、跨 restream 持续递增，帧内 sample.sequence 只是
//     载荷字段且每次流启动重置；同一发布不被后续 tick 重复交付）→
//     流重启检测（载荷 sample.sequence < 上次值 → model.reset()，历史环不跨流
//     存续——依赖通道水位跨流单调，帧序号回绕才可观测）→
//     有效帧 model.process / 无效帧 droppedFrames++（显式计数，不静默）→
//     LatestMailbox 状态发布（tryLoadState 按 sequence > lastSeen 消费）。
//   - start：submit_periodic_cancellable_with_handle（20 ms ≈ 策略 50 Hz），幂等
//     （重复 start 无效果），准入失败（含 Executor 异常路径）转译返回 false 并
//     保持非运行态；stop：cancel handle，幂等；tick noexcept + busy_ RAII 复位
//     守卫（异常安全）；tick 闭包持弱引用（掉队 tick 生命周期闭合）；析构防御
//     （析构内 stop，实例先于 executor shutdown 停止或析构）。
//   - PolicyDepthPreviewState { snapshot, fps(EMA ≥ 0), droppedFrames }。
//
// 测试方式：组件经 make_shared 持有并用真实 executor::Executor start()（tick
// 受 running_ 门控，直接驱动以启动为前提；weak_from_this 要求 shared 持有），
// 时序语义由"发布 → 有界 pollUntil 收敛到期望状态"验证——20 ms 周期 tick 与
// 测试线程直接 tick() 的交错由 busy_ 原子门（防重叠）与服务米制通道邮箱发布
// 序号水位（每帧恰被消费一次）收敛为确定终态，全部等待有界不悬挂。脚本化
// 假服务（本文件自建最小 ICameraService 全纯虚实现）按帧发布米制帧 + stop
// 排空门。start/stop 幂等/取消/准入/析构防御经 Executor 周期任务注册表
// （get_all_periodic_task_status）与静默窗观察。
//
// 被测面与范围：
//   1) 无帧空转：周期 tick + 直接 tick 均不发布状态（静默窗）。
//   2) 有新帧：状态发布、snapshot 字段正确（sourceSequence/processedFrames/grid
//      几何/全格灰度）、fps ≥ 0（随后 > 0）；水位推进（无新帧 tick 不重发
//      状态，静默窗观察）。
//   3) 无效源帧：droppedFrames 显式计数且状态仍发布（不静默）、模型计数不动、
//      随后有效帧恢复处理。
//   4) 流重启复位：seq 回绕（5,6,7 → 2）触发 reset——historyResets 推进 +
//      复位后单帧首帧填充（8 平面全为新帧值；无复位时前 7 格应为旧帧值，
//      判别可区分）；严格小于语义（回绕后更高序号不再触发）。
//   5) service stop 排空门（运行中组件对停后服务不再取得新帧）；组件 stop 后
//      tick 短路（无新状态，此时仅直接驱动，确定性）；双 stop 幂等。
//   6) 真实 Executor 生命周期：start 准入成功、双 start 幂等（恰 1 个周期任务
//      且 20 ms 周期、execution_count 增长）、stop 取消（0 个运行中周期任务 +
//      静默窗无新状态）、重复 stop 幂等、start/stop/start 重启无任务泄漏
//      （恰 1 个运行中）、start 准入失败契约见证（已关闭 Executor 上 start 应
//      返回 false——当前实现抛 std::runtime_error，缺陷 D1 见证）、析构防御
//      （stop 前析构不悬挂 + shutdown(true) 有界收敛 + 弱引用不锁已析构实例）。
//
// 缺陷见证说明（历史记录；D1/D2 已由主循环修复，回归由本套件与
// test_depth_policy_preview.cpp 锁定）：
//   D1 start 准入失败契约：submit_periodic_cancellable_with_handle 在默认执行器
//      缺失/定时器已停止（shutdown 后）时抛 std::runtime_error，start 未捕获
//      转译 → 已关闭 Executor 上 start() 抛异常而非返回 false（§6 复现）。
//      修复：start 捕获转译 false（running 保持 false）。
//   D2 PolicyDepthPreviewModel::process 因 grid stride 单位错误（像素数 vs 行
//      字节数）恒抛 → 有效帧处理全部失败。修复：wrap 传 gridW×4 字节。
//   D3（测试侧）本文件假服务最初以帧内 sample.sequence 做消费水位，与 adapter
//      的邮箱发布序号语义分叉——流重启（载荷序号回绕）被 fake 永久过滤，
//      §4 超时。修复：发布计数器做水位判据，载荷原样交付。
//
// DOD-02 适用性说明（如实取舍）：正常完成——§2/§3/§4/§6 全链路（发布 → 周期/
// 直接 tick → 模型 → 状态邮箱消费）；任务异常——tick noexcept + busy_ RAII 复位
// 守卫（异常安全闭环），Executor 周期任务的 tick 异常进入其 failure 体系
// （executor 自测覆盖）；提交拒绝——start 准入契约（含 Executor 异常路径转译
// false）§6 显式覆盖；执行中取消——stop 的 handle.cancel + running 短路 +
// 弱引用短路（§5/§6）；超时——全部等待为有界轮询（2s 死限）/静默窗（300ms），
// 不悬挂；shutdown——§6 析构防御 + shutdown(true) Completed 收敛。

#include "policy_depth_preview.hpp"

#include "test_util.hpp"

#include <executor/executor.hpp>

#include <rin/camera_service.hpp>
#include <rin/camera_types.hpp>
#include <rin/depth_policy_preview.hpp>
#include <rin/depth_preproc.hpp>
#include <rin/image_types.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using rin::DepthFrameF32;
using rin::DepthMetricSample;
using viewer::PolicyDepthPreview;
using viewer::PolicyDepthPreviewState;

// --- 断言与有界等待辅助（与 test_run_control.cpp 同纪律） ----------------------

void runSection(const char* name, void (*fn)()) {
    std::printf("== %s\n", name);
    fn();
}

template <typename Pred>
[[nodiscard]] bool pollUntil(Pred&& pred,
                             std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return pred();
}

template <typename HasNew>
[[nodiscard]] bool quietFor(HasNew&& hasNew, std::chrono::milliseconds window) {
    const auto deadline = std::chrono::steady_clock::now() + window;
    while (std::chrono::steady_clock::now() < deadline) {
        if (hasNew()) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return true;
}

constexpr std::chrono::milliseconds kQuietWindow{300};

// 期望成功的直接 tick 包裹（加速收敛的辅助；周期任务交错由水位收敛）。契约外
// 异常（当前 = D2 经 tick 传播）记失败并返回 false。
bool tryTick(PolicyDepthPreview& component, const char* context) {
    try {
        component.tick();
        return true;
    } catch (const std::exception& error) {
        RIN_CHECK_MSG(false, std::string("tick 意外异常（") + context + "）: " + error.what());
        return false;
    }
}

// --- 脚本化假相机服务（本测试领域；米制通道按帧脚本发布） -----------------------
//
// 米制通道语义对齐 ICameraService 契约与 adapter 实现（realsense_camera_service
// 的 LatestMailbox::try_load_newer_than 同款）：**水位为邮箱发布序号**（每次
// publish 单调 +1，跨 restream 持续递增，本 fake 以发布计数器承载），帧内
// sample.sequence 只是载荷字段（每次流启动重置）——二者分离正是 DEC-019 决策 3
// 流重启检测的前提（通道水位跨流单调，帧序号回绕才可观测）。stop() 后恒 false
// （排空契约最小等价）。帧读取与周期 tick 线程之间的共享经 mutex（"非阻塞"
// 指不等待帧到达，mutex 短临界区满足）；测试线程同步发布（不创建 std::thread）。

class FakeMetricCameraService final : public rin::ICameraService {
public:
    // --- 测试脚本面 ---
    void publishMetric(DepthMetricSample sample) {
        const std::scoped_lock lock(mutex_);
        latestMetric_ = std::move(sample);
        ++publishCount_;  // 邮箱发布序号（消费水位判据），跨流单调。
        empty_ = false;
    }

    void publishMetricFrame(std::uint64_t sequence, DepthFrameF32 frame) {
        DepthMetricSample sample;
        sample.sequence = sequence;
        sample.deviceTimestampMs = static_cast<double>(sequence) * 33.0;
        sample.frame = std::move(frame);
        publishMetric(std::move(sample));
    }

    // --- ICameraService（组件只消费 tryLoadDepthMetric；其余为契约桩） ---
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
    }
    [[nodiscard]] rin::CameraServiceState state() const override {
        const std::scoped_lock lock(mutex_);
        return state_;
    }
    [[nodiscard]] std::string lastError() const override { return {}; }
    [[nodiscard]] bool tryLoadFrame(rin::FrameKind, std::uint64_t&, rin::Frame&) override {
        return false;
    }
    [[nodiscard]] bool tryLoadGrayFrame(rin::GrayFrameKind, std::uint64_t&,
                                        rin::GrayFrame&) override {
        return false;
    }
    [[nodiscard]] bool tryLoadDepthMetric(std::uint64_t& lastSeenSequence,
                                          DepthMetricSample& out) override {
        const std::scoped_lock lock(mutex_);
        if (stopped_) {
            return false;  // 排空契约：stop 返回后不再有新发布。
        }
        // 水位过滤以**邮箱发布序号**（publishCount_）为判据（与 adapter 的
        // LatestMailbox::try_load_newer_than 同语义）；帧内 sample.sequence 仅
        // 载荷原样交付（回绕不被过滤——流重启检测依赖此语义）。不按
        // sample.valid() 过滤：无效采样照常交付，由组件 droppedFrames 显式计数。
        if (empty_ || publishCount_ <= lastSeenSequence) {
            return false;
        }
        out = latestMetric_;
        lastSeenSequence = publishCount_;
        return true;
    }
    [[nodiscard]] bool tryLoadIntrinsics(std::uint64_t&, rin::IntrinsicsSnapshot&) override {
        return false;
    }
    [[nodiscard]] bool tryLoadMotion(std::uint64_t&, rin::MotionSample&) override { return false; }
    [[nodiscard]] bool tryLoadPose(std::uint64_t&, rin::ImuSnapshot&) override { return false; }
    [[nodiscard]] bool tryLoadCatalog(std::uint64_t&, rin::DeviceCatalog&) override {
        return false;
    }
    [[nodiscard]] bool tryLoadEvent(rin::ServiceEvent&) override { return false; }

private:
    mutable std::mutex mutex_;
    DepthMetricSample latestMetric_;
    std::uint64_t publishCount_ = 0;  // 邮箱发布序号（消费水位判据，跨流单调）。
    bool empty_ = true;               // 尚未发布过（"无生产者"哨兵）。
    bool stopped_ = false;
    rin::CameraServiceState state_ = rin::CameraServiceState::Idle;
};

// --- 米制帧与网格核对辅助 ------------------------------------------------------

// 构造常量米制帧（64×36 == 默认 raw 网格，O2 同尺寸恒等，输入最小开销）。
[[nodiscard]] DepthFrameF32 makeConstantMetricFrame(float meters) {
    constexpr std::uint32_t kWidth = 64;
    constexpr std::uint32_t kHeight = 36;
    std::vector<float> buffer(static_cast<std::size_t>(kWidth) * kHeight, meters);
    auto shared = std::make_shared<const std::vector<float>>(std::move(buffer));
    return DepthFrameF32::wrap(kWidth, kHeight, kWidth, std::move(shared));
}

// 构造无效米制帧（防御路径脚本输入）。
[[nodiscard]] DepthMetricSample makeInvalidMetricSample(std::uint64_t sequence) {
    DepthMetricSample sample;
    sample.sequence = sequence;
    sample.deviceTimestampMs = 1.0;
    return sample;  // frame 默认构造 → valid() false。
}

// 网格布局冻结常量（DEC-019 决策 4；与 test_depth_policy_preview.cpp 同判据）。
constexpr std::uint32_t kGridW = 512;
constexpr std::uint32_t kGridH = 576;
constexpr std::uint32_t kCellW = 256;
constexpr std::uint32_t kCellH = 144;

// 扫描第 cellIndex 格（0..7，阅读序）：RGB == expectedGray 且 alpha == 255。
// 网格无效（row() 为 nullptr，防御 D2 现场）返回 1（计为失配，由上层先定位）。
[[nodiscard]] std::uint64_t cellMismatches(const rin::ImageU8& grid, std::uint32_t cellIndex,
                                           std::uint8_t expectedGray) {
    const std::uint32_t col = cellIndex % 2;
    const std::uint32_t row = cellIndex / 2;
    std::uint64_t mismatches = 0;
    for (std::uint32_t y = 0; y < kCellH; ++y) {
        const std::uint8_t* rowPixels = grid.row(row * kCellH + y);
        if (rowPixels == nullptr) {
            return 1;
        }
        const std::uint8_t* pixels = rowPixels + static_cast<std::size_t>(col * kCellW) * 4u;
        for (std::uint32_t x = 0; x < kCellW; ++x) {
            const bool grayOk = pixels[x * 4u + 0] == expectedGray &&
                                pixels[x * 4u + 1] == expectedGray &&
                                pixels[x * 4u + 2] == expectedGray;
            if (!grayOk || pixels[x * 4u + 3] != 255u) {
                ++mismatches;
            }
        }
    }
    return mismatches;
}

}  // namespace

int main() {
    // --- 1) 无帧空转：周期 tick + 直接 tick 均不发布状态 -----------------------------
    runSection("无帧空转", [] {
        executor::Executor executor;
        RIN_CHECK(executor.initialize_ex({}));
        auto service = std::make_shared<FakeMetricCameraService>();
        auto component = std::make_shared<PolicyDepthPreview>(service);
        RIN_CHECK(component->start(executor));
        for (int i = 0; i < 3; ++i) {
            RIN_CHECK(tryTick(*component, "空转"));
        }
        std::uint64_t lastSeen = 0;
        const bool quiet = quietFor(
            [&] {
                std::uint64_t seen = lastSeen;
                PolicyDepthPreviewState probe;
                return component->tryLoadState(seen, probe);
            },
            kQuietWindow);
        RIN_CHECK_MSG(quiet, "无帧空转不发布状态");
        component->stop();
        executor.shutdown(true);
    });

    // --- 2) 有新帧：状态发布 + 字段正确 + 水位推进 + fps -----------------------------
    runSection("有新帧发布与水位", [] {
        executor::Executor executor;
        RIN_CHECK(executor.initialize_ex({}));
        auto service = std::make_shared<FakeMetricCameraService>();
        auto component = std::make_shared<PolicyDepthPreview>(service);
        RIN_CHECK(component->start(executor));

        // 第一帧：seq 1，恒 1.25 m（默认 config [0,2.5] → 归一化 0.5 → 全格 128）。
        service->publishMetricFrame(1, makeConstantMetricFrame(1.25f));
        tryTick(*component, "首帧加速");
        std::uint64_t lastSeen = 0;
        PolicyDepthPreviewState state;
        const bool published = pollUntil([&] { return component->tryLoadState(lastSeen, state); });
        if (!published) {
            RIN_CHECK_MSG(false, "首帧状态未发布（有界死限内未收敛）");
            component->stop();
            executor.shutdown(true);
            return;
        }
        // lastSeen 为状态邮箱自有水位（发布即推进），与源序号独立；此处首帧后
        // 水位已从 0 前进。
        RIN_CHECK(lastSeen > 0);
        RIN_CHECK_EQ(state.snapshot.sourceSequence, std::uint64_t{1});
        RIN_CHECK_EQ(state.snapshot.processedFrames, std::uint64_t{1});
        RIN_CHECK_EQ(state.snapshot.historyResets, std::uint64_t{0});
        RIN_CHECK(state.snapshot.valid());
        RIN_CHECK_EQ(state.snapshot.grid.width(), kGridW);
        RIN_CHECK_EQ(state.snapshot.grid.height(), kGridH);
        RIN_CHECK(state.fps >= 0.0);  // fps(EMA) 非负。
        for (std::uint32_t cell = 0; cell < 8; ++cell) {
            const std::uint64_t mismatches = cellMismatches(state.snapshot.grid, cell, 128u);
            RIN_CHECK_MSG(mismatches == 0, std::string("首帧平面 ") + std::to_string(cell) +
                                               " 全格 128（失配 " + std::to_string(mismatches) +
                                               " px）");
        }

        // 水位推进：无新帧 tick（周期 + 直接）不重发（静默窗）——"同帧不重发"
        // 的正确语义：同一邮箱发布（水位判据）不会被后续 tick 重复交付；重发
        // 同一载荷帧在邮箱语义下是新发布（会再交付），与生产者"每帧发布一次"
        // 行为不符，不做断言。
        tryTick(*component, "无新帧");
        const bool noRepeat = quietFor(
            [&] {
                std::uint64_t seen = lastSeen;
                PolicyDepthPreviewState probe;
                return component->tryLoadState(seen, probe);
            },
            kQuietWindow);
        RIN_CHECK_MSG(noRepeat, "无新帧 tick 不重发状态");

        // 后续帧：fps 推进（有帧间隔 → EMA 初值 > 0）；processedFrames 累计。
        bool fpsAdvanced = false;
        std::uint64_t expectedFrames = 1;
        for (std::uint64_t seq = 2; seq <= 6 && !fpsAdvanced; ++seq) {
            service->publishMetricFrame(seq, makeConstantMetricFrame(1.25f));
            tryTick(*component, "fps 推进加速");
            ++expectedFrames;
            PolicyDepthPreviewState next;
            std::uint64_t seen = lastSeen;
            if (pollUntil([&] { return component->tryLoadState(seen, next); })) {
                lastSeen = seen;
                RIN_CHECK_EQ(next.snapshot.processedFrames, expectedFrames);
                RIN_CHECK_EQ(next.snapshot.sourceSequence, seq);
                if (next.fps > 0.0 && std::isfinite(next.fps)) {
                    fpsAdvanced = true;
                }
            } else {
                RIN_CHECK_MSG(false, "后续帧状态未发布（有界死限内未收敛）");
                break;
            }
        }
        RIN_CHECK_MSG(fpsAdvanced, "帧间隔后 fps > 0（EMA 初值）");
        component->stop();
        executor.shutdown(true);
    });

    // --- 3) 无效源帧：droppedFrames 显式计数（不静默），模型计数不动 ------------------
    runSection("无效源帧丢弃计数", [] {
        executor::Executor executor;
        RIN_CHECK(executor.initialize_ex({}));
        auto service = std::make_shared<FakeMetricCameraService>();
        auto component = std::make_shared<PolicyDepthPreview>(service);
        RIN_CHECK(component->start(executor));

        service->publishMetric(makeInvalidMetricSample(3));
        tryTick(*component, "无效帧加速");
        std::uint64_t lastSeen = 0;
        PolicyDepthPreviewState state;
        const bool published = pollUntil([&] { return component->tryLoadState(lastSeen, state); });
        if (!published) {
            RIN_CHECK_MSG(false, "无效帧状态未发布");
            component->stop();
            executor.shutdown(true);
            return;
        }
        RIN_CHECK(lastSeen > 0);  // 状态邮箱水位前进（与源序号独立）。
        RIN_CHECK_EQ(state.droppedFrames, std::uint64_t{1});  // 显式计数（不静默）。
        RIN_CHECK_EQ(state.snapshot.processedFrames, std::uint64_t{0});  // 模型未推进。
        RIN_CHECK(!state.snapshot.valid());  // 从未处理帧 → grid 无效。

        // 连续无效帧累计。
        service->publishMetric(makeInvalidMetricSample(4));
        tryTick(*component, "无效帧 2 加速");
        bool secondDropSeen = false;
        const bool secondDropBounded = pollUntil([&] {
            PolicyDepthPreviewState probe;
            std::uint64_t seen = lastSeen;
            if (component->tryLoadState(seen, probe) && probe.droppedFrames == 2) {
                lastSeen = seen;
                secondDropSeen = true;
            }
            return secondDropSeen;
        });
        RIN_CHECK_MSG(secondDropSeen && secondDropBounded, "连续无效帧 droppedFrames 累计为 2");

        // 随后有效帧恢复正常处理（水位不受影响）。
        service->publishMetricFrame(5, makeConstantMetricFrame(1.25f));
        tryTick(*component, "恢复有效帧加速");
        PolicyDepthPreviewState state2;
        std::uint64_t seen = lastSeen;
        const bool recovered = pollUntil([&] { return component->tryLoadState(seen, state2); });
        if (!recovered) {
            RIN_CHECK_MSG(false, "恢复有效帧状态未发布（有界死限内未收敛）");
            component->stop();
            executor.shutdown(true);
            return;
        }
        RIN_CHECK_EQ(state2.droppedFrames, std::uint64_t{2});
        RIN_CHECK_EQ(state2.snapshot.processedFrames, std::uint64_t{1});
        RIN_CHECK_EQ(state2.snapshot.sourceSequence, std::uint64_t{5});
        component->stop();
        executor.shutdown(true);
    });

    // --- 4) 流重启复位：seq 回绕 → reset + 首帧填充可观测 ----------------------------
    runSection("流重启复位", [] {
        executor::Executor executor;
        RIN_CHECK(executor.initialize_ex({}));
        auto service = std::make_shared<FakeMetricCameraService>();
        auto component = std::make_shared<PolicyDepthPreview>(service);
        RIN_CHECK(component->start(executor));

        // 流 A：seq 5/6/7 恒 0.625 m（→ 全格 64）；欠帧首帧填充 → 全格 64。
        for (std::uint64_t seq = 5; seq <= 7; ++seq) {
            service->publishMetricFrame(seq, makeConstantMetricFrame(0.625f));
            tryTick(*component, "重启前加速");
        }
        std::uint64_t lastSeen = 0;
        PolicyDepthPreviewState before;
        const bool streamed = pollUntil([&] {
            if (component->tryLoadState(lastSeen, before) && before.snapshot.processedFrames == 3) {
                return true;
            }
            return false;
        });
        if (!streamed) {
            RIN_CHECK_MSG(false, "流 A 3 帧未收敛（有界死限内未收敛）");
            component->stop();
            executor.shutdown(true);
            return;
        }
        RIN_CHECK_EQ(before.snapshot.historyResets, std::uint64_t{0});
        for (std::uint32_t cell = 0; cell < 8; ++cell) {
            RIN_CHECK_EQ(cellMismatches(before.snapshot.grid, cell, 64u), std::uint64_t{0});
        }

        // 流重启：seq 2（< 7，回绕）恒 1.25 m → reset + 单帧首帧填充 → 全格 128。
        // 判别：若无复位，历史环含 3 个旧帧 → 前 7 格应为 64、仅末格 128。
        service->publishMetricFrame(2, makeConstantMetricFrame(1.25f));
        tryTick(*component, "重启帧加速");
        PolicyDepthPreviewState after;
        std::uint64_t seen = lastSeen;
        const bool restarted = pollUntil([&] {
            return component->tryLoadState(seen, after) && after.snapshot.historyResets == 1;
        });
        if (!restarted) {
            RIN_CHECK_MSG(false, "重启帧状态未发布/未复位（有界死限内未收敛）");
            component->stop();
            executor.shutdown(true);
            return;
        }
        RIN_CHECK(seen > lastSeen);  // 状态邮箱水位前进。
        RIN_CHECK_EQ(after.snapshot.processedFrames, std::uint64_t{4});  // 计数连续。
        for (std::uint32_t cell = 0; cell < 8; ++cell) {
            const std::uint64_t mismatches = cellMismatches(after.snapshot.grid, cell, 128u);
            RIN_CHECK_MSG(mismatches == 0, std::string("重启后平面 ") + std::to_string(cell) +
                                               " 全为新帧值 128（历史已复位；失配 " +
                                               std::to_string(mismatches) + " px）");
        }

        // 严格小于语义：回绕后更高序号（2 → 3）不触发再次复位。
        service->publishMetricFrame(3, makeConstantMetricFrame(1.25f));
        tryTick(*component, "重启后新帧加速");
        PolicyDepthPreviewState later;
        std::uint64_t seen2 = seen;
        const bool advanced = pollUntil([&] {
            return component->tryLoadState(seen2, later) && later.snapshot.processedFrames == 5;
        });
        if (advanced) {
            RIN_CHECK_EQ(later.snapshot.historyResets, std::uint64_t{1});
            RIN_CHECK_EQ(later.snapshot.processedFrames, std::uint64_t{5});
        } else {
            RIN_CHECK_MSG(false, "重启后新帧状态未发布（有界死限内未收敛）");
        }
        component->stop();
        executor.shutdown(true);
    });

    // --- 5) service stop 排空门 + 组件 stop 后 tick 短路 + 双 stop 幂等 --------------
    runSection("stop 短路与幂等", [] {
        executor::Executor executor;
        RIN_CHECK(executor.initialize_ex({}));
        auto service = std::make_shared<FakeMetricCameraService>();
        auto component = std::make_shared<PolicyDepthPreview>(service);
        RIN_CHECK(component->start(executor));

        // 基线状态：无效帧（不触发 process，与 D2 无关）→ 状态发布。
        service->publishMetric(makeInvalidMetricSample(1));
        std::uint64_t lastSeen = 0;
        PolicyDepthPreviewState state;
        const bool published = pollUntil([&] { return component->tryLoadState(lastSeen, state); });
        RIN_CHECK_MSG(published, "基线无效帧状态发布");
        RIN_CHECK_EQ(state.droppedFrames, std::uint64_t{1});

        // service stop 排空门：运行中组件对停后服务不再取得新帧（静默窗）。
        service->stop();
        service->publishMetricFrame(2, makeConstantMetricFrame(1.875f));  // 停后发布被拒。
        const bool drained = quietFor(
            [&] {
                std::uint64_t seen = lastSeen;
                PolicyDepthPreviewState probe;
                return component->tryLoadState(seen, probe);
            },
            kQuietWindow);
        RIN_CHECK_MSG(drained, "service stop 后组件无新状态（排空契约）");

        // 组件 stop：running 门短路（此后仅直接驱动，确定性）；双 stop 幂等。
        component->stop();
        RIN_CHECK(!component->running());
        component->stop();  // 幂等：重复 stop 安全。
        service->publishMetricFrame(3, makeConstantMetricFrame(1.875f));
        if (tryTick(*component, "stop 后")) {
            PolicyDepthPreviewState stopped;
            std::uint64_t seen = lastSeen;
            RIN_CHECK(!component->tryLoadState(seen, stopped));
            RIN_CHECK_EQ(seen, lastSeen);
        }
        executor.shutdown(true);
    });

    // --- 6) 真实 Executor 生命周期：准入/幂等/取消/重启/析构防御 ----------------------
    // 本节只发布无效帧（不触发 model.process，与 D2 现场解耦）；有效帧处理经
    // 真实周期任务覆盖见 §2-§4（同款 start + Executor 交错收敛）。
    runSection("真实 Executor 生命周期", [] {
        executor::Executor executor;
        RIN_CHECK(executor.initialize_ex({}));
        auto service = std::make_shared<FakeMetricCameraService>();
        auto component = std::make_shared<PolicyDepthPreview>(service);

        // start 准入成功 + 双 start 幂等（恰 1 个周期任务，20 ms 周期）。
        RIN_CHECK(component->start(executor));
        RIN_CHECK(component->running());
        RIN_CHECK(component->start(executor));  // 幂等：重复 start 无效果。
        const auto periodic = executor.get_all_periodic_task_status();
        RIN_CHECK_EQ(periodic.size(), std::size_t{1});
        RIN_CHECK(periodic.empty() || periodic.front().period_ms == 20);

        // 周期任务存活：空转 tick 执行计数有界窗内增长（≈ 50 Hz）。
        const std::uint64_t executionsBefore =
            periodic.empty() ? 0 : periodic.front().execution_count;
        const bool executionAdvanced = pollUntil([&] {
            const auto statuses = executor.get_all_periodic_task_status();
            return !statuses.empty() && statuses.front().execution_count > executionsBefore;
        });
        RIN_CHECK_MSG(executionAdvanced, "execution_count 增长（周期任务存活）");

        // 周期 tick 消费新帧（无效帧：状态发布且 droppedFrames 显式计数）。
        service->publishMetric(makeInvalidMetricSample(10));
        std::uint64_t lastSeen = 0;
        PolicyDepthPreviewState state;
        const bool tickFired = pollUntil([&] { return component->tryLoadState(lastSeen, state); });
        RIN_CHECK_MSG(tickFired, "周期 tick 在死限内消费新帧");
        if (tickFired) {
            RIN_CHECK_EQ(state.droppedFrames, std::uint64_t{1});
        }

        // stop 取消：0 个运行中周期任务（有界收敛）+ 静默窗无新状态；重复 stop 幂等。
        component->stop();
        RIN_CHECK(!component->running());
        const bool cancelObserved = pollUntil([&] {
            for (const auto& status : executor.get_all_periodic_task_status()) {
                if (status.is_running) {
                    return false;
                }
            }
            return true;
        });
        RIN_CHECK_MSG(cancelObserved, "stop 后无运行中周期任务（取消生效）");
        component->stop();  // 幂等。
        service->publishMetric(makeInvalidMetricSample(11));
        std::uint64_t quietSeen = lastSeen;
        const bool quiet = quietFor(
            [&] {
                PolicyDepthPreviewState probe;
                return component->tryLoadState(quietSeen, probe);
            },
            kQuietWindow);
        RIN_CHECK_MSG(quiet, "stop 后静默窗无新状态（取消生效）");

        // start/stop/start 重启：无任务泄漏（恰 1 个运行中周期任务）。
        RIN_CHECK(component->start(executor));
        const bool singleRunner = pollUntil([&] {
            std::size_t running = 0;
            for (const auto& status : executor.get_all_periodic_task_status()) {
                if (status.is_running) {
                    ++running;
                }
            }
            return running == 1;
        });
        RIN_CHECK_MSG(singleRunner, "重启后恰 1 个运行中周期任务（无泄漏）");
        component->stop();

        // D1 缺陷见证：契约"准入失败返回 false 并保持非运行态"；当前实现对已
        // 关闭 Executor 的 start() 抛 std::runtime_error（submit 异常未转译）。
        executor::Executor dead;
        RIN_CHECK(dead.initialize_ex({}));
        dead.shutdown(true);
        auto lateComponent = std::make_shared<PolicyDepthPreview>(service);
        bool returnedFalse = false;
        try {
            returnedFalse = lateComponent->start(dead);
        } catch (const std::exception& error) {
            RIN_CHECK_MSG(false, std::string("D1 start 应返回 false 而非抛异常: ") + error.what());
        }
        if (returnedFalse) {
            RIN_CHECK(!lateComponent->running());  // 保持非运行态。
        }

        // 析构防御：stop 前析构不悬挂（析构内 stop），弱引用不锁已析构实例，
        // shutdown(true) 有界收敛。
        auto doomed = std::make_shared<PolicyDepthPreview>(service);
        RIN_CHECK(doomed->start(executor));
        std::weak_ptr<PolicyDepthPreview> watch = doomed;
        doomed.reset();  // 析构内 stop：handle cancel，排队 tick 经弱引用短路。
        RIN_CHECK(watch.expired());
        service->publishMetric(makeInvalidMetricSample(12));  // 无组件消费。
        executor.shutdown(true);  // 有界等待完成（不悬挂）。
        RIN_CHECK(true);
    });

    return rin_test::exitStatus();
}
