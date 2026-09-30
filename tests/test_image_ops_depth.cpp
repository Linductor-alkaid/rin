// M10 深度域节点 golden 数值测试（独立验证，Independent-Verification-Agent）：
// include/rin/workflow_types.hpp（PortType::Depth32F）、include/rin/image_types.hpp
// （depthF32Row）、src/core/image_ops.cpp（M10 深度节点）、
// src/workflow/default_catalog.hpp（M10 目录条目）。数值语义以
// docs/design/depth_policy_preproc_design.md §5（M8 冻结公式，O1-O5/O7）与
// docs/decisions/DEC-020-workflow-depth-domain.md 为唯一判据；本文件 golden
// 期望全部由冻结公式独立手推/测试内 double 参考实现交叉推导，不参考实现代码。
//
// 被测面与范围：
// 1) 工厂 makeDefaultImageNode 全分支（真实默认目录）：source_depth_metric →
//    nullptr（注入型）；depth_fill_invalid / depth_resize / depth_crop /
//    depth_gaussian_blur / depth_normalize → 实现（descriptor().typeId 一致，
//    dynamic_cast<IStatefulImageNode> == nullptr）；depth_history → 实现且
//    dynamic_cast<IStatefulImageNode> != nullptr（DEC-020 决策 3 标记接口）；
//    未知 typeId 抛 std::invalid_argument；
// 2) O1 fill：NaN/+Inf/-Inf/0/负值/正常混合逐像素 golden；invalid_below 恰等
//    （≤ 边界）与上方保留；16×12 伪随机混合图与测试内 double 参考交叉 ≤1e-6；
//    输入显式 stride > width×4（padding 哨兵字节）不泄漏、输出紧凑
//    （stride = width×4、byteSize = width×height×4）；输入格式不符（Gray8/
//    Rgba8）/无效帧/输入数量错位 apply 抛；
// 3) O2 resize：2×2→1×1 手推（恰 1/4 权重）；3×2→2×1 分数覆盖手推（列 1 权重
//    0.5/1.0）；848×480→64×36 与测试内 INTER_AREA double 参考交叉 ≤1e-6；同尺寸
//    恒等（含 padding 输入）；放大（宽/高任一）与零尺寸目标 apply 抛；
// 4) O3 crop：64×36 (18,0,16,16)→32×18 逐像素索引恒等检查（out(y,x) ==
//    in(y+18, x+16)，无插值）；退化（up+down ≥ H / left+right ≥ W）apply 抛；
// 5) O4 blur：σ=0 δ 核逐位恒等；常值图恒等；radius=1 σ=1 核系数经内部脉冲响应
//    读出与解析式 exp(−(i−r)²/2σ²) 归一化互证（2D = 外积）；reflect-101 角点
//    脉冲折返手推（corner = k1²、邻边 = k0·k1、总能量 = 1）；radius/sigma
//    越界（绕过图准入直接构造）apply 抛；
// 6) O5 normalize：双侧 clip（<near→0、>far→1）+ 区间内恒比（1.25∈[0,2.5]→0.5
//    精确）；非零 near；near == far / near > far apply 抛；
// 7) 容器换装纪律：全部节点输出 format == Depth32F、紧凑 stride、byteSize 一致、
//    pixels 缓冲字节数 == width×height×4；Depth32F ImageU8 make/wrap 最小行宽
//    拒绝；depthF32Row 有效性（越界/格式不符/无效图 → nullptr）；
// 8) 参数纪律（运行期直接构造，绕过图准入）：参数缺失（声明被清空）/种类错位
//    （Real 位赋 Integer、Integer 位赋 Real/String）构造抛 std::invalid_argument；
// 9) 图集成（真实默认目录 + makeDefaultImageNode + runNodeGraph + 注入器）：
//    source_depth_metric → fill → resize → crop → blur → normalize → history
//    全链；848×480 常量 1.25 m 注入两帧：中间产物逐级核对（fill 全 1.25 →
//    resize 64×36 → crop 32×18 → blur ≈1.25 → normalize 全 0.5 精确）；history
//    输出 32×144（8 平面 × 18 行）竖直堆叠，第一帧欠帧填充全 0.5（M8 O7 空环
//    首帧语义），第二帧按 idx = {1,6,…,31,36} 抽样 = 7 平面首帧 0.5 + 底平面
//    第二帧 0.8（(2.0−0)/2.5 精确）。
//
// golden 独立性说明：小图 golden 以字面值写出并附手推注释（只依赖 §5 冻结
// 公式）；大图用例由测试内自写 double 参考实现交叉验证（按冻结公式重写，不
// include 实现头）。
//
// DOD-02 适用性说明：本契约面全部为单线程纯逻辑（节点构造/apply/图编译求值均
// 顺序调用），并发矩阵不适用；引擎串行在飞/状态时序归
// test_workflow_depth_engine.cpp。
#include "test_util.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <rin/image_node.hpp>
#include <rin/image_ops.hpp>
#include <rin/image_types.hpp>
#include <rin/node_graph.hpp>
#include <rin/workflow_types.hpp>

#include "default_catalog.hpp"

namespace {

using rin::Connection;
using rin::IImageNode;
using rin::ImageNodeFactory;
using rin::ImageU8;
using rin::IStatefulImageNode;
using rin::NodeCatalog;
using rin::NodeDescriptor;
using rin::NodeGraph;
using rin::NodeGraphBuild;
using rin::NodeId;
using rin::NodeInstance;
using rin::ParamAssignment;
using rin::ParamValue;
using rin::PortDirection;
using rin::PortRef;
using rin::PortType;
using rin::WorkflowGraph;

// --- 基础夹具 ---------------------------------------------------------------

/// 目录按契约运行期不变；测试进程内静态缓存（值语义只读）。
const NodeDescriptor& catalogEntry(const std::string& typeId) {
    static const NodeCatalog frozen = rin::workflow_catalog::makeDefaultImageNodeCatalog();
    const NodeDescriptor* found = rin::findNodeDescriptor(frozen, typeId);
    if (found == nullptr) {
        std::printf("FAIL catalog missing typeId %s\n", typeId.c_str());
        std::exit(1);
    }
    return *found;
}

/// 按目录构造实例（可覆盖参数赋值；未列出的参数取声明默认值）。
std::unique_ptr<IImageNode> makeNode(const std::string& typeId,
                                     std::vector<ParamAssignment> params = {}) {
    NodeInstance instance;
    instance.id = 1;
    instance.typeId = typeId;
    instance.params = std::move(params);
    return rin::makeDefaultImageNode(catalogEntry(typeId), instance);
}

/// 声明被清空的目录项（参数缺失见证：运行期直接构造绕过图准入）。
NodeDescriptor strippedDescriptor(const std::string& typeId) {
    NodeDescriptor descriptor = catalogEntry(typeId);
    descriptor.params.clear();
    return descriptor;
}

std::unique_ptr<IImageNode> makeNodeRaw(const NodeDescriptor& descriptor,
                                        std::vector<ParamAssignment> params = {}) {
    NodeInstance instance;
    instance.id = 1;
    instance.typeId = descriptor.typeId;
    instance.params = std::move(params);
    return rin::makeDefaultImageNode(descriptor, instance);
}

/// 值填充的 Depth32F 图（紧凑 stride；values 行主序，尺寸必须吻合）。
ImageU8 depthImage(std::uint32_t width, std::uint32_t height, const std::vector<float>& values) {
    std::vector<std::uint8_t> bytes(values.size() * 4u);
    if (!values.empty()) {
        std::memcpy(bytes.data(), values.data(), bytes.size());
    }
    return ImageU8::wrap(PortType::Depth32F, width, height, width * 4u,
                         std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes)));
}

/// 带行尾 padding 的 Depth32F 图（padding 字节写入哨兵值；行宽 stride >= width×4）。
ImageU8 depthImagePadded(std::uint32_t width, std::uint32_t height,
                         const std::vector<float>& values, std::uint32_t strideBytes,
                         std::uint8_t padSentinel) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(strideBytes) * height, padSentinel);
    for (std::uint32_t y = 0; y < height; ++y) {
        std::memcpy(bytes.data() + static_cast<std::size_t>(strideBytes) * y,
                    values.data() + static_cast<std::size_t>(y) * width,
                    static_cast<std::size_t>(width) * 4u);
    }
    return ImageU8::wrap(PortType::Depth32F, width, height, strideBytes,
                         std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes)));
}

std::vector<float> floatsOf(const ImageU8& image) {
    std::vector<float> out(static_cast<std::size_t>(image.width()) * image.height());
    for (std::uint32_t y = 0; y < image.height(); ++y) {
        const float* row = rin::depthF32Row(image, y);
        if (row == nullptr) {
            RIN_CHECK_MSG(false, "depthF32Row unexpectedly null");
            return {};
        }
        std::copy_n(row, image.width(), out.begin() + static_cast<std::size_t>(y) * image.width());
    }
    return out;
}

void expectCompactDepth32F(const char* where, const ImageU8& image, std::uint32_t width,
                           std::uint32_t height) {
    RIN_CHECK_MSG(image.valid(), (std::string(where) + ": output invalid").c_str());
    RIN_CHECK_MSG(image.format() == PortType::Depth32F,
                  (std::string(where) + ": format != Depth32F").c_str());
    RIN_CHECK_MSG(image.width() == width, (std::string(where) + ": width mismatch").c_str());
    RIN_CHECK_MSG(image.height() == height, (std::string(where) + ": height mismatch").c_str());
    RIN_CHECK_MSG(image.stride() == width * 4u,
                  (std::string(where) + ": stride not compact").c_str());
    RIN_CHECK_MSG(image.byteSize() == static_cast<std::uint64_t>(width) * height * 4u,
                  (std::string(where) + ": byteSize mismatch").c_str());
    RIN_CHECK_MSG(image.pixels() != nullptr && image.pixels()->size() == image.byteSize(),
                  (std::string(where) + ": pixel buffer size mismatch").c_str());
}

bool nearlyEqual(float a, float b, double tolerance) {
    return std::fabs(static_cast<double>(a) - static_cast<double>(b)) <= tolerance;
}

void expectAll(const char* where, const ImageU8& image, float expected, double tolerance) {
    const std::vector<float> values = floatsOf(image);
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!nearlyEqual(values[i], expected, tolerance)) {
            RIN_CHECK_MSG(false,
                          (std::string(where) + ": pixel " + std::to_string(i) + " = " +
                           std::to_string(values[i]) + ", expected " + std::to_string(expected))
                              .c_str());
            return;
        }
    }
    RIN_CHECK(true);
}

/// 安全调用（预期异常路径用）：契约外异常记失败不 abort（文件头纪律）。
template <typename Fn>
bool throwsInvalidArgument(Fn&& fn) {
    try {
        fn();
    } catch (const std::invalid_argument&) {
        return true;
    } catch (const std::exception& error) {
        RIN_CHECK_MSG(false, (std::string("unexpected exception type: ") + error.what()).c_str());
        return false;
    } catch (...) {
        RIN_CHECK_MSG(false, "unexpected non-standard exception");
        return false;
    }
    return false;
}

// --- 测试内 double 参考（按 §5 冻结公式重写；不 include 实现头） -------------

double refFill(double v, double farValue, double invalidBelow) {
    return (!std::isfinite(v) || v <= invalidBelow) ? farValue : v;
}

/// O2 INTER_AREA 面积加权（src → dst 仅缩小；§5 O2 公式）。
double refResizeArea(const std::vector<double>& src, std::uint32_t srcW, std::uint32_t srcH,
                     std::uint32_t dstW, std::uint32_t dstH, std::uint32_t dx, std::uint32_t dy) {
    const double sx = static_cast<double>(srcW) / dstW;
    const double sy = static_cast<double>(srcH) / dstH;
    double sum = 0.0;
    for (std::uint32_t i = 0; i < srcW; ++i) {
        const double lo = std::max(static_cast<double>(i), static_cast<double>(dx) * sx);
        const double hi = std::min(static_cast<double>(i + 1), static_cast<double>(dx + 1) * sx);
        const double w = std::max(0.0, hi - lo);
        if (w == 0.0) {
            continue;
        }
        for (std::uint32_t j = 0; j < srcH; ++j) {
            const double yLo = std::max(static_cast<double>(j), static_cast<double>(dy) * sy);
            const double yHi =
                std::min(static_cast<double>(j + 1), static_cast<double>(dy + 1) * sy);
            const double h = std::max(0.0, yHi - yLo);
            if (h == 0.0) {
                continue;
            }
            sum += w * h * src[static_cast<std::size_t>(j) * srcW + i];
        }
    }
    return sum / (sx * sy);
}

}  // namespace

int main() {
    // ---- 1) 工厂分派全分支（真实默认目录） ----------------------------------
    {
        RIN_CHECK(makeNode("source_depth_metric") == nullptr);  // 注入型源节点。

        const char* operators[5] = {"depth_fill_invalid", "depth_resize", "depth_crop",
                                    "depth_gaussian_blur", "depth_normalize"};
        for (const char* typeId : operators) {
            std::unique_ptr<IImageNode> node = makeNode(typeId);
            RIN_CHECK_MSG(node != nullptr,
                          (std::string(typeId) + ": factory returned null").c_str());
            if (node != nullptr) {
                RIN_CHECK_MSG(node->descriptor().typeId == typeId,
                              (std::string(typeId) + ": descriptor typeId mismatch").c_str());
                RIN_CHECK_MSG(dynamic_cast<const IStatefulImageNode*>(node.get()) == nullptr,
                              (std::string(typeId) + ": must not be stateful").c_str());
            }
        }

        // depth_history：实现 + IStatefulImageNode 标记（DEC-020 决策 3）。
        {
            std::unique_ptr<IImageNode> node = makeNode("depth_history");
            RIN_CHECK(node != nullptr);
            if (node != nullptr) {
                RIN_CHECK(node->descriptor().typeId == "depth_history");
                RIN_CHECK(dynamic_cast<const IStatefulImageNode*>(node.get()) != nullptr);
            }
        }

        // 未知类型：显式抛 invalid_argument（不静默）。工厂契约以描述符为输入，
        // 未知 typeId 用手工描述符见证（目录查找本就拒绝未知类型）。
        RIN_CHECK(throwsInvalidArgument([] {
            NodeDescriptor unknown;
            unknown.typeId = "depth_history_nonexistent";
            unknown.inputs = {PortType::Depth32F};
            unknown.outputs = {PortType::Depth32F};
            NodeInstance instance;
            instance.id = 1;
            instance.typeId = unknown.typeId;
            return rin::makeDefaultImageNode(unknown, instance) != nullptr;
        }));
        RIN_CHECK(throwsInvalidArgument([] {
            NodeDescriptor unknown;
            unknown.typeId = "no_such_type";
            NodeInstance instance;
            instance.id = 1;
            instance.typeId = unknown.typeId;
            return rin::makeDefaultImageNode(unknown, instance) != nullptr;
        }));
    }

    // ---- 2) O1 fill：混合无效值 golden + invalid_below 边界 + padding 纪律 ---
    {
        std::unique_ptr<IImageNode> node = makeNode("depth_fill_invalid");
        RIN_CHECK(node != nullptr);
        if (node == nullptr) {
            return rin_test::exitStatus();
        }

        // 手推（far=2.5, below=0）：NaN/+Inf/-Inf/0 → 2.5；-0.5（≤0）→ 2.5；
        // 1.25 / 0.75 / 2.5 保留。
        const std::vector<float> input = {std::numeric_limits<float>::quiet_NaN(),
                                          std::numeric_limits<float>::infinity(),
                                          -std::numeric_limits<float>::infinity(),
                                          0.0f,
                                          -0.5f,
                                          1.25f,
                                          2.5f,
                                          0.75f};
        const std::vector<float> expected = {2.5f, 2.5f, 2.5f, 2.5f, 2.5f, 1.25f, 2.5f, 0.75f};
        std::vector<ImageU8> out = node->apply({depthImage(4, 2, input)});
        RIN_CHECK_EQ(out.size(), std::size_t{1});
        expectCompactDepth32F("fill", out[0], 4, 2);
        const std::vector<float> got = floatsOf(out[0]);
        for (std::size_t i = 0; i < expected.size(); ++i) {
            // NaN 输入已被替换，逐位比较安全。
            RIN_CHECK_MSG(std::memcmp(&got[i], &expected[i], 4) == 0,
                          ("fill: pixel " + std::to_string(i)).c_str());
        }

        // invalid_below 恰等边界（≤ 语义）：v == below → 填充；略高于 → 保留。
        {
            std::vector<ParamAssignment> params = {{"far_value", 1.0}, {"invalid_below", 0.5}};
            std::unique_ptr<IImageNode> below = makeNode("depth_fill_invalid", std::move(params));
            const std::vector<float> in = {0.5f, 0.5f + 1e-6f, -0.25f};
            std::vector<ImageU8> outBelow = below->apply({depthImage(3, 1, in)});
            const std::vector<float> gotBelow = floatsOf(outBelow[0]);
            RIN_CHECK_EQ(gotBelow[0], 1.0f);  // 恰等 → 填充。
            RIN_CHECK_EQ(gotBelow[1], 0.5f + 1e-6f);
            RIN_CHECK_EQ(gotBelow[2], 1.0f);
        }

        // 16×12 伪随机混合图 vs double 参考（≤1e-6）。
        {
            std::vector<float> pattern;
            pattern.reserve(16u * 12u);
            double seed = 0.123456789;
            for (std::uint32_t i = 0; i < 16u * 12u; ++i) {
                seed = std::fmod(seed * 16807.0, 2147483647.0);
                const double u = seed / 2147483647.0;
                // 混合：约 1/3 无效（0 / 负 / 非有限），其余 [0, 3) 正常值。
                if (i % 7 == 0) {
                    pattern.push_back(0.0f);
                } else if (i % 11 == 0) {
                    pattern.push_back(-static_cast<float>(u));
                } else if (i % 23 == 0) {
                    pattern.push_back(std::numeric_limits<float>::quiet_NaN());
                } else {
                    pattern.push_back(static_cast<float>(u * 3.0));
                }
            }
            std::vector<ImageU8> outRef = node->apply({depthImage(16, 12, pattern)});
            const std::vector<float> gotRef = floatsOf(outRef[0]);
            for (std::uint32_t i = 0; i < 16u * 12u; ++i) {
                const double ref = refFill(pattern[i], 2.5, 0.0);
                if (!nearlyEqual(gotRef[i], static_cast<float>(ref), 1e-6)) {
                    RIN_CHECK_MSG(false, ("fill ref: pixel " + std::to_string(i)).c_str());
                    break;
                }
            }
            RIN_CHECK(true);
        }

        // 容器换装纪律：显式 stride（4+3 像素 = 28 字节）+ padding 哨兵不泄漏。
        {
            const std::uint32_t strideBytes = 7u * 4u;
            ImageU8 padded = depthImagePadded(4, 2, input, strideBytes, 0xDE);
            RIN_CHECK(padded.valid());
            RIN_CHECK_EQ(padded.stride(), strideBytes);
            RIN_CHECK_EQ(padded.byteSize(), std::uint64_t{strideBytes} * 2u);
            std::vector<ImageU8> outPadded = node->apply({padded});
            expectCompactDepth32F("fill padded", outPadded[0], 4, 2);
            const std::vector<float> gotPadded = floatsOf(outPadded[0]);
            for (std::size_t i = 0; i < expected.size(); ++i) {
                RIN_CHECK_MSG(std::memcmp(&gotPadded[i], &expected[i], 4) == 0,
                              ("fill padded: pixel " + std::to_string(i)).c_str());
            }
        }

        // 防御：格式不符 / 无效帧 / 输入数量错位 → apply 抛 invalid_argument。
        {
            ImageU8 gray = ImageU8::make(PortType::Gray8, 4, 2);
            RIN_CHECK(throwsInvalidArgument([&] { (void)node->apply({gray}); }));
            ImageU8 rgba = ImageU8::make(PortType::Rgba8, 4, 2);
            RIN_CHECK(throwsInvalidArgument([&] { (void)node->apply({rgba}); }));
            ImageU8 invalid;
            RIN_CHECK(throwsInvalidArgument([&] { (void)node->apply({invalid}); }));
            RIN_CHECK(throwsInvalidArgument(
                [&] { (void)node->apply({depthImage(4, 2, input), depthImage(4, 2, input)}); }));
            RIN_CHECK(throwsInvalidArgument([&] { (void)node->apply({}); }));
        }
    }

    // ---- 3) O2 resize --------------------------------------------------------
    {
        std::unique_ptr<IImageNode> node = makeNode("depth_resize");
        RIN_CHECK(node != nullptr);

        // 2×2 → 1×1：每个源像素恰覆盖 1/4；(1+2+3+6)/4 = 3。
        {
            std::vector<ParamAssignment> params = {{"width", std::int64_t{1}},
                                                   {"height", std::int64_t{1}}};
            std::unique_ptr<IImageNode> tiny = makeNode("depth_resize", std::move(params));
            std::vector<ImageU8> out = tiny->apply({depthImage(2, 2, {1.0f, 2.0f, 3.0f, 6.0f})});
            expectCompactDepth32F("resize 2x2", out[0], 1, 1);
            RIN_CHECK_EQ(floatsOf(out[0])[0], 3.0f);
        }

        // 3×2 → 2×1 分数覆盖手推（sx=1.5, sy=2.0）：行覆盖区间 [0,2) 与像素行
        // [0,1)/[1,2) 各相交长度 1（h=1）；列 0 覆盖 [0,1.5)（w=1 与 w=0.5）。
        // out(0) = (1·3 + 0.5·6 + 1·0 + 0.5·3)/3 = 7.5/3 = 2.5；
        // out(1) = (0.5·6 + 1·9 + 0.5·3 + 1·3)/3 = 16.5/3 = 5.5。
        {
            std::vector<ParamAssignment> params = {{"width", std::int64_t{2}},
                                                   {"height", std::int64_t{1}}};
            std::unique_ptr<IImageNode> frac = makeNode("depth_resize", std::move(params));
            // 值按 (x,y)：v(0,0)=3 v(1,0)=6 v(2,0)=9 / v(0,1)=0 v(1,1)=3 v(2,1)=3。
            std::vector<ImageU8> out =
                frac->apply({depthImage(3, 2, {3.0f, 6.0f, 9.0f, 0.0f, 3.0f, 3.0f})});
            expectCompactDepth32F("resize 3x2", out[0], 2, 1);
            const std::vector<float> got = floatsOf(out[0]);
            RIN_CHECK_MSG(nearlyEqual(got[0], 2.5f, 1e-6), "resize 3x2: out(0)");
            RIN_CHECK_MSG(nearlyEqual(got[1], 5.5f, 1e-6), "resize 3x2: out(1)");
        }

        // 848×480 → 64×36 vs INTER_AREA double 参考（≤1e-6）。
        {
            std::vector<double> pattern(848u * 480u);
            double seed = 0.987654321;
            for (std::size_t i = 0; i < pattern.size(); ++i) {
                seed = std::fmod(seed * 48271.0, 2147483647.0);
                pattern[i] = (i % 13 == 0) ? 0.0 : seed / 2147483647.0 * 2.5;
            }
            std::vector<float> patternF(pattern.size());
            for (std::size_t i = 0; i < pattern.size(); ++i) {
                patternF[i] = static_cast<float>(pattern[i]);
            }
            std::vector<ImageU8> out = node->apply({depthImage(848, 480, patternF)});
            expectCompactDepth32F("resize 848", out[0], 64, 36);
            const std::vector<float> got = floatsOf(out[0]);
            for (std::uint32_t y = 0; y < 36; ++y) {
                for (std::uint32_t x = 0; x < 64; ++x) {
                    const double ref = refResizeArea(pattern, 848, 480, 64, 36, x, y);
                    if (!nearlyEqual(got[static_cast<std::size_t>(y) * 64 + x],
                                     static_cast<float>(ref), 1e-6)) {
                        RIN_CHECK_MSG(false, ("resize 848 ref: (" + std::to_string(x) + "," +
                                              std::to_string(y) + ")")
                                                 .c_str());
                        y = 36;
                        break;
                    }
                }
            }
            RIN_CHECK(true);
        }

        // 同尺寸恒等（默认参数 64×36；padding 输入亦然且输出紧凑）。
        {
            std::vector<float> values(64u * 36u);
            for (std::size_t i = 0; i < values.size(); ++i) {
                values[i] = static_cast<float>(i) * 0.01f;
            }
            std::vector<ImageU8> out = node->apply({depthImage(64, 36, values)});
            expectCompactDepth32F("resize identity", out[0], 64, 36);
            const std::vector<float> got = floatsOf(out[0]);
            RIN_CHECK(std::memcmp(got.data(), values.data(), values.size() * 4u) == 0);

            const std::uint32_t strideBytes = 70u * 4u;
            std::vector<ImageU8> outPadded =
                node->apply({depthImagePadded(64, 36, values, strideBytes, 0xC3)});
            expectCompactDepth32F("resize identity padded", outPadded[0], 64, 36);
            const std::vector<float> gotPadded = floatsOf(outPadded[0]);
            RIN_CHECK(std::memcmp(gotPadded.data(), values.data(), values.size() * 4u) == 0);
        }

        // 放大 / 零尺寸目标 → apply 抛。
        {
            std::vector<ParamAssignment> up = {{"width", std::int64_t{16}},
                                               {"height", std::int64_t{16}}};
            std::unique_ptr<IImageNode> upscale = makeNode("depth_resize", std::move(up));
            RIN_CHECK(throwsInvalidArgument(
                [&] { (void)upscale->apply({depthImage(8, 8, std::vector<float>(64, 1.0f))}); }));

            std::vector<ParamAssignment> upH = {{"width", std::int64_t{8}},
                                                {"height", std::int64_t{16}}};
            std::unique_ptr<IImageNode> upscaleH = makeNode("depth_resize", std::move(upH));
            RIN_CHECK(throwsInvalidArgument(
                [&] { (void)upscaleH->apply({depthImage(8, 8, std::vector<float>(64, 1.0f))}); }));

            std::vector<ParamAssignment> zero = {{"width", std::int64_t{0}},
                                                 {"height", std::int64_t{4}}};
            std::unique_ptr<IImageNode> zeroW = makeNode("depth_resize", std::move(zero));
            RIN_CHECK(throwsInvalidArgument(
                [&] { (void)zeroW->apply({depthImage(8, 8, std::vector<float>(64, 1.0f))}); }));
        }
    }

    // ---- 4) O3 crop：64×36 (18,0,16,16) → 32×18 索引逐位 ---------------------
    {
        std::unique_ptr<IImageNode> node = makeNode("depth_crop");
        RIN_CHECK(node != nullptr);

        std::vector<float> values(64u * 36u);
        for (std::uint32_t y = 0; y < 36; ++y) {
            for (std::uint32_t x = 0; x < 64; ++x) {
                values[static_cast<std::size_t>(y) * 64 + x] =
                    static_cast<float>(x * 100 + y + 1);  // 逐像素唯一（< 2^24 精确）。
            }
        }
        std::vector<ImageU8> out = node->apply({depthImage(64, 36, values)});
        expectCompactDepth32F("crop", out[0], 32, 18);
        const std::vector<float> got = floatsOf(out[0]);
        bool cropOk = true;
        for (std::uint32_t y = 0; y < 18 && cropOk; ++y) {
            for (std::uint32_t x = 0; x < 32; ++x) {
                const float expected = values[static_cast<std::size_t>(y + 18) * 64 + (x + 16)];
                if (std::memcmp(&got[static_cast<std::size_t>(y) * 32 + x], &expected, 4) != 0) {
                    RIN_CHECK_MSG(false, ("crop: (" + std::to_string(x) + "," + std::to_string(y) +
                                          ") != in(" + std::to_string(x + 16) + "," +
                                          std::to_string(y + 18) + ")")
                                             .c_str());
                    cropOk = false;
                    break;
                }
            }
        }
        RIN_CHECK(cropOk);

        // 退化 / 越界 → apply 抛（§5 O3：up+down ≥ H 或 left+right ≥ W）。
        {
            const auto cropThrows = [&](std::int64_t up, std::int64_t down, std::int64_t left,
                                        std::int64_t right) {
                std::vector<ParamAssignment> params = {
                    {"up", up}, {"down", down}, {"left", left}, {"right", right}};
                std::unique_ptr<IImageNode> bad = makeNode("depth_crop", std::move(params));
                return throwsInvalidArgument(
                    [&] { (void)bad->apply({depthImage(64, 36, values)}); });
            };
            RIN_CHECK(cropThrows(18, 18, 0, 0));  // up+down == 36 == H。
            RIN_CHECK(cropThrows(0, 37, 0, 0));   // up+down > H。
            RIN_CHECK(cropThrows(0, 0, 16, 48));  // left+right == 64 == W。
            RIN_CHECK(cropThrows(0, 0, 50, 20));  // left+right > W。
            RIN_CHECK(cropThrows(36, 0, 0, 0));  // 全图裁切拒绝（与 M4 crop 语义分叉）。
        }
    }

    // ---- 5) O4 blur ----------------------------------------------------------
    {
        std::unique_ptr<IImageNode> node = makeNode("depth_gaussian_blur");
        RIN_CHECK(node != nullptr);

        // σ=0：δ 核恒等（逐位）。
        {
            std::vector<float> values;
            values.reserve(5u * 4u);
            for (std::uint32_t i = 0; i < 5u * 4u; ++i) {
                values.push_back(static_cast<float>(i % 7) * 0.5f);
            }
            std::vector<ParamAssignment> params = {{"radius", std::int64_t{2}}, {"sigma", 0.0}};
            std::unique_ptr<IImageNode> delta = makeNode("depth_gaussian_blur", std::move(params));
            std::vector<ImageU8> out = delta->apply({depthImage(5, 4, values)});
            expectCompactDepth32F("blur sigma0", out[0], 5, 4);
            const std::vector<float> got = floatsOf(out[0]);
            RIN_CHECK(std::memcmp(got.data(), values.data(), values.size() * 4u) == 0);
        }

        // 常值图恒等（Σk = 1）。
        {
            std::vector<ImageU8> out =
                node->apply({depthImage(6, 5, std::vector<float>(30, 1.25f))});
            expectAll("blur const", out[0], 1.25f, 1e-6);
        }

        // 核系数互证：内部脉冲响应 = 解析核外积（radius=1, σ=1）。
        {
            const double k0 = std::exp(-0.5) / (1.0 + 2.0 * std::exp(-0.5));
            const double k1 = 1.0 / (1.0 + 2.0 * std::exp(-0.5));
            std::vector<float> impulse(25, 0.0f);
            impulse[2 * 5 + 2] = 1.0f;  // (x=2, y=2)，5×5 内部（reflect 不触及）。
            std::vector<ImageU8> out = node->apply({depthImage(5, 5, impulse)});
            expectCompactDepth32F("blur impulse", out[0], 5, 5);
            const std::vector<float> got = floatsOf(out[0]);
            bool kernelOk = true;
            for (std::uint32_t y = 0; y < 5 && kernelOk; ++y) {
                for (std::uint32_t x = 0; x < 5; ++x) {
                    const double wx = x == 2 ? k1 : (x == 1 || x == 3 ? k0 : 0.0);
                    const double wy = y == 2 ? k1 : (y == 1 || y == 3 ? k0 : 0.0);
                    const double expected = wx * wy;
                    if (!nearlyEqual(got[static_cast<std::size_t>(y) * 5 + x],
                                     static_cast<float>(expected), 1e-6)) {
                        RIN_CHECK_MSG(false, ("blur impulse: (" + std::to_string(x) + "," +
                                              std::to_string(y) + ")")
                                                 .c_str());
                        kernelOk = false;
                        break;
                    }
                }
            }
            RIN_CHECK(kernelOk);

            // reflect-101 角点脉冲折返手推（4×4，脉冲在 (0,0)）：
            // H 趟 x=-1→1：h[0][0] = k1；V 趟 y=-1→1：out(0,0) = k1²；
            // out(0,1) = out(1,0) = k0·k1；总能量 = Σk·Σk = 1。
            std::vector<float> corner(16, 0.0f);
            corner[0] = 1.0f;
            std::vector<ImageU8> outCorner = node->apply({depthImage(4, 4, corner)});
            const std::vector<float> gotCorner = floatsOf(outCorner[0]);
            RIN_CHECK_MSG(nearlyEqual(gotCorner[0], static_cast<float>(k1 * k1), 1e-6),
                          "blur reflect: corner (0,0) != k1^2");
            RIN_CHECK_MSG(nearlyEqual(gotCorner[1], static_cast<float>(k0 * k1), 1e-6),
                          "blur reflect: (1,0) != k0*k1");
            RIN_CHECK_MSG(nearlyEqual(gotCorner[4], static_cast<float>(k0 * k1), 1e-6),
                          "blur reflect: (0,1) != k0*k1");
            // reflect-101 对边缘脉冲不保能量（-1→1 折返落在 0 像素上）：非零输出
            // 恰为 {k1², k0·k1, k0·k1, k0²}，总能量 = (k0 + k1)²（手推）。
            double energy = 0.0;
            for (float v : gotCorner) {
                energy += v;
            }
            const double reflectEnergy = (k0 + k1) * (k0 + k1);
            RIN_CHECK_MSG(std::fabs(energy - reflectEnergy) <= 1e-6,
                          "blur reflect: energy != (k0+k1)^2");
        }

        // radius/sigma 越界（绕过图准入直接构造）：apply 抛 invalid_argument。
        {
            const auto blurThrows = [&](std::int64_t radius, double sigma) {
                std::vector<ParamAssignment> params = {{"radius", radius}, {"sigma", sigma}};
                std::unique_ptr<IImageNode> bad =
                    makeNode("depth_gaussian_blur", std::move(params));
                return throwsInvalidArgument(
                    [&] { (void)bad->apply({depthImage(4, 4, std::vector<float>(16, 1.0f))}); });
            };
            RIN_CHECK(blurThrows(0, 1.0));   // radius < 1。
            RIN_CHECK(blurThrows(11, 1.0));  // radius > 10。
            RIN_CHECK(blurThrows(1, -0.5));  // sigma < 0。
            RIN_CHECK(blurThrows(1, 10.5));  // sigma > 10。
            RIN_CHECK(blurThrows(1, std::numeric_limits<double>::infinity()));
        }
    }

    // ---- 6) O5 normalize -----------------------------------------------------
    {
        std::unique_ptr<IImageNode> node = makeNode("depth_normalize");
        RIN_CHECK(node != nullptr);

        // 双侧 clip + 区间内恒比：near=0, far=2.5。
        // 手推：-0.5→0；0→0；1.25→0.5（精确）；2.5→1；3.0→1。
        {
            const std::vector<float> in = {-0.5f, 0.0f, 1.25f, 2.5f, 3.0f};
            std::vector<ImageU8> out = node->apply({depthImage(5, 1, in)});
            expectCompactDepth32F("normalize", out[0], 5, 1);
            const std::vector<float> got = floatsOf(out[0]);
            const float expected[5] = {0.0f, 0.0f, 0.5f, 1.0f, 1.0f};
            for (std::size_t i = 0; i < 5; ++i) {
                RIN_CHECK_MSG(std::memcmp(&got[i], &expected[i], 4) == 0,
                              ("normalize: pixel " + std::to_string(i)).c_str());
            }
        }

        // 非零 near：[0.5, 1.5]。
        {
            std::vector<ParamAssignment> params = {{"near", 0.5}, {"far", 1.5}};
            std::unique_ptr<IImageNode> shifted = makeNode("depth_normalize", std::move(params));
            const std::vector<float> in = {0.0f, 0.5f, 1.0f, 1.5f, 2.0f};
            std::vector<ImageU8> out = shifted->apply({depthImage(5, 1, in)});
            const std::vector<float> got = floatsOf(out[0]);
            const float expected[5] = {0.0f, 0.0f, 0.5f, 1.0f, 1.0f};
            for (std::size_t i = 0; i < 5; ++i) {
                RIN_CHECK_MSG(nearlyEqual(got[i], expected[i], 1e-6),
                              ("normalize shifted: pixel " + std::to_string(i)).c_str());
            }
        }

        // near ≥ far → apply 抛。
        {
            std::vector<ParamAssignment> equal = {{"near", 1.0}, {"far", 1.0}};
            std::unique_ptr<IImageNode> degenerate = makeNode("depth_normalize", std::move(equal));
            RIN_CHECK(throwsInvalidArgument(
                [&] { (void)degenerate->apply({depthImage(2, 1, {1.0f, 2.0f})}); }));

            std::vector<ParamAssignment> inverted = {{"near", 2.0}, {"far", 1.0}};
            std::unique_ptr<IImageNode> reversed = makeNode("depth_normalize", std::move(inverted));
            RIN_CHECK(throwsInvalidArgument(
                [&] { (void)reversed->apply({depthImage(2, 1, {1.0f, 2.0f})}); }));
        }
    }

    // ---- 7) 容器 / depthF32Row / 参数纪律 ------------------------------------
    {
        // elementSize == 4（workflow_types.hpp M10 契约）。
        RIN_CHECK_EQ(rin::elementSize(PortType::Depth32F), 4u);

        // depthF32Row：有效行视图 + 越界/格式不符/无效图 nullptr。
        {
            ImageU8 image = depthImage(3, 2, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f});
            for (std::uint32_t y = 0; y < 2; ++y) {
                const float* row = rin::depthF32Row(image, y);
                RIN_CHECK(row != nullptr);
                if (row != nullptr) {
                    RIN_CHECK_EQ(row[0], y == 0 ? 1.0f : 4.0f);
                    RIN_CHECK_EQ(row[2], y == 0 ? 3.0f : 6.0f);
                }
            }
            RIN_CHECK(rin::depthF32Row(image, 2) == nullptr);  // y == height。
            RIN_CHECK(rin::depthF32Row(image, 99) == nullptr);
            const ImageU8 gray = ImageU8::make(PortType::Gray8, 3, 2);
            RIN_CHECK(rin::depthF32Row(gray, 0) == nullptr);  // 格式不符。
            const ImageU8 invalid;
            RIN_CHECK(rin::depthF32Row(invalid, 0) == nullptr);  // 无效图。
        }

        // make/wrap 最小行宽拒绝（stride 字节数语义）。
        {
            RIN_CHECK(!ImageU8::make(PortType::Depth32F, 4, 2, 4 * 4u - 1).valid());
            RIN_CHECK(ImageU8::make(PortType::Depth32F, 4, 2, 4 * 4u).valid());
            const auto pixels = std::make_shared<const std::vector<std::uint8_t>>(4u * 2u * 4u, 0);
            RIN_CHECK(!ImageU8::wrap(PortType::Depth32F, 4, 2, 4 * 4u - 1, pixels).valid());
            RIN_CHECK(ImageU8::wrap(PortType::Depth32F, 4, 2, 4 * 4u, pixels).valid());
        }

        // 参数缺失（声明被清空 + 无默认可读）构造抛。
        RIN_CHECK(throwsInvalidArgument(
            [&] { return makeNodeRaw(strippedDescriptor("depth_fill_invalid")) != nullptr; }));
        RIN_CHECK(throwsInvalidArgument(
            [&] { return makeNodeRaw(strippedDescriptor("depth_resize")) != nullptr; }));
        RIN_CHECK(throwsInvalidArgument(
            [&] { return makeNodeRaw(strippedDescriptor("depth_history")) != nullptr; }));

        // 种类错位构造抛（Real 位赋 Integer、Integer 位赋 Real/String）。
        RIN_CHECK(throwsInvalidArgument([&] {
            return makeNode("depth_fill_invalid", {{"far_value", std::int64_t{2}}}) != nullptr;
        }));
        RIN_CHECK(throwsInvalidArgument(
            [&] { return makeNode("depth_resize", {{"width", 0.5}}) != nullptr; }));
        RIN_CHECK(throwsInvalidArgument([&] {
            return makeNode("depth_history", {{"history_length", std::string("37")}}) != nullptr;
        }));

        // depth_history 参数约束（构造期，§4/DEC-020 默认链约束）。
        {
            // history_length = 0。
            RIN_CHECK(throwsInvalidArgument([&] {
                return makeNode("depth_history", {{"history_length", std::int64_t{0}},
                                                  {"sample_count", std::int64_t{2}},
                                                  {"sample_skip", std::int64_t{1}},
                                                  {"sample_delay", std::int64_t{0}}}) != nullptr;
            }));
            // history_length > 4096。
            RIN_CHECK(throwsInvalidArgument([&] {
                return makeNode("depth_history", {{"history_length", std::int64_t{4097}},
                                                  {"sample_count", std::int64_t{2}},
                                                  {"sample_skip", std::int64_t{1}},
                                                  {"sample_delay", std::int64_t{0}}}) != nullptr;
            }));
            // sample_count = 0。
            RIN_CHECK(throwsInvalidArgument([&] {
                return makeNode("depth_history", {{"history_length", std::int64_t{4}},
                                                  {"sample_count", std::int64_t{0}},
                                                  {"sample_skip", std::int64_t{1}},
                                                  {"sample_delay", std::int64_t{0}}}) != nullptr;
            }));
            // (count-1)·skip + 1 + delay > length（含 u64 回绕对抗面）。
            RIN_CHECK(throwsInvalidArgument([&] {
                return makeNode("depth_history", {{"history_length", std::int64_t{4}},
                                                  {"sample_count", std::int64_t{2}},
                                                  {"sample_skip", std::int64_t{4}},
                                                  {"sample_delay", std::int64_t{0}}}) != nullptr;
            }));
            // 恰等上界合法：framesNeeded = 1·3 + 1 + 0 = 4 == length。
            {
                std::unique_ptr<IImageNode> tight =
                    makeNode("depth_history", {{"history_length", std::int64_t{4}},
                                               {"sample_count", std::int64_t{2}},
                                               {"sample_skip", std::int64_t{3}},
                                               {"sample_delay", std::int64_t{0}}});
                RIN_CHECK(tight != nullptr);
            }
        }
    }

    // ---- 8) 图集成：米制链全 golden（runNodeGraph + 注入器） ------------------
    {
        const NodeCatalog catalog = rin::workflow_catalog::makeDefaultImageNodeCatalog();
        WorkflowGraph graph;
        const char* typeIds[7] = {"source_depth_metric", "depth_fill_invalid",  "depth_resize",
                                  "depth_crop",          "depth_gaussian_blur", "depth_normalize",
                                  "depth_history"};
        for (std::uint64_t i = 0; i < 7; ++i) {
            NodeInstance instance;
            instance.id = i + 1;
            instance.typeId = typeIds[i];  // 全部默认参数（≡ M8 冻结链）。
            graph.nodes.push_back(instance);
        }
        for (std::uint64_t i = 0; i < 6; ++i) {
            Connection connection;
            connection.from = PortRef{i + 1, PortDirection::Output, 0};
            connection.to = PortRef{i + 2, PortDirection::Input, 0};
            graph.connections.push_back(connection);
        }

        const WorkflowGraph constGraph = graph;
        NodeGraphBuild build = rin::buildNodeGraph(
            constGraph, catalog,
            [](const NodeDescriptor& descriptor, const NodeInstance& instance) {
                return rin::makeDefaultImageNode(descriptor, instance);
            },
            64);
        RIN_CHECK(build.validation.ok);
        RIN_CHECK(build.graph != nullptr);
        if (build.graph == nullptr) {
            return rin_test::exitStatus();
        }

        // 注入器：帧计数驱动（第一帧 1.25 m，第二帧 2.0 m；各为独立缓冲）。
        std::size_t frameIndex = 0;
        auto injector = [&frameIndex](const rin::NodeGraph::Node& node) {
            RIN_CHECK_MSG(node.descriptor->typeId == "source_depth_metric",
                          "injector: unexpected node");
            const float value = frameIndex == 0 ? 1.25f : 2.0f;
            std::vector<float> values(848u * 480u, value);
            ImageU8 frame = depthImage(848, 480, values);
            frameIndex++;
            return frame;
        };

        std::vector<std::vector<ImageU8>> outputs = rin::runNodeGraph(*build.graph, injector);
        RIN_CHECK_EQ(outputs.size(), std::size_t{7});

        // fill：848×480 全 1.25（常量图填充恒等）。
        expectCompactDepth32F("graph fill", outputs[1][0], 848, 480);
        expectAll("graph fill", outputs[1][0], 1.25f, 0.0);

        // resize：64×36 全 1.25（面积加权在常量图上的 double 残差 ≤1e-6）。
        expectCompactDepth32F("graph resize", outputs[2][0], 64, 36);
        expectAll("graph resize", outputs[2][0], 1.25f, 1e-6);

        // crop：32×18 全 1.25。
        expectCompactDepth32F("graph crop", outputs[3][0], 32, 18);
        expectAll("graph crop", outputs[3][0], 1.25f, 0.0);

        // blur：32×18 全 ≈1.25（常量图 Σk=1，double 累加残差 ≤1e-6）。
        expectCompactDepth32F("graph blur", outputs[4][0], 32, 18);
        expectAll("graph blur", outputs[4][0], 1.25f, 1e-6);

        // normalize：1.25/2.5 = 0.5 逐位精确（二进制可精确表示）。
        expectCompactDepth32F("graph normalize", outputs[5][0], 32, 18);
        expectAll("graph normalize", outputs[5][0], 0.5f, 0.0);

        // history：第一帧欠帧填充——8 平面 × 18 行 = 144 行全 0.5（M8 O7 idx =
        // {1,6,…,36} 全部指向首帧）。
        expectCompactDepth32F("graph history f1", outputs[6][0], 32, 144);
        expectAll("graph history f1", outputs[6][0], 0.5f, 0.0);

        // 第二帧（2.0 m → normalize 0.8）：pad = 35，idx {1,…,31}（7 平面）指向
        // 首帧 0.5，idx = 36（底平面）指向第二帧 0.8。
        std::vector<std::vector<ImageU8>> outputs2 = rin::runNodeGraph(*build.graph, injector);
        expectCompactDepth32F("graph history f2", outputs2[6][0], 32, 144);
        const std::vector<float> hist2 = floatsOf(outputs2[6][0]);
        for (std::uint32_t plane = 0; plane < 8; ++plane) {
            const float expected = plane < 7 ? 0.5f : 0.8f;
            for (std::uint32_t y = 0; y < 18; ++y) {
                const float* row =
                    reinterpret_cast<const float*>(outputs2[6][0].row(plane * 18u + y));
                for (std::uint32_t x = 0; x < 32; ++x) {
                    if (!nearlyEqual(row[x], expected, 0.0)) {
                        RIN_CHECK_MSG(false, ("graph history f2: plane " + std::to_string(plane) +
                                              " != " + std::to_string(expected))
                                                 .c_str());
                        plane = 8;
                        break;
                    }
                }
            }
        }
        RIN_CHECK_EQ(hist2.size(), std::size_t{32} * 144);
    }

    return rin_test::exitStatus();
}
