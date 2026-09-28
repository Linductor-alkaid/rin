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
    // 近黑远白：t = raw/frameMax，帧内最远有效像素恒为纯白（DEC-007 扩展）。
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

/// 自适应灰度（DEC-007 扩展）：两趟扫描——先取帧内最大有效原始值，再按
/// t = raw / frameMax 经 adaptiveGrayscaleColor 映射。深度比例对缩放不变
/// （raw/max 与 meters/maxMeters 相等），故不使用 depthScale/near/far。
bool convertDepth16ToRgba8Adaptive(const std::uint16_t* src,
                                   std::uint32_t width,
                                   std::uint32_t height,
                                   std::uint32_t srcStrideUnits,
                                   std::vector<std::uint8_t>& dst) {
    std::uint16_t maxRaw = 0;
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint16_t* srcRow = src + static_cast<std::size_t>(row) * srcStrideUnits;
        for (std::uint32_t column = 0; column < width; ++column) {
            maxRaw = std::max(maxRaw, srcRow[column]);
        }
    }
    dst.resize(requiredBytes(width, height));
    if (maxRaw == 0) {
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
                static_cast<float>(raw) / static_cast<float>(maxRaw);
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
    if (src == nullptr || width == 0 || height == 0) {
        return false;
    }
    if (srcStrideUnits < width) {
        return false;
    }
    if (!(depthScaleMeters > 0.0f) || !(farMeters > nearMeters)) {
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

}  // namespace rin
