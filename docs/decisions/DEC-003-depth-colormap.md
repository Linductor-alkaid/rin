# DEC-003：深度伪彩映射采用自研 jet 实现

> 状态：Accepted（jet 参数为暂定默认值，最迟 M2 冻结）
> 日期：2026-09-23
> 负责人：Linductor-alkaid
> 冻结里程碑：M2
> 替代/被替代：无

## 背景与问题

深度流为 Z16，需要映射为可视化 RGBA。librealsense 自带 `rs2::colorizer` 处理块，但其
输出依赖 librealsense 处理块体系，参数化与测试均绑定 SDK 类型，且其查找表构建在
SDK 内部线程上下文执行，行为不透明。

## 决策

在 adapter 内以纯函数实现 Z16→RGBA8 jet 伪彩：单文件、无 SDK 依赖、可离线单测
（含 0/无效深度处理、线性深度缩放），输入输出均为项目公共类型。默认深度缩放按设备
`depth_scale` 归一（映射 0–6.5 m 视觉区间，暂定默认值）。

## 备选方案

- `rs2::colorizer`：零实现成本，但不可单测、参数语义绑定 SDK、且在 worker 内引入
  额外处理块队列；观察期内不采用，若 M2 验收视觉质量不足再评估。

## 影响与风险

- CPU 转换在 worker 线程执行，848×480@30fps 约为 0.4 MB/帧的写入量，可接受；若升级
  高分辨率再评估 SIMD/LUT。
- 视觉风格与 RealSense 官方 viewer 不完全一致，属预期差异。

## 验证方式

`tests/test_depth_colormap.cpp`：单调性、端点色、0 深度透明/黑处理、stride 处理；
真机验收目视确认深度画面层次。

## 关联文档和工作项

`M1-04`、[librealsense 台账](../dependency_feedback/librealsense/ledger.md)
