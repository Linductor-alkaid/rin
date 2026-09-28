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
// 6) AdaptiveGrayscale（DEC-007 扩展）：adaptiveGrayscaleColor 端点/中点/超界
//    clamp/通道恒等；转换按帧内最大有效值归一化（参照 level=round(raw*255/frameMax)
//    逐像素核对）、raw=0 黑且不入 frameMax、全零帧、尺度与 near/far 不变性、
//    参数校验、stride padding 不泄漏、与 Grayscale 差异、灰度单调不减。
// 7) AdaptiveGrayscale P99 分位基准（频闪修复）：N=100 边界分位语义（第二大值成
//    基准、超分位截断纯白）、离群免疫（与去离群帧逐字节一致的频闪回归锁定）、
//    多档超分位值截断无差异、N<100 小帧退化为绝对最大值（不免疫离群的已知边界）、
//    固定种子 LCG 随机帧与排序参照逐字节核对、大帧（N>=100）既有不变量回归。
#include "test_util.hpp"

#include <algorithm>
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

Rgba adaptiveAt(float t) {
    Rgba c{};
    rin::adaptiveGrayscaleColor(t, c.data());
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

    // ============ AdaptiveGrayscale（DEC-007 扩展）============

    // 22) adaptiveGrayscaleColor 端点精确值：t=0 纯黑、t=1 纯白。
    {
        RIN_CHECK(adaptiveAt(0.0f) == (Rgba{0, 0, 0, 255}));
        RIN_CHECK(adaptiveAt(1.0f) == (Rgba{255, 255, 255, 255}));
    }

    // 23) 中点 t=0.5 -> 128（+0.5 取整）；超界 clamp；线性抽检；任意 t 下
    //     R==G==B 恒成立且 A=255。
    {
        RIN_CHECK(adaptiveAt(0.5f) == (Rgba{128, 128, 128, 255}));
        const Rgba a0 = adaptiveAt(0.0f);
        const Rgba a1 = adaptiveAt(1.0f);
        RIN_CHECK(adaptiveAt(-1.0f) == a0);
        RIN_CHECK(adaptiveAt(-0.0001f) == a0);
        RIN_CHECK(adaptiveAt(1.0001f) == a1);
        RIN_CHECK(adaptiveAt(2.0f) == a1);
        // level(0.25) = 0.25*255+0.5 = 64.25 -> 64。
        RIN_CHECK(adaptiveAt(0.25f) == (Rgba{64, 64, 64, 255}));
        // level(0.75) = 0.75*255+0.5 = 191.75 -> 191。
        RIN_CHECK(adaptiveAt(0.75f) == (Rgba{191, 191, 191, 255}));
        for (int i = -20; i <= 120; ++i) {  // 含超界样本 [-0.2, 1.2]
            const float t = static_cast<float>(i) / 100.0f;
            const Rgba c = adaptiveAt(t);
            RIN_CHECK_EQ(c[0], c[1]);
            RIN_CHECK_EQ(c[1], c[2]);
            RIN_CHECK_EQ(c[3], std::uint8_t{255});
        }
    }

    // 24) 转换语义（scale=0.001、near=0.2、far=6.5，与适配器常量一致）：帧内最大值
    //     像素纯白；其余非零像素按参照 level=round(raw*255/frameMax) 逐像素核对；
    //     raw=0 输出不透明黑且不参与 frameMax。
    {
        constexpr float kScale = 0.001f;
        constexpr float kNear = 0.2f;
        constexpr float kFar = 6.5f;
        const auto scheme = rin::DepthColorScheme::AdaptiveGrayscale;
        const std::uint16_t depth[6] = {0, 100, 500, 0, 250, 1000};  // frameMax=1000
        std::vector<std::uint8_t> rgba;
        RIN_CHECK(rin::convertDepth16ToRgba8(depth, 6, 1, 6, kScale, kNear, kFar, scheme, rgba));
        RIN_CHECK_EQ(rgba.size(), std::size_t{24});
        // 参照 level（正值下与实现 +0.5 截断一致）：-1 表示 raw==0 应为不透明黑。
        const int expectedLevel[6] = {-1, 26, 128, -1, 64, 255};
        for (int p = 0; p < 6; ++p) {
            const std::size_t offset = static_cast<std::size_t>(p) * 4;
            if (expectedLevel[p] < 0) {
                RIN_CHECK(sameColor(rgba.data() + offset, 0, 0, 0, 255));
                continue;
            }
            RIN_CHECK_EQ(rgba[offset + 0], static_cast<std::uint8_t>(expectedLevel[p]));
            RIN_CHECK_EQ(rgba[offset + 1], static_cast<std::uint8_t>(expectedLevel[p]));
            RIN_CHECK_EQ(rgba[offset + 2], static_cast<std::uint8_t>(expectedLevel[p]));
            RIN_CHECK_EQ(rgba[offset + 3], std::uint8_t{255});
            // 双重参照：经纯函数 adaptiveGrayscaleColor 以同一 t=raw/frameMax 核对。
            const Rgba ref =
                adaptiveAt(static_cast<float>(depth[static_cast<std::size_t>(p)]) / 1000.0f);
            RIN_CHECK(sameColor(rgba.data() + offset, ref[0], ref[1], ref[2], ref[3]));
        }
        RIN_CHECK(sameColor(rgba.data() + 20, 255, 255, 255, 255));  // 帧内最大 -> 纯白
    }

    // 25) 全帧 raw==0：整帧不透明黑（含 alpha==255），返回 true。
    {
        const auto scheme = rin::DepthColorScheme::AdaptiveGrayscale;
        std::vector<std::uint16_t> depth(6, 0);  // 3x2 全无效
        std::vector<std::uint8_t> rgba;
        RIN_CHECK(rin::convertDepth16ToRgba8(depth.data(), 3, 2, 3, 0.001f, 0.2f, 6.5f, scheme,
                                             rgba));
        RIN_CHECK_EQ(rgba.size(), std::size_t{24});
        for (std::size_t offset = 0; offset + 3 < rgba.size(); offset += 4) {
            RIN_CHECK(sameColor(rgba.data() + offset, 0, 0, 0, 255));
        }
    }

    // 26) 尺度不变性：同一 raw 图，depthScale=0.001 与 0.002 输出逐字节一致。
    {
        const auto scheme = rin::DepthColorScheme::AdaptiveGrayscale;
        const std::uint16_t depth[5] = {0, 300, 700, 1200, 2500};
        std::vector<std::uint8_t> outA;
        std::vector<std::uint8_t> outB;
        RIN_CHECK(rin::convertDepth16ToRgba8(depth, 5, 1, 5, 0.001f, 0.2f, 6.5f, scheme, outA));
        RIN_CHECK(rin::convertDepth16ToRgba8(depth, 5, 1, 5, 0.002f, 0.2f, 6.5f, scheme, outB));
        RIN_CHECK(outA == outB);
    }

    // 27) near/far 不影响输出（两组参数均合法，否则入口直接拒绝）：逐字节一致。
    {
        const auto scheme = rin::DepthColorScheme::AdaptiveGrayscale;
        const std::uint16_t depth[4] = {0, 400, 1600, 3200};
        std::vector<std::uint8_t> outA;
        std::vector<std::uint8_t> outB;
        RIN_CHECK(rin::convertDepth16ToRgba8(depth, 4, 1, 4, 0.001f, 0.2f, 6.5f, scheme, outA));
        RIN_CHECK(rin::convertDepth16ToRgba8(depth, 4, 1, 4, 0.001f, 1.0f, 3.0f, scheme, outB));
        RIN_CHECK(outA == outB);
    }

    // 28) 参数校验在 AdaptiveGrayscale 下同样生效：返回 false 且 dst 不被写。
    {
        const std::uint16_t depth[4] = {100, 200, 300, 400};
        std::vector<std::uint8_t> dst{0x5A, 0x5A};
        const std::vector<std::uint8_t> sentinel = dst;
        const auto scheme = rin::DepthColorScheme::AdaptiveGrayscale;
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

    // 29) stride > width：padding 哨兵 0xFFFF 不参与 frameMax 也不出现在输出；
    //     各像素取自正确行偏移（与紧凑输入逐字节一致 + 精确值抽检）。
    {
        constexpr std::uint32_t kWidth = 3;
        constexpr std::uint32_t kHeight = 2;
        constexpr std::uint32_t kStrideUnits = 5;
        const auto scheme = rin::DepthColorScheme::AdaptiveGrayscale;
        const std::uint16_t compact[6] = {1000, 2000, 4000, 4000, 2000, 1000};  // frameMax=4000
        std::vector<std::uint16_t> padded(kStrideUnits * kHeight, 0xFFFF);
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
        RIN_CHECK_EQ(fromPadded.size(), std::size_t{kWidth * kHeight * 4});
        // 精确值：raw=4000 -> 255、raw=2000 -> 128、raw=1000 -> 64。
        // 行 1 首像素取自行 1（若错位到行 0 padding 0xFFFF 会导致 frameMax 异常）。
        RIN_CHECK(sameColor(fromPadded.data() + 0, 64, 64, 64, 255));
        RIN_CHECK(sameColor(fromPadded.data() + 8, 255, 255, 255, 255));
        RIN_CHECK(sameColor(fromPadded.data() + 12, 255, 255, 255, 255));
        RIN_CHECK(sameColor(fromPadded.data() + 20, 64, 64, 64, 255));
        // 0xFFFF 若入 frameMax，raw=4000 只得 16；三态集合可区分该泄漏。
        for (std::size_t offset = 0; offset + 3 < fromPadded.size(); offset += 4) {
            const std::uint8_t level = fromPadded[offset];
            RIN_CHECK(level == 64 || level == 128 || level == 255);
            RIN_CHECK_EQ(fromPadded[offset + 3], std::uint8_t{255});
        }
    }

    // 30) 同一输入下 AdaptiveGrayscale 与 Grayscale 存在差异，三态（近黑/中灰/远白
    //     vs 近白/中灰/远黑）可区分。
    {
        const std::uint16_t depth[3] = {200, 3350, 6500};  // adaptive frameMax=6500
        std::vector<std::uint8_t> gray;
        std::vector<std::uint8_t> adaptive;
        RIN_CHECK(rin::convertDepth16ToRgba8(depth, 3, 1, 3, 0.001f, 0.2f, 6.5f,
                                             rin::DepthColorScheme::Grayscale, gray));
        RIN_CHECK(rin::convertDepth16ToRgba8(depth, 3, 1, 3, 0.001f, 0.2f, 6.5f,
                                             rin::DepthColorScheme::AdaptiveGrayscale, adaptive));
        bool differs = false;
        for (std::size_t i = 0; i < gray.size(); ++i) {
            if (gray[i] != adaptive[i]) {
                differs = true;
            }
        }
        RIN_CHECK(differs);
        // Grayscale：近白远黑（255/128/0）；Adaptive：近黑远白（8/131/255）。
        RIN_CHECK(sameColor(gray.data() + 0, 255, 255, 255, 255));
        RIN_CHECK(sameColor(gray.data() + 8, 0, 0, 0, 255));
        RIN_CHECK(sameColor(adaptive.data() + 0, 8, 8, 8, 255));
        RIN_CHECK(sameColor(adaptive.data() + 4, 131, 131, 131, 255));
        RIN_CHECK(sameColor(adaptive.data() + 8, 255, 255, 255, 255));
    }

    // 31) 单调性：raw 递增（非零、不超过 frameMax）时输出灰度单调不减；
    //     且任意样本 R==G==B（逐像素经 adaptiveGrayscaleColor 参照核对）。
    {
        const auto scheme = rin::DepthColorScheme::AdaptiveGrayscale;
        constexpr int kSamples = 64;
        std::vector<std::uint16_t> depth;
        depth.reserve(kSamples);
        for (int s = 0; s < kSamples; ++s) {
            // 100..1000 递增铺满（frameMax=1000，全部非零且 <= frameMax）。
            depth.push_back(
                static_cast<std::uint16_t>(100 + (1000 - 100) * s / (kSamples - 1)));
        }
        std::vector<std::uint8_t> rgba;
        RIN_CHECK(rin::convertDepth16ToRgba8(depth.data(),
                                             static_cast<std::uint32_t>(depth.size()), 1,
                                             static_cast<std::uint32_t>(depth.size()), 0.001f,
                                             0.2f, 6.5f, scheme, rgba));
        for (int s = 0; s < kSamples; ++s) {
            const std::size_t offset = static_cast<std::size_t>(s) * 4;
            RIN_CHECK_EQ(rgba[offset + 0], rgba[offset + 1]);
            RIN_CHECK_EQ(rgba[offset + 1], rgba[offset + 2]);
            RIN_CHECK_EQ(rgba[offset + 3], std::uint8_t{255});
            // 参照核对：经纯函数以同一 t=raw/frameMax 重算。
            const Rgba ref =
                adaptiveAt(static_cast<float>(depth[static_cast<std::size_t>(s)]) / 1000.0f);
            RIN_CHECK(sameColor(rgba.data() + offset, ref[0], ref[1], ref[2], ref[3]));
            if (s > 0) {
                RIN_CHECK(rgba[offset] >= rgba[offset - 4]);  // 近黑远白：非减
            }
        }
    }

    // ============ AdaptiveGrayscale P99 分位基准（频闪修复）============

    // 32) 分位语义（N=100 边界）：10x10 全有效帧，99 个 <=2000（第二大值恰为
    //     2000）+ 1 个 4000（排名 100）。rank = ceil(0.99*100) = 99 ->
    //     frameQuantile = 2000：4000（超分位）截断纯白 255；2000（恰在基准）
    //     纯白 255；1000 -> 128（1000*255/2000 = 127.5，+0.5 取整 128）。
    //     排序参照（升序取 rank-1 下标）逐像素双重核对。
    {
        const auto scheme = rin::DepthColorScheme::AdaptiveGrayscale;
        std::vector<std::uint16_t> depth(100, 1000);  // 10x10 全有效
        depth[98] = 2000;  // 第二大值：排名 99 = targetRank -> frameQuantile
        depth[99] = 4000;  // 最大值：排名 100，超分位截断
        std::vector<std::uint8_t> rgba;
        RIN_CHECK(rin::convertDepth16ToRgba8(depth.data(), 10, 10, 10, 0.001f, 0.2f, 6.5f,
                                             scheme, rgba));
        RIN_CHECK_EQ(rgba.size(), std::size_t{400});
        // 排序参照：有效值升序，rank-1 = 98 下标 -> 2000。
        std::vector<std::uint16_t> sorted(depth);
        std::sort(sorted.begin(), sorted.end());
        const std::uint16_t refQuantile = sorted[98];  // rank = 100 - 100/100 = 99
        RIN_CHECK_EQ(refQuantile, std::uint16_t{2000});
        RIN_CHECK(sameColor(rgba.data() + 99 * 4, 255, 255, 255, 255));  // 4000 超分位截断
        RIN_CHECK(sameColor(rgba.data() + 98 * 4, 255, 255, 255, 255));  // 2000 恰在基准
        for (int p = 0; p < 98; ++p) {  // 1000 -> 128（round-half-up：127.5 -> 128）
            RIN_CHECK(sameColor(rgba.data() + static_cast<std::size_t>(p) * 4, 128, 128,
                                128, 255));
        }
        // 双重核对：经纯函数以同一 t = raw/refQuantile 逐像素重算。
        for (int p = 0; p < 100; ++p) {
            const Rgba ref =
                adaptiveAt(static_cast<float>(depth[static_cast<std::size_t>(p)]) /
                           static_cast<float>(refQuantile));
            RIN_CHECK(sameColor(rgba.data() + static_cast<std::size_t>(p) * 4, ref[0],
                                ref[1], ref[2], ref[3]));
        }
    }

    // 33) 离群免疫（频闪回归锁定）：64x64 = 4096 全有效帧，4056 个 <=3000
    //     （4016 个 2500 + 40 个恰为 3000）+ 40 个 65535 离群（占比 <1%）。
    //     targetRank = 4096 - 40 = 4056 -> frameQuantile = 3000，离群不移动基准。
    //     断言：与"40 个离群换成 3000"的同一帧在非离群位置逐字节一致（整帧亮度
    //     稳定）；离群像素超分位截断纯白。
    {
        const auto scheme = rin::DepthColorScheme::AdaptiveGrayscale;
        constexpr std::size_t kCount = 4096;        // 64x64
        constexpr std::size_t kFirstOutlier = 4056;
        std::vector<std::uint16_t> withOutlier(kCount);
        std::vector<std::uint16_t> cleaned(kCount);
        for (std::size_t i = 0; i < kCount; ++i) {
            const std::uint16_t base =
                i < 4016 ? std::uint16_t{2500} : std::uint16_t{3000};
            withOutlier[i] = i < kFirstOutlier ? base : std::uint16_t{65535};
            cleaned[i] = base;  // 离群换成 3000
        }
        std::vector<std::uint8_t> fromOutlier;
        std::vector<std::uint8_t> fromClean;
        RIN_CHECK(rin::convertDepth16ToRgba8(withOutlier.data(), 64, 64, 64, 0.001f, 0.2f,
                                             6.5f, scheme, fromOutlier));
        RIN_CHECK(rin::convertDepth16ToRgba8(cleaned.data(), 64, 64, 64, 0.001f, 0.2f,
                                             6.5f, scheme, fromClean));
        RIN_CHECK_EQ(fromOutlier.size(), std::size_t{kCount * 4});
        RIN_CHECK_EQ(fromClean.size(), std::size_t{kCount * 4});
        for (std::size_t i = 0; i < kCount; ++i) {
            const std::size_t offset = i * 4;
            if (i >= kFirstOutlier) {
                // 离群像素超分位截断为纯白。
                RIN_CHECK(sameColor(fromOutlier.data() + offset, 255, 255, 255, 255));
                continue;
            }
            // 非离群位置：与无离群版本逐字节一致（基准未动 -> 亮度稳定）。
            RIN_CHECK(fromOutlier[offset + 0] == fromClean[offset + 0]);
            RIN_CHECK(fromOutlier[offset + 1] == fromClean[offset + 1]);
            RIN_CHECK(fromOutlier[offset + 2] == fromClean[offset + 2]);
            RIN_CHECK(fromOutlier[offset + 3] == fromClean[offset + 3]);
        }
    }

    // 34) 超分位截断：20x20 = 400 全有效帧，395 个 250 + 1 个 1000（排名 396 =
    //     targetRank -> frameQuantile = 1000）+ 4 个远超基准（1.5/2/4/16 倍）。
    //     多档超分位值均截断为纯白 255，彼此灰度无差异；基准以下不受影响。
    {
        const auto scheme = rin::DepthColorScheme::AdaptiveGrayscale;
        std::vector<std::uint16_t> depth(400, 250);
        depth[395] = 1000;   // 排名 396 = targetRank -> frameQuantile = 1000
        depth[396] = 1500;   // 1.5x
        depth[397] = 2000;   // 2x
        depth[398] = 4000;   // 4x
        depth[399] = 16000;  // 16x
        std::vector<std::uint8_t> rgba;
        RIN_CHECK(rin::convertDepth16ToRgba8(depth.data(), 20, 20, 20, 0.001f, 0.2f, 6.5f,
                                             scheme, rgba));
        RIN_CHECK_EQ(rgba.size(), std::size_t{1600});
        for (std::size_t i = 395; i < 400; ++i) {  // 四档超分位 + 基准值全为纯白
            RIN_CHECK(sameColor(rgba.data() + i * 4, 255, 255, 255, 255));
        }
        // 基准以下抽检：250 -> 0.25 -> 64（0.25*255+0.5 = 64.25 截断）。
        RIN_CHECK(sameColor(rgba.data() + 0, 64, 64, 64, 255));
        RIN_CHECK(sameColor(rgba.data() + 394 * 4, 64, 64, 64, 255));
    }

    // 35) 小帧退化性质（已知语义边界）：7x7 = 49（N<100）全有效帧含极端离群
    //     65535。P99 退化为绝对最大值 -> frameQuantile = 65535，离群不被免疫
    //     （真机小 ROI 场景的已知边界）。其余像素按 raw/65535 渲染：1000 -> 4、
    //     13107 -> 51（13107*255 = 65535*51，精确整倍数），参照逐像素核对。
    {
        const auto scheme = rin::DepthColorScheme::AdaptiveGrayscale;
        std::vector<std::uint16_t> depth(49, 1000);
        depth[47] = 13107;
        depth[48] = 65535;  // 极端离群：N<100 时成为归一化基准
        std::vector<std::uint8_t> rgba;
        RIN_CHECK(rin::convertDepth16ToRgba8(depth.data(), 7, 7, 7, 0.001f, 0.2f, 6.5f,
                                             scheme, rgba));
        RIN_CHECK_EQ(rgba.size(), std::size_t{196});
        RIN_CHECK(sameColor(rgba.data() + 48 * 4, 255, 255, 255, 255));  // 离群 -> 纯白
        RIN_CHECK(sameColor(rgba.data() + 47 * 4, 51, 51, 51, 255));
        RIN_CHECK(sameColor(rgba.data() + 0, 4, 4, 4, 255));
        // 参照核对：N<100 时 rank = N -> frameQuantile = 帧内最大值 65535。
        for (int p = 0; p < 49; ++p) {
            const Rgba ref =
                adaptiveAt(static_cast<float>(depth[static_cast<std::size_t>(p)]) /
                           65535.0f);
            RIN_CHECK(sameColor(rgba.data() + static_cast<std::size_t>(p) * 4, ref[0],
                                ref[1], ref[2], ref[3]));
        }
    }

    // 36) 排序参照随机化核对：固定种子 LCG 生成 32x32 = 1024 帧（值域 [1,65535]，
    //     约 1/8 置 0，另强制两个 0），与参照实现（有效值收集 + 排序 + 独立秩公式
    //     rank = N - floor(N/100)，与 ceil(99N/100) 恒等）计算的 frameQuantile
    //     渲染结果逐字节一致。
    {
        const auto scheme = rin::DepthColorScheme::AdaptiveGrayscale;
        constexpr std::size_t kCount = 1024;  // 32x32
        std::uint32_t lcg = 20260928u;        // 固定种子，保证可复现
        std::vector<std::uint16_t> depth(kCount);
        for (std::size_t i = 0; i < kCount; ++i) {
            lcg = lcg * 1103515245u + 12345u;
            const std::uint32_t sample = (lcg >> 16) & 0xFFFFu;
            depth[i] = (sample == 0 || sample % 8u == 0)
                           ? std::uint16_t{0}
                           : static_cast<std::uint16_t>(sample);
        }
        depth[0] = 0;      // 强制无效像素，确保 raw==0 分支被覆盖
        depth[513] = 0;
        // 参照：收集有效值排序，rank = N - floor(N/100)。
        std::vector<std::uint16_t> valid;
        valid.reserve(kCount);
        std::size_t zeroCount = 0;  // 强制置 0 之后统计，保证与帧一致
        for (const std::uint16_t raw : depth) {
            if (raw != 0) {
                valid.push_back(raw);
            } else {
                ++zeroCount;
            }
        }
        std::sort(valid.begin(), valid.end());
        const std::size_t rank = valid.size() - valid.size() / 100u;
        const std::uint16_t refQuantile = valid[rank - 1];
        RIN_CHECK(zeroCount >= 2);
        RIN_CHECK(valid.size() + zeroCount == kCount);
        RIN_CHECK(refQuantile >= 1);
        std::vector<std::uint8_t> rgba;
        RIN_CHECK(rin::convertDepth16ToRgba8(depth.data(), 32, 32, 32, 0.001f, 0.2f, 6.5f,
                                             scheme, rgba));
        RIN_CHECK_EQ(rgba.size(), std::size_t{kCount * 4});
        for (std::size_t i = 0; i < kCount; ++i) {
            const std::size_t offset = i * 4;
            if (depth[i] == 0) {
                RIN_CHECK(sameColor(rgba.data() + offset, 0, 0, 0, 255));
                continue;
            }
            const Rgba ref =
                adaptiveAt(static_cast<float>(depth[i]) /
                           static_cast<float>(refQuantile));
            RIN_CHECK(sameColor(rgba.data() + offset, ref[0], ref[1], ref[2], ref[3]));
        }
    }

    // 37) 既有不变量在 P99 大帧路径（N>=100：两趟直方图 + 渲染趟）下回归：
    //     全零帧不透明黑、尺度不变性、near/far 不变性、参数校验拒绝不写 dst、
    //     stride 哨兵隔离（小帧版本由既有 25-29 号用例继续覆盖；且 padding 哨兵
    //     0xFFFF 若进入直方图，validCount 与 targetRank 均被推移，e) 必然失败）。
    {
        const auto scheme = rin::DepthColorScheme::AdaptiveGrayscale;
        // a) 全零 64x64：整帧不透明黑，返回 true。
        std::vector<std::uint16_t> zeros(4096, 0);
        std::vector<std::uint8_t> black;
        RIN_CHECK(rin::convertDepth16ToRgba8(zeros.data(), 64, 64, 64, 0.001f, 0.2f, 6.5f,
                                             scheme, black));
        RIN_CHECK_EQ(black.size(), std::size_t{16384});
        bool allBlack = true;
        for (std::size_t offset = 0; offset + 3 < black.size(); offset += 4) {
            if (!sameColor(black.data() + offset, 0, 0, 0, 255)) {
                allBlack = false;
            }
        }
        RIN_CHECK(allBlack);
        // b) 尺度不变性 + c) near/far 不变性（64x64 全有效帧）。
        std::vector<std::uint16_t> depth(4096);
        for (std::size_t i = 0; i < 4096; ++i) {
            depth[i] = static_cast<std::uint16_t>(100 + (i * 37u) % 60000u);
        }
        std::vector<std::uint8_t> base;
        std::vector<std::uint8_t> scaleB;
        std::vector<std::uint8_t> windowB;
        RIN_CHECK(rin::convertDepth16ToRgba8(depth.data(), 64, 64, 64, 0.001f, 0.2f, 6.5f,
                                             scheme, base));
        RIN_CHECK(rin::convertDepth16ToRgba8(depth.data(), 64, 64, 64, 0.002f, 0.2f, 6.5f,
                                             scheme, scaleB));
        RIN_CHECK(rin::convertDepth16ToRgba8(depth.data(), 64, 64, 64, 0.001f, 1.0f, 3.0f,
                                             scheme, windowB));
        RIN_CHECK(base == scaleB);
        RIN_CHECK(base == windowB);
        // d) 参数校验：返回 false 且 dst 不被写。
        std::vector<std::uint8_t> dst{0x5A, 0x5A};
        const std::vector<std::uint8_t> sentinel = dst;
        RIN_CHECK(!rin::convertDepth16ToRgba8(nullptr, 64, 64, 64, 0.001f, 0.2f, 6.5f,
                                              scheme, dst));
        RIN_CHECK(!rin::convertDepth16ToRgba8(depth.data(), 0, 64, 64, 0.001f, 0.2f, 6.5f,
                                              scheme, dst));
        RIN_CHECK(!rin::convertDepth16ToRgba8(depth.data(), 64, 0, 64, 0.001f, 0.2f, 6.5f,
                                              scheme, dst));
        RIN_CHECK(!rin::convertDepth16ToRgba8(depth.data(), 64, 64, 63, 0.001f, 0.2f,
                                              6.5f, scheme, dst));
        RIN_CHECK(!rin::convertDepth16ToRgba8(depth.data(), 64, 64, 64, 0.0f, 0.2f, 6.5f,
                                              scheme, dst));
        RIN_CHECK(!rin::convertDepth16ToRgba8(depth.data(), 64, 64, 64, -1.0f, 0.2f, 6.5f,
                                              scheme, dst));
        RIN_CHECK(!rin::convertDepth16ToRgba8(depth.data(), 64, 64, 64, 0.001f, 6.5f,
                                              6.5f, scheme, dst));
        RIN_CHECK(!rin::convertDepth16ToRgba8(depth.data(), 64, 64, 64, 0.001f, 6.5f,
                                              0.2f, scheme, dst));
        RIN_CHECK(dst == sentinel);
        // e) stride 哨兵隔离：每行 64 数据 + 8 个 0xFFFF padding 单元。
        constexpr std::uint32_t kStrideUnits = 72;
        std::vector<std::uint16_t> padded(kStrideUnits * 64, 0xFFFF);
        for (std::uint32_t row = 0; row < 64; ++row) {
            for (std::uint32_t col = 0; col < 64; ++col) {
                padded[row * kStrideUnits + col] = depth[row * 64 + col];
            }
        }
        std::vector<std::uint8_t> fromPadded;
        RIN_CHECK(rin::convertDepth16ToRgba8(padded.data(), 64, 64, kStrideUnits, 0.001f,
                                             0.2f, 6.5f, scheme, fromPadded));
        RIN_CHECK(fromPadded == base);  // padding 单元不泄漏、行间不错位
    }

    return rin_test::exitStatus();
}
