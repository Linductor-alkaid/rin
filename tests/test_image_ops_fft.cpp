// M4-06 FFT 滤波节点族（fft_lowpass / fft_highpass / fft_bandpass）golden 数值
// 测试（独立验证）：include/rin/image_ops.hpp、src/core/image_ops.cpp（数值语义以
// docs/design/image_workflow_design.md §7 2026-09-29 M4-06 小节冻结公式为唯一
// 判据；本文件的 golden 期望全部由冻结公式独立手推/测试内自写 double 参考实现
// 推导，不参考实现代码）。
//
// 被测面与范围（对应设计文档 §7 FFT 族 golden 项）：
// 1) 工厂 makeDefaultImageNode：三个 FFT 类型返回实现且 descriptor().typeId 与
//    请求一致、端口签名与 §6 目录一致（Gray8→Gray8）；未知 typeId 抛
//    std::invalid_argument 且错误消息冻结为无后缀新口径
//    "no core implementation for node type 'X'"（M4 目录类型已全部实现，M4-04/05
//    时代的 "(M4-06 FFT operators are not implemented yet)" 后缀见证已过时）；
// 2) 构造期拒绝全分支（运行期直接构造 NodeInstance）：cutoff/lowCut/highCut 参数
//    缺失（声明缺失或实例未赋值且无默认可读）、种类错位（Real 位赋
//    Integer/string/Boolean）、非有限（NaN/±inf）、越界 [0,1]（−0.1/1.1）构造抛
//    std::invalid_argument；bandpass lowCut ≥ highCut（含相等；空通带为参数
//    矛盾，2026-09-29 裁决口径——通带 (lowCut, highCut] 须非空）构造抛；
//    cutoff = 0 / 1 边界合法；默认参数生效
//    （不赋值 → 0.2 / 0.2+0.6，以解析可判别的单频滤除/保留行为锁定）；
// 3) 正弦注入 → 单频滤除/保留：848×480（填充路径 pad 1024×512）与 512×512
//    （原生路径不填充）各覆盖低通/高通/带通，注入频率取填充栅格精确 bin
//    （f = 1/8 = 128/1024 = 64/512 周期/像素，k 整数），另含二维对角单频
//    （fx = fy = 1/16）。原生路径谱线精确落 bin、无截断泄漏 → 解析期望逐像素
//    断言（低通滤除 → 均值；高通/带通保留 → 输入−均值；带通全阻 → 0）；填充
//    路径零填充矩形截断的谱扩展是冻结语义的一部分（§7 边界语义披露：理想掩膜
//    在原图边界产生 Gibbs 振铃），解析常数期望不存在 → 以测试内自写 double
//    参考实现逐像素 ±1 字节锁定；
// 4) 填充/裁剪往返：cutoff ≥ √0.5（归一化频率上界 √(0.5²+0.5²)）低通恒等
//    （±1）——848×480 非平凡填充、512×512 原生、5×3（pad 8×4）、1×5（pad 2×8，
//    下界 2）；cutoff = √0.5 边界（角点 bin ρ = √0.5 恰被 ≤ 保留）；
// 5) 掩膜代数：同 cutoff 低通 + 高通 ≡ 掩膜补（double 域恒等由参考自检 2
//    锁定）→ 字节域"两输出相加与原图差 ≤ 1"用带限内容断言（宽带内容上
//    q_sat 对 HP = 原图−LP 的大幅负值钳制会破坏和恒等——饱和物理，注释附
//    实测数字）；bandpass(lo,hi) ≡ 低通(highCut) − 低通(lowCut) 掩膜差
//    → 带通输出与 q_sat(参考未量化 lowpass(hi) − lowpass(lo)) 逐像素差 ≤ 1
//    （量化后的 8bit 差按"先差后量化"饱和语义处理）；
// 6) DC 语义：原生路径常值图低通恒等（精确）、高通全黑（精确 0，含 cutoff=0
//    仅移除 DC）、带通全黑；斜坡图 cutoff=0 低通 = 均值（精确 floor(127.5+0.5)）、
//    cutoff=0 高通 = 斜坡 − 均值（逐像素解析 + 饱和）；填充路径 cutoff=0 掩膜
//    仅保留单一 DC bin → 输出 = Σ/(padW·padH) 全图均匀（848×480 常值 128 →
//    52101120/524288 = 99.375 → 99，精确；cutoff=0 高通 = 128 − 99.375 = 28.625
//    → 29 均匀，±1）；
// 7) 归一化频率跨尺寸不变：同一 f = 1/8 与 cutoff = 0.1 在 64×64 / 128×128
//    （原生）解析滤除、848×480（填充）参考滤除——cutoff 按归一化频率判定而非
//    绝对 bin（若实现误用 bin 下标，填充路径 bin 128 与原生 bin 64 行为将分裂，
//    参考逐像素比对可判别）；
// 8) 防御路径：输入数量 0/2、无效图、Rgba8 喂 Gray8 声明抛 std::invalid_argument；
// 9) stride padding 输入（行尾 padding 不影响结果、与紧凑输入输出逐字节一致）、
//    输出紧凑（stride == 宽×1）、不共享源像素（共享指针与数据指针核对）；
// 10) 边界推论抽查：cutoff = 1 低通恒等；掩膜边界 ≤/>/== 语义（cutoff = 0.125
//     恰在注入谱线上：低通保留（ρ ≤ c）、高通移除（ρ > c 不含等号）、
//     bandpass(lowCut=0.125, ·) 严格下界不含 ρ == lowCut、bandpass(·, highCut
//     =0.125) 闭上界含 ρ == highCut）；lowCut == highCut 空通带构造期拒绝
//     （2026-09-29 裁决：掩膜差公式为准，原薄壳推论系冻结文档笔误并已从
//     §7 删除——退化输入显式失败优于静默黑帧，错误消息含 "empty passband"；
//     原"薄壳保留 ρ == highCut"数值用例随裁决移除）。
//
// golden 独立性说明：本文件内自含一份 double 精度参考实现（refFftRaw：自写
// 迭代 radix-2 复数 FFT + 冻结归一化频率掩膜 + 1/(padW·padH) + floor(v+0.5) 饱和
// 量化），按 §7 冻结语义独立重写，不 include 实现头、不链接 kissfft；参考自检
// （c=1 往返恒等到 1e-6、低通+高通掩膜补恒等、c=0 单 bin 均值）先行验证参考
// 本身。解析期望（均值迁移/单 bin）均附手推过程注释。实现侧为 pinned
// kissfft float 中间 + double 掩膜判定（§7 精度条款：往返误差 0..255 域约 ±0.3），
// 全部数值断言以 ±1 字节容差锁定（DC 单 bin 用例按推导给精确断言）。
//
// DOD-02 适用性说明：本契约面全部为单线程纯逻辑（节点构造/apply/图编译求值均
// 顺序调用，无任务提交/队列/取消/超时/shutdown 语义，无跨上下文共享状态），
// 并发矩阵不适用（写法参照 test_image_ops_histogram.cpp 文件头）；并发行为归
// M4-07 引擎与 M5-08 契约套件。
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

#include <rin/image_node.hpp>
#include <rin/image_ops.hpp>
#include <rin/image_types.hpp>
#include <rin/node_graph.hpp>
#include <rin/workflow_types.hpp>

namespace {

using rin::Connection;
using rin::IImageNode;
using rin::ImageNodeFactory;
using rin::ImageU8;
using rin::NodeCatalog;
using rin::NodeDescriptor;
using rin::NodeGraph;
using rin::NodeId;
using rin::NodeInstance;
using rin::ParamAssignment;
using rin::ParamDescriptor;
using rin::ParamKind;
using rin::ParamValue;
using rin::PortDirection;
using rin::PortRef;
using rin::PortType;
using rin::ValidationIssueKind;
using rin::WorkflowGraph;

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

// --- 目录构造（schema 照抄 docs/design/image_workflow_design.md §6 表格）-------

NodeDescriptor makeSourceGrayDescriptor() {
    NodeDescriptor d;
    d.typeId = "source";
    d.displayName = "相机源";
    d.outputs = {PortType::Gray8};
    return d;
}

// §6：Real 参数声明（id: kind Real 默认 default [min,max]）。
ParamDescriptor realParam(const std::string& id, double defaultValue, double min, double max) {
    ParamDescriptor p;
    p.id = id;
    p.label = id;
    p.kind = ParamKind::Real;
    p.defaultValue = defaultValue;
    p.hasRange = true;
    p.minValue = min;
    p.maxValue = max;
    return p;
}

// §6：fft_lowpass FFT 低通 Gray8→Gray8，cutoff: Real 0.2 [0,1]。
NodeDescriptor makeFftLowpassDescriptor() {
    NodeDescriptor d;
    d.typeId = "fft_lowpass";
    d.displayName = "FFT 低通";
    d.inputs = {PortType::Gray8};
    d.outputs = {PortType::Gray8};
    d.params.push_back(realParam("cutoff", 0.2, 0.0, 1.0));
    return d;
}

// §6：fft_highpass FFT 高通 Gray8→Gray8，cutoff: Real 0.2 [0,1]。
NodeDescriptor makeFftHighpassDescriptor() {
    NodeDescriptor d;
    d.typeId = "fft_highpass";
    d.displayName = "FFT 高通";
    d.inputs = {PortType::Gray8};
    d.outputs = {PortType::Gray8};
    d.params.push_back(realParam("cutoff", 0.2, 0.0, 1.0));
    return d;
}

// §6：fft_bandpass FFT 带通 Gray8→Gray8，lowCut: Real 0.2 [0,1]；highCut: Real 0.6 [0,1]。
NodeDescriptor makeFftBandpassDescriptor() {
    NodeDescriptor d;
    d.typeId = "fft_bandpass";
    d.displayName = "FFT 带通";
    d.inputs = {PortType::Gray8};
    d.outputs = {PortType::Gray8};
    d.params.push_back(realParam("lowCut", 0.2, 0.0, 1.0));
    d.params.push_back(realParam("highCut", 0.6, 0.0, 1.0));
    return d;
}

// --- 实例构造助手 ------------------------------------------------------------

ParamAssignment assign(const std::string& paramId, ParamValue value) {
    return ParamAssignment{paramId, std::move(value)};
}

NodeInstance makeInstance(NodeId id, const std::string& typeId) {
    NodeInstance instance;
    instance.id = id;
    instance.typeId = typeId;
    return instance;
}

std::unique_ptr<IImageNode> makeNode(const NodeDescriptor& descriptor, NodeId id,
                                     std::vector<ParamAssignment> params = {}) {
    NodeInstance instance = makeInstance(id, descriptor.typeId);
    instance.params = std::move(params);
    return rin::makeDefaultImageNode(descriptor, instance);
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

// 工厂异常消息捕获（空串 = 未抛）。
std::string factoryError(const NodeDescriptor& descriptor, const NodeInstance& instance) {
    try {
        (void)rin::makeDefaultImageNode(descriptor, instance);
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
    return {};
}

// --- 图像构造与核对助手 ------------------------------------------------------

using GrayFn = std::function<std::uint8_t(std::uint32_t, std::uint32_t)>;

// floor(v+0.5)（round-half-up）后饱和 [0,255]（§7 量化纪律）。
std::uint8_t saturateRound(double value) {
    const double shifted = std::floor(value + 0.5);
    if (shifted <= 0.0) {
        return 0;
    }
    if (shifted >= 255.0) {
        return 255;
    }
    return static_cast<std::uint8_t>(shifted);
}

// 构造紧凑（stride=0 → 宽）或行尾 padding（stride=rowBytes）的 Gray8 图像；
// padding 区域填充 padByte（默认 0xEE，用于断言不泄漏进输出）。
ImageU8 makeGray(std::uint32_t width, std::uint32_t height, const GrayFn& pixel,
                 std::uint32_t rowBytes = 0, std::uint8_t padByte = 0xEE) {
    const std::uint32_t stride = rowBytes == 0 ? width : rowBytes;
    std::vector<std::uint8_t> buffer(std::size_t{stride} * height, padByte);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            buffer[std::size_t{stride} * y + x] = pixel(x, y);
        }
    }
    auto shared = std::make_shared<const std::vector<std::uint8_t>>(std::move(buffer));
    return ImageU8::wrap(PortType::Gray8, width, height, stride, std::move(shared));
}

// 确定性字节伪随机（LCG，测试内自含；与被测实现无共享状态）。
class ByteLcg {
public:
    explicit ByteLcg(std::uint32_t seed) : state_(seed) {}
    std::uint8_t next() {
        state_ = state_ * 1664525u + 1013904223u;
        return static_cast<std::uint8_t>((state_ >> 16) & 0xFFu);
    }

private:
    std::uint32_t state_;
};

ImageU8 makeNoise(std::uint32_t width, std::uint32_t height, std::uint32_t seed) {
    ByteLcg lcg{seed};
    return makeGray(width, height, [&lcg](std::uint32_t, std::uint32_t) { return lcg.next(); });
}

// 128 + A·sin(2π(fx·x + fy·y) + φ)（逐像素 round-half-up 饱和量化为 Gray8）。
// fx/fy 为每像素周期数（归一化频率；选择填充栅格精确 bin：fx = k/padW）。
ImageU8 makeSine(std::uint32_t width, std::uint32_t height, double fx, double fy, double amplitude,
                 double phase = 0.0) {
    return makeGray(width, height, [&](std::uint32_t x, std::uint32_t y) {
        const double value = 128.0 +
                             amplitude * std::sin(kTwoPi * (fx * static_cast<double>(x) +
                                                            fy * static_cast<double>(y)) +
                                                 phase);
        return saturateRound(value);
    });
}

// 逐像素字节期望向量（由 GrayFn 生成，紧凑行主序）。
std::vector<std::uint8_t> expectedBytes(std::uint32_t width, std::uint32_t height,
                                        const GrayFn& pixel) {
    std::vector<std::uint8_t> expected;
    expected.reserve(std::size_t{width} * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            expected.push_back(pixel(x, y));
        }
    }
    return expected;
}

std::vector<std::uint8_t> imageBytes(const ImageU8& image) {
    return *image.pixels();
}

// 灰度均值（量化输入的精确整数和 / 像素数）。
double grayMean(const ImageU8& image) {
    std::uint64_t sum = 0;
    for (std::uint32_t y = 0; y < image.height(); ++y) {
        const std::uint8_t* row = image.row(y);
        for (std::uint32_t x = 0; x < image.width(); ++x) {
            sum += row[x];
        }
    }
    return static_cast<double>(sum) /
           (static_cast<double>(image.width()) * static_cast<double>(image.height()));
}

// 逐字节最大偏差（尺寸不符返回 −1）。
int maxByteDiff(const std::vector<std::uint8_t>& got, const std::vector<std::uint8_t>& expected) {
    if (got.size() != expected.size()) {
        return -1;
    }
    int maxDiff = 0;
    for (std::size_t i = 0; i < got.size(); ++i) {
        const int diff = static_cast<int>(got[i]) - static_cast<int>(expected[i]);
        maxDiff = std::max(maxDiff, diff < 0 ? -diff : diff);
    }
    return maxDiff;
}

// 应用节点并核对输出契约（§7）：数量 1、有效 Gray8、尺寸与输入一致、输出紧凑
// （stride == 宽×1）、新缓冲（共享指针与数据指针均与输入不同）。返回像素字节；
// 契约违反返回空 vector（由调用方 RIN_CHECK 判空）。
std::vector<std::uint8_t> applyGray(const IImageNode& node, const ImageU8& input) {
    const std::vector<ImageU8> output = node.apply({input});
    if (output.size() != 1) {
        return {};
    }
    const ImageU8& image = output[0];
    if (!image.valid() || image.format() != PortType::Gray8) {
        return {};
    }
    if (image.width() != input.width() || image.height() != input.height()) {
        return {};
    }
    if (image.stride() != image.width()) {
        return {};  // 紧凑：Gray8 stride = 宽×1。
    }
    if (image.pixels() == input.pixels()) {
        return {};  // 不共享源像素（缓冲对象不同）。
    }
    if (image.pixels()->data() == input.pixels()->data()) {
        return {};  // 数据指针不同（非同缓冲别名）。
    }
    return *image.pixels();
}

// applyGray + 逐字节 ±tolerance 比对。
bool applyMatches(const IImageNode& node, const ImageU8& input,
                  const std::vector<std::uint8_t>& expected, int tolerance) {
    const std::vector<std::uint8_t> got = applyGray(node, input);
    if (got.empty()) {
        return false;
    }
    const int diff = maxByteDiff(got, expected);
    return diff >= 0 && diff <= tolerance;
}

// --- 独立 double 参考实现（按设计文档 §7 M4-06 冻结语义重写，不 include 实现
// --- 头、不接触 kissfft）：自写迭代 radix-2 复数 FFT（padW/padH 均为 2 幂），
// --- 行-列分离全复数 2D DFT；实输入 + 关于 (u,v)→(−u,−v) 对称的掩膜下与
// --- kissfftndr 半谱路径数学等价（半谱 bin (u,v) 隐含共轭对 (padW−u, padH−v)，
// --- 冻结掩膜按 ρ 对称 → 两路径逐 bin 掩膜值一致）。

enum class FftKind { Lowpass, Highpass, Bandpass };

struct Cx {
    double re = 0.0;
    double im = 0.0;
};

// 未归一化 radix-2 FFT（n 为 2 幂）；inverse=true 用 +2π 旋转因子（逆变换不除 n，
// 归一化由冻结语义的显式 1/(padW·padH) 承担）。
void fftInPlace(std::vector<Cx>& a, bool inverse) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; (j & bit) != 0u; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle =
            (inverse ? 2.0 : -2.0) * kPi / static_cast<double>(len);
        const Cx wlen{std::cos(angle), std::sin(angle)};
        for (std::size_t base = 0; base < n; base += len) {
            Cx w{1.0, 0.0};
            for (std::size_t k = 0; k < len / 2; ++k) {
                const Cx u = a[base + k];
                const Cx v = a[base + k + len / 2];
                const Cx product{v.re * w.re - v.im * w.im, v.re * w.im + v.im * w.re};
                a[base + k] = Cx{u.re + product.re, u.im + product.im};
                a[base + k + len / 2] = Cx{u.re - product.re, u.im - product.im};
                const double nextRe = w.re * wlen.re - w.im * wlen.im;
                w.im = w.re * wlen.im + w.im * wlen.re;
                w.re = nextRe;
            }
        }
    }
}

// 行-列分离 2D DFT（未归一化；w/h 均为 2 幂）。
void dft2InPlace(std::vector<Cx>& plane, std::uint32_t w, std::uint32_t h, bool inverse) {
    std::vector<Cx> line(w);
    for (std::uint32_t y = 0; y < h; ++y) {
        std::copy_n(plane.begin() + std::size_t{y} * w, w, line.begin());
        fftInPlace(line, inverse);
        std::copy(line.begin(), line.end(), plane.begin() + std::size_t{y} * w);
    }
    line.assign(h, Cx{});
    for (std::uint32_t x = 0; x < w; ++x) {
        for (std::uint32_t y = 0; y < h; ++y) {
            line[y] = plane[std::size_t{y} * w + x];
        }
        fftInPlace(line, inverse);
        for (std::uint32_t y = 0; y < h; ++y) {
            plane[std::size_t{y} * w + x] = line[y];
        }
    }
}

// 各维向上取 2 幂且下界 2（§7：1 像素宽/高同样填到 2）。
std::uint32_t padPow2(std::uint32_t value) {
    std::uint32_t p = 2;
    while (p < value) {
        p <<= 1;
    }
    return p;
}

// 冻结掩膜（理想锐截止，归一化频率）：低通保留 ρ ≤ cutoff；高通保留 ρ > cutoff
// （DC 恒移除）；带通保留 lowCut < ρ ≤ highCut（掩膜差；构造期已保证
// lowCut < highCut——2026-09-29 裁决 lowCut ≥ highCut 一律构造期拒绝，空通带
// 不可构造，参考无需覆盖 lo == hi）。全谱扩展口径：fx 对 u > padW/2 取负折返
// （掩膜关于 (u,v)→(−u,−v) 对称，ρ 不变）。
bool maskKeeps(FftKind kind, double cutoff, double lowCut, double highCut, double fx, double fy) {
    const double rho = std::sqrt(fx * fx + fy * fy);
    switch (kind) {
        case FftKind::Lowpass:
            return rho <= cutoff;
        case FftKind::Highpass:
            return rho > cutoff;
        case FftKind::Bandpass:
            return rho > lowCut && rho <= highCut;
    }
    return false;
}

// 参考主入口：返回裁回原尺寸、已乘 1/(padW·padH) 的未量化 double 图。
std::vector<double> refFftRaw(FftKind kind, double cutoff, double lowCut, double highCut,
                              const ImageU8& input) {
    const std::uint32_t w = input.width();
    const std::uint32_t h = input.height();
    const std::uint32_t padW = padPow2(w);
    const std::uint32_t padH = padPow2(h);
    std::vector<Cx> plane(std::size_t{padW} * padH);
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint8_t* row = input.row(y);
        for (std::uint32_t x = 0; x < w; ++x) {
            plane[std::size_t{y} * padW + x] = Cx{static_cast<double>(row[x]), 0.0};
        }
    }
    dft2InPlace(plane, padW, padH, false);
    for (std::uint32_t v = 0; v < padH; ++v) {
        const double fy =
            v <= padH / 2u ? static_cast<double>(v) / padH
                           : (static_cast<double>(v) - padH) / padH;
        for (std::uint32_t u = 0; u < padW; ++u) {
            const double fx =
                u <= padW / 2u ? static_cast<double>(u) / padW
                               : (static_cast<double>(u) - padW) / padW;
            if (!maskKeeps(kind, cutoff, lowCut, highCut, fx, fy)) {
                plane[std::size_t{v} * padW + u] = Cx{0.0, 0.0};  // 阻带精确置零。
            }
        }
    }
    dft2InPlace(plane, padW, padH, true);
    const double norm = 1.0 / (static_cast<double>(padW) * static_cast<double>(padH));
    std::vector<double> out(std::size_t{w} * h);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            out[std::size_t{y} * w + x] = plane[std::size_t{y} * padW + x].re * norm;
        }
    }
    return out;
}

}  // namespace

namespace {

// 参考量化输出（floor(v+0.5) 饱和）。
std::vector<std::uint8_t> quantizeSat(const std::vector<double>& raw) {
    std::vector<std::uint8_t> out;
    out.reserve(raw.size());
    for (const double value : raw) {
        out.push_back(saturateRound(value));
    }
    return out;
}

std::vector<std::uint8_t> refFftBytes(FftKind kind, double cutoff, double lowCut, double highCut,
                                      const ImageU8& input) {
    return quantizeSat(refFftRaw(kind, cutoff, lowCut, highCut, input));
}

// 解析期望：输出 ≈ 输入 − 均值（高通/带通保留谱线、移除均匀 DC 的原生路径
// 形态；被移除的低带量化噪声残差 σ ≪ 1，±1 字节容差覆盖）。
std::vector<std::uint8_t> expectedMinusMean(const ImageU8& input) {
    const double mean = grayMean(input);
    std::vector<std::uint8_t> expected;
    expected.reserve(std::size_t{input.width()} * input.height());
    for (std::uint32_t y = 0; y < input.height(); ++y) {
        const std::uint8_t* row = input.row(y);
        for (std::uint32_t x = 0; x < input.width(); ++x) {
            expected.push_back(saturateRound(static_cast<double>(row[x]) - mean));
        }
    }
    return expected;
}

Connection conn(NodeId from, std::uint32_t fromPort, NodeId to, std::uint32_t toPort) {
    Connection connection;
    connection.from = PortRef{from, PortDirection::Output, fromPort};
    connection.to = PortRef{to, PortDirection::Input, toPort};
    return connection;
}

ImageNodeFactory defaultFactory() {
    return [](const NodeDescriptor& descriptor, const NodeInstance& instance) {
        return rin::makeDefaultImageNode(descriptor, instance);
    };
}

}  // namespace

int main() {
    const NodeDescriptor sourceGrayDescriptor = makeSourceGrayDescriptor();
    const NodeDescriptor lowpassDescriptor = makeFftLowpassDescriptor();
    const NodeDescriptor highpassDescriptor = makeFftHighpassDescriptor();
    const NodeDescriptor bandpassDescriptor = makeFftBandpassDescriptor();
    const double sqrtHalf = std::sqrt(0.5);  // 与实现同为 IEEE double sqrt(0.5)。

    // ===================================================================
    // 0) 参考实现自检（golden 工具自身的正确性先行验证）
    // ===================================================================
    {
        const ImageU8 noise = makeNoise(16, 12, 0xC0FFEEu);
        const std::vector<std::uint8_t> original = imageBytes(noise);
        // 自检 1：c=1 低通 = 全通 → FFT 往返 ×1/N 精确恢复输入（double 域 1e-6）。
        const std::vector<double> identity = refFftRaw(FftKind::Lowpass, 1.0, 0.0, 0.0, noise);
        double identityError = 0.0;
        for (std::size_t i = 0; i < identity.size(); ++i) {
            identityError = std::max(identityError, std::abs(identity[i] - static_cast<double>(original[i])));
        }
        RIN_CHECK_MSG(identityError <= 1e-6, "参考自检：c=1 低通 FFT 往返恒等（≤1e-6）");
        // 自检 2：同 cutoff 低通 + 高通掩膜互补 → 未量化输出之和 = 输入。
        const std::vector<double> low =
            refFftRaw(FftKind::Lowpass, 0.3, 0.0, 0.0, noise);
        const std::vector<double> high =
            refFftRaw(FftKind::Highpass, 0.3, 0.0, 0.0, noise);
        double complementError = 0.0;
        for (std::size_t i = 0; i < low.size(); ++i) {
            complementError = std::max(complementError, std::abs(low[i] + high[i] - static_cast<double>(original[i])));
        }
        RIN_CHECK_MSG(complementError <= 1e-6, "参考自检：低通+高通掩膜补恒等（≤1e-6）");
        // 自检 3：c=0 低通仅保留 DC bin → 全图均匀 = Σ/(padW·padH)（填充平面
        // 均值；仅原生尺寸下等于图像均值——16×12 图像 pad 16×16 时二者不同）。
        const std::vector<double> dcOnly = refFftRaw(FftKind::Lowpass, 0.0, 0.0, 0.0, noise);
        std::uint64_t pixelSum = 0;
        for (std::uint32_t y = 0; y < noise.height(); ++y) {
            const std::uint8_t* row = noise.row(y);
            for (std::uint32_t x = 0; x < noise.width(); ++x) {
                pixelSum += row[x];
            }
        }
        const double paddedPlaneMean =
            static_cast<double>(pixelSum) /
            static_cast<double>(padPow2(noise.width()) * padPow2(noise.height()));
        double dcSpread = 0.0;
        for (const double value : dcOnly) {
            dcSpread = std::max(dcSpread, std::abs(value - paddedPlaneMean));
        }
        RIN_CHECK_MSG(dcSpread <= 1e-9, "参考自检：c=0 低通单 DC bin = Σ/(padW·padH) 均匀");
    }

    // ===================================================================
    // 1) 工厂 makeDefaultImageNode：三类型实现 + 未知类型消息冻结（新口径）
    // ===================================================================
    {
        const std::unique_ptr<IImageNode> lowpass = makeNode(lowpassDescriptor, 1);
        RIN_CHECK(lowpass != nullptr);
        RIN_CHECK(lowpass != nullptr && lowpass->descriptor().typeId == "fft_lowpass");
        RIN_CHECK(lowpass != nullptr &&
                  lowpass->descriptor().inputs == std::vector<PortType>{PortType::Gray8});
        RIN_CHECK(lowpass != nullptr &&
                  lowpass->descriptor().outputs == std::vector<PortType>{PortType::Gray8});

        const std::unique_ptr<IImageNode> highpass = makeNode(highpassDescriptor, 2);
        RIN_CHECK(highpass != nullptr);
        RIN_CHECK(highpass != nullptr && highpass->descriptor().typeId == "fft_highpass");
        RIN_CHECK(highpass != nullptr &&
                  highpass->descriptor().inputs == std::vector<PortType>{PortType::Gray8});
        RIN_CHECK(highpass != nullptr &&
                  highpass->descriptor().outputs == std::vector<PortType>{PortType::Gray8});

        const std::unique_ptr<IImageNode> bandpass = makeNode(bandpassDescriptor, 3);
        RIN_CHECK(bandpass != nullptr);
        RIN_CHECK(bandpass != nullptr && bandpass->descriptor().typeId == "fft_bandpass");
        RIN_CHECK(bandpass != nullptr &&
                  bandpass->descriptor().inputs == std::vector<PortType>{PortType::Gray8});
        RIN_CHECK(bandpass != nullptr &&
                  bandpass->descriptor().outputs == std::vector<PortType>{PortType::Gray8});

        // 未知 typeId：抛 invalid_argument，错误消息冻结为无后缀新口径
        // "no core implementation for node type 'X'"（M4 目录类型已全部实现，
        // 旧 "(M4-06 FFT operators are not implemented yet)" 后缀见证已过时）。
        for (const char* ghostId : {"no_such_operator", "fft_ultra_lowpass"}) {
            NodeDescriptor ghost;
            ghost.typeId = ghostId;
            const std::string message = factoryError(ghost, makeInstance(1, ghostId));
            RIN_CHECK_MSG(!message.empty(), std::string("未知类型应抛 invalid_argument：") + ghostId);
            RIN_CHECK_MSG(message == std::string("no core implementation for node type '") +
                                        ghostId + "'",
                          std::string("未知类型错误消息冻结（无后缀）：") + ghostId);
        }
    }

    // ===================================================================
    // 2) 构造期拒绝全分支（运行期直接构造 NodeInstance）
    // ===================================================================
    {
        // --- 2a) fft_lowpass / fft_highpass：cutoff 缺失/种类错位/非有限/越界 ---
        for (const NodeDescriptor& descriptor :
             {lowpassDescriptor, highpassDescriptor}) {
            const std::string& typeId = descriptor.typeId;
            // 参数声明缺失（目录项无 cutoff）。
            {
                NodeDescriptor missing = descriptor;
                missing.params.clear();
                RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] {
                    (void)makeNode(missing, 1);
                }), typeId + ": cutoff 声明缺失构造拒绝");
            }
            // 种类错位（Real 位赋 Integer / string / Boolean）。
            RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] {
                (void)makeNode(descriptor, 1, {assign("cutoff", std::int64_t{0})});
            }), typeId + ": cutoff 种类错位（Integer）构造拒绝");
            RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] {
                (void)makeNode(descriptor, 1, {assign("cutoff", std::string("0.2"))});
            }), typeId + ": cutoff 种类错位（string）构造拒绝");
            RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] {
                (void)makeNode(descriptor, 1, {assign("cutoff", true)});
            }), typeId + ": cutoff 种类错位（Boolean）构造拒绝");
            // 非有限。
            const double nanValue = std::numeric_limits<double>::quiet_NaN();
            const double infValue = std::numeric_limits<double>::infinity();
            RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] {
                (void)makeNode(descriptor, 1, {assign("cutoff", nanValue)});
            }), typeId + ": cutoff NaN 构造拒绝");
            RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] {
                (void)makeNode(descriptor, 1, {assign("cutoff", infValue)});
            }), typeId + ": cutoff +inf 构造拒绝");
            RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] {
                (void)makeNode(descriptor, 1, {assign("cutoff", -infValue)});
            }), typeId + ": cutoff −inf 构造拒绝");
            // 越界 [0,1]。
            RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] {
                (void)makeNode(descriptor, 1, {assign("cutoff", -0.1)});
            }), typeId + ": cutoff −0.1 构造拒绝");
            RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] {
                (void)makeNode(descriptor, 1, {assign("cutoff", 1.1)});
            }), typeId + ": cutoff 1.1 构造拒绝");
            // 边界合法：0 与 1 可构造。
            RIN_CHECK_MSG(makeNode(descriptor, 1, {assign("cutoff", 0.0)}) != nullptr,
                          typeId + ": cutoff=0 合法构造");
            RIN_CHECK_MSG(makeNode(descriptor, 1, {assign("cutoff", 1.0)}) != nullptr,
                          typeId + ": cutoff=1 合法构造");
        }

        // --- 2b) fft_bandpass：lowCut/highCut 全分支 + lowCut > highCut 矛盾 ---
        {
            // lowCut 声明缺失 / highCut 声明缺失。
            {
                NodeDescriptor missingLow = bandpassDescriptor;
                missingLow.params.pop_back();
                RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] {
                    (void)makeNode(missingLow, 1);
                }), "bandpass: lowCut 声明缺失构造拒绝");
            }
            {
                NodeDescriptor missingHigh = bandpassDescriptor;
                missingHigh.params.erase(missingHigh.params.begin());
                RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] {
                    (void)makeNode(missingHigh, 1);
                }), "bandpass: highCut 声明缺失构造拒绝");
            }
            // 种类错位。
            RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] {
                (void)makeNode(bandpassDescriptor, 1,
                               {assign("lowCut", std::int64_t{0}), assign("highCut", 0.6)});
            }), "bandpass: lowCut 种类错位（Integer）构造拒绝");
            RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] {
                (void)makeNode(bandpassDescriptor, 1,
                               {assign("lowCut", 0.2), assign("highCut", std::string("0.6"))});
            }), "bandpass: highCut 种类错位（string）构造拒绝");
            // 非有限。
            const double nanValue = std::numeric_limits<double>::quiet_NaN();
            const double infValue = std::numeric_limits<double>::infinity();
            RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] {
                (void)makeNode(bandpassDescriptor, 1,
                               {assign("lowCut", nanValue), assign("highCut", 0.6)});
            }), "bandpass: lowCut NaN 构造拒绝");
            RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] {
                (void)makeNode(bandpassDescriptor, 1,
                               {assign("lowCut", 0.2), assign("highCut", infValue)});
            }), "bandpass: highCut +inf 构造拒绝");
            // 越界。
            RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] {
                (void)makeNode(bandpassDescriptor, 1,
                               {assign("lowCut", -0.1), assign("highCut", 0.6)});
            }), "bandpass: lowCut −0.1 构造拒绝");
            RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] {
                (void)makeNode(bandpassDescriptor, 1,
                               {assign("lowCut", 0.2), assign("highCut", 1.1)});
            }), "bandpass: highCut 1.1 构造拒绝");
            // lowCut > highCut：空通带参数矛盾（同 conv_kernel kernel 长度先例）。
            RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] {
                (void)makeNode(bandpassDescriptor, 1,
                               {assign("lowCut", 0.6), assign("highCut", 0.2)});
            }), "bandpass: lowCut > highCut 构造拒绝");
            RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] {
                (void)makeNode(bandpassDescriptor, 1,
                               {assign("lowCut", 0.3), assign("highCut", 0.2)});
            }), "bandpass: lowCut > highCut（0.3/0.2）构造拒绝");
            // lowCut == highCut：同为空通带（通带 (lowCut, highCut] 须非空），
            // 构造期拒绝（2026-09-29 裁决：掩膜差公式为准，薄壳推论系文档笔误
            // 已从 §7 删除；退化输入显式失败优于静默黑帧）。
            {
                NodeInstance thinShell = makeInstance(1, "fft_bandpass");
                thinShell.params = {assign("lowCut", 0.125), assign("highCut", 0.125)};
                const std::string message = factoryError(bandpassDescriptor, thinShell);
                RIN_CHECK_MSG(!message.empty(),
                              "bandpass: lowCut == highCut 应抛 invalid_argument");
                RIN_CHECK_MSG(message.find("empty passband") != std::string::npos,
                              "bandpass: 空通带错误消息含 empty passband");
            }
            RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] {
                (void)makeNode(bandpassDescriptor, 1,
                               {assign("lowCut", 0.2), assign("highCut", 0.2)});
            }), "bandpass: lowCut == highCut（0.2/0.2）构造拒绝");
            // 不赋值取默认（0.2/0.6）可构造（默认行为数值锁定归 3) 组）。
            RIN_CHECK(makeNode(bandpassDescriptor, 1) != nullptr);
        }
    }

    // ===================================================================
    // 3) 默认参数生效（512×512 原生；f = 160/512 = 0.3125 与 f = 64/512 = 0.125
    //    两个探针把默认 cutoff ∈ (0.125, 0.3125)、lowCut ∈ (0.125, 0.3125)、
    //    highCut ≥ 0.3125 夹逼锁定——与 §6 冻结默认 0.2 / 0.2+0.6 一致）
    // ===================================================================
    {
        const ImageU8 sineFast = makeSine(512, 512, 0.3125, 0.0, 40.0);
        const ImageU8 sineSlow = makeSine(512, 512, 0.125, 0.0, 40.0);
        const std::vector<std::uint8_t> meanOnly(
            std::size_t{512} * 512, saturateRound(grayMean(sineFast)));
        const std::vector<std::uint8_t> minusMeanFast = expectedMinusMean(sineFast);

        // 默认低通 cutoff=0.2：0.3125 ∈ 阻带 → 滤除（解析：均值 ±1）；
        // 0.125 ≤ 0.2 ∈ 通带 → 保留（解析：≈ 输入 ±1）。
        {
            const std::unique_ptr<IImageNode> node = makeNode(lowpassDescriptor, 1);
            const std::vector<std::uint8_t> gotFast = applyGray(*node, sineFast);
            RIN_CHECK_MSG(!gotFast.empty() && maxByteDiff(gotFast, meanOnly) <= 1,
                          "fft_lowpass 默认 cutoff 滤除 f=0.3125（解析均值 ±1）");
            const std::vector<std::uint8_t> gotSlow = applyGray(*node, sineSlow);
            RIN_CHECK_MSG(!gotSlow.empty() &&
                              maxByteDiff(gotSlow, imageBytes(sineSlow)) <= 1,
                          "fft_lowpass 默认 cutoff 保留 f=0.125（解析 ≈ 输入 ±1）");
            RIN_CHECK_MSG(!gotFast.empty() &&
                              maxByteDiff(gotFast, refFftBytes(FftKind::Lowpass, 0.2, 0.0, 0.0,
                                                               sineFast)) <= 1,
                          "fft_lowpass 默认 cutoff 与参考一致（f=0.3125）");
        }
        // 默认高通 cutoff=0.2：0.3125 > 0.2 保留（解析：输入−均值 ±1）；
        // 0.125 ≤ 0.2 移除（高带量化噪声无解析常数 → 参考锁定）。
        {
            const std::unique_ptr<IImageNode> node = makeNode(highpassDescriptor, 1);
            const std::vector<std::uint8_t> gotFast = applyGray(*node, sineFast);
            RIN_CHECK_MSG(!gotFast.empty() && maxByteDiff(gotFast, minusMeanFast) <= 1,
                          "fft_highpass 默认 cutoff 保留 f=0.3125（解析 输入−均值 ±1）");
            const std::vector<std::uint8_t> gotSlow = applyGray(*node, sineSlow);
            RIN_CHECK_MSG(!gotSlow.empty() &&
                              maxByteDiff(gotSlow, refFftBytes(FftKind::Highpass, 0.2, 0.0, 0.0,
                                                               sineSlow)) <= 1,
                          "fft_highpass 默认 cutoff 滤除 f=0.125（参考 ±1）");
        }
        // 默认带通 (0.2, 0.6]：0.3125 ∈ 通带保留（解析：输入−均值 ±1）；
        // 0.125 ≤ lowCut=0.2 移除（通带量化噪声 → 参考锁定）。
        {
            const std::unique_ptr<IImageNode> node = makeNode(bandpassDescriptor, 1);
            const std::vector<std::uint8_t> gotFast = applyGray(*node, sineFast);
            RIN_CHECK_MSG(!gotFast.empty() && maxByteDiff(gotFast, minusMeanFast) <= 1,
                          "fft_bandpass 默认参数保留 f=0.3125（解析 输入−均值 ±1）");
            const std::vector<std::uint8_t> gotSlow = applyGray(*node, sineSlow);
            RIN_CHECK_MSG(!gotSlow.empty() &&
                              maxByteDiff(gotSlow, refFftBytes(FftKind::Bandpass, 0.0, 0.2, 0.6,
                                                               sineSlow)) <= 1,
                          "fft_bandpass 默认参数滤除 f=0.125（参考 ±1）");
        }
    }

    // ===================================================================
    // 4) 正弦注入 → 单频滤除/保留：512×512 原生（解析）与 848×480 填充（参考）
    //    注入 f = 1/8：512 路径 bin 64、1024 路径 bin 128，谱线精确落填充栅格
    //    bin；A = 40。原生路径谱线无截断泄漏 → 解析期望逐像素；填充路径的零
    //    填充矩形截断谱扩展属冻结语义（§7 边界语义披露）→ double 参考锁定。
    // ===================================================================
    {
        const ImageU8 sineNative = makeSine(512, 512, 0.125, 0.0, 40.0);
        const std::vector<std::uint8_t> meanOnlyNative(
            std::size_t{512} * 512, saturateRound(grayMean(sineNative)));
        const std::vector<std::uint8_t> minusMeanNative = expectedMinusMean(sineNative);

        // 低通 cutoff=0.1 < 0.125：谱线（ρ=0.125）被阻带精确置零 → 输出 = 均值
        // ± 通带内量化噪声（通带 ρ≤0.1 仅占全谱 (0.1/0.7071)² ≈ 2% 功率）。
        {
            const std::unique_ptr<IImageNode> node =
                makeNode(lowpassDescriptor, 1, {assign("cutoff", 0.1)});
            const std::vector<std::uint8_t> got = applyGray(*node, sineNative);
            RIN_CHECK_MSG(!got.empty() && maxByteDiff(got, meanOnlyNative) <= 1,
                          "原生 512² 低通 c=0.1 滤除 f=0.125（解析均值 ±1）");
            RIN_CHECK_MSG(!got.empty() && maxByteDiff(got,
                                                      refFftBytes(FftKind::Lowpass, 0.1, 0.0,
                                                                  0.0, sineNative)) <= 1,
                          "原生 512² 低通 c=0.1 与参考一致");
        }
        // 高通 cutoff=0.1：移除均匀 DC、保留谱线 → 输出 = 输入 − 均值 ±1
        // （被移除的低带噪声功率占比 ≈ 3%，σ ≈ 0.05）。
        {
            const std::unique_ptr<IImageNode> node =
                makeNode(highpassDescriptor, 1, {assign("cutoff", 0.1)});
            const std::vector<std::uint8_t> got = applyGray(*node, sineNative);
            RIN_CHECK_MSG(!got.empty() && maxByteDiff(got, minusMeanNative) <= 1,
                          "原生 512² 高通 c=0.1 保留 f=0.125（解析 输入−均值 ±1）");
            RIN_CHECK_MSG(!got.empty() && maxByteDiff(got,
                                                      refFftBytes(FftKind::Highpass, 0.1, 0.0,
                                                                  0.0, sineNative)) <= 1,
                          "原生 512² 高通 c=0.1 与参考一致");
        }
        // 带通 (0.05, 0.6)：DC 移除、谱线保留 → 输入 − 均值 ±1。
        {
            const std::unique_ptr<IImageNode> node =
                makeNode(bandpassDescriptor, 1, {assign("lowCut", 0.05), assign("highCut", 0.6)});
            const std::vector<std::uint8_t> got = applyGray(*node, sineNative);
            RIN_CHECK_MSG(!got.empty() && maxByteDiff(got, minusMeanNative) <= 1,
                          "原生 512² 带通 (0.05,0.6] 保留 f=0.125（解析 ±1）");
            RIN_CHECK_MSG(!got.empty() && maxByteDiff(got,
                                                      refFftBytes(FftKind::Bandpass, 0.0, 0.05,
                                                                  0.6, sineNative)) <= 1,
                          "原生 512² 带通 (0.05,0.6] 与参考一致");
        }
        // 带通 (0.05, 0.1]：DC（ρ=0）与谱线（ρ=0.125）均不在 (0.05, 0.1] → 全阻
        // → 输出 = 通带量化噪声（功率占比 ≈ 0.4%，σ ≈ 0.02）→ 精确 0 ±1。
        {
            const std::unique_ptr<IImageNode> node =
                makeNode(bandpassDescriptor, 1, {assign("lowCut", 0.05), assign("highCut", 0.1)});
            const std::vector<std::uint8_t> zero(std::size_t{512} * 512, std::uint8_t{0});
            const std::vector<std::uint8_t> got = applyGray(*node, sineNative);
            RIN_CHECK_MSG(!got.empty() && maxByteDiff(got, zero) <= 1,
                          "原生 512² 带通 (0.05,0.1] 全阻 f=0.125（解析 0 ±1）");
            RIN_CHECK_MSG(!got.empty() && maxByteDiff(got,
                                                      refFftBytes(FftKind::Bandpass, 0.0, 0.05,
                                                                  0.1, sineNative)) <= 1,
                          "原生 512² 带通 (0.05,0.1] 与参考一致");
        }

        // 填充路径 848×480（pad 1024×512；bin u = 128 恰为 f=128/1024）：四组
        // 全图与 double 参考逐像素 ±1（截断谱扩展/Gibbs 为冻结语义，解析常数
        // 期望不存在，见 §7 边界语义披露）。
        const ImageU8 sinePadded = makeSine(848, 480, 0.125, 0.0, 40.0);
        {
            const std::unique_ptr<IImageNode> node =
                makeNode(lowpassDescriptor, 1, {assign("cutoff", 0.1)});
            RIN_CHECK_MSG(applyMatches(*node, sinePadded,
                                       refFftBytes(FftKind::Lowpass, 0.1, 0.0, 0.0, sinePadded),
                                       1),
                          "填充 848×480 低通 c=0.1 滤除 f=0.125（参考 ±1）");
        }
        {
            const std::unique_ptr<IImageNode> node =
                makeNode(highpassDescriptor, 1, {assign("cutoff", 0.1)});
            RIN_CHECK_MSG(applyMatches(*node, sinePadded,
                                       refFftBytes(FftKind::Highpass, 0.1, 0.0, 0.0, sinePadded),
                                       1),
                          "填充 848×480 高通 c=0.1 保留 f=0.125（参考 ±1）");
        }
        {
            const std::unique_ptr<IImageNode> node =
                makeNode(bandpassDescriptor, 1, {assign("lowCut", 0.05), assign("highCut", 0.6)});
            RIN_CHECK_MSG(applyMatches(*node, sinePadded,
                                       refFftBytes(FftKind::Bandpass, 0.0, 0.05, 0.6, sinePadded),
                                       1),
                          "填充 848×480 带通 (0.05,0.6] 保留 f=0.125（参考 ±1）");
        }
        {
            const std::unique_ptr<IImageNode> node =
                makeNode(bandpassDescriptor, 1, {assign("lowCut", 0.05), assign("highCut", 0.1)});
            RIN_CHECK_MSG(applyMatches(*node, sinePadded,
                                       refFftBytes(FftKind::Bandpass, 0.0, 0.05, 0.1, sinePadded),
                                       1),
                          "填充 848×480 带通 (0.05,0.1] 全阻 f=0.125（参考 ±1）");
        }
    }

    // ===================================================================
    // 4b) 二维对角单频（fx = fy = 1/16，ρ = √2/16 ≈ 0.0884）：512 原生解析 +
    //     848×480 填充参考（填充路径 bin (64, 32) 仍精确落栅格）
    // ===================================================================
    {
        const ImageU8 diagonal = makeSine(512, 512, 0.0625, 0.0625, 40.0);
        const std::vector<std::uint8_t> original = imageBytes(diagonal);
        const std::vector<std::uint8_t> minusMean = expectedMinusMean(diagonal);
        // 低通 c=0.1：ρ=0.0884 ≤ 0.1 保留 → 输出 ≈ 输入 ±1（通带噪声占比
        // (0.1/0.7071)² ≈ 2%，σ ≈ 0.04）。
        {
            const std::unique_ptr<IImageNode> node =
                makeNode(lowpassDescriptor, 1, {assign("cutoff", 0.1)});
            RIN_CHECK_MSG(applyMatches(*node, diagonal, original, 1),
                          "原生 512² 对角单频低通 c=0.1 保留（解析 ≈ 输入 ±1）");
            RIN_CHECK_MSG(applyMatches(*node, diagonal,
                                       refFftBytes(FftKind::Lowpass, 0.1, 0.0, 0.0, diagonal), 1),
                          "原生 512² 对角单频低通与参考一致");
        }
        // 高通 c=0.1：ρ=0.0884 ≤ 0.1 谱线与 DC 均被阻带移除 → 全阻 → 输出 =
        // 通带外量化噪声（功率占比 ≈ 2%，σ ≈ 0.04 → 字节 0 ±1）。
        {
            const std::unique_ptr<IImageNode> node =
                makeNode(highpassDescriptor, 1, {assign("cutoff", 0.1)});
            const std::vector<std::uint8_t> zero(std::size_t{512} * 512, std::uint8_t{0});
            RIN_CHECK_MSG(applyMatches(*node, diagonal, zero, 1),
                          "原生 512² 对角单频高通 c=0.1 滤除（解析 0 ±1）");
            RIN_CHECK_MSG(applyMatches(*node, diagonal,
                                       refFftBytes(FftKind::Highpass, 0.1, 0.0, 0.0, diagonal), 1),
                          "原生 512² 对角单频高通与参考一致");
        }
        // 带通 (0.05, 0.6)：ρ=0.0884 ∈ (0.05, 0.6] 保留 → 输入 − 均值 ±1。
        {
            const std::unique_ptr<IImageNode> node =
                makeNode(bandpassDescriptor, 1, {assign("lowCut", 0.05), assign("highCut", 0.6)});
            RIN_CHECK_MSG(applyMatches(*node, diagonal, minusMean, 1),
                          "原生 512² 对角单频带通保留（解析 输入−均值 ±1）");
            RIN_CHECK_MSG(applyMatches(*node, diagonal,
                                       refFftBytes(FftKind::Bandpass, 0.0, 0.05, 0.6, diagonal), 1),
                          "原生 512² 对角单频带通与参考一致");
        }
        // 填充路径 848×480 对角单频：参考锁定。
        const ImageU8 diagonalPadded = makeSine(848, 480, 0.0625, 0.0625, 40.0);
        {
            const std::unique_ptr<IImageNode> node =
                makeNode(lowpassDescriptor, 1, {assign("cutoff", 0.1)});
            RIN_CHECK_MSG(applyMatches(*node, diagonalPadded,
                                       refFftBytes(FftKind::Lowpass, 0.1, 0.0, 0.0,
                                                   diagonalPadded), 1),
                          "填充 848×480 对角单频低通 c=0.1 保留（参考 ±1）");
        }
    }

    // ===================================================================
    // 5) 填充/裁剪往返：cutoff ≥ √0.5 低通恒等（±1）——归一化频率上界
    //    ρ_max = √(0.5² + 0.5²) = √0.5 ≈ 0.7071，全部 bin（含 DC）落在通带。
    // ===================================================================
    {
        const ImageU8 noisePadded = makeNoise(848, 480, 0xF777u);
        const std::vector<std::uint8_t> originalPadded = imageBytes(noisePadded);
        {
            const std::unique_ptr<IImageNode> node =
                makeNode(lowpassDescriptor, 1, {assign("cutoff", 1.0)});
            RIN_CHECK_MSG(applyMatches(*node, noisePadded, originalPadded, 1),
                          "填充 848×480 低通 c=1.0 恒等（解析 ±1）");
        }
        {
            const std::unique_ptr<IImageNode> node =
                makeNode(lowpassDescriptor, 1, {assign("cutoff", 0.71)});
            RIN_CHECK_MSG(applyMatches(*node, noisePadded, originalPadded, 1),
                          "填充 848×480 低通 c=0.71 > √0.5 恒等（解析 ±1）");
            RIN_CHECK_MSG(applyMatches(*node, noisePadded,
                                       refFftBytes(FftKind::Lowpass, 0.71, 0.0, 0.0,
                                                   noisePadded), 1),
                          "填充 848×480 低通 c=0.71 与参考一致");
        }
        // 边界：c = √0.5，角点 bin（fx=±0.5, fy=±0.5）ρ = √0.5 恰被 ≤ 保留 →
        // 恒等（掩膜判定双方同为 IEEE double sqrt(0.5)，精确可比）。
        {
            const std::unique_ptr<IImageNode> node =
                makeNode(lowpassDescriptor, 1, {assign("cutoff", sqrtHalf)});
            RIN_CHECK_MSG(applyMatches(*node, noisePadded, originalPadded, 1),
                          "填充 848×480 低通 c=√0.5 边界恒等（解析 ±1）");
            RIN_CHECK_MSG(applyMatches(*node, noisePadded,
                                       refFftBytes(FftKind::Lowpass, sqrtHalf, 0.0, 0.0,
                                                   noisePadded), 1),
                          "填充 848×480 低通 c=√0.5 与参考一致");
        }
        const ImageU8 noiseNative = makeNoise(512, 512, 0xA11CEu);
        const std::vector<std::uint8_t> originalNative = imageBytes(noiseNative);
        {
            const std::unique_ptr<IImageNode> node =
                makeNode(lowpassDescriptor, 1, {assign("cutoff", 1.0)});
            RIN_CHECK_MSG(applyMatches(*node, noiseNative, originalNative, 1),
                          "原生 512² 低通 c=1.0 恒等（解析 ±1）");
        }
        {
            const std::unique_ptr<IImageNode> node =
                makeNode(lowpassDescriptor, 1, {assign("cutoff", 0.71)});
            RIN_CHECK_MSG(applyMatches(*node, noiseNative, originalNative, 1),
                          "原生 512² 低通 c=0.71 恒等（解析 ±1）");
            RIN_CHECK_MSG(applyMatches(*node, noiseNative,
                                       refFftBytes(FftKind::Lowpass, 0.71, 0.0, 0.0,
                                                   noiseNative), 1),
                          "原生 512² 低通 c=0.71 与参考一致");
        }
        // 小尺寸填充：5×3 → pad 8×4；1×5 → pad 2×8（下界 2：1 像素宽填到 2）。
        {
            const ImageU8 small = makeGray(5, 3, [](std::uint32_t x, std::uint32_t y) {
                return static_cast<std::uint8_t>((x * 37u + y * 61u + 11u) & 0xFFu);
            });
            const std::unique_ptr<IImageNode> node =
                makeNode(lowpassDescriptor, 1, {assign("cutoff", 1.0)});
            RIN_CHECK_MSG(applyMatches(*node, small, imageBytes(small), 1),
                          "5×3（pad 8×4）低通 c=1.0 恒等（解析 ±1）");
            RIN_CHECK_MSG(applyMatches(*node, small,
                                       refFftBytes(FftKind::Lowpass, 1.0, 0.0, 0.0, small), 1),
                          "5×3（pad 8×4）低通与参考一致");
        }
        {
            const ImageU8 thin = makeGray(1, 5, [](std::uint32_t, std::uint32_t y) {
                return static_cast<std::uint8_t>((y * 53u + 7u) & 0xFFu);
            });
            const std::unique_ptr<IImageNode> node =
                makeNode(lowpassDescriptor, 1, {assign("cutoff", 1.0)});
            RIN_CHECK_MSG(applyMatches(*node, thin, imageBytes(thin), 1),
                          "1×5（pad 2×8，下界 2）低通 c=1.0 恒等（解析 ±1）");
            RIN_CHECK_MSG(applyMatches(*node, thin,
                                       refFftBytes(FftKind::Lowpass, 1.0, 0.0, 0.0, thin), 1),
                          "1×5（pad 2×8）低通与参考一致");
        }
        // 高通永不恒等（DC 恒被移除）：c=1.0 高通 = 输入 − 均值形态，参考锁定。
        {
            const std::unique_ptr<IImageNode> node =
                makeNode(highpassDescriptor, 1, {assign("cutoff", 1.0)});
            RIN_CHECK_MSG(applyMatches(*node, noiseNative,
                                       refFftBytes(FftKind::Highpass, 1.0, 0.0, 0.0,
                                                   noiseNative), 1),
                          "原生 512² 高通 c=1.0 非恒等（DC 移除，参考 ±1）");
        }
    }

    // ===================================================================
    // 6) 掩膜代数：低通 + 高通 ≡ 掩膜补；带通 ≡ 低通(highCut) − 低通(lowCut)
    //    掩膜差。double 域恒等由参考自检 2（LP_raw + HP_raw == 原图 ≤1e-6）
    //    精确锁定。字节域注意（8bit 饱和语义）：q_sat 对 x < 0 / x > 255 的
    //    钳制使"两输出相加 == 原图"仅在两分量均不触界时成立——宽带噪声图上
    //    LP(c) 是重平滑版本（值可偏离原图上百字节），HP = 原图 − LP 大幅负值
    //    被钳到 0（实测：5×3 c=0.4 噪声图 pixel14，LP 原始值 −4.52 → 0、HP
    //    = 55、和 55 vs 原图 50；848×480 c=0.2 噪声图 20 万像素 |和−原图| ≥ 2、
    //    最大 205——饱和物理，非实现缺陷）。故字节域和恒等用带限内容断言
    //    （c=0.2 > 内容带宽 0.15 → HP 值仅为 ±0.3 量级阻带残差）；宽带内容
    //    只做逐分量参考比对。
    // ===================================================================
    {
        struct SizeCase {
            std::uint32_t width;
            std::uint32_t height;
            std::uint32_t seed;
            bool paddedPath;  // 非 2 幂尺寸：裁剪截断谱扩展使 HP 参考值大幅越界
        };
        const std::vector<SizeCase> sizeCases = {
            SizeCase{848, 480, 0x5150u, true},
            SizeCase{512, 512, 0x6261u, false},
            SizeCase{5, 3, 0x7372u, false}};
        for (const SizeCase& sizeCase : sizeCases) {
            const ImageU8 wideband = makeNoise(sizeCase.width, sizeCase.height, sizeCase.seed);
            const std::vector<std::uint8_t> contentBytes =
                quantizeSat(refFftRaw(FftKind::Lowpass, 0.15, 0.0, 0.0, wideband));
            const ImageU8 content =
                makeGray(sizeCase.width, sizeCase.height,
                         [&contentBytes, &sizeCase](std::uint32_t x, std::uint32_t y) {
                             return contentBytes[std::size_t{y} * sizeCase.width + x];
                         });
            const std::vector<std::uint8_t> original = imageBytes(content);
            const std::unique_ptr<IImageNode> lowNode =
                makeNode(lowpassDescriptor, 1, {assign("cutoff", 0.2)});
            const std::unique_ptr<IImageNode> highNode =
                makeNode(highpassDescriptor, 1, {assign("cutoff", 0.2)});
            const std::vector<std::uint8_t> low = applyGray(*lowNode, content);
            const std::vector<std::uint8_t> high = applyGray(*highNode, content);
            RIN_CHECK(!low.empty() && !high.empty() && low.size() == high.size() &&
                      low.size() == original.size());
            // 参考未量化值（掩膜补：loRef + hiRef == content 精确，自检 2）。
            const std::vector<double> lowRefRaw =
                refFftRaw(FftKind::Lowpass, 0.2, 0.0, 0.0, content);
            const std::vector<double> highRefRaw =
                refFftRaw(FftKind::Highpass, 0.2, 0.0, 0.0, content);
            RIN_CHECK(!low.empty() && low.size() == lowRefRaw.size());
            if (!low.empty() && low.size() == high.size() && low.size() == original.size()) {
                if (!sizeCase.paddedPath) {
                    // 原生/小尺寸路径：内容带限在重新填充后保持（无截断扩展），
                    // HP 值仅为 ±0.3 量级阻带量化噪声 → 和 == 原图 ±1。
                    int maxSumDiff = 0;  // 8bit 相加用 int 承载，防回绕。
                    for (std::size_t i = 0; i < low.size(); ++i) {
                        const int total = static_cast<int>(low[i]) + static_cast<int>(high[i]);
                        const int diff = total - static_cast<int>(original[i]);
                        maxSumDiff = std::max(maxSumDiff, diff < 0 ? -diff : diff);
                    }
                    RIN_CHECK_MSG(maxSumDiff <= 1,
                                  "掩膜代数：LP+HP 与原图逐像素差 ≤ 1（原生路径带限内容）");
                } else {
                    // 填充路径：带限内容裁回 848×480 后重新填充，矩形截断把
                    // ρ > 0.2 能量重新注入（冻结零填充语义；实测 HP 参考值最低
                    // −9.81、12683 像素 |和−原图| ≥ 2、最大 10——q_sat 钳制负值
                    // 所致，非实现缺陷）。断言实现和 == q_sat(loRef)+q_sat(hiRef)
                    // ±2（逐分量字节级交叉偏差界），代数恒等本体由自检 2 锁定。
                    int maxConsistencyDiff = 0;
                    for (std::size_t i = 0; i < low.size(); ++i) {
                        const int expectedSum = static_cast<int>(saturateRound(lowRefRaw[i])) +
                                                static_cast<int>(saturateRound(highRefRaw[i]));
                        const int total = static_cast<int>(low[i]) + static_cast<int>(high[i]);
                        const int diff = total - expectedSum;
                        maxConsistencyDiff =
                            std::max(maxConsistencyDiff, diff < 0 ? -diff : diff);
                    }
                    RIN_CHECK_MSG(maxConsistencyDiff <= 2,
                                  "掩膜代数：LP+HP 与参考期望和一致 ±2（填充路径饱和感知）");
                }
            }
            RIN_CHECK_MSG(!low.empty() && maxByteDiff(low,
                                                      refFftBytes(FftKind::Lowpass, 0.2, 0.0,
                                                                  0.0, content)) <= 1,
                          "掩膜代数：LP 与参考一致（带限内容）");
            RIN_CHECK_MSG(!high.empty() && maxByteDiff(high,
                                                       refFftBytes(FftKind::Highpass, 0.2, 0.0,
                                                                   0.0, content)) <= 1,
                          "掩膜代数：HP 与参考一致（带限内容）");
        }
        // 6a-2) 宽带内容的逐分量保真（字节域和恒等不适用——见上饱和说明；
        //       掩膜补在 double 域由参考自检 2 锁定）。
        {
            const ImageU8 noise = makeNoise(848, 480, 0x5150u);
            const std::unique_ptr<IImageNode> lowNode =
                makeNode(lowpassDescriptor, 1, {assign("cutoff", 0.2)});
            const std::unique_ptr<IImageNode> highNode =
                makeNode(highpassDescriptor, 1, {assign("cutoff", 0.2)});
            RIN_CHECK_MSG(applyMatches(*lowNode, noise,
                                       refFftBytes(FftKind::Lowpass, 0.2, 0.0, 0.0, noise), 1),
                          "掩膜代数：宽带 LP 与参考一致");
            RIN_CHECK_MSG(applyMatches(*highNode, noise,
                                       refFftBytes(FftKind::Highpass, 0.2, 0.0, 0.0, noise), 1),
                          "掩膜代数：宽带 HP 与参考一致");
        }

        // 6b) 带通 ≡ 低通(highCut) − 低通(lowCut)（参考未量化差 + 8bit 饱和量化
        //     后比对；实现三输出各自亦与参考 ±1）。
        for (const auto& [image, bandParams] :
             {std::pair{makeNoise(848, 480, 0x8381u), std::pair{0.1, 0.4}},
              std::pair{makeNoise(512, 512, 0x9390u), std::pair{0.125, 0.5}}}) {
            const double loCut = bandParams.first;
            const double hiCut = bandParams.second;
            const std::vector<double> lowHiRaw =
                refFftRaw(FftKind::Lowpass, hiCut, 0.0, 0.0, image);
            const std::vector<double> lowLoRaw =
                refFftRaw(FftKind::Lowpass, loCut, 0.0, 0.0, image);
            // 掩膜差的 8bit 语义：先差后量化（饱和），非量化后相减（负差截 0 会
            // 破坏代数，见文件头 5) 说明）。
            std::vector<std::uint8_t> expectedBand(lowHiRaw.size());
            for (std::size_t i = 0; i < lowHiRaw.size(); ++i) {
                expectedBand[i] = saturateRound(lowHiRaw[i] - lowLoRaw[i]);
            }
            const std::unique_ptr<IImageNode> bandNode =
                makeNode(bandpassDescriptor, 1,
                         {assign("lowCut", loCut), assign("highCut", hiCut)});
            const std::vector<std::uint8_t> band = applyGray(*bandNode, image);
            RIN_CHECK_MSG(!band.empty() && maxByteDiff(band, expectedBand) <= 1,
                          "掩膜代数：BP ≡ LP(hi)−LP(lo)（参考未量化差 ±1）");
            RIN_CHECK_MSG(!band.empty() &&
                              maxByteDiff(band, refFftBytes(FftKind::Bandpass, 0.0, loCut,
                                                            hiCut, image)) <= 1,
                          "掩膜代数：BP 与参考一致");
            const std::unique_ptr<IImageNode> lowHiNode =
                makeNode(lowpassDescriptor, 1, {assign("cutoff", hiCut)});
            const std::unique_ptr<IImageNode> lowLoNode =
                makeNode(lowpassDescriptor, 1, {assign("cutoff", loCut)});
            RIN_CHECK_MSG(applyMatches(*lowHiNode, image,
                                       refFftBytes(FftKind::Lowpass, hiCut, 0.0, 0.0, image), 1),
                          "掩膜代数：LP(hi) 与参考一致");
            RIN_CHECK_MSG(applyMatches(*lowLoNode, image,
                                       refFftBytes(FftKind::Lowpass, loCut, 0.0, 0.0, image), 1),
                          "掩膜代数：LP(lo) 与参考一致");

        }

        // 6b-2) 实现内部代数形式：|BP_impl − max(0, q(LP(hi)) − q(LP(lo)))| ≤ 1。
        //       前提是全部值在 [0,255] 内（不触饱和界）：round-half-up 单调 +
        //       负差由 max(0,·) 吸收 → ≤1 可证。宽带内容上不成立——LP(hi) 原始
        //       值可超 255（512² 噪声 BP(0.125,0.5) 实测 LP(0.5) 原始值 345 →
        //       q_sat 255，字节域差被双重饱和破坏，最大偏差 90）——任意内容的
        //       规范判据是上面 6b 的"参考未量化差、先差后量化"形式。
        {
            const ImageU8 wideband = makeNoise(512, 512, 0x9390u);
            const std::vector<double> smoothRaw =
                refFftRaw(FftKind::Lowpass, 0.2, 0.0, 0.0, wideband);
            double smoothMean = 0.0;
            for (const double value : smoothRaw) {
                smoothMean += value;
            }
            smoothMean /= static_cast<double>(smoothRaw.size());
            // 内容 = 128 + 0.5·(低通去均值偏差) → 带宽 ≤ 0.2、值域约 [107,149]。
            std::vector<std::uint8_t> contentBytes(smoothRaw.size());
            for (std::size_t i = 0; i < smoothRaw.size(); ++i) {
                contentBytes[i] = saturateRound(128.0 + 0.5 * (smoothRaw[i] - smoothMean));
            }
            const ImageU8 content = makeGray(
                512, 512,
                [&contentBytes](std::uint32_t x, std::uint32_t y) {
                    return contentBytes[std::size_t{y} * 512u + x];
                });
            const std::unique_ptr<IImageNode> lowHiNode =
                makeNode(lowpassDescriptor, 1, {assign("cutoff", 0.3)});
            const std::unique_ptr<IImageNode> lowLoNode =
                makeNode(lowpassDescriptor, 2, {assign("cutoff", 0.1)});
            const std::unique_ptr<IImageNode> bandNode = makeNode(
                bandpassDescriptor, 3, {assign("lowCut", 0.1), assign("highCut", 0.3)});
            const std::vector<std::uint8_t> lowHi = applyGray(*lowHiNode, content);
            const std::vector<std::uint8_t> lowLo = applyGray(*lowLoNode, content);
            const std::vector<std::uint8_t> band = applyGray(*bandNode, content);
            RIN_CHECK(!lowHi.empty() && !lowLo.empty() && !band.empty());
            if (!lowHi.empty() && lowHi.size() == lowLo.size() && lowLo.size() == band.size()) {
                int maxImplDiff = 0;
                for (std::size_t i = 0; i < band.size(); ++i) {
                    const int d = static_cast<int>(lowHi[i]) - static_cast<int>(lowLo[i]);
                    const int expected = d > 0 ? d : 0;
                    const int diff = static_cast<int>(band[i]) - expected;
                    maxImplDiff = std::max(maxImplDiff, diff < 0 ? -diff : diff);
                }
                RIN_CHECK_MSG(maxImplDiff <= 1,
                              "掩膜代数：BP 与实现 LP 差的内部形式 ≤ 1（带限内容）");
            }
            // 同内容的规范形式（参考未量化差、先差后量化）。
            const std::vector<double> lowHiRaw =
                refFftRaw(FftKind::Lowpass, 0.3, 0.0, 0.0, content);
            const std::vector<double> lowLoRaw =
                refFftRaw(FftKind::Lowpass, 0.1, 0.0, 0.0, content);
            std::vector<std::uint8_t> expectedBand(lowHiRaw.size());
            for (std::size_t i = 0; i < lowHiRaw.size(); ++i) {
                expectedBand[i] = saturateRound(lowHiRaw[i] - lowLoRaw[i]);
            }
            RIN_CHECK_MSG(!band.empty() && maxByteDiff(band, expectedBand) <= 1,
                          "掩膜代数：BP ≡ LP(hi)−LP(lo)（带限内容参考未量化差 ±1）");
        }
    }

    // ===================================================================
    // 7) DC 语义（常值图 / 斜坡图；原生路径解析精确，填充路径单 bin 解析）
    // ===================================================================
    {
        // 7a) 原生 512² 常值 128：常量铺满栅格 → 谱仅 DC（任意 cutoff）。
        //   低通恒等（精确 128）；高通全黑（精确 0，含 c=0 仅移除 DC）；带通全黑。
        const ImageU8 constNative = makeGray(512, 512, [](std::uint32_t, std::uint32_t) {
            return std::uint8_t{128};
        });
        {
            const std::unique_ptr<IImageNode> lpDefault = makeNode(lowpassDescriptor, 1);
            const std::unique_ptr<IImageNode> lpZero =
                makeNode(lowpassDescriptor, 2, {assign("cutoff", 0.0)});
            const std::vector<std::uint8_t> all128(std::size_t{512} * 512, std::uint8_t{128});
            RIN_CHECK_MSG(applyMatches(*lpDefault, constNative, all128, 0),
                          "原生常值图低通 c=0.2 恒等（精确 128）");
            RIN_CHECK_MSG(applyMatches(*lpZero, constNative, all128, 0),
                          "原生常值图低通 c=0 仅 DC = 均值（精确 128）");
        }
        {
            const std::unique_ptr<IImageNode> hpDefault = makeNode(highpassDescriptor, 1);
            const std::unique_ptr<IImageNode> hpZero =
                makeNode(highpassDescriptor, 2, {assign("cutoff", 0.0)});
            const std::vector<std::uint8_t> all0(std::size_t{512} * 512, std::uint8_t{0});
            RIN_CHECK_MSG(applyMatches(*hpDefault, constNative, all0, 0),
                          "原生常值图高通 c=0.2 全黑（精确 0）");
            RIN_CHECK_MSG(applyMatches(*hpZero, constNative, all0, 0),
                          "原生常值图高通 c=0 仅移除 DC（精确 0）");
        }
        {
            const std::unique_ptr<IImageNode> bpDefault = makeNode(bandpassDescriptor, 1);
            const std::unique_ptr<IImageNode> bpZero =
                makeNode(bandpassDescriptor, 2, {assign("lowCut", 0.0), assign("highCut", 0.6)});
            const std::vector<std::uint8_t> all0(std::size_t{512} * 512, std::uint8_t{0});
            RIN_CHECK_MSG(applyMatches(*bpDefault, constNative, all0, 0),
                          "原生常值图带通 (0.2,0.6] 全黑（精确 0）");
            RIN_CHECK_MSG(applyMatches(*bpZero, constNative, all0, 0),
                          "原生常值图带通 (0,0.6] 全黑（DC 移除，精确 0）");
        }
        // 7b) 原生 16×16 常值 7（小尺寸同语义）。
        {
            const ImageU8 small = makeGray(16, 16, [](std::uint32_t, std::uint32_t) {
                return std::uint8_t{7};
            });
            const std::unique_ptr<IImageNode> lp = makeNode(lowpassDescriptor, 1);
            const std::unique_ptr<IImageNode> hp = makeNode(highpassDescriptor, 1);
            const std::vector<std::uint8_t> all7(std::size_t{16} * 16, std::uint8_t{7});
            const std::vector<std::uint8_t> all0(std::size_t{16} * 16, std::uint8_t{0});
            RIN_CHECK_MSG(applyMatches(*lp, small, all7, 0), "原生 16² 常值低通恒等（精确 7）");
            RIN_CHECK_MSG(applyMatches(*hp, small, all0, 0), "原生 16² 常值高通全黑（精确 0）");
        }
        // 7c) 原生 512² 斜坡（x mod 256，均值 127.5）：c=0 低通仅 DC → 均匀
        //     floor(127.5+0.5) = 128（精确）；c=0 高通仅移除 DC → 斜坡 − 127.5
        //     → floor(v+0.5) = max(0, x−127)（±1：float 往返噪声越过整数量化界）。
        const ImageU8 ramp = makeGray(512, 512, [](std::uint32_t x, std::uint32_t) {
            return static_cast<std::uint8_t>(x % 256u);
        });
        {
            const std::unique_ptr<IImageNode> lpZero =
                makeNode(lowpassDescriptor, 1, {assign("cutoff", 0.0)});
            const std::vector<std::uint8_t> all128(std::size_t{512} * 512, std::uint8_t{128});
            RIN_CHECK_MSG(applyMatches(*lpZero, ramp, all128, 0),
                          "原生斜坡低通 c=0 仅 DC = 均值 127.5 → 128（精确）");
        }
        {
            const std::unique_ptr<IImageNode> hpZero =
                makeNode(highpassDescriptor, 1, {assign("cutoff", 0.0)});
            const std::vector<std::uint8_t> expected = expectedBytes(512, 512,
                                                                     [](std::uint32_t x,
                                                                        std::uint32_t) {
                                                                         const std::uint32_t p =
                                                                             x % 256u;  // 斜坡像素值
                                                                         return p >= 128u
                                                                                    ? static_cast<std::uint8_t>(p - 127u)
                                                                                    : std::uint8_t{0};
                                                                     });
            RIN_CHECK_MSG(applyMatches(*hpZero, ramp, expected, 1),
                          "原生斜坡高通 c=0 = 斜坡−均值（max(0, x−127) ±1）");
        }
        // 7d) 填充 848×480 常值 128（pad 1024×512）：c=0 低通仅保留 DC bin →
        //     全图均匀 Σ/(padW·padH) = 848·480·128/524288 = 52101120/524288 =
        //     99.375 → floor(99.875) = 99（精确：单 bin 重建无空间结构）；c=0
        //     高通移除 DC bin → 128 − 99.375 = 28.625 → 29（均匀，±1 float 噪声）。
        const ImageU8 constPadded = makeGray(848, 480, [](std::uint32_t, std::uint32_t) {
            return std::uint8_t{128};
        });
        {
            const std::unique_ptr<IImageNode> lpZero =
                makeNode(lowpassDescriptor, 1, {assign("cutoff", 0.0)});
            const std::vector<std::uint8_t> all99(std::size_t{848} * 480, std::uint8_t{99});
            RIN_CHECK_MSG(applyMatches(*lpZero, constPadded, all99, 0),
                          "填充常值图低通 c=0 仅 DC = 99.375 → 99（精确）");
        }
        {
            const std::unique_ptr<IImageNode> hpZero =
                makeNode(highpassDescriptor, 1, {assign("cutoff", 0.0)});
            const std::vector<std::uint8_t> all29(std::size_t{848} * 480, std::uint8_t{29});
            RIN_CHECK_MSG(applyMatches(*hpZero, constPadded, all29, 1),
                          "填充常值图高通 c=0 = 128 − 99.375 = 28.625 → 29（±1）");
        }
        {
            const std::unique_ptr<IImageNode> lpOne =
                makeNode(lowpassDescriptor, 1, {assign("cutoff", 1.0)});
            const std::vector<std::uint8_t> all128(std::size_t{848} * 480, std::uint8_t{128});
            RIN_CHECK_MSG(applyMatches(*lpOne, constPadded, all128, 1),
                          "填充常值图低通 c=1.0 恒等（±1）");
            const std::unique_ptr<IImageNode> hpDefault = makeNode(highpassDescriptor, 2);
            RIN_CHECK_MSG(applyMatches(*hpDefault, constPadded,
                                       refFftBytes(FftKind::Highpass, 0.2, 0.0, 0.0,
                                                   constPadded), 1),
                          "填充常值图高通 c=0.2（边缘谱扩展非全黑，参考 ±1）");
        }
        // 7e) 填充 848×480 斜坡：c=0 低通 = Σ/(padW·padH) 均匀（精确单 bin）。
        {
            const ImageU8 rampPadded = makeGray(848, 480, [](std::uint32_t x, std::uint32_t) {
                return static_cast<std::uint8_t>(x % 256u);
            });
            const std::uint64_t sum = [] {
                std::uint64_t rowSum = 0;
                for (std::uint32_t x = 0; x < 848; ++x) {
                    rowSum += x % 256u;
                }
                return rowSum * 480u;
            }();
            const std::uint8_t dcByte = saturateRound(static_cast<double>(sum) / 524288.0);
            const std::unique_ptr<IImageNode> lpZero =
                makeNode(lowpassDescriptor, 1, {assign("cutoff", 0.0)});
            const std::vector<std::uint8_t> uniform(std::size_t{848} * 480, dcByte);
            RIN_CHECK_MSG(applyMatches(*lpZero, rampPadded, uniform, 0),
                          "填充斜坡低通 c=0 仅 DC = Σ/524288 均匀（精确）");
        }
    }

    // ===================================================================
    // 8) 归一化频率跨尺寸不变：同一 f=1/8、c=0.1 低通在原生 64²/128²（解析
    //    滤除）与填充 848×480（参考滤除，bin 128）行为一致——cutoff 判定按
    //    归一化频率而非绝对 bin（若误用 bin 下标，填充路径 bin 128 vs 原生
    //    bin 64 将分裂，参考比对可判别）。
    // ===================================================================
    {
        for (const std::uint32_t size : {64u, 128u}) {
            const ImageU8 sine = makeSine(size, size, 0.125, 0.0, 40.0);
            const std::vector<std::uint8_t> meanOnly(
                std::size_t{size} * size, saturateRound(grayMean(sine)));
            const std::unique_ptr<IImageNode> node =
                makeNode(lowpassDescriptor, 1, {assign("cutoff", 0.1)});
            RIN_CHECK_MSG(applyMatches(*node, sine, meanOnly, 1),
                          "原生 " + std::to_string(size) + "² 低通 c=0.1 滤除 f=0.125（解析 ±1）");
        }
        const ImageU8 sinePadded = makeSine(848, 480, 0.125, 0.0, 40.0);
        const std::unique_ptr<IImageNode> node =
            makeNode(lowpassDescriptor, 1, {assign("cutoff", 0.1)});
        RIN_CHECK_MSG(applyMatches(*node, sinePadded,
                                   refFftBytes(FftKind::Lowpass, 0.1, 0.0, 0.0, sinePadded), 1),
                      "填充 848×480 低通 c=0.1 滤除同一归一化频率（参考 ±1）");
    }

    // ===================================================================
    // 9) 防御路径：数量 0/2、无效图、Rgba8 喂 Gray8 声明
    // ===================================================================
    {
        const std::unique_ptr<IImageNode> lowpass = makeNode(lowpassDescriptor, 1);
        const std::unique_ptr<IImageNode> bandpass = makeNode(bandpassDescriptor, 2);
        const ImageU8 gray = makeGray(8, 8, [](std::uint32_t, std::uint32_t) { return 9; });
        // 构造 Rgba8（同 test_image_ops_histogram 的 wrap 手法）。
        std::vector<std::uint8_t> rgbaBuffer(std::size_t{8} * 8 * 4, std::uint8_t{200});
        auto rgbaShared =
            std::make_shared<const std::vector<std::uint8_t>>(std::move(rgbaBuffer));
        const ImageU8 rgbaImage =
            ImageU8::wrap(PortType::Rgba8, 8, 8, 32, std::move(rgbaShared));
        RIN_CHECK(rgbaImage.valid());
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)lowpass->apply({}); }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)lowpass->apply({gray, gray}); }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)lowpass->apply({ImageU8{}}); }));
        RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] { (void)lowpass->apply({rgbaImage}); }),
                      "fft_lowpass 格式错配（Rgba8 喂 Gray8 声明）拒绝");
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)bandpass->apply({}); }));
        RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] { (void)bandpass->apply({rgbaImage}); }),
                      "fft_bandpass 格式错配（Rgba8 喂 Gray8 声明）拒绝");
    }

    // ===================================================================
    // 10) stride padding 输入 / 紧凑输出 / 不共享源像素
    // ===================================================================
    {
        // 同一份像素内容做两种包装（紧凑 / 行尾 padding）——内容必须逐字节相同，
        // 才能把输出差异归因于 stride 处理。
        ByteLcg lcg{0xBEEF77u};
        std::vector<std::uint8_t> content(std::size_t{37} * 23);
        for (std::uint8_t& value : content) {
            value = lcg.next();
        }
        const ImageU8 compact =
            makeGray(37, 23, [&content](std::uint32_t x, std::uint32_t y) {
                return content[std::size_t{y} * 37u + x];
            });
        const ImageU8 padded =
            makeGray(37, 23, [&content](std::uint32_t x, std::uint32_t y) {
                return content[std::size_t{y} * 37u + x];
            }, 40u, 0xEE);
        RIN_CHECK_EQ(padded.stride(), std::uint32_t{40});
        const std::unique_ptr<IImageNode> node =
            makeNode(lowpassDescriptor, 1, {assign("cutoff", 0.3)});
        const std::vector<std::uint8_t> fromCompact = applyGray(*node, compact);
        const std::vector<std::uint8_t> fromPadded = applyGray(*node, padded);
        RIN_CHECK(!fromCompact.empty() && !fromPadded.empty());
        RIN_CHECK_MSG(fromCompact == fromPadded,
                      "行尾 padding 输入与紧凑输入输出逐字节一致（padding 不泄漏）");
        RIN_CHECK_MSG(!fromCompact.empty() &&
                          maxByteDiff(fromCompact,
                                      refFftBytes(FftKind::Lowpass, 0.3, 0.0, 0.0, compact)) <= 1,
                      "37×23（pad 64×32）低通 c=0.3 与参考一致");
        // 紧凑输出与不共享源像素的显式核对。
        const std::vector<ImageU8> output = node->apply({padded});
        RIN_CHECK_EQ(output.size(), std::size_t{1});
        RIN_CHECK_EQ(output[0].stride(), output[0].width());  // 输出紧凑，不继承输入 stride。
        RIN_CHECK_EQ(output[0].width(), std::uint32_t{37});
        RIN_CHECK_EQ(output[0].height(), std::uint32_t{23});
        RIN_CHECK(output[0].pixels() != padded.pixels());
        RIN_CHECK(output[0].pixels()->data() != padded.pixels()->data());
    }

    // ===================================================================
    // 11) 掩膜边界 ≤ / > 语义与空通带拒绝（cutoff = 0.125 恰在注入谱线上：
    //     f = 1/8 → ρ = 0.125 精确；原生路径解析 + 参考锁定）
    // ===================================================================
    {
        const ImageU8 sineNative = makeSine(512, 512, 0.125, 0.0, 40.0);
        const std::vector<std::uint8_t> original = imageBytes(sineNative);
        // 低通 c=0.125：ρ = 0.125 ≤ c 谱线保留 → ≈ 输入 ±1（≤ 含等号语义）。
        {
            const std::unique_ptr<IImageNode> node =
                makeNode(lowpassDescriptor, 1, {assign("cutoff", 0.125)});
            RIN_CHECK_MSG(applyMatches(*node, sineNative, original, 1),
                          "原生低通 c=0.125 边界保留谱线（ρ ≤ c 含等号，解析 ±1）");
            RIN_CHECK_MSG(applyMatches(*node, sineNative,
                                       refFftBytes(FftKind::Lowpass, 0.125, 0.0, 0.0,
                                                   sineNative), 1),
                          "原生低通 c=0.125 与参考一致");
        }
        // lowCut == highCut：空通带（通带 (lowCut, highCut] 须非空）构造期拒绝
        // （2026-09-29 裁决：掩膜差公式为准，原薄壳推论系冻结文档笔误并已从
        // §7 删除；退化输入显式失败优于静默黑帧，掩膜代数对全部可构造实例
        // 无特例）。错误消息含 "empty passband"。
        {
            NodeInstance thinShell = makeInstance(1, "fft_bandpass");
            thinShell.params = {assign("lowCut", 0.125), assign("highCut", 0.125)};
            const std::string message = factoryError(bandpassDescriptor, thinShell);
            RIN_CHECK_MSG(!message.empty(),
                          "薄壳 BP(0.125,0.125) 应构造期拒绝（空通带参数矛盾）");
            RIN_CHECK_MSG(message.find("empty passband") != std::string::npos,
                          "空通带错误消息含 empty passband");
        }
        // 高通 c=0.125：ρ = 0.125 > c 不成立 → 谱线与 DC 均移除（> 不含等号）；
        // 高带量化噪声（σ ≈ 0.18）无解析常数 → 参考锁定。
        {
            const std::unique_ptr<IImageNode> node =
                makeNode(highpassDescriptor, 1, {assign("cutoff", 0.125)});
            RIN_CHECK_MSG(applyMatches(*node, sineNative,
                                       refFftBytes(FftKind::Highpass, 0.125, 0.0, 0.0,
                                                   sineNative), 1),
                          "原生高通 c=0.125 边界移除谱线（ρ > c 不含等号，参考 ±1）");
        }
        // 带通严格下界：BP(0.125, 0.6) 不含 ρ == lowCut → 谱线被排除（参考锁定：
        // 若误含边界 bin，输出在正峰处 ≈ +40，参考差 ≫ 1 可判别）。
        {
            const std::unique_ptr<IImageNode> node =
                makeNode(bandpassDescriptor, 1,
                         {assign("lowCut", 0.125), assign("highCut", 0.6)});
            RIN_CHECK_MSG(applyMatches(*node, sineNative,
                                       refFftBytes(FftKind::Bandpass, 0.0, 0.125, 0.6,
                                                   sineNative), 1),
                          "原生带通 BP(0.125,0.6] 严格下界排除 ρ==lowCut（参考 ±1）");
        }
        // （原填充路径薄壳数值用例随裁决移除：lowCut == highCut 为构造期拒绝，
        // 与图像尺寸/填充路径无关，拒绝见证见上。）
    }

    // ===================================================================
    // 12) 图集成：source(Gray8)→fft_lowpass 与直接 apply 逐字节一致；
    //     bandpass lowCut>highCut 经工厂异常 → buildNodeGraph 显式 BadParam
    // ===================================================================
    {
        // 12a) 正向链路（64×48 原生 pad 64×64）。
        {
            NodeCatalog catalog;
            catalog.nodes = {sourceGrayDescriptor, lowpassDescriptor};

            WorkflowGraph graph;
            graph.nodes = {makeInstance(1, "source"), makeInstance(2, "fft_lowpass")};
            graph.connections = {conn(1, 0, 2, 0)};

            const rin::NodeGraphBuild build = rin::buildNodeGraph(graph, catalog, defaultFactory());
            RIN_CHECK(build.validation.ok);
            RIN_CHECK(build.graph != nullptr);
            RIN_CHECK_EQ(build.graph->size(), std::size_t{2});
            if (build.graph != nullptr) {
                const ImageU8 frame = makeNoise(64, 48, 0x0D0Fu);
                const std::vector<std::vector<ImageU8>> outputs = rin::runNodeGraph(
                    *build.graph, [&frame](const NodeGraph::Node&) { return frame; });
                RIN_CHECK_EQ(outputs.size(), std::size_t{2});
                RIN_CHECK_EQ(outputs[1].size(), std::size_t{1});
                RIN_CHECK(outputs[1][0].valid() && outputs[1][0].format() == PortType::Gray8);
                const std::unique_ptr<IImageNode> direct =
                    makeNode(lowpassDescriptor, 1);
                RIN_CHECK_MSG(*outputs[1][0].pixels() == *direct->apply({frame})[0].pixels(),
                              "图内 fft_lowpass 与直接 apply 逐字节一致");
                RIN_CHECK_MSG(maxByteDiff(*outputs[1][0].pixels(),
                                          refFftBytes(FftKind::Lowpass, 0.2, 0.0, 0.0, frame)) <= 1,
                              "图内 fft_lowpass（默认参数）与参考一致");
            }
        }
        // 12b) 参数矛盾：bandpass lowCut=0.6 > highCut=0.2 通过图校验（范围内
        //     合法赋值），工厂构造期拒绝 → buildNodeGraph 显式化 BadParam。
        {
            NodeCatalog catalog;
            catalog.nodes = {sourceGrayDescriptor, bandpassDescriptor};

            NodeInstance bandpass = makeInstance(2, "fft_bandpass");
            bandpass.params = {assign("lowCut", 0.6), assign("highCut", 0.2)};
            WorkflowGraph graph;
            graph.nodes = {makeInstance(1, "source"), bandpass};
            graph.connections = {conn(1, 0, 2, 0)};

            const rin::NodeGraphBuild build = rin::buildNodeGraph(graph, catalog, defaultFactory());
            RIN_CHECK(!build.validation.ok);
            RIN_CHECK(build.graph == nullptr);
            bool hasBadParam = false;
            for (const auto& issue : build.validation.issues) {
                hasBadParam = hasBadParam || issue.kind == ValidationIssueKind::BadParam;
            }
            RIN_CHECK_MSG(hasBadParam, "lowCut>highCut 工厂异常显式化为 BadParam");
        }
    }

    return rin_test::exitStatus();
}
