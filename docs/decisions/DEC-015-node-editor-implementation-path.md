# DEC-015：节点编辑器实现路径——EUI-NEO 原语自研画布与分期交互范式

> 状态：Accepted
> 日期：2026-09-24
> 负责人：Linductor-alkaid
> 冻结里程碑：M5
> 替代/被替代：无（取代总计划中 DEC-015 的暂定默认值）

## 背景与问题

EUI-NEO（pinned `782c5699`）无节点编辑器、画布平移缩放、小地图等现成组件；
节点画布是 M5 全项目最大单项工作量，需在实施前冻结实现路径与交互范式范围。
调研输入：[ui_workspace_research.md](../design/ui_workspace_research.md)
（节点画布工具交互公约数 + RPA 可迁移模式；2026-09-24 用户确认以该调研第 5 节
为基线冻结本决策）。约束：M4-09 视图契约已冻结 UI 数据面；`RULE-01` 公开头
零第三方类型；`RULE-05` UI 线程只做有界工作。

## 决策

1. **EUI-NEO 原语自研画布**：`rect`（节点框/端口/选择框）+ `polygon`（连线
   贝塞尔采样）+ `mousearea`（命中与拖拽）+ `ui.state`（视图变换与画布状态）
   + `loader`（实例生命周期）。不引入第三方节点编辑器依赖。
2. **交互几何下沉为平台无关纯逻辑**并独立单测：视图变换（平移/缩放/帧全图）、
   命中检测（节点/端口，命中区大于视觉尺寸）、画布操作到 `WorkflowGraph` 的
   同步与拒绝反馈（UI 预检复用 `validateWorkflowGraph`）、`ParamDescriptor`
   到控件状态的绑定、调色板分组过滤模型。
3. **P0 交互范式**（M5-03..M5-05 范围）：画布平移/缩放/框选/F 帧全图；双通道
   创建（调色板拖出 + 画布右键搜索菜单，同一 `NodeCatalog` 数据源）；连线
   （拖线中类型即时过滤、不兼容端口显式拒绝、`Alt+点击` 删线、贝塞尔走线、
   输入端口单驱动、输出扇出与悬空允许）；三栏布局与类型化参数面板
   （Boolean→开关、Integer/Real→滑条+输入、Enumeration→下拉、RealArray→
   自研矩阵网格编辑器，自定义卷积核用）；选中节点侧栏查看最近
   `NodeOutputSnapshot` 缩略图；校验问题列表 + 画布定位标注；运行控制与
   引擎状态徽标。
4. **节点视觉状态编码（P0，以契约为准）**：选中（brand/弱强调底）、校验错误
   （destructive 描边 + 列表条目）、执行失败（destructive + `NodeFailed`
   事件消息）；每节点耗时徽标取 `NodeStats`。**契约无逐节点"执行中"状态**，
   ComfyUI 式实时执行高亮列为 P1 契约演进候选（需扩展 M4-09，另行决策），
   不在本里程碑范围。
5. **P1（增强，契约不变项优先）**：Mute/Bypass 禁用语义（需先扩 M4-09 契约，
   未扩前不做 UI）、Reroute 拐点、Frame 分组框、backdrop 画布背景预览、
   小地图、断点/单步/从指定节点运行（需扩契约，另行决策）。**P2/不做**：
   节点内嵌交互 viewer、节点内联参数控件、subgraph 组合节点、撤销重做
   （未排期，需求驱动）。
6. **已知框架缺陷对策**：连线等逐帧变化 polygon 按台账 EUI-20260924-001
   先例处理（`dirtyKey` 键控或逐帧直绘），画布元素数量与绘制成本纳入
   M5-03 原型预算；快照缩略图上屏沿用 GpuFrameView 绕行路径
   （EUI-20260923-003 Open），不得走 `ImageStream`；浮层（右键菜单/拖线
   悬浮提示）遵循"根 stack 末位兄弟合成"层叠纪律（DEC-005）。

## 备选方案

- 引入第三方节点编辑器库（ImNodes/ImGuIZMO 类）：被否——`RULE-01`/`RULE-04`
  要求新依赖 pin + 台账 + 公开头零第三方类型，且即时模式库与 EUI-NEO retained
  模型不匹配，集成成本高于自研薄画布。
- 以 `datatable`/`virtuallist` 模拟节点列表（影刀式步骤表）：被否——无法表达
  DAG 拓扑与类型化连线，违背 SCOPE-09"拖拽式节点编辑器"边界。
- 全部交互依赖 EUI-NEO 现成组件拼装：被否——组件粒度（卡片/列表）无法控制
  命中、层叠与画布变换细节；组件仅用于面板层。

## 影响与风险

- 自研画布工作量最大：交互几何纯逻辑先行 + `M5-08` 假引擎驱动调试（DEC-016
  顺序）降低集成风险；性能结论以真引擎实测为准。
- 键盘操作（`F` 帧全图、`Del` 删除等）依赖 EUI-NEO 键盘事件暴露程度，
  M5-03 原型确认；不足时按 eui-neo 台账流程登记缺口后调整交互映射
  （鼠标等价路径必须始终存在）。
- 假引擎语义失真会使画布调试结论无效：假引擎与真引擎共用 M4-09 契约测试
  （DEC-016 既有约束）。

## 验证方式

- `M5-03` 坐标/命中/图同步纯逻辑单测；`M5-04` 控件-参数绑定与中间结果路径
  测试；`M5-05` 统计管道单测。
- `M5-08` 假引擎契约测试通过后驱动画布联调；`M5-06` 假换真集成回归；
  `M5-07` 真机端到端验收。

## 关联文档和工作项

- 总计划：`SCOPE-09`、DEC-015 暂定条目（本记录转"已记录"）。
- [m5-ui-workbench.md](../plans/m5-ui-workbench.md)：`M5-01`（本决策冻结）、
  `M5-03`..`M5-06`、`M5-08`。
- [ui_workspace_design.md](../design/ui_workspace_design.md)（交互流与原语
  映射设计落点）、
  [ui_workspace_research.md](../design/ui_workspace_research.md)（调研依据）、
  [workflow_types.hpp](../../include/rin/workflow_types.hpp) /
  [workflow_engine.hpp](../../include/rin/workflow_engine.hpp)（M4-09 契约）、
  [DEC-014](DEC-014-workbench-information-architecture.md)（信息架构）、
  [EUI-NEO 台账](../dependency_feedback/eui-neo/ledger.md)
  （EUI-20260923-003、EUI-20260924-001）。
