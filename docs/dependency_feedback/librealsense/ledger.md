# librealsense2 依赖台账

> 状态：Active
> 依赖：librealsense2（pinned 版本 2.58.3，源 commit `7c3ee3fb7c640e9f315e663907208cb56c4febfd`，
> https://github.com/realsenseai/librealsense ，Apache-2.0）
> 记录纪律：工程规范第 9.4 节（台账部分由 AGENTS.md“依赖独立台账”节扩展到所有依赖）。

## 依赖身份

- 来源：https://github.com/realsenseai/librealsense
- 锁定：`third_party/dependencies.lock` 行 `realsense2|...|7c3ee3fb7...|7c3ee3fb7...|OFF|external`
  （2026-09-23 起由 system 改为 external，见 [DEC-009](../../decisions/DEC-009-self-contained-deb-distribution.md)）
- 许可证：Apache-2.0（上游 LICENSE）
- 类别：external —— FetchContent 按 pinned commit 源码构建（运行库随 deb 分发；
  见 `cmake/Dependencies.cmake` 的裁剪选项与 C++ 标准隔离）。此前为 system 类
  （本机 `/usr/local` 安装 v2.58.3-6-g7c3ee3fb7）。
- 验证设备：Intel RealSense Depth Camera D435if（USB ID `8086:0b3a`，序列号
  261922074392，固件 5.15.1.55）；2026-09-23 经适配器实机验收：枚举 32+32 档位、双流
  640x480/848x480、内参读取、分辨率 restream 切换（1018ms）全部通过（M1 验证记录）。

## 集成决策

- 仅链接 `realsense2::realsense2`，不使用 GL 变体（渲染由 EUI-NEO 负责）。
- 不使用 `rs2::colorizer`，深度伪彩在 adapter 内自研（[DEC-003](../../decisions/DEC-003-depth-colormap.md)）。
- 阻塞采集 `wait_for_frames(timeout)` 固定超时（1s）以闭合取消路径（设计文档“并发模型”）。
- 公开契约不含 `rs2::` 类型；`rs2::` 只出现在 `src/adapters/realsense/`。
- 源码构建裁剪（2026-09-23，随 DEC-009）：`BUILD_EXAMPLES/BUILD_GRAPHICAL_EXAMPLES/
  BUILD_GLSL_EXTENSIONS/BUILD_TOOLS/BUILD_UNIT_TESTS/BUILD_PYTHON_BINDINGS/BUILD_ROSBAG2/
  BUILD_RS2_ALL/BUILD_WITH_DDS/CHECK_FOR_UPDATES` 全部 OFF，仅保留运行时库本体；
  上游 `cmake_minimum_required(3.10)` 触发 CMP0077 OLD，裁剪必须写 cache FORCE 并在
  配置后恢复，防止污染 executor / eui-neo 构建面。

## 条目

### LRS-20260923-001：无源码级引入路径，换机构建依赖系统预装

- 级别：P3（有而未用：可复现性增强项，非缺口）
- 现象/证据：`CMakeLists` 采用 system 类依赖；在没有 librealsense2 2.58.3 系统安装的
  机器上 configure 失败（`find_package` 报错）。
- 影响：跨机构建需先按上游文档安装 librealsense2；CI 镜像需固化安装步骤。
- 期望语义：`-DRIN_FETCH_LIBREALSENSE=ON` 时 FetchContent 源码构建并安装至构建树。
- 建议最小能力：`cmake/Dependencies.cmake` 增加 librealsense external 分支
  （`BUILD_EXAMPLES=OFF/BUILD_UTILS=OFF/GLFW_DIR=...`）。
- 状态：Resolved（2026-09-23：锁定行直接改为 external 并随 [DEC-009](../../decisions/DEC-009-self-contained-deb-distribution.md)
  落地 FetchContent 源码构建，超出暂定开关方案的预期）
- 跟进：2026-09-23 登记；2026-09-23 关闭。

### LRS-20260923-002：源码构建的两个构建面事实（C++20 不兼容 + json 拉取不 pin commit）

- 级别：P3（集成绕行登记，非缺口）
- 现象/证据（pinned `7c3ee3fb7...`，GCC 13.3，CMake 3.28）：
  1. `third-party/rsutils/include/rsutils/concurrency/concurrency.h:203` 使用类内构造函数
     冗余模板实参 `single_consumer_frame_queue< T >(unsigned ...)`；C++17 合法，C++20
     （GCC13 `-std=c++20`）报 `expected unqualified-id before 'unsigned'`。本项目全局
     `CMAKE_CXX_STANDARD 20` 会在源码构建时传导给 rsutils 触发。
  2. 上游无条件经 `CMake/external_json.cmake` 在 configure 期以**分支 tag**
     （`git clone --branch v3.12.0`）拉取 nlohmann/json：不提供系统库绕过选项，也不受
     本仓库锁清单的 commit 校验覆盖。
- 影响：(1) 源码构建必须以 C++17 编译 librealsense 域；(2) configure 依赖一次额外的
  GitHub 网络拉取，弱网环境可能失败需重试。
- 绕行方案（单一边界内）：`cmake/Dependencies.cmake` 在 realsense2 的
  `FetchContent_MakeAvailable` 前后局部切换 `CMAKE_CXX_STANDARD`（17→20），不修改
  pinned 源码；json 拉取接受上游行为，无法在不改 pinned 源码的前提下 commit-pin。
- 移除条件：(1) 上游修复 rsutils 头或本项目放弃全局 C++20 传导；(2) 上游提供 json
  系统库/外部路径选项时评估接入。
- 状态：Accepted（绕行已实施并在干净构建目录验证）
- 跟进：2026-09-23 登记。

### LRS-20261007-001：宿主内核 hid-sensor-hub/IIO 无 IMU 数据，启用运动流后合成帧同步器饿死整条 pipeline

- 级别：P2（宿主环境缺口：真机运动流与默认启动请求在本机不可用，视频面可绕行）
- 现象/证据（D435if，固件 5.15.1.55，内核 7.0.0-34-generic，pinned librealsense
  2.58.3，独立验证代理受控实验 + 主循环复跑）：
  1. 独立探针（原始 librealsense、逐字复刻 adapter `buildConfig`）：启用
     ACCEL/GYRO 时 `wait_for_frames` 持续超时（10 s 内 color=0 depth=0
     accel=0 gyro=0），但 RS2 DEBUG 日志显示传感器层 Depth/Color 帧 ~30fps
     正常到达（`FrameAccepted`）；关闭运动流（仅 RGB8+Z16）8 s 得 223/223 帧
     （≈27.9 fps）——即视频链健康，运动流使能时合成帧同步器因等不到运动帧
     永不产出 frameset，整条 pipeline（含视频）被饿死。
  2. 环境层直接证据：`/sys/bus/iio/devices/iio:device1/in_accel_x_raw` 恒 0
     （静置应有重力分量）；每次打开设备内核日志出现
     `hid-sensor-hub ... No report with id 0xffffffff found`；librealsense 日志
     `backend-hid.cpp:641 iio_hid_sensor: Frames didn't arrived within the
     predefined interval`。
  3. 权限/枚举无问题：`/dev/iio:device1/2`、`/dev/hidraw4` 均 0666（librealsense
     udev 规则已装且覆盖 8086:0b3a），USB SuperSpeed——`test_realsense_hardware`
     的两个 SKIP 判据（无设备、scan_element 权限）都不命中，测试按设计 FAIL
     （五预设一致，117 检查 27 失败，全部为数据面级联：首帧 5 s 超时起）。
  4. 历史对照：该测试本机此前从未真机执行过（M3 记录为 udev 前置 SKIP），
     本次为首次暴露，非 M11 回归（M11 前后行为一致）。
- 影响范围：本宿主机上默认 `enableMotion=true` 的启动请求（viewer 默认与
  硬件测试）得不到任何视频/深度帧；M11 真机冒烟第一轮因此"引擎 Running
  而零帧"（截图复核实证 fps 0.0/processed 0），以 `enableMotion=false`
  视频-only 请求复跑后 RGB+深度 29.1 fps 正常（截图 `screenshots/m11/
  m11_realcam_*.png`）。运动流真机验收（M3-08 IMU 频率/位姿/restream 恢复）
  在本机被阻断。
- 期望语义：运动流传感器打开成功但持续零样本时，`wait_for_frames` 不应
  连带饿死视频流（合成帧不应硬性等待运动帧）。
- 建议的最小能力/绕行：
  - 环境侧（首选）：更换 6.x 内核或修复 hid-sensor-hub/IIO 后复跑
    （`test_realsense_hardware` 五预设 + viewer 默认请求冒烟）；物理断电重插
    相机亦可尝试。
  - 应用侧（如判定本环境为长期形态，需 owner 裁定）：将"运动流打开成功但
    长期零样本"纳入适配器降级策略（现 M3-04 降级仅覆盖打开失败）或测试
    SKIP 判据——属验收契约变更，未实施。
- 延期影响：M3-08 运动流真机验收项在本机保持"未验证"；M11 真机冒烟以
  视频-only 请求完成（覆盖 M11 全部数据面语义，运动流与 M11 无关）。
- 可验收结果：视频-only 请求下双流 30 fps 端到端（29.1 fps 实测）、监看器
  实时预览随 seq 推进、参数下一帧生效（424x240→212x120）、停止排空；运动
  流修复后 `test_realsense_hardware` 全绿 + 默认请求复跑。
- 状态：Resolved（2026-10-08 owner 裁定"不复现则关闭"，当日取证轮未复现：
  内核 7.0.0-34-generic 不变（与 10-07 早间饿死轮同版本，排除内核版本因素），
  IIO accel 三轴实时读数含重力分量且逐秒变化（x≈2-7、y≈-940、z≈306-312，
  1s 间隔三次采样）；`realsense_hardware`（enableMotion=true 全链路，含 M3-08
  IMU 频率验收与含运动流 restream）连续 5 次通过（13.2s/次，debug 预设），
  加当日早间 M13 真机验收轮共 6+ 次开流零饿死。内核签名
  `hid-sensor-hub ... No report with id 0xffffffff` 仍在设备枚举时出现
  （journalctl 08:55:50）但数据面健康——该签名不具判定性。缺陷定性：间歇性
  宿主环境缺陷（10-07 早间两轮确定性饿死；10-07 晚间、10-08 未复现），触发
  条件未定位。重开条件：`realsense_hardware` 首帧超时型 FAIL，伴 IIO 读数
  恒零与 librealsense `backend-hid ... Frames didn't arrived` 日志。应用侧
  绕行保留：`RIN_DISABLE_MOTION=1`（viewer 会话级逃生口，零成本，不随本条
  关闭移除）；`test_realsense_fps_hardware` 为视频-only，与本条解耦。运动流
  真机验收（M3-08）阻断解除，其自动化载体已全绿）
- 跟进：2026-10-07 登记（Independent-Verification-Agent 受控实验证据链 +
  主循环复跑实证）；2026-10-07 增加应用侧最小绕行：viewer 启动请求支持
  `RIN_DISABLE_MOTION=1` 环境变量跳过运动流（用户手动测试/无 IMU 场景，
  随 M11 分支提交；运动流自动降级策略仍待 owner 裁定）；同日修复该绕行的
  第二入口泄漏——分辨率档位的 enableMotion 粘性恒取默认请求 true，restream
  命令会在禁用会话中重新带回运动流（用户验收发现"切换分辨率后输入停更"，
  根因即本条目饿死机理），现已将环境变量升级为会话级标志（启动请求与档位
  粘性同源，`ViewerContext::motionDisabledByEnv`）；2026-10-07 晚间复验轮
  IIO 通路恢复健康（五预设通过，见状态栏历史记录）；2026-10-08 owner 授权
  审查（M13 收尾），当日取证轮未复现，按裁定转 Resolved（取证数据见状态栏）。

### LRS-20261007-002：相机被占用时的打开失败拆除路径存在第三方数据竞争（tsan）

- 级别：P3（依赖缺陷候选：仅失败路径、仅相机被并发占用时触发，成功路径与
  空闲相机路径无报告）
- 现象/证据（Independent-Verification-Agent，真相机在位、tsan 预设）：
  相机被另一进程持有时 `CaptureLoop::startPipeline` 打开失败（errno=16
  VIDIOC busy）进入 config→pipeline→locked_transfer 拆除，librealsense2
  2.58（pinned 7c3ee3fb）内部 `librealsense::small_heap<int,256>::
  ~small_heap()`（pthread_cond_destroy，拆除线程）与 dispatcher 线程
  `small_heap::deallocate`（pthread_cond_signal，持 M0 锁）对同一堆块
  竞态，tsan 报告数据竞争。调用链位于
  src/adapters/realsense/realsense_camera_service.cpp:966（ Rin 侧仅为
  调用者）。
- 影响范围：失败拆除路径的潜在 UB（依赖内部），无 Rin 侧可观察行为异常；
  相机空闲时全部预设（含 tsan 真机）干净通过。
- 期望语义：打开失败拆除时 dispatcher 线程与堆析构有序（上游修复）。
- 建议的最小能力/绕行：无 Rin 侧绕行（不修改 pinned 源）；避免并发占用
  相机为操作纪律（本仓库验证流程已按占用重试规则处理）。
- 状态：Accepted（依赖缺陷候选登记；随上游升级复验）
- 跟进：2026-10-07 登记（独立验证 tsan 证据）。

## 跟进记录表

| 编号 | 状态 | 优先级 | 跟进 |
| --- | --- | --- | --- |
| LRS-20260923-001 | Resolved | P3 | 2026-09-23 登记；同日随 DEC-009 改 external 关闭 |
| LRS-20260923-002 | Accepted | P3 | 2026-09-23 登记；C++17 域隔离绕行已实施 |
