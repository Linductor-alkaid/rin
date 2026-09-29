#pragma once

// 相机帧源接缝（M5-06 假换真集成）：ICameraService 帧 → 引擎 WorkflowFrameInput
// （engine.hpp 帧源契约，DEC-013）。职责归属按 engine.hpp 的接缝说明为应用层
// （Adapter/应用层职责，RULE-01）：本文件只做有界校验与零拷贝包装，无 EUI 类型，
// 可独立单测（tests/test_run_control.cpp 对脚本化假相机服务验证）。
//
// 语义要点：
// - 非阻塞：帧泵 tick（Executor 周期任务上下文）是唯一调用方，实现仅经
//   ICameraService::tryLoadFrame 的"上次已见序号"过滤读取（LatestMailbox
//   try_load_newer_than，无锁）——满足帧源"非阻塞、不抛出"契约（engine.hpp）；
// - 零拷贝：Frame 的 RGBA8 共享像素缓冲（提交后不可变）经 ImageU8::wrap 接管，
//   预览与工作流共享同一缓冲，无逐帧复制（ImageU8 契约，image_types.hpp）；
// - 多源分发：图内每个 source 节点由引擎按节点 id 携独立水位调用（committedSeq_），
//   同一相机流对多个源节点各自推进，本实现无自持状态（engine.hpp 契约）；
// - 帧种类：source 节点（目录"相机源"）固定取 RGB 彩色流（与预览同一 Rgba8 帧）；
//   深度伪彩通道不进工作流——如需深度输入应按决策扩展目录（新增节点类型或
//   source 参数面），不在接缝内隐式分流；
// - 无效帧防御：frame.valid() 或 wrap 失败按"本帧不可用"返回 false（引擎按
//   无新帧处理），不阻塞不抛出；服务指针失效同样返回 false（引擎停止拉帧前的
//   关闭窗口内安全，见 app.cpp 关闭顺序）。

#include <memory>

#include <rin/camera_service.hpp>
#include <rin/camera_types.hpp>
#include <rin/image_types.hpp>
#include <rin/workflow_types.hpp>

#include "engine.hpp"

namespace viewer {

/// 构造相机帧源（M5-06）：从 service 的 kind 流取"比 lastSeen 新"的最新帧，
/// 零拷贝包装为工作流输入。service 以 shared_ptr 持有——引擎生命周期可能覆盖
/// 相机服务的停止/重置窗口（app.cpp 关闭顺序：服务先停、引擎后回收），值捕获
/// 保证帧源闭包不悬垂；停止后的服务读取恒为"无新帧"，无害。
[[nodiscard]] inline rin::WorkflowFrameSource makeCameraFrameSource(
    std::shared_ptr<rin::ICameraService> service, rin::FrameKind kind) {
    return [service = std::move(service),
            kind](rin::NodeId, std::uint64_t& lastSeenSequence,
                  rin::WorkflowFrameInput& out) -> bool {
        if (service == nullptr) {
            return false;
        }
        rin::Frame frame;
        if (!service->tryLoadFrame(kind, lastSeenSequence, frame) || !frame.valid()) {
            return false;
        }
        // 相机服务的两种流均已转换为 RGBA8（camera_types.hpp Frame 契约），与
        // source 节点声明输出类型一致；格式不符将由 runNodeGraph 注入语义显式
        // 失败（引擎 Failed），此处按 Rgba8 包装即可。
        out.image = rin::ImageU8::wrap(rin::PortType::Rgba8, frame.width, frame.height,
                                       frame.stride, frame.pixels);
        if (!out.image.valid()) {
            return false;  // 防御：包装失败按无新帧处理，不进入执行。
        }
        out.sourceSequence = frame.sequence;
        return true;  // lastSeenSequence 已由 tryLoadFrame 推进到本帧序号。
    };
}

}  // namespace viewer
