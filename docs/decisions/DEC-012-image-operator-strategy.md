# DEC-012：图像算子实现策略——自研基础算子 + kissfft 承载 FFT 族

> 状态：Accepted
> 日期：2026-09-28
> 负责人：Linductor-alkaid
> 冻结里程碑：M4（`M4-01` 基准冻结）
> 替代/被替代：无（取代总计划"暂定决策"中 DEC-012 的暂定表述）

## 背景与问题

M4 要在 Core 交付六个算子节点：裁切、降分辨率、自定义卷积核、高斯模糊、直方图
均衡、FFT 高通/低通/带通族（`SCOPE-08`），全部为 `EXEC-07` 下 Executor 有限任务
承载的同步 CPU 工作单元（`RULE-07`），公开契约零第三方类型（`RULE-01`）。`M4-01`
要求在三条实现路线中取证冻结：

- A. 引入 OpenCV（external pin），算子与 FFT 全部用库；
- B. 基础算子自研 + 小型 FFT 第三方库（kissfft 类）承载 FFT 族；
- C. 全自研（含 FFT）。

对比维度为构建体积、CI 时长、许可证与性能（512² 与 848×480 灰度 FFT 实测）。
848×480 是 D435if 默认流的原生尺寸（DEC-004），也是 FFT 节点的主要工作尺寸；
30 fps 下整帧预算 33.3 ms，FFT 滤波节点一次正逆往返（正向变换 + 频域掩膜之外的
逆变换与归一化）须显著低于该预算才能与链上其他节点共存。

关键约束：848 = 2⁴×53 含大质因子，480 = 2⁵×3×5 为平滑尺寸——纯 2 幂 radix-2
FFT 无法原生处理 848×480，必须零填充（2 幂化）或实现 Bluestein/混合基，这是
路线 B/C 的核心设计变量。

## 决策

采用 **路线 B：基础算子自研 + pinned kissfft（`131.2.0`）承载 FFT 族**：

1. **基础算子自研**（`M4-03`..`M4-05`）：裁切、降分辨率、卷积核、高斯模糊、
   直方图均衡均为可黄金向量直测的简单数值管线，自研无依赖、可控边界
   （`RULE-01` 天然满足）。
2. **FFT 族用 kissfft**（`M4-06`）：pin `131.2.0`
   （`7bce4153c6bc8aba2db0e889e576f9d00505cbe1`，BSD-3-Clause），`KISSFFT_DATATYPE=float`
   静态库，链接 `kissfft::kissfft`；不用其 SIMD 变体（按 4 路并行流处理，对逐帧
   单图路径引入成帧延迟，plain float 已满足预算）。pin 选择与理由见
   kissfft 台账 [KIS-20260928-001](../dependency_feedback/kissfft/ledger.md)
   （master 的 `kiss_fftndr_alloc` 溢出预检误报使 float 2D 配置不可用，`131.2.0`
   为最后一个无此缺陷的发布 tag）。
3. **FFT 节点内部零填充到 2 幂**：848×480 → 1024×512（宽高向上取 2 幂），掩膜
   在填充分辨率上构造，节点输出裁回原尺寸。契约参数面不变：截止频率为归一化
   频率（`M4-09` 契约 `cutoff`/`lowCut`/`highCut` ∈ [0,1]），填充只改变内部
   DFT 尺寸、不改变归一化频率语义。理由：kissfft 对含大质因子尺寸（848）走
   通用质数路径，实测单次变换 2.5-5 倍于 2 幂尺寸（见下表），不可用；填充后
   kissfft 性能与 OpenCV 原生处理同尺寸相当。该节点级设计约束随
   `image_workflow_design.md`（`M4-02` 产出）细化为 golden 测试项。
4. **不引入 OpenCV**：性能收益仅在"非 2 幂原生处理"一项（填充策略已消除该差异），
   而构建体积、CI 时长、deb 捆绑面（DEC-009 自包含分发）代价高出数量级（见
   下表）。

## 备选方案（四维取证，2026-09-28 实测）

性能基准方法与完整数据：`tools/fft_bench/`（方法、构建与复现命令见其 README；
结果为 CSV）。环境：x86_64 Linux（GNOME/XWayland），Intel Core Ultra 5 225H
（14 核，powersave governor，无 root 调频权限），GCC 13.3.0 `-O2 -DNDEBUG`
C++20，系统 OpenCV 4.6.0，kissfft `131.2.0` float 静态库；单线程，每配置 warmup
10 次后测 300 次 × ≥3 轮。本机 powersave 下进程整体存在约 2 倍的频率状态双峰
（P/E 核调度与调频所致，绑核后依旧），故以跨轮最小值（min）为稳定统计量并同时
给出典型 p50 带；**所有轮次中三种实现的相对排序一致，决策对频率状态不敏感**。
独立复验（Independent-Verification-Agent，2026-09-28）确认：全部 CHECK 通过、
决策口径四个往返值与核心排序逐格复现（OpenCV 原生 7.17 / kissfft 填充 10.31 /
自研 14.55 / kissfft 原生 ≥17.94 ms）；复验轮次 kissfft 的 512f/512i/1024i 三格
未采到快态（min 为表值 2.14 倍，恰在双峰边界），但内部比例与表值精确一致——
引用绝对数值时须注明快态采样条件，排序与比例结论不受影响。

单次 2D FFT 耗时（ms，min 口径；正/逆）：

| 配置 | OpenCV（CCS 实数路径） | kissfft（`kiss_fftndr`） | 自研 radix-2（复数行列） |
| --- | --- | --- | --- |
| 512×512 | 1.02 / 1.11 | 1.58 / 1.38 | 3.53 / 3.19 |
| 848×480（原生） | 3.44 / 3.61 | 8.85 / 8.91（p50 常态 9-20） | 不适用（须填充） |
| 1024×512（848×480 的填充目标） | 2.22 / 2.41 | 3.62 / 3.10 | 7.68 / 6.74 |

正逆往返（848×480 等效工作量，min 口径；预算 33.3 ms/帧）：

| 路线 | 往返 | 占帧预算 |
| --- | --- | --- |
| OpenCV 原生 848×480 | 7.05 ms | 21% |
| kissfft + 2 幂填充（1024×512 实测） | 6.72 ms（另加节点内填充/裁剪拷贝，约 1-2 ms 量级） | ~26% |
| 自研 radix-2 + 2 幂填充 | 14.42 ms | 43% |
| kissfft 原生 848×480（不填充） | ≥17.76 ms（p50 常态 28 ms 以上） | ≥53%，不可用 |

构建体积与 CI 时长：

| 维度 | OpenCV（裁剪 core-only 静态） | kissfft | 全自研 |
| --- | --- | --- | --- |
| 源码拉取 | 浅 clone（`--depth 1 --branch 4.6.0`）285 MB | 1.5 MB | 0 |
| configure | 83.07 s | 与 build 合计 1.42 s | — |
| build（`-j14` 本机） | 30.75 s（仅 core；含 FFT 所需全部） | （含上） | 秒级随项目 |
| 产物 | 静态 `libopencv_core.a` 8.94 MB（系统共享库 core 3.6 MB + imgproc 5.0 MB + 传递依赖 19 项） | 静态库 32 022 B | 自有源码 |
| CI 影响（估算） | 现 CI 全程 5.5-9 min（2 核 runner）；configure 近单线程 + build 按核数折算，估 +8-12 min/次，冷缓存另加 285 MB 拉取 | 估 +2-5 s/次 | 0 |
| 许可证 | Apache-2.0 | BSD-3-Clause | 无 |

CI 影响为按核数折算的估算（未在 2 核 runner 实测 OpenCV 构建），其余为实测值；
OpenCV 若供其余算子使用还需 imgproc（+5.0 MB 共享库）。

裁决：

- **A（OpenCV）否决**：性能优势仅在非 2 幂原生处理，填充策略下 kissfft 与之相当
  （6.72 vs 7.05 ms，均在预算 20-26%）；而构建体积（8.9 MB+ vs 32 KB）、CI 时长
  （+分钟级/次 vs +秒级/次）、deb 捆绑面（DEC-009）高出数量级。
- **B（选定）**：kissfft + 填充满足实时预算且与 OpenCV 性能相当；32 KB/1.4 s 的
  集成代价；BSD-3-Clause 兼容；FFT 数值正确性由上游实现 + golden 测试保障。
- **C（全自研）否决**：朴素 radix-2（填充后）单次变换 2.1-2.4 倍于 kissfft，
  往返 14.4 ms 占预算 43%（powersave 低频状态下约 30 ms，贴近整帧），需继续投入
  实数打包/转置列变换等优化与全套数值测试才能追平；在 B 以 32 KB 代价达标时，
  自研 FFT 的收益只有"零依赖"，不构成理由。

## 影响与风险

- **影响范围**：`M4-03`..`M4-06`（按本决策实现）、`M4-08`（性能实测以真引擎为准，
  本基准数字不替代真机记录）、`M4-02`（`image_workflow_design.md` 须按"FFT 节点
  内部 2 幂填充 + 归一化频率掩膜"细化节点设计）；`RULE-04` 合规（pin/lock/许可证/
  台账见"关联"）。
- **风险与缓解**：
  1. *kissfft 上游活跃度低、master 存在未发布缺陷*（KIS-20260928-001）：锁定
     `131.2.0` 隔离；升级须按台账移除条件验证。FFT 调用面收敛在 M4-06 单一节点
     实现内，必要时可整体替换 FFT 后端而不外溢。
  2. *填充引入的频域语义偏差*：掩膜按归一化频率在填充分辨率构造，阻带/通带边界
     的实际离散频率栅格随 DFT 尺寸变化；`M4-06` 的正弦注入→单频滤除 golden 测试
     须在 848×480（填充路径）与 512×512（原生路径）两尺寸各覆盖一组。
  3. *基准条件限制*：本机 powersave 双峰、非 CI 硬件；性能验收以 `M4-08` 真引擎
     实测为准（DEC-016 同款纪律），本基准只支撑选型。
- **台账**：新增 kissfft 台账（`docs/dependency_feedback/kissfft/ledger.md`），
  含 KIS-20260928-001；`third_party/dependencies.lock` 与
  `cmake/Dependencies.cmake`（kissfft 构建面裁剪）同步。

## 验证方式

1. 基准可复现：`tools/fft_bench/`（内置正确性交叉校验：三方往返误差 < 1e-3、
   kissfft/自研与 OpenCV 全复数谱幅值相对偏差 < 5e-3，任一失败退出码非 0）；
   独立复验由 Independent-Verification-Agent 按其 README 环境重跑并核对排序。
2. `M4-06` 数值测试：正弦注入→单频滤除（两种尺寸路径）、填充/裁剪往返误差、
   掩膜归一化频率语义。
3. `M4-08`：848×480 典型工作流全链路吞吐实测记录（真引擎），不得引用本基准
   数值作为性能声明。
4. 构建接线验证：`cmake --preset debug` 按 pin 拉取 kissfft（commit 校验）并
   构建 `kissfft::kissfft`；全量 ctest 回归（本决策变更中已执行，debug 19/19）。

## 关联文档和工作项

- [M4 里程碑](../plans/m4-cv-node-workflow.md)：`M4-01`（本决策）、`M4-02`、
  `M4-03`..`M4-06`、`M4-08`。
- [Rin 实施总计划](../plans/rin-implementation-plan.md)：`SCOPE-08`、`EXEC-07`、
  `RULE-01`/`RULE-04`/`RULE-07`、DEC-016 交错顺序。
- [kissfft 依赖台账](../dependency_feedback/kissfft/ledger.md)（KIS-20260928-001）、
  `third_party/dependencies.lock`、`cmake/Dependencies.cmake`。
- 基准工具：`tools/fft_bench/`（`fft_bench.cpp` + README：方法、口径、复现命令、
  kissfftndr 维序注记）。
- [DEC-004](DEC-004-default-stream-request.md)（848×480 默认流）、
  [DEC-009](DEC-009-self-contained-deb-distribution.md)（deb 捆绑面对体积维度的影响）。
