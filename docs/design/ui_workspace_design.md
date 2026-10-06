# 工作台 UI 设计：信息架构与节点编辑器

> 状态：Active
> 更新日期：2026-10-06
> 依据：[DEC-014](../decisions/DEC-014-workbench-information-architecture.md)、
> [DEC-015](../decisions/DEC-015-node-editor-implementation-path.md)、
> [DEC-005](../decisions/DEC-005-viewer-visual-design.md)、
> [DEC-016](../decisions/DEC-016-contract-first-workbench-order.md)、
> [M4-09 视图契约](../../include/rin/workflow_types.hpp)、
> [ui_workspace_research.md](ui_workspace_research.md)（调研依据）
> 实施工作项：[m5-ui-workbench.md](../plans/m5-ui-workbench.md)（`M5-02`..`M5-08`）

## 1. 定位与范围

本文冻结工作台的界面信息架构与操作逻辑：页面拓扑树、导航模型、状态与空态、
节点编辑器交互流、EUI-NEO 原语映射。它是 `M5-02`..`M5-05` 的实现依据；视觉
取值不在本文重复——一律遵循
[viewer 设计系统基准](viewer_design_system.md)与 `viewer_theme.hpp` 唯一令牌层
（新增视觉需求先扩令牌，禁止裸值）。数据契约（图模型、参数、统计、运行控制）
以 M4-09 头文件为准，本文只定义其 UI 呈现与交互，不重复定义。

M5 骨架对契约假引擎（`M5-08`）开发调试（DEC-016）；性能数字以真引擎实测为准。

## 2. 页面拓扑树

```
AppShell（单窗口，EUI-NEO DslAppConfig）
├─ 全局导航栏：预览 | 位姿 | 图像工作流 | 设置（图标+文字，左窄边）
├─ Page 预览
│   ├─ 画面区：RGB 卡 + 深度卡（现 viewer 布局，GpuFrameView 路径）
│   ├─ 设备与流配置卡：设备选择（DEC-006 Waiting 稳态）、分辨率
│   │   （深度配色按 DEC-014 决策 3 于 `M5-02` 落地设置页，本卡不再含配色入口）
│   └─ 内参卡
├─ Page 位姿：3D 位姿视图卡（dirtyKey 绕行）+ IMU 状态面板（现 M3 布局）
├─ Page 图像工作流（四区骨架，DEC-014；M11/DEC-021 移除右栏改四区）
│   ├─ 顶：工具栏（启动/停止、引擎状态徽标、校验状态、待生效、帧全图）
│   ├─ 左：节点调色板（NodeCatalog 分类分组 + 搜索框 + 滚动）
│   ├─ 中：节点画布（视图变换 + 节点（内嵌参数控件）+ 监看器节点（内嵌预览）
│   │        + 连线 + 选择框 + 右键菜单）
│   └─ 底：左列校验问题与事件列表（可折叠）+ 右列工作流总览（M11 自右栏迁入）
└─ Page 设置：深度配色切换（DEC-007 命令通道）、关于/版本信息
```

四页共享相机服务运行态；工作流页的启动/停止只作用于工作流引擎
（`IWorkflowEngine`），不改变相机采集。工作流页无节点画布之外的独立
"运行页"——运行控制内嵌工具栏（调研结论：运行/调试不换窗口）。

## 3. 导航模型

- 导航控件：设计首选 EUI-NEO `sidebar`，`M5-02` 原型验证结论为右锚定模态
  抽屉（非左侧常驻形态），`tabs`/`segmented` 为横向选择器，`navbar` 未列入
  上游组件文档且绑定组件库自有主题度量体系（与 DEC-005 唯一令牌层冲突）——
  按 [EUI-20260928-001](../dependency_feedback/eui-neo/ledger.md) 以 `rect`/
  `text` 原语 + viewer 令牌自绘左窄边导航栏（`navigation.hpp`），信息架构
  不变（DEC-014 允许的控件退化路径，台账登记后实施）。
- 页面切换保持各页 UI 状态（工作流页的视图变换、面板开合、选中态是
  `ui.state` 会话状态，不因导航丢失；导航模型只拥有当前页，不拥有页面内
  状态——`navigation.hpp` `NavigationState`）。
- 切离工作流页时引擎继续运行（引擎侧承载，`RULE-07`）；返回时经契约
  `tryLoadStats`/`tryLoadNodeOutput`/`tryLoadEvent` 最新态语义重新消费，
  以 `sequence` 推进刷新，禁止以 stale 快照恢复活动状态。页面不驻留
  定时器——刷新发生在 compose 边界取快照（`RULE-05`：渲染线程只做有界
  校验、取快照与提交）。
- 键盘导航：沿用 DEC-005 keyboard-first 原则；画布快捷键（`F` 帧全图、
  `Del` 删除选中、`Esc` 取消拖拽/收起菜单——`M5-03` 落地补充）依赖 EUI-NEO
  键盘事件暴露，`M5-03` 原型确认，不足时按台账流程登记（DEC-015 风险条目）；
  每个快捷键必须有鼠标等价路径。M1 的数字键分辨率速选随壳收窄到预览页
  （控件所在页）。

## 4. 状态与空态

| 场景 | 呈现 |
| --- | --- |
| 工作流页空图 | 画布中央引导卡："从左侧拖入节点开始"；不弹模态 |
| 相机 Waiting（无设备） | 沿用 DEC-006 稳态；工作流页允许搭图，启动被引擎拒绝时按准入错误呈现（`AdmissionResult.error`） |
| 引擎 Idle | 工具栏状态徽标 fgSubtle；启动按钮可用条件=当前图校验通过 |
| 引擎 Running | 徽标 success；停止可用；参数编辑即改即生效（明示"下一帧生效"）；图结构变更显示"待生效"，`GraphApplied` 事件后清除 |
| 引擎 Stopping | 徽标 warning；控件禁用至回到 Idle |
| 引擎 Failed | 徽标 destructive + `lastError`/事件列表定位；恢复路径=修复后重新启动（`start` 幂等准入） |
| 校验未通过 | 画布问题节点 destructive 描边；底列表逐条 `ValidationIssue`（kind + node 定位 + message），点击定位节点 |
| 节点执行失败 | `NodeFailed` 事件对应节点 destructive 徽标 + 消息入事件列表 |
| 停止/关闭排空 | `stop()` 返回后：快照缩略图清空、性能面板冻结为末次值并标注"已停止"；不得以 stale 数据显示活动状态（`workflow_engine.hpp` 契约） |

状态徽标颜色一律经 `theme::stateColor()` 语义映射扩展（先扩令牌/映射表，
再使用），与 viewer 现有 Streaming/Failed 映射同源。

## 5. 节点编辑器交互流

### 5.1 视图变换与坐标系

画布持有一套视图变换（平移偏移 + 缩放因子，`ui.state` 会话状态；缩放有
上下限），屏幕坐标与画布坐标互转为平台无关纯逻辑。节点位置只存在于画布
UI 状态（DEC-014 决策 5：不入契约）。缩放=滚轮（以指针为锚点）；
`F`/工具栏按钮=帧全图（全部节点落入视口）。

> M5-03 实施补记（2026-09-28）：本节原文"平移=拖空白处"与 §5.4"拖空白框选"
> 对左键拖空白语义冲突，实施时冻结消解为：**左键拖空白=框选**（§5.4 选择流
> 优先，Blender/TouchDesigner 范式），**中键拖拽=平移**；全部指针语义经画布
> 纯逻辑命中分发（DEC-015），命中优先级 端口 > 节点 > 连线 > 空白。§6 的
> 右键菜单与调色板过滤框映射同步落地为：右键创建菜单为自绘浮层（沿用
> EUI-20260928-001 的 rect/text 退化先例，未用 `components::contextMenu`），
> 调色板过滤框用 `components::input`（样式字段逐一取 viewer 令牌，不引入
> 组件主题度量）。

### 5.2 节点创建

1. **调色板拖出**：按住 `NodeDescriptor` 条目拖入画布松手落点创建
   `NodeInstance`（id 由图构造方分配，非零唯一）；拖拽过程显示类型名跟随
   光标。
2. **画布右键菜单**：右键空白处弹搜索菜单（同 `NodeCatalog` 数据源，即输
   即筛），选中后在该点创建。
3. 创建即选中该节点并聚焦右面板（"先选中、后显参"心智，影刀范式迁移）。
   调色板按 `NodeDescriptor` 分组展示（几何/滤波/频域/直方图），顶部过滤框
   即输即筛；分组与过滤为纯逻辑模型。

### 5.3 连线

- **发起**：从端口拖出（端口命中区大于视觉尺寸）；仅允许 Output→Input。
- **类型过滤**：拖线中即时过滤——目标端口与拖线 `PortType` 一致才高亮可
  落点，不一致显式不可落（调研：UE 兼容绿勾/拒绝指示的转译；Gray8/Rgba8
  各配一枚端口类型色，M5-02 扩令牌）。
- **落点**：落在兼容 Input 端口上建立 `Connection`（该输入已有入边时替换
  旧边——契约单驱动约束的 UI 表达，替换需可见：旧边移除动画或即时消失）；
  落在空白处仅取消拖线（P1 再评估"松手弹创建菜单"）。
- **删除**：`Alt+点击` 连线删除；选中节点删除时随删关联边。
- **走线**：输出右缘中点 → 输入左缘中点，三次贝塞尔按水平距离拟合，
  `polygon` 采样渲染；端口按 `NodeDescriptor` 签名序号纵向排布。

> M5-04 实施补记（2026-09-28，真机视觉反馈）：`polygon` 原语为填充语义
> （点集三角形扇、末点直连首点），开放曲线点集渲染为"弦线与曲线围成的封闭
> 区域"而非线条。走线实现为贝塞尔采样中心线 + 逐点切线法向偏移 ±width/2 的
> 带状封闭轮廓（`canvas_model.hpp` `wireRibbon`，宽度 `kWireWidth`=2.5 画布
> 单位、随缩放与节点图形同比例），粗细一致且随曲线弯曲；与 M3 位姿视图
> `segmentToQuad` 线段四边形同一先例。命中检测仍用贝塞尔采样折线距离
> （`kWireHitDistance` > 半宽，几何同源）。拖线预览同用带状轮廓；光标停回
> 发起端口的零长度情形（仅预览可达，已提交连线拒绝自连）不绘制。
- 画布即时反馈合法性与引擎准入一致：UI 预检复用 `validateWorkflowGraph`
  （契约"UI 预检与引擎准入共用唯一判据"），预检不过的图禁用启动并把问题
  映射到画布标注。

### 5.4 选中与删除

- 点击节点/连线单选（顶层优先）；拖空白框选；`Shift` 加选；点击空白清除
  选中。选中态用 brand/弱强调底 + 描边。
- `Del`/右键删除选中节点（连带其连线）；删除是纯 UI 图操作，可即时撤销
  能力不在 M5 范围（DEC-015 P2），删除前不弹确认（有校验与事件列表兜底）。
- 任何被拒绝的图操作（类型不匹配、成环、多驱动等）以 toast/底列表反馈
  具体原因，不静默失败。

### 5.5 参数编辑（作用域与控件映射）

- **作用域**：右面板只编辑**当前选中节点实例**的参数（`ParamAssignment`）；
  未赋值参数显示声明默认值；编辑即经 `requestParamUpdate` 逐参数提交，
  同步拒绝（未知节点/参数、类型不匹配、越界）就地报错。类型化控件由
  `ParamDescriptor` 驱动（绑定逻辑为纯逻辑单测对象）：

  | ParamKind | 控件 | 约束呈现 |
  | --- | --- | --- |
  | Boolean | `switch` | — |
  | Integer | `slider` + `input` | hasRange 时夹取并显示区间 |
  | Real | `slider` + `input` | 同上 |
  | Enumeration | `dropdown`（`bindOpen`/`onOpenChange` 接线，台账 EUI-20260923-002 语义） | 选项取 `enumOptions` |
  | RealArray | 自研矩阵网格编辑器（行列数可配，如 KxK 卷积核） | 全部有限值校验（契约） |

- 运行中编辑即时生效（下一帧语义），面板顶部常驻"参数下一帧生效"提示；
  图结构变更（增删节点/连线）在 Running 下走"待生效 → `GraphApplied` 已
  生效"状态条。

> M5-04 实施补记（2026-09-28）：本节控件映射按如下方式落地——Boolean 为
> `toggleSwitch`、Integer/Real 为 `slider`（hasRange 时归一化映射、Integer 取整、
> 显示区间文本）+ `input`（即输即校验）、Enumeration 为 `dropdown`（`bindOpen`
> 外接开合，台账 EUI-20260923-002；选中后组件自动收起弹层）、RealArray 为自研
> 矩阵网格（行列步进 +R/-R/+C/-C 与单元文本输入，形状不入契约；初值形状按扁平
> 数据完全平方数取 KxK、否则 1xN，重排尺寸行主序截断/零扩展）。组件接线纪律：
> 颜色样式字段逐一取 viewer 令牌（SliderStyle/SwitchStyle/DropdownStyle/
> InputStyle 显式赋值），组件几何度量（下拉标签字号/条目高度、开关行程）沿用
> 组件默认——与 §6 调色板过滤框"显式字号/inset"并列的第三种纪实现场；阴影仅
> 用于下拉弹层浮层。`slider`/`toggleSwitch`/`dropdown` 构建器无 position，统一
> 包裹一层定位 stack；下拉弹层浮于后续参数行之上依赖同父容器 zIndex（展开时
> 抬升包裹 stack，DEC-005"zIndex 不跨父容器"）。参数提交 = `requestParamUpdate`
> 逐参数（同步拒绝就地报错）+ 引擎接受后记入画布模型（图结构变更时随
> `applyGraph` 携带，图重建不丢参数）；**不经 `applyGraph`**——Running 下重复
> 图重建触发事件刷屏（假引擎已知重建语义，M4-07 对齐项）。运行中滑条连续提交
> 受参数命令队列容量约束，满时同步拒绝并就地报错（EXEC-07 显式呈现，不静默）。
> 面板参数行超出参数区下限（底部缩略图块锚定）截断并以"+N more parameters"
> 计数提示，不与缩略图重叠。控件回调按值捕获节点 id，不持有画布节点指针
> （画布图变更会重排节点数组）。

> M11 实施补记（2026-10-06，DEC-021）：本节作用域由"右面板只编辑当前选中
> 节点"改为**画布节点内嵌编辑自己的参数**（ComfyUI/蓝图范式；右面板删除）。
> 节点纵向结构 = 标题行 → 参数区（按声明序）→ 端口区 → 页脚；几何为纯函数
> `nodeHeight/nodeBounds/portPosition(descriptor, params)`（RealArray 行数随
> 实例形状，绘制与命中共用度量）。控件映射调整：Boolean→`toggleSwitch`（尺寸/
> 字号随画布缩放）、带范围 Integer/Real→`slider`+标签行右对齐当前生效值
> （目录现存标量参数全部 hasRange，文本输入路径不再呈现；ROI 联动约束沿用，
> 有效域映射 M6-06 语义）、Enumeration→**点击循环胶囊**（值 + "›"指示，下一
> 选项环绕；下拉弹层在画布 `clip` 视口内会被裁剪，overlay 浮层需全局单例
> 状态，循环切换为两者中侵入最小的等价表达，DEC-021 决策 6）、RealArray→
> 紧凑矩阵网格（形状行 R+/R-/C+/C- 步进 + 单元 `input`，M5-04 网格同语义）。
> 提交路径不变（`requestParamUpdate` + 画布模型记账，下一帧生效）；同步拒绝
> **不改变行布局**（几何纯函数不含动态错误行）：参数行标签区右侧 destructive
> "!" 标注 + 完整原因经画布反馈行（工具栏）显式呈现。控件回调按值捕获节点
> id；控件状态按节点持有（`WorkflowCanvasState::controls`，节点删除经
> `afterGraphChange` 清扫）。缩放下限 0.4 时控件命中区按比例缩小（缩放为总览、
> 放大编辑，ComfyUI/蓝图同款行为），不设特殊补偿。相机源节点参数区首行为
> 全局分辨率入口（点击循环档位，与预览页共享档位/状态/`requestResolution`
> 命令，M6-06 语义不变）。"参数下一帧生效"常驻提示随面板删除（待生效标注
> 仍由工具栏呈现图结构变更）。

### 5.6 中间结果查看

- 选中节点 → 右面板底部显示该节点最近 `NodeOutputSnapshot` 缩略图
  （`tryLoadNodeOutput`，每节点仅最新一幅，UI 侧不做有界缓存之外的保留）；
  缩略图按格式渲染（Gray8/Rgba8），随 `sourceSequence` 推进刷新，并显示
  分辨率与源帧序号。
- 悬空输出端口的节点天然可查看（契约允许悬空输出），作为"末端结果预览"
  的主要用法（对应 Blender Viewer 节点语义的轻量替代）。
- 上屏走 GpuFrameView 绕行路径（台账 EUI-20260923-003），缩略图降采样在
  提交侧完成（有界工作，`RULE-05`）。

> M5-04 实施补记（2026-09-28）：缩略图路径落地为 `NodeOutputCache`（每节点仅
> 最新一幅、容量 4 LRU 有界、引擎非 Running 整体排空——§4"停止/关闭排空"的
> pump 边界实现）+ `thumbnailRgbaFromSnapshot`（最近邻降采样、只缩不放、最长
> 边 ≤256，Gray8 复制为灰度 RGBA、Rgba8 处理 stride，sourceSequence 透传）+
> GpuFrameView 上传；meta 显示"宽 x 高 · seq N"。节点执行失败（`NodeFailed`）
> 经会话级失败标注呈现为画布节点 destructive 徽标 + 面板失败徽标（含最近失败
> 消息）+ 底部事件列表条目；`Started`/`Stopped` 事件清空标注（新会话/会话结束）；
> 事件通道为最新态语义，密集事件下可能漏读中间事件（契约允许），漏读只影响
> 标注时效不影响引擎行为。

> M11 实施补记（2026-10-06，DEC-021）：中间结果查看改为**监看器节点**
> （typeId `viewer`，调色板 View 分组）：Any 输入端口接受任意输出线（契约
> `PortType::Any`，类型过滤高亮恒兼容），节点内嵌预览窗直接显示输入图像
> （pump 逐监看器 `tryLoadNodeOutput` 拉取 + `thumbnailRgbaFromSnapshot`
> （最长边 ≤512）+ 每监看器一枚 `GpuFrameView` 上传，meta"宽 x 高 · seq N"
> 悬浮预览右上）；UI 无输出端口（纯逻辑 `portAt` 跳过，不可拖线）。契约层
> 监看器为 Any→Any 恒等透传（引擎零改动，产物走既有 per-node 邮箱），多监看器
> 并联可同时查看任意中间节点。选中节点缩略图/`NodeOutputCache`/右面板随 M11
> 删除；引擎非 Running 预览整体排空释放（§4 停止/关闭排空语义不变，GL 释放
> 仅渲染线程）；节点删除条目同步清理。悬空输出端口的"末端结果预览"用法由
> 监看器显式替代（Blender Viewer 节点语义的正名）。

### 5.7 执行状态与性能面板

- 工具栏徽标：`WorkflowEngineState` 四态（状态色经 `stateColor()` 扩展）。
- 右面板"工作流总览"（无选中时）：端到端 FPS、累计处理/丢弃帧、在飞任务数
  （`WorkflowStats`）；丢弃计数非零时以 warning 提示"过载丢弃中"（EXEC-07
  显式丢弃的可观察面，禁止静默）。
- 每节点耗时徽标：节点框底部 `avgCostMs`（`NodeStats`，滚动窗口均值），
  以等宽对齐小字呈现（DEC-005 mono 替代纪律）；选中节点在右面板展开
  last/avg/执行帧数。
- 逐节点实时"执行中"高亮不在 P0（契约无该状态，DEC-015 决策 4）。

> M5-05 实施补记（2026-09-28）：性能面板落地为 `apps/viewer/perf_model.hpp`
> `WorkflowPerfState` 统计管道 + 三处消费（§7"统计管道"测试对象）。pump 边界
> 经契约 `tryLoadStats` sequence 推进消费（最新态语义，中间快照合并，已见序号
> 水位只前进）；**停止/失败冻结与快照缩略图的排空不同**——末次值保留呈现，
> 活动/冻结由 `live`（引擎 `state()==Running`）区分，stale 数值只在冻结标注下
> 出现（§4"冻结为末次值并标注已停止"）；`clear()` 关闭排空保留序号水位——
> 复位会使排空后的消费复活 stale 快照（独立验证实证缺陷，已修复并加回归守卫；
> 新会话衔接依赖统计通道序号引擎实例内单调）。呈现侧：工具栏状态徽标为四态
> 点标 + 文本（色经 `workflowStateColor`，M5-02 扩展；启动/停止按钮归 §5.8
> M5-06）；节点耗时徽标绘于节点框底部页脚带（`%.1f ms` 统一一位小数替代等宽，
> 字号随画布缩放，无统计不绘制）；右面板总览四行（end-to-end/processed/
> dropped/in flight）数值右对齐固定列，dropped>0 追加 warning 行
> "overload - frames are being dropped"，非 Running 标注 "stopped"，从未运行
> 显示空态文案；选中节点面板头部展开 last/avg/执行帧数一行（与节点错误徽标、
> "下一帧生效"提示同列共存）。

> M6 实施补记（2026-09-29，[DEC-017](../decisions/DEC-017-workflow-camera-sources.md)）：
> 调色板扩展为四源型（相机源 RGB/深度伪彩/深度灰度/深度自适应——独立 typeId
> 保证端口类型静态可判）与 Gray8 域裁切/降分辨率（"裁切（灰度）"/"降分辨率
> （灰度）"）。裁切 ROI 控件防呆：已知输入尺寸（输入驱动节点最新产物快照，
> pump 同址有界消费）时 x/y/width/height 按互约束有效域夹取并呈现有效区间，
> 滑条映射与文本输入均在有效域内（越界从控件不可达）；未知尺寸回退声明范围，
> M4-03 apply 期显式拒绝保持兜底。相机源节点参数面板新增"Camera resolution"
> 下拉：与预览页选择器共享同一档位列表/选择状态/`requestResolution` 命令
> （全局 restream，提示文案明示"global - rebuilds the camera stream"）。

> M7 实施补记（2026-09-29）：调色板条目区改为 pinned `components::scrollView`
> 滚动容器（小窗口/目录扩展时条目可滚动，过滤词作 contentKey 驱动重测量，
> 滚动偏移绑定会话信号切页保持；样式字段取 viewer 令牌）。五区 docking 改为
> 可调大小：palette 宽 / context 宽 / 底部高 / Context 输出块高四处分隔条
> （mouseArea 拖拽，范围夹取），布局值存放于 WorkflowCanvasState（UI 私有
> 会话状态，DEC-014 延伸，不持久化）。

> M11 实施补记（2026-10-06，[DEC-021](../decisions/DEC-021-monitor-node-and-inline-editing.md)）：
> 右栏移除，五区改四区——参数编辑迁移为画布节点内嵌控件（§5.5），中间结果
> 查看由监看器节点承担（§5.6），工作流总览迁至底部区右列（本节 M5-05 语义
> 不变：四行数值 + 过载警示 + 冻结标注）；相机分辨率全局入口（M6-06）迁入
> 相机源节点内嵌（点击循环档位）；节点页脚耗时徽标升级 last·avg 双值；
> docking 分隔条余 palette 宽/底部高两处（context 宽与输出块高随面板删除）。
> 指针分发次序反转：画布全视口 mouseArea 先合成（视觉层之下），内嵌交互
> 控件后合成优先命中（EUI 命中测试自顶向下、子先于父、仅 interactive 拦截，
> pinned 4691fc0 源码取证），节点标题/卡片等非交互视觉穿透至 mouseArea，
> 拖动/框选/连线语义不变；滚轮缩放不受影响（scrollable 谓词穿透）。

### 5.8 运行控制

- 工具栏：启动（需当前图预检通过，否则禁用并提示问题数）/停止；无暂停
  （契约无暂停语义）。启动准入失败显示 `AdmissionResult.error`。
- 停止路径闭合：UI 发起 `stop()` 后按"排空"空态呈现（见第 4 节）；
  应用关闭走 `DslAppConfig::onShutdown` 既有顺序（先停工作流引擎与相机
  服务，再回收，EXEC-04），新增通道与任务纳入关闭回归。

> M5-06 实施补记（2026-09-29）：运行控制随假换真集成落地。工具栏新增
> Start/Stop（Fit 左侧）：启动可用条件 = 当前图校验通过（§4 唯一判据）且引擎
> Idle/Failed——Failed 恢复路径为"修复后重新启动"，点击序内先 stop 回 Idle
> 再 start（契约"Failed 为运行终态，stop 后可重启"）；停止可用条件 =
> Running/Failed（幂等；owner 线程同步调用、有界排空 ≤ maxInFlight 帧任务，
> 与 onShutdown 同纪律）；Stopping 全禁用。启动准入失败经 `AdmissionResult.error`
> 写工具栏反馈行。"待生效"标注（§4）落地为 `WorkflowCanvasState.graphPending`：
> Running 下图结构变更经 `applyGraph` 校验入队后置位，pump 消费 `GraphApplied`
> 事件或引擎离开 Running 清除；帧边界拒绝（Info 事件）时如实保持（画布图与
> 生效图确实不一致），工具栏 warning 色 "pending - applies next frame"。
> 帧源绑定：相机源节点取 RGB 彩色流（`apps/viewer/workflow_frame_source.hpp`
> 接缝，非阻塞"上次已见序号"过滤 + RGBA8 零拷贝 wrap；多源图各 source 节点
> 独立水位）；深度伪彩通道不进工作流，如需深度输入按决策扩展目录。契约假引擎
> （`M5-08`）随集成按计划移除，`M4-09` 契约与共用测试套件由真引擎延续。

## 6. EUI-NEO 原语映射

| UI 元素 | 原语/组件 | 纪律与已知风险 |
| --- | --- | --- |
| 页面骨架/五区布局 | `ui.column`/`ui.row` stack 组合 | 绘制顺序即层叠（zIndex 不跨父容器） |
| 全局导航 | 自绘窄边导航栏（`navigation.hpp`，`rect`/`text` + viewer 令牌；选型结论见台账 EUI-20260928-001，`M5-02` 已实施） | sidebar 原型受限（右锚定抽屉）；条目受控渲染，选中态由 `NavigationState` 驱动 |
| 画布视口 | `clip` 容器 + `ui.state`（视图变换） | 平移缩放只改变换，不重建子树 |
| 节点框 | `rect`（圆角 `kRadiusMd/Lg` 层级） | 选中/错误态描边用语义令牌 |
| 端口 | `rect` + `mousearea` | 命中区大于视觉尺寸 |
| 连线 | `polygon`（贝塞尔采样） | EUI-20260924-001：`dirtyKey` 键控或逐帧直绘，M5-03 预算绘制成本 |
| 选择框/拖线预览 | `rect`/`polygon` 浮层 | 根 stack 末位兄弟合成（DEC-005） |
| 右键菜单 | `contextmenu`（退化自绘浮层） | 同上；M5-03 原型验证 |
| 参数控件 | `switch`/`slider`/`input`/`dropdown` | dropdown 按 EUI-20260923-002 接线；M11 起画布内嵌（枚举为点击循环胶囊，DEC-021） |
| 内嵌参数控件（M11） | `slider`/`input`/`toggleSwitch` + 自绘枚举胶囊 | 控件尺寸/字号随画布缩放；命中优先于画布 mouseArea（合成次序，DEC-021 决策 4） |
| 监看器预览窗（M11） | `image::importGpuImage`（GpuFrameView 路径，每监看器一枚） | 禁走 `ImageStream`（EUI-20260923-003）；非 Running 排空释放 |
| RealArray 矩阵编辑器 | 自研（`input` 网格 + 增删行列） | M5-04 工作量条目（DEC-015） |
| 快照缩略图 | `image::importGpuImage`（GpuFrameView 路径） | 禁走 `ImageStream`（EUI-20260923-003） |
| 节点耗时/统计 | `text`（等宽对齐）+ `barchart`（耗时条，可选） | mono 缺位用统一小数位+固定列对齐（DEC-005） |
| 校验/事件列表 | `virtuallist` 或 `datatable` | 条目点击定位画布节点 |
| 错误摘要弹窗 | `toast` | "弹窗摘要、面板全文"两层报错（调研 §3.1） |
| 端口/参数提示 | `tooltip` | 内容取 `ParamDescriptor.label` 与类型 |

原语映射在 `M5-03`/`M5-04` 原型阶段如遇组件能力缺口，按
[EUI-NEO 台账](../dependency_feedback/eui-neo/ledger.md)流程登记后再绕行；
不得静默引入另一套 UI 设施。

## 7. 平台无关纯逻辑清单（测试对象，`M5-03`..`M5-05`）

- 视图变换：screen↔canvas 互转、缩放锚点与上下限、帧全图拟合。
- 命中检测：节点/端口/连线命中（端口扩大命中区）、顶层优先、框选集合。
- 画布图同步：创建/删除/连线/替换边到 `WorkflowGraph` 的变换，操作前
  `validateWorkflowGraph` 预检与拒绝原因映射。
- 参数绑定：`ParamDescriptor` → 控件状态与校验（范围/枚举/有限性）。
- 调色板模型：`NodeCatalog` 分组与过滤。
- 节点参数区几何（M11/DEC-021）：paramRowHeight/paramSectionHeight/
  nodeHeight/nodeBounds/portPosition 全族（实例 RealArray 形状联动、监看器
  预览窗、相机源分辨率行）；枚举循环取值 cycleEnumOption；监看器/驱动尺寸
  pump 的排空与清扫语义。
- 统计管道：`WorkflowStats`/`NodeStats`/快照的 sequence 推进消费与
  停止排空语义。

## 8. 分期对照

- **P0 = M5 范围**：第 2..7 节全部内容。
- **P1（增强，需先扩 M4-09 契约的另行决策）**：Mute/Bypass、逐节点执行中
  高亮、断点/单步/从指定节点运行；纯 UI 项（Reroute、Frame 分组、backdrop、
  小地图、连线松手创建菜单）在 P0 验收后按需求排期。
- **P2/未排期**：subgraph、撤销重做、模板入口（`POST-04` 方向关联）。
  （原列于 P2 的"节点内嵌 viewer、内联参数"已由 M11/DEC-021 提前交付。）

P1/P2 的交互契约变更必须先走决策记录与 `M4-09` 契约测试同步，再进 UI。

> P0 验收补记（2026-09-29，`M5-07`）：第 2 节页面拓扑树逐项与实现对应核对
> 通过（预览页配色入口已按 DEC-014 决策 3 修正为本节补记前文）；真机
> （D435if）端到端验收通过——拖拽搭图（三节点拖出创建）→ 连线（两条类型
> 色连线，悬空中间态→valid）→ 运行（Start 准入，真相机 30 fps 进真引擎）→
> 中间结果（选中节点参数面板 + 848x480 实时缩略图随 seq 推进）→ 性能面板
> （总览 FPS/处理/丢弃/在飞 + 节点耗时徽标）→ 停止排空（冻结末次值 +
> stopped 标注 + [stopped] 事件）。验收由脚本化交互时序驱动（OS 级输入注入
> 在本环境不可用：mutter Wayland 会话不投递 XTEST 事件、GLFW 过滤 XSendEvent
> 合成输入），指针事件序列在工作流页既有交互接缝（调色板拖出回调、画布
> press/drag/release 状态机、运行控制回调）注入，渲染/compose 输出为真实
> 路径；截图存档 `screenshots/m5-07/`。桌面端交互（拖拽/下拉/docking）此前
> 已经用户真机实操使用并通过 M6/M7 反馈修复循环验证。
