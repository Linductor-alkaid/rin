// M5-08 工作流引擎契约测试套件（DEC-016 契约先行）：IWorkflowEngine 公开契约
// （include/rin/workflow_engine.hpp + include/rin/workflow_types.hpp）的引擎无关
// 检查集，以引擎工厂参数化（WorkflowEngineFixture）；M5-08 假引擎与 M4-07 真引擎
// 共用同一套检查，替换实现不破坏本套件。
//
// 被测面（只断言契约陈述的语义，不依赖实现细节；图用 engine->catalog() 泛式构造：
// 无输入节点为源 + 单输入类型匹配节点链接，末端悬空输出）：
// - 生命周期/控制面：无图 start 拒绝且状态不变；非法图 applyGraph 同步拒绝
//   （!ok + issues 非空）且状态不变、start 仍拒绝；合法图 applyGraph ok、
//   start admitted 且 Running；Running 下应用第二张合法图经 GraphApplied 在帧
//   边界生效；Failed 后 stop→Idle 可重启（新会话推进）；
// - 统计通道：tryLoadStats 序号严格递增；processedFrames 会话累计达到阈值；
//   nodes 覆盖生效图全部节点且 executedFrames 递增；endToEndFps >= 0、
//   inFlight 有界；
// - 节点产物通道：源节点与末端悬空输出节点均保留最新一幅 valid() 快照、
//   pixels 非空、sourceSequence 随轮询单调不减；
// - 参数热更新：未知节点/未知参数/种类失配/越界值同步拒绝（false + error 非空）；
//   合法值受理（true）并经 ParamUpdated 事件报告；
// - 事件通道（最新态语义）：Started / GraphApplied / ParamUpdated / NodeFailed /
//   Stopped 可读；
// - 关闭排空语义：stop Running→Idle、双 stop 幂等；stop 返回后有界稳定性窗内
//   无新统计；stale 读取（stats/节点输出）保持旧值；引擎析构后
//   executor.shutdown(true) 干净收敛。
//
// DOD-02 适用性说明（与 tests/test_shutdown_drain.cpp 同纪律，如实取舍）：
// 正常完成——统计/产物/事件全链路覆盖；任务异常——makeFailing 注入的节点失败
// 经 future 排空→事件+lastError 可见（本套件失败路径阶段）；提交拒绝与执行中
// 取消——属 Executor 自身设施（引擎经 submit_cancellable/TimerHandle 使用，
// Executor 自测覆盖；引擎层的显式化结果 droppedFrames/Info 事件由各引擎特有
// 测试断言）；shutdown——每阶段收尾 engine.reset() + executor.shutdown(true)
// Completed；超时——所有等待均为有界轮询（默认 5s 死限，静默窗 300ms），
// 不悬挂；套件整体设计运行时间 < 30s。
//
// makeFailing 契约：fixture 提供的工厂必须返回"对 id==failNode 的节点在引擎
// 帧计数等于 failOnFrame（1 起）时必然失败"的引擎，且注入在重启后不再触发
// （引擎帧计数跨会话不复位，重启会话可推进）；为空时跳过失败路径检查。
// 图节点 id 由本套件固定分配：源=1、消费节点=2（makeFailing 的 failNode 应取 2）。
// NodeFailed 事件经最新态事件通道观察：失败转换处 NodeFailed 与 Failed 背靠背
// 发布，单次会话可能只见 Failed（契约允许"只关心最近一条"），套件以最多 6 个
// 独立失败会话采样捕获；引擎从不发布 NodeFailed 时重试耗尽后套件失败。
#pragma once

#include "test_util.hpp"

#include <executor/executor.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "rin/workflow_engine.hpp"
#include "rin/workflow_types.hpp"

namespace rin_test {

/// 共用轮询死限（单次有界等待上限；不许无限等待）。
inline constexpr std::chrono::milliseconds kContractPollDeadline{5000};
/// 静默检查窗（"无新发布"类断言的观察时长）。
inline constexpr std::chrono::milliseconds kContractQuietWindow{300};

/// 有界轮询：pred() 为真即返回 true；超时后做最后一次 pred() 并返回其结果。
template <typename Pred>
inline bool pollUntil(Pred&& pred,
                      std::chrono::milliseconds timeout = kContractPollDeadline) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return pred();
}

/// 静默检查：在 window 内 hasNew() 一旦为真即返回 false（出现新发布）；
/// 全窗安静返回 true。
template <typename HasNew>
inline bool quietFor(HasNew&& hasNew, std::chrono::milliseconds window) {
    const auto deadline = std::chrono::steady_clock::now() + window;
    while (std::chrono::steady_clock::now() < deadline) {
        if (hasNew()) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return true;
}

/// 工作流引擎契约套件的参数化 fixture。
struct WorkflowEngineFixture {
    /// 标准引擎工厂（引擎由套件内的每阶段 Executor 承载；套件负责 stop/reset/
    /// shutdown 生命周期）。
    std::function<std::shared_ptr<rin::IWorkflowEngine>(executor::Executor&)> make;
    /// 可选：构造"对 id==failNode 的节点在第 failOnFrame 帧（1 起）必然失败"的
    /// 引擎（失败路径检查用）；空则跳过失败路径检查。
    std::function<std::shared_ptr<rin::IWorkflowEngine>(executor::Executor&,
                                                        rin::NodeId failNode,
                                                        std::uint64_t failOnFrame)>
        makeFailing;
};

namespace contract_detail {

/// 标准链式图：源（无输入、有输出）-> 单输入类型匹配的消费节点（优先带参数者），
/// 末端输出悬空。节点 id 固定：源=1、消费=2。
struct ChainGraph {
    rin::WorkflowGraph graph;
    rin::NodeId sourceId = 1;
    rin::NodeId consumerId = 2;
};

inline bool buildStandardChainGraph(const rin::NodeCatalog& catalog, ChainGraph& out) {
    const rin::NodeDescriptor* source = nullptr;
    for (const rin::NodeDescriptor& descriptor : catalog.nodes) {
        if (descriptor.inputs.empty() && !descriptor.outputs.empty()) {
            source = &descriptor;
            break;
        }
    }
    if (source == nullptr) {
        return false;
    }
    const rin::NodeDescriptor* firstMatch = nullptr;
    const rin::NodeDescriptor* consumer = nullptr;
    for (const rin::NodeDescriptor& descriptor : catalog.nodes) {
        if (descriptor.inputs.size() == 1 &&
            descriptor.inputs.front() == source->outputs.front()) {
            if (firstMatch == nullptr) {
                firstMatch = &descriptor;
            }
            if (!descriptor.params.empty()) {
                consumer = &descriptor;
                break;
            }
        }
    }
    if (consumer == nullptr) {
        consumer = firstMatch;
    }
    if (consumer == nullptr) {
        return false;
    }

    rin::NodeInstance sourceNode;
    sourceNode.id = out.sourceId;
    sourceNode.typeId = source->typeId;
    rin::NodeInstance consumerNode;
    consumerNode.id = out.consumerId;
    consumerNode.typeId = consumer->typeId;
    out.graph.nodes = {sourceNode, consumerNode};
    rin::Connection connection;
    connection.from = rin::PortRef{out.sourceId, rin::PortDirection::Output, 0};
    connection.to = rin::PortRef{out.consumerId, rin::PortDirection::Input, 0};
    out.graph.connections = {connection};
    return true;
}

/// 单源悬空输出图（Running 下第二张合法图用）。
inline rin::WorkflowGraph buildSourceOnlyGraph(const rin::NodeCatalog& catalog,
                                               rin::NodeId nodeId) {
    rin::WorkflowGraph graph;
    for (const rin::NodeDescriptor& descriptor : catalog.nodes) {
        if (descriptor.inputs.empty() && !descriptor.outputs.empty()) {
            rin::NodeInstance node;
            node.id = nodeId;
            node.typeId = descriptor.typeId;
            graph.nodes = {node};
            return graph;
        }
    }
    return graph;
}

inline const rin::NodeStats* findNodeStats(const rin::WorkflowStats& stats,
                                           rin::NodeId node) {
    for (const rin::NodeStats& entry : stats.nodes) {
        if (entry.node == node) {
            return &entry;
        }
    }
    return nullptr;
}

/// 在图内节点上按谓词找参数声明（返回目录内指针 + 所属节点 id）。
inline const rin::ParamDescriptor* findParamInGraph(
    const rin::NodeCatalog& catalog, const rin::WorkflowGraph& graph,
    const std::function<bool(const rin::ParamDescriptor&)>& predicate,
    rin::NodeId& owner) {
    for (const rin::NodeInstance& instance : graph.nodes) {
        const rin::NodeDescriptor* descriptor =
            rin::findNodeDescriptor(catalog, instance.typeId);
        if (descriptor == nullptr) {
            continue;
        }
        for (const rin::ParamDescriptor& param : descriptor->params) {
            if (predicate(param)) {
                owner = instance.id;
                return &param;
            }
        }
    }
    return nullptr;
}

/// 构造与声明种类必然失配的值。
inline rin::ParamValue makeWrongKindValue(const rin::ParamDescriptor& descriptor) {
    if (descriptor.kind == rin::ParamKind::Boolean) {
        return rin::ParamValue{std::int64_t{1}};  // Boolean 声明给 Integer。
    }
    return rin::ParamValue{false};  // 其余种类给 Boolean 一律失配。
}

/// 构造越界值（要求 hasRange）。
inline rin::ParamValue makeOutOfRangeValue(const rin::ParamDescriptor& descriptor) {
    if (descriptor.kind == rin::ParamKind::Integer) {
        return rin::ParamValue{static_cast<std::int64_t>(descriptor.maxValue) + 1000};
    }
    return rin::ParamValue{descriptor.maxValue + 1000.0};
}

}  // namespace contract_detail

using contract_detail::buildSourceOnlyGraph;
using contract_detail::buildStandardChainGraph;
using contract_detail::ChainGraph;
using contract_detail::findNodeStats;
using contract_detail::findParamInGraph;
using contract_detail::makeOutOfRangeValue;
using contract_detail::makeWrongKindValue;

/// 运行全部契约检查（RIN_CHECK 计数）。每个阶段使用独立 Executor + 引擎实例；
/// 阶段收尾 engine.reset() 后 executor.shutdown(true) 必须干净收敛。
inline void runWorkflowEngineContractChecks(const WorkflowEngineFixture& fixture) {
    RIN_CHECK_MSG(static_cast<bool>(fixture.make), "fixture.make must be provided");
    if (!fixture.make) {
        return;
    }

    // ---- 阶段 1：无图 start 拒绝；非法图 applyGraph 拒绝且状态不变、start 仍拒绝 ----
    {
        executor::Executor executor;
        executor::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        std::shared_ptr<rin::IWorkflowEngine> engine = fixture.make(executor);
        RIN_CHECK(static_cast<bool>(engine));
        if (engine) {
            RIN_CHECK(engine->state() == rin::WorkflowEngineState::Idle);

            const rin::AdmissionResult noGraph = engine->start();
            RIN_CHECK(!noGraph.admitted);
            RIN_CHECK(!noGraph.error.empty());
            RIN_CHECK(engine->state() == rin::WorkflowEngineState::Idle);

            // 非法图（目录无关反例）：未知类型节点 + 悬空输入节点。
            rin::WorkflowGraph invalid;
            rin::NodeInstance ghost;
            ghost.id = 1;
            ghost.typeId = "contract_suite_no_such_type";
            invalid.nodes.push_back(ghost);
            for (const rin::NodeDescriptor& descriptor : engine->catalog().nodes) {
                if (!descriptor.inputs.empty()) {
                    rin::NodeInstance orphan;
                    orphan.id = 2;
                    orphan.typeId = descriptor.typeId;
                    invalid.nodes.push_back(orphan);
                    break;
                }
            }
            const rin::WorkflowValidation invalidResult = engine->applyGraph(invalid);
            RIN_CHECK(!invalidResult.ok);
            RIN_CHECK(!invalidResult.issues.empty());
            RIN_CHECK(engine->state() == rin::WorkflowEngineState::Idle);

            const rin::AdmissionResult stillRejected = engine->start();
            RIN_CHECK(!stillRejected.admitted);
            RIN_CHECK(engine->state() == rin::WorkflowEngineState::Idle);
        }
        engine.reset();
        RIN_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    }

    // ---- 阶段 2：合法图全流程（Started/统计/节点输出/参数/GraphApplied/stop 语义） ----
    {
        executor::Executor executor;
        executor::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        std::shared_ptr<rin::IWorkflowEngine> engine = fixture.make(executor);
        RIN_CHECK(static_cast<bool>(engine));
        if (engine) {
            ChainGraph chain;
            RIN_CHECK(buildStandardChainGraph(engine->catalog(), chain));

            const rin::WorkflowValidation applied = engine->applyGraph(chain.graph);
            RIN_CHECK_MSG(applied.ok, "standard chain graph must validate");
            const rin::AdmissionResult admission = engine->start();
            RIN_CHECK_MSG(admission.admitted,
                          "start with valid applied graph must be admitted");
            RIN_CHECK(engine->state() == rin::WorkflowEngineState::Running);

            rin::WorkflowEvent event;
            RIN_CHECK(pollUntil([&] {
                return engine->tryLoadEvent(event) &&
                       event.kind == rin::WorkflowEventKind::Started;
            }));

            // 统计：processedFrames 有界轮询内达到阈值。
            std::uint64_t statsSeen = 0;
            rin::WorkflowStats stats;
            RIN_CHECK(pollUntil([&] {
                return engine->tryLoadStats(statsSeen, stats) &&
                       stats.processedFrames >= 5;
            }));

            // 统计序号严格递增（连续多幅新快照）。
            {
                std::uint64_t seen = 0;
                int loads = 0;
                bool strictlyIncreasing = true;
                std::uint64_t previousSequence = 0;
                pollUntil(
                    [&] {
                        rin::WorkflowStats newer;
                        if (engine->tryLoadStats(seen, newer)) {
                            if (loads > 0 && newer.sequence <= previousSequence) {
                                strictlyIncreasing = false;
                            }
                            previousSequence = newer.sequence;
                            ++loads;
                        }
                        return loads >= 3;
                    },
                    std::chrono::milliseconds{2000});
                RIN_CHECK(loads >= 3);
                RIN_CHECK(strictlyIncreasing);
                RIN_CHECK(previousSequence > 0);
            }

            // fps/inFlight 有界面。
            {
                std::uint64_t seen = 0;
                rin::WorkflowStats latest;
                RIN_CHECK(engine->tryLoadStats(seen, latest));
                RIN_CHECK(latest.endToEndFps >= 0.0);
                RIN_CHECK(latest.inFlight <= 1024u);  // 无符号非负恒真；断言有界上界。
                RIN_CHECK(latest.processedFrames >= 5);
            }

            // nodes 覆盖图中全部节点且 executedFrames 递增。
            {
                std::uint64_t seen = 0;
                rin::WorkflowStats baseline;
                RIN_CHECK(engine->tryLoadStats(seen, baseline));
                const rin::NodeStats* baseSource =
                    findNodeStats(baseline, chain.sourceId);
                const rin::NodeStats* baseConsumer =
                    findNodeStats(baseline, chain.consumerId);
                RIN_CHECK_MSG(baseSource != nullptr && baseConsumer != nullptr,
                              "stats.nodes must cover all graph nodes");
                if (baseSource != nullptr && baseConsumer != nullptr) {
                    RIN_CHECK(baseSource->executedFrames >= 1 &&
                              baseConsumer->executedFrames >= 1);
                    const std::uint64_t sourceExecuted = baseSource->executedFrames;
                    const std::uint64_t consumerExecuted = baseConsumer->executedFrames;
                    RIN_CHECK(pollUntil([&] {
                        rin::WorkflowStats newer;
                        if (!engine->tryLoadStats(seen, newer)) {
                            return false;
                        }
                        const rin::NodeStats* s = findNodeStats(newer, chain.sourceId);
                        const rin::NodeStats* c =
                            findNodeStats(newer, chain.consumerId);
                        return s != nullptr && c != nullptr &&
                               s->executedFrames > sourceExecuted &&
                               c->executedFrames > consumerExecuted;
                    }));
                }
            }

            // 节点输出：源节点 + 末端悬空输出节点。
            {
                std::uint64_t sourceSeen = 0;
                rin::NodeOutputSnapshot sourceOut;
                RIN_CHECK(pollUntil([&] {
                    return engine->tryLoadNodeOutput(chain.sourceId, sourceSeen,
                                                     sourceOut) &&
                           sourceOut.valid();
                }));
                RIN_CHECK(sourceOut.pixels != nullptr && !sourceOut.pixels->empty());

                bool monotonic = true;
                std::uint64_t observed = sourceOut.sourceSequence;
                pollUntil(
                    [&] {
                        rin::NodeOutputSnapshot next;
                        if (engine->tryLoadNodeOutput(chain.sourceId, sourceSeen, next)) {
                            if (next.sourceSequence < observed) {
                                monotonic = false;
                            }
                            observed = next.sourceSequence;
                        }
                        return !monotonic || observed > sourceOut.sourceSequence;
                    },
                    std::chrono::milliseconds{2000});
                RIN_CHECK(monotonic);
                RIN_CHECK(observed > sourceOut.sourceSequence);

                std::uint64_t consumerSeen = 0;
                rin::NodeOutputSnapshot consumerOut;
                RIN_CHECK(pollUntil([&] {
                    return engine->tryLoadNodeOutput(chain.consumerId, consumerSeen,
                                                     consumerOut) &&
                           consumerOut.valid();
                }));
                RIN_CHECK(consumerOut.pixels != nullptr &&
                          !consumerOut.pixels->empty());
            }

            // 参数热更新：同步拒绝 + 合法值受理并经 ParamUpdated 报告。
            {
                std::string error;
                RIN_CHECK(!engine->requestParamUpdate(999'999, "contract_suite_param",
                                                      rin::ParamValue{std::int64_t{1}},
                                                      &error));
                RIN_CHECK(!error.empty());
                error.clear();
                RIN_CHECK(!engine->requestParamUpdate(
                    chain.consumerId, "contract_suite_missing_param",
                    rin::ParamValue{std::int64_t{1}}, &error));
                RIN_CHECK(!error.empty());

                rin::NodeId rangedOwner = rin::kInvalidNode;
                const rin::ParamDescriptor* ranged = findParamInGraph(
                    engine->catalog(), chain.graph,
                    [](const rin::ParamDescriptor& p) {
                        return p.hasRange && (p.kind == rin::ParamKind::Integer ||
                                              p.kind == rin::ParamKind::Real);
                    },
                    rangedOwner);
                if (ranged != nullptr) {
                    error.clear();
                    RIN_CHECK(!engine->requestParamUpdate(
                        rangedOwner, ranged->id, makeWrongKindValue(*ranged), &error));
                    RIN_CHECK(!error.empty());
                    error.clear();
                    RIN_CHECK(!engine->requestParamUpdate(
                        rangedOwner, ranged->id, makeOutOfRangeValue(*ranged), &error));
                    RIN_CHECK(!error.empty());
                }

                rin::NodeId anyOwner = rin::kInvalidNode;
                const rin::ParamDescriptor* anyParam = findParamInGraph(
                    engine->catalog(), chain.graph,
                    [](const rin::ParamDescriptor&) { return true; }, anyOwner);
                RIN_CHECK_MSG(anyParam != nullptr,
                              "standard chain graph consumer is expected to carry "
                              "params (fixture catalog convention)");
                if (anyParam != nullptr) {
                    RIN_CHECK(engine->requestParamUpdate(
                        anyOwner, anyParam->id, anyParam->defaultValue, nullptr));
                    RIN_CHECK(pollUntil([&] {
                        return engine->tryLoadEvent(event) &&
                               event.kind == rin::WorkflowEventKind::ParamUpdated;
                    }));
                }
            }

            // Running 下应用第二张合法图 → 帧边界 GraphApplied。
            {
                const rin::WorkflowGraph secondGraph =
                    buildSourceOnlyGraph(engine->catalog(), 7);
                const rin::WorkflowValidation applied2 =
                    engine->applyGraph(secondGraph);
                RIN_CHECK_MSG(applied2.ok, "second valid graph must apply while running");
                RIN_CHECK(pollUntil([&] {
                    return engine->tryLoadEvent(event) &&
                           event.kind == rin::WorkflowEventKind::GraphApplied;
                }));
            }

            // stop：Running→Idle、Stopped 事件、幂等双 stop。
            engine->stop();
            RIN_CHECK(engine->state() == rin::WorkflowEngineState::Idle);
            RIN_CHECK(engine->tryLoadEvent(event) &&
                      event.kind == rin::WorkflowEventKind::Stopped);
            engine->stop();
            RIN_CHECK(engine->state() == rin::WorkflowEngineState::Idle);

            // stop 返回后：有界稳定性窗内无新统计；stale 读取保持旧值。
            {
                std::uint64_t seen = 0;
                rin::WorkflowStats last;
                RIN_CHECK(engine->tryLoadStats(seen, last));
                const std::uint64_t statsSequenceAtStop = last.sequence;
                RIN_CHECK(quietFor(
                    [&] {
                        std::uint64_t probe = statsSequenceAtStop;
                        rin::WorkflowStats discarded;
                        return engine->tryLoadStats(probe, discarded);
                    },
                    kContractQuietWindow));
                std::uint64_t fromZero = 0;
                rin::WorkflowStats stale;
                RIN_CHECK(engine->tryLoadStats(fromZero, stale));
                RIN_CHECK_EQ(stale.sequence, statsSequenceAtStop);
            }
            {
                std::uint64_t outSeen = 0;
                rin::NodeOutputSnapshot staleOut;
                RIN_CHECK(engine->tryLoadNodeOutput(7, outSeen, staleOut));
                RIN_CHECK(staleOut.valid());
            }
        }
        engine.reset();
        RIN_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
    }

    // ---- 阶段 3：失败路径（makeFailing 提供时）----
    if (static_cast<bool>(fixture.makeFailing)) {
        const rin::NodeId failNode = 2;  // 标准链式图的消费节点。
        const std::uint64_t failOnFrame = 3;

        // NodeFailed 经最新态事件通道观察：引擎在失败转换处背靠背发布 NodeFailed
        // 与 Failed，契约允许调用方只关心最近一条，单次会话可能错过 NodeFailed
        // 快照。套件以多个独立会话重试捕获；若引擎从不发布 NodeFailed（node 字段
        // 正确），重试耗尽后套件失败（不会静默放过）。
        constexpr int kFailureAttempts = 6;
        bool sawNodeFailed = false;
        rin::NodeId nodeFailedId = rin::kInvalidNode;
        bool reachedFailedOnce = false;

        for (int attempt = 0; attempt < kFailureAttempts && !sawNodeFailed; ++attempt) {
            executor::Executor executor;
            executor::ExecutorConfig executorConfig;
            RIN_CHECK(executor.initialize(executorConfig));
            std::shared_ptr<rin::IWorkflowEngine> engine =
                fixture.makeFailing(executor, failNode, failOnFrame);
            RIN_CHECK(static_cast<bool>(engine));
            if (!engine) {
                RIN_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
                break;
            }
            ChainGraph chain;
            RIN_CHECK(buildStandardChainGraph(engine->catalog(), chain));
            RIN_CHECK_EQ(chain.consumerId, failNode);
            const rin::WorkflowValidation applied = engine->applyGraph(chain.graph);
            RIN_CHECK_MSG(applied.ok,
                          "failing-path fixture must accept the standard chain graph");

            const rin::AdmissionResult admission = engine->start();
            RIN_CHECK(admission.admitted);

            // 高频采样事件通道直至 Failed 终态（有界：死限 5s）。
            bool reachedFailed = false;
            const auto deadline = std::chrono::steady_clock::now() + kContractPollDeadline;
            while (std::chrono::steady_clock::now() < deadline) {
                rin::WorkflowEvent sampled;
                if (engine->tryLoadEvent(sampled)) {
                    if (sampled.kind == rin::WorkflowEventKind::NodeFailed) {
                        sawNodeFailed = true;
                        nodeFailedId = sampled.node;
                    }
                }
                if (engine->state() == rin::WorkflowEngineState::Failed) {
                    reachedFailed = true;
                    break;
                }
            }
            RIN_CHECK(reachedFailed);
            reachedFailedOnce = reachedFailedOnce || reachedFailed;
            RIN_CHECK(!engine->lastError().empty());

            if (attempt == 0 && reachedFailed) {
                // 帧停止推进：失败后静默窗内统计不再前进（首个会话验证一次）。
                std::uint64_t seen = 0;
                rin::WorkflowStats last;
                RIN_CHECK(engine->tryLoadStats(seen, last));
                const std::uint64_t sequenceAtFailure = last.sequence;
                RIN_CHECK(quietFor(
                    [&] {
                        std::uint64_t probe = sequenceAtFailure;
                        rin::WorkflowStats discarded;
                        return engine->tryLoadStats(probe, discarded);
                    },
                    kContractQuietWindow));
            }

            engine.reset();  // 析构内幂等 stop。
            RIN_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
        }

        RIN_CHECK(reachedFailedOnce);
        RIN_CHECK_MSG(sawNodeFailed,
                      "NodeFailed event must surface on the event channel "
                      "(latest-wins mailbox; sampled across repeated failure sessions)");
        RIN_CHECK_EQ(nodeFailedId, failNode);

        // 重启语义（独立会话）：Failed 后 stop→Idle 可再 start 并推进；注入只在
        // 帧计数 == failOnFrame 触发一次，重启会话帧序不回落，可正常推进。
        {
            executor::Executor executor;
            executor::ExecutorConfig executorConfig;
            RIN_CHECK(executor.initialize(executorConfig));
            std::shared_ptr<rin::IWorkflowEngine> engine =
                fixture.makeFailing(executor, failNode, failOnFrame);
            RIN_CHECK(static_cast<bool>(engine));
            if (engine) {
                ChainGraph chain;
                RIN_CHECK(buildStandardChainGraph(engine->catalog(), chain));
                RIN_CHECK(engine->applyGraph(chain.graph).ok);
                RIN_CHECK(engine->start().admitted);
                RIN_CHECK(pollUntil([&] {
                    return engine->state() == rin::WorkflowEngineState::Failed;
                }));
                engine->stop();
                RIN_CHECK(engine->state() == rin::WorkflowEngineState::Idle);
                const rin::AdmissionResult restart = engine->start();
                RIN_CHECK_MSG(restart.admitted,
                              "engine must be restartable after failure + stop");
                RIN_CHECK(engine->state() == rin::WorkflowEngineState::Running);
                {
                    std::uint64_t seen = 0;
                    rin::WorkflowStats stats;
                    RIN_CHECK(pollUntil([&] {
                        return engine->tryLoadStats(seen, stats) &&
                               stats.processedFrames >= 2;
                    }));
                }
                engine->stop();
                RIN_CHECK(engine->state() == rin::WorkflowEngineState::Idle);
            }
            engine.reset();
            RIN_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
        }
    }
}

}  // namespace rin_test
