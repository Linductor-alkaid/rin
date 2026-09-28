#pragma once

// 工作台导航壳（M5-02，[DEC-014]）：单窗口四页导航（预览 / 位姿 / 图像工作流 /
// 设置）的页面模型与左窄边导航栏。
//
// 页面切换语义（DEC-014 决策 1）：
// - 切页只推进 NavigationState::current（纯逻辑，平台无关单测对象）；各页 UI
//   状态由 ViewerContext / ui.state 持有，导航不触碰、不复位（页面切换状态
//   保持的结构保证：NavigationState 不拥有任何页面内状态）；
// - 相机服务运行态全局共享，切页不停止采集（app.cpp 的 pump()/shutdown()
//   不分页，EXEC-04 关闭顺序不变）。
//
// 控件选型（设计文档 §3/§6 要求 M5-02 原型验证，结论见台账 EUI-20260928-001）：
// EUI-NEO `sidebar` 为右锚定模态抽屉、`tabs`/`segmented` 为横向选择器，均无
// 左窄边导航形态；`navbar` 未列入上游《组件》文档且绑定组件库自有主题度量体系
// （与 DEC-005 viewer_theme 唯一令牌层冲突）。故以 rect/text 原语 + viewer
// 令牌自绘窄边导航栏（沿用 app.cpp composeSelect 自研先例）。图标取自框架捆绑
// 的 Font Awesome 7 Free Solid（codepoint 已对照字体 cmap 验证存在）。
//
// [DEC-014]: ../../docs/decisions/DEC-014-workbench-information-architecture.md

#include "viewer_theme.hpp"

#include <eui_neo.h>

#include <functional>

namespace viewer {

/// 工作台页面（DEC-014 决策 1 的四页全集；表序即导航栏自上而下顺序）。
enum class WorkbenchPage {
    Preview,   ///< 预览：RGB/深度画面、设备与分辨率选择、内参。
    Pose,      ///< 位姿：3D 位姿视图 + IMU 状态面板。
    Workflow,  ///< 图像工作流：五区骨架（DEC-014 决策 2，M5-03 起填充）。
    Settings,  ///< 设置：深度配色（DEC-007）与关于/版本信息。
};

/// 页面元数据：稳定 id（compose 元素 id / 未来状态键前缀）、导航栏文字与图标。
struct WorkbenchPageInfo {
    WorkbenchPage page;
    const char* id;
    const char* label;
    unsigned int icon;  ///< Font Awesome 7 Free Solid codepoint（图标字体见文件头）。
};

/// 四页元数据表（顺序即导航栏顺序；页数、序号与 id 稳定性由单测锁定）。
inline constexpr WorkbenchPageInfo kWorkbenchPages[] = {
    {WorkbenchPage::Preview,  "preview",  "Preview",  0xF06E},  // fa-eye
    {WorkbenchPage::Pose,     "pose",     "Pose",     0xF1B2},  // fa-cube
    {WorkbenchPage::Workflow, "workflow", "Workflow", 0xF0E8},  // fa-sitemap
    {WorkbenchPage::Settings, "settings", "Settings", 0xF013},  // fa-gear
};

inline constexpr int kWorkbenchPageCount =
    static_cast<int>(sizeof(kWorkbenchPages) / sizeof(kWorkbenchPages[0]));

/// 页面 → 元数据表序号；越界/非法值归 0（首页，防未枚举值）。
[[nodiscard]] inline constexpr int workbenchPageIndex(WorkbenchPage page) {
    const int index = static_cast<int>(page);
    return (index >= 0 && index < kWorkbenchPageCount) ? index : 0;
}

/// 页面稳定 id（kWorkbenchPages 表内项）。
[[nodiscard]] inline constexpr const char* workbenchPageId(WorkbenchPage page) {
    return kWorkbenchPages[workbenchPageIndex(page)].id;
}

/// 导航状态：只拥有"当前页"。四页全部恒可达（DEC-014：页面间无依赖与准入），
/// 因此 switchTo 为全函数（范围外的枚举值经 workbenchPageIndex 归一为首页，
/// current 永不携带非法值）；同页重复选择是 no-op（不触发无谓重绘语义）。
struct NavigationState {
    WorkbenchPage current = WorkbenchPage::Preview;

    /// 切换到目标页；返回是否发生切换（同页返回 false 且状态不变）。
    bool switchTo(WorkbenchPage page) {
        const WorkbenchPage target = kWorkbenchPages[workbenchPageIndex(page)].page;
        if (target == current) {
            return false;
        }
        current = target;
        return true;
    }

    /// 导航栏选中序号（kWorkbenchPages 表序，受控渲染的选中依据）。
    [[nodiscard]] int selectedIndex() const { return workbenchPageIndex(current); }
};

// --- 导航栏组装（EUI-NEO 原语 + viewer 令牌；视觉验收归真机回归，见 M5-07） ---

/// 左窄边导航栏宽度（图标 + 文字条目的最小可读宽度，4px 节奏）。
inline constexpr float kNavRailWidth = 76.0f;

/// 合成左窄边导航栏：顶部品牌标记 + 四页条目（图标+文字）。条目为受控渲染：
/// 选中态由 NavigationState::current 驱动（accentSurface 底 + brand 图标文字，
/// DEC-005 弱强调语义），非选中条目经 states() 提供悬停反馈。onSelect 在点击
/// 时携带目标页（是否切页由 NavigationState::switchTo 裁决）。
inline void composeNavRail(eui::Ui& ui, const NavigationState& nav, float height,
                           const std::function<void(WorkbenchPage)>& onSelect) {
    using namespace viewer::theme;  // 语义令牌（DEC-005）；函数内引入，不泄漏头文件作用域
    const ThemeTokens& tokens = dark();
    const float brandSize = 20.0f;
    const float brandY = kSpace3;
    const float itemsTop = brandY + brandSize + kSpace4;
    const float itemHeight = 52.0f;
    const float itemWidth = kNavRailWidth - kSpace2 * 2.0f;
    const float itemX = kSpace2;
    const float iconHeight = 24.0f;
    const float labelHeight = kFontXs + kSpace1;

    ui.stack("nav.rail")
        .position(0.0f, 0.0f)
        .size(kNavRailWidth, height)
        .content([&] {
            ui.rect("nav.rail.background")
                .size(kNavRailWidth, height)
                .color(tokens.card)
                .build();
            ui.rect("nav.rail.edge")
                .position(kNavRailWidth - kBorderHairline, 0.0f)
                .size(kBorderHairline, height)
                .color(tokens.cardBorder)
                .build();

            // 品牌标记（应用即相机应用：fa-video）。
            ui.text("nav.rail.brand")
                .position(0.0f, brandY)
                .size(kNavRailWidth, brandSize)
                .icon(0xF03D)  // fa-video
                .fontSize(brandSize)
                .color(tokens.brand)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();

            for (int index = 0; index < kWorkbenchPageCount; ++index) {
                const WorkbenchPageInfo& info = kWorkbenchPages[index];
                const bool active = index == nav.selectedIndex();
                const float itemY = itemsTop + static_cast<float>(index) * itemHeight;
                const std::string id = std::string("nav.rail.item.") + info.id;
                const eui::Color transparent{0.0f, 0.0f, 0.0f, 0.0f};

                ui.stack(id)
                    .position(itemX, itemY)
                    .size(itemWidth, itemHeight)
                    .content([&] {
                        ui.rect(id + ".hit")
                            .size(itemWidth, itemHeight)
                            .radius(kRadiusMd)
                            .color(active ? tokens.accentSurface : transparent)
                            .states(active ? tokens.accentSurface : transparent,
                                    active ? tokens.accentSurface : tokens.menuHover,
                                    active ? tokens.accentSurface : tokens.menuHover)
                            .onClick([info, onSelect] { onSelect(info.page); })
                            .build();
                        ui.text(id + ".icon")
                            .position(0.0f, kSpace1)
                            .size(itemWidth, iconHeight)
                            .icon(info.icon)
                            .fontSize(16.0f)
                            .color(active ? tokens.brand : tokens.fgSubtle)
                            .horizontalAlign(eui::HorizontalAlign::Center)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .build();
                        ui.text(id + ".label")
                            .position(0.0f, kSpace1 + iconHeight)
                            .size(itemWidth, labelHeight)
                            .text(info.label)
                            .fontSize(kFontXs)
                            .fontWeight(active ? kWeightMedium : kWeightRegular)
                            .color(active ? tokens.brand : tokens.fgSubtle)
                            .horizontalAlign(eui::HorizontalAlign::Center)
                            .build();
                    })
                    .build();
            }
        })
        .build();
}

}  // namespace viewer
