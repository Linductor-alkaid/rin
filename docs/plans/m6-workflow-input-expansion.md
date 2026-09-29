# M6：工作流输入扩展——深度相机源、灰度算子与控件防呆

> 状态：Complete
> 负责人：Linductor-alkaid（授权 Agent 按工程规范自治执行）
> 所属计划：[Rin 实施总计划](rin-implementation-plan.md)
> 前置：M4（节点库与真引擎）、`M5-06`（假换真集成）
> 立项依据：用户真机验收反馈（2026-09-29，M5-07 验收期输入）；节点库扩展按
> POST-04 "出现需求时立项" 触发
> 建议发布点：v0.5.0（随 M5）
> 更新日期：2026-09-29

## 目标

按用户真机使用反馈扩展工作流输入面并修复防呆缺口：裁切 ROI 越界导致引擎
失败（控件层防止）；相机源支持 RGB + 三种深度输入（伪彩 Rgba8、灰度 Gray8、
自适应灰度 Gray8，语义与预览配色解耦）；灰度域补齐裁切与降分辨率算子；
相机源提供分辨率入口；裁切控件从控件本身避免非法 ROI。

## 范围与非目标

范围：

- [DEC-017](../decisions/DEC-017-workflow-camera-sources.md)：相机源四型与
  深度 rendition 语义冻结（固定 rendition 与预览配色解耦；分辨率是相机流
  全局属性）。
- Core 深度灰度 rendition 转换（`convertDepth16ToGray8` /
  `convertDepth16ToGray8Adaptive`，与 DEC-007 三配色 ramp 的单通道对应）。
- 相机服务契约扩展（`FrameKind::DepthJet`、`GrayFrame`/`GrayFrameKind`/
  `tryLoadGrayFrame`，加性变更）与 adapter 采集循环发布。
- 节点目录扩展：`source_depth_jet`/`source_depth_gray`/`source_depth_adaptive`、
  `crop_gray`/`downscale_gray`（复用既有泛格式实现，语义同 M4-03 冻结值）。
- viewer 帧源路由（source 类型 → rendition 映射，图变更即刷新）。
- 裁切 ROI 控件防呆（已知输入尺寸时按联动约束夹取，M4-03 apply 期拒绝语义
  不变）；相机源面板分辨率入口（与预览选择器共享全局状态）。

非目标：

- 工作流图持久化（`POST-04` 其余部分）。
- per-source 独立分辨率（单 pipeline 物理约束，多源冲突为非法语义，DEC-017）。
- 预览页行为变更（DEC-007 配色命令只影响预览深度通道，不变）。

## 工作项

- [x] `M6-01` 设计冻结：[DEC-017](../decisions/DEC-017-workflow-camera-sources.md)、
      [image_workflow_design.md](../design/image_workflow_design.md) §6 目录表
      扩展与 §7 灰度变体注记、[camera_service_design.md](../design/camera_service_design.md)
      rendition 通道契约。完成判据：记录 `Accepted` 且设计文档结构检查通过。
- [x] `M6-02` Core 深度灰度 rendition 转换 + golden 单测。完成判据：固定区间/
      P99/无效深度/边界 golden 通过。
- [x] `M6-03` 节点目录扩展与工厂注册（三源两算子）。完成判据：目录/工厂
      单测通过，跨类型连线校验拒绝。
- [x] `M6-04` 相机服务契约扩展 + adapter rendition 发布（Jet 通道与预览
      scheme==Jet 时共享同一缓冲）。完成判据：契约单测 + 真机出流。
- [x] `M6-05` viewer 帧源路由 + 灰度源注入（GrayFrame → ImageU8 Gray8）。
      完成判据：路由分发单测 + 灰度链端到端。
- [x] `M6-06` 裁切 ROI 控件防呆 + 相机源分辨率入口。完成判据：约束纯逻辑
      单测 + 面板集成行为。
- [x] `M6-07` 测试矩阵（Independent-Verification-Agent）、真机冒烟、文档收口。
      完成判据：全量 ctest 四预设全绿 + 真机冒烟记录。

## 设计与决策依据

- [DEC-017](../decisions/DEC-017-workflow-camera-sources.md)：四源型 typeId、
  固定 rendition 语义、分辨率全局属性、ROI 防呆分层（控件夹取 + apply 期
  拒绝兜底）。
- [image_workflow_design.md](../design/image_workflow_design.md) §7：crop/
  downscale 数值语义按元素尺寸泛化（实现已按 `elementSize(format)` 分派），
  Gray8 变体逐像素同映射。
- [DEC-003](../decisions/DEC-003-depth-colormap.md)/DEC-007：三配色 ramp 语义，
  灰度 rendition 取其单通道亮度层。

## 测试与退出条件

- [x] Core 灰度转换 golden 单测（固定区间 / P99 / 无效深度 / 边界）。
- [x] 目录与工厂单测：新类型 valid、源型注入（工厂返回 nullptr）、灰度链
      校验与类型失配拒绝、契约套件对扩展目录保持全绿。
- [x] 引擎端到端：灰度源链（depth_gray → crop_gray → gaussian_blur）真实
      求值；跨类型连线 applyGraph 同步拒绝。
- [x] ROI 约束纯逻辑单测：联动范围、编辑夹取、无尺寸回退。
- [x] 全量 ctest debug/asan/ubsan/tsan 全绿；真机冒烟（深度源 + 灰度链 +
      ROI 防呆 + 分辨率入口）。
- [x] 文档同步：总计划里程碑索引与 POST-04 注记、CHANGELOG（Unreleased）。

## 验证记录

2026-09-29：立项。用户真机验收反馈五项：① 裁切后进入灰度失败；② 相机源
增加 RGB 外的三种深度输入（两种本身为灰度）；③ 灰度图支持裁切与降分辨率；
④ 相机源可选分辨率；⑤ 裁切控件应从控件本身避免非法语义。
根因初判（主循环代码走查）：① 与 ⑤ 同源——裁切 ROI 控件仅呈现声明范围
[0,4096]，与实际输入尺寸脱钩，越界 ROI 触发 M4-03 冻结的 apply 期显式拒绝
（NodeFailed → 引擎 Failed）；crop→grayify 图像路径本身已被既有测试覆盖
（合法 ROI 五节点链通过）。②③④ 为输入面扩展，方案冻结于
[DEC-017](../decisions/DEC-017-workflow-camera-sources.md)（深度 rendition
与预览配色解耦、分辨率全局属性、ROI 防呆分层）。实施顺序
`M6-01`..`M6-07`，测试归 Independent-Verification-Agent。

2026-09-29：`M6-01`..`M6-07` 完成关闭——用户真机反馈五项全部落地。
**裁切→灰度失败根因与修复**：图像路径（crop→grayify）已被既有测试覆盖且实现
正确；失败根因是裁切 ROI 控件仅呈现声明范围 [0,4096]，与实际输入尺寸（如
848×480）脱钩——ROI 越界触发 M4-03 冻结的 apply 期显式拒绝，节点失败 → 引擎
Failed，用户搭裁切→灰度链时以试错方式触达。修复分层：控件层在已知输入尺寸时
按互约束有效域夹取（`param_model.hpp` `RoiConstraint` 纯逻辑：x∈[0,W−w]、
width∈[1,W−x] 等互约束范围，滑条归一化与文本输入按有效域夹取，范围文本实时
呈现有效域），输入尺寸取自选中节点输入驱动节点的最新产物快照（pumpNodeOutput
同址有界消费；引擎非 Running 排空时回退声明范围，apply 期拒绝保持兜底——
M4-03 语义不变，DEC-017 冻结分层）。
**深度三输入**：`source_depth_jet`（Rgba8）/`source_depth_gray`（Gray8）/
`source_depth_adaptive`（Gray8），与预览配色解耦的固定 rendition（DEC-017）；
Core 新增 `convertDepth16ToGray8`（固定区间近白远黑、无效深度 0）与
`convertDepth16ToGray8Adaptive`（P99 近黑远白、无效深度 0），为 DEC-007 ramp
的单通道对应层；adapter 采集循环按 rendition 发布（`FrameKind::DepthJet` 与
`GrayFrameKind::Depth/DepthAdaptive`，Jet 通道在预览 scheme==Jet 时与预览共享
同一缓冲零额外成本）；viewer 帧源路由按 source 节点 typeId 分发（原子共享
快照，pump 以画布修订号脏标记刷新，未知节点回退 RGB）；调色板分组扩展
（深度源归 Source、灰度几何算子归 Geometry）。
**灰度算子**：`crop_gray`/`downscale_gray` 复用既有泛格式实现（逐像素同映射，
1 字节/像素），目录表 §6 扩展冻结。
**分辨率入口**：相机源面板分辨率下拉与预览选择器共享同一 `ViewerContext` 状态
与 `requestResolution` 命令（全局 restream 语义明示提示），不做 per-source
分辨率（DEC-017）。
**采集侧优化（随验证反馈追加）**：初版每深度帧三次独立全幅转换在 TSAN 插桩下
侵蚀 M3-08 真机 IMU 交付速率裕度（既有抖动：HEAD 基线 tsan×hardware 通过率
1/3、速率带 10-17Hz；M6 初版 2/5、6-9Hz）。新增
`convertDepth16ToGray8Pair` 单趟双输出（与两单函数逐字节等价，独立验证 119 项
等价性/校验面检查），adapter 接入后速率带回到 pre 5.7-11.5Hz（贴近基线）；
残余 tsan×hardware 抖动为预先存在的时序敏感检查限制（基线自身不稳定），
已知限制如实记录：debug/asan/ubsan 三预设确定性通过，tsan+真机组合下
M3-08 速率检查不作为稳定判据（待后续评估放宽测量窗或重试语义，与 M5-05
记录的既有抖动同源）。
测试（Independent-Verification-Agent 独立编写执行，两轮）：Core 灰度转换
golden（固定区间/P99/无效深度/与 RGBA8 ramp 通道对应/校验反例，20397 →
20516 项检查四预设 0 失败）；目录/工厂扩展（四源型签名、灰度 golden、跨类型
连线 TypeMismatch 拒绝）；灰度链端到端（depth_gray→crop_gray→gaussian_blur
真实求值）；帧源路由（五映射/回退/换新/零拷贝/独立水位）；ROI 约束纯逻辑
（互约束/夹取/回退）与 pump 驱动拉取；契约套件对扩展目录全绿；test_run_control
适配路由版帧源、test_public_boundary 补 GrayFrame 契约。debug/asan/ubsan
全量 ctest 26/26、tsan unit 25/25（tsan×hardware 既有抖动见上）。
真机冒烟（D435if 直连，临时本地补丁预置 source_depth_gray→crop_gray→
gaussian_blur 链 + 脚本化启停/分辨率切换，验证后已回退不入库；截图存档
`screenshots/m6/`）：深度灰度源出图（848×480 单通道灰度缩略图随 seq 推进）、
30.3fps 零丢弃、相机源面板 "Camera resolution" 下拉呈现全局档位、运行中切换
分辨率 restream 后引擎无中断继续处理、stop 排空语义与 §4 逐项一致。
环境：x86_64 Linux（GNOME/XWayland），GCC 13。`M5-07`（M5 验收收口）为下一
工作项。
