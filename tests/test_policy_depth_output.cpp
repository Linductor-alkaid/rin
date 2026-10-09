// 策略深度单帧输出组件测试（DEC-022，Independent-Verification-Agent）：
// apps/viewer/policy_depth_output.hpp —— 米制深度通道 → O6 冻结组合（无历史
// 分片）→ 灰度显示帧邮箱。判定依据：
// - O6/显示数值 = depth_policy_preproc_design.md 冻结公式（PolicyDepthConfig
//   默认值 = roboparty E3-Parkour 部署参考）独立手推，不参考实现代码；
// - 组件语义 = DEC-019 决策 3 同源纪律（tick 直接驱动 headless、最新态邮箱
//   "上次已见序号"消费、失败显式丢弃计数）。
//
// 覆盖：常值帧端到端量化（0.5→128）、无效像素填充远距白（0→255）、最近邻
// 放大尺寸（32×18 ×16 → 512×288）与索引映射、显示帧序列号/时间戳透传、
// 邮箱"上次已见序号"门控（无新帧 false）、O6 拒绝（源小于 raw 网格）显式
// 丢弃、无效源帧显式丢弃、无服务防御、policyDepthDisplayGray 边界钳制。
// 单线程 tick 直接驱动，不创建线程（DOD-02 并发矩阵不适用；邮箱为组件
// 唯一跨上下文状态，其并发语义由 kairo::comm 契约保证）。

#include "fake_camera_service.hpp"
#include "policy_depth_output.hpp"
#include "test_util.hpp"

#include <rin/depth_preproc.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {

using rin_test::nearF;

/// 常值米制帧（width×height，全部像素 = value）。
[[nodiscard]] rin::DepthFrameF32 constantDepth(std::uint32_t width, std::uint32_t height,
                                               float value) {
    rin::DepthFrameF32 frame = rin::DepthFrameF32::make(width, height);
    for (std::uint32_t y = 0; y < height; ++y) {
        float* row = const_cast<float*>(frame.row(y));
        for (std::uint32_t x = 0; x < width; ++x) {
            row[x] = value;
        }
    }
    return frame;
}

}  // namespace

int main() {
    // --- 1) 常值帧端到端：0.5 → 灰度 128，显示 512×288，序列号透传 ---
    {
        auto service = std::make_shared<rin_test::FakeCameraService>();
        viewer::PolicyDepthOutput output(service);
        rin::DepthMetricSample sample;
        sample.sequence = 41;
        sample.deviceTimestampMs = 12.5;
        sample.frame = constantDepth(848, 480, 1.25f);
        service->publishMetric(std::move(sample));

        output.tick();
        std::uint64_t lastSeen = 0;
        rin::Frame frame;
        RIN_CHECK_MSG(output.tryLoadFrame(lastSeen, frame),
                      "O6 常值帧应产出一帧显示快照");
        RIN_CHECK_EQ(frame.width, 512u);
        RIN_CHECK_EQ(frame.height, 288u);
        RIN_CHECK_EQ(frame.stride, 512u * 4u);
        RIN_CHECK(frame.valid());
        RIN_CHECK_EQ(frame.sequence, 41u);
        RIN_CHECK(nearF(static_cast<float>(frame.deviceTimestampMs), 12.5f));
        // 1.25 m → O5 归一化 0.5 → ×255 = 127.5 → lround 128（灰度复制 + 不透明）。
        bool allMatch = true;
        const std::size_t count = static_cast<std::size_t>(frame.stride) * frame.height;
        for (std::size_t i = 0; i < count; i += 4) {
            const auto* pixel = &(*frame.pixels)[i];
            if (pixel[0] != 128 || pixel[1] != 128 || pixel[2] != 128 || pixel[3] != 255) {
                allMatch = false;
                break;
            }
        }
        RIN_CHECK_MSG(allMatch, "常值 1.25 m 帧应整帧显示为灰度 128（最近邻放大保持）");
    }

    // --- 2) 无效像素 0.0 → O1 填充 farValue = depthFar = 2.5 → 白 ---
    {
        auto service = std::make_shared<rin_test::FakeCameraService>();
        viewer::PolicyDepthOutput output(service);
        rin::DepthMetricSample sample;
        sample.sequence = 7;
        sample.frame = constantDepth(848, 480, 0.0f);
        service->publishMetric(std::move(sample));

        output.tick();
        std::uint64_t lastSeen = 0;
        rin::Frame frame;
        RIN_CHECK(output.tryLoadFrame(lastSeen, frame));
        const std::uint8_t firstGray = (*frame.pixels)[0];
        RIN_CHECK_MSG(firstGray == 255,
                      "无效像素 0.0 应经 O1 填充 2.5 m → 归一化 1.0 → 灰度 255");
    }

    // --- 3) 最近邻索引映射：非常值帧按 x/scale、y/scale 取样 ---
    {
        auto service = std::make_shared<rin_test::FakeCameraService>();
        viewer::PolicyDepthOutput output(service);
        // 直接走显示帧构造（不经 O6，隔离映射断言）：32×18 恰为策略网格尺寸，
        // 构造 O6 输出形态需要经过冻结组合——改用小尺寸直通：16×9 输入经
        // O6 后为 18×32（与输入尺寸无关），值域由裁切后内容决定。此处直接
        // 验证 policyDepthDisplayFrame 的映射：源 4×2、scale = 512/4 = 128 →
        // 512×256，(x,y) 采样 (x/128, y/128)。
        rin::DepthFrameF32 source = rin::DepthFrameF32::make(4, 2);
        const float values[2][4] = {{0.0f, 0.25f, 0.5f, 1.0f}, {1.0f, 0.75f, 0.5f, 0.25f}};
        for (std::uint32_t y = 0; y < 2; ++y) {
            float* row = const_cast<float*>(source.row(y));
            for (std::uint32_t x = 0; x < 4; ++x) {
                row[x] = values[y][x];
            }
        }
        const rin::Frame display = viewer::policyDepthDisplayFrame(source);
        RIN_CHECK_EQ(display.width, 512u);
        RIN_CHECK_EQ(display.height, 256u);
        // 中心取样点：块 (200, 100) → 源 (1, 0) → 0.25 → 64。
        const std::size_t offset = 100u * display.stride + 200u * 4u;
        RIN_CHECK_EQ((*display.pixels)[offset + 0], 64);
        // 块 (400, 200) → 源 (3, 1) → 0.25 → 64。
        const std::size_t offset2 = 200u * display.stride + 400u * 4u;
        RIN_CHECK_EQ((*display.pixels)[offset2 + 0], 64);
    }

    // --- 4) 邮箱门控：无新米制帧的 tick 不产新快照 ---
    {
        auto service = std::make_shared<rin_test::FakeCameraService>();
        viewer::PolicyDepthOutput output(service);
        rin::DepthMetricSample sample;
        sample.sequence = 100;
        sample.frame = constantDepth(848, 480, 1.0f);
        service->publishMetric(std::move(sample));
        output.tick();

        std::uint64_t lastSeen = 0;
        rin::Frame frame;
        RIN_CHECK(output.tryLoadFrame(lastSeen, frame));
        RIN_CHECK(lastSeen >= 1);  // mailbox 自身序号（与源帧序号独立，单调推进）。
        RIN_CHECK_MSG(!output.tryLoadFrame(lastSeen, frame),
                      "无新帧的重复消费应返回 false（上次已见序号语义）");
        output.tick();  // 空转快路径：无新米制帧。
        RIN_CHECK(!output.tryLoadFrame(lastSeen, frame));
    }

    // --- 5) O6 拒绝显式丢弃：源小于 raw 网格（放大拒绝） ---
    {
        auto service = std::make_shared<rin_test::FakeCameraService>();
        viewer::PolicyDepthOutput output(service);
        rin::DepthMetricSample sample;
        sample.sequence = 5;
        sample.frame = constantDepth(32, 18, 1.0f);  // < 64×36 raw 网格。
        service->publishMetric(std::move(sample));

        output.tick();
        std::uint64_t lastSeen = 0;
        rin::Frame frame;
        RIN_CHECK_MSG(!output.tryLoadFrame(lastSeen, frame), "O6 拒绝不应发布快照");
        RIN_CHECK_EQ(output.droppedFrames(), 1u);
    }

    // --- 6) 无效源帧显式丢弃（契约允许 sample.valid() == false 的交付；假服务
    // 会把无效帧过滤为 false，故用最小桩直返无效采样） ---
    {
        struct InvalidMetricService final : public rin_test::FakeCameraService {
            [[nodiscard]] bool tryLoadDepthMetric(std::uint64_t& lastSeenSequence,
                                                  rin::DepthMetricSample& out) override {
                out = rin::DepthMetricSample{};  // 默认构造 = 无效帧。
                out.sequence = 6;
                lastSeenSequence = out.sequence;
                return true;
            }
        };
        auto service = std::make_shared<InvalidMetricService>();
        viewer::PolicyDepthOutput output(service);

        output.tick();
        RIN_CHECK_EQ(output.droppedFrames(), 1u);
        std::uint64_t lastSeen = 0;
        rin::Frame frame;
        RIN_CHECK(!output.tryLoadFrame(lastSeen, frame));
    }

    // --- 7) 无服务防御 + gray 钳制边界 ---
    {
        viewer::PolicyDepthOutput output(nullptr);
        output.tick();  // 不崩溃、无发布。
        std::uint64_t lastSeen = 0;
        rin::Frame frame;
        RIN_CHECK(!output.tryLoadFrame(lastSeen, frame));

        RIN_CHECK(viewer::policyDepthDisplayGray(-0.5) == 0);
        RIN_CHECK(viewer::policyDepthDisplayGray(0.0) == 0);
        RIN_CHECK(viewer::policyDepthDisplayGray(0.5) == 128);
        RIN_CHECK(viewer::policyDepthDisplayGray(1.0) == 255);
        RIN_CHECK(viewer::policyDepthDisplayGray(2.0) == 255);
    }

    // --- 8) start/stop 冒烟（真 Executor，shared 持有；幂等） ---
    {
        kairo::Executor executor;
        RIN_CHECK(executor.initialize({}));
        auto service = std::make_shared<rin_test::FakeCameraService>();
        auto output = std::make_shared<viewer::PolicyDepthOutput>(service);
        RIN_CHECK(output->start(executor));
        RIN_CHECK(output->start(executor));  // 重复 start 幂等。
        output->stop();
        output->stop();
        executor.shutdown(true);
    }

    // --- 8b) 持有契约：unique 持有（enable_shared_from_this 无主）被 start
    // 显式拒绝——真机缺陷回归（周期闭包 weak 恒空静默空转的失败类） ---
    {
        kairo::Executor executor;
        RIN_CHECK(executor.initialize({}));
        auto service = std::make_shared<rin_test::FakeCameraService>();
        auto output = std::make_unique<viewer::PolicyDepthOutput>(service);
        RIN_CHECK_MSG(!output->start(executor),
                      "unique 持有下 start 应显式拒绝（weak_from_this 无主）");
        executor.shutdown(true);
    }

    // --- 9) start() 周期回调路径（shared 持有 + 真实周期调度产出帧） ---
    {
        kairo::Executor executor;
        RIN_CHECK(executor.initialize({}));
        auto service = std::make_shared<rin_test::FakeCameraService>();
        auto output = std::make_shared<viewer::PolicyDepthOutput>(service);
        RIN_CHECK(output->start(executor));
        rin::DepthMetricSample sample;
        sample.sequence = 77;
        sample.frame = constantDepth(848, 480, 1.25f);
        service->publishMetric(std::move(sample));

        // 轮询等待周期 tick（20 ms）经闭包真实执行并发布（死限 2 s）。
        bool produced = false;
        std::uint64_t lastSeen = 0;
        rin::Frame frame;
        for (int waited = 0; waited < 2000 && !produced; waited += 5) {
            produced = output->tryLoadFrame(lastSeen, frame);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        RIN_CHECK_MSG(produced, "start() 后周期回调应经 weak 闭包真实产出显示帧");
        RIN_CHECK(produced && frame.width == 512u && frame.height == 288u);
        output->stop();
        executor.shutdown(true);
    }

    return rin_test::exitStatus();
}
