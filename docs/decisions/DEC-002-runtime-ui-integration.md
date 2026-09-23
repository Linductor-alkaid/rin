# DEC-002：Executor 运行时与 EUI-NEO 主循环的集成方式

> 状态：Accepted
> 日期：2026-09-23
> 负责人：Linductor-alkaid
> 冻结里程碑：M1
> 替代/被替代：无

## 背景与问题

AGENTS.md 要求 Executor 由明确的外部 owner 初始化并在非 worker 线程执行
`shutdown(true)`；而 EUI-NEO 的标准接入（`eui_neo_configure_app`）由框架提供 `main()`
并独占主循环，应用代码只有 `dslAppConfig()` / `compose()` / `onShutdown` 三个入口。
需要确定 owner 挂点，且不得使用 EUI-NEO 自带的 `app::async` 线程池（与"所有异步经
Executor"冲突）。

## 决策

- 不自写窗口主循环，接受框架 main（ copying 框架内部轮询/pacing 代码成本高且随上游
  演进易碎）。
- viewer 内定义 `AppRuntime`：在 `dslAppConfig()` 首次调用（主线程、窗口创建前，
  由框架在 `initialize/render` 中触发）惰性构造 `executor::Executor`、
  `initialize_ex({})`、创建并启动相机服务；该点每进程恰好执行一次且先于任何任务提交。
- 关闭点：`DslAppConfig::onShutdown`（框架文档：主线程、GPU 设备销毁前，允许资源未
  创建）顺序执行：`service->stop()`（request_stop → worker 回收）→
  `executor.shutdown(true)`；全幂等。满足"非 worker 线程 teardown"。
- UI 线程纪律（RULE-05）：`compose()` 只消费 `LatestMailbox` 快照并投递
  `eui::ImageStream`；不调用任何阻塞 API；不使用 `app::async`。

## 备选方案

- 自写 main + EUI-NEO 低层 runtime（`app::initialize`/自管循环）：owner 最直白，但需
  复制框架 pacing/输入/托盘逻辑，维护成本高，未采用。
- executor 在 `main()` 之外的独立初始化（如静态对象）：静态初始化顺序不可控，违反
  owner 明确性，拒绝。

## 影响与风险

- `onShutdown` 也运行于"初始化失败清理"路径：handler 必须容忍 `AppRuntime` 未启动
  （幂等 + 空检查），已列入关闭顺序测试。
- 框架若变更 `dslAppConfig()` 调用时机，初始化点语义需复核；已在 EUI-NEO 台账登记
  观察项。

## 验证方式

单测覆盖 `AppRuntime` 启动/关闭幂等与顺序；真机验收记录窗口关闭后进程干净退出。

## 关联文档和工作项

`M1-05`、[camera_service_design.md](../design/camera_service_design.md)、
[EUI-NEO 台账](../dependency_feedback/eui-neo/ledger.md)
