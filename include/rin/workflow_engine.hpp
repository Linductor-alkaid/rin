#pragma once

#include "rin/workflow_types.hpp"

namespace rin {

/// 工作流引擎契约（M4-09，DEC-016）：运行控制入口 + 有界最新态数据通道。
///
/// 公开头零第三方类型（RULE-01）；实现方内部用 executor::comm 邮箱承载
/// （EXEC-07：有限任务 + 有界在飞 + 显式丢弃）。M5-08 假引擎与 M4-07 真引擎
/// 实现同一契约，共用契约测试（DEC-016）。
///
/// 线程模型：applyGraph/start/stop/requestParamUpdate 供 owner 线程（主线程）
/// 调用；tryLoad* 供 UI 线程非阻塞调用；两者与执行上下文之间全部经有界通道。
class IWorkflowEngine {
public:
    virtual ~IWorkflowEngine() = default;

    /// 节点目录（调色板与参数 schema 的唯一来源；构建期确定，运行期不变）。
    [[nodiscard]] virtual const NodeCatalog& catalog() const = 0;

    /// 校验并应用图。同步执行 validateWorkflowGraph：不通过时返回校验结果且
    /// 当前生效图不变；通过时 Idle 下存为待运行图（start 时生效），Running 下
    /// 入队，引擎在帧边界排空重建后经 GraphApplied 事件报告。
    [[nodiscard]] virtual WorkflowValidation applyGraph(const WorkflowGraph& graph) = 0;

    /// 参数热更新（下一帧生效语义，DEC-013）；未知节点/参数或类型不匹配时同步
    /// 拒绝（返回 false 并填充 error），成功入队后经 ParamUpdated 事件报告。
    virtual bool requestParamUpdate(NodeId node, const std::string& paramId,
                                    const ParamValue& value,
                                    std::string* error = nullptr) = 0;

    /// Idle -> Running（需已应用有效图，否则拒绝）。返回 admitted=false 时不
    /// 改变状态；运行期失败经事件通道进入 Failed。
    [[nodiscard]] virtual AdmissionResult start() = 0;

    /// 任意状态 -> Stopping -> Idle；幂等；阻塞至执行任务回收完成。
    ///
    /// 排空语义与 ICameraService::stop 一致：返回后不再有新发布；通道内保留的
    /// 最新值对既有消费方保持 stale 语义（序号不回退），调用方不得据此恢复活动
    /// 状态——UI 派生状态（画布输出缩略图/性能面板）应在关闭路径显式排空。
    virtual void stop() = 0;

    [[nodiscard]] virtual WorkflowEngineState state() const = 0;
    [[nodiscard]] virtual std::string lastError() const = 0;

    /// 取 sequence > lastSeenSequence 的最新统计快照；无新快照返回 false 且
    /// 出参/序号保持不动。
    [[nodiscard]] virtual bool tryLoadStats(std::uint64_t& lastSeenSequence,
                                            WorkflowStats& out) = 0;

    /// 取指定节点 sequence > lastSeenSequence 的最新中间产物；无新产物返回 false。
    /// 引擎对当前生效图中已执行的节点保留最新一幅产物（悬空输出端口的末端节点
    /// 同样保留可查看——workflow_types.hpp 连线语义与 ui_workspace_design.md
    /// §5.6 "末端结果预览"）；节点未运行、尚未产生产物或不在当前生效图（如图
    /// 重建后移除）时返回 false。
    [[nodiscard]] virtual bool tryLoadNodeOutput(NodeId node,
                                                 std::uint64_t& lastSeenSequence,
                                                 NodeOutputSnapshot& out) = 0;

    /// 最新事件快照（无序号语义；调用方只关心最近一条）。
    [[nodiscard]] virtual bool tryLoadEvent(WorkflowEvent& out) = 0;
};

}  // namespace rin
