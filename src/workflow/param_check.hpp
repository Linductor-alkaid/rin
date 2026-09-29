#pragma once

#include "rin/workflow_types.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace rin::workflow_detail {

/// 参数值与声明的种类/约束匹配（规则与 validateWorkflowGraph 参数面一致）。
/// 两引擎共享的单一事实源（DEC-013）：requestParamUpdate 同步拒绝与帧边界
/// 应用期的防御复核使用同一判定。
[[nodiscard]] inline bool paramValueMatches(const ParamDescriptor& d,
                                            const ParamValue& value) {
    switch (d.kind) {
    case ParamKind::Boolean:
        return std::holds_alternative<bool>(value);
    case ParamKind::Integer: {
        const auto* i = std::get_if<std::int64_t>(&value);
        if (i == nullptr) {
            return false;
        }
        if (!d.hasRange) {
            return true;
        }
        // long double 比较：int64 边界不被 double 量化吞掉。
        const long double x = static_cast<long double>(*i);
        return x >= static_cast<long double>(d.minValue) &&
               x <= static_cast<long double>(d.maxValue);
    }
    case ParamKind::Real: {
        const auto* r = std::get_if<double>(&value);
        if (r == nullptr) {
            return false;
        }
        if (!d.hasRange) {
            return true;
        }
        // NaN 与任何比较均为 false，自然拒绝（hasRange 时）。
        return *r >= d.minValue && *r <= d.maxValue;
    }
    case ParamKind::Enumeration: {
        const auto* s = std::get_if<std::string>(&value);
        if (s == nullptr) {
            return false;
        }
        return std::find(d.enumOptions.begin(), d.enumOptions.end(), *s) !=
               d.enumOptions.end();
    }
    case ParamKind::RealArray: {
        const auto* a = std::get_if<std::vector<double>>(&value);
        if (a == nullptr) {
            return false;
        }
        return std::all_of(a->begin(), a->end(),
                           [](double x) { return std::isfinite(x); });
    }
    }
    return false;
}

/// 按 paramId 查节点类型内的参数声明；不存在返回 nullptr。
[[nodiscard]] inline const ParamDescriptor* findParamDescriptor(
    const NodeDescriptor& descriptor, const std::string& paramId) {
    for (const ParamDescriptor& p : descriptor.params) {
        if (p.id == paramId) {
            return &p;
        }
    }
    return nullptr;
}

/// 按 id 查图内节点实例（可变）；不存在返回 nullptr。
[[nodiscard]] inline NodeInstance* findNodeInstance(WorkflowGraph& graph,
                                                    NodeId node) {
    for (NodeInstance& instance : graph.nodes) {
        if (instance.id == node) {
            return &instance;
        }
    }
    return nullptr;
}

[[nodiscard]] inline const NodeInstance* findNodeInstance(const WorkflowGraph& graph,
                                                          NodeId node) {
    for (const NodeInstance& instance : graph.nodes) {
        if (instance.id == node) {
            return &instance;
        }
    }
    return nullptr;
}

}  // namespace rin::workflow_detail
