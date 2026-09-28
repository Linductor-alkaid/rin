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
//   5. 调色板模型 paletteGroups（分组归类、即输即筛、未知类型归 Other）。
//
// 目录构造为本文件私有的小型 rin::NodeCatalog（只链 rin::core，不引入
// fake_engine）。测试壳为 tests/test_util.hpp 的 RIN_CHECK*（无第三方框架），
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
// 一个带范围声明的 Real 参数；monitor：零输入单输出；blank：零输入零输出
// （覆盖 nodeHeight 的 max(inputs, outputs, 1) 下限分支）。
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
        RIN_CHECK_MSG((bounds.x == 0.0f && bounds.y == 0.0f && nearF(bounds.width, 648.0f) &&
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

    // nodeHeight = 26 + max(inputs, outputs, 1)*20 + 8。
    RIN_CHECK_MSG(nearF(viewer::nodeHeight(*a), 54.0f), "a: 26+1*20+8");
    RIN_CHECK_MSG(nearF(viewer::nodeHeight(*b), 74.0f), "b: 26+2*20+8");
    RIN_CHECK_MSG(nearF(viewer::nodeHeight(*c), 54.0f), "c: 26+1*20+8");
    RIN_CHECK_MSG(nearF(viewer::nodeHeight(*monitor), 54.0f),
                  "monitor: 0 inputs falls back to 1 row");
    RIN_CHECK_MSG(nearF(viewer::nodeHeight(*blank), 54.0f), "blank: no ports still one row");

    // nodeBounds。
    const viewer::CanvasRect boundsA = viewer::nodeBounds({200.0f, 100.0f}, *a);
    RIN_CHECK_MSG((boundsA.x == 200.0f && boundsA.y == 100.0f && boundsA.width == 148.0f &&
                   boundsA.height == 54.0f),
                  "nodeBounds = position + kNodeWidth x nodeHeight");

    // portPosition：输入靠左缘、输出靠右缘，y 按 index*20 + 行中点。
    {
        const viewer::CanvasPoint in0 =
            viewer::portPosition({200.0f, 100.0f}, {99, rin::PortDirection::Input, 0});
        RIN_CHECK_MSG(nearPoint(in0, {200.0f, 136.0f}), "input port at left edge, row midpoint");
        const viewer::CanvasPoint out0 =
            viewer::portPosition({200.0f, 100.0f}, {99, rin::PortDirection::Output, 0});
        RIN_CHECK_MSG(nearPoint(out0, {348.0f, 136.0f}), "output port at right edge");
        const viewer::CanvasPoint in1 =
            viewer::portPosition({400.0f, 100.0f}, {99, rin::PortDirection::Input, 1});
        RIN_CHECK_MSG(nearPoint(in1, {400.0f, 156.0f}), "second input one row below");
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
    const rin::NodeId iw1 = make(fx.model, "a", 100.0f, 100.0f);   // out0 (248,136)
    const rin::NodeId iw2 = make(fx.model, "a", 100.0f, 107.0f);   // out0 (248,143)
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
            it.onPress(fx.model, {348.0f, 136.0f}, 0, false, false);  // a.out0
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
        pressIgnore(it, fx.model, {348.0f, 136.0f});
        it.onDrag(fx.model, view, {600.0f, 300.0f}, {600.0f, 300.0f});
        const viewer::ReleaseResult rr = it.onRelease(fx.model, {700.0f, 400.0f}, false);
        RIN_CHECK_MSG((!rr.graphChanged && rr.message.empty()),
                      "release on blank cancels the drag silently");
        RIN_CHECK_MSG(fx.model.connections.empty(), "no edge created on cancel");
    }

    // 松开在不兼容输入端口：connect 拒绝消息透传。
    {
        GraphFixture fx;
        RIN_CHECK(make(fx.model, "a", 200.0f, 100.0f) != rin::kInvalidNode);
        RIN_CHECK(make(fx.model, "c", 500.0f, 100.0f) != rin::kInvalidNode);
        viewer::CanvasView view;
        viewer::CanvasInteraction it;
        pressIgnore(it, fx.model, {348.0f, 136.0f});
        const viewer::ReleaseResult rr = it.onRelease(fx.model, {500.0f, 136.0f}, false);
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
        pressIgnore(it, fx.model, {348.0f, 136.0f});
        const viewer::ReleaseResult rr = it.onRelease(fx.model, {648.0f, 136.0f}, false);
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
        pressIgnore(it, fx.model, {348.0f, 136.0f});
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
        // 连线 (348,136) -> (0,136)：水平穿过 (200..348)x(100..154) 的节点主体。
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
    return rin_test::exitStatus();
}
