# 工作台 UI 调研：RPA 与节点式画布工具参考

> 状态：Reference（`M5-01` 调研输入，支撑 `DEC-014`/`DEC-015` 冻结；本文不冻结任何决策）
> 更新日期：2026-09-24
> 调研发起：用户明确要求参考 RPA 软件的前端设计与用户体验（点名影刀），与 `M5-01`
> 既定的现代节点式工具调研合并执行
> 关联：[m5-ui-workbench.md](../plans/m5-ui-workbench.md)（`M5-01`）、
> [rin-implementation-plan.md](../plans/rin-implementation-plan.md)（DEC-014/015 暂定条目）、
> [workflow_types.hpp](../../include/rin/workflow_types.hpp)（M4-09 契约）、
> [viewer 设计系统基准](viewer_design_system.md)、
> [EUI-NEO 台账](../dependency_feedback/eui-neo/ledger.md)

## 1. 调研背景与方法

`M5-01` 要求调研现代节点式工具（Blender 合成器、TouchDesigner、Node-RED、Unreal
Blueprint、Intel RealSense Viewer 等）的页面组织与节点交互范式，产出
`ui_workspace_design.md` 并冻结 DEC-014（工作台信息架构）与 DEC-015（节点编辑器
实现路径）。用户在启动时追加要求：图像处理流画布与构建的前端风格参考 RPA 软件
（特别是影刀 RPA）的设计与用户体验。

调研按三路并行执行，本文综合其结论：

1. 影刀 RPA 深度调研（官网、帮助中心入口、第三方系列教程与评测交叉印证）；
2. RPA 编辑器横向对比（UiPath Studio/StudioX、Power Automate Desktop、
   Automation Anywhere A360、来也/实在/阿里云/艺赛旗）；
3. 节点式画布工具范式（Blender、TouchDesigner、Node-RED、Unreal Blueprint、
   Intel RealSense Viewer，及 ComfyUI/n8n/Dify/Houdini 简述）。

证据等级：官方文档与官网一手资料优先；官方站点为 SPA 无法直接抓取正文时，以官方
检索摘要 + 多个独立第三方教程交叉印证，并在正文标注；无法证实的细节明确写
"公开资料未证实"。外部来源 URL 汇总见第 7 节。

## 2. Rin 的约束锚点（调研结论的评判基准）

调研结论必须落在以下既有约束内，超出部分只能作为"待决策扩展"记录：

- **单窗口页面模型**：DEC-014 暂定"单窗口 + 页面导航（预览/位姿/图像工作流/
  设置）"；EUI-NEO 无停靠原语，多窗口/停靠布局在 M5 非目标内。
- **EUI-NEO 能力面**（pinned `782c5699`）：自研画布可用 `rect`/`polygon`/
  `mousearea`/`ui.state`/`loader` 原语与 button/card/dialog/dropdown/input/
  slider/switch/segmented/contextmenu/tooltip/toast/datatable/scrollview 等组件；
  **无节点编辑器、画布平移缩放、小地图等现成组件**（DEC-015 自研假设成立）。
  已知限制：zIndex 不跨父容器（绘制顺序即层叠）；retained layer 对 polygon
  点集变更不失效（EUI-20260924-001，P1 缺陷，位姿视图已以 `dirtyKey` 绕行）——
  画布连线若用大量逐帧变化 polygon，必须沿用该先例并预算绘制成本。
- **M4-09 视图契约（已冻结）**：UI 数据面已定——`NodeCatalog`/`NodeDescriptor`
  （调色板与端口签名唯一来源）、`ParamDescriptor`/`ParamValue`（类型化参数面板：
  Boolean/Integer/Real/Enumeration/RealArray，范围与枚举校验）、
  `validateWorkflowGraph`（12 类校验问题，含 `node` 定位）、`WorkflowStats`/
  `NodeStats`（耗时/FPS/丢弃/在飞）、`NodeOutputSnapshot`（每节点最近一幅中间
  产物）、运行控制（`applyGraph`/`start`/`stop`/`requestParamUpdate`，Running
  下图变更帧边界排空重建、参数热更新下一帧生效）。**M5 的增量在交互面，
  不在数据面。**
- **渲染纪律**：RULE-05/07——UI 线程只做有界校验、取快照与提交；执行与重活
  在引擎（Executor）侧。
- **视觉基准**：ZCode Design System 令牌翻译层（DEC-005，`viewer_theme.hpp`
  唯一令牌层），深色主题先行；语义状态色只映射真实状态。

## 3. RPA 编辑器调研

### 3.1 影刀 RPA（用户点名参考）

**定位与设计哲学**。影刀（杭州分叉智能）刻意避开传统 RPA 大客户项目制路线，
定位"人人可用的好工具"：零代码拖拽为主、影刀学院（college.yingdao.com）内建
新手教育、"魔法指令"对话生成流程（自动选指令/填参数/抓元素）。用户画像是电商
运营、财务等非技术人员，一切 UI 决策围绕"不写代码的人能看懂、敢上手"。

**编辑器布局**。单窗口五区结构（多个独立第三方教程描述一致，官方帮助中心为
SPA 未直接证实）：

| 区域 | 内容 | 对 Rin 的映射候选 |
| --- | --- | --- |
| 顶部菜单栏 | 文件/编辑/视图/运行（运行、调试运行、单步调试）/工具（元素库、变量管理） | 工作流页工具栏：运行控制入口 |
| 左侧指令箱 | 指令按类别分组（网页/Excel/桌面/鼠标键盘/流程控制/变量/数据处理），可展开，顶部搜索框 | 节点调色板：`NodeCatalog` 分类分组 + 即输即筛 |
| 中部流程区 | 指令自上而下线性排列（窗口最大区域） | 节点画布 |
| 右侧属性面板 | 选中指令后显示其全部参数（"先选中、后显示参数"） | 参数面板：选中节点后渲染类型化控件 |
| 底部日志/变量区 | 双标签：逐条指令执行时间与成败；运行时变量值 | 性能面板 + 错误/事件列表 |

**流程呈现范式**：线性步骤卡片列表 + 嵌套块（If/循环/Try-Catch-Finally 成对
块指令，块内缩进表达层级），不是自由节点图。指令严格按时序执行，与屏幕元素
交互——这是 RPA 与 Rin 数据流 DAG 的根本范式差异。

**创建与编排**：拖拽是唯一确认的添加方式（教程踩坑提示"双击指令箱指令不会
执行"）；分类树 + 搜索框优先于逐类展开；支持复制步骤；重复步骤封装为子流程
（社区经验"超过 20 步就抽子流程"），跨应用复用走"自定义指令"（参数化封装 +
发版）。

**参数编辑**：右侧属性面板模式；可确认控件有文本输入、下拉、元素拾取器；
参数支持引用变量（局部/全局/流程参数三类），变量面板实时查值。

**调试三件套**（影刀最受教程作者推崇的部分）：断点（动作左侧小圆点，快捷键
F9）→ 单步（F10）→ 观察（底部日志逐条记录、红字为错误；变量面板暂停时显示
当前值）。失败时弹红色错误弹窗，含**错误类型、错误位置（哪一步）、错误详情**
三要素；社区方法论明确"弹窗只是摘要，日志面板才是完整版"，配合"看日志→
编辑器定位该动作→查前置条件→加日志重跑→对比差异"五步定位法。

**异常处理**：Try/Catch/Finally 块 + Catch 内自定义重试（指数退避 + 抖动，
区分暂时性/永久性故障）+ 错误现场截图（带时间戳落盘）。

**市场生态**：应用市场为官网一级入口，"市场功能可直接获取现成应用，无需开发
就能使用"；官方批量供给模板（190+ 跨境指令与应用）。新手的第一步往往不是建
流程，而是**装一个现成应用改参数**。

**视觉语言**：消费级风格而非工程暗色风；红=错误、蓝点亮/灰=模式切换的克制
状态编码；步骤卡片具体配色规格公开资料未证实。

### 3.2 RPA 编辑器横向对比

- **UiPath Studio**：同一产品内以**文件类型**并存三种范式——Sequence（线性，
  首选）、Flowchart（自由图，官方同时警告大流程会"混乱交织"）、State Machine。
  调试体系最完整：断点/Break（被调试 activity 保持高亮）、Step Into/Over/Out、
  Slow Step（4 档慢速逐 activity 高亮）、Retry/Ignore、Focus Execution Point
  （漫游画布后一键回到执行点）、Locals/Call Stack 面板。设计期实时校验标注
  activity 并汇总错误面板。
- **UiPath StudioX**：同一活动库、两套信息架构——面向业务用户的线性步骤列表 +
  向导式配置，无自由画布。证明"同一引擎可按用户画像换交互范式"。
- **Power Automate Desktop**：单窗口三区（左 Actions 分组可搜索/中流程语句行/
  右 Variables 面板），**子流程以标签页呈现**组织规模；"任何错误信息都会显示，
  出错动作立即高亮"；动作参数经点击弹出的对话框编辑；痛点是变量全局共享、
  子流程无参数传递（静态类型校验弱）。
- **Automation Anywhere A360**：标志性**双视图 Bot Editor**——同一动作序列
  可切 Flow view（连线图）/List view（线性列表）/Dual view 分屏；官方快捷键
  文档确认视图切换与节点详情键盘操作。分工：flow 面向业务理解、list 面向
  开发者精编。
- **来也 UiBot**：分层混合——块间用流程图编排，块内用"可视化步骤列表 /
  源代码（Lua）"双视图（Alt+←/→ 切换），统一调试。
- **阿里云 RPA**：资料最完整的国产编辑器，五区布局（左组件树 300+/中画布/
  右属性面板/底日志+参数面板），可视化/编码双模式（Ctrl+Shift+P 对照查看），
  支持**从指定节点运行**、禁用节点、try-catch-finally 及重试。
- **实在智能**：重心转向 ChatRPA（对话生成并执行流程）；**艺赛旗**：画布连线
  + 录制器 + Python IDE 双模式。八斗智能未发现 RPA 编辑器产品资料（特此标注）。

**行业范式取舍共识**：强时序确定性步骤用线性列表，分支复杂的控制流用自由图，
长期运行业务实体用状态机；头部产品都拒绝二选一，以文件类型（UiPath）、双视图
（A360）、分层（来也）、子流程 tab（PAD）控制规模。

### 3.3 RPA 对 Rin 的借鉴与不适用

**可迁移模式**（范式无关，按价值排序）：

1. 单窗口五区布局骨架（左库/中画布/右属性/底日志与状态/顶工具栏）——已被大量
   非专业用户验证，与 DEC-014 单窗口页面模型兼容；
2. 指令库/组件面板信息架构：分类分组 + 顶部即输即筛 + 拖拽 + 最近使用/收藏；
3. 右侧常驻上下文属性面板："先选中、后显示参数"心智模型 + 类型化控件；
4. 调试三件套：断点标记、单步/从指定节点运行（阿里云原生支持，对图像流水线
   价值高——避免每次从头跑采集）、日志逐条记录 + 红字错误；
5. 错误弹窗三要素（类型/位置/详情）+ "弹窗摘要、面板全文"两层报错 + 一键
   定位失败节点；
6. 错误处理块思想：映射为节点失败策略（重试/降级）或错误边界节点；
7. 模板/示例流生态：内置"示例图像处理流模板"（一键加载改参数）降低 DAG
   上手门槛（对应总计划 POST-04 的延后方向，调研建议预留入口）。

**RPA 做得不好、Rin 应超越的点**：

- **画布内静态类型校验**：PAD 变量全局共享、A360/UiPath 类型校验弱是公认痛点；
  Rin 已有 `validateWorkflowGraph`（类型不匹配/多驱动/环/悬空输入显式拒绝），
  应做到连线拖拽中即时过滤 + 校验问题双击定位（RPA 未做到的）。
- **中间结果可视化**：RPA 只有变量值观察；Rin 有逐节点 `NodeOutputSnapshot`，
  缩略图查看是相对 RPA 的天然增强。
- **背压/丢弃可视化**：数据流的过载显式丢弃（`droppedFrames`）在 RPA 无对应物，
  需要新设计而非照搬。

**不适用点**：线性步骤范式本身（Rin 是类型驱动 DAG，有并行扇出/合流）；元素
拾取器/Selector 体系（无 GUI 控件概念）；逐行同步执行语义与对应性能面板形态
（Rin 是连续帧流 + 有界在飞 + 丢弃统计）。

## 4. 节点式画布工具调研

### 4.1 M5-01 点名工具

- **Blender 合成器**：快捷键驱动的典范——`Shift+A` Add 菜单（可键入搜索）、
  `N` 侧栏、**Backdrop 模式把 Viewer 节点输出铺在画布背景上**（`Shift+Ctrl+LMB`
  点击任意节点即切换预览目标）；`Ctrl+J` 收入 Frame 分组框、`M` 静音节点、
  `Ctrl+RMB` 划线切割连线、`Ctrl+Alt+RMB` 静音连线；Node Wrangler 插件提供
  批量连接与快速预览。
- **TouchDesigner**："网络 + 操作符"范式：OP 按家族分类配色（TOP/CHOP/SOP/
  MAT/DAT/COMP），generator 深色、filter 浅色，当前节点绿框、选中黄框；画布
  左键拖平移、中键横拖缩放、`f` 帧全图、`o` 网络总览（小地图）；**节点即
  viewer**：每个 OP 自带可开关显示区（`a` 批量开关），直接在节点上查看中间
  图像；参数进停靠参数对话框（`p`）或浮动窗——"节点上只放状态、参数进侧栏"
  是明确取舍。
- **Node-RED**：经典三栏（左调色板/中画布/右侧 sidebar 以 tab 组织
  Information/Help/**Debug**/Config）；节点下方状态点表达运行态；**Deploy
  语义**是"参数热更新"的独特回答：改动在点 deploy 前不生效，提供 Full/修改过
  的 flows/修改过的 nodes/重启 四档部署粒度，避免中断未改动运行态。
- **Unreal Blueprint**：**端口拖线智能创建**——拖到空处松手弹上下文菜单搜索
  建节点并自动连线；兼容引脚 hover 绿勾、不兼容显示不可连原因；引脚按数据类型
  着色，类型不匹配自动插入转换节点；`Ctrl+拖拽` 改接既有连线、`Alt+LMB` 断开、
  Reroute 节点整理走线；注释框分组；编译徽标状态 + Compiler Results 面板点击
  定位出错节点。
- **Intel RealSense Viewer**（产品参照，非节点工具）：**"设备/流/滤镜皆是一行
  可展开的卡片，行内开关即生效"**——源列表行内启停流、展开即分辨率/帧率下拉；
  侧栏 Post-processing 按滤镜逐个展开参数；录制带红点指示、回放源可拖进度条
  并显示录制时刻参数。适合 Rin 预览/位姿页沿用。

### 4.2 邻近工具简述

- **ComfyUI**（AI 图像节点流，与 Rin 场景最近）：Reroute 整理长线；Bypass
  （直通数据）/Mute 两级禁用；新版 Subgraph 折叠；执行队列 + **当前执行节点
  高亮 + 节点内实时预览中间图像**。
- **n8n**：与 Rin 最同构的"节点详情"参照——点开节点进入详情视图（参数 +
  **INPUT/OUTPUT 数据 tab**），`Execute step` 单步执行看单节点输出，整流运行
  逐阶段点亮；sticky note 画布注释。
- **Dify**：Workflow 画布"拖、连、配"三步编排 + 运行/日志面板。
- **Houdini**：节点 display/render flag + **每节点 cook/cached/error 状态徽标
  可配置显示**——逐节点耗时标注的直接样板。

### 4.3 交互惯例速查（跨工具公约数）

| 维度 | 主流惯例 |
| --- | --- |
| 画布基础 | 平移=拖空白/中键；缩放=滚轮；框选=拖框/Shift 加选；帧全图=`F`/`Home`；小地图=可选 |
| 节点创建 | 三通道并存：调色板拖出（Node-RED）、右键/快捷菜单搜索（Blender/UE/TD）、端口拖线松手菜单（UE/n8n） |
| 连线 | 左入右出、贝塞尔曲线、类型着色 + hover 兼容指示（UE 绿勾）、Alt 点删线、`Ctrl+拖` 改接、reroute/切割 |
| 参数位置 | 节点内联只放少量关键参数；完整参数进侧栏/抽屉/对话框（Node-RED/n8n/TD/Blender N 面板） |
| 中间结果 | Viewer 节点 + backdrop（Blender）、节点内嵌 viewer（TD/ComfyUI）、单步 INPUT/OUTPUT（n8n） |
| 执行状态 | 运行中/成功/失败三态着色 + 节点徽标（ComfyUI 高亮、Houdini cook 徽标、Node-RED 状态点） |
| 错误反馈 | 错误节点红框/图标 + 错误列表面板点击定位（UE Compiler Results）；连线期类型拦截（UE） |
| 布局组织 | "左调色板/中画布/右参数信息"三栏为跨行业共识；规模用分组框/子图/tab 控制 |

## 5. 综合分析：对 DEC-014/DEC-015 的建议输入

> 本节是建议，不是决策；DEC-014/015 冻结时以此为基线评审。

### 5.1 范式判断

Rin 的图像处理流是**类型驱动 DAG 数据流**（节点=算子、边=Gray8/Rgba8 类型化
图像、可并行扇出、无 GUI 时序），自由节点图是正确主范式——RPA 行业自身把
"分支复杂的控制流"交给自由图的取舍逻辑支持这一点。应从 RPA 借鉴的是**规模
控制与新手上手**手段（分类调色板、搜索、属性面板、调试三件套、错误定位、
模板），而非把画布改成步骤列表。节点画布工具提供交互惯例的公约数，Rin 无需
发明新交互。

### 5.2 对 DEC-014（工作台信息架构）的建议

- 保持单窗口页面导航（预览/位姿/图像工作流/设置）；**工作流页内部**采用
  RPA 验证过的五区骨架：顶工具栏（运行控制/校验状态）、左调色板
  （`NodeCatalog` 分类 + 搜索）、中画布、右上下文面板（参数编辑 + 节点
  快照预览，无选中时显示工作流总览统计）、底错误/事件列表（可折叠）。
- 预览/位姿页沿用 Intel RealSense Viewer 式"模块卡片 + 行内展开"组织，
  不被节点图吞噬简单场景的易用性。
- 参数热更新采用 Rin 契约的"下一帧生效"语义并在 UI 明示，**不引入
  Node-RED 式编辑态/运行态分离的 deploy**（Rin 无此需求；图结构变更在
  Running 下的"帧边界排空重建"用事件 `GraphApplied` 反馈到画布状态即可）。

### 5.3 对 DEC-015（节点编辑器实现路径）的建议

确认 EUI-NEO 原语自研画布（与暂定方向一致），交互几何下沉平台无关纯逻辑。
功能按投入产出排序：

**P0（MVP 必做）**

1. 画布基础四件套：拖空白平移、滚轮缩放、矩形框选、`F` 帧全图；
2. 节点创建双通道：调色板拖出 + 画布右键搜索菜单（同一 `NodeCatalog` 数据源）；
3. 连线交互：端口命中区大于视觉尺寸、拖线中类型即时过滤（不兼容端口红叉）、
   Gray8/Rgba8 各配一色（语义令牌内表达）、Alt+点击删线、贝塞尔走线；
4. 三栏布局 + 右侧类型化参数面板（`ParamDescriptor` 驱动：Boolean→开关、
   Integer/Real→滑条+输入、Enumeration→下拉、**RealArray→矩阵网格编辑器**
   ——自定义卷积核参数，现有组件无对应物，需自研并列入 M5-04 工作量）；
5. 中间结果查看：点击节点 → 右面板底部显示该节点最近 `NodeOutputSnapshot`
   缩略图（等价 n8n 单步 OUTPUT；快照机制契约已备，性价比最高）；
6. 执行状态三态着色（Idle/Running/Failed 对应节点描边）+ 节点底部耗时小徽标
   （`NodeStats`）+ 全局 FPS/丢弃计数（`WorkflowStats`）；
7. 校验反馈闭环：`ValidationIssue` 列表 + 双击定位节点 + 画布红点标注
   （契约已带 `node` 定位信息）；
8. 运行控制：工具栏启动/停止 + 状态徽标（`WorkflowEngineState`）。

**P1（二期增强）**：Mute/Bypass 两级禁用（图像流调试刚需，但 M4 契约无此
节点语义，需先扩契约再上 UI）；Reroute 拐点；Frame 分组框；backdrop 画布
背景预览（Blender 惯例，复用快照机制）；小地图；断点/单步调试（需引擎契约
扩展，M4-09 未含——登记为契约演进候选而非 M5 范围）。

**P2（不做/暂缓）**：TD/ComfyUI 式节点内嵌交互 viewer（原语成本过高，侧栏
快照已覆盖诉求）；节点内联参数控件（先统一右面板）；subgraph 组合节点；
模板市场（POST-04 方向）。

### 5.4 实现风险（调研发现 → EUI-NEO 映射）

- **连线渲染与 retained layer 缺陷**：大量逐帧变化 polygon 走 retained layer
  会触发 EUI-20260924-001 同型冻结；画布连线的点集随节点拖动变化，必须沿用
  `dirtyKey`/逐帧直绘先例，并把画布元素数量与绘制成本纳入预算（`M5-01` 原型
  先行验证，风险表已列）。
- **zIndex 不跨父容器**：右键菜单、拖线悬浮层等浮层遵循"根 stack 末位兄弟
  合成"先例（DEC-005）；`contextmenu` 组件能力需原型验证。
- **缩略图纹理路径**：`NodeOutputSnapshot` 像素上屏沿用 GpuFrameView 绕行
  路径（EUI-20260923-003 Open），不得走 `ImageStream`。
- **键盘操作**：Blender/UE 式快捷键依赖 EUI-NEO 键盘事件能力面，`M5-01`
  原型需确认 `ui.state`/mousearea 对键盘的暴露程度；不足则按台账流程登记。

### 5.5 调研限制与未证实项

- 影刀官方帮助中心为 SPA，正文未直接抓取；布局与调试流程经多个独立第三方
  教程交叉印证，步骤卡片视觉规格、块折叠行为、运行时当前步骤高亮等**公开资料
  未证实**，本文未将其作为依据。
- UiPath/A360 官方文档部分页面为 JS 渲染，面板级细节依赖官方检索摘要 + 社区
  交叉验证；八斗智能无 RPA 编辑器资料。
- ComfyUI 执行高亮细节部分来自社区观察；UE 注释框快捷键等零星细节未证实处
  均已标注。
- 全部结论为桌面端公开资料的定性归纳，未做用户实测；`M5-01` 的设计冻结前
  应以假引擎可交互原型（`M5-08` 之后）复核关键交互假设。

## 6. 文档关系与后续步骤

```
本文（调研输入） ──► DEC-014 工作台信息架构（待建，Proposed → Accepted）
                ──► DEC-015 节点编辑器实现路径（待建，Proposed → Accepted）
                ──► docs/design/ui_workspace_design.md（M5-01 产出：页面拓扑树 + 交互流 + 原语映射）
```

后续：以第 5 节建议为基线起草 DEC-014/015 与 `ui_workspace_design.md`；
`M5-01` 完成判据（设计文档 + 两决策 Accepted）达成后关闭该工作项。

## 7. 来源清单

影刀（官方）：[官网](https://www.yingdao.com)、
[帮助中心](https://www.yingdao.com/yddoc)、[影刀学院](https://college.yingdao.com)。
影刀（第三方教程，交叉印证）：[界面五区详解](https://blog.csdn.net/ruanjianwang888/article/details/162503699)、
[错误定位五步法](https://blog.csdn.net/vxharuanjian888/article/details/163027455)、
[异常处理体系](https://blog.csdn.net/vxharuanjian888/article/details/162131442)、
[开发界面与自定义指令](https://blog.csdn.net/ddf128/article/details/147780459)、
[try-catch-finally](https://blog.csdn.net/linyanrpa/article/details/161862856)、
[断点单步](https://blog.csdn.net/linyanrpa/article/details/161924075)、
[变量面板](https://blog.csdn.net/vxharuanjian888/article/details/163200382)、
[子流程](https://blog.csdn.net/vxharuanjian888/article/details/162588914)、
[新手常见错误](https://blog.csdn.net/vxharuanjian888/article/details/163369670)、
[指令分类速查](https://blog.csdn.net/ruanjianwang888/article/details/161862857)、
[易用性设计](https://blog.csdn.net/qq_27504375/article/details/139913625)。

RPA 横向（官方为主）：[UiPath Workflow Design](https://docs.uipath.com/studio/standalone/2023.10/user-guide/workflow-design)、
[UiPath About Debugging](https://docs.uipath.com/studio/standalone/2023.10/user-guide/about-debugging)、
[UiPath Debugging Actions](https://docs.uipath.com/studio/standalone/2023.10/user-guide/debugging-actions)、
[StudioX Introduction](https://docs.uipath.com/studiox/standalone/2024.10/user-guide/introduction)、
[PAD Flow designer](https://learn.microsoft.com/en-us/power-automate/desktop-flows/flow-designer)、
[PAD Introduction](https://learn.microsoft.com/en-us/power-automate/desktop-flows/introduction)、
[A360 快捷键文档](https://docs.automationanywhere.com)、
[阿里云 RPA 可视化开发模式](https://help.aliyun.com/zh/rpa/user-guide/visual-development-mode)、
[阿里云 RPA 文档中心](https://help.aliyun.com/zh/rpa/)、
[来也 documents](https://documents.laiye.com)、
[实在智能](https://www.ai-indeed.com)、
[艺赛旗](https://www.i-search.com.cn)、
[PAD 错误处理](https://www.serverlessnotes.com)、
[UiPath 论坛校验讨论](https://forum.uipath.com)。

节点画布（官方为主）：[Blender 节点编辑器界面](https://docs.blender.org/manual/en/latest/editors/node_editor/interface.html)、
[Blender 节点编辑](https://docs.blender.org/manual/en/latest/editors/node_editor/nodes/editing.html)、
[Node Wrangler](https://docs.blender.org/manual/en/latest/addons/node/node_wrangler.html)、
[TouchDesigner First Things](https://docs.derivative.ca/First_Things_to_Know_about_TouchDesigner)、
[TouchDesigner Network Editor](https://docs.derivative.ca/Network_Editor)、
[Node-RED 编辑器指南](https://nodered.org/docs/user-guide/editor)、
[Node-RED Debug 侧栏](https://nodered.org/docs/user-guide/editor/sidebar/debug)、
[FlowFuse：Node-RED 编辑器](https://flowfuse.com/blog/2025/01/node-red-editor/)、
[UE 连接节点](https://dev.epicgames.com/documentation/en-us/unreal-engine/connecting-nodes-in-unreal-engine)、
[UE Blueprint Tips](https://www.unrealengine.com/en-US/blog/blueprint-editor-tips-and-tricks)、
[UE 错误定位技巧](https://www.cbgamedev.com/blog/quick-dev-tip-103-ue4-ue5-auto-navigate-to-errors)、
[ComfyUI 节点文档](https://docs.comfy.org/reference/default_nodes)、
[ComfyUI 核心概念](https://docs.comfy.org/essential-concepts/core-concepts/nodes)、
[ComfyUI Subgraph](https://docs.comfy.org/interface/features/subgraph)、
[n8n Quickstart](https://docs.n8n.io/try-it-out/quickstart/)、
[Dify Workflow 概述](https://docs.dify.ai/en/guides/workflow)、
[Houdini 网络 flag](https://www.sidefx.com/docs/houdini/network/flags.html)、
[Houdini 网络编辑器选项](https://www.sidefx.com/docs/houdini/network/editopts.html)、
[RealSense 录制回放](https://github.com/IntelRealSense/librealsense/blob/master/doc/record-and-playback.md)、
[RealSense 深度后处理](https://dev.realsenseai.com/docs/depth-post-processing-for-realsense-depth-camera-d400-series)、
[RealSense D400 预设](https://github.com/IntelRealSense/librealsense/wiki/D400-Visual-Presets)。
