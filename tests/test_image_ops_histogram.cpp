// M4-05 直方图均衡与灰度化算子节点 golden 数值测试（独立验证）：
// include/rin/image_ops.hpp、src/core/image_ops.cpp（数值语义以
// docs/design/image_workflow_design.md §7 2026-09-29 M4-05 小节冻结公式为唯一
// 判据；本文件的 golden 期望全部由冻结公式独立手推/测试内参考实现推导，不参考
// 实现代码）。
//
// 被测面与范围（对应设计文档 §7 grayify/hist_eq golden 项）：
// 1) 工厂 makeDefaultImageNode："grayify"/"hist_eq" 返回实现且 descriptor().typeId
//    与请求一致、端口签名与 §6 目录一致（grayify Rgba8→Gray8、hist_eq Gray8→Gray8，
//    均无参数——无参数节点无构造期拒绝分支，不发明参数）；仍未实现类型（M4-06
//    FFT 族 fft_lowpass）抛 std::invalid_argument 且错误消息冻结为 "(M4-06 FFT
//    operators are not implemented yet)"（目录声明与工厂能力偏差显式暴露，不静默）；
// 2) grayify（Rgba8→Gray8，BT.601 定点亮度 Y = (77·R + 150·G + 29·B + 128) >> 8，
//    +128 右移 8 位即对 Y/256 的 round-half-up，Σ系数 = 256 保证 [0,255] 自然有界，
//    纯整数无浮点，alpha 不参与）：
//   - 通道错位判别 golden：单通道极值像素 1×1 逐字节断言（手推 R=255 → 77、
//     G=255 → 149、B=255 → 29，三值互异——若实现误按 BGR 读字节序，R/B 两例
//     的期望值互换即判别；G 例钉死中间字节权重）；
//   - round-half-up 半值边界：像素 (2,0,158) 的加权和 S = 77·2+29·158 = 4736 =
//     18.5×256（S ≡ 128 mod 256）→ Y = 19（.5 上取整，排除银行家舍入/截断）；
//   - 全黑/全白极值（0/255）与 4×2 混合小图全 8 字节手推 golden（含不同 alpha
//     值不影响亮度）；
//   - alpha 不参与：同 RGB 不同 alpha（0/7/128/255）输出逐字节相同；
//   - 输入行尾 padding（stride = 宽×4+4，padding 字节 0xEE）：输出与紧凑输入一致、
//     输出紧凑（stride == 宽、format Gray8、尺寸不变）、padding 不泄漏；
//   - 37×23 伪随机噪声图与测试内独立参考实现 refGrayify 逐字节交叉验证；
//   - 防御路径：输入数量 0/2、无效图、格式错配（Gray8 喂 Rgba8 声明）抛
//     std::invalid_argument；
//   - 图集成：source→grayify 单帧求值与直接 apply 逐字节一致；
// 3) hist_eq（Gray8→Gray8，cdf_min 映射 + 整数精确 round-half-up：out[v] =
//    floor((2·num+den)/(2·den))，num = (cdf[v]−cdf_min)·255、den = N−cdf_min；
//    最小/最大出现灰度精确映射 0/255；常值图（分母为零）定义为恒等输出）：
//   - 已知直方图 golden：6×4 图直方图 hist{2:3, 5:5, 100:9, 200:7}（N=24、
//     cdf_min=3、den=21），出现值 {2,5,100,200} → {0,61,170,255} 手推（注释含
//     cdf/num/den 中间量核算），全图 24 字节逐字节断言；行尾 padding 变体与紧凑
//     输入一致（若 padding 泄漏进直方图，0xEE=238 > 200 会引入新 v_max 使全部
//     映射偏移，可判别）；
//   - 极值映射：最小出现灰度 → 0、最大出现灰度 → 255（4×1 {7,7,9,9} 手推）；
//   - 常值图恒等：全 7 的 5×3 与 1×1、全 255 的 3×2、全 0 的 4×4——输出逐字节
//     等于输入（不是全白/全黑），且为新紧凑缓冲（不共享源像素）；
//   - 全量程均匀直方图精确恒等：256×2 图每级计数 2 → 逐字节等于输入（公式精确
//     给出 out[v] = v，非近似；含 padding 变体）；
//   - 子区间均匀线性重映射：[40,47] 每级 3 次（8×3）→ out[40+j] =
//     round-half-up(255·j/7) 手推 {0,36,73,109,146,182,219,255}（逐 j 附整数式
//     核算）；[10,40] 每级 2 次（31×2）锁定半值边界：out[11] =
//     round-half-up(255/30) = round-half-up(8.5) = 9（银行家舍入会得 8）；
//   - 输出为同尺寸新紧凑缓冲、不共享源像素（共享指针与数据指针均不同）；
//   - 防御路径同 grayify（格式错配为 Rgba8 喂 Gray8 声明）；
//   - 图集成（§7"RGBA 亮度域与 Gray8 路径一致"）：source→grayify→hist_eq 链路
//     输出与"参考亮度 Gray8 图（按冻结 grayify 公式推得）直接经 hist_eq"逐字节
//     一致（另以 source(Gray8)→hist_eq 单节点图对照）；
//   - 37×23 伪随机噪声图与测试内独立参考实现 refHistEq 逐字节交叉验证。
//
// golden 独立性说明：小图 golden 以字面字节向量写出，附手推过程注释（只依赖 §7
// 冻结公式）；大图用例由测试内自写的 refGrayify/refHistEq 整数参考实现交叉验证
// （按冻结公式重写，不 include 实现头）。
//
// DOD-02 适用性说明：本契约面全部为单线程纯逻辑（节点构造/apply/图编译求值均
// 顺序调用，无任务提交/队列/取消/超时/shutdown 语义，无跨上下文共享状态），
// 并发矩阵不适用（写法参照 test_image_ops_geometry.cpp 文件头）；并发行为归
// M4-07 引擎与 M5-08 契约套件。
#include "test_util.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
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
using rin::ParamDescriptor;
using rin::ParamKind;
using rin::ParamValue;
using rin::PortDirection;
using rin::PortRef;
using rin::PortType;
using rin::ValidationIssueKind;
using rin::WorkflowGraph;

// --- 目录构造（schema 照抄 docs/design/image_workflow_design.md §6 表格）-------

NodeDescriptor makeSourceRgbaDescriptor() {
    NodeDescriptor d;
    d.typeId = "source";
    d.displayName = "相机源";
    d.outputs = {PortType::Rgba8};
    return d;
}

NodeDescriptor makeSourceGrayDescriptor() {
    NodeDescriptor d;
    d.typeId = "source";
    d.displayName = "相机源";
    d.outputs = {PortType::Gray8};
    return d;
}

// §6：grayify 灰度化 Rgba8→Gray8，无参数。
NodeDescriptor makeGrayifyDescriptor() {
    NodeDescriptor d;
    d.typeId = "grayify";
    d.displayName = "灰度化";
    d.inputs = {PortType::Rgba8};
    d.outputs = {PortType::Gray8};
    return d;
}

// §6：hist_eq 直方图均衡 Gray8→Gray8，无参数。
NodeDescriptor makeHistEqDescriptor() {
    NodeDescriptor d;
    d.typeId = "hist_eq";
    d.displayName = "直方图均衡";
    d.inputs = {PortType::Gray8};
    d.outputs = {PortType::Gray8};
    return d;
}

// §6：fft_lowpass FFT 低通 Gray8→Gray8，cutoff: Real 0.2 [0,1]（M4-06 未实现，
// 仅用于"工厂偏差显式暴露"路径见证）。
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

// --- 实例构造助手 ------------------------------------------------------------

NodeInstance makeInstance(NodeId id, const std::string& typeId) {
    NodeInstance instance;
    instance.id = id;
    instance.typeId = typeId;
    return instance;
}

std::unique_ptr<IImageNode> makeNode(const NodeDescriptor& descriptor, NodeId id) {
    return rin::makeDefaultImageNode(descriptor, makeInstance(id, descriptor.typeId));
}

// --- 图像构造与核对助手 ------------------------------------------------------

using RgbaFn = std::function<std::array<std::uint8_t, 4>(std::uint32_t, std::uint32_t)>;
using GrayFn = std::function<std::uint8_t(std::uint32_t, std::uint32_t)>;

// 构造紧凑（stride=0 → 最小行宽）或行尾 padding（stride=rowBytes）的 Rgba8 图像；
// padding 区域填充 padByte（默认 0xEE，用于断言不泄漏进输出）。
ImageU8 makeRgba(std::uint32_t width, std::uint32_t height, const RgbaFn& pixel,
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

// 构造紧凑（stride=0 → 宽）或行尾 padding（stride=rowBytes）的 Gray8 图像。
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

// 由紧凑字节向量构造 Gray8 图像（参考亮度图注入用）。
ImageU8 wrapGray(std::uint32_t width, std::uint32_t height,
                 const std::vector<std::uint8_t>& bytes) {
    auto shared = std::make_shared<const std::vector<std::uint8_t>>(bytes);
    return ImageU8::wrap(PortType::Gray8, width, height, width, std::move(shared));
}

// Gray8 输出必须：数量 1、有效 Gray8、尺寸与输入一致、紧凑（stride == 宽）、
// 新缓冲（共享指针与数据指针均与输入不同）、逐字节等于期望。
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
        return false;  // 不共享源像素（缓冲对象不同）。
    }
    if (image.pixels()->data() == input.pixels()->data()) {
        return false;  // 数据指针不同（非同缓冲别名）。
    }
    if (expected.size() != image.pixels()->size()) {
        return false;
    }
    return std::equal(expected.begin(), expected.end(), image.pixels()->begin());
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

// --- 独立整数参考实现（按设计文档 §7 M4-05 冻结公式重写，不 include 实现头）----

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

// grayify 参考：Y = (77·R + 150·G + 29·B + 128) >> 8（byte0=R、byte1=G、byte2=B，
// alpha 不参与）；输出紧凑 W×H。
std::vector<std::uint8_t> refGrayify(const ImageU8& image) {
    std::vector<std::uint8_t> out(std::size_t{image.width()} * image.height());
    for (std::uint32_t y = 0; y < image.height(); ++y) {
        const std::uint8_t* row = image.row(y);
        std::uint8_t* dst = out.data() + std::size_t{y} * image.width();
        for (std::uint32_t x = 0; x < image.width(); ++x) {
            const std::uint8_t* pixel = row + std::size_t{x} * 4u;
            dst[x] = static_cast<std::uint8_t>(
                (77u * pixel[0] + 150u * pixel[1] + 29u * pixel[2] + 128u) >> 8);
        }
    }
    return out;
}

// hist_eq 参考：hist 全图统计；cdf[v] = Σ_{u≤v} hist[u]；cdf_min = hist[v_min]；
// out[v] = round-half-up(255·(cdf[v]−cdf_min)/(N−cdf_min))，整数精确式
// floor((2·num+den)/(2·den))（num = (cdf[v]−cdf_min)·255，den = N−cdf_min）；
// 常值图（cdf_min == N，分母为零）恒等输出；v < v_min 的空 bin 无像素可达。
std::vector<std::uint8_t> refHistEq(const ImageU8& image) {
    const std::uint64_t n = std::uint64_t{image.width()} * image.height();
    std::uint64_t histogram[256] = {};
    for (std::uint32_t y = 0; y < image.height(); ++y) {
        const std::uint8_t* row = image.row(y);
        for (std::uint32_t x = 0; x < image.width(); ++x) {
            ++histogram[row[x]];
        }
    }
    std::uint64_t cdfMin = 0;
    for (std::uint32_t v = 0; v < 256; ++v) {
        if (histogram[v] != 0) {
            cdfMin = histogram[v];  // = hist[v_min]。
            break;
        }
    }
    std::vector<std::uint8_t> out(std::size_t{image.width()} * image.height());
    if (cdfMin == n) {
        // 常值图：分母为零，§7 定义为逐像素恒等（新缓冲）。
        for (std::uint32_t y = 0; y < image.height(); ++y) {
            std::copy_n(image.row(y), image.width(),
                        out.data() + std::size_t{y} * image.width());
        }
        return out;
    }
    const std::uint64_t den = n - cdfMin;
    std::uint8_t lut[256] = {};
    std::uint64_t cdf = 0;
    bool reachedMin = false;
    for (std::uint32_t v = 0; v < 256; ++v) {
        cdf += histogram[v];
        if (!reachedMin) {
            if (histogram[v] == 0) {
                continue;  // v < v_min：cdf−cdf_min 在冻结公式域外，无像素可达。
            }
            reachedMin = true;
        }
        const std::uint64_t num = (cdf - cdfMin) * 255u;
        lut[v] = static_cast<std::uint8_t>((2u * num + den) / (2u * den));
    }
    for (std::uint32_t y = 0; y < image.height(); ++y) {
        const std::uint8_t* row = image.row(y);
        std::uint8_t* dst = out.data() + std::size_t{y} * image.width();
        for (std::uint32_t x = 0; x < image.width(); ++x) {
            dst[x] = lut[row[x]];
        }
    }
    return out;
}

Connection conn(NodeId from, std::uint32_t fromPort, NodeId to, std::uint32_t toPort) {
    Connection connection;
    connection.from = PortRef{from, PortDirection::Output, fromPort};
    connection.to = PortRef{to, PortDirection::Input, toPort};
    return connection;
}

}  // namespace

int main() {
    const NodeDescriptor sourceRgbaDescriptor = makeSourceRgbaDescriptor();
    const NodeDescriptor sourceGrayDescriptor = makeSourceGrayDescriptor();
    const NodeDescriptor grayifyDescriptor = makeGrayifyDescriptor();
    const NodeDescriptor histEqDescriptor = makeHistEqDescriptor();

    // ===================================================================
    // 1) 工厂 makeDefaultImageNode：M4-05 两个类型已实现，FFT 族仍显式拒绝
    // ===================================================================

    // --- 1a) grayify / hist_eq：返回实现且 descriptor().typeId 与 §6 签名一致 ---
    {
        const std::unique_ptr<IImageNode> grayify = makeNode(grayifyDescriptor, 1);
        RIN_CHECK(grayify != nullptr);
        RIN_CHECK(grayify != nullptr && grayify->descriptor().typeId == "grayify");
        RIN_CHECK(grayify != nullptr &&
                  grayify->descriptor().inputs == std::vector<PortType>{PortType::Rgba8});
        RIN_CHECK(grayify != nullptr &&
                  grayify->descriptor().outputs == std::vector<PortType>{PortType::Gray8});
        RIN_CHECK(grayify != nullptr && grayify->descriptor().params.empty());

        const std::unique_ptr<IImageNode> histEq = makeNode(histEqDescriptor, 2);
        RIN_CHECK(histEq != nullptr);
        RIN_CHECK(histEq != nullptr && histEq->descriptor().typeId == "hist_eq");
        RIN_CHECK(histEq != nullptr &&
                  histEq->descriptor().inputs == std::vector<PortType>{PortType::Gray8});
        RIN_CHECK(histEq != nullptr &&
                  histEq->descriptor().outputs == std::vector<PortType>{PortType::Gray8});
        RIN_CHECK(histEq != nullptr && histEq->descriptor().params.empty());
    }

    // --- 1b) 未实现类型（M4-06 FFT 族）工厂抛 invalid_argument，错误消息冻结 ---
    {
        const NodeDescriptor fftLowpass = makeFftLowpassDescriptor();
        const std::string message = factoryError(fftLowpass, makeInstance(1, "fft_lowpass"));
        RIN_CHECK_MSG(!message.empty(), "未实现类型应抛 invalid_argument：fft_lowpass");
        RIN_CHECK_MSG(message.find("(M4-06 FFT operators are not implemented yet)") !=
                          std::string::npos,
                      "未实现类型错误消息冻结：fft_lowpass");
        NodeDescriptor ghost;
        ghost.typeId = "no_such_operator";
        RIN_CHECK(throwsAs<std::invalid_argument>([&] {
            (void)rin::makeDefaultImageNode(ghost, makeInstance(1, "no_such_operator"));
        }));
    }

    // ===================================================================
    // 2) grayify：Y = (77·R + 150·G + 29·B + 128) >> 8
    // ===================================================================

    // --- 2a) 通道错位判别 golden：单通道极值 1×1（手推，见各注释）---
    {
        const std::unique_ptr<IImageNode> node = makeNode(grayifyDescriptor, 1);
        RIN_CHECK(node != nullptr);
        if (node == nullptr) {
            return rin_test::exitStatus();
        }
        // R=255,G=B=0：77·255=19635，+128=19763 = 77·256+51 → 77。
        // （若误按 BGR 读：byte0=255 被当作 B → 29 ≠ 77，判别成立。）
        RIN_CHECK_MSG(grayOutputMatches(*node,
                                        makeRgba(1, 1, [](std::uint32_t, std::uint32_t) {
                                            return std::array<std::uint8_t, 4>{255, 0, 0, 255};
                                        }),
                                        {77}),
                      "grayify R-only 极值 → 77");
        // G=255,R=B=0：150·255=38250，+128=38378 = 149·256+234 → 149。
        RIN_CHECK_MSG(grayOutputMatches(*node,
                                        makeRgba(1, 1, [](std::uint32_t, std::uint32_t) {
                                            return std::array<std::uint8_t, 4>{0, 255, 0, 255};
                                        }),
                                        {149}),
                      "grayify G-only 极值 → 149");
        // B=255,R=G=0：29·255=7395，+128=7523 = 29·256+99 → 29。
        // （若误按 BGR 读：byte2=255 被当作 R → 77 ≠ 29，判别成立。）
        RIN_CHECK_MSG(grayOutputMatches(*node,
                                        makeRgba(1, 1, [](std::uint32_t, std::uint32_t) {
                                            return std::array<std::uint8_t, 4>{0, 0, 255, 255};
                                        }),
                                        {29}),
                      "grayify B-only 极值 → 29");
        // 全黑：(0,0,0) → 128>>8 = 0；全白：Σ=256·255=65280，+128=65408 =
        // 255·256+128 → 255（Σ系数=256 自然有界，无饱和路径）。
        RIN_CHECK_MSG(grayOutputMatches(*node,
                                        makeRgba(1, 1, [](std::uint32_t, std::uint32_t) {
                                            return std::array<std::uint8_t, 4>{0, 0, 0, 255};
                                        }),
                                        {0}),
                      "grayify 全黑 → 0");
        RIN_CHECK_MSG(grayOutputMatches(*node,
                                        makeRgba(1, 1, [](std::uint32_t, std::uint32_t) {
                                            return std::array<std::uint8_t, 4>{
                                                255, 255, 255, 255};
                                        }),
                                        {255}),
                      "grayify 全白 → 255");
    }

    // --- 2b) round-half-up 半值边界：S ≡ 128 (mod 256) 的像素 ---
    {
        const std::unique_ptr<IImageNode> node = makeNode(grayifyDescriptor, 1);
        // (R,G,B)=(2,0,158)：S = 77·2 + 29·158 = 154+4582 = 4736 = 18.5×256，
        // +128 = 4864 = 19×256 → Y = 19（.5 精确上取整）。
        RIN_CHECK_MSG(grayOutputMatches(*node,
                                        makeRgba(1, 1, [](std::uint32_t, std::uint32_t) {
                                            return std::array<std::uint8_t, 4>{2, 0, 158, 255};
                                        }),
                                        {19}),
                      "grayify 半值边界 18.5 → 19（round-half-up）");
    }

    // --- 2c) 4×2 混合小图全 8 字节手推 golden（含各 alpha 值不参与）---
    // 手推（§7 冻结公式，逐像素）：
    //   (0,0,0,255)     S=0        → 0
    //   (255,255,255,255) S=65280  → 255
    //   (255,0,0,128)   S=19635    → 77   （alpha=128 不参与）
    //   (2,0,158,255)   S=4736     → 19   （半值边界）
    //   (0,255,0,7)     S=38250    → 149  （alpha=7 不参与）
    //   (0,0,255,0)     S=7395     → 29   （alpha=0 不参与）
    //   (128,64,32,255) S=77·128+150·64+29·32 = 9856+9600+928 = 20384，
    //                   +128=20512 = 80·256+32 → 80
    //   (250,251,252,255) S=19250+37650+7308 = 64208，+128=64336 =
    //                   251·256+80 → 251
    {
        const std::unique_ptr<IImageNode> node = makeNode(grayifyDescriptor, 1);
        const ImageU8 input = makeRgba(4, 2, [](std::uint32_t x, std::uint32_t y) {
            const std::array<std::array<std::uint8_t, 4>, 8> pixels = {{
                {0, 0, 0, 255}, {255, 255, 255, 255}, {255, 0, 0, 128}, {2, 0, 158, 255},
                {0, 255, 0, 7}, {0, 0, 255, 0},       {128, 64, 32, 255}, {250, 251, 252, 255},
            }};
            return pixels[std::size_t{y} * 4u + x];
        });
        const std::vector<std::uint8_t> expected = {0, 255, 77, 19, 149, 29, 80, 251};
        RIN_CHECK_MSG(grayOutputMatches(*node, input, expected),
                      "grayify 4×2 混合小图逐字节 golden");
    }

    // --- 2d) alpha 不参与：同 RGB 不同 alpha 输出逐字节相同 ---
    {
        const std::unique_ptr<IImageNode> node = makeNode(grayifyDescriptor, 1);
        const ImageU8 alphaLow = makeRgba(2, 1, [](std::uint32_t x, std::uint32_t) {
            return x == 0 ? std::array<std::uint8_t, 4>{10, 20, 30, 0}
                          : std::array<std::uint8_t, 4>{40, 50, 60, 255};
        });
        const ImageU8 alphaHigh = makeRgba(2, 1, [](std::uint32_t x, std::uint32_t) {
            return x == 0 ? std::array<std::uint8_t, 4>{10, 20, 30, 200}
                          : std::array<std::uint8_t, 4>{40, 50, 60, 5};
        });
        const std::vector<ImageU8> outLow = node->apply({alphaLow});
        const std::vector<ImageU8> outHigh = node->apply({alphaHigh});
        RIN_CHECK_EQ(outLow.size(), std::size_t{1});
        RIN_CHECK_EQ(outHigh.size(), std::size_t{1});
        RIN_CHECK(outLow[0].valid() && outHigh[0].valid());
        RIN_CHECK_MSG(*outLow[0].pixels() == *outHigh[0].pixels(),
                      "grayify alpha 不参与：同 RGB 不同 alpha 输出相同");
        // 与参考实现交叉（顺带锁定亮度值本身）。
        RIN_CHECK_MSG(*outLow[0].pixels() == refGrayify(alphaLow),
                      "grayify alpha 用例与参考实现一致");
    }

    // --- 2e) 输入行尾 padding：输出与紧凑输入一致、输出紧凑、padding 不泄漏 ---
    {
        const std::unique_ptr<IImageNode> node = makeNode(grayifyDescriptor, 1);
        const RgbaFn pixel = [](std::uint32_t x, std::uint32_t y) {
            return std::array<std::uint8_t, 4>{
                static_cast<std::uint8_t>(x * 7u + y * 3u),
                static_cast<std::uint8_t>(x * 11u + y * 5u),
                static_cast<std::uint8_t>(x * 13u + y * 2u + 1u),
                static_cast<std::uint8_t>(255 - x)};
        };
        const ImageU8 compact = makeRgba(5, 3, pixel);
        const ImageU8 padded = makeRgba(5, 3, pixel, 5u * 4u + 4u, 0xEE);
        RIN_CHECK_EQ(padded.stride(), std::uint32_t{24});
        RIN_CHECK_MSG(grayOutputMatches(*node, padded, refGrayify(compact)),
                      "grayify padding 输入与紧凑参考一致（padding 不泄漏）");
        const std::vector<ImageU8> out = node->apply({padded});
        RIN_CHECK_EQ(out.size(), std::size_t{1});
        RIN_CHECK_EQ(out[0].width(), std::uint32_t{5});
        RIN_CHECK_EQ(out[0].height(), std::uint32_t{3});
        RIN_CHECK_EQ(out[0].stride(), std::uint32_t{5});  // 输出紧凑，不继承输入 stride。
        RIN_CHECK_EQ(out[0].format(), PortType::Gray8);
    }

    // --- 2f) 37×23 伪随机噪声图与参考实现逐字节交叉验证 ---
    {
        const std::unique_ptr<IImageNode> node = makeNode(grayifyDescriptor, 1);
        ByteLcg lcg{0x5EED1234u};
        const ImageU8 input = makeRgba(37, 23, [&lcg](std::uint32_t, std::uint32_t) {
            return std::array<std::uint8_t, 4>{lcg.next(), lcg.next(), lcg.next(),
                                               lcg.next()};
        });
        RIN_CHECK_MSG(grayOutputMatches(*node, input, refGrayify(input)),
                      "grayify 37×23 噪声图与独立参考逐字节一致");
    }

    // --- 2g) 防御路径：数量 0/2、无效图、格式错配 ---
    {
        const std::unique_ptr<IImageNode> node = makeNode(grayifyDescriptor, 1);
        const ImageU8 rgba = makeRgba(2, 2, [](std::uint32_t, std::uint32_t) {
            return std::array<std::uint8_t, 4>{1, 2, 3, 255};
        });
        const ImageU8 gray = makeGray(2, 2, [](std::uint32_t, std::uint32_t) { return 7; });
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)node->apply({}); }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)node->apply({rgba, rgba}); }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)node->apply({ImageU8{}}); }));
        RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] { (void)node->apply({gray}); }),
                      "grayify 格式错配（Gray8 喂 Rgba8 声明）拒绝");
    }

    // --- 2h) 图集成：source→grayify 与直接 apply 逐字节一致 ---
    {
        NodeCatalog catalog;
        catalog.nodes = {sourceRgbaDescriptor, grayifyDescriptor};

        WorkflowGraph graph;
        graph.nodes = {makeInstance(1, "source"), makeInstance(2, "grayify")};
        graph.connections = {conn(1, 0, 2, 0)};

        const ImageNodeFactory factory = [](const NodeDescriptor& descriptor,
                                            const NodeInstance& instance) {
            return rin::makeDefaultImageNode(descriptor, instance);
        };
        const rin::NodeGraphBuild build = rin::buildNodeGraph(graph, catalog, factory);
        RIN_CHECK(build.validation.ok);
        RIN_CHECK(build.graph != nullptr);
        RIN_CHECK_EQ(build.graph->size(), std::size_t{2});
        if (build.graph != nullptr) {
            const ImageU8 frame = makeRgba(4, 2, [](std::uint32_t x, std::uint32_t y) {
                return std::array<std::uint8_t, 4>{
                    static_cast<std::uint8_t>(x * 61u + y * 17u),
                    static_cast<std::uint8_t>(x * 5u + y * 43u + 9u),
                    static_cast<std::uint8_t>(x * 29u + y * 3u + 100u), std::uint8_t{255}};
            });
            const std::vector<std::vector<ImageU8>> outputs =
                rin::runNodeGraph(*build.graph,
                                  [&frame](const NodeGraph::Node&) { return frame; });
            RIN_CHECK_EQ(outputs.size(), std::size_t{2});
            RIN_CHECK_EQ(outputs[1].size(), std::size_t{1});
            RIN_CHECK(outputs[1][0].valid() && outputs[1][0].format() == PortType::Gray8);
            RIN_CHECK_MSG(*outputs[1][0].pixels() == refGrayify(frame),
                          "图内 grayify 与直接 apply（参考）逐字节一致");
        }
    }

    // ===================================================================
    // 3) hist_eq：cdf_min 映射 + 整数精确 round-half-up
    // ===================================================================

    // --- 3a) 已知直方图 golden：6×4，hist{2:3, 5:5, 100:9, 200:7} ---
    // 手推（§7 冻结公式）：N = 24；cdf_min = hist[2] = 3；den = N−cdf_min = 21。
    //   v=2   : cdf=3  → num = (3−3)·255  = 0    → (0+21)/42     = 0   → 0
    //   v=5   : cdf=8  → num = (8−3)·255  = 1275 → (2550+21)/42  = 61  （61.21）
    //   v=100 : cdf=17 → num = (17−3)·255 = 3570 → (7140+21)/42  = 170 （3570/21=170
    //           整除，冻结整数式 floor(170.5)=170 与 round-half-up(170)=170 一致）
    //   v=200 : cdf=24 → num = (24−3)·255 = 5355 → (10710+21)/42 = 255 （255.5，
    //           v_max 精确映射 255）
    // 布局（行主序 6×4；计数 2:3 ✓ 5:5 ✓ 100:9 ✓ 200:7 ✓）：
    //   行0: 2 2 2 5 5 5      行1: 5 5 100 100 100 100
    //   行2: 100 100 100 100 100 200
    //   行3: 200 200 200 200 200 200
    {
        const std::unique_ptr<IImageNode> node = makeNode(histEqDescriptor, 1);
        RIN_CHECK(node != nullptr);
        if (node == nullptr) {
            return rin_test::exitStatus();
        }
        const GrayFn layout = [](std::uint32_t x, std::uint32_t y) {
            static const std::array<std::array<std::uint8_t, 6>, 4> rows = {{
                {2, 2, 2, 5, 5, 5},
                {5, 5, 100, 100, 100, 100},
                {100, 100, 100, 100, 100, 200},
                {200, 200, 200, 200, 200, 200},
            }};
            return rows[y][x];
        };
        const ImageU8 input = makeGray(6, 4, layout);
        const std::vector<std::uint8_t> expected = {
            0, 0, 0, 61, 61, 61,
            61, 61, 170, 170, 170, 170,
            170, 170, 170, 170, 170, 255,
            255, 255, 255, 255, 255, 255,
        };
        RIN_CHECK_MSG(grayOutputMatches(*node, input, expected),
                      "hist_eq 6×4 已知直方图逐字节 golden（0/61/170/255）");

        // 行尾 padding 变体（stride = 8，padding 0xEE=238）：直方图只覆盖 W×H；
        // 若 padding 泄漏进直方图，v_max 变为 238，全部映射偏移，可判别。
        const ImageU8 padded = makeGray(6, 4, layout, 8u, 0xEE);
        RIN_CHECK_MSG(grayOutputMatches(*node, padded, expected),
                      "hist_eq padding 输入与紧凑 golden 一致（padding 不入直方图）");
    }

    // --- 3b) 极值映射：最小出现灰度 → 0、最大出现灰度 → 255（4×1 {7,7,9,9}）---
    // 手推：N=4；cdf_min=hist[7]=2；den=2；v=7: cdf=2 → num=0 → 0；
    //       v=9: cdf=4 → num=(4−2)·255=510 → (1020+2)/4 = 255.5 → 255。
    {
        const std::unique_ptr<IImageNode> node = makeNode(histEqDescriptor, 1);
        const ImageU8 input = makeGray(4, 1, [](std::uint32_t x, std::uint32_t) {
            return x < 2 ? std::uint8_t{7} : std::uint8_t{9};
        });
        RIN_CHECK_MSG(grayOutputMatches(*node, input, {0, 0, 255, 255}),
                      "hist_eq 极值映射：v_min → 0、v_max → 255");
    }

    // --- 3c) 常值图恒等：全 7 / 全 255 / 全 0 / 1×1 ---
    // 手推：全图同值 → cdf_min = N → 分母为零 → §7 定义为逐像素恒等输出
    //（不是全白/全黑），新紧凑缓冲。
    {
        const std::unique_ptr<IImageNode> node = makeNode(histEqDescriptor, 1);
        RIN_CHECK_MSG(grayOutputMatches(*node, makeGray(5, 3, [](std::uint32_t, std::uint32_t) {
                                           return std::uint8_t{7};
                                       }),
                                       {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7}),
                      "hist_eq 常值图（全 7）恒等输出");
        RIN_CHECK_MSG(grayOutputMatches(*node, makeGray(1, 1, [](std::uint32_t, std::uint32_t) {
                                           return std::uint8_t{7};
                                       }),
                                       {7}),
                      "hist_eq 1×1 常值图恒等输出");
        RIN_CHECK_MSG(grayOutputMatches(*node, makeGray(3, 2, [](std::uint32_t, std::uint32_t) {
                                           return std::uint8_t{255};
                                       }),
                                       std::vector<std::uint8_t>(6, 255)),
                      "hist_eq 常值图（全 255）恒等输出（不是全黑）");
        RIN_CHECK_MSG(grayOutputMatches(*node, makeGray(4, 4, [](std::uint32_t, std::uint32_t) {
                                           return std::uint8_t{0};
                                       }),
                                       std::vector<std::uint8_t>(16, 0)),
                      "hist_eq 常值图（全 0）恒等输出（不是全白）");
        // 常值图同样不共享源像素（grayOutputMatches 内含共享指针与数据指针核对）。
    }

    // --- 3d) 全量程均匀直方图精确恒等：256×2，每级计数 2 ---
    // 手推：hist[v]=2（v∈[0,255]）；cdf[v] = 2(v+1)；cdf_min = 2；den = 512−2 = 510；
    //   num = (cdf−cdf_min)·255 = 2v·255 = 510v；
    //   out[v] = floor((2·510v + 510)/(2·510)) = floor(v + 0.5) = v（精确恒等，非近似）。
    {
        const std::unique_ptr<IImageNode> node = makeNode(histEqDescriptor, 1);
        const GrayFn ramp = [](std::uint32_t x, std::uint32_t) {
            return static_cast<std::uint8_t>(x & 0xFFu);
        };
        const ImageU8 input = makeGray(256, 2, ramp);
        std::vector<std::uint8_t> expected;
        expected.reserve(512);
        for (std::uint32_t y = 0; y < 2; ++y) {
            for (std::uint32_t x = 0; x < 256; ++x) {
                expected.push_back(static_cast<std::uint8_t>(x));
            }
        }
        RIN_CHECK_MSG(grayOutputMatches(*node, input, expected),
                      "hist_eq 全量程均匀直方图逐字节精确恒等");
        // padding 变体同恒等（padding 0xEE 不入直方图，否则 v_max=238 破坏均匀性）。
        const ImageU8 padded = makeGray(256, 2, ramp, 260u, 0xEE);
        RIN_CHECK_MSG(grayOutputMatches(*node, padded, expected),
                      "hist_eq 全量程均匀恒等（padding 输入）");
    }

    // --- 3e) 子区间均匀线性重映射：[40,47] 每级 3 次（8×3）---
    // 手推：N=24；cdf_min=hist[40]=3；den=21；v=40+j：cdf=3(j+1)，num=3j·255，
    //   out = floor((2·3j·255+21)/42) = round-half-up(255·j/7)：
    //   j=0: 0          → 0
    //   j=1: 255/7   = 36.43 → (1530+21)/42  = 1551/42  = 36.93 → 36
    //   j=2: 510/7   = 72.86 → (3060+21)/42  = 3081/42  = 73.36 → 73
    //   j=3: 765/7   = 109.29 → (4590+21)/42 = 4611/42  = 109.79 → 109
    //   j=4: 1020/7  = 145.71 → (6120+21)/42 = 6141/42  = 146.21 → 146
    //   j=5: 1275/7  = 182.14 → (7650+21)/42 = 7671/42  = 182.64 → 182
    //   j=6: 1530/7  = 218.57 → (9180+21)/42 = 9201/42  = 219.07 → 219
    //   j=7: 255·7/7 = 255    → (10710+21)/42 = 10731/42 = 255.5 → 255（v_max）
    {
        const std::unique_ptr<IImageNode> node = makeNode(histEqDescriptor, 1);
        const ImageU8 input = makeGray(8, 3, [](std::uint32_t x, std::uint32_t) {
            return static_cast<std::uint8_t>(40u + x);  // 每行 40..47，每级恰 3 次。
        });
        const std::vector<std::uint8_t> expected = {0, 36, 73, 109, 146, 182, 219, 255,
                                                    0, 36, 73, 109, 146, 182, 219, 255,
                                                    0, 36, 73, 109, 146, 182, 219, 255};
        RIN_CHECK_MSG(grayOutputMatches(*node, input, expected),
                      "hist_eq 子区间 [40,47] 均匀线性重映射 golden");
    }

    // --- 3f) 半值边界锁定：[10,40] 每级 2 次（31×2），out[11] = round-half-up(8.5) ---
    // 手推：N=62；cdf_min=hist[10]=2；den=60；v=11：cdf=4，num=(4−2)·255=510，
    //   out = (2·510+60)/(2·60) = 1080/120 = 9 精确（255/30 = 8.5 上取整；
    //   银行家舍入会得 8，本用例排除之）。
    {
        const std::unique_ptr<IImageNode> node = makeNode(histEqDescriptor, 1);
        const ImageU8 input = makeGray(31, 2, [](std::uint32_t x, std::uint32_t) {
            return static_cast<std::uint8_t>(10u + x);  // 每行 10..40，每级恰 2 次。
        });
        const std::vector<ImageU8> output = node->apply({input});
        RIN_CHECK_EQ(output.size(), std::size_t{1});
        RIN_CHECK(output[0].valid());
        // 端点锁定 + 半值字节锁定（其余字节交给参考实现交叉验证）。
        RIN_CHECK_EQ(output[0].pixels()->at(0), std::uint8_t{0});    // v=10 → 0
        RIN_CHECK_EQ(output[0].pixels()->at(1), std::uint8_t{9});    // 8.5 → 9
        RIN_CHECK_EQ(output[0].pixels()->at(30), std::uint8_t{255});  // v=40 → 255
        RIN_CHECK_EQ(output[0].pixels()->at(31), std::uint8_t{0});
        RIN_CHECK_EQ(output[0].pixels()->at(32), std::uint8_t{9});
        RIN_CHECK_EQ(output[0].pixels()->at(61), std::uint8_t{255});
        RIN_CHECK_MSG(*output[0].pixels() == refHistEq(input),
                      "hist_eq [10,40] 全图与参考实现一致");
    }

    // --- 3g) 37×23 伪随机噪声图与参考实现逐字节交叉验证 ---
    {
        const std::unique_ptr<IImageNode> node = makeNode(histEqDescriptor, 1);
        ByteLcg lcg{0xA5F00D7u};
        const ImageU8 input = makeGray(37, 23, [&lcg](std::uint32_t, std::uint32_t) {
            return lcg.next();
        });
        RIN_CHECK_MSG(grayOutputMatches(*node, input, refHistEq(input)),
                      "hist_eq 37×23 噪声图与独立参考逐字节一致");
    }

    // --- 3h) 防御路径：数量 0/2、无效图、格式错配 ---
    {
        const std::unique_ptr<IImageNode> node = makeNode(histEqDescriptor, 1);
        const ImageU8 gray = makeGray(3, 3, [](std::uint32_t, std::uint32_t) { return 9; });
        const ImageU8 rgba = makeRgba(3, 3, [](std::uint32_t, std::uint32_t) {
            return std::array<std::uint8_t, 4>{1, 2, 3, 255};
        });
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)node->apply({}); }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)node->apply({gray, gray}); }));
        RIN_CHECK(throwsAs<std::invalid_argument>([&] { (void)node->apply({ImageU8{}}); }));
        RIN_CHECK_MSG(throwsAs<std::invalid_argument>([&] { (void)node->apply({rgba}); }),
                      "hist_eq 格式错配（Rgba8 喂 Gray8 声明）拒绝");
    }

    // --- 3i) 图集成（§7"RGBA 亮度域与 Gray8 路径一致"）：
    //     source→grayify→hist_eq 链路 == 参考亮度 Gray8 图直接经 hist_eq ---
    {
        const ImageU8 frame = makeRgba(6, 4, [](std::uint32_t x, std::uint32_t y) {
            return std::array<std::uint8_t, 4>{
                static_cast<std::uint8_t>(x * 37u + y * 11u + 3u),
                static_cast<std::uint8_t>(x * 3u + y * 53u + 60u),
                static_cast<std::uint8_t>(x * 17u + y * 7u + 150u), std::uint8_t{255}};
        });
        // 参考亮度 Gray8 图（§7 冻结 grayify 公式推得，与被测实现无共享）。
        const ImageU8 luminance = wrapGray(6, 4, refGrayify(frame));
        const std::vector<std::uint8_t> expectedEq = refHistEq(luminance);

        // 链路图：source(Rgba8) → grayify → hist_eq。
        NodeCatalog chainCatalog;
        chainCatalog.nodes = {sourceRgbaDescriptor, grayifyDescriptor, histEqDescriptor};
        WorkflowGraph chainGraph;
        chainGraph.nodes = {makeInstance(1, "source"), makeInstance(2, "grayify"),
                            makeInstance(3, "hist_eq")};
        chainGraph.connections = {conn(1, 0, 2, 0), conn(2, 0, 3, 0)};

        // 单节点对照图：source(Gray8) → hist_eq（直接注入亮度图）。
        NodeCatalog directCatalog;
        directCatalog.nodes = {sourceGrayDescriptor, histEqDescriptor};
        WorkflowGraph directGraph;
        directGraph.nodes = {makeInstance(1, "source"), makeInstance(2, "hist_eq")};
        directGraph.connections = {conn(1, 0, 2, 0)};

        const ImageNodeFactory factory = [](const NodeDescriptor& descriptor,
                                            const NodeInstance& instance) {
            return rin::makeDefaultImageNode(descriptor, instance);
        };
        const rin::NodeGraphBuild chainBuild =
            rin::buildNodeGraph(chainGraph, chainCatalog, factory);
        const rin::NodeGraphBuild directBuild =
            rin::buildNodeGraph(directGraph, directCatalog, factory);
        RIN_CHECK(chainBuild.validation.ok && chainBuild.graph != nullptr);
        RIN_CHECK(directBuild.validation.ok && directBuild.graph != nullptr);
        if (chainBuild.graph != nullptr && directBuild.graph != nullptr) {
            const std::vector<std::vector<ImageU8>> chainOutputs = rin::runNodeGraph(
                *chainBuild.graph, [&frame](const NodeGraph::Node&) { return frame; });
            const std::vector<std::vector<ImageU8>> directOutputs = rin::runNodeGraph(
                *directBuild.graph, [&luminance](const NodeGraph::Node&) { return luminance; });
            RIN_CHECK_EQ(chainOutputs.size(), std::size_t{3});
            RIN_CHECK_EQ(directOutputs.size(), std::size_t{2});
            RIN_CHECK(chainOutputs[2].size() == 1 && chainOutputs[2][0].valid() &&
                      chainOutputs[2][0].format() == PortType::Gray8);
            RIN_CHECK(directOutputs[1].size() == 1 && directOutputs[1][0].valid());
            // 链路输出 == 手推亮度图直接均衡（逐字节）；同时 == 参考实现。
            RIN_CHECK_MSG(*chainOutputs[2][0].pixels() == *directOutputs[1][0].pixels(),
                          "source→grayify→hist_eq 链路与 Gray8 直衡路径逐字节一致");
            RIN_CHECK_MSG(*chainOutputs[2][0].pixels() == expectedEq,
                          "链路输出与独立参考逐字节一致");
        }
    }

    return rin_test::exitStatus();
}
