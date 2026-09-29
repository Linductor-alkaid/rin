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
/// AdaptiveGrayscale 不使用 depthScaleMeters/near/far（比例对尺度不变），改为按
/// 帧内有效深度的 99 分位归一化（频闪防护：孤立远近噪声无法移动基准），超过
/// 分位的像素截断为纯白；参数校验与其它配色保持一致。
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

/// Z16 -> Gray8（M6-02，DEC-017 固定 rendition）：DEC-007 Grayscale ramp 的
/// 单通道对应层——[nearMeters, farMeters] 线性归一后 level = round((1−t)·255)
/// （近白远黑），区间外截断；0（无效深度）输出 0（黑；Gray8 无 alpha 层）。
/// dst 行距为 width（紧凑）；dst 由函数调整大小；入参与 convertDepth16ToRgba8
/// 同规则校验。
bool convertDepth16ToGray8(const std::uint16_t* src,
                           std::uint32_t width,
                           std::uint32_t height,
                           std::uint32_t srcStrideUnits,
                           float depthScaleMeters,
                           float nearMeters,
                           float farMeters,
                           std::vector<std::uint8_t>& dst);

/// Z16 -> Gray8 自适应灰度（M6-02，DEC-017 固定 rendition）：DEC-007
/// AdaptiveGrayscale ramp 的单通道对应层——帧内有效深度 P99 归一化
/// （频闪防护语义与 RGBA8 路径逐字一致），level = round(t·255)（近黑远白），
/// 超分位截断为 255（白）；0（无效深度）输出 0（黑）。不消费 depthScale/
/// near/far（比例对尺度不变）；空指针/零尺寸/stride 不足校验与 RGBA8 路径
/// 一致。
bool convertDepth16ToGray8Adaptive(const std::uint16_t* src,
                                   std::uint32_t width,
                                   std::uint32_t height,
                                   std::uint32_t srcStrideUnits,
                                   std::vector<std::uint8_t>& dst);

/// Z16 -> 深度灰度 + 自适应灰度双 rendition 单趟输出（M6-06 采集侧优化）：
/// 逐字节等价于对同一输入分别调用 convertDepth16ToGray8 与
/// convertDepth16ToGray8Adaptive（同一公式层，共享直方图/分位与单趟写出，
/// 采集 worker 每帧少两趟全幅扫描）。gray/adaptive 由函数调整大小；校验同
/// convertDepth16ToGray8（scale/near/far 必须合法——灰度 rendition 消费）。
bool convertDepth16ToGray8Pair(const std::uint16_t* src,
                               std::uint32_t width,
                               std::uint32_t height,
                               std::uint32_t srcStrideUnits,
                               float depthScaleMeters,
                               float nearMeters,
                               float farMeters,
                               std::vector<std::uint8_t>& gray,
                               std::vector<std::uint8_t>& adaptive);

/// jet 映射纯函数：t 归一化到 [0,1]，输出 RGBA（A=255）。
void jetColor(float t, std::uint8_t out[4]) noexcept;

/// 灰度映射纯函数（DEC-007）：t=0（近）白、t=1（远）黑，R=G=B，A=255。
void grayscaleColor(float t, std::uint8_t out[4]) noexcept;

/// 自适应灰度映射纯函数（DEC-007 扩展）：t=raw/frameQuantile。t=0（近）黑、
/// t=1（分位深度）白，超出截断为白，R=G=B，A=255。
void adaptiveGrayscaleColor(float t, std::uint8_t out[4]) noexcept;

}  // namespace rin
