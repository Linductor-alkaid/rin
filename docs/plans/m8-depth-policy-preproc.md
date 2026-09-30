# M8：深度策略预处理算子——真机侧策略输入管线确定性复刻

> 状态：Complete
> 负责人：Linductor-alkaid（授权 Agent 按工程规范自治执行）
> 所属计划：[Rin 实施总计划](rin-implementation-plan.md)
> 前置：M4（算子纪律与 golden 方法论）
> 立项依据：用户需求（2026-09-30）——把 roboparty E3-Parkour 的深度预处理
> 管线在 RealSense 真机侧与仿真部署参考逐位对齐
> 建议发布点：v0.5.1（用户指定，与 M9/M10 合并发布）
> 更新日期：2026-09-30

## 目标

在 `rin_core` 补齐米制深度（float32，米）确定性预处理算子：无效填充、
面积降采样、裁切、高斯模糊、裁切归一化、组合函数与 37 帧时序环形历史
抽样，数值语义以 roboparty 部署参考实现为唯一权威冻结，使真机
RealSense 深度流可产出与仿真 `depth_encoder.onnx` 输入语义完全一致的
(8, 18, 32) float32 张量。

## 范围与非目标

范围：

- [DEC-018](../decisions/DEC-018-depth-policy-preproc-operators.md)：
  算子落 Core 纯函数层，工作流节点面不扩展；公式冻结于
  [depth_policy_preproc_design.md](../design/depth_policy_preproc_design.md)。
- `rin::DepthFrameF32` 米制深度帧类型（共享不可变缓冲、16 MiB 预算、
  stride 语义，镜像 `ImageU8` 纪律）。
- 五算子（O1 填充 / O2 面积降采样 / O3 裁切 / O4 高斯模糊 / O5 归一化）
  + O6 冻结配置组合函数 + O7 `PolicyDepthHistory` 时序抽样。
- golden/交叉验证测试矩阵（Independent-Verification-Agent）。

非目标：

- adapter 米制深度通道（`FrameKind` 扩展与 Z16→米制发布）：另行立项。
- 工作流 `Depth32` 端口类型、状态时序节点、ONNX 推理接入（DEC-018
  决策 1，后续立项候选）。
- 训练侧随机噪声管线复刻（训练专属增强，部署不复刻）。

## 工作项

- [x] `M8-01` 设计冻结：[DEC-018](../decisions/DEC-018-depth-policy-preproc-operators.md)
      与 [depth_policy_preproc_design.md](../design/depth_policy_preproc_design.md)
      （冻结公式 + 参考实现对照表）。完成判据：`Accepted` 且设计文档
      结构检查通过。
- [x] `M8-02` Core 算子实现（`include/rin/depth_preproc.hpp` +
      `src/core/depth_preproc.cpp`，接入 `rin_core`）。完成判据：四预设
      编译零新增警告；公开头零第三方类型。
- [x] `M8-03` golden/交叉验证测试矩阵（Independent-Verification-Agent）：
      逐算子手推 golden + 测试内双精度独立参考交叉（设计文档 §6）。
      完成判据：debug/asan/ubsan 三预设全绿，零消毒器诊断。
- [x] `M8-04` 文档收口：总计划状态、CHANGELOG、验证记录回写。

## 风险与阻塞

- 数值一致性：O2/O4 与 cv2 的位级一致性受浮点实现细节影响，判据取
  冻结公式双精度参考的 1e−6 绝对容差（公式本身与 cv2 数学定义同式），
  不承诺 cv2 位级复刻——已记录于设计文档。
- 真机端到端一致性依赖后续 adapter 米制通道与外参安装（非目标），
  验收边界限于 Core 算子层。

## 测试与退出条件

- 逐算子 golden 全分支（含拒绝路径）+ O6 全链 numpy 交叉 + O7 时序
  全分支；debug/asan/ubsan 三预设全绿。
- 公开边界检查（零第三方 include）通过。
- 文档同步矩阵（工程规范 §8）全项落盘。

## 验证记录

- 2026-09-30 `M8-01`：[DEC-018](../decisions/DEC-018-depth-policy-preproc-operators.md)
  冻结（`Accepted`），[depth_policy_preproc_design.md](../design/depth_policy_preproc_design.md)
  产出（冻结公式 + roboparty 参考实现逐式对照表）。
- 2026-09-30 `M8-02`：`rin/depth_preproc.hpp` + `src/core/depth_preproc.cpp`
  落地（`DepthFrameF32`、O1–O5 算子、O6 组合、O7 `PolicyDepthHistory`）；
  debug/asan/ubsan/release 四预设编译零新增警告，clang-format 干净。
- 2026-09-30 `M8-03`：独立验证两轮。第一轮 409 项检查暴露实现缺陷 3 处
  ——`make`/`wrap` 混淆元素/字节 stride（致命：全部算子产不出帧）、
  `valid()` framesNeeded 64 位乘法回绕、`append` 从 `row(0)` 扁平拷贝
  破坏 padding stride 帧——另确认测试侧期望缺陷 2 处（O1 将 0.5>0 误判
  为无效、O4 角点脉冲能量断言违背 reflect-101 语义）。主循环修复实现，
  测试侧由验证方修正期望（角点能量 = (边系数+中心系数)²，与 cv2
  BORDER_REFLECT_101 同语义）并新增内部脉冲单位能量守恒用例；
  `test_public_boundary` 补新头文件见证。第二轮复验 PASS：三预设全量
  ctest 27/27 全绿（`depth_preproc` 309 项检查、`public_boundary` 68 项
  检查、基线无回归），零消毒器诊断、零新增编译警告。
- 2026-09-30 `M8-04`：总计划状态、CHANGELOG（Unreleased 段）回写。
  后续衔接：adapter 米制深度通道（`FrameKind` 扩展）与 ONNX 推理接入
  按 DEC-018 备注另行立项。
