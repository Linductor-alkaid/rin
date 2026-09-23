# 更新日志

本项目的版本遵循语义化版本（工程规范 10.5）；tag 与里程碑"建议发布点"一一对应。

## [Unreleased]

## [0.1.0] - 2026-09-23（M1：viewer 基础能力）

### 新增

- 仓库骨架：AGENTS.md 协作约定、项目管理与工程规范、CMakePresets（debug/release/
  asan/ubsan/tsan）、依赖锁清单 `third_party/dependencies.lock` 与 configure 时 commit
  校验（`cmake/Dependencies.cmake`）。
- `rsv_core`：相机服务契约（`rsv::ICameraService`、`StreamRequest`、`Frame`、
  `IntrinsicsSnapshot`、`StreamCapabilities`、`ServiceEvent`）、显式相机状态机
  （Idle/Opening/Streaming/Restreaming/Stopping/Failed）、RGB8→RGBA8 与 Z16→jet
  RGBA8 像素转换；公开头零第三方类型。
- `rsv_realsense_adapter`：librealsense2 设备枚举/能力/内参读取、双流采集（executor
  blocking worker 承载）、协作取消与关闭收敛、`LatestMailbox` 帧与事件通道、分辨率
  切换（pipeline 重建）。
- `rsv_viewer`：EUI-NEO 实时预览应用——RGB/深度双画面（`ImageStream`）、设备能力驱动
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
