# M9：米制深度通道贯通——adapter 发布、契约扩展与 viewer 策略深度预览

> 状态：In Progress
> 负责人：Linductor-alkaid（授权 Agent 按工程规范自治执行）
> 所属计划：[Rin 实施总计划](rin-implementation-plan.md)
> 前置：M8（`rin/depth_preproc.hpp` 算子）、M6（rendition 通道发布先例）
> 立项依据：用户指示（2026-09-30，M8 合并后）"开始推进继续，更新viewer"
> 建议发布点：v0.6.0（与 M8 合并发布）
> 更新日期：2026-09-30

## 目标

把 M8 冻结管线接到真机数据上并在 viewer 呈现：adapter 采集 worker 发布
米制深度（Z16×depth_scale，float32），相机服务契约加性扩展
`tryLoadDepthMetric`，viewer 应用层以 Executor 周期 tick 驱动冻结管线
（O6 组合 + O7 时序抽样），预览页新增"Policy Depth"卡片显示 8 帧历史
网格与处理统计——用户可在真机流上直接目检策略输入管线的输出。

## 范围与非目标

范围：

- [DEC-019](../decisions/DEC-019-metric-depth-channel.md)：米制深度
  rendition 通道、契约扩展、Executor tick 预览组件、显示语义。
- adapter：`convertDepth16ToMetric` 纯转换（可独立单测）+ 采集循环发布。
- Core：`PolicyDepthPreviewSnapshot`/`PolicyDepthPreviewModel`（历史网格
  渲染，纯逻辑可 headless 单测）。
- viewer：`PolicyDepthPreview` tick 组件（无 EUI 类型，可脚本化单测）+
  预览页第三卡片（复用 composeViewCard/GpuFrameView）+ 关闭顺序接线。

非目标：

- ONNX 推理与策略运行时（张量交付、复位策略、配置面）——另行立项。
- 预览页以外的 UI 面（工作流页不动）。
- 米制帧的对齐（color↔depth alignment，POST-01 范畴）。

## 工作项

- [x] `M9-01` 设计冻结：[DEC-019](../decisions/DEC-019-metric-depth-channel.md)、
      [camera_service_design.md](../design/camera_service_design.md) 通道段。
      完成判据：`Accepted` 且设计文档结构检查通过。
- [x] `M9-02` 契约扩展 + adapter 发布（DepthMetricSample/
      tryLoadDepthMetric/convertDepth16ToMetric/采集循环发布/Fake 同步）。
      完成判据：四预设编译零新增警告；fake 通道语义可用。
- [x] `M9-03` Core 预览模型 + viewer tick 组件 + 预览页卡片 + 关闭顺序。
      完成判据：四预设编译零新增警告；模型纯逻辑可 headless 验证。
- [x] `M9-04` 测试矩阵（Independent-Verification-Agent）+ tsan 专项 +
      文档收口。完成判据：三预设全绿零消毒器诊断。真机（D435if）米制
      通道冒烟待用户验收（viewer 已带新卡片交付）。

## 风险与阻塞

- 预览纹理上屏走 EUI-20260923-003 绕行路径（外部 GL 导入），与既有视图
  同风险面（已在 M5 验证）。
- tick 组件生命周期与 shutdown 顺序：按 DEC-019 决策 3 的顺序接线，
  shutdown_drain 套件补回归。

## 测试与退出条件

- 转换/契约/模型/tick 四层单测全分支；debug/asan/ubsan 三预设全绿。
- 真机（D435if）冒烟：卡片出图、序号与速率推进、restream 复位、关闭排空。
- 文档同步矩阵落盘（总计划/CHANGELOG/设计文档）。

## 验证记录

- 2026-09-30 `M9-01`：[DEC-019](../decisions/DEC-019-metric-depth-channel.md)
  冻结（`Accepted`），camera_service_design.md 契约段同步（八通道）。
- 2026-09-30 `M9-02`/`M9-03`：契约扩展（DepthMetricSample + tryLoadDepthMetric
  纯虚，Fake/Null 同步）、adapter 米制通道（convertDepth16ToMetric 纯头 +
  采集循环发布）、Core 预览模型（PolicyDepthPreviewModel，2×4 网格冻结布局）、
  viewer tick 组件（Executor 20 ms 周期 + 弱引用 + busy RAII 守卫 + 流重启
  复位）与预览页第三卡片 + 关闭顺序接线；四预设编译 Rin 自研源码零警告。
- 2026-09-30 `M9-04`：独立验证两轮。首轮暴露实现缺陷 2 处（D2：ImageU8
  stride 字节语义误用像素数致预览模型恒抛——阻断；D1：start() 准入异常未
  转译 false）与测试侧缺陷 1 处（D3：fake 以帧内序号做水位，与真实 adapter
  邮箱发布序号语义不符，流重启用例不可达）。主循环修复 D1/D2，验证方修正
  D3 后复验 PASS：debug/asan/ubsan 三预设全量 ctest **30/30 全绿**
  （depth_metric_convert 60 项、depth_policy_preview 122 项、
  policy_depth_component 87 项、public_boundary 82 项检查），零消毒器诊断；
  tsan 专项四套件全绿（环境 ASLR 需 `setarch -R`，为宿主内核与 TSAN 兼容
  问题、非代码缺陷）。真机米制通道冒烟（卡片出图/序号速率推进/restream
  复位/关闭排空）待用户验收。
