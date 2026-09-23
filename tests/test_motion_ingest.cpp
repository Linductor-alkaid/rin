// M3-04 MotionIngest 离线行为测试（独立验证，rs2-free；EXEC-06 采集分支语义）：
// 1) 初态：统计零值、姿态不可用、两条邮箱为空；
// 2) 正常完成路径（DOD-02）：有效采样 → 运动邮箱发布原始采样（内容保持 + 会话
//    序号自 1 单调递增）→ IdentityImuFuser 推进 → 姿态快照（恒等姿态 + 实测统计
//    + 姿态序号）发布且 valid()；
// 3) 有界校验：无效采样（NaN/Inf 轴、损坏 kind）直接丢弃——不投递、不推进融合
//    器、不计入统计；
// 4) EMA 源频率（imu_motion_ingest.cpp kMotionRateEmaAlpha=0.1）：
//    - 已知 dt 一步到位（5ms→200Hz、4ms→250Hz），gyro/accel 两源独立；
//    - 稳态 dt 切换按指数收敛（5ms→2.5ms，60 样本内 |Hz−400| ≤ 2）；
//    - dt 非正 / 时间戳非有限或负值：只计数与发布，不污染频率估计；
//    - dt ≥ 1000ms（kMotionRateGapResetMs）视为流中断：频率记 0，下个样本重建窗口；
// 5) resetStreamState（restream/设备切换语义）：融合器复位（姿态回到不可用）+
//    频率窗口清零；会话累计样本数与会话序号跨复位保持连续（消费方 lastSeen 序号
//    不回退）；复位幂等；
// 6) ImuFuser 接缝（POST-05 可替换）：经测试桩融合器验证 ingest 对每个有效采样
//    恰好 advance 一次且看到原始采样（非重序号副本）；姿态发布以 hasPose() 为门
//    （未收敛保持"无新快照"）；resetStreamState 传递为 fuser->reset()；
// 7) 跨上下文（DOD-02 正常完成 + shutdown）：Executor 任务作生产者（模拟采集
//    worker 独占 ingest），主线程作消费者按"上次已见序号"语义读取——观察到的
//    采样序号严格递增、终值与统计精确一致，executor shutdown 干净收敛。
//
// DOD-02 其余条目适用性说明：提交拒绝——本路径不含 Executor 准入（ingest 由既有
// blocking worker 调用，任务编排不变更）；执行中取消/超时——ingest 是有界非阻塞
// 单元且不等待，取消与超时属于采集循环层（真机路径，realsense_hardware 覆盖）；
// 任务异常——邮箱 try_publish 为无锁不等待、失败即有界丢弃（最新态语义的既有背压
// 策略），单读者场景下 4 槽快照存储不会拒绝发布，丢弃注入需多读者钉住全部槽位，
// 离线单线程不可注入，如实记录不覆盖。生产者异常经 future::get 传播可观察（本测
// 试生产者不抛出，异常路径由 Executor 自身设施保证）。
#include "test_util.hpp"

#include <executor/comm/mailbox.hpp>
#include <executor/executor.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

#include "imu_fuser.hpp"
#include "imu_motion_ingest.hpp"
#include "rin/camera_types.hpp"

namespace {

using executor::comm::LatestMailbox;
using rin::detail::createImuFuser;
using rin::detail::ImuFuser;
using rin::detail::MotionIngest;
using rin::ImuSnapshot;
using rin::MotionSample;
using rin::MotionSourceStats;
using rin::MotionStreamKind;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

MotionSample makeSample(MotionStreamKind kind, double timestampMs, std::uint64_t sequence = 0) {
    MotionSample sample;
    sample.kind = kind;
    sample.axes = kind == MotionStreamKind::Accel ? std::array<float, 3>{0.0f, 0.0f, 9.81f}
                                                  : std::array<float, 3>{0.01f, -0.02f, 0.03f};
    sample.deviceTimestampMs = timestampMs;
    sample.sequence = sequence;
    return sample;
}

/// 观测型融合器桩：记录 advance/reset 调用与最后见到的采样，按配置在第 N 次
/// advance 后置姿态可用（模拟"姿态未收敛不发布"的门控，POST-05 可替换边界）。
class RecordingFuser final : public ImuFuser {
public:
    void advance(const MotionSample& sample) override {
        ++advances;
        last = sample;  // 原始采样逐字段拷贝（区别于重序号后的邮箱副本）。
        if (flipAfter > 0 && advances >= flipAfter) {
            poseAvailable = true;
        }
    }
    [[nodiscard]] bool hasPose() const noexcept override { return poseAvailable; }
    [[nodiscard]] std::array<float, 4> orientation() const noexcept override {
        return {1.0f, 0.0f, 0.0f, 0.0f};
    }
    void reset() noexcept override {
        ++resets;
        advances = 0;
        poseAvailable = false;
    }

    int advances = 0;
    int resets = 0;
    int flipAfter = 1;  // 0 = 永不可用；k = 第 k 次 advance 起可用。
    MotionSample last{};
    bool poseAvailable = false;
};

bool approx(float value, float expected, float tolerance) {
    return std::fabs(value - expected) <= tolerance;
}

/// 邮箱为空的便捷断言（不计数，配合 RIN_CHECK 使用）。
bool mailboxEmpty(const LatestMailbox<MotionSample>& mailbox) {
    MotionSample out;
    return !mailbox.try_load(out);
}

}  // namespace

int main() {
    // --- 1) 初态 ---
    {
        LatestMailbox<MotionSample> motion{"test.motion.init"};
        LatestMailbox<ImuSnapshot> pose{"test.pose.init"};
        MotionIngest ingest(motion, pose, createImuFuser());

        const MotionSourceStats stats = ingest.stats();
        RIN_CHECK_EQ(stats.gyroHz, 0.0f);
        RIN_CHECK_EQ(stats.accelHz, 0.0f);
        RIN_CHECK_EQ(stats.gyroSamples, std::uint64_t{0});
        RIN_CHECK_EQ(stats.accelSamples, std::uint64_t{0});
        RIN_CHECK(!ingest.hasPose());
        RIN_CHECK(mailboxEmpty(motion));
        ImuSnapshot snapshot;
        RIN_CHECK(!pose.try_load(snapshot));
    }

    // --- 2) 单样本正常完成路径 ---
    {
        LatestMailbox<MotionSample> motion{"test.motion.first"};
        LatestMailbox<ImuSnapshot> pose{"test.pose.first"};
        MotionIngest ingest(motion, pose, createImuFuser());

        const MotionSample input = makeSample(MotionStreamKind::Gyro, 1234.5);
        ingest.ingest(input);

        MotionSample published;
        RIN_CHECK(motion.try_load(published));
        RIN_CHECK(published.kind == MotionStreamKind::Gyro);
        RIN_CHECK_EQ(published.axes[0], 0.01f);  // 采样内容原样携带
        RIN_CHECK_EQ(published.axes[2], 0.03f);
        RIN_CHECK_EQ(published.deviceTimestampMs, 1234.5);
        RIN_CHECK_EQ(published.sequence, std::uint64_t{1});  // 会话序号自 1 起

        RIN_CHECK(ingest.hasPose());  // IdentityImuFuser：首个有效采样即姿态可用
        ImuSnapshot snapshot;
        RIN_CHECK(pose.try_load(snapshot));
        RIN_CHECK(snapshot.valid());
        RIN_CHECK_EQ(snapshot.orientation[0], 1.0f);  // 恒等姿态（M3-04 假实现契约）
        RIN_CHECK_EQ(snapshot.orientation[1], 0.0f);
        RIN_CHECK_EQ(snapshot.orientation[2], 0.0f);
        RIN_CHECK_EQ(snapshot.orientation[3], 0.0f);
        RIN_CHECK_EQ(snapshot.sequence, std::uint64_t{1});  // 姿态通道序号自 1 起
        RIN_CHECK_EQ(snapshot.sources.gyroSamples, std::uint64_t{1});
        RIN_CHECK_EQ(snapshot.sources.accelSamples, std::uint64_t{0});
        // 首样本只建立频率参考点：Hz 记 0（EMA 窗口尚无 dt），非负有限。
        RIN_CHECK_EQ(snapshot.sources.gyroHz, 0.0f);
    }

    // --- 3) 无效采样有界丢弃 ---
    {
        LatestMailbox<MotionSample> motion{"test.motion.invalid"};
        LatestMailbox<ImuSnapshot> pose{"test.pose.invalid"};
        MotionIngest ingest(motion, pose, createImuFuser());

        MotionSample nanAxis = makeSample(MotionStreamKind::Gyro, 10.0);
        nanAxis.axes[1] = kNaN;
        ingest.ingest(nanAxis);

        MotionSample infAxis = makeSample(MotionStreamKind::Accel, 10.0);
        infAxis.axes[2] = kInf;
        ingest.ingest(infAxis);

        MotionSample corrupted = makeSample(MotionStreamKind::Gyro, 10.0);
        corrupted.kind = static_cast<MotionStreamKind>(99);
        ingest.ingest(corrupted);

        // 丢弃语义：不投递、不推进融合器、不计入统计。
        RIN_CHECK(mailboxEmpty(motion));
        RIN_CHECK(!ingest.hasPose());
        ImuSnapshot snapshot;
        RIN_CHECK(!pose.try_load(snapshot));
        RIN_CHECK_EQ(ingest.stats().gyroSamples, std::uint64_t{0});
        RIN_CHECK_EQ(ingest.stats().accelSamples, std::uint64_t{0});

        // 有效样本混入无效流：无效的不占序号、不污染频率窗口。
        ingest.ingest(makeSample(MotionStreamKind::Gyro, 0.0));
        ingest.ingest(nanAxis);
        ingest.ingest(makeSample(MotionStreamKind::Gyro, 5.0));
        MotionSample published;
        RIN_CHECK(motion.try_load(published));
        RIN_CHECK_EQ(published.sequence, std::uint64_t{2});  // 无效样本不占会话序号
        RIN_CHECK(approx(ingest.stats().gyroHz, 200.0f, 0.01f));
    }

    // --- 4) 会话序号单调递增 + 邮箱最新态 + stale 读语义 ---
    {
        LatestMailbox<MotionSample> motion{"test.motion.sequence"};
        LatestMailbox<ImuSnapshot> pose{"test.pose.sequence"};
        MotionIngest ingest(motion, pose, createImuFuser());

        for (int index = 1; index <= 10; ++index) {
            ingest.ingest(makeSample(MotionStreamKind::Gyro, index * 5.0));
            MotionSample published;
            RIN_CHECK(motion.try_load(published));
            RIN_CHECK_EQ(published.sequence, static_cast<std::uint64_t>(index));
            ImuSnapshot snapshot;
            RIN_CHECK(pose.try_load(snapshot));
            RIN_CHECK_EQ(snapshot.sequence, static_cast<std::uint64_t>(index));
        }
        // 最新态：读到的是最后一个采样（时间戳与内容对应第 10 个）。
        MotionSample latest;
        RIN_CHECK(motion.try_load(latest));
        RIN_CHECK_EQ(latest.deviceTimestampMs, 50.0);

        // 消费方"上次已见序号"语义：无更新返回 false 且出参/lastSeen 不动。
        std::uint64_t lastSeen = motion.sequence();
        MotionSample out;
        out.axes = {7.0f, 7.0f, 7.0f};
        out.sequence = 555;
        RIN_CHECK(!motion.try_load_newer_than(lastSeen, out, lastSeen));
        RIN_CHECK_EQ(out.sequence, std::uint64_t{555});
        RIN_CHECK_EQ(out.axes[0], 7.0f);
        RIN_CHECK_EQ(lastSeen, motion.sequence());
    }

    // --- 5) EMA 源频率：已知 dt、两源独立、稳态收敛 ---
    {
        LatestMailbox<MotionSample> motion{"test.motion.rate"};
        LatestMailbox<ImuSnapshot> pose{"test.pose.rate"};
        MotionIngest ingest(motion, pose, createImuFuser());

        // gyro 5ms → 200Hz；accel 尚无 dt → 0。
        ingest.ingest(makeSample(MotionStreamKind::Gyro, 0.0));
        ingest.ingest(makeSample(MotionStreamKind::Gyro, 5.0));
        RIN_CHECK(approx(ingest.stats().gyroHz, 200.0f, 0.01f));
        RIN_CHECK_EQ(ingest.stats().accelHz, 0.0f);

        // accel 4ms → 250Hz；gyro 估计不受 accel 路径影响。
        ingest.ingest(makeSample(MotionStreamKind::Accel, 0.0));
        ingest.ingest(makeSample(MotionStreamKind::Accel, 4.0));
        RIN_CHECK(approx(ingest.stats().accelHz, 250.0f, 0.01f));
        RIN_CHECK(approx(ingest.stats().gyroHz, 200.0f, 0.01f));

        // gyro dt 切到 2.5ms（400Hz 档）：EMA 指数收敛，60 样本内 |Hz−400| ≤ 2。
        for (int index = 1; index <= 60; ++index) {
            ingest.ingest(makeSample(MotionStreamKind::Gyro, 5.0 + index * 2.5));
        }
        RIN_CHECK(approx(ingest.stats().gyroHz, 400.0f, 2.0f));
        RIN_CHECK(approx(ingest.stats().accelHz, 250.0f, 0.01f));  // accel 不受影响

        const MotionSourceStats stats = ingest.stats();
        RIN_CHECK_EQ(stats.gyroSamples, std::uint64_t{62});
        RIN_CHECK_EQ(stats.accelSamples, std::uint64_t{2});
    }

    // --- 6) dt 非正 / 时间戳非有限或负值：只计数，不污染频率估计 ---
    {
        LatestMailbox<MotionSample> motion{"test.motion.dt"};
        LatestMailbox<ImuSnapshot> pose{"test.pose.dt"};
        MotionIngest ingest(motion, pose, createImuFuser());

        ingest.ingest(makeSample(MotionStreamKind::Gyro, 0.0));
        ingest.ingest(makeSample(MotionStreamKind::Gyro, 5.0));
        RIN_CHECK(approx(ingest.stats().gyroHz, 200.0f, 0.01f));

        ingest.ingest(makeSample(MotionStreamKind::Gyro, 5.0));   // dt == 0
        RIN_CHECK(approx(ingest.stats().gyroHz, 200.0f, 0.01f));
        ingest.ingest(makeSample(MotionStreamKind::Gyro, 4.0));   // dt < 0
        RIN_CHECK(approx(ingest.stats().gyroHz, 200.0f, 0.01f));
        ingest.ingest(makeSample(MotionStreamKind::Gyro, kNaN));  // 时间戳非有限
        RIN_CHECK(approx(ingest.stats().gyroHz, 200.0f, 0.01f));
        ingest.ingest(makeSample(MotionStreamKind::Gyro, -100.0));  // 时间戳为负
        RIN_CHECK(approx(ingest.stats().gyroHz, 200.0f, 0.01f));
        RIN_CHECK_EQ(ingest.stats().gyroSamples, std::uint64_t{6});  // 计数不受影响

        // 参考点未被污染：正常样本 dt 仍从上一有效时间戳起算。
        ingest.ingest(makeSample(MotionStreamKind::Gyro, 10.0));
        RIN_CHECK(approx(ingest.stats().gyroHz, 200.0f, 0.01f));
        RIN_CHECK_EQ(ingest.stats().gyroSamples, std::uint64_t{7});

        // 异常时间戳样本本身仍然有效并已发布（valid() 不约束时间戳域）。
        MotionSample published;
        RIN_CHECK(motion.try_load(published));
        RIN_CHECK_EQ(published.deviceTimestampMs, 10.0);
        RIN_CHECK_EQ(published.sequence, std::uint64_t{7});
    }

    // --- 7) 长间隙（≥1000ms）：频率作废，窗口重建 ---
    {
        LatestMailbox<MotionSample> motion{"test.motion.gap"};
        LatestMailbox<ImuSnapshot> pose{"test.pose.gap"};
        MotionIngest ingest(motion, pose, createImuFuser());

        ingest.ingest(makeSample(MotionStreamKind::Gyro, 0.0));
        ingest.ingest(makeSample(MotionStreamKind::Gyro, 5.0));
        ingest.ingest(makeSample(MotionStreamKind::Gyro, 10.0));
        RIN_CHECK(approx(ingest.stats().gyroHz, 200.0f, 0.01f));

        ingest.ingest(makeSample(MotionStreamKind::Gyro, 2000.0));  // 间隙样本
        RIN_CHECK_EQ(ingest.stats().gyroHz, 0.0f);                  // 旧频率作废
        RIN_CHECK_EQ(ingest.stats().gyroSamples, std::uint64_t{4}); // 计数照常累计

        ingest.ingest(makeSample(MotionStreamKind::Gyro, 2005.0));  // 重建窗口
        RIN_CHECK(approx(ingest.stats().gyroHz, 200.0f, 0.01f));
    }

    // --- 8) resetStreamState（restream/设备切换语义）---
    {
        LatestMailbox<MotionSample> motion{"test.motion.reset"};
        LatestMailbox<ImuSnapshot> pose{"test.pose.reset"};
        MotionIngest ingest(motion, pose, createImuFuser());

        ingest.ingest(makeSample(MotionStreamKind::Gyro, 0.0));
        ingest.ingest(makeSample(MotionStreamKind::Gyro, 5.0));
        ingest.ingest(makeSample(MotionStreamKind::Gyro, 10.0));
        RIN_CHECK(ingest.hasPose());
        RIN_CHECK(approx(ingest.stats().gyroHz, 200.0f, 0.01f));

        // 复位：姿态回到不可用、频率窗口清零；会话累计样本数保留。
        ingest.resetStreamState();
        RIN_CHECK(!ingest.hasPose());
        RIN_CHECK_EQ(ingest.stats().gyroHz, 0.0f);
        RIN_CHECK_EQ(ingest.stats().gyroSamples, std::uint64_t{3});

        // 复位后序号连续（消费方 lastSeen 不回退）：下一采样会话序号接续 4。
        ingest.ingest(makeSample(MotionStreamKind::Gyro, 1000.0));  // 重建窗口首样本
        MotionSample published;
        RIN_CHECK(motion.try_load(published));
        RIN_CHECK_EQ(published.sequence, std::uint64_t{4});
        ImuSnapshot snapshot;
        RIN_CHECK(pose.try_load(snapshot));
        RIN_CHECK_EQ(snapshot.sequence, std::uint64_t{4});  // 姿态序号同样接续
        RIN_CHECK_EQ(ingest.stats().gyroHz, 0.0f);          // 新窗口首样本仍无 dt

        ingest.ingest(makeSample(MotionStreamKind::Gyro, 1005.0));
        RIN_CHECK(approx(ingest.stats().gyroHz, 200.0f, 0.01f));
        RIN_CHECK_EQ(ingest.stats().gyroSamples, std::uint64_t{5});  // 会话累计连续

        // 复位幂等：重复复位不改变可观察状态。
        ingest.resetStreamState();
        ingest.resetStreamState();
        RIN_CHECK(!ingest.hasPose());
        RIN_CHECK_EQ(ingest.stats().gyroHz, 0.0f);
        RIN_CHECK_EQ(ingest.stats().gyroSamples, std::uint64_t{5});
    }

    // --- 9) ImuFuser 接缝（POST-05 可替换边界）---
    {
        LatestMailbox<MotionSample> motion{"test.motion.seam"};
        LatestMailbox<ImuSnapshot> pose{"test.pose.seam"};
        auto fuser = std::make_unique<RecordingFuser>();
        RecordingFuser& stub = *fuser;
        stub.flipAfter = 3;  // 第 3 次推进起姿态可用
        MotionIngest ingest(motion, pose, std::move(fuser));

        for (int index = 1; index <= 5; ++index) {
            MotionSample input = makeSample(MotionStreamKind::Gyro, index * 5.0);
            input.sequence = 900 + static_cast<std::uint64_t>(index);  // 调用方自带序号
            ingest.ingest(input);
        }
        // 每个有效采样恰好推进一次，且融合器看到原始采样（非重序号邮箱副本）。
        RIN_CHECK_EQ(stub.advances, 5);
        RIN_CHECK_EQ(stub.last.sequence, std::uint64_t{905});
        RIN_CHECK_EQ(stub.last.deviceTimestampMs, 25.0);
        RIN_CHECK(stub.last.kind == MotionStreamKind::Gyro);

        // 姿态发布以 hasPose() 为门：样本 3 起才有快照（共 3 份），运动通道 5 份。
        ImuSnapshot snapshot;
        RIN_CHECK(pose.try_load(snapshot));
        RIN_CHECK_EQ(snapshot.sequence, std::uint64_t{3});
        RIN_CHECK_EQ(snapshot.orientation[0], 1.0f);  // 桩的恒等姿态
        MotionSample published;
        RIN_CHECK(motion.try_load(published));
        RIN_CHECK_EQ(published.sequence, std::uint64_t{5});

        // 无效采样不推进融合器。
        MotionSample invalid = makeSample(MotionStreamKind::Gyro, 30.0);
        invalid.axes[0] = kNaN;
        ingest.ingest(invalid);
        RIN_CHECK_EQ(stub.advances, 5);

        // resetStreamState 传递为 fuser->reset()：计数可见、姿态回到不可用。
        ingest.resetStreamState();
        RIN_CHECK_EQ(stub.resets, 1);
        RIN_CHECK_EQ(stub.advances, 0);
        RIN_CHECK(!ingest.hasPose());
    }
    {
        // 姿态永不收敛（flipAfter=0）：运动通道照常发布，姿态通道保持空。
        LatestMailbox<MotionSample> motion{"test.motion.noPose"};
        LatestMailbox<ImuSnapshot> pose{"test.pose.noPose"};
        auto fuser = std::make_unique<RecordingFuser>();
        fuser->flipAfter = 0;
        MotionIngest ingest(motion, pose, std::move(fuser));

        for (int index = 1; index <= 3; ++index) {
            ingest.ingest(makeSample(MotionStreamKind::Gyro, index * 5.0));
        }
        MotionSample published;
        RIN_CHECK(motion.try_load(published));
        RIN_CHECK_EQ(published.sequence, std::uint64_t{3});
        ImuSnapshot snapshot;
        RIN_CHECK(!pose.try_load(snapshot));  // "姿态未收敛保持无新快照"
        RIN_CHECK_EQ(ingest.stats().gyroSamples, std::uint64_t{3});
    }

    // --- 10) 跨上下文（Executor 生产者 + 主线程消费者）+ shutdown ---
    {
        executor::Executor executor;
        RIN_CHECK(executor.initialize_ex({}));

        LatestMailbox<MotionSample> motion{"test.motion.concurrent"};
        LatestMailbox<ImuSnapshot> pose{"test.pose.concurrent"};
        MotionIngest ingest(motion, pose, createImuFuser());

        constexpr int kTotalSamples = 1000;
        // 生产者模拟采集 worker：唯一调用 ingest 的上下文（MotionIngest 线程契约）。
        auto produced = executor.submit_auto([&ingest] {
            for (int index = 0; index < kTotalSamples; ++index) {
                const bool gyro = (index % 2) == 0;
                MotionSample sample = makeSample(
                    gyro ? MotionStreamKind::Gyro : MotionStreamKind::Accel, index * 2.5);
                ingest.ingest(sample);
            }
        });
        RIN_CHECK(produced.valid());  // admission 可观察

        // 消费者按"上次已见序号"语义轮询：观察序号必须严格递增（最新态可跳读）。
        std::uint64_t lastSeen = 0;
        MotionSample observed;
        std::uint64_t previousSessionSequence = 0;
        bool monotonic = true;
        bool reachedLatest = false;
        long attempts = 0;
        while (!reachedLatest && attempts < 50'000'000) {
            if (motion.try_load_newer_than(lastSeen, observed, lastSeen)) {
                if (observed.sequence <= previousSessionSequence) {
                    monotonic = false;
                }
                previousSessionSequence = observed.sequence;
                reachedLatest = observed.sequence ==
                                static_cast<std::uint64_t>(kTotalSamples);
            }
            ++attempts;
        }
        RIN_CHECK(reachedLatest);   // 有界等待内看到最新样本（超时即失败，不悬挂）
        RIN_CHECK(monotonic);       // 消费方视角会话序号不回退

        // 生产者正常完成（异常经 get 传播）；采集侧统计与发布侧精确一致。
        produced.get();
        const MotionSourceStats stats = ingest.stats();
        RIN_CHECK_EQ(stats.gyroSamples, std::uint64_t{500});
        RIN_CHECK_EQ(stats.accelSamples, std::uint64_t{500});
        // 每源相邻样本 dt 恒 5ms：EMA 收敛到 200Hz。
        RIN_CHECK(approx(stats.gyroHz, 200.0f, 0.5f));
        RIN_CHECK(approx(stats.accelHz, 200.0f, 0.5f));

        // 最新态终值：运动采样与姿态快照的会话序号都到达 kTotalSamples。
        MotionSample latestMotion;
        RIN_CHECK(motion.try_load(latestMotion));
        RIN_CHECK_EQ(latestMotion.sequence, std::uint64_t{1000});
        ImuSnapshot latestPose;
        RIN_CHECK(pose.try_load(latestPose));
        RIN_CHECK_EQ(latestPose.sequence, std::uint64_t{1000});
        RIN_CHECK(latestPose.valid());
        RIN_CHECK_EQ(latestPose.sources.gyroSamples, std::uint64_t{500});

        ingest.resetStreamState();  // 关闭前的复位路径不抛、不悬挂
        RIN_CHECK(!ingest.hasPose());

        executor.shutdown();  // 干净收敛（DOD-02 shutdown）
    }

    return rin_test::exitStatus();
}
