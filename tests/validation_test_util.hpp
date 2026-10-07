#pragma once

// 图校验断言 helper（M12/CR-39；从 test_util.hpp 拆出以保持该头零项目依赖）。
// 依赖 rin 公开契约（workflow_types）。

#include "test_util.hpp"

#include "rin/workflow_types.hpp"

namespace rin_test {

/// 校验问题存在性：图校验结果含指定种类的问题（原三处副本同款 2 参形态；
/// 限定节点的变体见 test_param_panel 的 hasIssueOn，属 viewer 模型专属）。
[[nodiscard]] inline bool hasKind(const rin::WorkflowValidation& validation,
                                  rin::ValidationIssueKind kind) {
    for (const rin::ValidationIssue& issue : validation.issues) {
        if (issue.kind == kind) {
            return true;
        }
    }
    return false;
}

}  // namespace rin_test
