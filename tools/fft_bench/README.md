# fft_bench：M4-01（DEC-012）FFT 实现策略基准

一次性的决策取证工具（M4-01，[DEC-012](../../docs/decisions/DEC-012-image-operator-strategy.md)）：
对比三种 FFT 实现策略在 512×512 与 848×480 灰度图上的单线程 float32 正/逆 2D FFT
耗时，并做正确性交叉校验。不参与项目构建与测试（无 CMake 目标）。

## 对照方

| backend | 实现 | 说明 |
| --- | --- | --- |
| `opencv` | 系统 OpenCV `cv::dft` | 实输入 CCS 打包最短路径（引入 OpenCV external pin 的代表性能） |
| `kissfft` | pinned kissfft（`KISSFFT_DATATYPE=float`，`kiss_fftndr`） | 自研算子 + 小型 FFT 库策略的代表性能 |
| `self_radix2` | 本文件内置迭代 radix-2 复数 FFT（行+列变换） | 全自研策略的代表性能；非 2 幂尺寸零填充到 2 幂（848×480 → 1024×512） |

尺寸：`512x512`（2 幂基线）、`848x480`（D435if 默认流原生尺寸）、`1024x512`
（848×480 的零填充目标，量化"库只跑 2 幂 + 节点填充"策略的成本）。

## 计时口径

- `forward`：实图缓冲 → 各方原生频谱。`self` 含实→复嵌入与填充置零（算法固有）；
  OpenCV/kissfft 直读实图缓冲，无额外拷贝。输入缓冲计时外预填。
- `inverse`：频谱 → 实图，含归一化（OpenCV `DFT_SCALE`；kissfft/self 显式
  `1/(H·W)` 缩放遍历计入——滤波节点完成一次正逆往返所必需）。
- 频谱掩膜乘（M4-06 滤波节点本体）不计入，与 FFT 选型正交。
- 统计：warmup 10 次后连测 `iters` 次，报 min / p50 / mean / p99（毫秒）。
- 汇聚（`SINK` 行）：消费变换结果防止死代码消除。

## 正确性校验（任何 FAIL 退出码非 0）

- 三方各自 forward→inverse 往返最大绝对误差 < 1e-3（float 容差）。
- kissfft 半谱、`self`（512×512 无填充时）与 OpenCV `DFT_COMPLEX_OUTPUT` 全复数谱
  的幅值相对偏差（max|a−b|/max|a|）< 5e-3；848×480 下 self 因填充语义不同不参与
  谱对照。

## 构建与运行（2026-09-28 实测环境）

```sh
# kissfft（pinned 131.2.0 = 7bce4153c6bc8aba2db0e889e576f9d00505cbe1）
git clone https://github.com/mborgerding/kissfft.git && git -C kissfft checkout 7bce4153
cmake -S kissfft -B build-kissfft -DKISSFFT_DATATYPE=float -DKISSFFT_STATIC=ON \
      -DKISSFFT_TEST=OFF -DKISSFFT_TOOLS=OFF -DKISSFFT_PKGCONFIG=OFF \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build-kissfft

# 基准（系统 OpenCV 4.6.0 头与库）
g++ -std=c++20 -O2 -DNDEBUG -I kissfft -I /usr/include/opencv4 \
    fft_bench.cpp -o fft_bench \
    -L build-kissfft -lkissfft-float -lopencv_core

./fft_bench 200   # 每 op 迭代数
```

已知 pin 约束：kissfft master（≥ 2026-01-31，提交 `aea492e`）在 `kiss_fftndr_alloc`
新增的溢出预检对 float 2D 配置误报（`memneeded` 逐维 int 除法截断后恰好
`<= sizeof(kiss_fft_scalar)`，返回 `(size_t)-1`/NULL），故锁定 131.2.0；
详见 kissfft 依赖台账（`docs/dependency_feedback/kissfft/ledger.md`）。

## kissfftndr 维序注记

`kiss_fftndr` 的实数半谱维是 `dims[ndims-1]`（内存最快维，须偶数），不是头文件
注释所述的 `dims[0]`。本工具传 `dims={h, w}`：输入 h×w 行主序实图，输出
h×(w/2+1) 复数半谱。误传 `{w, h}` 会按 848×(241) 越界写 480×(425) 缓冲
（heap corruption，实测复现）。
