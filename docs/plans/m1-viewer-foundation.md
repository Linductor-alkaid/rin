# M1：viewer 基础能力

> 状态：Completed（tsan 有一项环境限制记录，见验证记录）
> 负责人：Linductor-alkaid
> 所属计划：[realsense-vision 实施总计划](realsense-vision-implementation-plan.md)
> 前置：无
> 建议发布点：v0.1.0
> 更新日期：2026-09-23

## 目标

交付一个可运行的 RealSense 实时预览闭环：连接主机上的 RealSense 深度相机，在 EUI-NEO
窗口中实时显示 RGB 与深度画面，允许在设备支持的分辨率档位间切换，并显示当前流配置下
的彩色/深度相机内参。全部并发与生命周期由 pinned executor 承载。

## 范围与非目标

范围：`SCOPE-02`、`SCOPE-03`、`SCOPE-04`（见总计划）；单设备、单窗口、RGB+深度双流。

非目标：点云、对齐、录制回放、多设备、网络传输（见总计划 `POST-NN`）。

## 设计与决策依据

- [camera_service_design.md](../design/camera_service_design.md)
- [DEC-001](../decisions/DEC-001-dependency-pinning.md)、
  [DEC-002](../decisions/DEC-002-runtime-ui-integration.md)、
  [DEC-003](../decisions/DEC-003-depth-colormap.md)

## 工作项

- [x] `M1-01` Core 公共类型与 `ICameraService` 契约：`Frame`/`StreamRequest`/
      `IntrinsicsSnapshot`/`ServiceEvent`/状态集，公开头零第三方类型。
- [x] `M1-02` `CameraServiceStateMachine`：显式状态集与转换规则，非法转换拒绝并以
      `ServiceEvent` 报告；终态幂等。
- [x] `M1-03` `RealSenseCamera` 适配器：设备枚举/打开失败路径、双流配置、blocking
      worker 采集循环、协作取消、`LatestMailbox` 帧与事件发布、分辨率切换
      （Restreaming：停 pipeline → 以新配置重建 → 恢复）。
- [x] `M1-04` 像素转换：RGB8→RGBA8（行对齐 stride）、Z16→RGBA8 jet 伪彩（含 0/无效
      深度处理），纯函数、可单测。
- [x] `M1-05` viewer 应用：`AppRuntime`（executor owner）、双 `ImageStream` 面板、分辨率
      下拉（设备能力驱动）、内参文本面板、`onShutdown` 关闭顺序。
- [x] `M1-06` 单元测试：状态机全转换矩阵、转换函数、命令/帧邮箱有界性（假设备驱动
      Core，不依赖真实相机）。
- [x] `M1-07` 构建：debug/asan/ubsan 预设通过；公开头边界编译测试通过。
- [x] `M1-08` 真机验收：D435if 上运行 viewer，验证画面、分辨率切换、内参显示与关闭
      路径，截图与结论登记于本文件验证记录。

## 风险与阻塞

- EUI-NEO 与 executor 均 pinned 于滚动 master 附近 commit，API 可能演进；锁定 commit
  已在 `third_party/dependencies.lock` 固化，升级走独立变更。
- Wayland 会话下 GLFW 经 XWayland 运行，若真机验收受阻按工程规范第 4 节记录并给出
  补跑条件。（实际已通过 XWayland 完成验收，无阻塞。）
- librealsense 以系统安装形态使用（system 类依赖），换机构建需重装 2.58.3；已记录于
  [librealsense 台账](../dependency_feedback/librealsense/ledger.md)。

## 测试与退出条件

- [x] `ctest` 全部通过（debug/asan/ubsan），覆盖状态机、转换、邮箱边界。
- [x] 公开头第三方类型边界测试编译通过。
- [x] 真机 D435if：RGB 与深度画面实时刷新；分辨率下拉切换生效（画面与内参同步更新）；
      内参面板显示 fx/fy/cx/cy 与尺寸；窗口关闭进程干净退出（无崩溃、无悬挂线程报告）。
- [x] 验证记录含 commit、命令、环境与结果。

## 验证记录

### 2026-09-23：M1-01..M1-08 全部完成，M1 退出条件通过

- 范围：M1 全部工作项（骨架、契约、状态机、适配器、viewer、测试、真机验收）。
- 依据：[camera_service_design.md](../design/camera_service_design.md)、DEC-001/002/003。
- 验证（自动化，由 Independent-Verification-Agent 独立执行并出具报告）：
  - 环境：Ubuntu 24.04（内核 7.0.0-31-generic，x86_64），gcc 13.3.0，CMake 3.28.3/Ninja，
    librealsense2 2.58.3（系统），设备 RealSense D435IF（序列号 261922074392，固件
    5.15.1.55）。
  - debug/asan/ubsan 三个预设：configure/build/ctest 全绿，`100% tests passed,
    0 tests failed out of 4`（camera_state_machine 238 checks、pixel_format 274 checks、
    public_boundary 2 checks、realsense_hardware 41 checks，三预设计数一致）。
  - realsense_hardware（真机，标签 `hardware`）：Started 216ms；首帧 RGB/Depth 817ms；
    内参/能力快照校验通过；分辨率 640x480→848x480 切换（含 restream）1018ms 并收到
    匹配新分辨率帧；stop() 干净收敛 Idle；重复运行 4 次无抖动。
  - tsan 限制：内核高熵 ASLR 与 TSAN 固定影子内存冲突（`unexpected memory mapping`，
    无线程用例同样触发，属环境限制）；经 `setarch -R` 旁路验证 4/4 通过、零竞态报告。
    补跑条件：宿主机调整 `vm.mmap_rnd_bits` 或为测试包 setarch 启动器。
- 验证（GUI 真机验收，主循环执行）：
  - 命令：`cmake --build --preset debug` 后 `DISPLAY=:0 ./build/debug/apps/viewer/rsv_viewer`。
  - 窗口以 59-60 FPS 渲染；RGB/Depth 双画面实时更新（间隔 2s 双截图 xwd 哈希不同：
    `8ad81114…` vs `5d280c16…`）。
  - 分辨率切换经真实 UI 点击完成：下拉 848x480 → 640x360，状态栏 `Streaming -
    resolution applied`，内参同步更新（Color fx 603.298→452.474，Depth fx 423.990→
    319.993，均含畸变模型标注）。
  - WM 关窗（`_NET_CLOSE_WINDOW`）→ 进程干净退出，运行日志为空（onShutdown 路径：
    停服务 → worker 回收 → executor.shutdown(true)）。
  - 证据截图（本地留档，md5）：`viewer_fixed_layout.png`
    `c0f3d6aad1988f87b2e9d8ceb1e256e1`（848x480 全面板+内参）、
    `viewer_resolution_popup.png` `7f65b6bb313e5c4f93df648556478d66`、
    `viewer_640x360_applied.png` `61c18c36ad5638d900223bc1e9d5b403`（切换后）。
- 限制：
  - 验收场景为暗室，画面内容以噪点/亮条为主，仅用于证明链路与实时性，不代表成像
    质量验收。
  - EUI-NEO 弹层展开状态需 `bindOpen` 保留信号驱动（见 EUI-20260923-002）；首版
    viewer 曾缺失该接线，已在验收中发现并修复。
- 同步：总计划 SCOPE-02..05 勾选、依赖台账跟进记录、CHANGELOG、本文件状态。
