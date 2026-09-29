# Rin 实施总计划

> 状态：In Progress
> 最后更新：2026-09-29
> 负责人：Linductor-alkaid

## 当前整体状态

M1（viewer 基础能力）、M2（更名与 Linux 自包含分发）与 M3（IMU 位姿视图）均已
完成并通过验收（v0.1.0、v0.2.0 已发布；v0.3.0 为 M3 发布点，deb 自该版起按
Ubuntu 20.04 容器构建、focal 可用；见 [m2-linux-packaging.md](m2-linux-packaging.md)、
[m3-imu-pose-view.md](m3-imu-pose-view.md) 验证记录）。2026-09-24 立项 M3-M5
（IMU 位姿视图、图像处理节点工作流、工作台 UI），由自治开发工作流按工程规范逐项
推进；关键实现策略先经调研决策（DEC-010..015）冻结后实施。2026-09-24 经
[DEC-016](../decisions/DEC-016-contract-first-workbench-order.md) 冻结 M4/M5
交错实施策略：工作流视图契约先行（`M4-09`），M5 骨架对契约假引擎先行开发
（`M5-08`），M4 真引擎按同一契约实现后在 `M5-06` 集成替换；M5 完成点仍在 M4 之后。
2026-09-24 `M5-01` 完成：工作台 UI 调研落盘（[ui_workspace_research.md](../design/ui_workspace_research.md)，
含用户指定的 RPA/影刀参考），[DEC-014](../decisions/DEC-014-workbench-information-architecture.md) 与
[DEC-015](../decisions/DEC-015-node-editor-implementation-path.md) 冻结，
[ui_workspace_design.md](../design/ui_workspace_design.md) 产出。2026-09-24 `M5-08`
契约假引擎完成（`rin::workflow_fake` 库，`IWorkflowEngine` 仿真实现：Executor 周期
tick + 有限任务有界在飞 + 显式丢弃 + 图/参数帧边界生效 + 节点失败/取消/关闭路径；
与 M4-07 真引擎共用的契约测试套件入库，160 项检查 debug/asan/ubsan/tsan 四预设
0 失败）；M5 骨架进入 `M5-02` 导航壳与页面框架。2026-09-28 `M5-02` 完成：工作台
壳落地（DEC-014 四页导航 + 工作流页五区骨架 + 设置页收纳深度配色/关于；导航控件
按 [EUI-20260928-001](../dependency_feedback/eui-neo/ledger.md) 自绘左窄边栏，
`viewer_theme` 扩工作流状态色与端口类型色令牌；真机四页导航/切页状态保持/配色
切换/关闭路径回归通过，导航状态单测 308 项检查 debug/asan/ubsan 0 失败，全量
ctest 四预设 16/16）；2026-09-28 `M5-03` 完成：节点编辑器画布落地（对假引擎
开发调试——`canvas_model.hpp` 平台无关纯逻辑：视图变换/命中/图同步/交互状态机/
调色板模型，`node_canvas.hpp` 五区组装替代静态骨架，图变更经 `applyGraph`
与引擎同步、拒绝原因显式反馈；画布单测 422 项检查 debug/asan/ubsan 0 失败
[独立验证两轮，第一轮 3 缺陷修复后复验 PASS]，全量 ctest 四预设 17/17，
§5.1 平移/框选冲突在设计文档补记冻结；工作流页交互与视觉验收归 `M5-07`）；
2026-09-28 `M5-04` 完成：参数面板与中间结果查看落地（对假引擎开发调试——
`param_model.hpp` 纯逻辑：控件-参数绑定/RealArray 矩阵网格/产物有界缓存/缩略图
降采样/失败标注，`param_panel.hpp` 组装：类型化参数编辑 + 缩略图 + 节点错误
徽标，编辑经 `requestParamUpdate` 下一帧生效、引擎非 Running 缩略图排空；参数
面板单测 310 项检查 debug/asan/ubsan/tsan 四预设 0 失败 [独立验证两轮]，全量
ctest 四预设 18/18；冒烟发现并修复 M5-03 调色板临时目录悬垂指针潜伏缺陷，
真机复验稳定）；
`M5-05` 性能面板为下一工作项。2026-09-28 `M5-05` 完成：性能面板落地（对假引擎
开发调试——`perf_model.hpp` 纯逻辑统计管道 + 工具栏状态徽标/节点耗时徽标/总览
四行，冻结语义落实 §4；统计管道单测 736 项检查 debug/asan/ubsan/tsan 四预设 0
失败 [独立验证两轮，第一轮 1 缺陷修复后复验转绿]，全量 ctest 四预设 19/19，
真机冒烟通过）；2026-09-28 `M4-01` 完成：DEC-012 冻结（`Accepted`）——基础算子
自研 + pinned kissfft `131.2.0` 承载 FFT 族 + FFT 节点内部 2 幂填充，OpenCV 与
全自研路线经四维取证（性能/构建体积/CI 时长/许可证）否决；基准工具
`tools/fft_bench` 入库（含正确性交叉校验），kissfft 台账登记 master 溢出预检
误报缺陷（KIS-20260928-001），lock/Dependencies.cmake/台账索引同步。
2026-09-28 `M4-02` 完成：Core 图像与节点契约落地（`ImageU8` 共享不可变像素图像
+ 16 MiB 容量上界、`IImageNode` 同步节点契约与类型化参数模型（参数构造期定型，
热更新经帧边界重建）、`NodeGraph` 图编译（`validateWorkflowGraph` 唯一判据 +
节点数准入 + 稳定拓扑序）与 `runNodeGraph` 单帧求值（注入型源节点 + 防御性
输出核对）；设计文档 [image_workflow_design.md](../design/image_workflow_design.md)
产出（M4 节点目录表与逐算子 golden 测试项，DEC-012 FFT 2 幂填充约束 golden 化）；
契约单测 image_contracts 133 项 + node_graph 148 项 + public_boundary 30 项
debug/asan/ubsan 0 失败 [独立验证两轮，第一轮暴露 buildNodeGraph 拓扑组装缺陷
修复后复验 PASS]，全量 ctest 三预设 21/21）。2026-09-29 `M4-03` 完成：几何算子
节点落地（`makeDefaultImageNode` 默认工厂 + crop/downscale 实现，§7 冻结数值
语义：crop ROI 退化/越界 apply 期显式拒绝、输出紧凑新缓冲；downscale 输出尺寸
floor 公式、nearest 面积覆盖采样与 bilinear 中心对齐插值 round-half-up、scale=1
恒等；golden 几何单测 165 项 + public_boundary 扩展 33 项 debug/asan/ubsan
0 失败 [独立验证一轮 PASS]，全量 ctest 三预设 22/22）。2026-09-29 `M4-04`
完成：卷积/高斯算子节点落地（`conv_kernel`/`gaussian_blur`，§7 冻结数值语义：
相关语义核不翻转、clamp/reflect-101/zero 边界可配、可分离高斯与直接卷积数学
恒等；目录 schema 为 conv_kernel 增加 border 参数；golden 单测 171 项 +
public_boundary 扩展 37 项 debug/asan/ubsan 0 失败 [独立验证两轮]，全量 ctest
三预设 23/23）。2026-09-29 `M4-05` 完成：直方图均衡与灰度化节点落地
（`hist_eq` Gray8→Gray8 + `grayify` Rgba8→Gray8 BT.601 定点亮度桥接，§7 冻结
数值语义；golden 单测 79 项 + 见证适配 debug/asan/ubsan 0 失败 [独立验证一轮
PASS]，全量 ctest 三预设 24/24）。2026-09-29 `M4-06` 完成：FFT 滤波节点族
落地（`fft_lowpass`/`fft_highpass`/`fft_bandpass`，理想锐截止 + 2 幂填充 +
归一化频率掩膜，§7 冻结数值语义；kissfft 私有链接；FFT 单测 174 项 + 见证
适配 debug/asan/ubsan 0 失败 [独立验证三轮]，全量 ctest 三预设 25/25）。
2026-09-29 `M4-07` 完成：工作流真引擎落地（[DEC-013](../decisions/DEC-013-workflow-execution-model.md)
冻结——帧泵采样 + 最新帧快照 + 有界在飞显式丢弃；`rin_workflow` 库按 `M4-09`
契约实现 `IWorkflowEngine`，经 `buildNodeGraph`/`runNodeGraph` 求值真实算子，
`runNodeGraph` 加性扩展逐节点观测接缝（耗时/失败归因）；消费式图替换（同一性
判定，不复刻假引擎 peek 缺陷）、发布按提交序有序化、参数受理期预编译校验；
M4 目录构建提取为两引擎共享单一事实源；引擎测试 431 项（契约套件 90 + 特有
341）debug/asan/ubsan/tsan 四预设 0 失败 [独立验证两轮，首轮 1 实现缺陷
（源节点 executedFrames）修复后复验闭合]，全量 ctest 四预设 26/26）。
2026-09-29 `M4-08` 完成、M4 里程碑关闭：M4 测试矩阵独立验证四预设 26/26 全绿
（真机冒烟 PASS、零消毒器诊断、无新增警告）；848×480 典型工作流真引擎吞吐
实测落盘（[workflow-throughput-848x480.md](../benchmarks/workflow-throughput-848x480.md)，
`tools/workflow_bench` 工具随项目构建——典型 FFT 链 30 fps 相机速率零丢弃、
余量约 3.4 倍，FFT 低通为链主导成本；降分辨率前置把 FFT 成本降至约 1/4；
复验修订 3 处文档失实注记后闭合，只记录实测值与方法）。`M4-09` 视图契约完成
判据随 `M4-07` 合入全部满足（两引擎共用契约测试），工作项一并闭合。
2026-09-29 `M5-06` 完成、假换真集成落地：viewer 弃用契约假引擎改接 `M4-07`
真引擎，新增 `apps/viewer/workflow_frame_source.hpp` 相机帧源接缝（RGB 彩色流
非阻塞读取 + RGBA8 零拷贝 wrap，服务 shared_ptr 捕获闭合关闭窗口生命周期）；
工具栏 Start/Stop 运行控制（§5.8：校验通过 + Idle/Failed 可启动、Failed 停止
后重启、Running/Failed 可停止、准入错误显式反馈）与 Running 图变更"待生效"
标注（`GraphApplied` 后清除）；`rin_workflow_fake` 假引擎按既定计划随集成删除
（契约套件由真引擎独跑，`M4-09` 契约面不变）。run_control 集成测试 126 项 +
适配套件共 1605 项检查 debug/asan/ubsan/tsan 四预设 26/26 全绿 [独立验证]，
含 onShutdown 关闭顺序同构回归；真机冒烟（D435if）真相机 30fps 进真引擎零
丢弃、stop 排空语义与 §4 逐项一致（截图 `screenshots/m5-06/`）。
`M5-07`（M5 测试矩阵、真机验收与文档回写）为下一工作项。2026-09-29 用户
真机验收反馈五项立项 [M6](m6-workflow-input-expansion.md)（POST-04 触发）：
工作流输入扩展（[DEC-017](../decisions/DEC-017-workflow-camera-sources.md)
冻结）——相机源四型（RGB + 深度伪彩/灰度/自适应灰度固定 rendition，与预览
配色解耦）、灰度域裁切/降分辨率（crop_gray/downscale_gray，复用泛格式实现）、
裁切 ROI 控件联动夹取防呆（apply 期拒绝语义不变）、相机源面板分辨率入口
（与预览共享全局命令）；相机契约加性扩展（FrameKind::DepthJet + GrayFrame/
tryLoadGrayFrame）。2026-09-29 用户再反馈三项立项
[M7](m7-workbench-usability.md)：运行中改图产物陈旧（引擎产物邮箱改同 id
跨代共享 + 过代帧跳过发布）、分辨率下拉悬垂指针（绑定移入 ViewerContext）、
调色板滚动与五区 docking 可调（scrollView 组件 + mouseArea 分隔条）。
2026-09-29 `M5-07` 完成关闭、M5 里程碑收口：全预设测试矩阵 debug/asan/
ubsan/tsan 26/26 全绿（真机硬件四预设实跑，零消毒器诊断）；D435if 真机
端到端验收通过（拖拽搭图→连线→运行→中间结果→性能面板→停止排空，交互
接缝注入时序，截图 `screenshots/m5-07/`）；`ui_workspace_design.md` 拓扑
树逐项核对、CHANGELOG 回写、`SCOPE-09` 关闭。M4/M5/M6/M7 全部交付，
v0.5.0 发布点待用户发布。

## 交付边界（SCOPE）

- [x] `SCOPE-01` 仓库骨架与工程纪律：AGENTS.md、工程规范、CMake ≥ 3.25 + CMakePresets、
      依赖锁清单与 configure 时 commit 校验。
- [x] `SCOPE-02` 核心库 `rin_core`：相机服务契约（`ICameraService`）、帧/内参公共类型、
      显式状态机（`Idle/Opening/Streaming/Restreaming/Stopping/Failed`）、executor 承载的
      采集生命周期与 `executor::comm` 帧传递。
- [x] `SCOPE-03` librealsense 适配器 `rin_realsense_adapter`：设备枚举/打开、双流
      （RGB + 深度）配置、分辨率切换（pipeline 重建）、RGB→RGBA 转换、深度 Z16→RGBA8
      伪彩、内参读取；全部阻塞采集在 Executor blocking worker 中执行。
- [x] `SCOPE-04` EUI-NEO viewer 应用 `rin`：RGB 与深度实时画面、分辨率下拉选择、
      相机内参面板；Executor 生命周期 owner 为应用 `AppRuntime`，经
      `DslAppConfig::onShutdown` 闭合关闭顺序。
- [x] `SCOPE-05` 测试与验证：状态机/转换/边界单测（假设备），转换函数单测，ASAN/UBSAN
      预设，真机（D435if）人工验收记录。
- [x] `SCOPE-06` 项目标识统一更名 Rin，Linux 自包含 deb 分发（捆绑 librealsense2 +
      udev 规则）与 CI 工件导出（[DEC-008](../decisions/DEC-008-project-rename-to-rin.md)、
      [DEC-009](../decisions/DEC-009-self-contained-deb-distribution.md)）。
- [x] `SCOPE-07` IMU 姿态通路与 3D 位姿视图：ACCEL/GYRO 混合流采集、六轴姿态
      融合（Core 纯逻辑）、固定世界坐标系下相机位姿 3D 实时视图（视锥 + 坐标轴）
      与 IMU 状态面板（M3，[DEC-010](../decisions/)、[DEC-011](../decisions/)；
      2026-09-24 随 v0.3.0 收尾，见 [m3-imu-pose-view.md](m3-imu-pose-view.md)）。
- [x] `SCOPE-08` 图像处理节点库与工作流引擎：裁切、降分辨率、自定义卷积核、
      高斯模糊、直方图均衡、FFT 高通/低通/带通节点，DAG 工作流引擎（拓扑执行、
      逐节点中间产物、每节点耗时与端到端 FPS 统计，执行经 Executor）（M4，
      [DEC-012](../decisions/)、[DEC-013](../decisions/)；2026-09-29 随
      `M4-08` 关闭，见 [m4-cv-node-workflow.md](m4-cv-node-workflow.md)）。
- [x] `SCOPE-09` 工作台 UI：页面导航（预览 / 位姿 / 图像工作流 / 设置）、拖拽式
      节点编辑器（调色板、连线、参数面板、中间结果查看）、性能面板与运行控制，
      界面信息架构与操作逻辑拓扑树设计文档（M5，
      [DEC-014](../decisions/)、[DEC-015](../decisions/)、
      [DEC-016](../decisions/DEC-016-contract-first-workbench-order.md)；
      按 DEC-016 骨架对契约假引擎先行，完成后与 M4 真引擎集成；
      2026-09-29 随 `M5-07` 收口关闭，见
      [m5-ui-workbench.md](m5-ui-workbench.md) 验证记录；M6 输入扩展与
      M7 可用性修复已先行合入 master）。

## 不可破坏的架构约束（RULE）

- `RULE-01` 公开头文件（`include/rin/`）不得包含 librealsense2、EUI-NEO 或其他
  第三方类型；Adapter 依赖 Core 接口，Core 不依赖 Adapter。
- `RULE-02` 自研代码不得创建 `std::thread`/`std::jthread`/`std::async`/自建线程池；所有
  异步与阻塞 I/O 经 Executor 公开能力（AGENTS.md 强制条款）。
- `RULE-03` 跨上下文帧/命令/事件传递使用 `executor::comm` 原语，容量有界；不得用
  mutex+条件变量自建队列。
- `RULE-04` 第三方依赖全部 pin 到精确 commit 并登记 `third_party/dependencies.lock`，
  configure 时校验；不得修改 pinned 依赖源码。
- `RULE-05` EUI-NEO 的 UI/渲染线程上只做有界校验、取帧与提交，不执行阻塞等待；业务
  采集与处理不在该线程运行。
- `RULE-06` 所有依赖分别维护独立反馈台账（executor：`docs/executor_feedback/ledger.md`；
  其他：`docs/dependency_feedback/<dep>/ledger.md`），缺口必须登记后才能实施绕行。
- `RULE-07` CPU 密集处理（IMU 融合、图像工作流节点执行）不得在 EUI 渲染线程运行；
  其并发承载必须经 Executor 公开能力并可观察（`EXEC-06`/`EXEC-07`），统计展示只
  消费 Executor/comm 设施，不建平行监控。

## Executor 并发边界（EXEC）

- `EXEC-01` 采集主循环：`Executor::start_worker(BlockingWorkerSpec)` 专属 blocking worker
  承载 librealsense `wait_for_frames()`；句柄由 `RealSenseCamera` 持有；
  `worker.request_stop()` + `StopToken` 协作取消；`worker.stop()` 回收。
- `EXEC-02` 帧传递：采集 worker → UI 经 `executor::comm::LatestMailbox<FrameBundle>`
  （有界、最新态语义，与渲染端消费匹配）；UI → 采集 worker 的分辨率切换命令经
  `LatestMailbox<ControlCommand>`，由 worker 在循环边界消费并 `wakeup()` 解除阻塞等待。
- `EXEC-03` 事件与错误：worker 内失败以结构化 `ServiceEvent` 发布到
  `LatestMailbox<ServiceEvent>`，并驱动状态机进入 `Failed`；不抛异常跨越 executor 边界。
- `EXEC-04` 生命周期：owner 为 viewer 的 `AppRuntime`（主线程）；启动顺序 = initialize
  executor → 创建服务 → start worker；关闭顺序 = 停止命令生产者 → `request_stop` →
  worker stop 回收 → `Executor::shutdown(true)`，全部在主线程 `onShutdown` 回调完成。
- `EXEC-05` 无独立周期任务需求（当前）；若后续引入统计/心跳，使用 `submit_periodic`
  并登记条目。
- `EXEC-06` IMU 采样处理：ACCEL/GYRO 帧在采集 blocking worker 循环内分支处理
  （有界校验 + 融合推进 + `LatestMailbox` 最新态投递，无堆分配热路径）；
  `ImuFuser` 为 Core 纯逻辑，在采集 worker 内按样本推进；不新增线程。若
  `DEC-010` 要求独立融合节拍，改用 Executor timer/realtime 能力并更新本条；
  句柄归属与取消路径沿用 `EXEC-01` 模式。
- `EXEC-07` CV 工作流执行：帧驱动执行以 Executor 有限任务承载（`submit_auto` +
  有界在飞 + 显式丢弃策略与统计暴露）；节点为同步 CPU 工作单元；结果经
  `LatestMailbox`/`DoubleBuffer` 回 UI；不新增线程设施；健康度经 comm 统计与
  Executor 监控设施观察。

## 里程碑索引

| 里程碑 | 文件 | 依赖 | 建议发布点 |
| --- | --- | --- | --- |
| M1 viewer 基础能力 | [m1-viewer-foundation.md](m1-viewer-foundation.md) | 无 | v0.1.0 |
| M2 更名与 Linux 自包含分发 | [m2-linux-packaging.md](m2-linux-packaging.md) | M1 | v0.2.0 |
| M3 IMU 位姿通路与 3D 视图 | [m3-imu-pose-view.md](m3-imu-pose-view.md) | M1 | v0.3.0 |
| M4 图像处理节点与工作流引擎 | [m4-cv-node-workflow.md](m4-cv-node-workflow.md) | 无（建议 M3 后） | v0.4.0 |
| M5 工作台 UI（导航 / 节点编辑器 / 性能面板） | [m5-ui-workbench.md](m5-ui-workbench.md) | M3、M4-09（骨架先行，DEC-016）；完成依赖 M4 | v0.5.0 |
| M6 工作流输入扩展（深度源 / 灰度算子 / 控件防呆） | [m6-workflow-input-expansion.md](m6-workflow-input-expansion.md) | M4、M5-06 | v0.5.0（随 M5） |
| M7 工作台可用性（改图产物刷新 / 分辨率入口 / 滚动与可调 docking） | [m7-workbench-usability.md](m7-workbench-usability.md) | M6 | v0.5.0（随 M5） |

## 暂定决策（未冻结）

- `DEC-001`（已记录，librealsense2 部分被 DEC-009 取代）依赖锁定方式：lock 清单 +
  CMake 拉取；executor/eui-neo 源码引入（external 类）。
- `DEC-002`（已记录）UI 运行时集成：使用 EUI-NEO `eui_neo_configure_app` 提供的 main，
  `AppRuntime` 在 `dslAppConfig()` 首调点惰性初始化并在 `onShutdown` 关闭。
- `DEC-003`（已记录）深度伪彩使用自研 jet 映射而非 `rs2::colorizer`，保证 Core 边界
  纯净与可测试性。
- `DEC-004`（已记录）默认流配置 848x480@30 RGB+Z16。
- `DEC-005`（已记录）viewer 视觉层采用 ZCode Design System 的令牌翻译
  （`apps/viewer/viewer_theme.hpp` 为唯一令牌层）。
- `DEC-006`（已记录）热插拔与多设备选择：启动不依赖相机连接（`Waiting` 设计稳态），
  长驻 `rs2::context` 变化回调 + 在线设备目录，设备选择意图粘性。
- `DEC-007`（已记录）深度图配色运行时可选（jet / 灰度黑白）：命令通道仅切换后续
  帧转换 ramp，不重流；灰度极性暂定近白远黑。
- `DEC-008`（已记录）项目标识统一更名 Rin：命名空间/目标/选项/包名/文档；设备与
  SDK 相关标识保留 "realsense"。
- `DEC-009`（已记录）自包含 deb 分发：librealsense2 改 external 源码构建并随包
  捆绑（私有库目录 + rpath + udev 规则 + 桌面入口），依赖声明由 shlibdeps 生成；
  CI 导出 deb 工件。
- `DEC-010`（已记录）IMU 姿态融合算法：Mahony 显式互补滤波（四元数、PI 反馈 +
  陀螺零偏在线估计、重力参考 1g 门限）；六轴 yaw 漂移为物理限制，如实披露。
- `DEC-011`（已记录）3D 位姿视图渲染路径：CPU 投影 + EUI-NEO `polygon` 原语，
  投影数学为 Core 纯逻辑；原型实测投影 ~40 µs/帧（32 线段）、运行时 55 FPS
  连续重绘，姿态驱动旋转对照截图成立。
- `DEC-012`（已记录，2026-09-28 经 `M4-01` 基准冻结）图像算子实现策略：基础算子
  （裁切/降分辨率/卷积核/高斯模糊/直方图均衡）自研 + pinned kissfft `131.2.0`
  （BSD-3-Clause）承载 FFT 族，FFT 节点内部零填充到 2 幂尺寸（归一化频率参数
  语义不变）；OpenCV external pin 与全自研 FFT 经基准否决（性能相当/更差而
  构建体积与 CI 代价高数量级/实现负担重）；证据与复现见 `tools/fft_bench` 与
  [DEC-012](../decisions/DEC-012-image-operator-strategy.md)。
- `DEC-013`（已记录，2026-09-29 经 `M4-07` 实现冻结）工作流执行模型：
  帧泵采样 + 最新帧快照 + 有界在飞显式丢弃——帧泵为 Executor 允许抖动周期
  tick（引擎无自有线程），帧输入经 `WorkflowFrameSource` 接缝捕获快照
  （探测即消费），无新帧不提交、满载显式丢弃计数；逐节点耗时/失败归因经
  `runNodeGraph` 观测接缝实测；相机 worker 推送驱动、任务期拉帧、容量 1
  DropOldest 图通道与假引擎 peek 模式经论证否决；证据与语义见
  [DEC-013](../decisions/DEC-013-workflow-execution-model.md)。
- `DEC-014`（已记录）工作台信息架构：单窗口 + 左侧导航四页（预览 / 位姿 /
  图像工作流 / 设置）；工作流页五区骨架（工具栏 / 调色板 / 画布 / 右上下文
  面板 / 底校验与事件列表）；参数热更新"下一帧生效"明示，不引入 deploy
  式编辑态分离；画布布局为 UI 私有状态。
- `DEC-015`（已记录）节点编辑器实现路径：EUI-NEO 原语自研画布
  （`rect`+`mousearea`+`polygon`+`ui.state`），交互几何下沉为平台无关纯
  逻辑；P0/P1/P2 交互分期以 M4-09 契约为界；retained layer polygon 缺陷
  按 EUI-20260924-001 先例规避。
- `DEC-016`（已记录）M4/M5 交错实施策略：工作流视图契约先冻结（`M4-09`），
  M5 骨架对契约假引擎先行开发（`M5-08`），M4 真引擎按同一契约实现并在 `M5-06`
  假换真集成；假引擎必须复刻 `EXEC-07` 的 Executor/comm 语义并覆盖失败路径，
  性能结论以真引擎实测为准。
- `DEC-017`（已记录，2026-09-29 随 M6 立项冻结）工作流相机源四型与深度
  rendition 语义：`source`/`source_depth_jet`（Rgba8）/`source_depth_gray`/
  `source_depth_adaptive`（Gray8）独立 typeId（静态端口类型可判）；深度
  rendition 与预览配色解耦（DEC-007 命令只影响预览通道）；分辨率是相机流
  全局属性（工作流源面板与预览共享同一命令，不做 per-source）；ROI 防呆
  分层（控件夹取 + M4-03 apply 期拒绝兜底）。

## 通用完成定义（DOD）

- `DOD-01` 实现位于正确层并通过公开头第三方类型边界检查（编译测试）。
- `DOD-02` 新并发路径具备：正常完成、异常、取消、shutdown 测试。
- `DOD-03` debug/asan/ubsan 预设构建并测试通过（涉及跨上下文状态时加跑 tsan）。
- `DOD-04` 计划状态、设计、决策、台账与验证记录同步更新。
- `DOD-05` Commit 符合 `<type>(<scope>): <subject>` 规范。

## 延后项与触发条件（POST）

- `POST-01` 点云/对齐/录制能力：出现需要深度对齐彩色或离线回放的需求时立项。
- `POST-02` 多设备支持：出现多相机接入需求时立项。
- `POST-03` Windows/Android 平台矩阵：出现对应部署需求时立项。
- `POST-04` 工作流图持久化（保存/加载 JSON）与节点库扩展（边缘检测、形态学、
  色彩空间、阈值、中值滤波等）：M5 验收后按用户需求立项。2026-09-29 用户
  需求触发首批节点扩展，随 [M6](m6-workflow-input-expansion.md) 交付
  （深度 rendition 源与灰度域算子，DEC-017）；其余节点类型仍按需立项。
- `POST-05` T26x / `RS2_STREAM_POSE` 位姿源与 VIO/SLAM：出现对应硬件或算法需求
  时立项；M3 的 `ImuFuser` 边界应不阻碍替换为外部位姿源。

## 建议拆分顺序

先骨架后功能、先契约后实现：Core 类型与接口 → 假设备状态机测试 → librealsense 适配器
→ viewer 应用 → 真机验收。

M4/M5 按 [DEC-016](../decisions/DEC-016-contract-first-workbench-order.md) 交错推进：
视图契约（`M4-09`）→ 契约假引擎（`M5-08`）→ M5 骨架（`M5-02`..`M5-05`，对假引擎
调试）→ M4 算子与真引擎（`M4-03`..`M4-08`）→ 假换真集成与验收（`M5-06`..`M5-07`）。
