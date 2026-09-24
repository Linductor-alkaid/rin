# DEC-014：工作台信息架构——单窗口页面导航与工作流页五区骨架

> 状态：Accepted
> 日期：2026-09-24
> 负责人：Linductor-alkaid
> 冻结里程碑：M5
> 替代/被替代：无（取代总计划中 DEC-014 的暂定默认值）

## 背景与问题

M5 把单页 viewer 升级为工作台应用，需要冻结页面组织与导航模型：页面如何划分、
页面切换如何保持状态、图像工作流页（节点画布 + 参数面板 + 性能面板 + 运行控制）
内部如何布局。约束：EUI-NEO 无停靠/多窗口原语（多窗口与停靠布局在 M5 非目标）；
调研输入见 [ui_workspace_research.md](../design/ui_workspace_research.md)
（RPA 五区布局与节点画布工具的三栏共识；2026-09-24 用户确认以该调研第 5 节为
基线冻结本决策）。

## 决策

1. **单窗口 + 页面导航**：预览 / 位姿 / 图像工作流 / 设置 四页；左侧窄边全局
   导航（图标 + 文字）。页面切换不销毁会话状态，各页 UI 状态保持；相机服务
   全局共享运行态，切换页面不停止采集。实现控件以 EUI-NEO `sidebar` 组件为
   首选，原型受限时退化 `tabs`/`segmented`（信息架构不变，映射见设计文档）。
2. **图像工作流页五区骨架**（RPA 验证过的布局范式）：
   - 顶：工具栏——启动/停止（`IWorkflowEngine::start`/`stop`）、引擎状态徽标
     （`WorkflowEngineState`）、校验状态入口、帧全图操作；
   - 左：节点调色板——`NodeCatalog` 分类分组 + 顶部即输即筛；
   - 中：节点画布（交互范式见 [DEC-015](DEC-015-node-editor-implementation-path.md)）；
   - 右：上下文面板——选中节点时渲染类型化参数编辑与该节点最近中间产物快照；
     无选中时显示工作流总览（端到端 FPS、累计处理/丢弃、在飞）；
   - 底：校验问题与引擎事件列表（可折叠，条目可定位到画布节点）。
3. **预览/位姿页**沿用现有卡片式组织（Intel RealSense Viewer 式"模块卡片 +
   行内展开"），设置页收纳深度配色切换（DEC-007）与关于/版本信息。节点图
   不得吞噬简单预览场景的易用性。
4. **参数热更新语义**：采用 M4-09 契约的"下一帧生效"（`requestParamUpdate`），
   UI 明示实时生效；不引入 Node-RED 式"编辑态/运行态分离 + deploy"（Rin 无此
   需求）。图结构变更在 Running 下经 `applyGraph` 入队、帧边界排空重建，
   以 `GraphApplied` 事件在画布反馈"待生效/已生效"。
5. **画布布局（节点坐标）是 UI 私有状态**，不进入 M4-09 契约与持久化
   （`workflow_types.hpp` 契约注释既定；图持久化见 `POST-04`）。

## 备选方案

- 多窗口/停靠布局：被否——EUI-NEO 无停靠原语，M5 非目标（总计划）。
- 影刀式线性步骤卡片作为工作流主范式：被否——Rin 是类型驱动 DAG 数据流，
  存在并行扇出/合流；RPA 行业自身将复杂控制流交给自由图（调研 §3.2/§5.1）。
- 顶部 tabs 作为全局导航首选：降级为后备（`sidebar` 原型受限时使用），
  不影响信息架构。
- Node-RED 式 deploy 粒度部署：被否——与契约"下一帧生效 + 帧边界重建"语义
  重复，徒增心智负担。

## 影响与风险

- 页面增多使关闭顺序复杂化：新增通道与任务全部纳入 `EXEC-04` 关闭顺序回归
  （M5 风险表既有条目）。
- 页面切换时工作流引擎继续运行（引擎侧承载）：返回页面经契约 `tryLoad*`
  最新态语义重新消费；停止/关闭路径上 UI 派生状态（快照缩略图、性能面板）
  必须显式排空，不得以 stale 数据恢复活动状态（`workflow_engine.hpp` `stop`
  契约注释）。
- `sidebar` 组件开合/选中能力未经真机验证：M5-02 原型先行，受限时按台账
  流程登记并退化 tabs。

## 验证方式

- M5-02 导航状态单测 + 现有预览功能回归记录。
- 真机截图对照 [viewer 设计系统基准](../design/viewer_design_system.md)（DEC-005
  令牌纪律：新增视觉需求先扩令牌）。
- M5-07 真机端到端验收（页面导航 → 搭图 → 运行 → 中间结果 → 性能面板）。

## 关联文档和工作项

- 总计划：`SCOPE-09`、`EXEC-04`、DEC-014 暂定条目（本记录转"已记录"）。
- [m5-ui-workbench.md](../plans/m5-ui-workbench.md)：`M5-01`（本决策冻结）、
  `M5-02`..`M5-07`。
- [ui_workspace_design.md](../design/ui_workspace_design.md)（页面拓扑树与
  原语映射的设计落点）、
  [ui_workspace_research.md](../design/ui_workspace_research.md)（调研依据）、
  [workflow_types.hpp](../../include/rin/workflow_types.hpp)（M4-09 契约）、
  [DEC-005](DEC-005-viewer-visual-design.md)（视觉令牌）、
  [DEC-015](DEC-015-node-editor-implementation-path.md)（节点编辑器实现路径）、
  [DEC-016](DEC-016-contract-first-workbench-order.md)（交错实施）。
