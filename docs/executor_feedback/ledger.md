# Executor 反馈台账

> 状态：Active
> 依赖：executor（pinned `4731b16493ae996311a9e85f55bd137aee69418a`，
> https://github.com/Linductor-alkaid/executor ，Apache-2.0）
> 记录纪律：工程规范第 9.4 节；编号 `EXE-YYYYMMDD-NNN`；只写“不支持”不构成有效记录。

## 条目

（暂无条目。集成过程中确认的能力缺口按编号追加于此，并在代码/设计文档中引用。）

## 已核对非缺口事项

| 事项 | 结论 | 依据 |
| --- | --- | --- |
| librealsense `wait_for_frames()` 无法被 StopToken 直接中断 | 非缺口：StopToken 语义本就不覆盖第三方阻塞调用；采用带超时 `wait_for_frames(timeout)` + 循环边界检查取消，属于集成指南记载的正常边界处理 | `third_party/executor` blocking_io.hpp 注释、集成指南；[camera_service_design.md](../design/camera_service_design.md) |
| UI/渲染主循环运行于 GLFW 线程而非 Executor worker | 非缺口：窗口平台线程亲和属平台映射，AGENTS.md 规则 2 允许经外部事件循环边界协调；UI 线程只做邮箱快照消费（规则 11） | AGENTS.md、[DEC-002](../decisions/DEC-002-runtime-ui-integration.md) |
| `WorkerHandle` 无非停止性外部唤醒通道（`wakeup()` 仅绑定 `request_stop`，见 `blocking_io_executor.cpp:97`） | 非缺口：blocking worker 的设计语义即“wakeup 用于停止时解除阻塞”，业务命令的及时拾取应由 worker 自身的有界等待轮询实现（`wait_for_frames(1000ms)` 上界）；分辨率切换延迟 ≤1 个超时周期，满足 M1 需求 | executor 源码核对（2026-09-23）；`realsense_camera_service.cpp` `requestResolution` 注释 |

## 跟进记录表

| 编号 | 状态 | 优先级 | 跟进 |
| --- | --- | --- | --- |
| （空） | | | |
