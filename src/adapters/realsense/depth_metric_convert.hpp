#pragma once

// Z16 → 米制 float32 线性换算（M9，DEC-019）。header-only 且零 SDK 依赖：
// 与 librealsense2 解耦，便于无设备单测（test_motion_ingest 同款边界——
// adapter 内的纯数学单元独立可测）。语义冻结于 DEC-019：
// - 米制值 = Z16 码值 × depth_scale（distance_to_image_plane，米）；
// - 无效像素（Z16 = 0）保持 0.0——rendition 保持测量原值，"无效 = 远距"
//   的策略语义由消费方 fillDepthInvalid（DEC-018 O1）决定；
// - strideUnits 为行元素数（uint16 计），允许行尾 padding。
// 失败（空指针/零尺寸/stride 不足/非法 scale）返回 false，不部分写出。

#include <cmath>
#include <cstdint>
#include <vector>

namespace rin_realsense {

[[nodiscard]] inline bool convertDepth16ToMetric(const std::uint16_t* data, std::uint32_t width,
                                                 std::uint32_t height, std::uint32_t strideUnits,
                                                 float depthScale, std::vector<float>& out) {
    if (data == nullptr || width == 0 || height == 0 || strideUnits < width ||
        !(depthScale > 0.0f) || !std::isfinite(depthScale)) {
        return false;
    }
    out.assign(static_cast<std::size_t>(width) * height, 0.0f);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint16_t* src = data + static_cast<std::size_t>(strideUnits) * y;
        float* dst = out.data() + static_cast<std::size_t>(width) * y;
        for (std::uint32_t x = 0; x < width; ++x) {
            dst[x] = static_cast<float>(src[x]) * depthScale;
        }
    }
    return true;
}

}  // namespace rin_realsense
