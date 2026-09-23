# DEC-011：3D 位姿视图渲染路径（CPU 投影 + EUI-NEO polygon 原语）

> 状态：Accepted
> 日期：2026-09-24
> 负责人：Linductor-alkaid
> 冻结里程碑：M3（M3-02 调研冻结）
> 替代/被替代：无

## 背景与问题

M3 要在 viewer 中以固定世界坐标系实时呈现相机位姿（视锥 + 坐标轴 + 地面网格，
`SCOPE-07`）：姿态四元数由 `ImuFuser` 提供（[DEC-010](DEC-010-imu-attitude-fusion.md)），
经 `LatestMailbox` 发布（`EXEC-06`），UI 侧按帧消费。pinned EUI-NEO
（`third_party/dependencies.lock`，commit `782c5699`）没有现成 3D 视口组件，
候选渲染路径有三条（M3 计划已列）：shadertoy GLSL、CPU 投影 + `polygon`/`rect`
原语、2.5D transform。需要决策：用哪条路径渲染 3D 位姿视图，并以最小原型证据
冻结，同时满足边界约束：

- `RULE-01`：公开头不得引入 EUI-NEO 类型，投影数学须可脱离框架单测（M3-06 判据
  "投影数学单测通过"）。
- `RULE-05`/`RULE-07`：EUI 渲染线程只做有界工作；位姿融合在采集 worker
  （DEC-010/`EXEC-06`），不得进入渲染线程。
- 姿态以四元数表达；pitch ≈ ±90°（相机俯视/仰视）是常规姿态，渲染路径不得在该
  姿态附近出现表示奇异。

决策前对 pinned EUI-NEO 公开源码与文档的核对结论：

- DSL 图元枚举仅 `Row/Column/Stack/Flow/Rect/Polygon/Text/Image/Svg/Shadertoy`
  （`core/dsl.h` ElementKind；`docs/DSL.md`），无 3D 视口、无逐顶点 3D 坐标：
  `PolygonBuilder::points()` 接受 `std::vector<Vec2>`（`core/dsl.h:1121`），
  点集位于元素平面内。
- 2.5D transform（`rotateX/rotateY/rotateZ/perspective`）在
  `core/runtime/runtime_geometry.h:153-213` 固定按 Rz·Ry·Rx 欧拉序合成投影矩阵，
  供平面卡片翻转/透视动效使用；无真实深度缓冲，绘制顺序由 DSL 树序与 zIndex 决定
  （`docs/DSL.md` Transform/2.5D 节）。
- `polygon` 为独立 polygon shader，按多边形边段覆盖率抗锯齿（`docs/DSL.md`
  Polygon 节）；框架自身 `components::linechart.h:151-155` 即用 capsule polygon
  绘制折线，是"多边形画线"的既有先例。
- 每帧更新 `polygonPoints` 会被 runtime 检测并合并前后脏区
  （`core/runtime/runtime_update.h:853-864`），逐帧数据驱动重绘是受支持路径。
- 数据驱动重组经 `app::requestUpdate()`（`include/eui/app.h:49`）→
  `requestUiUpdate()` + `postEmptyEvent()`（`core/platform/platform.cpp:758-761`）
  唤醒休眠的 UI 循环；EUI 自身网络线程即用此模式跨线程唤醒 UI
  （`core/platform/network.cpp:247`）。
- Shadertoy 为 `mainImage()` fragment 图元（`docs/Shadertoy.md`）：几何只能以
  fragment 数学表达；OpenGL 由驱动运行期编译，Vulkan 需构建期 SPIR-V
  （glslangValidator + Vulkan SDK）。

## 决策

选定 **CPU 投影 + EUI-NEO `polygon` 原语** 作为 3D 位姿视图渲染路径：

1. **纯数学落 Core**：四元数→旋转矩阵、固定观察相机的 world→view 变换、透视投影、
   线段→屏幕空间四边形（等宽 quad，宽度可按深度缩放）、深度提示（可选）全部为
   无第三方类型的纯函数（`RULE-01`；沿 [DEC-003](DEC-003-depth-colormap.md)
   "呈现类纯逻辑落 Core"先例），直接支撑 M3-06 的投影数学单测。Core 输出
   2D 点集，viewer 在 UI 边界转换为 `eui::Vec2`。
2. **viewer 侧仅做有界组装**：compose 期内把投影点集写入稳定 id 的
   `ui.polygon(id).points(...)`（背景板 `rect` + 半透明视锥面 + 线框 quad）。
   面片/线段遮挡语义由 compose 声明序表达（painter's order）；预览风格为
   线框 + 半透明面，不需要深度缓冲。
3. **帧驱动**：姿态经 `LatestMailbox<ImuSnapshot>` 发布（`EXEC-06` 语义不变）；
   新快照就绪后由采集侧调用 `app::requestUpdate()` 唤醒重组（线程安全，EUI
   网络线程同款模式），compose 内读取 `tryLoadPose()` 并投影。无动画数据时
   UI 循环保持休眠，不空转。
4. **成本预算**：compose 期投影为 O(数十线段) 的有界纯计算（原型实测 ~40 µs/帧，
   见验证方式），属 `RULE-05` 允许的有界校验与提交，不构成 `RULE-07` 的 CPU
   密集处理；融合仍在采集 worker（DEC-010），不新增线程/队列（`RULE-02/03`）。
5. **后端无关**：仅使用 DSL 级 `polygon`/`rect` 公开图元，不写 shader，不引入
   新依赖（`RULE-04` 无新增 pin），OpenGL/Vulkan 后端行为一致。

## 备选方案

| 方案 | 结论 | 理由 |
| --- | --- | --- |
| **CPU 投影 + `polygon`/`rect`（选定）** | 采用 | 投影数学可 Core 纯逻辑单测；四元数直接转矩阵，无欧拉奇异；框架自身折线图即 polygon 画线先例；逐帧点集更新有脏区支持；实测成本可忽略 |
| Shadertoy GLSL（`ui.shadertoy`） | 否决 | 几何/投影进 GLSL：无法在 Core 单测（与 M3-06 判据冲突）、与 Core 数学双份维护；Vulkan 需构建期 SPIR-V 工具链（glslangValidator + Vulkan SDK），GL 侧为运行期编译（结构化报错但属运行期失败面）；RGBA32F 多 pass 离屏 + 全四边形 fragment 对线框场景过剩；线宽/抗锯齿需手写 SDF，代码量更大且收益为零 |
| 2.5D transform（`rotateX/Y/Z + perspective`） | 否决 | `polygon` 点集为元素平面内 2D `Vec2`（`core/dsl.h:1121`），无逐顶点 z——3D 线框须按平面拆分为大量元素并逐元素做欧拉分解；transform 固定 Rz·Ry·Rx 欧拉序（`runtime_geometry.h:153-213`），四元数→欧拉在 pitch ≈ ±90° 奇异（相机俯仰为常规姿态），逐帧数值不稳；无深度缓冲。该 API 设计目标是平面卡片特效，不承载任意 3D 场景 |
| 引入第三方 3D/渲染库（独立 GL 画布、bgfx 类） | 否决（未原型） | 违反 `RULE-01`（公开头第三方类型边界）与 `RULE-04`（新增 pin 需独立评估）；第二套渲染栈与 DslApp 帧循环、脏区、DPI、命中体系天然冲突；为一张位姿线框图引入重依赖不成比例 |

## 影响与风险

- **影响范围**：`M3-06`（3D 位姿视图控件按本决策实现）、`M3-03`（`ImuSnapshot`
  四元数语义，方向约定沿用 DEC-010 由 M3-03 锁定）、`M3-08`（真机姿态跟随验收）；
  `EXEC-06` 不变；`RULE-01/05/07` 满足方式见"决策"节。
- **风险与缓解**：
  1. *无深度缓冲导致遮挡语义弱*：面片/线段按 compose 序绘制，线框透视可见
     （原型中可见）。缓解：预览场景可接受；如需深度提示（远处线段调暗/变细），
     在 CPU 投影侧按 view z 处理即可——这正是 CPU 投影路径的灵活性，不需要
     框架能力。
  2. *每帧重组成本随几何规模线性增长*：当前规模（32 线段 + 5 面片）实测
     ~40 µs/帧；M5 工作台多卡片同屏时用 `loader`/`clip` 按可见性挂载，成本
     不叠加。若未来几何规模显著扩大（>数千线段）再评估分块/降频，当前不预设。
  3. *六轴 yaw 漂移造成视图缓慢偏转*：属 DEC-010 已披露的物理限制；视图提供
     重置（M3-06 范围内），与本渲染路径无关。
  4. *EUI-NEO 升级影响*：仅依赖 `polygon`/`rect`/`requestUpdate` 等稳定公开
     DSL/平台 API；升级时按 `docs/dependency_feedback/eui-neo/ledger.md` 流程
     核查。本次调研未发现新的框架能力缺口，无需新增台账条目（既有
     EUI-20260923-001..004 不涉及本路径；EUI-20260923-003 动态纹理 GL 栈问题
     与 polygon 路径无关）。
- **台账**：不引入新第三方依赖，不触及 Executor 能力边界，无台账新增项。

## 验证方式

M3-06 按本决策实现，并执行下列验收：

1. **投影数学单测**（Core 纯逻辑，`unit` 标签）：已知位姿四元数 + 固定观察相机
   → 期望屏幕点集（golden 向量，固定容差）；退化输入（零长度段、近平面后端点）
   剔除或裁剪行为；四元数规范化；与原型同公式。
2. **UI 集成**：`ctest`（debug/asan/ubsan，涉跨上下文姿态数据加 tsan）全绿
   （`DOD-02/03`）；`RULE-01` 边界编译测试保持通过。
3. **真机**：`M3-08` D435if 记录姿态跟随（视锥随相机实时旋转）、重置语义、
   restream 后恢复；不对真机提出原型合成数据的数值阈值。

### 最小原型证据（2026-09-24，M3-02 一次性原型，代码不入源码树）

原型：`/tmp/rin-dec011-proto/`（`pose_math.hpp`：四元数/矩阵/look-at/投影/
线段 quad，约 150 行；`main.cpp`：EUI-NEO DSL 应用，世界网格 + RGB 坐标轴 +
半透明视锥面 + 线框 + 相机局部轴，合成四元数时钟驱动；`CMakeLists.txt`：
standalone 工程 `add_subdirectory` pinned `eui-neo-src`）。

- 环境：Linux（Wayland/GNOME + XWayland），Intel Core Ultra 5 225H /
  Arrow Lake-P 核显；CMake 3.28.3，g++ 13.3.0（Release `-O2`，C++20）；
  EUI-NEO pinned commit `782c5699`（GLFW + OpenGL 后端，bundled
  freetype/libpng/zlib/md4c/glad，系统 glfw3）。
- 命令：
  `cmake -S /tmp/rin-dec011-proto -B /tmp/rin-dec011-proto/build -DCMAKE_BUILD_TYPE=Release`
  → `cmake --build /tmp/rin-dec011-proto/build -j8` → `DISPLAY=:0 ./build/dec011_proto`。
- 场景规模：每帧 32 条线段 quad（网格 18 + 世界轴 3 + 视锥边 8 + 相机轴 3）
  + 4 个半透明侧面三角 + 1 个像平面四边形 + 背景板，≈130-150 个多边形顶点。
- 实测（原型内 `steady_clock` 计时 + EUI 标题统计）：
  - CPU 投影 + 点集组装：avg 38-46 µs/帧（max 66-162 µs，含原型每帧
    `std::vector` 分配；占 60 fps 帧预算 16.7 ms 的 ~0.3%），每帧输出
    `quads=32 grid=18 axis=3 edge=8 cam=3`（debug 计数齐全，无静默丢失）。
  - EUI-NEO 运行时（`showDebugStatsInTitle` 标题统计，3 次采样）：55-56 FPS、
    GPU 0.58-0.64 ms/帧、Dirty 1.0/100%、Layer Cache 100%——逐帧
    `points()` 更新驱动连续重绘，重组频率实测 ~55 次/s（compose 计数
    单次运行：call=1 @ t=0.00s → call=121 @ t=2.17s → call=241 @ t=4.31s）。
  - 姿态驱动旋转：固定姿态两次运行（`DEC011_FIXED_T=2.5` →
    q=(0.904, 0.215, 0.306, -0.206)；`DEC011_FIXED_T=6.0` →
    q=(0.974, -0.112, -0.168, 0.104)），世界网格与坐标轴完全一致，视锥与
    相机轴呈明显不同朝向（两张窗口截图 md5 `9f99606e…` vs `45c20ec1…`，
    内容可见差异），证明"姿态驱动旋转"成立且世界系稳定。
- 截图与运行日志属一次性验证产物，不入库（工程规范 10.6）；关键数值已如上
  记录，复现方法即上述命令。
- 已知方法学限制：XWayland 下窗口 X 像素图不随后续 GL 呈现更新（首次呈现后
  `xwd`/`glReadPixels` 读回为陈旧帧），连续动画的活性以上述运行时标题统计
  （FPS/Dirty/逐秒刷新）与双姿态对照截图证明；该现象为截图取证手段限制，
  非 EUI 渲染缺陷，真机活跃显示下的实时性由 `M3-08` 复核。

## 关联文档和工作项

- [M3 里程碑](../plans/m3-imu-pose-view.md)：`M3-02`（本决策冻结）、`M3-03`
  （契约与四元数语义）、`M3-06`（按本决策实现 3D 位姿视图控件）、`M3-08`
  （真机验收）。
- [Rin 实施总计划](../plans/rin-implementation-plan.md)：`SCOPE-07`、
  `RULE-01`/`RULE-05`/`RULE-07`、`EXEC-06`。
- [DEC-010](DEC-010-imu-attitude-fusion.md)：姿态四元数来源与披露边界；
  [DEC-003](DEC-003-depth-colormap.md)：呈现类纯逻辑落 Core 的先例；
  [DEC-002](DEC-002-runtime-ui-integration.md)：EUI-NEO 运行时集成。
- [EUI-NEO 反馈台账](../dependency_feedback/eui-neo/ledger.md)：本次无新增
  缺口条目。
- 本地依据：pinned EUI-NEO `core/dsl.h:1121`（polygon 2D 点集）、
  `core/runtime/runtime_geometry.h:153-213`（2.5D 固定欧拉序投影）、
  `core/runtime/runtime_update.h:853-864`（逐帧点集脏区）、
  `include/eui/app.h:49` + `core/platform/platform.cpp:758-761`（跨线程 UI
  唤醒）、`components/linechart.h:151-155`（polygon 画线先例）、
  `docs/DSL.md`、`docs/Shadertoy.md`。
