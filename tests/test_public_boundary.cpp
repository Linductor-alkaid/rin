// 公开契约边界：仅包含 rin 公开头（目标未链接第三方 include 路径），
// 并实例化契约类型/接口，确保公开头零第三方类型（RULE-01）。
#include "test_util.hpp"

#include <memory>
#include <utility>
#include <vector>

#include <rin/camera_service.hpp>
#include <rin/image_node.hpp>
#include <rin/image_ops.hpp>
#include <rin/image_types.hpp>
#include <rin/node_graph.hpp>
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

// M4-02 Core 图像与节点契约最小实例化：IImageNode 桩 + ImageU8 + buildNodeGraph/
// runNodeGraph 全部经公开头可用（RULE-01：三头不引入第三方类型，仅链 rin::core）。
namespace {

class PassThroughNode final : public rin::IImageNode {
public:
    explicit PassThroughNode(rin::NodeDescriptor descriptor)
        : descriptor_(std::move(descriptor)) {}

    const rin::NodeDescriptor& descriptor() const noexcept override { return descriptor_; }
    std::vector<rin::ImageU8> apply(const std::vector<rin::ImageU8>& inputs) const override
    {
        std::vector<rin::ImageU8> outputs;
        for (rin::PortType type : descriptor_.outputs) {
            outputs.push_back(inputs.empty() ? rin::ImageU8::make(type, 2, 1)
                                             : rin::ImageU8::wrap(type, inputs.front().width(),
                                                                  inputs.front().height(),
                                                                  inputs.front().stride(),
                                                                  inputs.front().pixels()));
        }
        return outputs;
    }

private:
    rin::NodeDescriptor descriptor_;
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

    // M4-02 Core 图像与节点契约（最小实例化 + 一次编译/求值往返）：ImageU8 值对象、
    // IImageNode 多态桩、ImageNodeFactory 工厂接缝、NodeGraph/NodeGraphBuild 与
    // buildNodeGraph/runNodeGraph 公开入口全部可用。
    {
        rin::ImageU8 image = rin::ImageU8::make(rin::PortType::Gray8, 4, 2);
        RIN_CHECK(image.valid());
        RIN_CHECK_EQ(image.byteSize(), std::uint64_t{8});

        rin::NodeCatalog catalog;
        rin::NodeDescriptor source;
        source.typeId = "source";
        source.displayName = "源";
        source.outputs = {rin::PortType::Rgba8};
        rin::NodeDescriptor pass;
        pass.typeId = "pass";
        pass.displayName = "直通";
        pass.inputs = {rin::PortType::Rgba8};
        pass.outputs = {rin::PortType::Gray8};
        catalog.nodes = {source, pass};

        rin::WorkflowGraph graph;
        rin::NodeInstance sourceInstance;
        sourceInstance.id = 1;
        sourceInstance.typeId = "source";
        rin::NodeInstance passInstance;
        passInstance.id = 2;
        passInstance.typeId = "pass";
        graph.nodes = {sourceInstance, passInstance};
        rin::Connection connection;
        connection.from = rin::PortRef{1, rin::PortDirection::Output, 0};
        connection.to = rin::PortRef{2, rin::PortDirection::Input, 0};
        graph.connections = {connection};

        rin::ImageNodeFactory factory =
            [](const rin::NodeDescriptor& descriptor,
               const rin::NodeInstance&) -> std::unique_ptr<rin::IImageNode> {
            if (descriptor.inputs.empty()) {
                return nullptr;  // 注入型源节点。
            }
            return std::make_unique<PassThroughNode>(descriptor);
        };
        rin::NodeGraphBuild build = rin::buildNodeGraph(graph, catalog, factory);
        RIN_CHECK(build.validation.ok);
        RIN_CHECK(build.graph != nullptr);
        RIN_CHECK_EQ(build.graph->size(), std::size_t{2});

        const auto outputs = rin::runNodeGraph(
            *build.graph,
            [](const rin::NodeGraph::Node&) { return rin::ImageU8::make(rin::PortType::Rgba8, 2, 1); });
        RIN_CHECK_EQ(outputs.size(), std::size_t{2});
        RIN_CHECK(outputs[0][0].valid() && outputs[0][0].format() == rin::PortType::Rgba8);
        RIN_CHECK(outputs[1][0].valid() && outputs[1][0].format() == rin::PortType::Gray8);
    }

    // M4-03 几何算子工厂（仅公开头可见性 + 最小实例化，RULE-01）：rin/image_ops.hpp
    // 的 makeDefaultImageNode 经公开头可用——crop 返回实现（参数 schema 需声明，
    // 构造期参数缺失抛 std::invalid_argument 是冻结契约）、source 返回 nullptr
    // （注入型语义的最小见证；数值 golden 归 test_image_ops_geometry.cpp）。
    {
        rin::NodeDescriptor crop;
        crop.typeId = "crop";
        crop.displayName = "裁切";
        crop.inputs = {rin::PortType::Rgba8};
        crop.outputs = {rin::PortType::Rgba8};
        for (const char* id : {"x", "y", "width", "height"}) {
            rin::ParamDescriptor param;
            param.id = id;
            param.label = id;
            param.kind = rin::ParamKind::Integer;
            param.defaultValue = static_cast<std::int64_t>(0);
            crop.params.push_back(std::move(param));
        }
        rin::NodeDescriptor source;
        source.typeId = "source";
        source.displayName = "源";
        source.outputs = {rin::PortType::Rgba8};

        rin::NodeInstance cropInstance;
        cropInstance.id = 1;
        cropInstance.typeId = "crop";
        const std::unique_ptr<rin::IImageNode> node =
            rin::makeDefaultImageNode(crop, cropInstance);
        RIN_CHECK(node != nullptr);
        RIN_CHECK(node != nullptr && node->descriptor().typeId == "crop");

        rin::NodeInstance sourceInstance;
        sourceInstance.id = 2;
        sourceInstance.typeId = "source";
        RIN_CHECK(rin::makeDefaultImageNode(source, sourceInstance) == nullptr);
    }
    return rin_test::exitStatus();
}
