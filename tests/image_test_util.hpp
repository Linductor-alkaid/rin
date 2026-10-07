#pragma once

// 图像测试夹具唯一实现（M12/CR-40、CR-41，替代 test_image_ops_* 与面板测试的
// 手抄副本）。依赖仅 rin 公开图像契约（ImageU8），无第三方。

#include <rin/image_types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace rin_test {

// 构造紧凑（stride=0 → 最小行宽）或行尾 padding（stride=rowBytes）的 Rgba8 图像；
// padding 区域填充 padByte（默认 0xEE，用于断言不泄漏进输出）。
template <typename PixelFn>
[[nodiscard]] rin::ImageU8 makeRgba(std::uint32_t width, std::uint32_t height,
                                    const PixelFn& pixel, std::uint32_t rowBytes = 0,
                                    std::uint8_t padByte = 0xEE) {
    const std::uint32_t stride = rowBytes == 0 ? width * 4u : rowBytes;
    std::vector<std::uint8_t> buffer(std::size_t{stride} * height, padByte);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::array<std::uint8_t, 4> p = pixel(x, y);
            std::memcpy(buffer.data() + std::size_t{stride} * y + std::size_t{x} * 4u,
                        p.data(), 4);
        }
    }
    auto shared = std::make_shared<const std::vector<std::uint8_t>>(std::move(buffer));
    return rin::ImageU8::wrap(rin::PortType::Rgba8, width, height, stride, std::move(shared));
}

// 构造紧凑（stride=0 → 宽）或行尾 padding（stride=rowBytes）的 Gray8 图像。
template <typename PixelFn>
[[nodiscard]] rin::ImageU8 makeGray(std::uint32_t width, std::uint32_t height,
                                    const PixelFn& pixel, std::uint32_t rowBytes = 0,
                                    std::uint8_t padByte = 0xEE) {
    const std::uint32_t stride = rowBytes == 0 ? width : rowBytes;
    std::vector<std::uint8_t> buffer(std::size_t{stride} * height, padByte);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            buffer[std::size_t{stride} * y + x] = pixel(x, y);
        }
    }
    auto shared = std::make_shared<const std::vector<std::uint8_t>>(std::move(buffer));
    return rin::ImageU8::wrap(rin::PortType::Gray8, width, height, stride, std::move(shared));
}

// 确定性字节伪随机（LCG，测试自含；与被测实现无共享状态）。
class ByteLcg {
public:
    explicit ByteLcg(std::uint32_t seed) : state_(seed) {}
    std::uint8_t next() {
        state_ = state_ * 1664525u + 1013904223u;
        return static_cast<std::uint8_t>((state_ >> 16) & 0xFFu);
    }

private:
    std::uint32_t state_;
};

// 确定性 Rgba8 像素缓冲（M12/CR-41；各尺寸共用同一公式，测试断言不依赖具体
// 像素值，只要求跨调用稳定）：R=(x·2+y)、G=(x+y·3)、B=(x·5+y·7)、A=255。
[[nodiscard]] inline std::shared_ptr<const std::vector<std::uint8_t>>
deterministicRgbaPixels(std::uint32_t width, std::uint32_t height) {
    auto buffer = std::make_shared<std::vector<std::uint8_t>>(
        static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
            (*buffer)[offset + 0] = static_cast<std::uint8_t>((x * 2 + y) & 0xFF);
            (*buffer)[offset + 1] = static_cast<std::uint8_t>((x + y * 3) & 0xFF);
            (*buffer)[offset + 2] = static_cast<std::uint8_t>((x * 5 + y * 7) & 0xFF);
            (*buffer)[offset + 3] = 0xFF;
        }
    }
    return buffer;
}

}  // namespace rin_test
