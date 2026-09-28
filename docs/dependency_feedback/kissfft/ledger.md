# kissfft 依赖台账

> 状态：Active
> 依赖：kissfft（pinned tag `131.2.0`，commit `7bce4153c6bc8aba2db0e889e576f9d00505cbe1`，
> https://github.com/mborgerding/kissfft ，BSD-3-Clause）
> 记录纪律：工程规范第 9.4 节（台账部分由 AGENTS.md"依赖独立台账"节扩展到所有依赖）。

## 依赖身份

- 来源：https://github.com/mborgerding/kissfft
- 锁定：`third_party/dependencies.lock` 行
  `kissfft|https://github.com/mborgerding/kissfft.git|7bce4153c6bc8aba2db0e889e576f9d00505cbe1|...|OFF|external`
  （2026-09-28 随 [DEC-012](../../decisions/DEC-012-image-operator-strategy.md) 引入，
  M4-01）
- 许可证：BSD-3-Clause（`LICENSES/BSD-3-Clause`；`COPYING` 同文）
- 类别：external —— FetchContent 按 pinned commit 源码构建；`KISSFFT_DATATYPE=float`、
  `KISSFFT_STATIC=ON`，关闭 TEST/TOOLS/PKGCONFIG（`cmake/Dependencies.cmake` 的
  cache FORCE 裁剪，方式与 librealsense2 相同——上游 `cmake_minimum_required(3.10)`
  触发 CMP0077 OLD）
- 用途：M4-06 FFT 滤波节点族（`fft_lowpass`/`fft_highpass`/`fft_bandpass`）的频域
  变换；链接目标 `kissfft::kissfft`。选型依据与替代方案对比见
  [DEC-012](../../decisions/DEC-012-image-operator-strategy.md) 与
  `tools/fft_bench/`（基准工具与复现命令）。
- 体积/构建实测（2026-09-28，x86_64 Linux，GCC 13.3，CMake 3.28.3）：源码目录
  1.5MB；float 静态库 32022 字节；全新 configure+build 1.42s（14 核本机）。

## 集成决策

- 锁定 `131.2.0`（2025-09-11）而非 master：master 在 2026-01-31 提交 `aea492e`
  为 `kiss_fftndr_alloc` 加入的溢出预检存在误报（见 KIS-20260928-001），该提交
  尚未进入任何发布 tag。若上游修复并发布新 tag，按工程规范 10.7 升级并回归。
- 仅用 C API（`kiss_fft.h`/`kiss_fftndr.h`）；不使用 `kissfft.hh` C++ 模板头
  （避免第二套 API 面）。
- 不使用 `KISSFFT_DATATYPE=simd`（SSE）变体：SIMD 路径按 4 路并行流处理
  （`kiss_fft_scalar = __m128`），面向多信号批处理；工作流节点为逐帧单图处理，
  批 4 帧会引入成帧延迟，plain float 已满足实时预算（DEC-012 基准）。
- FFT 节点族对非 2 幂尺寸在节点内部零填充到 2 幂（848×480 → 1024×512），参数面
  截止频率为归一化频率（`M4-09` 契约 [0,1]），填充不改变参数语义；节点级设计
  约束记入 DEC-012，随 `image_workflow_design.md`（M4-02 产出）细化。

## 条目

### KIS-20260928-001：master `kiss_fftndr_alloc` 溢出预检对 float 2D 配置误报

- 级别：P2（上游缺陷，锁旧版规避；升级前必须确认修复）
- 现象/证据：master（`e5e3fac46e0d94a8f8170c06706b7a4218828333`，含 2026-01-31
  提交 `aea492e`）上，`kiss_fftndr_alloc(dims={512,512} 或 {480,848}, 2, *, …)` 恒
  返回 `(size_t)-1`/NULL。`kiss_fftndr.c` 溢出预检将 `memneeded` 逐维 int 除法
  （`check /= dims[k]`），`ntmp` 缓冲约为每像素 4.008 字节（`dimOther*(dimReal+2)`
  × `sizeof(float)`），截断后恰好 `check == sizeof(kiss_fft_scalar)`，命中
  `check <= sizeof(kiss_fft_scalar)` 误判为溢出。tag `131.2.0`（`7bce4153`）无此
  检查，同配置分配正常。复现：`tools/fft_bench/README.md` 环境下对 master 构建
  调 `kiss_fftndr_alloc` 查询长度即返回 `(size_t)-1`（本基准开发中实测）。
- 影响：float/double 2D 实数 N 维变换配置在 master 上不可用（查询路径与内部
  malloc 路径均返回 NULL）。
- 期望语义：溢出预检不应将合法配置判为溢出（应按字节数余量判断，或用
  size_t 全程运算）。
- 绕行方案：锁定无该检查的 `131.2.0`；锁定版本功能完整（基准三尺寸全部通过
  正确性校验）。绕行限于 lock 行 + 本条目，无代码侧 hack。
- 移除条件：上游发布含修复的 tag 后，按工程规范 10.7 评估升级（升级验证须含
  `tools/fft_bench` 正确性校验与 FFT 节点 golden 测试）。
- 上游反馈：未提交（评估中；如提交issue 后回填链接）。
