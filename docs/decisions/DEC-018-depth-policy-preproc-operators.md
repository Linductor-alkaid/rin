# DEC-018：米制深度策略预处理算子（Core 纯函数层，工作流节点面暂不扩展）

> 状态：Accepted
> 日期：2026-09-30
> 负责人：Linductor-alkaid
> 冻结里程碑：M8
> 替代/被替代：无（新增消费面；不改 DEC-012/DEC-017 已冻结语义）

## 背景与问题

用户需要把外部视觉策略项目（roboparty E3-Parkour，Isaac Lab 训练 +
MuJoCo sim2sim 部署）的深度预处理管线在 RealSense 真机侧逐位复刻：真机
送入 `depth_encoder.onnx` 的 (8, 18, 32) float32 张量必须与仿真部署参考
实现语义完全一致。该管线的确定性部分为六个算子：无效深度填充、面积降
采样、裁切、高斯模糊、裁切归一化、时序环形历史抽样（详见
[depth_policy_preproc_design.md](../design/depth_policy_preproc_design.md)
冻结公式）。

Rin 现状：M4 工作流算子面建立在 `PortType::Gray8/Rgba8` 字节域
（`ImageU8`）之上，面向视觉预览；米制深度（float32，米）不在端口类型
系统内，8-bit rendition（2.5 m/255 ≈ 1 cm 量化 + 伪彩不可逆）不能作为
网络输入。缺失这组算子。

## 决策

1. **算子落在 Core 纯函数层（`rin_core` 新公开头 `rin/depth_preproc.hpp`），
   不扩展工作流节点目录**。理由：
   - 逐帧无状态：`IImageNode::apply` 契约要求实现逻辑只读、可并发调用
     （image_node.hpp），时序环形历史抽样（37 帧跨帧状态）与该契约冲突；
     状态时序节点需要先做引擎执行模型扩展（DEC-013 修订），超出本次
     "算子补充"范围。
   - 类型系统成本：米制深度需要新 `PortType::Depth32`、float 承载的图像
     类型、图校验/UI 端口渲染/连线过滤全套契约扩展；而策略预处理是
     50 Hz 策略时钟驱动的部署管线，天然不经过工作流画布。两类需求
     （交互式视觉工作流 vs 固定策略预处理链）解耦，后者先以纯函数
     满足，前者留待真实需求出现再立项。
   - 纯函数可直接被后续 adapter 米制深度通道（`FrameKind` 扩展，另行
     立项）与策略运行时调用，不引入工作流引擎依赖。
2. **数值语义以 roboparty 部署参考实现为唯一权威冻结**
   （`sim2sim_rpo_parkour.py` 的 `crop_depth`/`preprocess_depth_frame_from_lowres`/
   `sample_depth_history` 与 `robolab/utils/noise/noise_model.py` 的
   `crop_and_resize`/`gaussian_blur_noise`/`depth_normalization`、
   `delayed_visualizable_image` 抽样语义），公式见设计文档；组合顺序
   冻结为"填充 → 降采样到 raw 网格 → 裁切 → 高斯模糊 → 裁切归一化"。
   与参考实现的唯一显式偏差：无效填充先于降采样（参考实现的 raw 已在
   64×36 网格上无需降采样；真实相机 848×480 → 64×36 时若先降采样，
   无效 0 会拉低邻域均值，违背"无效 = 无信息 = 远距"语义，故冻结
   填充在前）。
3. **公开 API 形态**：`DepthFrameF32`（float32 单通道米制深度帧，共享
   不可变缓冲、16 MiB 字节预算、stride 语义，镜像 `ImageU8` 纪律）+
   五个可独立调用的算子函数 + 一个冻结配置组合函数
   `preprocessPolicyDepthFrame` + 有界时序历史 `PolicyDepthHistory`
   （单写者约定，不加内部锁；跨上下文使用由上层经 `executor::comm`
   交付语义保证）。失败一律抛 `std::invalid_argument` 显式报告，不静默。
4. **零新增第三方依赖**：算子自研（double 中间量），高斯核公式与 cv2
   `getGaussianKernel`/M4-04 `gaussian_blur` 节点同式，面积降采样与
   cv2 `INTER_AREA`（64F 数学定义）同式；测试 oracle 用 numpy 双精度
   独立实现冻结公式（测试期工具，不入项目依赖）。

## 备选方案

- **扩展工作流节点目录（`PortType::Depth32` + 六节点）**：类型系统、
  UI、校验、状态节点执行模型四处契约级改动，成本与本次目标不成比例；
  且 8 算子链是固定顺序的部署管线，无画布编排价值。否决，留作后续
  立项候选。
- **引入 OpenCV 承载 resize/blur**：DEC-012 已以四维取证否决过 OpenCV
  依赖路线；两算子数学明确，自研 + golden 可控。否决。
- **把算子放进 adapter（`rin_realsense_adapter`）**：违反依赖方向
  （Adapter 依赖 Core 接口，算子是与设备无关的纯几何/数值处理）。
  否决。

## 影响与风险

- `rin_core` 新增公开头与实现（加性变更，既有契约不变）。
- 数值一致性风险由冻结公式 + golden 对拍测试承接（测试由
  Independent-Verification-Agent 执行）；真机端到端一致性还依赖 adapter
  米制深度通道与外参安装，均不在本决策范围，已在设计文档"边界"列明。
- `PolicyDepthHistory` 为有界内存（37×18×32×4 B ≈ 86 KB，配置校验上限
  4096 帧历史 × 16 MiB 单帧预算），无容量风险。

## 验证方式

- 逐算子 golden 手推用例 + 测试内 numpy 双精度独立参考交叉验证
  （37×23 随机图 + 848×480 → 64×36 全链），全预设 ctest 通过。
- 公开边界检查：公开头零第三方类型（既有 public_boundary 套件纪律）。

## 关联文档和工作项

- [M8 计划](../plans/m8-depth-policy-preproc.md)
- [depth_policy_preproc_design.md](../design/depth_policy_preproc_design.md)
  （冻结公式与参考实现对照）
- roboparty 侧参考：`robolab/scripts/mujoco/sim2sim_rpo_parkour.py`、
  `robolab/robolab/tasks/manager_based/parkour/parkour_env_cfg.py`
  （SceneCfg.camera 噪声管线与预处理顺序）、
  `robolab/robolab/utils/noise/noise_model.py`
