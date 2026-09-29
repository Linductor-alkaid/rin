// M4-03 几何算子节点 golden 数值测试（独立验证）：include/rin/image_ops.hpp、
// src/core/image_ops.cpp（数值语义以 docs/design/image_workflow_design.md §7 冻结
// 公式为唯一判据；本文件的 golden 期望全部由冻结公式独立手推/测试内参考实现推导，
// 不参考实现代码）。
//
// 被测面与范围（对应设计文档 §7 crop/downscale golden 项）：
// 1) 工厂 makeDefaultImageNode："source" → nullptr（注入型）；"crop"/"downscale"
//    返回实现且 descriptor().typeId 与请求一致；未实现类型（fft_lowpass，M4-04 起
//    gaussian_blur/conv_kernel 已实现并归 test_image_ops_convolution.cpp；M4-05 起
//    grayify/hist_eq 已实现并归 test_image_ops_histogram.cpp）抛
//    std::invalid_argument；
// 2) crop：像素级 golden（4×3 逐像素唯一，crop(1,1,2,2)）；全图裁切恒等；输入带
//    padding stride 时逐行 memcpy 正确且输出紧凑（stride = width×4，不继承输入
//    stride，padding 字节不泄漏）；越界拒绝（x+width > 输入宽 / y+height > 输入高 /
//    x = 输入宽且 width > 0 / 负值按 uint64 回绕落入拒绝）；退化拒绝（width=0 /
//    height=0）；默认参数（x=0,y=0,w=h=64）对 32×16 输入越界拒绝；无效输入图 /
//    输入数量 ≠ 1 / 输入格式与声明不符（Gray8 喂 Rgba8 声明）拒绝；输出与输入
//    pixels 缓冲对象不同（非共享）；参数缺失/种类错位（运行期直接构造）构造抛；
// 3) downscale：nearest 小图 golden（4×2 @0.5 → 2×1，采样下标手推）；bilinear
//    小图 golden（4×4 @0.5 → 2×2，恰含 x.5 的 round-half-up 半值用例；3×3 @0.5 →
//    1×1 = 中心像素；4×2 @0.5 逐通道独立、alpha 不预乘）；输出尺寸 floor 公式
//    （5×3 → 2×1；848×480 → 424×240，只断言尺寸）；scale=1.0 两种插值逐像素恒等
//    （37×23 渐变图 + 紧凑 stride）；常值图任意 scale 输出同常值；bilinear 线性
//    渐变 |out−理想值| ≤ 1（round-half-up 语义，水平/垂直各一）；37×23 渐变图与
//    测试内自写 double 参考实现交叉验证（nearest 精确一致、bilinear 每通道
//    |out−ref| ≤ 1）；退化输出拒绝（4×4 @0.1 → floor=0；1×32 @0.5 → 宽向退化）；
//    默认参数生效（不赋值 → nearest + 0.5，以与 bilinear 可区分的值断言）；
//    构造期拒绝（scale = 0 / 负 / 1.5 / NaN / +inf；interpolation = "bicubic"；
//    参数种类错位——Integer 位赋 Real、Real 位赋 string、声明无此参数）；
// 4) 图集成（自建 source/crop/downscale 三类型小目录，schema 照抄设计文档 §6）：
//    source→crop→downscale 链（注入 4×4 已知帧，crop(1,1,2,2) 中间产物与最终
//    1×1 逐字节 golden）；图执行中 crop 越界 → runNodeGraph 原样抛
//    std::invalid_argument；目录含未实现类型（fft_lowpass）时 buildNodeGraph
//    显式失败（validation.ok == false，issues 含 BadParam，graph 为 null）。
//
// golden 独立性说明：两组以上小图 golden 以字面字节向量写出，附手推过程注释
// （只依赖 §7 冻结公式）；大图用例由测试内自写的 refNearest/refBilinear double
// 参考实现交叉验证（按冻结公式重写，不 include 实现头）。
//
// DOD-02 适用性说明：本契约面全部为单线程纯逻辑（节点构造/apply/图编译求值均
// 顺序调用，无任务提交/队列/取消/超时/shutdown 语义，无跨上下文共享状态），
// 并发矩阵不适用（写法参照 test_image_contracts.cpp / test_node_graph.cpp 文件
// 头）；并发行为归 M4-07 引擎与 M5-08 契约套件。
#include "test_util.hpp"

#include <algorithm>
#include <array>
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

// --- 目录构造（schema 照抄 docs/design/image_workflow_design.md §6）-----------

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

NodeDescriptor makeSourceDescriptor() {
    NodeDescriptor d;
    d.typeId = "source";
    d.displayName = "相机源";
    d.outputs = {PortType::Rgba8};
    return d;
}

NodeDescriptor makeCropDescriptor() {
    NodeDescriptor d;
    d.typeId = "crop";
    d.displayName = "裁切";
    d.inputs = {PortType::Rgba8};
    d.outputs = {PortType::Rgba8};
    d.params = {integerParam("x", 0, 0, 4096), integerParam("y", 0, 0, 4096),
                integerParam("width", 64, 0, 4096), integerParam("height", 64, 0, 4096)};
    return d;
}

NodeDescriptor makeDownscaleDescriptor() {
    NodeDescriptor d;
    d.typeId = "downscale";
    d.displayName = "降分辨率";
    d.inputs = {PortType::Rgba8};
    d.outputs = {PortType::Rgba8};
    ParamDescriptor interpolation;
    interpolation.id = "interpolation";
    interpolation.label = "插值";
    interpolation.kind = ParamKind::Enumeration;
    interpolation.defaultValue = std::string("nearest");
    interpolation.enumOptions = {"nearest", "bilinear"};
    d.params.push_back(std::move(interpolation));
    ParamDescriptor scale;
    scale.id = "scale";
    scale.label = "scale";
    scale.kind = ParamKind::Real;
    scale.defaultValue = 0.5;
    scale.hasRange = true;
    scale.minValue = 0.1;
    scale.maxValue = 1.0;
    d.params.push_back(std::move(scale));
    return d;
}

// §6 的 fft_lowpass / gaussian_blur 声明（fft_lowpass 仅用于"工厂未实现该类型"与
// 图准入路径，M4-05 起 grayify/hist_eq 已实现归 test_image_ops_histogram.cpp；
// gaussian_blur 自 M4-04 起已实现，此处仅作 1c 实现签名见证）。图准入见证需
// Gray8 源（fft_lowpass 声明输入为 Gray8，Rgba8 源会在工厂前被 TypeMismatch 拒绝）。
NodeDescriptor makeSourceGrayDescriptor() {
    NodeDescriptor d;
    d.typeId = "source";
    d.displayName = "相机源";
    d.outputs = {PortType::Gray8};
    return d;
}

NodeDescriptor makeFftLowpassDescriptor() {
    NodeDescriptor d;
    d.typeId = "fft_lowpass";
    d.displayName = "FFT 低通";
    d.inputs = {PortType::Gray8};
    d.outputs = {PortType::Gray8};
    ParamDescriptor cutoff;
    cutoff.id = "cutoff";
    cutoff.label = "cutoff";
    cutoff.kind = ParamKind::Real;
    cutoff.defaultValue = 0.2;
    cutoff.hasRange = true;
    cutoff.minValue = 0.0;
    cutoff.maxValue = 1.0;
    d.params.push_back(std::move(cutoff));
    return d;
}

NodeDescriptor makeGaussianBlurDescriptor() {
    NodeDescriptor d;
    d.typeId = "gaussian_blur";
    d.displayName = "高斯模糊";
    d.inputs = {PortType::Gray8};
    d.outputs = {PortType::Gray8};
    d.params = {integerParam("radius", 3, 1, 10)};
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

NodeInstance cropInstance(NodeId id, std::int64_t x, std::int64_t y, std::int64_t width,
                          std::int64_t height) {
    NodeInstance instance = makeInstance(id, "crop");
    instance.params = {assign("x", x), assign("y", y), assign("width", width),
                       assign("height", height)};
    return instance;
}

NodeInstance downscaleInstance(NodeId id, const std::string& interpolation, double scale) {
    NodeInstance instance = makeInstance(id, "downscale");
    instance.params = {assign("interpolation", interpolation), assign("scale", scale)};
    return instance;
}

std::unique_ptr<IImageNode> cropNode(const NodeDescriptor& descriptor, std::int64_t x,
                                     std::int64_t y, std::int64_t width,
                                     std::int64_t height) {
    return rin::makeDefaultImageNode(descriptor, cropInstance(1, x, y, width, height));
}

std::unique_ptr<IImageNode> downscaleNode(const NodeDescriptor& descriptor,
                                          const std::string& interpolation, double scale) {
    return rin::makeDefaultImageNode(descriptor, downscaleInstance(1, interpolation, scale));
}

// --- 图像构造与核对助手 ------------------------------------------------------

using PixelFn = std::function<std::array<std::uint8_t, 4>(std::uint32_t, std::uint32_t)>;

// 构造紧凑（stride=0）或行尾 padding（stride=行字节数）的 Rgba8 图像；padding
// 区域填充 padByte（默认 0xEE，用于断言不泄漏进输出）。
ImageU8 makeRgba(std::uint32_t width, std::uint32_t height, const PixelFn& pixel,
                 std::uint32_t rowBytes = 0, std::uint8_t padByte = 0xEE) {
    const std::uint32_t stride = rowBytes == 0 ? width * 4u : rowBytes;
    std::vector<std::uint8_t> buffer(std::size_t{stride} * height, padByte);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::array<std::uint8_t, 4> p = pixel(x, y);
            std::memcpy(buffer.data() + std::size_t{stride} * y + std::size_t{x} * 4u,
                        p.data(), 4);
        }
    }
    auto shared = std::make_shared<const std::vector<std::uint8_t>>(std::move(buffer));
    return ImageU8::wrap(PortType::Rgba8, width, height, stride, std::move(shared));
}

std::vector<std::uint8_t> imageBytes(const ImageU8& image) {
    return *image.pixels();
}

// 输出必须是有效、紧凑（stride == width×elementSize）且逐字节等于期望的图像。
bool matchesCompact(const ImageU8& image, const std::vector<std::uint8_t>& expected) {
    if (!image.valid() || image.format() != PortType::Rgba8) {
        return false;
    }
    if (image.stride() != image.width() * 4u) {
        return false;  // 紧凑性（不继承输入 stride）。
    }
    if (expected.size() != image.pixels()->size()) {
        return false;
    }
    return std::equal(expected.begin(), expected.end(), image.pixels()->begin());
}

// 每通道 |out − ref| ≤ tolerance（大图交叉验证用；前提同 matchesCompact 的前两条）。
bool withinTolerance(const ImageU8& image, const std::vector<std::uint8_t>& reference,
                     int tolerance) {
    if (!image.valid() || image.stride() != image.width() * 4u) {
        return false;
    }
    if (reference.size() != image.pixels()->size()) {
        return false;
    }
    for (std::size_t i = 0; i < reference.size(); ++i) {
        const int diff = static_cast<int>(image.pixels()->at(i)) - static_cast<int>(reference.at(i));
        if (diff < -tolerance || diff > tolerance) {
            return false;
        }
    }
    return true;
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

// --- 独立 double 参考实现（按设计文档 §7 冻结公式重写，不 include 实现头）----

std::uint32_t floorDim(std::uint32_t inDim, double scale) {
    return static_cast<std::uint32_t>(std::floor(static_cast<double>(inDim) * scale));
}

std::vector<std::uint8_t> refNearest(const ImageU8& img, std::uint32_t outW,
                                     std::uint32_t outH) {
    const double ratioX = static_cast<double>(img.width()) / static_cast<double>(outW);
    const double ratioY = static_cast<double>(img.height()) / static_cast<double>(outH);
    std::vector<std::uint8_t> out(std::size_t{outW} * outH * 4u);
    for (std::uint32_t y = 0; y < outH; ++y) {
        const std::uint32_t srcY =
            std::min<std::uint32_t>(static_cast<std::uint32_t>(std::floor((y + 0.5) * ratioY)),
                                    img.height() - 1u);
        for (std::uint32_t x = 0; x < outW; ++x) {
            const std::uint32_t srcX =
                std::min<std::uint32_t>(static_cast<std::uint32_t>(std::floor((x + 0.5) * ratioX)),
                                        img.width() - 1u);
            const std::uint8_t* src = img.row(srcY) + std::size_t{srcX} * 4u;
            std::uint8_t* dst = out.data() + (std::size_t{y} * outW + x) * 4u;
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = src[3];
        }
    }
    return out;
}

std::vector<std::uint8_t> refBilinear(const ImageU8& img, std::uint32_t outW,
                                      std::uint32_t outH) {
    const double ratioX = static_cast<double>(img.width()) / static_cast<double>(outW);
    const double ratioY = static_cast<double>(img.height()) / static_cast<double>(outH);
    std::vector<std::uint8_t> out(std::size_t{outW} * outH * 4u);
    for (std::uint32_t y = 0; y < outH; ++y) {
        double srcYf = (y + 0.5) * ratioY - 0.5;
        srcYf = std::max(0.0, std::min(srcYf, static_cast<double>(img.height() - 1u)));
        const auto y0 = static_cast<std::uint32_t>(std::floor(srcYf));
        const double fy = srcYf - static_cast<double>(y0);
        const std::uint32_t y1 = std::min(y0 + 1u, img.height() - 1u);
        for (std::uint32_t x = 0; x < outW; ++x) {
            double srcXf = (x + 0.5) * ratioX - 0.5;
            srcXf = std::max(0.0, std::min(srcXf, static_cast<double>(img.width() - 1u)));
            const auto x0 = static_cast<std::uint32_t>(std::floor(srcXf));
            const double fx = srcXf - static_cast<double>(x0);
            const std::uint32_t x1 = std::min(x0 + 1u, img.width() - 1u);
            std::uint8_t* dst = out.data() + (std::size_t{y} * outW + x) * 4u;
            for (std::uint32_t c = 0; c < 4; ++c) {
                const double v =
                    (1.0 - fy) * ((1.0 - fx) * img.row(y0)[std::size_t{x0} * 4u + c] +
                                  fx * img.row(y0)[std::size_t{x1} * 4u + c]) +
                    fy * ((1.0 - fx) * img.row(y1)[std::size_t{x0} * 4u + c] +
                          fx * img.row(y1)[std::size_t{x1} * 4u + c]);
                const auto scaled = static_cast<int>(std::floor(v + 0.5));  // round-half-up
                dst[c] = static_cast<std::uint8_t>(std::max(0, std::min(255, scaled)));
            }
        }
    }
    return out;
}

// 37×23 渐变图（逐像素伪随机三通道，alpha 恒 255；大图交叉验证专用）。
std::array<std::uint8_t, 4> gradient37x23(std::uint32_t x, std::uint32_t y) {
    const int px = static_cast<int>(x);
    const int py = static_cast<int>(y);
    return {static_cast<std::uint8_t>((5 * px + 11 * py) % 256),
            static_cast<std::uint8_t>((13 * px + 3 * py + 37) % 256),
            static_cast<std::uint8_t>((2 * px + 23 * py + 90) % 256), std::uint8_t{255}};
}

Connection conn(NodeId from, std::uint32_t fromPort, NodeId to, std::uint32_t toPort) {
    Connection connection;
    connection.from = PortRef{from, PortDirection::Output, fromPort};
    connection.to = PortRef{to, PortDirection::Input, toPort};
    return connection;
}

}  // namespace

int main() {
    const NodeDescriptor sourceDescriptor = makeSourceDescriptor();
    const NodeDescriptor cropDescriptor = makeCropDescriptor();
    const NodeDescriptor downscaleDescriptor = makeDownscaleDescriptor();

    // ===================================================================
    // 1) 工厂 makeDefaultImageNode
    // ===================================================================

    // --- 1a) "source" → nullptr（注入型源节点）---
    RIN_CHECK(rin::makeDefaultImageNode(sourceDescriptor, makeInstance(1, "source")) == nullptr);

    // --- 1b) crop / downscale：返回实现且 descriptor().typeId 与请求一致 ---
    {
        const std::unique_ptr<IImageNode> crop =
            rin::makeDefaultImageNode(cropDescriptor, cropInstance(1, 0, 0, 2, 2));
        RIN_CHECK(crop != nullptr);
        RIN_CHECK(crop != nullptr && crop->descriptor().typeId == "crop");
        RIN_CHECK(crop != nullptr &&
                  crop->descriptor().inputs ==
                      std::vector<PortType>{PortType::Rgba8});
        RIN_CHECK(crop != nullptr &&
                  crop->descriptor().outputs ==
                      std::vector<PortType>{PortType::Rgba8});

        const std::unique_ptr<IImageNode> downscale =
            rin::makeDefaultImageNode(downscaleDescriptor, downscaleInstance(1, "nearest", 0.5));
        RIN_CHECK(downscale != nullptr);
        RIN_CHECK(downscale != nullptr && downscale->descriptor().typeId == "downscale");
    }

    // --- 1c) 实现签名见证扩展与未实现类型抛 std::invalid_argument（目录声明与
    //     工厂能力偏差显式暴露）：M4-04 起 gaussian_blur/conv_kernel 已实现
    //     （golden 归 test_image_ops_convolution.cpp）；M4-05 起 grayify/hist_eq
    //     已实现（golden 归 test_image_ops_histogram.cpp）；"未实现类型抛异常"
    //     见证改用仍在 M4-06 的 fft_lowpass 与未知 typeId 两例 ---
    {
        const NodeDescriptor gaussian = makeGaussianBlurDescriptor();
        const std::unique_ptr<IImageNode> blur =
            rin::makeDefaultImageNode(gaussian, makeInstance(1, "gaussian_blur"));
        RIN_CHECK(blur != nullptr);
        RIN_CHECK(blur != nullptr && blur->descriptor().typeId == "gaussian_blur");
        const NodeDescriptor fftLowpass = makeFftLowpassDescriptor();
        RIN_CHECK(throwsAs<std::invalid_argument>([&] {
            (void)rin::makeDefaultImageNode(fftLowpass, makeInstance(1, "fft_lowpass"));
        }));
        NodeDescriptor ghost;
        ghost.typeId = "fft_lowpass_nonexistent";
        RIN_CHECK(throwsAs<std::invalid_argument>([&] {
            (void)rin::makeDefaultImageNode(ghost, makeInstance(1, "fft_lowpass_nonexistent"));
        }));
    }

    // ===================================================================
    // 2) crop
    // ===================================================================

    // --- 2a) 像素级 golden：4×3 逐像素唯一，crop(1,1,2,2) ---
    // 手推（§7：输出 row(y) = 输入 row(y+ROI.y) 的 [ROI.x, ROI.x+width) 段）：
    //   输入像素 (px,py) = RGBA(R=4·py+px, G=R+100, B=R+200, A=255)，即
    //     行0: (0,100,200,255) (1,101,201,255) (2,102,202,255) (3,103,203,255)
    //     行1: (4,104,204,255) (5,105,205,255) (6,106,206,255) (7,107,207,255)
    //     行2: (8,108,208,255) (9,109,209,255) (10,110,210,255) (11,111,211,255)
    //   crop(1,1,2,2)：out(0,0)=in(1,1)=(5,105,205,255)  out(1,0)=in(2,1)=(6,106,206,255)
    //                 out(0,1)=in(1,2)=(9,109,209,255)  out(1,1)=in(2,2)=(10,110,210,255)
    {
        const ImageU8 input = makeRgba(4, 3, [](std::uint32_t x, std::uint32_t y) {
            const int r = static_cast<int>(4 * y + x);
            return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(r),
                                               static_cast<std::uint8_t>(r + 100),
                                               static_cast<std::uint8_t>(r + 200),
                                               std::uint8_t{255}};
        });
        const std::unique_ptr<IImageNode> node = cropNode(cropDescriptor, 1, 1, 2, 2);
        const std::vector<ImageU8> output = node->apply({input});
        RIN_CHECK_EQ(output.size(), std::size_t{1});
        RIN_CHECK(output[0].valid());
        RIN_CHECK_EQ(output[0].format(), PortType::Rgba8);
        RIN_CHECK_EQ(output[0].width(), std::uint32_t{2});
        RIN_CHECK_EQ(output[0].height(), std::uint32_t{2});
        // 输出紧凑新缓冲：stride == width×elementSize，与输入不共享像素。
        RIN_CHECK_EQ(output[0].stride(), std::uint32_t{8});
        RIN_CHECK(output[0].pixels() != input.pixels());
        RIN_CHECK(output[0].pixels().get() != input.pixels().get());
        const std::vector<std::uint8_t> expectedCrop = {
            5, 105, 205, 255,  6, 106, 206, 255,
            9, 109, 209, 255, 10, 110, 210, 255,
        };
        RIN_CHECK_MSG(matchesCompact(output[0], expectedCrop), "crop golden 4x3 @(1,1,2,2)");
    }

    // --- 2b) 全图裁切恒等：crop(0,0,4,3) 字节等于输入逻辑内容 ---
    {
        const ImageU8 input = makeRgba(4, 3, [](std::uint32_t x, std::uint32_t y) {
            const int r = static_cast<int>(4 * y + x);
            return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(r),
                                               static_cast<std::uint8_t>(r + 100),
                                               static_cast<std::uint8_t>(r + 200),
                                               std::uint8_t{255}};
        });
        const std::unique_ptr<IImageNode> node = cropNode(cropDescriptor, 0, 0, 4, 3);
        const std::vector<ImageU8> output = node->apply({input});
        RIN_CHECK_EQ(output.size(), std::size_t{1});
        RIN_CHECK(matchesCompact(output[0], imageBytes(input)));
        RIN_CHECK_EQ(output[0].width(), std::uint32_t{4});
        RIN_CHECK_EQ(output[0].height(), std::uint32_t{3});
        RIN_CHECK_EQ(output[0].stride(), std::uint32_t{16});
        // 仍是新缓冲（非共享）。
        RIN_CHECK(output[0].pixels() != input.pixels());
    }

    // --- 2c) 输入带行尾 padding：逐行 memcpy 按"输入 stride"取源行，输出紧凑、
    //     padding 字节（0xEE）不泄漏 ---
    // 手推：输入 4×2、stride=24（每行尾 8 字节 0xEE）；像素 (px,py) =
    //   RGBA(R=30·py+10+3·px, +1, +2, 255)：
    //     行0: (10,11,12) (13,14,15) (16,17,18) (19,20,21)；行1: (40,41,42) … (49,50,51)
    //   crop(1,0,2,2)：out 行0 = in 行0 的 x∈[1,3) = (13,14,15),(16,17,18)
    //                 out 行1 = in 行1 的 x∈[1,3) = (43,44,45),(46,47,48)
    {
        const ImageU8 input = makeRgba(
            4, 2,
            [](std::uint32_t x, std::uint32_t y) {
                const int r = static_cast<int>(30 * y + 10 + 3 * x);
                return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(r),
                                                   static_cast<std::uint8_t>(r + 1),
                                                   static_cast<std::uint8_t>(r + 2),
                                                   std::uint8_t{255}};
            },
            24);
        RIN_CHECK(input.valid());
        RIN_CHECK_EQ(input.stride(), std::uint32_t{24});

        const std::unique_ptr<IImageNode> node = cropNode(cropDescriptor, 1, 0, 2, 2);
        const std::vector<ImageU8> output = node->apply({input});
        RIN_CHECK_EQ(output.size(), std::size_t{1});
        // 输出紧凑：stride == 2×4 == 8（不继承输入的 24）。
        RIN_CHECK_EQ(output[0].stride(), std::uint32_t{8});
        const std::vector<std::uint8_t> expectedPadded = {
            13, 14, 15, 255, 16, 17, 18, 255,
            43, 44, 45, 255, 46, 47, 48, 255,
        };
        RIN_CHECK_MSG(matchesCompact(output[0], expectedPadded), "crop golden 4x2 stride=24 @(1,0,2,2)");
        // padding 字节不泄漏：输出缓冲中不含 0xEE。
        bool padLeak = false;
        for (const std::uint8_t byte : *output[0].pixels()) {
            padLeak = padLeak || byte == std::uint8_t{0xEE};
        }
        RIN_CHECK(!padLeak);
    }

    // --- 2d) 越界拒绝（§7：x+width > 输入宽 / y+height > 输入高，uint64 运算）---
    {
        const ImageU8 input = makeRgba(4, 3, [](std::uint32_t, std::uint32_t) {
            return std::array<std::uint8_t, 4>{1, 2, 3, 255};
        });
        const auto applyCrop = [&](std::int64_t x, std::int64_t y, std::int64_t w,
                                   std::int64_t h) {
            const std::unique_ptr<IImageNode> node = cropNode(cropDescriptor, x, y, w, h);
            return node->apply({input}).empty();
        };
        // x+width = 5 > 4；y+height = 4 > 3；x = 输入宽且 width > 0。
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)applyCrop(3, 0, 2, 3); }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)applyCrop(0, 2, 4, 2); }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)applyCrop(4, 0, 1, 1); }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)applyCrop(0, 3, 1, 1); }));
        // 负 width/height（运行期直接构造，绕过图准入范围）：≤ 0 退化 / uint64
        // 回绕越界，均必须拒绝。
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)applyCrop(0, 0, -1, 2); }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)applyCrop(0, 0, 2, -1); }));
        // 恰好贴边的 ROI 合法（闭区间语义：x+width == 输入宽）。
        {
            const std::unique_ptr<IImageNode> edge = cropNode(cropDescriptor, 3, 2, 1, 1);
            const std::vector<ImageU8> output = edge->apply({input});
            RIN_CHECK_EQ(output.size(), std::size_t{1});
            RIN_CHECK(output[0].valid());
            RIN_CHECK_EQ(output[0].width(), std::uint32_t{1});
            RIN_CHECK_EQ(output[0].height(), std::uint32_t{1});
        }
    }

    // --- 2e) 退化区域拒绝（width=0 / height=0）---
    {
        const ImageU8 input = makeRgba(4, 3, [](std::uint32_t, std::uint32_t) {
            return std::array<std::uint8_t, 4>{1, 2, 3, 255};
        });
        const std::unique_ptr<IImageNode> zeroWidth = cropNode(cropDescriptor, 0, 0, 0, 3);
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)zeroWidth->apply({input}); }));
        const std::unique_ptr<IImageNode> zeroHeight = cropNode(cropDescriptor, 0, 0, 4, 0);
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)zeroHeight->apply({input}); }));
    }

    // --- 2f) 默认参数生效（x=0,y=0,width=64,height=64）：对 32×16 输入越界拒绝 ---
    {
        const ImageU8 input = makeRgba(32, 16, [](std::uint32_t, std::uint32_t) {
            return std::array<std::uint8_t, 4>{9, 8, 7, 255};
        });
        const std::unique_ptr<IImageNode> node =
            rin::makeDefaultImageNode(cropDescriptor, makeInstance(1, "crop"));
        RIN_CHECK(node != nullptr);
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)node->apply({input}); }));

        // 对照：默认参数在足够大的输入上成功且尺寸为 64×64。
        const ImageU8 big = makeRgba(80, 80, [](std::uint32_t, std::uint32_t) {
            return std::array<std::uint8_t, 4>{9, 8, 7, 255};
        });
        const std::vector<ImageU8> output = node->apply({big});
        RIN_CHECK_EQ(output.size(), std::size_t{1});
        RIN_CHECK(output[0].valid());
        RIN_CHECK_EQ(output[0].width(), std::uint32_t{64});
        RIN_CHECK_EQ(output[0].height(), std::uint32_t{64});
        RIN_CHECK_EQ(output[0].stride(), std::uint32_t{256});
    }

    // --- 2g) 防御路径：无效输入图 / 输入数量 ≠ 1 / 格式与声明不符 ---
    {
        const ImageU8 input = makeRgba(4, 3, [](std::uint32_t, std::uint32_t) {
            return std::array<std::uint8_t, 4>{1, 2, 3, 255};
        });
        const std::unique_ptr<IImageNode> node = cropNode(cropDescriptor, 0, 0, 2, 2);

        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)node->apply({ImageU8{}}); }));  // 无效输入图。
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)node->apply({}); }));  // 0 输入。
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)node->apply({input, input}); }));  // 2 输入。
        // Gray8 输入喂 Rgba8 声明节点。
        RIN_CHECK(throwsAs<std::invalid_argument>([&] {
            (void)node->apply({ImageU8::make(PortType::Gray8, 4, 3)});
        }));
    }

    // --- 2h) 参数缺失 / 种类错位（运行期直接构造的非法实例）构造抛 ---
    {
        // 声明缺少 width 参数：参数缺失 → 构造抛。
        NodeDescriptor missing = makeCropDescriptor();
        missing.params.erase(std::remove_if(missing.params.begin(), missing.params.end(),
                                            [](const ParamDescriptor& p) {
                                                return p.id == "width";
                                            }),
                             missing.params.end());
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::makeDefaultImageNode(missing, makeInstance(1, "crop")); }));

        // 种类错位：x 声明 Integer、赋 Real → 构造抛。
        NodeInstance mismatched = makeInstance(1, "crop");
        mismatched.params = {assign("x", 0.5), assign("y", std::int64_t{0}),
                             assign("width", std::int64_t{2}), assign("height", std::int64_t{2})};
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::makeDefaultImageNode(cropDescriptor, mismatched); }));
    }

    // ===================================================================
    // 3) downscale
    // ===================================================================

    // --- 3a) nearest 小图 golden：4×2 @0.5 → 2×1 ---
    // 手推（§7：outW=floor(4×0.5)=2、outH=floor(2×0.5)=1；ratioX=4/2=2.0、
    //   ratioY=2/1=2.0；srcX(0)=floor(0.5×2)=1、srcX(1)=floor(1.5×2)=3、
    //   srcY(0)=floor(0.5×2)=1 —— 输出 = 输入行1的 x∈{1,3} 像素）：
    //   输入像素 (px,py) = RGBA(R=4·py+px, +50, +100, 255)：
    //     行0: (0,50,100,255) (1,51,101,255) (2,52,102,255) (3,53,103,255)
    //     行1: (4,54,104,255) (5,55,105,255) (6,56,106,255) (7,57,107,255)
    //   期望: (5,55,105,255) (7,57,107,255)
    {
        const ImageU8 input = makeRgba(4, 2, [](std::uint32_t x, std::uint32_t y) {
            const int r = static_cast<int>(4 * y + x);
            return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(r),
                                               static_cast<std::uint8_t>(r + 50),
                                               static_cast<std::uint8_t>(r + 100),
                                               std::uint8_t{255}};
        });
        const std::unique_ptr<IImageNode> node = downscaleNode(downscaleDescriptor, "nearest", 0.5);
        const std::vector<ImageU8> output = node->apply({input});
        RIN_CHECK_EQ(output.size(), std::size_t{1});
        RIN_CHECK_EQ(output[0].width(), std::uint32_t{2});
        RIN_CHECK_EQ(output[0].height(), std::uint32_t{1});
        RIN_CHECK_EQ(output[0].stride(), std::uint32_t{8});
        RIN_CHECK(output[0].pixels() != input.pixels());
        const std::vector<std::uint8_t> expectedNearest = {
            5, 55, 105, 255, 7, 57, 107, 255,
        };
        RIN_CHECK_MSG(matchesCompact(output[0], expectedNearest), "nearest golden 4x2 @0.5");
    }

    // --- 3b) bilinear 小图 golden：4×4 @0.5 → 2×2（恰含 x.5 半值 → round-half-up）---
    // 手推（ratioX=ratioY=4/2=2.0；srcXf(0)=0.5·2−0.5=0.5 → x0=0,fx=0.5,x1=1；
    //   srcXf(1)=1.5·2−0.5=2.5 → x0=2,fx=0.5,x1=3；y 同理 → 每输出点为 2×2 块
    //   等权均值，值恰为 x.5）：
    //   输入像素 (px,py) = RGBA(R=4·py+px, +60, +120, 255)。
    //   out(0,0): R=(0+1+4+5)/4=2.5 → floor(3.0)=3；G=62.5→63；B=122.5→123
    //   out(1,0): R=(2+3+6+7)/4=4.5 → 5；G=64.5→65；B=124.5→125
    //   out(0,1): R=(8+9+12+13)/4=10.5 → 11；G=70.5→71；B=130.5→131
    //   out(1,1): R=(10+11+14+15)/4=12.5 → 13；G=72.5→73；B=132.5→133
    {
        const ImageU8 input = makeRgba(4, 4, [](std::uint32_t x, std::uint32_t y) {
            const int r = static_cast<int>(4 * y + x);
            return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(r),
                                               static_cast<std::uint8_t>(r + 60),
                                               static_cast<std::uint8_t>(r + 120),
                                               std::uint8_t{255}};
        });
        const std::unique_ptr<IImageNode> node =
            downscaleNode(downscaleDescriptor, "bilinear", 0.5);
        const std::vector<ImageU8> output = node->apply({input});
        RIN_CHECK_EQ(output.size(), std::size_t{1});
        RIN_CHECK_EQ(output[0].width(), std::uint32_t{2});
        RIN_CHECK_EQ(output[0].height(), std::uint32_t{2});
        RIN_CHECK_EQ(output[0].stride(), std::uint32_t{8});
        const std::vector<std::uint8_t> expectedBilinear = {
            3, 63, 123, 255,   5, 65, 125, 255,
            11, 71, 131, 255, 13, 73, 133, 255,
        };
        RIN_CHECK_MSG(matchesCompact(output[0], expectedBilinear),
                      "bilinear golden 4x4 @0.5（round-half-up）");
    }

    // --- 3c) bilinear 小图 golden：3×3 @0.5 → 1×1 = 中心像素 ---
    // 手推（outW=floor(1.5)=1；ratioX=3/1=3.0；srcXf=0.5·3−0.5=1.0 → x0=1,fx=0，
    //   x1=min(2,2)=2 → 恰取中心像素 in(1,1)=(4,44,84,255)）。
    {
        const ImageU8 input = makeRgba(3, 3, [](std::uint32_t x, std::uint32_t y) {
            const int r = static_cast<int>(3 * y + x);
            return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(r),
                                               static_cast<std::uint8_t>(r + 40),
                                               static_cast<std::uint8_t>(r + 80),
                                               std::uint8_t{255}};
        });
        const std::unique_ptr<IImageNode> node =
            downscaleNode(downscaleDescriptor, "bilinear", 0.5);
        const std::vector<ImageU8> output = node->apply({input});
        RIN_CHECK_EQ(output.size(), std::size_t{1});
        RIN_CHECK_EQ(output[0].width(), std::uint32_t{1});
        RIN_CHECK_EQ(output[0].height(), std::uint32_t{1});
        RIN_CHECK_EQ(output[0].stride(), std::uint32_t{4});
        const std::vector<std::uint8_t> expectedCenter = {4, 44, 84, 255};
        RIN_CHECK_MSG(matchesCompact(output[0], expectedCenter), "bilinear golden 3x3 @0.5 → 中心像素");
    }

    // --- 3d) bilinear 逐通道独立、alpha 不预乘：4×2 @0.5 → 2×1 ---
    // 手推（行0=行1，srcYf=0.5·2−0.5=0.5 → 行间等权；srcXf 同 3b 的 x0/fx/x1；
    //   输入行: (100,50,25,0) (60,60,60,255) (200,40,80,128) (10,20,30,255)）：
    //   out(0,0): R=(100+60)/2=80  G=(50+60)/2=55  B=(25+60)/2=42.5→43  A=(0+255)/2=127.5→128
    //   out(1,0): R=(200+10)/2=105 G=(40+20)/2=30  B=(80+30)/2=55      A=(128+255)/2=191.5→192
    //   （若实现做了 alpha 预乘，out(0,0).R 将为 (0·0+60·255)/(0+255)=60 ≠ 80。）
    {
        const ImageU8 input = makeRgba(4, 2, [](std::uint32_t x, std::uint32_t) {
            constexpr std::array<std::array<std::uint8_t, 4>, 4> row{
                std::array<std::uint8_t, 4>{100, 50, 25, 0},
                std::array<std::uint8_t, 4>{60, 60, 60, 255},
                std::array<std::uint8_t, 4>{200, 40, 80, 128},
                std::array<std::uint8_t, 4>{10, 20, 30, 255}};
            return row[std::size_t(x)];
        });
        const std::unique_ptr<IImageNode> node =
            downscaleNode(downscaleDescriptor, "bilinear", 0.5);
        const std::vector<ImageU8> output = node->apply({input});
        RIN_CHECK_EQ(output.size(), std::size_t{1});
        const std::vector<std::uint8_t> expectedNoPremultiply = {
            80, 55, 43, 128, 105, 30, 55, 192,
        };
        RIN_CHECK_MSG(matchesCompact(output[0], expectedNoPremultiply),
                      "bilinear 逐通道独立、alpha 不预乘 4x2 @0.5");
    }

    // --- 3e) 输出尺寸 floor 公式（只断言尺寸）：5×3 @0.5 → 2×1；848×480 @0.5 → 424×240 ---
    {
        const std::unique_ptr<IImageNode> node = downscaleNode(downscaleDescriptor, "nearest", 0.5);
        {
            const ImageU8 input = makeRgba(5, 3, [](std::uint32_t, std::uint32_t) {
                return std::array<std::uint8_t, 4>{1, 2, 3, 255};
            });
            const std::vector<ImageU8> output = node->apply({input});
            RIN_CHECK_EQ(output.size(), std::size_t{1});
            RIN_CHECK(output[0].valid());
            RIN_CHECK_EQ(output[0].width(), std::uint32_t{2});
            RIN_CHECK_EQ(output[0].height(), std::uint32_t{1});
            RIN_CHECK_EQ(output[0].stride(), std::uint32_t{8});
        }
        {
            // 主流流配置幅面：848×480 → floor → 424×240（不四舍五入）。
            const ImageU8 input = makeRgba(848, 480, [](std::uint32_t x, std::uint32_t y) {
                return gradient37x23(x % 37u, y % 23u);
            });
            const std::vector<ImageU8> output = node->apply({input});
            RIN_CHECK_EQ(output.size(), std::size_t{1});
            RIN_CHECK(output[0].valid());
            RIN_CHECK_EQ(output[0].width(), std::uint32_t{424});
            RIN_CHECK_EQ(output[0].height(), std::uint32_t{240});
            RIN_CHECK_EQ(output[0].stride(), std::uint32_t{424 * 4u});
        }
    }

    // --- 3f) scale = 1.0 两种插值逐像素恒等（37×23 渐变图；公式自然给出，无特例）---
    {
        const ImageU8 input = makeRgba(37, 23, gradient37x23);
        for (const char* interpolation : {"nearest", "bilinear"}) {
            const std::unique_ptr<IImageNode> node =
                downscaleNode(downscaleDescriptor, interpolation, 1.0);
            const std::vector<ImageU8> output = node->apply({input});
            RIN_CHECK_EQ(output.size(), std::size_t{1});
            RIN_CHECK_MSG(matchesCompact(output[0], imageBytes(input)),
                          std::string("scale=1.0 恒等（") + interpolation + "）");
            RIN_CHECK_EQ(output[0].width(), std::uint32_t{37});
            RIN_CHECK_EQ(output[0].height(), std::uint32_t{23});
            RIN_CHECK_EQ(output[0].stride(), std::uint32_t{37 * 4u});
            RIN_CHECK(output[0].pixels() != input.pixels());  // 恒等也是新缓冲。
        }
    }

    // --- 3g) 常值图任意 scale 输出同常值（两种插值）---
    {
        const auto constantPixel = [](std::uint32_t, std::uint32_t) {
            return std::array<std::uint8_t, 4>{std::uint8_t{200}, std::uint8_t{30},
                                               std::uint8_t{250}, std::uint8_t{255}};
        };
        const ImageU8 input = makeRgba(7, 5, constantPixel);
        for (const double scale : {0.3, 0.5, 0.75, 1.0}) {
            for (const char* interpolation : {"nearest", "bilinear"}) {
                const std::unique_ptr<IImageNode> node =
                    downscaleNode(downscaleDescriptor, interpolation, scale);
                const std::vector<ImageU8> output = node->apply({input});
                RIN_CHECK_EQ(output.size(), std::size_t{1});
                // 期望缓冲按"输出尺寸"重建（尺寸由 floor 公式给出），逐像素同常量。
                std::vector<std::uint8_t> expectedConstant;
                if (output[0].valid()) {
                    const std::array<std::uint8_t, 4> constant = constantPixel(0, 0);
                    expectedConstant.reserve(std::size_t{output[0].width()} *
                                             output[0].height() * 4u);
                    for (std::uint32_t i = 0; i < output[0].width() * output[0].height();
                         ++i) {
                        expectedConstant.insert(expectedConstant.end(), constant.begin(),
                                                constant.end());
                    }
                }
                RIN_CHECK_MSG(matchesCompact(output[0], expectedConstant),
                              std::string("常值图 @") + (interpolation) + " scale=" +
                                  std::to_string(scale));
            }
        }
    }

    // --- 3h) bilinear 线性渐变 |out − 理想值| ≤ 1（round-half-up 语义）---
    // 手推（33×2 @0.5 → 16×1；ratioX=33/16=2.0625（二进制精确）；行相同 → 行间
    //   插值恒等；srcXf(outX) = (outX+0.5)×2.0625 − 0.5 即理想连续采样坐标，
    //   R 通道值 = 坐标本身。所有中间量二进制精确 → 逐点等于 floor(理想+0.5)）。
    {
        const auto rampX = [](std::uint32_t x, std::uint32_t) {
            return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(x),
                                               static_cast<std::uint8_t>(255u - x),
                                               static_cast<std::uint8_t>(2u * x),
                                               std::uint8_t{255}};
        };
        const ImageU8 horizontal = makeRgba(33, 2, rampX);
        const std::unique_ptr<IImageNode> node =
            downscaleNode(downscaleDescriptor, "bilinear", 0.5);
        const std::vector<ImageU8> output = node->apply({horizontal});
        RIN_CHECK_EQ(output.size(), std::size_t{1});
        RIN_CHECK_EQ(output[0].width(), std::uint32_t{16});
        RIN_CHECK_EQ(output[0].height(), std::uint32_t{1});

        const double ratioX = 33.0 / 16.0;
        bool toleranceOk = true;
        bool exactOk = true;
        for (std::uint32_t x = 0; x < 16; ++x) {
            const double ideal = (static_cast<double>(x) + 0.5) * ratioX - 0.5;
            const std::uint8_t* p = output[0].row(0) + std::size_t{x} * 4u;
            // 逐通道 round-half-up 精确预测：floor(该通道理想值 + 0.5)。注意
            // floor(v+0.5) 对每通道独立施加，不能先对坐标取整再线性变换
            // （floor(2i+0.5) ≠ 2·floor(i+0.5)，G/B 通道必须用各自理想值）。
            const int expectedR = static_cast<int>(std::floor(ideal + 0.5));
            const int expectedG = static_cast<int>(std::floor((255.0 - ideal) + 0.5));
            const int expectedB = static_cast<int>(std::floor((2.0 * ideal) + 0.5));
            toleranceOk = toleranceOk && std::abs(static_cast<int>(p[0]) - ideal) <= 1.0 &&
                          std::abs(255 - static_cast<int>(p[1]) - ideal) <= 1.0 &&
                          std::abs(static_cast<int>(p[2]) - 2.0 * ideal) <= 1.0 && p[3] == 255;
            exactOk = exactOk && p[0] == expectedR && p[1] == expectedG &&
                      p[2] == expectedB && p[3] == 255;
        }
        RIN_CHECK_MSG(toleranceOk, "bilinear 水平线性渐变 |out−理想| ≤ 1");
        RIN_CHECK_MSG(exactOk, "bilinear 水平线性渐变 round-half-up 精确预测");

        // 垂直渐变（33 行 × 2 列）：ratioY 主导，验证 y 轴同一语义。
        const auto rampY = [](std::uint32_t, std::uint32_t y) {
            return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(y),
                                               static_cast<std::uint8_t>(255u - y),
                                               std::uint8_t{255}, std::uint8_t{255}};
        };
        const ImageU8 vertical = makeRgba(2, 33, rampY);
        const std::vector<ImageU8> vOutput = node->apply({vertical});
        RIN_CHECK_EQ(vOutput.size(), std::size_t{1});
        RIN_CHECK_EQ(vOutput[0].width(), std::uint32_t{1});
        RIN_CHECK_EQ(vOutput[0].height(), std::uint32_t{16});
        const double ratioY = 33.0 / 16.0;
        bool verticalOk = true;
        for (std::uint32_t y = 0; y < 16; ++y) {
            const double ideal = (static_cast<double>(y) + 0.5) * ratioY - 0.5;
            const std::uint8_t* p = vOutput[0].row(y);
            verticalOk = verticalOk && std::abs(static_cast<int>(p[0]) - ideal) <= 1.0 &&
                         std::abs(255 - static_cast<int>(p[1]) - ideal) <= 1.0 && p[3] == 255;
        }
        RIN_CHECK_MSG(verticalOk, "bilinear 垂直线性渐变 |out−理想| ≤ 1");
    }

    // --- 3i) 大图交叉验证：37×23 渐变 @0.5 → 18×11，对测试内独立 double 参考 ---
    {
        const ImageU8 input = makeRgba(37, 23, gradient37x23);
        const std::uint32_t outW = floorDim(37, 0.5);
        const std::uint32_t outH = floorDim(23, 0.5);
        RIN_CHECK_EQ(outW, std::uint32_t{18});
        RIN_CHECK_EQ(outH, std::uint32_t{11});

        {
            const std::unique_ptr<IImageNode> node =
                downscaleNode(downscaleDescriptor, "nearest", 0.5);
            const std::vector<ImageU8> output = node->apply({input});
            RIN_CHECK_EQ(output.size(), std::size_t{1});
            RIN_CHECK_MSG(matchesCompact(output[0], refNearest(input, outW, outH)),
                          "nearest 37×23 与独立参考实现精确一致");
        }
        {
            const std::unique_ptr<IImageNode> node =
                downscaleNode(downscaleDescriptor, "bilinear", 0.5);
            const std::vector<ImageU8> output = node->apply({input});
            RIN_CHECK_EQ(output.size(), std::size_t{1});
            RIN_CHECK_MSG(withinTolerance(output[0], refBilinear(input, outW, outH), 1),
                          "bilinear 37×23 与独立参考实现每通道 |out−ref| ≤ 1");
        }
    }

    // --- 3j) 退化输出拒绝（任一维 floor < 1）---
    {
        const ImageU8 input = makeRgba(4, 4, [](std::uint32_t, std::uint32_t) {
            return std::array<std::uint8_t, 4>{1, 2, 3, 255};
        });
        const std::unique_ptr<IImageNode> tiny = downscaleNode(downscaleDescriptor, "nearest", 0.1);
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)tiny->apply({input}); }));
        // 单维退化：1×32 @0.5 → outW = floor(0.5) = 0。
        const ImageU8 thin = makeRgba(1, 32, [](std::uint32_t, std::uint32_t) {
            return std::array<std::uint8_t, 4>{1, 2, 3, 255};
        });
        const std::unique_ptr<IImageNode> node = downscaleNode(downscaleDescriptor, "nearest", 0.5);
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)node->apply({thin}); }));
    }

    // --- 3k) 默认参数生效（不赋值 → nearest + scale 0.5）---
    // 判据：4×2 输入下 nearest 结果 (5,55,105,255)(7,57,107,255) 与 bilinear 结果
    // (3,53,103,255)(5,55,105,255) 可区分（手推见 3a 与本注释）；默认参数必须
    // 命中 nearest 的值。
    {
        const ImageU8 input = makeRgba(4, 2, [](std::uint32_t x, std::uint32_t y) {
            const int r = static_cast<int>(4 * y + x);
            return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(r),
                                               static_cast<std::uint8_t>(r + 50),
                                               static_cast<std::uint8_t>(r + 100),
                                               std::uint8_t{255}};
        });
        const std::unique_ptr<IImageNode> node =
            rin::makeDefaultImageNode(downscaleDescriptor, makeInstance(1, "downscale"));
        RIN_CHECK(node != nullptr);
        const std::vector<ImageU8> output = node->apply({input});
        RIN_CHECK_EQ(output.size(), std::size_t{1});
        RIN_CHECK_EQ(output[0].width(), std::uint32_t{2});
        RIN_CHECK_EQ(output[0].height(), std::uint32_t{1});
        const std::vector<std::uint8_t> expectedDefault = {5, 55, 105, 255, 7, 57, 107, 255};
        RIN_CHECK_MSG(matchesCompact(output[0], expectedDefault),
                      "默认参数 = nearest + 0.5（与 bilinear 值可区分）");
    }

    // --- 3l) 构造期拒绝：scale 违反 (0,1]/有限、interpolation 非法、种类错位、参数缺失 ---
    {
        const auto makeScaleInstance = [](double scale) {
            NodeInstance instance = makeInstance(1, "downscale");
            instance.params = {assign("interpolation", std::string("nearest")),
                               assign("scale", scale)};
            return instance;
        };
        const auto accepts = [&](const NodeInstance& instance) {
            return rin::makeDefaultImageNode(downscaleDescriptor, instance) != nullptr;
        };

        // scale = 0 / 负 / >1 / NaN / +inf：构造抛。
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::makeDefaultImageNode(downscaleDescriptor, makeScaleInstance(0.0)); }));
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::makeDefaultImageNode(downscaleDescriptor, makeScaleInstance(-0.25)); }));
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::makeDefaultImageNode(downscaleDescriptor, makeScaleInstance(1.5)); }));
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::makeDefaultImageNode(downscaleDescriptor, makeScaleInstance(2.0)); }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] {
            (void)rin::makeDefaultImageNode(downscaleDescriptor,
                                            makeScaleInstance(std::numeric_limits<double>::quiet_NaN()));
        }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] {
            (void)rin::makeDefaultImageNode(downscaleDescriptor,
                                            makeScaleInstance(std::numeric_limits<double>::infinity()));
        }));
        // 边界内可构造（scale=1.0 见 3f；此处补 scale 下邻域）。
        RIN_CHECK(accepts(makeScaleInstance(0.1)));
        RIN_CHECK(accepts(makeScaleInstance(1.0)));

        // interpolation 非法选项：构造抛。
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)downscaleNode(downscaleDescriptor, "bicubic", 0.5); }));
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)downscaleNode(downscaleDescriptor, "", 0.5); }));
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)downscaleNode(downscaleDescriptor, "NEAREST", 0.5); }));

        // 种类错位（运行期直接构造）：interpolation 位赋 Real、scale 位赋 string。
        NodeInstance wrongKind = makeInstance(1, "downscale");
        wrongKind.params = {assign("interpolation", 1.0), assign("scale", 0.5)};
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::makeDefaultImageNode(downscaleDescriptor, wrongKind); }));
        NodeInstance wrongKind2 = makeInstance(1, "downscale");
        wrongKind2.params = {assign("interpolation", std::string("nearest")),
                             assign("scale", std::string("0.5"))};
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::makeDefaultImageNode(downscaleDescriptor, wrongKind2); }));

        // 参数缺失：声明中没有 scale / interpolation 参数声明。
        NodeDescriptor missingScale = makeDownscaleDescriptor();
        missingScale.params.erase(std::remove_if(missingScale.params.begin(),
                                                 missingScale.params.end(),
                                                 [](const ParamDescriptor& p) {
                                                     return p.id == "scale";
                                                 }),
                                  missingScale.params.end());
        RIN_CHECK(throwsAs<std::invalid_argument>(
            [&] { (void)rin::makeDefaultImageNode(missingScale, makeInstance(1, "downscale")); }));
        NodeDescriptor missingInterp = makeDownscaleDescriptor();
        missingInterp.params.erase(
            std::remove_if(missingInterp.params.begin(), missingInterp.params.end(),
                           [](const ParamDescriptor& p) { return p.id == "interpolation"; }),
            missingInterp.params.end());
        RIN_CHECK(throwsAs<std::invalid_argument>([&] {
            (void)rin::makeDefaultImageNode(missingInterp, makeInstance(1, "downscale"));
        }));
    }

    // ===================================================================
    // 4) 图集成（buildNodeGraph + runNodeGraph，三类型小目录）
    // ===================================================================

    // --- 4a) source→crop→downscale 链：注入 4×4 已知帧，逐级 golden ---
    // 手推：4×4 渐变（R=4·py+px, +60, +120, 255）→ crop(1,1,2,2)：
    //   (5,65,125,255)(6,66,126,255) / (9,69,129,255)(10,70,130,255)
    //   → nearest @0.5 → 1×1：srcX=srcY=floor(0.5×2)=1 → crop_out(1,1)=(10,70,130,255)。
    {
        NodeCatalog catalog;
        catalog.nodes = {sourceDescriptor, cropDescriptor, downscaleDescriptor};
        RIN_CHECK(catalog.valid());

        WorkflowGraph graph;
        graph.nodes = {makeInstance(1, "source"), cropInstance(2, 1, 1, 2, 2),
                       downscaleInstance(3, "nearest", 0.5)};
        graph.connections = {conn(1, 0, 2, 0), conn(2, 0, 3, 0)};

        const ImageNodeFactory factory = [](const NodeDescriptor& descriptor,
                                            const NodeInstance& instance) {
            return rin::makeDefaultImageNode(descriptor, instance);
        };
        const rin::NodeGraphBuild build = rin::buildNodeGraph(graph, catalog, factory);
        RIN_CHECK(build.validation.ok);
        RIN_CHECK(build.graph != nullptr);
        if (build.graph != nullptr) {
            const ImageU8 frame = makeRgba(4, 4, [](std::uint32_t x, std::uint32_t y) {
                const int r = static_cast<int>(4 * y + x);
                return std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(r),
                                                   static_cast<std::uint8_t>(r + 60),
                                                   static_cast<std::uint8_t>(r + 120),
                                                   std::uint8_t{255}};
            });
            const auto outputs = rin::runNodeGraph(
                *build.graph, [&frame](const NodeGraph::Node&) { return frame; });
            RIN_CHECK_EQ(outputs.size(), std::size_t{3});

            // 中间产物：crop 输出 2×2 逐字节 golden、紧凑新缓冲。
            RIN_CHECK_EQ(outputs[1].size(), std::size_t{1});
            const std::vector<std::uint8_t> expectedCropInGraph = {
                5, 65, 125, 255,  6, 66, 126, 255,
                9, 69, 129, 255, 10, 70, 130, 255,
            };
            RIN_CHECK_MSG(matchesCompact(outputs[1][0], expectedCropInGraph),
                          "图内 crop(1,1,2,2) 中间产物");
            RIN_CHECK(outputs[1][0].pixels() != frame.pixels());

            // 最终输出：downscale → 1×1。
            RIN_CHECK_EQ(outputs[2].size(), std::size_t{1});
            const std::vector<std::uint8_t> expectedFinal = {10, 70, 130, 255};
            RIN_CHECK_MSG(matchesCompact(outputs[2][0], expectedFinal),
                          "图内 downscale @0.5 最终产物");
            RIN_CHECK(outputs[2][0].pixels() != outputs[1][0].pixels());
        }
    }

    // --- 4b) 图执行中 crop 越界：runNodeGraph 原样抛 std::invalid_argument ---
    {
        NodeCatalog catalog;
        catalog.nodes = {sourceDescriptor, cropDescriptor};

        WorkflowGraph graph;
        graph.nodes = {makeInstance(1, "source"), cropInstance(2, 0, 0, 8, 8)};
        graph.connections = {conn(1, 0, 2, 0)};

        const ImageNodeFactory factory = [](const NodeDescriptor& descriptor,
                                            const NodeInstance& instance) {
            return rin::makeDefaultImageNode(descriptor, instance);
        };
        const rin::NodeGraphBuild build = rin::buildNodeGraph(graph, catalog, factory);
        RIN_CHECK(build.validation.ok);  // ROI 合法性是 apply 期判定（图像尺寸运行期才知）。
        RIN_CHECK(build.graph != nullptr);
        if (build.graph != nullptr) {
            const ImageU8 frame = makeRgba(4, 4, [](std::uint32_t, std::uint32_t) {
                return std::array<std::uint8_t, 4>{1, 2, 3, 255};
            });
            RIN_CHECK(throwsAs<std::invalid_argument>([&] {
                (void)rin::runNodeGraph(*build.graph,
                                        [&frame](const NodeGraph::Node&) { return frame; });
            }));
        }
    }

    // --- 4c) 目录含未实现类型（fft_lowpass）：buildNodeGraph 显式失败（BadParam）---
    // （M4-05 起 grayify/hist_eq 已实现，见证链改用仍在 M4-06 的 fft_lowpass；
    // 源声明 Gray8 输出以通过端口类型核对、精确触达工厂异常路径。）
    {
        NodeCatalog catalog;
        catalog.nodes = {makeSourceGrayDescriptor(), makeFftLowpassDescriptor()};

        WorkflowGraph graph;
        graph.nodes = {makeInstance(1, "source"), makeInstance(2, "fft_lowpass")};
        graph.connections = {conn(1, 0, 2, 0)};

        const ImageNodeFactory factory = [](const NodeDescriptor& descriptor,
                                            const NodeInstance& instance) {
            return rin::makeDefaultImageNode(descriptor, instance);
        };
        const rin::NodeGraphBuild build = rin::buildNodeGraph(graph, catalog, factory);
        RIN_CHECK(!build.validation.ok);
        RIN_CHECK(build.graph == nullptr);
        bool hasBadParam = false;
        for (const auto& issue : build.validation.issues) {
            hasBadParam = hasBadParam || issue.kind == ValidationIssueKind::BadParam;
        }
        RIN_CHECK_MSG(hasBadParam, "未实现类型经工厂异常显式化为 BadParam");
    }

    return rin_test::exitStatus();
}
