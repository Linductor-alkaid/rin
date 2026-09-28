#pragma once

// 参数面板与中间结果查看组装（M5-04，[DEC-014] 五区骨架右区 + §5.5/§5.6）。
// 依赖方向：param_panel.hpp → node_canvas.hpp（面板需要画布会话状态与选中集），
// 工作流页五区组装 composeWorkflowPage 亦由本文件承载（画布区来自
// node_canvas.hpp，面板与缩略图在本文件）。
//
// 参数编辑（§5.5）：右面板只编辑当前选中节点实例的参数；未赋值参数显示声明默认
// 值（param_model.hpp effectiveParamValue）；编辑即经 requestParamUpdate 逐参数
// 提交——同步拒绝（未知节点/参数、类型不匹配、越界）就地报错（control.error），
// 引擎接受后同步记入画布模型（图结构变更时随 applyGraph 携带，图重建不丢参数）。
// 面板顶部常驻"参数下一帧生效"提示。控件映射：Boolean→toggleSwitch、
// Integer/Real→slider+input（hasRange 时滑条归一化映射并显示区间）、
// Enumeration→dropdown（bindOpen/onOpenChange 外接开合，台账 EUI-20260923-002）、
// RealArray→自研矩阵网格（行列步进可配 + 单元文本输入，全格有限值校验）。
//
// 中间结果（§5.6）：选中节点 → 面板底部缩略图（tryLoadNodeOutput 经
// NodeOutputCache 有界缓存，每节点仅最新一幅）；缩略图降采样在提交侧完成
// （param_model.hpp thumbnailRgbaFromSnapshot，RULE-05 有界），上屏走
// GpuFrameView 绕行路径（台账 EUI-20260923-003，禁走 ImageStream）；显示分辨率
// 与源帧序号，随 sourceSequence 推进刷新。引擎非 Running 态整体排空（§4 停止/
// 关闭排空：stale 快照不得显示活动状态）。
//
// 节点错误可视化（§4）：NodeFailed 事件经 NodeFailureMarks 标注——画布节点
// destructive 徽标（composeCanvasNode）+ 消息入底部事件列表与面板徽标；
// Started/Stopped 清空（会话级）。事件通道为最新态语义，密集事件下可能漏读
// 中间事件（契约允许）。
//
// 组件接线纪律（DEC-005）：颜色字段全部取 viewer 令牌（SliderStyle/SwitchStyle/
// DropdownStyle/InputStyle 显式赋值）；组件几何度量（下拉标签字号/条目高度、
// 开关行程）沿用组件默认，与 M5-03 input 的显式字号/inset 并列记录于设计文档。
// 阴影仅用于下拉弹层浮层。slider/switch/dropdown 构建器无 position，统一包一层
// 定位 stack；下拉弹层浮于后续参数行之上依赖同父容器 zIndex（DEC-005：zIndex
// 不跨父容器，故 zIndex 加在包裹 stack 上）。控件回调按值捕获节点 id，不得捕获
// 画布节点指针（画布图变更会重排节点数组）。
//
// [DEC-014]: ../../docs/decisions/DEC-014-workbench-information-architecture.md

#include "canvas_model.hpp"
#include "gpu_frame_view.hpp"
#include "node_canvas.hpp"
#include "param_model.hpp"
#include "viewer_theme.hpp"

#include <eui_neo.h>

#include <rin/workflow_engine.hpp>
#include <rin/workflow_types.hpp>

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace viewer {

/// 单参数控件状态（选择变更时按节点描述符重建；地址经 std::map 节点稳定）。
struct ParamControlState {
    /// 标量文本（Integer/Real 输入框；Enumeration 当前选项 id）。
    eui::Signal<std::string> text;
    /// Boolean 开关状态。
    eui::Signal<bool> checked;
    /// Enumeration 下拉开合（EUI-20260923-002：必须外接状态）。
    eui::Signal<bool> open;
    /// RealArray 矩阵编辑器：形状 + 行主序值 + 单元文本（与值一一对应）。
    std::size_t matrixRows = 0;
    std::size_t matrixCols = 0;
    std::vector<double> matrixValues;
    std::vector<eui::Signal<std::string>> matrixCells;
    /// 就地报错（§5.5 同步拒绝）；空串 = 无错。
    std::string error;
};

/// 参数面板会话状态（ViewerContext 持有，地址稳定供闭包引用；DEC-014 决策 4：
/// 页面 UI 状态不因导航丢失）。缩略图缓存与 GL 上传视图随面板存续。
struct WorkflowPanelState {
    /// 控件当前绑定的节点（选择变更时 resetBindings，下次 compose 重建）。
    rin::NodeId boundNode = rin::kInvalidNode;
    std::map<std::string, ParamControlState> controls;
    /// 选中节点中间产物有界缓存（§5.6）与缩略图上传视图（§4 停止排空清空）。
    NodeOutputCache outputs;
    GpuFrameView thumbnail;
    std::string thumbMeta;

    /// 丢弃全部控件绑定（选择变更/关闭排空）。
    void resetBindings() {
        boundNode = rin::kInvalidNode;
        controls.clear();
    }
};

namespace {

using viewer::theme::kBorderHairline;
using viewer::theme::kFontBase;
using viewer::theme::kFontCaption;
using viewer::theme::kFontSm;
using viewer::theme::kFontXs;
using viewer::theme::kRadiusLg;
using viewer::theme::kRadiusMd;
using viewer::theme::kRadiusSm;
using viewer::theme::kSpace1;
using viewer::theme::kSpace2;
using viewer::theme::kSpace3;

/// 面板内联输入样式（M5-03 调色板过滤框同源：样式字段逐一取 viewer 令牌）。
inline components::InputStyle panelInputStyle(const theme::ThemeTokens& tokens) {
    components::InputStyle style;
    style.background = tokens.input;
    style.focused = tokens.input;
    style.border = tokens.inputBorder;
    style.focusBorder = tokens.brand;
    style.text = tokens.fg;
    style.placeholder = tokens.fgSubtlest;
    style.cursor = tokens.brand;
    style.shadow = core::Shadow{};
    style.radius = kRadiusMd;
    return style;
}

/// 面板编辑目标：单选且存在于画布模型的节点（多选/无选中返回 nullptr）。
[[nodiscard]] const CanvasNode* panelSelection(const CanvasGraphModel& model) {
    if (model.selection.size() != 1) {
        return nullptr;
    }
    return model.findNode(model.selection.front());
}

/// 参数编辑提交（§5.5）：纯逻辑校验 → 引擎 requestParamUpdate（同步拒绝 →
/// 就地报错；引擎优先于模型，保证模型与引擎目标图一致）→ 画布模型记账（下次
/// 图结构变更随 applyGraph 携带）。不经 afterGraphChange：参数热更新有独立
/// 通道，Running 下重复 applyGraph 会触发图重建事件刷屏（假引擎已知重建语义，
/// M4-07 对齐项）。返回是否提交成功。
[[nodiscard]] inline bool submitParamAssignment(WorkflowCanvasState& canvas,
                                                rin::IWorkflowEngine* engine,
                                                const rin::NodeId node,
                                                const rin::ParamDescriptor& descriptor,
                                                rin::ParamValue value,
                                                ParamControlState& control) {
    const ParamEditResult result = makeParamAssignment(descriptor, std::move(value));
    if (!result.ok) {
        control.error = result.error;
        return false;
    }
    if (engine != nullptr) {
        std::string error;
        if (!engine->requestParamUpdate(node, descriptor.id, result.assignment.value,
                                        &error)) {
            control.error = error.empty() ? "parameter update rejected" : std::move(error);
            return false;
        }
    }
    const CanvasOpResult applied = canvas.model.setParam(node, result.assignment);
    if (!applied.ok) {
        control.error = applied.error;
        return false;
    }
    control.error.clear();
    return true;
}

/// 选择变更时按节点描述符重建控件绑定（生效值 → 控件初值）。
inline void ensurePanelBindings(WorkflowCanvasState& canvas, WorkflowPanelState& panel,
                                const CanvasNode& node) {
    if (panel.boundNode == node.id) {
        return;
    }
    panel.resetBindings();
    panel.boundNode = node.id;
    const rin::NodeDescriptor* descriptor = canvas.model.descriptorFor(node);
    if (descriptor == nullptr) {
        return;
    }
    for (const rin::ParamDescriptor& pd : descriptor->params) {
        ParamControlState& control = panel.controls[pd.id];
        const rin::ParamValue* effective = effectiveParamValue(*descriptor, node.params, pd.id);
        if (effective == nullptr) {
            continue;  // 声明内参数必有默认值；防御性跳过。
        }
        switch (pd.kind) {
            case rin::ParamKind::Boolean:
                control.checked.set(std::get<bool>(*effective));
                break;
            case rin::ParamKind::Integer:
            case rin::ParamKind::Real:
                control.text.set(paramValueText(*effective));
                break;
            case rin::ParamKind::Enumeration:
                control.text.set(std::get<std::string>(*effective));
                control.open.set(false);
                break;
            case rin::ParamKind::RealArray: {
                const RealArrayGrid grid =
                    RealArrayGrid::fromFlat(std::get<std::vector<double>>(*effective));
                control.matrixRows = grid.rows;
                control.matrixCols = grid.cols;
                control.matrixValues = grid.values;
                control.matrixCells.resize(grid.values.size());
                for (std::size_t i = 0; i < grid.values.size(); ++i) {
                    control.matrixCells[i].set(formatRealText(grid.values[i]));
                }
                break;
            }
        }
    }
}

/// 面板底部：选中节点中间产物缩略图（§5.6）。meta 显示分辨率与源帧序号；
/// 无产物时显示引导文案（§4 空态）。
inline void composeNodeOutputBlock(eui::Ui& ui, const WorkflowPanelState& panel,
                                   const float x, const float y, const float width,
                                   const float height) {
    const theme::ThemeTokens& tokens = theme::dark();
    const float titleHeight = kFontSm + kSpace1;

    ui.stack("workflow.context.output")
        .position(x, y)
        .size(width, height)
        .content([&] {
            ui.text("workflow.context.output.title")
                .position(0.0f, 0.0f)
                .size(width * 0.5f, titleHeight)
                .text("Node output")
                .fontSize(kFontSm)
                .fontWeight(theme::kWeightSemibold)
                .color(tokens.fgSubtle)
                .build();
            ui.text("workflow.context.output.meta")
                .position(0.0f, 0.0f)
                .size(width, titleHeight)
                .text(panel.thumbMeta.empty() ? "—" : panel.thumbMeta)
                .fontSize(kFontXs)
                .color(tokens.fgSubtlest)
                .horizontalAlign(eui::HorizontalAlign::Right)
                .build();
            const float areaY = titleHeight + kSpace1;
            const float areaHeight = height - areaY;
            ui.rect("workflow.context.output.area")
                .position(0.0f, areaY)
                .size(width, areaHeight)
                .radius(kRadiusMd)
                .color(tokens.surface)
                .border(kBorderHairline, tokens.border)
                .build();
            if (panel.thumbnail.valid()) {
                ui.image("workflow.context.output.img")
                    .position(0.0f, areaY)
                    .size(width, areaHeight)
                    .texture(panel.thumbnail.image(), panel.thumbnail.revision())
                    .contain()
                    .radius(kRadiusMd)
                    .build();
            } else {
                ui.text("workflow.context.output.empty")
                    .position(0.0f, areaY)
                    .size(width, areaHeight)
                    .text("run the workflow to see output")
                    .fontSize(kFontXs)
                    .color(tokens.fgSubtlest)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
            }
        })
        .build();
}

}  // namespace

/// pump 边界的中间结果消费（RULE-05：渲染线程只做有界取快照与提交）。引擎
/// Running 时拉取单选节点的最新产物并上传缩略图（GL 当前线程）；非 Running
/// 整体排空（§4 停止/关闭排空）。返回 true 表示有可见变更（调用方需
/// requestUpdate）。
inline bool pumpNodeOutput(WorkflowCanvasState& canvas, WorkflowPanelState& panel,
                           rin::IWorkflowEngine& engine) {
    if (engine.state() != rin::WorkflowEngineState::Running) {
        if (panel.outputs.size() > 0 || panel.thumbnail.valid()) {
            panel.outputs.clear();
            panel.thumbnail.release();
            panel.thumbMeta.clear();
            return true;
        }
        return false;
    }
    const CanvasNode* node = panelSelection(canvas.model);
    if (node == nullptr) {
        return false;
    }
    if (!panel.outputs.pull(engine, node->id)) {
        return false;
    }
    const rin::NodeOutputSnapshot* snapshot = panel.outputs.find(node->id);
    if (snapshot == nullptr) {
        return false;
    }
    rin::Frame thumb;
    if (!thumbnailRgbaFromSnapshot(*snapshot, kThumbnailMaxDim, thumb)) {
        return false;
    }
    panel.thumbnail.update(thumb);
    panel.thumbMeta = std::to_string(snapshot->width) + " x " +
                      std::to_string(snapshot->height) + " · seq " +
                      std::to_string(snapshot->sourceSequence);
    return true;
}

/// 右面板：选中节点参数编辑 + 节点错误徽标 + 底部中间产物缩略图（§5.5/§5.6/
/// §4）；无选中保持 M5-02 空态文案（工作流总览归 M5-05 性能面板）。
inline void composeWorkflowContext(eui::Ui& ui, WorkflowCanvasState& canvas,
                                   WorkflowPanelState& panel, rin::IWorkflowEngine* engine,
                                   const float x, const float y, const float width,
                                   const float height) {
    const theme::ThemeTokens& tokens = theme::dark();
    const float pad = kSpace3;
    const float innerWidth = width - pad * 2.0f;
    constexpr float kOutputBlockHeight = 148.0f;
    const float outputTop = height - pad - kOutputBlockHeight;  // 面板内局部坐标。
    const float paramsLimit = outputTop - kSpace2;              // 参数行下限。

    ui.stack("workflow.context")
        .position(x, y)
        .size(width, height)
        .content([&] {
            ui.rect("workflow.context.card")
                .size(width, height)
                .radius(kRadiusLg)
                .color(tokens.card)
                .border(kBorderHairline, tokens.cardBorder)
                .build();
            ui.text("workflow.context.title")
                .position(pad, pad)
                .size(innerWidth, kFontSm + kSpace1)
                .text("Context")
                .fontSize(kFontSm)
                .fontWeight(theme::kWeightSemibold)
                .color(tokens.fgSubtle)
                .build();

            const CanvasNode* node = panelSelection(canvas.model);
            if (node == nullptr) {
                ui.text("workflow.context.empty")
                    .position(pad, pad + kFontSm + kSpace2)
                    .size(innerWidth, 40.0f)
                    .text(canvas.model.selection.empty()
                              ? "select a node to edit its parameters"
                              : std::to_string(canvas.model.selection.size()) +
                                    " nodes selected - select one to edit parameters")
                    .fontSize(kFontXs)
                    .color(tokens.fgSubtlest)
                    .build();
                return;
            }

            ensurePanelBindings(canvas, panel, *node);
            const rin::NodeDescriptor* descriptor = canvas.model.descriptorFor(*node);
            const rin::NodeId nodeId = node->id;  // 回调按值捕获，不持节点指针。
            float rowY = pad + kFontSm + kSpace2;
            if (descriptor == nullptr) {
                ui.text("workflow.context.unknownType")
                    .position(pad, rowY)
                    .size(innerWidth, kFontSm + kSpace1)
                    .text("unknown node type: " + node->typeId)
                    .fontSize(kFontSm)
                    .color(tokens.destructive)
                    .build();
                return;
            }

            // 节点头：显示名 + 实例 id（与画布节点 id 对应）。
            ui.text("workflow.context.nodeName")
                .position(pad, rowY)
                .size(innerWidth, kFontBase + kSpace1)
                .text(descriptor->displayName + " #" + std::to_string(nodeId))
                .fontSize(kFontBase)
                .fontWeight(theme::kWeightSemibold)
                .color(tokens.fg)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            rowY += kFontBase + kSpace1;

            // 节点错误可视化（§4 NodeFailed）：destructive 徽标 + 最近失败消息。
            if (const std::string* failure = canvas.failures.failureOf(nodeId);
                failure != nullptr) {
                ui.rect("workflow.context.failedBadge")
                    .position(pad, rowY)
                    .size(innerWidth, 20.0f)
                    .radius(kRadiusSm)
                    .color({0.0f, 0.0f, 0.0f, 0.0f})
                    .border(kBorderHairline, tokens.destructive)
                    .build();
                ui.text("workflow.context.failedBadge.label")
                    .position(pad + kSpace2, rowY)
                    .size(innerWidth - kSpace2 * 2.0f, 20.0f)
                    .text("node failed" + (failure->empty() ? "" : ": " + *failure))
                    .fontSize(kFontXs)
                    .color(tokens.destructive)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .maxWidth(innerWidth - kSpace2 * 2.0f)
                    .build();
                rowY += 20.0f + kSpace1;
            }

            // 常驻提示（§5.5）：运行中参数编辑"下一帧生效"。
            ui.text("workflow.context.hotHint")
                .position(pad, rowY)
                .size(innerWidth, kFontXs + kSpace1)
                .text("params apply on the next frame")
                .fontSize(kFontXs)
                .color(tokens.fgSubtlest)
                .build();
            rowY += kFontXs + kSpace2;

            const components::InputStyle inputStyle = panelInputStyle(tokens);

            // 参数行（超出参数区下限截断并计数提示，不与缩略图块重叠）。
            for (std::size_t pi = 0; pi < descriptor->params.size(); ++pi) {
                const rin::ParamDescriptor& pd = descriptor->params[pi];
                ParamControlState& control = panel.controls[pd.id];
                const std::string base = "workflow.context.param." + pd.id;

                // 行高预估（截断判定；error 行按存在与否计入）。
                float rowHeight = kFontSm + kSpace1;  // 标签行
                if (pd.kind == rin::ParamKind::Boolean) {
                    rowHeight += 22.0f;
                } else if (pd.kind == rin::ParamKind::RealArray) {
                    rowHeight += 22.0f + static_cast<float>(control.matrixRows) * 26.0f;
                } else {
                    rowHeight += (pd.hasRange ? 18.0f + kSpace1 : 0.0f) + 26.0f;
                }
                if (!control.error.empty()) {
                    rowHeight += kFontXs + kSpace1;
                }
                rowHeight += kSpace2;
                if (rowY + rowHeight > paramsLimit) {
                    ui.text("workflow.context.moreParams")
                        .position(pad, rowY)
                        .size(innerWidth, kFontXs + kSpace1)
                        .text("+" + std::to_string(descriptor->params.size() - pi) +
                              " more parameters")
                        .fontSize(kFontXs)
                        .color(tokens.fgSubtlest)
                        .build();
                    break;
                }

                // 标签行：label 左，区间/类型呈现右（§5.5"显示区间"）。
                std::string rangeText = paramRangeText(pd);
                if (rangeText.empty()) {
                    if (pd.kind == rin::ParamKind::Enumeration) {
                        rangeText = "enum";
                    } else if (pd.kind == rin::ParamKind::RealArray) {
                        rangeText = std::to_string(control.matrixRows) + " x " +
                                    std::to_string(control.matrixCols);
                    }
                }
                ui.text(base + ".label")
                    .position(pad, rowY)
                    .size(innerWidth * 0.5f, kFontSm + kSpace1)
                    .text(pd.label)
                    .fontSize(kFontSm)
                    .color(tokens.fgSubtle)
                    .build();
                ui.text(base + ".range")
                    .position(pad, rowY)
                    .size(innerWidth, kFontSm + kSpace1)
                    .text(rangeText)
                    .fontSize(kFontXs)
                    .color(tokens.fgSubtlest)
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .build();
                rowY += kFontSm + kSpace1;

                switch (pd.kind) {
                    case rin::ParamKind::Boolean: {
                        components::SwitchStyle switchStyle;
                        switchStyle.off = tokens.input;
                        switchStyle.on = tokens.brand;
                        switchStyle.knob = tokens.fg;
                        switchStyle.text = tokens.fg;
                        switchStyle.rowHover = tokens.menuHover;
                        switchStyle.rowPressed = tokens.menuHover;
                        ui.stack(base + ".switchWrap")
                            .position(pad, rowY)
                            .size(innerWidth, 22.0f)
                            .content([&] {
                                components::toggleSwitch(ui, base + ".switch")
                                    .size(innerWidth, 22.0f)
                                    .trackSize(36.0f, 18.0f)
                                    .fontSize(kFontXs)
                                    .style(switchStyle)
                                    .checked(control.checked.get())
                                    .onChange([&canvas, engine, nodeId, &pd,
                                               &control](const bool value) {
                                        if (submitParamAssignment(canvas, engine, nodeId, pd,
                                                                  value, control)) {
                                            control.checked.set(value);
                                        }
                                    })
                                    .build();
                            })
                            .build();
                        rowY += 22.0f;
                        break;
                    }
                    case rin::ParamKind::Integer:
                    case rin::ParamKind::Real: {
                        if (pd.hasRange) {
                            // 滑条：归一化值域映射，Integer 就近取整（§5.5 夹取）。
                            const float normalized = static_cast<float>(
                                valueToSlider(
                                    pd, *effectiveParamValue(*descriptor, node->params, pd.id))
                                    .value_or(0.0));
                            components::SliderStyle sliderStyle;
                            sliderStyle.track = tokens.input;
                            sliderStyle.fill = tokens.brand;
                            sliderStyle.knob = tokens.fg;
                            ui.stack(base + ".sliderWrap")
                                .position(pad, rowY)
                                .size(innerWidth, 18.0f)
                                .content([&] {
                                    components::slider(ui, base + ".slider")
                                        .size(innerWidth, 18.0f)
                                        .value(normalized)
                                        .style(sliderStyle)
                                        .onChange([&canvas, engine, nodeId, &pd,
                                                   &control](const float t) {
                                            const std::optional<double> value =
                                                sliderToValue(pd, t);
                                            if (!value) {
                                                return;
                                            }
                                            if (pd.kind == rin::ParamKind::Integer) {
                                                const auto integer = static_cast<std::int64_t>(
                                                    std::llround(*value));
                                                if (submitParamAssignment(canvas, engine,
                                                                          nodeId, pd, integer,
                                                                          control)) {
                                                    control.text.set(std::to_string(integer));
                                                }
                                            } else if (submitParamAssignment(
                                                           canvas, engine, nodeId, pd, *value,
                                                           control)) {
                                                control.text.set(formatRealText(*value));
                                            }
                                        })
                                        .build();
                                })
                                .build();
                            rowY += 18.0f + kSpace1;
                        }
                        // 数值输入：即输即校验（§5.5 就地报错；合法即提交）。
                        components::input(ui, base + ".input")
                            .position(pad, rowY)
                            .size(innerWidth, 26.0f)
                            .value(control.text.get())
                            .placeholder(paramValueText(pd.defaultValue))
                            .fontSize(kFontSm)
                            .inset(kSpace2)
                            .style(inputStyle)
                            .onChange([&canvas, engine, nodeId, &pd,
                                       &control](const std::string& text) {
                                control.text.set(text);
                                const ParamEditResult result =
                                    paramAssignmentFromText(pd, text);
                                if (!result.ok) {
                                    control.error = result.error;
                                    return;
                                }
                                (void)submitParamAssignment(canvas, engine, nodeId, pd,
                                                            result.assignment.value, control);
                            })
                            .build();
                        rowY += 26.0f;
                        break;
                    }
                    case rin::ParamKind::Enumeration: {
                        components::DropdownStyle dropdownStyle;
                        dropdownStyle.field = tokens.input;
                        dropdownStyle.fieldHover = tokens.input;
                        dropdownStyle.fieldPressed = tokens.input;
                        dropdownStyle.popup = tokens.menu;
                        dropdownStyle.optionHover = tokens.menuHover;
                        dropdownStyle.optionPressed = tokens.menuHover;
                        dropdownStyle.selected = tokens.accentSurface;
                        dropdownStyle.text = tokens.fg;
                        dropdownStyle.mutedText = tokens.fgSubtlest;
                        dropdownStyle.accent = tokens.brand;
                        dropdownStyle.border = tokens.inputBorder;
                        dropdownStyle.shadow = core::Shadow{true, {0.0f, 10.0f}, 18.0f, 8.0f,
                                                            {0.0f, 0.0f, 0.0f, 0.35f}};
                        dropdownStyle.radius = kRadiusLg;
                        int selectedIndex = 0;
                        for (std::size_t i = 0; i < pd.enumOptions.size(); ++i) {
                            if (pd.enumOptions[i] == control.text.get()) {
                                selectedIndex = static_cast<int>(i);
                                break;
                            }
                        }
                        // 包裹 stack 持有定位与弹层层级（展开时抬升，盖过后续参数
                        // 行；DEC-005：zIndex 同父容器内生效）。
                        ui.stack(base + ".dropdownWrap")
                            .position(pad, rowY)
                            .size(innerWidth, 26.0f)
                            .zIndex(control.open.get() ? 40 : 0)
                            .content([&] {
                                components::dropdown(ui, base + ".dropdown")
                                    .size(innerWidth, 26.0f)
                                    .itemHeight(24.0f)
                                    .items(pd.enumOptions)
                                    .selected(selectedIndex)
                                    .bindOpen(control.open)
                                    .style(dropdownStyle)
                                    .onChange([&canvas, engine, nodeId, &pd,
                                               &control](const int index) {
                                        if (index < 0 ||
                                            index >= static_cast<int>(pd.enumOptions.size())) {
                                            return;
                                        }
                                        const std::string& option =
                                            pd.enumOptions[static_cast<std::size_t>(index)];
                                        if (submitParamAssignment(canvas, engine, nodeId, pd,
                                                                  option, control)) {
                                            control.text.set(option);
                                        }
                                    })
                                    .build();
                            })
                            .build();
                        rowY += 26.0f;
                        break;
                    }
                    case rin::ParamKind::RealArray: {
                        // 矩阵网格编辑器（§5.5）：行列步进 + 单元文本，全格有限值。
                        ui.text(base + ".shape")
                            .position(pad, rowY)
                            .size(innerWidth - 4.0f * 38.0f, 18.0f)
                            .text(std::to_string(control.matrixRows) + " x " +
                                  std::to_string(control.matrixCols))
                            .fontSize(kFontXs)
                            .color(tokens.fgSubtlest)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .build();
                        constexpr float kStepperWidth = 34.0f;
                        const char* stepperLabels[] = {"+R", "-R", "+C", "-C"};
                        for (int s = 0; s < 4; ++s) {
                            const float stepperX =
                                pad + innerWidth -
                                (4.0f - static_cast<float>(s)) * (kStepperWidth + kSpace1);
                            ui.rect(base + ".stepper" + std::to_string(s))
                                .position(stepperX, rowY)
                                .size(kStepperWidth, 18.0f)
                                .radius(kRadiusSm)
                                .color(tokens.input)
                                .border(kBorderHairline, tokens.inputBorder)
                                .states(tokens.input, tokens.inputBorderHover,
                                        tokens.inputBorderHover)
                                .onClick([&canvas, engine, nodeId, &pd, &control, s] {
                                    RealArrayGrid grid;
                                    grid.rows = control.matrixRows;
                                    grid.cols = control.matrixCols;
                                    grid.values = control.matrixValues;
                                    grid.reshape(grid.rows + (s == 0 ? 1 : s == 1 ? -1 : 0),
                                                 grid.cols + (s == 2 ? 1 : s == 3 ? -1 : 0));
                                    control.matrixRows = grid.rows;
                                    control.matrixCols = grid.cols;
                                    control.matrixValues = grid.values;
                                    control.matrixCells.resize(grid.values.size());
                                    for (std::size_t i = 0; i < grid.values.size(); ++i) {
                                        control.matrixCells[i].set(
                                            formatRealText(grid.values[i]));
                                    }
                                    (void)submitParamAssignment(canvas, engine, nodeId, pd,
                                                                grid.values, control);
                                })
                                .build();                            ui.text(base + ".stepperText" + std::to_string(s))
                                .position(stepperX, rowY)
                                .size(kStepperWidth, 18.0f)
                                .text(stepperLabels[s])
                                .fontSize(kFontXs)
                                .color(tokens.fgSubtle)
                                .horizontalAlign(eui::HorizontalAlign::Center)
                                .verticalAlign(eui::VerticalAlign::Center)
                                .build();
                        }
                        rowY += 18.0f + kSpace1;
                        // 单元网格（行列数可配；单元宽度随列数均分）。
                        const float cellGap = kSpace1;
                        const float cellWidth =
                            (innerWidth -
                             static_cast<float>(control.matrixCols - 1) * cellGap) /
                            static_cast<float>(control.matrixCols);
                        for (std::size_t r = 0; r < control.matrixRows; ++r) {
                            for (std::size_t c = 0; c < control.matrixCols; ++c) {
                                const std::size_t index = r * control.matrixCols + c;
                                components::input(ui, base + ".cell" + std::to_string(index))
                                    .position(
                                        pad + static_cast<float>(c) * (cellWidth + cellGap),
                                        rowY)
                                    .size(cellWidth, 22.0f)
                                    .value(control.matrixCells[index].get())
                                    .fontSize(kFontXs)
                                    .inset(kSpace1)
                                    .style(inputStyle)
                                    .onChange([&canvas, engine, nodeId, &pd, &control,
                                               index](const std::string& text) {
                                        control.matrixCells[index].set(text);
                                        std::vector<double> values;
                                        values.reserve(control.matrixCells.size());
                                        bool parsed = true;
                                        for (std::size_t i = 0; i < control.matrixCells.size();
                                             ++i) {
                                            const std::optional<double> value =
                                                parseRealText(control.matrixCells[i].get());
                                            if (!value) {
                                                parsed = false;
                                                break;
                                            }
                                            values.push_back(*value);
                                        }
                                        if (!parsed) {
                                            control.error = "values must be finite numbers";
                                            return;
                                        }
                                        (void)submitParamAssignment(canvas, engine, nodeId,
                                                                    pd, values, control);
                                    })
                                    .build();
                            }
                            rowY += 22.0f + kSpace1;
                        }
                        rowY -= kSpace1;
                        break;
                    }
                }

                // 就地报错（§5.5 同步拒绝显式呈现；不静默失败）。
                if (!control.error.empty()) {
                    ui.text(base + ".error")
                        .position(pad, rowY)
                        .size(innerWidth, kFontXs + kSpace1)
                        .text(control.error)
                        .fontSize(kFontXs)
                        .color(tokens.destructive)
                        .maxWidth(innerWidth)
                        .build();
                    rowY += kFontXs + kSpace1;
                }
                rowY += kSpace2;
            }

            // 底部：选中节点中间产物缩略图（面板底部锚定，§2"底部：选中节点快照"）。
            composeNodeOutputBlock(ui, panel, pad, outputTop, innerWidth, kOutputBlockHeight);
        })
        .build();
}

// --- 工作流页五区组装（几何与 M5-02 骨架一致；面板最后合成，下拉弹层可浮于
// 底部列表之上） ---
inline void composeWorkflowPage(eui::Ui& ui, WorkflowCanvasState& state,
                                WorkflowPanelState& panel, rin::IWorkflowEngine* engine,
                                const float x, const float y, const float width,
                                const float height) {
    const float gap = theme::kSpace3;
    const float toolbarHeight = 44.0f;
    const float bottomHeight = 120.0f;
    const float paletteWidth = 200.0f;
    const float contextWidth = 264.0f;
    const float bodyTop = y + toolbarHeight + gap;
    const float bodyHeight = height - toolbarHeight - bottomHeight - gap * 2.0f;
    const float canvasWidth = width - paletteWidth - contextWidth - gap * 2.0f;

    composeWorkflowToolbar(ui, state, x, y, width, toolbarHeight);
    composeWorkflowPalette(ui, state, engine, x, bodyTop, paletteWidth, bodyHeight);
    composeWorkflowCanvas(ui, state, engine, x + paletteWidth + gap, bodyTop, canvasWidth,
                          bodyHeight);
    composeWorkflowIssues(ui, state, x, y + height - bottomHeight, width, bottomHeight);
    composeWorkflowContext(ui, state, panel, engine, x + width - contextWidth, bodyTop,
                           contextWidth, bodyHeight);
}

}  // namespace viewer
