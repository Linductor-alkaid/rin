// 公开契约边界：仅包含 rin 公开头（目标未链接第三方 include 路径），
// 并实例化契约类型/接口，确保公开头零第三方类型（RULE-01）。
#include "test_util.hpp"

#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <rin/camera_service.hpp>
#include <rin/depth_preproc.hpp>
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
    bool requestDepthColorScheme(rin::DepthColorScheme, std::string*) override { return false; }
    void stop() override {}
    rin::CameraServiceState state() const override { return rin::CameraServiceState::Idle; }
    std::string lastError() const override { return {}; }
    bool tryLoadFrame(rin::FrameKind, std::uint64_t&, rin::Frame&) override { return false; }
    // M6-04（DEC-017）：灰度 rendition 通道同样是公开契约纯虚——无生产者桩恒 false。
    bool tryLoadGrayFrame(rin::GrayFrameKind, std::uint64_t&, rin::GrayFrame&) override {
        return false;
    }
    bool tryLoadDepthMetric(std::uint64_t&, rin::DepthMetricSample&) override { return false; }
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
                            std::string*) override {
        return false;
    }
    rin::AdmissionResult start() override { return {}; }
    void stop() override {}
    rin::WorkflowEngineState state() const override { return rin::WorkflowEngineState::Idle; }
    std::string lastError() const override { return {}; }
    bool tryLoadStats(std::uint64_t&, rin::WorkflowStats&) override { return false; }
    bool tryLoadNodeOutput(rin::NodeId, std::uint64_t&, rin::NodeOutputSnapshot&) override {
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
    explicit PassThroughNode(rin::NodeDescriptor descriptor) : descriptor_(std::move(descriptor)) {}

    const rin::NodeDescriptor& descriptor() const noexcept override { return descriptor_; }
    std::vector<rin::ImageU8> apply(const std::vector<rin::ImageU8>& inputs) const override {
        std::vector<rin::ImageU8> outputs;
        for (rin::PortType type : descriptor_.outputs) {
            outputs.push_back(
                inputs.empty()
                    ? rin::ImageU8::make(type, 2, 1)
                    : rin::ImageU8::wrap(type, inputs.front().width(), inputs.front().height(),
                                         inputs.front().stride(), inputs.front().pixels()));
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

    // M6-04 灰度 rendition 通道（经公开接口多态调用，同时实例化 GrayFrame 契约
    // 类型）：无生产者桩语义与 RGBA8 通道一致——false 且出参/序号不动；附带
    // GrayFrame::valid() 的最小形态核对（M6-04 契约类型实例化）。
    {
        rin::GrayFrame gray;
        gray.width = 8;
        gray.height = 4;
        gray.stride = 8;
        RIN_CHECK(!gray.valid());  // pixels 缺失 → 无效（stride==width 已满足）。
        gray.pixels = std::make_shared<const std::vector<std::uint8_t>>(8 * 4, 0);
        RIN_CHECK(gray.valid());

        std::uint64_t graySequence = 13;
        rin::GrayFrame probe;
        probe.sequence = 777;
        RIN_CHECK(!service->tryLoadGrayFrame(rin::GrayFrameKind::Depth, graySequence, probe));
        RIN_CHECK(
            !service->tryLoadGrayFrame(rin::GrayFrameKind::DepthAdaptive, graySequence, probe));
        RIN_CHECK_EQ(graySequence, std::uint64_t{13});
        RIN_CHECK_EQ(probe.sequence, std::uint64_t{777});
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

        const auto outputs = rin::runNodeGraph(*build.graph, [](const rin::NodeGraph::Node&) {
            return rin::ImageU8::make(rin::PortType::Rgba8, 2, 1);
        });
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
        const std::unique_ptr<rin::IImageNode> node = rin::makeDefaultImageNode(crop, cropInstance);
        RIN_CHECK(node != nullptr);
        RIN_CHECK(node != nullptr && node->descriptor().typeId == "crop");

        rin::NodeInstance sourceInstance;
        sourceInstance.id = 2;
        sourceInstance.typeId = "source";
        RIN_CHECK(rin::makeDefaultImageNode(source, sourceInstance) == nullptr);
    }

    // M4-04 卷积算子工厂（仅公开头可见性 + 最小实例化，RULE-01）：gaussian_blur /
    // conv_kernel 声明完整 schema（§6 表格）后经 makeDefaultImageNode 返回实现且
    // typeId 一致（数值 golden 归 test_image_ops_convolution.cpp；构造期参数缺失
    // 抛 std::invalid_argument 是冻结契约，此处声明完整 schema 不触发）。
    {
        rin::NodeDescriptor blur;
        blur.typeId = "gaussian_blur";
        blur.displayName = "高斯模糊";
        blur.inputs = {rin::PortType::Gray8};
        blur.outputs = {rin::PortType::Gray8};
        {
            rin::ParamDescriptor radius;
            radius.id = "radius";
            radius.label = "radius";
            radius.kind = rin::ParamKind::Integer;
            radius.defaultValue = static_cast<std::int64_t>(3);
            radius.hasRange = true;
            radius.minValue = 1.0;
            radius.maxValue = 10.0;
            blur.params.push_back(std::move(radius));
        }
        {
            rin::ParamDescriptor sigma;
            sigma.id = "sigma";
            sigma.label = "sigma";
            sigma.kind = rin::ParamKind::Real;
            sigma.defaultValue = 1.5;
            sigma.hasRange = true;
            sigma.minValue = 0.0;
            sigma.maxValue = 10.0;
            blur.params.push_back(std::move(sigma));
        }
        rin::NodeInstance blurInstance;
        blurInstance.id = 1;
        blurInstance.typeId = "gaussian_blur";
        const std::unique_ptr<rin::IImageNode> blurNode =
            rin::makeDefaultImageNode(blur, blurInstance);
        RIN_CHECK(blurNode != nullptr);
        RIN_CHECK(blurNode != nullptr && blurNode->descriptor().typeId == "gaussian_blur");

        rin::NodeDescriptor conv;
        conv.typeId = "conv_kernel";
        conv.displayName = "自定义卷积";
        conv.inputs = {rin::PortType::Gray8};
        conv.outputs = {rin::PortType::Gray8};
        {
            rin::ParamDescriptor size;
            size.id = "size";
            size.label = "size";
            size.kind = rin::ParamKind::Enumeration;
            size.defaultValue = std::string("3");
            size.enumOptions = {"1", "3", "5"};
            conv.params.push_back(std::move(size));
        }
        {
            rin::ParamDescriptor kernel;
            kernel.id = "kernel";
            kernel.label = "kernel";
            kernel.kind = rin::ParamKind::RealArray;
            kernel.defaultValue =
                std::vector<double>{0, 0, 0, 0, 1, 0, 0, 0, 0};  // 3×3 单位核（行主序）
            conv.params.push_back(std::move(kernel));
        }
        {
            rin::ParamDescriptor border;
            border.id = "border";
            border.label = "border";
            border.kind = rin::ParamKind::Enumeration;
            border.defaultValue = std::string("clamp");
            border.enumOptions = {"clamp", "reflect", "zero"};
            conv.params.push_back(std::move(border));
        }
        rin::NodeInstance convInstance;
        convInstance.id = 2;
        convInstance.typeId = "conv_kernel";
        const std::unique_ptr<rin::IImageNode> convNode =
            rin::makeDefaultImageNode(conv, convInstance);
        RIN_CHECK(convNode != nullptr);
        RIN_CHECK(convNode != nullptr && convNode->descriptor().typeId == "conv_kernel");
    }

    // M4-05 直方图均衡与灰度化算子工厂（仅公开头可见性 + 最小实例化，RULE-01）：
    // grayify（Rgba8→Gray8）/ hist_eq（Gray8→Gray8）均无参数（§6 表格），无参数
    // 节点无构造期拒绝分支——声明不含参数直接实例化即返回实现（数值 golden 归
    // test_image_ops_histogram.cpp）。
    {
        rin::NodeDescriptor grayify;
        grayify.typeId = "grayify";
        grayify.displayName = "灰度化";
        grayify.inputs = {rin::PortType::Rgba8};
        grayify.outputs = {rin::PortType::Gray8};
        rin::NodeInstance grayifyInstance;
        grayifyInstance.id = 1;
        grayifyInstance.typeId = "grayify";
        const std::unique_ptr<rin::IImageNode> grayifyNode =
            rin::makeDefaultImageNode(grayify, grayifyInstance);
        RIN_CHECK(grayifyNode != nullptr);
        RIN_CHECK(grayifyNode != nullptr && grayifyNode->descriptor().typeId == "grayify");

        rin::NodeDescriptor histEq;
        histEq.typeId = "hist_eq";
        histEq.displayName = "直方图均衡";
        histEq.inputs = {rin::PortType::Gray8};
        histEq.outputs = {rin::PortType::Gray8};
        rin::NodeInstance histEqInstance;
        histEqInstance.id = 2;
        histEqInstance.typeId = "hist_eq";
        const std::unique_ptr<rin::IImageNode> histEqNode =
            rin::makeDefaultImageNode(histEq, histEqInstance);
        RIN_CHECK(histEqNode != nullptr);
        RIN_CHECK(histEqNode != nullptr && histEqNode->descriptor().typeId == "hist_eq");
    }

    // M4-06 FFT 滤波族工厂（仅公开头可见性 + 最小实例化，RULE-01）：三类型按
    // §6 表格声明完整 schema（Real 参数 + [0,1] 闭区间）后经 makeDefaultImageNode
    // 返回实现且 typeId 一致；实例不赋值 → 构造期取声明默认值（cutoff/lowCut
    // 0.2、highCut 0.6）。构造期参数缺失抛 std::invalid_argument 是冻结契约，
    // 以无参数声明的变体做最小拒绝见证（数值 golden 归 test_image_ops_fft.cpp）。
    {
        struct FftCase {
            const char* typeId;
            std::vector<const char*> paramIds;
        };
        const std::vector<FftCase> fftCases = {
            {"fft_lowpass", {"cutoff"}},
            {"fft_highpass", {"cutoff"}},
            {"fft_bandpass", {"lowCut", "highCut"}},
        };
        std::size_t instanceId = 1;
        for (const FftCase& fftCase : fftCases) {
            rin::NodeDescriptor descriptor;
            descriptor.typeId = fftCase.typeId;
            descriptor.displayName = fftCase.typeId;
            descriptor.inputs = {rin::PortType::Gray8};
            descriptor.outputs = {rin::PortType::Gray8};
            for (const char* id : fftCase.paramIds) {
                rin::ParamDescriptor param;
                param.id = id;
                param.label = id;
                param.kind = rin::ParamKind::Real;
                param.defaultValue = std::string(id) == "highCut" ? 0.6 : 0.2;
                param.hasRange = true;
                param.minValue = 0.0;
                param.maxValue = 1.0;
                descriptor.params.push_back(std::move(param));
            }
            rin::NodeInstance instance;
            instance.id = instanceId;
            instance.typeId = fftCase.typeId;
            const std::unique_ptr<rin::IImageNode> fftNode =
                rin::makeDefaultImageNode(descriptor, instance);
            RIN_CHECK(fftNode != nullptr);
            RIN_CHECK(fftNode != nullptr && fftNode->descriptor().typeId == fftCase.typeId);

            // 缺参拒绝见证：声明不含参数 → 实例无赋值且无默认可读 → 构造抛。
            rin::NodeDescriptor stripped = descriptor;
            stripped.params.clear();
            bool rejected = false;
            try {
                (void)rin::makeDefaultImageNode(stripped, instance);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            RIN_CHECK_MSG(rejected, std::string("FFT 工厂缺参构造拒绝：") + fftCase.typeId);
            ++instanceId;
        }
    }

    // M8 米制深度策略预处理算子（仅公开头可见性 + 最小实例化，RULE-01）：
    // rin/depth_preproc.hpp 的 DepthFrameF32（make/wrap/valid）、PolicyDepthConfig
    // （默认值/派生量）、O1 纯函数与 PolicyDepthHistory（append/sample）经公开头
    // 可用且不引入第三方类型（数值 golden 归 test_depth_preproc.cpp）。
    {
        rin::DepthFrameF32 frame = rin::DepthFrameF32::make(4, 2);
        RIN_CHECK(frame.valid());
        RIN_CHECK_EQ(frame.stride(), std::uint32_t{4});  // stride=0 → width（元素数）

        rin::PolicyDepthConfig config;
        RIN_CHECK(config.valid());
        RIN_CHECK_EQ(config.policyWidth(), std::uint32_t{32});
        RIN_CHECK_EQ(config.policyHeight(), std::uint32_t{18});

        rin::DepthFrameF32 filled = rin::fillDepthInvalid(frame, 2.5);
        RIN_CHECK(filled.valid() && filled.width() == 4 && filled.height() == 2);

        // 时序历史最小往返：非默认小配置（grid 8×8、crop (2,1,1,1) → 6×5 帧，
        // history 4、sample 2 skip 2 → framesNeeded = 3 ≤ 4）。
        rin::PolicyDepthConfig small;
        small.gridWidth = 8;
        small.gridHeight = 8;
        small.cropUp = 2;
        small.cropDown = 1;
        small.cropLeft = 1;
        small.cropRight = 1;
        small.historyLength = 4;
        small.sampleCount = 2;
        small.sampleSkip = 2;
        RIN_CHECK(small.valid());
        RIN_CHECK_EQ(small.policyWidth(), std::uint32_t{6});
        RIN_CHECK_EQ(small.policyHeight(), std::uint32_t{5});

        auto policyPixels = std::make_shared<const std::vector<float>>(30u, 1.0f);
        rin::DepthFrameF32 policyFrame = rin::DepthFrameF32::wrap(6, 5, 6, policyPixels);
        RIN_CHECK(policyFrame.valid());

        rin::PolicyDepthHistory history(small);
        history.append(policyFrame);
        RIN_CHECK_EQ(history.size(), std::size_t{1});
        RIN_CHECK_EQ(history.sample().size(), std::size_t{2} * 5 * 6);
    }

    // M10 深度域工作流端口（仅公开头可见性 + 最小实例化，RULE-01，DEC-020）：
    // rin/workflow_types.hpp 的 PortType::Depth32F（ImageU8 字节容器按 host 端序
    // 承载 float32 米制深度，elementSize = 4）、rin/image_types.hpp 的
    // depthF32Row 行视图助手、rin/image_node.hpp 的 IStatefulImageNode 标记接口
    // （引擎串行在飞探测面）经公开头可用且不引入第三方类型（数值 golden 归
    // test_image_ops_depth.cpp，引擎时序归 test_workflow_depth_engine.cpp）。
    {
        // Depth32F 端口语义最小实例化：elementSize = 4、ImageU8 承载、行视图。
        RIN_CHECK_EQ(rin::elementSize(rin::PortType::Depth32F), std::uint32_t{4});
        rin::ImageU8 depth = rin::ImageU8::make(rin::PortType::Depth32F, 2, 1);
        RIN_CHECK(depth.valid());
        RIN_CHECK_EQ(depth.stride(), std::uint32_t{8});  // 2 像素 × 4 字节（紧凑）。
        {
            // 行视图写读最小往返（wrap 前 buffer 可写；const 视图经 depthF32Row 读）。
            auto pixels = std::make_shared<std::vector<std::uint8_t>>(8);
            const float values[2] = {0.5f, 1.25f};
            std::memcpy(pixels->data(), values, sizeof(values));
            rin::ImageU8 wrapped = rin::ImageU8::wrap(
                rin::PortType::Depth32F, 2, 1, 8,
                std::const_pointer_cast<const std::vector<std::uint8_t>>(pixels));
            RIN_CHECK(wrapped.valid());
            const float* row = rin::depthF32Row(wrapped, 0);
            RIN_CHECK(row != nullptr);
            if (row != nullptr) {
                RIN_CHECK_EQ(row[0], 0.5f);
                RIN_CHECK_EQ(row[1], 1.25f);
            }
            RIN_CHECK(rin::depthF32Row(wrapped, 1) == nullptr);  // 越界 nullptr。
        }

        // IStatefulImageNode 标记接口最小实例化：双继承节点经 IImageNode* 可
        // dynamic_cast 探测（引擎 buildGeneration 的编译期探测面）。
        class WitnessStatefulNode final : public rin::IImageNode, public rin::IStatefulImageNode {
        public:
            const rin::NodeDescriptor& descriptor() const noexcept override { return descriptor_; }
            std::vector<rin::ImageU8> apply(
                const std::vector<rin::ImageU8>& inputs) const override {
                return inputs;
            }

        private:
            rin::NodeDescriptor descriptor_;
        };
        WitnessStatefulNode stateful;
        rin::IImageNode* asImage = &stateful;
        RIN_CHECK(dynamic_cast<rin::IStatefulImageNode*>(asImage) != nullptr);

        class WitnessStatelessNode final : public rin::IImageNode {
        public:
            const rin::NodeDescriptor& descriptor() const noexcept override { return descriptor_; }
            std::vector<rin::ImageU8> apply(
                const std::vector<rin::ImageU8>& inputs) const override {
                return inputs;
            }

        private:
            rin::NodeDescriptor descriptor_;
        };
        WitnessStatelessNode stateless;
        rin::IImageNode* asImage2 = &stateless;
        RIN_CHECK(dynamic_cast<rin::IStatefulImageNode*>(asImage2) == nullptr);
    }

    return rin_test::exitStatus();
}
