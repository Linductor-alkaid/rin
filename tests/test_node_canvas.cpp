// M5-03 节点编辑器画布纯逻辑测试 —— 独立验证（Independent Verification Agent）。
//
// 本文件由 Independent Verification Agent 全权重写（2026-09-28，分支
// feat/m5-03-node-canvas），按被测契约独立设计与执行，不依赖开发者自述。
//
// 被测面：apps/viewer/canvas_model.hpp（M5-03，DEC-015 交互几何下沉）：
//   1. 视图变换 CanvasView（screen↔canvas 往返、锚点缩放与上下限夹取、
//      帧全图 fit、centerViewOn）；
//   2. 几何基元（nodeHeight/nodeBounds/portPosition/canvasRectBetween/
//      pointSegmentDistance/sampleWire/bezierPoint）；
//   3. 画布图模型 CanvasGraphModel（创建/删除连带边/移动/连线替换与成环
//      拒绝——validateWorkflowGraph 唯一判据、悬空输入非阻塞、选中集与
//      selectedConnection 同步、命中检测 wireAt/portAt/nodeAt/nodesInRect）；
//   4. 交互状态机 CanvasInteraction（框选与 Shift 加选、节点拖动、连线拖拽
//      与取消、平移、右键删除/菜单、Alt 删线、deleteSelection）；
//   5. 调色板模型 paletteGroups（分组归类、即输即筛、未知类型归 Other）；
//   6. 连线带状轮廓 wireRibbon（M5-04 连线渲染修复：粗细一致曲线的 ±width/2
//      带状封闭轮廓，EUI polygon 填充语义下开放点集误渲染为"弦-曲线封闭
//      区域"的真机缺陷回归守卫）；
//   7. 监看器预览窗节点级高度与宽度（M11 验收追加 2026-10-07，独立验证）：
//      几何族尾参 monitorPreviewHeight/monitorWidth 的默认/最小/最大/越界
//      冻结值与线性关系、宽高双向独立、非 viewer 节点同值异型不受影响、
//      nodeAt/nodesInRect/portAt/wireAt/graphBounds/交互在预览增高/加宽下的
//      命中语义、两 viewer 独立尺寸，以及画/命中锚点一致性回归守卫
//      （cb8bd4c 修复与 28685c3 宽度扩展的同源性守卫）。
//
// 目录构造为本文件私有的小型 rin::NodeCatalog（只链 rin::core）。测试壳为 tests/test_util.hpp 的 RIN_CHECK*（无第三方框架），
// main 返回 rin_test::exitStatus()。单线程纯逻辑，无 sleep。
//
// 末节 "iva_defect_probes" 原为针对实现疑点的缺陷探针：2026-09-28 主循环
// 修复三个已确认缺陷（onPress use-after-rotate 拖错节点、按下优先级连线
// 先于节点、组拖动缺失）后，该区已按修复后语义改写为回归守卫——任何一条
// 失败即对应缺陷回归。首轮发现与本轮复验证据见两轮 IVA 验证报告。

#include "canvas_model.hpp"

#include "test_util.hpp"

#include <rin/workflow_types.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr float kTol = 1e-3f;
constexpr float kFitTol = 0.05f;

[[nodiscard]] bool nearF(float a, float b, float tol = kTol) {
    return std::fabs(a - b) <= tol;
}

[[nodiscard]] bool nearPoint(const viewer::CanvasPoint& a, const viewer::CanvasPoint& b,
                             float tol = kTol) {
    return nearF(a.x, b.x, tol) && nearF(a.y, b.y, tol);
}

// --- 本地小型目录（满足 NodeDescriptor::valid()/NodeCatalog::valid() 约束） ---

// a：Gray8 单输入单输出；b：Gray8 双输入单输出；c：Rgba8 单输入单输出 +
// 一个带范围声明的 Real 参数；monitor：零输入单输出（注意：typeId 非真实监看器
// "viewer"，isMonitorNode 判定为 false——历史夹具，覆盖 nodeHeight 的
// max(inputs, outputs, 1) 下限分支）；viewer：真实监看器（Any→Any 单输入单输出，
// M11/DEC-021 签名，预览窗高度节点级可调的几何被测对象）；blank：零输入零输出。
[[nodiscard]] rin::NodeCatalog makeCanvasCatalog() {
    rin::NodeCatalog catalog;
    catalog.nodes.push_back({"a", "Alpha", {rin::PortType::Gray8}, {rin::PortType::Gray8}, {}});
    catalog.nodes.push_back(
        {"b", "Bravo", {rin::PortType::Gray8, rin::PortType::Gray8}, {rin::PortType::Gray8}, {}});
    rin::ParamDescriptor sigma;
    sigma.id = "sigma";
    sigma.label = "Sigma";
    sigma.kind = rin::ParamKind::Real;
    sigma.defaultValue = 0.5;
    sigma.hasRange = true;
    sigma.minValue = 0.0;
    sigma.maxValue = 10.0;
    catalog.nodes.push_back(
        {"c", "Charlie", {rin::PortType::Rgba8}, {rin::PortType::Rgba8}, {sigma}});
    catalog.nodes.push_back({"monitor", "Monitor", {}, {rin::PortType::Gray8}, {}});
    catalog.nodes.push_back({"viewer", "Viewer", {rin::PortType::Any}, {rin::PortType::Any}, {}});
    catalog.nodes.push_back({"blank", "Blank", {}, {}, {}});
    return catalog;
}

// 调色板分组用目录：M4/M5 真实 typeId 集 + 一个未知类型（Other 不丢项）。
[[nodiscard]] rin::NodeCatalog makePaletteCatalog() {
    rin::NodeCatalog catalog;
    catalog.nodes.push_back({"source", "Source", {}, {rin::PortType::Gray8}, {}});
    catalog.nodes.push_back({"crop", "Crop", {rin::PortType::Gray8}, {rin::PortType::Gray8}, {}});
    catalog.nodes.push_back(
        {"downscale", "Downscale", {rin::PortType::Gray8}, {rin::PortType::Gray8}, {}});
    catalog.nodes.push_back(
        {"grayify", "Grayify", {rin::PortType::Gray8}, {rin::PortType::Gray8}, {}});
    catalog.nodes.push_back(
        {"gaussian_blur", "Gaussian Blur", {rin::PortType::Gray8}, {rin::PortType::Gray8}, {}});
    catalog.nodes.push_back(
        {"conv_kernel", "Convolution Kernel", {rin::PortType::Gray8}, {rin::PortType::Gray8}, {}});
    catalog.nodes.push_back(
        {"fft_lowpass", "FFT Low-pass", {rin::PortType::Gray8}, {rin::PortType::Gray8}, {}});
    catalog.nodes.push_back(
        {"fft_highpass", "FFT High-pass", {rin::PortType::Gray8}, {rin::PortType::Gray8}, {}});
    catalog.nodes.push_back(
        {"fft_bandpass", "FFT Band-pass", {rin::PortType::Gray8}, {rin::PortType::Gray8}, {}});
    catalog.nodes.push_back(
        {"hist_eq", "Histogram Equalize", {rin::PortType::Gray8}, {rin::PortType::Gray8}, {}});
    catalog.nodes.push_back(
        {"custom_tool", "Custom Tool", {rin::PortType::Gray8}, {rin::PortType::Gray8}, {}});
    return catalog;
}

// 图模型夹具：目录成员先行构造，model 持有其指针。
struct GraphFixture {
    rin::NodeCatalog catalog;
    viewer::CanvasGraphModel model;

    GraphFixture() : catalog{makeCanvasCatalog()}, model{&catalog} {}
};

// 断言成功的便捷创建；失败时返回 kInvalidNode（后续检查会失败但不崩溃）。
[[nodiscard]] rin::NodeId make(viewer::CanvasGraphModel& model, const char* typeId, float x,
                               float y) {
    const viewer::CanvasOpResult result = model.createNode(typeId, {x, y});
    RIN_CHECK_MSG(result.ok,
                  std::string("createNode(") + typeId + ") expected ok: " + result.error);
    return model.nodes.empty() ? rin::kInvalidNode : model.nodes.back().id;
}

[[nodiscard]] bool selectionEquals(const viewer::CanvasGraphModel& model,
                                   std::vector<rin::NodeId> expected) {
    std::vector<rin::NodeId> actual = model.selection;
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    return actual == expected;
}

[[nodiscard]] bool hasKind(const rin::WorkflowValidation& validation,
                           rin::ValidationIssueKind kind) {
    for (const rin::ValidationIssue& issue : validation.issues) {
        if (issue.kind == kind) {
            return true;
        }
    }
    return false;
}

// CanvasNode 无 operator==：逐字段比较。
[[nodiscard]] bool nodesEqual(const std::vector<viewer::CanvasNode>& a,
                              const std::vector<viewer::CanvasNode>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].typeId != b[i].typeId ||
            !(a[i].position == b[i].position)) {
            return false;
        }
    }
    return true;
}

// 消费被丢弃的 [[nodiscard]] 返回值（该步结果由后续状态断言覆盖）。
void pressIgnore(viewer::CanvasInteraction& it, viewer::CanvasGraphModel& m,
                 const viewer::CanvasPoint& p, const int button = 0, const bool shift = false,
                 const bool alt = false) {
    const viewer::PressResult r = it.onPress(m, p, button, shift, alt);
    static_cast<void>(r);
}

void releaseIgnore(viewer::CanvasInteraction& it, viewer::CanvasGraphModel& m,
                   const viewer::CanvasPoint& p, const bool shift = false) {
    const viewer::ReleaseResult r = it.onRelease(m, p, shift);
    static_cast<void>(r);
}

void runSection(const char* name, void (*fn)()) {
    std::printf("== %s\n", name);
    fn();
}

// --- 1. 视图变换 ---

void testViewTransform() {
    // toScreen/toCanvas 互为逆（screen = canvas*scale + pan）。
    {
        viewer::CanvasView view;
        view.scale = 1.7f;
        view.pan = {-33.5f, 210.25f};
        const viewer::CanvasPoint points[] = {{0.0f, 0.0f},
                                              {13.0f, -7.0f},
                                              {-45.5f, 88.0f},
                                              {320.0f, 240.0f},
                                              {0.125f, -0.125f}};
        for (const viewer::CanvasPoint& p : points) {
            const viewer::CanvasPoint s = view.toScreen(p);
            RIN_CHECK_MSG((nearF(s.x, p.x * view.scale + view.pan.x) &&
                           nearF(s.y, p.y * view.scale + view.pan.y)),
                          "toScreen matches canvas*scale+pan");
            const viewer::CanvasPoint back = view.toCanvas(s);
            RIN_CHECK_MSG(nearPoint(back, p), "toCanvas(toScreen(p)) == p");
            const viewer::CanvasPoint forward = view.toScreen(view.toCanvas(p));
            RIN_CHECK_MSG(nearPoint(forward, p), "toScreen(toCanvas(p)) == p");
        }
    }

    // zoomAt：factor<=0 或非有限 → no-op（逐字段不变）。
    {
        viewer::CanvasView view;
        view.scale = 1.3f;
        view.pan = {5.0f, -7.0f};
        const float invalid[] = {0.0f, -2.0f, std::nanf(""),
                                 std::numeric_limits<float>::infinity()};
        for (const float factor : invalid) {
            const float scaleBefore = view.scale;
            const viewer::CanvasPoint panBefore = view.pan;
            view.zoomAt({100.0f, 100.0f}, factor);
            RIN_CHECK_MSG((view.scale == scaleBefore && view.pan == panBefore),
                          "invalid zoom factor leaves view untouched");
        }
    }

    // zoomAt：锚点画布坐标在缩放前后不变；越界因子夹取到上限。
    {
        viewer::CanvasView view;
        view.scale = 1.5f;
        view.pan = {10.0f, 20.0f};
        const viewer::CanvasPoint anchor{100.0f, 100.0f};
        const viewer::CanvasPoint before = view.toCanvas(anchor);
        view.zoomAt(anchor, 2.0f);
        RIN_CHECK_MSG(view.scale == viewer::kZoomMax, "1.5*2 clamped to kZoomMax");
        RIN_CHECK_MSG(nearPoint(view.toCanvas(anchor), before), "anchor canvas point invariant");
    }

    // 连续放大到边界不再越界（锚点仍不动）。
    {
        viewer::CanvasView view;
        const viewer::CanvasPoint anchor{37.0f, -11.5f};
        const viewer::CanvasPoint before = view.toCanvas(anchor);
        for (int i = 0; i < 5; ++i) {
            view.zoomAt(anchor, 10.0f);
        }
        RIN_CHECK_MSG(view.scale == viewer::kZoomMax, "repeated zoom-in pinned at kZoomMax");
        RIN_CHECK_MSG(nearPoint(view.toCanvas(anchor), before),
                      "anchor invariant across repeated zoom-in");
    }

    // 连续缩小到边界不再越界。
    {
        viewer::CanvasView view;
        view.scale = 1.0f;
        view.pan = {40.0f, -12.0f};
        const viewer::CanvasPoint anchor{-5.0f, 64.0f};
        const viewer::CanvasPoint before = view.toCanvas(anchor);
        for (int i = 0; i < 5; ++i) {
            view.zoomAt(anchor, 0.01f);
        }
        RIN_CHECK_MSG(view.scale == viewer::kZoomMin, "repeated zoom-out pinned at kZoomMin");
        RIN_CHECK_MSG(nearPoint(view.toCanvas(anchor), before),
                      "anchor invariant across repeated zoom-out");
    }

    // 恰好落在边界内的缩放保持合法。
    {
        viewer::CanvasView view;
        view.zoomAt({50.0f, 50.0f}, 2.5f);
        RIN_CHECK_MSG(view.scale == 2.5f, "factor landing exactly on kZoomMax kept");
        RIN_CHECK_MSG((nearPoint(view.toCanvas({50.0f, 50.0f}), {50.0f, 50.0f})),
                      "anchor still invariant at boundary");
    }

    // fit：viewport<=0 时 no-op。
    {
        viewer::CanvasView view;
        view.scale = 1.7f;
        view.pan = {3.0f, 4.0f};
        const viewer::CanvasRect bounds{0.0f, 0.0f, 100.0f, 100.0f};
        view.fit(bounds, 0.0f, 600.0f);
        RIN_CHECK_MSG((view.scale == 1.7f && view.pan.x == 3.0f && view.pan.y == 4.0f),
                      "fit with viewportWidth<=0 is a no-op");
        view.fit(bounds, 800.0f, -1.0f);
        RIN_CHECK_MSG((view.scale == 1.7f && view.pan.x == 3.0f && view.pan.y == 4.0f),
                      "fit with viewportHeight<=0 is a no-op");
    }

    // fit：空 bounds（宽或高 <=0）复位 scale=1 / pan=0。
    {
        viewer::CanvasView view;
        view.zoomAt({400.0f, 300.0f}, 2.0f);
        RIN_CHECK(view.scale == 2.0f);
        view.fit({50.0f, 60.0f, 0.0f, 0.0f}, 800.0f, 600.0f);
        RIN_CHECK_MSG((view.scale == 1.0f && view.pan.x == 0.0f && view.pan.y == 0.0f),
                      "empty bounds reset view");
        view.zoomAt({400.0f, 300.0f}, 2.0f);
        view.fit({50.0f, 60.0f, 120.0f, 0.0f}, 800.0f, 600.0f);
        RIN_CHECK_MSG((view.scale == 1.0f && view.pan.x == 0.0f && view.pan.y == 0.0f),
                      "zero-height bounds reset view");
    }

    // fit：远原点 bounds 全部落入视口，含 32px 边距（此例 scale 触上限夹取）。
    {
        viewer::CanvasView view;
        view.fit({1000.0f, 2000.0f, 200.0f, 100.0f}, 800.0f, 600.0f);
        RIN_CHECK_MSG(view.scale == viewer::kZoomMax, "fit scale clamped to kZoomMax");
        RIN_CHECK_MSG((view.pan.x == -2350.0f && view.pan.y == -4825.0f), "fit pan exact");
        const viewer::CanvasPoint topLeft = view.toScreen({1000.0f, 2000.0f});
        const viewer::CanvasPoint bottomRight = view.toScreen({1200.0f, 2100.0f});
        RIN_CHECK_MSG((topLeft.x >= 32.0f - kFitTol && topLeft.y >= 32.0f - kFitTol),
                      "fit keeps top-left inside 32px margin");
        RIN_CHECK_MSG((bottomRight.x <= 768.0f + kFitTol && bottomRight.y <= 568.0f + kFitTol),
                      "fit keeps bottom-right inside viewport minus margin");
    }

    // fit：非夹取场景精确贴合（高度受限，上下边距恰为 32px）。
    {
        viewer::CanvasView view;
        view.fit({0.0f, 0.0f, 400.0f, 300.0f}, 800.0f, 600.0f);
        RIN_CHECK_MSG(nearF(view.scale, 536.0f / 300.0f, 1e-5f), "fit scale = availH/boundsH");
        const viewer::CanvasPoint topLeft = view.toScreen({0.0f, 0.0f});
        const viewer::CanvasPoint bottomRight = view.toScreen({400.0f, 300.0f});
        RIN_CHECK_MSG((nearF(topLeft.x, (800.0f - 400.0f * view.scale) * 0.5f, kFitTol) &&
                       nearF(topLeft.y, 32.0f, kFitTol)),
                      "fit top-left at margin");
        // 宽度非受限轴两侧留白对称：右缘 = 视口 - 左缘留白。
        RIN_CHECK_MSG((nearF(bottomRight.x, (800.0f + 400.0f * view.scale) * 0.5f, kFitTol) &&
                       nearF(bottomRight.y, 600.0f - 32.0f, kFitTol)),
                      "fit bottom-right at margin");
    }

    // centerViewOn：画布点移到视口中心，scale 不变。
    {
        viewer::CanvasView view;
        view.scale = 2.0f;
        view.pan = {100.0f, 100.0f};
        viewer::centerViewOn(view, {10.0f, 20.0f}, 800.0f, 600.0f);
        RIN_CHECK_MSG(view.scale == 2.0f, "centerViewOn keeps scale");
        RIN_CHECK_MSG((view.pan.x == 380.0f && view.pan.y == 260.0f), "centerViewOn pan exact");
        const viewer::CanvasPoint screen = view.toScreen({10.0f, 20.0f});
        RIN_CHECK_MSG(nearPoint(screen, {400.0f, 300.0f}), "target point at viewport center");
    }

    // 集成：graphBounds → fit 后全部节点框落入视口（含 32px 边距）。
    {
        GraphFixture fx;
        const rin::NodeId ia = make(fx.model, "a", 0.0f, 0.0f);
        const rin::NodeId ib = make(fx.model, "a", 500.0f, 200.0f);
        RIN_CHECK((ia != rin::kInvalidNode && ib != rin::kInvalidNode));
        const viewer::CanvasRect bounds = fx.model.graphBounds();
        RIN_CHECK_MSG((bounds.x == 0.0f && bounds.y == 0.0f && nearF(bounds.width, 700.0f) &&
                       nearF(bounds.height, 254.0f)),
                      "graphBounds is the union of node bounds");
        viewer::CanvasView view;
        view.fit(bounds, 1000.0f, 800.0f);
        const viewer::CanvasRect b2 = fx.model.graphBounds();
        const viewer::CanvasPoint topLeft = view.toScreen({b2.x, b2.y});
        const viewer::CanvasPoint bottomRight =
            view.toScreen({b2.x + b2.width, b2.y + b2.height});
        RIN_CHECK_MSG((topLeft.x >= 32.0f - kFitTol && topLeft.y >= 32.0f - kFitTol &&
                       bottomRight.x <= 968.0f + kFitTol && bottomRight.y <= 768.0f + kFitTol),
                      "fit(graphBounds) places all nodes inside viewport margin");
    }
}

// --- 2. 几何基元 ---

void testGeometry() {
    GraphFixture fx;
    const rin::NodeDescriptor* a = rin::findNodeDescriptor(fx.catalog, "a");
    const rin::NodeDescriptor* b = rin::findNodeDescriptor(fx.catalog, "b");
    const rin::NodeDescriptor* c = rin::findNodeDescriptor(fx.catalog, "c");
    const rin::NodeDescriptor* monitor = rin::findNodeDescriptor(fx.catalog, "monitor");
    const rin::NodeDescriptor* blank = rin::findNodeDescriptor(fx.catalog, "blank");
    RIN_CHECK((a != nullptr && b != nullptr && c != nullptr && monitor != nullptr &&
               blank != nullptr));
    RIN_CHECK(fx.catalog.valid());
    if (a == nullptr || b == nullptr || c == nullptr || monitor == nullptr ||
        blank == nullptr) {
        return;
    }

    // nodeHeight = 26 + 参数区 + max(inputs, outputs, 1)*20 + 8（M11/DEC-021 起
    // 参数区参与几何：无参数且非相机源为 0，标量参数行 14+12 + 行间距 4 + 首距 6）。
    RIN_CHECK_MSG(nearF(viewer::nodeHeight(*a), 54.0f), "a: 26+0+1*20+8 (no params)");
    RIN_CHECK_MSG(nearF(viewer::nodeHeight(*b), 74.0f), "b: 26+0+2*20+8");
    RIN_CHECK_MSG(nearF(viewer::nodeHeight(*c), 90.0f),
                  "c: 26+(6+26+4)+1*20+8 (one ranged Real param row)");
    RIN_CHECK_MSG(nearF(viewer::nodeHeight(*monitor), 54.0f),
                  "monitor: 0 inputs falls back to 1 row");
    RIN_CHECK_MSG(nearF(viewer::nodeHeight(*blank), 54.0f), "blank: no ports still one row");
    // 带 RealArray 参数（默认 9 值 → 3x3）时高度随实例赋值形状联动（M11）。
    {
        rin::NodeCatalog arrayCatalog;
        rin::ParamDescriptor kernel;
        kernel.id = "kernel";
        kernel.label = "Kernel";
        kernel.kind = rin::ParamKind::RealArray;
        kernel.defaultValue = std::vector<double>{0, 0, 0, 0, 1, 0, 0, 0, 0};
        arrayCatalog.nodes.push_back(
            {"arr", "Array", {rin::PortType::Gray8}, {rin::PortType::Gray8}, {kernel}});
        const rin::NodeDescriptor* arr = rin::findNodeDescriptor(arrayCatalog, "arr");
        RIN_CHECK(arr != nullptr);
        if (arr != nullptr) {
            // 默认 9 值 → fromFlat 3x3 → 形状行 14 + 3*20 网格 + 行间距 4 + 首距 6。
            RIN_CHECK_MSG(nearF(viewer::nodeHeight(*arr), 26.0f + 6.0f + 14.0f + 60.0f + 4.0f +
                                              20.0f + 8.0f),
                          "arr default: 3x3 grid rows");
            // 实例赋值 25 值 → 5x5 网格（行数随实例形状联动）。
            std::vector<rin::ParamAssignment> wide;
            wide.push_back({"kernel", std::vector<double>(25, 1.0)});
            RIN_CHECK_MSG(nearF(viewer::nodeHeight(*arr, wide),
                                26.0f + 6.0f + 14.0f + 100.0f + 4.0f + 20.0f + 8.0f),
                          "arr 25-value assignment: 5x5 grid rows");
            // 实例赋值 9 值但非平方（10 值 → 1x10）：行数坍缩为 1。
            std::vector<rin::ParamAssignment> flat;
            flat.push_back({"kernel", std::vector<double>(10, 1.0)});
            RIN_CHECK_MSG(nearF(viewer::nodeHeight(*arr, flat),
                                26.0f + 6.0f + 14.0f + 20.0f + 4.0f + 20.0f + 8.0f),
                          "arr 10-value assignment: 1x10 single grid row");
        }
    }

    // nodeBounds。
    const viewer::CanvasRect boundsA = viewer::nodeBounds({200.0f, 100.0f}, *a);
    RIN_CHECK_MSG((boundsA.x == 200.0f && boundsA.y == 100.0f && boundsA.width == 200.0f &&
                   boundsA.height == 54.0f),
                  "nodeBounds = position + kNodeWidth x nodeHeight");

    // portPosition（M11 四参签名）：输入靠左缘、输出靠右缘，y 按 index*20 + 行中点。
    {
        const viewer::CanvasPoint in0 = viewer::portPosition({200.0f, 100.0f}, *a, {},
                                                             {99, rin::PortDirection::Input, 0});
        RIN_CHECK_MSG(nearPoint(in0, {200.0f, 136.0f}), "input port at left edge, row midpoint");
        const viewer::CanvasPoint out0 =
            viewer::portPosition({200.0f, 100.0f}, *a, {}, {99, rin::PortDirection::Output, 0});
        RIN_CHECK_MSG(nearPoint(out0, {400.0f, 136.0f}), "output port at right edge");
        const viewer::CanvasPoint in1 =
            viewer::portPosition({400.0f, 100.0f}, *b, {}, {99, rin::PortDirection::Input, 1});
        RIN_CHECK_MSG(nearPoint(in1, {400.0f, 156.0f}), "second input one row below");
        // 端口纵向偏移含参数区（M11）：c 的输入 0 比无参节点低一个参数区。
        const viewer::CanvasPoint cIn0 =
            viewer::portPosition({0.0f, 0.0f}, *c, {}, {99, rin::PortDirection::Input, 0});
        RIN_CHECK_MSG(nearPoint(cIn0, {0.0f, 26.0f + 36.0f + 10.0f}),
                      "param section shifts the port row down");
        // params 实参参与：RealArray 赋值改变形状时端口同步下移（几何与实例联动）。
        rin::NodeCatalog arrayCatalog;
        rin::ParamDescriptor kernel;
        kernel.id = "kernel";
        kernel.label = "Kernel";
        kernel.kind = rin::ParamKind::RealArray;
        kernel.defaultValue = std::vector<double>{0, 0, 0, 0, 1, 0, 0, 0, 0};
        arrayCatalog.nodes.push_back(
            {"arr", "Array", {rin::PortType::Gray8}, {rin::PortType::Gray8}, {kernel}});
        if (const rin::NodeDescriptor* arr = rin::findNodeDescriptor(arrayCatalog, "arr")) {
            std::vector<rin::ParamAssignment> wide;
            wide.push_back({"kernel", std::vector<double>(25, 1.0)});
            const viewer::CanvasPoint wideIn0 =
                viewer::portPosition({0.0f, 0.0f}, *arr, wide, {99, rin::PortDirection::Input, 0});
            const float expectedY = 26.0f + 6.0f + 14.0f + 100.0f + 4.0f + 10.0f;
            RIN_CHECK_MSG(nearPoint(wideIn0, {0.0f, expectedY}),
                          "RealArray instance shape moves the port anchor");
        }
    }

    // canvasRectBetween：负向拖拽归一。
    {
        const viewer::CanvasRect r = viewer::canvasRectBetween({100.0f, 100.0f}, {40.0f, 30.0f});
        RIN_CHECK_MSG((r.x == 40.0f && r.y == 30.0f && r.width == 60.0f && r.height == 70.0f),
                      "negative drag normalized");
        const viewer::CanvasRect zero = viewer::canvasRectBetween({5.0f, 6.0f}, {5.0f, 6.0f});
        RIN_CHECK_MSG((zero.width == 0.0f && zero.height == 0.0f), "degenerate rect");
    }

    // pointSegmentDistance：端点/中点/超范围 clamp/退化线段。
    {
        const viewer::CanvasPoint a0{0.0f, 0.0f};
        const viewer::CanvasPoint b0{10.0f, 0.0f};
        RIN_CHECK_MSG(nearF(viewer::pointSegmentDistance({0.0f, 0.0f}, a0, b0), 0.0f),
                      "distance at segment start is 0");
        RIN_CHECK_MSG(nearF(viewer::pointSegmentDistance({10.0f, 0.0f}, a0, b0), 0.0f),
                      "distance at segment end is 0");
        RIN_CHECK_MSG(nearF(viewer::pointSegmentDistance({5.0f, 3.0f}, a0, b0), 3.0f),
                      "midpoint perpendicular distance");
        RIN_CHECK_MSG(nearF(viewer::pointSegmentDistance({5.0f, -3.0f}, a0, b0), 3.0f),
                      "perpendicular distance is symmetric");
        RIN_CHECK_MSG(nearF(viewer::pointSegmentDistance({15.0f, 4.0f}, a0, b0),
                            std::sqrt(25.0f + 16.0f)),
                      "beyond end clamps to endpoint distance");
        RIN_CHECK_MSG(nearF(viewer::pointSegmentDistance({-7.0f, 0.0f}, a0, b0), 7.0f),
                      "before start clamps to endpoint distance");
        RIN_CHECK_MSG(nearF(viewer::pointSegmentDistance({2.0f, 4.0f}, a0, b0), 4.0f),
                      "projection interior case");
        RIN_CHECK_MSG(nearF(
            viewer::pointSegmentDistance({6.0f, 7.0f}, {3.0f, 3.0f}, {3.0f, 3.0f}), 5.0f),
            "degenerate segment falls back to point distance");
    }

    // sampleWire：segments+1 个点，首尾等于端点；水平直线时中点在直线上。
    {
        const std::vector<viewer::CanvasPoint> pts =
            viewer::sampleWire({0.0f, 0.0f}, {100.0f, 0.0f}, 8);
        RIN_CHECK_MSG(pts.size() == 9, "sampleWire yields segments+1 points");
        RIN_CHECK_MSG((pts.front() == viewer::CanvasPoint{0.0f, 0.0f}),
                      "first sample is endpoint");
        RIN_CHECK_MSG((pts.back() == viewer::CanvasPoint{100.0f, 0.0f}),
                      "last sample is endpoint");
        RIN_CHECK_MSG((nearPoint(pts[4], {50.0f, 0.0f})), "straight wire midpoint on the line");

        const std::vector<viewer::CanvasPoint> two =
            viewer::sampleWire({0.0f, 0.0f}, {0.0f, 100.0f}, 1);
        RIN_CHECK_MSG(two.size() == 2, "segments=1 yields 2 points");
        RIN_CHECK_MSG((two.front() == viewer::CanvasPoint{0.0f, 0.0f} &&
                       two.back() == viewer::CanvasPoint{0.0f, 100.0f}),
                      "segments=1 endpoints exact");

        // 垂直短连线：水平控制点下限 40 仍对称，中点为线段中点。
        const std::vector<viewer::CanvasPoint> vertical =
            viewer::sampleWire({0.0f, 0.0f}, {0.0f, 100.0f}, 4);
        RIN_CHECK_MSG(vertical.size() == 5, "segments=4 yields 5 points");
        RIN_CHECK_MSG((nearPoint(vertical[2], {0.0f, 50.0f})), "symmetric S-curve midpoint");
    }

    // bezierPoint：t=0/1 端点精确；直线中点。
    {
        const viewer::CanvasPoint p0{1.0f, 2.0f};
        const viewer::CanvasPoint c0{4.0f, 2.0f};
        const viewer::CanvasPoint c1{7.0f, 2.0f};
        const viewer::CanvasPoint p1{10.0f, 2.0f};
        RIN_CHECK_MSG((viewer::bezierPoint(p0, c0, c1, p1, 0.0f) == p0), "t=0 is p0");
        RIN_CHECK_MSG((viewer::bezierPoint(p0, c0, c1, p1, 1.0f) == p1), "t=1 is p1");
        RIN_CHECK_MSG((nearPoint(viewer::bezierPoint(p0, c0, c1, p1, 0.5f), {5.5f, 2.0f})),
                      "collinear controls give the midpoint");
    }
}

// --- 3. 画布图模型：创建 ---

void testGraphCreate() {
    // id 从 1 起非零唯一递增。
    {
        GraphFixture fx;
        const rin::NodeId ia = make(fx.model, "a", 200.0f, 100.0f);
        const rin::NodeId ib = make(fx.model, "b", 500.0f, 100.0f);
        const rin::NodeId im = make(fx.model, "monitor", 800.0f, 100.0f);
        RIN_CHECK_MSG((ia == 1 && ib == 2 && im == 3), "ids are nonzero, unique, increasing");
        RIN_CHECK_MSG(fx.model.nextNodeId == 4, "nextNodeId advances per creation");
    }

    // 未知 typeId 拒绝：ok=false 且不消耗 id、不改变图。
    {
        GraphFixture fx;
        const viewer::CanvasOpResult r = fx.model.createNode("nope", {5.0f, 5.0f});
        RIN_CHECK_MSG(!r.ok, "unknown type rejected");
        RIN_CHECK_MSG(!r.error.empty(), "rejection carries a message");
        RIN_CHECK_MSG(fx.model.nodes.empty(), "no node created");
        RIN_CHECK_MSG(fx.model.nextNodeId == 1, "id not consumed");
        RIN_CHECK_MSG(fx.model.selection.empty(), "selection untouched");
        RIN_CHECK_MSG(fx.model.toGraph() == rin::WorkflowGraph{}, "graph unchanged");
    }

    // 负坐标夹取到 >= 0。
    {
        GraphFixture fx;
        RIN_CHECK(fx.model.createNode("a", {-50.5f, -0.25f}).ok);
        RIN_CHECK_MSG((fx.model.nodes.back().position.x == 0.0f &&
                       fx.model.nodes.back().position.y == 0.0f),
                      "negative position clamped to origin");
    }

    // 创建即独占选中、清除连线选中、校验缓存刷新（monitor 无悬空输入）。
    {
        GraphFixture fx;
        const rin::NodeId ia = make(fx.model, "a", 0.0f, 0.0f);
        const rin::NodeId ib = make(fx.model, "b", 300.0f, 0.0f);
        RIN_CHECK((fx.model.connect({ia, rin::PortDirection::Output, 0},
                                    {ib, rin::PortDirection::Input, 0})
                       .ok));
        fx.model.selectedConnection = fx.model.connections.front();
        fx.model.selectNode(ia, false);
        // selectNode 语义：独占选中节点会清除连线选中，重建状态后再验证创建行为。
        fx.model.selectedConnection = fx.model.connections.front();
        RIN_CHECK(fx.model.selectedConnection.has_value());
        const rin::NodeId im = make(fx.model, "monitor", 600.0f, 0.0f);
        RIN_CHECK_MSG(selectionEquals(fx.model, {im}), "new node exclusively selected");
        RIN_CHECK_MSG(!fx.model.selectedConnection.has_value(),
                      "wire selection cleared on create");
        RIN_CHECK_MSG(!fx.model.nodeHasIssue(im),
                      "monitor clean (validation refreshed after create)");
        RIN_CHECK_MSG((fx.model.nodeHasIssue(ia) && fx.model.nodeHasIssue(ib)),
                      "dangling inputs still flagged");
    }

    // 校验缓存刷新可见性：monitor 单独存在时校验通过（初始 ok=false 翻转为 true）。
    {
        GraphFixture fx;
        RIN_CHECK(make(fx.model, "monitor", 0.0f, 0.0f) != rin::kInvalidNode);
        RIN_CHECK_MSG(fx.model.validation.ok,
                      "monitor-only graph validates (validation refreshed)");
        RIN_CHECK_MSG(fx.model.validation.issues.empty(), "no issues for portless node");
    }

    // 有输入端口的类型单独存在：DanglingInput 标注但不拒绝创建；阻塞判定语义。
    {
        GraphFixture fx;
        const viewer::CanvasOpResult r = fx.model.createNode("a", {0.0f, 0.0f});
        RIN_CHECK_MSG(r.ok, "dangling input does not block creation");
        RIN_CHECK_MSG(!fx.model.validation.ok, "validation reports the dangling input");
        RIN_CHECK_MSG(hasKind(fx.model.validation, rin::ValidationIssueKind::DanglingInput),
                      "DanglingInput issue present");
        RIN_CHECK_MSG(fx.model.nodeHasIssue(1), "issue attached to the new node");
        RIN_CHECK_MSG(!viewer::blockingIssueForConnect(rin::ValidationIssueKind::DanglingInput),
                      "DanglingInput is non-blocking");
        RIN_CHECK_MSG(!viewer::blockingIssueForConnect(rin::ValidationIssueKind::BadParam),
                      "BadParam is non-blocking");
        RIN_CHECK_MSG(viewer::blockingIssueForConnect(rin::ValidationIssueKind::Cycle),
                      "Cycle is blocking");
        RIN_CHECK_MSG(viewer::blockingIssueForConnect(rin::ValidationIssueKind::TypeMismatch),
                      "TypeMismatch is blocking");
        RIN_CHECK_MSG(viewer::blockingIssueForConnect(rin::ValidationIssueKind::MultipleDrivers),
                      "MultipleDrivers is blocking");
    }

    // toGraph() 与模型同步（节点 id/类型/空参数 + 连线拷贝）。
    {
        GraphFixture fx;
        const rin::NodeId ia = make(fx.model, "a", 0.0f, 0.0f);
        const rin::NodeId ib = make(fx.model, "b", 300.0f, 0.0f);
        RIN_CHECK((fx.model.connect({ia, rin::PortDirection::Output, 0},
                                    {ib, rin::PortDirection::Input, 0})
                       .ok));
        const rin::WorkflowGraph graph = fx.model.toGraph();
        RIN_CHECK_MSG((graph.nodes.size() == 2 && graph.connections.size() == 1),
                      "toGraph mirrors model sizes");
        RIN_CHECK_MSG((graph.nodes[0].id == ia && graph.nodes[0].typeId == "a" &&
                       graph.nodes[0].params.empty()),
                      "first node mirrored with empty params");
        RIN_CHECK_MSG((graph.nodes[1].id == ib && graph.nodes[1].typeId == "b"),
                      "second node mirrored");
        RIN_CHECK_MSG(graph.connections == fx.model.connections, "connections mirrored");
    }
}

// --- 4. 画布图模型：删除与移动 ---

void testGraphDelete() {
    GraphFixture fx;
    const rin::NodeId ia = make(fx.model, "a", 200.0f, 100.0f);
    const rin::NodeId ib = make(fx.model, "b", 500.0f, 100.0f);
    const rin::NodeId ic = make(fx.model, "a", 800.0f, 100.0f);
    RIN_CHECK((fx.model.connect({ia, rin::PortDirection::Output, 0},
                                {ib, rin::PortDirection::Input, 0})
                   .ok));
    RIN_CHECK((fx.model.connect({ic, rin::PortDirection::Output, 0},
                                {ib, rin::PortDirection::Input, 1})
                   .ok));

    // 未知 id：整体拒绝且什么都不变（先校验后变更）。
    {
        const std::vector<viewer::CanvasNode> nodesBefore = fx.model.nodes;
        const std::vector<rin::Connection> edgesBefore = fx.model.connections;
        const viewer::CanvasOpResult r = fx.model.deleteNodes({ib, 999});
        RIN_CHECK_MSG(!r.ok, "delete with unknown id rejected");
        RIN_CHECK_MSG(nodesEqual(fx.model.nodes, nodesBefore),
                      "nodes unchanged on rejected delete");
        RIN_CHECK_MSG(fx.model.connections == edgesBefore, "edges unchanged on rejected delete");
    }

    // 删除节点连带两端连线；选中集与 selectedConnection 同步清理。
    fx.model.selectNode(ia, false);
    fx.model.selectedConnection = fx.model.connections.front();
    const viewer::CanvasOpResult r = fx.model.deleteNodes({ia});
    RIN_CHECK_MSG(r.ok, "valid delete accepted");
    RIN_CHECK_MSG(fx.model.nodes.size() == 2, "node removed");
    RIN_CHECK_MSG(fx.model.connections.size() == 1, "edges of deleted node removed");
    RIN_CHECK_MSG(fx.model.connections.front().from.node == ic, "surviving edge is ic's");
    RIN_CHECK_MSG(fx.model.selection.empty(), "deleted node removed from selection");
    RIN_CHECK_MSG(!fx.model.selectedConnection.has_value(),
                  "selection pointing at removed edge cleared");
    RIN_CHECK_MSG(fx.model.nodeHasIssue(ib), "validation refreshed after delete");

    // 删空后校验通过。
    RIN_CHECK(fx.model.deleteNodes({ib, ic}).ok);
    RIN_CHECK_MSG((fx.model.nodes.empty() && fx.model.connections.empty()), "graph emptied");
    RIN_CHECK_MSG((fx.model.validation.ok && fx.model.validation.issues.empty()),
                  "empty graph validates clean");
}

void testGraphMove() {
    GraphFixture fx;
    const rin::NodeId ia = make(fx.model, "a", 200.0f, 100.0f);
    const rin::NodeId ib = make(fx.model, "b", 500.0f, 100.0f);
    RIN_CHECK(fx.model.moveNode(ia, {10.0f, -20.0f}).ok);
    const viewer::CanvasNode* na = fx.model.findNode(ia);
    const viewer::CanvasNode* nb = fx.model.findNode(ib);
    RIN_CHECK((na != nullptr && nb != nullptr));
    if (na != nullptr) {
        RIN_CHECK_MSG((na->position.x == 210.0f && na->position.y == 80.0f),
                      "move applies delta");
    }
    if (nb != nullptr) {
        RIN_CHECK_MSG((nb->position.x == 500.0f && nb->position.y == 100.0f),
                      "other node untouched");
    }
    const viewer::CanvasOpResult r = fx.model.moveNode(999, {1.0f, 1.0f});
    RIN_CHECK_MSG(!r.ok, "move unknown id rejected");
}

// --- 5. 画布图模型：连线 ---

void testGraphConnect() {
    GraphFixture fx;
    const rin::NodeId ia = make(fx.model, "a", 200.0f, 100.0f);
    const rin::NodeId ib = make(fx.model, "b", 500.0f, 100.0f);

    // 仅 Output→Input。
    {
        const viewer::CanvasOpResult r1 = fx.model.connect(
            {ia, rin::PortDirection::Input, 0}, {ib, rin::PortDirection::Input, 0});
        const viewer::CanvasOpResult r2 = fx.model.connect(
            {ia, rin::PortDirection::Output, 0}, {ib, rin::PortDirection::Output, 0});
        RIN_CHECK_MSG((!r1.ok && !r2.ok), "non Output->Input rejected");
        RIN_CHECK_MSG(fx.model.connections.empty(), "no edge created by rejected connects");
    }

    // 端口序号越界。
    {
        const viewer::CanvasOpResult r1 = fx.model.connect(
            {ia, rin::PortDirection::Output, 5}, {ib, rin::PortDirection::Input, 0});
        const viewer::CanvasOpResult r2 = fx.model.connect(
            {ia, rin::PortDirection::Output, 0}, {ib, rin::PortDirection::Input, 2});
        RIN_CHECK_MSG((!r1.ok && !r2.ok), "out-of-range port index rejected");
        RIN_CHECK_MSG(fx.model.connections.empty(),
                      "no edge created by out-of-range connects");
    }

    // 自环。
    {
        const viewer::CanvasOpResult r = fx.model.connect({ia, rin::PortDirection::Output, 0},
                                                          {ia, rin::PortDirection::Input, 0});
        RIN_CHECK_MSG(!r.ok, "self loop rejected");
        RIN_CHECK_MSG(r.error.find("self") != std::string::npos, "self loop message");
    }

    // 类型不匹配：错误消息含 toString(PortType)。
    {
        const rin::NodeId ic = make(fx.model, "c", 800.0f, 100.0f);
        const viewer::CanvasOpResult r = fx.model.connect(
            {ia, rin::PortDirection::Output, 0}, {ic, rin::PortDirection::Input, 0});
        RIN_CHECK_MSG(!r.ok, "Gray8 -> Rgba8 rejected");
        RIN_CHECK_MSG((r.error.find("Gray8") != std::string::npos &&
                       r.error.find("Rgba8") != std::string::npos),
                      "mismatch message names both port types");
        RIN_CHECK_MSG(fx.model.connections.empty(), "no edge created by mismatched connect");
    }

    // 正常连接 + 输出扇出。
    RIN_CHECK((fx.model.connect({ia, rin::PortDirection::Output, 0},
                                {ib, rin::PortDirection::Input, 0})
                   .ok));
    RIN_CHECK((fx.model.connect({ia, rin::PortDirection::Output, 0},
                                {ib, rin::PortDirection::Input, 1})
                   .ok));
    RIN_CHECK_MSG(fx.model.connections.size() == 2, "fan-out keeps both edges");

    // 重复同边幂等。
    RIN_CHECK((fx.model.connect({ia, rin::PortDirection::Output, 0},
                                {ib, rin::PortDirection::Input, 0})
                   .ok));
    RIN_CHECK_MSG(fx.model.connections.size() == 2, "duplicate edge is idempotent");

    // 替换语义：目标输入口已有入边时替换旧边。
    {
        const rin::NodeId ia2 = make(fx.model, "a", 200.0f, 300.0f);
        RIN_CHECK((fx.model.connect({ia2, rin::PortDirection::Output, 0},
                                    {ib, rin::PortDirection::Input, 0})
                       .ok));
        RIN_CHECK_MSG(fx.model.connections.size() == 2,
                      "replacement keeps one edge per input");
        bool oldGone = true;
        bool newPresent = false;
        for (const rin::Connection& c : fx.model.connections) {
            if (c.to == rin::PortRef{ib, rin::PortDirection::Input, 0}) {
                newPresent = c.from.node == ia2;
            }
            if (c.from.node == ia &&
                c.to == rin::PortRef{ib, rin::PortDirection::Input, 0}) {
                oldGone = false;
            }
        }
        RIN_CHECK_MSG(oldGone, "old edge to the input removed");
        RIN_CHECK_MSG(newPresent, "new edge drives the input");
        RIN_CHECK_MSG(!hasKind(fx.model.validation, rin::ValidationIssueKind::MultipleDrivers),
                      "no MultipleDrivers after replacement");

        // 被替换边的 selectedConnection 同步清理。
        fx.model.selectedConnection = rin::Connection{
            {ia, rin::PortDirection::Output, 0}, {ib, rin::PortDirection::Input, 0}};
        RIN_CHECK((fx.model.connect({ia, rin::PortDirection::Output, 0},
                                    {ib, rin::PortDirection::Input, 0})
                       .ok));
        RIN_CHECK_MSG(!fx.model.selectedConnection.has_value(),
                      "selectedConnection cleared when its edge is replaced");
        RIN_CHECK((fx.model.connect({ia2, rin::PortDirection::Output, 0},
                                    {ib, rin::PortDirection::Input, 0})
                       .ok));
    }

    // 成环拒绝：A->B 后 B->A 必须拒绝且图不变（validateWorkflowGraph 唯一判据）。
    {
        const std::size_t before = fx.model.connections.size();
        const rin::Connection firstEdge = fx.model.connections.front();
        const viewer::CanvasOpResult r = fx.model.connect({ib, rin::PortDirection::Output, 0},
                                                          {ia, rin::PortDirection::Input, 0});
        RIN_CHECK_MSG(!r.ok, "cycle-forming connect rejected");
        RIN_CHECK_MSG(!r.error.empty(), "cycle rejection carries a message");
        RIN_CHECK_MSG(fx.model.connections.size() == before,
                      "connections unchanged on reject");
        RIN_CHECK_MSG(fx.model.connections.front() == firstEdge, "original edge intact");
        RIN_CHECK_MSG(!hasKind(fx.model.validation, rin::ValidationIssueKind::Cycle),
                      "stored validation has no cycle");
    }

    // 悬空输入非阻塞：图中始终存在悬空输入，连接仍成功。
    RIN_CHECK_MSG(hasKind(fx.model.validation, rin::ValidationIssueKind::DanglingInput),
                  "dangling inputs annotated throughout");
    RIN_CHECK_MSG(fx.model.connections.size() == 2, "graph still holds two edges");
}

void testGraphDisconnect() {
    GraphFixture fx;
    const rin::NodeId ia = make(fx.model, "a", 200.0f, 100.0f);
    const rin::NodeId ib = make(fx.model, "b", 500.0f, 100.0f);
    const rin::Connection e1{{ia, rin::PortDirection::Output, 0},
                             {ib, rin::PortDirection::Input, 0}};
    const rin::Connection ghost{{ib, rin::PortDirection::Output, 0},
                                {ia, rin::PortDirection::Input, 0}};

    const viewer::CanvasOpResult missing = fx.model.disconnect(ghost);
    RIN_CHECK_MSG(!missing.ok, "disconnect of absent edge rejected");
    RIN_CHECK_MSG(fx.model.connections.empty(), "absent edge disconnect leaves graph alone");

    RIN_CHECK(fx.model.connect(e1.from, e1.to).ok);
    fx.model.selectedConnection = e1;
    const viewer::CanvasOpResult r = fx.model.disconnect(e1);
    RIN_CHECK_MSG(r.ok, "existing edge disconnected");
    RIN_CHECK_MSG(fx.model.connections.empty(), "edge removed");
    RIN_CHECK_MSG(!fx.model.selectedConnection.has_value(),
                  "selectedConnection cleared with its edge");

    // 删除别的连线不动选中。
    RIN_CHECK(fx.model.connect(e1.from, e1.to).ok);
    const rin::Connection e2{{ia, rin::PortDirection::Output, 0},
                             {ib, rin::PortDirection::Input, 1}};
    RIN_CHECK(fx.model.connect(e2.from, e2.to).ok);
    fx.model.selectedConnection = e1;
    RIN_CHECK(fx.model.disconnect(e2).ok);
    RIN_CHECK_MSG((fx.model.selectedConnection.has_value() &&
                   *fx.model.selectedConnection == e1),
                  "selection to a different edge preserved");
}

// --- 6. 选中与绘制序 ---

void testSelectionAndOrder() {
    GraphFixture fx;
    const rin::NodeId ia = make(fx.model, "a", 200.0f, 100.0f);
    const rin::NodeId ib = make(fx.model, "a", 210.0f, 110.0f);  // 与 ia 重叠，后创建在顶
    const rin::NodeId ic = make(fx.model, "a", 800.0f, 100.0f);

    // raise：顶层优先影响 nodeAt 与 toGraph 绘制序（先于会触发 raise 的选中操作）。
    const viewer::CanvasNode* top = fx.model.nodeAt({260.0f, 130.0f});
    RIN_CHECK_MSG((top != nullptr && top->id == ib), "later node on top before raise");
    fx.model.raise(ia);
    top = fx.model.nodeAt({260.0f, 130.0f});
    RIN_CHECK_MSG((top != nullptr && top->id == ia), "raised node becomes topmost");
    RIN_CHECK_MSG((!fx.model.toGraph().nodes.empty() &&
                   fx.model.toGraph().nodes.back().id == ia),
                  "raised node last in draw order");
    RIN_CHECK_MSG(fx.model.nodeAt({150.0f, 300.0f}) == nullptr, "blank space misses all nodes");

    // 独占选中。
    fx.model.selectNode(ic, false);
    RIN_CHECK_MSG(selectionEquals(fx.model, {ic}), "exclusive select");
    RIN_CHECK_MSG(!fx.model.selectedConnection.has_value(),
                  "exclusive select clears wire selection");

    // additive=true：未选中 → 加入；已选中 → 移除。
    fx.model.selectNode(ia, true);
    RIN_CHECK_MSG(selectionEquals(fx.model, {ic, ia}), "additive select adds");
    fx.model.selectNode(ia, true);
    RIN_CHECK_MSG(selectionEquals(fx.model, {ic}), "additive select on selected removes");
    fx.model.selectNode(ia, true);
    fx.model.clearSelection();
    RIN_CHECK_MSG(fx.model.selection.empty(), "clearSelection empties selection");

    // nodesInRect：相交即入选。
    RIN_CHECK(fx.model.nodes.size() == 3);
    std::vector<rin::NodeId> all = fx.model.nodesInRect({0.0f, 0.0f, 2000.0f, 2000.0f});
    RIN_CHECK_MSG(all.size() == 3, "big rect catches all nodes");
    all = fx.model.nodesInRect({0.0f, 0.0f, 100.0f, 50.0f});
    RIN_CHECK_MSG(all.empty(), "empty region catches nothing");
    all = fx.model.nodesInRect({790.0f, 90.0f, 20.0f, 20.0f});
    RIN_CHECK_MSG((all.size() == 1 && all.front() == ic), "partial overlap selects");
}

// --- 7. 命中检测 ---

void testHitTesting() {
    GraphFixture fx;
    const rin::NodeId ia = make(fx.model, "a", 200.0f, 100.0f);
    const rin::NodeId iTop = make(fx.model, "a", 200.0f, 100.0f);  // 完全重叠，顶层
    const rin::NodeId ib = make(fx.model, "b", 400.0f, 100.0f);
    RIN_CHECK((ia != rin::kInvalidNode && iTop != rin::kInvalidNode && ib != rin::kInvalidNode));

    // portAt：命中半径 12 > 视觉半径 4；边界含 12、超过 12 不中。
    {
        const std::optional<rin::PortRef> boundary = fx.model.portAt({412.0f, 136.0f});
        RIN_CHECK_MSG((boundary.has_value() &&
                       *boundary == rin::PortRef{ib, rin::PortDirection::Input, 0}),
                      "hit radius boundary (distance 12) included");
        RIN_CHECK_MSG(!fx.model.portAt({413.0f, 136.0f}).has_value(), "distance 13 misses");
        const std::optional<rin::PortRef> expanded = fx.model.portAt({405.0f, 136.0f});
        RIN_CHECK_MSG(expanded.has_value(),
                      "distance 5 is outside visual radius 4 but inside hit radius");
        RIN_CHECK_MSG(!fx.model.portAt({274.0f, 127.0f}).has_value(),
                      "node body away from ports hits no port");
    }

    // portAt：两节点重叠时顶层节点端口优先。
    {
        const std::optional<rin::PortRef> p = fx.model.portAt({200.0f, 136.0f});
        RIN_CHECK_MSG((p.has_value() && p->node == iTop &&
                       p->direction == rin::PortDirection::Input),
                      "overlapping nodes: topmost node's port wins");
    }

    // nodeAt：顶层优先 + 空白未命中。
    {
        const viewer::CanvasNode* top = fx.model.nodeAt({250.0f, 120.0f});
        RIN_CHECK_MSG((top != nullptr && top->id == iTop), "nodeAt prefers topmost");
    }

    // wireAt：需要连线。两条水平连线 y=136 与 y=143（相距 7px）。
    const rin::NodeId iw1 = make(fx.model, "a", 100.0f, 100.0f);   // out0 (300,136)
    const rin::NodeId iw2 = make(fx.model, "a", 100.0f, 107.0f);   // out0 (300,143)
    const rin::NodeId ib2 = make(fx.model, "b", 400.0f, 107.0f);   // in0 (400,143)
    const rin::Connection w1{{iw1, rin::PortDirection::Output, 0},
                             {ib, rin::PortDirection::Input, 0}};
    const rin::Connection w2{{iw2, rin::PortDirection::Output, 0},
                             {ib2, rin::PortDirection::Input, 0}};
    RIN_CHECK(fx.model.connect(w1.from, w1.to).ok);
    RIN_CHECK(fx.model.connect(w2.from, w2.to).ok);

    {
        const std::optional<rin::Connection> hit = fx.model.wireAt({320.0f, 136.0f});
        RIN_CHECK_MSG((hit.has_value() && *hit == w1), "wire hit on the polyline");
        RIN_CHECK_MSG(!fx.model.wireAt({320.0f, 151.0f}).has_value(),
                      "distances 15/8 beyond kWireHitDistance both miss");
        RIN_CHECK_MSG(!fx.model.wireAt({320.0f, 127.0f}).has_value(),
                      "no wire within threshold far above");

        // 两条连线都在阈值内时取最近（边界含 7 的精确断言不设：采样折线在
        // 阈值处有 ~1e-5 量级浮点噪声，过约束）。
        const std::optional<rin::Connection> nearW1 = fx.model.wireAt({320.0f, 138.0f});
        RIN_CHECK_MSG((nearW1.has_value() && *nearW1 == w1),
                      "distance 2 vs 5: nearer first edge wins");
        const std::optional<rin::Connection> nearW2 = fx.model.wireAt({320.0f, 141.0f});
        RIN_CHECK_MSG((nearW2.has_value() && *nearW2 == w2),
                      "distance 5 vs 2: nearer second edge wins");
        const std::optional<rin::Connection> inside = fx.model.wireAt({320.0f, 149.5f});
        RIN_CHECK_MSG((inside.has_value() && *inside == w2),
                      "distance 6.5 within threshold hits the nearer wire");
    }
}

// --- 8. 交互状态机：框选 ---

void testInteractionMarquee() {
    // 框选命中。
    {
        GraphFixture fx;
        RIN_CHECK(make(fx.model, "a", 200.0f, 100.0f) != rin::kInvalidNode);
        RIN_CHECK(make(fx.model, "a", 500.0f, 100.0f) != rin::kInvalidNode);
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        const viewer::PressResult pr = it.onPress(fx.model, {100.0f, 10.0f}, 0, false, false);
        RIN_CHECK_MSG(pr.kind == viewer::PressResult::Kind::None, "blank press is untyped");
        RIN_CHECK_MSG((it.mode == viewer::InteractionMode::Marquee && !it.marqueeArmed),
                      "blank press arms marquee mode");
        it.onDrag(fx.model, view, {600.0f, 200.0f}, {600.0f, 200.0f});
        RIN_CHECK_MSG((it.marqueeArmed && it.marqueeing()),
                      "drag beyond threshold arms marquee");
        const viewer::ReleaseResult rr = it.onRelease(fx.model, {600.0f, 200.0f}, false);
        RIN_CHECK_MSG((!rr.graphChanged && rr.message.empty()), "marquee is not a graph change");
        RIN_CHECK_MSG(selectionEquals(fx.model, {1, 2}), "rect selects intersecting nodes");
        RIN_CHECK_MSG(it.mode == viewer::InteractionMode::None, "mode reset after release");
    }

    // 原位松开（未超阈值）= 清除选中。
    {
        GraphFixture fx;
        const rin::NodeId ia = make(fx.model, "a", 200.0f, 100.0f);
        fx.model.selectNode(ia, false);
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        pressIgnore(it, fx.model, {10.0f, 10.0f});
        it.onDrag(fx.model, view, {12.0f, 12.0f}, {12.0f, 12.0f});
        RIN_CHECK_MSG(!it.marqueeArmed, "2px drag does not arm");
        releaseIgnore(it, fx.model, {12.0f, 12.0f});
        RIN_CHECK_MSG(fx.model.selection.empty(), "in-place release clears selection");
        RIN_CHECK(ia != rin::kInvalidNode);
    }

    // 阈值严格大于 4px：恰 4px 不武装（即便矩形覆盖节点也走清除路径）。
    {
        GraphFixture fx;
        const rin::NodeId ia = make(fx.model, "a", 200.0f, 100.0f);
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        pressIgnore(it, fx.model, {198.0f, 96.0f});
        it.onDrag(fx.model, view, {202.0f, 100.0f}, {202.0f, 100.0f});
        RIN_CHECK_MSG(!it.marqueeArmed, "exactly 4px does not arm");
        releaseIgnore(it, fx.model, {202.0f, 100.0f});
        RIN_CHECK_MSG(fx.model.selection.empty(), "unarmed release clears even over a node");
        RIN_CHECK(ia != rin::kInvalidNode);
    }

    // 4.5px 武装且按矩形选中。
    {
        GraphFixture fx;
        RIN_CHECK(make(fx.model, "a", 200.0f, 100.0f) != rin::kInvalidNode);
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        pressIgnore(it, fx.model, {198.0f, 96.0f});
        it.onDrag(fx.model, view, {202.5f, 100.5f}, {202.5f, 100.5f});
        RIN_CHECK_MSG(it.marqueeArmed, "4.5px arms marquee");
        releaseIgnore(it, fx.model, {202.5f, 100.5f});
        RIN_CHECK_MSG(fx.model.selection.size() == 1,
                      "tiny rect still selects intersecting node");
    }

    // Shift+松开并入按下时基准选中集；无 Shift 只取矩形命中。
    {
        GraphFixture fx;
        const rin::NodeId ia = make(fx.model, "a", 200.0f, 100.0f);
        const rin::NodeId ib = make(fx.model, "a", 500.0f, 100.0f);
        fx.model.selectNode(ia, false);
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        pressIgnore(it, fx.model, {450.0f, 0.0f});
        RIN_CHECK_MSG((it.marqueeBaseSelection.size() == 1 &&
                       it.marqueeBaseSelection.front() == ia),
                      "base selection captured at press");
        it.onDrag(fx.model, view, {700.0f, 200.0f}, {700.0f, 200.0f});
        releaseIgnore(it, fx.model, {700.0f, 200.0f}, true);
        RIN_CHECK_MSG(selectionEquals(fx.model, {ia, ib}),
                      "shift release merges base selection");
        RIN_CHECK(ia != rin::kInvalidNode && ib != rin::kInvalidNode);
    }
    {
        GraphFixture fx;
        const rin::NodeId ia = make(fx.model, "a", 200.0f, 100.0f);
        const rin::NodeId ib = make(fx.model, "a", 500.0f, 100.0f);
        fx.model.selectNode(ia, false);
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        pressIgnore(it, fx.model, {450.0f, 0.0f});
        it.onDrag(fx.model, view, {700.0f, 200.0f}, {700.0f, 200.0f});
        releaseIgnore(it, fx.model, {700.0f, 200.0f}, false);
        RIN_CHECK_MSG(selectionEquals(fx.model, {ib}), "plain release takes rect hits only");
        RIN_CHECK(ia != rin::kInvalidNode && ib != rin::kInvalidNode);
    }
}

// --- 9. 交互状态机：节点拖动 ---

void testInteractionNodeDrag() {
    // 按下 → NodeDrag；onDrag 绝对定位 position = 按下基准 + (canvasPoint -
    // dragStartPoint)，单节点与旧 grabOffset 数学等价。
    {
        GraphFixture fx;
        const rin::NodeId ia = make(fx.model, "a", 200.0f, 100.0f);
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        const viewer::PressResult pr = it.onPress(fx.model, {250.0f, 120.0f}, 0, false, false);
        RIN_CHECK_MSG(pr.kind == viewer::PressResult::Kind::NodePressed, "body press reported");
        RIN_CHECK_MSG(
            (it.mode == viewer::InteractionMode::NodeDrag && it.draggedNode == ia),
            "press enters NodeDrag on the pressed node");
        RIN_CHECK_MSG((it.dragStartPoint.x == 250.0f && it.dragStartPoint.y == 120.0f),
                      "dragStartPoint is the press point");
        RIN_CHECK_MSG((it.dragGroupBase.size() == 1 && it.dragGroupBase.front().first == ia &&
                       nearPoint(it.dragGroupBase.front().second, {200.0f, 100.0f})),
                      "single-node group base is the pressed node's position");
        it.onDrag(fx.model, view, {320.0f, 90.0f}, {320.0f, 90.0f});
        const viewer::CanvasNode* na = fx.model.findNode(ia);
        RIN_CHECK(na != nullptr);
        if (na != nullptr) {
            RIN_CHECK_MSG((na->position.x == 270.0f && na->position.y == 70.0f),
                          "absolute positioning (200,100)+(320,90)-(250,120)");
        }
        const viewer::ReleaseResult rr = it.onRelease(fx.model, {320.0f, 90.0f}, false);
        RIN_CHECK_MSG((rr.message.empty() && !rr.graphChanged),
                      "node drag is not a graph change");
        RIN_CHECK_MSG((it.mode == viewer::InteractionMode::None &&
                       it.draggedNode == rin::kInvalidNode),
                      "release resets mode and draggedNode");
    }

    // 已在选中集中的节点再按下不重置选中集。
    {
        GraphFixture fx;
        const rin::NodeId ia = make(fx.model, "a", 200.0f, 100.0f);
        const rin::NodeId ib = make(fx.model, "a", 500.0f, 100.0f);
        fx.model.selectNode(ia, false);
        fx.model.selectNode(ib, true);
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        const viewer::PressResult pr = it.onPress(fx.model, {520.0f, 110.0f}, 0, false, false);
        RIN_CHECK_MSG(pr.kind == viewer::PressResult::Kind::NodePressed,
                      "group press reported");
        RIN_CHECK_MSG(selectionEquals(fx.model, {ia, ib}),
                      "pressing an already-selected node keeps the selection");
        RIN_CHECK(ia != rin::kInvalidNode && ib != rin::kInvalidNode);
    }

    // 组拖动正例：框选两节点后按下其中之一拖动，整组随动、相对位置不变、
    // 位置始终锚定按下时基准（多次 drag 事件不叠加）。
    {
        GraphFixture fx;
        const rin::NodeId ia = make(fx.model, "a", 200.0f, 100.0f);
        const rin::NodeId ib = make(fx.model, "a", 500.0f, 100.0f);
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        pressIgnore(it, fx.model, {100.0f, 50.0f});
        it.onDrag(fx.model, view, {700.0f, 200.0f}, {700.0f, 200.0f});
        releaseIgnore(it, fx.model, {700.0f, 200.0f});
        RIN_CHECK_MSG(selectionEquals(fx.model, {ia, ib}), "marquee selects both nodes");
        const viewer::PressResult pr = it.onPress(fx.model, {220.0f, 110.0f}, 0, false, false);
        RIN_CHECK_MSG((pr.kind == viewer::PressResult::Kind::NodePressed &&
                       selectionEquals(fx.model, {ia, ib})),
                      "pressing a group member keeps the selection");
        RIN_CHECK_MSG(it.draggedNode == ia, "dragged node is the pressed one");
        RIN_CHECK_MSG((it.dragGroupBase.size() == 2),
                      "group base captured for both selected nodes");
        it.onDrag(fx.model, view, {300.0f, 120.0f}, {300.0f, 120.0f});
        const viewer::CanvasNode* n1 = fx.model.findNode(ia);
        const viewer::CanvasNode* n2 = fx.model.findNode(ib);
        RIN_CHECK((n1 != nullptr && n2 != nullptr));
        if (n1 != nullptr && n2 != nullptr) {
            RIN_CHECK_MSG((nearPoint(n1->position, {280.0f, 110.0f}) &&
                           nearPoint(n2->position, {580.0f, 110.0f})),
                          "group drag moves both nodes by the drag delta");
            RIN_CHECK_MSG((nearF(n2->position.x - n1->position.x, 300.0f) &&
                           nearF(n2->position.y - n1->position.y, 0.0f)),
                          "relative positions preserved");
        }
        // 基准锚定：后续 drag 事件按按下基准重算，不逐事件叠加。
        it.onDrag(fx.model, view, {320.0f, 140.0f}, {320.0f, 140.0f});
        if (n1 != nullptr && n2 != nullptr) {
            RIN_CHECK_MSG((nearPoint(n1->position, {300.0f, 130.0f}) &&
                           nearPoint(n2->position, {600.0f, 130.0f})),
                          "position = press base + total delta (no event compounding)");
        }
        releaseIgnore(it, fx.model, {320.0f, 140.0f});
        RIN_CHECK_MSG(it.mode == viewer::InteractionMode::None, "release ends group drag");
        RIN_CHECK(ia != rin::kInvalidNode && ib != rin::kInvalidNode);
    }

    // Shift+点按节点切换其选中：多节点布局下由 iva_defect_probes（探针 4）
    // 作为回归守卫覆盖（曾在此暴露 use-after-rotate 缺陷）。
}

// --- 10. 交互状态机：连线拖拽 ---

void testInteractionConnectFlow() {
    // 松开在兼容输入端口：建立连线。
    {
        GraphFixture fx;
        const rin::NodeId ia = make(fx.model, "a", 200.0f, 100.0f);
        const rin::NodeId ib = make(fx.model, "b", 500.0f, 100.0f);
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        const viewer::PressResult pr =
            it.onPress(fx.model, {400.0f, 136.0f}, 0, false, false);  // a.out0
        RIN_CHECK_MSG((pr.kind == viewer::PressResult::Kind::PortPressed && pr.message.empty()),
                      "output port press reported");
        RIN_CHECK_MSG((it.connecting() && it.mode == viewer::InteractionMode::Connect),
                      "press on output port starts Connect mode");
        RIN_CHECK_MSG((it.connectFromPort() ==
                       rin::PortRef{ia, rin::PortDirection::Output, 0}),
                      "connectFromPort is the pressed output");
        it.onDrag(fx.model, view, {450.0f, 130.0f}, {450.0f, 130.0f});
        RIN_CHECK_MSG((nearPoint(it.connectCurrent, {450.0f, 130.0f})),
                      "drag updates connectCurrent");
        const viewer::ReleaseResult rr = it.onRelease(fx.model, {500.0f, 136.0f}, false);
        RIN_CHECK_MSG((rr.graphChanged && rr.message.empty()),
                      "release on compatible input connects");
        RIN_CHECK_MSG((fx.model.connections.size() == 1 &&
                       fx.model.connections.front() ==
                           rin::Connection{{ia, rin::PortDirection::Output, 0},
                                           {ib, rin::PortDirection::Input, 0}}),
                      "edge created between dragged ports");
        RIN_CHECK_MSG((!it.connecting() && it.mode == viewer::InteractionMode::None),
                      "connect mode ends after release");
        RIN_CHECK(ia != rin::kInvalidNode && ib != rin::kInvalidNode);
    }

    // 松开在空白：取消，无图变更、消息为空。
    {
        GraphFixture fx;
        RIN_CHECK(make(fx.model, "a", 200.0f, 100.0f) != rin::kInvalidNode);
        RIN_CHECK(make(fx.model, "b", 500.0f, 100.0f) != rin::kInvalidNode);
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        pressIgnore(it, fx.model, {400.0f, 136.0f});
        it.onDrag(fx.model, view, {600.0f, 300.0f}, {600.0f, 300.0f});
        const viewer::ReleaseResult rr = it.onRelease(fx.model, {700.0f, 400.0f}, false);
        RIN_CHECK_MSG((!rr.graphChanged && rr.message.empty()),
                      "release on blank cancels the drag silently");
        RIN_CHECK_MSG(fx.model.connections.empty(), "no edge created on cancel");
    }

    // 松开在不兼容输入端口：connect 拒绝消息透传。c 带 1 个参数（M11 起参数区
    // 参与几何），其输入端口锚点在 y = 100 + 26 + 36 + 10 = 172。
    {
        GraphFixture fx;
        RIN_CHECK(make(fx.model, "a", 200.0f, 100.0f) != rin::kInvalidNode);
        RIN_CHECK(make(fx.model, "c", 500.0f, 100.0f) != rin::kInvalidNode);
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        pressIgnore(it, fx.model, {400.0f, 136.0f});
        const viewer::ReleaseResult rr = it.onRelease(fx.model, {500.0f, 172.0f}, false);
        RIN_CHECK_MSG(!rr.graphChanged, "incompatible target changes nothing");
        RIN_CHECK_MSG((rr.message.find("Gray8") != std::string::npos &&
                       rr.message.find("Rgba8") != std::string::npos),
                      "rejection message passed through with port type names");
        RIN_CHECK_MSG(fx.model.connections.empty(), "no edge created on mismatch");
    }

    // 松开在输出端口：提示必须落在输入端口。
    {
        GraphFixture fx;
        RIN_CHECK(make(fx.model, "a", 200.0f, 100.0f) != rin::kInvalidNode);
        RIN_CHECK(make(fx.model, "a", 500.0f, 100.0f) != rin::kInvalidNode);
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        pressIgnore(it, fx.model, {400.0f, 136.0f});
        const viewer::ReleaseResult rr = it.onRelease(fx.model, {700.0f, 136.0f}, false);
        RIN_CHECK_MSG((!rr.graphChanged && rr.message == "wires end at an input port"),
                      "release on an output port reports guidance");
        RIN_CHECK_MSG(fx.model.connections.empty(), "no edge created");
    }

    // 从输入端口按下：不发起连线，按节点拖动处理并提示（合并节点分支后
    // 返回 NodePressed + portHint 消息）。
    {
        GraphFixture fx;
        const rin::NodeId ia = make(fx.model, "a", 200.0f, 100.0f);
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        const viewer::PressResult pr = it.onPress(fx.model, {200.0f, 136.0f}, 0, false, false);
        RIN_CHECK_MSG((pr.kind == viewer::PressResult::Kind::NodePressed &&
                       pr.message == "wires start at an output port"),
                      "input port press reports guidance and presses the node");
        RIN_CHECK_MSG(!it.connecting(), "no wire drag starts from an input port");
        RIN_CHECK_MSG((it.mode == viewer::InteractionMode::NodeDrag && it.draggedNode == ia),
                      "input port press falls back to node drag");
        it.onDrag(fx.model, view, {260.0f, 150.0f}, {260.0f, 150.0f});
        const viewer::CanvasNode* na = fx.model.findNode(ia);
        RIN_CHECK(na != nullptr);
        if (na != nullptr) {
            RIN_CHECK_MSG((na->position.x == 260.0f && na->position.y == 114.0f),
                          "fallback drag positions node absolutely");
        }
        RIN_CHECK(ia != rin::kInvalidNode);

        // 多节点布局：输入端口回退必须仍锚定被按节点（use-after-rotate 修复回归）。
        GraphFixture fx2;
        const rin::NodeId ia2 = make(fx2.model, "a", 200.0f, 100.0f);
        RIN_CHECK(make(fx2.model, "a", 500.0f, 100.0f) != rin::kInvalidNode);
        viewer::CanvasView view2;
        viewer::CanvasInteraction it2;
        const viewer::PressResult pr2 =
            it2.onPress(fx2.model, {200.0f, 136.0f}, 0, false, false);
        RIN_CHECK_MSG((pr2.message == "wires start at an output port" &&
                       it2.draggedNode == ia2 &&
                       it2.mode == viewer::InteractionMode::NodeDrag),
                      "input port fallback anchors the pressed node (multi-node)");
        it2.onDrag(fx2.model, view2, {230.0f, 146.0f}, {230.0f, 146.0f});
        const viewer::CanvasNode* n1 = fx2.model.findNode(ia2);
        const viewer::CanvasNode* n2 = fx2.model.findNode(2);
        RIN_CHECK((n1 != nullptr && n2 != nullptr));
        if (n1 != nullptr && n2 != nullptr) {
            RIN_CHECK_MSG((nearPoint(n1->position, {230.0f, 110.0f}) &&
                           nearPoint(n2->position, {500.0f, 100.0f})),
                          "fallback drag moves only the pressed node");
        }
    }

    // 拖回自身输入端口：自环拒绝透传。
    {
        GraphFixture fx;
        RIN_CHECK(make(fx.model, "a", 200.0f, 100.0f) != rin::kInvalidNode);
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        pressIgnore(it, fx.model, {400.0f, 136.0f});
        const viewer::ReleaseResult rr = it.onRelease(fx.model, {200.0f, 136.0f}, false);
        RIN_CHECK_MSG((!rr.graphChanged && rr.message.find("self") != std::string::npos),
                      "self-loop release passes the rejection message through");
        RIN_CHECK_MSG(fx.model.connections.empty(), "no self edge created");
    }
}

// --- 11. 交互状态机：连线与右键 ---

void testInteractionWireAndContext() {
    // Alt+点击连线 = 直接删除。
    {
        GraphFixture fx;
        RIN_CHECK(make(fx.model, "a", 200.0f, 100.0f) != rin::kInvalidNode);
        RIN_CHECK(make(fx.model, "b", 500.0f, 100.0f) != rin::kInvalidNode);
        RIN_CHECK((fx.model.connect({1, rin::PortDirection::Output, 0},
                                    {2, rin::PortDirection::Input, 0})
                       .ok));
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        const viewer::PressResult pr = it.onPress(fx.model, {430.0f, 136.0f}, 0, false, true);
        RIN_CHECK_MSG((pr.kind == viewer::PressResult::Kind::WireDeleted &&
                       pr.message == "connection removed" && pr.graphChanged),
                      "alt+click deletes the wire with feedback");
        RIN_CHECK_MSG(fx.model.connections.empty(), "wire removed");
    }

    // 无 Alt 点击连线 = 选中连线，节点选中清空。
    {
        GraphFixture fx;
        RIN_CHECK(make(fx.model, "a", 200.0f, 100.0f) != rin::kInvalidNode);
        RIN_CHECK(make(fx.model, "b", 500.0f, 100.0f) != rin::kInvalidNode);
        const rin::Connection edge{{1, rin::PortDirection::Output, 0},
                                   {2, rin::PortDirection::Input, 0}};
        RIN_CHECK(fx.model.connect(edge.from, edge.to).ok);
        fx.model.selectNode(1, false);
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        const viewer::PressResult pr = it.onPress(fx.model, {430.0f, 136.0f}, 0, false, false);
        RIN_CHECK_MSG(
            (pr.kind == viewer::PressResult::Kind::WireSelected && !pr.graphChanged),
            "plain click selects the wire");
        RIN_CHECK_MSG((fx.model.selectedConnection.has_value() &&
                       *fx.model.selectedConnection == edge),
                      "selectedConnection set to clicked wire");
        RIN_CHECK_MSG(fx.model.selection.empty(), "node selection cleared by wire selection");
    }

    // 右键按下节点 = 删除该节点（连带边）。
    {
        GraphFixture fx;
        RIN_CHECK(make(fx.model, "a", 200.0f, 100.0f) != rin::kInvalidNode);
        RIN_CHECK(make(fx.model, "b", 500.0f, 100.0f) != rin::kInvalidNode);
        RIN_CHECK((fx.model.connect({1, rin::PortDirection::Output, 0},
                                    {2, rin::PortDirection::Input, 0})
                       .ok));
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        const viewer::PressResult pr = it.onPress(fx.model, {250.0f, 120.0f}, 2, false, false);
        RIN_CHECK_MSG((pr.kind == viewer::PressResult::Kind::NodeDeleted &&
                       pr.message == "node removed" && pr.graphChanged),
                      "right press deletes the node with feedback");
        RIN_CHECK_MSG(fx.model.nodes.size() == 1, "node removed");
        RIN_CHECK_MSG(fx.model.connections.empty(), "its edges removed");
        RIN_CHECK_MSG(fx.model.nodeHasIssue(2), "validation refreshed after right-delete");
    }

    // 右键按下空白 = ContextMenu，无图变更。
    {
        GraphFixture fx;
        RIN_CHECK(make(fx.model, "a", 200.0f, 100.0f) != rin::kInvalidNode);
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        const viewer::PressResult pr = it.onPress(fx.model, {50.0f, 400.0f}, 2, false, false);
        RIN_CHECK_MSG((pr.kind == viewer::PressResult::Kind::ContextMenu && !pr.graphChanged &&
                       pr.message.empty()),
                      "right press on blank asks for context menu");
        RIN_CHECK_MSG(fx.model.nodes.size() == 1, "graph untouched");
        RIN_CHECK_MSG(it.mode == viewer::InteractionMode::None,
                      "no drag mode from right press");
    }
}

// --- 12. 交互状态机：平移与 deleteSelection ---

void testInteractionPanAndDeleteSelection() {
    // 中键按下 → Pan；首事件只初始化基准不平移，其后按屏幕增量平移。
    {
        GraphFixture fx;
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        const viewer::PressResult pr = it.onPress(fx.model, {100.0f, 100.0f}, 1, false, false);
        RIN_CHECK_MSG(pr.kind == viewer::PressResult::Kind::None, "middle press reports nothing");
        RIN_CHECK_MSG(it.mode == viewer::InteractionMode::Pan, "middle press enters Pan");
        it.onDrag(fx.model, view, {120.0f, 110.0f}, view.toCanvas({120.0f, 110.0f}));
        RIN_CHECK_MSG((view.pan.x == 0.0f && view.pan.y == 0.0f),
                      "first drag event only initializes the base");
        it.onDrag(fx.model, view, {150.0f, 140.0f}, view.toCanvas({150.0f, 140.0f}));
        RIN_CHECK_MSG((view.pan.x == 30.0f && view.pan.y == 30.0f),
                      "second drag translates view");
        it.onDrag(fx.model, view, {140.0f, 130.0f}, view.toCanvas({140.0f, 130.0f}));
        RIN_CHECK_MSG((view.pan.x == 20.0f && view.pan.y == 20.0f), "further drags accumulate");
        releaseIgnore(it, fx.model, {140.0f, 130.0f});
        RIN_CHECK_MSG(it.mode == viewer::InteractionMode::None, "release ends Pan");
    }

    // deleteSelection：优先删选中连线；否则删选中节点集；空选中返回空消息。
    {
        GraphFixture fx;
        RIN_CHECK(make(fx.model, "a", 200.0f, 100.0f) != rin::kInvalidNode);
        RIN_CHECK(make(fx.model, "b", 500.0f, 100.0f) != rin::kInvalidNode);
        RIN_CHECK((fx.model.connect({1, rin::PortDirection::Output, 0},
                                    {2, rin::PortDirection::Input, 0})
                       .ok));
        viewer::CanvasInteraction it;
        // 连线优先：仅置连线选中（selectNode 会清除连线选中，故不选节点）。
        fx.model.selectedConnection = fx.model.connections.front();
        const std::string m1 = it.deleteSelection(fx.model);
        RIN_CHECK_MSG(m1 == "connection removed", "wire selection deleted first");
        RIN_CHECK_MSG((fx.model.connections.empty() && fx.model.nodes.size() == 2),
                      "only the wire removed");
        RIN_CHECK_MSG(!fx.model.selectedConnection.has_value(),
                      "wire selection consumed by deleteSelection");
        fx.model.selectNode(1, false);
        fx.model.selectNode(2, true);
        const std::string m2 = it.deleteSelection(fx.model);
        RIN_CHECK_MSG(m2 == "node removed", "node selection deleted next");
        RIN_CHECK_MSG(fx.model.nodes.empty(), "nodes removed with their edges");
        const std::string m3 = it.deleteSelection(fx.model);
        RIN_CHECK_MSG(m3.empty(), "empty selection yields empty message");
    }
}

// --- 13. 调色板模型 ---

void testPalette() {
    const rin::NodeCatalog catalog = makePaletteCatalog();
    RIN_CHECK_MSG(catalog.valid(), "palette catalog is valid");

    // 空 filter：全部分组，组序 = 目录首次出现序，组内保持目录顺序。
    {
        const std::vector<viewer::PaletteGroup> groups = viewer::paletteGroups(catalog, "");
        RIN_CHECK_MSG(groups.size() == 6, "all groups present for empty filter");
        if (groups.size() == 6) {
            RIN_CHECK_MSG((groups[0].title == "Source" && groups[1].title == "Geometry" &&
                           groups[2].title == "Filter" && groups[3].title == "Frequency" &&
                           groups[4].title == "Histogram" && groups[5].title == "Other"),
                          "group order follows first appearance");
            RIN_CHECK_MSG((groups[0].items.size() == 1 &&
                           groups[0].items[0]->typeId == "source"),
                          "Source group contents");
            RIN_CHECK_MSG((groups[1].items.size() == 2 &&
                           groups[1].items[0]->typeId == "crop" &&
                           groups[1].items[1]->typeId == "downscale"),
                          "Geometry group keeps catalog order");
            RIN_CHECK_MSG((groups[2].items.size() == 3 &&
                           groups[2].items[0]->typeId == "grayify" &&
                           groups[2].items[1]->typeId == "gaussian_blur" &&
                           groups[2].items[2]->typeId == "conv_kernel"),
                          "Filter group keeps catalog order");
            RIN_CHECK_MSG((groups[3].items.size() == 3 &&
                           groups[3].items[0]->typeId == "fft_lowpass" &&
                           groups[3].items[1]->typeId == "fft_highpass" &&
                           groups[3].items[2]->typeId == "fft_bandpass"),
                          "Frequency group keeps catalog order");
            RIN_CHECK_MSG((groups[4].items.size() == 1 &&
                           groups[4].items[0]->typeId == "hist_eq"),
                          "Histogram group contents");
            RIN_CHECK_MSG((groups[5].items.size() == 1 &&
                           groups[5].items[0]->typeId == "custom_tool"),
                          "unknown typeId lands in Other without loss");
        }
    }

    // filter 命中 typeId；无匹配项的分组整组隐藏。
    {
        const std::vector<viewer::PaletteGroup> groups = viewer::paletteGroups(catalog, "fft");
        RIN_CHECK_MSG((groups.size() == 1 && groups[0].title == "Frequency" &&
                       groups[0].items.size() == 3),
                      "typeId substring filter keeps only matching group");
    }

    // 大小写不敏感。
    {
        const std::vector<viewer::PaletteGroup> groups = viewer::paletteGroups(catalog, "BLUR");
        RIN_CHECK_MSG((groups.size() == 1 && groups[0].title == "Filter" &&
                       groups[0].items.size() == 1 &&
                       groups[0].items[0]->typeId == "gaussian_blur"),
                      "filter is case-insensitive on typeId");
    }

    // displayName 匹配（typeId 不含 needle）。
    {
        const std::vector<viewer::PaletteGroup> groups =
            viewer::paletteGroups(catalog, "EQUALI");
        RIN_CHECK_MSG((groups.size() == 1 && groups[0].title == "Histogram" &&
                       groups[0].items.size() == 1 &&
                       groups[0].items[0]->typeId == "hist_eq"),
                      "filter matches displayName case-insensitively");
    }

    // 组内收敛：crop 只剩 crop。
    {
        const std::vector<viewer::PaletteGroup> groups = viewer::paletteGroups(catalog, "crop");
        RIN_CHECK_MSG((groups.size() == 1 && groups[0].title == "Geometry" &&
                       groups[0].items.size() == 1 &&
                       groups[0].items[0]->typeId == "crop"),
                      "filter narrows within a group");
    }

    // 无匹配 → 空结果。
    RIN_CHECK_MSG(viewer::paletteGroups(catalog, "zzz").empty(), "no matches yield no groups");
}

// --- 14. IVA 缺陷回归探针 ---
//
// 前身为缺陷探针。2026-09-28 主循环修复三个缺陷后，本区按修复后语义改写为
// 回归守卫：任何一条失败即对应缺陷回归。
//   探针 1（原缺陷 B）：按下优先级 = 端口 > 节点 > 连线 > 空白，连线横穿
//           节点主体时按节点处理；
//   探针 2（原缺陷 C）：组拖动——多选后按下任一选中节点拖动整组随动；
//   探针 3（原缺陷 A）：普通点击选中节点后拖动，拖的必须是按下的节点
//           （use-after-rotate 回归守卫：onPress 曾在 selectNode→raise 旋转
//           nodes 后继续解引用 nodeAt 指针，拖动了未按下的节点）；
//   探针 4（原缺陷 A）：Shift+点按节点切换选中（规格项，多节点布局）。

void testDefectProbes() {
    // 探针 1：连线横穿节点主体时，左键按下节点主体按“节点”处理（修复后
    // 优先级：端口 > 节点 > 连线 > 空白，与 canvas_model.hpp 命中检测注释一致）。
    {
        GraphFixture fx;
        RIN_CHECK(make(fx.model, "a", 200.0f, 100.0f) != rin::kInvalidNode);  // 主体被穿越
        RIN_CHECK(make(fx.model, "a", 0.0f, 100.0f) != rin::kInvalidNode);    // 目标在左侧
        // 连线 (400,136) -> (0,136)：水平穿过 (200..400)x(100..154) 的节点主体。
        RIN_CHECK((fx.model.connect({1, rin::PortDirection::Output, 0},
                                    {2, rin::PortDirection::Input, 0})
                       .ok));
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        const viewer::PressResult pr = it.onPress(fx.model, {280.0f, 136.0f}, 0, false, false);
        RIN_CHECK_MSG(pr.kind == viewer::PressResult::Kind::NodePressed,
                      "probe: node body wins over the crossing wire (documented priority)");
        RIN_CHECK_MSG(it.mode == viewer::InteractionMode::NodeDrag,
                      "probe: crossed node body enters NodeDrag");
        if (pr.kind != viewer::PressResult::Kind::NodePressed) {
            std::printf("IVA-PROBE wire-over-node: actual kind=%d message='%s'\n",
                        static_cast<int>(pr.kind), pr.message.c_str());
        }
    }

    // 探针 2（组拖动正例）：多选后按下其中之一拖动，整组随动且相对位置不变。
    {
        GraphFixture fx;
        RIN_CHECK(make(fx.model, "a", 200.0f, 100.0f) != rin::kInvalidNode);
        RIN_CHECK(make(fx.model, "a", 500.0f, 100.0f) != rin::kInvalidNode);
        fx.model.selectNode(1, false);
        fx.model.selectNode(2, true);
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        const viewer::PressResult pr = it.onPress(fx.model, {220.0f, 110.0f}, 0, false, false);
        RIN_CHECK_MSG((pr.kind == viewer::PressResult::Kind::NodePressed &&
                       selectionEquals(fx.model, {1, 2})),
                      "probe: group press keeps selection");
        it.onDrag(fx.model, view, {300.0f, 120.0f}, {300.0f, 120.0f});
        const viewer::CanvasNode* n1 = fx.model.findNode(1);
        const viewer::CanvasNode* n2 = fx.model.findNode(2);
        RIN_CHECK((n1 != nullptr && n2 != nullptr));
        if (n1 != nullptr && n2 != nullptr) {
            RIN_CHECK_MSG((nearPoint(n1->position, {280.0f, 110.0f}) &&
                           nearPoint(n2->position, {580.0f, 110.0f})),
                          "probe: group drag moves both nodes together");
            RIN_CHECK_MSG((nearF(n2->position.x - n1->position.x, 300.0f) &&
                           nearF(n2->position.y - n1->position.y, 0.0f)),
                          "probe: relative positions preserved");
        }
    }

    // 探针 3（use-after-rotate 回归守卫）：普通点击选中节点 1 后拖动，必须拖
    // 节点 1；未按下的节点 2 不得移动。
    {
        GraphFixture fx;
        RIN_CHECK(make(fx.model, "a", 200.0f, 100.0f) != rin::kInvalidNode);  // id 1
        RIN_CHECK(make(fx.model, "a", 500.0f, 100.0f) != rin::kInvalidNode);  // id 2
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        const viewer::PressResult pr = it.onPress(fx.model, {250.0f, 120.0f}, 0, false, false);
        RIN_CHECK_MSG(pr.kind == viewer::PressResult::Kind::NodePressed,
                      "probe: node press reported");
        RIN_CHECK_MSG(it.draggedNode == 1,
                      "probe: pressed node (id 1) must be the dragged node");
        it.onDrag(fx.model, view, {300.0f, 130.0f}, {300.0f, 130.0f});
        const viewer::CanvasNode* n1 = fx.model.findNode(1);
        const viewer::CanvasNode* n2 = fx.model.findNode(2);
        RIN_CHECK((n1 != nullptr && n2 != nullptr));
        if (n1 != nullptr && n2 != nullptr) {
            const bool ok = nearPoint(n1->position, {250.0f, 110.0f}) &&
                            nearPoint(n2->position, {500.0f, 100.0f});
            if (!ok) {
                std::printf("IVA-PROBE press-drag-wrong-node: node1=(%.0f,%.0f) "
                            "node2=(%.0f,%.0f) draggedNode=%llu\n",
                            n1->position.x, n1->position.y, n2->position.x, n2->position.y,
                            (unsigned long long)it.draggedNode);
            }
            RIN_CHECK_MSG(ok, "probe: drag moves the pressed node only");
        }
    }

    // 探针 4（use-after-rotate 回归守卫，规格项“Shift+点按节点切换其选中”）：
    // Shift+点按未选中节点加入选中集，再次 Shift+点按移除。
    // 注意 createNode 会让新建节点独占选中，先 clearSelection 再验证加选。
    {
        GraphFixture fx;
        RIN_CHECK(make(fx.model, "a", 200.0f, 100.0f) != rin::kInvalidNode);  // id 1
        RIN_CHECK(make(fx.model, "a", 500.0f, 100.0f) != rin::kInvalidNode);  // id 2
        fx.model.clearSelection();
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        pressIgnore(it, fx.model, {250.0f, 120.0f}, 0, true);
        RIN_CHECK_MSG(selectionEquals(fx.model, {1}),
                      "probe: shift press adds the pressed node only");
        releaseIgnore(it, fx.model, {250.0f, 120.0f});
        pressIgnore(it, fx.model, {250.0f, 120.0f}, 0, true);
        RIN_CHECK_MSG(fx.model.selection.empty(),
                      "probe: shift press again toggles the node off");
    }
}

// --- 15. 连线带状轮廓（wireRibbon，M5-04 连线渲染修复） ---
//
// 被测契约（canvas_model.hpp wireRibbon）：以 sampleWire 贝塞尔采样为中心线，
// 逐点切线法向偏移 ±width/2，构造前向 + 逆向回程的带状封闭轮廓（修复 EUI
// polygon 填充语义下开放曲线点集渲染成"弦-曲线封闭区域"的真机缺陷）；
// 相邻采样重合（零切向）沿用上一法向；width 非有限或 ≤0、样本 <2 时原样返回
// 中心线。命中检测 wireAt 仍用 sampleWire 折线（testHitTesting 覆盖，不在本区）。
//
// 法向取向（实现取向，探针实测后锁定）：画布坐标 y 向下，左→右水平线切向
// (+1,0) 经 (-ty,tx) 旋转得 +法向 (0,+1)——leftSide 在屏幕下方、rightSide 在
// 上方。
//
// 已知发散（探针实测，报告在案、按契约断言）：from==to 时中心线因 sampleWire
// 控制点 40px 下限（"短连线也有可读弧度"）外凸约 10.4px，ribbon 点距 from 最
// 远 ~11.61px > width/2+1e-3；ribbon 对"围绕中心线等宽"的自身契约仍严格成立
// （距中心线最远 == width/2）。本区断言中心线有界性而非 from 有界性；零长
// 连线是否收敛控制点偏移由主循环决策（UI 拒绝自连，实际不可达）。

// 解析贝塞尔导数 B'(t)（独立推导：3u²(C0-P0) + 6ut(C1-C0) + 3t²(P1-C1)），
// 用作"法向随切线旋转"的独立判据（避免与实现的中心差分切向同源互证）。
[[nodiscard]] viewer::CanvasPoint bezierDerivative(const viewer::CanvasPoint& p0,
                                                   const viewer::CanvasPoint& c0,
                                                   const viewer::CanvasPoint& c1,
                                                   const viewer::CanvasPoint& p1,
                                                   const float t) {
    const float u = 1.0f - t;
    const float w1 = 3.0f * u * u;
    const float w2 = 6.0f * u * t;
    const float w3 = 3.0f * t * t;
    return {w1 * (c0.x - p0.x) + w2 * (c1.x - c0.x) + w3 * (p1.x - c1.x),
            w1 * (c0.y - p0.y) + w2 * (c1.y - c0.y) + w3 * (p1.y - c1.y)};
}

// 点到中心线折线最近距离（粗细一致回归守卫用）。
[[nodiscard]] float distanceToCenterline(const viewer::CanvasPoint& p,
                                         const std::vector<viewer::CanvasPoint>& center) {
    float best = std::numeric_limits<float>::max();
    for (std::size_t i = 1; i < center.size(); ++i) {
        best = std::min(best, viewer::pointSegmentDistance(p, center[i - 1], center[i]));
    }
    if (center.size() == 1) {
        best = viewer::canvasDistance(p, center[0]);
    }
    return best;
}

void testWireRibbon() {
    const viewer::CanvasPoint from{100.0f, 100.0f};
    const viewer::CanvasPoint to{500.0f, 200.0f};
    constexpr int kSegments = 24;
    constexpr float kWidth = viewer::kWireWidth;

    const std::vector<viewer::CanvasPoint> center = viewer::sampleWire(from, to, kSegments);
    const std::vector<viewer::CanvasPoint> ribbon =
        viewer::wireRibbon(from, to, kSegments, kWidth);

    // 形状：点数 = 2*(segments+1)；前 (segments+1) 点为 +法向侧，后段为
    // -法向侧逆序（ribbon[i] 与 ribbon[2n-1-i] 配对，i = 0..n-1）。
    RIN_CHECK_MSG(ribbon.size() == 2 * center.size(),
                  "ribbon: point count is 2*(segments+1)");
    RIN_CHECK_MSG(center.size() == static_cast<std::size_t>(kSegments) + 1,
                  "ribbon: centerline is the sampleWire polyline");

    // 配对几何：每对距离 == width（浮点容差）；中点 == 中心线采样点。
    {
        float maxPairErr = 0.0f;
        float maxMidErr = 0.0f;
        for (std::size_t i = 0; i < center.size(); ++i) {
            const viewer::CanvasPoint& left = ribbon[i];
            const viewer::CanvasPoint& right =
                ribbon[2 * center.size() - 1 - i];
            maxPairErr = std::max(maxPairErr,
                                  std::fabs(viewer::canvasDistance(left, right) - kWidth));
            const viewer::CanvasPoint mid{(left.x + right.x) * 0.5f,
                                          (left.y + right.y) * 0.5f};
            maxMidErr = std::max(maxMidErr, viewer::canvasDistance(mid, center[i]));
        }
        RIN_CHECK_MSG(maxPairErr <= 1e-3f,
                      "ribbon: every paired side distance equals width");
        RIN_CHECK_MSG(maxMidErr <= 1e-3f,
                      "ribbon: every pair midpoint lands on the centerline sample");
    }

    // 水平直连线（from.y==to.y）：+法向 (0,+1)（画布 y 向下 = 屏幕下方），
    // leftSide 在下、rightSide 在上，x 随中心采样不动。
    {
        const viewer::CanvasPoint hFrom{200.0f, 100.0f};
        const viewer::CanvasPoint hTo{500.0f, 100.0f};
        const std::vector<viewer::CanvasPoint> hCenter =
            viewer::sampleWire(hFrom, hTo, kSegments);
        const std::vector<viewer::CanvasPoint> hRibbon =
            viewer::wireRibbon(hFrom, hTo, kSegments, kWidth);
        RIN_CHECK_MSG(hRibbon.size() == 2 * hCenter.size(),
                      "ribbon horizontal: point count preserved");
        const float half = kWidth * 0.5f;
        for (std::size_t i = 0; i < hCenter.size(); ++i) {
            const viewer::CanvasPoint& left = hRibbon[i];
            const viewer::CanvasPoint& right =
                hRibbon[2 * hCenter.size() - 1 - i];
            const bool sides = nearF(left.y, hCenter[i].y + half, 1e-3f) &&
                               nearF(right.y, hCenter[i].y - half, 1e-3f) &&
                               nearF(left.x, hCenter[i].x, 1e-3f) &&
                               nearF(right.x, hCenter[i].x, 1e-3f);
            if (!sides) {
                std::printf("IVA-DIAG horizontal i=%zu left=(%.4f,%.4f) "
                            "right=(%.4f,%.4f) center=(%.4f,%.4f)\n",
                            i, left.x, left.y, right.x, right.y, hCenter[i].x,
                            hCenter[i].y);
            }
            RIN_CHECK_MSG(sides,
                          "ribbon horizontal: left below, right above, x unchanged");
        }
        // 端点精确值锁定取向（防实现取向翻转的回归守卫）。
        RIN_CHECK_MSG(nearF(hRibbon.front().y, 101.25f, 1e-4f) &&
                          nearF(hRibbon.front().x, 200.0f, 1e-4f),
                      "ribbon horizontal: first left-side point at y+width/2");
        RIN_CHECK_MSG(nearF(hRibbon.back().y, 98.75f, 1e-4f),
                      "ribbon horizontal: last right-side point at y-width/2");
    }

    // 曲线法向随切线旋转：配对方向 d = leftSide[i]-rightSide[i] 应垂直于该点
    // 解析切向 B'(t)（独立推导判据；|cos| 容差 0.01 覆盖中心差分与解析导数的
    // 离散偏差，探针实测 0.0019），且取向全程一致（cross(d, B') 恒负）。
    {
        const float offset = viewer::wireControlOffset(from, to);
        const viewer::CanvasPoint c0{from.x + offset, from.y};
        const viewer::CanvasPoint c1{to.x - offset, to.y};
        float maxAbsCos = 0.0f;
        float maxCross = std::numeric_limits<float>::lowest();
        for (std::size_t i = 0; i < center.size(); ++i) {
            const viewer::CanvasPoint& left = ribbon[i];
            const viewer::CanvasPoint& right =
                ribbon[2 * center.size() - 1 - i];
            const viewer::CanvasPoint d{left.x - right.x, left.y - right.y};
            const float t =
                static_cast<float>(i) / static_cast<float>(kSegments);
            const viewer::CanvasPoint der = bezierDerivative(from, c0, c1, to, t);
            const float dLen = viewer::canvasDistance({0.0f, 0.0f}, d);
            const float derLen = viewer::canvasDistance({0.0f, 0.0f}, der);
            if (dLen > 0.0f && derLen > 0.0f) {
                const float cosv =
                    std::fabs((d.x * der.x + d.y * der.y) / (dLen * derLen));
                if (i > 0 && i + 1 < center.size()) {
                    maxAbsCos = std::max(maxAbsCos, cosv);
                }
                maxCross = std::max(maxCross, d.x * der.y - d.y * der.x);
            }
        }
        RIN_CHECK_MSG(maxAbsCos <= 0.01f,
                      "ribbon: pair direction is perpendicular to the analytic "
                      "tangent (|cos| <= 0.01 at interior samples)");
        RIN_CHECK_MSG(maxCross < 0.0f,
                      "ribbon: normal orientation is consistent along the curve "
                      "(cross(d, tangent) stays negative)");
    }

    // 回归守卫（粗细一致的几何含义）：ribbon 任意点到中心线折线最近距离
    // <= width/2 + 1e-3（曲线与水平两形态）。
    {
        float maxCurveDist = 0.0f;
        for (const viewer::CanvasPoint& p : ribbon) {
            maxCurveDist = std::max(maxCurveDist, distanceToCenterline(p, center));
        }
        RIN_CHECK_MSG(maxCurveDist <= kWidth * 0.5f + 1e-3f,
                      "ribbon: every point stays within width/2 of the centerline "
                      "(curve)");
        const viewer::CanvasPoint hFrom{200.0f, 100.0f};
        const viewer::CanvasPoint hTo{500.0f, 100.0f};
        const std::vector<viewer::CanvasPoint> hCenter =
            viewer::sampleWire(hFrom, hTo, kSegments);
        const std::vector<viewer::CanvasPoint> hRibbon =
            viewer::wireRibbon(hFrom, hTo, kSegments, kWidth);
        float maxHorizontalDist = 0.0f;
        for (const viewer::CanvasPoint& p : hRibbon) {
            maxHorizontalDist =
                std::max(maxHorizontalDist, distanceToCenterline(p, hCenter));
        }
        RIN_CHECK_MSG(maxHorizontalDist <= kWidth * 0.5f + 1e-3f,
                      "ribbon: every point stays within width/2 of the centerline "
                      "(horizontal)");
    }

    // 退化：width=0 / 负 / NaN 原样返回中心线（与 sampleWire 逐点相等）。
    {
        const std::vector<viewer::CanvasPoint> rZero =
            viewer::wireRibbon(from, to, kSegments, 0.0f);
        const std::vector<viewer::CanvasPoint> rNegative =
            viewer::wireRibbon(from, to, kSegments, -2.0f);
        const std::vector<viewer::CanvasPoint> rNaN =
            viewer::wireRibbon(from, to, kSegments, std::nanf(""));
        RIN_CHECK_MSG(rZero == center, "ribbon: width=0 returns the centerline");
        RIN_CHECK_MSG(rNegative == center,
                      "ribbon: negative width returns the centerline");
        RIN_CHECK_MSG(rNaN == center, "ribbon: NaN width returns the centerline");
    }

    // 退化：segments=1 最小可用（4 点，两对覆盖首末中心采样）。
    {
        const std::vector<viewer::CanvasPoint> minimal =
            viewer::wireRibbon(from, to, 1, kWidth);
        const std::vector<viewer::CanvasPoint> minimalCenter =
            viewer::sampleWire(from, to, 1);
        RIN_CHECK_MSG(minimal.size() == 4,
                      "ribbon: segments=1 yields 2*(1+1) points");
        for (std::size_t i = 0; i < minimalCenter.size(); ++i) {
            const viewer::CanvasPoint& left = minimal[i];
            const viewer::CanvasPoint& right =
                minimal[2 * minimalCenter.size() - 1 - i];
            RIN_CHECK_MSG(nearF(viewer::canvasDistance(left, right), kWidth, 1e-3f),
                          "ribbon: segments=1 pair width");
            const viewer::CanvasPoint mid{(left.x + right.x) * 0.5f,
                                          (left.y + right.y) * 0.5f};
            RIN_CHECK_MSG(nearPoint(mid, minimalCenter[i], 1e-3f),
                          "ribbon: segments=1 pair midpoint on the center sample");
        }
    }

    // 退化：from==to 不崩溃，形状与配对契约仍成立；所有点距"中心线"有界。
    // 距 from 的有界性见本区文件头"已知发散"——中心线本身因控制点 40px 下限
    // 外凸（sampleWire 既有几何，与命中检测同源），不在此断言。
    {
        const viewer::CanvasPoint same{100.0f, 100.0f};
        const std::vector<viewer::CanvasPoint> degenerate =
            viewer::wireRibbon(same, same, kSegments, kWidth);
        const std::vector<viewer::CanvasPoint> degenerateCenter =
            viewer::sampleWire(same, same, kSegments);
        RIN_CHECK_MSG(degenerate.size() == 2 * degenerateCenter.size(),
                      "ribbon degenerate: from==to keeps the point count");
        float maxCenterDist = 0.0f;
        float maxFromDist = 0.0f;
        for (const viewer::CanvasPoint& p : degenerate) {
            maxCenterDist =
                std::max(maxCenterDist, distanceToCenterline(p, degenerateCenter));
            maxFromDist = std::max(maxFromDist, viewer::canvasDistance(p, same));
        }
        RIN_CHECK_MSG(maxCenterDist <= kWidth * 0.5f + 1e-3f,
                      "ribbon degenerate: from==to stays within width/2 of the "
                      "centerline");
        // 信息性输出（非断言）：量化"距 from"发散，供报告与主循环决策。
        std::printf("ribbon degenerate from==to: max distance to from = %.4f "
                    "(requested bound width/2+1e-3 = %.4f; diverged as reported)\n",
                    maxFromDist, kWidth * 0.5f + 1e-3f);
    }
}

// --- 16. 监看器预览窗节点级高度几何（M11 验收追加 2026-10-07，独立验证） ---
//
// 被测契约（canvas_model.hpp + docs/design/ui_workspace_design.md §5.6 补记）：
//   - 常量：kMonitorPreviewHeight=140（默认）、kMonitorPreviewMinHeight=96、
//     kMonitorPreviewMaxHeight=640；
//   - CanvasNode::monitorPreviewHeight 节点级 UI 私有状态，默认 140；
//   - 几何族尾参 monitorPreviewHeight（默认参数保持旧调用兼容）：
//     viewer 节点高 = 26 + h + 6 + max(输入,输出,1)*20 + 8 = h + 60（1 进 1 出），
//     输入锚点 y = 26 + h + 6 + 10 = h + 42；
//   - 夹取属于写入（拖拽）与 compose 层：纯几何函数对越界值线性外推不夹取
//     （分层契约——本区按此断言，若未来在几何层内加夹取属行为变更应显式评审）；
//   - 非 viewer 节点几何与 monitorPreviewHeight 无关（同值异型对比）。
//
// 期望值全部由上述设计公式手推，不参考实现内部。

void testMonitorPreviewGeometry() {
    GraphFixture fx;
    const rin::NodeDescriptor* viewerD = rin::findNodeDescriptor(fx.catalog, "viewer");
    const rin::NodeDescriptor* a = rin::findNodeDescriptor(fx.catalog, "a");
    const rin::NodeDescriptor* c = rin::findNodeDescriptor(fx.catalog, "c");
    RIN_CHECK((viewerD != nullptr && a != nullptr && c != nullptr));
    RIN_CHECK(fx.catalog.valid());
    if (viewerD == nullptr || a == nullptr || c == nullptr) {
        return;
    }

    // 常量冻结（设计契约：默认 140、范围 [96,640]，默认落于开区间内）。
    RIN_CHECK_MSG(viewer::kMonitorPreviewHeight == 140.0f, "default preview height is 140");
    RIN_CHECK_MSG(viewer::kMonitorPreviewMinHeight == 96.0f, "min preview height is 96");
    RIN_CHECK_MSG(viewer::kMonitorPreviewMaxHeight == 640.0f, "max preview height is 640");
    RIN_CHECK(viewer::kMonitorPreviewMinHeight < viewer::kMonitorPreviewHeight);
    RIN_CHECK(viewer::kMonitorPreviewHeight < viewer::kMonitorPreviewMaxHeight);

    // CanvasNode 默认成员：createNode 路径初始化为 kMonitorPreviewHeight
    // （节点级状态默认值契约，viewer 与非 viewer 节点同默认）。
    {
        const rin::NodeId iv = make(fx.model, "viewer", 0.0f, 0.0f);
        const rin::NodeId ia = make(fx.model, "a", 0.0f, 0.0f);
        const viewer::CanvasNode* nv = fx.model.findNode(iv);
        const viewer::CanvasNode* na = fx.model.findNode(ia);
        RIN_CHECK((nv != nullptr && na != nullptr));
        if (nv != nullptr) {
            RIN_CHECK_MSG(nv->monitorPreviewHeight == viewer::kMonitorPreviewHeight,
                          "new viewer node starts at the default preview height");
        }
        if (na != nullptr) {
            RIN_CHECK_MSG(na->monitorPreviewHeight == viewer::kMonitorPreviewHeight,
                          "non-monitor nodes carry the default preview height field too");
        }
    }

    // portsOffsetY：viewer = 26 + h + 6；非 viewer 与 h 无关。
    RIN_CHECK_MSG(nearF(viewer::portsOffsetY(*viewerD, {}), 172.0f),
                  "portsOffsetY(viewer) default = 26+140+6");
    RIN_CHECK_MSG(nearF(viewer::portsOffsetY(*viewerD, {}, 96.0f), 128.0f),
                  "portsOffsetY(viewer, 96) = 128");
    RIN_CHECK_MSG(nearF(viewer::portsOffsetY(*viewerD, {}, 640.0f), 672.0f),
                  "portsOffsetY(viewer, 640) = 672");
    RIN_CHECK_MSG(nearF(viewer::portsOffsetY(*a, {}, 96.0f), 26.0f) &&
                      nearF(viewer::portsOffsetY(*a, {}, 640.0f), 26.0f),
                  "paramless non-monitor portsOffsetY ignores preview height");
    RIN_CHECK_MSG(nearF(viewer::portsOffsetY(*c, {}, 96.0f), 62.0f) &&
                      nearF(viewer::portsOffsetY(*c, {}, 640.0f), 62.0f),
                  "param node portsOffsetY ignores preview height");

    // nodeHeight(viewer) = h + 60：默认/最小/最大冻结值 + 默认参数路径等价。
    RIN_CHECK_MSG(nearF(viewer::nodeHeight(*viewerD), 200.0f),
                  "viewer height default = 26+140+6+20+8");
    RIN_CHECK_MSG(nearF(viewer::nodeHeight(*viewerD, {}), 200.0f),
                  "viewer height default (params defaulted)");
    RIN_CHECK_MSG(nearF(viewer::nodeHeight(*viewerD, {}, 96.0f), 156.0f),
                  "viewer height at min = 156");
    RIN_CHECK_MSG(nearF(viewer::nodeHeight(*viewerD, {}, 640.0f), 700.0f),
                  "viewer height at max = 700");
    RIN_CHECK(viewer::nodeHeight(*viewerD) == viewer::nodeHeight(*viewerD, {}));
    RIN_CHECK(viewer::nodeHeight(*viewerD, {}) ==
              viewer::nodeHeight(*viewerD, {}, viewer::kMonitorPreviewHeight));

    // 线性关系：高度差 == 预览高度差（斜率 1、截距 60 的设计公式）。
    {
        const float heights[] = {96.0f, 140.0f, 250.5f, 400.0f, 640.0f};
        for (const float h : heights) {
            RIN_CHECK_MSG(nearF(viewer::nodeHeight(*viewerD, {}, h), h + 60.0f),
                          "viewer nodeHeight is h+60 across the range");
        }
        RIN_CHECK_MSG(nearF(viewer::nodeHeight(*viewerD, {}, 640.0f) -
                                viewer::nodeHeight(*viewerD, {}, 96.0f),
                            544.0f),
                      "height difference equals preview height difference");
    }

    // 纯几何不夹取（夹取属写入/compose 层）：越界值线性外推。
    RIN_CHECK_MSG(nearF(viewer::nodeHeight(*viewerD, {}, 50.0f), 110.0f),
                  "below-min preview height is not clamped in pure geometry");
    RIN_CHECK_MSG(nearF(viewer::nodeHeight(*viewerD, {}, 2000.0f), 2060.0f),
                  "above-max preview height is not clamped in pure geometry");

    // 非 viewer 不受影响（同值异型对比：同 h=640 下 a=54、c=90、viewer=700）。
    RIN_CHECK_MSG(nearF(viewer::nodeHeight(*a, {}, 96.0f), 54.0f) &&
                      nearF(viewer::nodeHeight(*a, {}, 640.0f), 54.0f) &&
                      viewer::nodeHeight(*a, {}, 640.0f) == viewer::nodeHeight(*a),
                  "non-monitor height invariant across preview heights");
    RIN_CHECK_MSG(nearF(viewer::nodeHeight(*c, {}, 640.0f), 90.0f),
                  "param node height invariant across preview heights");
    RIN_CHECK_MSG(nearF(viewer::nodeHeight(*viewerD, {}, 640.0f) -
                            viewer::nodeHeight(*a, {}, 640.0f),
                        646.0f),
                  "same preview value, different type: viewer 700 vs a 54");

    // nodeBounds：宽恒 200，高随 h；默认参数路径等价。
    {
        const viewer::CanvasRect b = viewer::nodeBounds({100.0f, 50.0f}, *viewerD, {}, 400.0f);
        RIN_CHECK_MSG((b.x == 100.0f && b.y == 50.0f && nearF(b.width, 200.0f) &&
                       nearF(b.height, 460.0f)),
                      "viewer bounds at h=400: 200x460 at the position");
        const viewer::CanvasRect bDefault = viewer::nodeBounds({100.0f, 50.0f}, *viewerD);
        RIN_CHECK_MSG((nearF(bDefault.height, 200.0f) && nearF(bDefault.width, 200.0f)),
                      "viewer default bounds 200x200");
        const viewer::CanvasRect bMin = viewer::nodeBounds({0.0f, 0.0f}, *viewerD, {},
                                                           viewer::kMonitorPreviewMinHeight);
        RIN_CHECK_MSG(nearF(bMin.height, 156.0f), "viewer bounds at min: height 156");
        const viewer::CanvasRect bMax = viewer::nodeBounds({0.0f, 0.0f}, *viewerD, {},
                                                           viewer::kMonitorPreviewMaxHeight);
        RIN_CHECK_MSG(nearF(bMax.height, 700.0f), "viewer bounds at max: height 700");
    }

    // portPosition：viewer 输入/输出锚点 y = pos.y + h + 42（x 左缘/右缘）；
    // 默认参数路径与显式 140 等价；非 viewer 锚点不随 h 移动。
    {
        const float heights[] = {140.0f, 96.0f, 640.0f, 50.0f, 2000.0f};
        for (const float h : heights) {
            const viewer::CanvasPoint in0 = viewer::portPosition(
                {10.0f, 20.0f}, *viewerD, {}, {5, rin::PortDirection::Input, 0}, h);
            RIN_CHECK_MSG(nearPoint(in0, {10.0f, 20.0f + h + 42.0f}),
                          "viewer input anchor tracks the preview height");
            const viewer::CanvasPoint out0 = viewer::portPosition(
                {10.0f, 20.0f}, *viewerD, {}, {5, rin::PortDirection::Output, 0}, h);
            RIN_CHECK_MSG(nearPoint(out0, {210.0f, 20.0f + h + 42.0f}),
                          "viewer output anchor tracks the preview height");
        }
        const viewer::CanvasPoint inDefault = viewer::portPosition(
            {10.0f, 20.0f}, *viewerD, {}, {5, rin::PortDirection::Input, 0});
        RIN_CHECK_MSG(nearPoint(inDefault, {10.0f, 20.0f + 182.0f}),
                      "viewer default input anchor at +182");
        const viewer::CanvasPoint inExplicit = viewer::portPosition(
            {10.0f, 20.0f}, *viewerD, {}, {5, rin::PortDirection::Input, 0},
            viewer::kMonitorPreviewHeight);
        RIN_CHECK(inDefault == inExplicit);

        // 非 viewer：不同 h 下锚点逐位一致（同值异型对比）。
        const viewer::CanvasPoint aIn96 = viewer::portPosition(
            {10.0f, 20.0f}, *a, {}, {5, rin::PortDirection::Input, 0}, 96.0f);
        const viewer::CanvasPoint aIn640 = viewer::portPosition(
            {10.0f, 20.0f}, *a, {}, {5, rin::PortDirection::Input, 0}, 640.0f);
        RIN_CHECK_MSG(aIn96 == aIn640 && nearPoint(aIn96, {10.0f, 56.0f}),
                      "non-monitor input anchor fixed across preview heights");
        const viewer::CanvasPoint cIn640 = viewer::portPosition(
            {10.0f, 20.0f}, *c, {}, {5, rin::PortDirection::Input, 0}, 640.0f);
        RIN_CHECK_MSG(nearPoint(cIn640, {10.0f, 92.0f}),
                      "param node input anchor fixed across preview heights");
    }
}

// --- 17. 监看器预览高度的模型命中语义（nodeAt/nodesInRect/portAt/wireAt/
//         graphBounds/交互；M11 验收追加 2026-10-07，独立验证） ---
//
// 判据：CanvasGraphModel 全链命中调用携带 node.monitorPreviewHeight——
// 预览增高后节点包围盒覆盖更大区域、端口/连线锚点下移且旧位置不再命中；
// 两 viewer 节点各自独立高度互不影响。期望位置由设计公式手推。

void testMonitorPreviewHitSemantics() {
    // nodeAt：预览增高后包围盒覆盖更大区域（含底缘闭边界）；高度回落后
    // 增高区域不再命中。
    {
        GraphFixture fx;
        const rin::NodeId iv = make(fx.model, "viewer", 100.0f, 50.0f);
        viewer::CanvasNode* node = fx.model.findNode(iv);
        RIN_CHECK(node != nullptr);
        if (node == nullptr) {
            return;
        }
        node->monitorPreviewHeight = 400.0f;  // 高 460，底缘 y = 510。
        RIN_CHECK_MSG(fx.model.nodeAt({200.0f, 400.0f}) == node,
                      "nodeAt hits inside the enlarged preview area");
        RIN_CHECK_MSG(fx.model.nodeAt({200.0f, 510.0f}) == node,
                      "nodeAt includes the enlarged bottom edge");
        RIN_CHECK_MSG(fx.model.nodeAt({200.0f, 511.0f}) == nullptr,
                      "nodeAt misses just past the enlarged bottom edge");
        RIN_CHECK_MSG(fx.model.nodeAt({99.0f, 300.0f}) == nullptr,
                      "nodeAt misses left of the node");
        node->monitorPreviewHeight = viewer::kMonitorPreviewHeight;  // 高 200，底缘 250。
        RIN_CHECK_MSG(fx.model.nodeAt({200.0f, 400.0f}) == nullptr,
                      "enlarged area no longer hits after reset to default");
        RIN_CHECK_MSG(fx.model.nodeAt({200.0f, 250.0f}) == node,
                      "default bottom edge still inclusive");
        RIN_CHECK_MSG(fx.model.nodeAt({200.0f, 251.0f}) == nullptr,
                      "just past the default bottom edge misses");
    }

    // nodesInRect：框选命中随节点级高度（预览区内的矩形选中增高节点，
    // 同矩形在默认高度下落空）。
    {
        GraphFixture fx;
        const rin::NodeId iv = make(fx.model, "viewer", 0.0f, 0.0f);
        viewer::CanvasNode* node = fx.model.findNode(iv);
        RIN_CHECK(node != nullptr);
        if (node == nullptr) {
            return;
        }
        node->monitorPreviewHeight = 640.0f;  // bounds (0,0,200,700)。
        std::vector<rin::NodeId> hits = fx.model.nodesInRect({0.0f, 300.0f, 10.0f, 10.0f});
        RIN_CHECK_MSG(hits.size() == 1 && hits.front() == iv,
                      "marquee inside the enlarged preview region selects the node");
        node->monitorPreviewHeight = viewer::kMonitorPreviewHeight;  // bounds (0,0,200,200)。
        hits = fx.model.nodesInRect({0.0f, 300.0f, 10.0f, 10.0f});
        RIN_CHECK_MSG(hits.empty(),
                      "same marquee rect misses after shrinking to default height");
    }

    // portAt：viewer 输入锚点随预览高度下移（含命中半径边界），旧锚点不再命中；
    // 高度回落后锚点复原。
    {
        GraphFixture fx;
        const rin::NodeId iv = make(fx.model, "viewer", 0.0f, 0.0f);
        viewer::CanvasNode* node = fx.model.findNode(iv);
        RIN_CHECK(node != nullptr);
        if (node == nullptr) {
            return;
        }
        node->monitorPreviewHeight = 400.0f;  // 输入锚点 (0, 442)。
        const std::optional<rin::PortRef> atNew = fx.model.portAt({0.0f, 442.0f});
        RIN_CHECK_MSG((atNew.has_value() &&
                       *atNew == rin::PortRef{iv, rin::PortDirection::Input, 0}),
                      "input anchor hit-testable at the enlarged position");
        RIN_CHECK_MSG(fx.model.portAt({12.0f, 442.0f}).has_value(),
                      "hit radius boundary (12) at the enlarged anchor included");
        RIN_CHECK_MSG(!fx.model.portAt({13.0f, 442.0f}).has_value(),
                      "just outside (13) the enlarged anchor misses");
        RIN_CHECK_MSG(!fx.model.portAt({0.0f, 182.0f}).has_value(),
                      "the old default anchor position no longer hits");
        node->monitorPreviewHeight = viewer::kMonitorPreviewHeight;  // 锚点回到 (0,182)。
        const std::optional<rin::PortRef> atDefault = fx.model.portAt({0.0f, 182.0f});
        RIN_CHECK_MSG((atDefault.has_value() &&
                       *atDefault == rin::PortRef{iv, rin::PortDirection::Input, 0}),
                      "default anchor restored after resetting the height");
        RIN_CHECK_MSG(!fx.model.portAt({0.0f, 442.0f}).has_value(),
                      "enlarged anchor no longer hits after reset");
    }

    // wireAt：连线锚点随目标 viewer 的预览高度移动（水平直连线：源输出锚点与
    // viewer 输入锚点 y 对齐——源节点 y = h+6 时输出在 y=h+42）；旧锚点连线
    // 位置不再命中；高度改动即时反映（无缓存几何）。
    {
        GraphFixture fx;
        const rin::NodeId ia = make(fx.model, "a", 0.0f, 406.0f);  // out (200, 442)。
        const rin::NodeId iv = make(fx.model, "viewer", 400.0f, 0.0f);
        viewer::CanvasNode* monitor = fx.model.findNode(iv);
        RIN_CHECK((ia != rin::kInvalidNode && monitor != nullptr));
        if (monitor == nullptr) {
            return;
        }
        monitor->monitorPreviewHeight = 400.0f;  // in (400, 442)：水平直连线 y=442。
        RIN_CHECK((fx.model.connect({ia, rin::PortDirection::Output, 0},
                                    {iv, rin::PortDirection::Input, 0})
                       .ok));
        const std::optional<rin::Connection> onMoved =
            fx.model.wireAt({300.0f, 442.0f});
        RIN_CHECK_MSG((onMoved.has_value() && onMoved->from.node == ia &&
                       onMoved->to.node == iv),
                      "wire hit follows the lowered input anchor");
        RIN_CHECK_MSG(fx.model.wireAt({400.0f, 442.0f}).has_value(),
                      "wire endpoint at the enlarged anchor hit-testable");
        RIN_CHECK_MSG(!fx.model.wireAt({300.0f, 450.0f}).has_value(),
                      "8px below the moved wire misses");
        RIN_CHECK_MSG(fx.model.wireAt({300.0f, 448.5f}).has_value(),
                      "6.5px below the moved wire still within kWireHitDistance");
        RIN_CHECK_MSG(!fx.model.wireAt({300.0f, 449.5f}).has_value(),
                      "7.5px below the moved wire misses");
        RIN_CHECK_MSG(!fx.model.wireAt({300.0f, 182.0f}).has_value(),
                      "the old default-anchor wire line no longer hits");
        // 高度回落：锚点上移为弯线（a.out (200,442) → viewer.in (400,182)），
        // 直连线原位置不再命中，新输入锚点端点命中。
        monitor->monitorPreviewHeight = viewer::kMonitorPreviewHeight;
        RIN_CHECK_MSG(!fx.model.wireAt({300.0f, 442.0f}).has_value(),
                      "old straight-line position no longer on the wire");
        RIN_CHECK_MSG(fx.model.wireAt({400.0f, 182.0f}).has_value(),
                      "wire endpoint at the reset anchor hit-testable");
    }

    // graphBounds + 两 viewer 独立高度：联合包围盒由各节点自身高度决定，
    // 一节点的高度不影响另一节点的包围盒与命中。
    {
        GraphFixture fx;
        const rin::NodeId iv1 = make(fx.model, "viewer", 0.0f, 0.0f);
        const rin::NodeId iv2 = make(fx.model, "viewer", 500.0f, 0.0f);
        viewer::CanvasNode* n1 = fx.model.findNode(iv1);
        viewer::CanvasNode* n2 = fx.model.findNode(iv2);
        RIN_CHECK((n1 != nullptr && n2 != nullptr));
        if (n1 == nullptr || n2 == nullptr) {
            return;
        }
        n1->monitorPreviewHeight = 96.0f;   // 高 156。
        n2->monitorPreviewHeight = 640.0f;  // 高 700。
        const viewer::CanvasRect bounds = fx.model.graphBounds();
        RIN_CHECK_MSG((bounds.x == 0.0f && bounds.y == 0.0f && nearF(bounds.width, 700.0f) &&
                       nearF(bounds.height, 700.0f)),
                      "graphBounds union driven by per-node preview heights");
        RIN_CHECK_MSG(fx.model.nodeAt({100.0f, 150.0f}) == n1,
                      "small node hit within its own 156-high bounds");
        RIN_CHECK_MSG(fx.model.nodeAt({100.0f, 160.0f}) == nullptr,
                      "below the small node (outside the tall node's column) misses");
        RIN_CHECK_MSG(fx.model.nodeAt({600.0f, 600.0f}) == n2,
                      "tall node hit inside its own enlarged bounds");
        RIN_CHECK_MSG(fx.model.nodeAt({600.0f, 710.0f}) == nullptr,
                      "past the tall node's bottom edge misses");
        // 收缩大节点：联合包围盒随之收缩，小节点不受影响。
        n2->monitorPreviewHeight = 96.0f;
        const viewer::CanvasRect shrunk = fx.model.graphBounds();
        RIN_CHECK_MSG((nearF(shrunk.height, 156.0f) && nearF(shrunk.width, 700.0f)),
                      "graphBounds shrinks with the node preview height");
        RIN_CHECK_MSG(fx.model.nodeAt({600.0f, 600.0f}) == nullptr,
                      "tall node's old area no longer hits after shrink");
        RIN_CHECK_MSG(fx.model.nodeAt({100.0f, 150.0f}) == n1,
                      "small node unaffected by the other node's shrink");
    }

    // 交互状态机集成：增高后的预览区按下 = NodePressed；节点拖动只改位置，
    // 不重置预览高度。
    {
        GraphFixture fx;
        const rin::NodeId iv = make(fx.model, "viewer", 0.0f, 0.0f);
        viewer::CanvasNode* node = fx.model.findNode(iv);
        RIN_CHECK(node != nullptr);
        if (node == nullptr) {
            return;
        }
        node->monitorPreviewHeight = 400.0f;
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        const viewer::PressResult pr = it.onPress(fx.model, {100.0f, 300.0f}, 0, false, false);
        RIN_CHECK_MSG(pr.kind == viewer::PressResult::Kind::NodePressed,
                      "press inside the enlarged preview area presses the node");
        RIN_CHECK_MSG((it.mode == viewer::InteractionMode::NodeDrag && it.draggedNode == iv),
                      "drag anchors the enlarged monitor node");
        it.onDrag(fx.model, view, {120.0f, 320.0f}, {120.0f, 320.0f});
        RIN_CHECK_MSG(nearPoint(node->position, {20.0f, 20.0f}),
                      "node drag moves by the canvas delta");
        RIN_CHECK_MSG(node->monitorPreviewHeight == 400.0f,
                      "node drag preserves the node preview height");
        releaseIgnore(it, fx.model, {120.0f, 320.0f});
        RIN_CHECK_MSG(it.mode == viewer::InteractionMode::None, "release ends the drag");
    }
}

// --- 18. 画/命中锚点一致性（draw-hit consistency，cb8bd4c 修复回归守卫） ---
//
// 背景：bcc7229 落地时绘制层 5 处 portPosition/nodeHeight 调用漏传节点级
// monitorPreviewHeight（端口圆点、连线两端、拖线预览、问题面板定位取默认
// 140），与携带节点值的命中层（portAt/nodeAt/wireAt）几何分裂——独立验证
// 发现，cb8bd4c 修复。EUI compose 无法无头执行，本区以"绘制层逐字表达式"
// （node_canvas.hpp 同一调用形状：portPosition(node.position, *descriptor,
// node.params, port, node.monitorPreviewHeight)）计算锚点/高度，断言模型
// 命中层恰在该点命中。两层中任何一侧改用不同高度来源（默认值、compose 夹
// 取值）或漏传节点值，本区即失败。
//
// 覆盖高度含越界原始值（50/2000）：绘制层传 raw node.monitorPreviewHeight、
// 命中层同传 raw——一致性对越界写入同样成立（夹取属拖拽写入与 compose 呈现，
// 两层同源不经夹取）。

void testDrawHitAnchorConsistency() {
    const float heights[] = {96.0f, 140.0f, 400.0f, 640.0f, 50.0f, 2000.0f};
    for (const float h : heights) {
        GraphFixture fx;
        const rin::NodeDescriptor* viewerD = rin::findNodeDescriptor(fx.catalog, "viewer");
        const rin::NodeDescriptor* aD = rin::findNodeDescriptor(fx.catalog, "a");
        RIN_CHECK((viewerD != nullptr && aD != nullptr));
        if (viewerD == nullptr || aD == nullptr) {
            return;
        }
        const rin::NodeId ia = make(fx.model, "a", 0.0f, 0.0f);
        const rin::NodeId iv = make(fx.model, "viewer", 400.0f, 0.0f);
        viewer::CanvasNode* aNode = fx.model.findNode(ia);
        viewer::CanvasNode* vNode = fx.model.findNode(iv);
        RIN_CHECK((aNode != nullptr && vNode != nullptr));
        if (aNode == nullptr || vNode == nullptr) {
            return;
        }
        vNode->monitorPreviewHeight = h;
        RIN_CHECK((fx.model.connect({ia, rin::PortDirection::Output, 0},
                                    {iv, rin::PortDirection::Input, 0})
                       .ok));

        // 绘制层逐字表达式（composeCanvasNode 输入/输出端口圆点、
        // composeWorkflowCanvas 连线两端与拖线预览）。
        const viewer::CanvasPoint drawViewerIn =
            viewer::portPosition(vNode->position, *viewerD, vNode->params,
                                 {iv, rin::PortDirection::Input, 0},
                                 vNode->monitorPreviewHeight);
        const viewer::CanvasPoint drawViewerOut =
            viewer::portPosition(vNode->position, *viewerD, vNode->params,
                                 {iv, rin::PortDirection::Output, 0},
                                 vNode->monitorPreviewHeight);
        const viewer::CanvasPoint drawAOut =
            viewer::portPosition(aNode->position, *aD, aNode->params,
                                 {ia, rin::PortDirection::Output, 0},
                                 aNode->monitorPreviewHeight);

        // 命中层恰在绘制锚点命中：监看器输入（可连线目标）。
        const std::optional<rin::PortRef> hitIn = fx.model.portAt(drawViewerIn);
        RIN_CHECK_MSG((hitIn.has_value() && *hitIn == rin::PortRef{iv, rin::PortDirection::Input, 0}),
                      "model portAt hits exactly at the draw-layer viewer input anchor");
        // 非监看器输出（拖线发起端）。
        const std::optional<rin::PortRef> hitOut = fx.model.portAt(drawAOut);
        RIN_CHECK_MSG((hitOut.has_value() &&
                       *hitOut == rin::PortRef{ia, rin::PortDirection::Output, 0}),
                      "model portAt hits exactly at the draw-layer non-monitor output anchor");
        // 监看器隐藏输出端口：绘制层不绘制、命中层不命中（两层同跳过）。
        RIN_CHECK_MSG(!fx.model.portAt(drawViewerOut).has_value(),
                      "monitor output anchor stays non-hit-testable (hidden on both layers)");

        // 连线：绘制端点（from/to 两端逐字表达式）处 wireAt 命中同一条边。
        const std::optional<rin::Connection> wireAtTo = fx.model.wireAt(drawViewerIn);
        RIN_CHECK_MSG((wireAtTo.has_value() && wireAtTo->to.node == iv),
                      "wireAt hits the drawn wire at its to-end anchor");
        const std::optional<rin::Connection> wireAtFrom = fx.model.wireAt(drawAOut);
        RIN_CHECK_MSG((wireAtFrom.has_value() && wireAtFrom->from.node == ia),
                      "wireAt hits the drawn wire at its from-end anchor");

        // 节点底缘：绘制高度表达式（composeCanvasNode :1190 同形）与 nodeAt
        // 包围盒边界一致（内侧 0.5 命中、外侧 0.5 落空）。
        const float drawVHeight =
            viewer::nodeHeight(*viewerD, vNode->params, vNode->monitorPreviewHeight);
        RIN_CHECK_MSG(fx.model.nodeAt({vNode->position.x + viewer::kNodeWidth * 0.5f,
                                       vNode->position.y + drawVHeight - 0.5f}) == vNode,
                      "nodeAt hits just inside the drawn node bottom edge");
        RIN_CHECK_MSG(fx.model.nodeAt({vNode->position.x + viewer::kNodeWidth * 0.5f,
                                       vNode->position.y + drawVHeight + 0.5f}) == nullptr,
                      "nodeAt misses just past the drawn node bottom edge");

        // 端到端：以绘制锚点驱动交互状态机（拖线 from 绘制锚点 → 释放于
        // 监看器输入绘制锚点 → 建边）。
        if (h == 140.0f || h == 400.0f) {  // 代表性两态即可，避免重复建边断言。
            GraphFixture fx2;
            const rin::NodeId ia2 = make(fx2.model, "a", 0.0f, 0.0f);
            const rin::NodeId iv2 = make(fx2.model, "viewer", 400.0f, 0.0f);
            viewer::CanvasNode* a2 = fx2.model.findNode(ia2);
            viewer::CanvasNode* v2 = fx2.model.findNode(iv2);
            RIN_CHECK((a2 != nullptr && v2 != nullptr));
            if (a2 == nullptr || v2 == nullptr) {
                return;
            }
            v2->monitorPreviewHeight = h;
            const rin::NodeDescriptor* v2D = rin::findNodeDescriptor(fx2.catalog, "viewer");
            const rin::NodeDescriptor* a2D = rin::findNodeDescriptor(fx2.catalog, "a");
            RIN_CHECK((v2D != nullptr && a2D != nullptr));
            if (v2D == nullptr || a2D == nullptr) {
                return;
            }
            const viewer::CanvasPoint fromAnchor =
                viewer::portPosition(a2->position, *a2D, a2->params,
                                     {ia2, rin::PortDirection::Output, 0},
                                     a2->monitorPreviewHeight);
            const viewer::CanvasPoint toAnchor =
                viewer::portPosition(v2->position, *v2D, v2->params,
                                     {iv2, rin::PortDirection::Input, 0},
                                     v2->monitorPreviewHeight);
            viewer::CanvasView view;
            viewer::CanvasInteraction it;
            const viewer::PressResult pr = it.onPress(fx2.model, fromAnchor, 0, false, false);
            RIN_CHECK_MSG((pr.kind == viewer::PressResult::Kind::PortPressed && it.connecting()),
                          "press at the drawn output anchor starts a wire drag");
            const viewer::ReleaseResult rr = it.onRelease(fx2.model, toAnchor, false);
            RIN_CHECK_MSG((rr.graphChanged && fx2.model.connections.size() == 1 &&
                           fx2.model.connections.front().to.node == iv2),
                          "release at the drawn monitor input anchor connects");
        }
    }
}

// --- 19. 监看器节点级宽度几何与双向独立缩放（28685c3，独立验证） ---
//
// 被测契约（canvas_model.hpp + 设计文档 §5.6 / 计划 2026-10-07 晚第二段）：
//   - 常量：kMonitorMinWidth=160、kMonitorMaxWidth=800；CanvasNode::
//     monitorWidth 默认 kNodeWidth（200，落于区间内），"仅 viewer 生效"由
//     compose 层强制（非监看器恒画 kNodeWidth）；
//   - nodeBounds/portPosition 增宽度尾参（默认 kNodeWidth 兼容旧调用）：
//     viewer bounds = {pos, w, h+60}；输出锚点 x = pos.x + w，输入锚点 x =
//     pos.x（左缘与宽无关）；纵向几何（portsOffsetY/nodeHeight）无宽度参数；
//   - 宽高双向独立：改宽不动高、改高不动宽；
//   - 纯几何不夹取（夹取属拖拽写入/compose 呈现层，同高度分层契约）；
//   - 画/命中同源：端口/连线/命中层同传 raw node.monitorWidth（区别于节点
//     本体宽度的 compose 夹取——越界写入时本体卡片宽被夹到 800 而端口/
//     命中仍用 raw，此不对称已在验证报告登记，本区对端口/命中一致性按
//     raw 断言、对本体一致性仅限夹取==raw 的区间内值）。
//
// 期望值全部由上述设计公式手推。

void testMonitorWidthGeometry() {
    GraphFixture fx;
    const rin::NodeDescriptor* viewerD = rin::findNodeDescriptor(fx.catalog, "viewer");
    const rin::NodeDescriptor* a = rin::findNodeDescriptor(fx.catalog, "a");
    RIN_CHECK((viewerD != nullptr && a != nullptr));
    if (viewerD == nullptr || a == nullptr) {
        return;
    }

    // 常量冻结：[160,800]，默认 kNodeWidth=200 落于区间内。
    RIN_CHECK_MSG(viewer::kMonitorMinWidth == 160.0f, "min monitor width is 160");
    RIN_CHECK_MSG(viewer::kMonitorMaxWidth == 800.0f, "max monitor width is 800");
    RIN_CHECK(viewer::kMonitorMinWidth < viewer::kNodeWidth &&
              viewer::kNodeWidth < viewer::kMonitorMaxWidth);

    // CanvasNode 默认成员：createNode 路径 monitorWidth = kNodeWidth
    // （viewer 与非 viewer 同默认，宽高两字段独立默认）。
    {
        const rin::NodeId iv = make(fx.model, "viewer", 0.0f, 0.0f);
        const rin::NodeId ia = make(fx.model, "a", 0.0f, 0.0f);
        const viewer::CanvasNode* nv = fx.model.findNode(iv);
        const viewer::CanvasNode* na = fx.model.findNode(ia);
        RIN_CHECK((nv != nullptr && na != nullptr));
        if (nv != nullptr) {
            RIN_CHECK_MSG(nv->monitorWidth == viewer::kNodeWidth,
                          "new viewer node starts at the default node width");
        }
        if (na != nullptr) {
            RIN_CHECK_MSG(na->monitorWidth == viewer::kNodeWidth,
                          "non-monitor nodes carry the default width field too");
        }
    }

    // nodeBounds：宽随 w、高随 h，互不耦合；默认参数路径等价（旧 4 参调用）。
    {
        const float ws[] = {160.0f, 200.0f, 400.0f, 800.0f};
        for (const float w : ws) {
            const viewer::CanvasRect b = viewer::nodeBounds({10.0f, 20.0f}, *viewerD, {}, 140.0f, w);
            RIN_CHECK_MSG((b.x == 10.0f && b.y == 20.0f && nearF(b.width, w) &&
                           nearF(b.height, 200.0f)),
                          "viewer bounds follow the node width with height unchanged");
        }
        const viewer::CanvasRect wh = viewer::nodeBounds({0.0f, 0.0f}, *viewerD, {}, 96.0f, 800.0f);
        RIN_CHECK_MSG((nearF(wh.width, 800.0f) && nearF(wh.height, 156.0f)),
                      "width and height combine independently (800x156)");
        const viewer::CanvasRect bDefault = viewer::nodeBounds({0.0f, 0.0f}, *viewerD);
        const viewer::CanvasRect bExplicit =
            viewer::nodeBounds({0.0f, 0.0f}, *viewerD, {}, viewer::kMonitorPreviewHeight,
                               viewer::kNodeWidth);
        RIN_CHECK((bDefault.width == bExplicit.width && bDefault.height == bExplicit.height));
        // 高度不随宽变化：nodeHeight 无宽度维度，bounds.height 对任意 w 恒 h+60。
        for (const float w : ws) {
            RIN_CHECK_MSG(nearF(viewer::nodeBounds({0.0f, 0.0f}, *viewerD, {}, 400.0f, w).height,
                                460.0f),
                          "bounds height invariant across widths");
        }
        // 纯几何不夹取（写入/compose 层职责）：越界宽度线性取值。
        RIN_CHECK_MSG(nearF(viewer::nodeBounds({0.0f, 0.0f}, *viewerD, {}, 140.0f, 1000.0f).width,
                            1000.0f),
                      "above-max width not clamped in pure geometry");
        RIN_CHECK_MSG(nearF(viewer::nodeBounds({0.0f, 0.0f}, *viewerD, {}, 140.0f, 100.0f).width,
                            100.0f),
                      "below-min width not clamped in pure geometry");
    }

    // portPosition：输入锚点 x = pos.x（与宽无关）；输出锚点 x = pos.x + w；
    // 锚点 y 与宽无关（纵向几何无宽度维度）；默认参数路径等价。
    {
        const float ws[] = {160.0f, 200.0f, 400.0f, 800.0f, 1000.0f};
        for (const float w : ws) {
            const viewer::CanvasPoint in0 = viewer::portPosition(
                {10.0f, 20.0f}, *viewerD, {}, {5, rin::PortDirection::Input, 0}, 140.0f, w);
            RIN_CHECK_MSG(nearPoint(in0, {10.0f, 202.0f}),
                          "input anchor pinned to the left edge regardless of width");
            const viewer::CanvasPoint out0 = viewer::portPosition(
                {10.0f, 20.0f}, *viewerD, {}, {5, rin::PortDirection::Output, 0}, 140.0f, w);
            RIN_CHECK_MSG(nearPoint(out0, {10.0f + w, 202.0f}),
                          "output anchor x follows the node width");
        }
        const viewer::CanvasPoint outDefault = viewer::portPosition(
            {10.0f, 20.0f}, *viewerD, {}, {5, rin::PortDirection::Output, 0});
        const viewer::CanvasPoint outExplicit = viewer::portPosition(
            {10.0f, 20.0f}, *viewerD, {}, {5, rin::PortDirection::Output, 0},
            viewer::kMonitorPreviewHeight, viewer::kNodeWidth);
        RIN_CHECK(outDefault == outExplicit);
        RIN_CHECK_MSG(nearPoint(outDefault, {210.0f, 202.0f}),
                      "default output anchor at kNodeWidth right edge");
    }

    // 见证（HEAD 现状，非契约）：纯几何 nodeBounds/portPosition 对宽度不做
    // isMonitorNode 门控——非监看器描述符传入宽也生效（"仅 viewer 生效"只在
    // compose 层强制）。默认 kNodeWidth 下两无差异；此不对称已在验证报告
    // 登记，若任一层改变本断言失败以强制同步。
    {
        const viewer::CanvasRect wideA = viewer::nodeBounds({0.0f, 0.0f}, *a, {}, 140.0f, 800.0f);
        RIN_CHECK_MSG(nearF(wideA.width, 800.0f),
                      "witness: pure geometry applies width to non-monitor descriptors too");
        const viewer::CanvasPoint outA = viewer::portPosition(
            {0.0f, 0.0f}, *a, {}, {5, rin::PortDirection::Output, 0}, 140.0f, 800.0f);
        RIN_CHECK_MSG(nearPoint(outA, {800.0f, 36.0f}),
                      "witness: non-monitor output anchor moves with explicit width arg");
    }
}

// --- 20. 监看器宽度的模型命中语义与画/命中一致性（28685c3，独立验证） ---

void testMonitorWidthHitSemantics() {
    // nodeAt/nodesInRect：加宽后包围盒横向覆盖更大区域；回落默认后落空。
    {
        GraphFixture fx;
        const rin::NodeId iv = make(fx.model, "viewer", 100.0f, 50.0f);
        viewer::CanvasNode* node = fx.model.findNode(iv);
        RIN_CHECK(node != nullptr);
        if (node == nullptr) {
            return;
        }
        node->monitorWidth = 800.0f;  // bounds (100,50,800,200)，右缘 x=900。
        RIN_CHECK_MSG(fx.model.nodeAt({700.0f, 100.0f}) == node,
                      "nodeAt hits inside the widened area");
        RIN_CHECK_MSG(fx.model.nodeAt({900.0f, 100.0f}) == node,
                      "nodeAt includes the widened right edge");
        RIN_CHECK_MSG(fx.model.nodeAt({901.0f, 100.0f}) == nullptr,
                      "nodeAt misses just past the widened right edge");
        std::vector<rin::NodeId> hits = fx.model.nodesInRect({300.0f, 60.0f, 10.0f, 10.0f});
        RIN_CHECK_MSG(hits.size() == 1 && hits.front() == iv,
                      "marquee inside the widened region selects the node");
        node->monitorWidth = viewer::kNodeWidth;  // bounds (100,50,200,200)。
        RIN_CHECK_MSG(fx.model.nodeAt({700.0f, 100.0f}) == nullptr,
                      "widened area no longer hits after reset");
        RIN_CHECK_MSG(fx.model.nodesInRect({300.0f, 60.0f, 10.0f, 10.0f}).empty(),
                      "same rect misses after shrinking to default width");
    }

    // graphBounds + 两 viewer 宽高各自独立：联合包围盒由各节点自身宽高决定。
    {
        GraphFixture fx;
        const rin::NodeId iv1 = make(fx.model, "viewer", 0.0f, 0.0f);
        const rin::NodeId iv2 = make(fx.model, "viewer", 1000.0f, 0.0f);
        viewer::CanvasNode* n1 = fx.model.findNode(iv1);
        viewer::CanvasNode* n2 = fx.model.findNode(iv2);
        RIN_CHECK((n1 != nullptr && n2 != nullptr));
        if (n1 == nullptr || n2 == nullptr) {
            return;
        }
        n1->monitorWidth = 800.0f;   // bounds (0,0,800,200)。
        n2->monitorWidth = 160.0f;   // bounds (1000,0,160,200)。
        n2->monitorPreviewHeight = 640.0f;  // bounds (1000,0,160,700)。
        const viewer::CanvasRect bounds = fx.model.graphBounds();
        RIN_CHECK_MSG((nearF(bounds.x, 0.0f) && nearF(bounds.y, 0.0f) &&
                       nearF(bounds.width, 1160.0f) && nearF(bounds.height, 700.0f)),
                      "graphBounds union driven by per-node width and height");
        RIN_CHECK_MSG(fx.model.nodeAt({700.0f, 100.0f}) == n1,
                      "wide node hit inside its own widened bounds");
        RIN_CHECK_MSG(fx.model.nodeAt({1100.0f, 600.0f}) == n2,
                      "tall node hit inside its own height");
        RIN_CHECK_MSG(fx.model.nodeAt({700.0f, 300.0f}) == nullptr,
                      "wide-but-short node misses below its height");
        // 改宽不改高、改高不改宽（双向独立，模型层可观察）。
        n1->monitorWidth = 160.0f;
        RIN_CHECK_MSG(fx.model.nodeAt({700.0f, 100.0f}) == nullptr,
                      "width reset does not change height hit region");
        RIN_CHECK_MSG(fx.model.nodeAt({100.0f, 150.0f}) == n1,
                      "n1 still hit within its default-height bounds");
    }

    // 画/命中一致性（宽度维度，28685c3 同源性回归守卫）：以绘制层逐字表达式
    // （node_canvas.hpp :1373/:1388/:1554/:1557/:1587 同形：portPosition(...,
    // node.monitorPreviewHeight, node.monitorWidth)）计算锚点/包围盒，断言
    // 命中层恰在该点命中。区间内值（夹取==raw）另断言节点底缘/右缘与
    // nodeAt 边界一致；越界 raw 值只断言端口/连线层一致性（本体卡片宽在
    // compose 期夹取，与 raw 命中几何存在已登记的不对称）。
    {
        struct Size {
            float w;
            float h;
            bool inRange;
        };
        const Size sizes[] = {{160.0f, 96.0f, true},  {200.0f, 140.0f, true},
                              {800.0f, 640.0f, true}, {400.0f, 400.0f, true},
                              {1000.0f, 400.0f, false}, {100.0f, 50.0f, false}};
        for (const Size& sz : sizes) {
            GraphFixture fx;
            const rin::NodeDescriptor* viewerD = rin::findNodeDescriptor(fx.catalog, "viewer");
            const rin::NodeDescriptor* aD = rin::findNodeDescriptor(fx.catalog, "a");
            RIN_CHECK((viewerD != nullptr && aD != nullptr));
            if (viewerD == nullptr || aD == nullptr) {
                return;
            }
            const rin::NodeId ia = make(fx.model, "a", 0.0f, 0.0f);
            const rin::NodeId iv = make(fx.model, "viewer", 400.0f, 0.0f);
            viewer::CanvasNode* aNode = fx.model.findNode(ia);
            viewer::CanvasNode* vNode = fx.model.findNode(iv);
            RIN_CHECK((aNode != nullptr && vNode != nullptr));
            if (aNode == nullptr || vNode == nullptr) {
                return;
            }
            vNode->monitorWidth = sz.w;
            vNode->monitorPreviewHeight = sz.h;
            RIN_CHECK((fx.model.connect({ia, rin::PortDirection::Output, 0},
                                        {iv, rin::PortDirection::Input, 0})
                           .ok));

            const viewer::CanvasPoint drawViewerIn = viewer::portPosition(
                vNode->position, *viewerD, vNode->params, {iv, rin::PortDirection::Input, 0},
                vNode->monitorPreviewHeight, vNode->monitorWidth);
            const viewer::CanvasPoint drawAOut =
                viewer::portPosition(aNode->position, *aD, aNode->params,
                                     {ia, rin::PortDirection::Output, 0},
                                     aNode->monitorPreviewHeight, aNode->monitorWidth);

            // 命中层恰在绘制锚点命中（输入 x=左缘与宽无关；输出为非监看器
            // 默认宽）。监看器隐藏输出端口表达式锚点仍不可命中。
            const std::optional<rin::PortRef> hitIn = fx.model.portAt(drawViewerIn);
            RIN_CHECK_MSG((hitIn.has_value() &&
                           *hitIn == rin::PortRef{iv, rin::PortDirection::Input, 0}),
                          "portAt hits at the draw-layer input anchor for every size");
            const std::optional<rin::PortRef> hitAOut = fx.model.portAt(drawAOut);
            RIN_CHECK_MSG((hitAOut.has_value() &&
                           *hitAOut == rin::PortRef{ia, rin::PortDirection::Output, 0}),
                          "portAt hits at the draw-layer non-monitor output anchor");
            const viewer::CanvasPoint drawViewerOut = viewer::portPosition(
                vNode->position, *viewerD, vNode->params, {iv, rin::PortDirection::Output, 0},
                vNode->monitorPreviewHeight, vNode->monitorWidth);
            RIN_CHECK_MSG(!fx.model.portAt(drawViewerOut).has_value(),
                          "monitor output anchor stays non-hit-testable for every size");

            // 连线两端（绘制逐字表达式）处 wireAt 命中同一条边。
            const std::optional<rin::Connection> wireAtTo = fx.model.wireAt(drawViewerIn);
            RIN_CHECK_MSG((wireAtTo.has_value() && wireAtTo->to.node == iv),
                          "wireAt hits the drawn wire at its to-end anchor");
            const std::optional<rin::Connection> wireAtFrom = fx.model.wireAt(drawAOut);
            RIN_CHECK_MSG((wireAtFrom.has_value() && wireAtFrom->from.node == ia),
                          "wireAt hits the drawn wire at its from-end anchor");

            // 区间内值：本体包围盒边界（绘制表达式 nodeBounds 同形，夹取==raw）
            // 与 nodeAt 一致（内侧 0.5 命中、外侧 0.5 落空，右缘/底缘）。
            if (sz.inRange) {
                const viewer::CanvasRect drawBounds = viewer::nodeBounds(
                    vNode->position, *viewerD, vNode->params, vNode->monitorPreviewHeight,
                    vNode->monitorWidth);
                RIN_CHECK_MSG(fx.model.nodeAt({drawBounds.x + drawBounds.width - 0.5f,
                                               drawBounds.y + drawBounds.height * 0.5f}) == vNode,
                              "nodeAt hits just inside the drawn right edge");
                RIN_CHECK_MSG(fx.model.nodeAt({drawBounds.x + drawBounds.width + 0.5f,
                                               drawBounds.y + drawBounds.height * 0.5f}) == nullptr,
                              "nodeAt misses just past the drawn right edge");
                RIN_CHECK_MSG(fx.model.nodeAt({drawBounds.x + drawBounds.width * 0.5f,
                                               drawBounds.y + drawBounds.height - 0.5f}) == vNode,
                              "nodeAt hits just inside the drawn bottom edge");
                RIN_CHECK_MSG(fx.model.nodeAt({drawBounds.x + drawBounds.width * 0.5f,
                                               drawBounds.y + drawBounds.height + 0.5f}) == nullptr,
                              "nodeAt misses just past the drawn bottom edge");
            }
        }
    }

    // 交互集成：加宽预览区按下 = NodePressed；拖动只改 position，宽高两字段
    // 均保持（双向状态互不扰动）。
    {
        GraphFixture fx;
        const rin::NodeId iv = make(fx.model, "viewer", 0.0f, 0.0f);
        viewer::CanvasNode* node = fx.model.findNode(iv);
        RIN_CHECK(node != nullptr);
        if (node == nullptr) {
            return;
        }
        node->monitorWidth = 800.0f;
        node->monitorPreviewHeight = 400.0f;
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        const viewer::PressResult pr = it.onPress(fx.model, {700.0f, 300.0f}, 0, false, false);
        RIN_CHECK_MSG(pr.kind == viewer::PressResult::Kind::NodePressed,
                      "press inside the widened preview area presses the node");
        it.onDrag(fx.model, view, {710.0f, 310.0f}, {710.0f, 310.0f});
        RIN_CHECK_MSG(nearPoint(node->position, {10.0f, 10.0f}),
                      "drag moves the node by the canvas delta");
        RIN_CHECK_MSG((node->monitorWidth == 800.0f &&
                       node->monitorPreviewHeight == 400.0f),
                      "node drag preserves both width and height fields");
        releaseIgnore(it, fx.model, {710.0f, 310.0f});
        RIN_CHECK_MSG(it.mode == viewer::InteractionMode::None, "release ends the drag");
    }
}

}  // namespace

int main() {
    runSection("catalog_sanity", [] {
        // 目录构造自检（valid() 约束满足，测试前提成立）。
        RIN_CHECK(makeCanvasCatalog().valid());
        RIN_CHECK(makePaletteCatalog().valid());
    });
    runSection("view_transform", testViewTransform);
    runSection("geometry_basics", testGeometry);
    runSection("graph_model_create", testGraphCreate);
    runSection("graph_model_delete_move", testGraphDelete);
    runSection("graph_model_move", testGraphMove);
    runSection("graph_model_connect", testGraphConnect);
    runSection("graph_model_disconnect", testGraphDisconnect);
    runSection("selection_and_order", testSelectionAndOrder);
    runSection("hit_testing", testHitTesting);
    runSection("interaction_marquee", testInteractionMarquee);
    runSection("interaction_node_drag", testInteractionNodeDrag);
    runSection("interaction_connect_flow", testInteractionConnectFlow);
    runSection("interaction_wire_context", testInteractionWireAndContext);
    runSection("interaction_pan_delete_selection", testInteractionPanAndDeleteSelection);
    runSection("palette_groups", testPalette);
    runSection("iva_defect_probes", testDefectProbes);
    runSection("wire_ribbon", testWireRibbon);
    runSection("monitor_preview_geometry", testMonitorPreviewGeometry);
    runSection("monitor_preview_hit_semantics", testMonitorPreviewHitSemantics);
    runSection("draw_hit_anchor_consistency", testDrawHitAnchorConsistency);
    runSection("monitor_width_geometry", testMonitorWidthGeometry);
    runSection("monitor_width_hit_semantics", testMonitorWidthHitSemantics);
    return rin_test::exitStatus();
}
