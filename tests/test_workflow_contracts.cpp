// M4-09 工作流视图契约测试（独立验证）：include/rin/workflow_types.hpp、
// src/core/workflow_types.cpp（DEC-016 冻结的 UI↔引擎唯一数据面）。
//
// 被测面与范围：
// 1) paramKindOf：variant 备择序 (bool/int64/double/string/vector<double>) 与
//    ParamKind (Boolean/Integer/Real/Enumeration/RealArray) 一一对应；
// 2) toString 三组枚举（PortType / ParamKind / WorkflowEngineState）逐枚举值
//    锁定字符串；
// 3) ParamDescriptor::valid() 全分支：id 非空、默认值种类与 kind 匹配、
//    hasRange 仅限 Integer/Real 且 min<=max（闭区间边界 min==max 合法）、
//    Enumeration 选项非空且默认值在选项内、RealArray 默认值全部有限；
// 4) NodeDescriptor::valid()（typeId/displayName 非空 + 参数声明逐项 valid）、
//    NodeCatalog::valid()（非空、typeId 唯一、逐项 valid）；
// 5) findNodeDescriptor 命中/未命中；
// 6) NodeOutputSnapshot::valid()：node 非 0、pixels 非空、width/height>0、
//    stride >= 最小行宽（Gray8 为 width，Rgba8 为 width*4）、容量
//    pixels->size() >= stride*height 的逐字节边界；
// 7) validateWorkflowGraph：正例（空图 ok、单节点悬空输出 ok、source→grayify→
//    blur 链式缺省参数 ok、全参数种类合法赋值 ok、Integer/Real 闭区间边界 ok、
//    扇出 ok、多输入汇聚 ok）+ 全部 12 种 ValidationIssueKind 反例（节点面
//    InvalidNodeId/DuplicateNodeId/UnknownNodeType；参数面 BadParam 的未知
//    paramId/重复赋值/种类不匹配/闭区间越界/枚举越选项/非有限数组；连线面
//    DirectionMismatch/UnknownConnectionNode/PortOutOfRange/TypeMismatch/
//    SelfLoop/MultipleDrivers——含"完全相同的重复连线按 MultipleDrivers 计"；
//    DanglingInput 逐端口报告；直接环与 3 节点间接环 Cycle 且 node 为
//    kInvalidNode）+ 连线校验 first-issue-continue 短路语义 + 多问题累积。
// 8) M6-03/M6-05 默认目录扩展（DEC-017，src/workflow/default_catalog.hpp 单一
//    事实源）：四相机源型（source/source_depth_jet/source_depth_gray/
//    source_depth_adaptive）descriptor valid + 端口签名（无输入、Rgba8/Gray8
//    输出）+ source 居首（契约套件泛式构图依赖）；crop_gray/downscale_gray
//    descriptor valid 且参数 schema 与 Rgba8 版逐字一致；makeDefaultImageNode
//    工厂：四 source 型返回 nullptr（注入语义）、crop_gray/downscale_gray 返回
//    实现且 Gray8 apply golden（裁切子矩形 / nearest 面积覆盖采样）、未知
//    typeId 仍抛 invalid_argument；跨类型连线：source_depth_gray→grayify 报
//    TypeMismatch、source_depth_gray→crop_gray→gaussian_blur 合法图通过。
// 9) M12/CR-15 参数匹配器单一实现回归（DEC-013）：Integer 大整数边界（2^53 邻域，
//    修复前参数面本地 double 实现对值做 double 量化导致误判；修复后
//    workflow_detail::paramValueMatches 唯一实现、Integer 用 long double 比较）、
//    端点精确表示时紧贴端点值的判别用例（修复前错误接受的真实分界）、
//    validateWorkflowGraph（Core 判据）与 src/workflow/param_check.hpp 转发头
//    （引擎 requestParamUpdate/帧边界复核判据）双路径一致性，及常规
//    Boolean/Real/Enumeration/RealArray 接受/拒绝语义不变。
//
// DOD-02 适用性说明：本契约面全部为单线程纯逻辑值语义（valid() 判定与
// validateWorkflowGraph 纯函数，无任务提交/队列/取消/超时/shutdown 语义，
// 无跨上下文共享状态），并发矩阵不适用（写法参照 test_pose_math.cpp 文件头）；
// 契约数据的通道承载（kairo::comm 选型）属实现细节，不在本测试范围。
//
// 说明：标注"契约边界探针"的用例按冻结契约的严格语义断言（min<=max、
// Rgba8 最小行宽 width*4 的数学值）；若实现与其矛盾，失败即实现问题证据，
// 由主循环裁决修复，测试不迁就实现。
#include "test_util.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "default_catalog.hpp"
// M12/CR-15 回归（第 16 节）：经转发头直接调用引擎侧匹配器
// workflow_detail::paramValueMatches（src/workflow/param_check.hpp ->
// src/core/param_match.hpp），与 validateWorkflowGraph 参数面做双路径一致性核对。
#include "param_check.hpp"

#include <rin/image_ops.hpp>
#include <rin/image_types.hpp>
#include <rin/workflow_types.hpp>

namespace {

using rin::Connection;
using rin::findNodeDescriptor;
using rin::IImageNode;
using rin::ImageU8;
using rin::kInvalidNode;
using rin::NodeCatalog;
using rin::NodeDescriptor;
using rin::NodeId;
using rin::NodeInstance;
using rin::NodeOutputSnapshot;
using rin::ParamAssignment;
using rin::ParamDescriptor;
using rin::ParamKind;
using rin::ParamValue;
using rin::PortDirection;
using rin::PortRef;
using rin::PortType;
using rin::ValidationIssueKind;
using rin::WorkflowEngineState;
using rin::WorkflowGraph;
using rin::WorkflowValidation;
using rin::findNodeDescriptor;
using rin::paramKindOf;
using rin::toString;
using rin::validateWorkflowGraph;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

// --- 夹具：小目录（source 0入1出 Rgba8 / grayify Rgba8→Gray8 /
//     blur Gray8→Gray8 带全参数种类 / mix 双 Gray8 入 → Gray8 出）---

NodeDescriptor makeSource() {
    NodeDescriptor d;
    d.typeId = "source";
    d.displayName = "视频源";
    d.outputs = {PortType::Rgba8};
    return d;
}

NodeDescriptor makeGrayify() {
    NodeDescriptor d;
    d.typeId = "grayify";
    d.displayName = "灰度化";
    d.inputs = {PortType::Rgba8};
    d.outputs = {PortType::Gray8};
    return d;
}

NodeDescriptor makeBlur() {
    NodeDescriptor d;
    d.typeId = "blur";
    d.displayName = "高斯模糊";
    d.inputs = {PortType::Gray8};
    d.outputs = {PortType::Gray8};
    ParamDescriptor radius;
    radius.id = "radius";
    radius.label = "半径";
    radius.kind = ParamKind::Integer;
    radius.defaultValue = static_cast<std::int64_t>(3);
    radius.hasRange = true;
    radius.minValue = 1.0;
    radius.maxValue = 10.0;
    ParamDescriptor strength;
    strength.id = "strength";
    strength.label = "强度";
    strength.kind = ParamKind::Real;
    strength.defaultValue = 0.5;
    strength.hasRange = true;
    strength.minValue = 0.0;
    strength.maxValue = 1.0;
    ParamDescriptor mode;
    mode.id = "mode";
    mode.label = "模式";
    mode.kind = ParamKind::Enumeration;
    mode.defaultValue = std::string("fast");
    mode.enumOptions = {"fast", "quality"};
    ParamDescriptor kernel;
    kernel.id = "kernel";
    kernel.label = "卷积核";
    kernel.kind = ParamKind::RealArray;
    kernel.defaultValue = std::vector<double>{0.25, 0.5, 0.25};
    d.params = {radius, strength, mode, kernel};
    return d;
}

NodeDescriptor makeMix() {
    NodeDescriptor d;
    d.typeId = "mix";
    d.displayName = "混合";
    d.inputs = {PortType::Gray8, PortType::Gray8};
    d.outputs = {PortType::Gray8};
    return d;
}

NodeCatalog makeCatalog() {
    NodeCatalog catalog;
    catalog.nodes = {makeSource(), makeGrayify(), makeBlur(), makeMix()};
    return catalog;
}

NodeInstance makeNode(NodeId id, const std::string& typeId,
                      std::vector<ParamAssignment> params = {}) {
    NodeInstance node;
    node.id = id;
    node.typeId = typeId;
    node.params = std::move(params);
    return node;
}

ParamAssignment assign(const std::string& paramId, ParamValue value) {
    return ParamAssignment{paramId, std::move(value)};
}

Connection conn(NodeId from, std::uint32_t fromIndex, NodeId to, std::uint32_t toIndex) {
    Connection c;
    c.from = PortRef{from, PortDirection::Output, fromIndex};
    c.to = PortRef{to, PortDirection::Input, toIndex};
    return c;
}

// 合法链 source(1) -> grayify(2) -> blur(3)；blur 参数可注入（缺省合法）。
WorkflowGraph makeChain(std::vector<ParamAssignment> blurParams = {}) {
    WorkflowGraph g;
    g.nodes = {makeNode(1, "source"), makeNode(2, "grayify"),
               makeNode(3, "blur", std::move(blurParams))};
    g.connections = {conn(1, 0, 2, 0), conn(2, 0, 3, 0)};
    return g;
}

NodeOutputSnapshot makeSnapshot(PortType format, std::uint32_t width, std::uint32_t height,
                                std::uint32_t stride, std::size_t bytes) {
    NodeOutputSnapshot s;
    s.node = 5;
    s.format = format;
    s.width = width;
    s.height = height;
    s.stride = stride;
    s.sourceSequence = 42;
    s.pixels = std::make_shared<const std::vector<std::uint8_t>>(bytes, std::uint8_t{0});
    return s;
}

std::size_t countIssues(const WorkflowValidation& v, ValidationIssueKind kind, NodeId node) {
    std::size_t count = 0;
    for (const auto& issue : v.issues) {
        if (issue.kind == kind && issue.node == node) {
            ++count;
        }
    }
    return count;
}

bool hasKind(const WorkflowValidation& v, ValidationIssueKind kind) {
    for (const auto& issue : v.issues) {
        if (issue.kind == kind) {
            return true;
        }
    }
    return false;
}

// 参数 schema 逐字段相等（ParamDescriptor 无 operator==，逐字段比较）。
bool paramSchemasEqual(const std::vector<ParamDescriptor>& lhs,
                       const std::vector<ParamDescriptor>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const ParamDescriptor& a = lhs[i];
        const ParamDescriptor& b = rhs[i];
        if (a.id != b.id || a.label != b.label || a.kind != b.kind ||
            !(a.defaultValue == b.defaultValue) || a.hasRange != b.hasRange ||
            a.minValue != b.minValue || a.maxValue != b.maxValue ||
            a.enumOptions != b.enumOptions) {
            return false;
        }
    }
    return true;
}

// 确定性 Gray8 小图（pixel(y,x) = y*width + x 的低 8 位，递增图案）。
ImageU8 makeGrayImage(std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            pixels[static_cast<std::size_t>(y) * width + x] =
                static_cast<std::uint8_t>((y * width + x) & 0xFF);
        }
    }
    return ImageU8::wrap(PortType::Gray8, width, height, width,
                         std::make_shared<const std::vector<std::uint8_t>>(
                             std::move(pixels)));
}

}  // namespace

int main() {
    const NodeCatalog catalog = makeCatalog();
    RIN_CHECK(catalog.valid());

    // --- 1) paramKindOf：variant 备择序 -> ParamKind 一一对应 ---
    {
        RIN_CHECK(paramKindOf(ParamValue{true}) == ParamKind::Boolean);
        RIN_CHECK(paramKindOf(ParamValue{static_cast<std::int64_t>(-7)}) == ParamKind::Integer);
        RIN_CHECK(paramKindOf(ParamValue{0.5}) == ParamKind::Real);
        RIN_CHECK(paramKindOf(ParamValue{std::string("fast")}) == ParamKind::Enumeration);
        RIN_CHECK(paramKindOf(ParamValue{std::vector<double>{1.0, 2.0}}) == ParamKind::RealArray);
    }

    // --- 2) toString：三组枚举逐值锁定 ---
    {
        RIN_CHECK(std::string(toString(PortType::Gray8)) == "Gray8");
        RIN_CHECK(std::string(toString(PortType::Rgba8)) == "Rgba8");

        RIN_CHECK(std::string(toString(ParamKind::Boolean)) == "Boolean");
        RIN_CHECK(std::string(toString(ParamKind::Integer)) == "Integer");
        RIN_CHECK(std::string(toString(ParamKind::Real)) == "Real");
        RIN_CHECK(std::string(toString(ParamKind::Enumeration)) == "Enumeration");
        RIN_CHECK(std::string(toString(ParamKind::RealArray)) == "RealArray");

        RIN_CHECK(std::string(toString(WorkflowEngineState::Idle)) == "Idle");
        RIN_CHECK(std::string(toString(WorkflowEngineState::Running)) == "Running");
        RIN_CHECK(std::string(toString(WorkflowEngineState::Stopping)) == "Stopping");
        RIN_CHECK(std::string(toString(WorkflowEngineState::Failed)) == "Failed");
    }

    // --- 3) ParamDescriptor::valid() 全分支 ---
    {
        // 合法基线：Integer 带闭区间范围。
        ParamDescriptor integerRange;
        integerRange.id = "radius";
        integerRange.label = "半径";
        integerRange.kind = ParamKind::Integer;
        integerRange.defaultValue = static_cast<std::int64_t>(3);
        integerRange.hasRange = true;
        integerRange.minValue = 1.0;
        integerRange.maxValue = 10.0;
        RIN_CHECK(integerRange.valid());

        // id 非空要求。
        ParamDescriptor emptyId = integerRange;
        emptyId.id.clear();
        RIN_CHECK(!emptyId.valid());

        // 默认值种类必须与 kind 匹配（逐 mismatch 组合）。
        ParamDescriptor wrongBool = integerRange;
        wrongBool.defaultValue = true;  // bool vs Integer
        RIN_CHECK(!wrongBool.valid());
        ParamDescriptor wrongString = integerRange;
        wrongString.defaultValue = std::string("3");  // string vs Integer
        RIN_CHECK(!wrongString.valid());
        ParamDescriptor wrongInt = integerRange;
        wrongInt.kind = ParamKind::Real;  // int64 默认值 vs Real
        RIN_CHECK(!wrongInt.valid());
        ParamDescriptor wrongArray = integerRange;
        wrongArray.kind = ParamKind::RealArray;  // int64 默认值 vs RealArray
        RIN_CHECK(!wrongArray.valid());

        // hasRange 仅限 Integer/Real：其余种类带范围一律拒绝。
        ParamDescriptor booleanRange;
        booleanRange.id = "enabled";
        booleanRange.label = "开关";
        booleanRange.kind = ParamKind::Boolean;
        booleanRange.defaultValue = true;
        RIN_CHECK(booleanRange.valid());
        booleanRange.hasRange = true;
        booleanRange.minValue = 0.0;
        booleanRange.maxValue = 1.0;
        RIN_CHECK(!booleanRange.valid());

        ParamDescriptor enumRange = integerRange;
        enumRange.id = "mode";
        enumRange.kind = ParamKind::Enumeration;
        enumRange.defaultValue = std::string("a");
        enumRange.enumOptions = {"a", "b"};
        RIN_CHECK(enumRange.hasRange == true);
        RIN_CHECK(!enumRange.valid());  // Enumeration + hasRange -> invalid
        enumRange.hasRange = false;
        RIN_CHECK(enumRange.valid());

        ParamDescriptor arrayRange = integerRange;
        arrayRange.id = "kernel";
        arrayRange.kind = ParamKind::RealArray;
        arrayRange.defaultValue = std::vector<double>{1.0};
        RIN_CHECK(!arrayRange.valid());  // RealArray + hasRange -> invalid
        arrayRange.hasRange = false;
        RIN_CHECK(arrayRange.valid());

        // 范围闭区间：min>max 拒绝；min==max 合法（单点闭区间）；无范围不限。
        ParamDescriptor inverted = integerRange;
        inverted.minValue = 10.0;
        inverted.maxValue = 1.0;
        RIN_CHECK(!inverted.valid());
        ParamDescriptor singlePoint = integerRange;
        singlePoint.minValue = 5.0;
        singlePoint.maxValue = 5.0;
        RIN_CHECK(singlePoint.valid());
        ParamDescriptor unbounded = integerRange;
        unbounded.hasRange = false;
        RIN_CHECK(unbounded.valid());

        // 契约边界探针：hasRange 要求 min<=max；NaN 端点不满足任何 min<=max
        // 比较，按冻结契约应判 invalid。
        ParamDescriptor nanMin = integerRange;
        nanMin.kind = ParamKind::Real;
        nanMin.defaultValue = 0.5;
        nanMin.minValue = kNaN;
        nanMin.maxValue = 1.0;
        RIN_CHECK_MSG(!nanMin.valid(), "min=NaN 不满足 min<=max，应为 invalid");
        ParamDescriptor nanMax = integerRange;
        nanMax.kind = ParamKind::Real;
        nanMax.defaultValue = 0.5;
        nanMax.minValue = 0.0;
        nanMax.maxValue = kNaN;
        RIN_CHECK_MSG(!nanMax.valid(), "max=NaN 不满足 min<=max，应为 invalid");
        // ±Inf 端点满足 min<=max，契约上合法。
        ParamDescriptor infRange = nanMax;
        infRange.minValue = -kInf;
        infRange.maxValue = kInf;
        RIN_CHECK(infRange.valid());

        // Enumeration：选项非空且默认值必须在选项内。
        ParamDescriptor enumNoOptions = enumRange;
        enumNoOptions.enumOptions.clear();
        RIN_CHECK(!enumNoOptions.valid());
        ParamDescriptor enumOutside = enumRange;
        enumOutside.defaultValue = std::string("slow");  // 不在 {a,b} 内
        RIN_CHECK(!enumOutside.valid());
        RIN_CHECK(enumRange.valid());  // 基线合法

        // RealArray：默认值全部有限；空数组按契约"全部有限"空真合法。
        ParamDescriptor arrayOk = arrayRange;
        arrayOk.defaultValue = std::vector<double>{1.0, 0.0, -1.0};
        RIN_CHECK(arrayOk.valid());
        ParamDescriptor arrayEmpty = arrayOk;
        arrayEmpty.defaultValue = std::vector<double>{};
        RIN_CHECK(arrayEmpty.valid());
        ParamDescriptor arrayNan = arrayOk;
        arrayNan.defaultValue = std::vector<double>{1.0, kNaN};
        RIN_CHECK(!arrayNan.valid());
        ParamDescriptor arrayInf = arrayOk;
        arrayInf.defaultValue = std::vector<double>{kInf, 0.0};
        RIN_CHECK(!arrayInf.valid());
    }

    // --- 4) NodeDescriptor::valid() / NodeCatalog::valid() ---
    {
        // 最小合法：typeId/displayName 非空，无端口无参数（source 形态）。
        RIN_CHECK(makeSource().valid());

        NodeDescriptor noType = makeSource();
        noType.typeId.clear();
        RIN_CHECK(!noType.valid());

        NodeDescriptor noName = makeSource();
        noName.displayName.clear();
        RIN_CHECK(!noName.valid());

        // 参数声明逐项参与：全部合法 -> valid；任一非法 -> invalid。
        NodeDescriptor withParams = makeBlur();
        RIN_CHECK(withParams.valid());
        withParams.params[1].defaultValue = static_cast<std::int64_t>(1);  // strength: int64 vs Real
        RIN_CHECK(!withParams.valid());

        // 冻结范围：valid() 只要求 typeId/displayName 非空且参数声明逐项
        // valid；参数 id 的声明级唯一性不属 valid() 契约（实例级重复赋值由
        // validateWorkflowGraph 拒绝）。
        NodeDescriptor duplicateParamIds = makeBlur();
        duplicateParamIds.params[1].id = duplicateParamIds.params[0].id;
        RIN_CHECK(duplicateParamIds.valid());

        // NodeCatalog：非空、逐项 valid、typeId 唯一。
        NodeCatalog emptyCatalog;
        RIN_CHECK(!emptyCatalog.valid());
        RIN_CHECK(catalog.valid());

        NodeCatalog duplicateType = makeCatalog();
        duplicateType.nodes.push_back(makeBlur());  // "blur" 第二次出现
        RIN_CHECK(!duplicateType.valid());

        NodeCatalog invalidMember = makeCatalog();
        invalidMember.nodes[0].displayName.clear();
        RIN_CHECK(!invalidMember.valid());
    }

    // --- 5) findNodeDescriptor 命中/未命中 ---
    {
        const NodeDescriptor* blur = findNodeDescriptor(catalog, "blur");
        RIN_CHECK(blur != nullptr);
        RIN_CHECK(blur->typeId == "blur");
        RIN_CHECK_EQ(blur->params.size(), std::size_t{4});

        const NodeDescriptor* mix = findNodeDescriptor(catalog, "mix");
        RIN_CHECK(mix != nullptr);
        RIN_CHECK_EQ(mix->inputs.size(), std::size_t{2});

        RIN_CHECK(findNodeDescriptor(catalog, "nonexistent") == nullptr);
        NodeCatalog emptyCatalog;
        RIN_CHECK(findNodeDescriptor(emptyCatalog, "source") == nullptr);
    }

    // --- 6) NodeOutputSnapshot::valid()：stride / 容量逐字节边界 ---
    {
        // 默认构造：node=0、空指针、零尺寸 -> invalid。
        const NodeOutputSnapshot defaults;
        RIN_CHECK(!defaults.valid());

        // Gray8：最小 stride == width；对齐 stride > width 合法；容量恰好
        // stride*height 合法，差一字节拒绝。
        RIN_CHECK(makeSnapshot(PortType::Gray8, 4, 2, 4, 8).valid());
        RIN_CHECK(makeSnapshot(PortType::Gray8, 4, 2, 8, 16).valid());
        RIN_CHECK(!makeSnapshot(PortType::Gray8, 4, 2, 3, 6).valid());
        RIN_CHECK(!makeSnapshot(PortType::Gray8, 4, 2, 4, 7).valid());

        // Rgba8：最小 stride == width*4。
        RIN_CHECK(makeSnapshot(PortType::Rgba8, 4, 2, 16, 32).valid());
        RIN_CHECK(makeSnapshot(PortType::Rgba8, 4, 2, 20, 40).valid());
        RIN_CHECK(!makeSnapshot(PortType::Rgba8, 4, 2, 15, 30).valid());
        RIN_CHECK(!makeSnapshot(PortType::Rgba8, 4, 2, 16, 31).valid());

        // node=0 / 空像素 / 零尺寸逐项拒绝。
        {
            NodeOutputSnapshot s = makeSnapshot(PortType::Gray8, 4, 2, 4, 8);
            s.node = kInvalidNode;
            RIN_CHECK(!s.valid());
        }
        {
            NodeOutputSnapshot s = makeSnapshot(PortType::Gray8, 4, 2, 4, 8);
            s.pixels = nullptr;
            RIN_CHECK(!s.valid());
        }
        {
            NodeOutputSnapshot s = makeSnapshot(PortType::Gray8, 4, 2, 4, 8);
            s.width = 0;
            RIN_CHECK(!s.valid());
        }
        {
            NodeOutputSnapshot s = makeSnapshot(PortType::Gray8, 4, 2, 4, 8);
            s.height = 0;
            RIN_CHECK(!s.valid());
        }

        // 契约边界探针：Rgba8 最小行宽为 width*4（数学值）。width=2^30 时
        // width*4 = 2^32 超出 uint32，任何 uint32 stride 都不可能满足
        // "stride >= 最小行宽"，按冻结契约应判 invalid。
        {
            NodeOutputSnapshot overflow = makeSnapshot(PortType::Rgba8, 0x40000000u, 1, 0, 0);
            RIN_CHECK_MSG(!overflow.valid(),
                          "width*4 溢出 uint32 时最小行宽不可满足，应为 invalid");
        }
    }

    // --- 7) PortRef / Connection 相等语义（重复连线判定的基础）---
    {
        const Connection a = conn(1, 0, 2, 0);
        const Connection b = conn(1, 0, 2, 0);
        const Connection c = conn(1, 0, 2, 1);
        RIN_CHECK(a == b);
        RIN_CHECK(!(a != b));
        RIN_CHECK(a != c);
        RIN_CHECK(!(conn(2, 0, 1, 0) == conn(1, 0, 2, 0)));  // 方向参与相等
    }

    // --- 8) validateWorkflowGraph 正例 ---
    {
        // 空图（无节点无边）ok 且无 issue。
        const WorkflowValidation empty = validateWorkflowGraph(WorkflowGraph{}, catalog);
        RIN_CHECK(empty.ok);
        RIN_CHECK(empty.issues.empty());
        RIN_CHECK(empty.ok == empty.issues.empty());

        // 单 source：悬空输出允许（末端结果可直接查看）。
        WorkflowGraph solo;
        solo.nodes = {makeNode(1, "source")};
        const WorkflowValidation soloResult = validateWorkflowGraph(solo, catalog);
        RIN_CHECK(soloResult.ok);
        RIN_CHECK(soloResult.issues.empty());

        // 缺省参数链式：blur 未赋值参数取声明默认值。
        const WorkflowValidation chain = validateWorkflowGraph(makeChain(), catalog);
        RIN_CHECK(chain.ok);
        RIN_CHECK(chain.issues.empty());

        // 全参数种类且值合法的图。
        WorkflowGraph fullParams = makeChain(
            {assign("radius", ParamValue{static_cast<std::int64_t>(5)}),
             assign("strength", ParamValue{0.25}),
             assign("mode", ParamValue{std::string("quality")}),
             assign("kernel", ParamValue{std::vector<double>{0.25, 0.5, 0.25}})});
        const WorkflowValidation fullResult = validateWorkflowGraph(fullParams, catalog);
        RIN_CHECK(fullResult.ok);
        RIN_CHECK(fullResult.issues.empty());

        // Integer/Real 闭区间边界值（min 与 max 本身）合法。
        const WorkflowValidation radiusMin =
            validateWorkflowGraph(makeChain({assign("radius", ParamValue{static_cast<std::int64_t>(1)})}),
                                  catalog);
        RIN_CHECK(radiusMin.ok);
        const WorkflowValidation radiusMax =
            validateWorkflowGraph(makeChain({assign("radius", ParamValue{static_cast<std::int64_t>(10)})}),
                                  catalog);
        RIN_CHECK(radiusMax.ok);
        const WorkflowValidation strengthMin =
            validateWorkflowGraph(makeChain({assign("strength", ParamValue{0.0})}), catalog);
        RIN_CHECK(strengthMin.ok);
        const WorkflowValidation strengthMax =
            validateWorkflowGraph(makeChain({assign("strength", ParamValue{1.0})}), catalog);
        RIN_CHECK(strengthMax.ok);

        // 扇出：一个输出多条出边允许（两个 grayify 输出悬空仍 ok）。
        WorkflowGraph fanout;
        fanout.nodes = {makeNode(1, "source"), makeNode(2, "grayify"), makeNode(3, "grayify")};
        fanout.connections = {conn(1, 0, 2, 0), conn(1, 0, 3, 0)};
        const WorkflowValidation fanoutResult = validateWorkflowGraph(fanout, catalog);
        RIN_CHECK(fanoutResult.ok);
        RIN_CHECK(fanoutResult.issues.empty());

        // 多输入汇聚 + 扇出组合：source→grayify；grayify 扇出至 blur 与
        // mix#0；blur→mix#1。
        WorkflowGraph converge;
        converge.nodes = {makeNode(1, "source"), makeNode(2, "grayify"), makeNode(3, "blur"),
                          makeNode(4, "mix")};
        converge.connections = {conn(1, 0, 2, 0), conn(2, 0, 3, 0), conn(2, 0, 4, 0),
                                conn(3, 0, 4, 1)};
        const WorkflowValidation convergeResult = validateWorkflowGraph(converge, catalog);
        RIN_CHECK(convergeResult.ok);
        RIN_CHECK(convergeResult.issues.empty());
    }

    // --- 9) 节点面反例：InvalidNodeId / DuplicateNodeId / UnknownNodeType ---
    {
        // id==0 保留无效值。
        WorkflowGraph zeroId;
        zeroId.nodes = {makeNode(kInvalidNode, "source")};
        const WorkflowValidation result = validateWorkflowGraph(zeroId, catalog);
        RIN_CHECK(!result.ok);
        RIN_CHECK_EQ(result.issues.size(), std::size_t{1});
        RIN_CHECK_EQ(countIssues(result, ValidationIssueKind::InvalidNodeId, kInvalidNode),
                     std::size_t{1});

        // 图内重复 id（source 无输入端口，无 DanglingInput 噪声）。
        WorkflowGraph duplicated;
        duplicated.nodes = {makeNode(1, "source"), makeNode(1, "source")};
        const WorkflowValidation dupResult = validateWorkflowGraph(duplicated, catalog);
        RIN_CHECK(!dupResult.ok);
        RIN_CHECK_EQ(dupResult.issues.size(), std::size_t{1});
        RIN_CHECK_EQ(countIssues(dupResult, ValidationIssueKind::DuplicateNodeId, 1),
                     std::size_t{1});

        // typeId 不在目录。
        WorkflowGraph ghost;
        ghost.nodes = {makeNode(2, "nonexistent")};
        const WorkflowValidation ghostResult = validateWorkflowGraph(ghost, catalog);
        RIN_CHECK(!ghostResult.ok);
        RIN_CHECK_EQ(ghostResult.issues.size(), std::size_t{1});
        RIN_CHECK_EQ(countIssues(ghostResult, ValidationIssueKind::UnknownNodeType, 2),
                     std::size_t{1});
    }

    // --- 10) 参数面反例：BadParam 全分支（每例恰一条 issue，落在节点 3）---
    {
        const auto expectSingleBadParam = [&catalog](std::vector<ParamAssignment> params) {
            const WorkflowValidation v = validateWorkflowGraph(makeChain(std::move(params)), catalog);
            RIN_CHECK(!v.ok);
            RIN_CHECK_EQ(v.issues.size(), std::size_t{1});
            RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::BadParam, 3), std::size_t{1});
        };

        // 未知 paramId。
        expectSingleBadParam({assign("nope", ParamValue{true})});
        // 重复赋值（paramId 图内唯一）。
        expectSingleBadParam({assign("radius", ParamValue{static_cast<std::int64_t>(5)}),
                              assign("radius", ParamValue{static_cast<std::int64_t>(6)})});
        // 值种类与声明不匹配（逐组合）。
        expectSingleBadParam({assign("radius", ParamValue{true})});  // bool vs Integer
        expectSingleBadParam({assign("strength", ParamValue{static_cast<std::int64_t>(1)})});  // int64 vs Real
        expectSingleBadParam({assign("mode", ParamValue{0.5})});  // double vs Enumeration
        expectSingleBadParam({assign("kernel", ParamValue{std::string("x")})});  // string vs RealArray
        expectSingleBadParam({assign("radius", ParamValue{std::vector<double>{1.0}})});  // array vs Integer
        // hasRange 闭区间越界（Integer 与 Real 各取两侧界外值）。
        expectSingleBadParam({assign("radius", ParamValue{static_cast<std::int64_t>(0)})});
        expectSingleBadParam({assign("radius", ParamValue{static_cast<std::int64_t>(11)})});
        expectSingleBadParam({assign("strength", ParamValue{-0.5})});
        expectSingleBadParam({assign("strength", ParamValue{1.5})});
        // 枚举值不在选项内。
        expectSingleBadParam({assign("mode", ParamValue{std::string("ultra")})});
        // RealArray 含非有限值（NaN / Inf）。
        expectSingleBadParam({assign("kernel", ParamValue{std::vector<double>{0.5, kNaN}})});
        expectSingleBadParam({assign("kernel", ParamValue{std::vector<double>{kInf}})});
    }

    // --- 11) 连线面反例 ---
    {
        // DirectionMismatch：from 为输入端口（方向反转）。连线校验按
        // first-issue-continue 短路：只报方向问题，不追加端点/类型类问题。
        {
            WorkflowGraph g;
            g.nodes = {makeNode(1, "source"), makeNode(2, "grayify")};
            Connection reversed;
            reversed.from = PortRef{2, PortDirection::Input, 0};
            reversed.to = PortRef{1, PortDirection::Output, 0};
            g.connections = {reversed};
            const WorkflowValidation v = validateWorkflowGraph(g, catalog);
            RIN_CHECK(!v.ok);
            RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::DirectionMismatch, 1),
                         std::size_t{1});  // node 取 to.node
            RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::DanglingInput, 2), std::size_t{1});
            RIN_CHECK(!hasKind(v, ValidationIssueKind::UnknownConnectionNode));  // 短路
            RIN_CHECK(!hasKind(v, ValidationIssueKind::TypeMismatch));
            RIN_CHECK_EQ(v.issues.size(), std::size_t{2});
        }

        // UnknownConnectionNode：to 端节点不在图（node 取缺失端）。
        {
            WorkflowGraph g;
            g.nodes = {makeNode(1, "source")};
            g.connections = {conn(1, 0, 99, 0)};
            const WorkflowValidation v = validateWorkflowGraph(g, catalog);
            RIN_CHECK(!v.ok);
            RIN_CHECK_EQ(v.issues.size(), std::size_t{1});
            RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::UnknownConnectionNode, 99),
                         std::size_t{1});
        }
        // UnknownConnectionNode：from 端节点不在图（node 取缺失端）。注意入边
        // 判定语义：悬空检查只看端口是否存在指向它的连线，端点非法的连线仍算
        // 该端口的入边，不追加 DanglingInput（连线的非法性已单独报告）。
        {
            WorkflowGraph g;
            g.nodes = {makeNode(2, "grayify")};
            g.connections = {conn(99, 0, 2, 0)};
            const WorkflowValidation v = validateWorkflowGraph(g, catalog);
            RIN_CHECK(!v.ok);
            RIN_CHECK_EQ(v.issues.size(), std::size_t{1});
            RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::UnknownConnectionNode, 99),
                         std::size_t{1});
            RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::DanglingInput, 2), std::size_t{0});
        }

        // PortOutOfRange：输出端口序号越界（node 取越界端）。to 端口已有连线
        // 指向（同上入边语义），不追加 DanglingInput。
        {
            WorkflowGraph g;
            g.nodes = {makeNode(1, "source"), makeNode(2, "grayify")};
            g.connections = {conn(1, 1, 2, 0)};  // source 仅 1 个输出
            const WorkflowValidation v = validateWorkflowGraph(g, catalog);
            RIN_CHECK(!v.ok);
            RIN_CHECK_EQ(v.issues.size(), std::size_t{1});
            RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::PortOutOfRange, 1), std::size_t{1});
            RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::DanglingInput, 2), std::size_t{0});
        }
        // PortOutOfRange：输入端口序号越界。越界序号无法标记入边，该节点的
        // 有效端口实际无入边 → 追加 DanglingInput（与 from 侧越界对照）。
        {
            WorkflowGraph g;
            g.nodes = {makeNode(1, "source"), makeNode(2, "grayify")};
            g.connections = {conn(1, 0, 2, 1)};  // grayify 仅 1 个输入
            const WorkflowValidation v = validateWorkflowGraph(g, catalog);
            RIN_CHECK(!v.ok);
            RIN_CHECK_EQ(v.issues.size(), std::size_t{2});
            RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::PortOutOfRange, 2), std::size_t{1});
            RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::DanglingInput, 2), std::size_t{1});
        }

        // TypeMismatch：Rgba8 输出接入 Gray8 输入（node 取 to.node）。同上
        // 入边语义，不追加 DanglingInput。
        {
            WorkflowGraph g;
            g.nodes = {makeNode(1, "source"), makeNode(3, "blur")};
            g.connections = {conn(1, 0, 3, 0)};
            const WorkflowValidation v = validateWorkflowGraph(g, catalog);
            RIN_CHECK(!v.ok);
            RIN_CHECK_EQ(v.issues.size(), std::size_t{1});
            RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::TypeMismatch, 3), std::size_t{1});
        }

        // SelfLoop：类型相容的自身回连（Gray8→Gray8）；自环同时构成环。
        {
            WorkflowGraph g;
            g.nodes = {makeNode(3, "blur")};
            g.connections = {conn(3, 0, 3, 0)};
            const WorkflowValidation v = validateWorkflowGraph(g, catalog);
            RIN_CHECK(!v.ok);
            RIN_CHECK_EQ(v.issues.size(), std::size_t{2});
            RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::SelfLoop, 3), std::size_t{1});
            RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::DanglingInput, 3), std::size_t{0});
            RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::Cycle, kInvalidNode), std::size_t{1});
        }

        // MultipleDrivers：完全相同的重复连线——第二次 insert 失败亦按
        // MultipleDrivers 计。
        {
            WorkflowGraph g;
            g.nodes = {makeNode(2, "grayify"), makeNode(3, "blur"), makeNode(4, "blur")};
            g.connections = {conn(2, 0, 3, 0), conn(2, 0, 3, 0)};
            const WorkflowValidation v = validateWorkflowGraph(g, catalog);
            RIN_CHECK(!v.ok);
            RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::MultipleDrivers, 3), std::size_t{1});
            RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::DanglingInput, 4), std::size_t{1});
            RIN_CHECK(!hasKind(v, ValidationIssueKind::Cycle));  // 重复边不破坏 Kahn 剥离
        }
        // MultipleDrivers：异源双驱动同一输入端口。
        {
            WorkflowGraph g;
            g.nodes = {makeNode(2, "grayify"), makeNode(3, "blur"), makeNode(4, "blur")};
            g.connections = {conn(2, 0, 3, 0), conn(4, 0, 3, 0)};
            const WorkflowValidation v = validateWorkflowGraph(g, catalog);
            RIN_CHECK(!v.ok);
            RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::MultipleDrivers, 3), std::size_t{1});
            RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::DanglingInput, 4), std::size_t{1});
        }
    }

    // --- 12) DanglingInput：逐输入端口报告；悬空输出允许 ---
    {
        // 双输入 mix 独立存在：两个端口各报一条。
        WorkflowGraph alone;
        alone.nodes = {makeNode(4, "mix")};
        const WorkflowValidation aloneResult = validateWorkflowGraph(alone, catalog);
        RIN_CHECK(!aloneResult.ok);
        RIN_CHECK_EQ(aloneResult.issues.size(), std::size_t{2});
        RIN_CHECK_EQ(countIssues(aloneResult, ValidationIssueKind::DanglingInput, 4),
                     std::size_t{2});

        // 仅驱动 #0：只余 #1 悬空（逐端口粒度）。
        WorkflowGraph partial;
        partial.nodes = {makeNode(3, "blur"), makeNode(4, "mix")};
        partial.connections = {conn(3, 0, 4, 0)};
        const WorkflowValidation partialResult = validateWorkflowGraph(partial, catalog);
        RIN_CHECK(!partialResult.ok);
        RIN_CHECK_EQ(countIssues(partialResult, ValidationIssueKind::DanglingInput, 4),
                     std::size_t{1});
        RIN_CHECK_EQ(countIssues(partialResult, ValidationIssueKind::DanglingInput, 3),
                     std::size_t{1});
    }

    // --- 13) Cycle：直接环与 3 节点间接环（node 恒为 kInvalidNode）---
    {
        WorkflowGraph twoCycle;
        twoCycle.nodes = {makeNode(1, "blur"), makeNode(2, "blur")};
        twoCycle.connections = {conn(1, 0, 2, 0), conn(2, 0, 1, 0)};
        const WorkflowValidation twoResult = validateWorkflowGraph(twoCycle, catalog);
        RIN_CHECK(!twoResult.ok);
        RIN_CHECK_EQ(twoResult.issues.size(), std::size_t{1});
        RIN_CHECK_EQ(countIssues(twoResult, ValidationIssueKind::Cycle, kInvalidNode),
                     std::size_t{1});

        WorkflowGraph threeCycle;
        threeCycle.nodes = {makeNode(1, "blur"), makeNode(2, "blur"), makeNode(3, "blur")};
        threeCycle.connections = {conn(1, 0, 2, 0), conn(2, 0, 3, 0), conn(3, 0, 1, 0)};
        const WorkflowValidation threeResult = validateWorkflowGraph(threeCycle, catalog);
        RIN_CHECK(!threeResult.ok);
        RIN_CHECK_EQ(threeResult.issues.size(), std::size_t{1});
        RIN_CHECK_EQ(countIssues(threeResult, ValidationIssueKind::Cycle, kInvalidNode),
                     std::size_t{1});
    }

    // --- 14) 多问题累积：单图多条 issue（跨节点面/连线面/悬空/成环）---
    // 自环连线仍算端口入边（不追加 DanglingInput(3)），并因自边入度永不清零
    // 触发 Cycle。
    {
        WorkflowGraph g;
        g.nodes = {
            makeNode(1, "blur", {assign("radius", ParamValue{static_cast<std::int64_t>(99)})}),  // 越界
            makeNode(2, "ghost"),  // 未知类型
            makeNode(3, "blur"),   // 自环
            makeNode(4, "blur"),   // 被 1 驱动
        };
        g.connections = {conn(3, 0, 3, 0), conn(1, 0, 4, 0)};
        const WorkflowValidation v = validateWorkflowGraph(g, catalog);
        RIN_CHECK(!v.ok);
        RIN_CHECK(!v.issues.empty());
        RIN_CHECK_EQ(v.issues.size(), std::size_t{5});
        RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::BadParam, 1), std::size_t{1});
        RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::UnknownNodeType, 2), std::size_t{1});
        RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::SelfLoop, 3), std::size_t{1});
        RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::DanglingInput, 1), std::size_t{1});
        RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::DanglingInput, 3), std::size_t{0});
        RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::Cycle, kInvalidNode), std::size_t{1});
        RIN_CHECK(v.ok == v.issues.empty());
    }

    // --- 15) M6-03/M6-05 默认目录扩展（DEC-017）：四相机源型 + 灰度域算子 ---
    {
        const NodeCatalog def = rin::workflow_catalog::makeDefaultImageNodeCatalog();
        RIN_CHECK(def.valid());

        // source 居首（契约套件按"目录首个无输入节点"泛式构图，依赖此序）；
        // 新类型一律追加于既有条目之后。
        RIN_CHECK(!def.nodes.empty());
        RIN_CHECK_MSG(!def.nodes.empty() && def.nodes.front().typeId == "source",
                      "catalog: source stays first in the default catalog");

        struct SourceExpect {
            const char* typeId;
            PortType output;
        };
        const std::vector<SourceExpect> sources = {
            {"source", PortType::Rgba8},
            {"source_depth_jet", PortType::Rgba8},
            {"source_depth_gray", PortType::Gray8},
            {"source_depth_adaptive", PortType::Gray8},
        };
        for (const SourceExpect& expect : sources) {
            const NodeDescriptor* d = findNodeDescriptor(def, expect.typeId);
            RIN_CHECK_MSG(d != nullptr,
                          std::string("catalog: source type present: ") + expect.typeId);
            if (d == nullptr) {
                continue;
            }
            RIN_CHECK(d->valid());
            RIN_CHECK_MSG(d->inputs.empty(),
                          std::string(expect.typeId) + ": injection source has no inputs");
            RIN_CHECK(d->outputs.size() == 1);
            RIN_CHECK(d->outputs.size() == 1 && d->outputs.front() == expect.output);
            RIN_CHECK(!d->displayName.empty());
            RIN_CHECK_MSG(d->params.empty(),
                          std::string(expect.typeId) +
                              ": camera sources declare no params (resolution is a "
                              "global stream property)");
        }

        // 灰度域算子 descriptor valid；参数 schema 与 Rgba8 版逐字一致；
        // 端口签名为 Gray8 → Gray8。
        const NodeDescriptor* crop = findNodeDescriptor(def, "crop");
        const NodeDescriptor* cropGray = findNodeDescriptor(def, "crop_gray");
        const NodeDescriptor* down = findNodeDescriptor(def, "downscale");
        const NodeDescriptor* downGray = findNodeDescriptor(def, "downscale_gray");
        RIN_CHECK(crop != nullptr && cropGray != nullptr && down != nullptr &&
                  downGray != nullptr);
        if (crop != nullptr && cropGray != nullptr && down != nullptr &&
            downGray != nullptr) {
            RIN_CHECK(cropGray->valid() && downGray->valid());
            RIN_CHECK(cropGray->typeId == "crop_gray" && downGray->typeId == "downscale_gray");
            RIN_CHECK(!cropGray->displayName.empty() && !downGray->displayName.empty());
            const std::vector<PortType> grayPort{PortType::Gray8};
            RIN_CHECK(cropGray->inputs == grayPort && cropGray->outputs == grayPort);
            RIN_CHECK(downGray->inputs == grayPort && downGray->outputs == grayPort);
            RIN_CHECK_MSG(paramSchemasEqual(crop->params, cropGray->params),
                          "catalog: crop_gray param schema is verbatim crop");
            RIN_CHECK_MSG(paramSchemasEqual(down->params, downGray->params),
                          "catalog: downscale_gray param schema is verbatim downscale");
        }

        // 工厂：四个 source 型返回 nullptr（注入型语义）；未知 typeId 仍抛
        // std::invalid_argument（不静默）。
        for (const SourceExpect& expect : sources) {
            const NodeDescriptor* d = findNodeDescriptor(def, expect.typeId);
            if (d == nullptr) {
                continue;
            }
            const NodeInstance instance = makeNode(100, expect.typeId);
            RIN_CHECK_MSG(rin::makeDefaultImageNode(*d, instance) == nullptr,
                          std::string("factory: ") + expect.typeId +
                              " returns nullptr (injection semantics)");
        }
        {
            NodeDescriptor unknown;
            unknown.typeId = "nope_gray";
            unknown.displayName = "未知";
            unknown.outputs = {PortType::Gray8};
            const NodeInstance unknownInstance = makeNode(101, "nope_gray");
            bool rejected = false;
            try {
                (void)rin::makeDefaultImageNode(unknown, unknownInstance);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            RIN_CHECK_MSG(rejected, "factory: unknown typeId still throws invalid_argument");
        }

        // crop_gray apply golden：8x6 输入、ROI (2,1,3,2) → 输出像素 = 源区域
        // 逐像素搬运（与 Rgba8 版同映射，1 字节/像素）。
        {
            const NodeDescriptor* d = findNodeDescriptor(def, "crop_gray");
            RIN_CHECK(d != nullptr);
            if (d != nullptr) {
                NodeInstance instance =
                    makeNode(20, "crop_gray",
                             {assign("x", ParamValue{static_cast<std::int64_t>(2)}),
                              assign("y", ParamValue{static_cast<std::int64_t>(1)}),
                              assign("width", ParamValue{static_cast<std::int64_t>(3)}),
                              assign("height", ParamValue{static_cast<std::int64_t>(2)})});
                const std::unique_ptr<IImageNode> node =
                    rin::makeDefaultImageNode(*d, instance);
                RIN_CHECK(node != nullptr &&
                          node->descriptor().typeId == "crop_gray");
                if (node != nullptr) {
                    const ImageU8 input = makeGrayImage(8, 6);
                    const std::vector<ImageU8> outputs = node->apply({input});
                    RIN_CHECK(outputs.size() == 1 && outputs[0].valid());
                    RIN_CHECK(outputs.size() == 1 &&
                              outputs[0].format() == PortType::Gray8);
                    RIN_CHECK_EQ(outputs[0].width(), std::uint32_t{3});
                    RIN_CHECK_EQ(outputs[0].height(), std::uint32_t{2});
                    for (std::uint32_t y = 0; y < 2; ++y) {
                        for (std::uint32_t x = 0; x < 3; ++x) {
                            const std::uint8_t got =
                                outputs[0].row(y)[x];  // 输出紧凑（stride == width）。
                            const std::uint8_t want =
                                static_cast<std::uint8_t>(((1 + y) * 8 + (2 + x)) & 0xFF);
                            RIN_CHECK_EQ(got, want);
                        }
                    }
                }
            }
        }

        // downscale_gray nearest golden：4x4 → 2x2（面积覆盖采样：src = 1, 3；
        // pixel(y,x) = y*4+x → out(0,0)=in(1,1)=5、out(1,0)=in(1,3)=7、
        // out(0,1)=in(3,1)=13、out(1,1)=in(3,3)=15）。
        {
            const NodeDescriptor* d = findNodeDescriptor(def, "downscale_gray");
            RIN_CHECK(d != nullptr);
            if (d != nullptr) {
                NodeInstance instance =
                    makeNode(21, "downscale_gray",
                             {assign("interpolation", ParamValue{std::string("nearest")}),
                              assign("scale", ParamValue{0.5})});
                const std::unique_ptr<IImageNode> node =
                    rin::makeDefaultImageNode(*d, instance);
                RIN_CHECK(node != nullptr &&
                          node->descriptor().typeId == "downscale_gray");
                if (node != nullptr) {
                    const ImageU8 input = makeGrayImage(4, 4);
                    const std::vector<ImageU8> outputs = node->apply({input});
                    RIN_CHECK(outputs.size() == 1 && outputs[0].valid());
                    RIN_CHECK(outputs.size() == 1 &&
                              outputs[0].format() == PortType::Gray8);
                    RIN_CHECK_EQ(outputs[0].width(), std::uint32_t{2});
                    RIN_CHECK_EQ(outputs[0].height(), std::uint32_t{2});
                    const std::uint8_t expected[2][2] = {{5, 7}, {13, 15}};
                    for (std::uint32_t y = 0; y < 2; ++y) {
                        for (std::uint32_t x = 0; x < 2; ++x) {
                            RIN_CHECK_EQ(outputs[0].row(y)[x], expected[y][x]);
                        }
                    }
                }
            }
        }

        // 跨类型连线：source_depth_gray（Gray8 输出）→ grayify（声明 Rgba8 输入）
        // 报 TypeMismatch（挂到消费节点）。
        {
            WorkflowGraph g;
            g.nodes = {makeNode(1, "source_depth_gray"), makeNode(2, "grayify")};
            g.connections = {conn(1, 0, 2, 0)};
            const WorkflowValidation v = validateWorkflowGraph(g, def);
            RIN_CHECK(!v.ok);
            RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::TypeMismatch, 2),
                         std::size_t{1});
        }

        // 合法灰度链：source_depth_gray → crop_gray → gaussian_blur 通过校验。
        {
            WorkflowGraph g;
            g.nodes = {
                makeNode(1, "source_depth_gray"),
                makeNode(2, "crop_gray",
                         {assign("x", ParamValue{static_cast<std::int64_t>(0)}),
                          assign("y", ParamValue{static_cast<std::int64_t>(0)}),
                          assign("width", ParamValue{static_cast<std::int64_t>(16)}),
                          assign("height", ParamValue{static_cast<std::int64_t>(12)})}),
                makeNode(3, "gaussian_blur"),
            };
            g.connections = {conn(1, 0, 2, 0), conn(2, 0, 3, 0)};
            const WorkflowValidation v = validateWorkflowGraph(g, def);
            RIN_CHECK_MSG(v.ok, "catalog: gray chain source_depth_gray->crop_gray->"
                                "gaussian_blur validates");
        }
    }

    // --- 16) M12/CR-15 回归：参数匹配器单一实现（DEC-013）与 Integer 2^53 边界 ---
    // 被测面：validateWorkflowGraph 参数面（src/core/workflow_types.cpp 现经
    // src/core/param_match.hpp 的 workflow_detail::paramValueMatches 判定）与
    // src/workflow/param_check.hpp 转发头暴露的同一匹配器（引擎
    // requestParamUpdate / 帧边界防御复核共用）。修复前参数面是本地
    // paramValueInRange：Integer 值先 static_cast<double> 再比边界，2^53 以上
    // 奇数值被量化吞掉；引擎侧实现本就是 long double 精确比较，两路语义分叉。
    {
        // 16.a/16.b) Integer 大整数边界（CR-15 验收数值）。
        // 契约事实：ParamDescriptor::minValue/maxValue 是 double
        // （include/rin/workflow_types.hpp）。2^53+1 = 9007199254740993 在
        // double 中不可精确表示，存储量化为 2^53 = 9007199254740992.0；
        // 2^53+2 = 9007199254740994 精确。
        ParamDescriptor big;
        big.id = "big";
        big.label = "大整数";
        big.kind = ParamKind::Integer;
        big.defaultValue = static_cast<std::int64_t>(9007199254740993LL);
        big.hasRange = true;
        big.minValue = 9007199254740993.0;  // 存储为 9007199254740992.0（2^53）
        big.maxValue = 9007199254740994.0;  // 2^53+2，精确

        NodeDescriptor bigNode;
        bigNode.typeId = "bignum";
        bigNode.displayName = "大整数节点";
        bigNode.outputs = {PortType::Rgba8};
        bigNode.params = {big};
        NodeCatalog bigCatalog;
        bigCatalog.nodes = {makeSource(), bigNode};
        RIN_CHECK(bigCatalog.valid());

        // 单节点无连线：参数面是唯一可能的 issue 来源。
        const auto bigVerdict = [&bigCatalog](std::int64_t v) {
            WorkflowGraph g;
            g.nodes = {makeNode(1, "bignum", {assign("big", ParamValue{v})})};
            return validateWorkflowGraph(g, bigCatalog);
        };

        // 16.a 契约文档化断言（CR-15 1a 撤销，主循环裁决 2026-10-07）：赋值
        // 2^53 被接受——Integer 端点为 double（include/rin/workflow_types.hpp），
        // 声明 2^53+1 时存储即量化为 2^53，赋值 2^53 恰等于量化后的存储端点。
        // 端点精度问题登记为 CR-47；如需精确端点须 DEC 变更契约（int64 端点），
        // 不在 M12 范围。修复前后的参数面判定对这组数值一致（非本次行为变更点，
        // 仅固化契约事实防回归漂移）。
        {
            RIN_CHECK_MSG(big.minValue == 9007199254740992.0,
                          "CR-47: declared Integer min 2^53+1 quantizes to 2^53 (double endpoint)");
            const WorkflowValidation v =
                bigVerdict(static_cast<std::int64_t>(9007199254740992LL));
            RIN_CHECK_MSG(v.ok,
                          "CR-15 1a (revoked): Integer 2^53 equals the quantized stored min "
                          "2^53 and is accepted under the double-endpoint contract");
            RIN_CHECK(v.issues.empty());
        }
        // 16.b 验收（CR-15 1b）：恰为声明 min / max 的值接受；越上界最小步长拒绝。
        RIN_CHECK(bigVerdict(static_cast<std::int64_t>(9007199254740993LL)).ok);
        RIN_CHECK(bigVerdict(static_cast<std::int64_t>(9007199254740994LL)).ok);
        {
            const WorkflowValidation v =
                bigVerdict(static_cast<std::int64_t>(9007199254740995LL));
            RIN_CHECK(!v.ok);
            RIN_CHECK_EQ(countIssues(v, ValidationIssueKind::BadParam, 1), std::size_t{1});
        }

        // 16.c 判别用例（行为修复的真实分界）：端点可被 double 精确表示时，
        // 值侧不再量化——紧贴端点的奇数值与修复前 double 实现判定不同
        // （修复前：值先经 double 量化被吸入端点内 → 越界被错误接受）。
        // 同时守卫"不过度收紧"：区间内奇数值仍接受。
        {
            ParamDescriptor below;
            below.id = "big";
            below.label = "大整数";
            below.kind = ParamKind::Integer;
            below.defaultValue = static_cast<std::int64_t>(18014398509481984LL);  // 2^54
            below.hasRange = true;
            below.minValue = 18014398509481984.0;  // 2^54，精确
            below.maxValue = 3.0e16;
            ParamDescriptor above = below;
            above.minValue = 0.0;
            above.maxValue = 9007199254740996.0;  // 2^53+4，精确

            NodeDescriptor belowNode = bigNode;
            belowNode.typeId = "bignum_below";
            belowNode.params = {below};
            NodeDescriptor aboveNode = bigNode;
            aboveNode.typeId = "bignum_above";
            aboveNode.params = {above};
            NodeCatalog divCatalog;
            divCatalog.nodes = {makeSource(), belowNode, aboveNode};
            RIN_CHECK(divCatalog.valid());

            const auto divVerdict = [&divCatalog](const std::string& typeId, std::int64_t v) {
                WorkflowGraph g;
                g.nodes = {makeNode(1, typeId, {assign("big", ParamValue{v})})};
                return validateWorkflowGraph(g, divCatalog).ok;
            };
            // min=2^54、值 2^54−1：修复前 double(2^54−1) 量化为 2^54 被错误接受。
            RIN_CHECK_MSG(!divVerdict("bignum_below",
                                      static_cast<std::int64_t>(18014398509481983LL)),
                          "CR-15: Integer 2^54-1 must be rejected against exact min 2^54");
            // max=2^53+4、值 2^53+5：修复前量化为 2^53+4 被错误接受。
            RIN_CHECK_MSG(!divVerdict("bignum_above",
                                      static_cast<std::int64_t>(9007199254740997LL)),
                          "CR-15: Integer 2^53+5 must be rejected against exact max 2^53+4");
            // 区间 [0, 2^53+4] 内的奇数值 2^53+1：合法，不得过度收紧。
            RIN_CHECK(divVerdict("bignum_above", static_cast<std::int64_t>(9007199254740993LL)));
            // 区间内的偶数值 2^53+2 / 端点 2^53+4：合法。
            RIN_CHECK(divVerdict("bignum_above", static_cast<std::int64_t>(9007199254740994LL)));
            RIN_CHECK(divVerdict("bignum_above", static_cast<std::int64_t>(9007199254740996LL)));
        }

        // 16.d 双路径一致性（CR-15 1c）+ 常规语义回归（CR-15 1d）：
        // 同一用例经 validateWorkflowGraph（Core 判据）与经转发头的
        // workflow_detail::paramValueMatches（引擎判据）判定一致；常规
        // Boolean/Real/Enumeration/RealArray 接受/拒绝语义与修复前一致。
        {
            // 探针节点：无输入（无 DanglingInput 噪声）、Rgba8 输出、携带
            // blur 的四种参数声明 + 独立 Boolean 声明节点。
            NodeDescriptor paramProbe = makeBlur();
            paramProbe.typeId = "paramprobe";
            paramProbe.displayName = "参数探针";
            paramProbe.inputs.clear();
            paramProbe.outputs = {PortType::Rgba8};

            ParamDescriptor boolD;
            boolD.id = "flag";
            boolD.label = "开关";
            boolD.kind = ParamKind::Boolean;
            boolD.defaultValue = true;
            NodeDescriptor boolProbe = paramProbe;
            boolProbe.typeId = "boolprobe";
            boolProbe.displayName = "开关探针";
            boolProbe.params = {boolD};

            NodeCatalog probeCatalog;
            probeCatalog.nodes = {makeSource(), paramProbe, boolProbe};
            RIN_CHECK(probeCatalog.valid());

            // 目录定型后再取描述符引用（避免 push_back 重悬垂）。
            const ParamDescriptor& radiusD = probeCatalog.nodes[1].params[0];
            const ParamDescriptor& strengthD = probeCatalog.nodes[1].params[1];
            const ParamDescriptor& modeD = probeCatalog.nodes[1].params[2];
            const ParamDescriptor& kernelD = probeCatalog.nodes[1].params[3];
            const ParamDescriptor& flagD = probeCatalog.nodes[2].params[0];

            const auto agree = [&probeCatalog](const std::string& typeId,
                                               const ParamDescriptor& d, const ParamValue& v,
                                               bool expectMatch) {
                WorkflowGraph g;
                g.nodes = {makeNode(1, typeId, {assign(d.id, v)})};
                const bool viaValidate = validateWorkflowGraph(g, probeCatalog).ok;
                const bool viaMatcher = rin::workflow_detail::paramValueMatches(d, v);
                RIN_CHECK_EQ(viaValidate, viaMatcher);  // 双路径判定一致
                RIN_CHECK_EQ(viaMatcher, expectMatch);  // 期望语义（常规面回归）
            };
            const std::string probeType = "paramprobe";
            const std::string boolType = "boolprobe";

            // 种类错配（五种各一组）：两路径一致且都拒绝。
            agree(probeType, radiusD, ParamValue{true}, false);  // bool vs Integer
            agree(probeType, strengthD, ParamValue{static_cast<std::int64_t>(1)},
                  false);                                       // int64 vs Real
            agree(probeType, modeD, ParamValue{0.5}, false);    // double vs Enumeration
            agree(probeType, kernelD, ParamValue{std::string("x")}, false);  // string vs RealArray
            agree(boolType, flagD, ParamValue{static_cast<std::int64_t>(1)},
                  false);  // int64 vs Boolean
            // 越界（Integer/Real 两侧界外）：一致且拒绝。
            agree(probeType, radiusD, ParamValue{static_cast<std::int64_t>(0)}, false);
            agree(probeType, radiusD, ParamValue{static_cast<std::int64_t>(11)}, false);
            agree(probeType, strengthD, ParamValue{-0.5}, false);
            agree(probeType, strengthD, ParamValue{1.5}, false);
            // 界内：一致且接受。
            agree(probeType, radiusD, ParamValue{static_cast<std::int64_t>(5)}, true);
            agree(probeType, strengthD, ParamValue{0.25}, true);
            // 枚举：合法选项接受、越选项拒绝。
            agree(probeType, modeD, ParamValue{std::string("quality")}, true);
            agree(probeType, modeD, ParamValue{std::string("ultra")}, false);
            // RealArray：合法数组接受、含 NaN/Inf 拒绝。
            agree(probeType, kernelD, ParamValue{std::vector<double>{0.25, 0.5, 0.25}}, true);
            agree(probeType, kernelD, ParamValue{std::vector<double>{0.5, kNaN}}, false);
            agree(probeType, kernelD, ParamValue{std::vector<double>{kInf}}, false);
            // Boolean：true 接受。
            agree(boolType, flagD, ParamValue{true}, true);
            // 无范围（hasRange=false）：种类匹配即接受、错种拒绝。
            ParamDescriptor freeInt = radiusD;
            freeInt.hasRange = false;
            WorkflowGraph freeGraph;
            freeGraph.nodes = {makeNode(1, "paramprobe",
                                        {assign("radius", ParamValue{static_cast<std::int64_t>(-5)})})};
            NodeCatalog freeCatalog;
            freeCatalog.nodes = {makeSource(), [&] {
                                     NodeDescriptor n = paramProbe;
                                     n.params = {freeInt};
                                     return n;
                                 }()};
            RIN_CHECK(validateWorkflowGraph(freeGraph, freeCatalog).ok);
            RIN_CHECK(rin::workflow_detail::paramValueMatches(freeInt,
                                                              ParamValue{static_cast<std::int64_t>(-5)}));
            RIN_CHECK(!rin::workflow_detail::paramValueMatches(freeInt, ParamValue{true}));

            // 大整数边界四值同样双路径一致（判定值本身由 16.a/16.b 锁定）。
            for (const std::int64_t v :
                 {9007199254740992LL, 9007199254740993LL, 9007199254740994LL,
                  9007199254740995LL}) {
                const bool viaValidate = bigVerdict(v).ok;
                const bool viaMatcher =
                    rin::workflow_detail::paramValueMatches(big, ParamValue{v});
                RIN_CHECK_EQ(viaValidate, viaMatcher);
            }

            // 失败映射保持：拒绝用例恰为一条 BadParam，落在赋值节点。
            WorkflowGraph badGraph;
            badGraph.nodes = {makeNode(1, "paramprobe",
                                       {assign("radius", ParamValue{true})})};
            const WorkflowValidation bad = validateWorkflowGraph(badGraph, probeCatalog);
            RIN_CHECK(!bad.ok);
            RIN_CHECK_EQ(bad.issues.size(), std::size_t{1});
            RIN_CHECK_EQ(countIssues(bad, ValidationIssueKind::BadParam, 1), std::size_t{1});
        }
    }

    return rin_test::exitStatus();
}
