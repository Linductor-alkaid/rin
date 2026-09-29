# DEC-013：工作流执行模型——帧泵采样 + 最新帧快照 + 有界在飞显式丢弃

> 状态：Accepted
> 日期：2026-09-29
> 负责人：Linductor-alkaid
> 冻结里程碑：M4（`M4-07` 实现冻结）
> 替代/被替代：无（取代总计划"暂定决策"中 DEC-013 的暂定表述）

## 背景与问题

M4-07 在 Core 侧交付工作流真引擎（`SCOPE-08`/`EXEC-07`）：按 M4-09 契约
（`IWorkflowEngine`）实现拓扑序执行、源节点相机帧注入、逐节点耗时统计、
中间产物有界保留、过载显式丢弃、取消与关闭语义，并与 `M5-08` 假引擎共用
契约测试（DEC-016）。总计划的暂定表述为"最新帧驱动 + 有界在飞 + 显式丢弃 +
comm 统计暴露"，需在实现前冻结四个执行模型问题：

1. **帧从哪里来、谁来唤醒执行**：引擎不允许拥有线程设施（`RULE-02`），相机
   帧由 librealsense 采集 worker 发布（`EXEC-01`/`EXEC-02`，Adapter 层）；
   引擎既不能阻塞等待帧，也不能被采集 worker 直接驱动。
2. **Running 下图替换与参数热更新的消费语义**：假引擎（M5-08）的
   `drainBoundary` 以 `LatestMailbox` peek 读取待生效图（不消费），导致每帧
   重建 generation 并重发 `GraphApplied`——M4 里程碑已明确真引擎不得复刻该
   缺陷（[image_workflow_design.md](../design/image_workflow_design.md) §5.3）。
3. **在飞上限与丢弃语义**：图执行慢于相机帧率时，排队策略必须显式
   （`EXEC-07`：禁止静默排队）。
4. **逐节点统计的采集点**：单帧求值入口 `runNodeGraph` 在图内部逐节点执行，
   引擎从外部无法观测单节点耗时与失败归属。

## 决策

采用 **帧泵采样 + 最新帧快照 + 有界在飞显式丢弃** 模型：

### 1. 帧泵（Executor 周期 tick）是唯一的唤醒源

引擎不拥有线程（`RULE-02`）；执行由一个 Executor 允许抖动周期任务
（`submit_periodic_cancellable_with_handle`，帧泵 tick）驱动。tick 持引擎
生命周期互斥（与 start/stop/applyGraph 串行化，同假引擎纪律），单次 tick
执行：

1. 回收已完成帧任务（消费 future，异常显式化）；
2. 探测并捕获帧源输入：对**当前生效图**的全部源节点，按引擎侧
   "上次已见序号"（`ICameraService::tryLoadFrame` 同型语义）探测最新帧，
   有新帧即捕获为快照（`WorkflowFrameInput`：`ImageU8` + 相机帧序号）并
   推进序号；帧源返回无效图像按无输入处理；
3. 帧边界排空：图替换按**图对象同一性**判定是否新图（`LatestMailbox` 为纯
   快照语义、无消费操作，引擎以 shared_ptr 同一性记录已应用图）——仅实际
   变化时重建换代并发布一次 `GraphApplied`（假引擎 peek 缺陷的修复方式）；
   新代生效即为目标图（被替换的旧图不再执行），新代源节点尚无捕获输入期间
   提交门等待（帧不被执行，亦不计过载丢弃——图不可运行非过载）。可观察
   后果如实冻结：`GraphApplied` 发布于换代时刻（帧边界、先于新代首帧），
   不以新代是否已有输入为条件；换代至供帧的窗口内引擎无帧产出（统计冻结），
   被替换旧图节点的产物读取随图移除即时返回 false（M4-09 契约允许）；
   随后参数命令逐条消费应用（`MpscChannel` FIFO）并重建换代生效；
4. 有界准入：本 tick 捕获到新输入、生效图全部源节点均有捕获输入且在飞任务
   数 < `maxInFlight` 时，以 `submit_cancellable` 提交帧任务（携带输入快照 +
   生效代）；无新输入不提交（相机空闲时 tick 为廉价探测）；在飞已满时对
   捕获的新帧显式丢弃并计数（`droppedFrames`，EXEC-07）；
5. 提交被 Executor 拒绝（admission 失败）显式化：丢弃计数 + `Info` 事件，
   不静默重试。

**帧任务**：单帧为有界工作单元——帧入口检查协作取消（`StopToken`）后，以
`runNodeGraph` 同步执行整图（节点数准入 ≤ 64），执行中不再中断；取消与
停止在帧边界生效。帧完成后按执行序发布逐节点产物快照（每节点最新一幅，
`LatestMailbox`）与会话统计快照。

### 2. 帧源接缝：引擎拉取，适配器转换

```cpp
struct WorkflowFrameInput {
    ImageU8 image;                     // 必须有效；格式不符在执行期显式失败
    std::uint64_t sourceSequence = 0;  // 与相机帧 sequence 同源，透传产物快照
};
// lastSeenSequence 入参 = 引擎侧已消费序号，出参 = 本次看到的最新序号；
// false = 无比 lastSeenSequence 更新的帧（帧泵跳过捕获）。
using WorkflowFrameSource = std::function<bool(
    NodeId sourceNode, std::uint64_t& lastSeenSequence, WorkflowFrameInput& out)>;
```

相机帧 → `ImageU8` 的转换属 Adapter/应用层职责（`RULE-01`：引擎与
librealsense 解耦）；多源图由帧源实现按 `sourceNode` 分发。捕获时序（快照
语义）保证：同一帧不会被两个在飞任务重复消费；`maxInFlight > 1` 时各任务
携带各自捕获的快照。产物快照的 `sourceSequence` 传播语义：源节点 = 注入帧
序号，下游节点 = 其全部生产者 `sourceSequence` 的最小值（保守对齐）。

### 3. 逐节点统计经 `runNodeGraph` 观测接缝采集

`runNodeGraph` 增加可选逐节点观测参数（默认空，M4-02 契约的加性扩展）：

```cpp
using NodeExecutionObserver = std::function<void(
    const NodeGraph::Node& node, double costMs, const std::exception_ptr& error)>;
```

成功回调 `error == nullptr`、`costMs` 为该节点 `apply` 实测耗时；失败回调
携带节点异常副本后**原样重传**（`runNodeGraph` 异常传播语义不变）。引擎以
此采集：逐节点耗时（`NodeStats`，滚动窗口 32 帧）、节点失败归属
（`NodeFailed` 事件携带节点 id）、端到端 FPS（帧完成时刻滚动窗口）、
`inFlight`（Executor 句柄计数可观察面）。注入型源节点不经耗时观测（无
`apply` 可测，`lastCostMs`/`avgCostMs` 保持 0），其 `executedFrames` 由引擎
在注入点记账（注入器被调用即执行发生，随帧递增）——统计快照覆盖生效图全部
节点，与假引擎基线一致。统计/产物/事件通道与假引擎同型：
`LatestMailbox`（统计、产物、事件、图替换）+ `MpscChannel`（参数命令），
不建平行监控（`RULE-03`/`RULE-07`）。

### 4. 生命周期纪律与假引擎同款

掉队 tick 生命周期闭合（tick 闭包持 `weak_ptr`、入口提升强引用）、stop()
阻塞排空在飞任务（协作取消 + future 回收）后收敛 Idle、幂等、析构防御性
stop、Failed 运行终态（stop 后可重启新会话）、统计会话复位而通道序号实例
内单调——全部沿用 M5-08 假引擎已验证的模式（EXEC-04 排空语义与
`ICameraService::stop` 一致）。

### 5. 节点实例构建经配置化工厂

引擎配置接受 `ImageNodeFactory`（默认 `makeDefaultImageNode`）；图换代时以
新参数重建节点实例（"下一帧生效"，DEC-014/§4.1 参数构造期定型语义）。
测试经同一接缝注入计时/故障包装节点，不另设引擎内故障旋钮。

## 被否决的备选

- **相机 worker 推送驱动**（采集 worker 捕获到新帧时直接向引擎提交任务）：
  使 Adapter 耦合引擎的提交路径与生命周期，违背"第三方回调只做有界校验和
  投递"的边界纪律（AGENTS.md 工程约束 11）与 EXEC-01/02 的单向数据流；
  且采集 worker 内提交需感知引擎在飞状态，跨层共享并发状态。否决。
- **帧任务执行期拉取帧源**（任务开始时逐一拉取，替代捕获快照）：
  `maxInFlight > 1` 时两个在飞任务对同一"最新帧"重复消费或拉空失败（最新态
  语义无消费点），需要引入第二条序号线与提交/执行两阶段提交协议，复杂度
  与竞态面均高于捕获快照；且图换代后新增源节点在任务期拉取会引入
  "换代帧缺输入"失败路径。否决。
- **`MpscChannel`（容量 1，DropOldest）承载图替换**：消费语义原生成立，但
  "最新态"语义需 DropOldest 策略保证，且与假引擎的 `LatestMailbox` 选型
  分叉；同一性比较方案在保留既有通道选型的情况下达成等价消费语义。否决。
- **假引擎 peek 模式**（每帧重建 + 重发 `GraphApplied`）：已被 M4 计划
  标记为不得复刻的缺陷。否决。

## 影响与约束

- `runNodeGraph` 新增默认空观测参数为 M4-02 契约的**加性扩展**，既有调用方
  与测试不受影响；`image_workflow_design.md` §5.2 同步冻结。
- 引擎配置（`WorkflowEngineConfig`/`WorkflowFrameSource`）与工厂声明位于
  `src/workflow/engine.hpp`（实现层头文件）：工厂签名含 `executor::Executor&`
  第三方类型，按假引擎先例不入 `include/rin/` 公开契约。
- 引擎实现于 `rin_workflow` 静态库（`rin::core` + `executor::executor`），
  与假引擎（`rin_workflow_fake`）并列；M4 节点目录构建函数提取为两引擎共享
  的单一事实源（`src/workflow/default_catalog.hpp`），假引擎委托同名目录，
  schema 与 `M5-08` 冻结基线逐字段一致。
- 性能结论以 `M4-08` 真引擎实测为准（DEC-016 风险条款），本决策不含性能
  声明。
- DOD-02 并发矩阵（正常完成/任务异常/提交拒绝/执行中取消/shutdown）由
  契约套件（共用）+ 引擎特有测试覆盖；跨上下文通道与关闭路径跑 tsan 预设。
