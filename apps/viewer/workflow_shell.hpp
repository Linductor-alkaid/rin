#pragma once

// 图像工作流页五区骨架（M5-02 页面框架，[DEC-014] 决策 2）：工具栏（顶）/ 节点
// 调色板（左）/ 节点画布（中）/ 上下文面板（右）/ 校验与事件列表（底）。
//
// M5-02 只交付分区骨架与空态文案（空图引导语按 ui_workspace_design.md §4）；
// 引擎接入（IWorkflowEngine 状态徽标、运行控制）、调色板数据源（NodeCatalog）
// 与画布交互自 M5-03 起逐区替换本骨架，替换时保持五区几何不变。
//
// 边界（RULE-05/07）：纯静态布局提交（有界 rect/text），无状态、无通道消费。
//
// [DEC-014]: ../../docs/decisions/DEC-014-workbench-information-architecture.md

#include "viewer_theme.hpp"

#include <eui_neo.h>

namespace viewer {

/// 合成工作流页五区骨架。(x, y, width, height) 为页面内容区（相对根 stack）。
inline void composeWorkflowShell(eui::Ui& ui, float x, float y, float width, float height) {
    using namespace viewer::theme;  // 语义令牌（DEC-005）；函数内引入，不泄漏头文件作用域
    const ThemeTokens& tokens = dark();
    const float gap = kSpace3;
    const float toolbarHeight = 44.0f;
    const float bottomHeight = 120.0f;
    const float paletteWidth = 200.0f;
    const float contextWidth = 264.0f;
    const float bodyTop = y + toolbarHeight + gap;
    const float bodyHeight = height - toolbarHeight - bottomHeight - gap * 2.0f;
    const float canvasWidth =
        width - paletteWidth - contextWidth - gap * 2.0f;

    // 顶：工具栏（启动/停止、引擎状态徽标、校验状态入口——M5-03+ 接入）。
    ui.stack("workflow.toolbar")
        .position(x, y)
        .size(width, toolbarHeight)
        .content([&] {
            ui.rect("workflow.toolbar.card")
                .size(width, toolbarHeight)
                .radius(kRadiusLg)
                .color(tokens.card)
                .border(kBorderHairline, tokens.cardBorder)
                .build();
            ui.text("workflow.toolbar.title")
                .position(kSpace3, 0.0f)
                .size(200.0f, toolbarHeight)
                .text("Workflow")
                .fontSize(kFontBase)
                .fontWeight(kWeightSemibold)
                .color(tokens.fg)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            ui.text("workflow.toolbar.hint")
                .position(width - 320.0f - kSpace3, 0.0f)
                .size(320.0f, toolbarHeight)
                .text("run controls appear with the node editor")
                .fontSize(kFontSm)
                .color(tokens.fgSubtlest)
                .horizontalAlign(eui::HorizontalAlign::Right)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
        })
        .build();

    // 左：节点调色板（NodeCatalog 分组 + 过滤——M5-03 接入）。
    ui.stack("workflow.palette")
        .position(x, bodyTop)
        .size(paletteWidth, bodyHeight)
        .content([&] {
            ui.rect("workflow.palette.card")
                .size(paletteWidth, bodyHeight)
                .radius(kRadiusLg)
                .color(tokens.card)
                .border(kBorderHairline, tokens.cardBorder)
                .build();
            ui.text("workflow.palette.title")
                .position(kSpace3, kSpace3)
                .size(paletteWidth - kSpace3 * 2.0f, kFontSm + kSpace1)
                .text("Palette")
                .fontSize(kFontSm)
                .fontWeight(kWeightSemibold)
                .color(tokens.fgSubtle)
                .build();
            ui.text("workflow.palette.empty")
                .position(kSpace3, kSpace3 + kFontSm + kSpace2)
                .size(paletteWidth - kSpace3 * 2.0f, 40.0f)
                .text("node types appear here")
                .fontSize(kFontXs)
                .color(tokens.fgSubtlest)
                .build();
        })
        .build();

    // 中：节点画布（空图引导卡——真实空态，§4；画布交互 M5-03 接入）。
    ui.stack("workflow.canvas")
        .position(x + paletteWidth + gap, bodyTop)
        .size(canvasWidth, bodyHeight)
        .content([&] {
            ui.rect("workflow.canvas.area")
                .size(canvasWidth, bodyHeight)
                .radius(kRadiusLg)
                .color(tokens.surface)
                .border(kBorderHairline, tokens.border)
                .build();
            ui.text("workflow.canvas.emptyTitle")
                .position(0.0f, bodyHeight * 0.5f - 26.0f)
                .size(canvasWidth, kFontBase + kSpace2)
                .text("No nodes yet")
                .fontSize(kFontBase)
                .fontWeight(kWeightMedium)
                .color(tokens.fgSubtle)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .build();
            ui.text("workflow.canvas.emptyHint")
                .position(0.0f, bodyHeight * 0.5f + 2.0f)
                .size(canvasWidth, kFontSm + kSpace2)
                .text("drag nodes from the palette to start")
                .fontSize(kFontSm)
                .color(tokens.fgSubtlest)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .build();
        })
        .build();

    // 右：上下文面板（参数编辑 | 工作流总览 + 选中节点快照——M5-04 接入）。
    ui.stack("workflow.context")
        .position(x + width - contextWidth, bodyTop)
        .size(contextWidth, bodyHeight)
        .content([&] {
            ui.rect("workflow.context.card")
                .size(contextWidth, bodyHeight)
                .radius(kRadiusLg)
                .color(tokens.card)
                .border(kBorderHairline, tokens.cardBorder)
                .build();
            ui.text("workflow.context.title")
                .position(kSpace3, kSpace3)
                .size(contextWidth - kSpace3 * 2.0f, kFontSm + kSpace1)
                .text("Context")
                .fontSize(kFontSm)
                .fontWeight(kWeightSemibold)
                .color(tokens.fgSubtle)
                .build();
            ui.text("workflow.context.empty")
                .position(kSpace3, kSpace3 + kFontSm + kSpace2)
                .size(contextWidth - kSpace3 * 2.0f, 40.0f)
                .text("select a node to edit its parameters")
                .fontSize(kFontXs)
                .color(tokens.fgSubtlest)
                .build();
        })
        .build();

    // 底：校验问题与引擎事件列表（可折叠——M5-03+ 接入）。
    ui.stack("workflow.events")
        .position(x, y + height - bottomHeight)
        .size(width, bottomHeight)
        .content([&] {
            ui.rect("workflow.events.card")
                .size(width, bottomHeight)
                .radius(kRadiusLg)
                .color(tokens.card)
                .border(kBorderHairline, tokens.cardBorder)
                .build();
            ui.text("workflow.events.title")
                .position(kSpace3, kSpace3)
                .size(width - kSpace3 * 2.0f, kFontSm + kSpace1)
                .text("Validation & Events")
                .fontSize(kFontSm)
                .fontWeight(kWeightSemibold)
                .color(tokens.fgSubtle)
                .build();
            ui.text("workflow.events.empty")
                .position(kSpace3, kSpace3 + kFontSm + kSpace2)
                .size(width - kSpace3 * 2.0f, kFontSm + kSpace2)
                .text("graph issues and engine events appear here")
                .fontSize(kFontXs)
                .color(tokens.fgSubtlest)
                .build();
        })
        .build();
}

}  // namespace viewer
