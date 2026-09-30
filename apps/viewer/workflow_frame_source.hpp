#pragma once

// 相机帧源接缝（M5-06 引入，M6-05 扩展）：ICameraService rendition → 引擎
// WorkflowFrameInput（engine.hpp 帧源契约，DEC-013）。职责归属按 engine.hpp
// 的接缝说明为应用层（Adapter/应用层职责，RULE-01）：本文件只做有界校验、
// rendition 分发与零拷贝包装，无 EUI 类型，可独立单测
// （tests/test_run_control.cpp 对脚本化假相机服务验证）。
//
// 语义要点：
// - 非阻塞：帧泵 tick（Executor 周期任务上下文）是唯一调用方，实现仅经
//   ICameraService::tryLoadFrame / tryLoadGrayFrame 的"上次已见序号"过滤读取
//   （LatestMailbox try_load_newer_than，无锁）——满足帧源"非阻塞、不抛出"
//   契约（engine.hpp）；
// - 零拷贝：Frame（RGBA8）/GrayFrame（Gray8）的共享像素缓冲经 ImageU8::wrap
//   接管，预览与工作流共享同一缓冲，无逐帧复制（ImageU8 契约）；
// - 多源分发（M6-05，DEC-017）：图内每个 source 节点由引擎按节点 id 携独立
//   水位调用；rendition 由 WorkflowSourceRouter 按 source 节点 typeId 分发
//   （RGB / 深度伪彩 / 深度灰度 / 深度自适应灰度），路由快照在图变更时整体
//   换新（原子共享指针，tick 线程只读）；未知节点 id 防御性回退 RGB（引擎
//   覆盖检查要求每个源节点可持续取帧，静默缺帧会停滞整图）；
// - 固定 rendition（DEC-017）：深度伪彩恒 jet、深度灰度恒固定区间近白远黑、
//   自适应灰度恒 P99 近黑远白，全部与预览配色（DEC-007 命令）解耦；
// - 帧种类固定由路由决定；无效帧防御：valid() 或 wrap 失败按"本帧不可用"
//   返回 false（引擎按无新帧处理），不阻塞不抛出；服务指针失效同样返回
//   false（引擎停止拉帧前的关闭窗口内安全，见 app.cpp 关闭顺序）。

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rin/camera_service.hpp>
#include <rin/camera_types.hpp>
#include <rin/image_types.hpp>
#include <rin/workflow_types.hpp>

#include "engine.hpp"

namespace viewer {

/// 工作流相机源 rendition（DEC-017 四源型）。
enum class WorkflowSourceRendition {
    RgbColor,
    DepthJet,
    DepthGray,
    DepthAdaptiveGray,
    /// 米制深度（M10/DEC-020）：tryLoadDepthMetric → Depth32F 字节容器。
    DepthMetric,
};

/// source 节点 typeId → rendition；未知 typeId 防御性回退 RGB。
[[nodiscard]] inline WorkflowSourceRendition renditionForSourceType(const std::string& typeId) {
    if (typeId == "source_depth_jet") {
        return WorkflowSourceRendition::DepthJet;
    }
    if (typeId == "source_depth_gray") {
        return WorkflowSourceRendition::DepthGray;
    }
    if (typeId == "source_depth_adaptive") {
        return WorkflowSourceRendition::DepthAdaptiveGray;
    }
    if (typeId == "source_depth_metric") {
        return WorkflowSourceRendition::DepthMetric;
    }
    return WorkflowSourceRendition::RgbColor;
}

/// 帧源路由快照（WorkflowCanvasState/app.cpp 持有，图变更即 replace 整体
/// 换新；帧泵 tick 线程只读）。原子共享指针保证跨线程读取无撕裂，替换后
/// 旧快照随最后一个读者释放。
class WorkflowSourceRouter {
public:
    using RouteMap = std::unordered_map<rin::NodeId, WorkflowSourceRendition>;

    void replace(std::shared_ptr<const RouteMap> routes) {
        routes_.store(std::move(routes), std::memory_order_release);
    }

    [[nodiscard]] WorkflowSourceRendition renditionFor(rin::NodeId node) const {
        const std::shared_ptr<const RouteMap> routes = routes_.load(std::memory_order_acquire);
        if (routes != nullptr) {
            if (const auto it = routes->find(node); it != routes->end()) {
                return it->second;
            }
        }
        return WorkflowSourceRendition::RgbColor;  // 未知节点：回退 RGB（引擎
                                                   // 覆盖检查不容忍源静默缺帧）。
    }

private:
    std::atomic<std::shared_ptr<const RouteMap>> routes_;
};

/// 构造相机帧源（M6-05 路由版）：按 router 分发的 rendition 从 service 取
/// "比 lastSeen 新"的最新帧，零拷贝包装为工作流输入。service 以 shared_ptr
/// 持有——引擎生命周期可能覆盖相机服务的停止/重置窗口（app.cpp 关闭顺序：
/// 服务先停、引擎后回收），值捕获保证帧源闭包不悬垂；停止后的服务读取恒为
/// "无新帧"，无害。router 生命周期由调用方保证覆盖引擎（ViewerContext 成员，
/// 先于引擎析构）。
[[nodiscard]] inline rin::WorkflowFrameSource makeCameraFrameSource(
    std::shared_ptr<rin::ICameraService> service, const WorkflowSourceRouter& router) {
    return [service = std::move(service), &router](rin::NodeId sourceNode,
                                                   std::uint64_t& lastSeenSequence,
                                                   rin::WorkflowFrameInput& out) -> bool {
        if (service == nullptr) {
            return false;
        }
        switch (router.renditionFor(sourceNode)) {
            case WorkflowSourceRendition::DepthJet: {
                rin::Frame frame;
                if (!service->tryLoadFrame(rin::FrameKind::DepthJet, lastSeenSequence, frame) ||
                    !frame.valid()) {
                    return false;
                }
                out.image = rin::ImageU8::wrap(rin::PortType::Rgba8, frame.width, frame.height,
                                               frame.stride, frame.pixels);
                if (!out.image.valid()) {
                    return false;  // 防御：包装失败按无新帧处理，不进入执行。
                }
                out.sourceSequence = frame.sequence;
                return true;  // lastSeenSequence 已由 tryLoadFrame 推进。
            }
            case WorkflowSourceRendition::DepthMetric: {
                // 米制深度（M10，DEC-020）：DepthFrameF32 float 载荷 → Depth32F
                // 字节容器（一次拷贝；帧源契约要求 ImageU8 承载）。
                rin::DepthMetricSample sample;
                if (!service->tryLoadDepthMetric(lastSeenSequence, sample) || !sample.valid()) {
                    return false;
                }
                const std::size_t count =
                    static_cast<std::size_t>(sample.frame.width()) * sample.frame.height();
                std::vector<std::uint8_t> bytes(count * 4u);
                std::memcpy(bytes.data(), sample.frame.row(0), count * 4u);
                auto pixels = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
                out.image = rin::ImageU8::wrap(rin::PortType::Depth32F, sample.frame.width(),
                                               sample.frame.height(), sample.frame.width() * 4u,
                                               std::move(pixels));
                if (!out.image.valid()) {
                    return false;
                }
                out.sourceSequence = sample.sequence;
                return true;
            }
            case WorkflowSourceRendition::DepthGray:
            case WorkflowSourceRendition::DepthAdaptiveGray: {
                const rin::GrayFrameKind kind =
                    router.renditionFor(sourceNode) == WorkflowSourceRendition::DepthGray
                        ? rin::GrayFrameKind::Depth
                        : rin::GrayFrameKind::DepthAdaptive;
                rin::GrayFrame frame;
                if (!service->tryLoadGrayFrame(kind, lastSeenSequence, frame) || !frame.valid()) {
                    return false;
                }
                out.image = rin::ImageU8::wrap(rin::PortType::Gray8, frame.width, frame.height,
                                               frame.stride, frame.pixels);
                if (!out.image.valid()) {
                    return false;
                }
                out.sourceSequence = frame.sequence;
                return true;
            }
            case WorkflowSourceRendition::RgbColor:
                break;
        }
        rin::Frame frame;
        if (!service->tryLoadFrame(rin::FrameKind::Rgb, lastSeenSequence, frame) ||
            !frame.valid()) {
            return false;
        }
        // 相机服务的 RGBA8 rendition 与 source 节点声明输出类型一致；格式不符
        // 将由 runNodeGraph 注入语义显式失败（引擎 Failed），此处按 Rgba8 包装。
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
