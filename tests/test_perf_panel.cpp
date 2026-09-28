// M5-05 性能面板统计管道纯逻辑测试 —— 独立验证（Independent Verification
// Agent）。按被测契约（apps/viewer/perf_model.hpp、include/rin/workflow_engine.hpp
// stop/tryLoadStats 契约、ui_workspace_design.md §5.7/§4/§7）独立设计与执行，
// 不依赖开发者自述。
//
// 被测面：
//   1. 统计数值文本（DEC-005 mono 替代纪律）：formatCostMs / formatFps 的
//      "%.1f" + 固定后缀、0/整数/小数进位/整数进位/大数值（预期值经独立
//      gprintf 探针核实，避开恰好 .x5 的二进制表示边界）；
//   2. WorkflowPerfState 消费语义（对测试内契约桩引擎，全确定无时序）：
//      初态空、Idle 无变更 false、Running 下仅 live 翻转即 true、新快照 true、
//      无可见变更 false、最新态合并（中间快照一次直达最新）、lastSeen 只前进
//      不回退、nodeStats 精确内容匹配与图外 id 拒绝、停止后恰好一次 true
//      （live 翻转）且末次值冻结、clear 排空、clear 后对已停止引擎 consume
//      不复活 stale 快照（§4/§7 关闭排空语义；与 M3-07 test_shutdown_drain
//      "clear 后 stale 不复活、复活只能来自新会话新数据" 同纪律）、新会话
//      严格更新序号衔接；
//   3. 对契约假引擎（rin::workflow_fake）的集成管道：Idle 拒绝、启动衔接、
//      Running 下返回值语义与序号单调、最新态合并（200ms 间隙序号跨度 >= 2）、
//      节点统计（simulatedCostMs 常数 2.0 注入 -> last/avg ≈ 2.0、executedFrames
//      > 0、图外 id nullptr）、停止冻结（stop 后至多一次 true，末次值内容与
//      序号恒定）、新会话重启（sequence 严格大于停前、会话累计计数复位）；
//   4. 失败冻结（injectNodeFailure）：引擎 Failed 后 live()==false，末次快照
//      保持可读且 stop() 排空后内容不变。
//
// 测试壳为 tests/test_util.hpp 的 RIN_CHECK*（无第三方框架），main 返回
// rin_test::exitStatus()。引擎消费全部走契约面、owner（测试主线程）直接调用，
// 单线程无并发（DOD-02 矩阵不适用）；所有等待用秒级死限的有界轮询 pollUntil
// 防悬挂。executor 生命周期按 fake_engine.hpp 纪律：引擎 stop + 释放先于
// executor.shutdown(true)。compose 绘制路径（工具栏状态徽标/节点耗时徽标/
// 右面板工作流总览）需活动 EUI 运行时，headless 不可测，不在本文件范围
// （真机视觉验收归 M5-07，与 test_param_panel 同纪律）。

#include "perf_model.hpp"

#include "test_util.hpp"

#include <executor/executor.hpp>

#include <rin/workflow_engine.hpp>
#include <rin/workflow_types.hpp>

#include "fake_engine.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// --- 断言与轮询辅助（与 test_param_panel.cpp 同纪律） ---

void runSection(const char* name, void (*fn)()) {
    std::printf("== %s\n", name);
    fn();
}

[[nodiscard]] bool nearD(double a, double b, double tol = 1e-6) {
    return std::fabs(a - b) <= tol;
}

// 有界轮询（秒级死限，防悬挂；pred() 为真即返回）。
template <typename Pred>
bool pollUntil(Pred&& pred, std::chrono::milliseconds timeout =
                                std::chrono::milliseconds{5000}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return pred();
}

// 反复 consume 直到无可见变更（best-effort 排空；guard 防悬挂，Running 下
// 发布间隔 10ms 远大于单次 consume 周期，收敛确定）。
void drainPerf(viewer::WorkflowPerfState& perf, rin::IWorkflowEngine& engine) {
    int guard = 0;
    while (perf.consume(engine) && ++guard < 10000) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
}

// 构造统计快照（桩引擎用；processedFrames 随 sequence 便于辨识）。
[[nodiscard]] rin::WorkflowStats makeStats(std::uint64_t sequence,
                                           std::vector<rin::NodeStats> nodes) {
    rin::WorkflowStats stats;
    stats.sequence = sequence;
    stats.endToEndFps = 30.0;
    stats.processedFrames = sequence;
    stats.nodes = std::move(nodes);
    return stats;
}

// source(1) -> grayify(2) 两节点链（覆盖测试所需的最小有效图）。
[[nodiscard]] rin::WorkflowGraph makeTwoNodeGraph() {
    rin::WorkflowGraph graph;
    rin::NodeInstance source;
    source.id = 1;
    source.typeId = "source";
    rin::NodeInstance grayify;
    grayify.id = 2;
    grayify.typeId = "grayify";
    graph.nodes = {source, grayify};
    rin::Connection connection;
    connection.from = rin::PortRef{1, rin::PortDirection::Output, 0};
    connection.to = rin::PortRef{2, rin::PortDirection::Input, 0};
    graph.connections.push_back(connection);
    return graph;
}

// --- 契约桩引擎（纯逻辑消费语义的确定驱动，无 executor/线程） ---

// 模拟 IWorkflowEngine 统计面的最小桩：state 直接驱动；统计通道模拟
// LatestMailbox 契约（保留最新一幅，stop 后仍可读；tryLoadStats 只服务
// sequence > lastSeen 的快照并推进 lastSeen）。记录每次调用入口的
// lastSeen 值，供"水位只前进不回退"断言与缺陷定位。
class StatsStubEngine final : public rin::IWorkflowEngine {
public:
    void publish(rin::WorkflowStats snapshot) {
        retained_ = std::move(snapshot);
        hasRetained_ = true;
    }

    [[nodiscard]] const std::vector<std::uint64_t>& lastSeenHistory() const {
        return history_;
    }

    const rin::NodeCatalog& catalog() const override { return catalog_; }

    rin::WorkflowValidation applyGraph(const rin::WorkflowGraph&) override {
        return {};
    }

    bool requestParamUpdate(rin::NodeId, const std::string&, const rin::ParamValue&,
                            std::string*) override {
        return false;
    }

    rin::AdmissionResult start() override {
        state_ = rin::WorkflowEngineState::Running;
        rin::AdmissionResult admitted;
        admitted.admitted = true;
        return admitted;
    }

    void stop() override { state_ = rin::WorkflowEngineState::Idle; }

    [[nodiscard]] rin::WorkflowEngineState state() const override { return state_; }

    [[nodiscard]] std::string lastError() const override { return {}; }

    [[nodiscard]] bool tryLoadStats(std::uint64_t& lastSeenSequence,
                                    rin::WorkflowStats& out) override {
        history_.push_back(lastSeenSequence);
        if (!hasRetained_ || retained_.sequence <= lastSeenSequence) {
            return false;
        }
        out = retained_;
        lastSeenSequence = retained_.sequence;
        return true;
    }

    [[nodiscard]] bool tryLoadNodeOutput(rin::NodeId, std::uint64_t&,
                                         rin::NodeOutputSnapshot&) override {
        return false;
    }

    [[nodiscard]] bool tryLoadEvent(rin::WorkflowEvent&) override { return false; }

private:
    rin::NodeCatalog catalog_;
    rin::WorkflowEngineState state_ = rin::WorkflowEngineState::Idle;
    bool hasRetained_ = false;
    rin::WorkflowStats retained_;
    std::vector<std::uint64_t> history_;
};

// --- 1. 统计数值文本 ---

void testPerfFormatting() {
    // formatCostMs：%.1f + " ms"（预期值经独立探针程序按本机 glibc 核实）。
    RIN_CHECK_EQ(viewer::formatCostMs(0.0), std::string("0.0 ms"));
    RIN_CHECK_EQ(viewer::formatCostMs(2.0), std::string("2.0 ms"));
    RIN_CHECK_EQ(viewer::formatCostMs(2.5), std::string("2.5 ms"));
    RIN_CHECK_EQ(viewer::formatCostMs(0.1), std::string("0.1 ms"));
    // 小数进位（0.19 远离 .15/.25 二进制边界）与不进位（0.04/12.34）。
    RIN_CHECK_EQ(viewer::formatCostMs(0.19), std::string("0.2 ms"));
    RIN_CHECK_EQ(viewer::formatCostMs(0.04), std::string("0.0 ms"));
    RIN_CHECK_EQ(viewer::formatCostMs(12.34), std::string("12.3 ms"));
    RIN_CHECK_EQ(viewer::formatCostMs(12.36), std::string("12.4 ms"));
    // 跨整数位进位（99.99 -> 100.0）与普通大数。
    RIN_CHECK_EQ(viewer::formatCostMs(99.99), std::string("100.0 ms"));
    RIN_CHECK_EQ(viewer::formatCostMs(12345.678), std::string("12345.7 ms"));
    RIN_CHECK_EQ(viewer::formatCostMs(1000000.0), std::string("1000000.0 ms"));
    RIN_CHECK_EQ(viewer::formatCostMs(-2.5), std::string("-2.5 ms"));

    // formatFps：%.1f + " fps"。
    RIN_CHECK_EQ(viewer::formatFps(0.0), std::string("0.0 fps"));
    RIN_CHECK_EQ(viewer::formatFps(0.5), std::string("0.5 fps"));
    RIN_CHECK_EQ(viewer::formatFps(60.0), std::string("60.0 fps"));
    RIN_CHECK_EQ(viewer::formatFps(144.0), std::string("144.0 fps"));
    RIN_CHECK_EQ(viewer::formatFps(29.97), std::string("30.0 fps"));
    RIN_CHECK_EQ(viewer::formatFps(59.94), std::string("59.9 fps"));
    RIN_CHECK_EQ(viewer::formatFps(123456.78), std::string("123456.8 fps"));
}

// --- 2. 消费语义（契约桩引擎，全确定无时序） ---

void testStubConsumeSemantics() {
    viewer::WorkflowPerfState perf;
    StatsStubEngine engine;

    // 初态：全空。
    RIN_CHECK(perf.stats() == nullptr);
    RIN_CHECK(!perf.live());
    RIN_CHECK(perf.nodeStats(1) == nullptr);
    RIN_CHECK(perf.nodeStats(9999) == nullptr);

    // Idle（未运行）+ 无快照：consume false 且状态不变（契约点 2）。
    RIN_CHECK(!perf.consume(engine));
    RIN_CHECK(perf.stats() == nullptr);
    RIN_CHECK(!perf.live());

    // Running 无快照：仅 live 翻转即返回 true，stats 仍为空。
    RIN_CHECK(engine.start().admitted);
    RIN_CHECK_MSG(perf.consume(engine),
                  "stub: live flip alone is a visible change (true)");
    RIN_CHECK(perf.live());
    RIN_CHECK(perf.stats() == nullptr);

    // Running 无变化：false。
    RIN_CHECK(!perf.consume(engine));

    // 新快照 seq 1：true。
    engine.publish(makeStats(1, {}));
    RIN_CHECK(perf.consume(engine));
    RIN_CHECK(perf.stats() != nullptr && perf.stats()->sequence == 1);

    // 无新快照：false，序号不回退。
    RIN_CHECK(!perf.consume(engine));
    RIN_CHECK(perf.stats() != nullptr && perf.stats()->sequence == 1);

    // 最新态合并：连续发布 2、3，一次 consume 只见 3（中间快照被合并）。
    engine.publish(makeStats(2, {}));
    engine.publish(makeStats(3, {rin::NodeStats{3u, 1.5, 1.25, 7u},
                                 rin::NodeStats{9u, 0.5, 0.5, 2u}}));
    RIN_CHECK(perf.consume(engine));
    RIN_CHECK_MSG(perf.stats() != nullptr && perf.stats()->sequence == 3,
                  "stub: latest-wins coalescing skips intermediate sequence 2");

    // 更旧序号不可服务（mailbox 契约：只服务 strictly newer）。
    engine.publish(makeStats(2, {}));
    RIN_CHECK(!perf.consume(engine));
    RIN_CHECK(perf.stats() != nullptr && perf.stats()->sequence == 3);

    // lastSeen 只前进不回退（入口水位历史非降）。
    bool watermarksMonotonic = true;
    std::uint64_t previous = 0;
    for (const std::uint64_t seen : engine.lastSeenHistory()) {
        watermarksMonotonic = watermarksMonotonic && seen >= previous;
        previous = seen;
    }
    RIN_CHECK_MSG(watermarksMonotonic,
                  "stub: lastSeen passed to the engine never regresses");

    // 节点统计精确内容匹配；图外 id / 无效 id 拒绝（契约点 2/4 纯逻辑面）。
    const rin::NodeStats expectedNode3{3u, 1.5, 1.25, 7u};
    const rin::NodeStats expectedNode9{9u, 0.5, 0.5, 2u};
    const rin::NodeStats* n3 = perf.nodeStats(3);
    RIN_CHECK(n3 != nullptr);
    if (n3 != nullptr) {
        RIN_CHECK(*n3 == expectedNode3);
    }
    const rin::NodeStats* n9 = perf.nodeStats(9);
    RIN_CHECK(n9 != nullptr && *n9 == expectedNode9);
    RIN_CHECK(perf.nodeStats(4) == nullptr);
    RIN_CHECK(perf.nodeStats(rin::kInvalidNode) == nullptr);

    // 停止冻结：Idle 后恰好一次 true（live 翻转），末次值（seq 3）保持。
    engine.stop();
    RIN_CHECK_MSG(perf.consume(engine), "stub: stop flips live exactly once (true)");
    RIN_CHECK(!perf.live());
    RIN_CHECK(perf.stats() != nullptr && perf.stats()->sequence == 3);
    bool frozenStable = true;
    for (int i = 0; i < 5; ++i) {
        frozenStable = frozenStable && !perf.consume(engine);
    }
    RIN_CHECK_MSG(frozenStable, "stub: consumes after the stop flip return false");
    RIN_CHECK(perf.stats() != nullptr && perf.stats()->sequence == 3);
    RIN_CHECK(perf.nodeStats(3) != nullptr);  // 末次值冻结，不清空

    // clear 排空：全空（契约点 7 前半）。
    perf.clear();
    RIN_CHECK(perf.stats() == nullptr);
    RIN_CHECK(perf.nodeStats(3) == nullptr);
    RIN_CHECK(perf.nodeStats(9) == nullptr);
    RIN_CHECK(!perf.live());

    // 契约点 7 后半：clear 后对已停止引擎 consume 不复活旧快照。
    // （引擎统计通道按 stop 契约保留末次值，见 workflow_engine.hpp；排空后
    // 水位不得复位——否则 stale 快照被重新接纳，§4"关闭排空"失效。与
    // M3-07 test_shutdown_drain"clear 后 stale 不复活"同纪律。）
    RIN_CHECK_MSG(!perf.consume(engine),
                  "stub: consume on the stopped engine must not resurrect the "
                  "stale snapshot after clear");
    RIN_CHECK_MSG(perf.stats() == nullptr,
                  "stub: stale stats stay cleared after post-clear consume");
    RIN_CHECK(!perf.live());

    // 新会话衔接：Running + 严格更新的 seq 4 -> 恢复消费（对正确实现，
    // 复活缺陷被修为保留水位后同样成立：4 > 3 仍被服务）。
    RIN_CHECK(engine.start().admitted);
    engine.publish(makeStats(4, {}));
    RIN_CHECK(perf.consume(engine));
    RIN_CHECK(perf.live());
    RIN_CHECK(perf.stats() != nullptr && perf.stats()->sequence == 4);
}

// --- 3. 对契约假引擎的集成管道（契约点 2/3/4/5/7） ---

void testFakeEnginePipeline() {
    executor::Executor executor;
    executor::ExecutorConfig executorConfig;
    const bool initialized = executor.initialize(executorConfig);
    RIN_CHECK(initialized);
    if (!initialized) {
        return;
    }

    rin::FakeWorkflowEngineConfig config;
    config.frameWidth = 32;
    config.frameHeight = 24;
    config.frameInterval = std::chrono::milliseconds{10};
    config.maxInFlight = 2;
    // 常数仿真耗时 2.0（全部节点全部帧）：节点统计的确定断言依据。
    config.simulatedCostMs = [](const rin::NodeInstance&, std::uint64_t) {
        return 2.0;
    };
    std::shared_ptr<rin::IWorkflowEngine> engine =
        rin::createFakeWorkflowEngine(executor, std::move(config));

    const rin::WorkflowGraph graph = makeTwoNodeGraph();
    RIN_CHECK(engine->applyGraph(graph).ok);

    viewer::WorkflowPerfState perf;

    // 契约点 2：Idle（未启动）consume false 且状态不变。
    RIN_CHECK(!perf.consume(*engine));
    RIN_CHECK(perf.stats() == nullptr);
    RIN_CHECK(!perf.live());
    RIN_CHECK(perf.nodeStats(1) == nullptr);

    const rin::AdmissionResult admission = engine->start();
    RIN_CHECK(admission.admitted);
    if (!admission.admitted) {
        engine->stop();
        engine.reset();
        (void)executor.shutdown(true);
        return;
    }

    // 启动后首次 consume：true（live 翻转保证；快照是否已发布不确定，
    // 两种情形都返回 true）。
    RIN_CHECK(perf.consume(*engine));
    RIN_CHECK(perf.live());

    // 轮询直到拉到首幅快照。
    RIN_CHECK_MSG(pollUntil([&] {
                      perf.consume(*engine);
                      return perf.stats() != nullptr;
                  }),
                  "perf: running engine publishes the first stats snapshot");
    const rin::WorkflowStats* first = perf.stats();
    RIN_CHECK(first != nullptr);
    if (first != nullptr) {
        // 契约点 3：首幅快照形态。
        RIN_CHECK(first->sequence >= 1);
        RIN_CHECK(first->processedFrames >= 1);
        RIN_CHECK_EQ(first->nodes.size(), std::size_t{2});
        bool coversGraph = first->nodes.size() == 2;
        for (const rin::NodeStats& entry : first->nodes) {
            coversGraph = coversGraph && (entry.node == 1 || entry.node == 2);
        }
        RIN_CHECK_MSG(coversGraph, "perf: snapshot nodes cover graph nodes {1,2}");
    }

    // 契约点 3：Running 下多次 consume 的返回值语义与序号单调。
    std::uint64_t previousSequence = first != nullptr ? first->sequence : 0;
    bool previousLive = perf.live();
    for (int i = 0; i < 150; ++i) {
        const bool changed = perf.consume(*engine);
        const rin::WorkflowStats* snapshot = perf.stats();
        RIN_CHECK(snapshot != nullptr);
        if (snapshot == nullptr) {
            break;
        }
        RIN_CHECK_MSG(snapshot->sequence >= previousSequence,
                      "perf: visible sequence never regresses while running");
        RIN_CHECK(perf.live());
        if (changed) {
            RIN_CHECK_MSG(snapshot->sequence > previousSequence ||
                              perf.live() != previousLive,
                          "perf: true return implies a newer snapshot or a live flip");
        } else {
            RIN_CHECK_MSG(snapshot->sequence == previousSequence &&
                              perf.live() == previousLive,
                          "perf: false return implies no visible change");
        }
        previousSequence = snapshot->sequence;
        previousLive = perf.live();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    // 契约点 3：最新态合并——排空后静置约 20 帧，一次 consume 直达最新序号
    //（跨度 >= 2 证明中间快照被合并）。
    drainPerf(perf, *engine);
    const std::uint64_t beforeGap = perf.stats() ? perf.stats()->sequence : 0;
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    RIN_CHECK_MSG(perf.consume(*engine),
                  "perf: frames published during the gap are consumable");
    const rin::WorkflowStats* merged = perf.stats();
    RIN_CHECK(merged != nullptr);
    if (merged != nullptr) {
        RIN_CHECK_MSG(merged->sequence >= beforeGap + 2,
                      "perf: gap consume jumps >= 2 sequences (latest-wins merge)");
    }

    // 契约点 4：节点统计（常数 2.0 仿真耗时）与图外 id 拒绝。
    const rin::NodeStats* node1 = perf.nodeStats(1);
    const rin::NodeStats* node2 = perf.nodeStats(2);
    RIN_CHECK(node1 != nullptr && node2 != nullptr);
    if (node1 != nullptr) {
        RIN_CHECK(node1->executedFrames > 0);
        RIN_CHECK_MSG(nearD(node1->lastCostMs, 2.0),
                      "perf: node 1 lastCostMs ~= 2.0 (constant injection)");
        RIN_CHECK_MSG(nearD(node1->avgCostMs, 2.0),
                      "perf: node 1 avgCostMs ~= 2.0 (constant window)");
    }
    if (node2 != nullptr) {
        RIN_CHECK(node2->executedFrames > 0);
        RIN_CHECK_MSG(nearD(node2->lastCostMs, 2.0),
                      "perf: node 2 lastCostMs ~= 2.0");
        RIN_CHECK_MSG(nearD(node2->avgCostMs, 2.0),
                      "perf: node 2 avgCostMs ~= 2.0");
    }
    RIN_CHECK(perf.nodeStats(9999) == nullptr);
    RIN_CHECK(perf.nodeStats(rin::kInvalidNode) == nullptr);

    // 停止前建立足量会话累计（>= 10 帧），随后排空并记录末次快照。
    RIN_CHECK_MSG(pollUntil([&] {
                      perf.consume(*engine);
                      return perf.stats() != nullptr &&
                             perf.stats()->processedFrames >= 10;
                  }),
                  "perf: session accumulates >= 10 processed frames before stop");
    drainPerf(perf, *engine);
    RIN_CHECK(perf.stats() != nullptr);
    const rin::WorkflowStats preStop =
        perf.stats() != nullptr ? *perf.stats() : rin::WorkflowStats{};
    RIN_CHECK(preStop.processedFrames >= 10);
    RIN_CHECK(preStop.sequence >= 1);

    // 契约点 5（停止冻结）：stop() 返回后至多一次 true（live 翻转），此后
    // 反复 consume 返回 false，末次值（内容 + 序号）恒定，live 恒 false。
    engine->stop();
    RIN_CHECK_MSG(perf.consume(*engine),
                  "perf: first consume after stop reports the live flip");
    RIN_CHECK(!perf.live());
    const rin::WorkflowStats frozen =
        perf.stats() != nullptr ? *perf.stats() : rin::WorkflowStats{};
    RIN_CHECK(perf.stats() != nullptr);
    RIN_CHECK_MSG(frozen.sequence >= preStop.sequence,
                  "perf: frozen snapshot is the pre-stop one or newer (race window)");
    RIN_CHECK(frozen.processedFrames >= preStop.processedFrames);
    bool stableAfterStop = true;
    for (int i = 0; i < 100; ++i) {
        stableAfterStop = stableAfterStop && !perf.consume(*engine);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    RIN_CHECK_MSG(stableAfterStop,
                  "perf: consumes after the post-stop flip keep returning false");
    RIN_CHECK(perf.stats() != nullptr && *perf.stats() == frozen);
    RIN_CHECK(perf.stats() != nullptr && perf.stats()->sequence == frozen.sequence);
    RIN_CHECK(!perf.live());
    RIN_CHECK(perf.nodeStats(1) != nullptr);  // 冻结语义：末次节点统计仍可读

    // 契约点 5（新会话）：重启后 sequence 严格大于停前，会话累计复位。
    const rin::AdmissionResult restart = engine->start();
    RIN_CHECK(restart.admitted);
    RIN_CHECK_MSG(pollUntil([&] {
                      perf.consume(*engine);
                      return perf.stats() != nullptr &&
                             perf.stats()->sequence > frozen.sequence;
                  }),
                  "perf: new session publishes a strictly newer sequence");
    RIN_CHECK(perf.live());
    const rin::WorkflowStats* renewed = perf.stats();
    RIN_CHECK(renewed != nullptr);
    if (renewed != nullptr) {
        RIN_CHECK(renewed->sequence > frozen.sequence);
        RIN_CHECK(renewed->processedFrames >= 1);
        RIN_CHECK_MSG(renewed->processedFrames < frozen.processedFrames,
                      "perf: session counters restart with the new session "
                      "(snapshot verbatim, start reset)");
        const rin::NodeStats* renewedNode1 = perf.nodeStats(1);
        RIN_CHECK(renewedNode1 != nullptr);
        if (renewedNode1 != nullptr) {
            RIN_CHECK(renewedNode1->executedFrames >= 1);
            RIN_CHECK_MSG(renewedNode1->executedFrames < frozen.processedFrames,
                          "perf: node executedFrames restart with the new session");
        }
    }

    // 收敛到停止态，为契约点 7 准备已消费的末次快照。
    engine->stop();
    RIN_CHECK(perf.consume(*engine));  // live 翻转（可能同拍拉到末幅快照）
    drainPerf(perf, *engine);
    RIN_CHECK(perf.stats() != nullptr);

    // 契约点 7：clear 排空 + stale 统计不复活。
    perf.clear();
    RIN_CHECK(perf.stats() == nullptr);
    RIN_CHECK(perf.nodeStats(1) == nullptr);
    RIN_CHECK(perf.nodeStats(9999) == nullptr);
    RIN_CHECK(!perf.live());
    // 引擎统计通道按 stop 契约保留末次值；clear 后水位不得复位，否则下面
    // 的 consume 会把 stale 快照重新接纳进面板（§4"关闭排空"失效；与
    // M3-07 test_shutdown_drain"clear 后 stale 不复活"同纪律）。
    RIN_CHECK_MSG(!perf.consume(*engine),
                  "perf: consume on the stopped engine must not resurrect the "
                  "stale snapshot after clear");
    RIN_CHECK_MSG(perf.stats() == nullptr,
                  "perf: stale stats stay cleared after post-clear consume");
    RIN_CHECK(!perf.live());

    engine->stop();  // 幂等
    engine.reset();
    RIN_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
}

// --- 4. 失败冻结（契约点 6） ---

void testFakeEngineFailureFreeze() {
    executor::Executor executor;
    executor::ExecutorConfig executorConfig;
    const bool initialized = executor.initialize(executorConfig);
    RIN_CHECK(initialized);
    if (!initialized) {
        return;
    }

    rin::FakeWorkflowEngineConfig config;
    config.frameInterval = std::chrono::milliseconds{10};
    // 第 3 帧起 grayify（节点 2）失败：前两帧正常发布快照，失败前有末次值。
    config.injectNodeFailure = [](const rin::NodeInstance& node,
                                  std::uint64_t frameIndex) {
        return node.id == 2 && frameIndex >= 3;
    };
    std::shared_ptr<rin::IWorkflowEngine> engine =
        rin::createFakeWorkflowEngine(executor, std::move(config));

    RIN_CHECK(engine->applyGraph(makeTwoNodeGraph()).ok);

    viewer::WorkflowPerfState perf;
    const rin::AdmissionResult admission = engine->start();
    RIN_CHECK(admission.admitted);
    if (!admission.admitted) {
        engine->stop();
        engine.reset();
        (void)executor.shutdown(true);
        return;
    }

    // 轮询至 Failed（边轮询边消费，保持消费活跃）。
    RIN_CHECK_MSG(pollUntil([&] {
                      perf.consume(*engine);
                      return engine->state() == rin::WorkflowEngineState::Failed;
                  }),
                  "perf: injected node failure drives the engine to Failed");

    // 失败后 live false（引擎非 Running）；末次快照保持可读。
    RIN_CHECK(!perf.live());
    drainPerf(perf, *engine);  // 吸收任何在途尾帧的发布
    RIN_CHECK(perf.stats() != nullptr);
    if (perf.stats() != nullptr) {
        RIN_CHECK(perf.stats()->sequence >= 1);
        RIN_CHECK(perf.nodeStats(2) != nullptr);  // grayify 失败前已执行的统计
    }
    const rin::WorkflowStats atFailure =
        perf.stats() != nullptr ? *perf.stats() : rin::WorkflowStats{};

    // stop() 排空（Failed -> Idle）：末次值冻结不变，live 恒 false。
    engine->stop();
    RIN_CHECK(!perf.consume(*engine));
    RIN_CHECK(!perf.live());
    RIN_CHECK(perf.stats() != nullptr && *perf.stats() == atFailure);
    RIN_CHECK(perf.stats() != nullptr &&
              perf.stats()->sequence == atFailure.sequence);

    engine->stop();  // 幂等
    engine.reset();
    RIN_CHECK(executor.shutdown(true) == executor::ShutdownResult::Completed);
}

}  // namespace

int main() {
    runSection("perf_formatting", testPerfFormatting);
    runSection("perf_stub_consume_semantics", testStubConsumeSemantics);
    runSection("perf_fake_engine_pipeline", testFakeEnginePipeline);
    runSection("perf_fake_engine_failure_freeze", testFakeEngineFailureFreeze);
    return rin_test::exitStatus();
}
