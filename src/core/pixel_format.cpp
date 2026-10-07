#include "pixel_format.hpp"

#include <algorithm>
#include <cmath>

namespace rin {

void jetColor(float t, std::uint8_t out[4]) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    // 经典 jet 近似：r/g/b 各为三角波，相位相差 1/4 周期。
    const float r = std::clamp(1.5f - std::fabs(4.0f * t - 3.0f), 0.0f, 1.0f);
    const float g = std::clamp(1.5f - std::fabs(4.0f * t - 2.0f), 0.0f, 1.0f);
    const float b = std::clamp(1.5f - std::fabs(4.0f * t - 1.0f), 0.0f, 1.0f);
    out[0] = static_cast<std::uint8_t>(r * 255.0f + 0.5f);
    out[1] = static_cast<std::uint8_t>(g * 255.0f + 0.5f);
    out[2] = static_cast<std::uint8_t>(b * 255.0f + 0.5f);
    out[3] = 255;
}

void grayscaleColor(float t, std::uint8_t out[4]) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    // 近白远黑：亮度随归一化深度线性衰减（DEC-007 暂定极性）。
    const std::uint8_t level = static_cast<std::uint8_t>((1.0f - t) * 255.0f + 0.5f);
    out[0] = level;
    out[1] = level;
    out[2] = level;
    out[3] = 255;
}

void adaptiveGrayscaleColor(float t, std::uint8_t out[4]) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    // 近黑远白：t = raw/frameQuantile，分位深度映射为纯白（DEC-007 扩展修订）。
    const std::uint8_t level = static_cast<std::uint8_t>(t * 255.0f + 0.5f);
    out[0] = level;
    out[1] = level;
    out[2] = level;
    out[3] = 255;
}

namespace {

std::size_t requiredBytes(std::uint32_t width, std::uint32_t height) {
    return static_cast<std::size_t>(width) * 4u * height;
}

void fillOpaqueBlack(std::vector<std::uint8_t>& dst) {
    for (std::size_t offset = 0; offset + 3u < dst.size(); offset += 4u) {
        dst[offset] = 0;
        dst[offset + 1u] = 0;
        dst[offset + 2u] = 0;
        dst[offset + 3u] = 255;
    }
}

/// Z16 入口参数校验（M12/CR-07，各转换入口共用）：src 非空、尺寸非零、
/// stride 覆盖行宽。
bool validateDepth16Args(const std::uint16_t* src,
                         std::uint32_t width,
                         std::uint32_t height,
                         std::uint32_t srcStrideUnits) noexcept {
    if (src == nullptr || width == 0 || height == 0) {
        return false;
    }
    return srcStrideUnits >= width;
}

/// 米制归一化区间校验（M12/CR-07）：比例系数为正、far 严格大于 near。
bool validateDepthRange(float depthScaleMeters, float nearMeters,
                        float farMeters) noexcept {
    return depthScaleMeters > 0.0f && farMeters > nearMeters;
}

/// Gray8 ramp 级别（M12/CR-10，与 grayscaleColor/adaptiveGrayscaleColor 同式）：
/// inverted=true 近白远黑 level = round((1−t)·255)；否则近黑远白
/// level = round(t·255)。t 先钳制到 [0,1]。
std::uint8_t grayLevel(float t, bool inverted) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    const float level = inverted ? (1.0f - t) * 255.0f : t * 255.0f;
    return static_cast<std::uint8_t>(level + 0.5f);
}

/// 自适应灰度归一分位（DEC-007 扩展修订，频闪修复）：真机 Z16 存在占比极小、
/// 数值极端的孤立过远/过近噪声，绝对最大值基准会被单个飞点逐帧推动，造成整帧
/// 亮度跳变（频闪）。改取帧内有效深度的 P99 作基准后，占比 <1% 的噪声无法移动
/// 归一化基准；超过分位的像素经 ramp clamp 截断为纯白。
constexpr unsigned kAdaptiveFramePercentile = 99;

/// 帧内有效深度 P99 分位唯一实现（M12/CR-01，DEC-007 扩展修订）：高/低字节
/// 两级 256-bin 直方图，1 基目标秩 = ceil(P99% × validCount)，整数运算避免
/// 浮点边界，无浮点与状态。全帧无效深度（无有效像素）返回 false，不写
/// frameQuantile；否则返回 true 并写出分位基准。
bool computeFrameQuantile(const std::uint16_t* src,
                          std::uint32_t width,
                          std::uint32_t height,
                          std::uint32_t srcStrideUnits,
                          std::uint16_t& frameQuantile) {
    std::uint32_t histHigh[256] = {};
    std::uint64_t validCount = 0;
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint16_t* srcRow = src + static_cast<std::size_t>(row) * srcStrideUnits;
        for (std::uint32_t column = 0; column < width; ++column) {
            const std::uint16_t raw = srcRow[column];
            if (raw != 0) {
                ++histHigh[raw >> 8];
                ++validCount;
            }
        }
    }
    if (validCount == 0) {
        return false;
    }
    const std::uint64_t targetRank =
        (validCount * kAdaptiveFramePercentile + 99u) / 100u;
    std::uint32_t belowTarget = 0;
    unsigned highBin = 255;
    for (unsigned bin = 0; bin < 256; ++bin) {
        if (belowTarget + histHigh[bin] >= targetRank) {
            highBin = bin;
            break;
        }
        belowTarget += histHigh[bin];
    }
    std::uint32_t histLow[256] = {};
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint16_t* srcRow = src + static_cast<std::size_t>(row) * srcStrideUnits;
        for (std::uint32_t column = 0; column < width; ++column) {
            const std::uint16_t raw = srcRow[column];
            if (raw != 0 && (raw >> 8) == highBin) {
                ++histLow[raw & 0xFF];
            }
        }
    }
    const std::uint32_t inBinRank =
        static_cast<std::uint32_t>(targetRank - belowTarget);
    std::uint32_t lowCumulative = 0;
    unsigned lowBin = 255;
    for (unsigned bin = 0; bin < 256; ++bin) {
        lowCumulative += histLow[bin];
        if (lowCumulative >= inBinRank) {
            lowBin = bin;
            break;
        }
    }
    frameQuantile = static_cast<std::uint16_t>((highBin << 8) | lowBin);
    return true;
}

/// 自适应灰度（DEC-007 扩展）：先取帧内有效深度的 P99 分位 frameQuantile
/// （computeFrameQuantile 唯一实现），再按 t = raw / frameQuantile 经
/// adaptiveGrayscaleColor 映射。深度比例对缩放不变（raw/q 与 meters/qMeters
/// 相等），故不使用 depthScale/near/far（校验由 convertDepth16ToRgba8 入口
/// 统一执行）。有效像素不足 100 个时 P99 退化为绝对最大值
/// （rank = ceil(P*N) = N）。
bool convertDepth16ToRgba8Adaptive(const std::uint16_t* src,
                                   std::uint32_t width,
                                   std::uint32_t height,
                                   std::uint32_t srcStrideUnits,
                                   std::vector<std::uint8_t>& dst) {
    dst.resize(requiredBytes(width, height));
    std::uint16_t frameQuantile = 0;
    if (!computeFrameQuantile(src, width, height, srcStrideUnits, frameQuantile)) {
        // 全帧无效深度：与逐像素 raw==0 分支同语义，输出不透明黑。
        fillOpaqueBlack(dst);
        return true;
    }
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint16_t* srcRow = src + static_cast<std::size_t>(row) * srcStrideUnits;
        std::uint8_t* dstRow = dst.data() + static_cast<std::size_t>(row) * width * 4u;
        for (std::uint32_t column = 0; column < width; ++column) {
            const std::uint16_t raw = srcRow[column];
            if (raw == 0) {
                dstRow[column * 4u + 0] = 0;
                dstRow[column * 4u + 1] = 0;
                dstRow[column * 4u + 2] = 0;
                dstRow[column * 4u + 3] = 255;
                continue;
            }
            const float normalized =
                static_cast<float>(raw) / static_cast<float>(frameQuantile);
            adaptiveGrayscaleColor(normalized, dstRow + column * 4u);
        }
    }
    return true;
}

}  // namespace

bool convertRgb8ToRgba8(const std::uint8_t* src,
                        std::uint32_t width,
                        std::uint32_t height,
                        std::uint32_t srcStride,
                        std::vector<std::uint8_t>& dst) {
    if (src == nullptr || width == 0 || height == 0) {
        return false;
    }
    if (srcStride < width * 3u) {
        return false;
    }
    dst.resize(requiredBytes(width, height));
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint8_t* srcRow = src + static_cast<std::size_t>(row) * srcStride;
        std::uint8_t* dstRow = dst.data() + static_cast<std::size_t>(row) * width * 4u;
        for (std::uint32_t column = 0; column < width; ++column) {
            dstRow[column * 4u + 0] = srcRow[column * 3u + 0];
            dstRow[column * 4u + 1] = srcRow[column * 3u + 1];
            dstRow[column * 4u + 2] = srcRow[column * 3u + 2];
            dstRow[column * 4u + 3] = 255;
        }
    }
    return true;
}

bool convertDepth16ToRgba8(const std::uint16_t* src,
                           std::uint32_t width,
                           std::uint32_t height,
                           std::uint32_t srcStrideUnits,
                           float depthScaleMeters,
                           float nearMeters,
                           float farMeters,
                           DepthColorScheme scheme,
                           std::vector<std::uint8_t>& dst) {
    if (!validateDepth16Args(src, width, height, srcStrideUnits) ||
        !validateDepthRange(depthScaleMeters, nearMeters, farMeters)) {
        return false;
    }
    if (scheme == DepthColorScheme::AdaptiveGrayscale) {
        // 参数校验与其它配色一致；adaptive 路径本身不消费 near/far/depthScale。
        return convertDepth16ToRgba8Adaptive(src, width, height, srcStrideUnits, dst);
    }
    using Ramp = void (*)(float, std::uint8_t[4]);
    const Ramp ramp = scheme == DepthColorScheme::Grayscale ? grayscaleColor : jetColor;
    dst.resize(requiredBytes(width, height));
    const float range = farMeters - nearMeters;
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint16_t* srcRow = src + static_cast<std::size_t>(row) * srcStrideUnits;
        std::uint8_t* dstRow = dst.data() + static_cast<std::size_t>(row) * width * 4u;
        for (std::uint32_t column = 0; column < width; ++column) {
            const std::uint16_t raw = srcRow[column];
            if (raw == 0) {
                dstRow[column * 4u + 0] = 0;
                dstRow[column * 4u + 1] = 0;
                dstRow[column * 4u + 2] = 0;
                dstRow[column * 4u + 3] = 255;
                continue;
            }
            const float meters = static_cast<float>(raw) * depthScaleMeters;
            const float normalized = (meters - nearMeters) / range;
            ramp(normalized, dstRow + column * 4u);
        }
    }
    return true;
}

bool convertDepth16ToRgba8Jet(const std::uint16_t* src,
                              std::uint32_t width,
                              std::uint32_t height,
                              std::uint32_t srcStrideUnits,
                              float depthScaleMeters,
                              float nearMeters,
                              float farMeters,
                              std::vector<std::uint8_t>& dst) {
    return convertDepth16ToRgba8(src, width, height, srcStrideUnits, depthScaleMeters,
                                 nearMeters, farMeters, DepthColorScheme::Jet, dst);
}

bool convertDepth16ToGray8(const std::uint16_t* src,
                           std::uint32_t width,
                           std::uint32_t height,
                           std::uint32_t srcStrideUnits,
                           float depthScaleMeters,
                           float nearMeters,
                           float farMeters,
                           std::vector<std::uint8_t>& dst) {
    if (!validateDepth16Args(src, width, height, srcStrideUnits) ||
        !validateDepthRange(depthScaleMeters, nearMeters, farMeters)) {
        return false;
    }
    dst.resize(static_cast<std::size_t>(width) * height);
    const float range = farMeters - nearMeters;
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint16_t* srcRow = src + static_cast<std::size_t>(row) * srcStrideUnits;
        std::uint8_t* dstRow = dst.data() + static_cast<std::size_t>(row) * width;
        for (std::uint32_t column = 0; column < width; ++column) {
            const std::uint16_t raw = srcRow[column];
            if (raw == 0) {
                dstRow[column] = 0;  // 无效深度：黑（Gray8 无 alpha 层）。
                continue;
            }
            // grayscaleColor ramp 同式（近白远黑），经 grayLevel 唯一实现。
            const float meters = static_cast<float>(raw) * depthScaleMeters;
            const float normalized = (meters - nearMeters) / range;
            dstRow[column] = grayLevel(normalized, /*inverted=*/true);
        }
    }
    return true;
}

bool convertDepth16ToGray8Adaptive(const std::uint16_t* src,
                                   std::uint32_t width,
                                   std::uint32_t height,
                                   std::uint32_t srcStrideUnits,
                                   std::vector<std::uint8_t>& dst) {
    if (!validateDepth16Args(src, width, height, srcStrideUnits)) {
        return false;
    }
    // P99 分位与 RGBA8 自适应路径同语义（computeFrameQuantile 唯一实现）。
    dst.resize(static_cast<std::size_t>(width) * height);
    std::uint16_t frameQuantile = 0;
    if (!computeFrameQuantile(src, width, height, srcStrideUnits, frameQuantile)) {
        // 全帧无效深度：与逐像素 raw==0 分支同语义，输出黑。
        std::fill(dst.begin(), dst.end(), 0);
        return true;
    }
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint16_t* srcRow = src + static_cast<std::size_t>(row) * srcStrideUnits;
        std::uint8_t* dstRow = dst.data() + static_cast<std::size_t>(row) * width;
        for (std::uint32_t column = 0; column < width; ++column) {
            const std::uint16_t raw = srcRow[column];
            if (raw == 0) {
                dstRow[column] = 0;
                continue;
            }
            // adaptiveGrayscaleColor ramp 同式（近黑远白），经 grayLevel 唯一实现。
            const float normalized =
                static_cast<float>(raw) / static_cast<float>(frameQuantile);
            dstRow[column] = grayLevel(normalized, /*inverted=*/false);
        }
    }
    return true;
}

bool convertDepth16ToGray8Pair(const std::uint16_t* src,
                               std::uint32_t width,
                               std::uint32_t height,
                               std::uint32_t srcStrideUnits,
                               float depthScaleMeters,
                               float nearMeters,
                               float farMeters,
                               std::vector<std::uint8_t>& gray,
                               std::vector<std::uint8_t>& adaptive) {
    if (!validateDepth16Args(src, width, height, srcStrideUnits) ||
        !validateDepthRange(depthScaleMeters, nearMeters, farMeters)) {
        return false;
    }
    gray.resize(static_cast<std::size_t>(width) * height);
    adaptive.resize(static_cast<std::size_t>(width) * height);
    // P99 分位与单 rendition 自适应路径同一实现（computeFrameQuantile）。
    std::uint16_t frameQuantile = 0;
    if (!computeFrameQuantile(src, width, height, srcStrideUnits, frameQuantile)) {
        // 全帧无效深度：两 rendition 同语义输出黑。
        std::fill(gray.begin(), gray.end(), 0);
        std::fill(adaptive.begin(), adaptive.end(), 0);
        return true;
    }
    // 单趟写出：两 rendition 逐字节等于分别调用两个单 rendition 函数。
    const float range = farMeters - nearMeters;
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint16_t* srcRow = src + static_cast<std::size_t>(row) * srcStrideUnits;
        std::uint8_t* grayRow = gray.data() + static_cast<std::size_t>(row) * width;
        std::uint8_t* adaptiveRow =
            adaptive.data() + static_cast<std::size_t>(row) * width;
        for (std::uint32_t column = 0; column < width; ++column) {
            const std::uint16_t raw = srcRow[column];
            if (raw == 0) {
                grayRow[column] = 0;
                adaptiveRow[column] = 0;
                continue;
            }
            const float fixedNormalized =
                (static_cast<float>(raw) * depthScaleMeters - nearMeters) / range;
            grayRow[column] = grayLevel(fixedNormalized, /*inverted=*/true);
            const float adaptiveNormalized =
                static_cast<float>(raw) / static_cast<float>(frameQuantile);
            adaptiveRow[column] = grayLevel(adaptiveNormalized, /*inverted=*/false);
        }
    }
    return true;
}

}  // namespace rin
