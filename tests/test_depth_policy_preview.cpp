// M9 策略深度预览模型 golden 测试（独立验证，Independent-Verification-Agent）：
// include/rin/depth_policy_preview.hpp、src/core/depth_policy_preview.cpp。
//
// 语义判据：
//   - DEC-019 决策 4（docs/decisions/DEC-019-metric-depth-channel.md）：快照 =
//     8 帧历史网格，2 列 × 4 行，每帧 32×18 最近邻 ×8 → 256×144 单元，画布
//     512×576 Rgba8（灰度复制 RGB、alpha 恒 255），8 帧按 sample() 顺序
//     （oldest→newest）阅读序排布（第 0 帧左上、第 7 帧右下）；值域 [0,1] →
//     uint8 round(v×255)（round-half-up）量化显示。
//   - depth_policy_preproc_design.md §5 冻结公式（O6 组合 + O7 抽样下标
//     idx[i] = historyLength − (sampleCount−1−i)·sampleSkip − 1 − sampleDelay、
//     欠帧首帧填充）与 §4 默认配置（E3 部署值：64×36 网格、crop (18,0,16,16)、
//     blur r=1 σ=1、[0,2.5] 归一化、history 37、sample 8 skip 5 delay 0）。
//   - ImageU8 契约（image_types.hpp）：stride 为行字节数，Rgba8 最小行宽 =
//     width × 4 —— 512 像素宽画布的有效 stride 为 2048 字节。
//
// golden 独立性说明：期望值全部由冻结公式独立推导——常量帧经 O6 全链
// （填充 → 面积降采样 → 裁切 → 模糊 → 归一化）保持常量（面积均值/高斯质量
// 守恒/线性归一化的数学性质），归一化值 = (c − near)/(far − near) 以测试内
// double 参考求值；O7 抽样下标 {1,6,…,36} 由冻结公式算出。golden 数值另经
// M8 公开算子（O6+O7 真管线）独立交叉确认一致。不参考实现代码、不 include
// 实现源文件。O6/O7 数值语义本身已由 M8 套件（test_depth_preproc.cpp）全分支
// 锁定，本文件在其上锁定 M9 的渲染层：布局映射、量化、计数与拒绝路径。
//
// 被测面与范围：
//   1) 快照初态（未处理帧 → grid 无效、计数全 0）与 grid 几何冻结
//      （512×576、stride = width×4 字节、Rgba8、byteSize、valid() 直通）。
//   2) 网格布局 golden：37 帧可区分常量序列（第 j 帧恒 0.05·(j+1) 米）→
//      抽样下标 {2,7,…,37} 帧落位 {左上…右下}，逐平面全格扫描（RGB ==
//      期望灰度、alpha == 255）+ 角点/边界锚点 + 8 平面灰度互异（防塌缩/
//      转置/错位混淆）。
//   3) 欠帧布局 + 首帧填充渲染（2 帧历史 → 前 7 格 oldest 值、末格 newest 值）。
//   4) 量化 golden（自定义 [1.25,2.5] 区间命中精确归一化值）：v=0→0、
//      v=0.5→128（round-half-up）、v=1→255；alpha 恒 255 全画布。
//   5) M8 冻结管线一致性抽查：默认 config 恒定 1.25 m 全图 → 归一化 0.5 →
//      全格灰度 128。
//   6) 计数与 reset：processedFrames/sourceSequence 推进、reset 幂等复位历史
//      且 historyResets 计数推进、grid 保留最后状态直到下一帧、复位后首帧
//      填充可观测（历史确已清空）。
//   7) 快照整体换新：连续 process 产生新共享缓冲（指针不同）、旧快照内容
//      保持（共享不可变）。
//   8) 拒绝：无效帧 process 抛 invalid_argument、尺寸不足帧（< raw 网格，
//      O2 仅缩小）抛、非法 config 构造抛；拒绝后快照计数保持不变。
//
// 防御性说明：算子契约只在"输入无效/参数越界"时抛 std::invalid_argument；
// 为使实现缺陷（意外异常类型、grid 缓冲被拒）不致套件中途 abort、失败清单
// 完整可回报，所有期望成功的 process 调用经 tryProcess 包裹（意外异常记失败
// 并跳过该节后续断言），与 test_depth_preproc.cpp 的 tryOp 纪律一致。
//
// DOD-02 适用性说明：PolicyDepthPreviewModel 为单写者纯逻辑（process/reset/
// snapshot 顺序调用，无任务提交/队列/取消/超时/shutdown 语义）；执行上下文
// 归调用方（viewer tick 组件的并发面由 test_policy_depth_component.cpp 覆盖），
// 并发矩阵不适用（写法参照 test_depth_preproc.cpp 文件头）。

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <rin/depth_policy_preview.hpp>
#include <rin/depth_preproc.hpp>
#include <rin/image_types.hpp>

namespace {

using rin::DepthFrameF32;
using rin::ImageU8;
using rin::PolicyDepthConfig;
using rin::PolicyDepthPreviewModel;
using rin::PolicyDepthPreviewSnapshot;

// --- 帧与期望值助手（期望全部由冻结公式独立求值） -----------------------------

// 构造常量米制帧（紧凑 stride = width）。
[[nodiscard]] DepthFrameF32 makeConstantFrame(std::uint32_t width, std::uint32_t height,
                                              float value) {
    std::vector<float> buffer(static_cast<std::size_t>(width) * height, value);
    auto shared = std::make_shared<const std::vector<float>>(std::move(buffer));
    return DepthFrameF32::wrap(width, height, width, std::move(shared));
}

// 期望成功的 process 包裹：契约外异常（如 grid 缓冲被 ImageU8 契约拒绝）记失败
// 并返回 false，调用方跳过依赖本帧的后续断言（失败清单完整可回报）。
bool tryProcess(PolicyDepthPreviewModel& model, const DepthFrameF32& frame, std::uint64_t sequence,
                const char* context) {
    try {
        model.process(frame, sequence);
        return true;
    } catch (const std::exception& error) {
        RIN_CHECK_MSG(false, std::string("process 意外异常（") + context + "）: " + error.what());
        return false;
    }
}

// 冻结量化（DEC-019 决策 4）：uint8(v×255 + 0.5)，round-half-up；double 参考。
[[nodiscard]] std::uint8_t quantize(double normalized) {
    return static_cast<std::uint8_t>(normalized * 255.0 + 0.5);
}

// 常量帧经 O6 冻结组合的期望归一化值：常量在填充（值 > 0 非无效）、降采样、
// 裁切、模糊（质量守恒）各级保持，仅线性归一化改变。
[[nodiscard]] double expectedNormalized(double constantMeters, const PolicyDepthConfig& config) {
    const double clipped = std::min(std::max(constantMeters, config.depthNear), config.depthFar);
    return (clipped - config.depthNear) / (config.depthFar - config.depthNear);
}

// 默认配置（E3 部署值）下常量帧的期望格灰度。
[[nodiscard]] std::uint8_t expectedGrayDefault(double constantMeters) {
    return quantize(expectedNormalized(constantMeters, PolicyDepthConfig{}));
}

// 网格布局冻结常量（DEC-019 决策 4）。
constexpr std::uint32_t kGridW = 512;
constexpr std::uint32_t kGridH = 576;
constexpr std::uint32_t kCellW = 256;
constexpr std::uint32_t kCellH = 144;

// 扫描第 cellIndex 格（0..7，阅读序）：RGB 全 == expectedGray 且 alpha == 255，
// 返回不匹配像素数（0 = 全格符合）。
[[nodiscard]] std::uint64_t cellMismatches(const ImageU8& grid, std::uint32_t cellIndex,
                                           std::uint8_t expectedGray) {
    const std::uint32_t col = cellIndex % 2;
    const std::uint32_t row = cellIndex / 2;
    const std::uint32_t originX = col * kCellW;
    const std::uint32_t originY = row * kCellH;
    std::uint64_t mismatches = 0;
    for (std::uint32_t y = 0; y < kCellH; ++y) {
        const std::uint8_t* pixels = grid.row(originY + y);
        if (pixels == nullptr) {
            return 1;  // 网格无效（防御；由上层 valid 断言先行定位）。
        }
        for (std::uint32_t x = 0; x < kCellW; ++x) {
            const std::size_t base = (static_cast<std::size_t>(originX) + x) * 4u;
            const bool grayOk = pixels[base + 0] == expectedGray &&
                                pixels[base + 1] == expectedGray &&
                                pixels[base + 2] == expectedGray;
            const bool alphaOk = pixels[base + 3] == 255u;
            if (!grayOk || !alphaOk) {
                ++mismatches;
            }
        }
    }
    return mismatches;
}

// 读取网格单像素 RGBA。
struct PixelRgba {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 0;
};

[[nodiscard]] PixelRgba pixelAt(const ImageU8& grid, std::uint32_t x, std::uint32_t y) {
    const std::uint8_t* pixels = grid.row(y) + static_cast<std::size_t>(x) * 4u;
    return PixelRgba{pixels[0], pixels[1], pixels[2], pixels[3]};
}

}  // namespace

int main() {
    // --- 1) 快照初态与 grid 几何冻结 ---------------------------------------------
    {
        PolicyDepthPreviewModel model;
        const PolicyDepthPreviewSnapshot& initial = model.snapshot();
        RIN_CHECK(!initial.valid());  // 未处理任何帧 → grid 无效。
        RIN_CHECK_EQ(initial.processedFrames, std::uint64_t{0});
        RIN_CHECK_EQ(initial.historyResets, std::uint64_t{0});
        RIN_CHECK_EQ(initial.sourceSequence, std::uint64_t{0});
        RIN_CHECK(model.config().valid());
        RIN_CHECK_EQ(model.config().policyWidth(), std::uint32_t{32});
        RIN_CHECK_EQ(model.config().policyHeight(), std::uint32_t{18});
    }
    {
        PolicyDepthPreviewModel model;
        if (tryProcess(model, makeConstantFrame(848, 480, 1.25f), 42, "几何冻结")) {
            const PolicyDepthPreviewSnapshot& snapshot = model.snapshot();
            RIN_CHECK(snapshot.valid());
            RIN_CHECK(snapshot.grid.valid());
            RIN_CHECK(snapshot.grid.format() == rin::PortType::Rgba8);
            RIN_CHECK_EQ(snapshot.grid.width(), kGridW);
            RIN_CHECK_EQ(snapshot.grid.height(), kGridH);
            // stride 为行字节数（image_types.hpp 契约）：Rgba8 512 像素宽的
            // 最小有效行宽 = 512×4 = 2048 字节。
            RIN_CHECK_EQ(snapshot.grid.stride(), kGridW * rin::elementSize(rin::PortType::Rgba8));
            RIN_CHECK_EQ(snapshot.grid.byteSize(),
                         std::uint64_t{kGridW} * kGridH * 4u);  // Rgba8 打包。
            RIN_CHECK_EQ(snapshot.processedFrames, std::uint64_t{1});
            RIN_CHECK_EQ(snapshot.sourceSequence, std::uint64_t{42});
            RIN_CHECK_EQ(snapshot.historyResets, std::uint64_t{0});
        }
    }

    // --- 2) 网格布局 golden（37 帧可区分序列 → 8 平面落位） -----------------------
    // 第 j 帧（1-based）恒 0.05·(j+1) 米（∈ (0, 2.5) 不触发填充/截断）。默认配置
    // 抽样下标 idx = {1,6,11,16,21,26,31,36} → 第 {2,7,…,37} 帧；平面 i 落位
    // (col, row) = (i mod 2, i / 2)（阅读序：第 0 帧左上、第 7 帧右下）。
    {
        PolicyDepthPreviewModel model;
        bool allProcessed = true;
        for (std::uint64_t j = 1; j <= 37; ++j) {
            const float constant = 0.05f * static_cast<float>(j + 1);
            allProcessed =
                tryProcess(model, makeConstantFrame(848, 480, constant), j, "布局 golden") &&
                allProcessed;
        }
        if (allProcessed) {
            const PolicyDepthPreviewSnapshot& snapshot = model.snapshot();
            RIN_CHECK_EQ(snapshot.processedFrames, std::uint64_t{37});
            RIN_CHECK_EQ(snapshot.sourceSequence, std::uint64_t{37});

            // 每平面期望灰度：第 (2+5i) 帧常量 0.05·(2+5i+1) 米 → 归一化 /2.5 →
            // 量化（独立 double 参考）。
            std::vector<std::uint8_t> planeGray(8);
            for (std::uint32_t i = 0; i < 8; ++i) {
                const double constant = 0.05 * static_cast<double>(2 + 5 * i + 1);
                planeGray[i] = expectedGrayDefault(constant);
            }
            // 8 平面灰度互异（防塌缩/抽样错位混淆）。
            bool distinct = true;
            for (std::uint32_t i = 0; i < 8; ++i) {
                for (std::uint32_t k = i + 1; k < 8; ++k) {
                    distinct = distinct && planeGray[i] != planeGray[k];
                }
            }
            RIN_CHECK_MSG(distinct, "布局 golden 平面灰度互异");

            // 逐平面全格扫描：阅读序落位（第 0 帧左上 → 第 7 帧右下）。
            for (std::uint32_t i = 0; i < 8; ++i) {
                const std::uint64_t mismatches = cellMismatches(snapshot.grid, i, planeGray[i]);
                RIN_CHECK_MSG(mismatches == 0, std::string("平面 ") + std::to_string(i) +
                                                   " 全格灰度/alpha（失配 " +
                                                   std::to_string(mismatches) + " px，期望 " +
                                                   std::to_string(planeGray[i]) + "）");
            }

            // 角点/边界锚点（失败时可读定位）：格原点、格内点、画布四角。
            const PixelRgba topLeft = pixelAt(snapshot.grid, 0, 0);
            RIN_CHECK_EQ(topLeft.r, planeGray[0]);  // 第 0 帧（f2）左上。
            const PixelRgba plane1Start = pixelAt(snapshot.grid, kCellW, 0);
            RIN_CHECK_EQ(plane1Start.r, planeGray[1]);
            const PixelRgba plane2Start = pixelAt(snapshot.grid, 0, kCellH);
            RIN_CHECK_EQ(plane2Start.r, planeGray[2]);
            const PixelRgba plane6Start = pixelAt(snapshot.grid, 0, 3 * kCellH);
            RIN_CHECK_EQ(plane6Start.r, planeGray[6]);
            const PixelRgba bottomRight = pixelAt(snapshot.grid, kGridW - 1, kGridH - 1);
            RIN_CHECK_EQ(bottomRight.r, planeGray[7]);  // 第 7 帧（f37，最新）右下。
            const PixelRgba plane7Start = pixelAt(snapshot.grid, kCellW, 3 * kCellH);
            RIN_CHECK_EQ(plane7Start.r, planeGray[7]);
            RIN_CHECK_EQ(topLeft.a, 255u);
            RIN_CHECK_EQ(bottomRight.a, 255u);
        }
    }

    // --- 3) 欠帧布局 + 首帧填充渲染 ------------------------------------------------
    {
        PolicyDepthPreviewModel model;
        const bool ok = tryProcess(model, makeConstantFrame(848, 480, 0.625f), 1, "欠帧 1") &&
                        tryProcess(model, makeConstantFrame(848, 480, 1.25f), 2, "欠帧 2");
        if (ok) {
            const PolicyDepthPreviewSnapshot& snapshot = model.snapshot();
            RIN_CHECK_EQ(snapshot.processedFrames, std::uint64_t{2});
            RIN_CHECK_EQ(snapshot.sourceSequence, std::uint64_t{2});
            // 填充后序列 = 首帧 ×36 + 第 2 帧；下标 {1,6,…,31} → 首帧（0.625 m →
            // 归一化 0.25 → 灰度 64）、下标 36 → 第 2 帧（1.25 m → 0.5 → 128）：
            // 前 7 格 oldest 值、末格 newest 值。
            for (std::uint32_t i = 0; i < 7; ++i) {
                const std::uint64_t mismatches = cellMismatches(snapshot.grid, i, 64u);
                RIN_CHECK_MSG(mismatches == 0, std::string("欠帧平面 ") + std::to_string(i) +
                                                   " 首帧填充（失配 " + std::to_string(mismatches) +
                                                   " px）");
            }
            RIN_CHECK_EQ(cellMismatches(snapshot.grid, 7, 128u), std::uint64_t{0});
        }
    }

    // --- 4) 量化 golden（自定义 [1.25, 2.5] 区间精确命中 0 / 0.5 / 1） --------------
    {
        PolicyDepthConfig config;
        config.depthNear = 1.25;
        config.depthFar = 2.5;
        RIN_CHECK(config.valid());

        // v = 0（常量 1.25 m == near）→ 灰度 0。
        PolicyDepthPreviewModel zeroModel(config);
        if (tryProcess(zeroModel, makeConstantFrame(848, 480, 1.25f), 1, "量化 v=0")) {
            for (std::uint32_t i = 0; i < 8; ++i) {
                RIN_CHECK_MSG(cellMismatches(zeroModel.snapshot().grid, i, 0u) == 0,
                              std::string("量化 v=0 平面 ") + std::to_string(i));
            }
        }

        // v = 0.5（常量 1.875 m，0.5×255+0.5 = 128 round-half-up）→ 灰度 128。
        PolicyDepthPreviewModel halfModel(config);
        if (tryProcess(halfModel, makeConstantFrame(848, 480, 1.875f), 2, "量化 v=0.5")) {
            for (std::uint32_t i = 0; i < 8; ++i) {
                RIN_CHECK_MSG(cellMismatches(halfModel.snapshot().grid, i, 128u) == 0,
                              std::string("量化 v=0.5 平面 ") + std::to_string(i));
            }
        }

        // v = 1（常量 2.5 m == far）→ 灰度 255；alpha 全画布恒 255。
        PolicyDepthPreviewModel oneModel(config);
        if (tryProcess(oneModel, makeConstantFrame(848, 480, 2.5f), 3, "量化 v=1")) {
            for (std::uint32_t i = 0; i < 8; ++i) {
                RIN_CHECK_MSG(cellMismatches(oneModel.snapshot().grid, i, 255u) == 0,
                              std::string("量化 v=1 平面 ") + std::to_string(i));
            }
            bool alphaAll = true;
            for (std::uint32_t y = 0; y < kGridH && alphaAll; ++y) {
                const std::uint8_t* row = oneModel.snapshot().grid.row(y);
                for (std::uint32_t x = 0; x < kGridW; ++x) {
                    if (row[static_cast<std::size_t>(x) * 4u + 3u] != 255u) {
                        alphaAll = false;
                        break;
                    }
                }
            }
            RIN_CHECK(alphaAll);
        }
    }

    // --- 5) M8 冻结管线一致性抽查：恒定 1.25 m → 归一化 0.5 → 全格 128 --------------
    {
        PolicyDepthPreviewModel model;
        if (tryProcess(model, makeConstantFrame(848, 480, 1.25f), 7, "一致性抽查")) {
            for (std::uint32_t i = 0; i < 8; ++i) {
                RIN_CHECK_MSG(cellMismatches(model.snapshot().grid, i, 128u) == 0,
                              std::string("常量 1.25m 一致性平面 ") + std::to_string(i));
            }
        }
    }

    // --- 6) 计数与 reset（幂等复位 + historyResets 计数 + grid 保留） ---------------
    {
        PolicyDepthPreviewModel model;
        const bool ok = tryProcess(model, makeConstantFrame(848, 480, 0.625f), 1, "reset 1") &&
                        tryProcess(model, makeConstantFrame(848, 480, 1.25f), 2, "reset 2");
        if (ok) {
            RIN_CHECK_EQ(model.snapshot().processedFrames, std::uint64_t{2});
            RIN_CHECK_EQ(model.snapshot().sourceSequence, std::uint64_t{2});
            RIN_CHECK_EQ(model.snapshot().historyResets, std::uint64_t{0});

            // reset：grid 保留最后状态（内容与缓冲不变、计数保留），historyResets 推进。
            const std::uint8_t* before = model.snapshot().grid.row(0);
            model.reset();
            RIN_CHECK_EQ(model.snapshot().historyResets, std::uint64_t{1});
            RIN_CHECK(model.snapshot().valid());
            RIN_CHECK_EQ(cellMismatches(model.snapshot().grid, 0, 64u), std::uint64_t{0});
            RIN_CHECK_EQ(cellMismatches(model.snapshot().grid, 7, 128u), std::uint64_t{0});
            RIN_CHECK_EQ(model.snapshot().processedFrames, std::uint64_t{2});  // 计数保留。
            RIN_CHECK(before == model.snapshot().grid.row(0));  // grid 未换新（保留语义）。

            // reset 幂等（重复复位安全），historyResets 仍逐次计数。
            model.reset();
            RIN_CHECK_EQ(model.snapshot().historyResets, std::uint64_t{2});

            // 复位后首帧填充可观测：历史确已清空 → 8 平面全为新帧值（1.875 m →
            // 归一化 0.75 → 灰度 191）；若历史未清空则前 7 格仍为旧帧值 64。
            if (tryProcess(model, makeConstantFrame(848, 480, 1.875f), 3, "reset 后 3")) {
                for (std::uint32_t i = 0; i < 8; ++i) {
                    RIN_CHECK_MSG(cellMismatches(model.snapshot().grid, i, 191u) == 0,
                                  std::string("复位后平面 ") + std::to_string(i) +
                                      " 全为新帧值（历史已清空）");
                }
                RIN_CHECK_EQ(model.snapshot().processedFrames, std::uint64_t{3});
                RIN_CHECK_EQ(model.snapshot().sourceSequence, std::uint64_t{3});
                RIN_CHECK_EQ(model.snapshot().historyResets, std::uint64_t{2});  // 复位计数不丢。
            }
        }
    }

    // --- 7) 快照整体换新（新共享缓冲 + 旧快照共享不可变） ----------------------------
    {
        PolicyDepthPreviewModel model;
        if (tryProcess(model, makeConstantFrame(848, 480, 0.625f), 1, "换新 1")) {
            const PolicyDepthPreviewSnapshot first = model.snapshot();  // 持有共享所有权。
            RIN_CHECK(first.grid.valid());
            const std::uint8_t* firstPixels = first.grid.row(0);

            if (tryProcess(model, makeConstantFrame(848, 480, 1.875f), 2, "换新 2")) {
                const PolicyDepthPreviewSnapshot& second = model.snapshot();
                RIN_CHECK(second.grid.row(0) != firstPixels);  // grid 为新共享缓冲。
                RIN_CHECK_EQ(second.sourceSequence, std::uint64_t{2});
                RIN_CHECK_EQ(second.processedFrames, std::uint64_t{2});
                // 旧快照内容保持（第 1 帧后 8 平面全为首帧值 64）。
                for (std::uint32_t i = 0; i < 8; ++i) {
                    RIN_CHECK_MSG(cellMismatches(first.grid, i, 64u) == 0,
                                  std::string("旧快照不可变 平面 ") + std::to_string(i));
                }
            }
        }
    }

    // --- 8) 拒绝路径（invalid_argument + 快照不变） ---------------------------------
    {
        PolicyDepthPreviewModel model;
        if (tryProcess(model, makeConstantFrame(848, 480, 1.25f), 9, "拒绝基线")) {
            const std::uint64_t frames = model.snapshot().processedFrames;
            const std::uint8_t* gridBefore = model.snapshot().grid.row(0);

            // 无效帧拒绝。
            bool rejected = false;
            try {
                model.process(DepthFrameF32{}, 10);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            RIN_CHECK_MSG(rejected, "无效帧 process 抛 invalid_argument");
            RIN_CHECK_EQ(model.snapshot().processedFrames, frames);  // 快照计数不变。
            RIN_CHECK(model.snapshot().grid.row(0) == gridBefore);

            // 尺寸不足帧（32×18 < raw 网格 64×36，O2 仅缩小）拒绝。
            rejected = false;
            try {
                model.process(makeConstantFrame(32, 18, 1.0f), 11);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            RIN_CHECK_MSG(rejected, "尺寸不足帧 process 抛 invalid_argument");
            RIN_CHECK_EQ(model.snapshot().processedFrames, frames);
        }

        // 非法 config 构造拒绝：crop 消耗全部网格宽 → policyWidth 0。
        PolicyDepthConfig zeroWidth;
        zeroWidth.cropLeft = 32;
        zeroWidth.cropRight = 32;
        RIN_CHECK(!zeroWidth.valid());
        bool rejected = false;
        try {
            PolicyDepthPreviewModel bad(zeroWidth);
            (void)bad;
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        RIN_CHECK_MSG(rejected, "非法 config 构造抛 invalid_argument");

        // 非法 config 构造拒绝：framesNeeded (36) > historyLength (3)。
        PolicyDepthConfig shortHistory;
        shortHistory.historyLength = 3;
        RIN_CHECK(!shortHistory.valid());
        rejected = false;
        try {
            PolicyDepthPreviewModel bad(shortHistory);
            (void)bad;
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        RIN_CHECK_MSG(rejected, "framesNeeded 超限 config 构造抛 invalid_argument");
    }

    return rin_test::exitStatus();
}
