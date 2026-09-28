# M4：图像处理节点库与工作流引擎

> 状态：In Progress
> 负责人：Linductor-alkaid（授权 Agent 按工程规范自治执行）
> 所属计划：[Rin 实施总计划](rin-implementation-plan.md)
> 前置：M1（与 M3 无代码耦合，可并行排期；建议 M3 后实施）；`M4-09` 视图契约
> 按 DEC-016 先于 M5 骨架实施
> 建议发布点：v0.4.0
> 更新日期：2026-09-29

## 目标

在 Core 提供一组可组合的传统 CV 处理节点：图像裁切、降分辨率、自定义卷积核卷积、
高斯模糊、直方图均衡、基于傅里叶变换的滤波（高通 / 低通 / 带通），并以 DAG 工作流
引擎串接：逐节点输出中间图像、统计每节点处理耗时与端到端 FPS；执行经 Executor
承载（总计划 `EXEC-07`）。本里程碑只交付 Core 能力与测试，编辑器 UI 属 M5。

## 范围与非目标

范围：

- Core `imgproc` 模块：图像类型、节点契约、端口类型系统、`NodeGraph` 校验。
- 工作流视图契约（`M4-09`，[DEC-016](../decisions/DEC-016-contract-first-workbench-order.md)）：
  图模型、类型化参数、统计 schema、运行控制命令与事件语义，供 M5 假引擎与 UI
  先行开发，真引擎实现同一契约。
- 算子节点：裁切、降分辨率、自定义卷积核、高斯模糊、直方图均衡、FFT 滤波族。
- 工作流引擎：拓扑序执行、源节点注入、耗时/丢弃统计、中间产物有界保留、
  取消与关闭语义。
- 决策：DEC-012（算子实现策略与 FFT 依赖选型）、DEC-013（执行模型）、
  DEC-016（交错实施策略）。

非目标：

- GPU / shader 加速算子。
- 节点图持久化与节点库扩展（见总计划 `POST-04`）。
- 编辑器 UI（M5）。

## 设计与决策依据

- 分层约束（`RULE-01`）：imgproc 属 Core，不得引入 librealsense2/EUI-NEO 类型；
  算法可测试性优先（纯函数、golden 向量）。
- `RULE-02`/`RULE-03`/`EXEC-07`：引擎执行、统计与结果回传全部经 Executor 公开
  能力与 `executor::comm` 原语，有界在飞，显式丢弃策略。
- pinned librealsense/EUI-NEO 与本里程碑无直接依赖面；若 `DEC-012` 引入新的小型
  第三方库（如 FFT），按 `RULE-04` 登记 lock、许可证与供应链文档。

## 工作项

- [x] `M4-01` 调研并冻结图像算子实现策略（`DEC-012`）：对比引入 OpenCV
      （external pin）、自研算子 + 小型 FFT 第三方库（如 kissfft 类）、全自研 FFT
      的构建体积、CI 时长、许可证与性能（512² 与 848×480 灰度 FFT 实测）。
      完成判据：`DEC-012` `Accepted` + 基准数据；新依赖同步 lock/许可证/供应链
      记录。
- [x] `M4-02` Core 图像与节点契约：`ImageU8`（RGBA8 / Gray8、共享像素、容量有
      界）、`IImageNode`（类型化参数模型 + `apply`）、端口类型系统、`NodeGraph`
      构建与校验（环、悬空输入、类型不匹配显式拒绝）。完成判据：契约与校验单测
      通过。
- [x] `M4-03` 几何算子节点：裁切（ROI 越界与退化区域拒绝）、降分辨率（最近邻 /
      双线性）。完成判据：golden 数值测试通过。
- [x] `M4-04` 卷积算子节点：自定义 KxK 卷积核（1/3/5，边界填充策略可配）、高斯
      模糊（核尺寸 / sigma）。完成判据：数值测试（已知核响应、可分离高斯与直接
      卷积等价）通过。
- [ ] `M4-05` 直方图均衡节点：Gray8 与 RGBA 亮度域。完成判据：已知直方图映射与
      均匀性测试通过。
- [ ] `M4-06` FFT 滤波节点族：Gray 频域变换、低通 / 高通 / 带通（截止与带宽可
      调、掩膜形状按 `DEC-012` 定）、IFFT 回 RGBA 显示。完成判据：正弦注入→单频
      滤除数值测试通过。
- [ ] `M4-07` 工作流引擎（`EXEC-07`，按 `DEC-013`）：拓扑序执行、源节点（相机帧
      注入语义）、每节点耗时统计、中间产物有界保留（最近 N 张）、过载显式丢弃与
      计数、取消与排空、关闭幂等。完成判据：引擎测试（正常完成 / 节点异常 /
      取消 / 关闭 / 过载丢弃）+ debug/asan/ubsan 通过。对齐注记（2026-09-28，
      来自 `M5-08` 假引擎验证）：Running 下 `applyGraph` 待生效图的帧边界消费
      语义（`drainBoundary` 用 LatestMailbox peek 不消费导致假引擎每帧重建
      generation 并重发 `GraphApplied`）需在真引擎设计时一并裁决——真引擎不得
      复刻该缺陷，消费语义按 `M4-09` 契约"帧边界排空重建"语义明确；掉队 tick
      生命周期闭合模式（weak_ptr 闭包 + 入口提升）作为既有先例。
- [ ] `M4-08` M4 测试矩阵与性能记录：全预设测试通过；848×480 典型工作流全链路
      吞吐实测记录（只记录实测值与方法，不宣称未验证的性能目标）。
- [ ] `M4-09` 工作流视图契约冻结（[DEC-016](../decisions/DEC-016-contract-first-workbench-order.md)，
      先于 M5 骨架实施）：在 `include/rin/` 定义 UI 与引擎之间的唯一契约面——
      工作流图模型（节点/连线/校验结果）、类型化节点参数模型、逐节点耗时与端到端
      统计 schema、运行控制命令与结果/错误事件语义及 `executor::comm` 通道选型
      （`LatestMailbox`/`DoubleBuffer`）。契约不得包含 EUI-NEO 类型（`RULE-01`）。
      完成判据：契约头文件 + 类型/校验单测通过；`M5-08` 假引擎与 `M4-07` 真引擎
      均引用该契约。

## 实施顺序（DEC-016）

`M4-09` 视图契约最先实施，作为 M5 骨架（假引擎驱动）的契约基础；`M4-03`..`M4-08`
随后实施；`M4-07` 真引擎必须实现 `M4-09` 契约并通过与 `M5-08` 假引擎共用的契约
测试，替换集成在 `M5-06` 完成顺序不变。

## 风险与阻塞

- FFT 性能不达标：`M4-01` 基准前置降险；不达标时以降分辨率节点前置作为文档化
  的使用建议，不改声明。
- 自研算子数值错误：golden 向量 + 已知答案测试（合成图像）逐算子覆盖。
- 与相机帧率耦合的过载：显式丢弃策略 + `executor::comm` 统计暴露，禁止静默排队。

## 测试与退出条件

- [ ] `ctest`（debug/asan/ubsan，涉及跨上下文时 tsan）全绿，新增：契约校验、
      逐算子 golden、FFT 数值、引擎状态路径测试。
- [ ] `DEC-012`/`DEC-013` 记录生效并被实现引用。
- [ ] 引擎过载 / 取消 / 关闭测试通过（`M4-07`）。
- [ ] 性能实测记录落盘（`M4-08`），无未经验证的性能声明。
- [ ] 文档同步：新增 `docs/design/image_workflow_design.md`、总计划 `SCOPE-08`、
      CHANGELOG（Unreleased）。

## 验证记录

2026-09-29：`M4-04` 完成关闭——卷积/高斯算子节点落地（`rin_core`，零第三方公开
依赖，RULE-01）。`src/core/image_ops.cpp` 新增 `conv_kernel`（K ∈ {1,3,5} 自定义
核 + clamp/reflect/zero 可配边界）与 `gaussian_blur`（radius→K=2·radius+1 +
sigma，可分离两趟 + 固定 clamp 边界）实现，工厂同步扩展（未实现类型错误消息改指
M4-05..06）。数值语义随本工作项在
[design/image_workflow_design.md](../design/image_workflow_design.md) §6/§7 冻结：
卷积执行为相关语义（核不翻转，系数矩阵与邻域逐点对应，与常见图像编辑器自定义
卷积核及 OpenCV filter2D 一致，翻转语义由使用方翻转矩阵表达）、double 累加
round-half-up 饱和量化、核不自动归一化；reflect 为 reflect-101（−1→1、n→n−2，
周期 2(n−1)，n=1 一律取 0）、zero 越界按黑不补偿（暗边是策略语义）；高斯一维核
归一化 Σ=1、sigma=0 为 δ 核恒等输出（极限语义），clamp 填充为线性饱和下标映射
与滤波可交换，故可分离与直接 2D 卷积数学恒等、量化后逐像素差 ≤ 1（golden 容差）。
目录 schema 演进随本工作项双向落盘：§6 冠以演进记录，`conv_kernel` 增加 `border`
参数（Enumeration "clamp" [clamp,reflect,zero]），`M5-08` 假引擎目录同步扩展（未
赋值参数取默认值，既有图不受影响；M5 调色板/参数面板经 schema 自动获得新控件）。
测试（Independent-Verification-Agent 独立编写执行，两轮，实现文件对验证方只读以
保证 golden 独立性）：新增 `tests/test_image_ops_convolution.cpp` 171 项（构造期
拒绝全分支〔size 非法枚举/种类错位、kernel 缺失/长度≠K²/系数非有限、border 非法
枚举〕、默认参数生效、单位核恒等 K=1/3/5×三边界、4×4 已知核响应逐字节 golden
〔角点/边缘〕、单点移位核逐策略 golden + clamp≠reflect 见证、饱和/负响应截断/
round-half-up 半值、Laplacian 和=0 核、1×N 窄图 K=5 reflect-101 折返与 n=1 图、
37×23 噪声图与测试内独立 double 参考逐字节交叉验证、padding stride、防御路径、
图集成 golden 与 kernel 长度不符 buildNodeGraph 显式 BadParam；gaussian_blur 构造
期拒绝〔radius 0/11/负、sigma 负/NaN/±inf/越界、种类错位、声明缺失〕、默认参数
逐字节、sigma=0 恒等〔K=7>图幅〕、常值图、核系数 golden、脉冲/阶梯角点逐字节
golden + 质量守恒自检、可分离 vs 测试内直接 2D 参考 9 组 radius×sigma |diff|≤1、
stride 紧凑、sigma=0 恒等图链）；`test_image_ops_geometry.cpp` 适配（gaussian_blur
由"未实现类型"见证改为实现见证，未实现见证保留 grayify 与未知 typeId，166 项）；
`test_public_boundary.cpp` 扩展 M4-04 两类型完整 schema 最小实例化（33→37 项）；
`test_workflow_fake_engine.cpp` 默认目录断言扩展 conv_kernel size/kernel/border
schema 同步（全套件 246 项）。golden 期望全部由 §7 冻结公式独立手推 + 独立
Python 数值求值 + 测试内自写 double 参考推导，未参考实现代码；第一轮 2 项失败
经全模拟取证均为测试侧手推错误（K=5 单点核 zero 边界列偏移恒越界、阶梯图邻域
覆盖漏算 255 饱和列），实现未动复验 PASS。debug/asan/ubsan 三预设全量 ctest
23/23（含 D435if 真机硬件冒烟，设备在位通过），asan/ubsan 零报告、编译零警告
（-Wall -Wextra -Werror 口径复核 CLEAN）。环境：x86_64 Linux，GCC 13，CMake
presets debug/asan/ubsan；tsan 未跑（本层单线程纯逻辑，DOD-02 并发矩阵归
`M4-07` 引擎与 `M5-08` 契约套件，与 `M4-02`/`M4-03` 先例一致）。已知未验证风险：
数值级跨平台 libm（exp）一致性未断言，golden 脉冲用例距 x.5 量化边界最小裕度
0.12，常规平台差异不影响量化结果（验证方裕度分析）。`M4-05`（直方图均衡节点）
为下一工作项。

2026-09-29：`M4-03` 完成关闭——几何算子节点落地（`rin_core`，零第三方公开依赖，
RULE-01）。新增 `include/rin/image_ops.hpp`（`makeDefaultImageNode` 默认工厂：
crop/downscale 返回实现、"source" 返回 nullptr 注入语义、未实现 M4 类型抛
`std::invalid_argument` 显式暴露目录与工厂能力偏差，M4-04..06 扩展同口径）+
`src/core/image_ops.cpp`（算子输出统一"先填后冻结"路径：mutable 缓冲填充后经
`ImageU8::wrap` 以 const 共享，输出一律紧凑 stride = 宽×elementSize）。
数值语义随本工作项在
[design/image_workflow_design.md](../design/image_workflow_design.md) §7 冻结：
crop ROI 退化（w/h ≤ 0）与越界（x+w > 输入宽等，uint64 运算，运行期直接构造的
负值按无符号回绕自然落入越界拒绝）在 apply 期抛 `std::invalid_argument`（图像
尺寸是运行期数据，构造期无法判定），输出为逐行 memcpy 的新紧凑缓冲、不共享源
像素；downscale 输出尺寸 floor(in×scale)（< 1 退化输出显式拒绝）、scale ∈ (0,1]
构造期复核（准入范围 [0.1,1.0] 之外的直接构造防御）、nearest 面积覆盖采样
（src = min(floor((out+0.5)×ratio), in−1)）、bilinear 中心对齐（srcf =
(out+0.5)×ratio − 0.5 饱和后插值，逐通道独立、不预乘 alpha、floor(v+0.5)
round-half-up 量化）、scale=1.0 两种插值均逐像素恒等（公式自然给出，无特例
快路径）。测试（Independent-Verification-Agent 独立编写执行，一轮 PASS，
实现文件对验证方只读以保证 golden 独立性）：新增
`tests/test_image_ops_geometry.cpp` 165 项（工厂三分支/参数构造期拒绝、crop
像素级 golden/全图恒等/padding stride 输入/越界与退化全分支/默认参数 64×64、
downscale nearest 与 bilinear 手推字节向量 golden〔含 round-half-up 半值
锁定、逐通道独立 alpha 不预乘判别〕/floor 尺寸/scale=1 恒等/常值不变/线性
渐变容差/37×23 独立 double 参考交叉验证/退化输出拒绝/构造期非法 scale 与
未知插值拒绝、source→crop→downscale 图集成 golden、图内越界异常原样传播、
未实现类型 buildNodeGraph 显式 BadParam）；`test_public_boundary.cpp` 扩展
image_ops.hpp 包含与最小实例化（30→33 项）。debug/asan/ubsan 三预设全量
ctest 22/22（含 D435if 真机硬件冒烟，设备在位通过），asan/ubsan 零报告、编译
零警告。环境：x86_64 Linux，GCC 13，CMake presets debug/asan/ubsan；tsan 未跑
（本层单线程纯逻辑，DOD-02 并发矩阵归 `M4-07` 引擎，与 `M4-02` 先例一致）。
已知未测路径：nearest 下标 clamp 与 bilinear 饱和的上界分支数学上不可达
（scale ≤ 1 时 (out+0.5)×ratio < in 恒成立），仅作浮点防御。第一轮验证中
7 项中途失败经取证均为测试侧错误（期望缓冲尺寸误用、round-half-up 预测的
数学展开错误、边界测试 descriptor 缺参数声明），实现与冻结语义逐项吻合；
`M4-04`（卷积算子节点）为下一工作项。

2026-09-28：`M4-02` 完成关闭——Core 图像与节点契约落地（`rin_core`，零第三方
公开依赖，RULE-01）。新增 `include/rin/image_types.hpp`（`ImageU8`：Gray8/Rgba8、
共享不可变像素 `shared_ptr<const vector<uint8_t>>` 与 `NodeOutputSnapshot` 同款
承载、16 MiB 单图字节预算、stride/64 位乘法防回绕）+ `elementSize`；
`include/rin/image_node.hpp`（`IImageNode` 同步工作单元契约——参数构造期定型、
运行期热更新经帧边界重建实例实现"下一帧生效"；`ImageNodeFactory` 工厂接缝，
nullptr=注入型源节点；`effectiveParamValue` + 五个按 kind 的类型化读取助手）；
`include/rin/node_graph.hpp`（`NodeGraph` 编译图：`buildNodeGraph` 以
`validateWorkflowGraph` 为唯一判据（UI 预检/引擎准入不重复实现）+ 节点数准入
（默认 64，与假引擎同）+ 工厂异常/签名不符显式化为校验问题 + 稳定拓扑序；
`runNodeGraph` 单帧同步求值——注入型源节点语义（格式/有效性核对，M4-07 引擎
提供相机帧注入器）、算子输出防御性契约核对（数量/有效/格式违规显式抛出）、
节点异常原样传播；`M4-09` 视图契约之外的引擎侧延伸，端口类型系统复用
`PortType` 唯一类型面）。产出
[design/image_workflow_design.md](../design/image_workflow_design.md)：分层边界、
图像/端口类型语义、节点契约与参数模型、图编译与执行语义、M4-07 真引擎对齐项
（假引擎 `drainBoundary` peek 不消费缺陷不复刻，按"帧边界排空重建"消费）、
M4 节点目录表（与 `M5-08` 假目录签名一致，`grayify` 桥接节点注记）与逐算子
golden 测试项要求（DEC-012 的 FFT 内部 2 幂填充 + 归一化频率掩膜约束 golden
化：848×480 填充路径与 512×512 原生路径正弦注入→单频滤除各一组、填充/裁剪
往返误差、cutoff 跨尺寸语义不变）。测试（Independent-Verification-Agent 独立
编写执行，两轮）：新增 `tests/test_image_contracts.cpp`（ImageU8 正例/全负例/
回绕反例/预算边界/共享引用计数/参数模型三态生效值与五类类型化读取，133 项）、
`tests/test_node_graph.cpp`（buildNodeGraph 校验透传/准入/工厂失败显式化/注入
型/拓扑序/入边解析/空图，runNodeGraph 注入九分支/防御核对/异常传播/零拷贝
共享，148 项）；`test_public_boundary.cpp` 扩展三公开头包含与最小实例化
（30 项）。第一轮独立验证暴露一处实现缺陷：`buildNodeGraph` 组装只把入边写入
拓扑序位置而节点身份（id/descriptor/impl）滞留声明序，声明序≠拓扑序时边挂到
错误节点、消费者先于生产者执行（既有 M4-09/M5-08 测试未暴露——其图声明序恰为
拓扑序）；主循环修复为按拓扑序整体重排完整 Node 后第二轮复验 PASS（最小反例
程序输出逐字一致，原 7 项失败检查同批转绿）。debug/asan/ubsan 三预设全量
ctest 21/21（含 D435if 真机硬件冒烟，设备在位通过），asan/ubsan 零报告、编译
零警告。环境：x86_64 Linux，GCC 13，CMake presets debug/asan/ubsan；tsan 未跑
（本层单线程纯逻辑，DOD-02 并发矩阵归 `M4-07` 引擎与 `M5-08` 契约套件覆盖）。
已知未测路径：`buildNodeGraph` 防御性 Cycle 兜底经公开 API 不可达（校验先行
拒绝环）。`M4-03`（几何算子节点：裁切/降分辨率）为下一工作项。

2026-09-28：`M4-01` 完成关闭——[DEC-012](../decisions/DEC-012-image-operator-strategy.md)
冻结（`Accepted`）：**基础算子自研 + pinned kissfft `131.2.0`（BSD-3-Clause）承载
FFT 族，FFT 节点内部零填充到 2 幂**。取证方法：`tools/fft_bench/`（入库，方法/
口径/复现命令见其 README；内置正确性交叉校验——三方往返误差 < 1e-3、与 OpenCV
`DFT_COMPLEX_OUTPUT` 全复数谱幅值相对偏差 < 5e-3，任一失败退出码非 0）。三方
对照（系统 OpenCV 4.6.0 CCS 实数路径 / kissfft `kiss_fftndr` float / 朴素自研
radix-2；单线程 `-O2`；512×512、848×480、1024×512；每配置 300 次 × ≥3 轮，
min 口径）：512² 正/逆 OpenCV 1.02/1.11、kissfft 1.58/1.38、自研 3.53/3.19 ms；
848×480 原生 OpenCV 3.44/3.61，kissfft 8.85/8.91 ms（p50 常态 9-20，含大质
因子 848=2⁴×53 走通用质数路径，往返 ≥17.8 ms 超预算不可用）；1024×512（填充
目标）OpenCV 2.22/2.41、kissfft 3.62/3.10、自研 7.68/6.74 ms。裁决：填充后
kissfft 往返 6.72 ms（+节点内填充/裁剪 ~1-2 ms）与 OpenCV 原生 7.05 ms 相当
（帧预算 33.3 ms 的 21-26%），OpenCV 的唯一性能优势被填充策略消除，而其构建
体积（裁剪 core-only 静态 8.94 MB vs 32 KB）、CI 时长（configure 83 s + build
30.8 s @14 核 vs 合计 1.42 s；2 核 runner 估 +8-12 min/次 vs +2-5 s，估算已标注）
与 deb 捆绑面（DEC-009）高数量级，否决；全自研往返 14.42 ms（43% 预算，低频
状态 ~30 ms 贴帧）且需持续优化投入，否决。环境与限制：Intel Core Ultra 5 225H
（14 核，powersave，无 root 调频），进程级约 2 倍频率双峰如实记录，全部轮次
相对排序一致，决策不敏感；性能验收以 `M4-08` 真引擎实测为准。新依赖供应链
同步：`third_party/dependencies.lock` 行 `kissfft|…|7bce4153…|OFF|external`、
`cmake/Dependencies.cmake` 构建面裁剪（float 静态库，CMP0077 OLD 同款 cache
FORCE+恢复）、[kissfft 台账](../dependency_feedback/kissfft/ledger.md)（含
KIS-20260928-001：master `kiss_fftndr_alloc` 溢出预检对 float 2D 误报，锁定
131.2.0 规避）、台账索引追加。接线验证：`cmake --preset debug` 按 pin 拉取
（commit 校验通过）并产出 `libkissfft-float.a`；全量 ctest debug 19/19（含
D435if 真机硬件冒烟）。独立复验（Independent-Verification-Agent，5 项全过）：
按 README 自建基准 4 轮 CHECK 全 PASS、决策口径四个往返值与核心排序逐格复现
（OpenCV 7.17 / kissfft 填充 10.31 / 自研 14.55 / kissfft 原生 ≥17.94 ms；
kissfft 512f/512i/1024i 三格复验轮未采快态、min 为表值 2.14 倍而内部比例精确
一致——双峰环境性偏差，DEC-012 已注明引用口径）；方法论评审通过（计时口径与
README 声明吻合、SINK 防DCE、谱对照参考为真 OpenCV 全复数谱、kissfftndr 维序
`dims[ndims-1]` 实证）；master 缺陷最小程序取证（master HEAD 三配置三路径全部
返回 `(size_t)-1`/NULL，131.2.0 正常分配，机制数值 1070708/512/512→int 截断=4
吻合台账）；全新目录配置拉取 pin + `libkissfft-float.a` float 静态产物 + 无
测试/工具可执行产物 + Debug ctest 19/19 复现；文档一致性核对通过。**复验顺带
发现并随本分支修复三个既有缺陷**（均以干净 HEAD 复现证明与 M4-01 改动无关）：
① `tests/test_navigation.cpp` PageStateWitness 整体 `memcmp` 读到未初始化尾部
padding（56 vs 成员 52 字节），默认（无 CMAKE_BUILD_TYPE）配置下确定性失败——
改为逐成员 memcmp（契约语义即数据成员不被触碰，检查数 308→310）；②
`cmake/Dependencies.cmake` 离线检查在 `FETCHCONTENT_FULLY_DISCONNECTED=ON` 时对
已提供且校验通过的本地源仍无条件 FATAL（与文件头"离线构建"注释矛盾）——收窄为
仅在未提供本地源时报错；③ 同文件 `FETCHCONTENT_SOURCE_DIR_${下划线大写名}` 与
CMake FetchContent 约定（保留连字符，如 `EUI-NEO`）不符，eui-neo 本地源提示从未
被认领（在线路径静默 re-clone 同 pinned commit、离线路径无源可用）——改为按
原始大写名设置。三修复经 Independent-Verification-Agent 复验通过：① 无 build
type 全新配置 10/10、debug 5/5（原确定性失败消除）；② 离线全本地源配置成功、
在线全新目录配置不受影响、缺源/错源三例负向 FATAL 正确触发；③ 随 ② 的离线配置
无需手工 override 即完成（`FETCHCONTENT_SOURCE_DIR_EUI-NEO` 被认领）。已知限制
（低危不阻塞）：中止的 configure 可能在缓存遗留 realsense2 的
`BUILD_SHARED_LIBS=ON` FORCE 值（保存/恢复模式在 FATAL 路径不恢复，同目录重配置
需清空缓存）；完全离线配置仍需网络可达 github（librealsense 上游 json 拉取不受
`FETCHCONTENT_FULLY_DISCONNECTED` 约束，LRS-20260923-002 既有登记）。
`M4-02`（Core 图像与节点契约）为下一工作项。

2026-09-24：`M4-09` 契约冻结（部分完成，工作项未关闭）——commit `f27020b`..`727c776`
（分支 feat/core-workflow-view-contracts）：`include/rin/workflow_types.hpp`、
`include/rin/workflow_engine.hpp` 与 `validateWorkflowGraph` 纯逻辑入库；契约单测
204 项（Independent-Verification-Agent 独立编写执行）于 debug/asan/ubsan 三预设
0 失败，debug 全量 ctest 通过（realsense_hardware 无设备跳过）。第一轮独立验证
暴露两处实现与契约矛盾（NaN 范围端点绕过 `min>max` 校验；Rgba8 行宽
`width*4` uint32 回绕），已修复（`2ce18d3`）并经独立复验 PASS（三预设 204 项
0 失败、探针在位、无消毒器诊断）。环境：x86_64 Linux，GCC 13，CMake presets
debug/asan/ubsan。`M4-09` 完成判据剩余项：`M5-08` 假引擎与 `M4-07` 真引擎引用
该契约（随对应里程碑关闭）。
