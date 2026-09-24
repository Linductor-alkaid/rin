// M3-07 关闭路径回归测试（独立验证）：新增姿态通道的排空语义 + 跨上下文姿态数据
// （里程碑 M3-07 完成判据："关闭路径测试 + tsan 通过（跨上下文姿态数据）"；本目标
// 即 tsan 预设复跑目标）。
//
// 被测接缝（与 viewer 集成同构，全部真实代码，无 EUI 运行时依赖）：
// - 生产侧：Executor 任务独占 MotionIngest（adapter 真实接缝，createImuFuser() 工厂
//   真身 Mahony），模拟 EXEC-01/EXEC-06 采集 blocking worker——服务 stop() 返回后
//   worker 已回收、不再有新发布；
// - 通道：LatestMailbox<MotionSample>/<ImuSnapshot>（EXEC-06 最新态，容量有界）；
// - 消费侧：PoseViewState（apps/viewer/pose_view.hpp 真实类型）+ pumpPose() 助手
//   逐条复刻 app.cpp:326-336 的门控契约（Streaming/Restreaming 消费 tryLoadPose
//   语义的最新快照；其余状态回空态）——app.cpp 的 ViewerContext 位于匿名命名空间
//   且绑定 EUI 运行时/rs2 服务，无法 headless 直接实例化，此处以同构模型锁定语义；
// - 关闭顺序（app.cpp:343-362 / camera_service_design.md "viewer 集成"）：停止生产
//   （worker 回收）→ 消费侧排空（PoseViewState::clear()，陈旧快照不跨 shutdown
//   存活）→ executor.shutdown(true)，主线程完成，幂等。
//
// 场景覆盖：
// 1) 正常完成（DOD-02）：生产者 2000 样本跨上下文发布，消费者按"上次已见序号"
//    泵送——快照序号单调推进到终值、姿态 valid()、源频率/样本统计与发布侧一致；
// 2) 停止后无新发布（ICameraService::stop() 契约的发布半边，camera_service.hpp:36-38）：
//    生产者完成后 try_load_newer_than 恒 false，重复泵送不改变视图；
// 3) 排空语义（M3-07 核心）：clear() 回空态；stale 读取仍返回旧快照且序号不回退
//    （对既有消费方保持 stale 语义），但门控不再应用——视图不被陈旧数据复活，
//    面板门控输入（available && valid()）为 false；重复排空幂等；
// 4) shutdown：排空后 executor.shutdown(true) 主线程 Completed 收敛（不悬挂）；
// 5) 任务异常（DOD-02）：生产者中途中掷异常经 future 传播（不被吞掉），已发布
//    快照仍可读取，失败路径下排空 + shutdown 同样收敛；
// 6) 排空后新会话：resetStreamState（restream 语义，序号连续）+ 新生产者 + 门控
//    回 Streaming → 视图随"新"快照重新激活（序号 > 上次已见，不回退）——复活只能
//    来自新会话新数据，不能来自 shutdown 前的 stale 残留。
//
// DOD-02 其余条目适用性说明（如实取舍）：提交拒绝——M3-07 顺序在 shutdown(true)
// 之前完成排空，路径内不存在 shutdown 后提交（提交拒绝语义属 Executor 自身设施，
// ingest 亦无独立准入，与 M3-04 test_motion_ingest 取舍一致）；执行中取消——blocking
// worker 的协作取消属采集服务层（EXEC-01，camera_state_machine 覆盖、真机路径归
// M3-08），排空路径从 worker 回收完成后才开始；超时——路径内无定时等待，消费者
// 轮询以有界尝试数上限兜底（不悬挂即超时防线）。
#include "test_util.hpp"

#include <executor/comm/mailbox.hpp>
#include <executor/executor.hpp>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "imu_motion_ingest.hpp"
#include "pose_view.hpp"
#include "rin/camera_types.hpp"

namespace {

using executor::comm::LatestMailbox;
using rin::detail::createImuFuser;
using rin::detail::MotionIngest;
using rin::ImuSnapshot;
using rin::MotionSample;
using rin::MotionStreamKind;
using viewer::PoseViewState;

/// viewer 门控状态模型（app.cpp 以 rin::CameraServiceState 驱动同一决策）。
enum class UiStreamState {
    Streaming,
    Restreaming,
    Other,  // Idle/Opening/Waiting/Stopping/Failed 等：回空态。
};

/// 逐条复刻 app.cpp:326-336 的姿态消费门控（语义模型，见文件头说明）。
void pumpPose(LatestMailbox<ImuSnapshot>& pose, std::uint64_t& lastSeen, PoseViewState& view,
              UiStreamState state) {
    if (state == UiStreamState::Streaming || state == UiStreamState::Restreaming) {
        ImuSnapshot snapshot;
        if (pose.try_load_newer_than(lastSeen, snapshot, lastSeen)) {
            view.update(snapshot);
        }
    } else if (view.available) {
        view.clear();
    }
}

MotionSample makeSample(MotionStreamKind kind, double timestampMs) {
    MotionSample sample;
    sample.kind = kind;
    // ACCEL 恒为 +1g Z（精确重力对齐，姿态立即收敛）；GYRO 小角速度。
    sample.axes = kind == MotionStreamKind::Accel ? std::array<float, 3>{0.0f, 0.0f, 9.81f}
                                                  : std::array<float, 3>{0.01f, -0.02f, 0.03f};
    sample.deviceTimestampMs = timestampMs;
    return sample;
}

/// 采集循环运动分支模型：第 index 个样本（0 起），GYRO/ACCEL 交错、同源 dt=5ms。
MotionSample interleavedSample(int index) {
    const bool gyro = (index % 2) == 0;
    return makeSample(gyro ? MotionStreamKind::Gyro : MotionStreamKind::Accel, index * 2.5);
}

/// 消费到最新快照（有界尝试，超上限即失败——不悬挂）。
bool pumpUntilSequence(LatestMailbox<ImuSnapshot>& pose, std::uint64_t target,
                       std::uint64_t& lastSeen, PoseViewState& view, UiStreamState state,
                       long maxAttempts) {
    for (long attempts = 0; attempts < maxAttempts; ++attempts) {
        pumpPose(pose, lastSeen, view, state);
        if (view.available && view.snapshot.sequence >= target) {
            return true;
        }
    }
    return false;
}

bool approx(float value, float expected, float tolerance) {
    return std::fabs(value - expected) <= tolerance;
}

}  // namespace

int main() {
    // --- 1) + 2) 正常完成：跨上下文发布/消费；停止后无新发布 ---
    {
        executor::Executor executor;
        RIN_CHECK(executor.initialize_ex({}));

        LatestMailbox<MotionSample> motion{"test.drain.motion.main"};
        LatestMailbox<ImuSnapshot> pose{"test.drain.pose.main"};
        MotionIngest ingest(motion, pose, createImuFuser());
        PoseViewState view;
        std::uint64_t lastSeen = 0;

        constexpr int kTotalSamples = 2000;
        // 生产者模拟采集 blocking worker：唯一 ingest 调用上下文（MotionIngest 线程契约）。
        auto produced = executor.submit_auto([&ingest] {
            for (int index = 0; index < kTotalSamples; ++index) {
                ingest.ingest(interleavedSample(index));
            }
        });
        RIN_CHECK(produced.valid());  // admission 可观察

        // Streaming 门控下消费：快照序号推进到终值（跨上下文姿态数据可达）。
        RIN_CHECK(pumpUntilSequence(pose, static_cast<std::uint64_t>(kTotalSamples), lastSeen,
                                    view, UiStreamState::Streaming, 50'000'000));
        produced.get();  // 正常完成；异常经 get 传播（此处不抛）。

        // 视图持有最新快照：valid（面板/3D 视图可消费）、逐字段与发布侧一致。
        RIN_CHECK(view.available);
        RIN_CHECK(view.snapshot.valid());
        RIN_CHECK_EQ(view.snapshot.sequence, static_cast<std::uint64_t>(kTotalSamples));
        RIN_CHECK_EQ(view.snapshot.sources.gyroSamples, static_cast<std::uint64_t>(kTotalSamples / 2));
        RIN_CHECK_EQ(view.snapshot.sources.accelSamples, static_cast<std::uint64_t>(kTotalSamples / 2));
        RIN_CHECK(approx(view.snapshot.sources.gyroHz, 200.0f, 0.5f));
        RIN_CHECK(approx(view.snapshot.sources.accelHz, 200.0f, 0.5f));
        // 姿态单位四元数（Mahony 真身；面板欧拉读数的输入契约）。
        double normSq = 0.0;
        for (const float c : view.snapshot.orientation) {
            normSq += static_cast<double>(c) * c;
        }
        RIN_CHECK(std::fabs(normSq - 1.0) <= 1e-3);

        // 停止后（worker 回收）无新发布：通道不再前进，重复泵送不改变视图。
        const std::uint64_t poseSequenceAtStop = pose.sequence();
        const std::uint64_t motionSequenceAtStop = motion.sequence();
        const ImuSnapshot viewAtStop = view.snapshot;
        {
            ImuSnapshot noNew;
            std::uint64_t seen = lastSeen;
            MotionSample noNewMotion;
            std::uint64_t seenMotion = motionSequenceAtStop;
            RIN_CHECK(!pose.try_load_newer_than(lastSeen, noNew, seen));
            RIN_CHECK(!motion.try_load_newer_than(motionSequenceAtStop, noNewMotion, seenMotion));
        }
        pumpPose(pose, lastSeen, view, UiStreamState::Streaming);
        pumpPose(pose, lastSeen, view, UiStreamState::Streaming);
        RIN_CHECK(view.available);
        RIN_CHECK(view.snapshot.sequence == viewAtStop.sequence);
        RIN_CHECK_EQ(pose.sequence(), poseSequenceAtStop);
        RIN_CHECK_EQ(motion.sequence(), static_cast<std::uint64_t>(kTotalSamples));

        // --- 3) 排空语义（M3-07）：clear 后 stale 不复活 ---
        pumpPose(pose, lastSeen, view, UiStreamState::Other);  // 状态离开 Streaming → 排空
        RIN_CHECK(!view.available);                            // 回空态
        RIN_CHECK((view.baseline == std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f}));
        RIN_CHECK(!(view.available && view.snapshot.valid()));  // 面板门控不放行

        // stale 语义保持：旧快照仍可读、序号不回退（对既有消费方的既有值）。
        ImuSnapshot stale;
        RIN_CHECK(pose.try_load(stale));
        RIN_CHECK_EQ(stale.sequence, static_cast<std::uint64_t>(kTotalSamples));
        RIN_CHECK_EQ(pose.sequence(), poseSequenceAtStop);

        // 陈旧数据不得复活视图：stale 读不出新、门控（非 Streaming）不应用。
        for (int repeat = 0; repeat < 3; ++repeat) {
            ImuSnapshot discarded;
            RIN_CHECK(!pose.try_load_newer_than(lastSeen, discarded, lastSeen));
            pumpPose(pose, lastSeen, view, UiStreamState::Other);
            RIN_CHECK(!view.available);
        }
        // 重复排空幂等。
        view.clear();
        pumpPose(pose, lastSeen, view, UiStreamState::Other);
        RIN_CHECK(!view.available);
        RIN_CHECK((view.baseline == std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f}));

        // --- 4) 关闭顺序收尾：排空完成后 executor.shutdown(true)（主线程）收敛 ---
        RIN_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    }

    // --- 5) 任务异常（DOD-02）：生产者中掷异常 → 传播可观察 → 排空/shutdown 仍收敛 ---
    {
        executor::Executor executor;
        RIN_CHECK(executor.initialize_ex({}));

        LatestMailbox<MotionSample> motion{"test.drain.motion.fail"};
        LatestMailbox<ImuSnapshot> pose{"test.drain.pose.fail"};
        MotionIngest ingest(motion, pose, createImuFuser());
        PoseViewState view;
        std::uint64_t lastSeen = 0;

        auto produced = executor.submit_auto([&ingest] {
            for (int index = 0; index < 100; ++index) {
                ingest.ingest(interleavedSample(index));
            }
            throw std::runtime_error("worker failed mid-stream");
        });
        RIN_CHECK(produced.valid());

        // 失败前已发布的快照仍可被消费（有界轮询，不等待完成）。
        pumpUntilSequence(pose, 10, lastSeen, view, UiStreamState::Streaming, 5'000'000);

        bool exceptionObserved = false;
        try {
            produced.get();
        } catch (const std::runtime_error& error) {
            exceptionObserved = std::string(error.what()) == "worker failed mid-stream";
        }
        RIN_CHECK(exceptionObserved);  // 异常经 future 传播，未被吞掉

        // 失败路径下同样走排空 → shutdown（viewer 对 Failed 事件的处置同构）。
        pumpPose(pose, lastSeen, view, UiStreamState::Other);
        RIN_CHECK(!view.available);
        RIN_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    }

    // --- 6) 排空后新会话：复活只能来自新会话新数据（序号连续、不回退）---
    {
        executor::Executor executor;
        RIN_CHECK(executor.initialize_ex({}));

        LatestMailbox<MotionSample> motion{"test.drain.motion.session"};
        LatestMailbox<ImuSnapshot> pose{"test.drain.pose.session"};
        MotionIngest ingest(motion, pose, createImuFuser());
        PoseViewState view;
        std::uint64_t lastSeen = 0;

        constexpr int kFirstSession = 1000;
        auto first = executor.submit_auto([&ingest] {
            for (int index = 0; index < kFirstSession; ++index) {
                ingest.ingest(interleavedSample(index));
            }
        });
        RIN_CHECK(pumpUntilSequence(pose, static_cast<std::uint64_t>(kFirstSession), lastSeen,
                                    view, UiStreamState::Streaming, 50'000'000));
        first.get();
        RIN_CHECK(view.available);

        // 会话间排空（restream 经 Opening 窗口语义）：视图回空态。
        pumpPose(pose, lastSeen, view, UiStreamState::Other);
        RIN_CHECK(!view.available);

        // 新会话（restream：融合器复位、序号连续；时间戳继续推进）。
        ingest.resetStreamState();
        RIN_CHECK(!ingest.hasPose());
        constexpr int kSecondSession = 800;
        auto second = executor.submit_auto([&ingest] {
            for (int index = 0; index < kSecondSession; ++index) {
                MotionSample sample = interleavedSample(kFirstSession + index);
                sample.deviceTimestampMs += 10'000.0;  // 会话间隔
                ingest.ingest(sample);
            }
        });
        RIN_CHECK(pumpUntilSequence(
            pose, static_cast<std::uint64_t>(kFirstSession + kSecondSession), lastSeen, view,
            UiStreamState::Streaming, 50'000'000));
        second.get();

        // 视图重新激活：持有"新"快照（序号接续、不回退），非 shutdown 前 stale 残留。
        RIN_CHECK(view.available);
        RIN_CHECK(view.snapshot.valid());
        RIN_CHECK_EQ(view.snapshot.sequence,
                     static_cast<std::uint64_t>(kFirstSession + kSecondSession));
        RIN_CHECK_EQ(view.snapshot.sources.gyroSamples,
                     static_cast<std::uint64_t>((kFirstSession + kSecondSession) / 2));

        pumpPose(pose, lastSeen, view, UiStreamState::Other);
        RIN_CHECK(!view.available);
        RIN_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    }

    return rin_test::exitStatus();
}
