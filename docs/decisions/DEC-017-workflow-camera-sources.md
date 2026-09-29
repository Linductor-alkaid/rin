# DEC-017：工作流相机源四型与深度 rendition 语义

> 状态：Accepted
> 日期：2026-09-29
> 负责人：Linductor-alkaid
> 冻结里程碑：M6
> 替代/被替代：无（细化 DEC-003/DEC-007 的消费面边界）

## 背景与问题

M5-06 假换真集成后，工作流相机源只有 RGB 彩色流单一输入（M5-06 接缝决策：
深度伪彩通道不进工作流）。用户真机验收反馈要求：相机源除 RGB 外提供三种
深度输入（其中两种本身为灰度图）；灰度图可裁切/降分辨率；裁切控件能裁出
越界 ROI 导致 apply 期拒绝、引擎 Failed（控件层防呆缺失）；相机源需可输入
不同分辨率。

约束：相机服务为单 pipeline（一次 restream 一组 color/depth 模式）；预览
深度通道配色由 DEC-007 命令全局切换；`validateWorkflowGraph`/参数面板/画布
类型过滤建立在静态端口类型（`PortType::Gray8/Rgba8`）之上。

## 决策

1. **相机源四型（typeId 冻结）**：`source`（RGB，Rgba8，沿用既有 id）、
   `source_depth_jet`（深度伪彩，Rgba8）、`source_depth_gray`（深度灰度，
   Gray8）、`source_depth_adaptive`（深度自适应灰度，Gray8）。以独立 typeId
   而非"source + 参数"表达：源输出端口类型必须静态可判，灰度源直连 Gray8
   域算子（grayify 之后的灰度裁切/降分辨率）依赖静态类型校验，参数化源会
   使声明类型与运行期格式脱节（运行期注入失败 → 引擎 Failed，恰为防呆
   原则要消除的非法状态）。
2. **深度 rendition 与预览配色解耦**：三个深度源消费 adapter 发布的固定
   rendition 通道——Jet 恒为 jet 伪彩（Rgba8）、灰度恒为固定区间近白远黑
   （Gray8）、自适应灰度恒为 P99 归一化近黑远白（Gray8）；语义为 DEC-007
   三配色 ramp 的单通道对应（无效深度一律 0/黑，Gray8 无 alpha 层）。
   DEC-007 配色命令只影响预览深度通道，工作流 rendition 不随其变化。
3. **契约加性扩展**：`FrameKind` 增 `DepthJet`（Rgba8 rendition 走既有
   `tryLoadFrame`）；新增 `GrayFrame`（单通道帧：width/height/stride/
   sequence/timestamps/共享像素）与 `GrayFrameKind { Depth, DepthAdaptive }`
   及 `ICameraService::tryLoadGrayFrame`。既有 `Frame`/`tryLoadFrame` 语义
   不变。
4. **分辨率是相机流全局属性**：单 pipeline 下 per-source 分辨率是多源冲突
   的非法语义；工作流页相机源面板提供分辨率入口，与预览页选择器共享同一
   设备档位列表、同一选择状态与同一 `requestResolution` 命令（全局
   restream），不做节点参数。
5. **ROI 防呆分层**：M4-03 冻结的"crop ROI 退化/越界 apply 期显式拒绝"语义
   不变（兜底）；控件层新增防呆——已知输入尺寸（输入驱动节点最新产物快照）
   时，x/y/width/height 按联动约束（x∈[0,W−w]、width∈[1,W−x]、y/h 同理）
   夹取并呈现有效域；未知尺寸（引擎未运行/无快照）回退声明范围。

## 备选方案

- 源节点参数化选择输入流（单一 `source` + Enumeration 参数）：被否——静态
  端口类型无法随参数变化，运行期格式失配转引擎失败，违反防呆原则（§决策 1）。
- 多态端口（PortType 通配 + 类型推断）：被否——`M4-09` 契约、校验器、画布
  即时类型过滤与参数面板均建立在静态类型上，推断机制为本次需求引入的复杂度
  不成比例；`crop`/`downscale` 实现已按元素尺寸泛化，独立 Gray8 typeId 零
  语义成本。
- 工作流灰度输入经预览深度帧派生（取 R 通道等）：被否——rendition 依赖
  预览配色状态，语义不确定（§决策 2）。
- ROI 越界改为 apply 期静默夹取：被否——改变 M4-03 冻结语义（显式拒绝、
  禁止静默），且静默夹取掩盖语义错误；防呆归控件层。

## 影响与风险

- 采集循环每帧增加固定 rendition 计算（jet + 两种灰度，848×480 实测量级
  数毫秒）：Jet 通道在预览 scheme==Jet 时与预览共享同一缓冲（零额外成本）；
  真机冒烟复核采集帧率与 RSS。
- `ICameraService` 增加纯虚函数为破坏性接口变更（实现方需同步）：仓库内
  实现方仅 realsense adapter 与测试假服务，编译期可见。
- 节点目录扩展（4→13+2 型）：契约套件以"目录首个无输入节点"泛式构图，
  新类型追加于既有条目之后即天然兼容；调色板 Source/Geometry 组条目增多，
  面板几何不变。

## 验证方式

- Core 灰度转换 golden 单测（与 DEC-007 ramp 单通道对应、无效深度、边界）。
- 目录/工厂单测与契约套件（扩展目录全绿）；灰度链端到端（真引擎求值）。
- ROI 约束纯逻辑单测（联动范围/夹取/回退）。
- 真机冒烟：四源型出图、灰度链、ROI 滑条越界不可达、分辨率入口生效
  （预览选择器同步）。

## 关联文档和工作项

- 总计划：`SCOPE-08`、`POST-04`、里程碑索引。
- [m6-workflow-input-expansion.md](../plans/m6-workflow-input-expansion.md)：
  `M6-01`..`M6-07`。
- [image_workflow_design.md](../design/image_workflow_design.md) §6/§7、
  [camera_service_design.md](../design/camera_service_design.md) 公共契约节。
- [DEC-003](DEC-003-depth-colormap.md)、DEC-007（预览配色命令边界）。
