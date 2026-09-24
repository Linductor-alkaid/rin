#include "rin/workflow_types.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace rin {

const char* toString(PortType type) noexcept {
    switch (type) {
        case PortType::Gray8:
            return "Gray8";
        case PortType::Rgba8:
            return "Rgba8";
    }
    return "Unknown";
}

const char* toString(ParamKind kind) noexcept {
    switch (kind) {
        case ParamKind::Boolean:
            return "Boolean";
        case ParamKind::Integer:
            return "Integer";
        case ParamKind::Real:
            return "Real";
        case ParamKind::Enumeration:
            return "Enumeration";
        case ParamKind::RealArray:
            return "RealArray";
    }
    return "Unknown";
}

const char* toString(WorkflowEngineState state) noexcept {
    switch (state) {
        case WorkflowEngineState::Idle:
            return "Idle";
        case WorkflowEngineState::Running:
            return "Running";
        case WorkflowEngineState::Stopping:
            return "Stopping";
        case WorkflowEngineState::Failed:
            return "Failed";
    }
    return "Unknown";
}

ParamKind paramKindOf(const ParamValue& value) noexcept {
    // variant 备择顺序即 ParamKind 声明顺序（bool/int64/double/string/vector<double>）。
    return static_cast<ParamKind>(value.index());
}

namespace {

bool allFinite(const std::vector<double>& values) noexcept {
    return std::all_of(values.begin(), values.end(),
                       [](double v) { return std::isfinite(v); });
}

}  // namespace

bool ParamDescriptor::valid() const noexcept {
    if (id.empty()) {
        return false;
    }
    if (paramKindOf(defaultValue) != kind) {
        return false;
    }
    if (hasRange && kind != ParamKind::Integer && kind != ParamKind::Real) {
        return false;
    }
    if (hasRange &&
        (std::isnan(minValue) || std::isnan(maxValue) || minValue > maxValue)) {
        // NaN 与任何值比较均为 false，会绕过 min>max 检查并使一切赋值被判越界；
        // ±Inf 端点允许（语义为对应侧不限）。
        return false;
    }
    if (kind == ParamKind::Enumeration) {
        if (enumOptions.empty()) {
            return false;
        }
        const auto& selected = std::get<std::string>(defaultValue);
        if (!std::any_of(enumOptions.begin(), enumOptions.end(),
                         [&selected](const std::string& option) {
                             return option == selected;
                         })) {
            return false;
        }
    }
    if (kind == ParamKind::RealArray) {
        return allFinite(std::get<std::vector<double>>(defaultValue));
    }
    return true;
}

bool NodeDescriptor::valid() const noexcept {
    if (typeId.empty() || displayName.empty()) {
        return false;
    }
    return std::all_of(params.begin(), params.end(),
                       [](const ParamDescriptor& param) { return param.valid(); });
}

bool NodeCatalog::valid() const noexcept {
    if (nodes.empty()) {
        return false;
    }
    std::unordered_set<std::string> seen;
    seen.reserve(nodes.size());
    for (const NodeDescriptor& node : nodes) {
        if (!node.valid() || !seen.insert(node.typeId).second) {
            return false;
        }
    }
    return true;
}

const NodeDescriptor* findNodeDescriptor(const NodeCatalog& catalog,
                                         const std::string& typeId) noexcept {
    const auto it = std::find_if(catalog.nodes.begin(), catalog.nodes.end(),
                                 [&typeId](const NodeDescriptor& node) {
                                     return node.typeId == typeId;
                                 });
    return it == catalog.nodes.end() ? nullptr : &*it;
}

bool NodeOutputSnapshot::valid() const noexcept {
    if (node == kInvalidNode || pixels == nullptr || width == 0 || height == 0) {
        return false;
    }
    // 64 位乘法：Rgba8 的 width*4 在 uint32 下会回绕（width=2^30 时积为 0），
    // 使任何 stride 都满足比较；最小行宽超出 uint32 时任何 uint32 stride 必不满足。
    const std::uint64_t minStride =
        format == PortType::Rgba8 ? static_cast<std::uint64_t>(width) * 4u : width;
    return stride >= minStride &&
           pixels->size() >= static_cast<std::size_t>(stride) * height;
}

namespace {

bool paramValueInRange(const ParamDescriptor& descriptor, const ParamValue& value) noexcept {
    if (!descriptor.hasRange) {
        return true;
    }
    if (descriptor.kind == ParamKind::Integer) {
        const auto number = std::get<std::int64_t>(value);
        return static_cast<double>(number) >= descriptor.minValue &&
               static_cast<double>(number) <= descriptor.maxValue;
    }
    if (descriptor.kind == ParamKind::Real) {
        const auto number = std::get<double>(value);
        return number >= descriptor.minValue && number <= descriptor.maxValue;
    }
    return true;
}

bool paramAssignmentValid(const ParamDescriptor& descriptor, const ParamAssignment& assignment,
                          ValidationIssueKind* issueKind) {
    if (paramKindOf(assignment.value) != descriptor.kind) {
        *issueKind = ValidationIssueKind::BadParam;
        return false;
    }
    if (!paramValueInRange(descriptor, assignment.value)) {
        *issueKind = ValidationIssueKind::BadParam;
        return false;
    }
    if (descriptor.kind == ParamKind::Enumeration) {
        const auto& selected = std::get<std::string>(assignment.value);
        if (!std::any_of(descriptor.enumOptions.begin(), descriptor.enumOptions.end(),
                         [&selected](const std::string& option) {
                             return option == selected;
                         })) {
            *issueKind = ValidationIssueKind::BadParam;
            return false;
        }
    }
    if (descriptor.kind == ParamKind::RealArray) {
        if (!allFinite(std::get<std::vector<double>>(assignment.value))) {
            *issueKind = ValidationIssueKind::BadParam;
            return false;
        }
    }
    return true;
}

}  // namespace

WorkflowValidation validateWorkflowGraph(const WorkflowGraph& graph,
                                         const NodeCatalog& catalog) {
    WorkflowValidation result;
    auto reject = [&result](ValidationIssueKind kind, NodeId node, std::string message) {
        result.ok = false;
        result.issues.push_back({kind, node, std::move(message)});
    };

    std::unordered_map<NodeId, const NodeDescriptor*> descriptors;
    descriptors.reserve(graph.nodes.size());

    // 节点面：id、唯一性、类型存在性、参数赋值。
    std::unordered_set<NodeId> ids;
    ids.reserve(graph.nodes.size());
    for (const NodeInstance& node : graph.nodes) {
        if (node.id == kInvalidNode) {
            reject(ValidationIssueKind::InvalidNodeId, node.id,
                   "节点 id 为保留的无效值 0");
            continue;
        }
        if (!ids.insert(node.id).second) {
            reject(ValidationIssueKind::DuplicateNodeId, node.id,
                   "节点 id 在图内重复");
            continue;
        }
        const NodeDescriptor* descriptor = findNodeDescriptor(catalog, node.typeId);
        if (descriptor == nullptr) {
            reject(ValidationIssueKind::UnknownNodeType, node.id,
                   "未知节点类型: " + node.typeId);
            continue;
        }
        descriptors.emplace(node.id, descriptor);

        std::unordered_set<std::string> assigned;
        for (const ParamAssignment& assignment : node.params) {
            const auto it = std::find_if(
                descriptor->params.begin(), descriptor->params.end(),
                [&assignment](const ParamDescriptor& param) {
                    return param.id == assignment.paramId;
                });
            if (it == descriptor->params.end()) {
                reject(ValidationIssueKind::BadParam, node.id,
                       "未知参数: " + assignment.paramId);
                continue;
            }
            if (!assigned.insert(assignment.paramId).second) {
                reject(ValidationIssueKind::BadParam, node.id,
                       "参数重复赋值: " + assignment.paramId);
                continue;
            }
            ValidationIssueKind issueKind = ValidationIssueKind::BadParam;
            if (!paramAssignmentValid(*it, assignment, &issueKind)) {
                reject(issueKind, node.id, "参数值非法: " + assignment.paramId);
            }
        }
    }

    // 连线面：方向、端点存在性、端口序号、类型一致、重复/多驱动/自环。
    struct PortRefHash {
        std::size_t operator()(const PortRef& ref) const noexcept {
            const std::size_t direction =
                ref.direction == PortDirection::Output ? 1u : 0u;
            return std::hash<std::uint64_t>{}(ref.node) ^
                   (std::hash<std::uint32_t>{}(ref.index) << 32) ^ direction;
        }
    };
    std::unordered_set<PortRef, PortRefHash> drivenInputs;
    for (const Connection& connection : graph.connections) {
        if (connection.from.direction != PortDirection::Output ||
            connection.to.direction != PortDirection::Input) {
            reject(ValidationIssueKind::DirectionMismatch, connection.to.node,
                   "连线方向非法（from 须为输出端口，to 须为输入端口）");
            continue;
        }
        const auto fromIt = descriptors.find(connection.from.node);
        const auto toIt = descriptors.find(connection.to.node);
        if (fromIt == descriptors.end() || toIt == descriptors.end()) {
            reject(ValidationIssueKind::UnknownConnectionNode,
                   fromIt == descriptors.end() ? connection.from.node
                                               : connection.to.node,
                   "连线引用了图中不存在的节点");
            continue;
        }
        if (connection.from.index >= fromIt->second->outputs.size()) {
            reject(ValidationIssueKind::PortOutOfRange, connection.from.node,
                   "输出端口序号越界");
            continue;
        }
        if (connection.to.index >= toIt->second->inputs.size()) {
            reject(ValidationIssueKind::PortOutOfRange, connection.to.node,
                   "输入端口序号越界");
            continue;
        }
        if (fromIt->second->outputs[connection.from.index] !=
            toIt->second->inputs[connection.to.index]) {
            reject(ValidationIssueKind::TypeMismatch, connection.to.node,
                   std::string("端口类型不匹配: ") +
                       toString(fromIt->second->outputs[connection.from.index]) + " -> " +
                       toString(toIt->second->inputs[connection.to.index]));
            continue;
        }
        if (connection.from.node == connection.to.node) {
            reject(ValidationIssueKind::SelfLoop, connection.to.node,
                   "连线构成自环");
            continue;
        }
        if (!drivenInputs.insert(connection.to).second) {
            reject(ValidationIssueKind::MultipleDrivers, connection.to.node,
                   "输入端口被多条连线驱动");
            continue;
        }
    }

    // 悬空输入：目录声明的每个输入端口必须恰有一条入边（悬空输出允许）。
    for (const NodeInstance& node : graph.nodes) {
        const auto it = descriptors.find(node.id);
        if (it == descriptors.end()) {
            continue;  // 该节点已在节点面报告过问题。
        }
        std::vector<bool> driven(it->second->inputs.size(), false);
        for (const Connection& connection : graph.connections) {
            if (connection.to.node == node.id &&
                connection.to.direction == PortDirection::Input &&
                connection.to.index < driven.size()) {
                driven[connection.to.index] = true;
            }
        }
        for (std::size_t index = 0; index < driven.size(); ++index) {
            if (!driven[index]) {
                reject(ValidationIssueKind::DanglingInput, node.id,
                       "输入端口悬空: #" + std::to_string(index));
            }
        }
    }

    // 成环检测（Kahn 剥离）：入度 = 有效连线数；逐层剥离入度 0 的节点。
    {
        std::unordered_map<NodeId, std::size_t> inDegree;
        std::unordered_map<NodeId, std::vector<NodeId>> adjacency;
        inDegree.reserve(descriptors.size());
        for (const auto& entry : descriptors) {
            inDegree.emplace(entry.first, 0);
        }
        for (const Connection& connection : graph.connections) {
            if (inDegree.count(connection.from.node) != 0 &&
                inDegree.count(connection.to.node) != 0) {
                adjacency[connection.from.node].push_back(connection.to.node);
                ++inDegree[connection.to.node];
            }
        }
        std::vector<NodeId> ready;
        for (const auto& entry : inDegree) {
            if (entry.second == 0) {
                ready.push_back(entry.first);
            }
        }
        std::size_t consumed = 0;
        while (!ready.empty()) {
            const NodeId node = ready.back();
            ready.pop_back();
            ++consumed;
            const auto it = adjacency.find(node);
            if (it == adjacency.end()) {
                continue;
            }
            for (const NodeId next : it->second) {
                if (--inDegree[next] == 0) {
                    ready.push_back(next);
                }
            }
        }
        if (consumed != inDegree.size()) {
            reject(ValidationIssueKind::Cycle, kInvalidNode, "图存在环");
        }
    }

    result.ok = result.issues.empty();
    return result;
}

}  // namespace rin
