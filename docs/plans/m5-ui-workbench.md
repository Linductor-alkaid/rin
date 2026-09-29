# M5：工作台 UI——导航、节点编辑器与性能面板

> 状态：Complete
> 负责人：Linductor-alkaid（授权 Agent 按工程规范自治执行）
> 所属计划：[Rin 实施总计划](rin-implementation-plan.md)
> 前置：M3、`M4-09` 视图契约（骨架与假引擎先行，见
> [DEC-016](../decisions/DEC-016-contract-first-workbench-order.md)）；完成依赖
> M4 真引擎集成
> 建议发布点：v0.5.0
> 更新日期：2026-09-29

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
- [x] `M5-02` 导航壳与页面框架：页面导航（预览 / 位姿 / 工作流 / 设置）、页面
      切换状态保持、主题令牌扩展、现有预览功能回归。完成判据：导航状态单测 +
      预览回归记录。
- [x] `M5-03` 节点编辑器画布：节点框渲染 / 选中 / 拖动、端口连线创建与删除、
      画布平移缩放、调色板拖出创建节点、画布图与引擎 `NodeGraph` 同步（含非法
      操作拒绝反馈）。完成判据：坐标 / 命中 / 图同步逻辑的平台无关单测通过。
- [x] `M5-04` 参数面板与中间结果查看：选中节点的类型化参数编辑（滑块 / 输入 /
      下拉）、节点输出缩略图（有界缓存最近一帧）、节点错误可视化。完成判据：
      控件-参数绑定与中间结果路径测试通过。
- [x] `M5-05` 性能面板：每节点耗时（滚动窗口）、端到端 FPS、丢弃计数；数据源为
      引擎与 `executor::comm` 统计。完成判据：统计管道单测 + UI 展示集成。
- [x] `M5-06` 工作流运行控制：启动 / 停止、参数热更新语义（下一帧生效）、图结构
      变更时排空与重建。完成判据：引擎-UI 集成测试（含 `onShutdown` 关闭顺序
      回归）通过。
- [x] `M5-07` M5 测试矩阵、真机验收与文档回写：全预设测试通过；D435if 真机端到
      端验收（拖拽搭图→运行→中间结果→性能面板）记录；`ui_workspace_design.md`
      与 CHANGELOG 回写。无设备时按工程规范第 4 节记录补跑条件。
- [x] `M5-08` 契约假引擎（[DEC-016](../decisions/DEC-016-contract-first-workbench-order.md)，
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

- [x] `ctest`（debug/asan/ubsan/tsan）全绿，新增：导航状态、画布几何 / 命中 /
      图同步、参数绑定、统计管道、引擎-UI 集成与关闭回归测试、`M5-08` 假引擎
      与 `M4-07` 真引擎共用的契约测试。
- [x] `M5-06` 假换真集成回归记录（含 `onShutdown` 关闭顺序）。
- [x] `DEC-014`/`DEC-015`/`DEC-016` 记录生效并被实现引用。
- [x] `ui_workspace_design.md` 完成且与实现一致（页面拓扑树逐项可对应）。
- [x] 真机端到端验收记录或规范化未执行记录（`M5-07`）。
- [x] 文档同步：总计划 `SCOPE-09`、CHANGELOG（Unreleased）。

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

2026-09-24：`M5-08` 完成关闭——新增 `rin::workflow_fake` 静态库
（src/workflow/fake_engine.{hpp,cpp}）实现 `M4-09` 视图契约（IWorkflowEngine）：
合成源经 Executor 周期任务（timer 能力）驱动，每帧 `submit_cancellable` 有限任务
拓扑序执行；有界在飞（maxInFlight）显式丢弃计数与提交拒绝显式事件（EXEC-07）；
图替换走 `LatestMailbox` 最新态、参数热更新走 `MpscChannel` 逐条 FIFO、统计/产物/
事件走 `LatestMailbox`（`executor::comm` 选型按语义，AGENTS 规则 4）；Running 下
图与参数在帧边界排空生效（"下一帧生效"），Idle 下参数同步生效；节点失败（可注入）
→ NodeFailed → Failed → stop 回 Idle 可重启；stop 排空消费全部 future（TaskCancelled
与任务异常区分，取消不计失败），幂等，返回后无新发布、通道 stale 值可读（EXEC-04）。
契约澄清：workflow_engine.hpp `tryLoadNodeOutput` 注释按 workflow_types.hpp 连线
语义与 ui_workspace_design.md §5.6 修正（悬空输出端口的末端节点同样保留可查看，
原"悬空输出不保留"表述与两文档矛盾）。测试（Independent-Verification-Agent 独立
编写执行）：新增与 `M4-07` 真引擎共用的契约套件
tests/workflow_engine_contract_suite.hpp（工厂参数化，图从 catalog() 泛式构造，
M4-07 接入时复用）与 tests/test_workflow_fake_engine.cpp（共用套件 + 假引擎特有：
工厂校验/有界在飞显式丢弃/crop 参数下一帧生效/仿真耗时注入/会话复位与 sourceSequence
跨会话单调/Idle 参数同步生效/executor 关停鲁棒），160 项检查 debug/asan/ubsan/tsan
四预设 0 失败（tsan 因本机内核 ASLR 限制需 `setarch -R` 运行，改动前既有 tsan 测试
同样受影响；CI 需固定 `vm.mmap_rnd_bits` 或以 setarch 包裹）；debug 全量 ctest
14 通过 1 跳过（realsense_hardware 无设备）。第一轮独立验证暴露两处测试竞态设计
缺陷（最新态事件通道的中间事件单会话采样过度断言、会话复位阈值余量不足），由验证
代理修复后复验通过，无实现缺陷。环境：x86_64 Linux，GCC 13，CMake presets
debug/asan/ubsan/tsan。`M5-02`..`M5-05` 按计划仅依赖该假引擎开发。

2026-09-28：`M5-08` 缺陷修复补记（随 `M5-02` MR 的 CI 修复提交入账）——CI
（ubuntu runner / GCC 13 / asan）实报 `test_workflow_fake_engine` heap-use-after-free：
`stop()` 持 `lifecycleMutex_` cancel timer 后，一个已被执行器派发的掉队 tick 阻塞在
该锁上，`engine.reset()` 析构完成后 tick 被唤醒读已释放的 `state_`（本机时序快不
复现）。根因是 tick 闭包裸捕获 `this` 的生命周期缺口，非 Executor 能力缺口（不涉
台账）。修复：`FakeWorkflowEngine` 继承 `enable_shared_from_this`，tick 闭包只持
`weak_ptr`，worker 侧 `lock()` 提升强引用后才触碰成员——提升失败安全退出、成功则
对象存活至 tick 退出（同时闭合 worker 在提交点后被抢占的窗口）；头文件生命周期措辞
同步。测试（Independent-Verification-Agent 独立验证，含修复方案两轮对照压测）：
新增用例 11（25 轮 1ms tick 过载 + stop + reset 序列，预修复命中率 42%）；契约套件
两处抗抖修正（契约中立：停止后断言改为可重复稳定性断言而非"必然有产物"；失败路径
静默窗语义修正）；asan 累计 382 次运行 0 UAF（预修复同强度可命中），debug/asan/
ubsan 全量 ctest 16/16，tsan 目标测试通过。**遗留缺陷（另行处理，不阻塞）**：验证
过程发现 Running 下 `applyGraph` 的待生效图在 `drainBoundary` 中从不消费
（LatestMailbox peek 语义），导致每帧重建 generation 并重发 `GraphApplied`（功能
无害，代 churn 与事件刷屏）；涉及消费语义与事件频率决策，M4-07 真引擎对齐时一并
处理（见 `M4-07` 对齐项）。

2026-09-28：`M5-02` 完成关闭——工作台壳落地（DEC-014 四页 IA）：新增
`apps/viewer/navigation.hpp`（`WorkbenchPage` 四页模型 + 元数据表 + `NavigationState`
纯逻辑与 `composeNavRail` 左窄边导航栏组装；导航只拥有当前页，页面 UI 状态由
`ViewerContext`/`ui.state` 持有实现切页保持，相机服务运行态全局共享）与
`apps/viewer/workflow_shell.hpp`（五区骨架静态框架，空态文案按设计文档 §4，
M5-03 起逐区替换）；`app.cpp` 重构为 rail + 全局状态头 + 四页 switch 布局；
`viewer_theme.hpp` 扩令牌：`workflowStateColor`（§4 规格四态映射）与端口类型色
`portTypes()`/`portTypeColor`（§5.3，M5-03 消费）；深度配色选择按 DEC-014 决策 3
移入设置页（含关于/版本，`RIN_VERSION` 编译定义接 `project(Rin VERSION)`）；
M1 数字键分辨率速选收窄到预览页（控件所在页）。导航控件选型按设计文档 §3/§6
原型验证结论执行：`sidebar` 为右锚定模态抽屉、`tabs`/`segmented` 为横向选择器、
`navbar` 未列入上游组件文档且绑定组件库自有主题度量体系（与 DEC-005 冲突），按
[EUI-20260928-001](../dependency_feedback/eui-neo/ledger.md) 以 `rect`/`text` +
viewer 令牌自绘导航栏（composeSelect 同类先例），设计文档 §3/§6 映射已同步；图标
为框架捆绑 Font Awesome 7 Free Solid（codepoint 经字体 cmap 验证）。默认窗口
1280x860 → 1920x1200（五区骨架最小版面；EUI `Screen` 为逻辑尺寸，XWayland 2x
面板上逻辑视口为窗口一半，分辨率选择器改为内容区严格右锚定）。测试
（Independent-Verification-Agent 独立编写执行）：新增 tests/test_navigation.cpp
（元数据表完整性/索引归一/4x4 切换矩阵/同页 no-op/值语义/"导航不拥有页面状态"
见证结构/static_assert 编译期锁定/主题映射逐字段一致），并应实现加固（switchTo
范围外值经 `workbenchPageIndex` 归一、`workbenchPageId` 转 constexpr）补范围外
归一契约用例，308 项检查 debug/asan/ubsan 三预设 0 失败；全量 ctest 四预设
（tsan 按 `setarch -R` 既有纪律）16/16 通过（含 D435if 真机硬件冒烟）。真机回归
（D435if 直连，截图存档 `screenshots/m5-02/`）：预览页实时 RGB/深度 jet 出流 +
设备/分辨率选择 + 内参卡；位姿页 3D 位姿视图 + IMU 面板实时；工作流页五区骨架
与空态；设置页配色切换 Jet→Grayscale 即时生效（DEC-007 命令通道，头部事件行
同步）且跨页返回后设备/分辨率选择保持（切页状态保持）；经 WM_DELETE_WINDOW
标准关闭路径干净退出（onShutdown 顺序闭合，EXEC-04）。环境：x86_64 Linux
（GNOME/XWayland，3200x2000 @2x），GCC 13。`M5-03`..`M5-05` 依赖导航壳与假引擎
（`M5-08`）继续。

2026-09-28：`M5-03` 完成关闭——节点编辑器画布落地（对 `M5-08` 契约假引擎开发
调试，DEC-016）。新增 `apps/viewer/canvas_model.hpp`（DEC-015 交互几何下沉的
平台无关纯逻辑层）：视图变换（screen↔canvas 互转、指针锚点缩放 [0.4,2.5]、
帧全图拟合、视图定心）、命中检测（端口扩大命中区 12px > 视觉 4px、节点顶层
优先、连线贝塞尔采样距离 ≤7px、框选集合）、画布图模型（节点位置 UI 私有 +
`WorkflowGraph` 变换；连线经 `validateWorkflowGraph` 预检——成环/类型不匹配/
自环/越界拒绝并回滚，悬空输入为搭图中间态只做校验标注不阻塞，目标输入已有
入边替换旧边，重复同边幂等）、交互状态机（按下优先级 端口>节点>连线>空白；
平移=中键拖拽、左键拖空白=框选[>4px 阈值]、节点拖动含组拖动、连线拖拽即时
类型过滤、Alt+点击删线、右键删节点/空白弹创建菜单、Del 删选中）与调色板模型
（目录分组归类 + 即输即筛，未知类型归 Other 不丢项）。新增
`apps/viewer/node_canvas.hpp` 组装层（替代 M5-02 `workflow_shell.hpp` 静态
骨架，五区几何不变）：画布视口 stack+clip、节点/端口 rect、连线 polygon
（修订号作显式脏键，EUI-20260924-001 绕行）、拖线预览与框选浮层、
`components::mouseArea` 单热区承载全部指针语义（节点/连线为非交互视觉）、
调色板过滤框 `components::input`（样式字段逐一取 viewer 令牌，不引入组件
主题度量）、右键创建菜单自绘浮层（EUI-20260928-001 先例）与调色板拖拽跟随
浮层；工具栏含 Fit 按钮（F 键鼠标等价路径）与校验状态行，底区校验问题列表
（点击定位节点）+ 引擎事件行。app.cpp：ViewerContext 持有假引擎实例与画布
会话状态（切页保持），图变更统一经 `applyGraph` 同步（UI 预检与引擎准入共用
唯一判据，§5.3）；工作流页快捷键 F/Del/Esc（每项有鼠标等价路径）；关闭顺序
在服务停止后补引擎 stop+释放（fake_engine.hpp 生命周期纪律，EXEC-04）。
§5.1 原文"平移=拖空白处"与 §5.4"拖空白框选"冲突在设计文档补记冻结消解
（左键框选/中键平移），右键菜单与过滤框映射同步落地注记。测试
（Independent-Verification-Agent 独立编写执行，两轮）：`tests/test_node_canvas.cpp`
17 分区 422 项检查——视图变换（锚点缩放不变量/上下限夹取/拟合）、几何、图
模型（创建/删除连带边/替换边/成环拒绝/悬空非阻塞/校验缓存）、选中与命中
（顶层优先/扩大命中区/连线最近距离/框选）、交互状态机（框选/Shift 加选/组
拖动/连线拖拽与取消/右键/Del/平移）、调色板分组过滤；debug/asan/ubsan 三
预设 0 失败。第一轮验证暴露三处实现缺陷（点选节点后拖动错误节点的
use-after-rotate、按下优先级与文档矛盾、组拖动缺失），主循环修复后第二轮
复验 PASS 并以回归守卫锁定。全量 ctest 四预设 17/17（tsan 按 `setarch -R`
既有纪律）；真机启动冒烟（D435if 直连）：应用启动后假引擎接线、预览页出流
渲染 20s 稳定无崩溃（工作流页 compose 路径需输入自动化，交互与视觉验收按
既定纪律归 `M5-07` 真机端到端）。环境：x86_64 Linux（GNOME/Wayland），GCC 13。
`M5-04`（参数面板与中间结果查看）为下一工作项。

2026-09-28：`M5-04` 完成关闭——参数面板与中间结果查看落地（对 `M5-08` 契约
假引擎开发调试，DEC-016）。新增 `apps/viewer/param_model.hpp`（§5.5/§5.6/§7
平台无关纯逻辑层）：控件-参数绑定（严格文本解析、值对声明校验——种类匹配/
范围闭区间/枚举选项/RealArray 有限性、滑条归一化映射与 Integer 取整、生效值
解析"未赋值取声明默认"、控件初值文本）、`RealArrayGrid` 矩阵网格（扁平数据
推断 KxK/1xN、重排尺寸行主序截断/零扩展）、`NodeOutputCache` 有界缓存（每节点
仅最新一幅、容量 4 LRU 驱逐、停止/关闭排空）、`thumbnailRgbaFromSnapshot`
（Gray8/Rgba8 按格式、最近邻只缩不放最长边 ≤256、stride 处理、sourceSequence
透传）、`NodeFailureMarks`（NodeFailed 置位、Started/Stopped 清空的会话级
标注）。`canvas_model.hpp` 扩展：`CanvasNode.params` + `setParam`（替换同
paramId/追加；`toGraph` 携带赋值——图结构变更重建不丢参数；无效赋值经校验
标注 BadParam 呈现）。新增 `apps/viewer/param_panel.hpp` 组装层（上下文面板
与五区组装自 node_canvas.hpp 迁入）：右面板参数编辑按 §5.5 控件映射落地
（Boolean→`toggleSwitch`、Integer/Real→`slider`+`input`（hasRange 显示区间、
滑条归一化夹取）、Enumeration→`dropdown`（`bindOpen`/`onOpenChange` 外接开合，
台账 EUI-20260923-002）、RealArray→自研矩阵网格（行列步进 + 单元文本，全格
有限值校验））；编辑即经 `requestParamUpdate` 逐参数提交，同步拒绝就地报错，
引擎接受后同步记入画布模型（不经 `applyGraph`——Running 下重复图重建触发
事件刷屏，假引擎已知重建语义 M4-07 对齐项）；"参数下一帧生效"常驻提示；
参数行超出面板参数区下限截断并计数提示。底部缩略图（§5.6：GpuFrameView 绕行
路径 EUI-20260923-003，pump 边界消费；引擎非 Running 整体排空，§4 停止/关闭
排空语义）。节点错误可视化（§4）：NodeFailed 事件经 `NodeFailureMarks` →
画布节点 destructive 徽标 + 面板失败徽标 + 消息入底部事件列表。组件接线纪律
（DEC-005，设计文档 §5.5 补记同步）：颜色字段全部取 viewer 令牌显式赋值
（SliderStyle/SwitchStyle/DropdownStyle/InputStyle），几何度量沿用组件默认
（与 M5-03 input 显式字号/inset 并列记录）；slider/switch/dropdown 构建器无
position，统一包裹定位 stack；下拉弹层浮于后续参数行依赖同父容器 zIndex
（展开时抬升）；面板于五区中最后合成（弹层可浮于底部列表）。app.cpp：
`pumpNodeOutput` 消费（RULE-05 有界）、onShutdown 面板排空（缩略图清空/控件
绑定复位/失败标注清空）。测试（Independent-Verification-Agent 独立编写执行，
两轮）：`tests/test_param_panel.cpp` 12 分区 310 项检查——解析/校验/滑条映射/
生效值/矩阵网格/假引擎拉取与 LRU 驱逐与排空/缩略图降采样与 stride/失败标注
事件语义/画布 setParam 与 toGraph 携带/connect 预检携带参数；debug/asan/ubsan
/tsan 四预设 0 失败，debug 全量 ctest 18/18（含 D435if 真机硬件冒烟）。
**冒烟发现并修复 M5-03 潜伏缺陷**：`composeWorkflowPalette` 目录实参三目
`*catalog : NodeCatalog{}` 左值/纯右值混合，左值分支被拷贝为临时目录、语句
结束时析构，`PaletteGroup::items` 持有的节点指针全部悬垂（分组标题为值拷贝
而幸存）——工作流页 compose 路径此前从未真机执行过（M5-03 冒烟停留在预览页），
M5-04 冒烟（临时本地补丁：预置三节点图 + 选中节点 + 启动引擎注入延迟故障，
验证后已回退不入库）实证 `bad_alloc` 崩溃；修复为 `static const` 空目录使
三目两侧同为左值，复验真机运行 24s+ 稳定（RSS 平稳无增长，引擎启动/参数热
更新/缩略图消费/注入失败→Failed 排空路径全通过）+ 最终代码四预设全量复验。
环境：x86_64 Linux（GNOME/XWayland），GCC 13。面板交互与视觉验收按既定纪律
归 `M5-07` 真机端到端。复验过程另记录一项与本次无关的既有抖动：tsan+真机
组合下 `realsense_hardware` 的 M3-08 restream 后 EMA 频率恢复断言 8 次中 2 次
越界（插桩拖慢宿主时序，比值 0.483 贴近 0.5 下限；debug/asan/ubsan 确定性
通过，最终 tsan 全量全绿）——后续可评估放宽测量窗或重试语义。
`M5-05`（性能面板）为下一工作项。

2026-09-28：连线渲染修复（用户真机视觉反馈，随 `M5-04` 分支入账）——画布
连线被绘制成"两点连线与曲线的封闭区域"。根因：EUI-NEO `polygon` 原语为填充
语义（点集三角形扇、末点直连首点，`appendPolygonTriangleFan`），M5-03 将开放
贝塞尔采样点集直接提交，渲染为弦-曲线封闭面而非线条；设计文档 §5.3"polygon
采样渲染"的隐含"折线描边"假设不成立。修复：`canvas_model.hpp` 新增
`wireRibbon`（贝塞尔采样中心线 + 逐点切线法向偏移 ±width/2 的带状封闭轮廓，
`kWireWidth`=2.5 画布单位随缩放），`node_canvas.hpp` 连线与拖线预览改用之——
粗细一致且随曲线弯曲，与 M3 位姿视图 `segmentToQuad` 同一先例；命中检测
（`wireAt`）几何不变。拖线预览补零长度守卫（光标停回发起端口时控制点下限
使中心线外凸成小环；已提交连线拒绝自连，仅预览可达，不绘制）。测试
（Independent-Verification-Agent 独立编写执行）：`test_node_canvas.cpp` 新增
`wire_ribbon` 分区 46 项检查（配对几何/粗细一致/中心线重合/法向随解析贝塞尔
导数旋转/取向回归守卫/退化输入），468 项检查 debug/asan/ubsan/tsan 四预设
0 失败（tsan 3 次稳定）。真机冒烟（临时本地补丁预置两连线图，验证后回退）：
工作流页 30fps 重绘 16s+ 稳定，RSS 平稳。视觉确认待用户在 MR 分支复核。

2026-09-28：`M5-05` 完成关闭——性能面板落地（对 `M5-08` 契约假引擎开发调试，
DEC-016）。新增 `apps/viewer/perf_model.hpp`（§5.7/§4/§7 平台无关纯逻辑层）：
`WorkflowPerfState` 统计管道（契约 `tryLoadStats` 的 sequence 推进消费——
最新态语义合并中间快照、已见序号水位只前进；停止/失败冻结语义：末次值保留
呈现 + `live` 标志区分活动/冻结，落实 §4"性能面板冻结为末次值并标注已停止"——
与快照缩略图的清空排空不同；`clear()` 关闭排空保留序号水位）；`formatCostMs`/
`formatFps` 统一小数位 + 固定后缀（DEC-005 mono 替代纪律）。UI 集成：工具栏
引擎状态徽标（四态点标 + `toString`，色经 M5-02 `workflowStateColor` 扩展；
启动/停止按钮归 `M5-06` 运行控制）；节点框底部页脚带 `avgCostMs` 徽标（字号
随缩放，无统计不绘制）；右面板无选中时"工作流总览"（端到端 FPS/累计处理/
丢弃/在飞四行，数值右对齐固定列；dropped>0 追加 warning 行
"overload - frames are being dropped"（EXEC-07 显式丢弃可观察面，禁止静默）；
非 Running 冻结标注 stopped；从未运行显示空态文案），选中节点展开
last/avg/执行帧数一行。app.cpp：pump 边界 consume（RULE-05 有界），
onShutdown `perf.clear()` 排空（EXEC-04 关闭顺序，与 M5-04 面板排空同址）。
测试（Independent-Verification-Agent 独立编写执行，两轮）：
`tests/test_perf_panel.cpp` 4 分区 736 项检查——格式化、初态/Idle 拒绝、
sequence 推进消费与最新态合并、节点统计（注入常数耗时断言 last/avg）、停止
冻结与重启衔接（会话计数复位、序号跨会话单调）、失败冻结、clear 排空不复活；
第一轮实证一处实现缺陷：`clear()` 整体复位使已见序号归零，排空后对已停止
引擎 consume 复活 stale 快照（违反 §4 排空语义，M3-07 先例同纪律），主循环
修复为保留水位后复验转绿并以回归守卫锁定。复验 debug/asan/ubsan/tsan 四预设
736/0（tsan 0 警告，`setarch -R` 既有纪律），debug 全量 ctest 19/19（含
D435if 真机硬件冒烟）。真机冒烟（D435if 直连，临时本地补丁预置三节点图 +
脚本化选择/停止时序，验证后已回退不入库；截图存档 `screenshots/m5-05/`）：
工具栏 Running→Idle 徽标、节点 0.8/0.6 ms 徽标随帧推进、总览 30.3 fps/
processed 增长、选中节点 last 1.7 ms · avg 1.6 ms · 129 frames、stop 后冻结
末次值 + stopped 标注与 [stopped] 事件行，与 M5-04 参数编辑/缩略图共存无
冲突，全程无崩溃。环境：x86_64 Linux（GNOME/XWayland），GCC 13。面板交互与
视觉验收按既定纪律归 `M5-07` 真机端到端。`M5-06`（工作流运行控制）为下一
工作项。

2026-09-29：`M5-06` 完成关闭——工作流运行控制 + 假换真集成落地（§5.8 +
DEC-016 交错策略终点）。**假换真**：viewer 弃用 `M5-08` 契约假引擎，改接
`M4-07` 真引擎 `createWorkflowEngine`；新增 `apps/viewer/workflow_frame_source.hpp`
相机帧源接缝（engine.hpp `WorkflowFrameSource` 的应用层实现）——`ICameraService`
RGB 彩色流经"上次已见序号"过滤非阻塞读取（引擎帧泵 tick 为唯一调用方），
RGBA8 共享缓冲 `ImageU8::wrap` 零拷贝接管（预览与工作流共享同一缓冲），
无效帧/服务失效按"无新帧"防御；多源图各 source 节点独立水位（引擎按节点
分发）；服务以 shared_ptr 值捕获——关闭顺序中服务先停、引擎后回收的窗口内
残余 tick 取帧恒为"无新帧"，闭包不悬垂（EXEC-04 关闭顺序保持不变）。
深度伪彩通道不进工作流（如需深度输入按决策扩展目录，不在接缝内隐式分流）。
**运行控制（§5.8）**：工具栏新增 Start/Stop（Fit 左侧）——启动可用条件 =
当前图校验通过（§4 唯一判据）且引擎 Idle/Failed（Failed 恢复路径 = stop 回
Idle 后重启，点击序内完成；契约"Failed 为运行终态，stop 后可重启"）；停止
可用条件 = Running/Failed（幂等，owner 线程同步有界排空 ≤ maxInFlight 帧
任务，与 onShutdown 同纪律）；Stopping 全禁用，无暂停（契约无暂停语义）；
启动准入失败以 `AdmissionResult.error` 写反馈行显式呈现。**待生效标注（§4）**：
`WorkflowCanvasState.graphPending`——Running 下图结构变更经 `applyGraph` 校验
入队后置位，pump 消费 `GraphApplied` 事件或引擎离开 Running（stop 排空待生效
队列）清除；帧边界拒绝（Info 事件）时如实保持（画布图与生效图确实不一致）；
工具栏 warning 色 "pending - applies next frame"。**假引擎移除**：`rin_workflow_fake`
目标与 `fake_engine.{hpp,cpp}`、`test_workflow_fake_engine` 按既定计划随本次
集成删除（DEC-016 使命终结；假引擎 peek 缺陷连带消亡）；契约套件
`workflow_engine_contract_suite.hpp` 保留，真引擎独跑（`M4-09` 契约面不变）。
测试（Independent-Verification-Agent 独立编写执行）：适配 `test_workflow_engine`
（目录一致性改对 `makeDefaultImageNodeCatalog` 单一事实源逐字段）、
`test_param_panel`/`test_perf_panel`（fixture 换真引擎 + 合成帧源；crop 节点
显式携带合法 ROI——真实算子越界 ROI 运行期拒绝；perf 耗时断言从仿真常数改
实测区间 + 源节点恒 0（M4-07 源节点记账语义）；失败注入改 `requestParamUpdate`
热更越界 ROI 下一帧生效路径）；新增 `tests/test_run_control.cpp` 4 分区 126 项
检查——帧源接缝（最新帧水位/零拷贝指针相等/双源独立水位/无效帧防御）、运行
控制状态机（Idle 同步生效、Running 待生效置位/清除、校验拒绝不置位、stop
收敛）、端到端（假相机 → 接缝 → 真引擎 → NodeOutputCache/pumpNodeOutput/
参数热更新下一帧生效/停止排空）、**onShutdown 同构全序回归**（服务 stop →
引擎 stop+reset → 面板排空 → `shutdown(true)` Completed）与关停竞态防御。
1605 项检查 debug/asan/ubsan/tsan 四预设 26/26 全绿（0 消毒器诊断、0 tsan
警告；测试数量对照 master 仅减 fake_engine 仅增 run_control）。真机冒烟
（D435if 直连，临时本地补丁预置 source→downscale→grayify 三节点链 + 脚本化
启动/停止，验证后已回退不入库；截图存档 `screenshots/m5-06/`）：start 准入
后 Running，真相机 30fps 进真引擎（10s 处理 283–285 帧、零丢弃、29.8–30.0 fps），
灰度化节点实测 last 0.2 ms · avg 0.2 ms · 161→284 frames，缩略图 424x240 随
sourceSequence 推进，stop 收敛 Idle、缩略图排空空态、统计冻结末次值、
[stopped] 事件行，与 §4 排空语义逐项一致；全程无崩溃（工具栏交互与视觉细节
归 `M5-07` 真机端到端）。环境：x86_64 Linux（GNOME/XWayland），GCC 13。
`M5-07`（M5 测试矩阵、真机验收与文档回写）为下一工作项。

2026-09-29：`M5-07` 完成关闭、M5 里程碑收口。**全预设测试矩阵**
（Independent-Verification-Agent 独立执行）：debug/asan/ubsan/tsan 四预设
增量构建零错误零新增警告，ctest 全量 26/26 全绿（debug 28.8s / asan 35.6s /
ubsan 32.9s / tsan 按 `setarch -R` 既有纪律 90.3s），asan/ubsan/tsan 消毒器
诊断为 0；`realsense_hardware` 四预设均为真机实跑非 skip（D435if
261922074392 / 固件 5.15.1.55，debug verbose 222 checks 0 failures——含
RGB/深度帧内容校验、IMU 通道、分辨率切换与干净停止），M3-08 tsan×真机既有
抖动本轮未触发（四预设一次全绿，已知限制无需援引）。**真机端到端验收**
（D435if 直连，GNOME/XWayland 3200x2000@2x，应用逻辑视口 960x600）：脚本化
交互时序驱动完整闭环——调色板拖出创建三节点（相机源 RGB → 灰度化 →
FFT 低通，创建即选中）、端口 press/drag/release 经画布交互状态机建立两条
连线（Rgba8/Gray8 类型色，悬空中间态标注 → valid）、Start 准入 Running 后
真相机 30 fps 进真引擎（processed 46→97→140、end-to-end 29.97–30.15 fps、
启动瞬态丢弃 1 帧显式计数呈现、in flight ≤2 有界）、选中节点参数面板
（截止 0.2）+ 848x480 实时缩略图随 sourceSequence 推进（seq 220→282）、
性能面板总览（30.0 fps / processed / dropped / in flight + dropped 非零
warning 行）与节点耗时徽标（0.0 / 0.9 / 34.7 ms）、stop 排空回 Idle
（缩略图清空、统计冻结末次值 + stopped 标注、[started]/[stopped] 事件行）。
注入方式：OS 级输入自动化在本环境不可用（实证 mutter Wayland 会话不向 X
投递 XTEST 合成事件、GLFW 过滤 XSendEvent 合成输入——hover/焦点/按键均不
达应用），按既有纪律以临时本地补丁在工作流页交互接缝注入指针事件序列
（调色板拖出三回调、画布 press/drag/release 状态机、运行控制回调，与真实
输入驱动的处理路径相同），compose/渲染输出为真实路径；验证后补丁已回退
不入库（洁净树四预设重建测试全绿，见上）。桌面端交互（拖拽/下拉/docking/
快捷键）此前已经用户真机实操使用并经 M6/M7 反馈修复循环验证。截图存档
`screenshots/m5-07/`（运行态全图 / 选中节点产物与参数 / 性能总览 / 停止
冻结）。**文档回写**：`ui_workspace_design.md` §2 拓扑树逐项与实现对应
核对并修正预览页配色入口滞后描述（DEC-014 决策 3 已落地设置页）、§3 快捷键
清单补 `Esc`（M5-03 落地）、§8 P0 验收补记；CHANGELOG 补 `M5-02`..`M5-05`
工作台条目；总计划 `SCOPE-09` 关闭；M7 验证记录引用的 `screenshots/m7/`
截图补入库。环境：x86_64 Linux（GNOME/XWayland），GCC 13。M5 全部工作项
闭合，工作台 UI 里程碑完成；v0.5.0 发布点待用户发布。
