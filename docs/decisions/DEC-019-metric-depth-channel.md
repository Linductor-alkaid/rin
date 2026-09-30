# DEC-019：米制深度通道与 viewer 策略深度预览

> 状态：Accepted
> 日期：2026-09-30
> 负责人：Linductor-alkaid
> 冻结里程碑：M9
> 替代/被替代：无（M8 算子消费面的贯通，细化 DEC-018 的部署路径）

## 背景与问题

M8 交付了米制深度策略预处理算子（`rin/depth_preproc.hpp`），但算子到真机
数据之间缺两段：adapter 不发布米制深度（Z16 原始格式停在采集侧），viewer
没有可见入口——用户在 M8 验收时界面无任何新能力可测。需要把
"Z16 采集 → 米制帧 → 冻结管线 → 策略张量预览"贯通并在 viewer 呈现。

约束：相机服务为单 pipeline；米制帧是 float32 数据面（约 1.6 MiB/帧
@848×480/30 fps）；渲染线程纪律（RULE-05/07）禁止在 UI 线程做 CPU 密集
处理；O7 时序历史是跨帧有状态（单写者约定，depth_preproc.hpp 文件头注）。

## 决策

1. **adapter 发布米制深度 rendition**：采集 worker 内新增 Z16×depth_scale →
   float32 米制转换（线性单趟，与既有四路 rendition 转换同位置同负载量级），
   经新 `LatestMailbox<DepthMetricSample>` 发布（最新态语义，与
   Frame/GrayFrame 通道同模式）。`DepthMetricSample { sequence,
   deviceTimestampMs, frame(DepthFrameF32) }` 承载源序号与时间戳（
   `DepthFrameF32` 本身无序号字段，契约上沿用 MotionSample/IntrinsicsSnapshot
   的 payload-struct 先例）。无效像素保持 0.0（语义"无效"由下游 O1 填充，
   adapter 不预填远距—— rendition 保持测量原值，策略语义归消费方）。
2. **契约加性扩展（纯虚，同 M6-04 先例）**：
   `ICameraService::tryLoadDepthMetric(lastSeenSequence, DepthMetricSample&)`，
   "上次已见序号"最新态语义；无新帧返回 false。既有方法语义不变。
3. **策略预处理在 Executor 周期 tick 内执行，不在渲染线程**：viewer 应用层
   新组件 `PolicyDepthPreview`（无 EUI 类型，可 headless 单测）——
   `submit_periodic_cancellable_with_handle`（20 ms ≈ 策略 50 Hz；软调度、
   允许抖动，符合"允许抖动的后台周期任务使用 timer 能力"路由）驱动：
   tryLoadDepthMetric → O6 组合 → O7 append/sample → 渲染预览快照 →
   `LatestMailbox` 发布。tick 闭包持弱引用（引擎同款掉队 tick 生命周期
   闭合）；busy 跳过防重叠（周期 ≫ 单帧成本，防御性）；流重启检测
   （`sample.sequence < 上次值` → `PolicyDepthHistory` 复位，单写者 tick
   线程内完成）。生命周期 owner 为 ViewerContext（AppRuntime，AGENTS 规则
   7/8）：服务创建后 start，onShutdown 在服务 stop 之后、executor shutdown
   之前 stop。
4. **快照显示语义（M9 为预览，不是策略运行时）**：快照 = 8 帧历史网格
   （2 列 × 4 行，每帧 32×18 最近邻 ×8 → 512×576 Rgba8，灰度复制，旧→新
   阅读序、最新帧右下）+ 源序号 + 会话累计处理帧数 + 处理速率 EMA。
   预览消费 [0,1] 归一化值（×255 量化显示）；float 张量到策略推理的交付
   （ONNX 或进程间）不在本决策范围（另行立项）。预览历史不复位跨 stop
   间隙的陈旧帧语义由流重启检测覆盖（restream 必经 sequence 回绕）。
5. **PolicyDepthConfig 固定为默认值（E3-Parkour 部署值）**：不设 UI 参数
   ——配置已由 DEC-018 冻结，预览不是调参工具；后续策略运行时立项再议
   配置面。

## 备选方案

- **渲染线程内联预处理（pump 直接 process）**：每帧 0.3–1 ms CPU 计算
  违反 RULE-05/07 既有纪律（imu_panel 文件头明示"不在渲染线程执行 CPU
  密集处理"）。否决。
- **adapter 内执行冻结管线**：策略专属语义进入设备适配层，违反职责分层
  （adapter 保持设备 rendition 通用）；且 O7 有状态逻辑放进阻塞 worker
  使关闭排空复杂化。否决。
- **工作流节点承载（source_policy_depth）**：O7 时序有状态与 IImageNode
  无状态契约冲突（DEC-018 已论证），且预览不属于画布编排语义。否决。

## 影响与风险

- 契约纯虚扩展破坏性：`FakeCameraService`（test_run_control）与 adapter
  实现同步补齐（既有先例 M6-04 同路径）。
- 米制通道内存：1.6 MiB × 共享不可变（单消费者场景引用计数常驻 1–2 份）
  + 预览快照 512×576×4 ≈ 1.2 MiB（邮箱单槽换新）——有界。
- tick 20 ms 与 30 fps 帧率错配：tick 消费"上次已见序号"最新态，多 tick
  无帧为空转（false 快路径），不排队不积压（AGENTS 规则 10）。

## 验证方式

- 转换函数纯单测（含 stride/无效值/极端码值）；契约 fake 通道语义测试；
  预览模型 golden（历史网格布局/量化/复位）；tick 组件脚本化假服务时序
  测试（无帧空转/流重启复位/停止后无新快照）。
- 三预设全量 ctest（Independent-Verification-Agent）。
- 真机冒烟：D435if 出流后策略深度卡片出图、序号/速率推进。

## 关联文档和工作项

- [M9 计划](../plans/m9-metric-depth-channel.md)
- [camera_service_design.md](../design/camera_service_design.md)（rendition
  通道契约段）
- 前置：[DEC-018](DEC-018-depth-policy-preproc-operators.md)、
  [DEC-017](DEC-017-workflow-camera-sources.md)（发布模式先例）
