# DEC-005：viewer 视觉层采用 ZCode Design System 的令牌翻译

> 状态：Accepted
> 日期：2026-09-23
> 负责人：Linductor-alkaid
> 冻结里程碑：M1
> 替代/被替代：无

## 背景与问题

Rin 的前端（EUI-NEO）需要统一视觉语言。团队以 ZCode Design System
（语义色令牌 + `text-ui-*` 字阶 + 容器圆角层级 + 4px 间距节奏 + 克制阴影）为设计
基准，但该体系以 Tailwind/CSS 令牌表达，需翻译到 EUI-NEO 的即时 DSL。

## 决策

在 `apps/viewer/viewer_theme.hpp` 建立唯一令牌层，布局代码只引用语义令牌：

- **语义色**：`background/card/cardBorder/surface/border/input/menu/fg/fgSubtle/
  fgSubtlest/brand/accentSurface/success/warning/destructive`，深色主题具体值集中
  在 `theme::dark()`；语义状态色只用于真实状态（如 Streaming→success、
  Failed→destructive），禁止借色。
- **字阶**：`kFontXl/Lg/Base/Caption/Sm/Xs` = 18/16/14/13/12/10，对应 `text-ui-*`
  （默认 --ui-font-size 14px）；字重只用 400/500/600。
- **间距**：4/8/12/16/24 节奏；16px 为卡内边距基准。
- **圆角层级**：卡片 `xl(12)` → 基础控件 `lg(8)` → 卡内嵌套/画面区 `md(6)` → 下限
  `sm(4)`；不使用任意圆角值。
- **层叠**：层级靠背景对比与 1px 边框表达；阴影只属于浮层（下拉弹层保留组件自带
  overlay 阴影）。EUI 的 zIndex 不跨父容器，故下拉触发器作为根 stack 最后一个兄弟
  合成（绘制顺序即层叠顺序），弹层浮于所有卡片之上。
- **等宽字体**：EUI-NEO 未打包 mono 字体，技术数值（fx/fy/cx/cy）以统一小数位 +
  固定列对齐替代；引入 mono 字体资产后再切换 `fontFamily`。
- **键盘导航**：数字键 1-9 直选分辨率档位（keyboard-first 的一等输入路径）。

## 备选方案

- 继续 ad-hoc 样式（首版）：视觉不一致、暗色对比随意，已废弃。
- 照搬 Tailwind 类名层：EUI-NEO 无 CSS 引擎，翻译层不可避免。

## 影响与风险

- 新增视觉需求时必须先扩令牌再使用，禁止绕过令牌层写裸值（AGENTS.md 工程约束）。
- 深色主题先行；浅色/Zai 变体待产品需要时在 `viewer_theme.hpp` 增补第二套 tokens。

## 验证方式

真机截图（`viewer_final_design_tokens.png` 等）对照设计原则评审；键盘/鼠标两种
切换路径均在真机验证（M1 验证记录）。

## 关联文档和工作项

`M1-05`、
[viewer 设计系统基准（ZCode Design System 翻译层）](../design/viewer_design_system.md)
（基准原文 pin `zai-org/ZCode@872ad96`）、
[camera_service_design.md](../design/camera_service_design.md)、
[EUI-NEO 台账 EUI-20260923-002](../dependency_feedback/eui-neo/ledger.md)
