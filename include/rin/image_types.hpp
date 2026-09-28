#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "rin/workflow_types.hpp"

namespace rin {

/// 端口图像类型的单像素字节数（Gray8=1，Rgba8=4）；未知格式返回 0。
[[nodiscard]] std::uint32_t elementSize(PortType type) noexcept;

/// 单幅图像像素缓冲上界（字节）：16 MiB。覆盖当前支持的全部流配置
/// （1920×1200 Rgba8 ≈ 8.8 MiB）并留有余量；超出即构造失败（容量有界，
/// AGENTS.md 工程约束：所有缓冲有预算上限）。
inline constexpr std::uint64_t kMaxImageBytes = 16u * 1024u * 1024u;

/// 工作流图像（M4-02 Core 契约）：Gray8 / Rgba8、共享不可变像素、容量有界。
///
/// 像素缓冲经 shared_ptr<const vector<uint8_t>> 共享，构造（提交）后不可变，
/// 拷贝只复制元数据 + 引用计数——节点输出向下游多消费者与产物快照
/// （NodeOutputSnapshot 同款承载）传递零拷贝。stride 为行字节数，允许行尾
/// padding；缓冲字节数恰为 stride×height，行外读取按契约禁用。
///
/// 值语义：默认构造为无效空图像（valid()==false），仅作占位与工厂失败返回值；
/// 工厂函数（make/wrap）不抛异常，失败一律返回无效图像。
class ImageU8 {
public:
    /// 无效空图像。
    ImageU8() = default;

    /// 分配并构造：像素缓冲由本调用分配（值初始化为零）。格式未知、宽高为 0、
    /// stride 小于最小行宽或缓冲超过 kMaxImageBytes 时返回无效图像。
    [[nodiscard]] static ImageU8 make(PortType format, std::uint32_t width,
                                      std::uint32_t height, std::uint32_t stride = 0);

    /// 接管既有缓冲（适配器零拷贝路径、节点内填充缓冲等）。pixels 为空、尺寸
    /// 非法或缓冲小于 stride×height 时返回无效图像；接管后以 const 视图共享，
    /// 调用方不得再改写其内容。
    [[nodiscard]] static ImageU8 wrap(PortType format, std::uint32_t width,
                                      std::uint32_t height, std::uint32_t stride,
                                      std::shared_ptr<const std::vector<std::uint8_t>> pixels);

    /// 有效性：非默认构造成功（格式已知、宽高非零、stride 达到最小行宽、
    /// 像素缓冲非空且不小于 stride×height）。
    [[nodiscard]] bool valid() const noexcept;

    [[nodiscard]] PortType format() const noexcept { return format_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::uint32_t stride() const noexcept { return stride_; }

    /// 缓冲字节数（stride×height；无效图像为 0）。
    [[nodiscard]] std::uint64_t byteSize() const noexcept;

    /// 共享像素缓冲；无效图像返回空指针。
    [[nodiscard]] const std::shared_ptr<const std::vector<std::uint8_t>>&
    pixels() const noexcept {
        return pixels_;
    }

    /// 行首指针；图像无效或 y ≥ height 返回 nullptr。
    [[nodiscard]] const std::uint8_t* row(std::uint32_t y) const noexcept;

private:
    PortType format_ = PortType::Gray8;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t stride_ = 0;
    std::shared_ptr<const std::vector<std::uint8_t>> pixels_;
};

}  // namespace rin
