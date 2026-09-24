# 工作台 UI 设计：信息架构与节点编辑器

> 状态：Active
> 更新日期：2026-09-24
> 依据：[DEC-014](../decisions/DEC-014-workbench-information-architecture.md)、
> [DEC-015](../decisions/DEC-015-node-editor-implementation-path.md)、
> [DEC-005](../decisions/DEC-005-viewer-visual-design.md)、
> [DEC-016](../decisions/DEC-016-contract-first-workbench-order.md)、
> [M4-09 视图契约](../../include/rin/workflow_types.hpp)、
> [ui_workspace_research.md](ui_workspace_research.md)（调研依据）
> 实施工作项：[m5-ui-workbench.md](../plans/m5-ui-workbench.md)（`M5-02`..`M5-08`）

## 1. 定位与范围

本文冻结工作台的界面信息架构与操作逻辑：页面拓扑树、导航模型、状态与空态、
节点编辑器交互流、EUI-NEO 原语映射。它是 `M5-02`..`M5-05` 的实现依据；视觉
取值不在本文重复——一律遵循
[viewer 设计系统基准](viewer_design_system.md)与 `viewer_theme.hpp` 唯一令牌层
（新增视觉需求先扩令牌，禁止裸值）。数据契约（图模型、参数、统计、运行控制）
以 M4-09 头文件为准，本文只定义其 UI 呈现与交互，不重复定义。

M5 骨架对契约假引擎（`M5-08`）开发调试（DEC-016）；性能数字以真引擎实测为准。

## 2. 页面拓扑树

```
AppShell（单窗口，EUI-NEO DslAppConfig）
├─ 全局导航栏：预览 | 位姿 | 图像工作流 | 设置（图标+文字，左窄边）
├─ Page 预览
│   ├─ 画面区：RGB 卡 + 深度卡（现 viewer 布局，GpuFrameView 路径）
│   ├─ 设备与流配置卡：设备选择（DEC-006 Waiting 稳态）、分辨率、深度配色
│   └─ 内参卡
├─ Page 位姿：3D 位姿视图卡（dirtyKey 绕行）+ IMU 状态面板（现 M3 布局）
├─ Page 图像工作流（五区骨架，DEC-014）
│   ├─ 顶：工具栏（启动/停止、引擎状态徽标、校验状态、帧全图）
│   ├─ 左：节点调色板（NodeCatalog 分类分组 + 搜索框）
│   ├─ 中：节点画布（视图变换 + 节点 + 连线 + 选择框 + 右键菜单）
│   ├─ 右：上下文面板（参数编辑 | 工作流总览；底部：选中节点快照）
│   └─ 底：校验问题与事件列表（可折叠）
└─ Page 设置：深度配色切换（DEC-007 命令通道）、关于/版本信息
```

四页共享相机服务运行态；工作流页的启动/停止只作用于工作流引擎
（`IWorkflowEngine`），不改变相机采集。工作流页无节点画布之外的独立
"运行页"——运行控制内嵌工具栏（调研结论：运行/调试不换窗口）。

## 3. 导航模型

- 导航控件首选 EUI-NEO `sidebar`（DEC-014）；原型受限时退化 `tabs`/
  `segmented`，信息架构不变。当前页高亮用 brand/弱强调底（DEC-005 语义，
  不新增装饰色）。
- 页面切换保持各页 UI 状态（工作流页的视图变换、面板开合、选中态是
  `ui.state` 会话状态，不因导航丢失）。
- 切离工作流页时引擎继续运行（引擎侧承载，`RULE-07`）；返回时经契约
  `tryLoadStats`/`tryLoadNodeOutput`/`tryLoadEvent` 最新态语义重新消费，
  以 `sequence` 推进刷新，禁止以 stale 快照恢复活动状态。页面不驻留
  定时器——刷新发生在 compose 边界取快照（`RULE-05`：渲染线程只做有界
  校验、取快照与提交）。
- 键盘导航：沿用 DEC-005 keyboard-first 原则；画布快捷键（`F` 帧全图、
  `Del` 删除选中）依赖 EUI-NEO 键盘事件暴露，`M5-03` 原型确认，不足时按
  台账流程登记（DEC-015 风险条目）；每个快捷键必须有鼠标等价路径。

## 4. 状态与空态

| 场景 | 呈现 |
| --- | --- |
| 工作流页空图 | 画布中央引导卡："从左侧拖入节点开始"；不弹模态 |
| 相机 Waiting（无设备） | 沿用 DEC-006 稳态；工作流页允许搭图，启动被引擎拒绝时按准入错误呈现（`AdmissionResult.error`） |
| 引擎 Idle | 工具栏状态徽标 fgSubtle；启动按钮可用条件=当前图校验通过 |
| 引擎 Running | 徽标 success；停止可用；参数编辑即改即生效（明示"下一帧生效"）；图结构变更显示"待生效"，`GraphApplied` 事件后清除 |
| 引擎 Stopping | 徽标 warning；控件禁用至回到 Idle |
| 引擎 Failed | 徽标 destructive + `lastError`/事件列表定位；恢复路径=修复后重新启动（`start` 幂等准入） |
| 校验未通过 | 画布问题节点 destructive 描边；底列表逐条 `ValidationIssue`（kind + node 定位 + message），点击定位节点 |
| 节点执行失败 | `NodeFailed` 事件对应节点 destructive 徽标 + 消息入事件列表 |
| 停止/关闭排空 | `stop()` 返回后：快照缩略图清空、性能面板冻结为末次值并标注"已停止"；不得以 stale 数据显示活动状态（`workflow_engine.hpp` 契约） |

状态徽标颜色一律经 `theme::stateColor()` 语义映射扩展（先扩令牌/映射表，
再使用），与 viewer 现有 Streaming/Failed 映射同源。

## 5. 节点编辑器交互流

### 5.1 视图变换与坐标系

画布持有一套视图变换（平移偏移 + 缩放因子，`ui.state` 会话状态；缩放有
上下限），屏幕坐标与画布坐标互转为平台无关纯逻辑。节点位置只存在于画布
UI 状态（DEC-014 决策 5：不入契约）。平移=拖空白处；缩放=滚轮（以指针为
锚点）；`F`/工具栏按钮=帧全图（全部节点落入视口）。

### 5.2 节点创建

1. **调色板拖出**：按住 `NodeDescriptor` 条目拖入画布松手落点创建
   `NodeInstance`（id 由图构造方分配，非零唯一）；拖拽过程显示类型名跟随
   光标。
2. **画布右键菜单**：右键空白处弹搜索菜单（同 `NodeCatalog` 数据源，即输
   即筛），选中后在该点创建。
3. 创建即选中该节点并聚焦右面板（"先选中、后显参"心智，影刀范式迁移）。
   调色板按 `NodeDescriptor` 分组展示（几何/滤波/频域/直方图），顶部过滤框
   即输即筛；分组与过滤为纯逻辑模型。

### 5.3 连线

- **发起**：从端口拖出（端口命中区大于视觉尺寸）；仅允许 Output→Input。
- **类型过滤**：拖线中即时过滤——目标端口与拖线 `PortType` 一致才高亮可
  落点，不一致显式不可落（调研：UE 兼容绿勾/拒绝指示的转译；Gray8/Rgba8
  各配一枚端口类型色，M5-02 扩令牌）。
- **落点**：落在兼容 Input 端口上建立 `Connection`（该输入已有入边时替换
  旧边——契约单驱动约束的 UI 表达，替换需可见：旧边移除动画或即时消失）；
  落在空白处仅取消拖线（P1 再评估"松手弹创建菜单"）。
- **删除**：`Alt+点击` 连线删除；选中节点删除时随删关联边。
- **走线**：输出右缘中点 → 输入左缘中点，三次贝塞尔按水平距离拟合，
  `polygon` 采样渲染；端口按 `NodeDescriptor` 签名序号纵向排布。
- 画布即时反馈合法性与引擎准入一致：UI 预检复用 `validateWorkflowGraph`
  （契约"UI 预检与引擎准入共用唯一判据"），预检不过的图禁用启动并把问题
  映射到画布标注。

### 5.4 选中与删除

- 点击节点/连线单选（顶层优先）；拖空白框选；`Shift` 加选；点击空白清除
  选中。选中态用 brand/弱强调底 + 描边。
- `Del`/右键删除选中节点（连带其连线）；删除是纯 UI 图操作，可即时撤销
  能力不在 M5 范围（DEC-015 P2），删除前不弹确认（有校验与事件列表兜底）。
- 任何被拒绝的图操作（类型不匹配、成环、多驱动等）以 toast/底列表反馈
  具体原因，不静默失败。

### 5.5 参数编辑（作用域与控件映射）

- **作用域**：右面板只编辑**当前选中节点实例**的参数（`ParamAssignment`）；
  未赋值参数显示声明默认值；编辑即经 `requestParamUpdate` 逐参数提交，
  同步拒绝（未知节点/参数、类型不匹配、越界）就地报错。类型化控件由
  `ParamDescriptor` 驱动（绑定逻辑为纯逻辑单测对象）：

  | ParamKind | 控件 | 约束呈现 |
  | --- | --- | --- |
  | Boolean | `switch` | — |
  | Integer | `slider` + `input` | hasRange 时夹取并显示区间 |
  | Real | `slider` + `input` | 同上 |
  | Enumeration | `dropdown`（`bindOpen`/`onOpenChange` 接线，台账 EUI-20260923-002 语义） | 选项取 `enumOptions` |
  | RealArray | 自研矩阵网格编辑器（行列数可配，如 KxK 卷积核） | 全部有限值校验（契约） |

- 运行中编辑即时生效（下一帧语义），面板顶部常驻"参数下一帧生效"提示；
  图结构变更（增删节点/连线）在 Running 下走"待生效 → `GraphApplied` 已
  生效"状态条。

### 5.6 中间结果查看

- 选中节点 → 右面板底部显示该节点最近 `NodeOutputSnapshot` 缩略图
  （`tryLoadNodeOutput`，每节点仅最新一幅，UI 侧不做有界缓存之外的保留）；
  缩略图按格式渲染（Gray8/Rgba8），随 `sourceSequence` 推进刷新，并显示
  分辨率与源帧序号。
- 悬空输出端口的节点天然可查看（契约允许悬空输出），作为"末端结果预览"
  的主要用法（对应 Blender Viewer 节点语义的轻量替代）。
- 上屏走 GpuFrameView 绕行路径（台账 EUI-20260923-003），缩略图降采样在
  提交侧完成（有界工作，`RULE-05`）。

### 5.7 执行状态与性能面板

- 工具栏徽标：`WorkflowEngineState` 四态（状态色经 `stateColor()` 扩展）。
- 右面板"工作流总览"（无选中时）：端到端 FPS、累计处理/丢弃帧、在飞任务数
  （`WorkflowStats`）；丢弃计数非零时以 warning 提示"过载丢弃中"（EXEC-07
  显式丢弃的可观察面，禁止静默）。
- 每节点耗时徽标：节点框底部 `avgCostMs`（`NodeStats`，滚动窗口均值），
  以等宽对齐小字呈现（DEC-005 mono 替代纪律）；选中节点在右面板展开
  last/avg/执行帧数。
- 逐节点实时"执行中"高亮不在 P0（契约无该状态，DEC-015 决策 4）。

### 5.8 运行控制

- 工具栏：启动（需当前图预检通过，否则禁用并提示问题数）/停止；无暂停
  （契约无暂停语义）。启动准入失败显示 `AdmissionResult.error`。
- 停止路径闭合：UI 发起 `stop()` 后按"排空"空态呈现（见第 4 节）；
  应用关闭走 `DslAppConfig::onShutdown` 既有顺序（先停工作流引擎与相机
  服务，再回收，EXEC-04），新增通道与任务纳入关闭回归。

## 6. EUI-NEO 原语映射

| UI 元素 | 原语/组件 | 纪律与已知风险 |
| --- | --- | --- |
| 页面骨架/五区布局 | `ui.column`/`ui.row` stack 组合 | 绘制顺序即层叠（zIndex 不跨父容器） |
| 全局导航 | `sidebar`（退化 `tabs`/`segmented`） | M5-02 原型验证开合/选中能力 |
| 画布视口 | `clip` 容器 + `ui.state`（视图变换） | 平移缩放只改变换，不重建子树 |
| 节点框 | `rect`（圆角 `kRadiusMd/Lg` 层级） | 选中/错误态描边用语义令牌 |
| 端口 | `rect` + `mousearea` | 命中区大于视觉尺寸 |
| 连线 | `polygon`（贝塞尔采样） | EUI-20260924-001：`dirtyKey` 键控或逐帧直绘，M5-03 预算绘制成本 |
| 选择框/拖线预览 | `rect`/`polygon` 浮层 | 根 stack 末位兄弟合成（DEC-005） |
| 右键菜单 | `contextmenu`（退化自绘浮层） | 同上；M5-03 原型验证 |
| 参数控件 | `switch`/`slider`/`input`/`dropdown` | dropdown 按 EUI-20260923-002 接线 |
| RealArray 矩阵编辑器 | 自研（`input` 网格 + 增删行列） | M5-04 工作量条目（DEC-015） |
| 快照缩略图 | `image::importGpuImage`（GpuFrameView 路径） | 禁走 `ImageStream`（EUI-20260923-003） |
| 节点耗时/统计 | `text`（等宽对齐）+ `barchart`（耗时条，可选） | mono 缺位用统一小数位+固定列对齐（DEC-005） |
| 校验/事件列表 | `virtuallist` 或 `datatable` | 条目点击定位画布节点 |
| 错误摘要弹窗 | `toast` | "弹窗摘要、面板全文"两层报错（调研 §3.1） |
| 端口/参数提示 | `tooltip` | 内容取 `ParamDescriptor.label` 与类型 |

原语映射在 `M5-03`/`M5-04` 原型阶段如遇组件能力缺口，按
[EUI-NEO 台账](../dependency_feedback/eui-neo/ledger.md)流程登记后再绕行；
不得静默引入另一套 UI 设施。

## 7. 平台无关纯逻辑清单（测试对象，`M5-03`..`M5-05`）

- 视图变换：screen↔canvas 互转、缩放锚点与上下限、帧全图拟合。
- 命中检测：节点/端口/连线命中（端口扩大命中区）、顶层优先、框选集合。
- 画布图同步：创建/删除/连线/替换边到 `WorkflowGraph` 的变换，操作前
  `validateWorkflowGraph` 预检与拒绝原因映射。
- 参数绑定：`ParamDescriptor` → 控件状态与校验（范围/枚举/有限性）。
- 调色板模型：`NodeCatalog` 分组与过滤。
- 统计管道：`WorkflowStats`/`NodeStats`/快照的 sequence 推进消费与
  停止排空语义。

## 8. 分期对照

- **P0 = M5 范围**：第 2..7 节全部内容。
- **P1（增强，需先扩 M4-09 契约的另行决策）**：Mute/Bypass、逐节点执行中
  高亮、断点/单步/从指定节点运行；纯 UI 项（Reroute、Frame 分组、backdrop、
  小地图、连线松手创建菜单）在 P0 验收后按需求排期。
- **P2/未排期**：节点内嵌 viewer、内联参数、subgraph、撤销重做、模板入口
  （`POST-04` 方向关联）。

P1/P2 的交互契约变更必须先走决策记录与 `M4-09` 契约测试同步，再进 UI。
