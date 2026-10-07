// M4-08 实测工具：848×480 典型工作流全链路吞吐（M4-07 真引擎，DEC-013 执行模型）。
//
// 测量对象：createWorkflowEngine 真引擎在默认配置（maxInFlight=2、帧泵 5ms、
// Executor 默认配置）下对三条典型链的端到端吞吐与逐节点耗时。方法、环境记录
// 与结果落盘见 tools/workflow_bench/README.md 与
// docs/benchmarks/workflow-throughput-848x480.md；本工具只产出实测值，
// 不含性能目标断言（DEC-012/DEC-016：性能结论以真引擎实测记录为准）。
//
// 帧输入为合成图案（共享不可变缓冲零拷贝注入），刻意隔离"引擎 + 算子"成本与
// 相机帧 → ImageU8 转换成本（后者归 Adapter/应用层，RULE-01），不代表采集路径。
//
// Executor owner：本进程 main（AGENTS.md owner 纪律）——initialize → 各运行
// 依次创建引擎 / start / stop / 析构 → shutdown(true)。引擎实例先于 executor
// shutdown 结束生命周期；测量循环位于非 worker 线程（main）。

#include <kairo/executor.hpp>

#include "engine.hpp"

#include <rin/workflow_types.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

using rin::AdmissionResult;
using rin::IWorkflowEngine;
using rin::ImageU8;
using rin::NodeInstance;
using rin::NodeId;
using rin::ParamAssignment;
using rin::ParamValue;
using rin::PortRef;
using rin::PortDirection;
using rin::WorkflowEngineConfig;
using rin::WorkflowEngineState;
using rin::WorkflowFrameInput;
using rin::WorkflowGraph;
using rin::WorkflowStats;

/// 帧尺寸：D435if 默认流原生尺寸（M4-08 工作项口径）。
constexpr std::uint32_t kFrameWidth = 848;
constexpr std::uint32_t kFrameHeight = 480;

/// 每轮 warmup / 测量帧数（camera30 模式下测量窗 ≈ 8 s）。
constexpr std::uint64_t kWarmupFrames = 60;
constexpr std::uint64_t kMeasureFrames = 240;

/// 每个（链 × 输入模式）重复轮数；汇总取中位数。
constexpr int kRepeats = 3;

/// 单阶段有界等待死限（防悬挂；正常远小于此值）。
constexpr auto kPhaseTimeout = 60s;

/// 相机模式输入速率（D435if RGB 默认 30 fps）。
constexpr double kCameraFps = 30.0;

/// 合成帧图案：确定性、非退化（梯度 + 双频正弦），使 grayify/blur/FFT/hist_eq
/// 均做真实工作。共享不可变缓冲，帧注入零拷贝。
const std::shared_ptr<const std::vector<std::uint8_t>>& framePattern() {
    static const std::shared_ptr<const std::vector<std::uint8_t>> pixels = [] {
        auto buffer = std::make_shared<std::vector<std::uint8_t>>(
            static_cast<std::size_t>(kFrameWidth) * kFrameHeight * 4);
        auto clamp8 = [](double v) {
            return static_cast<std::uint8_t>(std::clamp(v, 0.0, 255.0));
        };
        for (std::uint32_t y = 0; y < kFrameHeight; ++y) {
            for (std::uint32_t x = 0; x < kFrameWidth; ++x) {
                const double fx = static_cast<double>(x);
                const double fy = static_cast<double>(y);
                const double wave = 96.0 + 40.0 * std::sin(fx * 0.05) +
                                    24.0 * std::sin((fx + fy) * 0.021) + 0.06 * fy;
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * kFrameWidth + x) * 4;
                (*buffer)[offset + 0] = clamp8(wave);
                (*buffer)[offset + 1] = clamp8(wave + 18.0);
                (*buffer)[offset + 2] = clamp8(255.0 - wave);
                (*buffer)[offset + 3] = 0xFF;
            }
        }
        return buffer;
    }();
    return pixels;
}

ImageU8 wrapFrame() {
    return ImageU8::wrap(rin::PortType::Rgba8, kFrameWidth, kFrameHeight,
                         kFrameWidth * 4, framePattern());
}

/// 饱和帧源：每次探测都交付新帧（测量引擎最大处理吞吐；过载丢弃按 EXEC-07
/// 显式计数，属预期观察项而非缺陷）。
rin::WorkflowFrameSource saturatingSource() {
    return [](NodeId, std::uint64_t& lastSeen, WorkflowFrameInput& out) {
        out.sourceSequence = lastSeen + 1;
        out.image = wrapFrame();
        lastSeen = out.sourceSequence;
        return true;
    };
}

/// 相机速率帧源：按绝对时间表 30 fps 交付（探测不阻塞；落后时按"最新帧"
/// 语义逐探测补交付一帧，交付节奏受帧泵 5ms 粒度约束）。
class PacedSource {
public:
    explicit PacedSource(double fps)
        : fps_(fps), start_(std::chrono::steady_clock::now()) {}

    rin::WorkflowFrameSource fn() {
        return [self = this](NodeId, std::uint64_t& lastSeen,
                             WorkflowFrameInput& out) {
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                              self->start_)
                    .count();
            const std::uint64_t dueFrames =
                static_cast<std::uint64_t>(elapsed * self->fps_);
            if (self->published_ >= dueFrames) {
                return false;  // 按时间表尚无新帧。
            }
            ++self->published_;
            out.sourceSequence = lastSeen + 1;
            out.image = wrapFrame();
            lastSeen = out.sourceSequence;
            return true;
        };
    }

private:
    double fps_;
    std::chrono::steady_clock::time_point start_;
    std::uint64_t published_ = 0;
};

/// 典型链定义：节点 id 与输出标签一一对应，图均为单源链式 DAG。
struct ChainSpec {
    const char* name;
    const char* chain;
    WorkflowGraph graph;
    std::vector<std::pair<NodeId, const char*>> labels;
};

WorkflowGraph chainOf(std::initializer_list<const char*> typeIds) {
    WorkflowGraph graph;
    NodeId id = 1;
    for (const char* typeId : typeIds) {
        NodeInstance node;
        node.id = id++;
        node.typeId = typeId;
        if (typeId == std::string_view{"downscale"}) {
            // 降分辨率前置变体使用 bilinear 质量（scale 取默认 0.5）。
            node.params.push_back(ParamAssignment{
                "interpolation", ParamValue{std::string{"bilinear"}}});
        }
        graph.nodes.push_back(std::move(node));
    }
    for (std::size_t i = 1; i < graph.nodes.size(); ++i) {
        rin::Connection connection;
        connection.from = PortRef{graph.nodes[i - 1].id, PortDirection::Output, 0};
        connection.to = PortRef{graph.nodes[i].id, PortDirection::Input, 0};
        graph.connections.push_back(connection);
    }
    return graph;
}

ChainSpec makeChainSpec(const char* name, const char* chainText,
                        std::initializer_list<const char*> typeIds) {
    ChainSpec spec;
    spec.name = name;
    spec.chain = chainText;
    spec.graph = chainOf(typeIds);
    NodeId id = 1;
    for (const char* typeId : typeIds) {
        spec.labels.emplace_back(id++, typeId);
    }
    return spec;
}

/// 测量读数：经 tryLoadStats 轮询的最新统计（sequence 通道语义由引擎保证）。
struct StatsReader {
    explicit StatsReader(IWorkflowEngine& engineRef) : engine(engineRef) {}

    IWorkflowEngine& engine;
    std::uint64_t lastSeenSequence = 0;
    WorkflowStats last;

    bool pull() { return engine.tryLoadStats(lastSeenSequence, last); }
};

/// 单轮测量结果。
struct RunResult {
    double externalFps = 0.0;    // 测量窗（完成帧数 / 墙钟时长）。
    double engineFps = 0.0;      // 引擎自报 endToEndFps（滚动窗口）。
    std::uint64_t processed = 0; // 测量窗完成帧数。
    std::uint64_t dropped = 0;   // 测量窗显式丢弃计数增量。
    std::vector<std::pair<std::string, double>> nodeAvgMs;
    bool ok = false;
    std::string error;
};

/// 有界轮询等待；poll 为轮询体（返回 true 达成），死限 kPhaseTimeout。
template <typename Poll>
bool pollUntil(Poll poll) {
    const auto deadline = std::chrono::steady_clock::now() + kPhaseTimeout;
    for (;;) {
        if (poll()) {
            return true;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(2ms);
    }
}

/// 单轮测量：新引擎实例 → applyGraph/start → warmup → 测量窗 → 采样 → stop。
RunResult runOnce(kairo::Executor& executor, const ChainSpec& chain,
                  rin::WorkflowFrameSource source) {
    RunResult result;

    WorkflowEngineConfig config;  // 发布默认：maxInFlight=2、帧泵 5ms。
    config.frameSource = std::move(source);
    auto engine = rin::createWorkflowEngine(executor, std::move(config));

    const rin::WorkflowValidation validation = engine->applyGraph(chain.graph);
    if (!validation.ok) {
        result.error = "applyGraph rejected: " +
                       (validation.issues.empty()
                            ? std::string{}
                            : validation.issues.front().message);
        return result;
    }
    const AdmissionResult admission = engine->start();
    if (!admission.admitted) {
        result.error = "start rejected: " + admission.error;
        return result;
    }

    StatsReader reader{*engine};

    // 失败监视：任何等待阶段转入 Failed 即终止（典型链不应失败）。
    auto failedNow = [&engine, &result]() -> bool {
        if (engine->state() == WorkflowEngineState::Failed) {
            result.error = "engine Failed: " + engine->lastError();
            return true;
        }
        return false;
    };

    // warmup：等待累计完成 kWarmupFrames 帧（跳过启动瞬态）。
    if (!pollUntil([&] {
            reader.pull();
            return failedNow() || reader.last.processedFrames >= kWarmupFrames;
        })) {
        if (result.error.empty()) {
            result.error = "warmup timeout";
        }
        engine->stop();
        return result;
    }

    // 测量窗：以 warmup 退出时的完成数/丢弃数为基线，推进 kMeasureFrames。
    const std::uint64_t baselineProcessed = reader.last.processedFrames;
    const std::uint64_t baselineDropped = reader.last.droppedFrames;
    const auto t0 = std::chrono::steady_clock::now();
    if (!pollUntil([&] {
            reader.pull();
            return failedNow() ||
                   reader.last.processedFrames >=
                       baselineProcessed + kMeasureFrames;
        })) {
        if (result.error.empty()) {
            result.error = "measure timeout";
        }
        engine->stop();
        return result;
    }
    const auto t1 = std::chrono::steady_clock::now();
    if (failedNow()) {
        engine->stop();
        return result;
    }

    result.processed = reader.last.processedFrames - baselineProcessed;
    result.dropped = reader.last.droppedFrames - baselineDropped;
    result.externalFps =
        static_cast<double>(result.processed) /
        std::chrono::duration<double>(t1 - t0).count();
    result.engineFps = reader.last.endToEndFps;
    for (const auto& [id, label] : chain.labels) {
        for (const auto& nodeStats : reader.last.nodes) {
            if (nodeStats.node == id) {
                result.nodeAvgMs.emplace_back(label, nodeStats.avgCostMs);
            }
        }
    }
    result.ok = true;

    engine->stop();  // 排空收敛 Idle（会话结束，析构前显式停止）。
    return result;
}

const char* modeName(bool saturate) { return saturate ? "saturate" : "camera30"; }

double medianOf(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    if (n == 0) {
        return 0.0;
    }
    return n % 2 == 1 ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

void printRun(const ChainSpec& chain, bool saturate, int repeat,
              const RunResult& result) {
    std::printf("%-10s %-9s #%d  ext=%7.2f fps  engine=%7.2f fps  "
                "processed=%llu dropped=%llu\n",
                chain.name, modeName(saturate), repeat, result.externalFps,
                result.engineFps,
                static_cast<unsigned long long>(result.processed),
                static_cast<unsigned long long>(result.dropped));
    if (!result.ok) {
        std::printf("           ERROR: %s\n", result.error.c_str());
        return;
    }
    std::printf("           per-node avg ms:");
    for (const auto& [label, avgMs] : result.nodeAvgMs) {
        std::printf(" %s=%.3f", label.c_str(), avgMs);
    }
    std::printf("\n");
}

}  // namespace

int main() {
    std::printf("workflow_bench: 848x480 typical workflow throughput "
                "(real engine, DEC-013)\n");
    std::printf("frame=%ux%u Rgba8  engine defaults: maxInFlight=2 pump=5ms  "
                "executor=default  warmup=%llu measure=%llu repeats=%d\n",
                kFrameWidth, kFrameHeight,
                static_cast<unsigned long long>(kWarmupFrames),
                static_cast<unsigned long long>(kMeasureFrames), kRepeats);
    std::printf("hw_concurrency=%u\n", std::thread::hardware_concurrency());

    kairo::Executor executor;
    kairo::ExecutorConfig executorConfig;
    if (!executor.initialize(executorConfig)) {
        std::fprintf(stderr, "executor initialize failed\n");
        return 1;
    }

    struct ModeSpec {
        bool saturate;
        const char* description;
    };
    static constexpr ModeSpec kModes[] = {
        {true, "帧源饱和（最大处理吞吐；过载丢弃为显式预期行为）"},
        {false, "帧源 30 fps 定速（相机速率；余量检查）"},
    };

    static const ChainSpec kChains[] = {
        makeChainSpec("typical",
                      "source -> grayify -> gaussian_blur -> fft_lowpass -> "
                      "hist_eq",
                      {"source", "grayify", "gaussian_blur", "fft_lowpass",
                       "hist_eq"}),
        makeChainSpec("downscale",
                      "source -> downscale(bilinear,0.5) -> grayify -> "
                      "gaussian_blur -> fft_lowpass -> hist_eq",
                      {"source", "downscale", "grayify", "gaussian_blur",
                       "fft_lowpass", "hist_eq"}),
        makeChainSpec("spatial",
                      "source -> grayify -> gaussian_blur -> hist_eq "
                      "(no-FFT reference)",
                      {"source", "grayify", "gaussian_blur", "hist_eq"}),
    };

    struct SummaryRow {
        const ChainSpec* chain;
        bool saturate;
        double externalFps = 0.0;
        double engineFps = 0.0;
        std::vector<std::pair<std::string, double>> nodeAvgMs;
    };
    std::vector<SummaryRow> summary;
    summary.reserve(std::size(kChains) * std::size(kModes));

    bool allOk = true;
    for (const ChainSpec& chain : kChains) {
        for (const ModeSpec& mode : kModes) {
            std::printf("\n=== %s | %s ===\n%s\n", chain.name,
                        modeName(mode.saturate), mode.description);
            std::printf("chain: %s\n", chain.chain);

            std::vector<RunResult> runs;
            for (int repeat = 1; repeat <= kRepeats; ++repeat) {
                PacedSource paced(kCameraFps);
                RunResult result =
                    runOnce(executor, chain,
                            mode.saturate ? saturatingSource() : paced.fn());
                printRun(chain, mode.saturate, repeat, result);
                if (!result.ok) {
                    allOk = false;
                    break;
                }
                runs.push_back(std::move(result));
            }

            SummaryRow row;
            row.chain = &chain;
            row.saturate = mode.saturate;
            if (runs.size() == static_cast<std::size_t>(kRepeats)) {
                std::vector<double> externalSamples;
                externalSamples.reserve(runs.size());
                for (const RunResult& run : runs) {
                    externalSamples.push_back(run.externalFps);
                }
                const double medianExternal = medianOf(externalSamples);
                row.externalFps = medianExternal;
                // 引擎自报 fps 与逐节点耗时取外测 fps 最接近中位数的一轮
                // （代表样本；逐节点滚动窗口 rin::kStatsWindow，轮间稳定）。
                const RunResult* representative = &runs.front();
                double bestDelta = std::abs(representative->externalFps -
                                            medianExternal);
                for (const RunResult& run : runs) {
                    const double delta =
                        std::abs(run.externalFps - medianExternal);
                    if (delta < bestDelta) {
                        bestDelta = delta;
                        representative = &run;
                    }
                }
                row.engineFps = representative->engineFps;
                row.nodeAvgMs = representative->nodeAvgMs;
            }
            summary.push_back(std::move(row));
        }
    }

    std::printf("\n==== 汇总（%d 轮中位数）====\n", kRepeats);
    for (const SummaryRow& row : summary) {
        std::printf("%-10s %-9s ext=%7.2f fps  engine=%7.2f fps\n",
                    row.chain->name, modeName(row.saturate), row.externalFps,
                    row.engineFps);
        if (!row.nodeAvgMs.empty()) {
            std::printf("%-10s %-9s per-node avg ms:", "", "");
            for (const auto& [label, avgMs] : row.nodeAvgMs) {
                std::printf(" %s=%.3f", label.c_str(), avgMs);
            }
            std::printf("\n");
        }
    }

    executor.shutdown(true);
    return allOk ? 0 : 1;
}
