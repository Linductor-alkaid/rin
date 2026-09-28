# 图像工作流设计：Core 算子契约与执行语义（M4）

> 状态：Active
> 日期：2026-09-28（随 `M4-02` 产出）
> 负责人：Linductor-alkaid
> 关联：[M4 里程碑](../plans/m4-cv-node-workflow.md)、[DEC-012](../decisions/DEC-012-image-operator-strategy.md)、
> [DEC-013（暂定，M4-07 冻结）](../plans/rin-implementation-plan.md)、
> [工作台 UI 设计](ui_workspace_design.md)（UI 消费方视角）

## 1. 定位与范围

本文冻结 M4 图像工作流的 Core 契约与执行语义：图像类型、端口类型系统、节点契约与
类型化参数模型、图编译与单帧求值，以及 M4 节点目录与逐节点 golden 测试项要求。
实现载体为 `include/rin/image_types.hpp`、`include/rin/image_node.hpp`、
`include/rin/node_graph.hpp`（`rin_core`，零第三方依赖）。

工作流视图契约（`M4-09`，`workflow_types.hpp`/`workflow_engine.hpp`）是 UI 与引擎
之间的唯一数据面；本文的 Core 契约是其引擎侧延伸，共用 `validateWorkflowGraph`
作为图合法性的唯一判据（UI 预检与引擎准入不重复实现）。工作流页 UI 按工作台设计
§5 只消费引擎契约；统计与产物语义以 `M4-09` 契约为准，本文不重复定义。

非目标：GPU/shader 加速算子；图持久化（`POST-04`）；编辑器 UI（M5）；引擎的
Executor 承载细节（`M4-07` 实现，执行模型经 DEC-013 冻结）。

## 2. 分层与依赖边界

- `RULE-01`：三个公开头只依赖标准库与同目录契约头；算子与 FFT 后端（kissfft，
  DEC-012）细节收敛在 `src/` 实现内，FFT 调用面收敛在 M4-06 单一节点实现。
- `RULE-07`/`EXEC-07`：节点是同步 CPU 工作单元；`runNodeGraph` 的单帧求值由
  M4-07 引擎在 Executor 有限任务体内调用，统计/取消/有界在飞/显式丢弃由引擎经
  Executor 公开能力与 `executor::comm` 承载，不在图像契约内。
- 工作集有界（AGENTS.md 工程约束）：单幅图像字节预算 `kMaxImageBytes`（16 MiB）；
  单图节点数准入上限（默认 64，构建时可配）；单帧求值期间各节点输出存活至消费者
  完成为止，帧结束后的产物保留策略（每节点最新一幅）归引擎。

## 3. 图像类型与端口类型系统

### 3.1 图像类型 `ImageU8`

- 格式即端口类型：`PortType::Gray8` / `PortType::Rgba8`（`M4-09` 契约的枚举是
  M4 节点集唯一的类型面，不另设图像格式枚举）；`elementSize(PortType)` 给出单
  像素字节数（1 / 4）。
- 共享不可变像素：像素缓冲以 `shared_ptr<const vector<uint8_t>>` 承载，构造
  （提交）后不可变；拷贝只复制元数据与引用计数，节点输出向多个消费者与产物
  快照（`NodeOutputSnapshot` 同款承载）传递零拷贝。
- 容量有界：`make`/`wrap` 拒绝缓冲超过 `kMaxImageBytes`（16 MiB，覆盖当前支持
  的全部流配置 1920×1200 Rgba8 ≈ 8.8 MiB 并留余量）；宽高为 0、stride 小于最小
  行宽同样显式失败，返回无效图像（`valid()==false`），不抛异常。
- stride 语义：行字节数，`≥ width×elementSize`，行尾 padding 允许；缓冲字节数
  恰为 `stride×height`，行外读取按契约禁用（`row(y)` 越界返回 nullptr）。乘法
  一律 64 位（`NodeOutputSnapshot::valid` 同款防回绕纪律）。
- `wrap` 接管既有缓冲（适配器零拷贝路径、节点内填充缓冲）：接管后以 const 视图
  共享，调用方不得再改写；缓冲小于 `stride×height` 时拒绝。

### 3.2 端口类型系统

连线兼容性 = 两端 `PortType` 相等（`validateWorkflowGraph` 的 `TypeMismatch` 判
据），图像运行时格式与端口声明的一致性由 `runNodeGraph` 的防御性核对兜底。节点
签名（`NodeDescriptor::inputs/outputs`）是端口面唯一来源；目录在构建期确定、
运行期不变，编译后 `NodeGraph::Node::descriptor` 指向目录内声明（图不得比目录
长寿）。

## 4. 节点契约与类型化参数模型

### 4.1 `IImageNode`

- `descriptor()`：返回与目录声明一致的类型签名（`buildNodeGraph` 构造时防御性
  核对 typeId 一致）。
- `apply(inputs)`：单帧同步执行；入参数量与 `descriptor().inputs` 一致、顺序与
  入边一致、类型由端口类型系统保证；返回数量与 `descriptor().outputs` 一致且逐
  幅有效、格式与声明一致（违反由 `runNodeGraph` 抛 `std::runtime_error` 显式
  暴露，禁止静默）。失败以异常报告（引擎捕获 → `NodeFailed` 事件 → `Failed`，
  EXEC-07），不得返回无效图像静默失败。
- 逻辑只读：`apply` 为 const；实现不得保留 `inputs` 的引用越过本次调用（需要
  延续时共享像素所有权拷贝）。参数在构造期定型，实例无逐帧可变状态——运行期
  参数热更新（"下一帧生效"，DEC-014）由引擎在帧边界以新参数重建节点实例实现，
  `apply` 路径不感知参数变更；这也使节点实例天然可被引擎串行调度（DEC-013 的
  有界在飞策略不引入节点内数据竞争）。

### 4.2 类型化参数模型

- `effectiveParamValue(descriptor, instance, paramId)`：生效值读取基元——实例已
  赋值取赋值（同 paramId 多条取最后一条，防御语义；图校验拒绝重复赋值），未
  赋值取声明默认值；未声明 paramId 返回 nullptr。
- `paramBoolean` / `paramInteger` / `paramReal` / `paramEnumeration` /
  `paramRealArray`：按声明 kind 匹配后的类型化读取（kind 不符返回空，防御运行
  期直接构造的实例）。赋值合法性（范围/枚举/有限性）由 `validateWorkflowGraph`
  在图准入时判定，节点实现内不做重复校验。

### 4.3 节点工厂接缝

`ImageNodeFactory(descriptor, instance) -> unique_ptr<IImageNode>`：按目录声明与
实例参数构造可执行节点；返回 nullptr 表示注入型源节点（typeId 无节点实现，执行
时由引擎注入相机帧）；构造失败抛异常，由 `buildNodeGraph` 转为显式校验问题。
M4-03..M4-06 逐算子提供工厂实现并注册进引擎目录。

## 5. 图编译与执行语义

### 5.1 `buildNodeGraph`：编译入口

1. `validateWorkflowGraph`（唯一判据）不通过 → 原样返回问题清单，不产出图；
2. 节点数准入：超过 `maxNodes` 显式拒绝（`BadParam`，与假引擎准入同默认 64）；
3. 逐节点调用工厂：抛异常或返回实例 typeId 与目录不一致 → 显式校验问题
   （`BadParam`）；目录缺类型（校验后不可达）→ `UnknownNodeType` 兜底；
4. 稳定拓扑排序：反复按图内声明序收录就绪节点（生产者已全部收录），同轮内后继
   立即可见——确定性排序，顺序 = 依赖正确性 + 声明序；校验通过后环不存在，
   排序停滞按 `Cycle` 防御性兜底拒绝。

入边解析为 `InputEdge{producerIndex, outputPort}`（producerIndex 为执行序下标），
与 `descriptor->inputs` 对齐；悬空输入已在第 1 步拒绝（可执行图的输入必然全
连接，零入边节点即注入点）。

### 5.2 `runNodeGraph`：单帧同步求值

- 按拓扑序执行整图，返回与执行序对齐的逐节点输出；帧内生产者输出存活至消费者
  完成为止，帧末全部返回（保留策略归引擎）。
- 注入型源节点：声明输出数为 0 时不注入；输出数非 1、注入器返回图像无效或格式
  与声明不符 → 抛 `std::runtime_error`。注入器由调用方提供（引擎注入相机帧，
  M4-07 源节点语义；测试注入合成帧）；多源图共享同一注入策略，由注入器按节点
  分发。
- 实现节点：按入边收集生产者输出（像素共享零拷贝）→ `apply` → 防御性契约核对
  （数量/有效性/格式），违反抛 `std::runtime_error`。
- 节点内部异常原样传播，不在此层包装（引擎区分节点失败与系统失败）。
- 与引擎的关系：`runNodeGraph` 不做统计、不做准入、不感知取消——M4-07 引擎在
  Executor 有限任务体内调用它并包络统计（每节点耗时/端到端 FPS/丢弃计数）、
  有界在飞与协作取消（帧边界检查取消状态）。

### 5.3 M4-07 真引擎对齐项（假引擎已知偏差，不得复刻）

- Running 下 `applyGraph` 待生效图的消费：假引擎 `drainBoundary` 以 LatestMailbox
  peek 读取（不消费），导致每帧重建 generation 并重发 `GraphApplied`。真引擎按
  `M4-09` 契约"帧边界排空重建"语义消费（读取即消费），仅在图实际变化时重建并
  发布事件。
- 掉队 tick 生命周期闭合：假引擎的 `weak_ptr` 闭包 + 入口提升模式作为既有先例；
  真引擎的帧任务句柄与取消路径按 EXEC-07 组织。
- 参数热更新在帧边界以新参数重建节点实例（§4.1），不修改运行中实例。

## 6. M4 节点目录（调色板契约）

目录签名与参数 schema 以 `M5-08` 假引擎 `makeDefaultFakeCatalog()` 冻结的基线为
准（M5 调色板/参数面板按其开发）；真引擎目录提供同一组 typeId 与 schema，数值
语义归 `M4-03`..`M4-06` 实现：

| typeId | 显示名 | 输入 | 输出 | 参数（id: kind 默认 [范围]） |
| --- | --- | --- | --- | --- |
| `source` | 相机源 | — | Rgba8 | —（注入型，§5.2） |
| `crop` | 裁切 | Rgba8 | Rgba8 | x/y/width/height: Integer 0 [0,4096]（w 默认 64，h 默认 64） |
| `downscale` | 降分辨率 | Rgba8 | Rgba8 | interpolation: Enumeration "nearest" [nearest,bilinear]；scale: Real 0.5 [0.1,1.0] |
| `grayify` | 灰度化 | Rgba8 | Gray8 | — |
| `gaussian_blur` | 高斯模糊 | Gray8 | Gray8 | radius: Integer 3 [1,10]；sigma: Real 1.5 [0,10] |
| `conv_kernel` | 自定义卷积 | Gray8 | Gray8 | size: Enumeration "3" [1,3,5]；kernel: RealArray（3×3 单位核，行主序） |
| `hist_eq` | 直方图均衡 | Gray8 | Gray8 | — |
| `fft_lowpass` | FFT 低通 | Gray8 | Gray8 | cutoff: Real 0.2 [0,1] |
| `fft_highpass` | FFT 高通 | Gray8 | Gray8 | cutoff: Real 0.2 [0,1] |
| `fft_bandpass` | FFT 带通 | Gray8 | Gray8 | lowCut: Real 0.2 [0,1]；highCut: Real 0.6 [0,1] |

`grayify` 是 RGBA 相机帧进入 Gray8 算子族的桥接节点（不在 M4 总计划算子清单
正文内，随 `M5-08` 假目录入库并按 DEC-016 作为 M4 真实现的一部分交付）。

## 7. 逐节点 golden 测试项要求

各算子 golden 数值测试（`M4-03`..`M4-06`，合成图像 + 已知答案，容差随工作项
冻结）至少覆盖：

- `crop`（M4-03）：ROI 越界（x+width > 图宽等）显式拒绝、退化区域（0 宽/高）
  拒绝、正常裁切像素级 golden、stride 保持。
- `downscale`（M4-03）：nearest/bilinear 各一组已知小图 golden、scale=1.0 恒等、
  输出尺寸公式冻结（向下取整）。
- `conv_kernel`（M4-04）：1/3/5 核尺寸与 kernel RealArray 长度一致性（K²）、
  单位核恒等、已知核响应 golden（边缘/角点）、边界填充策略可配且逐策略 golden。
- `gaussian_blur`（M4-04）：可分离高斯与直接卷积数值等价（容差内）、已知
  sigma/radius 响应 golden、边界填充一致。
- `hist_eq`（M4-05）：已知直方图映射 golden、均匀分布输入近似恒等（均匀性）、
  RGBA 亮度域与 Gray8 路径一致。
- `grayify`：亮度域公式 golden（与 hist_eq 的 RGBA 亮度域同公式）。
- FFT 族（M4-06，DEC-012 节点级约束的 golden 化）：
  - 内部零填充到 2 幂（宽高各自向上取 2 幂；848×480 → 1024×512），掩膜按
    归一化频率在填充分辨率上构造，IFFT 后裁回原尺寸——填充/裁剪往返误差
    golden；
  - 正弦注入 → 单频滤除：848×480（填充路径）与 512×512（原生路径）各覆盖一组
    （低通/高通/带通各测）；
  - 截止/带宽为归一化频率 ∈ [0,1]，语义不随填充尺寸改变（同 cutoff 在两个尺寸
    路径滤除同一归一化频率）；
  - 掩膜形状按 DEC-012 冻结（工作项冻结时记录）。

## 8. 测试矩阵（M4-02 契约层）

- `ImageU8`：make/wrap 正例、无效元数据（零宽高/未知格式/stride 不足/超预算/
  缓冲过短）负例、64 位乘法边界、row 越界、共享像素不可变与零拷贝共享。
- 参数模型：声明/赋值/缺省三态生效值、未知 paramId、kind 不符、各 kind 类型化
  读取。
- `buildNodeGraph`：校验不通过原样透传、节点数准入、工厂异常/签名不一致显式化、
  稳定拓扑序（链式/分叉/多轮就绪）、入边解析正确性、空图。
- `runNodeGraph`：注入语义（注入帧/格式不符抛出/多源）、防御性输出核对（数量/
  无效图/格式不符）、节点异常传播、像素共享（下游改动不可见）、空图。
- 工程规范 DOD：debug/asan/ubsan 全绿；本层为单线程纯逻辑，无跨上下文状态
  （DOD-02 并发矩阵归 M4-07 引擎与 `M5-08` 契约套件）。

## 9. 关联文档和工作项

- [M4 里程碑](../plans/m4-cv-node-workflow.md)：`M4-02`（本文）、`M4-03`..`M4-06`
  （§6/§7 逐算子）、`M4-07`（§5.3 对齐项）、`M4-08`（性能实测）。
- [DEC-012](../decisions/DEC-012-image-operator-strategy.md)：FFT 依赖选型与 2 幂
  填充约束（§7 golden 化）。
- [工作台 UI 设计](ui_workspace_design.md) §5：UI 对引擎契约的消费面（只消费不
  重复实现）。
- [kissfft 台账](../dependency_feedback/kissfft/ledger.md)：KIS-20260928-001（FFT
  后端锁定约束）。
