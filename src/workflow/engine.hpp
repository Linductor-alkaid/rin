#pragma once

#include <kairo/executor.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

#include "rin/image_node.hpp"
#include "rin/image_ops.hpp"
#include "rin/node_graph.hpp"
#include "rin/workflow_engine.hpp"
#include "rin/workflow_types.hpp"

namespace rin {

/// 注入型源节点的帧输入（M4-07 帧源接缝，DEC-013 冻结）：本帧图像 + 源帧序号。
///
/// image 必须有效（无效图像由帧泵按"无输入"处理，不进入执行）；格式与源节点
/// 声明输出类型不符时不阻塞捕获，进入执行后由 runNodeGraph 注入语义显式失败
/// （帧源契约违规 → 引擎 Failed，禁止静默）。sourceSequence 与相机帧 sequence
/// 同源，透传给该源节点及其下游的产物快照（下游取各生产者的最小值）。
struct WorkflowFrameInput {
    ImageU8 image;
    std::uint64_t sourceSequence = 0;
};

/// 帧源接缝：取 sourceNode 的最新工作流输入。lastSeenSequence 入参为引擎侧
/// 已消费序号，出参为本次读取看到的最新序号（ICameraService::tryLoadFrame
/// 同型"上次已见序号"过滤，实现方无需自持状态）；返回 false = 无比
/// lastSeenSequence 更新的帧（帧泵跳过捕获）。实现方负责相机帧 → ImageU8
/// 转换（Adapter/应用层职责，RULE-01），必须非阻塞且不抛出（转换错误在
/// 实现内消化为返回 false；抛出按引擎失败路径显式化）。多源图由实现按
/// sourceNode 分发；帧泵是唯一调用方（单线程串行），探测即捕获（消费）。
using WorkflowFrameSource = std::function<bool(NodeId sourceNode,
                                               std::uint64_t& lastSeenSequence,
                                               WorkflowFrameInput& out)>;

/// M4-07 工作流真引擎配置（DEC-013 执行模型）。
struct WorkflowEngineConfig {
    /// 有界在飞上限（EXEC-07）：在飞帧任务达到上限时，新捕获的帧显式丢弃并
    /// 计入 droppedFrames。必须 ≥ 1。
    std::size_t maxInFlight = 2;
    /// 单图节点数准入上限（与 buildNodeGraph/假引擎同默认）。
    std::size_t maxNodes = kDefaultMaxGraphNodes;
    /// 参数热更新命令队列容量（MpscChannel 逐条 FIFO）；0 按 1 处理。
    std::size_t paramQueueCapacity = 64;
    /// 帧泵周期（Executor 允许抖动周期任务）：每次 tick 探测帧源、执行帧边界
    /// 排空并按有界准入提交帧任务。必须 > 0。
    std::chrono::milliseconds pumpInterval{5};
    /// 节点目录；为空时使用 M4 默认目录（与假引擎目录同源，见
    /// src/workflow/default_catalog.hpp）。
    NodeCatalog catalog;
    /// 帧源（必需）：见 WorkflowFrameSource。
    WorkflowFrameSource frameSource;
    /// 节点工厂（默认 makeDefaultImageNode）；测试/扩展经此注入包装节点。
    ImageNodeFactory nodeFactory;
};

/// IWorkflowEngine 真实现（M4-07，DEC-013 执行模型；实现层工厂——签名含
/// executor 第三方类型，按假引擎先例不入 include/rin/ 公开契约）。
///
/// 与 M5-08 假引擎实现同一契约并共用契约测试（DEC-016）。相对假引擎的真语义：
/// - 节点经目录 + nodeFactory 编译（buildNodeGraph），帧任务调用 runNodeGraph
///   同步求值，产出真实算子结果；
/// - 帧输入经 WorkflowFrameSource 捕获（快照语义，同一帧不被两个在飞任务
///   重复消费）；无新帧的 tick 不提交任务；
/// - Running 下图替换为消费式语义：仅实际变化（图对象同一性）时重建换代并
///   发布一次 GraphApplied——不复刻假引擎 peek 不消费导致的逐帧重发缺陷；
/// - 逐节点耗时/失败归属经 runNodeGraph 观测接缝实测（NodeStats.lastCostMs/
///   avgCostMs 为 apply 真实耗时，非仿真值）；
/// - sourceSequence 为帧源透传的相机帧序号（源节点），下游节点取各生产者的
///   最小值（保守对齐）；跨会话不复位由帧源序号保证。
///
/// 其余语义与假引擎一致（DEC-013 §4）：图/参数"下一帧生效"（帧边界换代，
/// 节点实例以新参数重建）；Idle/Failed 下参数热更新同步改待运行图并即时
/// 发布 ParamUpdated；会话统计 start 复位、统计通道序号实例内单调；stop()
/// 阻塞排空在飞任务后收敛 Idle（幂等）；Failed 为运行终态，stop 后可重启；
/// 提交拒绝显式化（丢弃计数 + Info 事件）。
///
/// 生命周期：executor 必须已 initialize；实例必须经本工厂以 shared_ptr 持有
/// （帧泵闭包经 enable_shared_from_this 实现掉队 tick 的生命周期闭合，与假
/// 引擎同款纪律）。实例必须先于 executor shutdown 停止或析构（析构内幂等
/// stop）。frameSource 必须非阻塞。
///
/// 工厂校验：frameSource 为空、maxInFlight == 0 或 pumpInterval <= 0 抛
/// std::invalid_argument；paramQueueCapacity == 0 按 1 处理（与假引擎同）。
[[nodiscard]] std::shared_ptr<IWorkflowEngine> createWorkflowEngine(
    kairo::Executor& executor, WorkflowEngineConfig config = {});

}  // namespace rin
