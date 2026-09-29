# workflow_bench：M4-08 848×480 典型工作流全链路吞吐实测

M4-08 工作项的实测工具（[M4 计划](../../docs/plans/m4-cv-node-workflow.md)）：
用 M4-07 工作流真引擎（`createWorkflowEngine`，[DEC-013](../../docs/decisions/DEC-013-workflow-execution-model.md)
执行模型）测量 848×480 典型工作流的全链路吞吐与逐节点耗时。与 `fft_bench`
（M4-01/DEC-012 决策取证，独立编译）不同，本工具链接仓库库目标、随项目构建
（`RIN_BUILD_TOOLS=ON` 默认开启），不进入 ctest（无 `add_test`）。

纪律：只记录实测值与方法，不宣称未验证的性能目标（DEC-012/DEC-016）；本工具
的数字不得反向用作 FFT 选型声明（DEC-012 的选型证据在 `tools/fft_bench/`）。

## 测量对象与链定义

帧输入：848×480 Rgba8 合成图案（确定性：梯度 + 双频正弦，非退化内容），
经 `WorkflowFrameSource` 零拷贝注入（共享不可变缓冲），刻意隔离"引擎 + 算子"
成本与相机帧 → ImageU8 转换成本（后者归 Adapter/应用层，RULE-01），不代表
librealsense 采集路径。

引擎配置：发布默认（`maxInFlight=2`、帧泵 5ms）；Executor 默认配置
（自适应线程数 ≈ hw_concurrency、work stealing 开启）。

| 链 | 图（source 输出 Rgba8，其余 Gray8） | 说明 |
| --- | --- | --- |
| `typical` | source → grayify → gaussian_blur(默认 r=3,σ=1.5) → fft_lowpass(默认 cutoff=0.2) → hist_eq | 相机帧 → 灰度 → 模糊 → 频域低通 → 均衡的典型全链路；FFT 在 848×480 填充路径（内部 2 幂填充 → 1024×512） |
| `downscale` | source → downscale(bilinear, 0.5) → grayify → gaussian_blur → fft_lowpass → hist_eq | 降分辨率前置变体（设计文档 §7 "强振铃先降分辨率"使用建议的量化对照）；FFT 在 424×240 填充路径（→ 512×256） |
| `spatial` | source → grayify → gaussian_blur → hist_eq | 无 FFT 空间域参照（"改用空间域算子"建议的量化对照） |

## 输入模式与测量协议

两种输入模式，各跑 3 轮取中位数：

- `saturate`：帧源每次探测都交付新帧 → 测引擎最大处理吞吐。输入速率受帧泵
  粒度上限约束（1 帧/5ms tick ≈ 200 fps）；在飞满载时新帧显式丢弃并计入
  `droppedFrames`（EXEC-07 显式过载路径，预期行为而非缺陷）。
- `camera30`：帧源按绝对时间表 30 fps 交付（D435if RGB 默认速率）→ 余量检查
  （预期 `droppedFrames == 0`、吞吐 ≈ 输入速率）。

单轮协议：新建引擎实例 → `applyGraph`/`start` → warmup 60 帧（跳过启动瞬态）
→ 测量窗：以 warmup 退出时的 `processedFrames`/`droppedFrames` 为基线，推进
240 帧（camera30 下 ≈ 8 s）→ 读取统计 → `stop()`（排空收敛 Idle）。

双口径交叉：

- `ext fps`：测量窗完成帧数 / 墙钟时长（`steady_clock`）。完成数轮询粒度
  2ms、统计发布经帧泵 tick，窗口边界偏差 ≤ 1 帧（<1%）。
- `engine fps`：引擎自报 `WorkflowStats.endToEndFps`（滚动窗口）。
- 逐节点耗时：`NodeStats.avgCostMs`（滚动窗口 32）取外测 fps 最接近中位数的
  代表轮；`source` 为注入型源节点，无 apply 可测，恒为 0。

## 构建与运行

```sh
cmake --preset release            # 实测用 Release（release 预设为 -O3 -DNDEBUG；
                                  # DEC-012 fft_bench 基准为显式 -O2，跨工具量级
                                  # 对照时注意该差异）
cmake --build --preset release --target workflow_bench
./build/release/tools/workflow_bench
```

无命令行参数；一次运行依次执行 3 链 × 2 模式 × 3 轮（约 2 分钟）。退出码
非 0 表示存在链校验失败、引擎 Failed 或阶段超时。

## 结果记录与环境限制

- 实测记录落盘：[docs/benchmarks/workflow-throughput-848x480.md](../../docs/benchmarks/workflow-throughput-848x480.md)
  （环境、方法、数值、限制与使用建议）。
- 实测值随 CPU 频率状态浮动（本机 powersave 无频率锁定，DEC-012 同款环境限制）：
  独立复验实测同一（链 × 模式）内相邻轮吞吐摆动可达约 2.9 倍（低频态到频率
  恢复的过渡被轮窗采样）。因此 **saturate 模式的绝对数值只宜作量级与模式内
  相对解读，精确数值复现需频率锁定**（如 performance governor，需 root）；
  camera30 模式受频率漂移影响小（30 fps 下余量大，轮间实测 29.9–30.1 fps）。
  per-node 耗时在相机定速模式（低负载、低频）下系统性高于饱和模式，均为真实
  测量条件，读数时应连同模式一起解读。
- 双口径（ext fps 与 engine fps）在 camera30 与贴近帧泵上限的链上交叉一致性
  好（实测差 < 3%）；`typical | saturate` 等中间速率链上 engine fps 为滚动
  窗口瞬时速率的单点采样，与外测均值可偏离至 ~10–30%（帧批量完成 + 频率
  漂移），以 ext fps 为准。
- 本工具测量"引擎 + 算子"吞吐：不含相机采集、帧转换、UI 显示路径；不构成
  采集端到端性能声明（M5-07 真机端到端验收另行覆盖）。
