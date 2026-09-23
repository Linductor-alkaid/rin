# realsense-vision 实施总计划

> 状态：In Progress
> 最后更新：2026-09-23
> 负责人：Linductor-alkaid

## 当前整体状态

M1（viewer 基础能力）已完成并通过真机验收（见
[m1-viewer-foundation.md](m1-viewer-foundation.md) 验证记录）；后续里程碑未立项。

## 交付边界（SCOPE）

- [x] `SCOPE-01` 仓库骨架与工程纪律：AGENTS.md、工程规范、CMake ≥ 3.25 + CMakePresets、
      依赖锁清单与 configure 时 commit 校验。
- [x] `SCOPE-02` 核心库 `rsv_core`：相机服务契约（`ICameraService`）、帧/内参公共类型、
      显式状态机（`Idle/Opening/Streaming/Restreaming/Stopping/Failed`）、executor 承载的
      采集生命周期与 `executor::comm` 帧传递。
- [x] `SCOPE-03` librealsense 适配器 `rsv_realsense_adapter`：设备枚举/打开、双流
      （RGB + 深度）配置、分辨率切换（pipeline 重建）、RGB→RGBA 转换、深度 Z16→RGBA8
      伪彩、内参读取；全部阻塞采集在 Executor blocking worker 中执行。
- [x] `SCOPE-04` EUI-NEO viewer 应用 `rsv_viewer`：RGB 与深度实时画面、分辨率下拉选择、
      相机内参面板；Executor 生命周期 owner 为应用 `AppRuntime`，经
      `DslAppConfig::onShutdown` 闭合关闭顺序。
- [x] `SCOPE-05` 测试与验证：状态机/转换/边界单测（假设备），转换函数单测，ASAN/UBSAN
      预设，真机（D435if）人工验收记录。

## 不可破坏的架构约束（RULE）

- `RULE-01` 公开头文件（`include/rs_vision/`）不得包含 librealsense2、EUI-NEO 或其他
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

## 里程碑索引

| 里程碑 | 文件 | 依赖 | 建议发布点 |
| --- | --- | --- | --- |
| M1 viewer 基础能力 | [m1-viewer-foundation.md](m1-viewer-foundation.md) | 无 | v0.1.0 |

## 暂定决策（未冻结）

- `DEC-001`（已记录）依赖锁定方式：lock 清单 + CMake 拉取；librealsense2 使用系统安装
  2.58.3（system 类），executor/eui-neo 源码引入（external 类）。
- `DEC-002`（已记录）UI 运行时集成：使用 EUI-NEO `eui_neo_configure_app` 提供的 main，
  `AppRuntime` 在 `dslAppConfig()` 首调点惰性初始化并在 `onShutdown` 关闭。
- `DEC-003`（已记录）深度伪彩使用自研 jet 映射而非 `rs2::colorizer`，保证 Core 边界
  纯净与可测试性。
- `DEC-004`（已记录）默认流配置 848x480@30 RGB+Z16。
- `DEC-005`（已记录）viewer 视觉层采用 ZCode Design System 的令牌翻译
  （`apps/viewer/viewer_theme.hpp` 为唯一令牌层）。

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

## 建议拆分顺序

先骨架后功能、先契约后实现：Core 类型与接口 → 假设备状态机测试 → librealsense 适配器
→ viewer 应用 → 真机验收。
