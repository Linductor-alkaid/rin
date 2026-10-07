#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rin::detail {

/// Reflect-101 采样下标唯一实现（M12/CR-02，M4-04 冻结公式）：周期 2(n−1)
/// 折返（−1→1、n→n−2）；n <= 1（含防御性 n <= 0，零宽高在图像层已被拒绝）
/// 时一律取 0。image_ops（Gray8 域）与 depth_preproc（Depth32F 域）共用，
/// 禁止单边修改。
[[nodiscard]] inline std::int64_t reflectIndex101(std::int64_t index,
                                                  std::int64_t size) noexcept {
    if (size <= 1) {
        return 0;
    }
    const std::int64_t period = 2 * (size - 1);
    std::int64_t folded = index % period;
    if (folded < 0) {
        folded += period;
    }
    if (folded >= size) {
        folded = period - folded;
    }
    return folded;
}

/// 一维高斯核唯一实现（M12/CR-03，M8 冻结公式）：G[i] ∝ exp(−(i−r)²/(2σ²))
/// 归一化 Σ=1；sigma=0 为 δ 核（极限语义，输出逐像素恒等）。二维核为该核与
/// 自身的外积（对称可分离）。image_ops（Gray8 域）与 depth_preproc
/// （Depth32F 域）共用，禁止单边修改。
[[nodiscard]] inline std::vector<double> gaussianKernel1D(std::size_t radius,
                                                          double sigma) {
    const std::size_t size = 2 * radius + 1;
    std::vector<double> kernel(size, 0.0);
    if (sigma == 0.0) {
        kernel[radius] = 1.0;
        return kernel;
    }
    double sum = 0.0;
    for (std::size_t i = 0; i < size; ++i) {
        const double delta = static_cast<double>(i) - static_cast<double>(radius);
        kernel[i] = std::exp(-(delta * delta) / (2.0 * sigma * sigma));
        sum += kernel[i];
    }
    for (double& value : kernel) {
        value /= sum;
    }
    return kernel;
}

/// O7 环形历史抽样下标唯一实现（M12/CR-06，M8 冻结公式）：欠帧首帧填充，
/// 概念序列 = [首帧] × (L − count) + 实际帧（oldest→newest）；第 index 个
/// 抽样（0 基，oldest 起）的 idx 单调递增。返回环内槽位。count=0 时调用方
/// 依赖环缓冲已清零（pos 恒为首帧槽位）。
[[nodiscard]] inline std::size_t historySamplePos(std::size_t historyLength,
                                                  std::size_t count, std::size_t head,
                                                  std::size_t index, std::size_t sampleCount,
                                                  std::size_t sampleSkip,
                                                  std::size_t sampleDelay) noexcept {
    const std::size_t first = (head + historyLength - count) % historyLength;
    const std::size_t pad = historyLength - count;
    const std::size_t idx = historyLength -
                            ((sampleCount - 1u - index) * sampleSkip) - 1u - sampleDelay;
    return idx < pad ? first : (first + (idx - pad)) % historyLength;
}

/// O7 入环推进唯一实现（M12/CR-06）：head 环进；count 未满 L 前递增。
inline void advanceHistoryRing(std::size_t& head, std::size_t& count,
                               std::size_t historyLength) noexcept {
    head = (head + 1u) % historyLength;
    if (count < historyLength) {
        ++count;
    }
}

/// O7 历史参数约束唯一实现（M12/CR-06，M8 冻结约束；对齐
/// delayed_visualizable_image.check_delay_bounds 的通过条件 ≤）：L ∈ [1,4096]；
/// count/skip 为正；count/skip/delay ≤ L；(count−1)·skip+1+delay ≤ L（乘法
/// 回绕钳制：三者 ≤ L 后 64 位乘法无回绕）。
[[nodiscard]] inline bool historyParamsValid(std::size_t historyLength,
                                             std::size_t sampleCount, std::size_t sampleSkip,
                                             std::size_t sampleDelay) noexcept {
    if (historyLength == 0 || historyLength > 4096) {
        return false;
    }
    if (sampleCount == 0 || sampleSkip == 0) {
        return false;
    }
    if (sampleCount > historyLength || sampleSkip > historyLength ||
        sampleDelay > historyLength) {
        return false;
    }
    const std::uint64_t framesNeeded =
        (static_cast<std::uint64_t>(sampleCount) - 1u) *
            static_cast<std::uint64_t>(sampleSkip) +
        1u + static_cast<std::uint64_t>(sampleDelay);
    return framesNeeded <= static_cast<std::uint64_t>(historyLength);
}

}  // namespace rin::detail
