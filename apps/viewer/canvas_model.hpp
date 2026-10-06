#pragma once

// 节点编辑器画布模型（M5-03，[DEC-015]）：节点画布的平台无关纯逻辑层。
//
// 职责边界（DEC-015 决策 2：交互几何全部下沉为平台无关纯逻辑以保证可测试性）：
// - 视图变换：screen↔canvas 互转、以指针为锚点的缩放（上下限）、帧全图拟合
//   （ui_workspace_design.md §5.1 / §7）；
// - 画布图模型：节点位置（DEC-014 决策 5：UI 私有状态，不入契约）+ 选中集 +
//   到 `rin::WorkflowGraph` 的变换（创建/删除/连线/替换边），连线操作经
//   `validateWorkflowGraph` 预检（契约"UI 预检与引擎准入共用唯一判据"）；
// - 命中检测：节点（顶层优先）/ 端口（命中区大于视觉尺寸）/ 连线（贝塞尔采样
//   距离）/ 框选集合（§5.3 / §5.4 / §7）；
// - 交互状态机：按下/拖动/松开的模式迁移（平移、节点拖动、框选、连线拖拽），
//   输入为画布坐标点，输出为模型变更与反馈消息；
// - 调色板模型：`NodeCatalog` 分组与即输即筛过滤（§5.2 / §7）。
//
// 本头文件不包含 EUI-NEO 类型（纯逻辑单测对象，tests/test_node_canvas.cpp）；
// 视觉组装见 node_canvas.hpp。画布内交互决策（左键拖空白=框选、中键拖拽=平移，
// 与 §5.1/§5.4 条文的消解）已在设计文档 §5.1 补记，M5-03 验证记录同步。

#include "param_model.hpp"

#include <rin/workflow_types.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace viewer {

// --- 画布几何基元 ---

struct CanvasPoint {
    float x = 0.0f;
    float y = 0.0f;
};

[[nodiscard]] inline CanvasPoint operator+(const CanvasPoint& a, const CanvasPoint& b) {
    return {a.x + b.x, a.y + b.y};
}

[[nodiscard]] inline CanvasPoint operator-(const CanvasPoint& a, const CanvasPoint& b) {
    return {a.x - b.x, a.y - b.y};
}

inline CanvasPoint& operator+=(CanvasPoint& a, const CanvasPoint& b) {
    a.x += b.x;
    a.y += b.y;
    return a;
}

[[nodiscard]] inline bool operator==(const CanvasPoint& a, const CanvasPoint& b) {
    return a.x == b.x && a.y == b.y;
}

[[nodiscard]] inline float canvasDistance(const CanvasPoint& a, const CanvasPoint& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

struct CanvasRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    [[nodiscard]] bool contains(const CanvasPoint& p) const {
        return p.x >= x && p.y >= y && p.x <= x + width && p.y <= y + height;
    }

    [[nodiscard]] bool intersects(const CanvasRect& other) const {
        return x < other.x + other.width && other.x < x + width && y < other.y + other.height &&
               other.y < y + height;
    }
};

/// 由两个角点构造规范矩形（负方向拖拽归一）。
[[nodiscard]] inline CanvasRect canvasRectBetween(const CanvasPoint& a, const CanvasPoint& b) {
    const float minX = std::min(a.x, b.x);
    const float minY = std::min(a.y, b.y);
    return {minX, minY, std::abs(a.x - b.x), std::abs(a.y - b.y)};
}

// --- 视图变换（§5.1）：scale > 0；screen = canvas * scale + pan ---

inline constexpr float kZoomMin = 0.4f;
inline constexpr float kZoomMax = 2.5f;

struct CanvasView {
    float scale = 1.0f;
    CanvasPoint pan{0.0f, 0.0f};

    [[nodiscard]] CanvasPoint toCanvas(const CanvasPoint& screen) const {
        return {(screen.x - pan.x) / scale, (screen.y - pan.y) / scale};
    }

    [[nodiscard]] CanvasPoint toScreen(const CanvasPoint& canvas) const {
        return {canvas.x * scale + pan.x, canvas.y * scale + pan.y};
    }

    /// 以屏幕锚点为中心缩放（锚点的画布坐标在缩放前后保持不变，§5.1"以指针为
    /// 锚点"）；缩放因子必须有限且 > 0，结果被夹取到 [kZoomMin, kZoomMax]。
    void zoomAt(const CanvasPoint& screenAnchor, float factor) {
        if (!std::isfinite(factor) || factor <= 0.0f) {
            return;
        }
        const CanvasPoint anchor = toCanvas(screenAnchor);
        scale = std::clamp(scale * factor, kZoomMin, kZoomMax);
        pan = {screenAnchor.x - anchor.x * scale, screenAnchor.y - anchor.y * scale};
    }

    /// 帧全图（§5.1"全部节点落入视口"）：bounds 为空（无节点）时复位默认视图。
    void fit(const CanvasRect& bounds, const float viewportWidth, const float viewportHeight) {
        if (viewportWidth <= 0.0f || viewportHeight <= 0.0f) {
            return;
        }
        if (bounds.width <= 0.0f || bounds.height <= 0.0f) {
            scale = 1.0f;
            pan = {0.0f, 0.0f};
            return;
        }
        constexpr float kFitMargin = 32.0f;
        const float availW = std::max(1.0f, viewportWidth - kFitMargin * 2.0f);
        const float availH = std::max(1.0f, viewportHeight - kFitMargin * 2.0f);
        scale = std::clamp(std::min(availW / bounds.width, availH / bounds.height), kZoomMin,
                           kZoomMax);
        pan = {(viewportWidth - bounds.width * scale) * 0.5f - bounds.x * scale,
               (viewportHeight - bounds.height * scale) * 0.5f - bounds.y * scale};
    }
};

/// 视图定心（§5.4"点击定位节点"）：把画布点移到视口中心，比例不变。
inline void centerViewOn(CanvasView& view, const CanvasPoint& canvasCenter,
                         const float viewportWidth, const float viewportHeight) {
    view.pan = {viewportWidth * 0.5f - canvasCenter.x * view.scale,
                viewportHeight * 0.5f - canvasCenter.y * view.scale};
}

// --- 节点视觉度量（画布坐标；node_canvas.hpp 的绘制消费同一组常量） ---

/// 监看器 typeId（M11/DEC-021）：UI 私有呈现知识（与 paletteGroupFor 同类，
/// 不入契约）。监看器为 Any→Any 透传 sink：画布隐藏其输出端口，节点内嵌
/// 预览窗显示输入图像。
inline constexpr const char* kMonitorTypeId = "viewer";

[[nodiscard]] inline bool isMonitorNode(const std::string& typeId) {
    return typeId == kMonitorTypeId;
}

/// 相机源节点（M11/DEC-021：分辨率全局入口内嵌于源节点，DEC-017 决策 4）。
/// typeId 前缀 "source" 判定沿用 M5-04 右面板先例；参数区几何与内嵌绘制
/// 同源使用本谓词。
[[nodiscard]] inline bool isCameraSourceNode(const std::string& typeId) {
    return typeId.rfind("source", 0) == 0;
}

inline constexpr float kNodeWidth = 200.0f;
inline constexpr float kNodeHeaderHeight = 26.0f;
inline constexpr float kNodePortRowHeight = 20.0f;
inline constexpr float kNodeFooterHeight = 8.0f;
/// 监看器预览窗高度（画布坐标，宽 = 节点宽 − 2×kParamPadX）：
/// kMonitorPreviewHeight 为默认值，节点级可经右下角缩放手柄调整
/// （M11 验收反馈，CanvasNode::monitorPreviewHeight），范围
/// [kMonitorPreviewMinHeight, kMonitorPreviewMaxHeight]。
inline constexpr float kMonitorPreviewHeight = 140.0f;
inline constexpr float kMonitorPreviewMinHeight = 96.0f;
inline constexpr float kMonitorPreviewMaxHeight = 640.0f;
inline constexpr float kPortVisualRadius = 4.0f;
/// 端口命中区大于视觉尺寸（§5.3）。
inline constexpr float kPortHitRadius = 12.0f;
/// 连线命中距离（贝塞尔采样折线的点到线段距离阈值）。
inline constexpr float kWireHitDistance = 7.0f;
/// 连线视觉宽度（画布坐标；wireRibbon 带状轮廓用，小于命中距离）。
inline constexpr float kWireWidth = 2.5f;

// --- 内嵌参数区几何（M11/DEC-021 决策 3；绘制与命中共用同一组度量） ---

inline constexpr float kParamPadX = 8.0f;       ///< 参数区左右内边距
inline constexpr float kParamSectionGap = 6.0f;  ///< 参数区与端口区间距
inline constexpr float kParamRowGap = 4.0f;      ///< 参数行间距
inline constexpr float kParamLabelHeight = 14.0f;  ///< 标签+当前值行高
inline constexpr float kParamSliderHeight = 12.0f;  ///< 滑条行高
inline constexpr float kParamPillHeight = 18.0f;    ///< 枚举胶囊/开关行高
inline constexpr float kParamGridCellHeight = 20.0f;  ///< RealArray 单元行高
inline constexpr float kParamGridShapeHeight = 14.0f;  ///< RealArray 形状行高

/// RealArray 参数的当前网格行数（实例生效值 → RealArrayGrid 形状语义）；
/// 未赋值取声明默认值，非 RealArray 返回 1。
[[nodiscard]] inline std::size_t paramGridRows(const rin::NodeDescriptor& descriptor,
                                               const rin::ParamDescriptor& pd,
                                               const std::vector<rin::ParamAssignment>& params) {
    if (pd.kind != rin::ParamKind::RealArray) {
        return 1;
    }
    const rin::ParamValue* effective = effectiveParamValue(descriptor, params, pd.id);
    if (effective == nullptr) {
        return 1;
    }
    if (const auto* flat = std::get_if<std::vector<double>>(effective); flat != nullptr) {
        return RealArrayGrid::fromFlat(*flat).rows;
    }
    return 1;
}

/// 单参数行高（画布坐标）：Boolean=开关行、Integer/Real=标签行+滑条、
/// Enumeration=胶囊行、RealArray=形状行+网格行数（随实例形状）。
[[nodiscard]] inline float paramRowHeight(const rin::NodeDescriptor& descriptor,
                                          const rin::ParamDescriptor& pd,
                                          const std::vector<rin::ParamAssignment>& params) {
    switch (pd.kind) {
        case rin::ParamKind::Boolean:
            return kParamPillHeight;
        case rin::ParamKind::Integer:
        case rin::ParamKind::Real:
            return kParamLabelHeight + kParamSliderHeight;
        case rin::ParamKind::Enumeration:
            return kParamPillHeight;
        case rin::ParamKind::RealArray:
            return kParamGridShapeHeight +
                   static_cast<float>(paramGridRows(descriptor, pd, params)) * kParamGridCellHeight;
    }
    return kParamPillHeight;
}

/// 参数区总高（含首行前距与行间距；无参数节点为 0）。相机源节点计入分辨率
/// 入口行（M11：内嵌于源节点，与绘制同源）；监看器无参数区（预览窗替代，
/// portsOffsetY 分流）。
[[nodiscard]] inline float paramSectionHeight(
    const rin::NodeDescriptor& descriptor, const std::vector<rin::ParamAssignment>& params) {
    if (isMonitorNode(descriptor.typeId)) {
        return 0.0f;
    }
    const bool cameraRow = isCameraSourceNode(descriptor.typeId);
    if (!cameraRow && descriptor.params.empty()) {
        return 0.0f;  // 无参数且非源节点：无参数区（端口区紧跟标题）。
    }
    float height = kParamSectionGap;  // 标题行与首参数行的间距。
    if (cameraRow) {
        height += kParamPillHeight + kParamRowGap;  // Camera resolution 行。
    }
    for (const rin::ParamDescriptor& pd : descriptor.params) {
        height += paramRowHeight(descriptor, pd, params) + kParamRowGap;
    }
    return height;
}

/// 端口区在节点内的纵向偏移：标题 + 参数区（监看器为预览窗，高度节点级）。
[[nodiscard]] inline float portsOffsetY(const rin::NodeDescriptor& descriptor,
                                        const std::vector<rin::ParamAssignment>& params,
                                        const float monitorPreviewHeight = kMonitorPreviewHeight) {
    return kNodeHeaderHeight + (isMonitorNode(descriptor.typeId)
                                    ? monitorPreviewHeight + kParamSectionGap
                                    : paramSectionHeight(descriptor, params));
}

/// 节点高度 = 标题行 + 参数区（或预览窗）+ max(输入数, 输出数, 1) 个端口行 +
/// 底部留白（M11 起参数区/预览区参与，实例 RealArray 形状联动；监看器预览
/// 高度节点级可调，M11 验收反馈）。
[[nodiscard]] inline float nodeHeight(
    const rin::NodeDescriptor& descriptor,
    const std::vector<rin::ParamAssignment>& params = {},
    const float monitorPreviewHeight = kMonitorPreviewHeight) {
    const float rows = static_cast<float>(
        std::max<std::size_t>(1, std::max(descriptor.inputs.size(), descriptor.outputs.size())));
    return portsOffsetY(descriptor, params, monitorPreviewHeight) + rows * kNodePortRowHeight +
           kNodeFooterHeight;
}

[[nodiscard]] inline CanvasRect nodeBounds(
    const CanvasPoint& position, const rin::NodeDescriptor& descriptor,
    const std::vector<rin::ParamAssignment>& params = {},
    const float monitorPreviewHeight = kMonitorPreviewHeight) {
    return {position.x, position.y, kNodeWidth,
            nodeHeight(descriptor, params, monitorPreviewHeight)};
}

/// 端口锚点（§5.3"端口按签名序号纵向排布"；输入靠左缘、输出右缘；纵向偏移
/// 含参数区/预览窗，M11 起签名与几何解耦）。
[[nodiscard]] inline CanvasPoint portPosition(
    const CanvasPoint& position, const rin::NodeDescriptor& descriptor,
    const std::vector<rin::ParamAssignment>& params, const rin::PortRef& port,
    const float monitorPreviewHeight = kMonitorPreviewHeight) {
    const float rowY = portsOffsetY(descriptor, params, monitorPreviewHeight) +
                       static_cast<float>(port.index) * kNodePortRowHeight +
                       kNodePortRowHeight * 0.5f;
    return {port.direction == rin::PortDirection::Input ? position.x
                                                        : position.x + kNodeWidth,
            position.y + rowY};
}

/// 连线走线（§5.3）：输出右缘中点 → 输入左缘中点，三次贝塞尔按水平距离拟合。
[[nodiscard]] inline CanvasPoint bezierPoint(const CanvasPoint& p0, const CanvasPoint& c0,
                                             const CanvasPoint& c1, const CanvasPoint& p1,
                                             float t) {
    const float u = 1.0f - t;
    const float w0 = u * u * u;
    const float w1 = 3.0f * u * u * t;
    const float w2 = 3.0f * u * t * t;
    const float w3 = t * t * t;
    return {w0 * p0.x + w1 * c0.x + w2 * c1.x + w3 * p1.x,
            w0 * p0.y + w1 * c0.y + w2 * c1.y + w3 * p1.y};
}

/// 贝塞尔控制点：水平切线，长度随水平距离拟合（下限保证短连线也有可读弧度）。
[[nodiscard]] inline float wireControlOffset(const CanvasPoint& from, const CanvasPoint& to) {
    return std::max(40.0f, std::abs(to.x - from.x) * 0.5f);
}

[[nodiscard]] inline std::vector<CanvasPoint> sampleWire(const CanvasPoint& from,
                                                         const CanvasPoint& to, int segments) {
    const float offset = wireControlOffset(from, to);
    const CanvasPoint c0{from.x + offset, from.y};
    const CanvasPoint c1{to.x - offset, to.y};
    std::vector<CanvasPoint> points;
    points.reserve(static_cast<std::size_t>(std::max(1, segments)) + 1);
    for (int i = 0; i <= segments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segments);
        points.push_back(bezierPoint(from, c0, c1, to, t));
    }
    return points;
}

/// 连线带状轮廓（粗细一致的曲线）：`polygon` 原语为填充语义（点集三角形扇、
/// 末点直连首点），开放曲线点集会渲染成"弦线与曲线围成的封闭区域"而非线条
/// （M5-04 真机反馈实证）。走线以贝塞尔采样为中心线，逐点求切线法向并偏移
/// ±width/2，构造前向 + 逆向回程的带状封闭轮廓；与 M3 位姿视图 segmentToQuad
/// 的线段四边形同一先例。width 为画布坐标单位（随缩放与节点图形同比例）；
/// 相邻采样重合（零切向）时沿用上一法向，不放大为异常。
[[nodiscard]] inline std::vector<CanvasPoint> wireRibbon(const CanvasPoint& from,
                                                         const CanvasPoint& to,
                                                         const int segments,
                                                         const float width) {
    const std::vector<CanvasPoint> center = sampleWire(from, to, segments);
    if (center.size() < 2 || !(width > 0.0f) || !std::isfinite(width)) {
        return center;
    }
    const float half = width * 0.5f;
    const std::size_t n = center.size();
    std::vector<CanvasPoint> leftSide(n);
    std::vector<CanvasPoint> rightSide(n);
    CanvasPoint normal{0.0f, 1.0f};
    for (std::size_t i = 0; i < n; ++i) {
        const CanvasPoint& prev = center[i > 0 ? i - 1 : 0];
        const CanvasPoint& next = center[i + 1 < n ? i + 1 : n - 1];
        const CanvasPoint tangent = next - prev;
        const float length = std::sqrt(tangent.x * tangent.x + tangent.y * tangent.y);
        if (length > 1e-6f) {
            normal = {-tangent.y / length, tangent.x / length};
        }
        leftSide[i] = {center[i].x + normal.x * half, center[i].y + normal.y * half};
        rightSide[i] = {center[i].x - normal.x * half, center[i].y - normal.y * half};
    }
    std::vector<CanvasPoint> ribbon;
    ribbon.reserve(n * 2);
    ribbon.insert(ribbon.end(), leftSide.begin(), leftSide.end());
    ribbon.insert(ribbon.end(), rightSide.rbegin(), rightSide.rend());
    return ribbon;
}

/// 点到线段最短距离。
[[nodiscard]] inline float pointSegmentDistance(const CanvasPoint& p, const CanvasPoint& a,
                                                const CanvasPoint& b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float lengthSq = dx * dx + dy * dy;
    if (lengthSq <= 0.0f) {
        return canvasDistance(p, a);
    }
    const float t =
        std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / lengthSq, 0.0f, 1.0f);
    return canvasDistance(p, {a.x + t * dx, a.y + t * dy});
}

// --- 画布图模型：节点位置 + 选中集 + WorkflowGraph 变换 ---

/// 画布节点：类型实例 + UI 私有位置（DEC-014 决策 5）+ 已赋值参数（M5-04：
/// 参数面板提交的持久层，随图结构变更经 applyGraph 携带，避免图重建丢失参数）。
struct CanvasNode {
    rin::NodeId id = rin::kInvalidNode;
    std::string typeId;
    CanvasPoint position;
    std::vector<rin::ParamAssignment> params;
    /// 监看器预览窗高度（画布坐标；仅 viewer 节点参与几何，UI 私有状态
    /// 同 position 先例不入契约）。手柄拖拽调整，compose 期夹取。
    float monitorPreviewHeight = kMonitorPreviewHeight;
};

/// 一次画布图操作的结果；ok=false 时 error 携带人可读拒绝原因（§5.4 不静默失败）。
struct CanvasOpResult {
    bool ok = false;
    std::string error;
};

/// 连线预检中"阻塞本次操作"的问题类别：成环/类型不匹配/多驱动等结构性冲突直接
/// 拒绝并回滚（§5.4"被拒绝的图操作（类型不匹配、成环、多驱动等）"）；悬空输入
/// 是搭图过程的合法中间态（§4 校验标注呈现），参数问题与连线操作无关——两者
/// 只做画布标注，不阻塞操作。
[[nodiscard]] inline bool blockingIssueForConnect(const rin::ValidationIssueKind kind) {
    switch (kind) {
        case rin::ValidationIssueKind::DanglingInput:
        case rin::ValidationIssueKind::BadParam:
            return false;
        default:
            return true;
    }
}

struct CanvasGraphModel {
    /// 节点目录（调色板与端口签名的唯一来源）；生命周期由持有方保证。
    const rin::NodeCatalog* catalog = nullptr;
    /// 绘制序 = 数组序（先到后 = 底到顶）；命中检测自顶层（末尾）向下。
    std::vector<CanvasNode> nodes;
    std::vector<rin::Connection> connections;
    rin::NodeId nextNodeId = 1;
    /// 选中节点集（去重；§5.4 点击单选 / Shift 加选 / 点击空白清除）。
    std::vector<rin::NodeId> selection;
    /// 选中的连线（单选；Alt+点击直接删除，Del 删除选中项）。
    std::optional<rin::Connection> selectedConnection;
    /// 最近一次图校验缓存（每次变更后刷新；画布标注与启动准入同源）。
    rin::WorkflowValidation validation;

    [[nodiscard]] const CanvasNode* findNode(const rin::NodeId id) const {
        for (const CanvasNode& node : nodes) {
            if (node.id == id) {
                return &node;
            }
        }
        return nullptr;
    }

    [[nodiscard]] CanvasNode* findNode(const rin::NodeId id) {
        for (CanvasNode& node : nodes) {
            if (node.id == id) {
                return &node;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const rin::NodeDescriptor* descriptorFor(const CanvasNode& node) const {
        return catalog != nullptr ? rin::findNodeDescriptor(*catalog, node.typeId) : nullptr;
    }

    /// 组装契约图（M4-09）：携带已赋值参数（未赋值参数由引擎取声明默认值）。
    [[nodiscard]] rin::WorkflowGraph toGraph() const {
        rin::WorkflowGraph graph;
        graph.nodes.reserve(nodes.size());
        for (const CanvasNode& node : nodes) {
            graph.nodes.push_back({node.id, node.typeId, node.params});
        }
        graph.connections = connections;
        return graph;
    }

    /// 刷新校验缓存（UI 预检与引擎准入共用 validateWorkflowGraph，§5.3）。
    void revalidate() {
        validation = validateWorkflowGraph(toGraph(), catalog != nullptr ? *catalog
                                                                        : rin::NodeCatalog{});
    }

    /// 节点是否带校验问题（画布 destructive 描边标注，§4）。
    [[nodiscard]] bool nodeHasIssue(const rin::NodeId id) const {
        for (const rin::ValidationIssue& issue : validation.issues) {
            if (issue.node == id) {
                return true;
            }
        }
        return false;
    }

    /// 置顶（选中/拖动时提升绘制序，保证命中"顶层优先"与视觉一致）。
    void raise(const rin::NodeId id) {
        const auto it =
            std::find_if(nodes.begin(), nodes.end(),
                         [id](const CanvasNode& node) { return node.id == id; });
        if (it != nodes.end() && std::next(it) != nodes.end()) {
            std::rotate(it, std::next(it), nodes.end());
        }
    }

    /// 调色板落点创建（§5.2.1）：分配非零唯一 id，创建即独占选中（§5.2.3）。
    [[nodiscard]] CanvasOpResult createNode(const std::string& typeId, const CanvasPoint& pos) {
        if (catalog == nullptr ||
            rin::findNodeDescriptor(*catalog, typeId) == nullptr) {
            return {false, "unknown node type: " + typeId};
        }
        CanvasNode node;
        node.id = nextNodeId;
        node.typeId = typeId;
        node.position = pos;
        const rin::NodeDescriptor* descriptor = rin::findNodeDescriptor(*catalog, typeId);
        if (descriptor == nullptr) {
            return {false, "unknown node type: " + typeId};
        }
        // 位置下限：节点至少部分落在画布第一象限，避免拖出后不可见。
        node.position.x = std::max(0.0f, pos.x);
        node.position.y = std::max(0.0f, pos.y);
        nodes.push_back(node);
        ++nextNodeId;
        selection.assign(1, node.id);
        selectedConnection.reset();
        raise(node.id);
        revalidate();
        return {true, {}};
    }

    /// 删除节点（§5.4：连带其连线；选中集同步清理）。
    [[nodiscard]] CanvasOpResult deleteNodes(const std::vector<rin::NodeId>& ids) {
        for (const rin::NodeId id : ids) {
            if (findNode(id) == nullptr) {
                return {false, "unknown node"};
            }
        }
        for (const rin::NodeId id : ids) {
            connections.erase(std::remove_if(connections.begin(), connections.end(),
                                             [id](const rin::Connection& c) {
                                                 return c.from.node == id || c.to.node == id;
                                             }),
                              connections.end());
            nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                                       [id](const CanvasNode& node) { return node.id == id; }),
                        nodes.end());
            selection.erase(std::remove(selection.begin(), selection.end(), id),
                            selection.end());
        }
        if (selectedConnection &&
            std::find(connections.begin(), connections.end(), *selectedConnection) ==
                connections.end()) {
            selectedConnection.reset();
        }
        revalidate();
        return {true, {}};
    }

    /// 拖动位移（画布坐标增量；§5.4 节点拖动）。
    [[nodiscard]] CanvasOpResult moveNode(const rin::NodeId id, const CanvasPoint& delta) {
        CanvasNode* node = findNode(id);
        if (node == nullptr) {
            return {false, "unknown node"};
        }
        node->position += delta;
        return {true, {}};
    }

    /// 参数赋值提交（M5-04 §5.5）：替换同 paramId 旧赋值（实例内 paramId 唯一）
    /// 或追加；调用方（参数面板）负责提交前的声明校验——此处仅存储并刷新校验
    /// 缓存（BadParam 等问题经校验标注呈现）。
    [[nodiscard]] CanvasOpResult setParam(const rin::NodeId id,
                                          const rin::ParamAssignment& assignment) {
        CanvasNode* node = findNode(id);
        if (node == nullptr) {
            return {false, "unknown node"};
        }
        auto it = std::find_if(node->params.begin(), node->params.end(),
                               [&assignment](const rin::ParamAssignment& existing) {
                                   return existing.paramId == assignment.paramId;
                               });
        if (it != node->params.end()) {
            *it = assignment;
        } else {
            node->params.push_back(assignment);
        }
        revalidate();
        return {true, {}};
    }

    /// 建立连线（§5.3）：仅 Output→Input；类型一致；拒绝自环与成环；目标输入
    /// 已有入边时替换旧边（契约单驱动约束的 UI 表达）；重复同边幂等。
    [[nodiscard]] CanvasOpResult connect(const rin::PortRef& from, const rin::PortRef& to) {
        if (from.direction != rin::PortDirection::Output ||
            to.direction != rin::PortDirection::Input) {
            return {false, "connect an output port to an input port"};
        }
        const CanvasNode* fromNode = findNode(from.node);
        const CanvasNode* toNode = findNode(to.node);
        if (fromNode == nullptr || toNode == nullptr) {
            return {false, "unknown node"};
        }
        const rin::NodeDescriptor* fromDescriptor = descriptorFor(*fromNode);
        const rin::NodeDescriptor* toDescriptor = descriptorFor(*toNode);
        if (fromDescriptor == nullptr || toDescriptor == nullptr) {
            return {false, "unknown node type"};
        }
        if (from.index >= fromDescriptor->outputs.size() ||
            to.index >= toDescriptor->inputs.size()) {
            return {false, "port index out of range"};
        }
        if (from.node == to.node) {
            return {false, "self connection is not allowed"};
        }
        const rin::PortType outType = fromDescriptor->outputs[from.index];
        const rin::PortType inType = toDescriptor->inputs[to.index];
        // 连线兼容性（M11/DEC-021）：两端相等，或输入端声明 Any（监看器接受
        // 任意具体类型输出）。
        if (outType != inType && inType != rin::PortType::Any) {
            return {false, std::string("type mismatch: ") + rin::toString(outType) + " to " +
                               rin::toString(inType)};
        }
        const rin::Connection candidate{from, to};
        if (std::find(connections.begin(), connections.end(), candidate) != connections.end()) {
            return {true, {}};  // 幂等：重复同边 no-op
        }
        // 单驱动替换：先移除目标输入口的既有入边，再试加新边。
        std::vector<rin::Connection> tentative = connections;
        tentative.erase(std::remove_if(tentative.begin(), tentative.end(),
                                       [&to](const rin::Connection& c) { return c.to == to; }),
                        tentative.end());
        tentative.push_back(candidate);
        const rin::WorkflowValidation check = validateWorkflowGraph(
            [&] {
                rin::WorkflowGraph graph;
                graph.nodes.reserve(nodes.size());
                for (const CanvasNode& node : nodes) {
                    graph.nodes.push_back({node.id, node.typeId, node.params});
                }
                graph.connections = tentative;
                return graph;
            }(),
            catalog != nullptr ? *catalog : rin::NodeCatalog{});
        for (const rin::ValidationIssue& issue : check.issues) {
            if (blockingIssueForConnect(issue.kind)) {
                return {false, issue.message};
            }
        }
        connections = std::move(tentative);
        selectedConnection.reset();
        revalidate();
        return {true, {}};
    }

    /// 删除连线（§5.3 Alt+点击 / Del 删除选中连线）。
    [[nodiscard]] CanvasOpResult disconnect(const rin::Connection& connection) {
        const auto it = std::find(connections.begin(), connections.end(), connection);
        if (it == connections.end()) {
            return {false, "connection does not exist"};
        }
        connections.erase(it);
        if (selectedConnection == connection) {
            selectedConnection.reset();
        }
        revalidate();
        return {true, {}};
    }

    // --- 选中（§5.4） ---

    /// 点击选中：additive=false 独占选中；additive=true 切换该节点的选中成员
    /// （Shift 加选/移除）。
    void selectNode(const rin::NodeId id, const bool additive) {
        if (!additive) {
            selection.assign(1, id);
            selectedConnection.reset();
            raise(id);
            return;
        }
        const auto it = std::find(selection.begin(), selection.end(), id);
        if (it != selection.end()) {
            selection.erase(it);
        } else {
            selection.push_back(id);
            raise(id);
        }
    }

    void clearSelection() {
        selection.clear();
        selectedConnection.reset();
    }

    /// 框选集合（§5.4"拖空白框选"）：与节点包围盒相交即入选。
    [[nodiscard]] std::vector<rin::NodeId> nodesInRect(const CanvasRect& rect) const {
        std::vector<rin::NodeId> hits;
        for (const CanvasNode& node : nodes) {
            const rin::NodeDescriptor* descriptor = descriptorFor(node);
            if (descriptor == nullptr) {
                continue;
            }
            if (rect.intersects(nodeBounds(node.position, *descriptor, node.params,
                                            node.monitorPreviewHeight))) {
                hits.push_back(node.id);
            }
        }
        return hits;
    }

    // --- 命中检测（§7）：端口 > 节点 > 连线 > 空白（自顶层向下） ---

    /// 端口命中（命中区大于视觉尺寸，§5.3）：自顶层节点向下，先入后出。
    /// 监看器输出端口不参与命中（M11/DEC-021：UI 隐藏的端口不可拖线）。
    [[nodiscard]] std::optional<rin::PortRef> portAt(const CanvasPoint& point) const {
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
            const rin::NodeDescriptor* descriptor = descriptorFor(*it);
            if (descriptor == nullptr) {
                continue;
            }
            for (std::uint32_t index = 0; index < descriptor->inputs.size(); ++index) {
                const rin::PortRef port{it->id, rin::PortDirection::Input, index};
                if (canvasDistance(point,
                                   portPosition(it->position, *descriptor, it->params, port,
                                                it->monitorPreviewHeight)) <= kPortHitRadius) {
                    return port;
                }
            }
            if (isMonitorNode(it->typeId)) {
                continue;  // 输出端口隐藏（透传 sink）。
            }
            for (std::uint32_t index = 0; index < descriptor->outputs.size(); ++index) {
                const rin::PortRef port{it->id, rin::PortDirection::Output, index};
                if (canvasDistance(point,
                                   portPosition(it->position, *descriptor, it->params, port,
                                                it->monitorPreviewHeight)) <= kPortHitRadius) {
                    return port;
                }
            }
        }
        return std::nullopt;
    }

    /// 节点命中（顶层优先，§5.4）。
    [[nodiscard]] const CanvasNode* nodeAt(const CanvasPoint& point) const {
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
            const rin::NodeDescriptor* descriptor = descriptorFor(*it);
            if (descriptor == nullptr) {
                continue;
            }
            if (nodeBounds(it->position, *descriptor, it->params, it->monitorPreviewHeight)
                    .contains(point)) {
                return &*it;
            }
        }
        return nullptr;
    }

    /// 连线命中：贝塞尔采样折线到点最近距离 ≤ kWireHitDistance。
    [[nodiscard]] std::optional<rin::Connection> wireAt(const CanvasPoint& point) const {
        std::optional<rin::Connection> hit;
        float best = kWireHitDistance;
        for (const rin::Connection& connection : connections) {
            const CanvasNode* fromNode = findNode(connection.from.node);
            const CanvasNode* toNode = findNode(connection.to.node);
            if (fromNode == nullptr || toNode == nullptr) {
                continue;
            }
            const rin::NodeDescriptor* fromDescriptor = descriptorFor(*fromNode);
            const rin::NodeDescriptor* toDescriptor = descriptorFor(*toNode);
            if (fromDescriptor == nullptr || toDescriptor == nullptr) {
                continue;
            }
            const CanvasPoint from = portPosition(fromNode->position, *fromDescriptor,
                                                  fromNode->params, connection.from,
                                                  fromNode->monitorPreviewHeight);
            const CanvasPoint to = portPosition(toNode->position, *toDescriptor,
                                                toNode->params, connection.to,
                                                toNode->monitorPreviewHeight);
            const std::vector<CanvasPoint> samples = sampleWire(from, to, 24);
            for (std::size_t i = 1; i < samples.size(); ++i) {
                const float d = pointSegmentDistance(point, samples[i - 1], samples[i]);
                if (d <= best) {
                    best = d;
                    hit = connection;
                }
            }
        }
        return hit;
    }

    /// 全图包围盒（帧全图拟合输入；空图返回空矩形）。
    [[nodiscard]] CanvasRect graphBounds() const {
        CanvasRect bounds{};
        bool first = true;
        for (const CanvasNode& node : nodes) {
            const rin::NodeDescriptor* descriptor = descriptorFor(node);
            if (descriptor == nullptr) {
                continue;
            }
            const CanvasRect b = nodeBounds(node.position, *descriptor, node.params,
                                            node.monitorPreviewHeight);
            if (first) {
                bounds = b;
                first = false;
                continue;
            }
            const float minX = std::min(bounds.x, b.x);
            const float minY = std::min(bounds.y, b.y);
            const float maxX = std::max(bounds.x + bounds.width, b.x + b.width);
            const float maxY = std::max(bounds.y + bounds.height, b.y + b.height);
            bounds = {minX, minY, maxX - minX, maxY - minY};
        }
        return bounds;
    }
};

// --- 交互状态机（§5.1/§5.3/§5.4）：输入为画布坐标，输出为模型变更与反馈 ---

enum class InteractionMode {
    None,
    Pan,        ///< 中键拖空白平移（§5.1，画布交互消解决策见文件头）。
    NodeDrag,   ///< 左键拖节点移动（§5.4）。
    Marquee,    ///< 左键拖空白框选（§5.4）。
    Connect,    ///< 从输出端口拖出连线（§5.3）。
};

/// 按下结果：UI 据此呈现右键菜单/反馈消息；message 仅在非空时展示；
/// graphChanged=true 时调用方需要同步引擎（§5.3 画布图与引擎同步）。
struct PressResult {
    enum class Kind {
        None,
        NodePressed,
        PortPressed,
        WireSelected,
        WireDeleted,
        NodeDeleted,
        ContextMenu,
    };
    Kind kind = Kind::None;
    std::string message;
    bool graphChanged = false;
};

/// 松开结果：语义同 PressResult。
struct ReleaseResult {
    std::string message;
    bool graphChanged = false;
};

struct CanvasInteraction {
    InteractionMode mode = InteractionMode::None;
    rin::NodeId draggedNode = rin::kInvalidNode;
    /// NodeDrag 增量基准：按下点（画布坐标）与整组基准位置（§5.4 框选/加选后
    /// 按下任一选中节点拖动整组；单节点时组大小为 1）。
    CanvasPoint dragStartPoint{0.0f, 0.0f};
    std::vector<std::pair<rin::NodeId, CanvasPoint>> dragGroupBase;
    CanvasPoint marqueeStart;      ///< Marquee 起点（画布坐标）。
    CanvasPoint marqueeCurrent;
    bool marqueeArmed = false;     ///< 拖动超过阈值后视为框选，否则松开=点击空白。
    CanvasPoint connectCurrent;    ///< 连线拖拽的当前端点（画布坐标）。
    std::vector<rin::NodeId> marqueeBaseSelection;  ///< Shift 加选拖拽时的基准选中集。
    CanvasPoint lastScreen;        ///< Pan 增量基准（屏幕坐标）。
    bool panStarted = false;

    /// 是否处于连线拖拽中（node_canvas.hpp 绘制预览线）。
    [[nodiscard]] bool connecting() const { return mode == InteractionMode::Connect; }

    /// 连线发起端口（拖线预览与类型过滤读取；未连线时返回无效引用）。
    [[nodiscard]] rin::PortRef connectFromPort() const { return connectFrom_; }

    /// 框选拖拽中（node_canvas.hpp 绘制选择框）。
    [[nodiscard]] bool marqueeing() const { return mode == InteractionMode::Marquee; }

    /// 按下（button: 0=Left 1=Middle 2=Right；shift/alt 为修饰键快照）。
    /// 命中优先级（§7）：端口 > 节点 > 连线 > 空白（自顶层向下）。
    [[nodiscard]] PressResult onPress(CanvasGraphModel& model, const CanvasPoint& point,
                                      const int button, const bool shift, const bool alt) {
        mode = InteractionMode::None;
        switch (button) {
            case 1:  // 中键：平移（§5.1；左键让给框选，消解决策见文件头）。
                mode = InteractionMode::Pan;
                lastScreen = point;
                panStarted = false;
                return {};
            case 2:  // 右键：节点=删除（§5.4"Del/右键删除"）；空白=创建菜单（§5.2.2）。
                if (const CanvasNode* node = model.nodeAt(point); node != nullptr) {
                    const rin::NodeId id = node->id;
                    const CanvasOpResult result = model.deleteNodes({id});
                    return {result.ok ? PressResult::Kind::NodeDeleted
                                      : PressResult::Kind::None,
                            result.ok ? "node removed" : result.error,
                            result.ok};
                }
                return {PressResult::Kind::ContextMenu, {}, false};
            default:
                break;
        }

        std::string portHint;
        if (const std::optional<rin::PortRef> port = model.portAt(point); port) {
            if (port->direction == rin::PortDirection::Output) {
                mode = InteractionMode::Connect;
                connectFrom_ = *port;
                connectCurrent = point;
                return {PressResult::Kind::PortPressed, {}, false};
            }
            // §5.3 仅允许 Output→Input 发起；输入端口按下按节点按压处理并提示。
            portHint = "wires start at an output port";
        }

        if (const CanvasNode* node = model.nodeAt(point); node != nullptr) {
            const rin::NodeId id = node->id;  // selectNode→raise 会旋转 nodes，
            const bool alreadySelected =      // 之后不得再经此指针访问。
                std::find(model.selection.begin(), model.selection.end(), id) !=
                model.selection.end();
            if (shift || !alreadySelected) {
                model.selectNode(id, shift);
            }
            // 组拖动基准（§5.4）：按下已选中节点 = 整组随动，否则单节点。
            dragStartPoint = point;
            dragGroupBase.clear();
            const bool pressedSelected =
                std::find(model.selection.begin(), model.selection.end(), id) !=
                model.selection.end();
            if (pressedSelected) {
                for (const rin::NodeId sid : model.selection) {
                    if (const CanvasNode* member = model.findNode(sid); member != nullptr) {
                        dragGroupBase.push_back({sid, member->position});
                    }
                }
            } else if (const CanvasNode* single = model.findNode(id); single != nullptr) {
                dragGroupBase.push_back({id, single->position});
            }
            draggedNode = id;
            mode = InteractionMode::NodeDrag;
            return {PressResult::Kind::NodePressed, portHint, false};
        }

        if (const std::optional<rin::Connection> wire = model.wireAt(point); wire) {
            if (alt) {
                const CanvasOpResult result = model.disconnect(*wire);
                return {result.ok ? PressResult::Kind::WireDeleted : PressResult::Kind::None,
                        result.ok ? "connection removed" : result.error,
                        result.ok};
            }
            model.clearSelection();
            model.selectedConnection = wire;
            return {PressResult::Kind::WireSelected, {}, false};
        }

        // 空白：左键待框选（拖动超过阈值成框选，原位松开=清除选中，§5.4）。
        mode = InteractionMode::Marquee;
        marqueeArmed = false;
        marqueeStart = point;
        marqueeCurrent = point;
        marqueeBaseSelection = model.selection;
        return {};
    }

    /// 拖动推进：screenPoint 为视口屏幕坐标（Pan 平移增量基准），canvasPoint
    /// 为换算后的画布坐标（节点/框选/连线语义）。
    void onDrag(CanvasGraphModel& model, CanvasView& view, const CanvasPoint& screenPoint,
                const CanvasPoint& canvasPoint) {
        switch (mode) {
            case InteractionMode::Pan: {
                if (!panStarted) {
                    lastScreen = screenPoint;
                    panStarted = true;
                    return;
                }
                view.pan += screenPoint - lastScreen;
                lastScreen = screenPoint;
                return;
            }
            case InteractionMode::NodeDrag: {
                // 组拖动：整组按按下时基准位置 + 增量（§5.4；单节点同构）。
                const CanvasPoint delta = canvasPoint - dragStartPoint;
                for (const auto& [id, base] : dragGroupBase) {
                    if (CanvasNode* node = model.findNode(id); node != nullptr) {
                        node->position = base + delta;
                    }
                }
                return;
            }
            case InteractionMode::Marquee: {
                marqueeCurrent = canvasPoint;
                if (std::abs(marqueeCurrent.x - marqueeStart.x) > 4.0f ||
                    std::abs(marqueeCurrent.y - marqueeStart.y) > 4.0f) {
                    marqueeArmed = true;
                }
                return;
            }
            case InteractionMode::Connect: {
                connectCurrent = canvasPoint;
                return;
            }
            case InteractionMode::None:
                return;
        }
    }

    /// 松开：提交框选/连线；返回反馈消息与图变更标记（§5.4 拒绝原因显式反馈）。
    [[nodiscard]] ReleaseResult onRelease(CanvasGraphModel& model, const CanvasPoint& point,
                                          const bool shift) {
        ReleaseResult result;
        switch (mode) {
            case InteractionMode::Marquee: {
                if (marqueeArmed) {
                    std::vector<rin::NodeId> hits =
                        model.nodesInRect(canvasRectBetween(marqueeStart, marqueeCurrent));
                    if (shift) {
                        // Shift 加选：并入按下时的基准选中集（§5.4）。
                        for (const rin::NodeId id : marqueeBaseSelection) {
                            if (std::find(hits.begin(), hits.end(), id) == hits.end()) {
                                hits.push_back(id);
                            }
                        }
                    }
                    model.selection = hits;
                    model.selectedConnection.reset();
                } else {
                    model.clearSelection();  // 原位松开 = 点击空白清除选中。
                }
                break;
            }
            case InteractionMode::Connect: {
                const std::optional<rin::PortRef> target = model.portAt(point);
                const bool validTarget = target && target->direction == rin::PortDirection::Input;
                if (validTarget) {
                    const CanvasOpResult op = model.connect(connectFrom_, *target);
                    result.message = op.ok ? "" : op.error;
                    result.graphChanged = op.ok;
                } else if (target) {
                    result.message = "wires end at an input port";
                } else {
                    result.message = "";  // §5.3 落在空白处仅取消拖线。
                }
                break;
            }
            case InteractionMode::None:
            case InteractionMode::Pan:
            case InteractionMode::NodeDrag:
                break;
        }
        mode = InteractionMode::None;
        draggedNode = rin::kInvalidNode;
        return result;
    }

    /// Del 删除选中（§5.4：节点与其连线、选中连线）；返回反馈消息（可为空）。
    [[nodiscard]] std::string deleteSelection(CanvasGraphModel& model) {
        std::string message;
        if (model.selectedConnection) {
            const rin::Connection connection = *model.selectedConnection;
            const CanvasOpResult result = model.disconnect(connection);
            message = result.ok ? "connection removed" : result.error;
            return message;
        }
        if (!model.selection.empty()) {
            const CanvasOpResult result = model.deleteNodes(model.selection);
            message = result.ok ? "node removed" : result.error;
        }
        return message;
    }

private:
    /// 连线发起端口（onPress 命中输出端口时记录，onRelease 作为连线起点）。
    rin::PortRef connectFrom_;
};

// --- 调色板模型（§5.2/§7）：目录分组与即输即筛过滤 ---

struct PaletteGroup {
    std::string title;
    std::vector<const rin::NodeDescriptor*> items;
};

/// 类型 → 分组归类表（UI 私有呈现；契约无类别字段）。未知类型归入 Other，
/// 保证目录扩展时调色板不丢项。M6-03 深度源归 Source、灰度域几何算子归
/// Geometry（与 Rgba8 版同组，DEC-017）。
[[nodiscard]] inline const char* paletteGroupFor(const std::string& typeId) {
    if (typeId == "source" || typeId == "source_depth_jet" ||
        typeId == "source_depth_gray" || typeId == "source_depth_adaptive") {
        return "Source";
    }
    if (typeId == "crop" || typeId == "downscale" || typeId == "crop_gray" ||
        typeId == "downscale_gray") {
        return "Geometry";
    }
    if (typeId == "grayify" || typeId == "gaussian_blur" || typeId == "conv_kernel") {
        return "Filter";
    }
    if (typeId == "fft_lowpass" || typeId == "fft_highpass" || typeId == "fft_bandpass") {
        return "Frequency";
    }
    if (typeId == "hist_eq") {
        return "Histogram";
    }
    if (isMonitorNode(typeId)) {
        return "View";  // 监看器（M11/DEC-021）：末端预览 sink。
    }
    return "Other";
}

[[nodiscard]] inline std::string toLowerAscii(const std::string& text) {
    std::string lowered = text;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lowered;
}

/// 分组 + 过滤（§5.2"即输即筛"）：filter 对 typeId 与 displayName 做大小写不
/// 敏感子串匹配；无匹配项的分组整组隐藏。目录顺序即组内条目顺序。
[[nodiscard]] inline std::vector<PaletteGroup> paletteGroups(const rin::NodeCatalog& catalog,
                                                             const std::string& filter) {
    std::vector<PaletteGroup> groups;
    const std::string needle = toLowerAscii(filter);
    for (const rin::NodeDescriptor& descriptor : catalog.nodes) {
        const char* title = paletteGroupFor(descriptor.typeId);
        if (!needle.empty()) {
            const bool matched =
                toLowerAscii(descriptor.typeId).find(needle) != std::string::npos ||
                toLowerAscii(descriptor.displayName).find(needle) != std::string::npos;
            if (!matched) {
                continue;
            }
        }
        auto it = std::find_if(groups.begin(), groups.end(), [title](const PaletteGroup& g) {
            return g.title == title;
        });
        if (it == groups.end()) {
            groups.push_back({title, {}});
            it = std::prev(groups.end());
        }
        it->items.push_back(&descriptor);
    }
    return groups;
}

}  // namespace viewer
