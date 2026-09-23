# 更新日志

本项目的版本遵循语义化版本（工程规范 10.5）；tag 与里程碑"建议发布点"一一对应。

## [Unreleased]

- （暂无）

## [0.2.0] - 2026-09-24（M2：Rin 更名与 Linux 自包含分发）

### 新增

- Linux 自包含 deb 分发（DEC-009）：`cpack -G DEB` 产出 `rin_<版本>_amd64.deb`，
  含 `/usr/bin/rin`、捆绑的 librealsense2 运行库（`/usr/lib/rin`，RUNPATH 定位）、
  RealSense udev 规则（安装后免 root 使用设备）、桌面入口与 hicolor 图标；deb
  依赖由 `dpkg-shlibdeps` 自动生成。安装规则组件化（`rin` 组件），依赖的 install
  规则不进入分发包。
- GitHub Actions CI（M2-05）：push/PR 构建并运行测试、构建 Release 并导出 deb
  工件；`v*` tag 触发将 deb 附着到 GitHub Release。
- 深度图配色运行时可选（DEC-007）：viewer 控制行新增 Palette 下拉，可在 jet 伪彩
  与灰度黑白（近白远黑）间即时切换；切换仅影响后续帧转换、不重流；`Waiting` 态
  可预设，接入后按所选配色出流。公共契约新增 `DepthColorScheme` 与
  `ICameraService::requestDepthColorScheme`（等待/出流状态有效、粘性，终态拒绝）。
- 相机热插拔与多设备选择（DEC-006）：启动不依赖相机连接（`Waiting` 设计稳态）；
  运行中经 `rs2::context` 设备变化回调自动感知插拔并刷新在线设备目录；唯一设备
  自动选中，多设备时由用户在 Device 下拉中选择（`requestDevice`，选择意图粘性
  跨插拔保留）；活动设备被移除自动回到等待态并可在重插后恢复出流。
- 键盘 1-9 直选分辨率档位。

### 变更

- 项目更名为 **Rin**（DEC-008）：C++ 命名空间 `rsv`→`rin`、公开头目录
  `include/rs_vision/`→`include/rin/`、构建目标 `rin_core`/`rin_realsense_adapter`/
  `rin`、CMake 选项前缀 `RIN_`、窗口标题 "Rin"；设备与 SDK 相关标识保留
  "realsense"。仓库远端 `Linductor-alkaid/rin`。
- librealsense2 由 system 类依赖改为 external 源码构建（DEC-009，pinned commit
  `7c3ee3fb7...` 即上游 2.58.3 不变）：不再要求系统预装；构建面裁剪为仅运行时库，
  并在源码构建域内以 C++17 编译（rsutils 头与 GCC13 C++20 不兼容，
  LRS-20260923-002）。
- viewer 视觉层按 ZCode Design System 令牌翻译重构（DEC-005）：语义色/字阶/圆角
  层级/间距节奏集中于 `apps/viewer/viewer_theme.hpp`；画面卡化、状态语义色、
  下拉令牌化样式；控制行上移为工具栏位，下拉弹层经根 stack 末位合成浮于卡片之上。
- 新增键盘切换分辨率（数字键 1-9 直选档位）。

### 修复

- viewer 视频画面仅左缘更新、其余全黑（EUI-20260923-003）：绕行 EUI-NEO ImageStream
  动态纹理上传缺陷（官方 dynamic_texture 示例同环境可复现），改经外部 GPU 图像接口
  （`GpuFrameView`：UI 线程自有 GL 纹理上传 + `importGpuImage` 导入）。真机复验
  完整画面输出正常。

## [0.1.0] - 2026-09-23（M1：viewer 基础能力）

### 新增

- 仓库骨架：AGENTS.md 协作约定、项目管理与工程规范、CMakePresets（debug/release/
  asan/ubsan/tsan）、依赖锁清单 `third_party/dependencies.lock` 与 configure 时 commit
  校验（`cmake/Dependencies.cmake`）。
- `rin_core`：相机服务契约（`rin::ICameraService`、`StreamRequest`、`Frame`、
  `IntrinsicsSnapshot`、`StreamCapabilities`、`ServiceEvent`）、显式相机状态机
  （Idle/Opening/Streaming/Restreaming/Stopping/Failed）、RGB8→RGBA8 与 Z16→jet
  RGBA8 像素转换；公开头零第三方类型。
- `rin_realsense_adapter`：librealsense2 设备枚举/能力/内参读取、双流采集（executor
  blocking worker 承载）、协作取消与关闭收敛、`LatestMailbox` 帧与事件通道、分辨率
  切换（pipeline 重建）。
- `rin`：EUI-NEO 实时预览应用——RGB/深度双画面（`ImageStream`）、设备能力驱动
  的分辨率下拉、彩色/深度内参面板；Executor owner 挂 `dslAppConfig()` 首调点，
  关闭经 `DslAppConfig::onShutdown` 闭合。
- 测试：状态机全矩阵（238 checks）、像素转换（274 checks）、公开头边界、真机
  hardware 冒烟（41 checks，标签 `hardware`，无设备时 SKIP）。

### 依赖

- executor pinned `4731b16493ae996311a9e85f55bd137aee69418a`（external）
- eui-neo pinned `782c56993dc1890e0589e2100cfa74322bb0e0bf`（external）
- realsense2 系统 2.58.3（system；源 commit `7c3ee3fb7c640e9f315e663907208cb56c4febfd`）
- 各依赖独立反馈台账见 `docs/dependency_feedback/`（executor 在
  `docs/executor_feedback/ledger.md`）。

### 已知限制

- tsan 预设在本机内核（7.0.0-31，高熵 ASLR）下需 `setarch -R` 旁路运行。
- librealsense2 为系统依赖（system 类），无系统安装的机器 configure 失败
  （LRS-20260923-001）。
