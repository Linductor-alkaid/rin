# M5：工作台 UI——导航、节点编辑器与性能面板

> 状态：In Progress
> 负责人：Linductor-alkaid（授权 Agent 按工程规范自治执行）
> 所属计划：[Rin 实施总计划](rin-implementation-plan.md)
> 前置：M3、`M4-09` 视图契约（骨架与假引擎先行，见
> [DEC-016](../decisions/DEC-016-contract-first-workbench-order.md)）；完成依赖
> M4 真引擎集成
> 建议发布点：v0.5.0
> 更新日期：2026-09-24

## 目标

把单页 viewer 升级为工作台应用：页面导航（预览 / 位姿 / 图像工作流 / 设置）、
拖拽式节点编辑器（调色板拖出创建、连线、参数面板、中间结果查看）、性能面板
（每节点耗时、端到端 FPS、丢弃计数），并以设计文档沉淀整个软件的界面信息架构与
操作逻辑拓扑树。

## 范围与非目标

范围：

- 决策与设计：DEC-014（工作台信息架构）、DEC-015（节点编辑器实现路径）、
  DEC-016（契约先行交错实施）、`docs/design/ui_workspace_design.md`（页面拓扑树 +
  交互流）。
- 契约假引擎（`M5-08`）：实现 `M4-09` 视图契约，供画布/面板先行开发调试。
- 导航壳与页面框架、viewer 主题令牌扩展。
- 节点编辑器画布与 M4 工作流引擎的 UI 集成。
- 性能面板与工作流运行控制。

非目标：

- 工作流图持久化（`POST-04`）。
- 多窗口 / 停靠布局（EUI-NEO 无停靠原语，按 `DEC-014` 采用单窗口页面模型）。
- 新增处理算子（M4 边界）。

## 设计与决策依据

- [viewer_design_system.md](../design/viewer_design_system.md)：ZCode Design
  System 令牌翻译规则，新页面与控件沿用 `viewer_theme.hpp` 唯一令牌层。
- [image_workflow_design.md](../design/image_workflow_design.md)（M4 产出）：
  引擎契约与统计语义，UI 只消费不重复实现。
- pinned EUI-NEO 能力面：`rect`/`polygon`/`mousearea`/`ui.state`/`loader`、
  `tabs`/`sidebar`/`slider`/`input`/`datatable`；无现成节点编辑器（`DEC-015`
  裁决自研画布实现路径；若确认框架缺口按 eui-neo 台账流程登记）。
- `RULE-05`：渲染线程只做有界校验、取帧与提交；节点画布交互状态在 UI 侧维护，
  执行与重活在引擎（Executor）侧。

## 工作项

- [x] `M5-01` 调研并冻结工作台信息架构与节点编辑器交互（`DEC-014` + `DEC-015`）：
      调研现代工具（Blender 合成器、TouchDesigner、Node-RED、Unreal Blueprint、
      Intel RealSense Viewer 等）的页面组织与节点交互范式，产出
      [ui_workspace_design.md](../design/ui_workspace_design.md)：页面拓扑树、
      导航模型、状态与空态、节点编辑器交互流（创建 / 连线 / 选中 / 删除 /
      参数编辑的作用域）与 EUI-NEO 原语映射。调研事实与可借鉴分析记录见
      [ui_workspace_research.md](../design/ui_workspace_research.md)
      （含用户指定的 RPA/影刀参考）。完成判据：设计文档完成并通过结构检查；
      [DEC-014](../decisions/DEC-014-workbench-information-architecture.md)/
      [DEC-015](../decisions/DEC-015-node-editor-implementation-path.md)
      `Accepted`。
- [ ] `M5-02` 导航壳与页面框架：页面导航（预览 / 位姿 / 工作流 / 设置）、页面
      切换状态保持、主题令牌扩展、现有预览功能回归。完成判据：导航状态单测 +
      预览回归记录。
- [ ] `M5-03` 节点编辑器画布：节点框渲染 / 选中 / 拖动、端口连线创建与删除、
      画布平移缩放、调色板拖出创建节点、画布图与引擎 `NodeGraph` 同步（含非法
      操作拒绝反馈）。完成判据：坐标 / 命中 / 图同步逻辑的平台无关单测通过。
- [ ] `M5-04` 参数面板与中间结果查看：选中节点的类型化参数编辑（滑块 / 输入 /
      下拉）、节点输出缩略图（有界缓存最近一帧）、节点错误可视化。完成判据：
      控件-参数绑定与中间结果路径测试通过。
- [ ] `M5-05` 性能面板：每节点耗时（滚动窗口）、端到端 FPS、丢弃计数；数据源为
      引擎与 `executor::comm` 统计。完成判据：统计管道单测 + UI 展示集成。
- [ ] `M5-06` 工作流运行控制：启动 / 停止、参数热更新语义（下一帧生效）、图结构
      变更时排空与重建。完成判据：引擎-UI 集成测试（含 `onShutdown` 关闭顺序
      回归）通过。
- [ ] `M5-07` M5 测试矩阵、真机验收与文档回写：全预设测试通过；D435if 真机端到
      端验收（拖拽搭图→运行→中间结果→性能面板）记录；`ui_workspace_design.md`
      与 CHANGELOG 回写。无设备时按工程规范第 4 节记录补跑条件。
- [ ] `M5-08` 契约假引擎（[DEC-016](../decisions/DEC-016-contract-first-workbench-order.md)，
      先于画布/面板实施）：按 `M4-09` 视图契约实现假工作流引擎——合成节点图与
      预置中间结果、仿真逐节点耗时/FPS/丢弃统计；承载与传递必须经 Executor 有限
      任务与 `executor::comm` 通道，复刻 `EXEC-07` 的有界在飞、显式丢弃、提交
      拒绝语义，并覆盖节点失败、取消、关闭路径。完成判据：假引擎通过与 `M4-07`
      真引擎共用的契约测试；`M5-03`..`M5-05` 仅依赖该假引擎开发。

## 实施顺序（DEC-016）

`M5-01` 调研冻结 DEC-014/015 → `M4-09` 视图契约 → `M5-08` 契约假引擎 →
`M5-02`..`M5-05` 骨架对假引擎开发调试 → M4 算子与真引擎（`M4-03`..`M4-08`）→
`M5-06` 假换真集成 → `M5-07` 验收收尾。骨架阶段的全部 UI 调试结论仅在假引擎
语义与真契约一致时有效；性能数字以真引擎实测为准。

## 风险与阻塞

- EUI-NEO 无现成节点编辑器，自研画布是全项目最大单项工作量：`M5-01` 调研与
  原型先行；交互几何（命中 / 连线）全部下沉为平台无关纯逻辑以保证可测试性。
- retained 渲染模型下画布局部更新与 z 序：遵守已知限制（无事件冒泡、zIndex 不跨
  父容器），必要时以绘制顺序与 `loader` 实例生命周期解决，并记录偏差。
- 页面增多后关闭顺序复杂化：所有新增通道与任务纳入 `EXEC-04` 关闭顺序回归。
- 假引擎语义失真导致 UI 调试结论无效（DEC-016）：假引擎与真引擎共用 `M4-09`
  契约测试；假引擎必须覆盖提交拒绝、节点失败、取消、关闭路径，不得只实现
  正常路径；性能面板不得以假引擎统计对外宣称性能。

## 测试与退出条件

- [ ] `ctest`（debug/asan/ubsan/tsan）全绿，新增：导航状态、画布几何 / 命中 /
      图同步、参数绑定、统计管道、引擎-UI 集成与关闭回归测试、`M5-08` 假引擎
      与 `M4-07` 真引擎共用的契约测试。
- [ ] `M5-06` 假换真集成回归记录（含 `onShutdown` 关闭顺序）。
- [ ] `DEC-014`/`DEC-015`/`DEC-016` 记录生效并被实现引用。
- [ ] `ui_workspace_design.md` 完成且与实现一致（页面拓扑树逐项可对应）。
- [ ] 真机端到端验收记录或规范化未执行记录（`M5-07`）。
- [ ] 文档同步：总计划 `SCOPE-09`、CHANGELOG（Unreleased）。

## 验证记录

2026-09-24：`M5-01` 调研输入完成（工作项未关闭）——新增
[ui_workspace_research.md](../design/ui_workspace_research.md)：三路并行外部调研
（影刀 RPA 深度调研——用户点名参考；RPA 编辑器横向对比 UiPath Studio/StudioX、
Power Automate Desktop、A360、来也/实在/阿里云/艺赛旗；节点式画布工具范式
Blender/TouchDesigner/Node-RED/Unreal Blueprint/RealSense Viewer 及
ComfyUI/n8n/Dify/Houdini），综合出对 DEC-014/015 的建议输入（单窗口页面导航 +
工作流页五区骨架；自由节点图主范式；P0/P1/P2 交互清单）与 EUI-NEO 实现风险
（retained layer polygon 缺陷对连线的同型影响、RealArray 卷积核矩阵编辑器需自研、
缩略图沿用 GpuFrameView 绕行路径）。证据等级：官方文档优先，SPA 未抓到处以多个
独立第三方来源交叉印证，未证实项在文档内逐条标注；约 50 个来源 URL 落盘。
`M5-01` 完成判据剩余项：`ui_workspace_design.md` 产出 + 结构检查、`DEC-014`/
`DEC-015` 记录并 `Accepted`。

2026-09-24：`M5-01` 完成关闭——用户确认调研结论（[ui_workspace_research.md](../design/ui_workspace_research.md)
第 5 节为基线）后冻结
[DEC-014](../decisions/DEC-014-workbench-information-architecture.md)（单窗口
四页导航 + 工作流页五区骨架 + 参数热更新"下一帧生效"明示 + 画布布局 UI 私有）
与 [DEC-015](../decisions/DEC-015-node-editor-implementation-path.md)（EUI-NEO
原语自研画布、交互几何纯逻辑下沉、P0/P1/P2 交互分期以 M4-09 契约为界、
retained layer polygon 缺陷按 EUI-20260924-001 先例规避），两记录 `Accepted`；
产出 [ui_workspace_design.md](../design/ui_workspace_design.md)（页面拓扑树、
导航模型、状态与空态、节点编辑器交互流、EUI-NEO 原语映射表、平台无关纯逻辑
测试清单、分期对照）。结构检查通过：单一级标题、层级连续、相对链接逐一命中
（含新增两决策记录与设计文档互链）、代码围栏配对、外部 URL 不作仓库内链接。
总计划 DEC-014/015 暂定条目同步转"已记录"，当前状态段更新。范围注记：P0
不含逐节点实时执行高亮与断点/单步（契约无对应语义，列为 P1 契约演进候选）。
