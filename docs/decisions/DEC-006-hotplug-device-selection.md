# DEC-006：相机热插拔与多设备选择

> 状态：Accepted
> 日期：2026-09-23
> 负责人：Linductor-alkaid
> 冻结里程碑：M1
> 替代/被替代：无

## 背景与问题

M1 初版要求 `start()` 时相机必须在线，设备拔出即 `Failed`，且只能使用启动时枚举到的
第一台设备。产品要求：启动不依赖相机连接；运行中支持热插拔；多台相机时由用户选择，
唯一相机时自动选择并输出画面。

## 决策

1. **状态机新增 `Waiting` 态**（状态集扩展为
   `Idle/Opening/Streaming/Restreaming/Waiting/Stopping/Failed`）：
   - `start()` 无设备 → `Opening → Waiting`（不是 Failed；等待是设计稳态）；
   - 流中断且活动设备已不在总线上 → `Streaming/Restreaming → Waiting`；
   - `Waiting` 下设备到达或用户选定 → `Waiting → Opening → Streaming`；
   - `Waiting → Stopping → Idle` 保持关闭收敛；设备仍在总线上但打开/流出错仍有界
     重试（3 次）后 `Failed`（真实错误保持可见）。
2. **热插拔监视**：适配器构造时创建长驻 `rs2::context` 并注册
   `set_devices_changed_callback`；回调（librealsense 线程）仅向
   `executor::comm::LatestMailbox` 发布一条轻量信号（AGENTS.md 规则 11），枚举与
   状态推进全部在采集 worker 内完成。worker 以 ≤300ms 的有界轮询消费信号（配合
   `wait_for_frames` 1s 超时检查点），保证取消路径不受影响。
3. **设备目录与选择**：公共契约以 `DeviceCatalog`（全部在线设备的名称/序列号/固件/
   分辨率档位 + `activeSerial` + `activeIsAuto`）发布；`ICameraService::requestDevice
   (serial)` 下发选择命令。自动选择策略在 worker 内执行：用户指定优先；未指定且
   **恰有一台**时自动选中；多台未指定则保持 `Waiting` 并提示用户选择。活动设备丢失
   后剩余唯一设备同样自动选中。
4. **指定设备打开**：`rs2::config::enable_device(serial)` 绑定所选设备；流中断时以
   序列号是否仍在总线上区分"设备移除"（→ Waiting）与"真实流出错"（有界重试后 →
   Failed）。

## 备选方案

- 纯轮询（不注册回调）：实现最简，但插拔感知延迟 = 轮询周期且无法即时响应；回调 +
  有界轮询组合更符合规则 11 的"回调只投递"纪律，未采用纯轮询。
- 拔出即 Failed：与"热插拔自动恢复"的产品目标冲突，未采用。

## 影响与风险

- 状态集变化同步 AGENTS.md、设计文档与测试矩阵；`Waiting` 是稳态而非错误，viewer
  以中性/警示色展示。
- `set_devices_changed_callback` 回调线程为 librealsense 内部线程：捕获
  `weak_ptr` 控制块防悬挂，回调内只做邮箱投递。
- 物理插拔无法程序化模拟（无免密 sudo）：`Waiting` 相关转换由状态机单测覆盖，
  端到端插拔依赖真机人工验收（本机 D435if 可手动插拔）。

## 验证方式

状态机全矩阵单测；真机验收：无相机启动（先拔后启）→ Waiting 提示 → 插入 → 自动
出流；流中拔出 → Waiting → 重插/换插恢复；多设备场景（如同时接入 D435if 与模拟
设备不可行时）以选择命令 + 日志验证。

## 关联文档和工作项

[camera_service_design.md](../design/camera_service_design.md)、
[librealsense 台账](../dependency_feedback/librealsense/ledger.md)
