#include "rin/depth_preproc.hpp"

// reflect-101 边界下标与一维高斯核的唯一实现（M12/CR-02、CR-03 收敛，冻结公式）。
#include "sample_kernels.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rin {

namespace {

/// 算子输出的唯一构造路径：先在 mutable 缓冲上填充，再冻结为共享不可变像素
/// （DepthFrameF32 构造后不可变纪律下的合法写序）；输出一律紧凑（stride = 宽）。
/// 元数据被拒属于实现缺陷（输入已通过防御核对，输出尺寸有界），显式抛出。
DepthFrameF32 freezeDepthFrame(std::uint32_t width, std::uint32_t height,
                               std::vector<float> buffer) {
    auto shared = std::make_shared<const std::vector<float>>(std::move(buffer));
    DepthFrameF32 frame = DepthFrameF32::wrap(width, height, width, std::move(shared));
    if (!frame.valid()) {
        throw std::runtime_error("depth operator produced a rejected output buffer");
    }
    return frame;
}

/// 单输入算子的共同防御（DEC-018 失败纪律：无效输入显式报告，不静默）。
void requireValidInput(const char* opName, const DepthFrameF32& frame) {
    if (!frame.valid()) {
        throw std::invalid_argument(std::string(opName) + " input frame is invalid");
    }
}

/// 64 位加法防 uint32 回绕（image_types.cpp metadataValid 同款纪律）。
std::uint64_t u64(std::uint32_t value) noexcept {
    return static_cast<std::uint64_t>(value);
}

/// reflect-101 边界下标：唯一实现见 sample_kernels.hpp（M12/CR-02 收敛，
/// M4-04 冻结公式：−1→1、n→n−2；n<=1 的维度下标恒 0）。
using detail::reflectIndex101;

}  // namespace

DepthFrameF32 DepthFrameF32::make(std::uint32_t width, std::uint32_t height, std::uint32_t stride) {
    // stride 语义为元素数（设计文档 §3）；默认 0 取最小行宽 width 个元素。
    const std::uint64_t effectiveStride = stride == 0 ? u64(width) : u64(stride);
    if (width == 0 || height == 0 || effectiveStride < u64(width)) {
        return {};
    }
    const std::uint64_t bytes = effectiveStride * u64(height) * 4u;
    if (bytes > kMaxDepthFrameBytes) {
        return {};
    }
    auto pixels = std::make_shared<const std::vector<float>>(
        static_cast<std::size_t>(effectiveStride * u64(height)), 0.0f);
    DepthFrameF32 frame;
    frame.width_ = width;
    frame.height_ = height;
    frame.stride_ = static_cast<std::uint32_t>(effectiveStride);
    frame.pixels_ = std::move(pixels);
    return frame;
}

DepthFrameF32 DepthFrameF32::wrap(std::uint32_t width, std::uint32_t height, std::uint32_t stride,
                                  std::shared_ptr<const std::vector<float>> pixels) {
    const std::uint64_t effectiveStride = stride == 0 ? u64(width) : u64(stride);
    if (width == 0 || height == 0 || effectiveStride < u64(width) || pixels == nullptr ||
        pixels->size() < static_cast<std::size_t>(effectiveStride * u64(height))) {
        return {};
    }
    DepthFrameF32 frame;
    frame.width_ = width;
    frame.height_ = height;
    frame.stride_ = static_cast<std::uint32_t>(effectiveStride);
    frame.pixels_ = std::move(pixels);
    return frame;
}

bool DepthFrameF32::valid() const noexcept {
    return pixels_ != nullptr && width_ > 0 && height_ > 0 && u64(stride_) >= u64(width_) &&
           pixels_->size() >= static_cast<std::size_t>(u64(stride_) * u64(height_));
}

std::uint64_t DepthFrameF32::byteSize() const noexcept {
    if (!valid()) {
        return 0;
    }
    return u64(stride_) * u64(height_) * 4u;
}

const float* DepthFrameF32::row(std::uint32_t y) const noexcept {
    if (!valid() || y >= height_) {
        return nullptr;
    }
    return pixels_->data() + u64(y) * u64(stride_);
}

bool PolicyDepthConfig::valid() const noexcept {
    if (gridWidth == 0 || gridHeight == 0) {
        return false;
    }
    // 裁切派生量必须为正（64 位防回绕）。
    if (u64(cropUp) + u64(cropDown) >= u64(gridHeight) ||
        u64(cropLeft) + u64(cropRight) >= u64(gridWidth)) {
        return false;
    }
    if (!(depthNear < depthFar) || !std::isfinite(depthNear) || !std::isfinite(depthFar)) {
        return false;
    }
    if (blurRadius < 1 || blurRadius > 10) {
        return false;
    }
    if (!(blurSigma >= 0.0 && blurSigma <= 10.0) || !std::isfinite(blurSigma)) {
        return false;
    }
    // O7 历史参数约束唯一判据（M12/CR-06，sample_kernels.hpp）。
    if (!detail::historyParamsValid(historyLength, sampleCount, sampleSkip, sampleDelay)) {
        return false;
    }
    return true;
}

/// 裁切后维度唯一推导（M12/CR-14）：grid 减两侧裁切；越界回卷/零/超 uint32
/// 一律 0（调用方以 0 视为配置非法）。
std::uint32_t policyExtent(std::uint64_t grid, std::uint64_t cropLeading,
                           std::uint64_t cropTrailing) noexcept {
    if (cropLeading + cropTrailing > grid) {
        return 0;
    }
    const std::uint64_t extent = grid - (cropLeading + cropTrailing);
    if (extent == 0 || extent > u64(UINT32_MAX)) {
        return 0;
    }
    return static_cast<std::uint32_t>(extent);
}

std::uint32_t PolicyDepthConfig::policyWidth() const noexcept {
    return policyExtent(u64(gridWidth), u64(cropLeft), u64(cropRight));
}

std::uint32_t PolicyDepthConfig::policyHeight() const noexcept {
    return policyExtent(u64(gridHeight), u64(cropUp), u64(cropDown));
}

DepthFrameF32 fillDepthInvalid(const DepthFrameF32& in, double farValue, double invalidBelow) {
    requireValidInput("fillDepthInvalid", in);
    if (!std::isfinite(farValue)) {
        throw std::invalid_argument("fillDepthInvalid farValue must be finite");
    }
    const auto far = static_cast<float>(farValue);
    const auto below = static_cast<float>(invalidBelow);
    std::vector<float> out(static_cast<std::size_t>(u64(in.width()) * u64(in.height())));
    for (std::uint32_t y = 0; y < in.height(); ++y) {
        const float* src = in.row(y);
        float* dst = out.data() + u64(y) * u64(in.width());
        for (std::uint32_t x = 0; x < in.width(); ++x) {
            const float v = src[x];
            dst[x] = (!std::isfinite(v) || v <= below) ? far : v;
        }
    }
    return freezeDepthFrame(in.width(), in.height(), std::move(out));
}

DepthFrameF32 resizeDepthArea(const DepthFrameF32& in, std::uint32_t dstWidth,
                              std::uint32_t dstHeight) {
    requireValidInput("resizeDepthArea", in);
    if (dstWidth == 0 || dstHeight == 0) {
        throw std::invalid_argument("resizeDepthArea destination size must be positive");
    }
    if (dstWidth > in.width() || dstHeight > in.height()) {
        throw std::invalid_argument(
            "resizeDepthArea only supports shrinking (upscaling semantics rejected)");
    }
    if (dstWidth == in.width() && dstHeight == in.height()) {
        // 同尺寸恒等（紧凑新缓冲）。
        std::vector<float> out(static_cast<std::size_t>(u64(dstWidth) * u64(dstHeight)));
        for (std::uint32_t y = 0; y < dstHeight; ++y) {
            std::copy_n(in.row(y), dstWidth, out.data() + u64(y) * u64(dstWidth));
        }
        return freezeDepthFrame(dstWidth, dstHeight, std::move(out));
    }

    const double sx = static_cast<double>(in.width()) / static_cast<double>(dstWidth);
    const double sy = static_cast<double>(in.height()) / static_cast<double>(dstHeight);
    std::vector<float> out(static_cast<std::size_t>(u64(dstWidth) * u64(dstHeight)));
    for (std::uint32_t dy = 0; dy < dstHeight; ++dy) {
        // 目标行 dy 的源覆盖区间 [y0, y1)；行内各列共享行权重（先算好）。
        const double y0 = static_cast<double>(dy) * sy;
        const double y1 = static_cast<double>(dy + 1) * sy;
        struct RowWeight {
            std::uint32_t index;
            double weight;
        };
        // 源行数上界 = ceil(sy) + 1；848×480 → 64×36 时 sy ≈ 13.3，栈上缓冲足够。
        std::vector<RowWeight> rows;
        rows.reserve(static_cast<std::size_t>(std::ceil(sy)) + 2);
        for (std::uint32_t j = static_cast<std::uint32_t>(std::floor(y0));
             j <
             std::min<std::uint32_t>(static_cast<std::uint32_t>(std::ceil(y1)) + 1u, in.height());
             ++j) {
            const double h =
                std::min(y1, static_cast<double>(j) + 1.0) - std::max(y0, static_cast<double>(j));
            if (h > 0.0) {
                rows.push_back({j, h});
            }
        }
        float* dst = out.data() + u64(dy) * u64(dstWidth);
        for (std::uint32_t dx = 0; dx < dstWidth; ++dx) {
            const double x0 = static_cast<double>(dx) * sx;
            const double x1 = static_cast<double>(dx + 1) * sx;
            double sum = 0.0;
            for (const RowWeight& rw : rows) {
                const float* src = in.row(rw.index);
                double rowSum = 0.0;
                for (std::uint32_t i = static_cast<std::uint32_t>(std::floor(x0));
                     i < std::min<std::uint32_t>(static_cast<std::uint32_t>(std::ceil(x1)) + 1u,
                                                 in.width());
                     ++i) {
                    const double w = std::min(x1, static_cast<double>(i) + 1.0) -
                                     std::max(x0, static_cast<double>(i));
                    if (w > 0.0) {
                        rowSum += w * static_cast<double>(src[i]);
                    }
                }
                sum += rw.weight * rowSum;
            }
            dst[dx] = static_cast<float>(sum / (sx * sy));
        }
    }
    return freezeDepthFrame(dstWidth, dstHeight, std::move(out));
}

DepthFrameF32 cropDepth(const DepthFrameF32& in, std::uint32_t up, std::uint32_t down,
                        std::uint32_t left, std::uint32_t right) {
    requireValidInput("cropDepth", in);
    if (u64(up) + u64(down) >= u64(in.height()) || u64(left) + u64(right) >= u64(in.width())) {
        throw std::invalid_argument("cropDepth region degenerates or exceeds input");
    }
    const std::uint32_t outWidth =
        static_cast<std::uint32_t>(u64(in.width()) - u64(left) - u64(right));
    const std::uint32_t outHeight =
        static_cast<std::uint32_t>(u64(in.height()) - u64(up) - u64(down));
    std::vector<float> out(static_cast<std::size_t>(u64(outWidth) * u64(outHeight)));
    for (std::uint32_t y = 0; y < outHeight; ++y) {
        const float* src = in.row(y + up) + left;
        std::copy_n(src, outWidth, out.data() + u64(y) * u64(outWidth));
    }
    return freezeDepthFrame(outWidth, outHeight, std::move(out));
}

DepthFrameF32 gaussianBlurDepth(const DepthFrameF32& in, std::uint32_t radius, double sigma) {
    requireValidInput("gaussianBlurDepth", in);
    if (radius < 1 || radius > 10) {
        throw std::invalid_argument("gaussianBlurDepth radius must be in [1, 10]");
    }
    if (!(sigma >= 0.0 && sigma <= 10.0) || !std::isfinite(sigma)) {
        throw std::invalid_argument("gaussianBlurDepth sigma must be in [0, 10]");
    }
    const std::size_t kSize = 2u * radius + 1u;
    // 一维高斯核唯一实现（M12/CR-03；sigma=0 为 δ 核的极限语义）。
    const std::vector<double> kernel = detail::gaussianKernel1D(radius, sigma);

    const std::uint32_t w = in.width();
    const std::uint32_t h = in.height();
    // 可分离两趟：水平 → 垂直；中间量 double 不量化，最终一次转 float32。
    std::vector<double> horizontal(static_cast<std::size_t>(u64(w) * u64(h)), 0.0);
    const std::int64_t wi = static_cast<std::int64_t>(w);
    for (std::uint32_t y = 0; y < h; ++y) {
        const float* src = in.row(y);
        double* dst = horizontal.data() + u64(y) * u64(w);
        for (std::uint32_t x = 0; x < w; ++x) {
            double acc = 0.0;
            for (std::size_t k = 0; k < kSize; ++k) {
                const std::int64_t sx =
                    reflectIndex101(static_cast<std::int64_t>(x) + static_cast<std::int64_t>(k) -
                                        static_cast<std::int64_t>(radius),
                                    wi);
                acc += kernel[k] * static_cast<double>(src[static_cast<std::size_t>(sx)]);
            }
            dst[x] = acc;
        }
    }
    std::vector<double> vertical(static_cast<std::size_t>(u64(w) * u64(h)), 0.0);
    const std::int64_t hi = static_cast<std::int64_t>(h);
    for (std::uint32_t y = 0; y < h; ++y) {
        double* dst = vertical.data() + u64(y) * u64(w);
        for (std::uint32_t x = 0; x < w; ++x) {
            double acc = 0.0;
            for (std::size_t k = 0; k < kSize; ++k) {
                const std::int64_t sy =
                    reflectIndex101(static_cast<std::int64_t>(y) + static_cast<std::int64_t>(k) -
                                        static_cast<std::int64_t>(radius),
                                    hi);
                acc += kernel[k] * horizontal[static_cast<std::size_t>(sy) * u64(w) + x];
            }
            dst[x] = acc;
        }
    }
    std::vector<float> out(static_cast<std::size_t>(u64(w) * u64(h)));
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<float>(vertical[i]);
    }
    return freezeDepthFrame(w, h, std::move(out));
}

DepthFrameF32 clipNormalizeDepth(const DepthFrameF32& in, double near, double far) {
    requireValidInput("clipNormalizeDepth", in);
    if (!(near < far)) {
        throw std::invalid_argument("clipNormalizeDepth requires near < far");
    }
    const double range = far - near;
    std::vector<float> out(static_cast<std::size_t>(u64(in.width()) * u64(in.height())));
    for (std::uint32_t y = 0; y < in.height(); ++y) {
        const float* src = in.row(y);
        float* dst = out.data() + u64(y) * u64(in.width());
        for (std::uint32_t x = 0; x < in.width(); ++x) {
            const double v = std::min(std::max(static_cast<double>(src[x]), near), far);
            dst[x] = static_cast<float>((v - near) / range);
        }
    }
    return freezeDepthFrame(in.width(), in.height(), std::move(out));
}

DepthFrameF32 preprocessPolicyDepthFrame(const DepthFrameF32& rawMetricDepth,
                                         const PolicyDepthConfig& config) {
    if (!config.valid()) {
        throw std::invalid_argument("preprocessPolicyDepthFrame config is invalid");
    }
    requireValidInput("preprocessPolicyDepthFrame", rawMetricDepth);
    const DepthFrameF32 filled = fillDepthInvalid(rawMetricDepth, config.depthFar);
    const DepthFrameF32 grid = resizeDepthArea(filled, config.gridWidth, config.gridHeight);
    const DepthFrameF32 cropped =
        cropDepth(grid, config.cropUp, config.cropDown, config.cropLeft, config.cropRight);
    const DepthFrameF32 blurred = gaussianBlurDepth(cropped, config.blurRadius, config.blurSigma);
    return clipNormalizeDepth(blurred, config.depthNear, config.depthFar);
}

PolicyDepthHistory::PolicyDepthHistory(const PolicyDepthConfig& config) : config_(config) {
    if (!config_.valid()) {
        throw std::invalid_argument("PolicyDepthHistory config is invalid");
    }
    ring_.assign(static_cast<std::size_t>(u64(config_.historyLength) * u64(config_.policyHeight()) *
                                          u64(config_.policyWidth())),
                 0.0f);
}

void PolicyDepthHistory::reset() noexcept {
    head_ = 0;
    count_ = 0;
}

void PolicyDepthHistory::append(const DepthFrameF32& frame) {
    if (!frame.valid()) {
        throw std::invalid_argument("PolicyDepthHistory append frame is invalid");
    }
    if (frame.width() != config_.policyWidth() || frame.height() != config_.policyHeight()) {
        throw std::invalid_argument(
            "PolicyDepthHistory append frame size mismatch: expected " +
            std::to_string(config_.policyWidth()) + "x" + std::to_string(config_.policyHeight()) +
            ", got " + std::to_string(frame.width()) + "x" + std::to_string(frame.height()));
    }
    const std::size_t frameSize =
        static_cast<std::size_t>(u64(config_.policyWidth()) * u64(config_.policyHeight()));
    // 逐行按源 stride 拷贝（帧可能带行尾 padding，禁止从 row(0) 起扁平拷贝）。
    float* dst = ring_.data() + head_ * frameSize;
    const std::uint32_t width = config_.policyWidth();
    for (std::uint32_t y = 0; y < config_.policyHeight(); ++y) {
        std::copy_n(frame.row(y), width, dst + u64(y) * u64(width));
    }
    // 入环推进唯一实现（M12/CR-06）。
    detail::advanceHistoryRing(head_, count_, config_.historyLength);
}

std::vector<float> PolicyDepthHistory::sample() const {
    const std::size_t frameSize =
        static_cast<std::size_t>(u64(config_.policyWidth()) * u64(config_.policyHeight()));
    std::vector<float> out(config_.sampleCount * frameSize, 0.0f);
    if (count_ == 0) {
        return out;
    }
    // 抽样下标唯一实现（M12/CR-06）：欠帧首帧填充，概念序列 = [首帧] ×
    // (L − count) + 实际帧（oldest→newest）。
    for (std::size_t i = 0; i < config_.sampleCount; ++i) {
        const std::size_t pos = detail::historySamplePos(config_.historyLength, count_, head_,
                                                         i, config_.sampleCount,
                                                         config_.sampleSkip,
                                                         config_.sampleDelay);
        std::copy_n(ring_.data() + pos * frameSize, frameSize, out.data() + i * frameSize);
    }
    return out;
}

}  // namespace rin
