# EUI-NEO 依赖台账

> 状态：Active
> 依赖：EUI-NEO（pinned `4691fc0a5c1fde6f3e22f1ac454ed87c7a17f722`，上游 dev 分支——
> 含 #71/#72/#73/#76/#77/#78 修复；待上游发布正式版本后回迁 main/tag，
> https://github.com/sudoevolve/EUI-NEO ，Apache-2.0）
> 记录纪律：工程规范第 9.4 节（台账部分由 AGENTS.md“依赖独立台账”节扩展到所有依赖）。

## 依赖身份

- 来源：https://github.com/sudoevolve/EUI-NEO（备用镜像 AtomGit）
- 锁定：`third_party/dependencies.lock` 行 `eui-neo|...|791cb46b...|...|OFF|external`
  （2026-10-03 由 `782c5699` 升至 dev `4691fc0a`，验证会话见文末；2026-10-09
  为包含滑条 zIndex 修复 EUI-20261009-001 / 上游 PR #96 临时改指 fork 分支
  提交 `791cb46b`（基 dev `c444e53`），上游合入后回迁 upstream，见条目跟进）
- 许可证：Apache-2.0（上游 LICENSE）
- 类别：external（FetchContent 源码引入 / 本地 pinned clone）
- 集成方式：`add_subdirectory` → `eui::neo`；应用目标经 `eui_neo_configure_app()`
  获得入口与资源部署（[DEC-002](../../decisions/DEC-002-runtime-ui-integration.md)）。
  GLFW 引入当前经 `cmake/Dependencies.cmake` 特例路由（见 EUI-20261003-001/002/003
  绕行；上游 #79/#80/#81 修复前维持）。
- 后端：GLFW 3.4（依赖自带 `3rd/glfw`，已含上游 X11 IME 修复）+ OpenGL；
  glad/FreeType 等其余由上游 3rd/ 提供。

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
  内参同步更新）并提交上游 issue #72。2026-09-28 `M5-04` 参数面板 Enumeration
  下拉按本条语义接线（`bindOpen` 外接开合 + 选项点击组件回调自动收起），行为
  正确；弹层浮于后续参数行依赖同父容器 zIndex 抬升（DEC-005 层叠纪律），无新
  缺口。

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
- 状态：Open（绕行维持；2026-10-04 真机回归证实 dev `4691fc0` 的 #71 修复
  （e5ca594）对本机环境无效，切回尝试已回滚，见下）
- 绕行方案（单一边界内，符合台账纪律）：`apps/viewer/gpu_frame_view.hpp`——
  UI/渲染线程以自有 GL 纹理 `glTexSubImage2D` 上传 RGBA8，经
  `eui::image::importGpuImage` 导入绘制；revision 失效 + `app::requestUpdate()`
  驱动重绘；分辨率切换换新纹理导入，旧纹理由框架 retirement 队列经 owner deleter
  回收；`onShutdown` 释放引用。移除条件：上游修复 ImageStream 并经真机回归后，
  切回 `.stream()` 路径并删除 GpuFrameView。
- 跟进：2026-09-23 登记；同日真机验证绕行后完整画面 + 键盘/鼠标切换 + 干净退出。
  2026-10-04 切回尝试（独立验证会话，dev `4691fc0`，D435IF 真机）：viewer 与
  合成探针双重复现——viewer RGB/Depth 卡逐像素差分仅左缘第 1 列更新、区域纯单色
  （#71 症状逐字复现，提交侧 tryLoadFrame/submit 正常）；独立合成探针（无相机、
  RGBA8 移动彩条，存档 `screenshots/upstream-repro/imagestream_rgba8_probe_20261004.cpp`，
  截图 `..._stripe.png` 与 `viewer_imagestream_20261004_left_strip.png`）仅渲染
  1-2 像素宽垂直条纹——判定 e5ca594（GL 像素解包状态隔离）未覆盖本机
  Mesa Intel ARL + XWayland 的触发路径，缺陷不在 Rin 集成层与相机内容。切回
  改动全部回滚，绕行恢复。勘误：dev 验证会话（2026-10-03）中"gpu_image_probe
  通过 ⇒ #71 已验证"的推断不成立——该探针覆盖的是 `importGpuImage` 外部导入
  路径（即绕行所走路径），非 ImageStream 上传路径。待持探针证据跟进上游 #71。
  2026-10-04 同机 A/B 对照（独立验证，bundled GLFW 3.4 X11 + Mesa 25.2.8，
  唯一变量为 eui 版本）：OLD `782c569` / NEW `4691fc0` × 官方 dynamic_texture
  / RGBA8 合成探针四格——RGBA8 探针在两版症状**逐位一致**（仅 x=52-53 两条
  2px 窄条帧间变化 0.18%、图像区其余逐像素冻结、seq 正常推进；交叉验证含
  Rin 构建树二进制三份一致），判定 e5ca594 对该症状零改善。官方 YUV 示例在
  两版均正常——2026-09 报告中的"竖向色带"未在 GLFW 3.4 环境复现（当时为
  系统 GLFW 3.3），该簇症状疑与 GLFW/环境相关，与 RGBA8 窄条非同因。上游
  修复配套探针仅做 `glGetTexImage` 内容回读验证、不覆盖屏幕渲染结果，故其
  通过不构成修复有效的证据。附加线索：探针在同上下文含 EUI 文字渲染时即
  复现，指向同上下文其他 GL 上传方与 ImageStream 渲染路径的交互。A/B 证据
  （含探针源码与截图对）已作为评论提交上游
  sudoevolve/EUI-NEO#71（issuecomment-5980595234）。

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

### EUI-20260928-001：无左窄边常驻导航原语，全局导航控件按 viewer 令牌自绘

- 级别：P3（形态缺口观察项；自绘绕行，非阻塞缺陷）
- 现象/证据（2026-09-28，M5-02 导航控件选型原型验证，pinned `782c5699`）：
  DEC-014 冻结的全局导航形态为"左侧窄边导航（图标+文字）"，设计文档 §6 首选
  `sidebar`、退化 `tabs`/`segmented`。逐一核对：
  - `components/sidebar.h`：右锚定模态抽屉——面板几何 `panelX = width -
    panelWidth`（右缘贴靠）+ 全屏 scrim + 右滑入动画 + 关闭按钮，为临时浮层
    语义，无左侧常驻/内联布局形态，不适用全局导航；
  - `components/tabs.h` / `segmented.h`（上游《组件》文档 §segmented/tabs）：
    横向选择器，形态与冻结 IA（左窄边）不符，仅可作信息架构降级；
  - `components/navbar.h`：垂直导航栏、形态吻合，但未列入上游《组件》文档
    （无能力/稳定性承诺），且颜色/字号/间距全部取自组件库自有
    `theme::ThemeColorTokens`/`ThemeMetricTokens` 度量体系——引入即与
    DEC-005"viewer_theme.hpp 唯一令牌层"纪律冲突，每条目自带描边与阴影的
    视觉语汇也与 Rin 设计系统基准不符。
- 影响：按映射表首选/退化项均无法实现冻结的左窄边导航形态。
- 期望语义：上游提供文档化的左窄边导航原语（垂直 rail/侧边导航，接受外部
  令牌与条目模板）。
- 建议最小能力：`components` 增加文档化 navigation rail 组件，支持
  items/selected/onChange 受控接线与主题令牌注入。
- 绕行方案（单一边界内，符合台账纪律）：`apps/viewer/navigation.hpp`
  `composeNavRail`——以 `rect`/`text` 原语 + viewer 令牌自绘四页导航栏
  （沿用 composeSelect 对 dropdown 的自研先例，EUI-20260923-002/003 同类）；
  条目受控渲染（选中态由 `NavigationState` 驱动，无组件内状态），图标为框架
  捆绑 Font Awesome 7 Free Solid（codepoint 已对照字体 cmap 验证）。静态四
  条目、无文本输入/滚动等复杂交互，风险有界。
- 移除条件：上游提供文档化左窄边导航组件且支持外部令牌注入后，替换自绘实现
  并做真机视觉回归。
- 状态：Open（绕行已实施，M5-02 引用本编号）
- 跟进：2026-09-28 登记。

### EUI-20261003-001：dev `d7b15ea` 硬依赖 GLFW 3.4 API，auto 模式解析系统 GLFW 3.3 时编译失败

- 级别：P2（dev 分支集成阻断；bundled 模式绕行）
- 现象/证据（2026-10-03，Rin debug 预设，eui-neo dev `4691fc0`）：
  - `core/window/window_backend.cpp:582` 无条件调用 `glfwWindowHintString(GLFW_WAYLAND_APP_ID, appId)`；
    该常量仅 GLFW 3.4 定义。
  - `3rd/dependencies.cmake` 在 `EUI_DEPS_MODE=auto`（默认）下先 `find_package(glfw3 CONFIG)`，
    本机系统 GLFW 为 3.3（`/usr/include/GLFW/glfw3.h` 无 `GLFW_WAYLAND_APP_ID`），
    编译报 `error: 'GLFW_WAYLAND_APP_ID' was not declared in this scope`（Rin 构建日志）。
  - dev 之前 pinned `782c569` 在同一环境以 auto + 系统 GLFW 3.3 构建通过（无该引用）。
- 影响：凡系统仅装 GLFW 3.3 且走 auto 模式的集成方，dev 起无法编译；#77 修复对
  系统(GLFW 3.3)路径不可用。
- 期望语义：对 GLFW 3.3 保持兼容（`#ifdef GLFW_WAYLAND_APP_ID` 条件编译），或
  auto 模式校验系统 GLFW 版本不满足时回退 bundled/fetch。
- 建议最小能力：`window_backend.cpp` 对 `GLFW_WAYLAND_APP_ID` 加版本/宏保护，或在
  CMake 配置期校验 glfw3 版本 ≥ 3.4 并给出明确诊断。
- 绕行方案（已实施，单一边界内）：`cmake/Dependencies.cmake` 对 eui-neo 特例——
  `FetchContent_Populate` 后先由依赖自带 `3rd/glfw`（3.4，含上游提交的 IME 修复）
  `add_subdirectory` 预定义 `glfw` 目标，再 `add_subdirectory` eui-neo 本体，使上游
  走 "Using existing GLFW target" 分支，跳过系统 glfw 探测（不再依赖
  `EUI_DEPS_MODE`）；本地构建因缺 `libxkbcommon-dev` 另传 `-DGLFW_BUILD_WAYLAND=OFF`
  仅编 X11 后端（运行时为 XWayland，与原 pinned 运行路径一致），CI 已安装所需
  dev 包、走默认 Wayland+X11。
- 移除条件：上游修复 #79 并被锁定版本包含后，恢复 `FetchContent_MakeAvailable`
  原生路径并删除该特例。
- 状态：PR 已提交（上游 sudoevolve/EUI-NEO#85；2026-10-05 受开发者邀请在
  Linux 本机复现、修复并提交）
- 跟进：2026-10-03 登记；同日干净 dev 检出复现并上报 #79。2026-10-05 修复：
  `window_backend.cpp` 桌面标识 hint 宏守卫 + auto 模式 `find_package(glfw3 3.4)`
  回退 bundled（PR #85）。独立验证：预定义系统 3.3 目标路径编译通过（阴性
  对照 base 复现报错）、auto 回退 bundled 生效、bundled 3.4 下 `.appId` 运行时
  WM_CLASS 读回一致；上游合并后随锁清单升级移除 Rin 侧同型绕行。

### EUI-20261003-002：dev `4691fc0` GLFW X11 IME 补丁脚本哈希常量与真实 GLFW 3.4 不符，bundled/fetch 路径配置期 FATAL

- 级别：P2（dev 分支 GLFW 路径集成阻断；哈希校正绕行）
- 现象/证据（2026-10-03，与 EUI-20261003-001 同一构建会话）：
  - `scripts/patch_glfw_x11_ime.cmake` 声明
    `expected_source_hash=BBCBDD40...`（称"bundled GLFW 3.4"）与
    `expected_output_hash=B1B9E54E...`（打补丁后）。
  - 实测官方 GLFW 3.4 zip（SHA256 `a133ddc3...`，与 dev `3rd/dependencies.cmake`
    FetchContent 声明一致）解出的 `src/x11_window.c` 哈希为 `7c5a60a5...` ≠
    `BBCBDD40...`；dev 捆绑文件（`4691fc0` 已直接提交 6 行 IME 修复）哈希为
    `8ce625aa...`，与两个期望值均不符。
  - `3rd/dependencies.cmake:271-291` 在 `EUI_OWNS_GLFW_TARGET=ON`（bundled/fetch
    均如此）时无条件执行该脚本 → 配置期
    `Failed to prepare the GLFW X11 IME fix: Unsupported GLFW X11 source hash`。
    即该提交使捆绑与 fetch 两条 GLFW 路径都无法配置（脚本对"已含修复的捆绑文件"
    与"原始 3.4"均不识别）。
- 影响：Linux + GLFW 后端的捆绑/fetch 集成在 dev 起配置即失败。
- 期望语义：脚本哈希表覆盖真实 GLFW 3.4 原始哈希与已修复哈希（或改为检测
  KeyPress/KeyRelease 上下文匹配而非哈希白名单）。
- 建议最小能力：更新两个哈希常量为实测值（pristine `7C5A60A5...`、patched
  `8CE625AA...`）。
- 绕行方案（已实施，单一边界内）：同 EUI-20261003-001——`cmake/Dependencies.cmake`
  预先定义 `glfw` 目标后，上游 `EUI_OWNS_GLFW_TARGET` 保持 OFF，补丁脚本在 Rin 构建
  中不再执行（捆绑 `3rd/glfw` 已直接含修复，行为无偏移）。另：本地依赖测试克隆
  `/home/linductor/eui-neo` 的 `scripts/patch_glfw_x11_ime.cmake` 有一处未提交哈希
  校正（`expected_output_hash` → `8CE625AA...`），仅供跑上游自带测试套件使用，
  不影响 Rin 构建。
- 移除条件：上游修正脚本（#80）并被锁定版本包含后，恢复原生路径并还原本地克隆。
- 状态：PR 已提交（上游 sudoevolve/EUI-NEO#84；d7ccfc2 仅修正 source hash，
  output hash 仍错——Linux bundled/fetch 双路径 configure 期依然 FATAL，已在
  #80 评论附证据并提一行修复 PR #84）
- 跟进：2026-10-03 登记+哈希取证（官方 3.4=`7C5A60A5...`、变换产物=dev 捆绑
  文件=`8CE625AA...`）；2026-10-04 上游 d7ccfc2 关闭 #80（半修）；2026-10-05
  复测发现残余缺陷（bundled 源 8CE625AA 两分支均不匹配 / fetch 产物校验
  mismatch），评论 #80 + PR #84（expected_output_hash → 8CE625AA）。
- 跟进：2026-10-03 登记。

### EUI-20261003-003：dev `c7efcf1`（#76 修复）在库本体引入 `catch(...)`，与 `eui_apply_compile_options` 非 Debug 的 `-fno-exceptions` 冲突，eui_neo 库非 Debug 配置无法编译

- 级别：P1（dev 分支阻断 Rin release/dist 预设）
- 现象/证据（2026-10-03，独立验证会话实测复现）：
  - `core/platform/platform.cpp:286`（#76 修复 c7efcf1 引入）使用 `catch (...)`
    包裹 popen 命令拼装；
  - `CMakeLists.txt` `eui_apply_compile_options()` L125/L131 对非 Debug 配置向
    除 `vcd_viewer`/`gpu_image_probe` 外的全部目标（含库目标 `eui_neo`）施加
    `-fno-exceptions` → 非 Debug 编译报
    `error: exception handling disabled, use '-fexceptions' to enable`；
  - Debug 配置不触发，解释上游 CI 未发现；本次独立验证初始非 Debug 构建即失败，
    加 `-DCMAKE_BUILD_TYPE=Debug` 后通过。
- 影响：Rin 以 release/dist 预设构建 dev 版依赖时在依赖编译期失败；台账既有
  EUI-20260923-004（app 目标同源冲突）的绕行救不了库本体。
- 期望语义：库本体不施加 `-fno-exceptions`（或平台层避免异常构造），或提供
  显式 opt-out 目标属性。
- 建议最小能力：`eui_apply_compile_options()` 的 `-fno-exceptions` 豁免范围
  覆盖 `eui_neo` 库目标，或 `platform.cpp` 改用错误码路径。
- 绕行方案（已实施，单一边界内）：`cmake/Dependencies.cmake` 在 eui-neo
  add_subdirectory 后对 `eui_neo` 目标追加
  `$<$<NOT:$<CONFIG:Debug>>:-fexceptions>`（GCC 后写优先，与 EUI-20260923-004
  的 app 目标绕行同型；Debug 配置上游本就不施加）。本机 debug/release 两预设
  全量构建验证通过。
- 移除条件：上游修复 #81 并被锁定版本包含后，删除该追加。
- 状态：PR 已提交（上游 sudoevolve/EUI-NEO#86；2026-10-05 受开发者邀请在
  Linux 本机复现、修复并提交）
- 跟进：2026-10-03 登记；同日干净 dev 检出 + Release 复现 platform.cpp:286
  编译失败。2026-10-05 修复：`temp_directory_path()` 抛出重载 + `catch(...)`
  改 `error_code` 非抛出重载（PR #86）。独立验证：Release `-fno-exceptions`
  下库本体建成；platform_dialog Debug 常规通过 + Release 经 `-fexceptions`
  fixture 链接 Release 库运行通过。附带发现测试侧同型缺口（上游 #87）。

### EUI-20261003-004：上游测试 `tests/unit/image_stream.cpp` 与实现脱节，Debug 构建下确定性 SIGABRT

- 级别：P3（上游测试缺陷，不阻断 Rin 集成）
- 现象/证据（2026-10-03，独立验证会话，确定性复现 3/3）：
  - `tests/unit/image_stream.cpp:34` 断言 BGRA8 帧提交必须被拒绝（源自 a969ed5）；
    但实现 `core/render/image_stream.cpp:104-105` `ImageFrame::valid()` 显式接受
    BGRA8，`submit()`（:181-191）仅做 `valid()` 校验 → 返回 true → 断言失败
    （exit 134）；仅在 assert 生效的 Debug 构建暴露，Release 下 NDEBUG 掩盖。
  - 与本次 5 个修复提交无关（e5ca594 未触碰该文件），为 dev HEAD 既有脱节。
- 影响：上游测试套件在 Debug 下不可用（本次验证 33 项中唯一失败）。
- 期望语义：测试断言与当前实现语义对齐（BGRA8 合法或恢复拒绝）。
- 状态：Resolved（上游 #82 已由 dev `123f0c5` 修复：BGRA8 用例更新，2026-10-05
  Release/Debug 复测均通过）
- 跟进：2026-10-03 登记+上报；2026-10-05 上游修复复测关闭。

### EUI-20261005-001：非 Debug 配置下测试夹具与应用目标自身异常语法与 `-fno-exceptions` 冲突

- 级别：P3（上游测试构建缺陷：`EUI_BUILD_TEST_FIXTURES=ON` 的 Release 下
  `platform_dialog`/`dsl_app_lifecycle_probe` 等编不过、ctest Not Run；不阻断
  Rin 集成——Rin 不构建上游 fixtures）
- 现象/证据（2026-10-05，dev `123f0c5`，验证 #81 修复时发现）：`tests/unit/
  platform_dialog.cpp:17` 的 `throw` 与 `include/eui/detail/dsl_app_impl.h:301`
  的 try/catch 在豁免清单（仅 vcd_viewer/gpu_image_probe）之外，Release 33 项
  ctest 31 过 2 Not Run；base 123f0c5 即如此（预存在）。platform_dialog 以
  `-fexceptions` fixture 链接 Release（`-fno-exceptions`）库本体运行通过
  （exit 0），逻辑本身可跑。
- 状态：Reported（上游 issue sudoevolve/EUI-NEO#87）
- 跟进：2026-10-05 登记 + 上报 #87（建议：断言改非抛出或豁免清单补测试/应用
  目标）。

### EUI-20261009-001：组件滑条内部 `.hit` 自带 zIndex(10)，经子树 z 排序使根级浮层失去命中优先级

- 级别：P2（功能性交互缺陷：浮层下拉在含滑条节点的页面全部不可点选；宿主可
  显式声明浮层 z 绕行）
- 现象/证据（2026-10-09，v0.5.3 debug 真机，D435IF，脚本化合成指针事件
  A/B 复现）：
  - 工作流页画布：`相机源 深度米制 → 深度无效填充`（带滑条参数）接线后，
    相机源节点的分辨率下拉浮层点击全部穿透——scrim 与菜单项均不接收事件，
    事件落到画布 `mouseArea`（onPress 即收起浮层）；同一画布接 `监看器`
    （无滑条参数）则浮层交互完全正常。A/B 唯一差异即页面内是否存在滑条。
  - 命中路径取证（pinned `4691fc0`）：命中与绘制遍历按
    `orderedChildren`/`orderedRoots` 排序，键为 `subtreeMaxZIndex`
    （`core/dsl.h` `rebuildOrderedElements`，升序、命中逆序）；
    `components/slider.h` 的内部热区 `".hit"` 自带 `.zIndex(10)`——任一滑条
    存在于页面子树即把该子树的 z 上限抬到 10，默认 z=0 的根级浮层
    （scrim/菜单面板）在排序中落到页面之下，浮层先于页面合成也无法挽回
    （排序不看合成序，只看 z）。
- 影响：凡页面内容包含 z>0 元素（现仅滑条；组件库若未来在更多组件内部
  使用 zIndex，同型风险扩大），根级模态浮层（下拉/scrim/拖拽跟随）的
  命中与绘制均被页面内容压过。Rin 工作流页症状：接任何带滑条参数的模块
  （深度域全部算子、crop/downscale 等）后，所有相机源节点的分辨率下拉
  与右键创建菜单无法点选。
- 期望语义：组件内部热区的 zIndex 不应影响宿主浮层与页面内容的层序；
  或文档明示"根级浮层必须显式声明高于页面内容的 zIndex"。
- 建议最小能力：slider（及其他自带 zIndex 的组件）改用不参与子树排序的
  命中提升机制，或提供关闭内部 zIndex 的选项。
- 绕行方案（已实施，单一边界内）：`apps/viewer/viewer_components.hpp`
  新增 `kOverlayZIndex=1000`——`composeFloatingPanel` 的 scrim 取
  `kOverlayZIndex`、面板栈取 `+1`（实测同值 tie 时先合成的 scrim 抢先
  命中，面板须显式再高一级）；`composeSelect` 下拉栈与调色板拖拽跟随
  同取 `kOverlayZIndex`。全部根级浮层恒高于页面内容（页面内容 z 目前
  上限 10，余量充足）。
- 移除条件：上游组件不再内部使用 zIndex（或提供文档化的浮层层级约定）
  后，可移除显式 z 声明并做浮层点击回归。
- 状态：Open（绕行已实施；上游修复 PR sudoevolve/EUI-NEO#96 已提交，等待合入）
- 跟进：2026-10-09 登记；修复 commit 见 `fix(viewer)` 系列提交。2026-10-09 上游
  化：issue sudoevolve/EUI-NEO#95 + PR #96（fork 分支
  `fix/slider-hit-zindex-subtree-order` @ `791cb46`，基于 dev `c444e53`；
  上游 ctest 34/34 含新增回归测试 `slider_subtree_z_order`，修复前该测试
  复现 `subtreeMaxZIndex=10`）。锁清单同步升级：
  `third_party/dependencies.lock` eui-neo 行改为 fork URL @ `791cb46`
  （`4691fc0` → `791cb46`），**回迁条件：PR #96 合入上游后，锁清单回迁
  upstream dev 对应提交并恢复原 URL**；Rin 侧 `kOverlayZIndex` 声明在
  回迁复评前保留（防御性：不依赖宿主组件"内部不使用 zIndex"这一隐含
  前提，且实测同值 tie 的排序行为未在框架层定论）。



| 编号 | 状态 | 优先级 | 跟进 |
| --- | --- | --- | --- |
| EUI-20260923-001 | Reported | P3 | 上游 #73；设计已按幂等 owner 规避 |
| EUI-20260923-002 | Reported | P3 | 上游 #72；viewer 已按现语义接线并完成真机点击验收 |
| EUI-20260923-003 | Reported | P1 | 上游 #71（附复现截图）；viewer 绕行维持（GpuFrameView）；2026-10-04 同机 A/B 证实 e5ca594 对 RGBA8 流路径零改善，切回已回滚；A/B 证据已评论上游 #71，待上游回应 |
| EUI-20260923-004 | Reported | P3 | 2026-09-23 登记；viewer 目标以 `-fexceptions -frtti` 绕行，Release 打包验证通过 |
| EUI-20260924-001 | Open | P1 | 2026-09-24 登记；retained layer 签名缺 polygon points 致 3D 位姿视图冻结（插桩+像素差分取证）；viewer 已以 pose 场景 polygon dirtyKey 绕行（pose_view.hpp）；待上报上游，修复后回归移除绕行 |
| EUI-20260928-001 | Open | P3 | 2026-09-28 登记；sidebar=右锚定抽屉、tabs/segmented 横向、navbar 未文档化且绑定组件主题体系；viewer 自绘窄边导航栏（navigation.hpp），上游出文档化 rail 后替换 |
| EUI-20261003-001 | PR 提交 | P2 | 上游 #79 → PR #85（2026-10-05 Linux 本机修复+验证）；Rin 侧 bundled 绕行维持至上游合并 |
| EUI-20261003-002 | PR 提交 | P2 | 上游 #80 半修（d7ccfc2 仅 source hash）；残余 output hash 缺陷评论 #80 + PR #84（2026-10-05） |
| EUI-20261003-003 | PR 提交 | P1 | 上游 #81 → PR #86（2026-10-05 error_code 非抛出重载修复+验证）；Rin 侧 `-fexceptions` 追加绕行维持至上游合并 |
| EUI-20261003-004 | Resolved | P3 | 上游 #82 已由 123f0c5 修复（`tests/unit/image_stream.cpp` 陈旧断言，2026-10-05 Release/Debug 复测通过） |
| EUI-20261005-001 | Reported | P3 | 上游 #87；Release 下测试夹具/应用目标异常语法编不过（platform_dialog throw、dsl_app_impl try/catch），预存在豁免缺口；不阻断 Rin |
| EUI-20261009-001 | Open | P2 | 滑条内部 zIndex(10) 经子树排序压过根级浮层（分辨率下拉不可点选）；viewer 浮层显式 kOverlayZIndex=1000 绕行（viewer_components.hpp）；上游 issue #95 / PR #96（fork `791cb46b`），锁清单已暂指 fork 修复提交，上游合入后回迁 |

## dev 分支验证会话（2026-10-03）

- 背景：上游 dev（`4691fc0a5c1fde6f3e22f1ac454ed87c7a17f722`）关闭了本方全部
  6 个 issue（#71/#72/#73/#76/#77/#78），对应 5 个修复提交（e5ca594/6d39b88/
  d7b15ea/c7efcf1/4691fc0）。2026-10-03 正式将锁清单 pinned 升至 dev HEAD
  （第三方依赖升级，独立验证通过后经 MR 合入）。
- 构建系统变更：`cmake/Dependencies.cmake` 对 eui-neo 特例路由（Populate +
  预定义 `glfw` 目标 + 对 `eui_neo` 追加 `-fexceptions`，绕过上游
  #79/#80/#81；见各条绕行方案）。本机（无 libxkbcommon-dev）本地构建另传
  `-DGLFW_BUILD_WAYLAND=OFF`，CI 走默认 Wayland+X11。
- 代码接线：`apps/viewer/app.cpp` 增加 `.appId("rin")`（#77 对应能力的最小
  接线；onStart 接线另行决策，EUI-20260923-001 的幂等设计仍成立）。
- 验证结果（独立验证会话，2026-10-03）：
  - Rin 测试套件（debug）：31 项，30 通过 / 1 跳过（`realsense_hardware`，
    无相机，预期跳过）。
  - 上游套件（bundled + Debug）：33 项，32 通过 / 1 失败（EUI-20261003-004，
    与本次修复无关）；8 个探针全过，其中 `gpu_image_probe` 通过（该探针覆盖
    `importGpuImage` 外部导入路径）。勘误（2026-10-04）：当时据此次通过推断
    "#71 已验证"不成立——ImageStream 上传路径的修复效果须经真机回归确认，
    结论见 EUI-20260923-003 跟进记录：e5ca594 对本机无效。
  - viewer 运行时：无相机进入 Waiting 稳态（DEC-006），`WM_CLASS=("rin","rin")`
    实测确认（#77 X11 侧 + Rin `.appId("rin")` 接线），存活 2 分 29 秒无崩溃、
    日志干净、SIGTERM 退出码 143（正常终止）。
  - 按 issue 结论：#71/#72/#73/#76/#77 已验证通过；#78 仅构建级验证
    （补丁产物哈希 `8CE625AA...` 在两个构建树校验通过；IME 交互行为需人工
    输入法环境验证）。
- 残余风险：#71 相机帧真机回归（GpuFrameView → `.stream()` 切回）待相机接入；
  Release/打包构建被 EUI-20261003-003 阻断；上游新缺陷已上报：#79（EUI-20261003-001）、#80（EUI-20261003-002）、
  #81（EUI-20261003-003）、#82（EUI-20261003-004）。
