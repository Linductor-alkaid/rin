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

## 跟进记录表

| 编号 | 状态 | 优先级 | 跟进 |
| --- | --- | --- | --- |
| LRS-20260923-001 | Resolved | P3 | 2026-09-23 登记；同日随 DEC-009 改 external 关闭 |
| LRS-20260923-002 | Accepted | P3 | 2026-09-23 登记；C++17 域隔离绕行已实施 |
