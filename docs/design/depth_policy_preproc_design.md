# 深度策略预处理算子设计（`rin/depth_preproc.hpp`，M8）

> 状态：Accepted（DEC-018 冻结）
> 日期：2026-09-30
> 负责人：Linductor-alkaid

## 1. 定位与范围

`rin_core` 内的米制深度（float32，米）确定性预处理算子集，用于把 RealSense
深度流逐位复刻为视觉策略（roboparty E3-Parkour）`depth_encoder.onnx` 的输入
张量 (8, 18, 32) float32。纯 CPU、单线程、零第三方依赖（RULE-01/DEC-012）；
不进入 M4 工作流节点面（DEC-018 决策 1）。

非范围：adapter 米制深度通道（`FrameKind` 扩展）、ONNX 推理、工作流
Depth32 端口类型、训练侧随机噪声管线的复刻（噪声是训练专属增强，部署
不复刻——真机噪声天然落在训练分布内）。

## 2. 参考权威

部署参考实现（唯一权威，逐式对照）：

| 本设计 | roboparty 参考位置 |
|---|---|
| O1 填充 | `sim2sim_rpo_parkour.py` `preprocess_depth_frame_from_lowres` 的 `np.nan_to_num(nan/±inf → depth_far)` |
| O2 降采样 | cv2 `INTER_AREA`（64F 数学定义；参考实现 raw 已在 64×36 网格，此步为真实相机 848×480 → 64×36 的网格对齐新增） |
| O3 裁切 | `noise_model.py` `crop_and_resize`（`resize_shape=None` 分支）/ `sim2sim` `crop_depth`：`crop_region=(up,down,left,right)`，rows `[up, H−down)`、cols `[left, W−right)` |
| O4 高斯模糊 | `GaussianBlurNoiseCfg(kernel_size=3, sigma=1)` → `gaussian_blur_noise`；核式与 cv2 `getGaussianKernel` 同式 |
| O5 归一化 | `noise_model.py` `depth_normalization`：clip [near, far] 后 `(v−near)/(far−near)` |
| O7 时序抽样 | `delayed_visualizable_image`（`sensor_history_length=37, history_skip_frames=5, num_output_frames=8, delayed_frame_ranges=(0,1)`，部署取 delay=0）+ `sim2sim` `sample_depth_history`（欠帧首帧填充） |

## 3. 数据类型

`rin::DepthFrameF32`：单通道 float32 米制深度帧。镜像 `ImageU8` 纪律：
共享不可变像素（`shared_ptr<const vector<float>>`）、stride 为元素数
（≥ width）、单帧字节预算 16 MiB（`kMaxDepthFrameBytes`）、`make`/`wrap`
失败返回无效帧不抛异常、默认构造无效。像素值语义为
`distance_to_image_plane`（Z，米；librealsense Z16 × depth_scale 或射线
网格同义）。

## 4. 冻结配置 `PolicyDepthConfig`

对齐 `parkour_env_cfg.py` `SceneCfg.camera` 固定预处理与观测配置（默认值
即 E3-Parkour 部署值，全部构造期可见、`valid()` 判定）：

| 字段 | 默认值 | 来源 |
|---|---:|---|
| `gridWidth × gridHeight` | 64 × 36 | 射线网格 `PinholeCameraPatternCfg(width=64, height=36)` |
| `cropUp/cropDown/cropLeft/cropRight` | 18 / 0 / 16 / 16 | `CropAndResizeCfg(crop_region=(18, 0, 16, 16))` |
| `blurRadius / blurSigma` | 1 / 1.0 | `GaussianBlurNoiseCfg(kernel_size=3, sigma=1)` |
| `depthNear / depthFar` | 0.0 / 2.5 | `DepthNormalizationCfg(depth_range=(0.0, 2.5))` |
| `historyLength` | 37 | `data_histories={...: 37}` |
| `sampleCount / sampleSkip / sampleDelay` | 8 / 5 / 0 | `delayed_visualizable_image` 参数，部署取 delay=0（训练随机 0–1 帧，取最新帧落在授权内） |

派生量：`policyWidth = gridWidth − cropLeft − cropRight`（= 32）、
`policyHeight = gridHeight − cropUp − cropDown`（= 18）。

`valid()`：网格/裁切/历史全部为正且派生量 > 0；`depthNear < depthFar`；
`1 ≤ blurRadius ≤ 10`、`0 ≤ blurSigma ≤ 10`；`sampleCount ≥ 1`、
`sampleSkip ≥ 1`、`(sampleCount − 1)·sampleSkip + 1 + sampleDelay ≤
historyLength`（对齐 `delayed_visualizable_image.check_delay_bounds` 的通过
条件）、`historyLength ≤ 4096`。

## 5. 冻结算子公式

所有算子：输入帧无效（`valid()==false`）或参数越界一律抛
`std::invalid_argument`（显式失败，不静默）；中间量 double 累加，输出
float32；输出为紧凑新缓冲（stride = 宽）。

### O1 `fillDepthInvalid(in, farValue, invalidBelow = 0.0)`

```
out[x,y] = farValue            当 !isfinite(v) 或 v ≤ invalidBelow
         = v                   否则
```

真机语义：Z16 无效像素（0 × depth_scale = 0）与非有限值统一按"无信息 =
远距"处理（对齐仿真 `min_distance=0.1` 无命中 + max clip + 训练孔洞值
2.5 的三重语义）。

### O2 `resizeDepthArea(in, dstWidth, dstHeight)`

面积加权箱式平均（= cv2 `INTER_AREA` 对 64F 的数学定义）：

```
sx = srcW/dstW, sy = srcH/dstH（实数，仅允许缩小：dstW ≤ srcW 且 dstH ≤ srcH，
    否则抛——放大语义在 cv2 中回退 bilinear，属另一公式，显式拒绝避免分叉）
out(x,y) = Σᵢ Σⱼ wᵢ·hⱼ·src(i,j) / (sx·sy)
  其中源列 i 的水平覆盖区间为 [x·sx, (x+1)·sx) 与像素 i 的 [i, i+1) 之交
  长度 wᵢ ≥ 0（行同理 hⱼ）；Σwᵢ = sx、Σhⱼ = sy 恒成立（归一化不依赖
  逐权重除法）。
```

double 累加，一次转 float32。`dst == src` 尺寸时逐像素恒等。

### O3 `cropDepth(in, up, down, left, right)`

```
out 行域 [up, srcH − down)，列域 [left, srcW − right)；紧凑新缓冲。
up + down ≥ srcH 或 left + right ≥ srcW → 抛 std::invalid_argument。
```

与 `crop_and_resize`/`crop_depth` 索引逐位一致（(18,0,16,16) 于 64×36 →
18×32，无插值）。

### O4 `gaussianBlurDepth(in, radius, sigma)`

一维核（K = 2·radius + 1，与 M4-04 `gaussian_blur`/cv2 `getGaussianKernel`
同式）：

```
σ = 0：δ 核（恒等输出）
否则 k[i] = exp(−(i−radius)²/(2σ²))，k 归一化（Σk = 1，double）
```

可分离两趟（水平 → 垂直），中间量 double 不量化；边界 reflect-101
（`−1 → 1、n → n−2`，尺寸为 1 的维度下标恒 0；= cv2 `GaussianBlur` 64F
默认 `BORDER_REFLECT_101`）；输出一次转 float32。radius ∈ [1,10]、
sigma ∈ [0,10] 越界抛。

### O5 `clipNormalizeDepth(in, near, far)`

```
out = (clamp(v, near, far) − near) / (far − near)；near ≥ far → 抛。
```

与 `depth_normalization`（`normalize=True, output_range=(0,1)` 分支）一致。

### O6 组合 `preprocessPolicyDepthFrame(rawMetricDepth, config)`

顺序冻结（失败链任一步抛即透传）：

```
fillDepthInvalid(far = depthFar)
→ resizeDepthArea(gridWidth, gridHeight)     // 任意输入分辨率 → raw 射线网格
→ cropDepth(cropUp, cropDown, cropLeft, cropRight)
→ gaussianBlurDepth(blurRadius, blurSigma)
→ clipNormalizeDepth(depthNear, depthFar)
```

输出恰为 `policyHeight × policyWidth`（默认 18×32）。与参考实现的唯一
显式偏差：填充先于降采样（DEC-018 决策 2——无效 0 不得拉低面积平均的
邻域均值；参考实现 raw 已在 64×36 网格上，该顺序差异只由真实相机的
降采样步骤引入）。

### O7 时序历史 `PolicyDepthHistory(config)`

- 容量 `historyLength`（构造期定型）；`append(frame)`：帧尺寸必须等于
  `policyHeight × policyWidth` 否则抛；环满覆盖最旧。单写者约定：append
  与 sample 由同一执行上下文调用（50 Hz 策略 tick 内），类不加内部锁。
- `sample()` 返回 CHW float32（`sampleCount × policyHeight × policyWidth`，
  oldest → newest，连续存储）：

```
若历史为空：全 0。
否则：索引数组 idx[i] = historyLength − (sampleCount − 1 − i)·sampleSkip
      − 1 − sampleDelay（i = 0..sampleCount−1，单调递增， oldest → newest）；
      不足 historyLength 帧时头部以首帧复制填充（idx 指向填充后序列），
      与 sim2sim `sample_depth_history` 逐位一致。
      默认配置 idx = {1, 6, 11, 16, 21, 26, 31, 36}。
```

训练侧欠帧语义为 Isaac `AsyncCircularBuffer` reset 零填充 + 观测管理器
`valid` 遮罩，部署参考选择首帧填充消除启动瞬态；本设计随部署参考
（首帧填充），全 0 仅保留"从未 append"分支以匹配 sim2sim 空缓冲分支。

## 6. 测试要求（golden 项）

- O1：NaN/+Inf/−Inf/0/负值/正常值混合 golden；`invalidBelow` 非默认值。
- O2：848×480 → 64×36 与测试内双精度独立参考交叉（随机图，
  |diff| ≤ 1e−6 绝对）；2×2 → 1×1、3×2 → 2×1 等小图手推 golden（分数
  覆盖权重）；整数倍（4×）恒比；同尺寸恒等；放大拒绝；无效帧拒绝。
- O3：64×36 (18,0,16,16) → 18×32 索引逐位；退化/越界拒绝全分支。
- O4：核系数 golden（radius=1, σ=1 → [0.274068619, 0.451862761,
  0.274068619]）；σ=0 恒等；常值图恒等（质量守恒）；角点脉冲
  reflect-101 折返手推（能量 = (边系数+中心系数)²，reflect-101 不复制
  边缘像素，角点无单位能量守恒，与 cv2 BORDER_REFLECT_101 同语义）+
  内部脉冲单位能量守恒；1×N 窄图；与测试内直接 2D double 参考 +
  独立双精度参考交叉（|diff| ≤ 1e−6）。
- O5：clip 双侧 + 区间内恒比 + (near, far) 退化拒绝。
- O6：合成 848×480 全链 vs 测试内独立双精度参考（按冻结顺序实现）逐像素
  |diff| ≤ 1e−6；输出尺寸/值域 [0,1] 断言。
- O7：欠帧首帧填充分支（append 1/5/36 帧的 sample 逐位 golden）、满环
  滚动（append 40 帧）、索引集 {1,6,…,36}、空历史全 0、CHW 排布
  oldest→newest、尺寸不符抛、4096 帧历史上限校验。
- 公开边界：公开头零第三方 include。

## 7. 关联文档和工作项

- [DEC-018](../decisions/DEC-018-depth-policy-preproc-operators.md)
- [M8 计划](../plans/m8-depth-policy-preproc.md)
- 既有算子纪律参照：[image_workflow_design.md](image_workflow_design.md) §7
