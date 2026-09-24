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
//
// DOD-02 适用性说明：本契约面全部为单线程纯逻辑值语义（valid() 判定与
// validateWorkflowGraph 纯函数，无任务提交/队列/取消/超时/shutdown 语义，
// 无跨上下文共享状态），并发矩阵不适用（写法参照 test_pose_math.cpp 文件头）；
// 契约数据的通道承载（executor::comm 选型）属实现细节，不在本测试范围。
//
// 说明：标注"契约边界探针"的用例按冻结契约的严格语义断言（min<=max、
// Rgba8 最小行宽 width*4 的数学值）；若实现与其矛盾，失败即实现问题证据，
// 由主循环裁决修复，测试不迁就实现。
#include "test_util.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "rin/workflow_types.hpp"

namespace {

using rin::Connection;
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

    return rin_test::exitStatus();
}
