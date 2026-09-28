#include "rin/image_node.hpp"

namespace rin {

const ParamValue* effectiveParamValue(const NodeDescriptor& descriptor,
                                      const NodeInstance& instance,
                                      const std::string& paramId) {
    for (const auto& param : descriptor.params) {
        if (param.id == paramId) {
            // 实例赋值查晚于声明：最后一条赋值生效（图校验已拒绝重复，防御语义）。
            for (auto it = instance.params.rbegin(); it != instance.params.rend(); ++it) {
                if (it->paramId == paramId) {
                    return &it->value;
                }
            }
            return &param.defaultValue;
        }
    }
    return nullptr;
}

namespace {

/// 类型化读取的共同骨架：声明存在、值种类与请求 kind 一致才取出。
template <typename T>
std::optional<T> typedParam(const NodeDescriptor& descriptor, const NodeInstance& instance,
                            const std::string& paramId) {
    const ParamValue* value = effectiveParamValue(descriptor, instance, paramId);
    if (value == nullptr) {
        return std::nullopt;
    }
    if (const auto* typed = std::get_if<T>(value)) {
        return *typed;
    }
    return std::nullopt;
}

}  // namespace

std::optional<bool> paramBoolean(const NodeDescriptor& descriptor,
                                 const NodeInstance& instance,
                                 const std::string& paramId) {
    return typedParam<bool>(descriptor, instance, paramId);
}

std::optional<std::int64_t> paramInteger(const NodeDescriptor& descriptor,
                                         const NodeInstance& instance,
                                         const std::string& paramId) {
    return typedParam<std::int64_t>(descriptor, instance, paramId);
}

std::optional<double> paramReal(const NodeDescriptor& descriptor,
                                const NodeInstance& instance, const std::string& paramId) {
    return typedParam<double>(descriptor, instance, paramId);
}

std::optional<std::string> paramEnumeration(const NodeDescriptor& descriptor,
                                            const NodeInstance& instance,
                                            const std::string& paramId) {
    return typedParam<std::string>(descriptor, instance, paramId);
}

std::optional<std::vector<double>> paramRealArray(const NodeDescriptor& descriptor,
                                                  const NodeInstance& instance,
                                                  const std::string& paramId) {
    return typedParam<std::vector<double>>(descriptor, instance, paramId);
}

}  // namespace rin
