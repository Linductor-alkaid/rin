# DEC-020：工作流深度域（Depth32F）——米制深度算子进节点画布，废弃固定预览卡片

> 状态：Accepted
> 日期：2026-09-30
> 负责人：Linductor-alkaid
> 冻结里程碑：M10
> 替代/被替代：取代 DEC-019 决策 4（固定预览卡片显示语义）与决策 5 中的
> "tick 组件承载管线"路径；DEC-019 决策 1/2（米制 rendition 通道与契约
> 扩展）保留并成为本决策的数据源。细化 DEC-018（算子在画布的形态）。

## 背景与问题

用户验收反馈（2026-09-30，M9 预览卡片演示后）：希望在**工作流画布里自己
组装**这条管线（米制深度源 → 填充 → 降采样 → 裁切 → 模糊 → 归一化 →
历史堆叠），而不是预览页固定 "Policy Depth" 卡片——固定卡片不可组合、
不可观察中间产物，与工作台"拖拽搭图"的产品形态相悖。

约束：M4 契约的像素传输载体是 `ImageU8` 字节缓冲（`runNodeGraph`/帧源/
产物快照/统计全线按其冻结），米制深度是 float32 数据；`depth_history`
跨帧有状态，与 `IImageNode::apply` 逻辑只读契约冲突，且引擎 `maxInFlight=2`
允许帧任务并发/乱序完成（发布有序化不解决执行序）。

## 决策

1. **`PortType::Depth32F`：字节容器上的 float32 端口语义**。传输载体仍为
   `ImageU8`（缓冲为 host 端序 float32 像素序列，`elementSize = 4`、stride
   为字节数 ≥ width×4、无效深度 0.0、distance_to_image_plane 米制语义）。
   `runNodeGraph`/`NodeGraph`/引擎/产物快照管道零改动——端口类型系统本就
   按语义区分 Gray8/Rgba8 共用同一字节容器纪律，Depth32F 是该纪律的第三个
   实例。float 视图经 Core 助手（`depthF32Row` 等）显式解释，禁止散布
   reinterpret_cast。
2. **节点集（typeId 冻结，目录尾部追加）**：`source_depth_metric`（相机源，
   输出 Depth32F）、`depth_fill_invalid`（far_value=2.5、invalid_below=0.0）、
   `depth_resize`（width=64、height=36，面积加权仅缩小）、`depth_crop`
   （up=18/down=0/left=16/right=16）、`depth_gaussian_blur`（radius=1、
   sigma=1.0）、`depth_normalize`（near=0.0、far=2.5）、`depth_history`
   （history_length=37、sample_count=8、sample_skip=5、sample_delay=0；
   **有状态**）。数值语义全部复用 M8 冻结公式（depth_policy_preproc_design
   §5）；默认参数链 ≡ 冻结管线。`depth_history` 输出为抽样平面的**竖直
   堆叠**（sample_count × H 行，oldest 在顶、最新在底）——即 CHW 布局的
   平面序展示，供下游交付与缩略图显示。
3. **有状态节点执行模型（DEC-013 加性修订）**：新增标记接口
   `IStatefulImageNode`；引擎 `buildGeneration` 编译期探测，含状态节点的
   生成代以**串行在飞**执行——上一帧任务未完成时本 tick 跳过提交（staged
   最新帧语义；不计过载丢弃，同覆盖检查先例）。节点内另按输入像素缓冲
   指针同一性去重（staged 旧帧重提交不重复入环）。状态生命周期 = 节点
   实例生命周期：图替换/参数变更重建生成代即复位（显式语义，非缺陷）；
   历史内容随相机流重启的复位由帧载荷序号在引擎外不可见，不做（米制帧 0
   无效语义与训练授权的 0–1 帧延迟覆盖启动瞬态）。
   **重试边界（独立验证确认语义）**：串行门开后的 staged 提交以"下一
   捕获帧到达的 tick"为界——`!hasNew` 空转提前返回仍先于 staged 重提交；
   真实相机 30 fps 持续产帧下额外延迟 ≤ 一个帧周期（≈33 ms），不可观测，
   且引擎级去重语义（多源 staged 重提交不重复入环）依赖该行为。
4. **相机源路由**：`WorkflowSourceRendition` 增 `DepthMetric`（应用层帧源
   经 `tryLoadDepthMetric` 读取，`DepthFrameF32` float 缓冲 → Depth32F
   字节容器拷贝包装）；路由 typeId `source_depth_metric`。
5. **Depth32F 缩略图显示语义**：逐帧 P99（有效像素 v>0 的 99 分位）归一，
   无效（0）输出黑、其余 255×(1−v/P99)（近白远黑，同 DEC-007 灰度约定）；
   确定性、对米制与归一化链路均有对比度。
6. **移除 M9 固定预览卡片**：`PolicyDepthPreview` tick 组件、
   `PolicyDepthPreviewModel`、预览页第三卡片与 app 接线全部删除（被画布
   组合能力取代）；米制通道、契约与转换函数保留。

## 备选方案

- **平行 `ImageF32` 类型贯穿管线**：runNodeGraph/帧源/快照/缩略图/统计
  全线 variant 化，契约套件重写——成本与收益不成比例。否决。
- **字节域量化承载（Gray8 近似米制）**：2.5 m/255 ≈ 1 cm 量化破坏策略
  输入一致性。否决。
- **节点内 mutex 保证状态一致**：maxInFlight=2 下执行序不确定，互斥只防
  数据竞争不防乱序。否决（引擎串行在飞才是正确层）。

## 影响与风险

- 目录契约：新条目尾部追加（契约套件按"首个无输入节点"泛式构图，既有
  测试不破坏）。
- 串行在飞降低含状态图的吞吐上限（≈1 帧延迟流水）；米制链单帧成本
  ≪ 20ms tick，实测无影响——基准数据入验证记录。
- `depth_history` 参数变更即复位历史：参数面板热更新路径（帧边界重建）
  自带该行为，文档明示。

## 验证方式

- 节点 golden（复用 M8 双精度参考法）+ 引擎状态节点时序（乱序完成注入
  压力、跨代复位、staged 重提交去重）+ 帧源米制路由 + 缩略图映射。
- 三预设全量 ctest（IVA）；真机画布组装验收（用户）。

## 关联文档和工作项

- [M10 计划](../plans/m10-workflow-depth-domain.md)
- [image_workflow_design.md](../design/image_workflow_design.md) §6/§7 扩展
- [DEC-018](DEC-018-depth-policy-preproc-operators.md)（冻结公式）、
  [DEC-019](DEC-019-metric-depth-channel.md)（数据源通道）
