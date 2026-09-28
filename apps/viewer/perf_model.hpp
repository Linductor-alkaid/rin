#pragma once

// 性能面板统计管道的平台无关纯逻辑层（M5-05，ui_workspace_design.md §5.7/§4/§7）。
// 职责边界（DEC-015 决策 2：统计消费语义下沉纯逻辑，独立单测）：
// - 统计快照消费：契约 tryLoadStats 的 sequence 推进（只取 strictly newer 的
//   最新快照，中间快照可跳过——最新态语义），pump 边界有界消费（RULE-05）；
// - 停止/关闭排空（§4"性能面板冻结为末次值并标注已停止"）：与缩略图排空不同，
//   末次值保留呈现，live 标志区分活动/冻结——引擎非 Running 时一律 false，
//   UI 不得以 stale 数据显示活动状态；
// - 节点统计查找：选中节点面板展开（last/avg/执行帧数）与画布节点耗时徽标
//   （avgCostMs）的唯一样属；
// - 数值文本：DEC-005 mono 替代纪律（比例字体无等宽族，统一小数位 + 固定后缀
//   达成列对齐）。
//
// 本头文件不包含 EUI-NEO 类型（纯逻辑单测对象，tests/test_perf_panel.cpp）；
// EUI 组装见 node_canvas.hpp（工具栏徽标/节点耗时徽标）与 param_panel.hpp
// （右面板工作流总览/选中节点耗时展开），消费入口在 app.cpp pump。

#include <rin/workflow_engine.hpp>
#include <rin/workflow_types.hpp>

#include <cstdint>
#include <cstdio>
#include <string>

namespace viewer {

/// 耗时文本（§5.7 节点耗时徽标/面板展开用）：统一一位小数 + 固定 " ms" 后缀。
[[nodiscard]] inline std::string formatCostMs(const double ms) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f ms", ms);
    return buffer;
}

/// 帧率文本（§5.7 工作流总览用）：统一一位小数 + 固定 " fps" 后缀。
[[nodiscard]] inline std::string formatFps(const double fps) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f fps", fps);
    return buffer;
}

/// 性能面板统计管道状态（WorkflowCanvasState 持有，地址稳定供闭包引用）。
///
/// 消费纪律：consume 在 pump 边界调用（每帧一次，非阻塞）；tryLoadStats 只在
/// 有 strictly newer 快照时返回，Running 下逐帧推进，中间快照被最新态语义合并。
/// 停止后引擎不再发布（workflow_engine.hpp stop 契约），末次快照保留呈现——
/// 冻结不是清空：§4 明确"性能面板冻结为末次值并标注已停止"，活动/冻结由
/// live 区分（引擎 state()==Running），stale 数值只在冻结标注下出现。
/// 新会话（重启）后 sequence 续增（引擎实例内单调），消费自然衔接，会话累计
/// 计数按快照原文呈现（start 复位是引擎语义）。
class WorkflowPerfState {
public:
    /// pump 边界消费：拉取 sequence > 已见序号的最新快照并同步活动/冻结标志。
    /// 返回 true 表示可见变更（新快照或活动/冻结切换），调用方需 requestUpdate。
    bool consume(rin::IWorkflowEngine& engine) {
        bool changed = false;
        rin::WorkflowStats snapshot;
        if (engine.tryLoadStats(lastSeenSequence_, snapshot)) {
            stats_ = std::move(snapshot);
            hasStats_ = true;
            changed = true;
        }
        const bool running = engine.state() == rin::WorkflowEngineState::Running;
        if (running != live_) {
            live_ = running;
            changed = true;
        }
        return changed;
    }

    /// 关闭排空（§4 关闭路径）：清空消费态，stale 统计不跨 shutdown 存活。
    /// 已见序号水位保留：引擎 stop 后统计通道保留末次值（workflow_engine.hpp
    /// 契约），水位复位会让排空后的 consume 复活 stale 快照；新会话快照序号
    /// 引擎实例内单调递增，衔接不受影响。clear 后挂全新引擎实例（序号从 1
    /// 重来）不在使用路径内（app.cpp 先 stop+reset 引擎再 clear）。
    void clear() {
        stats_ = rin::WorkflowStats{};
        hasStats_ = false;
        live_ = false;
    }

    /// 最新已消费快照；从未消费返回 nullptr。停止后继续返回末次值（冻结语义）。
    [[nodiscard]] const rin::WorkflowStats* stats() const noexcept {
        return hasStats_ ? &stats_ : nullptr;
    }

    /// 引擎是否活动（Running）；false 时面板呈冻结末次值 + "stopped" 标注。
    [[nodiscard]] bool live() const noexcept { return live_; }

    /// 节点统计查找（面板展开与画布耗时徽标的唯一样属）；无快照或快照不含
    /// 该节点（未运行/不在当前生效图）返回 nullptr。
    [[nodiscard]] const rin::NodeStats* nodeStats(const rin::NodeId node) const noexcept {
        if (!hasStats_) {
            return nullptr;
        }
        for (const rin::NodeStats& entry : stats_.nodes) {
            if (entry.node == node) {
                return &entry;
            }
        }
        return nullptr;
    }

private:
    std::uint64_t lastSeenSequence_ = 0;
    rin::WorkflowStats stats_;
    bool hasStats_ = false;
    bool live_ = false;
};

}  // namespace viewer
