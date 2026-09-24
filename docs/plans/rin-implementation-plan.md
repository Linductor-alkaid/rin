# Rin 实施总计划

> 状态：In Progress
> 最后更新：2026-09-24
> 负责人：Linductor-alkaid

## 当前整体状态

M1（viewer 基础能力）、M2（更名与 Linux 自包含分发）与 M3（IMU 位姿视图）均已
完成并通过验收（v0.1.0、v0.2.0 已发布；v0.3.0 为 M3 发布点，deb 自该版起按
Ubuntu 20.04 容器构建、focal 可用；见 [m2-linux-packaging.md](m2-linux-packaging.md)、
[m3-imu-pose-view.md](m3-imu-pose-view.md) 验证记录）。2026-09-24 立项 M3-M5
（IMU 位姿视图、图像处理节点工作流、工作台 UI），由自治开发工作流按工程规范逐项
推进；关键实现策略先经调研决策（DEC-010..015）冻结后实施。

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
- [ ] `SCOPE-08` 图像处理节点库与工作流引擎：裁切、降分辨率、自定义卷积核、
      高斯模糊、直方图均衡、FFT 高通/低通/带通节点，DAG 工作流引擎（拓扑执行、
      逐节点中间产物、每节点耗时与端到端 FPS 统计，执行经 Executor）（M4，
      [DEC-012](../decisions/)、[DEC-013](../decisions/)）。
- [ ] `SCOPE-09` 工作台 UI：页面导航（预览 / 位姿 / 图像工作流 / 设置）、拖拽式
      节点编辑器（调色板、连线、参数面板、中间结果查看）、性能面板与运行控制，
      界面信息架构与操作逻辑拓扑树设计文档（M5，
      [DEC-014](../decisions/)、[DEC-015](../decisions/)）。

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
| M5 工作台 UI（导航 / 节点编辑器 / 性能面板） | [m5-ui-workbench.md](m5-ui-workbench.md) | M3、M4 | v0.5.0 |

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
- `DEC-012`（暂定，M4 经 M4-01 基准冻结）图像算子实现策略：暂定自研算子 +
  小型 FFT 第三方库（备选引入 OpenCV external pin），以构建/许可证/性能基准冻结；
  新依赖按 `RULE-04` 登记。
- `DEC-013`（暂定，M4 经 M4-07 实现冻结）工作流执行模型：暂定"最新帧驱动 +
  有界在飞 + 显式丢弃 + comm 统计暴露"。
- `DEC-014`（暂定，M5 经 M5-01 调研冻结）工作台信息架构：暂定"单窗口 + 页面
  导航（预览 / 位姿 / 图像工作流 / 设置）"，以现代工具调研与设计文档冻结。
- `DEC-015`（暂定，M5 经 M5-01 调研冻结）节点编辑器实现路径：暂定 EUI-NEO
  原语自研画布（`rect`+`mousearea`+`polygon`+`ui.state`），交互几何下沉为平台
  无关纯逻辑。

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
  色彩空间、阈值、中值滤波等）：M5 验收后按用户需求立项。
- `POST-05` T26x / `RS2_STREAM_POSE` 位姿源与 VIO/SLAM：出现对应硬件或算法需求
  时立项；M3 的 `ImuFuser` 边界应不阻碍替换为外部位姿源。

## 建议拆分顺序

先骨架后功能、先契约后实现：Core 类型与接口 → 假设备状态机测试 → librealsense 适配器
→ viewer 应用 → 真机验收。
