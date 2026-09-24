#pragma once

#include <executor/executor.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

#include "rin/workflow_engine.hpp"
#include "rin/workflow_types.hpp"

namespace rin {

/// M5-08 契约假引擎配置（DEC-016）：合成源帧、仿真统计与故障注入旋钮。
///
/// 全部字段仅供 UI 骨架先行开发（M5-03..M5-05）与契约测试使用；仿真耗时与
/// FPS 是调试数据，不得对外宣称任何性能结论（DEC-016 风险条款）。
struct FakeWorkflowEngineConfig {
    /// 合成源帧尺寸（所有节点输出同尺寸；crop/downscale 参数可缩放输出宽高）。
    std::uint32_t frameWidth = 64;
    std::uint32_t frameHeight = 48;
    /// 合成源帧间隔（Executor 周期 tick 周期；允许抖动的后台周期任务）。
    std::chrono::milliseconds frameInterval{33};
    /// 有界在飞上限（EXEC-07）：达到上限的新帧显式丢弃并计入 droppedFrames。
    std::size_t maxInFlight = 2;
    /// 单图节点数准入上限（applyGraph 同步拒绝，超限返回校验问题）。
    std::size_t maxNodes = 64;
    /// 参数热更新命令队列容量（逐条 FIFO；满时 requestParamUpdate 同步拒绝）。
    std::size_t paramQueueCapacity = 64;
    /// 节点目录；为空时使用 makeDefaultFakeCatalog() 的 M4 调色板。
    NodeCatalog catalog;
    /// 仿真逐节点耗时（毫秒，写入 NodeStats）；为空时用按节点类型与帧序号的
    /// 确定性缺省表。
    std::function<double(const NodeInstance&, std::uint64_t frameIndex)> simulatedCostMs;
    /// 故障注入：返回 true 表示该节点在该帧执行失败（NodeFailed 事件 + Failed）。
    /// 为空时永不失败。
    std::function<bool(const NodeInstance&, std::uint64_t frameIndex)> injectNodeFailure;
};

/// M4 算子调色板的假目录（UI 调色板/参数面板开发用）：source / crop /
/// downscale / grayify / gaussian_blur / conv_kernel / hist_eq / fft_lowpass /
/// fft_highpass / fft_bandpass；端口签名与参数 schema 按 M4 范围冻结，
/// 算子数值语义由 M4 真引擎实现（假引擎只产出合成图案）。
[[nodiscard]] NodeCatalog makeDefaultFakeCatalog();

/// IWorkflowEngine 的契约假实现（M5-08，DEC-016）。
///
/// 语义复刻 EXEC-07：帧驱动执行以 Executor 有限任务承载（周期 tick + 有界在飞
/// 准入 + 显式丢弃计数 + 提交拒绝显式事件）；图/参数命令与统计/产物/事件全部经
/// executor::comm 通道（图替换用 LatestMailbox 最新态，参数热更新用 MpscChannel
/// 逐条 FIFO，统计与产物用 LatestMailbox 最新态）；不新增线程设施。
///
/// 线程模型（与契约一致）：applyGraph/start/stop/requestParamUpdate 供 owner
/// 线程调用；tryLoad* 供 UI 线程非阻塞调用。Running 下的图重建与参数热更新在
/// 帧边界（执行任务开头）排空生效（"下一帧生效"语义）。
///
/// 假引擎特有约定（真引擎在 M4-07/M5-06 对齐或取代）：
/// - sourceSequence 为引擎实例内单调递增的合成帧序号（跨会话不复位，UI 的
///   "上次已见序号"过滤跨重启保持正确）；
/// - processedFrames/droppedFrames/节点 executedFrames 为会话累计（start 时
///   复位）；统计通道 sequence 引擎实例内单调；
/// - stop() 排空中发现的任务异常以 NodeFailed/Failed 事件与 lastError 保持
///   可见，但终态按 stop() 语义收敛到 Idle；
/// - Failed 后 stop() -> Idle，可再次 start()（新会话）；Failed 为运行终态，
///   不自动恢复；
/// - Idle（未运行）下 requestParamUpdate 直接同步改待运行图并发布 ParamUpdated。
///
/// 生命周期：executor 必须已 initialize；实例必须先于 executor shutdown 停止
/// 或析构（析构内幂等 stop）。
[[nodiscard]] std::shared_ptr<IWorkflowEngine> createFakeWorkflowEngine(
    executor::Executor& executor, FakeWorkflowEngineConfig config = {});

}  // namespace rin
