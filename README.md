# Rin

Rin 是一个基于 Intel RealSense 深度相机的实时预览应用：连接与主机相连的 RealSense
深度相机，开窗实时显示 RGB 彩色图像与深度图像，支持选择输出分辨率、切换深度配色
（jet 伪彩 / 灰度黑白），并显示当前流配置下的相机内参。

核心闭环：`枚举并打开 RealSense 设备 → 按所选分辨率启动双流 → Executor 承载的采集
循环持续发布帧 → EUI-NEO 窗口实时渲染 RGB/深度并显示内参`。

## 安装（Linux，deb）

从 [Releases](https://github.com/Linductor-alkaid/rin/releases) 下载 `rin_<版本>_amd64.deb`
（CI 在每个 `v*` tag 上自动产出）：

```bash
sudo apt install ./rin_<版本>_amd64.deb
rin
```

deb 自包含：捆绑 pinned 版本的 librealsense2 运行库（`/usr/lib/rin`）与 RealSense
udev 规则（安装后免 root 访问设备），无需预装 Intel RealSense SDK。插上相机即可使用。
仅在无设备时进入 `Waiting` 稳态，接入后自动出流。

支持的系统：Ubuntu 24.04（amd64）及以上依赖兼容的发行版。

## 从源码构建

依赖：CMake ≥ 3.25、Ninja、GCC/Clang（C++20）、libusb-1.0 与 libudev 开发包、
OpenGL/X11 开发包。librealsense2、Executor、EUI-NEO 均按 pinned commit 由 CMake
拉取源码构建，无需预装。

```bash
cmake --preset debug          # 或 release / asan / ubsan / tsan
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

构建 deb 安装包：

```bash
cmake --preset release
cmake --build build/release
cmake --build build/release --target package    # 产物位于 build/release/dist/
```

预设选项：`RIN_BUILD_TESTS`（默认 ON）、`RIN_BUILD_VIEWER`（默认 ON）、
`RIN_ENABLE_ASAN/UBSAN/TSAN`（sanitizer 预设自带）。

## 架构

- `src/core`（`rin_core`）：相机服务契约（`rin` 命名空间下的
  `ICameraService` 等）、显式相机状态机、像素转换；零第三方依赖。
- `src/adapters/realsense`（`rin_realsense_adapter`）：librealsense2 设备适配，
  阻塞采集由 Executor blocking worker 承载，`executor::comm` 邮箱传递帧与事件。
- `apps/viewer`（`rin`）：EUI-NEO 前端应用，Executor 生命周期 owner。

公开头文件不包含任何第三方类型；依赖方向指向抽象。详见
[工程规范](docs/project/project-standards.md)、
[实施总计划](docs/plans/rin-implementation-plan.md)与
[决策记录](docs/decisions/)。

## 协作约定

代码、测试、文档与提交须遵循仓库根 [AGENTS.md](AGENTS.md) 与工程规范；第三方依赖
变更须登记 `third_party/dependencies.lock` 与对应反馈台账。
