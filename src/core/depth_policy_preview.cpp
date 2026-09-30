#include "rin/depth_policy_preview.hpp"

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace rin {

namespace {

/// 网格布局（DEC-019 决策 4 冻结）：2 列 × 4 行、每帧最近邻 ×8。
inline constexpr std::uint32_t kGridCols = 2;
inline constexpr std::uint32_t kGridRows = 4;
inline constexpr std::uint32_t kCellScale = 8;

}  // namespace

PolicyDepthPreviewModel::PolicyDepthPreviewModel(const PolicyDepthConfig& config)
    : config_(config), history_(config_) {}

void PolicyDepthPreviewModel::process(const DepthFrameF32& metricFrame,
                                      std::uint64_t sourceSequence) {
    if (!metricFrame.valid()) {
        throw std::invalid_argument("PolicyDepthPreviewModel process frame is invalid");
    }
    const DepthFrameF32 patch = preprocessPolicyDepthFrame(metricFrame, config_);
    history_.append(patch);
    const std::vector<float> sample = history_.sample();

    const std::uint32_t cellW = config_.policyWidth() * kCellScale;
    const std::uint32_t cellH = config_.policyHeight() * kCellScale;
    const std::uint32_t gridW = cellW * kGridCols;
    const std::uint32_t gridH = cellH * kGridRows;
    const std::size_t frameSize =
        static_cast<std::size_t>(config_.policyWidth()) * config_.policyHeight();

    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(gridW) * gridH * 4u, 0u);
    for (std::uint32_t i = 0; i < config_.sampleCount; ++i) {
        const std::uint32_t col = i % kGridCols;
        const std::uint32_t row = i / kGridCols;
        const std::uint32_t originX = col * cellW;
        const std::uint32_t originY = row * cellH;
        const float* plane = sample.data() + static_cast<std::size_t>(i) * frameSize;
        for (std::uint32_t y = 0; y < cellH; ++y) {
            const float* srcRow =
                plane + static_cast<std::size_t>(y / kCellScale) * config_.policyWidth();
            std::uint8_t* dstRow =
                rgba.data() + (static_cast<std::size_t>(originY + y) * gridW + originX) * 4u;
            for (std::uint32_t x = 0; x < cellW; ++x) {
                const float v = srcRow[x / kCellScale];
                // 值域 [0,1]（clipNormalizeDepth 保证）；round-half-up 量化显示。
                const auto gray = static_cast<std::uint8_t>(v * 255.0f + 0.5f);
                dstRow[x * 4u + 0] = gray;
                dstRow[x * 4u + 1] = gray;
                dstRow[x * 4u + 2] = gray;
                dstRow[x * 4u + 3] = 255u;
            }
        }
    }

    auto pixels = std::make_shared<const std::vector<std::uint8_t>>(std::move(rgba));
    // ImageU8 stride 语义为行字节数（image_types.hpp；与 DepthFrameF32 的元素
    // 数约定不同）：Rgba8 最小行宽 = 宽 × 4。
    ImageU8 grid = ImageU8::wrap(PortType::Rgba8, gridW, gridH,
                                 gridW * elementSize(PortType::Rgba8), std::move(pixels));
    if (!grid.valid()) {
        throw std::runtime_error("policy depth preview produced a rejected grid buffer");
    }

    snapshot_.sourceSequence = sourceSequence;
    ++snapshot_.processedFrames;
    snapshot_.grid = std::move(grid);
}

void PolicyDepthPreviewModel::reset() noexcept {
    history_.reset();
    ++snapshot_.historyResets;
}

}  // namespace rin
