# 相机服务与 viewer 架构设计

> 状态：Active
> 更新日期：2026-09-23
> 关联：[DEC-001](../decisions/DEC-001-dependency-pinning.md)、
> [DEC-002](../decisions/DEC-002-runtime-ui-integration.md)、
> [DEC-003](../decisions/DEC-003-depth-colormap.md)、
> [DEC-006](../decisions/DEC-006-hotplug-device-selection.md)、
> [DEC-007](../decisions/DEC-007-depth-palette.md)

## 分层

```
apps/viewer (rin, EUI-NEO 前端)
        │  依赖
        ▼
include/rin (公开契约：ICameraService + POD 类型，零第三方类型)
        ▲
        │ 实现
src/adapters/realsense (rin_realsense_adapter, librealsense2 + executor)
src/core (rin_core, 状态机 + 公共类型实现, executor::comm)
```

依赖方向：viewer → 契约 + 适配器工厂；adapter → core 契约。core 与契约不反向依赖任何
adapter，也不包含 librealsense2/EUI-NEO 头（由编译测试锁定，见 `tests/test_public_boundary.cpp`）。

## 公共契约（include/rin/）

- `CameraServiceState`：`Idle / Opening / Streaming / Restreaming / Waiting / Stopping /
  Failed`（`Waiting` 为热插拔稳态，见 DEC-006）。
- `StreamRequest`：彩色+深度的 width/height/fps（成对请求，M1 不支持单流）。
- `DepthColorScheme`：深度图输出配色 `Jet / Grayscale`（DEC-007；灰度近白远黑，
  无效深度 0 输出不透明黑）。
- `Frame`：`kind(Rgb|Depth)`、`width/height/stride`、`sequence`、`deviceTimestampMs`、
  像素缓冲（`std::shared_ptr<const std::vector<std::uint8_t>>`，RGBA8 打包）。
- `IntrinsicsSnapshot`：彩色/深度各自的 `width/height/fx/fy/cx/cy` 与畸变系数/模型。
- `StreamCapabilities`：设备名/序列号/固件 + 彩色/深度支持的分辨率档位（start 准入阶段
  同步枚举后经能力通道发布）。
- `ServiceEvent`：`kind`（Started/ResolutionChanged/Info/Stopped/Failed）、`state`、人读
  消息、时间戳。
- `ICameraService`：`start(StreamRequest)`、`requestResolution(StreamRequest)`、
  `requestDevice(serial)`、`requestDepthColorScheme(DepthColorScheme)`、`stop()`、
  `state()`、`lastError()`，以及四条"上次已见序号"语义的非阻塞读取通道
  `tryLoadFrame / tryLoadIntrinsics / tryLoadCapabilities / tryLoadEvent`。
  接口只使用 std 类型（RULE-01）；`executor::comm::LatestMailbox` 由实现内部持有。
- `createRealSenseCameraService(executor::Executor&)`：工厂（声明在 adapter 头
  `src/adapters/realsense/realsense_camera_service.hpp`，viewer 只经工厂创建；
  executor 类型随该头对 viewer 可见，属运行时契约而非设备 SDK 泄漏）。

## 状态机

```
Idle --start()--> Opening --无设备--> Waiting --设备到达/用户选定--> Opening
                                     \--profile就绪--> Streaming --requestResolution()--> Restreaming
Streaming/Restreaming --活动设备移除--> Waiting（DEC-006 热插拔稳态）
Streaming/Restreaming --流错误(设备在位,有界重试耗尽)--> Failed
Waiting/Streaming/Restreaming --stop()--> Stopping --> Idle
Failed --stop()--> Stopping --> Idle
```

- 所有转换在 `RealSenseCamera` 内单线程推进（worker 线程或 start/stop 调用线程），
  状态写入使用原子，读取方（UI）只消费快照与事件。
- 终态幂等：对 `Idle`/`Failed` 重复 `stop()` 是 no-op；`Failed` 后必须先 `stop()` 才能
  重新 `start()`（重建 pipeline 资源）。
- 迟到帧：`Stopping` 之后到达的帧直接丢弃，不发布到帧邮箱。

## 并发模型（对应总计划 EXEC-NN）

| 工作载荷 | Executor 能力 | 句柄所有者 | 取消路径 |
| --- | --- | --- | --- |
| librealsense 阻塞采集循环 | `start_worker(BlockingWorkerSpec)` | `RealSenseCamera`（`WorkerHandle`） | `request_stop()` → 循环边界退出 → `stop()` 回收 |
| UI→采集命令（分辨率切换 / 设备选择 / 深度配色） | `LatestMailbox<ControlCommand>` + `worker.wakeup()` | `RealSenseCamera` | 关闭时 mailbox 停止投递 |
| 采集→UI 帧 | `LatestMailbox<Frame>` ×2（RGB/深度） | `RealSenseCamera` 提供、viewer 消费 | 关闭后不再发布 |
| 采集→UI 事件/内参 | `LatestMailbox<ServiceEvent>` / `LatestMailbox<IntrinsicsSnapshot>` | 同上 | 同上 |

采集循环骨架（worker 线程）：

```
run(StopToken):
  打开 device/pipeline，按请求 cfg 配置双流，读取内参并发布；state=Streaming
  loop:
    若 stop_token.stop_requested(): 跳出
    若命令邮箱有新 StreamRequest: state=Restreaming；stop/restart pipeline；更新内参
    若命令邮箱有新深度配色（DEC-007）: 仅切换后续帧转换 ramp，不重流
    pipeline.wait_for_frames(timeout)
      成功: RGB8→RGBA8、Z16→按所选配色 RGBA8（纯函数，见 DEC-003/DEC-007），发布帧邮箱
      失败: 发布 ServiceEvent(Failed) 并跳出 → state=Failed
  退出: release 资源；state = (失败? Failed : Idle)
```

`wait_for_frames` 使用带超时版本（默认 1s < 关闭预算），保证 `wakeup()`/停止请求到达后
循环可在该超时内到达取消检查点；这是第三方阻塞调用无法被 StopToken 直接中断时的
边界处理，属于 API 的正常用法而非能力缺口（executor 集成指南：StopToken 不能中断
第三方 read/poll 调用）。

## EUI-NEO viewer 集成（对应 DEC-002）

- 框架提供 main；`AppRuntime` 为唯一 executor owner：
  - 初始化点：`dslAppConfig()` 首次调用（主线程，窗口创建前）——惰性构造
    `executor::Executor` + `initialize_ex({})` + 创建服务并 `start(默认请求)`。
  - 关闭点：`DslAppConfig::onShutdown`（主线程，GPU 设备销毁前）：`service.stop()`
    （内部 request_stop + worker stop 回收）→ `executor.shutdown(true)`；幂等，容忍
    初始化失败路径上资源未创建。
- `compose()`（UI/渲染线程）只做：`try_load_newer_than` 取最新帧 → 上传 `GpuFrameView`
  （自有 GL 纹理 + `importGpuImage` 导入，EUI-20260923-003 绕行；ImageStream 路径在
  当前 GL 栈渲染异常且官方示例可复现）→ 组装控件。不阻塞等待，不在 UI 线程调用
  librealsense。外部纹理非动画元素：有新帧时调用 `app::requestUpdate()` 驱动重绘。
- 设备目录与热插拔（DEC-006）：适配器构造时长驻 `rs2::context` 并注册
  `set_devices_changed_callback`；回调（librealsense 线程）仅向 `LatestMailbox` 投递
  信号（规则 11），worker 以 ≤300ms 有界轮询消费并重新枚举发布 `DeviceCatalog`
  （requested 在线优先 > 唯一设备自动 > 多台未选保持 Waiting）。流中断且活动序列号
  不在总线上 → Waiting；设备仍在则有界重试后 Failed。
- 内参面板：compose 消费 `IntrinsicsSnapshot` 邮箱，渲染文本。

## 失败与可观测

- admission 拒绝/worker 启动失败：`start_worker` 返回 `WorkerHandle::started()==false`
  → `state=Failed` + `ServiceEvent`（携带 executor 报错文本），viewer 显示于面板。
- 帧邮箱为最新态语义：渲染落后时丢弃旧帧（有界、有意识的选择，容量 1~2），不做无界
  积压；发布失败（理论不可达）计为事件。
- 所有状态转换与失败经 `ServiceEvent` 邮箱对 viewer 可见，viewer 顶部显示当前状态。
