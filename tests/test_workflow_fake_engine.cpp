// M5-08 契约假引擎独立验证（DEC-016 契约先行，Independent-Verification-Agent）。
//
// 被测面：src/workflow/fake_engine.hpp/.cpp（rin::createFakeWorkflowEngine /
// rin::makeDefaultFakeCatalog / FakeWorkflowEngineConfig）实现的 IWorkflowEngine
// 公开契约（include/rin/workflow_engine.hpp），以及假引擎特有语义：
// - 工厂校验：frameWidth/frameHeight==0、maxInFlight==0、frameInterval<=0 抛
//   std::invalid_argument；
// - 默认调色板目录：M4 节点集与参数 schema（source/grayify/gaussian_blur+radius）；
// - EXEC-07 有界在飞显式丢弃：maxInFlight=1 + 过载帧 → droppedFrames>0 且
//   processedFrames 仍增长、inFlight 不越界（提交拒绝显式化归 Executor 自身
//   设施，引擎侧只断言显式化结果）；
// - 参数"下一帧生效"（DEC-013）：crop width 热更新 → 输出快照 width 跟随；
// - simulatedCostMs 注入：NodeStats.lastCostMs/avgCostMs 精确等于注入常数；
// - 会话复位与实例内单调：start 复位 processedFrames；sourceSequence 跨会话
//   不复位（UI "上次已见序号"过滤跨重启正确）；
// - Idle 参数热更新：同步改待运行图 + ParamUpdated 立即可读，start 后生效；
// - 关停竞态防御：Running 中 executor.shutdown(false) 后 stop() 有界收敛 Idle；
// - owner 纪律正常路径：stop + reset 后 executor.shutdown(true) Completed。
//
// 契约面（引擎无关）由 tests/workflow_engine_contract_suite.hpp 共用套件覆盖
// （本文件先以假引擎 fixture 运行它）；DOD-02 适用性说明见套件文件头：正常完成/
// 任务异常（注入失败）/shutdown 由套件覆盖，提交拒绝/执行中取消归 Executor 设施，
// 超时以有界轮询（rin_test::pollUntil，默认 5s 死限）防悬挂，套件 + 特有检查
// 整体 < 30s。
#include "test_util.hpp"

#include <executor/executor.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "fake_engine.hpp"
#include "rin/workflow_engine.hpp"
#include "rin/workflow_types.hpp"
#include "workflow_engine_contract_suite.hpp"

namespace {

using executor::ShutdownResult;
using rin::createFakeWorkflowEngine;
using rin::FakeWorkflowEngineConfig;
using rin::IWorkflowEngine;
using rin::NodeCatalog;
using rin::NodeDescriptor;
using rin::NodeId;
using rin::NodeInstance;
using rin::NodeOutputSnapshot;
using rin::ParamDescriptor;
using rin::ParamValue;
using rin::WorkflowEngineState;
using rin::WorkflowEvent;
using rin::WorkflowEventKind;
using rin::WorkflowGraph;
using rin::WorkflowStats;

/// 小配置：32x24 帧、2ms tick（契约套件与多数特有检查共用）。
FakeWorkflowEngineConfig smallConfig() {
    FakeWorkflowEngineConfig config;
    config.frameWidth = 32;
    config.frameHeight = 24;
    config.frameInterval = std::chrono::milliseconds{2};
    return config;
}

/// source(1) -> consumerType(2) 标准链（末端悬空输出）。
WorkflowGraph makeChain(const std::string& consumerType) {
    WorkflowGraph graph;
    NodeInstance source;
    source.id = 1;
    source.typeId = "source";
    NodeInstance consumer;
    consumer.id = 2;
    consumer.typeId = consumerType;
    graph.nodes = {source, consumer};
    rin::Connection connection;
    connection.from = rin::PortRef{1, rin::PortDirection::Output, 0};
    connection.to = rin::PortRef{2, rin::PortDirection::Input, 0};
    graph.connections = {connection};
    return graph;
}

}  // namespace

int main() {
    // ---- 1) 共用契约套件（假引擎 fixture：小配置 + 注入失败）----
    {
        rin_test::WorkflowEngineFixture fixture;
        fixture.make = [](executor::Executor& executor) -> std::shared_ptr<IWorkflowEngine> {
            return createFakeWorkflowEngine(executor, smallConfig());
        };
        fixture.makeFailing =
            [](executor::Executor& executor, NodeId failNode,
               std::uint64_t failOnFrame) -> std::shared_ptr<IWorkflowEngine> {
            FakeWorkflowEngineConfig config = smallConfig();
            config.injectNodeFailure =
                [failNode, failOnFrame](const NodeInstance& node, std::uint64_t frameIndex) {
                    return node.id == failNode && frameIndex == failOnFrame;
                };
            return createFakeWorkflowEngine(executor, std::move(config));
        };
        rin_test::runWorkflowEngineContractChecks(fixture);
    }

    // ---- 2) 工厂校验：非法配置抛 std::invalid_argument ----
    {
        executor::Executor executor;
        executor::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        const auto rejectsInvalid = [&executor](FakeWorkflowEngineConfig bad) {
            try {
                (void)createFakeWorkflowEngine(executor, std::move(bad));  // 预期在此抛出。
            } catch (const std::invalid_argument&) {
                return true;
            } catch (...) {
                return false;
            }
            return false;
        };
        FakeWorkflowEngineConfig zeroWidth = smallConfig();
        zeroWidth.frameWidth = 0;
        RIN_CHECK(rejectsInvalid(std::move(zeroWidth)));
        FakeWorkflowEngineConfig zeroHeight = smallConfig();
        zeroHeight.frameHeight = 0;
        RIN_CHECK(rejectsInvalid(std::move(zeroHeight)));
        FakeWorkflowEngineConfig zeroInFlight = smallConfig();
        zeroInFlight.maxInFlight = 0;
        RIN_CHECK(rejectsInvalid(std::move(zeroInFlight)));
        FakeWorkflowEngineConfig zeroInterval = smallConfig();
        zeroInterval.frameInterval = std::chrono::milliseconds{0};
        RIN_CHECK(rejectsInvalid(std::move(zeroInterval)));
        FakeWorkflowEngineConfig negativeInterval = smallConfig();
        negativeInterval.frameInterval = std::chrono::milliseconds{-5};
        RIN_CHECK(rejectsInvalid(std::move(negativeInterval)));
        RIN_CHECK(executor.shutdown(true) == ShutdownResult::Completed);
    }

    // ---- 3) makeDefaultFakeCatalog：M4 调色板与参数 schema ----
    {
        const NodeCatalog catalog = rin::makeDefaultFakeCatalog();
        RIN_CHECK(catalog.valid());
        RIN_CHECK(rin::findNodeDescriptor(catalog, "source") != nullptr);
        RIN_CHECK(rin::findNodeDescriptor(catalog, "grayify") != nullptr);
        const NodeDescriptor* blur = rin::findNodeDescriptor(catalog, "gaussian_blur");
        RIN_CHECK(blur != nullptr);
        if (blur != nullptr) {
            bool hasRadius = false;
            for (const ParamDescriptor& param : blur->params) {
                hasRadius = hasRadius || param.id == "radius";
            }
            RIN_CHECK(hasRadius);
        }
    }

    // ---- 4) 有界在飞显式丢弃：maxInFlight=1 + 1ms tick + 大帧制造过载 ----
    {
        executor::Executor executor;
        executor::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        FakeWorkflowEngineConfig config = smallConfig();
        config.frameWidth = 256;
        config.frameHeight = 256;
        config.frameInterval = std::chrono::milliseconds{1};
        config.maxInFlight = 1;
        std::shared_ptr<IWorkflowEngine> engine =
            createFakeWorkflowEngine(executor, std::move(config));
        RIN_CHECK(engine->applyGraph(makeChain("grayify")).ok);
        RIN_CHECK(engine->start().admitted);

        std::uint64_t seen = 0;
        WorkflowStats stats;
        bool inFlightBounded = true;
        RIN_CHECK(rin_test::pollUntil([&] {
            if (engine->tryLoadStats(seen, stats)) {
                if (stats.inFlight > 1) {
                    inFlightBounded = false;
                }
                return stats.droppedFrames > 0;
            }
            return false;
        }));
        RIN_CHECK(inFlightBounded);
        RIN_CHECK(stats.inFlight <= 1);  // 在飞计数不越过 maxInFlight=1 上界。

        // 丢弃发生的同时 processedFrames 仍增长（显式丢弃而非停摆/静默排队）。
        const std::uint64_t processedAtDrop = stats.processedFrames;
        RIN_CHECK(rin_test::pollUntil([&] {
            return engine->tryLoadStats(seen, stats) &&
                   stats.processedFrames > processedAtDrop;
        }));

        engine->stop();
        engine.reset();
        RIN_CHECK(executor.shutdown(true) == ShutdownResult::Completed);
    }

    // ---- 5) crop 参数生效（下一帧语义）----
    {
        executor::Executor executor;
        executor::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        std::shared_ptr<IWorkflowEngine> engine =
            createFakeWorkflowEngine(executor, smallConfig());
        RIN_CHECK(engine->applyGraph(makeChain("crop")).ok);
        RIN_CHECK(engine->start().admitted);

        // 未赋参数时 crop 输出走 fallback（frameWidth/frameHeight）。
        std::uint64_t seen = 0;
        NodeOutputSnapshot out;
        RIN_CHECK(rin_test::pollUntil([&] {
            return engine->tryLoadNodeOutput(2, seen, out) && out.valid();
        }));
        RIN_CHECK_EQ(out.width, 32u);
        RIN_CHECK_EQ(out.height, 24u);

        std::string error;
        RIN_CHECK(engine->requestParamUpdate(2, "width",
                                             ParamValue{static_cast<std::int64_t>(16)},
                                             &error));
        RIN_CHECK(error.empty());
        WorkflowEvent event;
        RIN_CHECK(rin_test::pollUntil([&] {
            return engine->tryLoadEvent(event) &&
                   event.kind == WorkflowEventKind::ParamUpdated;
        }));
        RIN_CHECK(rin_test::pollUntil([&] {
            return engine->tryLoadNodeOutput(2, seen, out) && out.width == 16;
        }));
        RIN_CHECK_EQ(out.height, 24u);  // height 未更新，保持 fallback。

        engine->stop();
        engine.reset();
        RIN_CHECK(executor.shutdown(true) == ShutdownResult::Completed);
    }

    // ---- 6) simulatedCostMs 注入：lastCostMs/avgCostMs 精确等于常数 ----
    {
        executor::Executor executor;
        executor::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        FakeWorkflowEngineConfig config = smallConfig();
        constexpr double kInjectedCostMs = 7.25;
        config.simulatedCostMs = [](const NodeInstance&, std::uint64_t) {
            return kInjectedCostMs;
        };
        std::shared_ptr<IWorkflowEngine> engine =
            createFakeWorkflowEngine(executor, std::move(config));
        RIN_CHECK(engine->applyGraph(makeChain("grayify")).ok);
        RIN_CHECK(engine->start().admitted);

        std::uint64_t seen = 0;
        WorkflowStats stats;
        RIN_CHECK(rin_test::pollUntil([&] {
            if (!engine->tryLoadStats(seen, stats)) {
                return false;
            }
            if (stats.nodes.size() != 2) {
                return false;
            }
            for (const auto& node : stats.nodes) {
                if (node.executedFrames < 1) {
                    return false;
                }
            }
            return true;
        }));
        for (const auto& node : stats.nodes) {
            RIN_CHECK_MSG(node.lastCostMs == kInjectedCostMs,
                          "lastCostMs must equal injected constant");
            RIN_CHECK_MSG(node.avgCostMs == kInjectedCostMs,
                          "avgCostMs must equal injected constant");
        }

        engine->stop();
        engine.reset();
        RIN_CHECK(executor.shutdown(true) == ShutdownResult::Completed);
    }

    // ---- 7) 会话复位与序号单调（跨会话 sourceSequence 不复位）----
    {
        executor::Executor executor;
        executor::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        std::shared_ptr<IWorkflowEngine> engine =
            createFakeWorkflowEngine(executor, smallConfig());
        RIN_CHECK(engine->applyGraph(makeChain("grayify")).ok);
        RIN_CHECK(engine->start().admitted);

        // 会话 1：累计足够帧（余量要能吸收轮询器被调度抖动拉大的快照间隔）。
        std::uint64_t statsSeen = 0;
        WorkflowStats stats;
        RIN_CHECK(rin_test::pollUntil([&] {
            return engine->tryLoadStats(statsSeen, stats) &&
                   stats.processedFrames >= 100;
        }));
        const std::uint64_t processedSession1 = stats.processedFrames;
        std::uint64_t outSeen = 0;
        NodeOutputSnapshot out;
        RIN_CHECK(rin_test::pollUntil([&] {
            return engine->tryLoadNodeOutput(2, outSeen, out) && out.valid();
        }));
        const std::uint64_t sequenceSession1 = out.sourceSequence;

        engine->stop();
        RIN_CHECK(engine->state() == WorkflowEngineState::Idle);

        // 会话 2：processedFrames 复位从小值重新增长；sourceSequence 越过会话 1。
        RIN_CHECK(engine->start().admitted);
        // 紧自旋捕获会话 2 首幅新统计（不睡眠；首幅在首个 tick 后 ~2ms 到达，
        // 睡眠轮询在负载下可能一口越过多幅快照）。
        std::uint64_t firstProcessedSession2 = 0;
        bool haveFirstSession2 = false;
        {
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds{2000};
            while (std::chrono::steady_clock::now() < deadline) {
                if (engine->tryLoadStats(statsSeen, stats)) {
                    firstProcessedSession2 = stats.processedFrames;
                    haveFirstSession2 = true;
                    break;
                }
            }
        }
        RIN_CHECK(haveFirstSession2);
        RIN_CHECK_MSG(firstProcessedSession2 < processedSession1,
                      "session-2 processedFrames must restart below session-1 total");
        RIN_CHECK(rin_test::pollUntil([&] {
            return engine->tryLoadNodeOutput(2, outSeen, out) &&
                   out.sourceSequence > sequenceSession1;
        }));

        engine->stop();
        engine.reset();
        RIN_CHECK(executor.shutdown(true) == ShutdownResult::Completed);
    }

    // ---- 8) Idle 参数热更新：同步生效 + ParamUpdated 立即可读，start 后反映 ----
    {
        executor::Executor executor;
        executor::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        std::shared_ptr<IWorkflowEngine> engine =
            createFakeWorkflowEngine(executor, smallConfig());
        RIN_CHECK(engine->applyGraph(makeChain("crop")).ok);
        RIN_CHECK(engine->state() == WorkflowEngineState::Idle);

        std::string error;
        RIN_CHECK(engine->requestParamUpdate(2, "width",
                                             ParamValue{static_cast<std::int64_t>(16)},
                                             &error));
        RIN_CHECK(error.empty());
        WorkflowEvent event;
        RIN_CHECK(engine->tryLoadEvent(event) &&
                  event.kind == WorkflowEventKind::ParamUpdated);

        RIN_CHECK(engine->start().admitted);
        std::uint64_t seen = 0;
        NodeOutputSnapshot out;
        RIN_CHECK(rin_test::pollUntil([&] {
            return engine->tryLoadNodeOutput(2, seen, out) && out.valid();
        }));
        RIN_CHECK_EQ(out.width, 16u);

        engine->stop();
        engine.reset();
        RIN_CHECK(executor.shutdown(true) == ShutdownResult::Completed);
    }

    // ---- 9) shutdown 鲁棒：Running 中 executor.shutdown(false) 后 stop() 有界收敛 ----
    {
        executor::Executor executor;
        executor::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        std::shared_ptr<IWorkflowEngine> engine =
            createFakeWorkflowEngine(executor, smallConfig());
        RIN_CHECK(engine->applyGraph(makeChain("grayify")).ok);
        RIN_CHECK(engine->start().admitted);
        {
            std::uint64_t seen = 0;
            WorkflowStats stats;
            RIN_CHECK(rin_test::pollUntil([&] {
                return engine->tryLoadStats(seen, stats) &&
                       stats.processedFrames >= 3;
            }));
        }

        executor.shutdown(false);  // 对抗路径：引擎仍 Running。
        engine->stop();            // 必须有界返回，不悬挂。
        RIN_CHECK(engine->state() == WorkflowEngineState::Idle);
        engine.reset();  // 析构幂等 stop（executor 已 shutdown，不再调 shutdown）。
    }

    // ---- 10) owner 纪律正常路径收尾：stop + reset 后 shutdown(true) 干净 ----
    {
        executor::Executor executor;
        executor::ExecutorConfig executorConfig;
        RIN_CHECK(executor.initialize(executorConfig));
        std::shared_ptr<IWorkflowEngine> engine =
            createFakeWorkflowEngine(executor, smallConfig());
        RIN_CHECK(engine->applyGraph(makeChain("grayify")).ok);
        RIN_CHECK(engine->start().admitted);
        {
            std::uint64_t seen = 0;
            WorkflowStats stats;
            RIN_CHECK(rin_test::pollUntil([&] {
                return engine->tryLoadStats(seen, stats) &&
                       stats.processedFrames >= 2;
            }));
        }
        engine->stop();
        RIN_CHECK(engine->state() == WorkflowEngineState::Idle);
        engine.reset();
        RIN_CHECK(executor.shutdown(true) == ShutdownResult::Completed);
    }

    return rin_test::exitStatus();
}
