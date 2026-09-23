# M3：IMU 数据通路与相机位姿 3D 视图

> 状态：Planned
> 负责人：Linductor-alkaid（授权 Agent 按工程规范自治执行）
> 所属计划：[Rin 实施总计划](rin-implementation-plan.md)
> 前置：M1（M2 无代码依赖）
> 建议发布点：v0.3.0
> 更新日期：2026-09-24

## 目标

让 RealSense 设备（D435if）的加速度计/陀螺仪数据可控、可取、可融合：Core 提供六轴
姿态融合，viewer 在固定世界坐标系的 3D 场景中实时呈现相机位姿（视锥 + 坐标轴），
并显示 IMU 源频率与姿态数值。数据通路遵守现行边界：`ICameraService` 的
`tryLoad*`/`LatestMailbox` 模式，采集仍由 Executor blocking worker 承载。

## 范围与非目标

范围：

- Core：`MotionSample` / `ImuSnapshot` 公共类型、`ImuFuser` 纯逻辑融合器、
  `ICameraService` 新增 `tryLoadMotion()` / `tryLoadPose()` 通道。
- Adapter：video + ACCEL/GYRO 混合 pipeline、motion intrinsics、gyro→color 外参、
  采集循环 motion 分支、restream 与设备切换语义。
- Viewer：IMU 状态面板（频率/姿态数值）与 3D 位姿视图控件。
- 决策：DEC-010（姿态融合算法）、DEC-011（3D 渲染路径）。

非目标：

- 视觉惯性里程计 / SLAM；六轴 IMU 长期漂移在文档中如实披露。
- IMU 标定流程（直接使用 SDK 出厂 motion intrinsics）。
- T26x / `RS2_STREAM_POSE` 位姿源（见总计划 `POST-05`）。

## 设计与决策依据

- [camera_service_design.md](../design/camera_service_design.md)：四层架构、
  采集循环骨架、`LatestMailbox` 通道语义；本里程碑按同一模式扩展 IMU 通道。
- pinned librealsense 2.58.3：`RS2_STREAM_ACCEL/GYRO` + `RS2_FORMAT_MOTION_XYZ32F`、
  `rs2::motion_frame::get_motion_data()`、
  `rs2::motion_stream_profile::get_motion_intrinsics()`、`examples/motion`。
- pinned EUI-NEO：无现成 3D 视口；候选路径为 shadertoy GLSL、CPU 投影 +
  `polygon`/`rect`、2.5D transform（`DEC-011` 裁决）。
- `EXEC-06`：IMU 采样处理承载方式（采集 worker 循环内分支 + 融合器按样本推进，
  姿态经 `LatestMailbox` 发布；不新增线程）。

## 工作项

- [ ] `M3-01` 调研并冻结 IMU 姿态融合方案（`DEC-010`）：对比互补滤波 / Mahony /
      Madgwick / 简化 EKF 在六轴（无磁力计）场景的漂移特性、计算量、参数敏感性与
      可测试性，给出选型理由、备选与数值验收方案。完成判据：`DEC-010` 状态
      `Accepted`，含依据、备选与可执行的验收方法。
- [ ] `M3-02` 调研并冻结 3D 位姿视图渲染路径（`DEC-011`）：在 EUI-NEO 可用原语
      中选定实现，产出最小原型证据（渲染相机视锥 + 坐标轴，姿态驱动旋转）。
      完成判据：`DEC-011` `Accepted` + 原型记录（文件、命令、证据）。
- [ ] `M3-03` Core IMU 数据契约与通道：`MotionSample`（三轴比力/角速度、设备时间
      戳、序列）、`ImuSnapshot`（姿态四元数、源频率统计、序列）、
      `ICameraService::tryLoadMotion()/tryLoadPose()`、`StreamRequest` IMU 使能位、
      `DeviceCatalog` IMU 能力与速率上报；`NullService` 与 `public_boundary` 测试
      同步扩展。完成判据：契约测试通过，RULE-01 边界编译测试保持通过。
- [ ] `M3-04` 适配器 IMU 流支持：混合 pipeline 配置（video + ACCEL/GYRO）、motion
      intrinsics 读取、gyro→color 外参链、采集循环内 motion 帧轻量分支处理、
      `ImuFuser` 接入与姿态发布、restream/设备切换重建含 IMU。完成判据：离线
      （假实现/纯逻辑）测试覆盖分支与命令语义；真机冒烟按规范记录。
- [ ] `M3-05` Core `ImuFuser` 实现（按 `DEC-010`）：纯逻辑、可注入初值与参数、
      时间戳单调、复位语义幂等。完成判据：数值测试（静态重力对齐收敛、已知旋转
      序列跟踪误差上界、复位幂等）通过。
- [ ] `M3-06` 3D 位姿视图控件（按 `DEC-011`）：固定世界坐标系（坐标轴 + 地面
      网格），相机视锥与相机轴随姿态实时更新，视图重置，姿态不可用时的空态。
      完成判据：投影数学单测通过，UI 集成可用。
- [ ] `M3-07` viewer 集成与关闭回归：IMU 状态面板（源频率、姿态数值）、3D 视图
      卡位；`onShutdown` 关闭顺序回归（新增姿态通道的排空语义）。完成判据：
      关闭路径测试 + tsan 通过（跨上下文姿态数据）。
- [ ] `M3-08` M3 测试矩阵与真机验收：debug/asan/ubsan/tsan 全部通过；D435if 真机
      记录（IMU 出流频率、姿态跟随、restream 后 IMU 恢复、3D 视图）。无设备时按
      工程规范第 4 节记录原因、负责人与补跑条件，不冒充完成。

## 风险与阻塞

- 六轴陀螺零偏漂移导致姿态缓慢发散：属传感器物理限制，选型时评估零偏估计，
  文档如实披露使用边界。
- EUI-NEO 渲染路径能力未知风险：`M3-02` 原型前置降险；若发现框架缺口按
  `docs/dependency_feedback/eui-neo/ledger.md` 流程登记。
- IMU 帧率（约 100-400Hz）对采集循环的影响：motion 分支只做有界校验 + 邮箱
  最新态投递 + 融合推进（无堆分配热路径），视频帧处理不受影响需测试佐证。

## 测试与退出条件

- [ ] `ctest`（debug/asan/ubsan/tsan 预设）全绿，新增：IMU 契约、融合数值、
      投影数学、关闭回归测试。
- [ ] `ImuFuser` 数值验收（`M3-05` 判据）通过并留验证记录。
- [ ] 3D 投影数学测试通过（`M3-06`）。
- [ ] 关闭/取消路径回归 + tsan 通过（`M3-07`）。
- [ ] 真机验收记录或规范化未执行记录（`M3-08`）。
- [ ] 文档同步：`camera_service_design.md` IMU 通路、`DEC-010`/`DEC-011`、总计划
      `SCOPE-07`、CHANGELOG（Unreleased）。

## 验证记录

（按日期追加）
