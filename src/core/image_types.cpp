#include "rin/image_types.hpp"

namespace rin {

std::uint32_t elementSize(PortType type) noexcept {
    switch (type) {
        case PortType::Gray8:
            return 1;
        case PortType::Rgba8:
            return 4;
    }
    return 0;
}

namespace {

/// 共同的元数据校验（64 位乘法，防 uint32 回绕——与 NodeOutputSnapshot::valid
/// 同款纪律）；成功时经 minStrideOut 返回最小行宽、bytesOut 返回缓冲字节数。
bool metadataValid(PortType format, std::uint32_t width, std::uint32_t height,
                   std::uint32_t stride, std::uint64_t& minStrideOut,
                   std::uint64_t& bytesOut) noexcept {
    const std::uint64_t element = elementSize(format);
    if (element == 0 || width == 0 || height == 0) {
        return false;
    }
    const std::uint64_t minStride = static_cast<std::uint64_t>(width) * element;
    const std::uint64_t effectiveStride = stride == 0 ? minStride : stride;
    if (effectiveStride < minStride) {
        return false;
    }
    const std::uint64_t bytes = effectiveStride * height;
    if (bytes > kMaxImageBytes) {
        return false;
    }
    minStrideOut = minStride;
    bytesOut = bytes;
    return true;
}

}  // namespace

ImageU8 ImageU8::make(PortType format, std::uint32_t width, std::uint32_t height,
                      std::uint32_t stride) {
    std::uint64_t minStride = 0;
    std::uint64_t bytes = 0;
    if (!metadataValid(format, width, height, stride, minStride, bytes)) {
        return {};
    }
    auto pixels = std::make_shared<const std::vector<std::uint8_t>>(
        static_cast<std::size_t>(bytes), std::uint8_t{0});
    ImageU8 image;
    image.format_ = format;
    image.width_ = width;
    image.height_ = height;
    image.stride_ = stride == 0 ? static_cast<std::uint32_t>(minStride) : stride;
    image.pixels_ = std::move(pixels);
    return image;
}

ImageU8 ImageU8::wrap(PortType format, std::uint32_t width, std::uint32_t height,
                      std::uint32_t stride,
                      std::shared_ptr<const std::vector<std::uint8_t>> pixels) {
    std::uint64_t minStride = 0;
    std::uint64_t bytes = 0;
    if (pixels == nullptr ||
        !metadataValid(format, width, height, stride, minStride, bytes) ||
        pixels->size() < bytes) {
        return {};
    }
    ImageU8 image;
    image.format_ = format;
    image.width_ = width;
    image.height_ = height;
    image.stride_ = stride == 0 ? static_cast<std::uint32_t>(minStride) : stride;
    image.pixels_ = std::move(pixels);
    return image;
}

bool ImageU8::valid() const noexcept {
    if (pixels_ == nullptr || width_ == 0 || height_ == 0) {
        return false;
    }
    std::uint64_t minStride = 0;
    std::uint64_t bytes = 0;
    if (!metadataValid(format_, width_, height_, stride_, minStride, bytes)) {
        return false;
    }
    return pixels_->size() >= bytes;
}

std::uint64_t ImageU8::byteSize() const noexcept {
    return static_cast<std::uint64_t>(stride_) * height_;
}

const std::uint8_t* ImageU8::row(std::uint32_t y) const noexcept {
    if (pixels_ == nullptr || y >= height_) {
        return nullptr;
    }
    return pixels_->data() + static_cast<std::uint64_t>(stride_) * y;
}

}  // namespace rin
