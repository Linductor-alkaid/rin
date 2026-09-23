#include "pixel_format.hpp"

#include <algorithm>
#include <cmath>

namespace rsv {

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

namespace {

std::size_t requiredBytes(std::uint32_t width, std::uint32_t height) {
    return static_cast<std::size_t>(width) * 4u * height;
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

bool convertDepth16ToRgba8Jet(const std::uint16_t* src,
                              std::uint32_t width,
                              std::uint32_t height,
                              std::uint32_t srcStrideUnits,
                              float depthScaleMeters,
                              float nearMeters,
                              float farMeters,
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
            jetColor(normalized, dstRow + column * 4u);
        }
    }
    return true;
}

}  // namespace rsv
