# kairo 反馈台账

> 状态：Active
> 依赖：kairo（pinned `d9602ea6762806be320b9543e3b36f27f7dd5b1a`，v0.6.0，
> https://github.com/Linductor-alkaid/kairo ，MIT）
> 记录纪律：工程规范第 9.4 节；编号 `EXE-YYYYMMDD-NNN`；只写"不支持"不构成有效记录。
> 沿革：本台账原承载依赖 executor（Apache-2.0，pin `4731b16`）；2026-10-04 随上游
> 更名 kairo 迁移至 `docs/kairo_feedback/`，编号前缀 `EXE-` 沿用以保持记录连续。

## 条目

（暂无能力缺口条目。集成过程中确认的能力缺口按编号追加于此，并在代码/设计文档中引用。）

## 集成决策记录

### EXE-20261004-001 依赖更名迁移：executor → kairo 0.6.0

- **决策**：跟随上游将依赖 executor 更名迁移为 kairo 0.6.0（项目更名 + 兼容层清理，
  破坏性变更窗口）。锁定身份：`kairo`，pin `d9602ea`（tag v0.6.0），URL
  `https://github.com/Linductor-alkaid/kairo.git`，许可证 Apache-2.0 → **MIT**（上游更名时变更）。
- **依据**：上游 `docs/MIGRATION.md` §"从 0.5.x 升级到 0.6.0"；破坏性提交
  `6fb8f29`（项目更名）、`49a5615`（移除兼容层、Result 变体接管主名）、`6e7657a`
  （`QosClass::HardRealtime`→`Critical`、删除 `AffinityHint::exclusive`，Rin 无使用面）。
- **pin 说明**：pin 取 v0.6.0 tag 提交 `d9602ea` 而非上游移动的 master HEAD（迁移期间
  HEAD 已从 `5673d58` 推进至 `8e3dba5`）；tag 与 HEAD 之间仅网站文案与上游自测去抖动
  差异，库本体无 API/行为差异。
- **Rin 侧适配面**：namespace `executor::`→`kairo::`、include `<executor/…>`→`<kairo/…>`、
  target `executor::executor`→`kairo::kairo`、选项 `EXECUTOR_*`→`KAIRO_*`；
  `initialize_ex()`→`initialize()`（约 12 处，tests；`ExecutorResult` 的 `operator bool`
  为 explicit，`const bool x = initialize(...)` 需显式转换）；`submit_periodic_cancellable_with_handle`
  →`submit_periodic_cancellable`（`src/workflow/engine.cpp` 一处）。
- **行为复查项**（升级吸收 0.5.2/0.5.3 可观察变化）：periodic 取消保留 `Cancelled` 终态、
  重复取消返回 `AlreadyCancelled`；周期定时器网格锚定；shutdown 对 parked 依赖任务的
  `runtime_error` 结算。由全量回归（debug/asan/ubsan）验证。
- **验收**：debug 预设全目标编译通过（115 目标）；全量测试结果见对应 MR 描述。

## 已核对非缺口事项

| 事项 | 结论 | 依据 |
| --- | --- | --- |
| librealsense `wait_for_frames()` 无法被 StopToken 直接中断 | 非缺口：StopToken 语义本就不覆盖第三方阻塞调用；采用带超时 `wait_for_frames(timeout)` + 循环边界检查取消，属于集成指南记载的正常边界处理 | `third_party/kairo` blocking_io.hpp 注释、集成指南；[camera_service_design.md](../design/camera_service_design.md) |
| UI/渲染主循环运行于 GLFW 线程而非 Executor worker | 非缺口：窗口平台线程亲和属平台映射，AGENTS.md 规则 2 允许经外部事件循环边界协调；UI 线程只做邮箱快照消费（规则 11） | AGENTS.md、[DEC-002](../decisions/DEC-002-runtime-ui-integration.md) |
| `WorkerHandle` 无非停止性外部唤醒通道（`wakeup()` 仅绑定 `request_stop`，见 `blocking_io_executor.cpp:97`） | 非缺口：blocking worker 的设计语义即“wakeup 用于停止时解除阻塞”，业务命令的及时拾取应由 worker 自身的有界等待轮询实现（`wait_for_frames(1000ms)` 上界）；分辨率切换延迟 ≤1 个超时周期，满足 M1 需求 | executor 源码核对（2026-09-23）；`realsense_camera_service.cpp` `requestResolution` 注释 |

## 跟进记录表

| 编号 | 状态 | 优先级 | 跟进 |
| --- | --- | --- | --- |
| （空） | | | |
