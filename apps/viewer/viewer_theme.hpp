#pragma once

// viewer 视觉令牌：将 ZCode Design System 的语义令牌体系翻译为 EUI-NEO DSL 值
// （DEC-005）。规则：
// - 一律引用此处的语义令牌，禁止在布局代码里写一次性颜色/字号/圆角（Design System
//   "Color Usage Rules"/"Typography rules"）；
// - 字阶对应 text-ui-*（--ui-font-size 默认 14px），圆角遵循容器层级 xl→lg→md→sm，
//   间距采用 4px 基准节奏；
// - 深浅层级靠背景对比与 1px 边框表达，阴影只用于浮层（下拉弹层）。

#include <eui_neo.h>

#include <rin/camera_types.hpp>

namespace viewer::theme {

// --- 字阶（text-ui-* 翻译） ---
constexpr float kFontXl = 18.0f;       // text-ui-xl：一级阅读标题
constexpr float kFontLg = 16.0f;       // text-ui-lg：二级标题
constexpr float kFontBase = 14.0f;     // text-ui-base：正文/常用控件/面板标题
constexpr float kFontCaption = 13.0f;  // text-ui-caption：次级说明
constexpr float kFontSm = 12.0f;       // text-ui-sm：辅助信息
constexpr float kFontXs = 10.0f;       // text-ui-xs：徽标/极弱元数据

// --- 字重 ---
constexpr int kWeightRegular = 400;
constexpr int kWeightMedium = 500;
constexpr int kWeightSemibold = 600;

// --- 间距（4px 基准节奏） ---
constexpr float kSpace1 = 4.0f;
constexpr float kSpace2 = 8.0f;
constexpr float kSpace3 = 12.0f;
constexpr float kSpace4 = 16.0f;
constexpr float kSpace6 = 24.0f;

// --- 圆角层级（容器层级：首个圆角容器 xl，向下 lg→md→sm，sm 为下限） ---
constexpr float kRadiusXl = 12.0f;  // 卡片/面板
constexpr float kRadiusLg = 8.0f;   // 基础控件（下拉触发器等）
constexpr float kRadiusMd = 6.0f;   // 卡内嵌套容器/菜单项
constexpr float kRadiusSm = 4.0f;   // 下限

constexpr float kBorderHairline = 1.0f;

// --- 语义色令牌（深色主题；对应 Zai Dark 的角色翻译） ---
struct ThemeTokens {
    eui::Color background;     // 页面根
    eui::Color card;           // 标准内容卡
    eui::Color cardBorder;     // 卡片描边
    eui::Color surface;        // 低强调承载面（画面区）
    eui::Color border;         // 默认分隔
    eui::Color input;          // 可编辑字段底
    eui::Color inputBorder;
    eui::Color inputBorderHover;
    eui::Color menu;           // 下拉浮层底
    eui::Color menuHover;      // 菜单项悬停
    eui::Color fg;             // 主文本
    eui::Color fgSubtle;       // 次级文本
    eui::Color fgSubtlest;     // 极弱元数据
    eui::Color brand;          // 品牌强调（稀用：选中态/关键指示）
    eui::Color accentSurface;  // 弱强调底（选中高亮）
    eui::Color success;
    eui::Color warning;
    eui::Color destructive;
};

inline const ThemeTokens& dark() {
    static const ThemeTokens tokens{
        .background = {0.063f, 0.063f, 0.071f, 1.0f},
        .card = {0.110f, 0.110f, 0.122f, 1.0f},
        .cardBorder = {0.200f, 0.200f, 0.220f, 1.0f},
        .surface = {0.085f, 0.085f, 0.094f, 1.0f},
        .border = {0.180f, 0.180f, 0.200f, 1.0f},
        .input = {0.140f, 0.140f, 0.155f, 1.0f},
        .inputBorder = {0.220f, 0.220f, 0.243f, 1.0f},
        .inputBorderHover = {0.300f, 0.300f, 0.330f, 1.0f},
        .menu = {0.150f, 0.150f, 0.165f, 1.0f},
        .menuHover = {0.205f, 0.205f, 0.225f, 1.0f},
        .fg = {0.920f, 0.920f, 0.935f, 1.0f},
        .fgSubtle = {0.620f, 0.620f, 0.660f, 1.0f},
        .fgSubtlest = {0.420f, 0.420f, 0.460f, 1.0f},
        .brand = {0.450f, 0.550f, 0.980f, 1.0f},
        .accentSurface = {0.450f, 0.550f, 0.980f, 0.16f},
        .success = {0.350f, 0.750f, 0.470f, 1.0f},
        .warning = {0.850f, 0.650f, 0.250f, 1.0f},
        .destructive = {0.880f, 0.360f, 0.360f, 1.0f},
    };
    return tokens;
}

/// 服务状态 → 语义色（仅真实语义状态使用语义色；非法借用视为设计缺陷）。
inline eui::Color stateColor(const rin::CameraServiceState state) {
    const ThemeTokens& tokens = dark();
    switch (state) {
        case rin::CameraServiceState::Streaming:
            return tokens.success;
        case rin::CameraServiceState::Restreaming:
        case rin::CameraServiceState::Opening:
            return tokens.warning;
        case rin::CameraServiceState::Failed:
            return tokens.destructive;
        case rin::CameraServiceState::Idle:
        case rin::CameraServiceState::Stopping:
            return tokens.fgSubtle;
    }
    return tokens.fgSubtle;
}

}  // namespace viewer::theme
