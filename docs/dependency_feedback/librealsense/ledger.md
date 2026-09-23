# librealsense2 依赖台账

> 状态：Active
> 依赖：librealsense2（pinned 版本 2.58.3，源 commit `7c3ee3fb7c640e9f315e663907208cb56c4febfd`，
> https://github.com/realsenseai/librealsense ，Apache-2.0）
> 记录纪律：工程规范第 9.4 节（台账部分由 AGENTS.md“依赖独立台账”节扩展到所有依赖）。

## 依赖身份

- 来源：https://github.com/realsenseai/librealsense
- 锁定：`third_party/dependencies.lock` 行 `librealsense2|...|7c3ee3fb7...|2.58.3|OFF|system`
- 许可证：Apache-2.0（上游 LICENSE）
- 类别：system —— `find_package(realsense2 2.58.3 REQUIRED)`；本机 `/usr/local` 安装
  （头文件 + CMake config + `librealsense2.so.2.58.3`），由源码树同 commit 构建
  （`~/librealsense`，v2.58.3-6-g7c3ee3fb7）。
- 验证设备：Intel RealSense Depth Camera D435if（USB ID `8086:0b3a`，序列号
  261922074392，固件 5.15.1.55）；2026-09-23 经适配器实机验收：枚举 32+32 档位、双流
  640x480/848x480、内参读取、分辨率 restream 切换（1018ms）全部通过（M1 验证记录）。

## 集成决策

- 仅链接 `realsense2::realsense2`，不使用 GL 变体（渲染由 EUI-NEO 负责）。
- 不使用 `rs2::colorizer`，深度伪彩在 adapter 内自研（[DEC-003](../../decisions/DEC-003-depth-colormap.md)）。
- 阻塞采集 `wait_for_frames(timeout)` 固定超时（1s）以闭合取消路径（设计文档“并发模型”）。
- 公开契约不含 `rs2::` 类型；`rs2::` 只出现在 `src/adapters/realsense/`。

## 条目

### LRS-20260923-001：无源码级引入路径，换机构建依赖系统预装

- 级别：P3（有而未用：可复现性增强项，非缺口）
- 现象/证据：`CMakeLists` 采用 system 类依赖；在没有 librealsense2 2.58.3 系统安装的
  机器上 configure 失败（`find_package` 报错）。
- 影响：跨机构建需先按上游文档安装 librealsense2；CI 镜像需固化安装步骤。
- 期望语义：`-DRSV_FETCH_LIBREALSENSE=ON` 时 FetchContent 源码构建并安装至构建树。
- 建议最小能力：`cmake/Dependencies.cmake` 增加 librealsense external 分支
  （`BUILD_EXAMPLES=OFF/BUILD_UTILS=OFF/GLFW_DIR=...`）。
- 状态：Accepted（作为 POST 项；触发条件见总计划 `POST` 节与 DEC-001 备选方案）
- 跟进：2026-09-23 登记。

## 跟进记录表

| 编号 | 状态 | 优先级 | 跟进 |
| --- | --- | --- | --- |
| LRS-20260923-001 | Accepted | P3 | 2026-09-23 登记，挂 POST |
