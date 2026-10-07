// M11 监看器节点与 Any 端口类型契约测试 —— 独立验证（Independent Verification
// Agent）。按被测契约（include/rin/workflow_types.hpp、src/core/{workflow_types,
// image_types,node_graph,image_ops}.cpp、src/workflow/default_catalog.hpp、
// apps/viewer/canvas_model.hpp；决策 docs/decisions/DEC-021-monitor-node-and-
// inline-editing.md）独立设计与执行，不依赖开发者自述。
//
// 被测面：
//   1. PortType::Any 契约基元：toString(Any)=="Any"、四型字符串互异；
//      elementSize(Any)==0（三具体型回归 1/4/4）；ImageU8::make/wrap 以 Any
//      为格式返回无效图像（实际图像格式永不为 Any）；
//   2. validateWorkflowGraph Any 判据全矩阵（默认目录）：Any 输入 ×
//      Rgba8/Gray8/Depth32F 输出均合法；Any 输出 → 三具体型输入各一拒绝
//      （TypeMismatch）；Any→Any 合法；三型等值连接与具体型错配回归不变；
//   3. 目录：makeDefaultImageNodeCatalog 含 viewer（inputs={Any}、outputs={Any}、
//      无参数、displayName 监看器）、追加于既有条目之后（source 居首契约保持、
//      既有 22 项顺序不变）、catalog.valid()；
//   4. 恒等节点（makeDefaultImageNode("viewer")）：非 nullptr、
//      descriptor().typeId=="viewer"、apply 单有效输入透传（共享像素零拷贝、
//      输出格式=输入格式）、输入数≠1 与无效图抛 std::invalid_argument；
//   5. runNodeGraph 集成：source(Rgba8)→viewer / source_depth_gray(Gray8)→
//      viewer / source_depth_metric(Depth32F)→viewer 三图注入合成帧，viewer
//      输出与注入帧逐字节一致且共享缓冲（零拷贝）；声明 Any 的输出端口接受
//      任意具体格式产出（防御核对路径）；
//   6. canvas_model 纯逻辑（M11/DEC-021）：paramSectionHeight 全分支（监看器 0 /
//      无参非源 0 / 相机源 Camera resolution 行 / 标量参数 / 枚举 / RealArray
//      实例形状 9→3 行、25→5 行）、nodeHeight(viewer) = 标题+预览窗+间距+
//      max(1,1) 端口行+页脚、portPosition 纵向偏移含参数区、connect Any 三型
//      接受与 Any 输出拒绝、portAt 跳过监看器输出端口（输入端口正常命中）、
//      paletteGroupFor("viewer")=="View"、isMonitorNode/isCameraSourceNode 谓词。
//
// WorkflowCanvasState::afterGraphChange 的 controls/driverSizes 清扫与
// pumpMonitorViews/pumpDriverSizes 的生命周期语义需 eui_neo 链接（node_canvas.hpp）
// 与真引擎，在 tests/test_run_control.cpp §3/§7/§9 覆盖（headless 纪律同该文件）。
//
// 测试壳为 tests/test_util.hpp 的 RIN_CHECK*（无第三方框架），main 返回
// rin_test::exitStatus()。单线程纯逻辑，无 sleep。链接 rin::core（viewer 头为
// header-only；默认目录为纯头文件单一事实源，无 src 链接——与
// test_workflow_contracts 同纪律）。

#include "canvas_model.hpp"

#include "test_util.hpp"

#include "validation_test_util.hpp"

#include "default_catalog.hpp"

#include <rin/image_node.hpp>
#include <rin/image_ops.hpp>
#include <rin/image_types.hpp>
#include <rin/node_graph.hpp>
#include <rin/workflow_types.hpp>

#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// nearF/runSection/hasKind 由 tests/test_util.hpp 唯一提供
// （M12/CR-39；原本文件手抄副本已删除）。
using rin_test::hasKind;
using rin_test::nearF;
using rin_test::runSection;

[[nodiscard]] bool imagesByteEqual(const rin::ImageU8& a, const rin::ImageU8& b) {
    return a.pixels() != nullptr && a.pixels() == b.pixels() &&
           a.byteSize() == b.byteSize() && *a.pixels() == *b.pixels();
}

/// 两节点链 A(out0) → B(in0) 的目录图（默认目录）。
[[nodiscard]] rin::WorkflowGraph makeChain(const char* aTypeId, const char* bTypeId) {
    rin::WorkflowGraph graph;
    rin::NodeInstance a;
    a.id = 1;
    a.typeId = aTypeId;
    rin::NodeInstance b;
    b.id = 2;
    b.typeId = bTypeId;
    graph.nodes = {a, b};
    graph.connections = {
        rin::Connection{{1, rin::PortDirection::Output, 0},
                        {2, rin::PortDirection::Input, 0}},
    };
    return graph;
}

/// 三节点链 A → B → C（默认目录）。
[[nodiscard]] rin::WorkflowGraph makeTriple(const char* aTypeId, const char* bTypeId,
                                            const char* cTypeId) {
    rin::WorkflowGraph graph;
    const char* typeIds[3] = {aTypeId, bTypeId, cTypeId};
    for (std::uint64_t i = 0; i < 3; ++i) {
        rin::NodeInstance instance;
        instance.id = i + 1;
        instance.typeId = typeIds[i];
        graph.nodes.push_back(instance);
    }
    for (std::uint64_t i = 0; i < 2; ++i) {
        graph.connections.push_back(
            rin::Connection{{i + 1, rin::PortDirection::Output, 0},
                            {i + 2, rin::PortDirection::Input, 0}});
    }
    return graph;
}

// --- 1. PortType::Any 契约基元 ---

void testAnyPortTypeBasics() {
    // toString：四型互异、Any 字面量。
    RIN_CHECK_MSG(std::string(rin::toString(rin::PortType::Any)) == "Any",
                  "toString(Any) == \"Any\"");
    RIN_CHECK(std::string(rin::toString(rin::PortType::Gray8)) == "Gray8");
    RIN_CHECK(std::string(rin::toString(rin::PortType::Rgba8)) == "Rgba8");
    RIN_CHECK(std::string(rin::toString(rin::PortType::Depth32F)) == "Depth32F");

    // elementSize：Any=0（实际图像格式不可为 Any 的天然拒绝）；具体型回归。
    RIN_CHECK_EQ(rin::elementSize(rin::PortType::Gray8), 1u);
    RIN_CHECK_EQ(rin::elementSize(rin::PortType::Rgba8), 4u);
    RIN_CHECK_EQ(rin::elementSize(rin::PortType::Depth32F), 4u);
    RIN_CHECK_EQ(rin::elementSize(rin::PortType::Any), 0u);

    // ImageU8::make 以 Any 为格式：元数据校验拒绝 → 无效图像（不抛）。
    {
        const rin::ImageU8 image = rin::ImageU8::make(rin::PortType::Any, 8, 6);
        RIN_CHECK_MSG(!image.valid(), "make(Any) yields an invalid image");
        RIN_CHECK_EQ(image.byteSize(), std::uint64_t{0});
        RIN_CHECK(image.pixels() == nullptr);
    }
    // wrap 同理（零拷贝路径同样拒绝 Any）。
    {
        const auto buffer = std::make_shared<const std::vector<std::uint8_t>>(32, 0);
        const rin::ImageU8 image =
            rin::ImageU8::wrap(rin::PortType::Any, 8, 6, 4, buffer);
        RIN_CHECK_MSG(!image.valid(), "wrap(Any) yields an invalid image");
        RIN_CHECK(image.pixels() == nullptr);
    }
}

// --- 2. validateWorkflowGraph Any 判据全矩阵 ---

void testAnyValidationMatrix() {
    const rin::NodeCatalog catalog = rin::workflow_catalog::makeDefaultImageNodeCatalog();

    // Any 输入（viewer.in0）× 三具体型输出：均合法（无任何问题）。
    {
        const char* sources[3] = {"source", "source_depth_gray", "source_depth_metric"};
        for (const char* sourceType : sources) {
            const rin::WorkflowValidation v =
                rin::validateWorkflowGraph(makeChain(sourceType, "viewer"), catalog);
            RIN_CHECK_MSG(v.ok && v.issues.empty(),
                          (std::string("Any input accepts ") + sourceType + " output")
                              .c_str());
        }
    }

    // Any 输出 → 具体型输入：TypeMismatch（三型各一）。
    {
        const char* consumers[3] = {"grayify", "gaussian_blur", "depth_normalize"};
        for (const char* consumer : consumers) {
            // source(Any 兼容) → viewer(Any) → consumer(具体型输入)：
            // 唯一类型问题 = viewer.out0 的 Any 到具体型。
            const rin::WorkflowValidation v = rin::validateWorkflowGraph(
                makeTriple("source", "viewer", consumer), catalog);
            RIN_CHECK_MSG(!v.ok, (std::string("Any output into ") + consumer +
                                  " input must not validate")
                                     .c_str());
            RIN_CHECK_MSG(hasKind(v, rin::ValidationIssueKind::TypeMismatch),
                          (std::string("Any output into ") + consumer +
                           " input reports TypeMismatch")
                              .c_str());
            // 拒绝原因命名两侧类型（Any 与目标具体型）。
            bool namesAnyAndTarget = false;
            for (const rin::ValidationIssue& issue : v.issues) {
                if (issue.kind == rin::ValidationIssueKind::TypeMismatch &&
                    issue.message.find("Any") != std::string::npos) {
                    namesAnyAndTarget = true;
                }
            }
            RIN_CHECK_MSG(namesAnyAndTarget,
                          (std::string("mismatch message names Any for ") + consumer)
                              .c_str());
        }
    }

    // Any → Any：viewer → viewer 合法（全图无悬空输入）。
    {
        const rin::WorkflowValidation v = rin::validateWorkflowGraph(
            makeTriple("source", "viewer", "viewer"), catalog);
        RIN_CHECK_MSG(v.ok && v.issues.empty(), "Any output into Any input validates");
    }

    // 三型等值连接回归：判据仍是"两端相等"。
    {
        RIN_CHECK_MSG(rin::validateWorkflowGraph(makeChain("source", "grayify"), catalog)
                          .ok,
                      "Rgba8 -> Rgba8 equal-type edge still validates");
        RIN_CHECK_MSG(
            rin::validateWorkflowGraph(makeChain("source_depth_gray", "gaussian_blur"),
                                       catalog)
                .ok,
            "Gray8 -> Gray8 equal-type edge still validates");
        RIN_CHECK_MSG(rin::validateWorkflowGraph(
                          makeChain("source_depth_metric", "depth_normalize"), catalog)
                          .ok,
                      "Depth32F -> Depth32F equal-type edge still validates");
        // 具体型错配回归：Rgba8 -> Gray8 输入仍拒绝。
        const rin::WorkflowValidation mismatch =
            rin::validateWorkflowGraph(makeChain("source", "gaussian_blur"), catalog);
        RIN_CHECK_MSG(!mismatch.ok &&
                          hasKind(mismatch, rin::ValidationIssueKind::TypeMismatch),
                      "concrete Rgba8 -> Gray8 mismatch still rejected");
    }
}

// --- 3. 目录 viewer 项 ---

void testCatalogViewerEntry() {
    const rin::NodeCatalog catalog = rin::workflow_catalog::makeDefaultImageNodeCatalog();
    RIN_CHECK_MSG(catalog.valid(), "default catalog with viewer stays valid");

    const rin::NodeDescriptor* viewer = rin::findNodeDescriptor(catalog, "viewer");
    RIN_CHECK_MSG(viewer != nullptr, "catalog contains 'viewer'");
    if (viewer == nullptr) {
        return;
    }
    RIN_CHECK_MSG(viewer->typeId == "viewer", "viewer typeId");
    RIN_CHECK_MSG(viewer->displayName == "监看器", "viewer displayName");
    RIN_CHECK_MSG(viewer->inputs.size() == 1 &&
                      viewer->inputs[0] == rin::PortType::Any,
                  "viewer signature: single Any input");
    RIN_CHECK_MSG(viewer->outputs.size() == 1 &&
                      viewer->outputs[0] == rin::PortType::Any,
                  "viewer signature: single Any output");
    RIN_CHECK_MSG(viewer->params.empty(), "viewer declares no parameters");
    RIN_CHECK_MSG(viewer->valid(), "viewer descriptor is valid");

    // 追加位置契约：viewer 是末项；source 居首（契约套件泛式构图假设）；
    // 既有 22 项（HEAD 基线）顺序不变。
    RIN_CHECK_EQ(catalog.nodes.size(), std::size_t{23});
    RIN_CHECK_MSG(catalog.nodes.front().typeId == "source",
                  "source remains the first catalog entry");
    RIN_CHECK_MSG(catalog.nodes.back().typeId == "viewer",
                  "viewer appended after all existing entries");
    const char* kHeadOrder[22] = {
        "source",           "source_depth_jet",     "source_depth_gray",
        "source_depth_adaptive", "crop",            "crop_gray",
        "downscale",        "downscale_gray",       "grayify",
        "gaussian_blur",    "conv_kernel",          "hist_eq",
        "fft_lowpass",      "fft_highpass",         "fft_bandpass",
        "source_depth_metric", "depth_fill_invalid", "depth_resize",
        "depth_crop",       "depth_gaussian_blur",  "depth_normalize",
        "depth_history",
    };
    for (std::size_t i = 0; i < 22; ++i) {
        RIN_CHECK_MSG(catalog.nodes[i].typeId == kHeadOrder[i],
                      (std::string("existing entry order preserved at ") +
                       std::to_string(i) + ": expected " + kHeadOrder[i] + ", got " +
                       catalog.nodes[i].typeId)
                          .c_str());
    }
}

// --- 4. 恒等节点（makeDefaultImageNode("viewer")） ---

void testViewerIdentityNode() {
    const rin::NodeCatalog catalog = rin::workflow_catalog::makeDefaultImageNodeCatalog();
    const rin::NodeDescriptor* descriptor = rin::findNodeDescriptor(catalog, "viewer");
    RIN_CHECK(descriptor != nullptr);
    if (descriptor == nullptr) {
        return;
    }
    const rin::NodeInstance instance{1, "viewer", {}};
    const std::unique_ptr<rin::IImageNode> node =
        rin::makeDefaultImageNode(*descriptor, instance);
    RIN_CHECK_MSG(node != nullptr, "factory returns a node for 'viewer'");
    if (node == nullptr) {
        return;
    }
    RIN_CHECK_MSG(node->descriptor().typeId == "viewer",
                  "node reports its declared typeId");

    // 透传：三具体型各一，输出 = 输入（格式随输入、共享像素零拷贝、逐字节一致）。
    {
        struct Case {
            rin::PortType format;
            std::uint32_t width;
            std::uint32_t height;
            std::uint32_t stride;
        };
        const Case cases[] = {
            {rin::PortType::Gray8, 7, 5, 7},
            {rin::PortType::Rgba8, 6, 4, 24},
            {rin::PortType::Depth32F, 5, 3, 20},
        };
        for (const Case& c : cases) {
            std::vector<std::uint8_t> bytes(
                static_cast<std::size_t>(c.stride) * c.height);
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                bytes[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xFF);
            }
            const rin::ImageU8 input = rin::ImageU8::wrap(
                c.format, c.width, c.height, c.stride,
                std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes)));
            RIN_CHECK(input.valid());
            const std::vector<rin::ImageU8> outputs = node->apply({input});
            RIN_CHECK_EQ(outputs.size(), std::size_t{1});
            if (outputs.size() == 1) {
                RIN_CHECK_MSG(outputs[0].valid(), "viewer output is valid");
                RIN_CHECK_MSG(outputs[0].format() == input.format(),
                              "viewer output format follows the input");
                RIN_CHECK_MSG(outputs[0].pixels().get() == input.pixels().get(),
                              "viewer output shares the input buffer (zero copy)");
                RIN_CHECK_MSG(imagesByteEqual(outputs[0], input),
                              "viewer output bytes equal the input bytes");
                RIN_CHECK_EQ(outputs[0].width(), input.width());
                RIN_CHECK_EQ(outputs[0].height(), input.height());
                RIN_CHECK_EQ(outputs[0].stride(), input.stride());
            }
        }
    }

    // 输入数≠1：0 与 2 均抛 std::invalid_argument。
    {
        bool threwZero = false;
        try {
            (void)node->apply({});
        } catch (const std::invalid_argument&) {
            threwZero = true;
        }
        RIN_CHECK_MSG(threwZero, "apply with zero inputs throws invalid_argument");

        const rin::ImageU8 a = rin::ImageU8::make(rin::PortType::Gray8, 4, 4);
        const rin::ImageU8 b = rin::ImageU8::make(rin::PortType::Gray8, 4, 4);
        bool threwTwo = false;
        try {
            (void)node->apply({a, b});
        } catch (const std::invalid_argument&) {
            threwTwo = true;
        }
        RIN_CHECK_MSG(threwTwo, "apply with two inputs throws invalid_argument");
    }

    // 无效输入图：抛 invalid_argument（不静默）。
    {
        bool threwInvalid = false;
        try {
            (void)node->apply({rin::ImageU8{}});
        } catch (const std::invalid_argument&) {
            threwInvalid = true;
        }
        RIN_CHECK_MSG(threwInvalid, "apply with an invalid image throws invalid_argument");
    }
}

// --- 5. runNodeGraph 集成（注入合成帧；Any 输出的防御核对路径） ---

void testRunNodeGraphViewerPassthrough() {
    const rin::NodeCatalog catalog = rin::workflow_catalog::makeDefaultImageNodeCatalog();
    const rin::ImageNodeFactory factory = rin::makeDefaultImageNode;

    struct Case {
        const char* sourceType;
        rin::PortType format;
        std::uint32_t width;
        std::uint32_t height;
        std::uint32_t stride;
    };
    const Case cases[] = {
        {"source", rin::PortType::Rgba8, 6, 4, 24},
        {"source_depth_gray", rin::PortType::Gray8, 7, 5, 7},
        {"source_depth_metric", rin::PortType::Depth32F, 5, 3, 20},
    };
    for (const Case& c : cases) {
        // 合成帧：确定性图案（Depth32F 走 float 位模式，逐字节比较不经解释）。
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(c.stride) * c.height);
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<std::uint8_t>((i * 17 + 3) & 0xFF);
        }
        const rin::ImageU8 injected = rin::ImageU8::wrap(
            c.format, c.width, c.height, c.stride,
            std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes)));
        RIN_CHECK(injected.valid());

        const rin::NodeGraphBuild build =
            rin::buildNodeGraph(makeChain(c.sourceType, "viewer"), catalog, factory);
        RIN_CHECK_MSG(build.validation.ok && build.graph != nullptr,
                      (std::string("chain builds for ") + c.sourceType).c_str());
        if (build.graph == nullptr) {
            continue;
        }
        const std::vector<std::vector<rin::ImageU8>> outputs = rin::runNodeGraph(
            *build.graph,
            [&injected](const rin::NodeGraph::Node&) { return injected; });
        RIN_CHECK_EQ(outputs.size(), std::size_t{2});
        if (outputs.size() != 2) {
            continue;
        }
        // 源节点输出 = 注入帧（注入路径回归）。
        RIN_CHECK_MSG(imagesByteEqual(outputs[0][0], injected),
                      "source output equals the injected frame");
        // 监看器输出 = 注入帧逐字节一致 + 零拷贝；输出格式为具体型（Any 声明
        // 的防御核对接受任意具体格式产出，runNodeGraph 不抛）。
        RIN_CHECK_MSG(imagesByteEqual(outputs[1][0], injected),
                      (std::string("viewer output byte-equals the injected frame (") +
                       c.sourceType + ")")
                          .c_str());
        RIN_CHECK_MSG(outputs[1][0].pixels().get() == injected.pixels().get(),
                      "viewer output shares the injected buffer (zero copy)");
        RIN_CHECK_MSG(outputs[1][0].format() == c.format &&
                          c.format != rin::PortType::Any,
                      "viewer output carries the concrete input format, never Any");
    }
}

// --- 6. canvas_model 纯逻辑（M11/DEC-021 几何 / 连线 / 命中 / 分组 / 谓词） ---

void testCanvasMonitorModel() {
    const rin::NodeCatalog catalog = rin::workflow_catalog::makeDefaultImageNodeCatalog();

    // 谓词：kMonitorTypeId / isMonitorNode / isCameraSourceNode（前缀判定）。
    RIN_CHECK_MSG(std::string(viewer::kMonitorTypeId) == "viewer",
                  "kMonitorTypeId == \"viewer\"");
    RIN_CHECK_MSG(viewer::isMonitorNode("viewer"), "isMonitorNode(viewer)");
    RIN_CHECK_MSG(!viewer::isMonitorNode("monitor") && !viewer::isMonitorNode("") &&
                      !viewer::isMonitorNode("viewers"),
                  "isMonitorNode is exact-match, not prefix");
    RIN_CHECK_MSG(viewer::isCameraSourceNode("source"), "camera source: source");
    RIN_CHECK_MSG(viewer::isCameraSourceNode("source_depth_jet"),
                  "camera source: source_depth_jet");
    RIN_CHECK_MSG(viewer::isCameraSourceNode("source_depth_metric"),
                  "camera source: source_depth_metric");
    RIN_CHECK_MSG(!viewer::isCameraSourceNode("crop") &&
                      !viewer::isCameraSourceNode("viewer") &&
                      !viewer::isCameraSourceNode("my_source"),
                  "camera source is a 'source' prefix test");

    // paramSectionHeight 全分支。
    const rin::NodeDescriptor* viewerD = rin::findNodeDescriptor(catalog, "viewer");
    const rin::NodeDescriptor* grayify = rin::findNodeDescriptor(catalog, "grayify");
    const rin::NodeDescriptor* source = rin::findNodeDescriptor(catalog, "source");
    const rin::NodeDescriptor* downscale = rin::findNodeDescriptor(catalog, "downscale");
    const rin::NodeDescriptor* blur = rin::findNodeDescriptor(catalog, "gaussian_blur");
    const rin::NodeDescriptor* conv = rin::findNodeDescriptor(catalog, "conv_kernel");
    RIN_CHECK((viewerD != nullptr && grayify != nullptr && source != nullptr &&
               downscale != nullptr && blur != nullptr && conv != nullptr));
    if (viewerD == nullptr || grayify == nullptr || source == nullptr ||
        downscale == nullptr || blur == nullptr || conv == nullptr) {
        return;
    }
    // 监看器：参数区 0（预览窗替代）。
    RIN_CHECK_MSG(nearF(viewer::paramSectionHeight(*viewerD, {}), 0.0f),
                  "monitor param section is 0");
    // 无参数且非相机源：0（端口区紧跟标题）。
    RIN_CHECK_MSG(nearF(viewer::paramSectionHeight(*grayify, {}), 0.0f),
                  "paramless non-source node has no param section");
    // 相机源：Camera resolution 行 = 首距 6 + 胶囊 18 + 行距 4。
    RIN_CHECK_MSG(nearF(viewer::paramSectionHeight(*source, {}), 28.0f),
                  "camera source param section = 6 + 18 + 4");
    // 枚举 + 带范围标量（downscale）：6 + (18+4) + (14+12+4)。
    RIN_CHECK_MSG(nearF(viewer::paramSectionHeight(*downscale, {}), 58.0f),
                  "downscale param section = enum pill + ranged scalar rows");
    // 两个带范围标量（gaussian_blur）：6 + (26+4) + (26+4)。
    RIN_CHECK_MSG(nearF(viewer::paramSectionHeight(*blur, {}), 66.0f),
                  "blur param section = two ranged scalar rows");
    // RealArray 实例形状联动（conv_kernel）：默认 9 值 → 3x3 网格。
    RIN_CHECK_MSG(nearF(viewer::paramSectionHeight(*conv, {}), 128.0f),
                  "conv default (3x3 kernel) param section");
    // 25 值赋值 → 5x5 网格（行数随实例生效值）。
    {
        std::vector<rin::ParamAssignment> wide;
        wide.push_back({"kernel", std::vector<double>(25, 0.5)});
        RIN_CHECK_MSG(nearF(viewer::paramSectionHeight(*conv, wide), 168.0f),
                      "conv 25-value assignment (5x5) param section");
    }
    // 9 值显式赋值与默认等价。
    {
        std::vector<rin::ParamAssignment> same;
        same.push_back({"kernel", std::vector<double>(9, 1.0)});
        RIN_CHECK_MSG(nearF(viewer::paramSectionHeight(*conv, same), 128.0f),
                      "conv explicit 9-value assignment keeps 3x3");
    }
    // paramGridRows / paramRowHeight 单元（各 ParamKind 行高冻结值）。注意
    // effectiveParamValue 只在实例赋值与"节点描述符声明内"查找默认值——合成
    // 参数必须经本地目录声明承载（挂在无参描述符上会取不到默认值）。
    {
        rin::ParamDescriptor pd;
        pd.id = "p";
        pd.label = "P";
        pd.kind = rin::ParamKind::Boolean;
        pd.defaultValue = false;
        rin::NodeCatalog local;
        local.nodes.push_back({"host", "Host", {}, {}, {pd}});
        const rin::NodeDescriptor* host = rin::findNodeDescriptor(local, "host");
        RIN_CHECK(host != nullptr);
        if (host != nullptr) {
            RIN_CHECK_MSG(nearF(viewer::paramRowHeight(*host, pd, {}), 18.0f),
                          "Boolean row = pill height 18");
            pd.kind = rin::ParamKind::Real;
            pd.defaultValue = 1.0;
            pd.hasRange = true;
            pd.minValue = 0.0;
            pd.maxValue = 10.0;
            RIN_CHECK_MSG(nearF(viewer::paramRowHeight(*host, pd, {}), 26.0f),
                          "ranged scalar row = label 14 + slider 12");
            pd.kind = rin::ParamKind::Enumeration;
            pd.defaultValue = std::string("a");
            pd.enumOptions = {"a", "b"};
            RIN_CHECK_MSG(nearF(viewer::paramRowHeight(*host, pd, {}), 18.0f),
                          "enumeration row = pill height 18");
            // RealArray 行随形状：描述符必须承载最终形态（目录存的是声明副本，
            // 先 push 再改写 pd 不会同步——故以终态声明重建宿主）。
            pd.kind = rin::ParamKind::RealArray;
            pd.defaultValue = std::vector<double>(9, 0.0);
            rin::NodeCatalog arrayHost;
            arrayHost.nodes.push_back({"arrayHost", "ArrayHost", {}, {}, {pd}});
            const rin::NodeDescriptor* arrayDesc =
                rin::findNodeDescriptor(arrayHost, "arrayHost");
            RIN_CHECK(arrayDesc != nullptr);
            if (arrayDesc != nullptr) {
                RIN_CHECK_MSG(nearF(viewer::paramRowHeight(*arrayDesc, pd, {}), 74.0f),
                              "RealArray row = shape 14 + 3 rows x 20");
                RIN_CHECK_EQ(viewer::paramGridRows(*arrayDesc, pd, {}), std::size_t{3});
                std::vector<rin::ParamAssignment> wide;
                wide.push_back({"p", std::vector<double>(25, 0.0)});
                RIN_CHECK_EQ(viewer::paramGridRows(*arrayDesc, pd, wide),
                             std::size_t{5});
            }
        }
    }

    // nodeHeight(viewer) = 标题 26 + 预览 140 + 间距 6 + max(1,1)*20 + 页脚 8。
    RIN_CHECK_MSG(nearF(viewer::nodeHeight(*viewerD, {}),
                        viewer::kNodeHeaderHeight + viewer::kMonitorPreviewHeight +
                            viewer::kParamSectionGap + 1.0f * viewer::kNodePortRowHeight +
                            viewer::kNodeFooterHeight),
                  "viewer node height = header + preview + gap + one port row + footer");
    RIN_CHECK_MSG(nearF(viewer::nodeHeight(*viewerD, {}), 200.0f),
                  "viewer node height frozen value 200");
    const viewer::CanvasRect viewerBounds = viewer::nodeBounds({0.0f, 0.0f}, *viewerD, {});
    RIN_CHECK_MSG(nearF(viewerBounds.width, 200.0f) && nearF(viewerBounds.height, 200.0f),
                  "viewer node bounds 200x200");

    // M11 验收追加（2026-10-07）：监看器视窗大小可调——节点级预览高度变体
    // （几何族尾参显式传递；范围常量冻结 [96,640]；默认参数路径等价）。
    RIN_CHECK_MSG(viewer::kMonitorPreviewMinHeight == 96.0f &&
                      viewer::kMonitorPreviewMaxHeight == 640.0f,
                  "preview height range constants frozen at [96,640]");
    RIN_CHECK_MSG(nearF(viewer::nodeHeight(*viewerD, {}, viewer::kMonitorPreviewMinHeight),
                        156.0f),
                  "viewer height at min preview = 156");
    RIN_CHECK_MSG(nearF(viewer::nodeHeight(*viewerD, {}, viewer::kMonitorPreviewMaxHeight),
                        700.0f),
                  "viewer height at max preview = 700");
    RIN_CHECK_MSG(viewer::nodeHeight(*viewerD) == viewer::nodeHeight(*viewerD, {}) &&
                      viewer::nodeHeight(*viewerD, {}) ==
                          viewer::nodeHeight(*viewerD, {}, viewer::kMonitorPreviewHeight),
                  "default-argument geometry path equals the explicit default");

    // portPosition：监看器输入锚点纵向偏移含预览窗（y = 26+140+6 + 10）。
    {
        const viewer::CanvasPoint in0 = viewer::portPosition(
            {0.0f, 0.0f}, *viewerD, {}, {7, rin::PortDirection::Input, 0});
        RIN_CHECK_MSG(nearF(in0.x, 0.0f) &&
                          nearF(in0.y, viewer::kNodeHeaderHeight +
                                           viewer::kMonitorPreviewHeight +
                                           viewer::kParamSectionGap +
                                           viewer::kNodePortRowHeight * 0.5f),
                      "viewer input anchor sits below the preview window");
        // 节点级高度变体：锚点 y = 26 + h + 6 + 10（min 96 → 138、max 640 → 682）。
        const viewer::CanvasPoint inMin = viewer::portPosition(
            {0.0f, 0.0f}, *viewerD, {}, {7, rin::PortDirection::Input, 0},
            viewer::kMonitorPreviewMinHeight);
        RIN_CHECK_MSG(nearF(inMin.x, 0.0f) && nearF(inMin.y, 138.0f),
                      "viewer input anchor at min preview height");
        const viewer::CanvasPoint inMax = viewer::portPosition(
            {0.0f, 0.0f}, *viewerD, {}, {7, rin::PortDirection::Input, 0},
            viewer::kMonitorPreviewMaxHeight);
        RIN_CHECK_MSG(nearF(inMax.x, 0.0f) && nearF(inMax.y, 682.0f),
                      "viewer input anchor at max preview height");
        // 带参节点（downscale）：输入锚点纵向偏移含参数区（y = 26+58+10）。
        const viewer::CanvasPoint dIn0 = viewer::portPosition(
            {0.0f, 0.0f}, *downscale, {}, {7, rin::PortDirection::Input, 0});
        RIN_CHECK_MSG(nearF(dIn0.y, 26.0f + 58.0f + 10.0f),
                      "param section shifts port anchors down");
    }

    // 画布图模型：connect Any 三型接受 / Any 输出拒绝 / viewer→viewer。
    {
        viewer::CanvasGraphModel model;
        model.catalog = &catalog;
        RIN_CHECK(model.createNode("source", {0.0f, 0.0f}).ok);              // id 1
        RIN_CHECK(model.createNode("source_depth_gray", {0.0f, 0.0f}).ok);  // id 2
        RIN_CHECK(model.createNode("source_depth_metric", {0.0f, 0.0f}).ok);// id 3
        RIN_CHECK(model.createNode("viewer", {300.0f, 0.0f}).ok);           // id 4
        RIN_CHECK(model.createNode("gaussian_blur", {600.0f, 0.0f}).ok);    // id 5
        RIN_CHECK(model.createNode("viewer", {900.0f, 0.0f}).ok);           // id 6
        // 三型输出 → viewer.in0（替换边语义逐一替换）。
        RIN_CHECK_MSG(model.connect({1, rin::PortDirection::Output, 0},
                                    {4, rin::PortDirection::Input, 0})
                          .ok,
                      "canvas connect: Rgba8 -> Any accepted");
        RIN_CHECK_MSG(model.connect({2, rin::PortDirection::Output, 0},
                                    {4, rin::PortDirection::Input, 0})
                          .ok,
                      "canvas connect: Gray8 -> Any accepted");
        RIN_CHECK_MSG(model.connect({3, rin::PortDirection::Output, 0},
                                    {4, rin::PortDirection::Input, 0})
                          .ok,
                      "canvas connect: Depth32F -> Any accepted");
        RIN_CHECK_MSG(model.connections.size() == 1,
                      "replacement keeps one edge on the Any input");
        // Any 输出 → 具体型输入：拒绝且消息含两侧类型名。
        const viewer::CanvasOpResult rejected = model.connect(
            {4, rin::PortDirection::Output, 0}, {5, rin::PortDirection::Input, 0});
        RIN_CHECK_MSG(!rejected.ok, "canvas connect: Any output to Gray8 rejected");
        RIN_CHECK_MSG(rejected.error.find("Any") != std::string::npos &&
                          rejected.error.find("Gray8") != std::string::npos,
                      "rejection message names Any and Gray8");
        // viewer → viewer（Any→Any）合法：v1.in0 需被驱动。
        RIN_CHECK_MSG(model.connect({4, rin::PortDirection::Output, 0},
                                    {6, rin::PortDirection::Input, 0})
                          .ok,
                      "canvas connect: Any -> Any accepted");

        // portAt：监看器输出端口锚点不参与命中（隐藏端口不可拖线）；
        // 输入端口正常命中。
        const rin::NodeDescriptor* viewerDesc =
            rin::findNodeDescriptor(catalog, "viewer");
        RIN_CHECK(viewerDesc != nullptr);
        if (viewerDesc != nullptr) {
            const viewer::CanvasNode* monitor = model.findNode(4);
            RIN_CHECK(monitor != nullptr);
            if (monitor != nullptr) {
                const viewer::CanvasPoint outAnchor = viewer::portPosition(
                    monitor->position, *viewerDesc, monitor->params,
                    {4, rin::PortDirection::Output, 0});
                RIN_CHECK_MSG(!model.portAt(outAnchor).has_value(),
                              "monitor output anchor is not hit-testable");
                RIN_CHECK_MSG(
                    !model.portAt({outAnchor.x + 5.0f, outAnchor.y + 5.0f}).has_value(),
                    "monitor output neighborhood is not hit-testable either");
                const viewer::CanvasPoint inAnchor = viewer::portPosition(
                    monitor->position, *viewerDesc, monitor->params,
                    {4, rin::PortDirection::Input, 0});
                const std::optional<rin::PortRef> hit = model.portAt(inAnchor);
                RIN_CHECK_MSG(hit.has_value() && hit->node == 4 &&
                                  hit->direction == rin::PortDirection::Input &&
                                  hit->index == 0,
                              "monitor input anchor is hit-testable");
            }
        }
        // 非监看器节点的输出端口仍可命中（回归）。
        {
            const rin::NodeDescriptor* blurDesc =
                rin::findNodeDescriptor(catalog, "gaussian_blur");
            RIN_CHECK(blurDesc != nullptr);
            const viewer::CanvasNode* blurNode = model.findNode(5);
            RIN_CHECK(blurNode != nullptr);
            if (blurDesc != nullptr && blurNode != nullptr) {
                const viewer::CanvasPoint outAnchor = viewer::portPosition(
                    blurNode->position, *blurDesc, blurNode->params,
                    {5, rin::PortDirection::Output, 0});
                const std::optional<rin::PortRef> hit = model.portAt(outAnchor);
                RIN_CHECK_MSG(hit.has_value() && hit->node == 5 &&
                                  hit->direction == rin::PortDirection::Output,
                              "non-monitor output anchors remain hit-testable");
            }
        }
    }

    // paletteGroupFor：viewer 归 View；M6 冻结的既有分组不变；未知归 Other。
    // （M10 深度域 typeId 未入分组表、落 Other 是 HEAD 既有行为——"目录扩展时
    // 调色板不丢项"的文档化回退，非 M11 回归，此处按现状见证。）
    RIN_CHECK_MSG(std::string(viewer::paletteGroupFor("viewer")) == "View",
                  "paletteGroupFor(viewer) == View");
    RIN_CHECK_MSG(std::string(viewer::paletteGroupFor("source")) == "Source" &&
                      std::string(viewer::paletteGroupFor("source_depth_jet")) ==
                          "Source" &&
                      std::string(viewer::paletteGroupFor("source_depth_gray")) ==
                          "Source" &&
                      std::string(viewer::paletteGroupFor("source_depth_adaptive")) ==
                          "Source" &&
                      std::string(viewer::paletteGroupFor("crop")) == "Geometry" &&
                      std::string(viewer::paletteGroupFor("grayify")) == "Filter" &&
                      std::string(viewer::paletteGroupFor("fft_lowpass")) ==
                          "Frequency" &&
                      std::string(viewer::paletteGroupFor("hist_eq")) == "Histogram",
                  "existing palette groups unchanged");
    RIN_CHECK_MSG(std::string(viewer::paletteGroupFor("source_depth_metric")) ==
                          "Other" &&
                      std::string(viewer::paletteGroupFor("depth_normalize")) ==
                          "Other" &&
                      std::string(viewer::paletteGroupFor("custom")) == "Other",
                  "unlisted depth types and unknown types fall into Other (HEAD behavior)");
    // 默认目录过滤 "viewer"：恰好 View 一组、含监看器条目。
    {
        const std::vector<viewer::PaletteGroup> groups =
            viewer::paletteGroups(catalog, "viewer");
        RIN_CHECK_MSG(groups.size() == 1 && groups[0].title == "View" &&
                          groups[0].items.size() == 1 &&
                          groups[0].items[0]->typeId == "viewer",
                      "palette filter 'viewer' yields the View group with the monitor");
    }
}

}  // namespace

int main() {
    runSection("any_port_type_basics", testAnyPortTypeBasics);
    runSection("any_validation_matrix", testAnyValidationMatrix);
    runSection("catalog_viewer_entry", testCatalogViewerEntry);
    runSection("viewer_identity_node", testViewerIdentityNode);
    runSection("run_node_graph_viewer_passthrough", testRunNodeGraphViewerPassthrough);
    runSection("canvas_monitor_model", testCanvasMonitorModel);
    return rin_test::exitStatus();
}
