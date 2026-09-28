# M5：工作台 UI——导航、节点编辑器与性能面板

> 状态：In Progress
> 负责人：Linductor-alkaid（授权 Agent 按工程规范自治执行）
> 所属计划：[Rin 实施总计划](rin-implementation-plan.md)
> 前置：M3、`M4-09` 视图契约（骨架与假引擎先行，见
> [DEC-016](../decisions/DEC-016-contract-first-workbench-order.md)）；完成依赖
> M4 真引擎集成
> 建议发布点：v0.5.0
> 更新日期：2026-09-28

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
