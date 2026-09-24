# EUI-NEO 依赖台账

> 状态：Active
> 依赖：EUI-NEO（pinned `782c56993dc1890e0589e2100cfa74322bb0e0bf`，
> https://github.com/sudoevolve/EUI-NEO ，Apache-2.0）
> 记录纪律：工程规范第 9.4 节（台账部分由 AGENTS.md“依赖独立台账”节扩展到所有依赖）。

## 依赖身份

- 来源：https://github.com/sudoevolve/EUI-NEO（备用镜像 AtomGit）
- 锁定：`third_party/dependencies.lock` 行 `eui-neo|...|782c5699...|...|OFF|external`
- 许可证：Apache-2.0（上游 LICENSE）
- 类别：external（FetchContent 源码引入 / 本地 pinned clone）
- 集成方式：`add_subdirectory` → `eui::neo`；应用目标经 `eui_neo_configure_app()`
  获得入口与资源部署（[DEC-002](../../decisions/DEC-002-runtime-ui-integration.md)）。
- 后端：GLFW + OpenGL（默认；GLFW/glad/FreeType 等由上游 3rd/ 提供）。

## 集成决策

- 帧显示使用 `eui::ImageStream`（容量 2 有界最新帧邮箱）+ `ui.image(...).stream(...)`；
  提交发生在 UI compose 阶段（数据来自 executor `LatestMailbox` 快照），生产语义与
  上游文档“任意生产线程 submit”一致。
- 不使用 `app::async`（框架线程池）——与 AGENTS.md“所有异步经 Executor”冲突；上游
  框架内部自用不受限。
- 生命周期挂点：`dslAppConfig()` 首调（惰性启动）/ `onShutdown`（GPU 销毁前关闭）。

## 条目

### EUI-20260923-001：框架提供 main，缺少显式应用初始化钩子

- 级别：P3（有而未用：观察项，非缺口）
- 现象/证据：`core/app/glfw_app_main.cpp` 的 `eui_app_run()` 无应用注册式 init 回调；
  生命周期入口只有 `dslAppConfig()`（被 `initialize/render` 周期调用）与
  `onShutdown`（`include/eui/detail/dsl_app_impl.h:366`，先于 runtime/网络清理）。
- 影响：Executor owner 初始化只能挂在 `dslAppConfig()` 首调点（函数局部 static 保证
  一次性和主线程语义）；若上游改变调用时机需复核。
- 期望语义：`DslAppConfig` 增加 `onReady`/`onStart` 一次性回调。
- 建议最小能力：上游 config 增加 `onStart(std::function<void()>)`，在窗口创建后、
  首帧 compose 前调用一次。
- 状态：Reported（上游 issue sudoevolve/EUI-NEO#73；不影响当前集成正确性，幂等设计已覆盖）
- 跟进：2026-09-23 登记；2026-09-23 提交上游 issue #73。

### EUI-20260923-002：`components::dropdown` 弹层展开必须经 `bindOpen`/`onOpenChange` 外接状态

- 级别：P3（使用方式观察项，非缺口）
- 现象/证据：`components/dropdown.h` 的 `build()` 中弹层可见性与命中开关均读取构建期
  常量 `open_`；字段 `onClick` 仅调用可选的 `onOpenChange_` 回调。未接 `bindOpen`/
  `onOpenChange` 时点击字段无任何效果（viewer 首版真机点击无响应，截图证据）。
- 影响：应用必须为每个下拉保留 `eui::Signal<bool>` 开合状态并在选中后自行关闭弹层。
- 期望语义：未外接开合状态时框架内部保留默认开合行为。
- 建议最小能力：`build()` 在未注册 `onOpenChange_` 时使用内部 retained 开合状态。
- 状态：Reported（上游 issue sudoevolve/EUI-NEO#72；viewer 已按现语义接线：
  `resolutionOpen` 信号，行为正确）
- 跟进：2026-09-23 登记；同日真机 UI 点击验证分辨率切换链路（848x480→640x360，
  内参同步更新）并提交上游 issue #72。

### EUI-20260923-003：动态纹理（`eui::ImageStream`）在当前 GL 栈渲染异常

- 级别：P1（上游缺陷，阻塞"经 ImageStream 显示相机流"这一官方推荐路径）
- 现象/证据（本机均复现，2026-09-23）：
  - 环境：Ubuntu 24.04，Mesa 25.2.8，`GL_RENDERER = Mesa Intel(R) Graphics (ARL)`，
    GL 4.6 Compatibility；`GALLIUM_DRIVER=softpipe` 下同样复现（排除 Intel 驱动专属）。
  - **上游官方示例复现**：`examples/dynamic_texture.cpp`（NV12/I420/P010 轮播）渲染为
    竖向色带而非移动渐变（截图存档
    `screenshots/upstream-repro/official_dynamic_texture_intel_mesa_bands.png` 与
    `..._softpipe_bands.png`；issue 内嵌线上副本见 #71）。
  - 本仓库 viewer（RGBA8 提交，30fps）：画面仅左缘数像素列更新、其余全黑。
  - 合成渐变最小探针（RGBA8，~10fps 提交）：画面全黑（
    `screenshots/upstream-repro/minimal_rgba8_probe_black.png`），`submit()` 无失败返回。
  - 采集侧排除：librealsense 原始帧落盘 100% 像素有效（848x480 RGB8 stride 2544），
    深度 57% 非零；GPU/环境整体排除：见绕行方案效果。
- 影响：凡经 `ImageStream` 提交的实时画面在本环境不可用；`importGpuImage` 外部
  GPU 图像路径在同一环境渲染完全正常（完整画面截图
  `screenshots/viewer_gpuimage_switch640.png`），故判定缺陷位于 ImageStream 的
  CPU 帧上传/纹理更新路径，而非 GL 环境整体。
- 期望语义：RGBA8/YUV 帧上传后纹理内容完整、按提交序呈现。
- 建议最小能力：修复 ImageStream 的纹理上传路径（疑似 PBO/行距/异步映射处理）；
  或在文档标注已知不兼容的驱动范围。
- 状态：Open（已按官方支持的外部 GPU 图像接口在 viewer 内绕行，见下）
- 绕行方案（单一边界内，符合台账纪律）：`apps/viewer/gpu_frame_view.hpp`——
  UI/渲染线程以自有 GL 纹理 `glTexSubImage2D` 上传 RGBA8，经
  `eui::image::importGpuImage` 导入绘制；revision 失效 + `app::requestUpdate()`
  驱动重绘；分辨率切换换新纹理导入，旧纹理由框架 retirement 队列经 owner deleter
  回收；`onShutdown` 释放引用。移除条件：上游修复 ImageStream 并经真机回归后，
  切回 `.stream()` 路径并删除 GpuFrameView。
- 跟进：2026-09-23 登记；同日真机验证绕行后完整画面 + 键盘/鼠标切换 + 干净退出。

### EUI-20260923-004：`eui_neo_configure_app` 目标被施加 `-fno-exceptions`，与 Executor 异常式 API 冲突

- 级别：P3（宿主策略冲突，应用侧可绕行，非缺口）
- 现象/证据：`CMakeLists.txt` 的 `eui_apply_compile_options()` 对全部 app 目标在非
  Debug 配置统一追加 `-fno-exceptions`（及 `-fno-rtti`）。viewer 的 `app.cpp` 直接
  内联 Executor 公开头（`executor/task_cancellation.hpp` 等会抛出取消异常），Release
  构建报 `error: exception handling disabled`；Debug 配置不触发，故 M1（仅 debug/
  asan/ubsan 预设）未暴露，Release 打包构建时复现。
- 影响：任何集成 Executor 异常式 API 的 app 目标都无法在非 Debug 配置按默认编译
  选项通过。
- 期望语义：框架宿主策略不应假定应用不使用异常，或提供显式的 opt-out 目标属性。
- 建议最小能力：`eui_neo_configure_app` 增加 `NO_EXCEPTIONS` 选项（默认保持现行为），
  或将 `-fno-exceptions` 限定于框架注入的入口 TU 而非整个应用目标。
- 绕行方案（单一边界内）：`apps/viewer/CMakeLists.txt` 在 `eui_neo_configure_app(rin)`
  之后对该目标追加 `-fexceptions -frtti`（GCC 后写优先，仅覆盖 app 目标自身）。
- 移除条件：上游提供 opt-out 后改用官方开关。
- 状态：Reported（绕行已实施；Release 打包验证通过）
- 跟进：2026-09-23 登记。

### EUI-20260924-001：retained layer 签名不含 polygon points，点集更新后缓存层永不失效（3D 位姿视图冻结）

- 级别：P1（实时 polygon 内容在缓存层内冻结，Rin 3D 位姿视图核心功能不可用）
- 现象/证据（2026-09-24 真机运行时取证，插桩 + 像素差分，日志 `/tmp/iva_rin_stderr.log`、
  `/tmp/iva_run3_stderr.log`、`/tmp/iva_run4_stderr.log`，截图 `/tmp/iva_p{A,A2,B}.xwd`）：
  - 环境：Ubuntu 24.04，Mesa Intel (ARL)，debug 构建，GLFW X11（XWayland）窗口
    1280x860 @59 FPS。
  - 提交侧（Rin compose 边界，每帧 37 个 polygon：grid 18 + world axis 3 + face 5 +
    frustum edge 8 + cam axis 3；元素 id `view.pose.grid.*`、`view.pose.axis.*`、
    `view.pose.face.*`、`view.pose.edge.0..7`、`view.pose.cam.*`）：采样 79/79 帧全部
    提交 37 polys，视锥 apex→像平面 edge.0 的 quad 点集在 79/79 个采样间隔内逐帧
    不同（静止相机下 ~1px/0.5s 漂移，30s 累计 12-18px；运动激励下幅度更大），
    segmentToQuad 无剔除（edge0State=1 全程）。
  - 屏幕侧（xwd 抓窗口客户区像素差分）：t=8s 与 t=30s 两帧之间，位姿卡区域
    0 像素变化（完全逐位相同）；同一窗口 RGB 视频卡 26.5-26.7% 像素/秒变化
    （直播常新）、IMU 面板数字持续刷新、整窗 59 FPS 重绘——唯有 polygon 内容冻结在
    首次烘焙的姿态。
  - EUI 自报统计（窗口标题）：`Draw R14 P0 TP33 T44 I3`——平均每帧 polygon 直接绘制
    0 次（37 个 polygon 元素全部走 retained layer 缓存路径）；`Layer H2 M4 D2 Re2`。
  - 上游机制定位（pinned `782c5699`）：
    - `core/runtime/runtime_update.h` `updatePolygon()` L853-858 能正确检出点集变化
      （`!samePoints(instance.points, element.polygonPoints)`，容差
      `closeEnough` ε=0.001 逻辑像素，`core/runtime/runtime_animation.h` L71/L144），
      拷贝新点并 `addDirtyUnion`——失效检测正常。
    - `core/runtime/runtime_render.h` `retainedElementPaintSignature()` L672-756 对
      id/kind/zIndex/bounds/颜色/文本/图片 contentVersion 等做哈希，**唯独不含
      `element.polygonPoints`**；`renderRetainedElements()` L836-838 以
      `layer.signature == signature` 判定缓存层复用并直接 blit 旧纹理。
    - 结论：仅点集变化的子树签名恒定 → 缓存层永不失效（pendingSignature 稳定帧
      计数也永不满足重建条件）→ 首帧姿态被永久缓存，dirty 区域重绘的是旧纹理。
- 影响：凡位于 retained layer 候选子树（clip/高绘制成本）内、id/布局/颜色稳定而
  仅 points 逐帧变化的 polygon，视觉内容冻结在层首次重建时的状态；Rin 3D 位姿视图
  （DEC-011）是该缺陷的直接受害者——真机表现为"冻结在初始姿态"，运动激励下同样
  不动。
- 期望语义：polygon 点集变化应使所在 retained layer 缓存失效并重建（点集参与签名，
  或 pointsChanged 触发所在层 dirty）。
- 建议最小能力：`retainedElementPaintSignature()` 将 `element.polygonPoints`
  （尺寸 + 逐点量化值）混入签名；或 `updatePolygon` 检出 `pointsChanged` 时使包含该
  元素的 retained layer 失效。
- 复现要点（上游最小复现）：层缓存候选容器（`clip()` + 足够绘制成本）内放一个
  polygon：id/position/size/color 每帧不变，仅 points 以 >0.001px/帧 连续变化——
  预期内容移动，实际冻结；标题统计 `P0`（polygon 直接绘制为 0）可作判定指纹。
- 候选绕行（已实施，选型记录）：位姿场景 polygon 全部携带非空
  `dirtyKey`（取姿态通道序号，`apps/viewer/pose_view.hpp` `composePoseScene`）——
  非 `dirtyKey` 变签名再等层重建，而是利用框架"显式键控内容"语义：
  `elementBlocksRetainedLayer()` 对非空 `dirtyKey` 元素直接排除出 retained
  layer 改为逐帧直接绘制，`updateExplicitDirtyKey` 在键变化时补脏区。代价为位姿
  场景 37 个 polygon 逐帧直接绘制（DEC-011 原型实测同量级组装 ~40µs/帧，预算
  内）；键取会话单调序号，静止时稳定、随快照推进变化。备选的 id 附加帧计数
  方案否决：id 不稳定波及 instance/state 作用域生命周期，收益不优于 dirtyKey。
- 状态：Open（绕行已实施；待上报上游，上游修复后移除 dirtyKey 绕行并回归）
- 跟进：2026-09-24 登记；运行时取证完成，插桩已还原；同日绕行实施
  （`pose_view.hpp`，引用本编号）。

## 跟进记录表

| 编号 | 状态 | 优先级 | 跟进 |
| --- | --- | --- | --- |
| EUI-20260923-001 | Reported | P3 | 上游 #73；设计已按幂等 owner 规避 |
| EUI-20260923-002 | Reported | P3 | 上游 #72；viewer 已按现语义接线并完成真机点击验收 |
| EUI-20260923-003 | Reported | P1 | 上游 #71（附复现截图）；viewer 已绕行（GpuFrameView），上游修复后回归 |
| EUI-20260923-004 | Reported | P3 | 2026-09-23 登记；viewer 目标以 `-fexceptions -frtti` 绕行，Release 打包验证通过 |
| EUI-20260924-001 | Open | P1 | 2026-09-24 登记；retained layer 签名缺 polygon points 致 3D 位姿视图冻结（插桩+像素差分取证）；viewer 已以 pose 场景 polygon dirtyKey 绕行（pose_view.hpp）；待上报上游，修复后回归移除绕行 |
