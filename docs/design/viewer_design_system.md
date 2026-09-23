# viewer 设计系统基准（ZCode Design System 翻译层）

> 状态：Active
> 更新日期：2026-09-23
> 基准来源：[zai-org/ZCode `DESIGN.md`](https://github.com/zai-org/ZCode/blob/872ad960de7ec172591f7e1952f7849229f94521/DESIGN.md)
> pin `872ad960de7ec172591f7e1952f7849229f94521`（2026-09-20 "feat: open source"，当前唯一上游版本）
> 令牌取值唯一来源：`apps/viewer/viewer_theme.hpp`
> 关联：[DEC-005](../decisions/DEC-005-viewer-visual-design.md)（采纳本基准的决策）、
> [DEC-002](../decisions/DEC-002-runtime-ui-integration.md)（viewer 集成）、
> [DEC-003](../decisions/DEC-003-depth-colormap.md)（深度伪彩，见"范围边界"）

## 定位

ZCode Design System 是上游为 AI 辅助 UI 工作提供的可移植设计系统，本仓库将其固定为
viewer 前端（EUI-NEO）的视觉基准（DEC-005）。本文不复制上游全文——上游原文以"来源"处
pin 的版本为准——只负责三件事：

1. **固定版本**：记录上游 pin，上游更新不自动生效（见"维护与再固定"）。
2. **令牌翻译**：把上游语义令牌体系映射为 `apps/viewer/viewer_theme.hpp` 的 EUI-NEO
   编译期常量；具体取值只在 `viewer_theme.hpp::theme::dark()` 维护，本文不重复数值。
3. **落地规则与偏差**：登记上游原则在本仓库的执行规则，以及 EUI-NEO 能力限制导致的
   已知偏差和绕行。

agent 生成或修改 viewer UI 前，先读本文件与 `viewer_theme.hpp`，不凭记忆猜测任何
取值；这与上游"先查本文件再写样式"的原则一致。

## 核心原则（译述上游 Core principles）

1. **不臆造取值**：颜色、字号、字重、间距、圆角、阴影一律来自令牌，不写一次性裸值。
2. **语义优先于字面**：布局代码引用语义令牌（角色名），不直接引用具体数值。
3. **先查基准**：写任何样式代码前先对照本文件与令牌层。
4. **语义状态色不作装饰**：success/warning/destructive 只用于真实状态，禁止借色。

## 令牌翻译（上游 → `viewer_theme.hpp`）

### 颜色（语义色）

上游以 CSS 变量定义深/浅两套语义色；本仓库深色先行，翻译为 `ThemeTokens` 字段
（当前取值见 `theme::dark()`）：

| 上游语义令牌 | `ThemeTokens` 字段 | viewer 用途 |
| --- | --- | --- |
| background | `background` | 页面根背景 |
| card / card-border | `card` / `cardBorder` | 内容卡与描边 |
| surface | `surface` | 低强调承载面（视频画面区） |
| border | `border` | 默认分隔线 |
| input / input-border(-hover) | `input` / `inputBorder` / `inputBorderHover` | 可编辑字段 |
| menu | `menu` / `menuHover` | 下拉浮层 |
| fg / fg-muted / fg-subtle | `fg` / `fgSubtle` / `fgSubtlest` | 主文本/次级/极弱元数据 |
| brand | `brand` / `accentSurface` | 品牌强调（稀用）/弱强调选中底 |

颜色使用规则（上游 Color usage rules 的落地）：

- 语义状态色只映射真实状态，经 `theme::stateColor()` 集中定义：
  `Streaming→success`、`Opening/Restreaming→warning`、`Failed→destructive`、
  `Idle/Stopping/Waiting→fgSubtle`。新增状态先扩该函数，不得在布局代码就地选色。
- brand 只用于选中态/关键指示，不作大面积装饰。

### 字阶（text-ui-* → `kFont*`）

| 上游 | 令牌 | px | 用途 |
| --- | --- | --- | --- |
| text-ui-xl | `kFontXl` | 18 | 一级阅读标题 |
| text-ui-lg | `kFontLg` | 16 | 二级标题 |
| text-ui-base | `kFontBase` | 14 | 正文/常用控件/面板标题 |
| text-ui-caption | `kFontCaption` | 13 | 次级说明 |
| text-ui-sm | `kFontSm` | 12 | 辅助信息 |
| text-ui-xs | `kFontXs` | 10 | 徽标/极弱元数据 |

字重只用 400/500/600（`kWeightRegular/Medium/Semibold`）；不使用其他字号与字重。

### 间距（4px 基准节奏）

`kSpace1/2/3/4/6` = 4/8/12/16/24；16px 为卡内边距基准。不使用节奏外数值。

### 圆角（容器层级）

`kRadiusXl/Lg/Md/Sm` = 12/8/6/4：卡片 `xl` → 基础控件 `lg` → 卡内嵌套/菜单项 `md`
→ 下限 `sm`。首个圆角容器决定层级起点，向下逐级收窄；不使用任意圆角值。

### 层叠与阴影

- 层级靠背景对比与 1px 边框（`kBorderHairline`）表达，不靠阴影堆叠。
- 阴影只属于浮层：下拉弹层保留组件自带 overlay 阴影。
- EUI-NEO 的 zIndex 不跨父容器：下拉触发器作为根 stack 最后一个兄弟合成，
  绘制顺序即层叠顺序，弹层浮于所有卡片之上。

## 已知偏差与绕行（相对上游假设）

| 上游假设 | 本仓库现实 | 处理 | 依据 |
| --- | --- | --- | --- |
| mono 字体渲染技术数值 | EUI-NEO 未打包 mono 字体资产 | fx/fy/cx/cy 等以统一小数位 + 固定列对齐替代；引入 mono 资产后切换 `fontFamily` | DEC-005 |
| CSS zIndex 管理层叠 | zIndex 不跨父容器 | 根 stack 末位兄弟合成（绘制顺序即层叠） | DEC-005、[EUI 台账 EUI-20260923-002](../dependency_feedback/eui-neo/ledger.md) |
| CSS 变量/类名令牌层 | EUI-NEO 即时 DSL，无 CSS 引擎 | 编译期常量令牌层（`viewer_theme.hpp`），翻译不可避免 | DEC-005 |

新偏差必须先登记本表（附依据），再实施绕行；不得静默偏离上游原则。

## 范围边界

- 深度画面的像素颜色（jet 伪彩映射）是数据可视化，由 [DEC-003](../decisions/DEC-003-depth-colormap.md)
  决定，不属本系统 UI 令牌；其承载控件（画面卡、标签、状态徽标）的样式遵循本基准。
- 深色主题先行。需要浅色/Zai 变体时在 `viewer_theme.hpp` 增补第二套 tokens，并同步
  本文件的翻译表与 DEC-005。

## 维护与再固定

上游 `DESIGN.md` 更新不自动生效。同步流程按依赖引用纪律执行：

1. diff 评审上游变更，判断是否影响本仓库令牌翻译或偏差登记。
2. 更新本文"来源"pin 与下方历史表；需要时同步 `viewer_theme.hpp` 与 DEC-005。
3. 真机截图对照本基准复验，结论记入对应里程碑验证记录。

| 日期 | 上游 pin | 变更 |
| --- | --- | --- |
| 2026-09-23 | `872ad96`（"feat: open source"，上游现行唯一版本） | 首次固定为 viewer 视觉基准（DEC-005） |

## 验证记录

2026-09-23：建立本基准文件。翻译表与 `apps/viewer/viewer_theme.hpp` 现值逐一核对一致；
状态色映射与 `stateColor()` 一致；真机视觉证据沿用 DEC-005/M1-05 验证记录
（`viewer_final_design_tokens.png` 等）。
