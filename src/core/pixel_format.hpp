#pragma once

#include <cstdint>
#include <vector>

#include "rin/camera_types.hpp"

namespace rin {

/// RGB8（打包，srcStride 字节行距）-> RGBA8（打包，dst 行距 width*4）。
/// dst 由函数调整大小；src 长度不足返回 false 且不写 dst。
bool convertRgb8ToRgba8(const std::uint8_t* src,
                        std::uint32_t width,
                        std::uint32_t height,
                        std::uint32_t srcStride,
                        std::vector<std::uint8_t>& dst);

/// Z16 -> RGBA8（按所选配色，DEC-007）。
/// depthScaleMeters：每单位原始值对应的米数；区间 [nearMeters, farMeters] 线性归一后
/// 经 scheme 对应的 ramp 映射，区间外截断；0（无效深度）输出不透明黑。
bool convertDepth16ToRgba8(const std::uint16_t* src,
                           std::uint32_t width,
                           std::uint32_t height,
                           std::uint32_t srcStrideUnits,
                           float depthScaleMeters,
                           float nearMeters,
                           float farMeters,
                           DepthColorScheme scheme,
                           std::vector<std::uint8_t>& dst);

/// Z16 -> RGBA8 jet 伪彩（DEC-003）；等价于 convertDepth16ToRgba8(Jet)。
bool convertDepth16ToRgba8Jet(const std::uint16_t* src,
                              std::uint32_t width,
                              std::uint32_t height,
                              std::uint32_t srcStrideUnits,
                              float depthScaleMeters,
                              float nearMeters,
                              float farMeters,
                              std::vector<std::uint8_t>& dst);

/// jet 映射纯函数：t 归一化到 [0,1]，输出 RGBA（A=255）。
void jetColor(float t, std::uint8_t out[4]) noexcept;

/// 灰度映射纯函数（DEC-007）：t=0（近）白、t=1（远）黑，R=G=B，A=255。
void grayscaleColor(float t, std::uint8_t out[4]) noexcept;

}  // namespace rin
