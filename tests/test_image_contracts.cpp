// M4-02 Core 图像与节点契约测试（独立验证）：include/rin/image_types.hpp、
// include/rin/image_node.hpp、src/core/image_types.cpp、src/core/image_node.cpp。
//
// 被测面与范围（对应 docs/design/image_workflow_design.md §8 测试矩阵前两行）：
// 1) elementSize：Gray8=1、Rgba8=4；
// 2) ImageU8::make 正例：零初始化缓冲、stride=0 取最小行宽、行尾 padding、
//    预算内最大幅面；无效元数据负例：零宽/零高/未知格式/stride 小于最小行宽；
// 3) 64 位乘法与容量边界（契约边界探针，按冻结语义断言、不迁就实现）：
//    width=2^30 的 Rgba8（width*4 在 uint32 下回绕为 0 的反例）必须拒绝；
//    stride×height ≥ 2^32 的回绕反例必须拒绝；恰等 kMaxImageBytes 成功、
//    超 1 字节（经宽/高/stride 任一维）失败；
// 4) ImageU8::wrap：接管共享缓冲（缓冲对象同一性）、null 缓冲/元数据非法/
//    缓冲短于 stride×height 负例、超尺寸缓冲合法、stride=0 取最小行宽；
// 5) 默认态与 row/byteSize：默认构造无效、byteSize=0、row 恒 nullptr；
//    行指针 = 基址 + stride×y、y ≥ height 越界 nullptr、padding 下行距正确；
// 6) 值语义：拷贝共享像素（引用计数 + 缓冲对象同一性）、移动转移所有权；
//    像素缓冲 const 不可变（pixels() 返回 shared_ptr<const vector<uint8_t>>，
//    类型面 static_assert）；
// 7) 参数模型：effectiveParamValue 三态（未赋值取声明默认值——指针指向声明内
//    默认值；已赋值取实例值——指针指向赋值存储；多条赋值取最后一条；未声明
//    paramId 返回 nullptr）；paramBoolean/paramInteger/paramReal/
//    paramEnumeration/paramRealArray 五种类型化读取：kind 匹配取值拷贝、
//    未声明或种类不符（含运行期直接构造的种类错位赋值）返回 nullopt。
//
// DOD-02 适用性说明：本契约面全部为单线程纯逻辑值语义（图像值对象与参数
// 查找纯函数，无任务提交/队列/取消/超时/shutdown 语义，无跨上下文共享状态），
// 并发矩阵不适用（写法参照 test_motion_contracts.cpp / test_workflow_contracts.cpp
// 文件头）；并发行为归 M4-07 引擎与 M5-08 契约套件。
#include "test_util.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <rin/image_node.hpp>
#include <rin/image_types.hpp>
#include <rin/workflow_types.hpp>

namespace {

using rin::ImageU8;
using rin::NodeDescriptor;
using rin::NodeInstance;
using rin::ParamAssignment;
using rin::ParamDescriptor;
using rin::ParamKind;
using rin::ParamValue;
using rin::PortType;
using rin::effectiveParamValue;
using rin::elementSize;
using rin::kMaxImageBytes;
using rin::paramBoolean;
using rin::paramEnumeration;
using rin::paramInteger;
using rin::paramReal;
using rin::paramRealArray;

static_assert(kMaxImageBytes == 16u * 1024u * 1024u,
              "kMaxImageBytes 冻结为 16 MiB（契约常量）");
static_assert(std::is_same_v<
                  decltype(std::declval<const ImageU8&>().pixels()),
                  const std::shared_ptr<const std::vector<std::uint8_t>>&>,
              "pixels() 必须返回 const 缓冲的共享视图（构造后不可变，类型面锁定）");

ParamDescriptor makeParam(const std::string& id, ParamKind kind, ParamValue defaultValue) {
    ParamDescriptor p;
    p.id = id;
    p.label = id;
    p.kind = kind;
    p.defaultValue = std::move(defaultValue);
    return p;
}

// 全参数种类的小目录项：enable:Boolean / radius:Integer / sigma:Real /
// mode:Enumeration / kernel:RealArray。
NodeDescriptor makeTypedDescriptor() {
    NodeDescriptor d;
    d.typeId = "typed";
    d.displayName = "类型化参数节点";
    d.outputs = {PortType::Gray8};
    d.params = {
        makeParam("enable", ParamKind::Boolean, true),
        makeParam("radius", ParamKind::Integer, static_cast<std::int64_t>(7)),
        makeParam("sigma", ParamKind::Real, 1.5),
        makeParam("mode", ParamKind::Enumeration, std::string("fast")),
        makeParam("kernel", ParamKind::RealArray, std::vector<double>{0.25, 0.5, 0.25}),
    };
    return d;
}

ParamAssignment assign(const std::string& paramId, ParamValue value) {
    return ParamAssignment{paramId, std::move(value)};
}

}  // namespace

int main() {
    // --- 1) elementSize ---
    RIN_CHECK_EQ(elementSize(PortType::Gray8), std::uint32_t{1});
    RIN_CHECK_EQ(elementSize(PortType::Rgba8), std::uint32_t{4});

    // --- 2) ImageU8::make 正例 ---
    {
        // 最小幅面：1x1 Gray8，缓冲 1 字节且零初始化。
        const ImageU8 tiny = ImageU8::make(PortType::Gray8, 1, 1);
        RIN_CHECK(tiny.valid());
        RIN_CHECK_EQ(tiny.format(), PortType::Gray8);
        RIN_CHECK_EQ(tiny.width(), std::uint32_t{1});
        RIN_CHECK_EQ(tiny.height(), std::uint32_t{1});
        RIN_CHECK_EQ(tiny.stride(), std::uint32_t{1});
        RIN_CHECK_EQ(tiny.byteSize(), std::uint64_t{1});
        RIN_CHECK(tiny.pixels() != nullptr);
        RIN_CHECK_EQ(tiny.pixels()->size(), std::size_t{1});

        // Gray8：stride=0 取最小行宽（width×1），缓冲零初始化。
        const ImageU8 gray = ImageU8::make(PortType::Gray8, 8, 4);
        RIN_CHECK(gray.valid());
        RIN_CHECK_EQ(gray.stride(), std::uint32_t{8});
        RIN_CHECK_EQ(gray.byteSize(), std::uint64_t{32});
        RIN_CHECK_EQ(gray.pixels()->size(), std::size_t{32});
        bool allZero = true;
        for (const std::uint8_t byte : *gray.pixels()) {
            allZero = allZero && byte == 0;
        }
        RIN_CHECK(allZero);

        // Rgba8：最小行宽 width×4。
        const ImageU8 rgba = ImageU8::make(PortType::Rgba8, 4, 3);
        RIN_CHECK(rgba.valid());
        RIN_CHECK_EQ(rgba.stride(), std::uint32_t{16});
        RIN_CHECK_EQ(rgba.byteSize(), std::uint64_t{48});

        // 行尾 padding：显式 stride 大于最小行宽，byteSize = stride×height。
        const ImageU8 padded = ImageU8::make(PortType::Gray8, 4, 3, 12);
        RIN_CHECK(padded.valid());
        RIN_CHECK_EQ(padded.stride(), std::uint32_t{12});
        RIN_CHECK_EQ(padded.byteSize(), std::uint64_t{36});

        // 预算内最大幅面：4096×4096 Gray8 = 16 MiB。
        const ImageU8 maxGray = ImageU8::make(PortType::Gray8, 4096, 4096);
        RIN_CHECK(maxGray.valid());
        RIN_CHECK_EQ(maxGray.byteSize(), kMaxImageBytes);
        const ImageU8 maxRgba = ImageU8::make(PortType::Rgba8, 2048, 2048);
        RIN_CHECK(maxRgba.valid());
        RIN_CHECK_EQ(maxRgba.byteSize(), kMaxImageBytes);
    }
    {
        // 无效元数据负例：零宽 / 零高 / 未知格式 / stride 小于最小行宽。
        RIN_CHECK(!ImageU8::make(PortType::Gray8, 0, 4).valid());
        RIN_CHECK(!ImageU8::make(PortType::Gray8, 8, 0).valid());
        RIN_CHECK(!ImageU8::make(PortType::Gray8, 0, 0).valid());
        // 枚举损坏值（实现防御路径；写法沿用 test_motion_contracts 的 corrupted
        // kind 惯例）。
        const auto unknown = static_cast<PortType>(99);
        RIN_CHECK(!ImageU8::make(unknown, 4, 4).valid());
        // Gray8 最小行宽 = width：8 字节 < 8+1？width=8 需要 ≥ 8，给 7/0 均拒绝。
        RIN_CHECK(!ImageU8::make(PortType::Gray8, 8, 4, 7).valid());
        // Rgba8 最小行宽 = width×4：width=4 需要 ≥ 16，15 拒绝。
        RIN_CHECK(!ImageU8::make(PortType::Rgba8, 4, 3, 15).valid());
        // stride=0 表示取最小行宽（合法，等价于显式最小行宽）。
        RIN_CHECK(ImageU8::make(PortType::Rgba8, 4, 3, 0).valid());
        RIN_CHECK(ImageU8::make(PortType::Rgba8, 4, 3, 16).valid());
    }

    // --- 3) 64 位乘法与容量边界（契约边界探针）---
    {
        // uint32 回绕反例：width=2^30、Rgba8 时 width*4 在 uint32 下 ≡ 0，
        // 若按 32 位计算会"通过"校验；按契约（64 位乘法）必须拒绝。
        constexpr std::uint32_t kHugeWidth = 1u << 30;
        RIN_CHECK(!ImageU8::make(PortType::Rgba8, kHugeWidth, 1).valid());
        RIN_CHECK(!ImageU8::make(PortType::Gray8, kHugeWidth, 1).valid());

        // stride×height ≥ 2^32 的回绕反例：65536×65536 = 4 GiB（uint32 下 ≡ 0），
        // 64 位计算下远超 16 MiB 预算，必须拒绝（且不得先分配）。
        RIN_CHECK(!ImageU8::make(PortType::Gray8, 65536, 65536).valid());
        // 单行超宽：stride×1 = 接近 uint32 上限，64 位下仍超预算。
        RIN_CHECK(!ImageU8::make(PortType::Gray8, 1, 1, 0xFFFFFFFFu).valid());

        // 恰等 kMaxImageBytes 成功（闭区间上界）；超 1 字节失败——经宽、高、
        // stride 三个维度逐一构造。
        RIN_CHECK(ImageU8::make(PortType::Gray8, 4096, 4096).valid());
        RIN_CHECK(!ImageU8::make(PortType::Gray8, 4097, 4096).valid());
        RIN_CHECK(!ImageU8::make(PortType::Gray8, 4096, 4097).valid());
        RIN_CHECK(!ImageU8::make(PortType::Gray8, 4096, 4096, 4097).valid());
        RIN_CHECK(ImageU8::make(PortType::Rgba8, 2048, 2048).valid());
        RIN_CHECK(!ImageU8::make(PortType::Rgba8, 2048, 2049).valid());
        // stride padding 挤占预算：width=1、stride=16 MiB 恰等预算可行，
        // 再多 1 字节 padding 拒绝。
        RIN_CHECK(ImageU8::make(PortType::Gray8, 1, 1, 16u * 1024u * 1024u).valid());
        RIN_CHECK(!ImageU8::make(PortType::Gray8, 1, 1, 16u * 1024u * 1024u + 1).valid());
    }

    // --- 4) ImageU8::wrap ---
    {
        // 接管共享缓冲：缓冲对象同一性（同一 shared_ptr 目标），内容保留。
        auto buffer = std::make_shared<const std::vector<std::uint8_t>>(
            std::size_t{32}, std::uint8_t{0xAB});
        const ImageU8 wrapped = ImageU8::wrap(PortType::Gray8, 8, 4, 8, buffer);
        RIN_CHECK(wrapped.valid());
        RIN_CHECK_EQ(wrapped.format(), PortType::Gray8);
        RIN_CHECK_EQ(wrapped.width(), std::uint32_t{8});
        RIN_CHECK_EQ(wrapped.height(), std::uint32_t{4});
        RIN_CHECK_EQ(wrapped.stride(), std::uint32_t{8});
        RIN_CHECK_EQ(wrapped.byteSize(), std::uint64_t{32});
        RIN_CHECK(wrapped.pixels() == buffer);
        RIN_CHECK_EQ(wrapped.pixels()->at(5), std::uint8_t{0xAB});

        // 超尺寸缓冲合法（契约：pixels->size() < stride×height 才拒绝）。
        auto oversized = std::make_shared<const std::vector<std::uint8_t>>(
            std::size_t{40}, std::uint8_t{0});
        RIN_CHECK(ImageU8::wrap(PortType::Gray8, 8, 4, 8, oversized).valid());

        // stride=0 按最小行宽解释（与 make 共用元数据校验）。
        auto exact = std::make_shared<const std::vector<std::uint8_t>>(std::size_t{32},
                                                                       std::uint8_t{0});
        const ImageU8 minStride = ImageU8::wrap(PortType::Gray8, 8, 4, 0, exact);
        RIN_CHECK(minStride.valid());
        RIN_CHECK_EQ(minStride.stride(), std::uint32_t{8});

        // 负例：null 缓冲 / 元数据非法 / 缓冲短于 stride×height。
        RIN_CHECK(!ImageU8::wrap(PortType::Gray8, 8, 4, 8, nullptr).valid());
        RIN_CHECK(!ImageU8::wrap(PortType::Gray8, 0, 4, 0, exact).valid());
        RIN_CHECK(!ImageU8::wrap(static_cast<PortType>(99), 8, 4, 8, exact).valid());
        RIN_CHECK(!ImageU8::wrap(PortType::Gray8, 8, 4, 9, exact).valid());
        auto shortBuffer = std::make_shared<const std::vector<std::uint8_t>>(std::size_t{31},
                                                                             std::uint8_t{0});
        RIN_CHECK(!ImageU8::wrap(PortType::Gray8, 8, 4, 8, shortBuffer).valid());
        // 逐字节边界：size == stride×height 恰好合法。
        RIN_CHECK(ImageU8::wrap(PortType::Gray8, 8, 4, 8, exact).valid());
    }

    // --- 5) 默认态与 row/byteSize ---
    {
        // 默认构造：无效空图像占位——valid=false、byteSize=0、row 恒 nullptr、
        // pixels 空指针、元数据清零。
        const ImageU8 empty;
        RIN_CHECK(!empty.valid());
        RIN_CHECK_EQ(empty.byteSize(), std::uint64_t{0});
        RIN_CHECK(empty.pixels() == nullptr);
        RIN_CHECK(empty.row(0) == nullptr);
        RIN_CHECK(empty.row(100) == nullptr);
        RIN_CHECK_EQ(empty.width(), std::uint32_t{0});
        RIN_CHECK_EQ(empty.height(), std::uint32_t{0});
        // 无效图像（make 失败返回值）同态。
        const ImageU8 failed = ImageU8::make(PortType::Gray8, 0, 5);
        RIN_CHECK(!failed.valid());
        RIN_CHECK(failed.row(0) == nullptr);
    }
    {
        // 行指针：row(y) = 基址 + stride×y；y ≥ height 越界 nullptr；padding
        // 下相邻行相差 stride 而非最小行宽。
        const ImageU8 padded = ImageU8::make(PortType::Gray8, 4, 3, 12);
        RIN_CHECK(padded.valid());
        const std::uint8_t* base = padded.pixels()->data();
        RIN_CHECK(padded.row(0) == base);
        RIN_CHECK(padded.row(1) == base + 12);
        RIN_CHECK(padded.row(2) == base + 24);
        RIN_CHECK(padded.row(3) == nullptr);
        RIN_CHECK(padded.row(0xFFFFFFFFu) == nullptr);
        // 无 padding 图像行距 = 最小行宽。
        const ImageU8 tight = ImageU8::make(PortType::Rgba8, 4, 3);
        const std::uint8_t* tightBase = tight.pixels()->data();
        RIN_CHECK(tight.row(0) == tightBase);
        RIN_CHECK(tight.row(2) == tightBase + 16 * 2);
        RIN_CHECK(tight.row(3) == nullptr);
    }

    // --- 6) 值语义：拷贝共享、移动转移、const 不可变 ---
    {
        const ImageU8 original = ImageU8::make(PortType::Rgba8, 2, 2);
        RIN_CHECK(original.valid());
        const std::size_t countBefore = original.pixels().use_count();

        // 拷贝共享像素（引用计数 + 缓冲对象同一性）；从可变副本移动只转移所有权。
        ImageU8 copy = original;
        RIN_CHECK(copy.valid());
        RIN_CHECK(copy.pixels() == original.pixels());
        RIN_CHECK_EQ(copy.pixels().get(), original.pixels().get());
        RIN_CHECK_EQ(copy.pixels().use_count(), countBefore + 1);

        const ImageU8 moved = std::move(copy);
        RIN_CHECK(moved.valid());
        RIN_CHECK(moved.pixels() == original.pixels());
        RIN_CHECK_EQ(moved.pixels().use_count(), countBefore + 1);
        RIN_CHECK_EQ(moved.width(), std::uint32_t{2});
        RIN_CHECK_EQ(moved.stride(), std::uint32_t{8});

        // 缓冲对象唯一（两次独立 make 不共享）。
        const ImageU8 other = ImageU8::make(PortType::Rgba8, 2, 2);
        RIN_CHECK(other.pixels() != original.pixels());
    }

    // --- 7) 参数模型：effectiveParamValue 三态 ---
    {
        const NodeDescriptor descriptor = makeTypedDescriptor();
        const NodeInstance unassigned;  // 无任何赋值。

        // 未赋值：取声明默认值；指针指向声明内默认值（无拷贝）。
        RIN_CHECK(effectiveParamValue(descriptor, unassigned, "radius") ==
                  &descriptor.params[1].defaultValue);
        RIN_CHECK(paramInteger(descriptor, unassigned, "radius") ==
                  std::optional<std::int64_t>(7));

        // 已赋值：取实例值；指针指向赋值存储。
        NodeInstance assigned;
        assigned.params = {assign("sigma", 2.0)};
        const ParamValue* sigma = effectiveParamValue(descriptor, assigned, "sigma");
        RIN_CHECK(sigma == &assigned.params[0].value);
        RIN_CHECK(paramReal(descriptor, assigned, "sigma") == std::optional<double>(2.0));
        // 其余参数仍取默认值。
        RIN_CHECK(paramInteger(descriptor, assigned, "radius") ==
                  std::optional<std::int64_t>(7));

        // 多条赋值（图校验拒绝重复，此处防御语义）：取最后一条。
        NodeInstance duplicated;
        duplicated.params = {assign("radius", static_cast<std::int64_t>(1)),
                             assign("radius", static_cast<std::int64_t>(9))};
        RIN_CHECK(paramInteger(descriptor, duplicated, "radius") ==
                  std::optional<std::int64_t>(9));

        // 未声明 paramId：effectiveParamValue 返回 nullptr。
        RIN_CHECK(effectiveParamValue(descriptor, unassigned, "nope") == nullptr);
        // 赋值到未声明 paramId：仍按声明门控返回 nullptr（不读实例孤儿赋值）。
        NodeInstance ghost;
        ghost.params = {assign("ghost", true)};
        RIN_CHECK(effectiveParamValue(descriptor, ghost, "ghost") == nullptr);
        RIN_CHECK(paramBoolean(descriptor, ghost, "ghost") == std::nullopt);
    }

    // --- 8) 参数模型：五种类型化读取 ---
    {
        const NodeDescriptor descriptor = makeTypedDescriptor();
        const NodeInstance unassigned;

        // kind 匹配：取声明默认值的拷贝。
        RIN_CHECK(paramBoolean(descriptor, unassigned, "enable") == std::optional<bool>(true));
        RIN_CHECK(paramInteger(descriptor, unassigned, "radius") ==
                  std::optional<std::int64_t>(7));
        RIN_CHECK(paramReal(descriptor, unassigned, "sigma") == std::optional<double>(1.5));
        RIN_CHECK(paramEnumeration(descriptor, unassigned, "mode") ==
                  std::optional<std::string>("fast"));
        const std::optional<std::vector<double>> kernel =
            paramRealArray(descriptor, unassigned, "kernel");
        RIN_CHECK(kernel.has_value());
        RIN_CHECK_EQ(kernel->size(), std::size_t{3});
        RIN_CHECK_EQ((*kernel)[0], 0.25);
        RIN_CHECK_EQ((*kernel)[1], 0.5);
        RIN_CHECK_EQ((*kernel)[2], 0.25);

        // kind 不符（声明种类 vs 读取种类逐错位组合）：nullopt。
        RIN_CHECK(paramBoolean(descriptor, unassigned, "radius") == std::nullopt);
        RIN_CHECK(paramInteger(descriptor, unassigned, "sigma") == std::nullopt);
        RIN_CHECK(paramReal(descriptor, unassigned, "enable") == std::nullopt);
        RIN_CHECK(paramEnumeration(descriptor, unassigned, "sigma") == std::nullopt);
        RIN_CHECK(paramRealArray(descriptor, unassigned, "mode") == std::nullopt);
        RIN_CHECK(paramInteger(descriptor, unassigned, "enable") == std::nullopt);
        RIN_CHECK(paramBoolean(descriptor, unassigned, "kernel") == std::nullopt);

        // 未声明 paramId：所有读取返回 nullopt。
        RIN_CHECK(paramBoolean(descriptor, unassigned, "nope") == std::nullopt);
        RIN_CHECK(paramInteger(descriptor, unassigned, "nope") == std::nullopt);
        RIN_CHECK(paramReal(descriptor, unassigned, "nope") == std::nullopt);
        RIN_CHECK(paramEnumeration(descriptor, unassigned, "nope") == std::nullopt);
        RIN_CHECK(paramRealArray(descriptor, unassigned, "nope") == std::nullopt);

        // 运行期直接构造的种类错位赋值（防御路径）：按值的实际种类判定，
        // 与声明 kind 不符返回 nullopt，不得误读。
        NodeInstance mismatched;
        mismatched.params = {assign("sigma", static_cast<std::int64_t>(5)),
                             assign("radius", 2.5),
                             assign("enable", std::string("true"))};
        RIN_CHECK(paramReal(descriptor, mismatched, "sigma") == std::nullopt);
        RIN_CHECK(paramInteger(descriptor, mismatched, "radius") == std::nullopt);
        RIN_CHECK(paramBoolean(descriptor, mismatched, "enable") == std::nullopt);

        // 实例赋值正确种类时逐 kind 覆盖默认值。
        NodeInstance assigned;
        assigned.params = {
            assign("enable", false),
            assign("radius", static_cast<std::int64_t>(42)),
            assign("sigma", 0.25),
            assign("mode", std::string("quality")),
            assign("kernel", std::vector<double>{1, 0, 0, 0, 1, 0, 0, 0, 1}),
        };
        RIN_CHECK(paramBoolean(descriptor, assigned, "enable") == std::optional<bool>(false));
        RIN_CHECK(paramInteger(descriptor, assigned, "radius") ==
                  std::optional<std::int64_t>(42));
        RIN_CHECK(paramReal(descriptor, assigned, "sigma") == std::optional<double>(0.25));
        RIN_CHECK(paramEnumeration(descriptor, assigned, "mode") ==
                  std::optional<std::string>("quality"));
        const std::optional<std::vector<double>> identity =
            paramRealArray(descriptor, assigned, "kernel");
        RIN_CHECK(identity.has_value());
        RIN_CHECK_EQ(identity->size(), std::size_t{9});
        RIN_CHECK_EQ((*identity)[4], 1.0);
        RIN_CHECK_EQ((*identity)[1], 0.0);

        // 读取是拷贝：读取后改写实例赋值不影响已取出的值。
        const std::optional<std::int64_t> snapshot = paramInteger(descriptor, assigned, "radius");
        assigned.params[1].value = static_cast<std::int64_t>(43);
        RIN_CHECK(snapshot == std::optional<std::int64_t>(42));
    }

    return rin_test::exitStatus();
}
