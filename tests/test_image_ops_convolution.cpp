// M4-04 卷积算子节点 golden 数值测试（独立验证）：include/rin/image_ops.hpp、
// src/core/image_ops.cpp（数值语义以 docs/design/image_workflow_design.md §7
// 2026-09-29 M4-04 小节冻结公式为唯一判据；本文件的 golden 期望全部由冻结公式
// 独立手推/测试内参考实现推导，不参考实现代码）。
//
// 被测面与范围（对应设计文档 §7 conv_kernel/gaussian_blur golden 项）：
// 1) 工厂 makeDefaultImageNode："gaussian_blur"/"conv_kernel" 返回实现且
//    descriptor().typeId 与请求一致；M4-06 起 FFT 族（fft_lowpass/fft_highpass/
//    fft_bandpass）已实现并归 test_image_ops_fft.cpp（M4-05 起 grayify/hist_eq
//    已实现并归 test_image_ops_histogram.cpp），未知 typeId 抛
//    std::invalid_argument 且错误消息冻结为无后缀新口径
//    "no core implementation for node type 'X'"（目录声明与工厂能力偏差
//    显式暴露，不静默）；
// 2) conv_kernel（Gray8→Gray8，相关语义核不翻转，double 累加后 floor(v+0.5)
//    饱和量化，核不自动归一化）：
//   - 构造期拒绝全分支：size 非法枚举/种类错位、kernel 缺失/种类错位/长度≠K²
//     （K=3 给 4 个、K=5 给 9 个、K=1 给 2 个）/系数 NaN/±inf、border 非法枚举/
//     种类错位；
//   - 默认参数生效（不赋值 → size="3" 单位核：以恒等输出锁 kernel/size 默认；
//     只赋全 1 核不带 border 赋值，以 1×2 图上手推值锁 border="clamp" 默认）；
//   - 单位核恒等（K=1/3/5 × clamp/reflect/zero 在 4×4 逐像素唯一图上恒等——
//     恒等同时锁边界策略对单位核无影响）；
//   - 已知核响应 golden：非对称全非零核 1..9 在 4×4 图（v=x+y）全 16 字节手推
//     golden（角点 42/147/147/252、中心 114/159/204、边缘）；单点移位核在 3×3
//     图上 clamp/reflect/zero 三策略各自逐字节 golden（clamp≠reflect 差异见证）；
//     scale 类核：K=1 2× 核 >255 饱和（128→256→255）、中心 −2/下 +1 核 <0 截 0、
//     x.5 round-half-up 半值用例（0.5×5=2.5→3，排除银行家舍入）、Laplacian 和=0
//     核（中心负响应截断 + 常值图全归零）；
//   - 1×N / N×1 窄图 K=5 reflect-101 折返手推 golden（−2→2、−1→1，与 clamp/
//     zero 逐字节可区分；n=1 图 reflect/clamp 下标归 0、zero 越界跳过）；
//   - 37×23 伪随机噪声图 × K=5 非对称核（系数全部为二进制精确 dyadic 值 → 参考
//     实现与实现求和次序无关、逐字节可比）× 三边界策略，与测试内独立 double
//     参考交叉验证；
//   - 输入带行尾 padding：输出为紧凑新缓冲（stride=宽×1）、padding 字节不泄漏、
//     结果与紧凑输入一致；
//   - 防御路径：无效输入图/输入数量≠1/输入格式与声明不符（Rgba8 喂 Gray8 声明）
//     拒绝；
//   - 图集成：source(Gray8)→conv_kernel 链 runNodeGraph 全 1 核逐字节 golden；
//     图内 kernel 长度不符经 buildNodeGraph 显式失败（BadParam，graph null）；
// 3) gaussian_blur（Gray8→Gray8，一维核 K=2·radius+1 归一化 Σ=1，可分离两趟
//    （水平→垂直）固定 clamp 边界，double 中间结果一次量化）：
//   - 构造期拒绝：radius 0/11/负、sigma 负/NaN/inf/>10、种类错位（radius 位
//     Real、sigma 位 string）、参数声明缺失；边界值 radius=1/10、sigma=0/10
//     可构造；
//   - 默认参数生效（不赋值输出与显式 radius=3 + sigma=1.5 实例逐字节一致）；
//   - sigma=0 δ 核恒等（非常数图、K=7 > 图幅仍逐字节恒等）；
//   - 常值图任意 radius/sigma 输出同常值（Σ=1 归一化的推论）；
//   - 核系数 golden：radius=1 + sigma=1.0 归一化系数 = [0.274068619, 0.451862761,
//     0.274068619]（冻结公式 exp(−(i−1)²/(2·1²)) 归一化的独立数值求值，1e-9）；
//   - 已知响应 golden：角点脉冲（5×5 黑底 src(0,0)=255）逐字节手推
//     [[134,51,0,0,0],[51,19,0,0,0],0×3]（clamp 使脉冲能量按归一化核权重折返；
//     总量自检 134+51+51+19 = 255 = Σ二维核×255，能量无泄漏）；列阶梯图逐字节
//     手推 [0,70,185,255,255]（255a≈69.89→70、255(a+b)≈185.11→185、h(3)/h(4)
//     覆盖全 255 列 → 整核权和 ×255=255）；
//   - 可分离 vs 直接卷积等价：测试内按 §7 公式自写直接 2D 卷积 double 参考
//     （clamp 边界、同款 floor(v+0.5) 饱和量化），radius∈{1,3,10}×sigma∈
//     {0.5,1.5,10} 共 9 组 × 37×23 伪随机噪声图，逐像素 |可分离 − 直接| ≤ 1；
//   - 输出 stride 紧凑（== 宽）；输入含行尾 padding 时结果与紧凑输入一致；
//   - 防御路径同 conv_kernel；
//   - 图集成：source(Gray8)→gaussian_blur（sigma=0）runNodeGraph 恒等 golden。
//
// golden 独立性说明：小图 golden 以字面字节向量写出，附手推过程注释（只依赖 §7
// 冻结公式；blur golden 涉及的 exp/归一化数值由冻结公式的独立数值求值得出）；
// 大图用例由测试内自写的 refConvolve2D/refBlurDirect2D double 参考实现交叉验证
// （按冻结公式重写，不 include 实现头）。
//
// DOD-02 适用性说明：本契约面全部为单线程纯逻辑（节点构造/apply/图编译求值均
// 顺序调用，无任务提交/队列/取消/超时/shutdown 语义，无跨上下文共享状态），
// 并发矩阵不适用（写法参照 test_image_ops_geometry.cpp 文件头）；并发行为归
// M4-07 引擎与 M5-08 契约套件。
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
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

// --- 目录构造（schema 照抄 docs/design/image_workflow_design.md §6 表格）-------

ParamDescriptor integerParam(const std::string& id, std::int64_t defaultValue, double min,
                             double max) {
    ParamDescriptor p;
    p.id = id;
    p.label = id;
    p.kind = ParamKind::Integer;
    p.defaultValue = defaultValue;
    p.hasRange = true;
    p.minValue = min;
    p.maxValue = max;
    return p;
}

ParamDescriptor enumerationParam(const std::string& id, const std::string& defaultValue,
                                 const std::vector<std::string>& options) {
    ParamDescriptor p;
    p.id = id;
    p.label = id;
    p.kind = ParamKind::Enumeration;
    p.defaultValue = defaultValue;
    p.enumOptions = options;
    return p;
}

NodeDescriptor makeSourceGrayDescriptor() {
    NodeDescriptor d;
    d.typeId = "source";
    d.displayName = "相机源";
    d.outputs = {PortType::Gray8};
    return d;
}

NodeDescriptor makeConvKernelDescriptor() {
    NodeDescriptor d;
    d.typeId = "conv_kernel";
    d.displayName = "自定义卷积";
    d.inputs = {PortType::Gray8};
    d.outputs = {PortType::Gray8};
    // §6：size: Enumeration "3" [1,3,5]；kernel: RealArray（3×3 单位核，行主序）；
    // border: Enumeration "clamp" [clamp,reflect,zero]。
    d.params.push_back(enumerationParam("size", "3", {"1", "3", "5"}));
    ParamDescriptor kernel;
    kernel.id = "kernel";
    kernel.label = "kernel";
    kernel.kind = ParamKind::RealArray;
    kernel.defaultValue = std::vector<double>{0, 0, 0, 0, 1, 0, 0, 0, 0};
    d.params.push_back(std::move(kernel));
    d.params.push_back(enumerationParam("border", "clamp", {"clamp", "reflect", "zero"}));
    return d;
}

NodeDescriptor makeGaussianBlurDescriptor() {
    NodeDescriptor d;
    d.typeId = "gaussian_blur";
    d.displayName = "高斯模糊";
    d.inputs = {PortType::Gray8};
    d.outputs = {PortType::Gray8};
    // §6：radius: Integer 3 [1,10]；sigma: Real 1.5 [0,10]。
    d.params.push_back(integerParam("radius", 3, 1, 10));
    ParamDescriptor sigma;
    sigma.id = "sigma";
    sigma.label = "sigma";
    sigma.kind = ParamKind::Real;
    sigma.defaultValue = 1.5;
    sigma.hasRange = true;
    sigma.minValue = 0.0;
    sigma.maxValue = 10.0;
    d.params.push_back(std::move(sigma));
    return d;
}

// 基础 descriptor 构造（typeId + 输入端口 + Gray8 输出；FFT 族见证在此之上补
// §6 参数 schema——M4-06 起 FFT 族已实现，工厂要求声明完整参数面）。
NodeDescriptor makeUnimplementedDescriptor(const char* typeId, PortType input) {
    NodeDescriptor d;
    d.typeId = typeId;
    d.displayName = typeId;
    d.inputs = {input};
    d.outputs = {PortType::Gray8};
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

NodeInstance convInstance(NodeId id, const std::string& size, std::vector<double> kernel,
                          const std::string& border) {
    NodeInstance instance = makeInstance(id, "conv_kernel");
    instance.params = {assign("size", size), assign("kernel", std::move(kernel)),
                       assign("border", border)};
    return instance;
}

NodeInstance blurInstance(NodeId id, std::int64_t radius, double sigma) {
    NodeInstance instance = makeInstance(id, "gaussian_blur");
    instance.params = {assign("radius", radius), assign("sigma", sigma)};
    return instance;
}

std::unique_ptr<IImageNode> convNode(const NodeDescriptor& descriptor, NodeId id,
                                     const std::string& size, std::vector<double> kernel,
                                     const std::string& border) {
    return rin::makeDefaultImageNode(descriptor, convInstance(id, size, std::move(kernel), border));
}

std::unique_ptr<IImageNode> blurNode(const NodeDescriptor& descriptor, NodeId id,
                                     std::int64_t radius, double sigma) {
    return rin::makeDefaultImageNode(descriptor, blurInstance(id, radius, sigma));
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

// --- Gray8 图像构造与核对助手 --------------------------------------------------

using GrayFn = std::function<std::uint8_t(std::uint32_t, std::uint32_t)>;

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

// 输出必须：数量 1、有效 Gray8、尺寸与输入一致、紧凑（stride == 宽）、新缓冲
//（不与输入共享像素）、逐字节等于期望。
bool grayOutputMatches(const IImageNode& node, const ImageU8& input,
                       const std::vector<std::uint8_t>& expected) {
    const std::vector<ImageU8> output = node.apply({input});
    if (output.size() != 1) {
        return false;
    }
    const ImageU8& image = output[0];
    if (!image.valid() || image.format() != PortType::Gray8) {
        return false;
    }
    if (image.width() != input.width() || image.height() != input.height()) {
        return false;
    }
    if (image.stride() != image.width()) {
        return false;  // 紧凑：Gray8 stride = 宽×1。
    }
    if (image.pixels() == input.pixels()) {
        return false;  // 新缓冲：输出与输入不共享像素对象。
    }
    if (expected.size() != image.pixels()->size()) {
        return false;
    }
    return std::equal(expected.begin(), expected.end(), image.pixels()->begin());
}

int maxAbsDiff(const ImageU8& image, const std::vector<std::uint8_t>& reference) {
    if (!image.valid() || image.stride() != image.width() ||
        reference.size() != image.pixels()->size()) {
        return 256;
    }
    int worst = 0;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        const int diff =
            static_cast<int>(image.pixels()->at(i)) - static_cast<int>(reference.at(i));
        worst = std::max(worst, std::abs(diff));
    }
    return worst;
}

bool containsPadByte(const ImageU8& image, std::uint8_t padByte) {
    for (const std::uint8_t byte : *image.pixels()) {
        if (byte == padByte) {
            return true;
        }
    }
    return false;
}

// --- 独立 double 参考实现（按设计文档 §7 冻结公式重写，不 include 实现头）----

std::uint8_t refQuantize(double value) {
    const double quantized = std::floor(value + 0.5);  // round-half-up
    return static_cast<std::uint8_t>(std::max(0.0, std::min(255.0, quantized)));
}

enum class RefBorder { Clamp, Reflect, Zero };

// §7 冻结边界映射：clamp = 下标饱和 [0,n−1]；reflect = reflect-101（−1→1、
// n→n−2，周期 2(n−1)，n=1 一律取 0）；zero = 越界以 −1 哨兵表示（该项不贡献）。
std::int64_t refBorderIndex(std::int64_t index, std::int64_t size, RefBorder border) {
    switch (border) {
    case RefBorder::Clamp:
        return std::clamp<std::int64_t>(index, 0, size - 1);
    case RefBorder::Reflect: {
        if (size <= 1) {
            return 0;
        }
        const std::int64_t period = 2 * (size - 1);
        std::int64_t folded = index % period;
        if (folded < 0) {
            folded += period;
        }
        if (folded >= size) {
            folded = period - folded;
        }
        return folded;
    }
    case RefBorder::Zero:
        return (index >= 0 && index < size) ? index : std::int64_t{-1};
    }
    return 0;
}

// 直接 2D 相关卷积参考（§7：out(x,y) = Σ_{i,j} kernel[i·K+j] ·
// src(border(y+i−r), border(x+j−r))，r=(K−1)/2，核不翻转）。
std::vector<std::uint8_t> refConvolve2D(const ImageU8& image, const std::vector<double>& kernel,
                                        int kernelSize, RefBorder border) {
    const std::int64_t width = static_cast<std::int64_t>(image.width());
    const std::int64_t height = static_cast<std::int64_t>(image.height());
    const std::int64_t radius = kernelSize / 2;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height);
    for (std::int64_t y = 0; y < height; ++y) {
        for (std::int64_t x = 0; x < width; ++x) {
            double accumulator = 0.0;
            for (std::int64_t i = 0; i < kernelSize; ++i) {
                const std::int64_t sy = refBorderIndex(y + i - radius, height, border);
                if (sy < 0) {
                    continue;  // zero 策略越界：按黑计，不贡献。
                }
                const std::uint8_t* row = image.row(static_cast<std::uint32_t>(sy));
                for (std::int64_t j = 0; j < kernelSize; ++j) {
                    const std::int64_t sx = refBorderIndex(x + j - radius, width, border);
                    if (sx < 0) {
                        continue;
                    }
                    accumulator +=
                        kernel[static_cast<std::size_t>(i) * kernelSize +
                               static_cast<std::size_t>(j)] *
                        row[static_cast<std::size_t>(sx)];
                }
            }
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                static_cast<std::size_t>(x)] = refQuantize(accumulator);
        }
    }
    return out;
}

// §7：一维核 G[i] ∝ exp(−(i−r)²/(2σ²)) 归一化 Σ=1；sigma=0 为 δ 核。
std::vector<double> refGaussianKernel1D(int radius, double sigma) {
    const std::size_t size = static_cast<std::size_t>(2 * radius + 1);
    std::vector<double> kernel(size, 0.0);
    if (sigma == 0.0) {
        kernel[static_cast<std::size_t>(radius)] = 1.0;
        return kernel;
    }
    double sum = 0.0;
    for (std::size_t i = 0; i < size; ++i) {
        const double delta = static_cast<double>(i) - static_cast<double>(radius);
        kernel[i] = std::exp(-(delta * delta) / (2.0 * sigma * sigma));
        sum += kernel[i];
    }
    for (double& value : kernel) {
        value /= sum;
    }
    return kernel;
}

// 直接 2D 高斯卷积参考（一维核的外积 + 固定 clamp + 同款量化）；用于与被测
// 可分离实现做 §7 冻结的"量化后逐像素差 ≤ 1"交叉验证。
std::vector<std::uint8_t> refBlurDirect2D(const ImageU8& image, int radius, double sigma) {
    const std::vector<double> g = refGaussianKernel1D(radius, sigma);
    const std::int64_t width = static_cast<std::int64_t>(image.width());
    const std::int64_t height = static_cast<std::int64_t>(image.height());
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height);
    for (std::int64_t y = 0; y < height; ++y) {
        for (std::int64_t x = 0; x < width; ++x) {
            double accumulator = 0.0;
            for (int i = 0; i <= 2 * radius; ++i) {
                const std::int64_t sy =
                    std::clamp<std::int64_t>(y + i - radius, 0, height - 1);
                const std::uint8_t* row = image.row(static_cast<std::uint32_t>(sy));
                for (int j = 0; j <= 2 * radius; ++j) {
                    const std::int64_t sx =
                        std::clamp<std::int64_t>(x + j - radius, 0, width - 1);
                    accumulator += g[static_cast<std::size_t>(i)] *
                                   g[static_cast<std::size_t>(j)] *
                                   row[static_cast<std::size_t>(sx)];
                }
            }
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                static_cast<std::size_t>(x)] = refQuantize(accumulator);
        }
    }
    return out;
}

// 37×23 伪随机噪声（逐像素确定、非常数、含全值域起伏；大图交叉验证专用）。
std::uint8_t noisePixel(std::uint32_t x, std::uint32_t y) {
    const int px = static_cast<int>(x);
    const int py = static_cast<int>(y);
    const int n = 5 * px + 11 * py + (7 * px * px + 3 * py) % 29;
    return static_cast<std::uint8_t>(n % 256);
}

Connection conn(NodeId from, std::uint32_t fromPort, NodeId to, std::uint32_t toPort) {
    Connection connection;
    connection.from = PortRef{from, PortDirection::Output, fromPort};
    connection.to = PortRef{to, PortDirection::Input, toPort};
    return connection;
}

}  // namespace

int main() {
    const NodeDescriptor sourceDescriptor = makeSourceGrayDescriptor();
    const NodeDescriptor convDescriptor = makeConvKernelDescriptor();
    const NodeDescriptor blurDescriptor = makeGaussianBlurDescriptor();

    // ===================================================================
    // 1) 工厂 makeDefaultImageNode：M4-04 两个类型已实现，其余仍显式拒绝
    // ===================================================================

    {
        const std::unique_ptr<IImageNode> conv =
            rin::makeDefaultImageNode(convDescriptor, makeInstance(1, "conv_kernel"));
        RIN_CHECK(conv != nullptr);
        RIN_CHECK(conv != nullptr && conv->descriptor().typeId == "conv_kernel");
        RIN_CHECK(conv != nullptr &&
                  conv->descriptor().inputs == std::vector<PortType>{PortType::Gray8});
        RIN_CHECK(conv != nullptr &&
                  conv->descriptor().outputs == std::vector<PortType>{PortType::Gray8});

        const std::unique_ptr<IImageNode> blur =
            rin::makeDefaultImageNode(blurDescriptor, makeInstance(1, "gaussian_blur"));
        RIN_CHECK(blur != nullptr);
        RIN_CHECK(blur != nullptr && blur->descriptor().typeId == "gaussian_blur");
        RIN_CHECK(blur != nullptr &&
                  blur->descriptor().inputs == std::vector<PortType>{PortType::Gray8});
        RIN_CHECK(blur != nullptr &&
                  blur->descriptor().outputs == std::vector<PortType>{PortType::Gray8});
    }

    // M4-06 起 FFT 族已实现（golden 归 test_image_ops_fft.cpp）：按 §6 schema
    // 声明后经工厂返回实现（typeId 见证）；未知 typeId 抛 invalid_argument 且
    // 错误消息冻结为无后缀新口径（原"(M4-06 FFT operators are not implemented
    // yet)"见证随 FFT 族落地而过时）。
    {
        NodeDescriptor lowpass = makeUnimplementedDescriptor("fft_lowpass", PortType::Gray8);
        {
            ParamDescriptor cutoff;
            cutoff.id = "cutoff";
            cutoff.label = "cutoff";
            cutoff.kind = ParamKind::Real;
            cutoff.defaultValue = 0.2;
            cutoff.hasRange = true;
            cutoff.minValue = 0.0;
            cutoff.maxValue = 1.0;
            lowpass.params.push_back(std::move(cutoff));
        }
        const std::unique_ptr<IImageNode> fftNode =
            rin::makeDefaultImageNode(lowpass, makeInstance(1, "fft_lowpass"));
        RIN_CHECK(fftNode != nullptr);
        RIN_CHECK(fftNode != nullptr && fftNode->descriptor().typeId == "fft_lowpass");
    }
    {
        NodeDescriptor ghost;
        ghost.typeId = "no_such_operator";
        const std::string message = factoryError(ghost, makeInstance(1, "no_such_operator"));
        RIN_CHECK_MSG(!message.empty(), "未知类型应抛 invalid_argument：no_such_operator");
        RIN_CHECK_MSG(message == "no core implementation for node type 'no_such_operator'",
                      "未知类型错误消息冻结（无后缀新口径）：no_such_operator");
    }

    // ===================================================================
    // 2) conv_kernel
    // ===================================================================

    // --- 2a) 构造期拒绝全分支 ---
    {
        // size 非法枚举选项（声明只允许 "1"|"3"|"5"）。
        for (const char* bad : {"2", "7", "", "zero", "3 "}) {
            RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] {
                (void)convNode(convDescriptor, 1, bad, std::vector<double>(9, 1.0), "clamp");
            }), std::string("size 非法枚举拒绝：") + bad);
        }
        // size 种类错位（Enumeration 位赋 Real，运行期直接构造）。
        {
            NodeInstance wrongKind = makeInstance(1, "conv_kernel");
            wrongKind.params = {assign("size", 3.0), assign("kernel", std::vector<double>(9, 1.0)),
                                assign("border", std::string("clamp"))};
            RIN_CHECK(throwsAs<std::invalid_argument>(
                [&] { (void)rin::makeDefaultImageNode(convDescriptor, wrongKind); }));
        }
        // kernel 声明缺失（参数缺失）。
        {
            NodeDescriptor missing = makeConvKernelDescriptor();
            missing.params.erase(std::remove_if(missing.params.begin(), missing.params.end(),
                                                [](const ParamDescriptor& p) {
                                                    return p.id == "kernel";
                                                }),
                                 missing.params.end());
            RIN_CHECK(throwsAs<std::invalid_argument>(
                [&] { (void)rin::makeDefaultImageNode(missing, makeInstance(1, "conv_kernel")); }));
        }
        // kernel 种类错位（RealArray 位赋 string）。
        {
            NodeInstance wrongKind = makeInstance(1, "conv_kernel");
            wrongKind.params = {assign("size", std::string("3")),
                                assign("kernel", std::string("not-an-array")),
                                assign("border", std::string("clamp"))};
            RIN_CHECK(throwsAs<std::invalid_argument>(
                [&] { (void)rin::makeDefaultImageNode(convDescriptor, wrongKind); }));
        }
        // kernel 长度 ≠ K²：K=3 给 4 个、K=5 给 9 个、K=1 给 2 个。
        RIN_CHECK(throwsAs<std::invalid_argument>([&] {
            (void)convNode(convDescriptor, 1, "3", std::vector<double>(4, 1.0), "clamp");
        }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] {
            (void)convNode(convDescriptor, 1, "5", std::vector<double>(9, 1.0), "clamp");
        }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] {
            (void)convNode(convDescriptor, 1, "1", std::vector<double>(2, 1.0), "clamp");
        }));
        // 系数含非有限值：NaN / +inf / −inf。
        {
            std::vector<double> kernel(9, 0.0);
            kernel[4] = std::numeric_limits<double>::quiet_NaN();
            RIN_CHECK(throwsAs<std::invalid_argument>(
                [&] { (void)convNode(convDescriptor, 1, "3", kernel, "clamp"); }));
        }
        RIN_CHECK(throwsAs<std::invalid_argument>([&] {
            (void)convNode(convDescriptor, 1, "1",
                           std::vector<double>{std::numeric_limits<double>::infinity()}, "clamp");
        }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] {
            (void)convNode(convDescriptor, 1, "1",
                           std::vector<double>{-std::numeric_limits<double>::infinity()},
                           "clamp");
        }));
        // border 非法枚举选项。
        for (const char* bad : {"wrap", "", "CLAMP", "mirror"}) {
            RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] {
                (void)convNode(convDescriptor, 1, "3", std::vector<double>(9, 1.0), bad);
            }), std::string("border 非法枚举拒绝：") + bad);
        }
        // border 种类错位（Enumeration 位赋 Integer）。
        {
            NodeInstance wrongKind = makeInstance(1, "conv_kernel");
            wrongKind.params = {assign("size", std::string("3")),
                                assign("kernel", std::vector<double>(9, 1.0)),
                                assign("border", std::int64_t{0})};
            RIN_CHECK(throwsAs<std::invalid_argument>(
                [&] { (void)rin::makeDefaultImageNode(convDescriptor, wrongKind); }));
        }
        // 边界内可构造：size "1"/"5" 搭配对应长度核。
        RIN_CHECK(convNode(convDescriptor, 1, "1", std::vector<double>{1.0}, "zero") != nullptr);
        RIN_CHECK(convNode(convDescriptor, 1, "5", std::vector<double>(25, 0.5), "reflect") !=
                  nullptr);
    }

    // --- 2b) 默认参数生效（不赋值 → size="3" 单位核 + border="clamp"）---
    {
        // 不赋值：3×3 单位核 → 恒等（锁 size="3" 与单位核默认；1×2 图）。
        const ImageU8 column = makeGray(1, 2, [](std::uint32_t, std::uint32_t y) {
            return static_cast<std::uint8_t>(10 * (y + 1));  // [10, 20]
        });
        const std::unique_ptr<IImageNode> defaults =
            rin::makeDefaultImageNode(convDescriptor, makeInstance(1, "conv_kernel"));
        RIN_CHECK(defaults != nullptr);
        RIN_CHECK_MSG(grayOutputMatches(*defaults, column, {10, 20}),
                      "默认参数 = K3 单位核（恒等输出）");

        // 只赋全 1 核（border 不赋值）：1×2 图上手推锁 border="clamp" 默认——
        // 手推（全 1 核 K=3，列 [10,20]，宽 1 → 列下标恒饱和 0）：
        //   out(0,0)：行 {clamp(−1),clamp(0),clamp(1)} = {0,0,1} → 3·10+3·10+3·20 = 120
        //   out(0,1)：行 {0,1,clamp(2)=1}             → 3·10+3·20+3·20 = 150
        // 对照：border="zero" → [30,30]；border="reflect" → [150,120]（均可区分）。
        const std::unique_ptr<IImageNode> onesKernel =
            convNode(convDescriptor, 1, "3", std::vector<double>(9, 1.0), "clamp");
        RIN_CHECK_MSG(grayOutputMatches(*onesKernel, column, {120, 150}),
                      "border 默认 = clamp（与 zero [30,30] / reflect [150,120] 可区分）");
    }

    // --- 2c) 单位核恒等（K=1/3/5 × 三边界策略；4×4 逐像素唯一图，恒等也锁
    //     边界策略无影响）---
    {
        const ImageU8 unique = makeGray(4, 4, [](std::uint32_t x, std::uint32_t y) {
            return static_cast<std::uint8_t>(4 * y + x);  // 0..15 逐像素唯一
        });
        const std::vector<std::uint8_t> expectedIdentity = *unique.pixels();
        for (const char* size : {"1", "3", "5"}) {
            const std::size_t k2 = size == std::string("1")
                                       ? std::size_t{1}
                                       : (size == std::string("3") ? std::size_t{9}
                                                                   : std::size_t{25});
            std::vector<double> kernel(k2, 0.0);
            kernel[k2 / 2] = 1.0;  // 中心 1 的单位核
            for (const char* border : {"clamp", "reflect", "zero"}) {
                const std::unique_ptr<IImageNode> node =
                    convNode(convDescriptor, 2, size, kernel, border);
                RIN_CHECK_MSG(grayOutputMatches(*node, unique, expectedIdentity),
                              std::string("单位核恒等 K=") + size + " border=" + border);
            }
        }
    }

    // --- 2d) 已知核响应 golden：非对称全非零核 1..9 在 4×4 图（v=x+y），
    //     clamp 边界，全 16 字节手推 ---
    // 手推（§7 相关语义核不翻转；out(x,y)=Σ kernel[i·3+j]·v(clamp(x+j−1),clamp(y+i−1))）：
    //   图（v=x+y）：行0=[0,1,2,3] 行1=[1,2,3,4] 行2=[2,3,4,5] 行3=[3,4,5,6]
    //   角点 out(0,0)：邻域行列均饱和 → (1+2+4+5)·v(0,0) + (3+6)·v(1,0)
    //                  + (7+8)·v(0,1) + 9·v(1,1) = 0+9+15+18 = 42
    //   中心 out(1,1) = 1·0+2·1+3·2 + 4·1+5·2+6·3 + 7·2+8·3+9·4 = 114
    //   中心 out(2,2) = 1·2+2·3+3·4 + 4·3+5·4+6·5 + 7·4+8·5+9·6 = 204
    //   边缘 out(0,3) = (1+2)·v(0,2)+3·v(1,2) + (4+5+7+8)·v(0,3) + (6+9)·v(1,3)
    //                 = 6+9+72+60 = 147
    //   角点 out(3,3) = [1·4+2·5+3·5] + [4·5+5·6+6·6] + [7·5+8·6+9·6] = 29+86+137 = 252
    {
        const ImageU8 image = makeGray(4, 4, [](std::uint32_t x, std::uint32_t y) {
            return static_cast<std::uint8_t>(x + y);
        });
        const std::unique_ptr<IImageNode> node =
            convNode(convDescriptor, 1, "3", {1, 2, 3, 4, 5, 6, 7, 8, 9}, "clamp");
        const std::vector<std::uint8_t> expected = {
            42,  75, 120, 147,
            81, 114, 159, 186,
            126, 159, 204, 231,
            147, 180, 225, 252,
        };
        RIN_CHECK_MSG(grayOutputMatches(*node, image, expected),
                      "3×3 核 1..9 在 4×4（v=x+y）clamp 全图 golden");
    }

    // --- 2e) 边界策略逐策略 golden：单点移位核（核[0]=1，out(x,y)=src(y−1,x−1)）
    //     在 3×3 图（v=3y+x）上，clamp/reflect/zero 各自手推 ---
    // 图：[0 1 2 / 3 4 5 / 6 7 8]；r=1，out(x,y)=src(border(y−1),border(x−1))。
    //   clamp：下标饱和 → 行 {[0,0,0],[0,0,0],[3,3,4]} 列同 →
    //     [0 0 1 / 0 0 1 / 3 3 4]
    //   reflect-101（n=3：−1→1、3→1）→ 取 (1,1),(0,1),(1,0),(0,0) 处值 →
    //     [4 3 4 / 1 0 1 / 4 3 4]
    //   zero：x−1<0 或 y−1<0 的项按黑 → 只有 x≥1 且 y≥1 保留 →
    //     [0 0 0 / 0 0 1 / 0 3 4]
    {
        const ImageU8 image = makeGray(3, 3, [](std::uint32_t x, std::uint32_t y) {
            return static_cast<std::uint8_t>(3 * y + x);
        });
        const std::vector<double> kernel = {1, 0, 0, 0, 0, 0, 0, 0, 0};
        const std::unique_ptr<IImageNode> clampNode =
            convNode(convDescriptor, 1, "3", kernel, "clamp");
        RIN_CHECK_MSG(grayOutputMatches(*clampNode, image, {0, 0, 1, 0, 0, 1, 3, 3, 4}),
                      "clamp 逐策略 golden（单点移位核）");
        const std::unique_ptr<IImageNode> reflectNode =
            convNode(convDescriptor, 1, "3", kernel, "reflect");
        RIN_CHECK_MSG(grayOutputMatches(*reflectNode, image, {4, 3, 4, 1, 0, 1, 4, 3, 4}),
                      "reflect 逐策略 golden（单点移位核）");
        const std::unique_ptr<IImageNode> zeroNode =
            convNode(convDescriptor, 1, "3", kernel, "zero");
        RIN_CHECK_MSG(grayOutputMatches(*zeroNode, image, {0, 0, 0, 0, 0, 1, 0, 3, 4}),
                      "zero 逐策略 golden（单点移位核）");
        // clamp 与 reflect 的差异见证：同一核同一图，两策略输出不同。
        {
            const std::vector<ImageU8> clampOut = clampNode->apply({image});
            const std::vector<ImageU8> reflectOut = reflectNode->apply({image});
            RIN_CHECK_EQ(clampOut.size(), std::size_t{1});
            RIN_CHECK_EQ(reflectOut.size(), std::size_t{1});
            RIN_CHECK(*clampOut[0].pixels() != *reflectOut[0].pixels());
        }
    }

    // --- 2f) scale 类核（和≠1）：饱和、截断、x.5 round-half-up、边缘检测和=0 ---
    {
        // 2× 放大核 K=1：out = 2·src；128→256 饱和 255，120→240 原样，200→400→255。
        const ImageU8 image = makeGray(1, 3, [](std::uint32_t, std::uint32_t y) {
            static const std::uint8_t values[3] = {120, 128, 200};
            return values[y];
        });
        const std::unique_ptr<IImageNode> scale2 =
            convNode(convDescriptor, 1, "1", std::vector<double>{2.0}, "clamp");
        RIN_CHECK_MSG(grayOutputMatches(*scale2, image, {240, 255, 255}),
                      "2× 核 >255 饱和（128→256→255）");

        // 中心 −2 / 下 +1 核：out = src(下) − 2·src；列 [10,20,50] →
        // [20−20, 50−40, clamp→50−100=−50] = [0, 10, −50→0]（<0 截 0）。
        const ImageU8 column = makeGray(1, 3, [](std::uint32_t, std::uint32_t y) {
            static const std::uint8_t values[3] = {10, 20, 50};
            return values[y];
        });
        const std::unique_ptr<IImageNode> negative =
            convNode(convDescriptor, 1, "3", {0, 0, 0, 0, -2, 0, 0, 1, 0}, "clamp");
        RIN_CHECK_MSG(grayOutputMatches(*negative, column, {0, 10, 0}),
                      "负响应 <0 截 0（中心 −2 / 下 +1 核）");

        // x.5 round-half-up 半值用例：K=1 核 [0.5]：0.5·3=1.5→2、0.5·5=2.5→3
        //（银行家舍入会给出 2，此用例锁定 half-up）、0.5·7=3.5→4。
        const ImageU8 halves = makeGray(1, 3, [](std::uint32_t, std::uint32_t y) {
            static const std::uint8_t values[3] = {3, 5, 7};
            return values[y];
        });
        const std::unique_ptr<IImageNode> halfUp =
            convNode(convDescriptor, 1, "1", std::vector<double>{0.5}, "clamp");
        RIN_CHECK_MSG(grayOutputMatches(*halfUp, halves, {2, 3, 4}),
                      "x.5 round-half-up（2.5→3，排除银行家舍入）");

        // Laplacian 和=0 核（边缘检测）：5×5 黑底脉冲 src(2,2)=255 →
        // 四邻 255、中心 −4·255=−1020→0、其余 0（手推逐字节）。
        const ImageU8 impulse = makeGray(5, 5, [](std::uint32_t x, std::uint32_t y) {
            return (x == 2u && y == 2u) ? std::uint8_t{255} : std::uint8_t{0};
        });
        const std::unique_ptr<IImageNode> laplacian =
            convNode(convDescriptor, 1, "3", {0, 1, 0, 1, -4, 1, 0, 1, 0}, "clamp");
        RIN_CHECK_MSG(grayOutputMatches(
                          *laplacian, impulse,
                          {0, 0, 0, 0, 0, 0, 0, 255, 0, 0, 0, 255, 0, 255, 0, 0, 0, 255, 0, 0,
                           0, 0, 0, 0, 0}),
                      "Laplacian 和=0 核：四邻 255、中心负响应截断 0");
        // 常值图 + 和=0 核：内部 0；clamp 角点/边缘倍增项为负 → 截 0 → 全图 0。
        const ImageU8 constant = makeGray(2, 2, [](std::uint32_t, std::uint32_t) {
            return std::uint8_t{100};
        });
        RIN_CHECK_MSG(grayOutputMatches(*laplacian, constant, {0, 0, 0, 0}),
                      "常值图 + 和=0 核 → 全 0（负倍增项截断）");
    }

    // --- 2g) 1×N / N×1 窄图 K=5 reflect-101 折返（含 n=1 图）---
    {
        // K=5 核仅核[0]=1：out(x,y) = src(border(y−2), border(x−2))。
        // 列图 [5,15,25,35]（1×4）：y−2 ∈ {−2,−1,0,1} → reflect-101（周期 6）：
        //   −2→2、−1→1、0→0、1→1 → out = [25, 15, 5, 15]。
        // clamp 对照 [5,5,5,15]（−2→0、−1→0）；zero 对照 [0,0,0,0]——唯一核项的
        // 列偏移 x−2 = −2 在宽 1 图上恒越界 → 该项不贡献（按黑计）。
        const ImageU8 column = makeGray(1, 4, [](std::uint32_t, std::uint32_t y) {
            return static_cast<std::uint8_t>(10 * y + 5);  // [5,15,25,35]
        });
        std::vector<double> kernel(25, 0.0);
        kernel[0] = 1.0;
        const std::unique_ptr<IImageNode> reflectNode =
            convNode(convDescriptor, 1, "5", kernel, "reflect");
        RIN_CHECK_MSG(grayOutputMatches(*reflectNode, column, {25, 15, 5, 15}),
                      "K=5 reflect-101 折返（−2→2、−1→1）");
        const std::unique_ptr<IImageNode> clampNode =
            convNode(convDescriptor, 1, "5", kernel, "clamp");
        RIN_CHECK_MSG(grayOutputMatches(*clampNode, column, {5, 5, 5, 15}),
                      "K=5 clamp 对照（与 reflect 可区分）");
        const std::unique_ptr<IImageNode> zeroNode =
            convNode(convDescriptor, 1, "5", kernel, "zero");
        RIN_CHECK_MSG(grayOutputMatches(*zeroNode, column, {0, 0, 0, 0}),
                      "K=5 zero 对照（唯一核项 x−2 恒越界 → 全 0）");

        // 行图 [5,15,25,35]（4×1）：核[i=2][j=0]=1 → out(x,y)=src(border(x−2))，
        // 高 1 → reflect/clamp 下标归 0 → out = [25, 15, 5, 15]。
        const ImageU8 row = makeGray(4, 1, [](std::uint32_t x, std::uint32_t) {
            return static_cast<std::uint8_t>(10 * x + 5);  // [5,15,25,35]
        });
        std::vector<double> kernelRow(25, 0.0);
        kernelRow[2 * 5 + 0] = 1.0;
        const std::unique_ptr<IImageNode> rowNode =
            convNode(convDescriptor, 1, "5", kernelRow, "reflect");
        RIN_CHECK_MSG(grayOutputMatches(*rowNode, row, {25, 15, 5, 15}),
                      "N×1 窄图 K=5 reflect 折返");

        // n=1 图：reflect/clamp 下标一律 0；全 1 核 25 项 → out = 25·77 → 饱和；
        // zero 策略下仅中心项在界内 → out = 77。
        const ImageU8 single = makeGray(1, 1, [](std::uint32_t, std::uint32_t) {
            return std::uint8_t{77};
        });
        std::vector<double> ones(25, 1.0);
        RIN_CHECK_MSG(grayOutputMatches(*convNode(convDescriptor, 1, "5", ones, "clamp"),
                                        single, {255}),
                      "n=1 图 K=5 全 1 核 clamp → 25·77 饱和 255");
        RIN_CHECK_MSG(grayOutputMatches(*convNode(convDescriptor, 1, "5", ones, "reflect"),
                                        single, {255}),
                      "n=1 图 K=5 全 1 核 reflect → 25·77 饱和 255");
        RIN_CHECK_MSG(grayOutputMatches(*convNode(convDescriptor, 1, "5", ones, "zero"),
                                        single, {77}),
                      "n=1 图 K=5 全 1 核 zero → 仅中心项 77");
    }

    // --- 2h) 大图交叉验证：37×23 噪声 × K=5 非对称核 × 三边界策略 ---
    // 核系数全部为二进制精确 dyadic 值（±0.25/±0.5/… 的组合），乘加过程无舍入
    // 误差 → 参考实现与被测实现求和次序无关，可逐字节比较。
    {
        const ImageU8 noise = makeGray(37, 23, noisePixel);
        const std::vector<double> kernel = {
            0.5,  -1.25, 0.0,  2.0,  0.75,
            1.0,  0.25, -0.5,  1.5,  0.0,
            -2.0, 1.0,   3.0, -0.25, 0.5,
            0.0,  1.5,   0.5, -1.0,  2.0,
            2.5,  0.0,  -0.75, 1.0, 0.25,
        };
        for (const char* border : {"clamp", "reflect", "zero"}) {
            const RefBorder refBorder = border == std::string("clamp")
                                            ? RefBorder::Clamp
                                            : (border == std::string("reflect")
                                                   ? RefBorder::Reflect
                                                   : RefBorder::Zero);
            const std::vector<std::uint8_t> reference =
                refConvolve2D(noise, kernel, 5, refBorder);
            const std::unique_ptr<IImageNode> node =
                convNode(convDescriptor, 3, "5", kernel, border);
            RIN_CHECK_MSG(grayOutputMatches(*node, noise, reference),
                          std::string("37×23 K=5 与独立参考逐字节一致（") + border + "）");
        }
    }

    // --- 2i) 输入带行尾 padding：输出紧凑新缓冲、padding 不泄漏、结果一致 ---
    {
        const GrayFn pattern = [](std::uint32_t x, std::uint32_t y) {
            return static_cast<std::uint8_t>(17 * x + 31 * y + 3);
        };
        const ImageU8 padded = makeGray(4, 3, pattern, 6);  // 每行尾 2 字节 0xEE
        RIN_CHECK(padded.valid());
        RIN_CHECK_EQ(padded.stride(), std::uint32_t{6});
        const ImageU8 compact = makeGray(4, 3, pattern);

        const std::unique_ptr<IImageNode> node =
            convNode(convDescriptor, 1, "3", {1, 2, 3, 4, 5, 6, 7, 8, 9}, "clamp");
        const std::vector<ImageU8> fromPadded = node->apply({padded});
        RIN_CHECK_EQ(fromPadded.size(), std::size_t{1});
        RIN_CHECK(fromPadded[0].valid());
        RIN_CHECK_EQ(fromPadded[0].stride(), std::uint32_t{4});  // 紧凑：不继承输入 6。
        RIN_CHECK(!containsPadByte(fromPadded[0], 0xEE));        // padding 字节不泄漏。
        RIN_CHECK_MSG(grayOutputMatches(*node, padded,
                                        *node->apply({compact})[0].pixels()),
                      "padding 输入与紧凑输入结果一致");
    }

    // --- 2j) 防御路径：无效输入图 / 输入数量 ≠ 1 / 格式与声明不符 ---
    {
        const ImageU8 input = makeGray(3, 3, [](std::uint32_t x, std::uint32_t y) {
            return static_cast<std::uint8_t>(x + y + 1);
        });
        const std::unique_ptr<IImageNode> node =
            convNode(convDescriptor, 1, "3", std::vector<double>(9, 1.0), "clamp");
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)node->apply({ImageU8{}}); }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)node->apply({}); }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)node->apply({input, input}); }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] {
            (void)node->apply({ImageU8::make(PortType::Rgba8, 3, 3)});
        }));
    }

    // --- 2k) 图集成：source(Gray8)→conv_kernel runNodeGraph golden；图内
    //     kernel 长度不符经 buildNodeGraph 显式 BadParam ---
    {
        NodeCatalog catalog;
        catalog.nodes = {sourceDescriptor, convDescriptor};
        RIN_CHECK(catalog.valid());

        // 全 1 核 3×3 在 3×2 图（v=3y+x）上的手推 golden：
        //   out(0,0) = [v(0,0)+v(0,0)+v(1,0)] + [同] + [v(0,1)+v(0,1)+v(1,1)]
        //            = 1 + 1 + 10 = 12
        //   out(1,0) = 3 + 3 + 12 = 18      out(2,0) = 5 + 5 + 14 = 24
        //   out(0,1) = 1 + 10 + 10 = 21     out(1,1) = 3 + 12 + 12 = 27
        //   out(2,1) = 5 + 14 + 14 = 33
        WorkflowGraph graph;
        graph.nodes = {makeInstance(1, "source"),
                       convInstance(2, "3", std::vector<double>(9, 1.0), "clamp")};
        graph.connections = {conn(1, 0, 2, 0)};

        const ImageNodeFactory factory = [](const NodeDescriptor& descriptor,
                                            const NodeInstance& instance) {
            return rin::makeDefaultImageNode(descriptor, instance);
        };
        const rin::NodeGraphBuild build = rin::buildNodeGraph(graph, catalog, factory);
        RIN_CHECK(build.validation.ok);
        RIN_CHECK(build.graph != nullptr);
        if (build.graph != nullptr) {
            const ImageU8 frame = makeGray(3, 2, [](std::uint32_t x, std::uint32_t y) {
                return static_cast<std::uint8_t>(3 * y + x);
            });
            const auto outputs = rin::runNodeGraph(
                *build.graph, [&frame](const NodeGraph::Node&) { return frame; });
            RIN_CHECK_EQ(outputs.size(), std::size_t{2});
            RIN_CHECK_EQ(outputs[1].size(), std::size_t{1});
            const std::vector<std::uint8_t> expectedGraph = {12, 18, 24, 21, 27, 33};
            const ImageU8& out = outputs[1][0];
            RIN_CHECK(out.valid());
            RIN_CHECK_EQ(out.format(), PortType::Gray8);
            RIN_CHECK_EQ(out.stride(), std::uint32_t{3});
            RIN_CHECK_MSG(std::equal(expectedGraph.begin(), expectedGraph.end(),
                                     out.pixels()->begin()),
                          "source→conv_kernel 图 golden（全 1 核 3×2）");
            RIN_CHECK(out.pixels() != frame.pixels());
        }

        // 图内构造期非法（kernel 长度 ≠ K²）：图校验通过（RealArray 有限、
        // 枚举合法），工厂抛异常 → buildNodeGraph 显式 BadParam，graph null。
        WorkflowGraph badGraph;
        badGraph.nodes = {makeInstance(1, "source"),
                          convInstance(2, "3", std::vector<double>(4, 1.0), "clamp")};
        badGraph.connections = {conn(1, 0, 2, 0)};
        const rin::NodeGraphBuild badBuild = rin::buildNodeGraph(badGraph, catalog, factory);
        RIN_CHECK(!badBuild.validation.ok);
        RIN_CHECK(badBuild.graph == nullptr);
        bool hasBadParam = false;
        for (const auto& issue : badBuild.validation.issues) {
            hasBadParam = hasBadParam || issue.kind == ValidationIssueKind::BadParam;
        }
        RIN_CHECK_MSG(hasBadParam, "kernel 长度不符经工厂异常显式化为 BadParam");
    }

    // ===================================================================
    // 3) gaussian_blur
    // ===================================================================

    // --- 3a) 构造期拒绝与边界值 ---
    {
        for (const std::int64_t badRadius : {std::int64_t{0}, std::int64_t{11}, std::int64_t{-1},
                                             std::int64_t{-10}}) {
            RIN_CHECK_MSG(throwsAs<std::invalid_argument>(
                              [&] { (void)blurNode(blurDescriptor, 1, badRadius, 1.5); }),
                          "radius 越界拒绝");
        }
        for (const double badSigma : {-0.5, 10.5, std::numeric_limits<double>::quiet_NaN(),
                                      std::numeric_limits<double>::infinity(),
                                      -std::numeric_limits<double>::infinity()}) {
            RIN_CHECK_MSG(throwsAs<std::invalid_argument>(
                              [&] { (void)blurNode(blurDescriptor, 1, 3, badSigma); }),
                          "sigma 越界/非有限拒绝");
        }
        // 种类错位：radius 位赋 Real、sigma 位赋 string。
        {
            NodeInstance wrongKind = makeInstance(1, "gaussian_blur");
            wrongKind.params = {assign("radius", 3.0), assign("sigma", 1.5)};
            RIN_CHECK(throwsAs<std::invalid_argument>(
                [&] { (void)rin::makeDefaultImageNode(blurDescriptor, wrongKind); }));
        }
        {
            NodeInstance wrongKind = makeInstance(1, "gaussian_blur");
            wrongKind.params = {assign("radius", std::int64_t{3}),
                                assign("sigma", std::string("1.5"))};
            RIN_CHECK(throwsAs<std::invalid_argument>(
                [&] { (void)rin::makeDefaultImageNode(blurDescriptor, wrongKind); }));
        }
        // 参数声明缺失：radius / sigma 各自从声明中移除。
        {
            NodeDescriptor missing = makeGaussianBlurDescriptor();
            missing.params.erase(std::remove_if(missing.params.begin(), missing.params.end(),
                                                [](const ParamDescriptor& p) {
                                                    return p.id == "radius";
                                                }),
                                 missing.params.end());
            RIN_CHECK(throwsAs<std::invalid_argument>(
                [&] { (void)rin::makeDefaultImageNode(missing, makeInstance(1, "gaussian_blur")); }));
        }
        {
            NodeDescriptor missing = makeGaussianBlurDescriptor();
            missing.params.erase(std::remove_if(missing.params.begin(), missing.params.end(),
                                                [](const ParamDescriptor& p) {
                                                    return p.id == "sigma";
                                                }),
                                 missing.params.end());
            RIN_CHECK(throwsAs<std::invalid_argument>(
                [&] { (void)rin::makeDefaultImageNode(missing, makeInstance(1, "gaussian_blur")); }));
        }
        // 边界值可构造：radius=1/10 × sigma=0/10。
        RIN_CHECK(blurNode(blurDescriptor, 1, 1, 0.0) != nullptr);
        RIN_CHECK(blurNode(blurDescriptor, 1, 10, 10.0) != nullptr);
    }

    // --- 3b) 默认参数生效（不赋值 == 显式 radius=3 + sigma=1.5）---
    {
        const ImageU8 noise = makeGray(37, 23, noisePixel);
        const std::unique_ptr<IImageNode> defaults =
            rin::makeDefaultImageNode(blurDescriptor, makeInstance(1, "gaussian_blur"));
        RIN_CHECK(defaults != nullptr);
        const std::unique_ptr<IImageNode> explicitDefaults = blurNode(blurDescriptor, 2, 3, 1.5);
        const std::vector<ImageU8> fromDefaults = defaults->apply({noise});
        const std::vector<ImageU8> fromExplicit = explicitDefaults->apply({noise});
        RIN_CHECK_EQ(fromDefaults.size(), std::size_t{1});
        RIN_CHECK_EQ(fromExplicit.size(), std::size_t{1});
        RIN_CHECK_MSG(*fromDefaults[0].pixels() == *fromExplicit[0].pixels(),
                      "默认参数 == radius=3 + sigma=1.5（逐字节一致）");
    }

    // --- 3c) sigma=0 δ 核恒等（非常数图；radius=3 → K=7 > 图幅仍逐字节恒等）---
    {
        const ImageU8 image = makeGray(5, 4, [](std::uint32_t x, std::uint32_t y) {
            return static_cast<std::uint8_t>(17 * x + 31 * y);
        });
        const std::vector<std::uint8_t> expected = *image.pixels();
        for (const std::int64_t radius : {std::int64_t{1}, std::int64_t{3}}) {
            const std::unique_ptr<IImageNode> node = blurNode(blurDescriptor, 1, radius, 0.0);
            RIN_CHECK_MSG(grayOutputMatches(*node, image, expected),
                          "sigma=0 δ 核恒等（含边界像素）");
        }
    }

    // --- 3d) 常值图任意 radius/sigma 输出同常值（Σ=1 归一化的推论）---
    {
        const ImageU8 constant = makeGray(6, 4, [](std::uint32_t, std::uint32_t) {
            return std::uint8_t{87};
        });
        for (const std::int64_t radius : {std::int64_t{1}, std::int64_t{3}, std::int64_t{10}}) {
            for (const double sigma : {0.5, 1.5, 10.0}) {
                const std::unique_ptr<IImageNode> node = blurNode(blurDescriptor, 1, radius, sigma);
                RIN_CHECK_MSG(grayOutputMatches(*node, constant, std::vector<std::uint8_t>(24, 87)),
                              "常值图输出同常值");
            }
        }
    }

    // --- 3e) 核系数 golden：radius=1 + sigma=1.0 ---
    // 手推（§7 公式）：G[i] ∝ exp(−(i−1)²/2) → [e^{−0.5}, 1, e^{−0.5}]，
    // 归一化 Σ=1 → [0.274068619, 0.451862761, 0.274068619]（独立数值求值）。
    {
        const std::vector<double> kernel = refGaussianKernel1D(1, 1.0);
        RIN_CHECK_EQ(kernel.size(), std::size_t{3});
        RIN_CHECK_MSG(std::abs(kernel[0] - 0.274068619) < 1e-9 &&
                          std::abs(kernel[1] - 0.451862761) < 1e-9 &&
                          std::abs(kernel[2] - 0.274068619) < 1e-9,
                      "radius=1 sigma=1.0 归一化系数 golden");
        RIN_CHECK_MSG(std::abs(kernel[0] + kernel[1] + kernel[2] - 1.0) < 1e-12, "Σ G[i] = 1");
    }

    // --- 3f) 已知响应 golden：角点脉冲（radius=1，sigma=1.0，一维核 [a,b,a]）---
    // 手推（a=0.274068619、b=0.451862761；二维核为其外积；5×5 黑底 src(0,0)=255）：
    //   水平趟（行 0）：h(0,0)=(a+b)·255（下标 −1/0 饱和到 0，脉冲按权重双计）、
    //     h(1,0)=a·255（下标 {0,1,2} → 仅 src(0,0) 命中 g0）、h(x≥2,0)=0；其余行 0。
    //   垂直趟：out(0,0)=(a+b)·h(0,0)=255(a+b)²≈134.38→134；out(1,0)=(a+b)·h(1,0)
    //     =255a(a+b)≈50.73→51；out(0,1)=a·h(0,0)→51；out(1,1)=a·h(1,0)=255a²≈19.15→19；
    //     其余全 0。
    //   质量自检：134+51+51+19 = 255 = Σ二维核 × 255（clamp 折返不增减能量）。
    {
        const ImageU8 image = makeGray(5, 5, [](std::uint32_t x, std::uint32_t y) {
            return (x == 0u && y == 0u) ? std::uint8_t{255} : std::uint8_t{0};
        });
        const std::unique_ptr<IImageNode> node = blurNode(blurDescriptor, 1, 1, 1.0);
        const std::vector<std::uint8_t> expected = {
            134, 51, 0, 0, 0,
            51,  19, 0, 0, 0,
            0,   0, 0, 0, 0,
            0,   0, 0, 0, 0,
            0,   0, 0, 0, 0,
        };
        RIN_CHECK_MSG(grayOutputMatches(*node, image, expected),
                      "角点脉冲 golden（radius=1 sigma=1.0）");
        // 质量守恒自检：脉冲能量无泄漏。
        const std::vector<ImageU8> output = node->apply({image});
        std::uint64_t total = 0;
        for (const std::uint8_t byte : *output[0].pixels()) {
            total += byte;
        }
        RIN_CHECK_EQ(total, std::uint64_t{255});
    }

    // --- 3g) 已知响应 golden：列阶梯图（radius=1，sigma=1.0）---
    // 手推：列 0-1 = 0、列 2-4 = 255，行间相同 → 垂直趟权重和 Σ=1 不改变行内分布。
    // 水平趟：h = [0, 255a, 255(a+b), 255, 255]（h(1) 下标 {0,1,2} → 仅 src(2)=255
    //   命中 g2=a；h(2) 覆盖 {1,2,3} → (a+b)·255；h(3) 覆盖 {2,3,4} 全 255 →
    //   (a+b+a)·255 = 255；h(4) 覆盖 {3,4,4} → 255）。
    // 量化：255a≈69.89→70、255(a+b)≈185.11→185。
    {
        const ImageU8 image = makeGray(5, 3, [](std::uint32_t x, std::uint32_t) {
            return x < 2u ? std::uint8_t{0} : std::uint8_t{255};
        });
        const std::unique_ptr<IImageNode> node = blurNode(blurDescriptor, 1, 1, 1.0);
        const std::vector<std::uint8_t> expectedRow = {0, 70, 185, 255, 255};
        std::vector<std::uint8_t> expected;
        for (int row = 0; row < 3; ++row) {
            expected.insert(expected.end(), expectedRow.begin(), expectedRow.end());
        }
        RIN_CHECK_MSG(grayOutputMatches(*node, image, expected),
                      "列阶梯 golden [0,70,185,255,255]");
    }

    // --- 3h) 可分离 vs 直接卷积等价（§7 冻结容差：量化后逐像素差 ≤ 1）---
    // 37×23 伪随机噪声图 × radius∈{1,3,10}×sigma∈{0.5,1.5,10}，与测试内按 §7
    // 公式自写的直接 2D double 参考（clamp + 同款量化）交叉验证。
    {
        const ImageU8 noise = makeGray(37, 23, noisePixel);
        for (const std::int64_t radius : {std::int64_t{1}, std::int64_t{3}, std::int64_t{10}}) {
            for (const double sigma : {0.5, 1.5, 10.0}) {
                const std::unique_ptr<IImageNode> node =
                    blurNode(blurDescriptor, 1, radius, sigma);
                const std::vector<ImageU8> output = node->apply({noise});
                RIN_CHECK_EQ(output.size(), std::size_t{1});
                if (output.size() != 1) {
                    continue;
                }
                const std::vector<std::uint8_t> reference =
                    refBlurDirect2D(noise, static_cast<int>(radius), sigma);
                const int worst = maxAbsDiff(output[0], reference);
                RIN_CHECK_MSG(worst <= 1, std::string("可分离 vs 直接 2D 参考 |diff| ≤ 1（r=") +
                                               std::to_string(radius) +
                                               ", σ=" + std::to_string(sigma) +
                                               "，实测 max=" + std::to_string(worst) + "）");
            }
        }
    }

    // --- 3i) 输出 stride 紧凑（== 宽）；padding 输入结果不受影响 ---
    {
        const GrayFn pattern = [](std::uint32_t x, std::uint32_t y) {
            return static_cast<std::uint8_t>(5 * x + 3 * y + 40);
        };
        const ImageU8 padded = makeGray(5, 4, pattern, 8);  // 每行尾 3 字节 0xEE
        RIN_CHECK_EQ(padded.stride(), std::uint32_t{8});
        const ImageU8 compact = makeGray(5, 4, pattern);

        const std::unique_ptr<IImageNode> node = blurNode(blurDescriptor, 1, 2, 1.25);
        const std::vector<ImageU8> fromPadded = node->apply({padded});
        RIN_CHECK_EQ(fromPadded.size(), std::size_t{1});
        RIN_CHECK(fromPadded[0].valid());
        RIN_CHECK_EQ(fromPadded[0].stride(), std::uint32_t{5});  // 紧凑：== 宽。
        RIN_CHECK(!containsPadByte(fromPadded[0], 0xEE));
        const std::vector<ImageU8> fromCompact = node->apply({compact});
        RIN_CHECK_EQ(fromCompact.size(), std::size_t{1});
        RIN_CHECK_MSG(*fromPadded[0].pixels() == *fromCompact[0].pixels(),
                      "padding 输入与紧凑输入结果一致");
    }

    // --- 3j) 防御路径：无效输入图 / 输入数量 ≠ 1 / 格式与声明不符 ---
    {
        const ImageU8 input = makeGray(3, 3, [](std::uint32_t x, std::uint32_t y) {
            return static_cast<std::uint8_t>(x + y + 1);
        });
        const std::unique_ptr<IImageNode> node = blurNode(blurDescriptor, 1, 1, 1.0);
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)node->apply({ImageU8{}}); }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)node->apply({}); }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)node->apply({input, input}); }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] {
            (void)node->apply({ImageU8::make(PortType::Rgba8, 3, 3)});
        }));
    }

    // --- 3k) 图集成：source(Gray8)→gaussian_blur（sigma=0 恒等链）golden ---
    {
        NodeCatalog catalog;
        catalog.nodes = {sourceDescriptor, blurDescriptor};
        RIN_CHECK(catalog.valid());

        WorkflowGraph graph;
        graph.nodes = {makeInstance(1, "source"), blurInstance(2, 3, 0.0)};
        graph.connections = {conn(1, 0, 2, 0)};

        const ImageNodeFactory factory = [](const NodeDescriptor& descriptor,
                                            const NodeInstance& instance) {
            return rin::makeDefaultImageNode(descriptor, instance);
        };
        const rin::NodeGraphBuild build = rin::buildNodeGraph(graph, catalog, factory);
        RIN_CHECK(build.validation.ok);
        RIN_CHECK(build.graph != nullptr);
        if (build.graph != nullptr) {
            const ImageU8 frame = makeGray(4, 3, [](std::uint32_t x, std::uint32_t y) {
                return static_cast<std::uint8_t>(13 * x + 7 * y + 1);
            });
            const auto outputs = rin::runNodeGraph(
                *build.graph, [&frame](const NodeGraph::Node&) { return frame; });
            RIN_CHECK_EQ(outputs.size(), std::size_t{2});
            RIN_CHECK_EQ(outputs[1].size(), std::size_t{1});
            const ImageU8& out = outputs[1][0];
            RIN_CHECK(out.valid());
            RIN_CHECK_EQ(out.format(), PortType::Gray8);
            RIN_CHECK_EQ(out.stride(), std::uint32_t{4});
            RIN_CHECK_MSG(std::equal(frame.pixels()->begin(), frame.pixels()->end(),
                                     out.pixels()->begin()),
                          "source→gaussian_blur(sigma=0) 图恒等 golden");
            RIN_CHECK(out.pixels() != frame.pixels());
        }
    }

    return rin_test::exitStatus();
}
