// M9 米制深度转换纯单测（独立验证，Independent-Verification-Agent）：
// src/adapters/realsense/depth_metric_convert.hpp（header-only，零 SDK 依赖，
// namespace rin_realsense）。
//
// 语义判据（DEC-019 决策 1，docs/decisions/DEC-019-metric-depth-channel.md）：
//   - 米制值 = Z16 码值 × depth_scale（distance_to_image_plane，米）——线性
//     单趟换算，期望由冻结公式独立推导（测试内 double 参考交叉）；
//   - 无效像素（Z16 = 0）保持 0.0——rendition 保持测量原值，"无效 = 远距"
//     的策略语义由消费方 fillDepthInvalid（DEC-018 O1）决定；
//   - strideUnits 为行元素数（uint16 计），≥ width，允许行尾 padding，
//     padding 不得泄漏进输出；
//   - 失败（空指针/零尺寸/stride 不足/scale ≤ 0 或非有限）返回 false，
//     不部分写出（out 内容保持调用前原样）。
//
// 被测面与范围：
//   1) 精确换算：码值 × scale（scale 1.0 恒等、scale 0.5 倍频、典型
//      0.001 毫米→米），0 码值恒 0.0（含 +0 符号位）；非零最小码值 1 与
//      最大码值 65535 两端点。
//   2) strideUnits：== width 紧凑行距、> width 行距与对角模式逐元素（padding
//      哨兵 0xABAB 不泄漏）；全零图（全无效像素）输出全 0.0。
//   3) 全分支拒绝：nullptr / width 0 / height 0 / strideUnits < width /
//      scale ∈ {0, −0.0, 负值, NaN, +Inf, −Inf}，且每次拒绝均不部分写出
//      （预填哨兵的 out 内容与尺寸保持不变）。
//   4) out 复用重置语义：预填垃圾缓冲复用后尺寸恰为 width×height、值域
//      全部换新（无遗留）。
//   5) 848×480 规模（RealSense 默认深度幅面 + 典型 depth scale 0.001）：
//      伪随机码值全量 double 参考交叉（|diff| ≤ 1e−3 绝对）+ 冻结公式
//      float 形式逐元素精确相等互证。
//
// DOD-02 适用性说明：被测面为单线程纯函数（无任务提交/队列/取消/超时/
// shutdown 语义，无跨上下文共享状态），并发矩阵不适用（写法参照
// test_depth_preproc.cpp 文件头）。

#include "test_util.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "depth_metric_convert.hpp"

namespace {

using rin_realsense::convertDepth16ToMetric;

// 冻结公式 double 参考（DEC-019：米制值 = Z16 × scale）。
[[nodiscard]] double refMetric(std::uint16_t code, double scale) {
    return static_cast<double>(code) * scale;
}

}  // namespace

int main() {
    // --- 1) 精确换算与端点 ------------------------------------------------------
    {
        // scale = 1.0：码值恒等（float 精确）。
        const std::vector<std::uint16_t> codes = {0, 1, 2, 100, 12345, 65534, 65535};
        std::vector<float> out;
        const std::uint16_t* data = codes.data();
        RIN_CHECK(convertDepth16ToMetric(data, static_cast<std::uint32_t>(codes.size()), 1,
                                         static_cast<std::uint32_t>(codes.size()), 1.0f, out));
        RIN_CHECK_EQ(out.size(), codes.size());
        bool identity = true;
        for (std::size_t i = 0; i < codes.size(); ++i) {
            if (out[i] != static_cast<float>(codes[i])) {
                identity = false;
            }
        }
        RIN_CHECK(identity);         // scale 1.0 → 米制值 == 码值（无舍入漂移）。
        RIN_CHECK_EQ(out[0], 0.0f);  // 无效码值 0 → 0.0（保持测量原值）。

        // scale = 0.5：倍频；非零最小/最大码值端点。
        RIN_CHECK(convertDepth16ToMetric(data, static_cast<std::uint32_t>(codes.size()), 1,
                                         static_cast<std::uint32_t>(codes.size()), 0.5f, out));
        RIN_CHECK_EQ(out[1], 0.5f);      // 非零最小码值 1。
        RIN_CHECK_EQ(out[6], 32767.5f);  // 65535 × 0.5（float 精确）。
        RIN_CHECK_EQ(out[2], 1.0f);
        RIN_CHECK_EQ(out[0], 0.0f);

        // 典型 depth scale 0.001（毫米 → 米）：小图 golden（double 参考交叉）。
        const std::uint16_t scene[6] = {0, 500, 1200, 65535, 1, 3000};
        RIN_CHECK(convertDepth16ToMetric(scene, 3, 2, 3, 0.001f, out));
        RIN_CHECK_EQ(out.size(), std::size_t{6});
        for (std::size_t i = 0; i < 6; ++i) {
            const double expected = refMetric(scene[i], 0.001);
            RIN_CHECK_MSG(std::fabs(static_cast<double>(out[i]) - expected) <= 1e-3,
                          std::string("毫米→米 golden [") + std::to_string(i) + "]");
        }
        RIN_CHECK_EQ(out[0], 0.0f);
        RIN_CHECK_EQ(out[3], 65.535f);  // 最大码值端点（0.001f 换算的 float 最近值）。

        // 0 码值的符号位：恒 +0.0（非 −0、非 NaN）。
        RIN_CHECK(out[0] == 0.0f && !std::signbit(out[0]));
    }

    // --- 2) strideUnits 行距与 padding 不泄漏 -----------------------------------
    {
        constexpr std::uint32_t kWidth = 5;
        constexpr std::uint32_t kHeight = 4;
        constexpr std::uint32_t kStride = 8;
        constexpr std::uint16_t kPadSentinel = 0xABAB;

        // 行距 == width：紧凑。
        std::vector<std::uint16_t> compact(kWidth * kHeight);
        for (std::size_t i = 0; i < compact.size(); ++i) {
            compact[i] = static_cast<std::uint16_t>(i * 37u);
        }
        std::vector<float> out;
        RIN_CHECK(convertDepth16ToMetric(compact.data(), kWidth, kHeight, kWidth, 0.25f, out));
        RIN_CHECK_EQ(out.size(), std::size_t{kWidth} * kHeight);
        bool compactOk = true;
        for (std::size_t i = 0; i < compact.size(); ++i) {
            if (out[i] != static_cast<float>(compact[i]) * 0.25f) {
                compactOk = false;
            }
        }
        RIN_CHECK(compactOk);

        // 行距 > width：行偏移正确 + 行尾 padding 哨兵不泄漏（若按 strideUnits
        // 读出会得到 218.45 而非期望值）。
        std::vector<std::uint16_t> padded(kStride * kHeight, kPadSentinel);
        for (std::uint32_t y = 0; y < kHeight; ++y) {
            for (std::uint32_t x = 0; x < kWidth; ++x) {
                // 对角可区分模式：(y, x) → 1000·y + x（行/列错位可判）。
                padded[std::size_t{y} * kStride + x] = static_cast<std::uint16_t>(1000u * y + x);
            }
        }
        out.assign(1, -1.0f);
        RIN_CHECK(convertDepth16ToMetric(padded.data(), kWidth, kHeight, kStride, 1.0f, out));
        RIN_CHECK_EQ(out.size(), std::size_t{kWidth} * kHeight);
        bool paddedOk = true;
        for (std::uint32_t y = 0; y < kHeight; ++y) {
            for (std::uint32_t x = 0; x < kWidth; ++x) {
                const float expected = static_cast<float>(1000u * y + x);
                if (out[std::size_t{y} * kWidth + x] != expected) {
                    paddedOk = false;
                }
            }
        }
        RIN_CHECK(paddedOk);  // 行距与内容逐元素正确（padding 未混入）。

        // 全零图（全无效像素）：输出全 0.0。
        std::vector<std::uint16_t> zeros(kWidth * kHeight, 0);
        out.assign(1, 9.0f);
        RIN_CHECK(convertDepth16ToMetric(zeros.data(), kWidth, kHeight, kWidth, 0.001f, out));
        bool allZero = out.size() == kWidth * kHeight;
        for (const float value : out) {
            allZero = allZero && value == 0.0f;
        }
        RIN_CHECK(allZero);
    }

    // --- 3) 全分支拒绝（不部分写出） ---------------------------------------------
    {
        const std::uint16_t scene[6] = {10, 20, 30, 40, 50, 60};
        const std::uint16_t* data = scene;
        const float kValidScale = 0.001f;

        struct RejectCase {
            const char* name;
            const std::uint16_t* ptr;
            std::uint32_t width;
            std::uint32_t height;
            std::uint32_t strideUnits;
            float scale;
        };
        const std::vector<RejectCase> cases = {
            {"nullptr data", nullptr, 3, 2, 3, kValidScale},
            {"width 0", data, 0, 2, 3, kValidScale},
            {"height 0", data, 3, 0, 3, kValidScale},
            {"stride < width", data, 3, 2, 2, kValidScale},
            {"stride 0", data, 3, 2, 0, kValidScale},
            {"scale 0", data, 3, 2, 3, 0.0f},
            {"scale -0.0", data, 3, 2, 3, -0.0f},
            {"scale negative", data, 3, 2, 3, -0.001f},
            {"scale NaN", data, 3, 2, 3, std::numeric_limits<float>::quiet_NaN()},
            {"scale +Inf", data, 3, 2, 3, std::numeric_limits<float>::infinity()},
            {"scale -Inf", data, 3, 2, 3, -std::numeric_limits<float>::infinity()},
        };
        for (const RejectCase& reject : cases) {
            // 预填哨兵：拒绝路径不得部分写出（尺寸与内容保持原样）。
            std::vector<float> out(11, 7.5f);
            const bool accepted = convertDepth16ToMetric(reject.ptr, reject.width, reject.height,
                                                         reject.strideUnits, reject.scale, out);
            RIN_CHECK_MSG(!accepted, std::string("拒绝分支：") + reject.name);
            bool untouched = out.size() == 11;
            for (const float value : out) {
                untouched = untouched && value == 7.5f;
            }
            RIN_CHECK_MSG(untouched, std::string("拒绝不部分写出：") + reject.name);
        }
    }

    // --- 4) out 复用重置语义 ------------------------------------------------------
    {
        const std::uint16_t scene[4] = {100, 200, 0, 400};
        std::vector<float> out(100, 3.25f);  // 预填垃圾（尺寸与值均不同）。
        RIN_CHECK(convertDepth16ToMetric(scene, 2, 2, 2, 0.01f, out));
        RIN_CHECK_EQ(out.size(), std::size_t{4});  // 恰好 width×height（无遗留）。
        RIN_CHECK_EQ(out[0], 1.0f);
        RIN_CHECK_EQ(out[1], 2.0f);
        RIN_CHECK_EQ(out[2], 0.0f);
        RIN_CHECK_EQ(out[3], 4.0f);
    }

    // --- 5) 848×480 规模（全量 double 参考交叉 + 冻结公式 float 形式精确互证） ----
    {
        constexpr std::uint32_t kWidth = 848;
        constexpr std::uint32_t kHeight = 480;
        constexpr float kScale = 0.001f;
        std::vector<std::uint16_t> depth(static_cast<std::size_t>(kWidth) * kHeight);
        for (std::uint32_t y = 0; y < kHeight; ++y) {
            for (std::uint32_t x = 0; x < kWidth; ++x) {
                // 确定性伪随机码值（覆盖 0 / 小值 / 大值全值域）。
                depth[std::size_t{y} * kWidth + x] =
                    static_cast<std::uint16_t>((y * 848u + x * 7u) % 65536u);
            }
        }
        std::vector<float> out;
        RIN_CHECK(convertDepth16ToMetric(depth.data(), kWidth, kHeight, kWidth, kScale, out));
        RIN_CHECK_EQ(out.size(), static_cast<std::size_t>(kWidth) * kHeight);

        bool exactOk = true;   // 冻结公式的 float 计算形式逐元素相等。
        bool doubleOk = true;  // double 参考交叉 |diff| ≤ 1e−3。
        for (std::size_t i = 0; i < depth.size(); ++i) {
            const float exact = static_cast<float>(depth[i]) * kScale;
            if (out[i] != exact) {
                exactOk = false;
            }
            if (std::fabs(static_cast<double>(out[i]) - refMetric(depth[i], 0.001)) > 1e-3) {
                doubleOk = false;
            }
        }
        RIN_CHECK(exactOk);
        RIN_CHECK(doubleOk);
    }

    return rin_test::exitStatus();
}
