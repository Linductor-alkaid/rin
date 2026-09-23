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
- 状态：Open（向上游反馈前保持观察；不影响当前集成正确性，幂等设计已覆盖）
- 跟进：2026-09-23 登记。

### EUI-20260923-002：`components::dropdown` 弹层展开必须经 `bindOpen`/`onOpenChange` 外接状态

- 级别：P3（使用方式观察项，非缺口）
- 现象/证据：`components/dropdown.h` 的 `build()` 中弹层可见性与命中开关均读取构建期
  常量 `open_`；字段 `onClick` 仅调用可选的 `onOpenChange_` 回调。未接 `bindOpen`/
  `onOpenChange` 时点击字段无任何效果（viewer 首版真机点击无响应，截图证据）。
- 影响：应用必须为每个下拉保留 `eui::Signal<bool>` 开合状态并在选中后自行关闭弹层。
- 期望语义：未外接开合状态时框架内部保留默认开合行为。
- 建议最小能力：`build()` 在未注册 `onOpenChange_` 时使用内部 retained 开合状态。
- 状态：Open（已在 viewer 中按现语义接线：`resolutionOpen` 信号，行为正确）
- 跟进：2026-09-23 登记；同日真机 UI 点击验证分辨率切换链路（848x480→640x360，
  内参同步更新）。

### EUI-20260923-003：动态纹理（`eui::ImageStream`）在当前 GL 栈渲染异常

- 级别：P1（上游缺陷，阻塞"经 ImageStream 显示相机流"这一官方推荐路径）
- 现象/证据（本机均复现，2026-09-23）：
  - 环境：Ubuntu 24.04，Mesa 25.2.8，`GL_RENDERER = Mesa Intel(R) Graphics (ARL)`，
    GL 4.6 Compatibility；`GALLIUM_DRIVER=softpipe` 下同样复现（排除 Intel 驱动专属）。
  - **上游官方示例复现**：`examples/dynamic_texture.cpp`（NV12/I420/P010 轮播）渲染为
    竖向色带而非移动渐变（留档 `/tmp/dyn_shot.png`、`/tmp/dyn_soft.png`）。
  - 本仓库 viewer（RGBA8 提交，30fps）：画面仅左缘数像素列更新、其余全黑。
  - 合成渐变最小探针（RGBA8，~10fps 提交）：画面全黑，`submit()` 无失败返回。
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

## 跟进记录表

| 编号 | 状态 | 优先级 | 跟进 |
| --- | --- | --- | --- |
| EUI-20260923-001 | Open | P3 | 2026-09-23 登记；设计已按幂等 owner 规避 |
| EUI-20260923-002 | Open | P3 | 2026-09-23 登记；viewer 已按现语义接线并完成真机点击验收 |
| EUI-20260923-003 | Open | P1 | 2026-09-23 登记；viewer 已绕行（GpuFrameView），上游修复后回归 |
