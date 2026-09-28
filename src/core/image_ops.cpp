#include "rin/image_ops.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rin {

namespace {

/// 算子输出的唯一构造路径：先在 mutable 缓冲上填充，再冻结为共享不可变像素
/// （ImageU8 构造后不可变纪律下的合法写序）；输出一律紧凑（stride = 最小行宽）。
/// 元数据或预算被拒属于实现缺陷（输入已通过防御核对，输出尺寸有界），显式抛出。
ImageU8 freezeImage(PortType format, std::uint32_t width, std::uint32_t height,
                    std::vector<std::uint8_t> buffer) {
    auto shared = std::make_shared<const std::vector<std::uint8_t>>(std::move(buffer));
    ImageU8 image = ImageU8::wrap(format, width, height, 0, std::move(shared));
    if (!image.valid()) {
        throw std::runtime_error("image operator produced a rejected output buffer");
    }
    return image;
}

/// 单输入算子的共同防御：入参数量、有效性与格式一致性（runNodeGraph 已核对，
/// apply 被直接调用时同样显式失败，禁止静默）。
const ImageU8& singleInput(const NodeDescriptor& descriptor,
                           const std::vector<ImageU8>& inputs) {
    if (inputs.size() != 1) {
        throw std::invalid_argument("'" + descriptor.typeId + "' expects exactly one input, got " +
                                    std::to_string(inputs.size()));
    }
    const ImageU8& image = inputs.front();
    if (!image.valid()) {
        throw std::invalid_argument("'" + descriptor.typeId + "' input image is invalid");
    }
    if (descriptor.inputs.empty() || image.format() != descriptor.inputs.front()) {
        throw std::invalid_argument("'" + descriptor.typeId + "' input is " +
                                    toString(image.format()) + ", declared " +
                                    (descriptor.inputs.empty()
                                         ? std::string("no input")
                                         : std::string(toString(descriptor.inputs.front()))));
    }
    return image;
}

/// 构造期参数读取：声明缺失或值种类错位（运行期直接构造的实例）显式失败，
/// 由 buildNodeGraph 转为校验问题（BadParam）。
std::int64_t requiredInteger(const NodeDescriptor& descriptor, const NodeInstance& instance,
                             const std::string& paramId) {
    const std::optional<std::int64_t> value = paramInteger(descriptor, instance, paramId);
    if (!value) {
        throw std::invalid_argument("'" + descriptor.typeId + "': parameter '" + paramId +
                                    "' is missing or has a mismatched kind");
    }
    return *value;
}

/// 裁切（M4-03）：Rgba8 → Rgba8，ROI 参数构造期定型；图像尺寸是运行期数据，
/// 退化/越界在 apply 期显式拒绝。数值语义见 image_workflow_design.md §7。
class CropImageNode final : public IImageNode {
public:
    CropImageNode(const NodeDescriptor& descriptor, const NodeInstance& instance)
        : descriptor_(descriptor) {
        x_ = requiredInteger(descriptor_, instance, "x");
        y_ = requiredInteger(descriptor_, instance, "y");
        width_ = requiredInteger(descriptor_, instance, "width");
        height_ = requiredInteger(descriptor_, instance, "height");
    }

    [[nodiscard]] const NodeDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }

    [[nodiscard]] std::vector<ImageU8> apply(const std::vector<ImageU8>& inputs) const override {
        const ImageU8& source = singleInput(descriptor_, inputs);
        // 无符号域统一判定：负值（运行期直接构造，图准入已拒绝）回绕为极大值，
        // 自然落入越界拒绝，无需单独分支；求和用 uint64 防回绕。
        const auto x = static_cast<std::uint64_t>(x_);
        const auto y = static_cast<std::uint64_t>(y_);
        const auto width = static_cast<std::uint64_t>(width_);
        const auto height = static_cast<std::uint64_t>(height_);
        if (width == 0 || height == 0) {
            throw std::invalid_argument("crop roi is degenerate (zero width/height)");
        }
        if (x + width > source.width() || y + height > source.height()) {
            throw std::invalid_argument(
                "crop roi (" + std::to_string(x_) + ", " + std::to_string(y_) + ", " +
                std::to_string(width_) + "x" + std::to_string(height_) +
                ") exceeds input bounds " + std::to_string(source.width()) + "x" +
                std::to_string(source.height()));
        }
        const std::uint32_t es = elementSize(source.format());
        const auto roiW = static_cast<std::uint32_t>(width);
        const auto roiH = static_cast<std::uint32_t>(height);
        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(roiW) * es * roiH);
        for (std::uint32_t row = 0; row < roiH; ++row) {
            const std::uint8_t* src = source.row(static_cast<std::uint32_t>(y) + row);
            std::copy_n(src + static_cast<std::size_t>(x) * es,
                        static_cast<std::size_t>(roiW) * es,
                        buffer.data() + static_cast<std::size_t>(row) * roiW * es);
        }
        return {freezeImage(source.format(), roiW, roiH, std::move(buffer))};
    }

private:
    NodeDescriptor descriptor_;  /// 值拷贝：工厂与节点实例生命周期解耦。
    std::int64_t x_ = 0;
    std::int64_t y_ = 0;
    std::int64_t width_ = 0;
    std::int64_t height_ = 0;
};

/// 降分辨率插值（M4-03 冻结语义）。
enum class DownscaleInterpolation {
    Nearest,
    Bilinear,
};

/// 降分辨率（M4-03）：Rgba8 → Rgba8，插值方式与缩放系数构造期定型；输出尺寸
/// floor(in×scale)（< 1 为退化输出，apply 期显式拒绝）。数值语义见
/// image_workflow_design.md §7（nearest 面积覆盖采样、bilinear 中心对齐插值、
/// round-half-up 量化，scale=1.0 恒等）。
class DownscaleImageNode final : public IImageNode {
public:
    DownscaleImageNode(const NodeDescriptor& descriptor, const NodeInstance& instance)
        : descriptor_(descriptor) {
        const std::optional<std::string> interpolation =
            paramEnumeration(descriptor_, instance, "interpolation");
        if (!interpolation) {
            throw std::invalid_argument("'" + descriptor_.typeId + "': parameter 'interpolation'"
                                        " is missing or has a mismatched kind");
        }
        if (*interpolation == "nearest") {
            interpolation_ = DownscaleInterpolation::Nearest;
        } else if (*interpolation == "bilinear") {
            interpolation_ = DownscaleInterpolation::Bilinear;
        } else {
            throw std::invalid_argument("'" + descriptor_.typeId + "': unknown interpolation"
                                        " option '" + *interpolation + "'");
        }
        const std::optional<double> scale = paramReal(descriptor_, instance, "scale");
        if (!scale) {
            throw std::invalid_argument("'" + descriptor_.typeId + "': parameter 'scale'"
                                        " is missing or has a mismatched kind");
        }
        // 图准入范围为 [0.1,1.0]；构造期复核 (0,1] 为防御运行期直接构造的实例。
        if (!std::isfinite(*scale) || *scale <= 0.0 || *scale > 1.0) {
            throw std::invalid_argument("'" + descriptor_.typeId + "': scale must be finite"
                                        " in (0, 1], got " + std::to_string(*scale));
        }
        scale_ = *scale;
    }

    [[nodiscard]] const NodeDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }

    [[nodiscard]] std::vector<ImageU8> apply(const std::vector<ImageU8>& inputs) const override {
        const ImageU8& source = singleInput(descriptor_, inputs);
        const double outWidth = std::floor(static_cast<double>(source.width()) * scale_);
        const double outHeight = std::floor(static_cast<double>(source.height()) * scale_);
        // 上界复核同时兜住 scale > 1 直接构造（降分辨率不得放大输出）。
        if (!(outWidth >= 1.0 && outWidth <= static_cast<double>(source.width())) ||
            !(outHeight >= 1.0 && outHeight <= static_cast<double>(source.height()))) {
            throw std::invalid_argument(
                "downscale output degenerates (floor(size*scale) not in [1, input size])");
        }
        const auto outW = static_cast<std::uint32_t>(outWidth);
        const auto outH = static_cast<std::uint32_t>(outHeight);
        const std::uint32_t es = elementSize(source.format());
        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(outW) * es * outH);
        if (interpolation_ == DownscaleInterpolation::Nearest) {
            downscaleNearest(source, outW, outH, es, buffer);
        } else {
            downscaleBilinear(source, outW, outH, es, buffer);
        }
        return {freezeImage(source.format(), outW, outH, std::move(buffer))};
    }

private:
    /// srcX = min(floor((outX+0.5)×ratio), in−1)：面积覆盖采样（像素 i 覆盖
    /// [i, i+1)）；clamp 为浮点防御（数学上乘积 < in 恒成立）。
    void downscaleNearest(const ImageU8& source, std::uint32_t outW, std::uint32_t outH,
                          std::uint32_t es, std::vector<std::uint8_t>& buffer) const {
        const double ratioX = static_cast<double>(source.width()) / outW;
        const double ratioY = static_cast<double>(source.height()) / outH;
        for (std::uint32_t oy = 0; oy < outH; ++oy) {
            const auto sy = nearestIndex(oy, ratioY, source.height());
            std::uint8_t* dstRow = buffer.data() + static_cast<std::size_t>(oy) * outW * es;
            for (std::uint32_t ox = 0; ox < outW; ++ox) {
                const auto sx = nearestIndex(ox, ratioX, source.width());
                const std::uint8_t* src = source.row(sy) + static_cast<std::size_t>(sx) * es;
                std::copy_n(src, es, dstRow + static_cast<std::size_t>(ox) * es);
            }
        }
    }

    /// srcXf = (outX+0.5)×ratio − 0.5 饱和到 [0, in−1] 后中心对齐双线性；
    /// 逐通道独立加权（Rgba8 不做 alpha 预乘补偿），floor(v+0.5) 量化。
    void downscaleBilinear(const ImageU8& source, std::uint32_t outW, std::uint32_t outH,
                           std::uint32_t es, std::vector<std::uint8_t>& buffer) const {
        const double ratioX = static_cast<double>(source.width()) / outW;
        const double ratioY = static_cast<double>(source.height()) / outH;
        const double maxX = static_cast<double>(source.width() - 1);
        const double maxY = static_cast<double>(source.height() - 1);
        for (std::uint32_t oy = 0; oy < outH; ++oy) {
            const double syf = std::clamp((oy + 0.5) * ratioY - 0.5, 0.0, maxY);
            const auto y0 = static_cast<std::uint32_t>(std::floor(syf));
            const double fy = syf - static_cast<double>(y0);
            const std::uint32_t y1 = std::min(y0 + 1, source.height() - 1);
            const std::uint8_t* row0 = source.row(y0);
            const std::uint8_t* row1 = source.row(y1);
            std::uint8_t* dstRow = buffer.data() + static_cast<std::size_t>(oy) * outW * es;
            for (std::uint32_t ox = 0; ox < outW; ++ox) {
                const double sxf = std::clamp((ox + 0.5) * ratioX - 0.5, 0.0, maxX);
                const auto x0 = static_cast<std::uint32_t>(std::floor(sxf));
                const double fx = sxf - static_cast<double>(x0);
                const std::uint32_t x1 = std::min(x0 + 1, source.width() - 1);
                std::uint8_t* dst = dstRow + static_cast<std::size_t>(ox) * es;
                for (std::uint32_t c = 0; c < es; ++c) {
                    const double v =
                        (1.0 - fx) * (1.0 - fy) * row0[static_cast<std::size_t>(x0) * es + c] +
                        fx * (1.0 - fy) * row0[static_cast<std::size_t>(x1) * es + c] +
                        (1.0 - fx) * fy * row1[static_cast<std::size_t>(x0) * es + c] +
                        fx * fy * row1[static_cast<std::size_t>(x1) * es + c];
                    // 凸组合下 v ∈ [0,255]；clamp 为浮点防御。
                    const double quantized = std::floor(v + 0.5);
                    dst[c] = static_cast<std::uint8_t>(std::clamp(quantized, 0.0, 255.0));
                }
            }
        }
    }

    [[nodiscard]] static std::uint32_t nearestIndex(std::uint32_t outCoord, double ratio,
                                                    std::uint32_t inputSize) {
        const double src = (static_cast<double>(outCoord) + 0.5) * ratio;
        const auto index = static_cast<std::uint32_t>(std::floor(src));
        return std::min(index, inputSize - 1);
    }

    NodeDescriptor descriptor_;  /// 值拷贝：工厂与节点实例生命周期解耦。
    DownscaleInterpolation interpolation_ = DownscaleInterpolation::Nearest;
    double scale_ = 0.5;
};

}  // namespace

std::unique_ptr<IImageNode> makeDefaultImageNode(const NodeDescriptor& descriptor,
                                                 const NodeInstance& instance) {
    if (descriptor.typeId == "source") {
        return nullptr;  // 注入型源节点（M4-07 引擎注入相机帧）。
    }
    if (descriptor.typeId == "crop") {
        return std::make_unique<CropImageNode>(descriptor, instance);
    }
    if (descriptor.typeId == "downscale") {
        return std::make_unique<DownscaleImageNode>(descriptor, instance);
    }
    throw std::invalid_argument("no core implementation for node type '" + descriptor.typeId +
                                "' (M4-04..06 operators are not implemented yet)");
}

}  // namespace rin
