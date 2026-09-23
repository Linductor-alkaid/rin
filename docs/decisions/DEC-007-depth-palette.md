# DEC-007：深度图配色运行时可选（jet / 灰度黑白）

> 状态：Accepted（灰度极性"近白远黑"为暂定默认，随 DEC-003 视觉参数一同冻结）
> 日期：2026-09-23
> 负责人：Linductor-alkaid
> 冻结里程碑：M2
> 替代/被替代：无（扩展 DEC-003 的配色维度）

## 背景与问题

DEC-003 将深度伪彩固化为 adapter 内的 jet 映射，viewer 侧无选择权。实际使用中
jet 伪彩不利于观察深度连续层次，需要提供灰度黑白（黑白）输出选项，且要求运行中
可切换、不中断出流。

## 决策

- 公共契约新增 `rin::DepthColorScheme { Jet, Grayscale }` 与
  `ICameraService::requestDepthColorScheme`（Waiting/Opening/Streaming/Restreaming
  下有效，粘性跨插拔/设备切换保留；Idle/Failed/Stopping 拒绝）。
- 配色命令沿用 DEC-006 的 `LatestMailbox<ControlCommand>` 通道：worker 在 Waiting
  轮询与流循环命令检查点消费，**仅切换后续帧的转换 ramp，不重启 pipeline、不进入
  Restreaming**；切换经 `ServiceEventKind::Info`（`depth palette: jet|grayscale`）
  对观察者可见。
- Core 新增统一入口 `convertDepth16ToRgba8(..., scheme, dst)` 与灰度 ramp
  `grayscaleColor`；`convertDepth16ToRgba8Jet` 保留为 Jet 薄包装，DEC-003 契约
  与既有测试不变。灰度极性：近处白（t=0 → 255）、远处黑（t=1 → 0）；无效深度
  （raw==0）与 jet 一致输出不透明黑。
- viewer 控制行新增 "Palette" 下拉（Jet/Grayscale），复用自研 select；不依赖设备
  目录，Waiting 态即可预设，接入后按所选配色出流。

## 备选方案

- **改 `Frame` 契约传递原始 Z16、由 viewer 侧转换**：把配色策略上移到消费端，
  语义灵活，但破坏"邮箱传 RGBA8 成品帧"的既有契约（DEC-003），GPU 上传路径、
  帧校验与既有测试全部受牵连，且 viewer 需要知道 depthScale 与视觉区间——SDK
  语义泄漏到 UI 层。否决。
- **把配色打包进 `StreamRequest`，经 Restreaming 应用**：实现最省（复用分辨率
  切换路径），但配色是纯后处理属性，重启 pipeline 会造成画面闪烁与内参重发布，
  语义错误。否决。
- **切换时发布新事件类型（如 `ConfigChanged`）**：当前事件消费者只区分
  Started/Stopped/Failed 三类语义，Info + 稳定消息文本已足够观察；避免枚举膨胀。

## 影响与风险

- 转换开销与 jet 相当（同一循环、不同 ramp），不改变 DEC-003 的性能结论。
- `Frame` 契约不变；palette 状态为 worker 私有成员，无新增共享状态。
- 灰度极性（近白远黑）为视觉暂定值，若与用户预期相反，仅需翻转
  `grayscaleColor` 一处并同步测试/文档。

## 验证方式

- `tests/test_pixel_format.cpp`：灰度端点/线性语义/超界 clamp/R=G=B 性质、统一
  入口参数校验、raw==0 黑、depthScale 换算（200→255、3350→128、6500→0）、区间外
  截断、单调性、stride 填充、Jet 包装逐字节回归（274 → 700 checks）。
- `tests/test_realsense_hardware.cpp`（真机 D435if）：Idle 拒绝、流中切换不进入
  Restreaming、Info 消息精确匹配、切换后帧内容灰度性质（全帧 R==G==B）/切回后
  存在彩色像素、同值幂等、stop 后拒绝（144 → 176 checks）。
- 真机 GUI：Palette 下拉切换即时生效（截图留档 m1 验证记录）。

## 关联文档和工作项

[M1 验证记录](../plans/m1-viewer-foundation.md)、
[DEC-003](DEC-003-depth-colormap.md)、[DEC-006](DEC-006-hotplug-device-selection.md)、
[camera_service_design.md](../design/camera_service_design.md)
