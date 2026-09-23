// 像素转换单元测试（验证代理）：
// 1) convertRgb8ToRgba8：普通行通道直拷 + A=255；行距 padding 不泄漏；
//    stride 不足 / 零宽高 / nullptr 返回 false 且 dst 不被写。
// 2) convertDepth16ToRgba8Jet：raw=0 黑色；near/far 端点与中点颜色（jetColor 对照 +
//    精确值）；depthScale 换算；区间外截断；strideUnits padding；非法参数。
// 3) jetColor：单调区间、t 超界 clamp、alpha 恒 255。
// 4) grayscaleColor（DEC-007）：端点精确值、中点、t 超界 clamp、R==G==B 恒成立。
// 5) convertDepth16ToRgba8 统一入口：Grayscale 路径参数校验、raw=0 黑色、端点/中点
//    精确值（近白远黑）、区间外截断、随距离亮度单调不增、行 padding 等价、
//    Jet 包装与统一入口逐字节一致（回归锁定）、Grayscale 与 Jet 输出存在差异。
#include "test_util.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "pixel_format.hpp"

namespace {

using Rgba = std::array<std::uint8_t, 4>;

bool sameColor(const std::uint8_t* pixel, std::uint8_t r, std::uint8_t g, std::uint8_t b,
               std::uint8_t a) {
    return pixel[0] == r && pixel[1] == g && pixel[2] == b && pixel[3] == a;
}

Rgba jetAt(float t) {
    Rgba c{};
    rin::jetColor(t, c.data());
    return c;
}

Rgba grayAt(float t) {
    Rgba c{};
    rin::grayscaleColor(t, c.data());
    return c;
}

}  // namespace

int main() {
    // ================= convertRgb8ToRgba8 =================

    // 1) 普通 3 像素行：R/G/B 逐字节直拷（无通道交换），A=255。
    {
        const std::uint8_t rgb[9] = {0x10, 0x20, 0x30, 0xAA, 0xBB, 0xCC, 0x00, 0x7F, 0xFF};
        std::vector<std::uint8_t> rgba;
        RIN_CHECK(rin::convertRgb8ToRgba8(rgb, 3, 1, 9, rgba));
        RIN_CHECK_EQ(rgba.size(), std::size_t{12});
        for (int p = 0; p < 3; ++p) {
            RIN_CHECK_EQ(rgba[static_cast<std::size_t>(p) * 4 + 0], rgb[p * 3 + 0]);
            RIN_CHECK_EQ(rgba[static_cast<std::size_t>(p) * 4 + 1], rgb[p * 3 + 1]);
            RIN_CHECK_EQ(rgba[static_cast<std::size_t>(p) * 4 + 2], rgb[p * 3 + 2]);
            RIN_CHECK_EQ(rgba[static_cast<std::size_t>(p) * 4 + 3], std::uint8_t{255});
        }
    }

    // 2) 行距 padding：srcStride=10（6 数据 + 4 哨兵 padding），2x2；
    //    padding 字节不进入结果，行间不错位。
    {
        constexpr std::uint32_t kWidth = 2;
        constexpr std::uint32_t kHeight = 2;
        constexpr std::uint32_t kStride = 10;
        std::vector<std::uint8_t> src(kStride * kHeight, 0);
        for (std::uint32_t row = 0; row < kHeight; ++row) {
            for (std::uint32_t col = 0; col < kWidth; ++col) {
                std::uint8_t* px = src.data() + row * kStride + col * 3;
                px[0] = static_cast<std::uint8_t>(row * 16 + col * 3 + 1);
                px[1] = static_cast<std::uint8_t>(row * 16 + col * 3 + 2);
                px[2] = static_cast<std::uint8_t>(row * 16 + col * 3 + 3);
            }
            for (std::uint32_t i = kWidth * 3; i < kStride; ++i) {
                src[row * kStride + i] = 0xEE;  // padding 哨兵
            }
        }
        std::vector<std::uint8_t> rgba;
        RIN_CHECK(rin::convertRgb8ToRgba8(src.data(), kWidth, kHeight, kStride, rgba));
        RIN_CHECK_EQ(rgba.size(), std::size_t{kWidth * kHeight * 4});
        for (std::uint32_t row = 0; row < kHeight; ++row) {
            for (std::uint32_t col = 0; col < kWidth; ++col) {
                const std::uint8_t* s = src.data() + row * kStride + col * 3;
                const std::uint8_t* d =
                    rgba.data() + (row * kWidth + col) * 4;
                RIN_CHECK_EQ(d[0], s[0]);
                RIN_CHECK_EQ(d[1], s[1]);
                RIN_CHECK_EQ(d[2], s[2]);
                RIN_CHECK_EQ(d[3], std::uint8_t{255});
            }
        }
        // 结果不含任何 padding 哨兵字节（合法输出只有数据值 1..22 与 alpha=255）。
        for (const std::uint8_t byte : rgba) {
            RIN_CHECK(byte != 0xEE);
        }
    }

    // 3) 非法参数：返回 false 且 dst 保持原样。
    {
        const std::uint8_t rgb[9] = {};
        std::vector<std::uint8_t> dst{0x5A, 0x5A, 0x5A};
        const std::vector<std::uint8_t> sentinel = dst;
        RIN_CHECK(!rin::convertRgb8ToRgba8(nullptr, 3, 1, 3, dst));   // nullptr
        RIN_CHECK(!rin::convertRgb8ToRgba8(rgb, 0, 1, 3, dst));       // width=0
        RIN_CHECK(!rin::convertRgb8ToRgba8(rgb, 3, 0, 9, dst));       // height=0
        RIN_CHECK(!rin::convertRgb8ToRgba8(rgb, 3, 1, 8, dst));       // stride < width*3
        RIN_CHECK(!rin::convertRgb8ToRgba8(rgb, 3, 1, 0, dst));       // stride=0
        RIN_CHECK(dst == sentinel);
    }

    // ================= convertDepth16ToRgba8Jet =================

    // 4) raw=0 -> 不透明黑；near/far 端点与中点 -> jet 颜色（jetColor 对照 + 精确值）。
    //    scale=0.001：raw=1000/2000/3000 -> 1.0/2.0/3.0 m，near=1，far=3。
    {
        constexpr float kScale = 0.001f;
        constexpr float kNear = 1.0f;
        constexpr float kFar = 3.0f;
        const std::uint16_t depth[4] = {0, 1000, 2000, 3000};
        std::vector<std::uint8_t> rgba;
        RIN_CHECK(rin::convertDepth16ToRgba8Jet(depth, 4, 1, 4, kScale, kNear, kFar, rgba));
        RIN_CHECK_EQ(rgba.size(), std::size_t{16});

        RIN_CHECK(sameColor(rgba.data(), 0, 0, 0, 255));  // raw=0 无效深度
        const float expectedT[4] = {0.0f, 0.0f, 0.5f, 1.0f};
        for (int p = 1; p < 4; ++p) {
            const Rgba ref = jetAt(expectedT[p]);
            RIN_CHECK(sameColor(rgba.data() + p * 4, ref[0], ref[1], ref[2], ref[3]));
        }
        // 精确语义色：t=0 暗蓝、t=0.5 绿峰（jet 绿峰带 128 红/蓝）、t=1 暗红。
        RIN_CHECK(sameColor(rgba.data() + 4, 0, 0, 128, 255));
        RIN_CHECK(sameColor(rgba.data() + 8, 128, 255, 128, 255));
        RIN_CHECK(sameColor(rgba.data() + 12, 128, 0, 0, 255));
    }

    // 5) depthScale 换算：scale=0.001、raw=2000 -> 2.0m；near=0/far=4 -> t=0.5。
    //    若实现漏乘 scale，raw=2000 会被当作远超 far，t 截断为 1（暗红），可区分。
    {
        const std::uint16_t depth[2] = {2000, 1000};
        std::vector<std::uint8_t> rgba;
        RIN_CHECK(rin::convertDepth16ToRgba8Jet(depth, 2, 1, 2, 0.001f, 0.0f, 4.0f, rgba));
        const Rgba mid = jetAt(0.5f);
        const Rgba quarter = jetAt(0.25f);
        RIN_CHECK(sameColor(rgba.data(), mid[0], mid[1], mid[2], mid[3]));
        RIN_CHECK(sameColor(rgba.data() + 4, quarter[0], quarter[1], quarter[2], quarter[3]));
    }

    // 6) 区间外截断：raw 低于 near / 高于 far 分别等价 t=0 / t=1。
    {
        const std::uint16_t depth[2] = {1, 65000};  // 0.001 m 与 65 m
        std::vector<std::uint8_t> rgba;
        RIN_CHECK(rin::convertDepth16ToRgba8Jet(depth, 2, 1, 2, 0.001f, 1.0f, 3.0f, rgba));
        const Rgba lo = jetAt(0.0f);
        const Rgba hi = jetAt(1.0f);
        RIN_CHECK(sameColor(rgba.data(), lo[0], lo[1], lo[2], lo[3]));
        RIN_CHECK(sameColor(rgba.data() + 4, hi[0], hi[1], hi[2], hi[3]));
    }

    // 7) srcStrideUnits padding：2x3、strideUnits=5（每行 2 个 padding 单元），
    //    行 1 数据必须来自行 1 而非行 0 尾部。
    {
        constexpr std::uint32_t kWidth = 3;
        constexpr std::uint32_t kHeight = 2;
        constexpr std::uint32_t kStrideUnits = 5;
        std::vector<std::uint16_t> depth(kStrideUnits * kHeight, 0xFFFF);
        const std::uint16_t row0[kWidth] = {1000, 2000, 3000};
        const std::uint16_t row1[kWidth] = {3000, 2000, 1000};
        for (std::uint32_t c = 0; c < kWidth; ++c) {
            depth[c] = row0[c];
            depth[kStrideUnits + c] = row1[c];
        }
        std::vector<std::uint8_t> rgba;
        RIN_CHECK(rin::convertDepth16ToRgba8Jet(depth.data(), kWidth, kHeight, kStrideUnits,
                                                0.001f, 1.0f, 3.0f, rgba));
        RIN_CHECK_EQ(rgba.size(), std::size_t{kWidth * kHeight * 4});
        const Rgba t0 = jetAt(0.0f);
        const Rgba t05 = jetAt(0.5f);
        const Rgba t1 = jetAt(1.0f);
        // 行 0：1000/2000/3000 -> t=0/0.5/1
        RIN_CHECK(sameColor(rgba.data() + 0, t0[0], t0[1], t0[2], t0[3]));
        RIN_CHECK(sameColor(rgba.data() + 4, t05[0], t05[1], t05[2], t05[3]));
        RIN_CHECK(sameColor(rgba.data() + 8, t1[0], t1[1], t1[2], t1[3]));
        // 行 1：3000/2000/1000 -> t=1/0.5/0（错位到行 0 padding 0xFFFF 会得 t=1）
        RIN_CHECK(sameColor(rgba.data() + 12, t1[0], t1[1], t1[2], t1[3]));
        RIN_CHECK(sameColor(rgba.data() + 16, t05[0], t05[1], t05[2], t05[3]));
        RIN_CHECK(sameColor(rgba.data() + 20, t0[0], t0[1], t0[2], t0[3]));
    }

    // 8) 非法参数：返回 false 且 dst 不被写。
    {
        const std::uint16_t depth[4] = {100, 200, 300, 400};
        std::vector<std::uint8_t> dst{0x5A, 0x5A};
        const std::vector<std::uint8_t> sentinel = dst;
        RIN_CHECK(!rin::convertDepth16ToRgba8Jet(nullptr, 2, 1, 2, 0.001f, 1.0f, 3.0f, dst));
        RIN_CHECK(!rin::convertDepth16ToRgba8Jet(depth, 0, 1, 0, 0.001f, 1.0f, 3.0f, dst));
        RIN_CHECK(!rin::convertDepth16ToRgba8Jet(depth, 2, 0, 2, 0.001f, 1.0f, 3.0f, dst));
        RIN_CHECK(!rin::convertDepth16ToRgba8Jet(depth, 3, 1, 2, 0.001f, 1.0f, 3.0f, dst));
        RIN_CHECK(!rin::convertDepth16ToRgba8Jet(depth, 2, 1, 2, 0.0f, 1.0f, 3.0f, dst));
        RIN_CHECK(!rin::convertDepth16ToRgba8Jet(depth, 2, 1, 2, -1.0f, 1.0f, 3.0f, dst));
        RIN_CHECK(!rin::convertDepth16ToRgba8Jet(depth, 2, 1, 2, 0.001f, 3.0f, 3.0f, dst));
        RIN_CHECK(!rin::convertDepth16ToRgba8Jet(depth, 2, 1, 2, 0.001f, 3.0f, 1.0f, dst));
        RIN_CHECK(dst == sentinel);
    }

    // ================= jetColor =================

    // 9) 端点精确值（实现为经典 jet 三角波近似）。
    {
        RIN_CHECK(jetAt(0.0f) == (Rgba{0, 0, 128, 255}));
        RIN_CHECK(jetAt(0.25f) == (Rgba{0, 128, 255, 255}));
        RIN_CHECK(jetAt(0.5f) == (Rgba{128, 255, 128, 255}));
        RIN_CHECK(jetAt(0.75f) == (Rgba{255, 128, 0, 255}));
        RIN_CHECK(jetAt(1.0f) == (Rgba{128, 0, 0, 255}));
    }

    // 10) t 超界 clamp：下/上界外与边界一致。
    {
        const Rgba j0 = jetAt(0.0f);
        const Rgba j1 = jetAt(1.0f);
        RIN_CHECK(jetAt(-0.5f) == j0);
        RIN_CHECK(jetAt(-0.0001f) == j0);
        RIN_CHECK(jetAt(1.0001f) == j1);
        RIN_CHECK(jetAt(2.0f) == j1);
    }

    // 11) 单调区间：b 升[0,0.25]降[0.25,0.5]；g 升[0.25,0.5]降[0.5,0.75]；
    //     r 升[0.5,0.75]降[0.75,1]；alpha 恒 255。
    {
        auto mono = [](float tA, float tB, int steps, int channel, bool increasing) {
            for (int s = 0; s < steps; ++s) {
                const float t0 = tA + (tB - tA) * static_cast<float>(s) / steps;
                const float t1 = tA + (tB - tA) * static_cast<float>(s + 1) / steps;
                const Rgba c0 = jetAt(t0);
                const Rgba c1 = jetAt(t1);
                if (increasing) {
                    RIN_CHECK(c0[static_cast<std::size_t>(channel)] <=
                              c1[static_cast<std::size_t>(channel)]);
                } else {
                    RIN_CHECK(c0[static_cast<std::size_t>(channel)] >=
                              c1[static_cast<std::size_t>(channel)]);
                }
            }
        };
        mono(0.00f, 0.25f, 25, 2, true);
        mono(0.25f, 0.50f, 25, 2, false);
        mono(0.25f, 0.50f, 25, 1, true);
        mono(0.50f, 0.75f, 25, 1, false);
        mono(0.50f, 0.75f, 25, 0, true);
        mono(0.75f, 1.00f, 25, 0, false);

        for (int i = -20; i <= 120; i += 5) {  // 含超界样本
            const float t = static_cast<float>(i) / 100.0f;
            RIN_CHECK_EQ(jetAt(t)[3], std::uint8_t{255});
        }
    }

    // ================= grayscaleColor（DEC-007）=================

    // 12) 端点精确值：t=0（近）纯白、t=1（远）纯黑；alpha 恒 255。
    {
        RIN_CHECK(grayAt(0.0f) == (Rgba{255, 255, 255, 255}));
        RIN_CHECK(grayAt(1.0f) == (Rgba{0, 0, 0, 255}));
    }

    // 13) 中点精确值：t=0.5 -> level=(1-0.5)*255+0.5=128；R==G==B 且 A=255。
    {
        RIN_CHECK(grayAt(0.5f) == (Rgba{128, 128, 128, 255}));
        for (int i = 0; i <= 20; ++i) {  // 任意 t：R==G==B 且 alpha 恒 255（含超界样本）
            const float t = static_cast<float>(i) / 20.0f - 0.25f;  // [-0.25, 0.75]
            const Rgba c = grayAt(t);
            RIN_CHECK_EQ(c[0], c[1]);
            RIN_CHECK_EQ(c[1], c[2]);
            RIN_CHECK_EQ(c[3], std::uint8_t{255});
        }
    }

    // 14) t 超界 clamp：下/上界外与端点一致；线性语义抽检（t=0.25 -> 191）。
    {
        const Rgba g0 = grayAt(0.0f);
        const Rgba g1 = grayAt(1.0f);
        RIN_CHECK(grayAt(-0.5f) == g0);
        RIN_CHECK(grayAt(-0.0001f) == g0);
        RIN_CHECK(grayAt(1.0001f) == g1);
        RIN_CHECK(grayAt(2.0f) == g1);
        // level(0.25) = 0.75*255+0.5 = 191.75 -> 截断 191。
        RIN_CHECK(grayAt(0.25f) == (Rgba{191, 191, 191, 255}));
        // level(0.75) = 0.25*255+0.5 = 64.25 -> 截断 64。
        RIN_CHECK(grayAt(0.75f) == (Rgba{64, 64, 64, 255}));
    }

    // ============ convertDepth16ToRgba8 统一入口（DEC-007）============

    // 15) 参数校验在 Grayscale 下同样生效：返回 false 且 dst 不被写。
    {
        const std::uint16_t depth[4] = {100, 200, 300, 400};
        std::vector<std::uint8_t> dst{0x5A, 0x5A};
        const std::vector<std::uint8_t> sentinel = dst;
        const auto scheme = rin::DepthColorScheme::Grayscale;
        RIN_CHECK(!rin::convertDepth16ToRgba8(nullptr, 2, 1, 2, 0.001f, 0.2f, 6.5f, scheme, dst));
        RIN_CHECK(!rin::convertDepth16ToRgba8(depth, 0, 1, 0, 0.001f, 0.2f, 6.5f, scheme, dst));
        RIN_CHECK(!rin::convertDepth16ToRgba8(depth, 2, 0, 2, 0.001f, 0.2f, 6.5f, scheme, dst));
        RIN_CHECK(!rin::convertDepth16ToRgba8(depth, 3, 1, 2, 0.001f, 0.2f, 6.5f, scheme, dst));
        RIN_CHECK(!rin::convertDepth16ToRgba8(depth, 2, 1, 1, 0.001f, 0.2f, 6.5f, scheme, dst));
        RIN_CHECK(!rin::convertDepth16ToRgba8(depth, 2, 1, 2, 0.0f, 0.2f, 6.5f, scheme, dst));
        RIN_CHECK(!rin::convertDepth16ToRgba8(depth, 2, 1, 2, -1.0f, 0.2f, 6.5f, scheme, dst));
        RIN_CHECK(!rin::convertDepth16ToRgba8(depth, 2, 1, 2, 0.001f, 6.5f, 6.5f, scheme, dst));
        RIN_CHECK(!rin::convertDepth16ToRgba8(depth, 2, 1, 2, 0.001f, 6.5f, 0.2f, scheme, dst));
        RIN_CHECK(dst == sentinel);
    }

    // 16) Grayscale 转换语义（scale=0.001、near=0.2、far=6.5，与适配器常量一致）：
    //     raw=0 不透明黑；raw=200（0.2m, t=0）白；raw=6500（6.5m, t=1）黑；
    //     raw=3350（3.35m, t=0.5）128。
    {
        constexpr float kScale = 0.001f;
        constexpr float kNear = 0.2f;
        constexpr float kFar = 6.5f;
        const auto scheme = rin::DepthColorScheme::Grayscale;
        const std::uint16_t depth[4] = {0, 200, 3350, 6500};
        std::vector<std::uint8_t> rgba;
        RIN_CHECK(rin::convertDepth16ToRgba8(depth, 4, 1, 4, kScale, kNear, kFar, scheme, rgba));
        RIN_CHECK_EQ(rgba.size(), std::size_t{16});
        RIN_CHECK(sameColor(rgba.data(), 0, 0, 0, 255));  // raw=0 无效深度
        RIN_CHECK(sameColor(rgba.data() + 4, 255, 255, 255, 255));  // t=0 近白
        RIN_CHECK(sameColor(rgba.data() + 8, 128, 128, 128, 255));  // t=0.5 中灰
        RIN_CHECK(sameColor(rgba.data() + 12, 0, 0, 0, 255));       // t=1 远黑
    }

    // 17) 区间外截断：raw=100（<near）与 raw=200 同值；raw=65535（>far）与 raw=6500 同值。
    {
        const auto scheme = rin::DepthColorScheme::Grayscale;
        const std::uint16_t depth[4] = {100, 200, 6500, 65535};
        std::vector<std::uint8_t> rgba;
        RIN_CHECK(rin::convertDepth16ToRgba8(depth, 4, 1, 4, 0.001f, 0.2f, 6.5f, scheme, rgba));
        for (int p = 0; p < 4; ++p) {
            const Rgba ref = grayAt(p < 2 ? 0.0f : 1.0f);
            RIN_CHECK(sameColor(rgba.data() + static_cast<std::size_t>(p) * 4, ref[0], ref[1],
                                ref[2], ref[3]));
        }
        RIN_CHECK(sameColor(rgba.data(), 255, 255, 255, 255));  // 下方截断 == t=0
        RIN_CHECK(sameColor(rgba.data() + 8, 0, 0, 0, 255));    // 上方截断 == t=1
    }

    // 18) 随距离单调不增：区间内递增 raw 序列，灰度亮度序列非增；
    //     且任意样本 R==G==B（逐像素经 grayscaleColor 参照核对）。
    {
        const auto scheme = rin::DepthColorScheme::Grayscale;
        constexpr float kScale = 0.001f;
        constexpr float kNear = 0.2f;
        constexpr float kFar = 6.5f;
        const float kRange = kFar - kNear;  // 与实现同一 float 表达式，参照重算逐位一致
        constexpr int kSamples = 64;
        std::vector<std::uint16_t> depth;
        depth.reserve(kSamples);
        for (int s = 0; s < kSamples; ++s) {
            // 200..6500 线性铺满 [near, far]（米制 0.2..6.5）。
            depth.push_back(static_cast<std::uint16_t>(
                200 + (6500 - 200) * s / (kSamples - 1)));
        }
        std::vector<std::uint8_t> rgba;
        RIN_CHECK(rin::convertDepth16ToRgba8(depth.data(), static_cast<std::uint32_t>(
                                                       depth.size()),
                                             1, static_cast<std::uint32_t>(depth.size()),
                                             kScale, kNear, kFar, scheme, rgba));
        for (int s = 0; s < kSamples; ++s) {
            const std::size_t offset = static_cast<std::size_t>(s) * 4;
            RIN_CHECK_EQ(rgba[offset + 0], rgba[offset + 1]);
            RIN_CHECK_EQ(rgba[offset + 1], rgba[offset + 2]);
            RIN_CHECK_EQ(rgba[offset + 3], std::uint8_t{255});
            // 参照核对：经 grayscaleColor 纯函数重算同一 t。
            const float t =
                (static_cast<float>(depth[static_cast<std::size_t>(s)]) * kScale - kNear) /
                kRange;
            const Rgba ref = grayAt(t);
            RIN_CHECK(sameColor(rgba.data() + offset, ref[0], ref[1], ref[2], ref[3]));
            if (s > 0) {
                RIN_CHECK(rgba[offset] <= rgba[offset - 4]);  // 亮度非增
            }
        }
    }

    // 19) srcStrideUnits > width（行填充）：输出与紧凑输入逐字节一致。
    {
        constexpr std::uint32_t kWidth = 3;
        constexpr std::uint32_t kHeight = 2;
        constexpr std::uint32_t kStrideUnits = 5;
        const auto scheme = rin::DepthColorScheme::Grayscale;
        const std::uint16_t compact[6] = {0, 1000, 3350, 6500, 200, 40000};
        std::vector<std::uint16_t> padded(kStrideUnits * kHeight, 0xFFFF);  // 哨兵 padding
        for (std::uint32_t row = 0; row < kHeight; ++row) {
            for (std::uint32_t col = 0; col < kWidth; ++col) {
                padded[row * kStrideUnits + col] = compact[row * kWidth + col];
            }
        }
        std::vector<std::uint8_t> fromCompact;
        std::vector<std::uint8_t> fromPadded;
        RIN_CHECK(rin::convertDepth16ToRgba8(compact, kWidth, kHeight, kWidth, 0.001f, 0.2f,
                                             6.5f, scheme, fromCompact));
        RIN_CHECK(rin::convertDepth16ToRgba8(padded.data(), kWidth, kHeight, kStrideUnits,
                                             0.001f, 0.2f, 6.5f, scheme, fromPadded));
        RIN_CHECK(fromCompact == fromPadded);  // padding 单元不泄漏、行间不错位
    }

    // 20) 回归锁定：Jet 薄包装与统一入口 Jet 输出逐字节一致（含 raw=0 与区间外样本）。
    {
        const std::uint16_t depth[7] = {0, 100, 200, 1500, 3350, 6500, 65535};
        std::vector<std::uint8_t> viaWrapper;
        std::vector<std::uint8_t> viaUnified;
        RIN_CHECK(rin::convertDepth16ToRgba8Jet(depth, 7, 1, 7, 0.001f, 0.2f, 6.5f, viaWrapper));
        RIN_CHECK(rin::convertDepth16ToRgba8(depth, 7, 1, 7, 0.001f, 0.2f, 6.5f,
                                             rin::DepthColorScheme::Jet, viaUnified));
        RIN_CHECK(viaWrapper == viaUnified);
    }

    // 21) 同一输入下 Grayscale 与 Jet 存在差异（非端点 raw：3350 -> t=0.5）。
    {
        const std::uint16_t depth[3] = {200, 3350, 6500};
        std::vector<std::uint8_t> gray;
        std::vector<std::uint8_t> jet;
        RIN_CHECK(rin::convertDepth16ToRgba8(depth, 3, 1, 3, 0.001f, 0.2f, 6.5f,
                                             rin::DepthColorScheme::Grayscale, gray));
        RIN_CHECK(rin::convertDepth16ToRgba8(depth, 3, 1, 3, 0.001f, 0.2f, 6.5f,
                                             rin::DepthColorScheme::Jet, jet));
        bool differs = false;
        for (std::size_t i = 0; i < gray.size(); ++i) {
            if (gray[i] != jet[i]) {
                differs = true;
            }
        }
        RIN_CHECK(differs);
        // 差异应出现在非端点像素（raw=3350）：灰度 128 vs jet 绿峰 {128,255,128}。
        RIN_CHECK(sameColor(jet.data() + 4, 128, 255, 128, 255));
        RIN_CHECK(sameColor(gray.data() + 4, 128, 128, 128, 255));
    }

    return rin_test::exitStatus();
}
