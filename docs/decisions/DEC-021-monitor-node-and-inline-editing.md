# DEC-021：监看器节点、Any 端口类型与画布内嵌参数编辑

> 状态：Accepted
> 日期：2026-10-06
> 负责人：Linductor-alkaid
> 冻结里程碑：M11
> 替代/被替代：无（细化 DEC-014 五区信息架构与 DEC-015 画布实现路径的交互层）

## 背景与问题

用户需求（2026-10-06）：节点画布参照 ComfyUI / Unreal 蓝图范式——① 画布中
每个节点块内直接显示并编辑自己的参数（context 就地化）；② 调色板提供"监看器"
（monitor）节点，任意输出线均可接入其输入，输入图像直接显示在监看器节点内的
预览窗口处，监看器无输出端口；③ 移除右侧上下文面板。

约束与现状：

- 端口类型系统（`PortType::Gray8/Rgba8/Depth32F`）要求连线两端类型相等，
  监看器"接受任意输出"无法用现有三型表达；以"每类型一枚输入端口"表达则与
  "每输入恰一条入边"校验冲突（未用的输入口悬空被拒）。
- 引擎产物通道（per-node `LatestMailbox<NodeOutputSnapshot>`，M7-02 跨代共享）
  以 `runNodeGraph` 的节点输出为发布源；无输出端口的节点没有产物可发布。
- 画布指针语义由覆盖视口的一层 `components::mouseArea` 经纯逻辑命中分发
  （DEC-015 决策 2），节点内嵌交互控件（滑条/输入/开关）若被该层遮挡则不可
  操作。
- 参数热更新走 `requestParamUpdate`（DEC-013/DEC-014），ROI 联动约束
  （DEC-017 控件层防呆）依赖输入驱动节点的最新产物尺寸。

## 决策

1. **`PortType::Any` 通配端口类型（契约加性扩展）**：`Any` 只出现在目录声明
   位（端口签名），表达"接受任意具体类型"的输入端口；实际图像格式永不为
   `Any`（`elementSize(Any) == 0`，`ImageU8::make/wrap` 的元数据校验天然拒绝）。
   连线兼容性判据由"两端 `PortType` 相等"扩展为"相等，或输入端口声明为
   `Any`"；输出为 `Any` 的端口只能连入 `Any` 输入（判据自然给出，`Any` 输出
   连具体类型输入仍为 `TypeMismatch`）。`runNodeGraph` 防御性核对同步：声明
   `Any` 的输出端口接受任意具体格式（注入路径不涉及：注入型源节点声明具体
   类型）。
2. **监看器节点 `viewer`（Any→Any 透传 sink）**：目录新增
   `viewer`（"监看器"），签名 1 输入 `Any` + 1 输出 `Any`，无参数。Core 实现
   为恒等节点（`apply` 返回输入，共享像素零拷贝）——引擎零改动，产物快照经
   既有 per-node 邮箱照常发布。**UI 画布隐藏其输出端口**（纯逻辑 `portAt`
   同步跳过，不可见即不可拖线）：契约层监看器是透传节点，UI 层它是末端 sink，
   用户诉求"无输出端口"呈现在交互面。位置：目录追加于既有条目之后（保持
   "source 居首"的契约套件假设）。
3. **画布内嵌参数编辑（交互范式迁移）**：节点框纵向结构冻结为
   `标题行 → 参数区（按声明序）→ 端口区（左右缘，现有语义）→ 页脚（耗时徽标）`。
   控件映射（复用 §5.5 既有纪律与 `param_model` 纯逻辑）：Boolean→`toggleSwitch`、
   带范围 Integer/Real→`slider`+当前值文本（目录现存标量参数全部带范围）、
   Enumeration→点击循环到下一选项的值胶囊（提交路径同 `requestParamUpdate`）、
   RealArray→紧凑矩阵网格（`RealArrayGrid` 形状语义不变）；就地报错沿用
   `control.error`。ROI 联动约束沿用，尺寸源改为 per-node 驱动尺寸缓存
   （见 5）。节点几何（含 RealArray 行数随实例形状变化）由
   `nodeHeight(descriptor, params)` 纯函数族计算，命中/框选/帧全图同源。
4. **指针分发次序反转（画布内嵌控件可行性基础）**：画布全视口 `mouseArea`
   改为先合成（视觉层之下），节点与其内嵌交互组件后合成。依据（pinned
   EUI-NEO 4691fc0 源码取证，`core/runtime/runtime_input.h`）：命中测试按
   "子先于父、兄弟按 zIndex 稳定排序后合成优先、仅 interactive 元素匹配"
   自顶向下返回首个命中——内嵌控件优先接收事件，节点标题/卡片等非交互视觉
   穿透至画布 mouseArea（拖动/框选/连线语义不变）。滚轮缩放不受影响
   （`hitTestScrollable` 谓词仅匹配滚动元素，控件不匹配则穿透）。
5. **右栏移除与信息再安置**：工作流页由五区改四区（顶工具栏/左调色板/中画布/
   底校验事件）。工作流总览（end-to-end FPS/processed/dropped/in flight、
   过载警示、冻结标注，§5.7 语义不变）迁至底部区右列；选中节点中间结果
   缩略图由监看器节点替代（每监看器一枚 `GpuFrameView`，pump 拉取
   `tryLoadNodeOutput` 上传，非 Running 整体排空释放——§4 停止/关闭排空语义
   不变）；相机分辨率全局入口（DEC-017 决策 4）内嵌到相机源节点（点击循环
   档位，与预览页共享同一选择状态与 `requestResolution` 命令）；节点页脚
   耗时徽标升级为 `last · avg` 双值。右栏参数面板与 Context 输出块（含其
   docking 分隔条）删除。
6. **枚举点击循环**：画布内枚举参数与分辨率入口采用"点击切换到下一选项"
   （小选项集 2..5 个即时可遍历；下拉弹层在画布 `clip` 视口内会被裁剪，
   overlay 浮层则引入全局单例状态，循环切换是两者中侵入最小的等价表达）。

## 后果

- `validateWorkflowGraph`、`runNodeGraph`、`elementSize`、`toString(PortType)`
  与 UI 端口色（`Any`→中性次级色）需同步扩展；契约测试（Any 判据、viewer
  透传 golden、目录签名）随本决策入库。
- 画布节点高度不再由端口数唯一决定（参数区/预览区/RealArray 形状参与），
  `nodeHeight/nodeBounds/portPosition` 纯逻辑签名扩展，画布单测同步。
- 缩放下限 0.4 时内嵌控件命中区按比例缩小（ComfyUI/蓝图同款行为：缩放是
  总览，编辑在放大档进行），不设特殊补偿。
- UI 私有呈现知识新增 `kMonitorTypeId == "viewer"`（与 `paletteGroupFor`
  同类，不入契约）；若未来出现更多 sink 类型再考虑目录层呈现元数据。

## 关联

- [M11 计划](../plans/m11-canvas-monitor-inline.md)
- [工作台 UI 设计](../design/ui_workspace_design.md)（§2/§5.5/§5.6 补记）
- [图像工作流设计](../design/image_workflow_design.md)（§3.2/§6 目录表同步）
- [DEC-014](DEC-014-workbench-information-architecture.md)、
  [DEC-015](DEC-015-node-editor-implementation-path.md)、
  [DEC-017](DEC-017-workflow-camera-sources.md)、
  [DEC-013](DEC-013-workflow-execution-model.md)
