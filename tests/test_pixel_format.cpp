// 像素转换单元测试（验证代理）：
// 1) convertRgb8ToRgba8：普通行通道直拷 + A=255；行距 padding 不泄漏；
//    stride 不足 / 零宽高 / nullptr 返回 false 且 dst 不被写。
// 2) convertDepth16ToRgba8Jet：raw=0 黑色；near/far 端点与中点颜色（jetColor 对照 +
//    精确值）；depthScale 换算；区间外截断；strideUnits padding；非法参数。
// 3) jetColor：单调区间、t 超界 clamp、alpha 恒 255。
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
    rsv::jetColor(t, c.data());
    return c;
}

}  // namespace

int main() {
    // ================= convertRgb8ToRgba8 =================

    // 1) 普通 3 像素行：R/G/B 逐字节直拷（无通道交换），A=255。
    {
        const std::uint8_t rgb[9] = {0x10, 0x20, 0x30, 0xAA, 0xBB, 0xCC, 0x00, 0x7F, 0xFF};
        std::vector<std::uint8_t> rgba;
        RSV_CHECK(rsv::convertRgb8ToRgba8(rgb, 3, 1, 9, rgba));
        RSV_CHECK_EQ(rgba.size(), std::size_t{12});
        for (int p = 0; p < 3; ++p) {
            RSV_CHECK_EQ(rgba[static_cast<std::size_t>(p) * 4 + 0], rgb[p * 3 + 0]);
            RSV_CHECK_EQ(rgba[static_cast<std::size_t>(p) * 4 + 1], rgb[p * 3 + 1]);
            RSV_CHECK_EQ(rgba[static_cast<std::size_t>(p) * 4 + 2], rgb[p * 3 + 2]);
            RSV_CHECK_EQ(rgba[static_cast<std::size_t>(p) * 4 + 3], std::uint8_t{255});
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
        RSV_CHECK(rsv::convertRgb8ToRgba8(src.data(), kWidth, kHeight, kStride, rgba));
        RSV_CHECK_EQ(rgba.size(), std::size_t{kWidth * kHeight * 4});
        for (std::uint32_t row = 0; row < kHeight; ++row) {
            for (std::uint32_t col = 0; col < kWidth; ++col) {
                const std::uint8_t* s = src.data() + row * kStride + col * 3;
                const std::uint8_t* d =
                    rgba.data() + (row * kWidth + col) * 4;
                RSV_CHECK_EQ(d[0], s[0]);
                RSV_CHECK_EQ(d[1], s[1]);
                RSV_CHECK_EQ(d[2], s[2]);
                RSV_CHECK_EQ(d[3], std::uint8_t{255});
            }
        }
        // 结果不含任何 padding 哨兵字节（合法输出只有数据值 1..22 与 alpha=255）。
        for (const std::uint8_t byte : rgba) {
            RSV_CHECK(byte != 0xEE);
        }
    }

    // 3) 非法参数：返回 false 且 dst 保持原样。
    {
        const std::uint8_t rgb[9] = {};
        std::vector<std::uint8_t> dst{0x5A, 0x5A, 0x5A};
        const std::vector<std::uint8_t> sentinel = dst;
        RSV_CHECK(!rsv::convertRgb8ToRgba8(nullptr, 3, 1, 3, dst));   // nullptr
        RSV_CHECK(!rsv::convertRgb8ToRgba8(rgb, 0, 1, 3, dst));       // width=0
        RSV_CHECK(!rsv::convertRgb8ToRgba8(rgb, 3, 0, 9, dst));       // height=0
        RSV_CHECK(!rsv::convertRgb8ToRgba8(rgb, 3, 1, 8, dst));       // stride < width*3
        RSV_CHECK(!rsv::convertRgb8ToRgba8(rgb, 3, 1, 0, dst));       // stride=0
        RSV_CHECK(dst == sentinel);
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
        RSV_CHECK(rsv::convertDepth16ToRgba8Jet(depth, 4, 1, 4, kScale, kNear, kFar, rgba));
        RSV_CHECK_EQ(rgba.size(), std::size_t{16});

        RSV_CHECK(sameColor(rgba.data(), 0, 0, 0, 255));  // raw=0 无效深度
        const float expectedT[4] = {0.0f, 0.0f, 0.5f, 1.0f};
        for (int p = 1; p < 4; ++p) {
            const Rgba ref = jetAt(expectedT[p]);
            RSV_CHECK(sameColor(rgba.data() + p * 4, ref[0], ref[1], ref[2], ref[3]));
        }
        // 精确语义色：t=0 暗蓝、t=0.5 绿峰（jet 绿峰带 128 红/蓝）、t=1 暗红。
        RSV_CHECK(sameColor(rgba.data() + 4, 0, 0, 128, 255));
        RSV_CHECK(sameColor(rgba.data() + 8, 128, 255, 128, 255));
        RSV_CHECK(sameColor(rgba.data() + 12, 128, 0, 0, 255));
    }

    // 5) depthScale 换算：scale=0.001、raw=2000 -> 2.0m；near=0/far=4 -> t=0.5。
    //    若实现漏乘 scale，raw=2000 会被当作远超 far，t 截断为 1（暗红），可区分。
    {
        const std::uint16_t depth[2] = {2000, 1000};
        std::vector<std::uint8_t> rgba;
        RSV_CHECK(rsv::convertDepth16ToRgba8Jet(depth, 2, 1, 2, 0.001f, 0.0f, 4.0f, rgba));
        const Rgba mid = jetAt(0.5f);
        const Rgba quarter = jetAt(0.25f);
        RSV_CHECK(sameColor(rgba.data(), mid[0], mid[1], mid[2], mid[3]));
        RSV_CHECK(sameColor(rgba.data() + 4, quarter[0], quarter[1], quarter[2], quarter[3]));
    }

    // 6) 区间外截断：raw 低于 near / 高于 far 分别等价 t=0 / t=1。
    {
        const std::uint16_t depth[2] = {1, 65000};  // 0.001 m 与 65 m
        std::vector<std::uint8_t> rgba;
        RSV_CHECK(rsv::convertDepth16ToRgba8Jet(depth, 2, 1, 2, 0.001f, 1.0f, 3.0f, rgba));
        const Rgba lo = jetAt(0.0f);
        const Rgba hi = jetAt(1.0f);
        RSV_CHECK(sameColor(rgba.data(), lo[0], lo[1], lo[2], lo[3]));
        RSV_CHECK(sameColor(rgba.data() + 4, hi[0], hi[1], hi[2], hi[3]));
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
        RSV_CHECK(rsv::convertDepth16ToRgba8Jet(depth.data(), kWidth, kHeight, kStrideUnits,
                                                0.001f, 1.0f, 3.0f, rgba));
        RSV_CHECK_EQ(rgba.size(), std::size_t{kWidth * kHeight * 4});
        const Rgba t0 = jetAt(0.0f);
        const Rgba t05 = jetAt(0.5f);
        const Rgba t1 = jetAt(1.0f);
        // 行 0：1000/2000/3000 -> t=0/0.5/1
        RSV_CHECK(sameColor(rgba.data() + 0, t0[0], t0[1], t0[2], t0[3]));
        RSV_CHECK(sameColor(rgba.data() + 4, t05[0], t05[1], t05[2], t05[3]));
        RSV_CHECK(sameColor(rgba.data() + 8, t1[0], t1[1], t1[2], t1[3]));
        // 行 1：3000/2000/1000 -> t=1/0.5/0（错位到行 0 padding 0xFFFF 会得 t=1）
        RSV_CHECK(sameColor(rgba.data() + 12, t1[0], t1[1], t1[2], t1[3]));
        RSV_CHECK(sameColor(rgba.data() + 16, t05[0], t05[1], t05[2], t05[3]));
        RSV_CHECK(sameColor(rgba.data() + 20, t0[0], t0[1], t0[2], t0[3]));
    }

    // 8) 非法参数：返回 false 且 dst 不被写。
    {
        const std::uint16_t depth[4] = {100, 200, 300, 400};
        std::vector<std::uint8_t> dst{0x5A, 0x5A};
        const std::vector<std::uint8_t> sentinel = dst;
        RSV_CHECK(!rsv::convertDepth16ToRgba8Jet(nullptr, 2, 1, 2, 0.001f, 1.0f, 3.0f, dst));
        RSV_CHECK(!rsv::convertDepth16ToRgba8Jet(depth, 0, 1, 0, 0.001f, 1.0f, 3.0f, dst));
        RSV_CHECK(!rsv::convertDepth16ToRgba8Jet(depth, 2, 0, 2, 0.001f, 1.0f, 3.0f, dst));
        RSV_CHECK(!rsv::convertDepth16ToRgba8Jet(depth, 3, 1, 2, 0.001f, 1.0f, 3.0f, dst));
        RSV_CHECK(!rsv::convertDepth16ToRgba8Jet(depth, 2, 1, 2, 0.0f, 1.0f, 3.0f, dst));
        RSV_CHECK(!rsv::convertDepth16ToRgba8Jet(depth, 2, 1, 2, -1.0f, 1.0f, 3.0f, dst));
        RSV_CHECK(!rsv::convertDepth16ToRgba8Jet(depth, 2, 1, 2, 0.001f, 3.0f, 3.0f, dst));
        RSV_CHECK(!rsv::convertDepth16ToRgba8Jet(depth, 2, 1, 2, 0.001f, 3.0f, 1.0f, dst));
        RSV_CHECK(dst == sentinel);
    }

    // ================= jetColor =================

    // 9) 端点精确值（实现为经典 jet 三角波近似）。
    {
        RSV_CHECK(jetAt(0.0f) == (Rgba{0, 0, 128, 255}));
        RSV_CHECK(jetAt(0.25f) == (Rgba{0, 128, 255, 255}));
        RSV_CHECK(jetAt(0.5f) == (Rgba{128, 255, 128, 255}));
        RSV_CHECK(jetAt(0.75f) == (Rgba{255, 128, 0, 255}));
        RSV_CHECK(jetAt(1.0f) == (Rgba{128, 0, 0, 255}));
    }

    // 10) t 超界 clamp：下/上界外与边界一致。
    {
        const Rgba j0 = jetAt(0.0f);
        const Rgba j1 = jetAt(1.0f);
        RSV_CHECK(jetAt(-0.5f) == j0);
        RSV_CHECK(jetAt(-0.0001f) == j0);
        RSV_CHECK(jetAt(1.0001f) == j1);
        RSV_CHECK(jetAt(2.0f) == j1);
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
                    RSV_CHECK(c0[static_cast<std::size_t>(channel)] <=
                              c1[static_cast<std::size_t>(channel)]);
                } else {
                    RSV_CHECK(c0[static_cast<std::size_t>(channel)] >=
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
            RSV_CHECK_EQ(jetAt(t)[3], std::uint8_t{255});
        }
    }
    return rsv_test::exitStatus();
}
