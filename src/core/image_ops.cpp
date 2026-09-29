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

/// round-half-up 量化后饱和到 [0,255]（M4-03 bilinear 同款量化语义，M4-04 起为
/// 卷积/高斯族共用助手）。
std::uint8_t quantizeU8(double value) {
    const double quantized = std::floor(value + 0.5);
    return static_cast<std::uint8_t>(std::clamp(quantized, 0.0, 255.0));
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

/// 边界填充策略（M4-04 冻结语义，§7）。
enum class ConvBorder {
    Clamp,    /// 复制边缘像素（下标饱和到 [0,n−1]，默认；模糊类核无暗边）。
    Reflect,  /// 镜像不重复边缘像素（reflect-101：−1→1、n→n−2）。
    Zero,     /// 越界像素按 0（黑）计，对应系数项不贡献累加和。
};

/// Reflect-101 采样下标：周期 2(n−1) 折返（−1→1、n→n−2）；n=1 时一律取 0
/// （零宽高在 ImageU8 层已被拒绝，此为防御）。
std::int64_t reflectIndex(std::int64_t index, std::int64_t size) {
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

/// 自定义卷积（M4-04）：Gray8 → Gray8，核尺寸/系数/边界策略构造期定型；输出
/// 与输入同尺寸。执行为相关语义（核不翻转，系数矩阵与邻域逐点对应），double
/// 累加后 round-half-up 饱和量化；数值语义见 image_workflow_design.md §7。
class ConvKernelImageNode final : public IImageNode {
public:
    ConvKernelImageNode(const NodeDescriptor& descriptor, const NodeInstance& instance)
        : descriptor_(descriptor) {
        const std::optional<std::string> size = paramEnumeration(descriptor_, instance, "size");
        if (!size) {
            throw std::invalid_argument("'" + descriptor_.typeId + "': parameter 'size'"
                                        " is missing or has a mismatched kind");
        }
        if (*size == "1") {
            kernelSize_ = 1;
        } else if (*size == "3") {
            kernelSize_ = 3;
        } else if (*size == "5") {
            kernelSize_ = 5;
        } else {
            throw std::invalid_argument("'" + descriptor_.typeId + "': unknown size option '" +
                                        *size + "'");
        }
        const std::optional<std::vector<double>> kernel =
            paramRealArray(descriptor_, instance, "kernel");
        if (!kernel) {
            throw std::invalid_argument("'" + descriptor_.typeId + "': parameter 'kernel'"
                                        " is missing or has a mismatched kind");
        }
        if (kernel->size() != static_cast<std::size_t>(kernelSize_) * kernelSize_) {
            throw std::invalid_argument(
                "'" + descriptor_.typeId + "': kernel expects " +
                std::to_string(kernelSize_ * kernelSize_) + " coefficients (row-major " +
                std::to_string(kernelSize_) + "x" + std::to_string(kernelSize_) + "), got " +
                std::to_string(kernel->size()));
        }
        for (const double coefficient : *kernel) {
            if (!std::isfinite(coefficient)) {
                throw std::invalid_argument("'" + descriptor_.typeId +
                                            "': kernel coefficients must be finite");
            }
        }
        kernel_ = *kernel;
        border_ = requiredBorder(descriptor_, instance);
    }

    [[nodiscard]] const NodeDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }

    [[nodiscard]] std::vector<ImageU8> apply(const std::vector<ImageU8>& inputs) const override {
        const ImageU8& source = singleInput(descriptor_, inputs);
        const std::uint32_t width = source.width();
        const std::uint32_t height = source.height();
        const std::uint32_t es = elementSize(source.format());
        const std::int64_t radius = kernelSize_ / 2;
        // 采样下标预解析（每输出坐标 × K；Zero 策略越界为 -1 哨兵，跳过累加）。
        std::vector<std::int64_t> sourceX(static_cast<std::size_t>(width) * kernelSize_);
        for (std::uint32_t x = 0; x < width; ++x) {
            for (std::int64_t j = 0; j < kernelSize_; ++j) {
                sourceX[static_cast<std::size_t>(x) * kernelSize_ + j] =
                    resolveBorder(static_cast<std::int64_t>(x) + j - radius, width);
            }
        }
        std::vector<std::int64_t> sourceY(static_cast<std::size_t>(height) * kernelSize_);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::int64_t j = 0; j < kernelSize_; ++j) {
                sourceY[static_cast<std::size_t>(y) * kernelSize_ + j] =
                    resolveBorder(static_cast<std::int64_t>(y) + j - radius, height);
            }
        }
        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(width) * es * height);
        for (std::uint32_t y = 0; y < height; ++y) {
            const std::int64_t* offsetY = &sourceY[static_cast<std::size_t>(y) * kernelSize_];
            std::uint8_t* dstRow = buffer.data() + static_cast<std::size_t>(y) * width * es;
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::int64_t* offsetX = &sourceX[static_cast<std::size_t>(x) * kernelSize_];
                std::uint8_t* dst = dstRow + static_cast<std::size_t>(x) * es;
                for (std::uint32_t c = 0; c < es; ++c) {
                    double accumulator = 0.0;
                    for (std::int64_t ky = 0; ky < kernelSize_; ++ky) {
                        if (offsetY[ky] < 0) {
                            continue;  // Zero 策略越界：按黑计，不贡献。
                        }
                        const std::uint8_t* row = source.row(
                            static_cast<std::uint32_t>(offsetY[ky]));
                        for (std::int64_t kx = 0; kx < kernelSize_; ++kx) {
                            if (offsetX[kx] < 0) {
                                continue;
                            }
                            accumulator += kernel_[static_cast<std::size_t>(ky) * kernelSize_ +
                                                   kx] *
                                           row[static_cast<std::size_t>(offsetX[kx]) * es + c];
                        }
                    }
                    dst[c] = quantizeU8(accumulator);
                }
            }
        }
        return {freezeImage(source.format(), width, height, std::move(buffer))};
    }

private:
    /// 构造期边界策略读取（ Enumeration，默认 clamp 由目录声明给出；此处读取
    /// 生效值并防御非法选项/种类错位）。
    static ConvBorder requiredBorder(const NodeDescriptor& descriptor,
                                     const NodeInstance& instance) {
        const std::optional<std::string> border = paramEnumeration(descriptor, instance, "border");
        if (!border) {
            throw std::invalid_argument("'" + descriptor.typeId + "': parameter 'border'"
                                        " is missing or has a mismatched kind");
        }
        if (*border == "clamp") {
            return ConvBorder::Clamp;
        }
        if (*border == "reflect") {
            return ConvBorder::Reflect;
        }
        if (*border == "zero") {
            return ConvBorder::Zero;
        }
        throw std::invalid_argument("'" + descriptor.typeId + "': unknown border option '" +
                                    *border + "'");
    }

    /// 采样下标解析：越界按策略映射；Zero 策略返回 -1 哨兵（按黑计）。
    [[nodiscard]] std::int64_t resolveBorder(std::int64_t index, std::uint32_t size) const {
        const std::int64_t bound = static_cast<std::int64_t>(size);
        switch (border_) {
        case ConvBorder::Clamp:
            return std::clamp(index, std::int64_t{0}, bound - 1);
        case ConvBorder::Reflect:
            return reflectIndex(index, bound);
        case ConvBorder::Zero:
            return (index >= 0 && index < bound) ? index : std::int64_t{-1};
        }
        return 0;
    }

    NodeDescriptor descriptor_;  /// 值拷贝：工厂与节点实例生命周期解耦。
    std::int64_t kernelSize_ = 3;
    std::vector<double> kernel_{0, 0, 0, 0, 1, 0, 0, 0, 0};
    ConvBorder border_ = ConvBorder::Clamp;
};

/// 高斯模糊（M4-04）：Gray8 → Gray8，半径（K = 2·radius+1）与 sigma 构造期定型；
/// 可分离两趟（水平→垂直）+ 固定 clamp 边界 + double 中间结果一次量化。数值
/// 语义见 image_workflow_design.md §7（sigma=0 为 δ 核恒等输出）。
class GaussianBlurImageNode final : public IImageNode {
public:
    GaussianBlurImageNode(const NodeDescriptor& descriptor, const NodeInstance& instance)
        : descriptor_(descriptor) {
        const std::optional<std::int64_t> radius = paramInteger(descriptor_, instance, "radius");
        if (!radius) {
            throw std::invalid_argument("'" + descriptor_.typeId + "': parameter 'radius'"
                                        " is missing or has a mismatched kind");
        }
        // 图准入范围 [1,10]；构造期复核为防御运行期直接构造的实例。
        if (*radius < 1 || *radius > 10) {
            throw std::invalid_argument("'" + descriptor_.typeId + "': radius must be in"
                                        " [1, 10], got " + std::to_string(*radius));
        }
        radius_ = static_cast<int>(*radius);
        const std::optional<double> sigma = paramReal(descriptor_, instance, "sigma");
        if (!sigma) {
            throw std::invalid_argument("'" + descriptor_.typeId + "': parameter 'sigma'"
                                        " is missing or has a mismatched kind");
        }
        if (!std::isfinite(*sigma) || *sigma < 0.0 || *sigma > 10.0) {
            throw std::invalid_argument("'" + descriptor_.typeId + "': sigma must be finite"
                                        " in [0, 10], got " + std::to_string(*sigma));
        }
        sigma_ = *sigma;
    }

    [[nodiscard]] const NodeDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }

    [[nodiscard]] std::vector<ImageU8> apply(const std::vector<ImageU8>& inputs) const override {
        const ImageU8& source = singleInput(descriptor_, inputs);
        const std::uint32_t width = source.width();
        const std::uint32_t height = source.height();
        const std::uint32_t es = elementSize(source.format());
        const std::vector<double> kernel = gaussianKernel();
        const int radius = radius_;
        // 水平趟：行内 clamp 下标（逐行预解析源下标，double 中间结果不量化）。
        std::vector<std::int64_t> offsetX(static_cast<std::size_t>(width) * kernel.size());
        for (std::uint32_t x = 0; x < width; ++x) {
            for (std::size_t j = 0; j < kernel.size(); ++j) {
                offsetX[static_cast<std::size_t>(x) * kernel.size() + j] = std::clamp(
                    static_cast<std::int64_t>(x) + static_cast<std::int64_t>(j) - radius,
                    std::int64_t{0}, static_cast<std::int64_t>(width) - 1);
            }
        }
        std::vector<double> intermediate(static_cast<std::size_t>(width) * es * height);
        for (std::uint32_t y = 0; y < height; ++y) {
            const std::uint8_t* srcRow = source.row(y);
            double* dstRow = intermediate.data() + static_cast<std::size_t>(y) * width * es;
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::int64_t* offsets =
                    &offsetX[static_cast<std::size_t>(x) * kernel.size()];
                double* dst = dstRow + static_cast<std::size_t>(x) * es;
                for (std::uint32_t c = 0; c < es; ++c) {
                    double accumulator = 0.0;
                    for (std::size_t j = 0; j < kernel.size(); ++j) {
                        accumulator +=
                            kernel[j] * srcRow[static_cast<std::size_t>(offsets[j]) * es + c];
                    }
                    dst[c] = accumulator;
                }
            }
        }
        // 垂直趟：中间结果行下标 clamp，最终一次 round-half-up 饱和量化。
        std::vector<const double*> rowOffsets(kernel.size());
        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(width) * es * height);
        for (std::uint32_t y = 0; y < height; ++y) {
            std::uint8_t* dstRow = buffer.data() + static_cast<std::size_t>(y) * width * es;
            for (std::size_t j = 0; j < kernel.size(); ++j) {
                const std::int64_t sy = std::clamp(static_cast<std::int64_t>(y) +
                                                       static_cast<std::int64_t>(j) - radius,
                                                   std::int64_t{0},
                                                   static_cast<std::int64_t>(height) - 1);
                rowOffsets[j] = intermediate.data() + static_cast<std::size_t>(sy) * width * es;
            }
            for (std::uint32_t x = 0; x < width; ++x) {
                std::uint8_t* dst = dstRow + static_cast<std::size_t>(x) * es;
                for (std::uint32_t c = 0; c < es; ++c) {
                    double accumulator = 0.0;
                    for (std::size_t j = 0; j < kernel.size(); ++j) {
                        accumulator += kernel[j] * rowOffsets[j][static_cast<std::size_t>(x) * es + c];
                    }
                    dst[c] = quantizeU8(accumulator);
                }
            }
        }
        return {freezeImage(source.format(), width, height, std::move(buffer))};
    }

private:
    /// 一维高斯核：G[i] ∝ exp(−(i−r)²/(2σ²)) 归一化 Σ=1；sigma=0 为 δ 核
    /// （极限语义，输出逐像素恒等）。二维核为该核与自身的外积（对称可分离）。
    [[nodiscard]] std::vector<double> gaussianKernel() const {
        const std::size_t size = static_cast<std::size_t>(2 * radius_ + 1);
        std::vector<double> kernel(size, 0.0);
        if (sigma_ == 0.0) {
            kernel[static_cast<std::size_t>(radius_)] = 1.0;
            return kernel;
        }
        double sum = 0.0;
        for (std::size_t i = 0; i < size; ++i) {
            const double delta = static_cast<double>(i) - static_cast<double>(radius_);
            kernel[i] = std::exp(-(delta * delta) / (2.0 * sigma_ * sigma_));
            sum += kernel[i];
        }
        for (double& value : kernel) {
            value /= sum;
        }
        return kernel;
    }

    NodeDescriptor descriptor_;  /// 值拷贝：工厂与节点实例生命周期解耦。
    int radius_ = 3;
    double sigma_ = 1.5;
};

/// 灰度化（M4-05）：Rgba8 → Gray8，BT.601 定点亮度——RGBA 亮度域的唯一冻结
/// 公式（image_workflow_design.md §7）：Y = (77·R + 150·G + 29·B + 128) >> 8
/// （+128 右移即 round-half-up；Σ系数 = 256 保证 [0,255] 自然有界，纯整数无
/// 浮点；alpha 不参与）。无参数节点，构造期无需读取参数。
class GrayifyImageNode final : public IImageNode {
public:
    explicit GrayifyImageNode(NodeDescriptor descriptor) noexcept
        : descriptor_(std::move(descriptor)) {}

    [[nodiscard]] const NodeDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }

    [[nodiscard]] std::vector<ImageU8> apply(const std::vector<ImageU8>& inputs) const override {
        const ImageU8& source = singleInput(descriptor_, inputs);
        const std::uint32_t width = source.width();
        const std::uint32_t height = source.height();
        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(width) * height);
        for (std::uint32_t y = 0; y < height; ++y) {
            const std::uint8_t* srcRow = source.row(y);
            std::uint8_t* dstRow = buffer.data() + static_cast<std::size_t>(y) * width;
            for (std::uint32_t x = 0; x < width; ++x) {
                // Rgba8 字节序：byte0 = R、byte1 = G、byte2 = B；byte3 = α 不参与。
                const std::uint8_t* pixel = srcRow + static_cast<std::size_t>(x) * 4;
                dstRow[x] = static_cast<std::uint8_t>(
                    (77u * pixel[0] + 150u * pixel[1] + 29u * pixel[2] + 128u) >> 8);
            }
        }
        return {freezeImage(PortType::Gray8, width, height, std::move(buffer))};
    }

private:
    NodeDescriptor descriptor_;  /// 值拷贝：工厂与节点实例生命周期解耦。
};

/// 直方图均衡（M4-05）：Gray8 → Gray8，cdf_min 映射 + 整数 round-half-up
/// （image_workflow_design.md §7）：out[v] = round-half-up(255·(cdf[v] − cdf_min)/
/// (N − cdf_min))；最大出现灰度精确映射 255、最小出现灰度映射 0；常值图（分母
/// 为零）定义为恒等输出。无参数节点。
class HistEqImageNode final : public IImageNode {
public:
    explicit HistEqImageNode(NodeDescriptor descriptor) noexcept
        : descriptor_(std::move(descriptor)) {}

    [[nodiscard]] const NodeDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }

    [[nodiscard]] std::vector<ImageU8> apply(const std::vector<ImageU8>& inputs) const override {
        const ImageU8& source = singleInput(descriptor_, inputs);
        const std::uint32_t width = source.width();
        const std::uint32_t height = source.height();
        // 全图直方图（singleInput 已核对 Gray8，1 字节/像素）。
        std::uint64_t histogram[256] = {};
        for (std::uint32_t y = 0; y < height; ++y) {
            const std::uint8_t* srcRow = source.row(y);
            for (std::uint32_t x = 0; x < width; ++x) {
                ++histogram[srcRow[x]];
            }
        }
        // cdf_min = 最小出现灰度的计数；常值图（cdf_min == N）恒等输出。
        std::uint64_t cdfMin = 0;
        for (std::uint32_t v = 0; v < 256; ++v) {
            if (histogram[v] != 0) {
                cdfMin = histogram[v];
                break;
            }
        }
        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(width) * height);
        if (cdfMin == static_cast<std::uint64_t>(width) * height) {
            // 常值图：无对比度可拉伸，逐像素保留原值（新紧凑缓冲，不共享源像素）。
            for (std::uint32_t y = 0; y < height; ++y) {
                std::copy_n(source.row(y), width,
                            buffer.data() + static_cast<std::size_t>(y) * width);
            }
            return {freezeImage(PortType::Gray8, width, height, std::move(buffer))};
        }
        // 256 级映射表：整数精确 round-half-up out = floor((2·num + den)/(2·den))，
        // num = (cdf[v] − cdf_min)·255、den = N − cdf_min（uint64：N ≤ kMaxImageBytes
        // ≤ 2²⁴，无溢出）；v_min 之前的空 bin 无像素可达，取值不参与输出。
        const std::uint64_t denominator =
            static_cast<std::uint64_t>(width) * height - cdfMin;
        std::uint8_t lut[256];
        std::uint64_t cumulative = 0;
        bool seenValue = false;
        for (std::uint32_t v = 0; v < 256; ++v) {
            cumulative += histogram[v];
            if (!seenValue && histogram[v] != 0) {
                seenValue = true;
            }
            if (!seenValue) {
                lut[v] = 0;
                continue;
            }
            const std::uint64_t numerator = (cumulative - cdfMin) * 255u;
            lut[v] = static_cast<std::uint8_t>((2u * numerator + denominator) /
                                               (2u * denominator));
        }
        for (std::uint32_t y = 0; y < height; ++y) {
            const std::uint8_t* srcRow = source.row(y);
            std::uint8_t* dstRow = buffer.data() + static_cast<std::size_t>(y) * width;
            for (std::uint32_t x = 0; x < width; ++x) {
                dstRow[x] = lut[srcRow[x]];
            }
        }
        return {freezeImage(PortType::Gray8, width, height, std::move(buffer))};
    }

private:
    NodeDescriptor descriptor_;  /// 值拷贝：工厂与节点实例生命周期解耦。
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
    if (descriptor.typeId == "gaussian_blur") {
        return std::make_unique<GaussianBlurImageNode>(descriptor, instance);
    }
    if (descriptor.typeId == "conv_kernel") {
        return std::make_unique<ConvKernelImageNode>(descriptor, instance);
    }
    if (descriptor.typeId == "grayify") {
        return std::make_unique<GrayifyImageNode>(descriptor);
    }
    if (descriptor.typeId == "hist_eq") {
        return std::make_unique<HistEqImageNode>(descriptor);
    }
    throw std::invalid_argument("no core implementation for node type '" + descriptor.typeId +
                                "' (M4-06 FFT operators are not implemented yet)");
}

}  // namespace rin
