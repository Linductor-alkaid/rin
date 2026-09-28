#pragma once

// 节点编辑器画布组装（M5-03，[DEC-014] 五区骨架的工作流页填充 / [DEC-015]
// EUI-NEO 原语自研画布）。替代 M5-02 的 workflow_shell.hpp 静态骨架（五区几何
// 不变：工具栏 44 / 调色板 200 / 画布 / 上下文 264 / 底部 120，间距 kSpace3）；
// M5-04 起上下文面板（参数面板/缩略图）与五区组装迁移至 param_panel.hpp。
//
// 交互结构（DEC-015 决策 2：交互几何下沉纯逻辑）：画布内全部指针语义由覆盖
// 视口的一层 `components::mouseArea` 承载，经 canvas_model.hpp 的命中检测与
// 交互状态机分发——节点/连线为非交互视觉元素，不参与命中（retained layer 的
// polygon 绘制签名缺陷按 EUI-20260924-001 以 dirtyKey 键控绕行，画布修订号
// 作脏键：交互期间逐帧直绘，静止时键稳定）。连线即时类型过滤（§5.3）：拖线
// 中兼容输入端口高亮 brand、不兼容端口降为次级色。
//
// 引擎同步（§5.3/§5.4）：每次图变更经 afterGraphChange 调用 `applyGraph`——
// UI 预检与引擎准入共用 validateWorkflowGraph 唯一判据，中间态（悬空输入）
// 以校验标注呈现而非阻塞搭图；被拒绝的连线操作以反馈行显式说明（不静默）。
//
// 原语映射（ui_workspace_design.md §6）：画布视口 stack+clip、节点 rect、
// 端口 rect、连线 polygon（贝塞尔采样）、右键创建菜单为自绘浮层（沿用
// EUI-20260928-001 的 rect/text 退化先例，未用 components::contextMenu）、
// 调色板过滤输入为 components::input（样式字段逐一取 viewer 令牌，不引入
// 组件主题度量）。键盘路径（F/Del/Esc）在 app.cpp 的 DslAppConfig::onKeyEvent，
// 每项均有鼠标等价路径（Fit 按钮/右键删除/点击空白）。
//
// [DEC-014]: ../../docs/decisions/DEC-014-workbench-information-architecture.md
// [DEC-015]: ../../docs/decisions/DEC-015-node-editor-implementation-path.md

#include "canvas_model.hpp"
#include "param_model.hpp"
#include "viewer_theme.hpp"

#include <eui_neo.h>

#include <rin/workflow_engine.hpp>
#include <rin/workflow_types.hpp>

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace viewer {

/// 工作流页画布会话状态（ViewerContext 持有，地址稳定供闭包引用；DEC-014
/// 决策 4：页面 UI 状态不因导航丢失，导航层不触碰本结构）。
struct WorkflowCanvasState {
    /// 画布图模型；catalog 指向引擎目录（构建期确定，随引擎生命周期）。
    CanvasGraphModel model;
    CanvasView view;
    CanvasInteraction interaction;
    /// 画布视口尺寸与窗口绝对矩形（compose 时写入；落点/菜单坐标换算）。
    CanvasPoint viewport{0.0f, 0.0f};
    CanvasRect areaRect{0.0f, 0.0f, 0.0f, 0.0f};
    /// 最近悬停点（视口局部坐标；滚轮缩放锚点，§5.1 以指针为锚点）。
    CanvasPoint lastMouse{0.0f, 0.0f};

    // 调色板拖出创建（§5.2.1）：拖拽过程显示类型名跟随光标。
    bool paletteDragging = false;
    std::string paletteDragType;
    CanvasPoint paletteDragPos{0.0f, 0.0f};  // 窗口逻辑坐标（mouseArea 全局）。

    // 画布右键创建菜单（§5.2.2，自绘浮层；即输即筛）。
    bool menuOpen = false;
    CanvasPoint menuPos{0.0f, 0.0f};
    eui::Signal<std::string> menuFilter;
    eui::Signal<std::string> paletteFilter;

    /// 最近一次操作反馈（§5.4 拒绝原因显式反馈，不静默失败）。
    std::string feedback;
    /// 引擎最新事件行（底部列表；pump 消费 tryLoadEvent 写入）。
    std::string lastEvent;
    /// 节点执行失败标注（M5-04 §4：NodeFailed → destructive 徽标；会话级，
    /// Started/Stopped 事件清空；事件消费在 app.cpp pump）。
    NodeFailureMarks failures;
    /// 画布内容修订号（连线 polygon 的显式脏键，EUI-20260924-001 绕行）。
    std::uint64_t revision = 0;

    /// 图变更统一出口：UI 预检与引擎准入共用唯一判据（§5.3）——有引擎时以
    /// applyGraph 的同步校验结果为准（引擎拒绝时保持当前生效图不变，契约），
    /// 无引擎（纯逻辑测试）时本地 revalidate。
    void afterGraphChange(rin::IWorkflowEngine* engine) {
        if (engine != nullptr) {
            model.validation = engine->applyGraph(model.toGraph());
        } else {
            model.revalidate();
        }
        ++revision;
    }

    /// 画布局部（视口）坐标 → 画布坐标。
    [[nodiscard]] CanvasPoint toCanvas(const CanvasPoint& local) const {
        return view.toCanvas(local);
    }

    /// 窗口全局坐标 → 画布坐标（落点/菜单创建位置）。
    [[nodiscard]] CanvasPoint globalToCanvas(const CanvasPoint& global) const {
        return view.toCanvas({global.x - areaRect.x, global.y - areaRect.y});
    }
};

/// 引擎事件 → 底部列表一行（契约无事件种类字符串，UI 呈现自绘）。
[[nodiscard]] inline std::string workflowEventLine(const rin::WorkflowEvent& event) {
    const char* kind = "event";
    switch (event.kind) {
        case rin::WorkflowEventKind::Started:
            kind = "started";
            break;
        case rin::WorkflowEventKind::GraphApplied:
            kind = "graph applied";
            break;
        case rin::WorkflowEventKind::ParamUpdated:
            kind = "param updated";
            break;
        case rin::WorkflowEventKind::Info:
            kind = "info";
            break;
        case rin::WorkflowEventKind::NodeFailed:
            kind = "node failed";
            break;
        case rin::WorkflowEventKind::Stopped:
            kind = "stopped";
            break;
        case rin::WorkflowEventKind::Failed:
            kind = "failed";
            break;
    }
    std::string line = std::string("[") + kind + "]";
    if (!event.message.empty()) {
        line += " " + event.message;
    }
    return line;
}

/// 校验问题类别 → 列表短标签（kind + node 定位 + message，§4）。
[[nodiscard]] inline const char* validationKindLabel(const rin::ValidationIssueKind kind) {
    switch (kind) {
        case rin::ValidationIssueKind::InvalidNodeId:
            return "invalid id";
        case rin::ValidationIssueKind::DuplicateNodeId:
            return "duplicate id";
        case rin::ValidationIssueKind::UnknownNodeType:
            return "unknown type";
        case rin::ValidationIssueKind::PortOutOfRange:
            return "port out of range";
        case rin::ValidationIssueKind::DirectionMismatch:
            return "direction mismatch";
        case rin::ValidationIssueKind::UnknownConnectionNode:
            return "unknown connection node";
        case rin::ValidationIssueKind::TypeMismatch:
            return "type mismatch";
        case rin::ValidationIssueKind::MultipleDrivers:
            return "multiple drivers";
        case rin::ValidationIssueKind::SelfLoop:
            return "self loop";
        case rin::ValidationIssueKind::Cycle:
            return "cycle";
        case rin::ValidationIssueKind::DanglingInput:
            return "dangling input";
        case rin::ValidationIssueKind::BadParam:
            return "bad param";
    }
    return "issue";
}

namespace {

using viewer::theme::kBorderHairline;
using viewer::theme::kFontCaption;
using viewer::theme::kFontSm;
using viewer::theme::kFontXs;
using viewer::theme::kRadiusLg;
using viewer::theme::kRadiusMd;
using viewer::theme::kRadiusSm;
using viewer::theme::kSpace1;
using viewer::theme::kSpace2;
using viewer::theme::kSpace3;

/// 节点标题字号随缩放（下限保持可读；缩放上限 2.5 时 35px 仍为单行）。
[[nodiscard]] inline float scaledFont(const float base, const float scale,
                                      const float minimum = 8.0f) {
    return std::max(minimum, base * scale);
}

// --- 工具栏（§5.8 运行控制 M5-06 接入；本区当前：标题/校验状态/反馈/Fit） ---
inline void composeWorkflowToolbar(eui::Ui& ui, WorkflowCanvasState& state, const float x,
                                   const float y, const float width, const float height) {
    const theme::ThemeTokens& tokens = theme::dark();
    const float pad = kSpace3;
    const float fitWidth = 56.0f;

    ui.stack("workflow.toolbar")
        .position(x, y)
        .size(width, height)
        .content([&] {
            ui.rect("workflow.toolbar.card")
                .size(width, height)
                .radius(kRadiusLg)
                .color(tokens.card)
                .border(kBorderHairline, tokens.cardBorder)
                .build();
            ui.text("workflow.toolbar.title")
                .position(pad, 0.0f)
                .size(120.0f, height)
                .text("Workflow")
                .fontSize(theme::kFontBase)
                .fontWeight(theme::kWeightSemibold)
                .color(tokens.fg)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            char issues[64];
            if (state.model.validation.ok) {
                std::snprintf(issues, sizeof(issues), "graph valid");
            } else {
                std::snprintf(issues, sizeof(issues), "%zu issue(s)",
                              state.model.validation.issues.size());
            }
            ui.text("workflow.toolbar.status")
                .position(120.0f + pad, 0.0f)
                .size(200.0f, height)
                .text(issues)
                .fontSize(kFontSm)
                .color(state.model.validation.ok ? tokens.fgSubtlest : tokens.warning)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            // 帧全图按钮（§5.1 快捷键 F 的鼠标等价路径）。
            ui.rect("workflow.toolbar.fit")
                .position(width - pad - fitWidth, (height - 26.0f) * 0.5f)
                .size(fitWidth, 26.0f)
                .radius(kRadiusMd)
                .color(tokens.input)
                .border(kBorderHairline, tokens.inputBorder)
                .states(tokens.input, tokens.inputBorderHover, tokens.inputBorderHover)
                .onClick([&state] {
                    state.view.fit(state.model.graphBounds(), state.viewport.x,
                                   state.viewport.y);
                    ++state.revision;
                })
                .build();
            ui.text("workflow.toolbar.fitText")
                .position(width - pad - fitWidth, (height - 26.0f) * 0.5f)
                .size(fitWidth, 26.0f)
                .text("Fit")
                .fontSize(kFontSm)
                .color(tokens.fgSubtle)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            if (!state.feedback.empty()) {
                ui.text("workflow.toolbar.feedback")
                    .position(340.0f, 0.0f)
                    .size(width - 340.0f - fitWidth - pad * 2.0f, height)
                    .text(state.feedback)
                    .fontSize(kFontSm)
                    .color(tokens.fgSubtlest)
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .maxWidth(width - 340.0f - fitWidth - pad * 2.0f)
                    .build();
            }
        })
        .build();
}

// --- 调色板（§5.2：目录分组 + 即输即筛 + 拖出创建） ---
inline void composeWorkflowPalette(eui::Ui& ui, WorkflowCanvasState& state,
                                   rin::IWorkflowEngine* engine, const float x, const float y,
                                   const float width, const float height) {
    const theme::ThemeTokens& tokens = theme::dark();
    const float pad = kSpace3;
    const float inputHeight = 28.0f;
    const float groupTitleHeight = kFontXs + kSpace2;
    const float itemHeight = 26.0f;

    ui.stack("workflow.palette")
        .position(x, y)
        .size(width, height)
        .clip()
        .content([&] {
            ui.rect("workflow.palette.card")
                .size(width, height)
                .radius(kRadiusLg)
                .color(tokens.card)
                .border(kBorderHairline, tokens.cardBorder)
                .build();
            ui.text("workflow.palette.title")
                .position(pad, pad)
                .size(width - pad * 2.0f, kFontSm + kSpace1)
                .text("Palette")
                .fontSize(kFontSm)
                .fontWeight(theme::kWeightSemibold)
                .color(tokens.fgSubtle)
                .build();

            // 即输即筛（§5.2）：过滤框样式字段逐一取 viewer 令牌（DEC-005），
            // 不引入组件主题度量（inset/字号显式给定）。
            components::InputStyle inputStyle;
            inputStyle.background = tokens.input;
            inputStyle.focused = tokens.input;
            inputStyle.border = tokens.inputBorder;
            inputStyle.focusBorder = tokens.brand;
            inputStyle.text = tokens.fg;
            inputStyle.placeholder = tokens.fgSubtlest;
            inputStyle.cursor = tokens.brand;
            inputStyle.shadow = core::Shadow{};
            inputStyle.radius = kRadiusMd;
            components::input(ui, "workflow.palette.filter")
                .position(pad, pad + kFontSm + kSpace2)
                .size(width - pad * 2.0f, inputHeight)
                .bind(state.paletteFilter)
                .placeholder("filter")
                .fontSize(kFontSm)
                .inset(kSpace2)
                .style(inputStyle)
                .build();

            // 目录引用必须两侧同为左值：`cond ? *catalog : NodeCatalog{}` 会因
            // 左值/纯右值混合把左值分支拷贝成临时目录，语句结束时 items 里指向
            // 其节点元素的指针全部悬垂（M5-04 冒烟实证的 M5-03 潜伏缺陷）。
            static const rin::NodeCatalog kEmptyPaletteCatalog{};
            const rin::NodeCatalog& paletteCatalog =
                state.model.catalog != nullptr ? *state.model.catalog
                                               : kEmptyPaletteCatalog;
            const std::vector<PaletteGroup> groups =
                paletteGroups(paletteCatalog, state.paletteFilter.get());
            float rowY = pad + kFontSm + kSpace2 + inputHeight + kSpace2;
            for (const PaletteGroup& group : groups) {
                ui.text("workflow.palette.group." + group.title)
                    .position(pad, rowY)
                    .size(width - pad * 2.0f, groupTitleHeight)
                    .text(group.title)
                    .fontSize(kFontXs)
                    .color(tokens.fgSubtlest)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
                rowY += groupTitleHeight;
                for (const rin::NodeDescriptor* descriptor : group.items) {
                    const std::string base =
                        std::string("workflow.palette.item.") + descriptor->typeId;
                    const eui::Color transparent{0.0f, 0.0f, 0.0f, 0.0f};
                    ui.rect(base + ".row")
                        .position(pad, rowY)
                        .size(width - pad * 2.0f, itemHeight)
                        .radius(kRadiusSm)
                        .color(transparent)
                        .states(transparent, tokens.menuHover, tokens.menuHover)
                        .build();
                    ui.text(base + ".label")
                        .position(pad + kSpace2, rowY)
                        .size(width - pad * 2.0f - kSpace2, itemHeight)
                        .text(descriptor->displayName)
                        .fontSize(kFontCaption)
                        .color(tokens.fg)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .build();
                    // 拖出创建（§5.2.1）：拖拽越过阈值才进入拖拽态（浮层跟随
                    // 光标），落在画布内创建、画布外取消；原位单击不触发。
                    components::mouseArea(ui, base + ".drag")
                        .position(pad, rowY)
                        .size(width - pad * 2.0f, itemHeight)
                        .cursor(eui::CursorShape::Hand)
                        .onDragStart([&state, descriptor](
                                         const components::MouseEvent& event) {
                            state.paletteDragging = true;
                            state.paletteDragType = descriptor->typeId;
                            state.paletteDragPos = {event.globalX, event.globalY};
                        })
                        .onDrag([&state](const components::MouseDragEvent& event) {
                            state.paletteDragPos = {event.globalX, event.globalY};
                        })
                        .onDragEnd([&state, engine](const components::MouseDragEvent& event) {
                            state.paletteDragging = false;
                            if (!state.areaRect.contains({event.globalX, event.globalY})) {
                                return;  // §5.2.1 落点在画布外 = 取消。
                            }
                            const CanvasPoint canvas =
                                state.globalToCanvas({event.globalX, event.globalY});
                            const CanvasPoint origin{
                                canvas.x - kNodeWidth * 0.5f,
                                canvas.y - kNodeHeaderHeight * 0.5f};
                            const CanvasOpResult result = state.model.createNode(
                                state.paletteDragType, origin);
                            state.feedback = result.ok ? "" : result.error;
                            state.afterGraphChange(engine);
                        })
                        .build();
                    rowY += itemHeight;
                }
                rowY += kSpace1;
            }
        })
        .build();
}

// --- 节点框（§5.3 端口纵向排布 / §4 选中与问题描边） ---
inline void composeCanvasNode(eui::Ui& ui, WorkflowCanvasState& state,
                              const CanvasNode& node,
                              const std::optional<rin::PortType>& draftType) {
    const theme::ThemeTokens& tokens = theme::dark();
    const rin::NodeDescriptor* descriptor = state.model.descriptorFor(node);
    if (descriptor == nullptr) {
        return;  // 目录缺项（契约校验 UnknownNodeType）：跳过绘制，问题走列表。
    }
    const float s = state.view.scale;
    const CanvasPoint origin = state.view.toScreen(node.position);
    const float w = kNodeWidth * s;
    const float h = nodeHeight(*descriptor) * s;
    const bool selected =
        std::find(state.model.selection.begin(), state.model.selection.end(), node.id) !=
        state.model.selection.end();
    const bool invalid = state.model.nodeHasIssue(node.id);
    // 节点错误可视化（M5-04 §4）：执行失败（NodeFailed）与校验问题同样以
    // destructive 描边标注，失败另加徽标；消息走面板徽标与底部事件列表。
    const bool failed = state.failures.failureOf(node.id) != nullptr;
    const std::string base = "workflow.canvas.node." + std::to_string(node.id);

    ui.stack(base)
        .position(origin.x, origin.y)
        .size(w, h)
        .content([&] {
            ui.rect(base + ".card")
                .size(w, h)
                .radius(std::max(2.0f, kRadiusMd * s))
                .color(selected ? tokens.accentSurface : tokens.card)
                .border(kBorderHairline,
                        (invalid || failed) ? tokens.destructive
                                            : (selected ? tokens.brand : tokens.cardBorder))
                .build();
            ui.text(base + ".title")
                .position(kSpace2 * s, 0.0f)
                .size(w - kSpace2 * 2.0f * s, kNodeHeaderHeight * s)
                .text(descriptor->displayName)
                .fontSize(scaledFont(kFontSm, s))
                .color(tokens.fg)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            if (failed) {
                const float badge = 12.0f * s;
                ui.rect(base + ".failedBadge")
                    .position(w - badge - 3.0f * s, 3.0f * s)
                    .size(badge, badge)
                    .radius(badge * 0.5f)
                    .color(tokens.destructive)
                    .build();
                ui.text(base + ".failedBadge.mark")
                    .position(w - badge - 3.0f * s, 3.0f * s)
                    .size(badge, badge)
                    .text("!")
                    .fontSize(scaledFont(kFontXs, s, 6.0f))
                    .color(tokens.background)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
            }
            // 端口（视觉尺寸 kPortVisualRadius；输入左缘、输出右缘，§5.3）。
            const float r = kPortVisualRadius * s;
            const auto portColor = [&](const rin::PortType type,
                                       const rin::PortDirection direction) {
                if (draftType && direction == rin::PortDirection::Input) {
                    // 拖线中即时类型过滤（§5.3）：兼容高亮 brand，不兼容降次级。
                    return *draftType == type ? tokens.brand : tokens.fgSubtlest;
                }
                return theme::portTypeColor(type);
            };
            for (std::size_t i = 0; i < descriptor->inputs.size(); ++i) {
                const rin::PortRef port{node.id, rin::PortDirection::Input,
                                        static_cast<std::uint32_t>(i)};
                const CanvasPoint local =
                    portPosition(node.position, port) - node.position;
                ui.rect(base + ".in." + std::to_string(i))
                    .position(local.x * s - r, local.y * s - r)
                    .size(r * 2.0f, r * 2.0f)
                    .radius(r)
                    .color(portColor(descriptor->inputs[i], rin::PortDirection::Input))
                    .build();
            }
            for (std::size_t i = 0; i < descriptor->outputs.size(); ++i) {
                const rin::PortRef port{node.id, rin::PortDirection::Output,
                                        static_cast<std::uint32_t>(i)};
                const CanvasPoint local = portPosition(node.position, port) - node.position;
                ui.rect(base + ".out." + std::to_string(i))
                    .position(local.x * s - r, local.y * s - r)
                    .size(r * 2.0f, r * 2.0f)
                    .radius(r)
                    .color(portColor(descriptor->outputs[i], rin::PortDirection::Output))
                    .build();
            }
        })
        .build();
}

// --- 节点画布（§5.1 视图变换 / §5.3 连线 / §5.4 选择） ---
inline void composeWorkflowCanvas(eui::Ui& ui, WorkflowCanvasState& state,
                                  rin::IWorkflowEngine* engine, const float x, const float y,
                                  const float width, const float height) {
    const theme::ThemeTokens& tokens = theme::dark();
    state.viewport = {width, height};
    state.areaRect = {x, y, width, height};
    const std::string dirty = std::to_string(state.revision);

    // 拖线中的即时类型过滤基准（§5.3）：发起输出端口的类型。
    std::optional<rin::PortType> draftType;
    if (state.interaction.connecting()) {
        const rin::PortRef from = state.interaction.connectFromPort();
        if (const CanvasNode* fromNode = state.model.findNode(from.node);
            fromNode != nullptr) {
            if (const rin::NodeDescriptor* fd = state.model.descriptorFor(*fromNode);
                fd != nullptr && from.index < fd->outputs.size()) {
                draftType = fd->outputs[from.index];
            }
        }
    }

    ui.stack("workflow.canvas")
        .position(x, y)
        .size(width, height)
        .clip()
        .content([&] {
            ui.rect("workflow.canvas.area")
                .size(width, height)
                .radius(kRadiusLg)
                .color(tokens.surface)
                .border(kBorderHairline, tokens.border)
                .build();

            // 连线（先于节点 = 下层；非交互不挡命中；修订号作脏键逐帧直绘）。
            for (std::size_t i = 0; i < state.model.connections.size(); ++i) {
                const rin::Connection& connection = state.model.connections[i];
                const CanvasNode* fromNode = state.model.findNode(connection.from.node);
                const CanvasNode* toNode = state.model.findNode(connection.to.node);
                if (fromNode == nullptr || toNode == nullptr) {
                    continue;
                }
                const rin::NodeDescriptor* fd = state.model.descriptorFor(*fromNode);
                const rin::NodeDescriptor* td = state.model.descriptorFor(*toNode);
                if (fd == nullptr || td == nullptr ||
                    connection.from.index >= fd->outputs.size() ||
                    connection.to.index >= td->inputs.size()) {
                    continue;
                }
                const CanvasPoint a = portPosition(fromNode->position, connection.from);
                const CanvasPoint b = portPosition(toNode->position, connection.to);
                std::vector<eui::Vec2> points;
                points.reserve(25);
                for (const CanvasPoint& p : sampleWire(a, b, 24)) {
                    const CanvasPoint screen = state.view.toScreen(p);
                    points.push_back({screen.x, screen.y});
                }
                const bool selected = state.model.selectedConnection == connection;
                ui.polygon("workflow.canvas.wire." + std::to_string(i))
                    .position(0.0f, 0.0f)
                    .size(width, height)
                    .points(std::move(points))
                    .color(selected ? tokens.brand : theme::portTypeColor(fd->outputs[connection.from.index]))
                    .dirtyKey(dirty)
                    .build();
            }

            // 拖线预览（§5.3）：发起端口 → 当前指针，类型色。
            if (state.interaction.connecting()) {
                const rin::PortRef from = state.interaction.connectFromPort();
                if (const CanvasNode* fromNode = state.model.findNode(from.node);
                    fromNode != nullptr && draftType) {
                    const CanvasPoint a = portPosition(fromNode->position, from);
                    const CanvasPoint b = state.interaction.connectCurrent;
                    std::vector<eui::Vec2> points;
                    points.reserve(25);
                    for (const CanvasPoint& p : sampleWire(a, b, 24)) {
                        const CanvasPoint screen = state.view.toScreen(p);
                        points.push_back({screen.x, screen.y});
                    }
                    ui.polygon("workflow.canvas.wire.draft")
                        .position(0.0f, 0.0f)
                        .size(width, height)
                        .points(std::move(points))
                        .color(theme::portTypeColor(*draftType))
                        .dirtyKey(dirty)
                        .build();
                }
            }

            // 节点（绘制序 = 模型数组序，后到顶）。
            for (const CanvasNode& node : state.model.nodes) {
                composeCanvasNode(ui, state, node, draftType);
            }

            // 框选矩形（§5.4 拖空白框选）。
            if (state.interaction.marqueeing() && state.interaction.marqueeArmed) {
                const CanvasRect rect = canvasRectBetween(state.interaction.marqueeStart,
                                                          state.interaction.marqueeCurrent);
                const CanvasPoint p0 = state.view.toScreen({rect.x, rect.y});
                ui.rect("workflow.canvas.marquee")
                    .position(p0.x, p0.y)
                    .size(rect.width * state.view.scale, rect.height * state.view.scale)
                    .radius(kRadiusSm)
                    .color(tokens.accentSurface)
                    .border(kBorderHairline, tokens.brand)
                    .dirtyKey(dirty)
                    .build();
            }

            // 空态引导（§4：画布中央引导卡，不弹模态）。
            if (state.model.nodes.empty()) {
                ui.text("workflow.canvas.emptyTitle")
                    .position(0.0f, height * 0.5f - 26.0f)
                    .size(width, theme::kFontBase + kSpace2)
                    .text("No nodes yet")
                    .fontSize(theme::kFontBase)
                    .fontWeight(theme::kWeightMedium)
                    .color(tokens.fgSubtle)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .build();
                ui.text("workflow.canvas.emptyHint")
                    .position(0.0f, height * 0.5f + 2.0f)
                    .size(width, kFontSm + kSpace2)
                    .text("drag nodes from the palette to start")
                    .fontSize(kFontSm)
                    .color(tokens.fgSubtlest)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .build();
            }

            // 指针热区（最后合成 = 画布顶层；节点/连线为非交互视觉，全部指针
            // 语义经纯逻辑命中分发，DEC-015）。
            components::mouseArea(ui, "workflow.canvas.input")
                .size(width, height)
                .acceptedButtons(eui::PointerButton::Left | eui::PointerButton::Middle |
                                 eui::PointerButton::Right)
                .cursor(eui::CursorShape::Arrow)
                .dragThreshold(3.0f)
                .onPress([&state, engine](const components::MouseEvent& event) {
                    state.lastMouse = {event.x, event.y};
                    if (state.menuOpen) {  // 任意画布按下先收起创建菜单。
                        state.menuOpen = false;
                        state.menuFilter.set("");
                        return;
                    }
                    const int button =
                        event.button == eui::PointerButton::Middle ? 1
                        : event.button == eui::PointerButton::Right ? 2
                                                                    : 0;
                    const CanvasPoint canvas = state.toCanvas({event.x, event.y});
                    const PressResult result =
                        state.interaction.onPress(state.model, canvas, button,
                                                  event.modifiers.shift,
                                                  event.modifiers.alt);
                    if (!result.message.empty()) {
                        state.feedback = result.message;
                    }
                    if (result.graphChanged) {
                        // 右键删除节点等即时变更：与松开路径同一引擎同步出口。
                        state.afterGraphChange(engine);
                    }
                    ++state.revision;
                })
                .onDrag([&state](const components::MouseDragEvent& event) {
                    state.lastMouse = {event.x, event.y};
                    state.interaction.onDrag(state.model, state.view, {event.x, event.y},
                                             state.toCanvas({event.x, event.y}));
                    ++state.revision;
                })
                .onRelease([&state, engine](const components::MouseEvent& event) {
                    const CanvasPoint canvas = state.toCanvas({event.x, event.y});
                    const ReleaseResult result =
                        state.interaction.onRelease(state.model, canvas,
                                                    event.modifiers.shift);
                    if (!result.message.empty()) {
                        state.feedback = result.message;
                    }
                    if (result.graphChanged) {
                        state.afterGraphChange(engine);  // §5.3 画布图与引擎同步。
                    }
                    ++state.revision;
                })
                .onMove([&state](const components::MouseEvent& event) {
                    state.lastMouse = {event.x, event.y};
                    return true;
                })
                .onScroll([&state](const components::MouseScrollEvent& event) {
                    // 滚轮缩放（§5.1：以指针为锚点；上下限在 CanvasView 夹取）。
                    const float factor = std::pow(1.2f, event.stepY);
                    state.view.zoomAt(state.lastMouse, factor);
                    ++state.revision;
                })
                .build();
        })
        .build();
}

// --- 上下文面板与五区组装（M5-04 起迁移至 param_panel.hpp：参数编辑、节点
// 错误徽标与中间产物缩略图；composeWorkflowPage 同址） ---

// --- 底部：校验问题与事件列表（§4：逐条 kind + node + message，点击定位） ---
inline void composeWorkflowIssues(eui::Ui& ui, WorkflowCanvasState& state, const float x,
                                  const float y, const float width, const float height) {
    const theme::ThemeTokens& tokens = theme::dark();
    const float pad = kSpace3;
    const float titleHeight = kFontSm + kSpace1;
    const float rowHeight = 22.0f;
    constexpr std::size_t kMaxRows = 3;

    ui.stack("workflow.issues")
        .position(x, y)
        .size(width, height)
        .clip()
        .content([&] {
            ui.rect("workflow.issues.card")
                .size(width, height)
                .radius(kRadiusLg)
                .color(tokens.card)
                .border(kBorderHairline, tokens.cardBorder)
                .build();
            ui.text("workflow.issues.title")
                .position(pad, pad)
                .size(width - pad * 2.0f, titleHeight)
                .text("Validation & Events")
                .fontSize(kFontSm)
                .fontWeight(theme::kWeightSemibold)
                .color(tokens.fgSubtle)
                .build();
            ui.text("workflow.issues.event")
                .position(pad, pad + titleHeight)
                .size(width - pad * 2.0f, rowHeight)
                .text(state.lastEvent.empty() ? "engine events appear here" : state.lastEvent)
                .fontSize(kFontXs)
                .color(state.lastEvent.empty() ? tokens.fgSubtlest : tokens.fgSubtle)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();

            const std::size_t rows =
                std::min(state.model.validation.issues.size(), kMaxRows);
            float rowY = pad + titleHeight + rowHeight;
            for (std::size_t i = 0; i < rows; ++i) {
                const rin::ValidationIssue& issue = state.model.validation.issues[i];
                const std::string base = "workflow.issues.row" + std::to_string(i);
                const std::string label = std::string("[") +
                                          validationKindLabel(issue.kind) + "] " +
                                          issue.message;
                const bool locatable = issue.node != rin::kInvalidNode &&
                                       state.model.findNode(issue.node) != nullptr;
                const eui::Color transparent{0.0f, 0.0f, 0.0f, 0.0f};
                ui.rect(base)
                    .position(pad, rowY)
                    .size(width - pad * 2.0f, rowHeight)
                    .radius(kRadiusSm)
                    .color(transparent)
                    .states(transparent, locatable ? tokens.menuHover : transparent,
                            locatable ? tokens.menuHover : transparent)
                    .onClick([&state, issue] {
                        if (issue.node == rin::kInvalidNode) {
                            return;
                        }
                        if (const CanvasNode* node = state.model.findNode(issue.node);
                            node != nullptr) {
                            // 点击定位（§4）：视图定心到问题节点。
                            const rin::NodeDescriptor* descriptor =
                                state.model.descriptorFor(*node);
                            const float h =
                                descriptor != nullptr ? nodeHeight(*descriptor) : 40.0f;
                            centerViewOn(state.view,
                                         {node->position.x + kNodeWidth * 0.5f,
                                          node->position.y + h * 0.5f},
                                         state.viewport.x, state.viewport.y);
                            state.feedback = "located node " + std::to_string(issue.node);
                            ++state.revision;
                        }
                    })
                    .build();
                ui.text(base + ".label")
                    .position(pad + kSpace2, rowY)
                    .size(width - pad * 2.0f - kSpace2, rowHeight)
                    .text(label)
                    .fontSize(kFontXs)
                    .color(tokens.warning)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .maxWidth(width - pad * 2.0f - kSpace2)
                    .build();
                rowY += rowHeight;
            }
            if (state.model.validation.issues.size() > kMaxRows) {
                ui.text("workflow.issues.more")
                    .position(pad + kSpace2, rowY)
                    .size(width - pad * 2.0f - kSpace2, rowHeight)
                    .text("+" +
                          std::to_string(state.model.validation.issues.size() - kMaxRows) +
                          " more")
                    .fontSize(kFontXs)
                    .color(tokens.fgSubtlest)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
            }
        })
        .build();
}

}  // namespace

// --- 右键创建菜单（§5.2.2：浮层 + 即输即筛；自绘，EUI-20260928-001 先例） ---
inline void composeWorkflowCreateMenu(eui::Ui& ui, WorkflowCanvasState& state,
                                      rin::IWorkflowEngine* engine, const float windowWidth,
                                      const float windowHeight) {
    if (!state.menuOpen || state.model.catalog == nullptr) {
        return;
    }
    const theme::ThemeTokens& tokens = theme::dark();
    const float pad = kSpace2;
    const float width = 190.0f;
    const float inputHeight = 28.0f;
    const float groupTitleHeight = kFontXs + kSpace1;
    const float itemHeight = 24.0f;
    const std::vector<PaletteGroup> groups =
        paletteGroups(*state.model.catalog, state.menuFilter.get());
    std::size_t itemCount = 0;
    for (const PaletteGroup& group : groups) {
        itemCount += group.items.size();
    }
    const float height = pad * 2.0f + inputHeight + kSpace2 +
                         static_cast<float>(groups.size()) * groupTitleHeight +
                         static_cast<float>(itemCount) * itemHeight;
    const float x = std::clamp(state.menuPos.x, 8.0f, std::max(8.0f, windowWidth - width - 8.0f));
    const float y =
        std::clamp(state.menuPos.y, 8.0f, std::max(8.0f, windowHeight - height - 8.0f));

    // 全窗口透明阻挡层：菜单外任意点击收起（§5.2.2 菜单是模态浮层）。
    ui.rect("workflow.menu.scrim")
        .position(0.0f, 0.0f)
        .size(windowWidth, windowHeight)
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .onClick([&state] {
            state.menuOpen = false;
            state.menuFilter.set("");
        })
        .build();

    ui.stack("workflow.menu")
        .position(x, y)
        .size(width, height)
        .content([&] {
            ui.rect("workflow.menu.panel")
                .size(width, height)
                .radius(kRadiusLg)
                .color(tokens.menu)
                .border(kBorderHairline, tokens.border)
                .shadow(18.0f, 10.0f, 8.0f, {0.0f, 0.0f, 0.0f, 0.35f})
                .build();
            components::InputStyle inputStyle;
            inputStyle.background = tokens.input;
            inputStyle.focused = tokens.input;
            inputStyle.border = tokens.inputBorder;
            inputStyle.focusBorder = tokens.brand;
            inputStyle.text = tokens.fg;
            inputStyle.placeholder = tokens.fgSubtlest;
            inputStyle.cursor = tokens.brand;
            inputStyle.shadow = core::Shadow{};
            inputStyle.radius = kRadiusMd;
            components::input(ui, "workflow.menu.filter")
                .position(pad, pad)
                .size(width - pad * 2.0f, inputHeight)
                .bind(state.menuFilter)
                .placeholder("filter")
                .fontSize(kFontSm)
                .inset(kSpace2)
                .style(inputStyle)
                .build();

            float rowY = pad + inputHeight + kSpace2;
            for (const PaletteGroup& group : groups) {
                ui.text("workflow.menu.group." + group.title)
                    .position(pad, rowY)
                    .size(width - pad * 2.0f, groupTitleHeight)
                    .text(group.title)
                    .fontSize(kFontXs)
                    .color(tokens.fgSubtlest)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
                rowY += groupTitleHeight;
                for (const rin::NodeDescriptor* descriptor : group.items) {
                    const std::string base =
                        std::string("workflow.menu.item.") + descriptor->typeId;
                    const eui::Color transparent{0.0f, 0.0f, 0.0f, 0.0f};
                    ui.rect(base)
                        .position(pad, rowY)
                        .size(width - pad * 2.0f, itemHeight)
                        .radius(kRadiusSm)
                        .color(transparent)
                        .states(transparent, tokens.menuHover, tokens.menuHover)
                        .onClick([&state, engine, descriptor] {
                            // §5.2.2：选中后在该点创建；创建即选中（模型内）。
                            const CanvasPoint canvas = state.globalToCanvas(state.menuPos);
                            const CanvasOpResult result = state.model.createNode(
                                descriptor->typeId,
                                {canvas.x - kNodeWidth * 0.5f,
                                 canvas.y - kNodeHeaderHeight * 0.5f});
                            state.feedback = result.ok ? "" : result.error;
                            state.menuOpen = false;
                            state.menuFilter.set("");
                            state.afterGraphChange(engine);
                        })
                        .build();
                    ui.text(base + ".label")
                        .position(pad + kSpace2, rowY)
                        .size(width - pad * 2.0f - kSpace2, itemHeight)
                        .text(descriptor->displayName)
                        .fontSize(kFontCaption)
                        .color(tokens.fg)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .build();
                    rowY += itemHeight;
                }
            }
        })
        .build();
}

// --- 调色板拖拽浮层（§5.2.1：类型名跟随光标） ---
inline void composeWorkflowDragGhost(eui::Ui& ui, WorkflowCanvasState& state) {
    if (!state.paletteDragging || state.model.catalog == nullptr) {
        return;
    }
    const theme::ThemeTokens& tokens = theme::dark();
    const rin::NodeDescriptor* descriptor =
        rin::findNodeDescriptor(*state.model.catalog, state.paletteDragType);
    if (descriptor == nullptr) {
        return;
    }
    const float width = 120.0f;
    const float height = 24.0f;
    ui.stack("workflow.ghost")
        .position(state.paletteDragPos.x + 10.0f, state.paletteDragPos.y + 10.0f)
        .size(width, height)
        .content([&] {
            ui.rect("workflow.ghost.card")
                .size(width, height)
                .radius(kRadiusSm)
                .color(tokens.menu)
                .border(kBorderHairline, tokens.brand)
                .build();
            ui.text("workflow.ghost.label")
                .position(theme::kSpace2, 0.0f)
                .size(width - theme::kSpace2 * 2.0f, height)
                .text(descriptor->displayName)
                .fontSize(kFontXs)
                .color(tokens.fg)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
        })
        .build();
}

}  // namespace viewer
