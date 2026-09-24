// 公开契约边界：仅包含 rin 公开头（目标未链接第三方 include 路径），
// 并实例化契约类型/接口，确保公开头零第三方类型（RULE-01）。
#include "test_util.hpp"

#include <memory>

#include <rin/camera_service.hpp>
#include <rin/workflow_engine.hpp>

namespace {

class NullService final : public rin::ICameraService {
public:
    rin::StartOutcome start(const rin::StreamRequest&) override { return {}; }
    bool requestResolution(const rin::StreamRequest&, std::string*) override { return false; }
    bool requestDevice(const std::string&, std::string*) override { return false; }
    bool requestDepthColorScheme(rin::DepthColorScheme, std::string*) override
    {
        return false;
    }
    void stop() override {}
    rin::CameraServiceState state() const override { return rin::CameraServiceState::Idle; }
    std::string lastError() const override { return {}; }
    bool tryLoadFrame(rin::FrameKind, std::uint64_t&, rin::Frame&) override { return false; }
    bool tryLoadIntrinsics(std::uint64_t&, rin::IntrinsicsSnapshot&) override { return false; }
    bool tryLoadMotion(std::uint64_t&, rin::MotionSample&) override { return false; }
    bool tryLoadPose(std::uint64_t&, rin::ImuSnapshot&) override { return false; }
    bool tryLoadCatalog(std::uint64_t&, rin::DeviceCatalog&) override { return false; }
    bool tryLoadEvent(rin::ServiceEvent&) override { return false; }
};

class NullWorkflowEngine final : public rin::IWorkflowEngine {
public:
    const rin::NodeCatalog& catalog() const override { return catalog_; }
    rin::WorkflowValidation applyGraph(const rin::WorkflowGraph&) override { return {}; }
    bool requestParamUpdate(rin::NodeId, const std::string&, const rin::ParamValue&,
                            std::string*) override
    {
        return false;
    }
    rin::AdmissionResult start() override { return {}; }
    void stop() override {}
    rin::WorkflowEngineState state() const override { return rin::WorkflowEngineState::Idle; }
    std::string lastError() const override { return {}; }
    bool tryLoadStats(std::uint64_t&, rin::WorkflowStats&) override { return false; }
    bool tryLoadNodeOutput(rin::NodeId, std::uint64_t&, rin::NodeOutputSnapshot&) override
    {
        return false;
    }
    bool tryLoadEvent(rin::WorkflowEvent&) override { return false; }

private:
    rin::NodeCatalog catalog_;
};

}  // namespace

int main() {
    std::shared_ptr<rin::ICameraService> service = std::make_shared<NullService>();
    RIN_CHECK(service != nullptr);
    RIN_CHECK(!service->start({}).admitted);

    // M3-03 两条新通道（经公开接口多态调用，同时实例化 MotionSample/ImuSnapshot
    // 契约类型）：无生产者桩语义——无新数据返回 false，且 lastSeenSequence 与 out
    // 出参保持不动（与实现通道所用 LatestMailbox::try_load_newer_than 的 stale
    // 读取不更新序号一致）。
    {
        std::uint64_t motionSequence = 41;
        rin::MotionSample motion;
        motion.kind = rin::MotionStreamKind::Gyro;
        motion.axes = {1.0f, 2.0f, 3.0f};
        motion.sequence = 999;
        RIN_CHECK(!service->tryLoadMotion(motionSequence, motion));
        RIN_CHECK_EQ(motionSequence, std::uint64_t{41});
        RIN_CHECK_EQ(motion.sequence, std::uint64_t{999});
        RIN_CHECK_EQ(motion.axes[0], 1.0f);
        RIN_CHECK_EQ(motion.kind, rin::MotionStreamKind::Gyro);
    }
    {
        std::uint64_t poseSequence = 7;
        rin::ImuSnapshot pose;
        pose.sequence = 888;
        RIN_CHECK(!service->tryLoadPose(poseSequence, pose));
        RIN_CHECK_EQ(poseSequence, std::uint64_t{7});
        RIN_CHECK_EQ(pose.sequence, std::uint64_t{888});
        RIN_CHECK_EQ(pose.orientation[0], 1.0f);  // 默认恒等姿态未被触碰
    }

    // M4-09 工作流视图契约（经公开接口多态调用，同时实例化契约类型）：
    // 无生产者桩语义与相机服务一致——无新数据返回 false 且出参不动。
    {
        std::shared_ptr<rin::IWorkflowEngine> engine = std::make_shared<NullWorkflowEngine>();
        RIN_CHECK(engine != nullptr);
        RIN_CHECK(!engine->start().admitted);
        RIN_CHECK_EQ(engine->state(), rin::WorkflowEngineState::Idle);

        std::uint64_t statsSequence = 5;
        rin::WorkflowStats stats;
        stats.endToEndFps = 12.0;
        RIN_CHECK(!engine->tryLoadStats(statsSequence, stats));
        RIN_CHECK_EQ(statsSequence, std::uint64_t{5});
        RIN_CHECK_EQ(stats.endToEndFps, 12.0);

        std::uint64_t outputSequence = 3;
        rin::NodeOutputSnapshot output;
        output.node = 9;
        RIN_CHECK(!engine->tryLoadNodeOutput(9, outputSequence, output));
        RIN_CHECK_EQ(outputSequence, std::uint64_t{3});
        RIN_CHECK_EQ(output.node, rin::NodeId{9});

        rin::WorkflowEvent event;
        event.kind = rin::WorkflowEventKind::Started;
        RIN_CHECK(!engine->tryLoadEvent(event));
        RIN_CHECK_EQ(event.kind, rin::WorkflowEventKind::Started);
    }
    return rin_test::exitStatus();
}
