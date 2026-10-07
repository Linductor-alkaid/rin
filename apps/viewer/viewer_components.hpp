#pragma once

// viewer 组合层共享脚手架（M12/CR-33、CR-36、CR-38）：卡片底+标题栏、名称/值
// 行列表、模态浮层壳的唯一实现。一律消费 viewer_theme 令牌（DEC-005）；仅
// 依赖 eui_neo 与令牌层，供 app/node_canvas/imu_panel 等组合单元复用。

#include <eui_neo.h>

#include <algorithm>
#include <string>
#include <utility>

#include "viewer_theme.hpp"

namespace viewer {

/// 卡片内容区度量（composeCardShell 返回）：内容排版所需的三个冻结量。
struct CardGeometry {
    float pad;         ///< 内容左/上内边距（kSpace3）
    float titleHeight; ///< 标题行高（kFontSm + kSpace1）
    float contentTop;  ///< 常规内容起始 y = pad + titleHeight + kSpace2
};

/// 卡片底 + 标题栏唯一脚手架（M12/CR-33，DEC-005 令牌）：在当前 stack 内容
/// 坐标系绘制卡片底与标题（kFontSm semibold fgSubtle，位于 (pad, pad)）；
/// 子元素 id 派生为 "<id>.card" / "<id>.title"。返回内容区度量。
/// radius 默认卡片层 kRadiusXl；嵌入区工具卡可用 kRadiusLg。
inline CardGeometry composeCardShell(eui::Ui& ui, const theme::ThemeTokens& tokens,
                                     const std::string& id, const char* title, float width,
                                     float height, float radius = theme::kRadiusXl) {
    const float pad = theme::kSpace3;
    const float titleHeight = theme::kFontSm + theme::kSpace1;
    ui.rect(id + ".card")
        .size(width, height)
        .radius(radius)
        .color(tokens.card)
        .border(theme::kBorderHairline, tokens.cardBorder)
        .build();
    ui.text(id + ".title")
        .position(pad, pad)
        .size(width - pad * 2.0f, titleHeight)
        .text(title)
        .fontSize(theme::kFontSm)
        .fontWeight(theme::kWeightSemibold)
        .color(tokens.fgSubtle)
        .build();
    return {pad, titleHeight, pad + titleHeight + theme::kSpace2};
}

/// 名称/值行列表度量（M12/CR-38）：值列 x 与右上角元数据小字宽（原 IMU/内参
/// 卡同款冻结布局度量 96/130，唯一化双份魔法数）。
inline constexpr float kLabeledRowsValueX = 96.0f;
inline constexpr float kLabeledRowsMetaWidth = 130.0f;

/// 名称/值行列表唯一脚手架（M12/CR-38）：行高 kFontBase + kSpace2；名称列
/// caption/medium/fgSubtle（占位至 valueX），值列 base/fg 自 valueX 起。
/// 行 id 派生为 "<idPrefix>.<rowIds[i]>.name" / ".value"。
inline void composeLabeledRows(eui::Ui& ui, const theme::ThemeTokens& tokens,
                               const std::string& idPrefix, float pad, float top, float width,
                               float valueX, std::size_t count, const char* const* rowIds,
                               const char* const* names, const std::string* values) {
    const float rowHeight = theme::kFontBase + theme::kSpace2;
    for (std::size_t index = 0; index < count; ++index) {
        const float rowY = top + static_cast<float>(index) * rowHeight;
        ui.text(idPrefix + "." + rowIds[index] + ".name")
            .position(pad, rowY)
            .size(valueX - pad - theme::kSpace2, rowHeight)
            .text(names[index])
            .fontSize(theme::kFontCaption)
            .fontWeight(theme::kWeightMedium)
            .color(tokens.fgSubtle)
            .build();
        ui.text(idPrefix + "." + rowIds[index] + ".value")
            .position(valueX, rowY)
            .size(width - valueX - pad, rowHeight)
            .text(values[index])
            .fontSize(theme::kFontBase)
            .color(tokens.fg)
            .build();
    }
}

/// 模态浮层壳唯一脚手架（M12/CR-36）：8px 边缘钳制定位 + 全窗口透明 scrim
/// （点击触发 onDismiss）+ 浮层面板（kRadiusLg / menu / hairline border /
/// 浮层阴影）。content 在面板之上合成菜单项（面板局部坐标）。
template <typename OnDismiss, typename Content>
void composeFloatingPanel(eui::Ui& ui, const theme::ThemeTokens& tokens,
                          const std::string& id, float posX, float posY, float width,
                          float height, float windowWidth, float windowHeight,
                          OnDismiss&& onDismiss, Content&& content) {
    const float x = std::clamp(posX, 8.0f, std::max(8.0f, windowWidth - width - 8.0f));
    const float y = std::clamp(posY, 8.0f, std::max(8.0f, windowHeight - height - 8.0f));

    // 全窗口透明阻挡层：菜单外任意点击收起（§5.2.2 菜单是模态浮层）。
    ui.rect(id + ".scrim")
        .position(0.0f, 0.0f)
        .size(windowWidth, windowHeight)
        .color(theme::kTransparent)
        .onClick(std::forward<OnDismiss>(onDismiss))
        .build();

    ui.stack(id)
        .position(x, y)
        .size(width, height)
        .content([&] {
            ui.rect(id + ".panel")
                .size(width, height)
                .radius(theme::kRadiusLg)
                .color(tokens.menu)
                .border(theme::kBorderHairline, tokens.border)
                .shadow(theme::kOverlayShadowOffsetX, theme::kOverlayShadowOffsetY,
                        theme::kOverlayShadowBlur, theme::kOverlayShadowColor)
                .build();
            content();
        });
}

}  // namespace viewer
