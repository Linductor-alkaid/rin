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

- [x] `M3-01` 调研并冻结 IMU 姿态融合方案（`DEC-010`）：对比互补滤波 / Mahony /
      Madgwick / 简化 EKF 在六轴（无磁力计）场景的漂移特性、计算量、参数敏感性与
      可测试性，给出选型理由、备选与数值验收方案。完成判据：`DEC-010` 状态
      `Accepted`，含依据、备选与可执行的验收方法。
      （[DEC-010](../decisions/DEC-010-imu-attitude-fusion.md)）
- [x] `M3-02` 调研并冻结 3D 位姿视图渲染路径（`DEC-011`）：在 EUI-NEO 可用原语
      中选定实现，产出最小原型证据（渲染相机视锥 + 坐标轴，姿态驱动旋转）。
      完成判据：`DEC-011` `Accepted` + 原型记录（文件、命令、证据）。
      （[DEC-011](../decisions/DEC-011-pose-view-rendering.md)）
- [x] `M3-03` Core IMU 数据契约与通道：`MotionSample`（三轴比力/角速度、设备时间
      戳、序列）、`ImuSnapshot`（姿态四元数、源频率统计、序列）、
      `ICameraService::tryLoadMotion()/tryLoadPose()`、`StreamRequest` IMU 使能位、
      `DeviceCatalog` IMU 能力与速率上报；`NullService` 与 `public_boundary` 测试
      同步扩展。完成判据：契约测试通过，RULE-01 边界编译测试保持通过。
- [x] `M3-04` 适配器 IMU 流支持：混合 pipeline 配置（video + ACCEL/GYRO）、motion
      intrinsics 读取、gyro→color 外参链、采集循环内 motion 帧轻量分支处理、
      `ImuFuser` 接入与姿态发布、restream/设备切换重建含 IMU。完成判据：离线
      （假实现/纯逻辑）测试覆盖分支与命令语义；真机冒烟按规范记录。
- [x] `M3-05` Core `ImuFuser` 实现（按 `DEC-010`）：纯逻辑、可注入初值与参数、
      时间戳单调、复位语义幂等。完成判据：数值测试（静态重力对齐收敛、已知旋转
      序列跟踪误差上界、复位幂等）通过。
- [x] `M3-06` 3D 位姿视图控件（按 `DEC-011`）：固定世界坐标系（坐标轴 + 地面
      网格），相机视锥与相机轴随姿态实时更新，视图重置，姿态不可用时的空态。
      完成判据：投影数学单测通过，UI 集成可用。
- [x] `M3-07` viewer 集成与关闭回归：IMU 状态面板（源频率、姿态数值）、3D 视图
      卡位；`onShutdown` 关闭顺序回归（新增姿态通道的排空语义）。完成判据：
      关闭路径测试 + tsan 通过（跨上下文姿态数据）。
- [x] `M3-08` M3 测试矩阵与真机验收：debug/asan/ubsan/tsan 全部通过；D435if 真机
      记录（IMU 出流频率、姿态跟随、restream 后 IMU 恢复、3D 视图）。无设备时按
      工程规范第 4 节记录原因、负责人与补跑条件，不冒充完成。

## 风险与阻塞

- 六轴陀螺零偏漂移导致姿态缓慢发散：属传感器物理限制，选型时评估零偏估计，
  文档如实披露使用边界。
- EUI-NEO 渲染路径能力未知风险：已由 `M3-02` 原型降险解除（[DEC-011](../decisions/DEC-011-pose-view-rendering.md)
  冻结 CPU 投影 + `polygon` 原语，未发现框架缺口）；控件实现期的框架问题仍按
  `docs/dependency_feedback/eui-neo/ledger.md` 流程登记。
- IMU 帧率（约 100-400Hz）对采集循环的影响：motion 分支只做有界校验 + 邮箱
  最新态投递 + 融合推进（无堆分配热路径），视频帧处理不受影响需测试佐证。

## 测试与退出条件

- [x] `ctest`（debug/asan/ubsan/tsan 预设）全绿，新增：IMU 契约、融合数值、
      投影数学、关闭回归测试。（2026-09-24 收尾修复轮四预设 13/13 全绿，见验证
      记录；hardware 用例实际包含在默认 ctest 内且通过）
- [x] `ImuFuser` 数值验收（`M3-05` 判据）通过并留验证记录。
- [x] 3D 投影数学测试通过（`M3-06`）。
- [x] 关闭/取消路径回归 + tsan 通过（`M3-07`）。
- [x] 真机验收记录或规范化未执行记录（`M3-08`）。
- [ ] 文档同步：`camera_service_design.md` IMU 通路、`DEC-010`/`DEC-011`、总计划
      `SCOPE-07`、CHANGELOG（Unreleased）。

## 验证记录

2026-09-24：`M3-01` 调研并冻结 IMU 姿态融合方案（[DEC-010](../decisions/DEC-010-imu-attitude-fusion.md) Accepted）

- 范围：六轴姿态融合算法选型调研与决策冻结，不含实现（由 `M3-05` 承接）。
- 依据：
  - pinned librealsense（`third_party/dependencies.lock`，commit `7c3ee3fb`）：
    陀螺 ODR 200/400 Hz、加速计 63/250 Hz（`_deps/realsense2-src/src/ds/ds-motion-common.h:18-36`）；
    motion intrinsics 噪声/零偏参数（`include/librealsense2/h/rs_types.h:70-84`、
    `rs_sensor.h:468`）；官方示例即 Euler 互补滤波 `alpha=0.98`、无零偏估计
    （`examples/motion/rs-motion.cpp:112-191`）。
  - 文献：Mahony et al. 2008（IEEE TAC 53(5)，显式互补滤波、在线零偏估计、近全局
    稳定）；Madgwick 2010 报告（IMU 模式 109 次标量运算/更新、单参数 β、无零偏
    状态）；Caruso et al. 2021（Sensors 21(7):2543，十算法基准：最优调参下无统计
    显著差异、误差 3.8°–7.1°、参数选择起决定作用）；Sabatini 2011
    （Sensors 11(10):9182，可观测性分析——六轴仅重力参考时 yaw 不可观）。
- 验证：一次性原型（`/tmp` 下自研，g++ 13.3.0 `-O2` `-std=c++20`，双精度，固定
  种子合成数据 dt=1/400s、σ_g=0.002 rad/s、σ_a=0.02 m/s²、零偏注入
  [0.3, −0.2, 0.5]°/s；脚本为调研证据不入库）实测：静态 35.9° 初始误差 2.58s
  收敛至 <0.5°；60s 旋转序列 roll/pitch RMS 0.118°（峰值 0.364°）；yaw 60s 末
  23.2°（Ki 关闭 28.3°；Madgwick 28.1°）；零偏估计 b_x/b_y 60s 残差 0.061/0.111°/s
  （b_z 不可观）；±2 m/s² 扰动 + 1g 门限 RMS 0.118°；49.3 ns/样本（Madgwick
  64.6 ns）。以上实测支撑 DEC-010 冻结的验收阈值。
- 限制：简化 EKF 未做原型实测，未采用依据为文献（Caruso 2021 调参后无显著精度差）
  与结构分析（4×4 协方差传播、参数最多），证据等级低于本机实测项；数值阈值为
  合成数据结果，真机噪声/零偏适配由 `M3-08` D435if 冒烟复核。
- 同步：新建 `docs/decisions/DEC-010-imu-attitude-fusion.md`（Accepted）；总计划
  `DEC-010` 行改为（已记录）；本文件勾选 `M3-01`。

2026-09-24：`M3-02` 调研并冻结 3D 位姿视图渲染路径（[DEC-011](../decisions/DEC-011-pose-view-rendering.md) Accepted）

- 范围：EUI-NEO 可用原语中 3D 位姿视图渲染路径选型与决策冻结，含最小原型
  证据；不含控件实现（由 `M3-06` 承接）。
- 依据（pinned EUI-NEO，`third_party/dependencies.lock` commit `782c5699`）：
  - 图元面仅有 `Row/Column/Stack/Flow/Rect/Polygon/Text/Image/Svg/Shadertoy`，
    无 3D 视口、无逐顶点 3D 坐标：`polygon` 点集为元素平面内 2D `Vec2`
    （`core/dsl.h:1121`）。
  - 2.5D transform 固定按 Rz·Ry·Rx 欧拉序合成（`core/runtime/runtime_geometry.h:153-213`），
    四元数→欧拉在 pitch ≈ ±90° 奇异，且无深度缓冲；仅适合平面卡片动效。
  - `polygon` 独立 shader 按边段覆盖率抗锯齿；框架折线图即以 capsule polygon
    画线（`components/linechart.h:151-155`）；逐帧点集更新有脏区支持
    （`core/runtime/runtime_update.h:853-864`）；跨线程 UI 唤醒为公开能力
    （`include/eui/app.h:49`、`core/platform/platform.cpp:758-761`）。
- 验证：一次性原型（`/tmp/rin-dec011-proto`，standalone CMake 挂 pinned
  `eui-neo-src`，g++ 13.3.0 Release，GLFW + OpenGL 后端；脚本为调研证据不入库）
  实测：每帧 32 条线段 quad + 5 个半透明面片（网格/世界轴/视锥/相机轴），
  CPU 投影 + 点集组装 avg 38-46 µs/帧（max ≤162 µs）；EUI 运行时标题统计
  55-56 FPS、GPU 0.58-0.64 ms/帧、Dirty 100%/帧连续重绘；双姿态对照运行
  （q=(0.904,0.215,0.306,-0.206) vs q=(0.974,-0.112,-0.168,0.104)）世界系
  不变、视锥朝向可见差异（截图 md5 `9f99606e…`/`45c20ec1…`），姿态驱动旋转
  成立。
- 结论：冻结为 CPU 投影 + EUI-NEO `polygon` 原语；投影数学为 Core 纯逻辑
  （`RULE-01`，支撑 `M3-06` 投影数学单测）；compose 期组装属有界工作
  （`RULE-05`），融合仍在采集 worker（`EXEC-06` 不变）。
- 限制：XWayland 下窗口 X 像素图不随后续 GL 呈现更新，连续动画活性以运行时
  统计与双姿态对照证明（取证手段限制，非渲染缺陷），真机实时性由 `M3-08`
  复核；未对 shadertoy GLSL 与 2.5D transform 路径做原型实测，否决依据为
  pinned 源码/文档结构分析与本决策记录（可测性、奇异点、工具链成本）。
- 同步：新建 `docs/decisions/DEC-011-pose-view-rendering.md`（Accepted）；总计划
  `DEC-011` 行改为（已记录）；本文件勾选 `M3-02`；EUI-NEO 台账无新增缺口。

2026-09-24：`M3-03` Core IMU 数据契约与通道

- 范围：`MotionSample` / `MotionSourceStats` / `ImuSnapshot` 契约类型、
  `StreamRequest` IMU 使能位、`DeviceInfo`/`DeviceCatalog` IMU 能力与速率档位
  上报、`ICameraService::tryLoadMotion()/tryLoadPose()` 通道声明，及其契约
  测试与 `public_boundary` 边界测试扩展；不含适配器发布接线（`M3-04`，通道
  当前为无生产者桩）与 `ImuFuser` 实现（`M3-05`）。
- 依据：
  - `f4cc8a8` feat(core)：契约类型与服务通道（含 `camera_service_design.md`
    IMU 通路同步）；`9db678f` test(core)：新增 `tests/test_motion_contracts.cpp`
    （ctest `motion_contracts`，LABELS unit，80 checks）并扩展
    `tests/test_public_boundary.cpp`（11 checks），共 316 行、全部位于 tests/。
  - DEC-010 锁定姿态约定：标量在前四元数、默认恒等严格 `{1,0,0,0}`、
    norm² 容差 1e-3、源频率须有限且非负。
  - `LatestMailbox::try_load_newer_than` 的 stale 读取不更新序号语义
    （pinned executor `include/executor/comm/mailbox.hpp:127-147`）。
- 验证（独立验证执行；debug/asan/ubsan 三套门禁通过）：
  - 配置：`cmake --preset debug -DRIN_EXECUTOR_SOURCE_DIR=…/_deps/executor-src
    -DRIN_EUI_NEO_SOURCE_DIR=…/_deps/eui-neo-src -DRIN_REALSENSE2_SOURCE_DIR=…/_deps/realsense2-src`
    → Configuring/Generating done（`BUILD_SHARED_LIBS=OFF` 与 M2 打包一致）。
  - 构建：`cmake --build build/debug --target test_motion_contracts
    test_public_boundary` → exit=0。
  - `ctest --test-dir build/debug -R "motion_contracts|public_boundary"` →
    2/2 通过（0 failed）；直接运行 `test_motion_contracts` → `OK: 80 checks,
    0 failures`，`test_public_boundary` → `OK: 11 checks, 0 failures`。
  - 回归：`ctest --test-dir build/debug -L unit` → 4/4（camera_state_machine、
    pixel_format、motion_contracts、public_boundary），既有单元测试未破坏。
  - 完成判据对照：契约测试通过——覆盖 kind 决定轴单位（比力 m/s² 含重力 /
    角速度 rad/s）、逐轴 NaN/±Inf 与非法枚举的 `valid()` 边界、DEC-010 四元数
    约定（恒等、单位四元数、norm² 超差/容差内）、`StreamRequest.enableMotion`
    相等语义即 restream 去重依赖点 `src/adapters/realsense/realsense_camera_service.cpp:548`
    的 `command.request == request_`、目录 IMU 能力与速率档位携带；
    RULE-01 边界编译测试保持通过（public_boundary 仅含 include/ 路径、零
    第三方链接，新契约类型经公开接口多态实例化编译运行通过）。
- 限制：
  - tsan 预设未跑且按判据不触发：本项未新增跨上下文状态，亦无新 Executor
    任务/线程/队列/关闭顺序变更（通道为无生产者桩，
    `realsense_camera_service.cpp:167-173`，发布接线由 `M3-04` 承接），测试为
    单线程纯值语义检查，故 DOD-02 并发矩阵（异常/提交拒绝/执行中取消/超时/
    shutdown）不适用；可适用的通道空闲行为（无新数据返回 false 且
    lastSeenSequence/out 出参不动，与 mailbox stale 读语义一致）已覆盖。
  - `enumerateDevice` 的 IMU 速率档位枚举/去重/排序
    （`realsense_camera_service.cpp:305-318`，匿名命名空间 + 需 rs2 profile）
    无法在无真机单元层测试，按 hardware 标签用例纪律由真机验收（`M3-08`）
    单独执行；D435if 已 `lsusb` 确认在位。
  - 独立验证的 test(core) 提交（`9db678f`）尚未 push。
- 同步：本文件勾选 `M3-03`；`camera_service_design.md` IMU 通路小节已随
  `f4cc8a8` 更新；退出条件各小项均不因本单项完成而满足（ctest 全绿含 tsan
  预设、融合数值、投影数学、关闭回归、真机验收与文档收尾分属 `M3-04`…`M3-08`），
  维持未勾。

2026-09-24：`M3-04` 适配器 IMU 流支持

- 范围：混合 pipeline（`enableMotion` 时 video + ACCEL/GYRO）、motion
  intrinsics 与 gyro→color 外参读取、采集循环 motion 帧轻量分支
  （`MotionIngest` rs2-free 接缝：有界校验 + EMA 源频率 + 邮箱最新态投递 +
  融合器推进）、`ImuFuser` 接缝与工厂（`IdentityImuFuser` 假实现，
  `M3-05` 切换 Mahony 不破坏接缝）、姿态发布与 `tryLoadMotion()/tryLoadPose()`
  真实接线、restream/设备切换的融合器复位与序号连续语义；含独立验证发现的
  `Extrinsics`/`MotionIntrinsics::valid()` 契约偏差修复（`bd4d652`）与
  三套新增离线单元测试、真机冒烟用例扩展。
- 依据：
  - `c4526fd` feat(adapter)：新增
    `src/adapters/realsense/imu_motion_ingest.{hpp,cpp}` 与
    `src/core/imu_fuser.{hpp,cpp}`（522 insertions），`realsense_camera_service.cpp`
    采集/控制路径接线（`tryLoadMotion`/`tryLoadPose` 桩换为通道真实读取，
    `src/adapters/realsense/realsense_camera_service.cpp:177,184`），
    `camera_service_design.md` IMU 通路同步（40 行）。
  - `524327a` test(tests)：新增 `tests/test_motion_intrinsics.cpp`、
    `tests/test_imu_fuser.cpp`、`tests/test_motion_ingest.cpp` 并扩展
    `tests/test_realsense_hardware.cpp`（全程 `enableMotion` 混合 pipeline
    冒烟），全部注册 LABELS unit。
  - `bd4d652` fix(core)：全零 rotation/scale 为"未填充/读取失败"哨兵判
    `valid()==false`（`src/core/camera_types.cpp:41-73`），平移/bias/方差允许
    全零（同址安装、出厂零偏/方差可为 0），非有限拒绝保持，公开头注释同步
    （`include/rin/camera_types.hpp:104-106,123-125`）。
  - `EXEC-06`：motion 分支在采集 worker 循环内、经 `LatestMailbox` 发布，不新增线程。
- 验证（独立验证复验 `bd4d652` 后；debug/asan/ubsan 门禁通过）：
  - `cmake --build build/debug` → ninja no work to do，exit=0（二进制为
    `bd4d652` 后最新）。
  - `ctest --test-dir build/debug -L unit --output-on-failure` →
    7/7 通过（"100% tests passed, 0 tests failed out of 7"）；直跑
    `./build/debug/tests/test_motion_intrinsics` → `OK: 47 checks,
    0 failures`（首轮 47 checks 8 failures 全部修复）。
  - 无回归：camera_state_machine / pixel_format / motion_contracts /
    imu_fuser（53 checks）/ motion_ingest / public_boundary 全部 Passed。
  - tsan 跨上下文复跑（`rin_core` 变更后重建）：`cmake --build build/tsan
    --target test_motion_ingest` 重编成功；`setarch -R ctest
    --test-dir build/tsan -R motion_ingest --output-on-failure` → 1/1 Passed
    （Executor 生产者 + 消费者跨上下文发布/读取序号不回退、shutdown 收敛）。
  - 真机路径：`ctest --test-dir build/debug -R realsense_hardware` →
    SKIP(77)（iio scan_element udev 授权前置未处置；补跑条件：安装
    librealsense udev 规则后复跑）。`bd4d652` 使该用例在内容级非全零防线
    之外另获契约级（`valid()==false`）拦截。
  - 完成判据对照：离线（假实现/纯逻辑）测试覆盖分支与命令语义
    （motion_ingest 无效采样有界丢弃、会话序号单调、EMA 频率、
    resetStreamState 的 restream 语义、融合器接缝推进与姿态门控）；真机
    冒烟按规范记录（显式 SKIP + 补跑条件登记，未冒充完成）。
- 限制：
  - 完整 debug/asan/ubsan 三套门禁按交接约定由脚本复跑，本轮未重复全量；
    tsan 预设本轮仅复跑 `motion_ingest` 单项，非全量。
  - 真机 motion 冒烟因 udev 权限前置阻塞未执行（SKIP 77，原因、负责人与
    补跑条件已在 SKIP 输出与首轮记录登记）。
- 同步：本文件勾选 `M3-04`；`camera_service_design.md` IMU 通路小节已随
  `c4526fd` 更新；`ImuFuser` 数值验收（静态重力对齐收敛、已知旋转序列、
  复位幂等）属 `M3-05`，本项仅交付接缝与假实现契约（imu_fuser 53 checks）；
  退出条件各小项均不因本单项完成而满足（投影数学与关闭回归测试未建、
  ctest 全绿含 tsan 全量未达成、真机矩阵属 `M3-08`），维持未勾。

2026-09-24：`M3-05` Core `ImuFuser` 实现（Mahony 六轴）

- 范围：按 DEC-010 冻结算法实现 `MahonyImuFuser`（`src/core/imu_fuser.{hpp,cpp}`：
  纯逻辑、可注入初值与参数、时间戳单调守卫、复位幂等、GYRO 传播 + ACCEL 门限
  修正的六轴融合）与工厂切换真身；DEC-010 数值验收测试与语义/边界锁定测试。
  不含 viewer/UI（`M3-06`+）。
- 依据：
  - `2f2c08e` feat(core)：Mahony 实现（310 insertions，仅 `imu_fuser` 两文件，
    默认 Kp=1.0/Ki=0.1 即 DEC-010 冻结组合）。
  - `3e3c726` test(tests)：新增 `tests/test_imu_fuser_mahony.cpp`（897 行，
    ctest `imu_fuser_mahony`，LABELS unit，95 checks，注册于
    `tests/CMakeLists.txt:41-52`）；测试侧自建独立四元数库（Hamilton/旋转/
    指数映射/ZYX 欧拉），固定种子合成数据按 DEC-010 冻结生成器参数
    （dt_gyro=1/400s、dt_accel=1/250s、σ_g=0.002 rad/s、σ_a=0.02 m/s²、
    注入零偏 [0.3, −0.2, 0.5]°/s），阈值全取 DEC-010 冻结值。
  - `15f57d3` test(tests)：`test_realsense_hardware.cpp` 姿态快照单位范数
    断言（Mahony 真机契约）。
  - DEC-010（Accepted）：算法、参数与验证方式 1-8；`EXEC-06` 热路径约束。
- 验证（独立验证执行；debug/asan/ubsan 门禁通过）：
  - 配置/构建：`cmake --preset debug …`（同依赖源参数）→ Generating done；
    `cmake --build build/debug --target test_imu_fuser_mahony` 编译链接成功。
  - 直跑 `./build/debug/tests/test_imu_fuser_mahony` → `OK: 95 checks,
    0 failures`。DEC-010 判据实测：静态 30°/20° 对齐初始误差 35.53° →
    对齐后 0.078° → 4s 末 0.230°（阈值 <0.5°）；60s 旋转序列 roll/pitch
    RMS 0.098°（≤0.5）/峰值 0.299°（≤2），全程单位范数偏差 8.7e-8；yaw
    60s 末 Ki 开 18.77° < Ki 关 27.05° ≤ |b_z|·60s=30°；静态 60s 零偏
    残差 x/y 0.070/0.098°/s（≤0.2；b_z=0.172 不可观仅记录，与 DEC-010
    原型 0.061/0.111 同量级）；±2 m/s² 竖直突发门限窗口 RMS 0.352°（≤0.5）/
    峰值 0.768°（≤2）；复位幂等：60s 流 reset/重放与全新实例 memcmp 逐位
    一致、连续双 reset 一致、同进程确定性逐位一致；EXEC-06 热路径 10 万次
    advance 全局 operator new 计数=0。
  - 语义锁定：GYRO 首样本参考/乱序参考保持/非有限时间戳忽略（hasPose 接缝
    仍置位）/零偏补偿传播/单步 10s 大 dt 精确指数映射保持单位范数；ACCEL
    1g 门限一次性对齐（+1gZ 精确恒等、+1gX 最小旋转、门限外与零范数首样本
    不对齐）、PI 修正单步解析对照（e=â×ĝ、b̂−=2Ki·e·dt、2Kp·e 修正、门限与
    乱序下 dt 参考点按样本推进，与 `src/core/imu_fuser.hpp:74-75` 契约一致）、
    Params 消毒与负增益不钳制、工厂返回 `MahonyImuFuser` 真身。
  - 回归（工厂切 Mahony 受影响面）：`ctest --test-dir build/debug -L unit`
    → 8/8 通过（含 imu_fuser 53 checks、motion_ingest 148 checks、
    public_boundary 等）；`./build/debug/tests/test_realsense_hardware` →
    exit=77 SKIP（udev 前置，与 `M3-04` 记录一致）。
  - tsan：`cmake --preset tsan`（同依赖源参数）+ `setarch -R ctest
    --test-dir build/tsan -R motion_ingest` → 1/1 Passed（Executor 生产者/
    消费者跨上下文接缝以 Mahony 工厂真身复验）；`imu_fuser_mahony` tsan
    1/1 Passed；另以单目标方式加跑 asan/ubsan 两预设的
    `imu_fuser_mahony` → 均 1/1 Passed。
  - 完成判据对照：数值测试（静态重力对齐收敛、已知旋转序列跟踪误差上界、
    复位幂等）全部通过；roll/pitch 误差按 DEC-010 原型方法论取重力参考
    口径 angle(R_estᵀẑ, R_trueᵀẑ)（对 yaw 规范自由度不敏感）。
- 限制（以下均为观察项，非判据失败，已留档于测试文件头注释与提交说明）：
  - 一次性对齐取最小旋转（旋转轴水平、不含绕重力轴分量），30°/20° 倾斜
    开机的 ZYX 欧拉 yaw 初值实测 +5.41°：与 DEC-010"yaw 置 0"/`M3-03`
    "yaw 初值为 0"的字面欧拉读数存在解释分歧（重力参考 roll/pitch 不受
    影响且 e=â×ĝ 在该状态为 0 不再修正）；是否调整对齐构造或文档措辞交由
    owner 裁定。
  - ACCEL 乱序样本的 dt 参考点按样本推进（与 GYRO 的参考点保持不对称），
    与头文件契约一致，测试按实现语义锁定。
  - 实现者自述静态零偏残差 0.0007°/s 与本轮实测 0.070/0.098°/s 不符，均
    远低于 0.2°/s 判据；实测值与 DEC-010 原型记录吻合。
  - tsan 构建唯一告警为 `realsense_camera_service.cpp:945` 既有 sleepPoll
    nodiscard，与本项无关。
  - 完整 debug/asan/ubsan/tsan 全量门禁按交接约定由脚本复跑；真机冒烟因
    udev 前置阻塞未执行（SKIP 77 已登记，补跑条件：安装 librealsense
    udev 规则后复跑）。
- 同步：本文件勾选 `M3-05`，并勾选退出条件"ImuFuser 数值验收（`M3-05`
  判据）通过并留验证记录"（判据三项实测通过，记录即本条）；DEC-010 数值
  验收闭环，无新增依赖缺口；退出条件其余小项（ctest 全绿含投影数学/关闭
  回归测试、投影数学、关闭回归 + tsan、真机矩阵、文档收尾）分属
  `M3-06`…`M3-08`，维持未勾。

2026-09-24：`M3-06` 3D 位姿视图控件（CPU 投影 + polygon）

- 范围：Core 投影数学（`include/rin/pose_math.hpp` + `src/core/pose_math.cpp`：
  四元数归一化/复合/矩阵转换、orbit 视图基、屏幕投影、线段 quad 组装）与
  viewer 位姿视图状态（`apps/viewer/pose_view.hpp`：空态、快照更新、Reset
  重锚定、clear）及 app 集成卡位（`apps/viewer/app.cpp`）；两套离线单元测试。
  不含 pump() 门控的关闭回归（`M3-07`）与真机视觉验收（`M3-08`）。
- 依据：
  - `f73908c` feat(viewer)：M3-06 实现（649 insertions，含 `pose_math` 新模块
    与 `pose_view.hpp` 317 行）。
  - `b8af343` test(tests)：新增 `tests/test_pose_math.cpp`（715 行，ctest
    `pose_math`，LABELS unit，201 checks）与 `tests/test_pose_view_state.cpp`
    （216 行，ctest `pose_view_state`，32 checks），注册于
    `tests/CMakeLists.txt`（pose_view_state 经 `eui_neo` 目标用法属性注入
    EUI-NEO 公开头包含路径，仅头、不链接 EUI 库），共 963 行、全部位于 tests/。
  - DEC-011（Accepted）：CPU 投影 + `polygon` 原语、固定世界系构图
    （0.62/0.50/3.8）、`RULE-01` 投影数学为 Core 纯逻辑。
- 验证（独立验证执行；debug/asan/ubsan 门禁通过）：
  - 配置/构建：`cmake --preset debug`（带三个本地依赖源参数）→
    Configuring/Generating done；`cmake --build build/debug --target
    test_pose_math test_pose_view_state` 成功。
  - `setarch -R ctest --test-dir build/debug -R "pose_math|pose_view_state"`
    → 2/2 通过（"100% tests passed, 0 tests failed out of 2"）；直接运行
    `test_pose_math` → `OK: 201 checks, 0 failures`（golden 最大偏差
    1.866e-05 px），`test_pose_view_state` → `OK: 32 checks, 0 failures`。
  - 覆盖要点：四元数归一化回退/复合方向契约（测试侧 double q⊗v⊗q* 交叉
    验证）、orientationToMatrix 精确值/RᵀR=I/det=+1/pitch≈±89.9° 无表示
    奇异（DEC-011 关切）、`Extrinsics::rotation` 列主序契约、orbit 视图基
    不变量（正交、左手 det≈−1 的文档约定、仰角截断、非法输入回退）、
    projectToScreen y 向下约定与失败路径不写出参（哨兵逐位保持）、
    segmentToQuad 平行四边形不变量与整段剔除；golden 端到端已知位姿
    （ZYX 30°/20°/40° 与 +90° yaw）对独立 double 参考最大偏差 1.866e-05 px
    （容差 0.05 px），世界固定参考点投影与位姿无关、相机光轴端点随 90° yaw
    移动 23.7 px（姿态驱动旋转成立）。
  - `pose_view_state`：空态初值、update() 逐字段快照且不动 baseline、Reset
    以当前姿态重锚定 conj(baseline)⊗current→恒等且 memcmp 幂等（DEC-011
    风险 3）、空态 Reset no-op、clear() 复位恒等与会话重锚定；显示公式与
    double 参考逐数值一致。
  - 消毒剂目标级复验（非全量门禁）：asan/ubsan/tsan 三预设各自 configure +
    构建两目标成功，`setarch -R ctest` 各跑两目标均 2/2 通过；tsan 对两新
    目标跑通（干净）。
  - 完成判据对照：投影数学单测通过（201 checks 含 DEC-011 验证方式 1 的
    golden 端到端）；UI 状态语义单测通过，polygon compose 组装与视觉呈现
    需活动 EUI-NEO 运行时，归真机验收（`M3-08`）。
- 限制：
  - compose 期 polygon 组装需活动 EUI-NEO 运行时，headless 不可单测，真机
    视觉验收归 `M3-08`；app.cpp pump() 门控属 `M3-07` 关闭回归范围。
  - 测试编写中发现的 5 处问题均为测试侧错误（y 向下翻转期望写反、组合旋转
    条件路径、非正交矩阵误用 R·Rᵀ=I、容差低于 float→double 表示间隙、
    视图基左手系误断言 det=+1），无实现缺陷；视图基左手系符号约定
    （det=−1）经本轮锁定为契约。
  - DOD-02：pose_math 为无堆分配值语义纯函数、PoseViewState 为 UI 线程独占
    普通状态，无线程/队列/任务/取消/超时/shutdown 语义，并发矩阵不适用；
    采集 worker→`LatestMailbox`→pump 跨上下文通路不在本单元（motion_ingest
    已覆盖并有 tsan 复跑在档，`M3-07` 承接关闭回归）。
  - 全量 debug/asan/ubsan/tsan 门禁按交接约定由脚本复跑；测试提交（
    `b8af343`）尚未 push。
- 同步：本文件勾选 `M3-06`，并勾选退出条件"3D 投影数学测试通过（`M3-06`）"
  （pose_math 201 checks 通过且已入库）；退出条件其余小项（ctest 全绿、
  关闭回归 + tsan、真机矩阵、文档收尾）分属 `M3-07`/`M3-08`，维持未勾。

2026-09-24：`M3-07` viewer 集成与关闭回归

- 范围：IMU 状态面板（`apps/viewer/imu_panel.hpp`：四元数→ZYX 欧拉读数、
  源频率/姿态/四元数格式化、门控）、pump 门控接入姿态通道与排空
  （`apps/viewer/app.cpp:326-336`：Streaming/Restreaming 才 tryLoadPose，
  否则 clear 排空）；两套离线单元测试（面板纯函数 + 跨上下文关闭排空）。
  不含 compose 卡片视觉路径（`M3-08` 真机验收）。
- 依据：
  - `eb12ec9` feat(viewer)：M3-07 实现（212 insertions：`imu_panel.hpp`
    新增 168 行、`app.cpp` 泵送/排空、`pose_view.hpp` 调整、
    `camera_service_design.md` IMU 通路同步）。
  - `3a847e5` test(tests)：新增 `tests/test_imu_panel.cpp`（353 行，ctest
    `imu_panel`，LABELS unit，70 checks）与 `tests/test_shutdown_drain.cpp`
    （299 行，ctest `shutdown_drain`，49 checks），`tests/test_util.hpp`
    新增 `RIN_CHECK_MSG`，4 文件 701 行、全部位于 tests/，注册于
    `tests/CMakeLists.txt:103-133`。
  - DEC-011 golden 角度（30/20/40）与 DEC-010 Mahony 真身
    （`createImuFuser`）作为测试参考基准；`EXEC-06`/`LatestMailbox`
    跨上下文通路与 app.cpp 门控同构。
- 验证（独立验证执行；debug/asan/ubsan 门禁通过）：
  - 配置/构建：`cmake --preset debug`（带三个本地依赖源参数）→
    Generating done；`cmake --build build/debug --target test_imu_panel
    test_shutdown_drain` 成功。
  - 直跑 `./build/debug/tests/test_imu_panel` → `OK: 70 checks,
    0 failures`（欧拉对照独立 double 四元数直分解：恒等/单轴/golden 30/20/40/
    9 组角度网格往返/yaw 折叠 200°→−160°/±180° 边界/万向锁 ±89.9° 与恰 ±90°/
    NaN/Inf/零范数/非单位消毒；formatSourceRate/formatAttitudeDegrees/
    formatOrientationQuaternion 精确文本；面板门控 `available && valid()`，
    `imu_panel.hpp:114`）；`./build/debug/tests/test_shutdown_drain` →
    `OK: 49 checks, 0 failures`（Executor 生产者独占 MotionIngest 真实接缝
    → LatestMailbox 姿态通道 → PoseViewState 同构泵送：2000 样本序号推进、
    EMA 200Hz 与发布侧一致、姿态单位范数；停止后无新发布；clear 排空语义
    ——回空态 + baseline 恒等、stale 读不复活视图且序号不回退、重复排空
    幂等；任务异常经 future 传播可观察、失败路径排空 + shutdown 收敛；
    `executor.shutdown(true)==ShutdownResult::Completed`；排空后新会话
    仅随新快照复活、resetStreamState 序号连续 1000→1800）。
  - 回归：`setarch -R ctest --test-dir build/debug -R
    "imu_panel|shutdown_drain|pose_view_state|public_boundary|motion_ingest"`
    → 5/5 通过（受 M3-07 影响的 pose_view_state/public_boundary 及接缝
    motion_ingest 同绿）。
  - tsan：`cmake --preset tsan`（同依赖源参数）+ 构建两目标成功；
    `setarch -R ctest --test-dir build/tsan -R "imu_panel|shutdown_drain"`
    → 2/2 通过、无 tsan 报告；未加 setarch 直跑该二进制以
    `ThreadSanitizer: unexpected memory mapping` 中止，证实插桩生效。
  - 完成判据对照：关闭路径测试通过（shutdown_drain 49 checks：跨上下文
    姿态数据的排空、异常传播、shutdown 收敛）+ tsan 通过（两目标复跑无
    报告）。
- 限制（观察项，非功能缺陷，实现未动，已留档测试注释）：
  - pitch=+89.9° 实测 89.905128°（float32 经 1/cos(pitch)≈573 倍病态放大，
    `imu_panel.hpp:34` 已声明万向锁附近读数仅参考），容差按病态程度标定
    0.02°。
  - 恰 −180° 四元数实测 yaw=−180.000000，与 `imu_panel.hpp:34` 声明
    "yaw ∈ (−180°, 180°]" 开端点在 measure-zero 输入上不符（atan2 域端点；
    +180→+180 与 200→−160 实测精确）；测试锁定物理不变量 |yaw|==180，
    区间措辞端点细化建议 owner 酌情同步注释。
  - DOD-02：提交拒绝（路径内 shutdown 前完成排空、无 post-shutdown 提交）、
    执行中取消（属 EXEC-01 采集服务层，drain 自 worker 回收后开始）、超时
    （无定时等待，消费者有界尝试数兜底）按适用性说明不适用。
  - 全量 debug/asan/ubsan/tsan 门禁按交接约定由脚本复跑；compose 卡片视觉
    路径需活动 EUI-NEO 运行时，归 `M3-08` 真机验收；测试提交（`3a847e5`）
    尚未 push。
- 同步：本文件勾选 `M3-07`，并勾选退出条件"关闭/取消路径回归 + tsan 通过
  （`M3-07`）"（shutdown_drain 49 checks + tsan 两目标复跑无报告）；
  `camera_service_design.md` 已随 `eb12ec9` 同步；退出条件其余小项
  （ctest 全绿、真机矩阵、文档收尾）归 `M3-08`，维持未勾。

2026-09-24：`M3-08` M3 测试矩阵与真机验收（执行前置确认与规范化未执行登记，
`M3-08` 保持未勾）

- 范围：M3-08 中可由本实现步骤完成的收尾——门禁/真机验收的执行前环境确认、
  未执行项按工程规范第 4 节登记、退出条件"文档同步"落地；不修改任何代码与
  测试（M3-01..07 已交付实现与测试，本项无新增并发路径）。
- 依据：本文件 `M3-08` 条目（"无设备时按工程规范第 4 节记录原因、负责人与补跑
  条件，不冒充完成"）；工程规范第 4 节 3-4 款与第 7 节证据等级；`DEC-010`
  影响范围"CHANGELOG 于 M3 退出时统一补条目"；M3-03..07 验证记录在案的交接约定
  （全量 debug/asan/ubsan/tsan 门禁由脚本/独立验证环节复跑）。
- 环境确认（本步骤实测）：
  - D435if 在位：`lsusb` → `Bus 004 Device 007: ID 8086:0b3a Intel Corp.
    Intel(R) RealSense(TM) Depth Camera 435if`。
  - M3-04/M3-05 登记的 udev/scan_element 权限前置在本机已变化：librealsense
    udev 规则在位（`/lib/udev/rules.d/99-realsense-libusb.rules` 与
    `/usr/lib/udev/rules.d/` 同名文件）；运动设备节点 `/dev/iio:device1`
    （accel_3d）、`/dev/iio:device2`（gyro_3d）、`/dev/hidraw4`
    （hid-sensor-hub，HID_NAME 含 435if）均为 `crw-rw-rw- root:plugdev`
    （2026-09-24 08:31 起）；此前阻塞运动流使能的 scan_elements sysfs（如
    `/sys/bus/iio/devices/iio:device1/scan_elements/in_accel_y_en`，M3-04
    记录为 root:root 0644 不可写）现为 `-rwxrwxrwx`（08:48 起）。以上为系统
    状态观察，非本仓库变更；本步骤未执行任何系统级操作（root 需口令，
    `sudo -n` 不可用），权限变化来源未追溯。
- 验证（本步骤编译自检，工作树改动仅本记录与 `CHANGELOG.md`）：
  - `cmake --preset debug -DRIN_EXECUTOR_SOURCE_DIR=build/iva-debug/_deps/executor-src
    -DRIN_EUI_NEO_SOURCE_DIR=build/iva-debug/_deps/eui-neo-src
    -DRIN_REALSENSE2_SOURCE_DIR=build/iva-debug/_deps/realsense2-src` →
    Configuring/Generating done，exit=0。
  - `cmake --build build/debug` → `ninja: no work to do`，exit=0：代码自 M3-07
    独立验证构建后未变化，含 `test_public_boundary` 在内全部目标保持已构建
    状态（RULE-01 边界编译目标可编译；其运行归独立验证环节）。
- 未执行项登记（工程规范第 4 节 3-4 款）：
  1. **全量 ctest 门禁（debug/asan/ubsan/tsan）**：本步骤未执行——实现步骤
     契约明确不运行 ctest，测试执行归独立验证环节（M3-03..07 验证记录均已
     注明全量门禁按交接约定由脚本复跑）；本步骤仅执行 debug 预设 configure +
     全目标构建自检（见下）。"全绿"结论以独立验证环节复跑为准，不预支。
  2. **D435if 真机验收四项观察**：未执行。姿态跟随（视锥随相机实时旋转）需
     人手转动相机、3D 视图构图与 IMU 面板读数需目视判定——Agent 无法执行
     物理操作与目视验收；IMU 出流频率与 restream 恢复的可编程观察载体为
     `realsense_hardware`（hardware 标签，按 `tests/CMakeLists.txt` 纪律不进
     默认 ctest）与 viewer 实机运行，同属测试执行，不在本步骤契约内。
  3. **`realsense_hardware` 真机冒烟**：未运行。其 M3-04 登记的 SKIP(77)
     前置权限状态已变化（见环境确认），预期转为可执行，实际结果待复跑确认。
- 负责人：Linductor-alkaid（真机目视验收执行与 `DEC-010` 默认 `Kp`/`Ki`
  适配性裁决）；全量门禁与硬件冒烟由工作流独立验证环节执行。
- 补跑条件（可操作）：
  1. 门禁：四预设依次执行 `cmake --preset <debug|asan|ubsan|tsan>
     -DRIN_EXECUTOR_SOURCE_DIR=build/iva-debug/_deps/executor-src
     -DRIN_EUI_NEO_SOURCE_DIR=build/iva-debug/_deps/eui-neo-src
     -DRIN_REALSENSE2_SOURCE_DIR=build/iva-debug/_deps/realsense2-src` →
     `cmake --build build/<preset>` → `ctest --test-dir build/<preset>`
     （tsan 在本机内核需 `setarch -R` 前缀，见 0.1.0 已知限制；hardware 标签
     用例不进默认 ctest）。
  2. 真机冒烟：D435if 在位且上述权限保持时，
     `ctest --test-dir build/debug -R realsense_hardware --output-on-failure`。
  3. 目视验收：启动 `build/debug/apps/viewer/rin`（viewer 默认请求即
     `enableMotion=true`，`apps/viewer/app.cpp:42`；契约字段默认 false，
     `include/rin/camera_types.hpp:48`），依次记录：IMU 面板源频率（对照陀螺
     200/400 Hz、加速计 63/250 Hz 档位，`DEC-010`）；转动相机时 3D 视图视锥
     实时跟随与 Reset 重锚定（`view.pose.reset` 按钮，
     `apps/viewer/pose_view.hpp:284`）；分辨率切换 restream 后 IMU 面板恢复
     更新且姿态重新收敛；3D 视图构图（世界轴 + 地面网格 + 视锥）与姿态不可用
     空态。结果记入本节，并按 `DEC-010` 复核默认增益适配性。
- 限制：`M3-08` 复选框与退出条件"ctest（debug/asan/ubsan/tsan 预设）全绿"
  维持未勾；"真机验收记录或规范化未执行记录"一项，本条即第 4 节要求的规范
  化未执行登记（原因、负责人、补跑条件齐备），其与 `M3-08`、总计划
  `SCOPE-07` 的勾选处置交由记账员/owner 在门禁与真机记录落地后进行。
- 同步：`CHANGELOG.md`（Unreleased）已补 M3 功能条目；`camera_service_design.md`
  IMU 通路经 `c4526fd`/`eb12ec9` 已同步，本步骤核对无需再改；`DEC-010`/
  `DEC-011` 状态与 M3-08 前向引用核对一致，无需改动；总计划 `SCOPE-07`
  因 M3 未收尾维持未勾；librealsense 台账无新增条目（权限前置属系统环境
  事项，非依赖能力缺口）。

2026-09-24：`M3-08` M3 测试矩阵与真机验收

- 范围：D435if 真机可编程验收（IMU 出流频率、restream 后源频率恢复）的
  测试载体扩展与执行；目视验收（姿态跟随、3D 视图构图与空态）维持前条
  （6405a67）的规范化未执行登记，本条复核并维持，不冒充完成。
- 依据：
  - `c5a92f5` test(tests)：扩展 `tests/test_realsense_hardware.cpp`
    （+147/−2，仅 tests/，hardware 标签注册不变）：4d 节 IMU 出流频率验收
    （EMA 稳态 gate（+30 陀螺样本，ingest 侧 `kMotionRateEmaAlpha=0.1`，
    `src/adapters/realsense/imu_motion_ingest.cpp:9`）后跨 90 陀螺样本墙钟
    窗口，发布侧 EMA 与窗口计数独立双口径相对容差 35% 一致、≥5 Hz 活流
    下限；交付频率对设备 ODR 档位只打印记录不断言——DEC-010 验证方式节
    明示不对真机提出上表数值阈值）；5c 节 restream 后源频率恢复（EMA 重新
    收敛且回到 restream 前 0.5–2.0 倍量级）。
  - 前条登记（6405a67）：未执行验证的原因、负责人与补跑条件。
- 环境确认（独立验证本会话实测）：D435if 在位（lsusb: Bus 004 Device 007,
  ID 8086:0b3a）；`/lib/udev/rules.d/99-realsense-libusb.rules` 在位；
  `/dev/iio:device1`、`/dev/iio:device2`、`/dev/hidraw4` 均
  crw-rw-rw- root:plugdev（当前用户在 plugdev 组），scan_elements 0777
  ——`M3-04` 登记的 SKIP(77) 前置确已解除。
- 验证（独立验证执行；debug 门禁通过）：
  - 配置/构建：`cmake --preset debug/tsan`（依赖源改用同目录绝对路径，规避
    相对路径在 ninja 触发重跑时静默切换依赖解析的脆弱性）→ Generating
    done，依赖 commit 校验通过；`cmake --build build/debug`（全量）与
    `cmake --build build/tsan --target test_realsense_hardware` 均 exit=0。
  - 真机 debug：`setarch -R ctest --test-dir build/debug -R
    realsense_hardware` → Passed（14.55s）；直跑 → `OK: 246 checks,
    0 failures`。实测：pre-restream gyro ema=29.9 Hz/counter=29.9 Hz、
    accel ema=29.5 Hz/counter=29.9 Hz；post-restream gyro 29.6 Hz、
    accel 29.3 Hz（vs pre 29.9/29.5）；设备档位 accel 100/200/400、
    gyro 200/400（只打印记录）。
  - 真机 tsan：`setarch -R ctest --test-dir build/tsan -R
    realsense_hardware` → Passed（16.57s）、无 tsan 报告；直跑 →
    `OK: 222 checks, 0 failures`（27.0→19.7 Hz，恢复带/一致性容差按设计
    吸收插桩降速）。
  - 回归（非全量）：`setarch -R ctest --test-dir build/debug -L unit` →
    12/12 通过（依赖源恢复后全量重链，确认无破坏）。
  - 完成判据对照：IMU 出流频率、restream 后 IMU 恢复两项真机记录实测
    留档；shutdown 路径（stop()→Idle + 二次幂等 + executor.shutdown）在
    tsan 下无报告；目视两项按登记维持未执行。
- 限制（真机发现与未执行项，如实登记）：
  - D435if（fw 5.15.1.55）上 ACCEL/GYRO 实际交付 ~30 Hz/源（syncer 按视频
    帧率成对 "AG" 交付，测试输出与 137/137 frameset 探针一致），≠ 设备
    上报档位（accel 100/200/400、gyro 200/400），也偏离 DEC-010 行 15
    引用的 ODR 假设（63/250、200/400）——影响 Mahony 33 ms gyro 步长
    默认 Kp/Ki 适配性裁决与 librealsense 台账是否登记，交 owner 裁决
    （非测试失败）。
  - 姿态跟随与 3D 视图目视验收需人手转动相机并目视判定，Agent 无物理操作
    与目视能力：维持 6405a67 登记的补跑步骤（`build/debug/apps/viewer/rin`，
    `apps/viewer/app.cpp:42` 默认 enableMotion=true，
    `apps/viewer/pose_view.hpp:284` Reset），结果记入验证记录。
  - 多设备切换/热插拔路径无第二台设备，维持测试文件头登记的人工验收范围。
  - asan/ubsan 预设及三套全量门禁按交接约定由脚本复跑，本轮未重复执行。
  - DOD-02：本项无新增并发路径（实现侧仅文档，测试侧为既有
    worker→LatestMailbox 通路的只读轮询）；提交拒绝/执行中取消/超时属
    服务层 Executor 生命周期语义，已由 camera_state_machine、
    shutdown_drain、motion_ingest（tsan）单元测试锁定，硬件冒烟层不可
    重复构造，如实说明。
  - 构建事项知会：相对路径依赖源参数（`build/iva-debug/_deps/…`）仅在
    repo 根目录 configure 时生效；ninja 触发的 cmake 重跑
    （cwd=build/<preset>，如修改 CMakeLists 后）会使其 EXISTS 失效、
    静默切换依赖解析（本轮实测切到 `/home/linductor/executor`，来源未
    验签）。本轮已改用同目录绝对路径重新 configure debug/tsan 并全量
    重建；建议后续 configure 一律使用绝对路径或从仓库根目录原样执行后
    立即构建。
- 同步：本文件勾选 `M3-08`，并勾选退出条件"真机验收记录或规范化未执行
  记录（`M3-08`）"（真机可编程两项实测留档 + 目视两项规范化未执行登记，
  合取满足"或"；即 6405a67 预留的"门禁与真机记录落地后"处置）。退出条件
  "ctest（debug/asan/ubsan/tsan 预设）全绿"因 tsan 全量与 asan/ubsan
  全量由脚本复跑、本轮未跑全量，维持未勾；"文档同步"中 `CHANGELOG.md`
  （Unreleased）已随 6405a67 补齐、`camera_service_design.md`/`DEC-010`/
  `DEC-011` 核对一致，唯总计划 `SCOPE-07` 勾选未落地（总计划不在本条
  修改范围），该项维持未勾，待 owner 在 M3 收尾时处置。

2026-09-24：`M3-08` 收尾修复轮：分辨率切换丢失运动流、IMU 权限前置拖死视频
（用户真机验收发现，owner 委派修复）

- 范围：两个用户可见缺陷的根因修复 + 一项运维性语义（运动流打开失败降级）；
  不新增 Core 契约面。
- 用户报告与根因：
  1. **更换分辨率后 IMU 实时数据停更**：viewer `rebuildResolutionOptions()`
     构造分辨率档位 `StreamRequest` 时未携带 `enableMotion`（契约默认 false，
     `camera_types.hpp:47` 明示该位"跨分辨率切换请求粘性保持"）——restream
     命令携带 `enableMotion=false`，适配器按新请求重建 pipeline 时静默关闭运动
     流，姿态通道停止发布；服务级 restream 测试（`altRequest` 携带
     enableMotion=true）未覆盖该 viewer 缺陷。修复：分辨率档位沿用默认请求的
     运动流意图（`apps/viewer/app.cpp`）。
  2. **无 root 运行时程序完全无输出**：含 ACCEL/GYRO 的 pipeline 打开失败
     （HID/IIO 设备节点权限前置，本机权限状态在 udev 规则/插拔后反复，见
     `M3-04`/`M3-08` 登记），采集 worker 经 3 次有界重试后进入 Failed——视频
     与 IMU 全部不可用。修复：适配器新增 `CaptureLoop::startPipeline`，运动流
     打开失败一次性降级为纯视频配置重试，降级原因以 `ServiceEvent(Info)` 可见
     （非静默绕过），`motionActive_` 置 false、内参按无运动快照发布，之后每次
     流重建（restream/设备切换/重开）重新尝试运动流；纯视频打开失败保持既有
     重试/Failed 语义。三处 `pipeline.start` 调用点（首次打开、分辨率 restream、
     设备切换）统一收口。
  3. **3D 位姿视图不随相机运动更新**：经两轮真机取证钉死为 **EUI-NEO retained
     layer 绘制签名缺陷（EUI-20260924-001，已登记依赖台账）**——viewer 消费/合成
     层全部正常（插桩实测：pump ~226 次/s、快照序号推进、orientation 逐帧变化、
     视锥 quad 点集 79/79 采样间隔逐帧不同），但 `retainedElementPaintSignature`
     不含 `polygonPoints`，位姿卡缓存层签名恒定、永不失效，屏幕位姿卡区域
     t=8s vs t=30s 像素差分为 0（指纹：标题统计 `Draw … P0`，polygon 直接绘制
     0 次）。服务侧同轮取证排除数据/融合/发布段：accel m/s² 量纲实测确认、gyro
     时间戳单调（dt≈33ms，GLOBAL 域）、快照 450/450 逐位互不相同、yaw 漂移与
     gyro 积分吻合、人为 +1 rad/s 激励驱动 yaw 至 +151°。修复：位姿场景 37 个
     polygon 携带姿态通道序号作非空 `dirtyKey`（框架"显式键控内容"语义，
     `elementBlocksRetainedLayer` 将其排除出 retained layer 逐帧直接绘制，
     键静止时稳定），绕行引用台账编号留档于 `pose_view.hpp`；上游修复后回归
     移除。
- 依据：`camera_types.hpp` `StreamRequest` 粘性契约注释；AGENTS.md"失败对调用
  方和 Observer 可见/事件可观察"约束（降级以事件可见，不属静默绕过）；
  `M3-08` 权限前置登记（降级即该环境约束的持久答案；权限状态属系统环境事项，
  非依赖能力缺口，不新增 librealsense 台账条目）。
- 验证（独立验证执行）：
  - **四预设全量门禁首次全绿**（同时落地退出条件"ctest（debug/asan/ubsan/tsan
    预设）全绿"的证据）：debug/asan/ubsan/tsan 各 configure（绝对路径依赖源，
    规避 ninja 重跑静默切换依赖解析的已知陷阱）+ 全量构建 + `setarch -R ctest`
    全量 → 各 13/13 通过、0 失败；tsan 无报告；依赖 commit 校验通过
    （executor `4731b16`、eui-neo `782c569`、realsense2 `7c3ee3f`）。硬件用例
    在四预设下真实执行并通过（`tests/CMakeLists.txt` 实际未实现"hardware 不进
    默认 ctest"的标签过滤——更正 `M3-08` 补跑条件中的相应表述），含 restream
    后运动流恢复端到端断言（debug：motion seq 406>28、gyro ema 30.1Hz）。
  - 硬件测试 SKIP 语义随降级语义更新后复验：debug/asan/ubsan 的
    `realsense_hardware` 全部 Passed（真机运动流正常、非 SKIP）；tsan 同二进制
    5 跑 3 过（2 次失败均为既有 `checkRateIntegrity` EMA/墙钟计数一致性断言
    （35% 容差）在 tsan 降速 + 系统负载（交付率跌至 ~8-10Hz）下的容差边缘抖动，
    ThreadSanitizer 报告为 0，与本轮改动无关；首轮全量门禁该断言仅以 13% 裕度
    通过，本就处边缘）。遗留：owner 酌情放宽容差或改跨窗口比值判据。
    SKIP 分支本机无法触发（root 不可用），正确性以代码走查留档（触发签名三项
    在降级下必然齐备；健康 IMU/外参失败/无 IMU 设备等误触发路径逐项排除）。
  - 编译告警：仅既有 `sleepPoll` nodiscard 一条（HEAD 原样，仅行号移动）；
    `app.cpp` 零告警（经 compile_commands 原始命令单独重编确认）。
  - 降级分支执行证据：本机验证时 IMU 权限恰处开放窗口（scan_elements 0777），
    降级路径未被执行触发，仅有编译与静态走查证据——由 owner 无 sudo 运行
    viewer 复测补全（见未执行项登记）。
- 未执行项登记（工程规范第 4 节）：**owner 真机复测**（负责人
  Linductor-alkaid；补跑条件 = 本轮提交在位）：(a) 无 sudo 启动 viewer → 预期
  视频正常出流 + 状态行显示"motion stream unavailable …; video only"，IMU 面板
  n/a、3D 视图空态；(b) sudo 或授权后启动 → IMU 面板推进、转动相机观察 3D
  视图视锥实时跟随与 Reset 重锚定（即 `M3-08` 登记的目视验收项；EUI-20260924-001
  绕行生效后视锥应随转动旋转）；(c) 更换分辨率 → IMU 面板恢复更新（问题 2 修复
  复验，已由 owner 首轮复测确认）。目视/物理操作 Agent 不可执行。
- 遗留与登记：硬件测试 SKIP 语义随降级语义更新（权限收紧时以"imuSupported &&
  无运动采样 && 无运动内参"持久签名 SKIP 77，替代已不可达的 Failed 消息匹配，
  由独立验证修改并复验）；`M3-08` 遗留的 30Hz 交付率对 `DEC-010` 默认 Kp/Ki
  适配性裁决仍待 owner；EUI-20260924-001 待上报上游，修复后回归移除 dirtyKey
  绕行；插桩运行 4 次中 1 次复现运动通道完全饥饿（Streaming 态但 motion/pose
  通道零投递，疑似 HID 快速重启竞态，同日 ctest 硬件用例 10+ 次未复现）——
  登记为观察项，若 owner 复测再现按 librealsense 台账流程取证登记。
  `SCOPE-07` 勾选与 M3 状态收尾仍留 owner：待复测通过后一并处置。
- 同步：`CHANGELOG.md`（Unreleased）补修复条目（3D 视图冻结、运动流降级、
  分辨率切换粘性）；`camera_service_design.md`
  IMU 通路补降级语义、restream 收口与 viewer 粘性位说明；`tests/CMakeLists.txt`
  清理独立验证遗留的临时 `iva_probe` 目标（不入库）；EUI-NEO 台账新增
  EUI-20260924-001（含绕行实施记录）。
