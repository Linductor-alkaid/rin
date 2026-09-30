// M8 米制深度策略预处理算子 golden 数值测试（独立验证）：include/rin/depth_preproc.hpp、
// src/core/depth_preproc.cpp（数值语义以 docs/design/depth_policy_preproc_design.md
// §4 冻结配置约束与 §5 冻结算子公式为唯一判据；本文件的 golden 期望全部由冻结公式
// 独立手推/测试内 double 参考实现推导，不参考实现代码、不 include 实现源文件）。
//
// 被测面与范围（对应设计文档 §6 golden 项）：
// 0) DepthFrameF32 契约：默认构造无效；make 零值初始化/stride 语义/单帧字节预算
//    （kMaxDepthFrameBytes 恰等 16 MiB 可建、超限拒绝）；wrap 共享只读视图/缓冲
//    不足拒绝；row 越界 nullptr；byteSize。
// 1) PolicyDepthConfig（§4）：默认值逐字段冻结、policyWidth/policyHeight 派生、
//    valid() 全部约束反例（网格/裁切/深度区间/模糊半径 σ/历史/抽样，非法 config
//    构造 PolicyDepthHistory 一并拒绝）与边界正例（radius 1/10、sigma 0/10、
//    sampleDelay 恰好上界、historyLength 4096/4097、微型 historyLength=1）；
//    u64 乘法回绕对抗用例（数学上违反 framesNeeded ≤ historyLength 的极端
//    sampleCount 必须仍被判无效）。
// 2) O1 fillDepthInvalid：NaN/+Inf/−Inf/0/负值/正常值混合 golden；invalidBelow
//    非默认（含恰等边界 ≤ 与负阈值）；farValue 非有限拒绝；无效帧拒绝；输出
//    紧凑新缓冲（padding 输入不泄漏）。
// 3) O2 resizeDepthArea：2×2→1×1、3×2→2×1 分数覆盖权重手推 golden；8×8→2×2
//    整数倍 4× 恒比；同尺寸逐像素恒等（含 padding 输入、恒等仍为新缓冲）；
//    848×480→64×36、848×480→848×240、5×4→3×2、7×5→3×2 与测试内独立 double
//    参考交叉（|diff| ≤ 1e−6 绝对）；放大（含单维放大）与零尺寸目标拒绝；
//    无效帧拒绝。
// 4) O3 cropDepth：64×36 (18,0,16,16) → 18×32 索引逐位（含 stride padding 输入、
//    padding 不泄漏）；小图 golden；退化（up+down == H、left+right == W）与越界
//    （>）拒绝全分支；贴边合法边界；无效帧拒绝。
// 5) O4 gaussianBlurDepth：核系数 golden（radius=1, σ=1 → [0.274068619,
//    0.451862761, 0.274068619]，由冻结公式独立数值求值后再对字面 golden 互证，
//    经 1×3 脉冲响应读出）；σ=0 δ 核恒等（K > 图幅深度折返仍恒等）；常值图
//    恒等（质量守恒）；5×5 角点脉冲 reflect-101 折返手推 golden（能量
//    (a+b)²：边缘不复制边缘像素，无单位守恒）+ 7×7 内部脉冲单位能量守恒
//    （(2a+b)² = 1）；1×6 窄图手推（线性渐变内部恒等、边界按折返）；13×9 伪随机
//    图 radius∈{1,3,10}×σ∈{0.5,1.5,10} 与测试内直接 2D double 参考及独立可分离
//    参考交叉（≤1e−6）；padding 输入与紧凑输入一致；radius/sigma 越界与无效帧
//    拒绝。
// 6) O5 clipNormalizeDepth：双侧 clip、区间内恒比、非零 near；near ≥ far 拒绝；
//    无效帧拒绝。
// 7) O6 preprocessPolicyDepthFrame：合成 848×480（正常/NaN/+Inf/0/负值混合）
//    全链 vs 测试内按冻结顺序（填充 → 面积降采样 → 裁切 → 模糊 → 归一化）
//    独立实现的 double 参考逐像素 ≤1e−6（输入含无效像素，填充先于降采样的
//    冻结次序被交叉用例隐式锁定）；输出恰 18×32、紧凑、值域 [0,1]；config
//    无效/输入无效拒绝。
// 8) O7 PolicyDepthHistory：空历史全 0；append 1/5/36 帧欠帧首帧填充逐位
//    golden；满环（37 帧）与滚动（40 帧）抽样下标 {1,6,…,36}；CHW
//    oldest→newest 排布（帧内容含平面内梯度，可判转置/错位混淆）；
//    sampleDelay=1 下标 {0,5,…,35}；非默认小配置（6×5 帧、history 10、
//    sample 3 skip 3）；historyLength=1 微型环滚动；reset 幂等；尺寸不符/
//    无效帧抛；padding stride 帧按逻辑内容入环（对抗用例）；非法 config
//    构造抛（见 1）。
//
// golden 独立性说明：小图 golden 以字面数值写出，附手推过程注释（只依赖 §5
// 冻结公式；模糊核与脉冲响应数值由冻结公式的独立数值求值得出）；大图用例由
// 测试内自写的 refAreaResize/refBlurSeparable/refBlurDirect2D/refFill/refCrop/
// refClipNormalize double 参考交叉验证（按冻结公式重写，不 include 实现头），
// 参考实现自身先经小图手推 golden 自检（自检先行）。
//
// 防御性说明：算子契约只在"输入无效/参数越界"时抛 std::invalid_argument；为使
// 实现缺陷（如意外抛出其他异常类型、构造出无效帧）不致套件中途 abort、失败清单
// 完整可回报，所有期望成功的算子调用经 tryOp/tryCall 包裹（意外异常记失败并继续），
// 且所有对无效帧的原生解引用均有 valid() 守卫。
//
// DOD-02 适用性说明：本契约面全部为单线程纯逻辑（算子纯函数 + 单写者
// PolicyDepthHistory 的顺序 append/sample，无任务提交/队列/取消/超时/shutdown
// 语义，无跨上下文共享状态），并发矩阵不适用（写法参照
// test_image_ops_geometry.cpp 文件头）。
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <rin/depth_preproc.hpp>

namespace {

using rin::DepthFrameF32;
using rin::PolicyDepthConfig;
using rin::PolicyDepthHistory;

// radius=1、σ=1 归一化核的冻结字面 golden（设计文档 §6；与 M4-04 gaussian_blur
// 同式同值）：k[0] = k[2] = exp(−0.5)/(1+2·exp(−0.5))，k[1] = 1/(1+2·exp(−0.5))。
constexpr double kGoldenA = 0.274068619;
constexpr double kGoldenB = 0.451862761;

// --- 帧构造与核对助手 --------------------------------------------------------

using PixelFn = std::function<float(std::uint32_t, std::uint32_t)>;

// 构造紧凑帧（stride = width），内容由 pixel(x, y) 给出。make/wrap 契约为
// "失败返回无效帧不抛异常"，因此本助手不抛；构造失败由下游 valid() 守卫记失败。
DepthFrameF32 makeDepth(std::uint32_t width, std::uint32_t height, const PixelFn& pixel) {
    std::vector<float> buffer(std::size_t{width} * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            buffer[std::size_t{y} * width + x] = pixel(x, y);
        }
    }
    auto shared = std::make_shared<const std::vector<float>>(std::move(buffer));
    return DepthFrameF32::wrap(width, height, width, std::move(shared));
}

// 构造行尾 padding 帧；逻辑内容由 pixel 给出，padding 区填 padValue（用于断言
// padding 字节不泄漏进输出）。
DepthFrameF32 makeDepthPadded(std::uint32_t width, std::uint32_t height, std::uint32_t stride,
                              const PixelFn& pixel, float padValue) {
    std::vector<float> buffer(std::size_t{stride} * height, padValue);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            buffer[std::size_t{stride} * y + x] = pixel(x, y);
        }
    }
    auto shared = std::make_shared<const std::vector<float>>(std::move(buffer));
    return DepthFrameF32::wrap(width, height, stride, std::move(shared));
}

// 输出必须是有效、紧凑（stride == 宽）且尺寸匹配的帧。
bool compactValid(const DepthFrameF32& frame, std::uint32_t width, std::uint32_t height) {
    return frame.valid() && frame.width() == width && frame.height() == height &&
           frame.stride() == width && frame.pixels() != nullptr &&
           frame.pixels()->size() == std::size_t{width} * height;
}

// 逐像素位级相等（期望为 float 字面/生成值；期望不含 NaN）。无效帧安全返回 false。
bool frameExact(const DepthFrameF32& frame, const std::vector<float>& expected) {
    if (!compactValid(frame, frame.width(), frame.height()) ||
        expected.size() != frame.pixels()->size()) {
        return false;
    }
    return std::equal(expected.begin(), expected.end(), frame.pixels()->begin());
}

// 逐像素 |out − ref| 最大值（ref 为 double 参考；结构不符返回 +inf）。
double frameMaxDiff(const DepthFrameF32& frame, const std::vector<double>& reference) {
    if (!compactValid(frame, frame.width(), frame.height()) ||
        reference.size() != frame.pixels()->size()) {
        return std::numeric_limits<double>::infinity();
    }
    double maxDiff = 0.0;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        maxDiff =
            std::max(maxDiff, std::abs(static_cast<double>(frame.pixels()->at(i)) - reference[i]));
    }
    return maxDiff;
}

// 逻辑像素 → double 向量（经 row() 取逻辑内容，跳过 padding 区；无效帧返回空）。
std::vector<double> frameToDoubles(const DepthFrameF32& frame) {
    if (!frame.valid()) {
        return {};
    }
    std::vector<double> out(std::size_t{frame.width()} * frame.height());
    for (std::uint32_t y = 0; y < frame.height(); ++y) {
        const float* row = frame.row(y);
        for (std::uint32_t x = 0; x < frame.width(); ++x) {
            out[std::size_t{y} * frame.width() + x] = static_cast<double>(row[x]);
        }
    }
    return out;
}

// 逻辑像素 → float 向量（经 row() 取逻辑内容；无效帧返回空）。
std::vector<float> frameLogical(const DepthFrameF32& frame) {
    if (!frame.valid()) {
        return {};
    }
    std::vector<float> out(std::size_t{frame.width()} * frame.height());
    for (std::uint32_t y = 0; y < frame.height(); ++y) {
        const float* row = frame.row(y);
        std::copy_n(row, frame.width(), out.begin() + std::ptrdiff_t{y} * frame.width());
    }
    return out;
}

bool vectorExact(const std::vector<float>& got, const std::vector<float>& expected) {
    return got.size() == expected.size() &&
           std::equal(expected.begin(), expected.end(), got.begin());
}

template <typename Exception, typename Fn>
bool throwsAs(Fn&& fn) {
    try {
        fn();
    } catch (const Exception&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

// 期望成功的算子调用防护：契约外异常（实现缺陷路径）记失败并返回 fallback，
// 保证套件可跑完、失败清单完整。
template <typename T, typename Fn>
T tryOp(const char* what, Fn&& fn, T fallback) {
    try {
        return fn();
    } catch (const std::exception& e) {
        RIN_CHECK_MSG(false, std::string(what) + " 意外抛出: " + e.what());
        return std::move(fallback);
    } catch (...) {
        RIN_CHECK_MSG(false, std::string(what) + " 意外抛出（非 std::exception）");
        return std::move(fallback);
    }
}

// void 版防护（append 等无返回值调用）。
template <typename Fn>
void tryCall(const char* what, Fn&& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        RIN_CHECK_MSG(false, std::string(what) + " 意外抛出: " + e.what());
    } catch (...) {
        RIN_CHECK_MSG(false, std::string(what) + " 意外抛出（非 std::exception）");
    }
}

// 确定性伪随机（LCG，[0,1)；24 位有效尾数）。
double lcg01(std::uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return static_cast<double>(state >> 8) / 16777216.0;
}

// --- 独立 double 参考实现（按设计文档 §5 冻结公式重写，不 include 实现头）----

// reflect-101 无限折返（−1 → 1、n → n−2；尺寸 1 的维度下标恒 0）。
std::int64_t refReflect101(std::int64_t index, std::int64_t size) {
    if (size == 1) {
        return 0;
    }
    while (index < 0 || index >= size) {
        if (index < 0) {
            index = -index;
        }
        if (index >= size) {
            index = 2 * (size - 1) - index;
        }
    }
    return index;
}

// O4 一维归一化高斯核（σ=0 → δ 核；k[i] = exp(−(i−r)²/(2σ²)) 归一化 Σ=1）。
std::vector<double> refGaussianKernel(std::uint32_t radius, double sigma) {
    const std::size_t kSize = std::size_t{2} * radius + 1;
    std::vector<double> kernel(kSize, 0.0);
    if (sigma == 0.0) {
        kernel[radius] = 1.0;
        return kernel;
    }
    double sum = 0.0;
    for (std::size_t i = 0; i < kSize; ++i) {
        const double d = static_cast<double>(i) - static_cast<double>(radius);
        kernel[i] = std::exp(-(d * d) / (2.0 * sigma * sigma));
        sum += kernel[i];
    }
    for (double& value : kernel) {
        value /= sum;
    }
    return kernel;
}

// O2 面积加权箱式降采样：out(x,y) = Σᵢⱼ wᵢ·hⱼ·src / (sx·sy)，源列 i 的水平覆盖
// 区间 [i, i+1) 与 [x·sx, (x+1)·sx) 之交；单层扁平累加（独立求和次序）。
std::vector<double> refAreaResize(const std::vector<double>& src, std::uint32_t srcW,
                                  std::uint32_t srcH, std::uint32_t dstW, std::uint32_t dstH) {
    if (src.empty()) {
        return {};
    }
    const double sx = static_cast<double>(srcW) / static_cast<double>(dstW);
    const double sy = static_cast<double>(srcH) / static_cast<double>(dstH);
    std::vector<double> out(std::size_t{dstW} * dstH);
    for (std::uint32_t dy = 0; dy < dstH; ++dy) {
        const double y0 = static_cast<double>(dy) * sy;
        const double y1 = static_cast<double>(dy + 1) * sy;
        for (std::uint32_t dx = 0; dx < dstW; ++dx) {
            const double x0 = static_cast<double>(dx) * sx;
            const double x1 = static_cast<double>(dx + 1) * sx;
            double sum = 0.0;
            for (std::uint32_t j = static_cast<std::uint32_t>(std::floor(y0));
                 j < std::min<std::uint32_t>(static_cast<std::uint32_t>(std::ceil(y1)) + 1u, srcH);
                 ++j) {
                const double h = std::min(y1, static_cast<double>(j) + 1.0) -
                                 std::max(y0, static_cast<double>(j));
                if (h <= 0.0) {
                    continue;
                }
                for (std::uint32_t i = static_cast<std::uint32_t>(std::floor(x0));
                     i <
                     std::min<std::uint32_t>(static_cast<std::uint32_t>(std::ceil(x1)) + 1u, srcW);
                     ++i) {
                    const double w = std::min(x1, static_cast<double>(i) + 1.0) -
                                     std::max(x0, static_cast<double>(i));
                    if (w <= 0.0) {
                        continue;
                    }
                    sum += w * h * src[std::size_t{j} * srcW + i];
                }
            }
            out[std::size_t{dy} * dstW + dx] = sum / (sx * sy);
        }
    }
    return out;
}

// O4 可分离两趟模糊（水平 → 垂直，reflect-101，double 全程不量化）。
std::vector<double> refBlurSeparable(const std::vector<double>& src, std::uint32_t width,
                                     std::uint32_t height, std::uint32_t radius, double sigma) {
    if (src.empty()) {
        return {};
    }
    const std::vector<double> k = refGaussianKernel(radius, sigma);
    const auto ki = static_cast<std::int64_t>(radius);
    const std::int64_t wi = static_cast<std::int64_t>(width);
    const std::int64_t hi = static_cast<std::int64_t>(height);
    std::vector<double> tmp(std::size_t{width} * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            double acc = 0.0;
            for (std::size_t t = 0; t < k.size(); ++t) {
                const std::int64_t sx = refReflect101(
                    static_cast<std::int64_t>(x) + static_cast<std::int64_t>(t) - ki, wi);
                acc += k[t] * src[std::size_t{y} * width + static_cast<std::uint32_t>(sx)];
            }
            tmp[std::size_t{y} * width + x] = acc;
        }
    }
    std::vector<double> out(tmp.size());
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            double acc = 0.0;
            for (std::size_t t = 0; t < k.size(); ++t) {
                const std::int64_t sy = refReflect101(
                    static_cast<std::int64_t>(y) + static_cast<std::int64_t>(t) - ki, hi);
                acc += k[t] * tmp[static_cast<std::size_t>(sy) * width + x];
            }
            out[std::size_t{y} * width + x] = acc;
        }
    }
    return out;
}

// O4 直接 2D 卷积参考（二维核 = 一维核外积 k[i]·k[j]，reflect-101；与可分离
// 路线独立，用于互相印证）。
std::vector<double> refBlurDirect2D(const std::vector<double>& src, std::uint32_t width,
                                    std::uint32_t height, std::uint32_t radius, double sigma) {
    if (src.empty()) {
        return {};
    }
    const std::vector<double> k = refGaussianKernel(radius, sigma);
    const auto ki = static_cast<std::int64_t>(radius);
    const std::int64_t wi = static_cast<std::int64_t>(width);
    const std::int64_t hi = static_cast<std::int64_t>(height);
    std::vector<double> out(std::size_t{width} * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            double acc = 0.0;
            for (std::size_t i = 0; i < k.size(); ++i) {
                const std::size_t sy = static_cast<std::size_t>(refReflect101(
                    static_cast<std::int64_t>(y) + static_cast<std::int64_t>(i) - ki, hi));
                for (std::size_t j = 0; j < k.size(); ++j) {
                    const std::size_t sx = static_cast<std::size_t>(refReflect101(
                        static_cast<std::int64_t>(x) + static_cast<std::int64_t>(j) - ki, wi));
                    acc += k[i] * k[j] * src[sy * width + sx];
                }
            }
            out[std::size_t{y} * width + x] = acc;
        }
    }
    return out;
}

// O1 填充参考：!isfinite(v) 或 v ≤ invalidBelow → farValue。
std::vector<double> refFill(const std::vector<double>& src, double farValue, double invalidBelow) {
    std::vector<double> out(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) {
        const double v = src[i];
        out[i] = (!std::isfinite(v) || v <= invalidBelow) ? farValue : v;
    }
    return out;
}

// O3 裁切参考：行域 [up, H−down)、列域 [left, W−right)。
std::vector<double> refCrop(const std::vector<double>& src, std::uint32_t width,
                            std::uint32_t height, std::uint32_t up, std::uint32_t down,
                            std::uint32_t left, std::uint32_t right) {
    if (src.empty()) {
        return {};
    }
    const std::uint32_t outW = width - left - right;
    const std::uint32_t outH = height - up - down;
    std::vector<double> out(std::size_t{outW} * outH);
    for (std::uint32_t y = 0; y < outH; ++y) {
        for (std::uint32_t x = 0; x < outW; ++x) {
            out[std::size_t{y} * outW + x] = src[std::size_t{y + up} * width + (x + left)];
        }
    }
    return out;
}

// O5 裁切归一化参考：(clamp(v, near, far) − near) / (far − near)。
std::vector<double> refClipNormalize(const std::vector<double>& src, double near, double far) {
    std::vector<double> out(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) {
        const double v = std::min(std::max(src[i], near), far);
        out[i] = (v - near) / (far - near);
    }
    return out;
}

// --- O7 时序历史帧内容（逐帧唯一 + 平面内梯度，可判平面错位/转置混淆）--------
// frame k 的像素 (x, y) = 1000·k + w·y + x + 1（k ≤ 40 时 < 2^24，float32 精确）。

float histPixel(std::uint32_t k, std::uint32_t x, std::uint32_t y, std::uint32_t width) {
    return static_cast<float>(1000.0 * static_cast<double>(k) + static_cast<double>(width * y + x) +
                              1.0);
}

std::vector<float> histPlane(std::uint32_t k, std::uint32_t width, std::uint32_t height) {
    std::vector<float> plane(std::size_t{width} * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            plane[std::size_t{y} * width + x] = histPixel(k, x, y, width);
        }
    }
    return plane;
}

// 期望 sample 向量：按 oldest → newest 的帧序号列表拼接 CHW 平面。
std::vector<float> histSampleExpected(const std::vector<std::uint32_t>& frameIndices,
                                      std::uint32_t width, std::uint32_t height) {
    std::vector<float> expected;
    expected.reserve(frameIndices.size() * std::size_t{width} * height);
    for (const std::uint32_t index : frameIndices) {
        const std::vector<float> plane = histPlane(index, width, height);
        expected.insert(expected.end(), plane.begin(), plane.end());
    }
    return expected;
}

}  // namespace

int main() {
    // ===================================================================
    // 0) DepthFrameF32 契约（设计文档 §3）
    // ===================================================================

    // --- 0a) 默认构造无效 ---
    {
        const DepthFrameF32 empty;
        RIN_CHECK(!empty.valid());
        RIN_CHECK_EQ(empty.width(), std::uint32_t{0});
        RIN_CHECK_EQ(empty.height(), std::uint32_t{0});
        RIN_CHECK_EQ(empty.stride(), std::uint32_t{0});
        RIN_CHECK_EQ(empty.byteSize(), std::uint64_t{0});
        RIN_CHECK(empty.pixels() == nullptr);
        RIN_CHECK(empty.row(0) == nullptr);
    }

    // --- 0b) make：零值初始化、stride 语义、字节预算（stride 为元素数，≥ width）---
    {
        const DepthFrameF32 frame = DepthFrameF32::make(4, 3);
        RIN_CHECK(frame.valid());
        RIN_CHECK_EQ(frame.width(), std::uint32_t{4});
        RIN_CHECK_EQ(frame.height(), std::uint32_t{3});
        RIN_CHECK_EQ(frame.stride(), std::uint32_t{4});  // stride=0 → width
        RIN_CHECK_EQ(frame.byteSize(), std::uint64_t{48});
        bool allZero = true;
        if (frame.valid()) {
            for (const float v : *frame.pixels()) {
                allZero = allZero && v == 0.0f;
            }
        }
        RIN_CHECK(allZero);  // 值初始化为零
    }
    {
        const DepthFrameF32 padded = DepthFrameF32::make(4, 2, 6);
        RIN_CHECK(padded.valid());
        RIN_CHECK_EQ(padded.stride(), std::uint32_t{6});
        RIN_CHECK_EQ(padded.byteSize(), std::uint64_t{48});  // stride×height×4
        RIN_CHECK(padded.row(0) != nullptr && padded.row(1) != nullptr);
        RIN_CHECK_EQ(padded.row(1) - padded.row(0), std::ptrdiff_t{6});
        RIN_CHECK(padded.row(2) == nullptr);  // y ≥ height → nullptr
    }
    RIN_CHECK(!DepthFrameF32::make(0, 3).valid());     // 零宽
    RIN_CHECK(!DepthFrameF32::make(3, 0).valid());     // 零高
    RIN_CHECK(!DepthFrameF32::make(4, 3, 3).valid());  // stride < width
    {
        // 2048×2048 F32 = 16 MiB 恰等预算 → 可建；各超 1 行/1 列 → 拒绝。
        const DepthFrameF32 exact = DepthFrameF32::make(2048, 2048);
        RIN_CHECK(exact.valid());
        RIN_CHECK_EQ(exact.byteSize(), std::uint64_t{16} * 1024 * 1024);
        RIN_CHECK_EQ(exact.byteSize(), rin::kMaxDepthFrameBytes);
    }
    RIN_CHECK(!DepthFrameF32::make(2048, 2049).valid());  // +1 行 → 超 16 MiB
    RIN_CHECK(!DepthFrameF32::make(2049, 2048).valid());  // +1 列 stride → 超 16 MiB

    // --- 0c) wrap：共享只读视图、缓冲不足拒绝（紧凑 wrap stride=width 必须有效）---
    RIN_CHECK(!DepthFrameF32::wrap(4, 2, 4, nullptr).valid());
    {
        const auto emptyBuf = std::make_shared<const std::vector<float>>();
        RIN_CHECK(!DepthFrameF32::wrap(1, 1, 1, emptyBuf).valid());
    }
    {
        const auto shortBuf = std::make_shared<const std::vector<float>>(7, 1.0f);  // 需 8
        RIN_CHECK(!DepthFrameF32::wrap(4, 2, 4, shortBuf).valid());
    }
    {
        const auto shortStride = std::make_shared<const std::vector<float>>(11, 1.0f);  // 需 12
        RIN_CHECK(!DepthFrameF32::wrap(4, 2, 6, shortStride).valid());
    }
    {
        const auto buf = std::make_shared<const std::vector<float>>(12, 2.5f);  // 6×2 恰等
        const DepthFrameF32 frame = DepthFrameF32::wrap(4, 2, 6, buf);
        RIN_CHECK(frame.valid());
        RIN_CHECK_EQ(buf.use_count(), 2);  // 共享（测试 + 帧）
        RIN_CHECK(frame.pixels().get() == buf.get());
        const DepthFrameF32 copy = frame;  // 值语义共享同一缓冲
        RIN_CHECK_EQ(buf.use_count(), 3);
        RIN_CHECK(copy.pixels().get() == buf.get());
        // 只读视图类型面：pixels() 即 shared_ptr<const vector<float>>。
        const std::shared_ptr<const std::vector<float>> view = frame.pixels();
        RIN_CHECK(view == buf);
    }
    {
        const auto bigBuf = std::make_shared<const std::vector<float>>(16, 1.0f);  // > 12
        RIN_CHECK(DepthFrameF32::wrap(4, 2, 6, bigBuf).valid());  // ≥ stride×height 即可
    }
    RIN_CHECK(
        !DepthFrameF32::wrap(0, 2, 0, std::make_shared<const std::vector<float>>(4, 0.0f)).valid());

    // ===================================================================
    // 参考实现自检（先于交叉验证；手推 golden 见各节）
    // ===================================================================
    {
        // refAreaResize：2×2 [1,2;3,4] → 1×1 = 2.5（等权均值）。
        const std::vector<double> tiny = refAreaResize({1, 2, 3, 4}, 2, 2, 1, 1);
        RIN_CHECK(tiny.size() == 1 && std::abs(tiny[0] - 2.5) <= 1e-12);
        // refGaussianKernel(1, 1.0) 对冻结字面 golden（1e−9）。
        const std::vector<double> k = refGaussianKernel(1, 1.0);
        RIN_CHECK_EQ(k.size(), std::size_t{3});
        RIN_CHECK(std::abs(k[0] - kGoldenA) <= 1e-9 && std::abs(k[2] - kGoldenA) <= 1e-9);
        RIN_CHECK(std::abs(k[1] - kGoldenB) <= 1e-9);
        RIN_CHECK(std::abs(k[0] + k[1] + k[2] - 1.0) <= 1e-12);
        // refReflect101 手推：−1→1、n→n−2、周期折返、n=1 恒 0。
        RIN_CHECK(refReflect101(-1, 3) == 1 && refReflect101(3, 3) == 1);
        RIN_CHECK(refReflect101(5, 3) == 1 && refReflect101(4, 3) == 0);
        RIN_CHECK(refReflect101(-3, 3) == 1 && refReflect101(-4, 3) == 0);
        RIN_CHECK(refReflect101(47, 13) == 1 && refReflect101(-1, 1) == 0);
    }

    // ===================================================================
    // 1) PolicyDepthConfig（设计文档 §4）
    // ===================================================================

    // --- 1a) 默认值逐字段冻结 + 派生量 ---
    {
        const PolicyDepthConfig cfg;
        RIN_CHECK_EQ(cfg.gridWidth, std::uint32_t{64});
        RIN_CHECK_EQ(cfg.gridHeight, std::uint32_t{36});
        RIN_CHECK_EQ(cfg.cropUp, std::uint32_t{18});
        RIN_CHECK_EQ(cfg.cropDown, std::uint32_t{0});
        RIN_CHECK_EQ(cfg.cropLeft, std::uint32_t{16});
        RIN_CHECK_EQ(cfg.cropRight, std::uint32_t{16});
        RIN_CHECK_EQ(cfg.blurRadius, std::uint32_t{1});
        RIN_CHECK(cfg.blurSigma == 1.0);
        RIN_CHECK(cfg.depthNear == 0.0);
        RIN_CHECK(cfg.depthFar == 2.5);
        RIN_CHECK_EQ(cfg.historyLength, std::size_t{37});
        RIN_CHECK_EQ(cfg.sampleCount, std::size_t{8});
        RIN_CHECK_EQ(cfg.sampleSkip, std::size_t{5});
        RIN_CHECK_EQ(cfg.sampleDelay, std::size_t{0});
        RIN_CHECK(cfg.valid());
        RIN_CHECK_EQ(cfg.policyWidth(), std::uint32_t{32});
        RIN_CHECK_EQ(cfg.policyHeight(), std::uint32_t{18});
    }
    {
        // 非默认派生：grid 8×8、crop (2,1,1,1) → 6×5。
        PolicyDepthConfig cfg;
        cfg.gridWidth = 8;
        cfg.gridHeight = 8;
        cfg.cropUp = 2;
        cfg.cropDown = 1;
        cfg.cropLeft = 1;
        cfg.cropRight = 1;
        RIN_CHECK(cfg.valid());
        RIN_CHECK_EQ(cfg.policyWidth(), std::uint32_t{6});
        RIN_CHECK_EQ(cfg.policyHeight(), std::uint32_t{5});
    }

    // --- 1b) valid() 反例全分支 + 非法 config 构造 PolicyDepthHistory 抛 ---
    {
        std::vector<std::function<void(PolicyDepthConfig&)>> invalidMutations;
        invalidMutations.push_back([](PolicyDepthConfig& c) { c.gridWidth = 0; });
        invalidMutations.push_back([](PolicyDepthConfig& c) { c.gridHeight = 0; });
        invalidMutations.push_back([](PolicyDepthConfig& c) {
            c.cropUp = 18;
            c.cropDown = 18;  // == gridHeight → policyHeight 退化
        });
        invalidMutations.push_back([](PolicyDepthConfig& c) { c.cropUp = 37; });  // > gridHeight
        invalidMutations.push_back([](PolicyDepthConfig& c) {
            c.cropLeft = 16;
            c.cropRight = 48;  // == gridWidth → policyWidth 退化
        });
        invalidMutations.push_back([](PolicyDepthConfig& c) {
            c.cropLeft = 17;
            c.cropRight = 48;
        });  // > gridWidth
        invalidMutations.push_back([](PolicyDepthConfig& c) {
            c.depthNear = 0.0;
            c.depthFar = 0.0;
        });  // 相等
        invalidMutations.push_back([](PolicyDepthConfig& c) {
            c.depthNear = 2.5;
            c.depthFar = 0.0;
        });  // 倒置
        invalidMutations.push_back(
            [](PolicyDepthConfig& c) { c.depthNear = std::numeric_limits<double>::quiet_NaN(); });
        invalidMutations.push_back(
            [](PolicyDepthConfig& c) { c.depthFar = std::numeric_limits<double>::infinity(); });
        invalidMutations.push_back([](PolicyDepthConfig& c) { c.blurRadius = 0; });
        invalidMutations.push_back([](PolicyDepthConfig& c) { c.blurRadius = 11; });
        invalidMutations.push_back([](PolicyDepthConfig& c) { c.blurSigma = -0.1; });
        invalidMutations.push_back([](PolicyDepthConfig& c) { c.blurSigma = 10.5; });
        invalidMutations.push_back(
            [](PolicyDepthConfig& c) { c.blurSigma = std::numeric_limits<double>::quiet_NaN(); });
        invalidMutations.push_back(
            [](PolicyDepthConfig& c) { c.blurSigma = std::numeric_limits<double>::infinity(); });
        invalidMutations.push_back([](PolicyDepthConfig& c) { c.historyLength = 0; });
        invalidMutations.push_back([](PolicyDepthConfig& c) { c.historyLength = 4097; });
        invalidMutations.push_back([](PolicyDepthConfig& c) { c.sampleCount = 0; });
        invalidMutations.push_back([](PolicyDepthConfig& c) { c.sampleSkip = 0; });
        // framesNeeded = (8−1)·5+1+delay = 36+delay > 37：delay = 2 超上界 1 帧。
        invalidMutations.push_back([](PolicyDepthConfig& c) { c.sampleDelay = 2; });
        invalidMutations.push_back([](PolicyDepthConfig& c) { c.sampleSkip = 6; });   // 43 > 37
        invalidMutations.push_back([](PolicyDepthConfig& c) { c.sampleCount = 9; });  // 41 > 37
        // u64 乘法回绕对抗：(2^63)·2 = 2^64 回绕为 0 → 数学上 framesNeeded = 2^64+1
        // ≫ 37，实现若按回绕值判定将误判有效（冻结约束定义在数学整数上）。
        invalidMutations.push_back([](PolicyDepthConfig& c) {
            c.sampleCount = (std::size_t{1} << 63) + 1;
            c.sampleSkip = 2;
        });

        for (const auto& mutate : invalidMutations) {
            PolicyDepthConfig cfg;
            mutate(cfg);
            RIN_CHECK_MSG(!cfg.valid(), "非法 config 必须 valid()==false");
            RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&cfg] {
                              PolicyDepthHistory history(cfg);
                              (void)history;
                          }),
                          "非法 config 构造 PolicyDepthHistory 必须抛 std::invalid_argument");
        }
    }

    // --- 1c) valid() 边界正例（构造通过）---
    {
        PolicyDepthConfig cfg;  // radius/sigma 上界：10 / 10.0
        cfg.blurRadius = 10;
        cfg.blurSigma = 10.0;
        RIN_CHECK(cfg.valid());
        const PolicyDepthHistory history(cfg);
        RIN_CHECK(history.empty());
    }
    {
        PolicyDepthConfig cfg;  // sigma 下界 0（δ 核）
        cfg.blurSigma = 0.0;
        RIN_CHECK(cfg.valid());
    }
    {
        // delay 恰好上界：36 + 1 = 37 ≤ 37（check_delay_bounds 通过条件）。
        PolicyDepthConfig cfg;
        cfg.sampleDelay = 1;
        RIN_CHECK(cfg.valid());
        const PolicyDepthHistory history(cfg);
        RIN_CHECK(history.empty());
    }
    {
        // 历史上限边界：4096 可构造（≤ 上限）；4097 已在 1b 拒绝。
        PolicyDepthConfig cfg;
        cfg.historyLength = 4096;
        RIN_CHECK(cfg.valid());
        const PolicyDepthHistory history(cfg);
        RIN_CHECK(history.empty());
    }
    {
        // 微型环：historyLength=1、sampleCount=1（framesNeeded = 1 ≤ 1）。
        PolicyDepthConfig cfg;
        cfg.gridWidth = 8;
        cfg.gridHeight = 8;
        cfg.cropUp = 2;
        cfg.cropDown = 1;
        cfg.cropLeft = 1;
        cfg.cropRight = 1;
        cfg.historyLength = 1;
        cfg.sampleCount = 1;
        cfg.sampleSkip = 1;
        cfg.sampleDelay = 0;
        RIN_CHECK(cfg.valid());
        PolicyDepthHistory history(cfg);
        RIN_CHECK_EQ(history.frameWidth(), std::uint32_t{6});
        RIN_CHECK_EQ(history.frameHeight(), std::uint32_t{5});
        const auto frame = [](std::uint32_t k) {
            return makeDepth(
                6, 5, [k](std::uint32_t x, std::uint32_t y) { return histPixel(k, x, y, 6); });
        };
        tryCall("O7 append", [&] { history.append(frame(0)); });
        tryCall("O7 append", [&] { history.append(frame(1)); });  // 环满覆盖最旧（容量 1）
        RIN_CHECK_EQ(history.size(), std::size_t{1});
        // idx[0] = 1 − 0 − 1 − 0 = 0 → 最新帧。
        RIN_CHECK_MSG(vectorExact(history.sample(), histSampleExpected({1}, 6, 5)),
                      "historyLength=1 环满覆盖后 sample 取最新帧");
    }

    // ===================================================================
    // 2) O1 fillDepthInvalid（§5：!isfinite(v) 或 v ≤ invalidBelow → farValue）
    // ===================================================================

    // --- 2a) NaN/+Inf/−Inf/0/负值/正常值混合 golden（far=2.5，invalidBelow=0）---
    // 手推：NaN/+Inf/−Inf 非有限 → 2.5；0 ≤ 0 → 2.5；−0.25 ≤ 0 → 2.5；
    //       1.25 > 0 → 1.25；2.5 > 0 → 2.5；0.5 > 0 → 0.5。
    {
        const DepthFrameF32 input = makeDepth(4, 2, [](std::uint32_t x, std::uint32_t y) {
            const float mixed[2][4] = {
                {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                 -std::numeric_limits<float>::infinity(), 0.0f},
                {-0.25f, 1.25f, 2.5f, 0.5f}};
            return mixed[y][x];
        });
        RIN_CHECK_MSG(input.valid(), "前置：测试输入帧构造有效");
        const DepthFrameF32 output = tryOp(
            "O1 fillDepthInvalid", [&] { return rin::fillDepthInvalid(input, 2.5); },
            DepthFrameF32{});
        const std::vector<float> expected = {2.5f, 2.5f, 2.5f, 2.5f, 2.5f, 1.25f, 2.5f, 0.5f};
        RIN_CHECK_MSG(frameExact(output, expected), "O1 混合 golden 4×2");
        RIN_CHECK(compactValid(output, 4, 2));         // 紧凑新缓冲
        RIN_CHECK(output.pixels() != input.pixels());  // 非共享
    }

    // --- 2b) invalidBelow 非默认（0.5；恰等边界走 ≤）---
    // 手推：NaN/+Inf/−Inf → 2.5；0 ≤ 0.5 → 2.5；0.5 ≤ 0.5 → 2.5（恰等）；
    //       0.75 > 0.5 → 0.75；0.25 ≤ 0.5 → 2.5；2.0 > 0.5 → 2.0。
    {
        const DepthFrameF32 input = makeDepth(4, 2, [](std::uint32_t x, std::uint32_t y) {
            const float mixed[2][4] = {
                {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                 -std::numeric_limits<float>::infinity(), 0.0f},
                {0.5f, 0.75f, 0.25f, 2.0f}};
            return mixed[y][x];
        });
        RIN_CHECK_MSG(input.valid(), "前置：测试输入帧构造有效");
        const DepthFrameF32 output = tryOp(
            "O1 fillDepthInvalid", [&] { return rin::fillDepthInvalid(input, 2.5, 0.5); },
            DepthFrameF32{});
        const std::vector<float> expected = {2.5f, 2.5f, 2.5f, 2.5f, 2.5f, 0.75f, 2.5f, 2.0f};
        RIN_CHECK_MSG(frameExact(output, expected), "O1 invalidBelow=0.5 golden 4×2");
    }

    // --- 2c) 负阈值 invalidBelow = −0.75 ---
    // 手推：−1.0 ≤ −0.75 → 2.5；−0.75 ≤ −0.75 → 2.5（恰等）；−0.5 > −0.75 → −0.5；
    //       0.25 > −0.75 → 0.25。
    {
        const DepthFrameF32 input = makeDepth(2, 2, [](std::uint32_t x, std::uint32_t y) {
            const float mixed[2][2] = {{-1.0f, -0.75f}, {-0.5f, 0.25f}};
            return mixed[y][x];
        });
        RIN_CHECK_MSG(input.valid(), "前置：测试输入帧构造有效");
        const DepthFrameF32 output = tryOp(
            "O1 fillDepthInvalid", [&] { return rin::fillDepthInvalid(input, 2.5, -0.75); },
            DepthFrameF32{});
        const std::vector<float> expected = {2.5f, 2.5f, -0.5f, 0.25f};
        RIN_CHECK_MSG(frameExact(output, expected), "O1 invalidBelow=−0.75 golden 2×2");
    }

    // --- 2d) farValue 非有限拒绝 + 无效帧拒绝 + padding 输入不泄漏 ---
    {
        const DepthFrameF32 input =
            makeDepth(2, 1, [](std::uint32_t, std::uint32_t) { return 1.0f; });
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::fillDepthInvalid(input, std::numeric_limits<double>::quiet_NaN()); }));
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::fillDepthInvalid(input, std::numeric_limits<double>::infinity()); }));
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::fillDepthInvalid(input, -std::numeric_limits<double>::infinity()); }));
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::fillDepthInvalid(DepthFrameF32{}, 2.5); }));

        // padding 输入：逻辑内容 [[NaN, 0.5], [−1, 2]]，padding = −12345 → 输出紧凑
        // 且 padding 不泄漏。手推（far=2.5，invalidBelow=0 默认）：NaN → 2.5；
        // 0.5 > 0 保留 0.5；−1 ≤ 0 → 2.5；2.0 保留（期望逐位 = 逻辑内容填充结果）。
        const DepthFrameF32 padded = makeDepthPadded(
            2, 2, 5,
            [](std::uint32_t x, std::uint32_t y) {
                const float mixed[2][2] = {{std::numeric_limits<float>::quiet_NaN(), 0.5f},
                                           {-1.0f, 2.0f}};
                return mixed[y][x];
            },
            -12345.0f);
        RIN_CHECK_MSG(padded.valid(), "前置：padding 测试帧构造有效");
        const DepthFrameF32 output = tryOp(
            "O1 fillDepthInvalid", [&] { return rin::fillDepthInvalid(padded, 2.5); },
            DepthFrameF32{});
        const std::vector<float> expectedPadded = {2.5f, 0.5f, 2.5f, 2.0f};
        RIN_CHECK_MSG(frameExact(output, expectedPadded), "O1 padding 输入紧凑不泄漏");
    }

    // ===================================================================
    // 3) O2 resizeDepthArea（§5：面积加权箱式平均，仅缩小）
    // ===================================================================

    // --- 3a) 2×2 → 1×1：sx=sy=2，等权均值（手推 (1+2+3+4)/4 = 2.5）---
    {
        const DepthFrameF32 input = makeDepth(2, 2, [](std::uint32_t x, std::uint32_t y) {
            const float v[2][2] = {{1.0f, 2.0f}, {3.0f, 4.0f}};
            return v[y][x];
        });
        RIN_CHECK_MSG(input.valid(), "前置：测试输入帧构造有效");
        const DepthFrameF32 output = tryOp(
            "O2 resizeDepthArea", [&] { return rin::resizeDepthArea(input, 1, 1); },
            DepthFrameF32{});
        RIN_CHECK(compactValid(output, 1, 1));
        const std::vector<float> expected = {2.5f};
        RIN_CHECK_MSG(frameExact(output, expected), "O2 2×2→1×1 golden");
    }

    // --- 3b) 3×2 → 2×1：分数覆盖权重手推 ---
    // sx = 3/2 = 1.5、sy = 2。out(0,0)：列覆盖 [0,1.5) → col0 w=1、col1 w=0.5；
    //   行覆盖 [0,2) → 两行 h=1。值 = (1·1+0.5·2) + (1·4+0.5·5) = 8.5 → /3。
    // out(1,0)：列覆盖 [1.5,3) → col1 w=0.5、col2 w=1。
    //   值 = (0.5·2+1·3) + (0.5·5+1·6) = 12.5 → /3。
    {
        const DepthFrameF32 input = makeDepth(3, 2, [](std::uint32_t x, std::uint32_t y) {
            const float v[2][3] = {{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}};
            return v[y][x];
        });
        RIN_CHECK_MSG(input.valid(), "前置：测试输入帧构造有效");
        const DepthFrameF32 output = tryOp(
            "O2 resizeDepthArea", [&] { return rin::resizeDepthArea(input, 2, 1); },
            DepthFrameF32{});
        RIN_CHECK(compactValid(output, 2, 1));
        const std::vector<float> expected = {8.5f / 3.0f, 12.5f / 3.0f};
        RIN_CHECK_MSG(frameMaxDiff(output, {8.5 / 3.0, 12.5 / 3.0}) <= 1e-6,
                      "O2 3×2→2×1 分数覆盖 golden");
        RIN_CHECK_MSG(frameExact(output, expected), "O2 3×2→2×1 float32 位级");
    }

    // --- 3c) 8×8 → 2×2：整数倍 4× 恒比（v = x + 0.25·y，块均值）---
    // 手推（每块 4×4，除以 sx·sy = 16）：
    //   (0,0)：ΣΣx = 4·6 = 24，0.25·ΣΣy = 0.25·4·6 = 6 → 30/16 = 1.875
    //   (1,0)：x∈[4,8)：4·22 = 88，+6 → 94/16 = 5.875
    //   (0,1)：y∈[4,8)：24 + 0.25·4·22 = 24+22 → 46/16 = 2.875
    //   (1,1)：88+22 → 110/16 = 6.875
    {
        const DepthFrameF32 input = makeDepth(8, 8, [](std::uint32_t x, std::uint32_t y) {
            return static_cast<float>(static_cast<double>(x) + 0.25 * static_cast<double>(y));
        });
        RIN_CHECK_MSG(input.valid(), "前置：测试输入帧构造有效");
        const DepthFrameF32 output = tryOp(
            "O2 resizeDepthArea", [&] { return rin::resizeDepthArea(input, 2, 2); },
            DepthFrameF32{});
        RIN_CHECK(compactValid(output, 2, 2));
        const std::vector<float> expected = {1.875f, 5.875f, 2.875f, 6.875f};
        RIN_CHECK_MSG(frameExact(output, expected), "O2 8×8→2×2 整数倍 4× 恒比");
    }

    // --- 3d) 同尺寸恒等：逐像素位级、紧凑新缓冲（含 padding 输入）---
    {
        std::uint32_t seed = 424242u;
        const DepthFrameF32 input = makeDepth(37, 23, [&seed](std::uint32_t, std::uint32_t) {
            return static_cast<float>(0.1 + 2.35 * lcg01(seed));
        });
        RIN_CHECK_MSG(input.valid(), "前置：测试输入帧构造有效");
        const std::vector<float> logical = frameLogical(input);
        const DepthFrameF32 output = tryOp(
            "O2 resizeDepthArea", [&] { return rin::resizeDepthArea(input, 37, 23); },
            DepthFrameF32{});
        RIN_CHECK(compactValid(output, 37, 23));
        RIN_CHECK_MSG(frameExact(output, logical), "O2 同尺寸逐像素恒等");
        RIN_CHECK(output.pixels() != input.pixels());  // 恒等仍是新缓冲

        // padding 输入恒等：输出紧凑 = 逻辑内容，padding（−12345）不泄漏。
        std::uint32_t seed2 = 777u;
        const DepthFrameF32 padded = makeDepthPadded(
            5, 3, 9,
            [&seed2](std::uint32_t, std::uint32_t) {
                return static_cast<float>(0.5 + 1.5 * lcg01(seed2));
            },
            -12345.0f);
        RIN_CHECK_MSG(padded.valid(), "前置：padding 测试帧构造有效");
        const std::vector<float> paddedLogical = frameLogical(padded);
        const DepthFrameF32 same = tryOp(
            "O2 resizeDepthArea", [&] { return rin::resizeDepthArea(padded, 5, 3); },
            DepthFrameF32{});
        RIN_CHECK_MSG(frameExact(same, paddedLogical), "O2 padding 输入同尺寸恒等（紧凑）");
    }

    // --- 3e) 大图与测试内独立 double 参考交叉（|diff| ≤ 1e−6 绝对）---
    {
        // 848×480 → 64×36（sx = 13.25、sy = 480/36 ≈ 13.333，分数覆盖）。
        std::uint32_t seed = 20260930u;
        const DepthFrameF32 input = makeDepth(848, 480, [&seed](std::uint32_t, std::uint32_t) {
            return static_cast<float>(0.1 + 2.35 * lcg01(seed));
        });
        RIN_CHECK_MSG(input.valid(), "前置：测试输入帧构造有效");
        const DepthFrameF32 output = tryOp(
            "O2 resizeDepthArea", [&] { return rin::resizeDepthArea(input, 64, 36); },
            DepthFrameF32{});
        RIN_CHECK(compactValid(output, 64, 36));
        if (output.valid()) {
            const double diff =
                frameMaxDiff(output, refAreaResize(frameToDoubles(input), 848, 480, 64, 36));
            RIN_CHECK_MSG(diff <= 1e-6, "O2 848×480→64×36 独立参考交叉");

            // 848×480 → 848×240（sx = 1、sy = 2：单维缩小）。
            const DepthFrameF32 tall = tryOp(
                "O2 resizeDepthArea", [&] { return rin::resizeDepthArea(input, 848, 240); },
                DepthFrameF32{});
            RIN_CHECK(compactValid(tall, 848, 240));
            const double diffTall =
                frameMaxDiff(tall, refAreaResize(frameToDoubles(input), 848, 480, 848, 240));
            RIN_CHECK_MSG(diffTall <= 1e-6, "O2 848×480→848×240 独立参考交叉");
        }

        // 5×4 → 3×2（sx = 5/3、sy = 2 双分数）。
        std::uint32_t seed3 = 991u;
        const DepthFrameF32 small = makeDepth(5, 4, [&seed3](std::uint32_t, std::uint32_t) {
            return static_cast<float>(0.2 + 1.8 * lcg01(seed3));
        });
        RIN_CHECK_MSG(small.valid(), "前置：测试输入帧构造有效");
        const DepthFrameF32 outSmall = tryOp(
            "O2 resizeDepthArea", [&] { return rin::resizeDepthArea(small, 3, 2); },
            DepthFrameF32{});
        RIN_CHECK(compactValid(outSmall, 3, 2));
        if (outSmall.valid()) {
            const double diffSmall =
                frameMaxDiff(outSmall, refAreaResize(frameToDoubles(small), 5, 4, 3, 2));
            RIN_CHECK_MSG(diffSmall <= 1e-6, "O2 5×4→3×2 独立参考交叉");
        }

        // 7×5 → 3×2（sx = 7/3、sy = 2.5）。
        std::uint32_t seed4 = 313u;
        const DepthFrameF32 small2 = makeDepth(7, 5, [&seed4](std::uint32_t, std::uint32_t) {
            return static_cast<float>(0.3 + 1.7 * lcg01(seed4));
        });
        RIN_CHECK_MSG(small2.valid(), "前置：测试输入帧构造有效");
        const DepthFrameF32 outSmall2 = tryOp(
            "O2 resizeDepthArea", [&] { return rin::resizeDepthArea(small2, 3, 2); },
            DepthFrameF32{});
        RIN_CHECK(compactValid(outSmall2, 3, 2));
        if (outSmall2.valid()) {
            const double diffSmall2 =
                frameMaxDiff(outSmall2, refAreaResize(frameToDoubles(small2), 7, 5, 3, 2));
            RIN_CHECK_MSG(diffSmall2 <= 1e-6, "O2 7×5→3×2 独立参考交叉");
        }
    }

    // --- 3f) 拒绝分支：放大（含单维）、零尺寸、无效帧 ---
    {
        const DepthFrameF32 input =
            makeDepth(4, 4, [](std::uint32_t, std::uint32_t) { return 1.0f; });
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::resizeDepthArea(input, 8, 8); }));  // 双维放大
        const DepthFrameF32 camera =
            makeDepth(848, 480, [](std::uint32_t, std::uint32_t) { return 1.0f; });
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::resizeDepthArea(camera, 900, 240); }));  // 单维放大（宽）
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::resizeDepthArea(camera, 848, 500); }));  // 单维放大（高）
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::resizeDepthArea(camera, 0, 240); }));  // 零宽目标
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::resizeDepthArea(camera, 848, 0); }));  // 零高目标
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::resizeDepthArea(DepthFrameF32{}, 2, 2); }));  // 无效帧
    }

    // ===================================================================
    // 4) O3 cropDepth（§5：行 [up, H−down)、列 [left, W−right)；退化/越界抛）
    // ===================================================================

    // --- 4a) 64×36 (18,0,16,16) → 18×32 索引逐位 ---
    // 手推：out(y,x) = in(y+18, x+16)；输入 in(x,y) = 100·y + x（float32 精确）。
    {
        const DepthFrameF32 input = makeDepth(64, 36, [](std::uint32_t x, std::uint32_t y) {
            return static_cast<float>(100u * y + x);
        });
        RIN_CHECK_MSG(input.valid(), "前置：测试输入帧构造有效");
        const DepthFrameF32 output = tryOp(
            "O3 cropDepth", [&] { return rin::cropDepth(input, 18, 0, 16, 16); }, DepthFrameF32{});
        RIN_CHECK(compactValid(output, 32, 18));
        RIN_CHECK(output.pixels() != input.pixels());
        std::vector<float> expected;
        expected.reserve(576);
        for (std::uint32_t y = 0; y < 18; ++y) {
            for (std::uint32_t x = 0; x < 32; ++x) {
                expected.push_back(static_cast<float>(100u * (y + 18) + (x + 16)));
            }
        }
        RIN_CHECK_MSG(frameExact(output, expected), "O3 64×36 (18,0,16,16) → 18×32 索引逐位");
    }

    // --- 4b) padding stride 输入：按逻辑内容取窗，padding 不泄漏 ---
    {
        const DepthFrameF32 padded = makeDepthPadded(
            64, 36, 71,
            [](std::uint32_t x, std::uint32_t y) { return static_cast<float>(100u * y + x); },
            -12345.0f);
        RIN_CHECK_MSG(padded.valid(), "前置：padding 测试帧构造有效");
        const DepthFrameF32 output = tryOp(
            "O3 cropDepth", [&] { return rin::cropDepth(padded, 18, 0, 16, 16); }, DepthFrameF32{});
        RIN_CHECK(compactValid(output, 32, 18));  // 紧凑（不继承 71）
        std::vector<float> expected;
        expected.reserve(576);
        for (std::uint32_t y = 0; y < 18; ++y) {
            for (std::uint32_t x = 0; x < 32; ++x) {
                expected.push_back(static_cast<float>(100u * (y + 18) + (x + 16)));
            }
        }
        RIN_CHECK_MSG(frameExact(output, expected), "O3 padding stride 输入索引逐位");
    }

    // --- 4c) 小图 golden：3×3 (1,1,1,1) → 1×1；4×3 (0,1,0,2) → 2×2 ---
    {
        const DepthFrameF32 input = makeDepth(
            3, 3, [](std::uint32_t x, std::uint32_t y) { return static_cast<float>(10u * y + x); });
        RIN_CHECK_MSG(input.valid(), "前置：测试输入帧构造有效");
        const DepthFrameF32 center = tryOp(
            "O3 cropDepth", [&] { return rin::cropDepth(input, 1, 1, 1, 1); }, DepthFrameF32{});
        RIN_CHECK(compactValid(center, 1, 1));
        RIN_CHECK_MSG(frameExact(center, {11.0f}), "O3 3×3 (1,1,1,1) → in(1,1)");

        const DepthFrameF32 input43 = makeDepth(
            4, 3, [](std::uint32_t x, std::uint32_t y) { return static_cast<float>(10u * y + x); });
        // 行域 [0, 3−1) = [0,2)、列域 [0, 4−2) = [0,2)：out = [0,1;10,11]。
        const DepthFrameF32 topLeft = tryOp(
            "O3 cropDepth", [&] { return rin::cropDepth(input43, 0, 1, 0, 2); }, DepthFrameF32{});
        RIN_CHECK(compactValid(topLeft, 2, 2));
        RIN_CHECK_MSG(frameExact(topLeft, {0.0f, 1.0f, 10.0f, 11.0f}), "O3 4×3 (0,1,0,2) golden");
    }

    // --- 4d) 退化/越界拒绝全分支 + 贴边合法边界 + 无效帧 ---
    {
        const DepthFrameF32 input = makeDepth(64, 36, [](std::uint32_t x, std::uint32_t y) {
            return static_cast<float>(100u * y + x);
        });
        RIN_CHECK_MSG(input.valid(), "前置：测试输入帧构造有效");
        // up+down == H / > H；left+right == W / > W。
        RIN_CHECK(
            throwsAs<std::invalid_argument>([&] { (void)rin::cropDepth(input, 18, 18, 0, 0); }));
        RIN_CHECK(
            throwsAs<std::invalid_argument>([&] { (void)rin::cropDepth(input, 19, 18, 0, 0); }));
        RIN_CHECK(
            throwsAs<std::invalid_argument>([&] { (void)rin::cropDepth(input, 0, 0, 32, 32); }));
        RIN_CHECK(
            throwsAs<std::invalid_argument>([&] { (void)rin::cropDepth(input, 0, 0, 32, 33); }));
        // 贴边合法（up+down = H−1、left+right = W−1）。
        {
            const DepthFrameF32 bottomRow = tryOp(
                "O3 cropDepth", [&] { return rin::cropDepth(input, 35, 0, 0, 0); },
                DepthFrameF32{});
            RIN_CHECK(compactValid(bottomRow, 64, 1));
            std::vector<float> expectedRow;
            expectedRow.reserve(64);
            for (std::uint32_t x = 0; x < 64; ++x) {
                expectedRow.push_back(static_cast<float>(100u * 35 + x));  // in(x, 35)
            }
            RIN_CHECK_MSG(frameExact(bottomRow, expectedRow), "O3 贴边行裁切索引逐位");
        }
        {
            const DepthFrameF32 pixel = tryOp(
                "O3 cropDepth", [&] { return rin::cropDepth(input, 0, 35, 63, 0); },
                DepthFrameF32{});
            RIN_CHECK(compactValid(pixel, 1, 1));
            RIN_CHECK_MSG(frameExact(pixel, {63.0f}), "O3 贴边单像素裁切 = in(63,0)");
        }
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::cropDepth(DepthFrameF32{}, 1, 1, 1, 1); }));
    }

    // ===================================================================
    // 5) O4 gaussianBlurDepth（§5：可分离两趟 + reflect-101 + double 中间量）
    // ===================================================================

    // 冻结公式独立求值：a = exp(−0.5)/(1+2·exp(−0.5))、b = 1/(1+2·exp(−0.5))。
    const double expHalf = std::exp(-0.5);
    const double normSum = 1.0 + 2.0 * expHalf;
    const double kernelA = expHalf / normSum;  // k[0] = k[2]
    const double kernelB = 1.0 / normSum;      // k[1]

    // --- 5a) 核系数 golden：经 1×3 脉冲响应读出 ---
    // 手推（水平一维核 out(x) = Σₜ k[t]·src[refl(x+t−1)]，高度 1 时垂直趟恒等）：
    //   src = [1,0,0] → out(0) = k1·1 = b；out(1) = k0·1 = a；out(2) = 0
    //   src = [0,1,0] → out(0) = a·1 + a·1 = 2a；out(1) = b；out(2) = 2a
    {
        const DepthFrameF32 left = makeDepth(3, 1, [](std::uint32_t x, std::uint32_t) {
            const float v[3] = {1.0f, 0.0f, 0.0f};
            return v[x];
        });
        RIN_CHECK_MSG(left.valid(), "前置：测试输入帧构造有效");
        const DepthFrameF32 outLeft = tryOp(
            "O4 gaussianBlurDepth", [&] { return rin::gaussianBlurDepth(left, 1, 1.0); },
            DepthFrameF32{});
        RIN_CHECK(compactValid(outLeft, 3, 1));
        RIN_CHECK_MSG(frameMaxDiff(outLeft, {kernelB, kernelA, 0.0}) <= 1e-6,
                      "O4 核系数 golden（[1,0,0] 脉冲 → [b, a, 0]）");

        const DepthFrameF32 center = makeDepth(3, 1, [](std::uint32_t x, std::uint32_t) {
            const float v[3] = {0.0f, 1.0f, 0.0f};
            return v[x];
        });
        const DepthFrameF32 outCenter = tryOp(
            "O4 gaussianBlurDepth", [&] { return rin::gaussianBlurDepth(center, 1, 1.0); },
            DepthFrameF32{});
        RIN_CHECK_MSG(frameMaxDiff(outCenter, {2.0 * kernelA, kernelB, 2.0 * kernelA}) <= 1e-6,
                      "O4 核系数 golden（[0,1,0] 脉冲 → [2a, b, 2a]）");
        // 一致性：2a + b = 1（归一化）。
        RIN_CHECK(std::abs(2.0 * kernelA + kernelB - 1.0) <= 1e-12);
    }

    // --- 5b) σ=0 δ 核恒等（K > 图幅、深度折返仍逐位恒等）---
    {
        std::uint32_t seed = 555u;
        const DepthFrameF32 input = makeDepth(5, 4, [&seed](std::uint32_t, std::uint32_t) {
            return static_cast<float>(0.1 + 2.3 * lcg01(seed));
        });
        RIN_CHECK_MSG(input.valid(), "前置：测试输入帧构造有效");
        const std::vector<float> logical = frameLogical(input);
        for (const std::uint32_t radius : {1u, 3u, 10u}) {
            const DepthFrameF32 output = tryOp(
                "O4 gaussianBlurDepth", [&] { return rin::gaussianBlurDepth(input, radius, 0.0); },
                DepthFrameF32{});
            RIN_CHECK(compactValid(output, 5, 4));
            RIN_CHECK_MSG(frameExact(output, logical),
                          "O4 σ=0 δ 核恒等 radius=" + std::to_string(radius));
        }
    }

    // --- 5c) 常值图恒等（Σk = 1 质量守恒；double 中间量不量化的推论）---
    {
        const DepthFrameF32 input =
            makeDepth(7, 5, [](std::uint32_t, std::uint32_t) { return 1.75f; });
        RIN_CHECK_MSG(input.valid(), "前置：测试输入帧构造有效");
        const DepthFrameF32 output = tryOp(
            "O4 gaussianBlurDepth", [&] { return rin::gaussianBlurDepth(input, 3, 1.5); },
            DepthFrameF32{});
        RIN_CHECK(compactValid(output, 7, 5));
        const std::vector<float> expected(35, 1.75f);
        RIN_CHECK_MSG(frameExact(output, expected), "O4 常值图 1.75 恒等（radius=3, σ=1.5）");
    }

    // --- 5d) 角点脉冲 reflect-101 折返手推 golden（5×5，脉冲在 (0,0)）---
    // 手推：水平趟行 0：[1,0,0,0,0] → [b, a, 0, 0, 0]（x=0 折返取 s(1)=0，脉冲仅被
    //       中心权重 b 命中一次）；垂直趟列 0/1 同构 → out(0,0) = b²、out(1,0) = a·b、
    //       out(0,1) = a·b、out(1,1) = a²，其余恰 0；总能量 = (a+b)²（见下方断言）。
    {
        const DepthFrameF32 input = makeDepth(5, 5, [](std::uint32_t x, std::uint32_t y) {
            return (x == 0 && y == 0) ? 1.0f : 0.0f;
        });
        RIN_CHECK_MSG(input.valid(), "前置：测试输入帧构造有效");
        const DepthFrameF32 output = tryOp(
            "O4 gaussianBlurDepth", [&] { return rin::gaussianBlurDepth(input, 1, 1.0); },
            DepthFrameF32{});
        RIN_CHECK(compactValid(output, 5, 5));
        std::vector<double> expected(25, 0.0);
        expected[0] = kernelB * kernelB;  // (x=0, y=0)
        expected[1] = kernelA * kernelB;  // (x=1, y=0)
        expected[5] = kernelA * kernelB;  // (x=0, y=1)
        expected[6] = kernelA * kernelA;  // (x=1, y=1)
        const double diff = frameMaxDiff(output, expected);
        RIN_CHECK_MSG(diff <= 1e-6, "O4 角点脉冲 reflect-101 折返 golden");
        if (output.valid()) {
            // 其余 21 像素恰为 0（无能量泄漏到远端）。
            bool restZero = true;
            for (std::size_t i = 0; i < 25; ++i) {
                if (i != 0 && i != 1 && i != 5 && i != 6) {
                    restZero = restZero && output.pixels()->at(i) == 0.0f;
                }
            }
            RIN_CHECK(restZero);
            // reflect-101 角点脉冲能量 = (边系数+中心系数)²：边缘不复制边缘像素
            // （refl(−1)=1 处 s(1)=0），脉冲仅被中心权重 b（x=0/y=0 中心趟）与邻位
            // 边系数 a（x=1/y=1 边趟）命中，无单位能量守恒（与 cv2
            // BORDER_REFLECT_101 同语义；与像素级 golden 求和一致）。
            double energy = 0.0;
            for (const float v : *output.pixels()) {
                energy += static_cast<double>(v);
            }
            const double cornerEnergy = (kernelA + kernelB) * (kernelA + kernelB);
            RIN_CHECK_MSG(std::abs(energy - cornerEnergy) <= 1e-6,
                          "O4 角点脉冲能量 = (a+b)²（reflect-101 边缘不复制边缘像素）");
        }
    }

    // --- 5d') 内部脉冲单位能量守恒（7×7，脉冲在 (3,3)，radius=1）---
    // 手推：核支撑 ±1 严格含于图内，任一趟的每个核系数都直接命中脉冲各一次：
    //   水平趟行 3 → [0,0,a,b,a,0,0]（行和 2a+b = 1），垂直趟同构不触边界 →
    //   总能量 = (2a+b)² = 1（质量守恒的真正见证；对照 5d 角点情形的 (a+b)²）。
    {
        const DepthFrameF32 input = makeDepth(7, 7, [](std::uint32_t x, std::uint32_t y) {
            return (x == 3 && y == 3) ? 1.0f : 0.0f;
        });
        RIN_CHECK_MSG(input.valid(), "前置：测试输入帧构造有效");
        const DepthFrameF32 output = tryOp(
            "O4 gaussianBlurDepth", [&] { return rin::gaussianBlurDepth(input, 1, 1.0); },
            DepthFrameF32{});
        RIN_CHECK(compactValid(output, 7, 7));
        if (output.valid()) {
            double energy = 0.0;
            for (const float v : *output.pixels()) {
                energy += static_cast<double>(v);
            }
            RIN_CHECK_MSG(std::abs(energy - 1.0) <= 1e-6, "O4 内部脉冲单位能量守恒");
            // 峰值 = 中心系数平方（可分离两趟各取中心权重）。
            RIN_CHECK_MSG(std::abs(static_cast<double>(output.pixels()->at(3 * 7 + 3)) -
                                   kernelB * kernelB) <= 1e-6,
                          "O4 内部脉冲峰值 = b²");
        }
    }

    // --- 5e) 1×6 窄图手推（线性渐变：内部恒等、边界按 reflect-101）---
    // 手推（v = 0.1..0.6，radius=1, σ=1）：线性函数满足 a·v(x−1)+b·v(x)+a·v(x+1) = v(x)，
    //   内部恒等；out(0) = a·v(1)+b·v(0)+a·v(1) = 2a·0.2 + b·0.1；out(5) = 2a·0.5 + b·0.6。
    {
        const DepthFrameF32 input = makeDepth(6, 1, [](std::uint32_t x, std::uint32_t) {
            return static_cast<float>(0.1 + 0.1 * static_cast<double>(x));
        });
        RIN_CHECK_MSG(input.valid(), "前置：测试输入帧构造有效");
        const DepthFrameF32 output = tryOp(
            "O4 gaussianBlurDepth", [&] { return rin::gaussianBlurDepth(input, 1, 1.0); },
            DepthFrameF32{});
        RIN_CHECK(compactValid(output, 6, 1));
        std::vector<double> expected;
        expected.push_back(2.0 * kernelA * 0.2 + kernelB * 0.1);
        expected.push_back(0.2);
        expected.push_back(0.3);
        expected.push_back(0.4);
        expected.push_back(0.5);
        expected.push_back(2.0 * kernelA * 0.5 + kernelB * 0.6);
        RIN_CHECK_MSG(frameMaxDiff(output, expected) <= 1e-6, "O4 1×6 窄图手推 golden");
    }

    // --- 5f) 13×9 伪随机图 × radius∈{1,3,10}×σ∈{0.5,1.5,10}：双参考交叉 ---
    {
        std::uint32_t seed = 1357u;
        const DepthFrameF32 input = makeDepth(13, 9, [&seed](std::uint32_t, std::uint32_t) {
            return static_cast<float>(0.05 + 2.4 * lcg01(seed));
        });
        RIN_CHECK_MSG(input.valid(), "前置：测试输入帧构造有效");
        const std::vector<double> src = frameToDoubles(input);
        for (const std::uint32_t radius : {1u, 3u, 10u}) {
            for (const double sigma : {0.5, 1.5, 10.0}) {
                const DepthFrameF32 output = tryOp(
                    "O4 gaussianBlurDepth",
                    [&] { return rin::gaussianBlurDepth(input, radius, sigma); }, DepthFrameF32{});
                RIN_CHECK(compactValid(output, 13, 9));
                const std::string tag =
                    "radius=" + std::to_string(radius) + " sigma=" + std::to_string(sigma);
                if (output.valid()) {
                    const double diffSep =
                        frameMaxDiff(output, refBlurSeparable(src, 13, 9, radius, sigma));
                    RIN_CHECK_MSG(diffSep <= 1e-6, "O4 可分离参考交叉 " + tag);
                    const double diffDirect =
                        frameMaxDiff(output, refBlurDirect2D(src, 13, 9, radius, sigma));
                    RIN_CHECK_MSG(diffDirect <= 1e-6, "O4 直接 2D 参考交叉 " + tag);
                }
            }
        }
        // 两条参考路线互相印证（double 全程，差应在机器精度级）。
        const std::vector<double> refSep = refBlurSeparable(src, 13, 9, 3, 1.5);
        const std::vector<double> refDir = refBlurDirect2D(src, 13, 9, 3, 1.5);
        double refGap = 0.0;
        for (std::size_t i = 0; i < refSep.size(); ++i) {
            refGap = std::max(refGap, std::abs(refSep[i] - refDir[i]));
        }
        RIN_CHECK_MSG(refGap <= 1e-12, "O4 可分离/直接 2D 参考互证");
    }

    // --- 5g) padding 输入与紧凑输入一致 + 输出紧凑 ---
    {
        std::vector<float> values(13 * 9);
        std::uint32_t seedA = 246u;
        for (float& v : values) {
            v = static_cast<float>(0.2 + 2.0 * lcg01(seedA));
        }
        const DepthFrameF32 compact =
            makeDepth(13, 9, [&](std::uint32_t x, std::uint32_t y) { return values[y * 13 + x]; });
        const DepthFrameF32 padded = makeDepthPadded(
            13, 9, 20, [&](std::uint32_t x, std::uint32_t y) { return values[y * 13 + x]; },
            -12345.0f);
        RIN_CHECK_MSG(compact.valid() && padded.valid(), "前置：测试输入帧构造有效");
        const DepthFrameF32 outCompact = tryOp(
            "O4 gaussianBlurDepth", [&] { return rin::gaussianBlurDepth(compact, 2, 0.7); },
            DepthFrameF32{});
        const DepthFrameF32 outPadded = tryOp(
            "O4 gaussianBlurDepth", [&] { return rin::gaussianBlurDepth(padded, 2, 0.7); },
            DepthFrameF32{});
        RIN_CHECK(compactValid(outCompact, 13, 9));
        RIN_CHECK(compactValid(outPadded, 13, 9));
        RIN_CHECK_MSG(frameExact(outPadded, frameLogical(outCompact)),
                      "O4 padding 输入结果与紧凑输入逐位一致");
    }

    // --- 5h) radius/sigma 越界与无效帧拒绝（边界值合法已在 5b/5f 覆盖）---
    {
        const DepthFrameF32 input =
            makeDepth(4, 4, [](std::uint32_t, std::uint32_t) { return 1.0f; });
        RIN_CHECK(
            throwsAs<std::invalid_argument>([&] { (void)rin::gaussianBlurDepth(input, 0, 1.0); }));
        RIN_CHECK(
            throwsAs<std::invalid_argument>([&] { (void)rin::gaussianBlurDepth(input, 11, 1.0); }));
        RIN_CHECK(
            throwsAs<std::invalid_argument>([&] { (void)rin::gaussianBlurDepth(input, 1, -0.1); }));
        RIN_CHECK(
            throwsAs<std::invalid_argument>([&] { (void)rin::gaussianBlurDepth(input, 1, 10.5); }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] {
            (void)rin::gaussianBlurDepth(input, 1, std::numeric_limits<double>::quiet_NaN());
        }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] {
            (void)rin::gaussianBlurDepth(input, 1, std::numeric_limits<double>::infinity());
        }));
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::gaussianBlurDepth(DepthFrameF32{}, 1, 1.0); }));
    }

    // ===================================================================
    // 6) O5 clipNormalizeDepth（§5：clip [near, far] 后 (v−near)/(far−near)）
    // ===================================================================

    // --- 6a) 双侧 clip + 区间内恒比（near=0, far=2.5）---
    // 手推：−1 → clip 0 → 0；0 → 0；0.625 → 0.25；1.25 → 0.5；2.5 → 1；3.0 → clip 1。
    {
        const DepthFrameF32 input = makeDepth(3, 2, [](std::uint32_t x, std::uint32_t y) {
            const float v[2][3] = {{-1.0f, 0.0f, 0.625f}, {1.25f, 2.5f, 3.0f}};
            return v[y][x];
        });
        RIN_CHECK_MSG(input.valid(), "前置：测试输入帧构造有效");
        const DepthFrameF32 output = tryOp(
            "O5 clipNormalizeDepth", [&] { return rin::clipNormalizeDepth(input, 0.0, 2.5); },
            DepthFrameF32{});
        RIN_CHECK(compactValid(output, 3, 2));
        const std::vector<float> expected = {0.0f, 0.0f, 0.25f, 0.5f, 1.0f, 1.0f};
        RIN_CHECK_MSG(frameExact(output, expected), "O5 双侧 clip + 区间内恒比 golden");
    }

    // --- 6b) 非零 near（near=1.0, far=3.0）---
    // 手推：0.5 → clip 1 → 0；1.0 → 0；2.0 → 0.5；3.5 → clip 3 → 1。
    {
        const DepthFrameF32 input = makeDepth(2, 2, [](std::uint32_t x, std::uint32_t y) {
            const float v[2][2] = {{0.5f, 1.0f}, {2.0f, 3.5f}};
            return v[y][x];
        });
        const DepthFrameF32 output = tryOp(
            "O5 clipNormalizeDepth", [&] { return rin::clipNormalizeDepth(input, 1.0, 3.0); },
            DepthFrameF32{});
        RIN_CHECK_MSG(frameExact(output, {0.0f, 0.0f, 0.5f, 1.0f}), "O5 非零 near golden");
    }

    // --- 6c) near ≥ far 拒绝 + 无效帧拒绝 ---
    {
        const DepthFrameF32 input =
            makeDepth(2, 1, [](std::uint32_t, std::uint32_t) { return 1.0f; });
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::clipNormalizeDepth(input, 2.5, 2.5); }));
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::clipNormalizeDepth(input, 3.0, 2.5); }));
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::clipNormalizeDepth(DepthFrameF32{}, 0.0, 2.5); }));
    }

    // ===================================================================
    // 7) O6 preprocessPolicyDepthFrame（§5 冻结顺序组合）
    // ===================================================================

    // --- 7a) 合成 848×480 全链 vs 冻结顺序独立 double 参考 ---
    {
        // 伪随机米制深度 [0.1, 2.45]，混入 NaN（%997）、+Inf（%1013）、−0.5（%7331）、
        // 0（%409）：填充先于降采样的冻结次序被交叉用例隐式锁定（无效 0 不参与
        // 面积平均的邻域均值拉低）。
        constexpr std::uint32_t kSrcW = 848;
        constexpr std::uint32_t kSrcH = 480;
        std::vector<float> raw(std::size_t{kSrcW} * kSrcH);
        std::uint32_t seed = 20260930u;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (i % 997 == 0) {
                raw[i] = std::numeric_limits<float>::quiet_NaN();
            } else if (i % 1013 == 0) {
                raw[i] = std::numeric_limits<float>::infinity();
            } else if (i % 7331 == 0) {
                raw[i] = -0.5f;
            } else if (i % 409 == 0) {
                raw[i] = 0.0f;
            } else {
                raw[i] = static_cast<float>(0.1 + 2.35 * lcg01(seed));
            }
        }
        auto rawShared = std::make_shared<const std::vector<float>>(raw);
        const DepthFrameF32 input = DepthFrameF32::wrap(kSrcW, kSrcH, kSrcW, rawShared);
        RIN_CHECK_MSG(input.valid(), "前置：测试输入帧构造有效");

        const PolicyDepthConfig config;  // 默认 = E3-Parkour 部署值
        const DepthFrameF32 output = tryOp(
            "O6 preprocessPolicyDepthFrame",
            [&] { return rin::preprocessPolicyDepthFrame(input, config); }, DepthFrameF32{});
        RIN_CHECK(compactValid(output, 32, 18));  // policyHeight × policyWidth
        RIN_CHECK(output.pixels() != input.pixels());

        // 测试内按冻结顺序重写的 double 参考：填充 → 面积降采样 → 裁切 → 模糊 → 归一化。
        std::vector<double> src(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) {
            src[i] = static_cast<double>(raw[i]);
        }
        const std::vector<double> filled = refFill(src, config.depthFar, 0.0);
        const std::vector<double> grid =
            refAreaResize(filled, kSrcW, kSrcH, config.gridWidth, config.gridHeight);
        const std::vector<double> cropped =
            refCrop(grid, config.gridWidth, config.gridHeight, config.cropUp, config.cropDown,
                    config.cropLeft, config.cropRight);
        const std::vector<double> blurred =
            refBlurSeparable(cropped, config.policyWidth(), config.policyHeight(),
                             config.blurRadius, config.blurSigma);
        const std::vector<double> normalized =
            refClipNormalize(blurred, config.depthNear, config.depthFar);
        const double diff = frameMaxDiff(output, normalized);
        RIN_CHECK_MSG(diff <= 1e-6, "O6 848×480 全链 vs 冻结顺序独立参考");

        // 值域 [0,1]。
        if (output.valid()) {
            bool inRange = true;
            for (const float v : *output.pixels()) {
                inRange = inRange && v >= 0.0f && v <= 1.0f;
            }
            RIN_CHECK_MSG(inRange, "O6 输出值域 [0,1]");
        }
    }

    // --- 7b) config 无效 / 输入无效拒绝 ---
    {
        const DepthFrameF32 input =
            makeDepth(8, 8, [](std::uint32_t, std::uint32_t) { return 1.0f; });
        PolicyDepthConfig badBlur;
        badBlur.blurRadius = 0;
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::preprocessPolicyDepthFrame(input, badBlur); }));
        PolicyDepthConfig badRange;
        badRange.depthNear = 2.5;
        badRange.depthFar = 2.5;
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::preprocessPolicyDepthFrame(input, badRange); }));
        PolicyDepthConfig badDelay;
        badDelay.sampleDelay = 2;  // framesNeeded = 38 > 37
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::preprocessPolicyDepthFrame(input, badDelay); }));
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::preprocessPolicyDepthFrame(DepthFrameF32{}, PolicyDepthConfig{}); }));
    }

    // ===================================================================
    // 8) O7 PolicyDepthHistory（§5：环形历史 + 首帧填充抽样）
    // ===================================================================

    const auto policyFrame = [](std::uint32_t k) {
        return makeDepth(32, 18,
                         [k](std::uint32_t x, std::uint32_t y) { return histPixel(k, x, y, 32); });
    };

    // --- 8a) 空历史全 0 + 元数据 ---
    {
        const PolicyDepthHistory history(PolicyDepthConfig{});
        RIN_CHECK(history.empty());
        RIN_CHECK_EQ(history.size(), std::size_t{0});
        RIN_CHECK_EQ(history.frameWidth(), std::uint32_t{32});
        RIN_CHECK_EQ(history.frameHeight(), std::uint32_t{18});
        const std::vector<float> expected(8 * 576, 0.0f);
        RIN_CHECK_MSG(vectorExact(history.sample(), expected), "O7 空历史全 0（8×18×32）");
    }

    // --- 8b) append 1 帧：欠帧首帧填充（padded[idx<36] 全部指向首帧）---
    {
        PolicyDepthHistory history(PolicyDepthConfig{});
        tryCall("O7 append", [&] { history.append(policyFrame(0)); });
        RIN_CHECK_EQ(history.size(), std::size_t{1});
        const std::vector<std::uint32_t> indices(8, 0u);
        RIN_CHECK_MSG(vectorExact(history.sample(), histSampleExpected(indices, 32, 18)),
                      "O7 append 1 帧 → 8 平面全为首帧");
    }

    // --- 8c) append 5 帧：欠帧首帧填充（pad=32，idx {1..31} → f0，36 → f4）---
    {
        PolicyDepthHistory history(PolicyDepthConfig{});
        for (std::uint32_t k = 0; k < 5; ++k) {
            tryCall("O7 append", [&history, &policyFrame, k] { history.append(policyFrame(k)); });
        }
        RIN_CHECK_EQ(history.size(), std::size_t{5});
        // 概念序列 = [f0]×32 + [f0..f4]；idx {1,6,11,16,21,26,31,36} →
        // {f0,f0,f0,f0,f0,f0,f0,f4}。
        const std::vector<std::uint32_t> indices = {0, 0, 0, 0, 0, 0, 0, 4};
        RIN_CHECK_MSG(vectorExact(history.sample(), histSampleExpected(indices, 32, 18)),
                      "O7 append 5 帧欠帧首帧填充逐位 golden");
    }

    // --- 8d) append 36 帧（pad=1；idx {1,6,…,31,36} → {f0,f5,…,f30,f35}）---
    // --- 8e) append 37 帧（满环；idx {1,6,…,36} → {f1,f6,…,f36}）---
    // --- 8f) append 40 帧（滚动；环持 f3..f39 → {f4,f9,…,f34,f39}）---
    {
        PolicyDepthHistory history(PolicyDepthConfig{});
        for (std::uint32_t k = 0; k < 36; ++k) {
            tryCall("O7 append", [&history, &policyFrame, k] { history.append(policyFrame(k)); });
        }
        RIN_CHECK_EQ(history.size(), std::size_t{36});
        const std::vector<std::uint32_t> indices36 = {0, 5, 10, 15, 20, 25, 30, 35};
        RIN_CHECK_MSG(vectorExact(history.sample(), histSampleExpected(indices36, 32, 18)),
                      "O7 append 36 帧欠帧首帧填充逐位 golden");

        tryCall("O7 append", [&] { history.append(policyFrame(36)); });
        RIN_CHECK_EQ(history.size(), std::size_t{37});
        const std::vector<std::uint32_t> indices37 = {1, 6, 11, 16, 21, 26, 31, 36};
        RIN_CHECK_MSG(vectorExact(history.sample(), histSampleExpected(indices37, 32, 18)),
                      "O7 满环 37 帧抽样下标 {1,6,…,36}");

        for (std::uint32_t k = 37; k < 40; ++k) {
            tryCall("O7 append", [&history, &policyFrame, k] { history.append(policyFrame(k)); });
        }
        RIN_CHECK_EQ(history.size(), std::size_t{37});  // 环满不再增长
        const std::vector<std::uint32_t> indices40 = {4, 9, 14, 19, 24, 29, 34, 39};
        RIN_CHECK_MSG(vectorExact(history.sample(), histSampleExpected(indices40, 32, 18)),
                      "O7 滚动 40 帧后抽样 = f4..f39 的 {1,6,…,36}");

        // reset 幂等：清空后全 0，再 append 单帧全平面生效。
        history.reset();
        RIN_CHECK(history.empty());
        RIN_CHECK_EQ(history.size(), std::size_t{0});
        const std::vector<float> zeros(8 * 576, 0.0f);
        RIN_CHECK(vectorExact(history.sample(), zeros));
        tryCall("O7 append", [&] { history.append(policyFrame(7)); });
        const std::vector<std::uint32_t> indicesReset(8, 7u);
        RIN_CHECK(vectorExact(history.sample(), histSampleExpected(indicesReset, 32, 18)));
    }

    // --- 8g) sampleDelay=1：idx {0,5,…,35}（满环 37 帧）---
    {
        PolicyDepthConfig cfg;
        cfg.sampleDelay = 1;
        PolicyDepthHistory history(cfg);
        for (std::uint32_t k = 0; k < 37; ++k) {
            tryCall("O7 append", [&history, &policyFrame, k] { history.append(policyFrame(k)); });
        }
        const std::vector<std::uint32_t> indices = {0, 5, 10, 15, 20, 25, 30, 35};
        RIN_CHECK_MSG(vectorExact(history.sample(), histSampleExpected(indices, 32, 18)),
                      "O7 delay=1 抽样下标 {0,5,…,35}");
    }

    // --- 8h) 非默认小配置（6×5 帧、history 10、sample 3 skip 3）---
    // idx[i] = 10 − (2−i)·3 − 1 = {3, 6, 9}；pad = 7 → {first, first, f2}。
    {
        PolicyDepthConfig cfg;
        cfg.gridWidth = 8;
        cfg.gridHeight = 8;
        cfg.cropUp = 2;
        cfg.cropDown = 1;
        cfg.cropLeft = 1;
        cfg.cropRight = 1;
        cfg.historyLength = 10;
        cfg.sampleCount = 3;
        cfg.sampleSkip = 3;
        cfg.sampleDelay = 0;
        PolicyDepthHistory history(cfg);
        RIN_CHECK_EQ(history.frameWidth(), std::uint32_t{6});
        RIN_CHECK_EQ(history.frameHeight(), std::uint32_t{5});
        const auto frame = [](std::uint32_t k) {
            return makeDepth(
                6, 5, [k](std::uint32_t x, std::uint32_t y) { return histPixel(k, x, y, 6); });
        };
        for (std::uint32_t k = 0; k < 3; ++k) {
            tryCall("O7 append", [&history, &frame, k] { history.append(frame(k)); });
        }
        const std::vector<std::uint32_t> indices = {0, 0, 2};
        RIN_CHECK_MSG(vectorExact(history.sample(), histSampleExpected(indices, 6, 5)),
                      "O7 非默认小配置欠帧首帧填充 golden");
    }

    // --- 8i) padding stride 帧按逻辑内容入环（对抗用例）---
    {
        PolicyDepthHistory history(PolicyDepthConfig{});
        std::vector<float> values(32 * 18);
        std::uint32_t seed = 808u;
        for (float& v : values) {
            v = static_cast<float>(0.25 + 2.0 * lcg01(seed));
        }
        const DepthFrameF32 padded = makeDepthPadded(
            32, 18, 40, [&](std::uint32_t x, std::uint32_t y) { return values[y * 32 + x]; },
            -12345.0f);
        RIN_CHECK_MSG(padded.valid(), "前置：padding 测试帧构造有效");
        tryCall("O7 append", [&] { history.append(padded); });
        std::vector<float> allPlanes;
        allPlanes.reserve(8 * values.size());
        for (std::size_t plane = 0; plane < 8; ++plane) {
            allPlanes.insert(allPlanes.end(), values.begin(), values.end());
        }
        RIN_CHECK_MSG(vectorExact(history.sample(), allPlanes),
                      "O7 padding stride 帧 append 后 sample 为逻辑内容（逐平面）");
    }

    // --- 8j) 尺寸不符 / 无效帧 append 抛 ---
    {
        PolicyDepthHistory history(PolicyDepthConfig{});
        RIN_CHECK(throwsAs<std::invalid_argument>([&] {
            history.append(makeDepth(31, 18, [](std::uint32_t, std::uint32_t) { return 1.0f; }));
        }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] {
            history.append(makeDepth(32, 17, [](std::uint32_t, std::uint32_t) { return 1.0f; }));
        }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { history.append(DepthFrameF32{}); }));
        // 拒绝后历史仍为空（失败不产生部分状态）。
        RIN_CHECK(history.empty());
    }

    return rin_test::exitStatus();
}
