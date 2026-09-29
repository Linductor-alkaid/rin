# 848×480 典型工作流全链路吞吐实测（M4-08）

> 状态：Active（M4-08 实测记录，真引擎）
> 日期：2026-09-29
> 工具：[tools/workflow_bench/](../../tools/workflow_bench/README.md)（方法与复现命令的单一事实源）
> 关联：[M4 计划](../plans/m4-cv-node-workflow.md)（`M4-08`）、
> [DEC-012](../decisions/DEC-012-image-operator-strategy.md)、
> [DEC-013](../decisions/DEC-013-workflow-execution-model.md)、
> [image_workflow_design.md](../design/image_workflow_design.md) §7

## 目的与纪律

M4-08 要求对 848×480 典型工作流做真引擎全链路吞吐实测并落盘记录（只记录
实测值与方法，不宣称未验证的性能目标；DEC-012 选型基准数字不得用作吞吐
声明）。本记录是 M4 里程碑的吞吐事实源；工程规范 §7 证据等级中"满足性能
目标"级别的声明不在本记录范围内（项目当前无已冻结的吞吐目标）。

## 方法（摘要，完整定义见工具 README）

- 被测面：`createWorkflowEngine` 真引擎 + M4 真实算子（`rin_core`/`rin_workflow`，
  Release `-O3 -DNDEBUG`），发布默认引擎配置（`maxInFlight=2`、帧泵 5ms）与
  Executor 默认配置（自适应线程 ≈ 14、work stealing 开启）。注意 DEC-012
  fft_bench 选型基准为显式 `-O2`，跨工具量级对照时存在该编译口径差异。
- 帧输入：848×480 Rgba8 确定性合成图案，经 `WorkflowFrameSource` 零拷贝注入；
  不含 librealsense 采集与相机帧转换（Adapter 层职责，RULE-01），不含 UI 显示。
- 三条典型链：`typical`（source→grayify→gaussian_blur(r=3,σ=1.5)→
  fft_lowpass(cutoff=0.2)→hist_eq，FFT 走 848×480 填充路径 →1024×512）、
  `downscale`（source→downscale(bilinear,0.5)→grayify→gaussian_blur→
  fft_lowpass→hist_eq，FFT 走 424×240 填充路径 →512×256）、`spatial`
  （source→grayify→gaussian_blur→hist_eq，无 FFT 参照）。
- 两种输入模式：`saturate`（帧源每次探测交付新帧 → 最大处理吞吐；输入速率受
  帧泵粒度上限 ≈ 200 fps 约束，在飞满载丢弃为 EXEC-07 显式预期行为）、
  `camera30`（帧源按绝对时间表 30 fps 交付 → 相机速率余量检查）。
- 协议：每轮 warmup 60 帧后，以完成帧数基线推进 240 帧计时（camera30 下
  ≈ 8 s）；每（链 × 模式）3 轮取中位数。双口径交叉：外部墙钟
  fps（窗口边界偏差 ≤ 1 帧，<1%）与引擎自报 `endToEndFps`（滚动窗口）；
  逐节点耗时为 `NodeStats.avgCostMs`（滚动窗口 32）代表轮。

## 环境

- x86_64 Linux（Ubuntu 24.04，内核 7.0.0-34-generic），GCC 13.3.0。
- Intel Core Ultra 5 225H（14 核），cpufreq governor `powersave`——与 DEC-012
  基准同机的双峰频率环境：同配置轮间吞吐差异可达 ±15%，低负载（camera30）
  下 per-node 耗时系统性高于高负载（saturate）模式，均为真实测量条件。
- 构建口径：`cmake --preset release`（`-O3 -DNDEBUG`），kissfft 131.2.0 float
  随项目同口径构建；`workflow_bench.cpp` 经 `-Wall -Wextra -Werror` 单元复核
  零警告。

## 实测数据

### 主记录（最终二进制完整运行，2026-09-29）

| 链 | 模式 | ext fps | engine fps | 逐节点 avgCostMs（ms） | 测量窗丢弃 |
| --- | --- | ---: | ---: | --- | ---: |
| typical | saturate | 102.9 | 107.9 | grayify 0.19 / blur 4.64 / fft_lowpass 10.43 / hist_eq 0.22 | >0（≈225/窗，输入 200 fps 封顶） |
| typical | camera30 | 30.0 | 30.1 | grayify 0.25 / blur 6.40 / fft_lowpass 13.48 / hist_eq 0.21 | 0 |
| downscale | saturate | 159.9 | 157.1 | downscale 4.01 / grayify 0.05 / blur 1.22 / fft_lowpass 2.87 / hist_eq 0.07 | 少量（9–39/窗） |
| downscale | camera30 | 30.0 | 30.1 | downscale 5.05 / grayify 0.06 / blur 1.65 / fft_lowpass 4.29 / hist_eq 0.11 | 0 |
| spatial | saturate | 197.9 | 197.7 | grayify 0.24 / blur 5.67 / hist_eq 0.41 | 0 |
| spatial | camera30 | 30.0 | 30.1 | grayify 0.29 / blur 6.71 / hist_eq 0.51 | 0 |

注：saturate 模式下主表数值为标称频率态的单次代表运行；`typical | saturate`
的丢弃为帧泵捕获速率（≈200 fps）高于处理吞吐时的显式过载路径，非缺陷；
ext 与 engine fps 的双口径交叉边界见"稳定性参考"。

### 稳定性参考（独立完整运行汇总，同日）

三次独立完整运行（主循环一次 + Independent-Verification-Agent 两次）的
camera30 工况高度稳定：各链 29.94–30.04 fps、dropped 全 0（18 轮无一例外），
camera30 下 `typical` 链 fft_lowpass 10.4–14.9 ms/帧。

saturate 工况对频率状态敏感：`typical | saturate` 三次运行轮值分别为
~100–104 fps（标称频率态两次）、39.2/51.1/112.1 fps（复验第二次运行，机器
处于 run1 之后低频态，频率恢复过程被轮窗采样捕获）；`downscale | saturate`
159–198 fps；`spatial | saturate` 稳定 ~197 fps（贴近帧泵输入上限）。同配置
相邻轮摆动实测可达约 2.9 倍——saturate 绝对数值只作量级与模式内相对解读。

双口径交叉的实测边界：camera30 与贴近帧泵上限的链上 ext/engine fps 差
< 3%；`typical | saturate` 等中间速率链上 engine fps 为滚动窗口瞬时速率的
单点采样（测量窗结束时刻读一次），与外测均值实测可偏离至 ~9–29%（帧批量
完成 + 频率漂移），交叉结论以 ext fps 为准。

独立复验：Independent-Verification-Agent 按 [工具 README](../../tools/workflow_bench/README.md)
完成静态方法学核对（README 与源码一致性、基线/有界等待/口径定义）与两次
完整独立重跑、11 项逐项核对（9 项 PASS、2 项针对本记录初稿失实注记的 FAIL，
随本记录修订闭合），运行输出留档 /tmp/wb_run1.txt、/tmp/wb_run2.txt；完整
结论见 [M4 计划验证记录](../plans/m4-cv-node-workflow.md)。

## 解读与使用建议（实测支撑）

1. **典型 FFT 链满足 30 fps 相机速率且有余量**：`typical` 链在 camera30 下
   零丢弃、稳定 30.0 fps（三次独立运行 18 轮 29.94–30.04 fps、零丢弃，为本
   记录最强的可复现结论）；标称频率态下饱和处理 ≈ 102 fps（约 3.4 倍于
   30 fps 输入），低频态下饱和吞吐显著下降（39–112 fps 实测）但 camera30
   余量不受影响。帧预算口径：串行逐节点耗时和 ≈ 16.4–19.9 ms（饱和/定速
   两种频率状态）< 33.3 ms。
2. **FFT 低通为典型链主导成本**：848×480 填充路径（1024×512）下
   `fft_lowpass` 10.4–13.5 ms/帧，占链内算子串行耗时的约 70–80%；
   grayify/hist_eq 均 ≤ 0.3 ms，高斯模糊 4.6–6.4 ms。
3. **"强振铃先降分辨率"的量化支撑**（[image_workflow_design.md](../design/image_workflow_design.md)
   §7 边界语义披露的使用建议）：`downscale` 前置变体把 FFT 成本从
   10.4–13.5 ms 降到 2.9–4.3 ms（填充目标 1024×512 → 512×256），链饱和吞吐
   从 ≈ 102 fps 升到 ≈ 160–190 fps；降分辨率同时按 §7 语义物理上抑制填充
   振铃的相对可见尺度。强振铃场景的替代方案 `spatial`（空间域高斯模糊 +
   均衡）饱和 ≈ 198 fps（已贴近帧泵 200 fps 输入上限）且逐帧成本 ≈ 6–7 ms。
4. **过载路径实测可见**：saturate 模式下 `droppedFrames` 显式增长、
   `inFlight` 有界（引擎契约测试覆盖的语义在吞吐面得到一致观察）。

## 限制

- 不含相机采集、相机帧 → ImageU8 转换与 UI 显示路径；M5-07 真机端到端验收
  另行覆盖采集与显示的真实链路。本记录不外推为"应用级帧率"。
- 单机实测（powersave、非 CI 硬件、无频率锁定）：数值代表本机条件下的
  可复现方法与量级，不构成跨平台或最低配置的性能承诺；saturate 模式轮间
  波动实测可达约 2.9 倍（频率态漂移），精确数值复现需频率锁定（需 root，
  本记录未执行）。
- 测量窗边界与统计发布粒度引入 ≤ 1 帧的窗口偏差（<1%）；`saturate` 模式
  输入速率受帧泵 5ms 粒度上限（≈ 200 fps）约束，高于该上限的引擎吞吐不可
  由本工具分辨（`spatial | saturate` 197.9 fps 已贴近该上限，其真实处理上限
  高于实测值）。
