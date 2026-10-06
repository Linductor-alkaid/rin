#pragma once

// 节点编辑器画布组装（M5-03 起；M11/DEC-021 重构为工作流页唯一组装层）。
// 五区骨架（DEC-014）随右栏移除改四区：顶工具栏 / 左调色板 / 中画布 / 底部
// 校验事件（右列并排工作流总览，M5-05 语义不变）。参数编辑（M5-04 右面板）
// 迁移为画布节点内嵌控件（ComfyUI/蓝图范式），中间结果查看（M5-04 缩略图）
// 由监看器节点承担（Any→Any 透传 sink，节点内嵌预览窗）。
//
// 交互结构（DEC-015 决策 2 + M11/DEC-021 决策 4）：画布内全部指针语义由覆盖
// 视口的一层 `components::mouseArea` 承载，经 canvas_model.hpp 的命中检测与
// 交互状态机分发；**该层先合成（视觉层之下）**，节点内嵌交互控件（滑条/输入/
// 开关/胶囊）后合成——EUI 命中测试自顶向下、子先于父、仅 interactive 元素
// 拦截（pinned 4691fc0 core/runtime/runtime_input.h 取证），控件优先接收事件、
// 节点标题/卡片等非交互视觉穿透至画布 mouseArea（拖动/框选/连线语义不变）；
// 滚轮缩放不受影响（hitTestScrollable 谓词控件不匹配则穿透）。
//
// 内嵌参数控件（M11/DEC-021 决策 3）：节点纵向结构 = 标题 → 参数区（按声明
// 序）→ 端口区 → 页脚（last·avg 耗时徽标）。控件映射复用 §5.5 纪律与
// param_model 纯逻辑：Boolean→toggleSwitch、带范围 Integer/Real→slider+当前
// 值文本、Enumeration→点击循环胶囊（决策 6）、RealArray→紧凑矩阵网格（行列
// 步进 + 单元输入）；编辑即经 requestParamUpdate 提交（下一帧生效）、同步
// 拒绝就地报错（control.error）、引擎接受后记入画布模型。相机源节点内嵌全局
// 分辨率入口（DEC-017 决策 4：与预览页共享档位/状态/命令，点击循环档位）。
// ROI 联动约束（M6-06）沿用，输入尺寸源为 per-node 驱动尺寸缓存（pump 刷新）。
//
// 监看器（M11/DEC-021 决策 2）：typeId "viewer"，画布隐藏输出端口（纯逻辑
// portAt 同步跳过），节点内嵌预览窗显示输入图像（pumpMonitorViews 拉取
// tryLoadNodeOutput 上传 GpuFrameView；非 Running 整体排空释放，§4 语义）。
//
// 连线即时类型过滤（§5.3）：拖线中兼容输入端口高亮 brand、不兼容端口降为
// 次级色；Any 输入端口（监看器）恒兼容。
//
// 引擎同步（§5.3/§5.4）：每次图变更经 afterGraphChange 调用 `applyGraph`
// ——UI 预检与引擎准入共用 validateWorkflowGraph 唯一判据；afterGraphChange
// 同时清扫已删节点的内联控件/监看器/驱动尺寸缓存（UI 状态不跨节点存续）。
//
// 原语映射（ui_workspace_design.md §6）：画布视口 stack+clip、节点 rect、
// 端口 rect、连线 polygon（贝塞尔采样 + 法向偏移带状轮廓 wireRibbon）、
// 右键创建菜单为自绘浮层（EUI-20260928-001 先例）、调色板过滤输入为
// components::input。键盘路径（F/Del/Esc）在 app.cpp 的 DslAppConfig::
// onKeyEvent，每项均有鼠标等价路径。
//
// [DEC-014]: ../../docs/decisions/DEC-014-workbench-information-architecture.md
// [DEC-015]: ../../docs/decisions/DEC-015-node-editor-implementation-path.md
// [DEC-021]: ../../docs/decisions/DEC-021-monitor-node-and-inline-editing.md

#include "canvas_model.hpp"
#include "gpu_frame_view.hpp"
#include "param_model.hpp"
#include "perf_model.hpp"
#include "viewer_theme.hpp"

#include <eui_neo.h>

#include <rin/workflow_engine.hpp>
#include <rin/workflow_types.hpp>

#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace viewer {

/// 单参数控件状态（M5-04 随右面板引入；M11 起按节点持有，地址经 std::map
/// 节点稳定——控件回调捕获 map 值引用，节点删除经 afterGraphChange 清扫）。
struct ParamControlState {
    /// 标量文本（Integer/Real 当前值显示；Enumeration 当前选项 id）。
    eui::Signal<std::string> text;
    /// Boolean 开关状态。
    eui::Signal<bool> checked;
    /// Enumeration 下拉开合（M5-04 遗留：内嵌胶囊不使用，保留控件状态位）。
    eui::Signal<bool> open;
    /// RealArray 矩阵编辑器：形状 + 行主序值 + 单元文本（与值一一对应）。
    std::size_t matrixRows = 0;
    std::size_t matrixCols = 0;
    std::vector<double> matrixValues;
    std::vector<eui::Signal<std::string>> matrixCells;
    /// 就地报错（§5.5 同步拒绝）；空串 = 无错。
    std::string error;
};

/// 单节点的内嵌参数控件状态集（paramId → 控件）。
struct NodeParamControls {
    std::map<std::string, ParamControlState> controls;
};

/// 监看器预览视图（M11/DEC-021）：每监看器节点一枚 GL 纹理视图与产物水位；
/// 上传/释放只能发生在渲染线程（pump/onFrame），节点删除与非 Running 排空
/// 经 pumpMonitorViews 完成。
struct MonitorView {
    std::uint64_t lastSeen = 0;  /// 该节点产物通道的上次已见序号。
    GpuFrameView view;
    std::string meta;  /// "宽 x 高 · seq N"（无产物为空）。
};

using MonitorViews = std::map<rin::NodeId, MonitorView>;

/// 输入驱动节点最新产物尺寸（M11：M6-06 ROI 联动约束的 per-node 尺寸源，
/// 替代右面板 NodeOutputCache 的选中节点消费）。
struct DriverOutputSize {
    std::uint64_t lastSeen = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

/// 相机分辨率入口绑定（M6-06，DEC-017）：分辨率是相机流全局属性——源节点
/// 内嵌入口与预览页选择器共享同一档位列表、同一选择状态（ViewerContext）与
/// 同一 `requestResolution` 命令（全局 restream）。labels/index 由 app.cpp
/// 组装期绑定（仅 compose 调用期有效）；M11 起内嵌入口为点击循环档位
/// （DEC-021 决策 6），无弹层开合状态。
struct CameraResolutionBinding {
    const std::vector<std::string>* labels = nullptr;
    int selectedIndex = -1;
    std::function<void(int)> onPick;  /// 浮层选项点击（M11 验收反馈：下拉选择）。
};

/// 工作流页画布会话状态（ViewerContext 持有，地址稳定供闭包引用；DEC-014
/// 决策 4：页面 UI 状态不因导航丢失，导航层不触碰本结构）。
struct WorkflowCanvasState {
    /// 画布图模型；catalog 指向引擎目录（构建期确定，随引擎生命周期）。
    CanvasGraphModel model;
    CanvasView view;
    CanvasInteraction interaction;
    /// 画布视口尺寸与窗口绝对矩形（compose 时写入；落点/菜单坐标换算）。
    CanvasPoint viewport{0.0f, 0.0f};
    CanvasRect areaRect{0.0f, 0.0f, 0.0f, 0.0f};
    /// 最近悬停点（视口局部坐标；滚轮缩放锚点，§5.1 以指针为锚点）。
    CanvasPoint lastMouse{0.0f, 0.0f};

    // 调色板拖出创建（§5.2.1）：拖拽过程显示类型名跟随光标。
    bool paletteDragging = false;
    std::string paletteDragType;
    CanvasPoint paletteDragPos{0.0f, 0.0f};  // 窗口逻辑坐标（mouseArea 全局）。

    // 画布右键创建菜单（§5.2.2，自绘浮层；即输即筛）。
    bool menuOpen = false;
    CanvasPoint menuPos{0.0f, 0.0f};
    // 相机分辨率下拉浮层（M11 用户验收反馈：下拉选择替代点击循环；同
    // 创建菜单的窗口级 overlay 模式——画布 clip 视口内组件弹层会被裁剪）。
    bool resolutionMenuOpen = false;
    CanvasPoint resolutionMenuPos{0.0f, 0.0f};  // 窗口逻辑坐标。
    eui::Signal<std::string> menuFilter;
    eui::Signal<std::string> paletteFilter;
    /// 调色板滚动偏移（M7-03，scrollView bind；目录/过滤变化由 contentKey
    /// 重测量，滚动位置随信号保持）。
    eui::Signal<float> paletteScroll{0.0f};

    /// 四区 docking 布局（M7-04 引入；M11/DEC-021 移除右栏后余两处分隔条）：
    /// 调色板宽与底部高可调，会话级，不持久化。
    float paletteWidth = 200.0f;  /// 调色板宽 [150, 400]
    float bottomHeight = 120.0f;  /// 底部校验/事件区高 [72, 300]
    /// 活动分隔条（-1 无；0 = palette 右缘、1 = 底部上缘）与拖拽起始。
    int dockDrag = -1;
    float dockDragStartPointer = 0.0f;
    float dockDragStartValue = 0.0f;

    /// 节点内嵌参数控件状态（M11：按节点持有；afterGraphChange 清扫已删节点）。
    std::map<rin::NodeId, NodeParamControls> controls;
    /// 监看器预览视图（M11；渲染线程 pump 独占写入）。
    MonitorViews monitors;
    /// 输入驱动节点产物尺寸缓存（M11：ROI 联动约束尺寸源；非 Running 清空）。
    std::map<rin::NodeId, DriverOutputSize> driverSizes;

    /// 最近一次操作反馈（§5.4 拒绝原因显式反馈，不静默失败）。
    std::string feedback;
    /// 引擎最新事件行（底部列表；pump 消费 tryLoadEvent 写入）。
    std::string lastEvent;
    /// 节点执行失败标注（M5-04 §4：NodeFailed → destructive 徽标；会话级，
    /// Started/Stopped 事件清空；事件消费在 app.cpp pump）。
    NodeFailureMarks failures;
    /// 性能面板统计管道（M5-05 §5.7：sequence 推进消费 + 停止冻结语义；
    /// 消费在 app.cpp pump，工具栏状态徽标/节点耗时徽标/底部总览读取）。
    WorkflowPerfState perf;
    /// 待生效标注（M5-06 §4）：Running 下图结构变更已入队引擎、尚未经
    /// GraphApplied 帧边界应用。afterGraphChange 置位（校验通过才入队），
    /// pump 消费 GraphApplied 事件或引擎离开 Running（stop 排空待生效队列，
    /// 画布图即待运行图）时清除；帧边界拒绝（Info 事件）时保持——画布图与
    /// 生效图确实不一致，"待生效"如实呈现。
    bool graphPending = false;
    /// 画布内容修订号（连线 polygon 的显式脏键，EUI-20260924-001 绕行）。
    std::uint64_t revision = 0;

    /// 图变更统一出口：UI 预检与引擎准入共用唯一判据（§5.3）——有引擎时以
    /// applyGraph 的同步校验结果为准（引擎拒绝时保持当前生效图不变，契约），
    /// 无引擎（纯逻辑测试）时本地 revalidate。Running 下同步校验通过即入队
    /// 帧边界应用 → 置 graphPending（§4"待生效"，GraphApplied 后清除）；
    /// Idle/Failed 下同步生效，无待生效语义。同时清扫已删节点的内联控件/
    /// 监看器条目/驱动尺寸缓存（监看器纹理释放归 pumpMonitorViews——GL 释放
    /// 只在渲染线程，此处仅置位让 pump 清理）。
    void afterGraphChange(rin::IWorkflowEngine* engine) {
        if (engine != nullptr) {
            model.validation = engine->applyGraph(model.toGraph());
            graphPending = model.validation.ok &&
                           engine->state() == rin::WorkflowEngineState::Running;
        } else {
            model.revalidate();
            graphPending = false;
        }
        sweepStaleNodeState();
        ++revision;
    }

    /// 清扫已不在画布图内的节点状态条目（controls/driverSizes；监看器条目
    /// 含 GL 纹理，清扫归 pumpMonitorViews——release 只能在渲染线程）。
    void sweepStaleNodeState() {
        const auto known = [this](rin::NodeId id) {
            return model.findNode(id) != nullptr;
        };
        for (auto it = controls.begin(); it != controls.end();) {
            it = known(it->first) ? std::next(it) : controls.erase(it);
        }
        for (auto it = driverSizes.begin(); it != driverSizes.end();) {
            it = known(it->first) ? std::next(it) : driverSizes.erase(it);
        }
    }

    /// 画布局部（视口）坐标 → 画布坐标。
    [[nodiscard]] CanvasPoint toCanvas(const CanvasPoint& local) const {
        return view.toCanvas(local);
    }

    /// 窗口全局坐标 → 画布坐标（落点/菜单创建位置）。
    [[nodiscard]] CanvasPoint globalToCanvas(const CanvasPoint& global) const {
        return view.toCanvas({global.x - areaRect.x, global.y - areaRect.y});
    }
};

/// 引擎事件 → 底部列表一行（契约无事件种类字符串，UI 呈现自绘）。
[[nodiscard]] inline std::string workflowEventLine(const rin::WorkflowEvent& event) {
    const char* kind = "event";
    switch (event.kind) {
        case rin::WorkflowEventKind::Started:
            kind = "started";
            break;
        case rin::WorkflowEventKind::GraphApplied:
            kind = "graph applied";
            break;
        case rin::WorkflowEventKind::ParamUpdated:
            kind = "param updated";
            break;
        case rin::WorkflowEventKind::Info:
            kind = "info";
            break;
        case rin::WorkflowEventKind::NodeFailed:
            kind = "node failed";
            break;
        case rin::WorkflowEventKind::Stopped:
            kind = "stopped";
            break;
        case rin::WorkflowEventKind::Failed:
            kind = "failed";
            break;
    }
    std::string line = std::string("[") + kind + "]";
    if (!event.message.empty()) {
        line += " " + event.message;
    }
    return line;
}

/// 校验问题类别 → 列表短标签（kind + node 定位 + message，§4）。
[[nodiscard]] inline const char* validationKindLabel(const rin::ValidationIssueKind kind) {
    switch (kind) {
        case rin::ValidationIssueKind::InvalidNodeId:
            return "invalid id";
        case rin::ValidationIssueKind::DuplicateNodeId:
            return "duplicate id";
        case rin::ValidationIssueKind::UnknownNodeType:
            return "unknown type";
        case rin::ValidationIssueKind::PortOutOfRange:
            return "port out of range";
        case rin::ValidationIssueKind::DirectionMismatch:
            return "direction mismatch";
        case rin::ValidationIssueKind::UnknownConnectionNode:
            return "unknown connection node";
        case rin::ValidationIssueKind::TypeMismatch:
            return "type mismatch";
        case rin::ValidationIssueKind::MultipleDrivers:
            return "multiple drivers";
        case rin::ValidationIssueKind::SelfLoop:
            return "self loop";
        case rin::ValidationIssueKind::Cycle:
            return "cycle";
        case rin::ValidationIssueKind::DanglingInput:
            return "dangling input";
        case rin::ValidationIssueKind::BadParam:
            return "bad param";
    }
    return "issue";
}

namespace {

using viewer::theme::kBorderHairline;
using viewer::theme::kFontCaption;
using viewer::theme::kFontSm;
using viewer::theme::kFontXs;
using viewer::theme::kRadiusLg;
using viewer::theme::kRadiusMd;
using viewer::theme::kRadiusSm;
using viewer::theme::kSpace1;
using viewer::theme::kSpace2;
using viewer::theme::kSpace3;

/// 节点标题字号随缩放（下限保持可读；缩放上限 2.5 时 35px 仍为单行）。
[[nodiscard]] inline float scaledFont(const float base, const float scale,
                                      const float minimum = 8.0f) {
    return std::max(minimum, base * scale);
}

/// 内嵌控件输入样式（M5-03 调色板过滤框同源：样式字段逐一取 viewer 令牌）。
inline components::InputStyle inlineInputStyle(const theme::ThemeTokens& tokens) {
    components::InputStyle style;
    style.background = tokens.input;
    style.focused = tokens.input;
    style.border = tokens.inputBorder;
    style.focusBorder = tokens.brand;
    style.text = tokens.fg;
    style.placeholder = tokens.fgSubtlest;
    style.cursor = tokens.brand;
    style.shadow = core::Shadow{};
    style.radius = kRadiusSm;
    return style;
}

/// 参数编辑提交（§5.5 语义不变，M11 起由内嵌控件调用）：纯逻辑校验 → 引擎
/// requestParamUpdate（同步拒绝 → 就地报错；引擎优先于模型，保证模型与引擎
/// 目标图一致）→ 画布模型记账（下次图结构变更随 applyGraph 携带）。不经
/// afterGraphChange：参数热更新有独立通道，Running 下重复 applyGraph 会触发
/// 图重建事件刷屏。返回是否提交成功。
[[nodiscard]] inline bool submitParamAssignment(WorkflowCanvasState& canvas,
                                                rin::IWorkflowEngine* engine,
                                                const rin::NodeId node,
                                                const rin::ParamDescriptor& descriptor,
                                                rin::ParamValue value,
                                                ParamControlState& control) {
    const ParamEditResult result = makeParamAssignment(descriptor, std::move(value));
    if (!result.ok) {
        control.error = result.error;
        canvas.feedback = "'" + descriptor.label + "': " + result.error;
        return false;
    }
    if (engine != nullptr) {
        std::string error;
        if (!engine->requestParamUpdate(node, descriptor.id, result.assignment.value, &error)) {
            control.error = error.empty() ? "parameter update rejected" : std::move(error);
            canvas.feedback = "'" + descriptor.label + "': " + control.error;
            return false;
        }
    }
    const CanvasOpResult applied = canvas.model.setParam(node, result.assignment);
    if (!applied.ok) {
        control.error = applied.error;
        canvas.feedback = "'" + descriptor.label + "': " + applied.error;
        return false;
    }
    control.error.clear();
    return true;
}

/// 节点内嵌控件绑定（M11：M5-04 ensurePanelBindings 的 per-node 化）：首次
/// compose 时按节点描述符以生效值初始化控件初值（幂等；map 值地址稳定供
/// 回调引用）。
inline void ensureNodeControls(WorkflowCanvasState& canvas, const CanvasNode& node,
                               const rin::NodeDescriptor& descriptor) {
    if (canvas.controls.find(node.id) != canvas.controls.end()) {
        return;
    }
    NodeParamControls& entry = canvas.controls[node.id];
    for (const rin::ParamDescriptor& pd : descriptor.params) {
        ParamControlState& control = entry.controls[pd.id];
        const rin::ParamValue* effective =
            effectiveParamValue(descriptor, node.params, pd.id);
        if (effective == nullptr) {
            continue;  // 声明内参数必有默认值；防御性跳过。
        }
        switch (pd.kind) {
            case rin::ParamKind::Boolean:
                control.checked.set(std::get<bool>(*effective));
                break;
            case rin::ParamKind::Integer:
            case rin::ParamKind::Real:
                control.text.set(paramValueText(*effective));
                break;
            case rin::ParamKind::Enumeration:
                control.text.set(std::get<std::string>(*effective));
                control.open.set(false);
                break;
            case rin::ParamKind::RealArray: {
                const RealArrayGrid grid =
                    RealArrayGrid::fromFlat(std::get<std::vector<double>>(*effective));
                control.matrixRows = grid.rows;
                control.matrixCols = grid.cols;
                control.matrixValues = grid.values;
                control.matrixCells.resize(grid.values.size());
                for (std::size_t i = 0; i < grid.values.size(); ++i) {
                    control.matrixCells[i].set(formatRealText(grid.values[i]));
                }
                break;
            }
        }
    }
}

// --- 工具栏（§5.8 运行控制，M5-06：启动/停止 + 待生效标注；本区：标题/校验状态/
// 引擎状态徽标/待生效/反馈/运行按钮/Fit。状态徽标按 §5.7：四态点标 + toString，
// 色经 workflowStateColor） ---
//
// 按钮语义（§4/§5.8）：启动可用条件 = 当前图校验通过且引擎 Idle/Failed（Failed
// 的恢复路径 = 修复后重新启动；Failed 下 stop 先回到 Idle，契约"Failed 为运行
// 终态，stop 后可重启"）；停止可用条件 = Running/Failed（幂等，Failed 下作用为
// 显式回到 Idle）；Stopping 下控件全部禁用至回到 Idle。无暂停（契约无暂停语义）。
// start 的准入失败以 AdmissionResult.error 显式呈现（反馈行），不静默。stop() 为
// owner 线程同步调用、阻塞至在飞帧任务回收完成（有界：≤ maxInFlight 帧任务，
// 与 onShutdown 同一纪律，workflow_engine.hpp 契约）。
inline void composeWorkflowToolbar(eui::Ui& ui, WorkflowCanvasState& state,
                                   rin::IWorkflowEngine* engine, const float x,
                                   const float y, const float width, const float height) {
    const theme::ThemeTokens& tokens = theme::dark();
    const rin::WorkflowEngineState engineState =
        engine != nullptr ? engine->state() : rin::WorkflowEngineState::Idle;
    const float pad = kSpace3;
    const float fitWidth = 56.0f;
    const float runWidth = 56.0f;
    const float runGap = 8.0f;
    const float fitX = width - pad - fitWidth;
    const float stopX = fitX - runGap - runWidth;
    const float startX = stopX - runGap - runWidth;
    const bool startEnabled = state.model.validation.ok &&
                              (engineState == rin::WorkflowEngineState::Idle ||
                               engineState == rin::WorkflowEngineState::Failed);
    const bool stopEnabled = engineState == rin::WorkflowEngineState::Running ||
                             engineState == rin::WorkflowEngineState::Failed;

    const auto runButton = [&](const char* id, const char* label, const float bx,
                               const bool enabled, const std::function<void()>& onClick) {
        ui.rect(std::string("workflow.toolbar.") + id)
            .position(bx, (height - 26.0f) * 0.5f)
            .size(runWidth, 26.0f)
            .radius(kRadiusMd)
            .color(tokens.input)
            .border(kBorderHairline, enabled ? tokens.inputBorder : tokens.cardBorder)
            .states(tokens.input, enabled ? tokens.inputBorderHover : tokens.inputBorder,
                    enabled ? tokens.inputBorderHover : tokens.inputBorder)
            .onClick([onClick, enabled] {
                if (enabled) {
                    onClick();
                }
            })
            .build();
        ui.text(std::string("workflow.toolbar.") + id + "Text")
            .position(bx, (height - 26.0f) * 0.5f)
            .size(runWidth, 26.0f)
            .text(label)
            .fontSize(kFontSm)
            .color(enabled ? tokens.fgSubtle : tokens.fgSubtlest)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .build();
    };

    ui.stack("workflow.toolbar")
        .position(x, y)
        .size(width, height)
        .content([&] {
            ui.rect("workflow.toolbar.card")
                .size(width, height)
                .radius(kRadiusLg)
                .color(tokens.card)
                .border(kBorderHairline, tokens.cardBorder)
                .build();
            ui.text("workflow.toolbar.title")
                .position(pad, 0.0f)
                .size(120.0f, height)
                .text("Workflow")
                .fontSize(theme::kFontBase)
                .fontWeight(theme::kWeightSemibold)
                .color(tokens.fg)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            char issues[64];
            if (state.model.validation.ok) {
                std::snprintf(issues, sizeof(issues), "graph valid");
            } else {
                std::snprintf(issues, sizeof(issues), "%zu issue(s)",
                              state.model.validation.issues.size());
            }
            ui.text("workflow.toolbar.status")
                .position(120.0f + pad, 0.0f)
                .size(200.0f, height)
                .text(issues)
                .fontSize(kFontSm)
                .color(state.model.validation.ok ? tokens.fgSubtlest : tokens.warning)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            // 引擎状态徽标（§5.7 执行状态）：状态点 + 文本，语义色映射与 M5-02
            // 令牌扩展同源。
            const eui::Color stateTint = theme::workflowStateColor(engineState);
            ui.rect("workflow.toolbar.stateDot")
                .position(344.0f, (height - kSpace2) * 0.5f)
                .size(kSpace2, kSpace2)
                .radius(kSpace2)
                .color(stateTint)
                .build();
            ui.text("workflow.toolbar.state")
                .position(344.0f + kSpace2 + kSpace2, 0.0f)
                .size(90.0f, height)
                .text(rin::toString(engineState))
                .fontSize(kFontSm)
                .color(stateTint)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            // 待生效标注（§4）：Running 下图结构变更入队引擎后、GraphApplied
            // 帧边界应用前呈现（消费在 app.cpp pump；帧边界拒绝时如实保持——
            // 画布图与生效图确实不一致）。
            if (state.graphPending) {
                ui.text("workflow.toolbar.pending")
                    .position(452.0f, 0.0f)
                    .size(190.0f, height)
                    .text("pending - applies next frame")
                    .fontSize(kFontXs)
                    .color(tokens.warning)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
            }
            // 帧全图按钮（§5.1 快捷键 F 的鼠标等价路径）。
            ui.rect("workflow.toolbar.fit")
                .position(fitX, (height - 26.0f) * 0.5f)
                .size(fitWidth, 26.0f)
                .radius(kRadiusMd)
                .color(tokens.input)
                .border(kBorderHairline, tokens.inputBorder)
                .states(tokens.input, tokens.inputBorderHover, tokens.inputBorderHover)
                .onClick([&state] {
                    state.view.fit(state.model.graphBounds(), state.viewport.x,
                                   state.viewport.y);
                    ++state.revision;
                })
                .build();
            ui.text("workflow.toolbar.fitText")
                .position(fitX, (height - 26.0f) * 0.5f)
                .size(fitWidth, 26.0f)
                .text("Fit")
                .fontSize(kFontSm)
                .color(tokens.fgSubtle)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            // 运行控制（§5.8，M5-06）：启动/停止；回调反馈经反馈行显式呈现。
            runButton("start", "Start", startX, startEnabled, [&state, engine] {
                if (engine == nullptr) {
                    return;
                }
                if (engine->state() == rin::WorkflowEngineState::Failed) {
                    engine->stop();  // Failed 为运行终态：stop 回 Idle 后方可重启。
                }
                const rin::AdmissionResult admission = engine->start();
                state.feedback =
                    admission.admitted
                        ? ""
                        : (admission.error.empty() ? "start rejected" : admission.error);
            });
            runButton("stop", "Stop", stopX, stopEnabled, [&state, engine] {
                if (engine != nullptr) {
                    engine->stop();  // 幂等；有界排空（见函数头注释）。
                    state.feedback.clear();
                }
            });
            if (!state.feedback.empty()) {
                ui.text("workflow.toolbar.feedback")
                    .position(452.0f, 0.0f)
                    .size(startX - 452.0f - runGap, height)
                    .text(state.feedback)
                    .fontSize(kFontSm)
                    .color(tokens.fgSubtlest)
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .maxWidth(startX - 452.0f - runGap)
                    .build();
            }
        })
        .build();
}

// --- 调色板（§5.2：目录分组 + 即输即筛 + 拖出创建；M7-03 起条目区为
// components::scrollView 滚动容器——pinned 组件，小窗口/目录扩展时条目可滚动） ---
inline void composeWorkflowPalette(eui::Ui& ui, WorkflowCanvasState& state,
                                   rin::IWorkflowEngine* engine, const float x,
                                   const float y, const float width, const float height) {
    const theme::ThemeTokens& tokens = theme::dark();
    const float pad = kSpace3;
    const float inputHeight = 28.0f;

    ui.stack("workflow.palette")
        .position(x, y)
        .size(width, height)
        .content([&] {
            ui.rect("workflow.palette.card")
                .size(width, height)
                .radius(kRadiusLg)
                .color(tokens.card)
                .border(kBorderHairline, tokens.cardBorder)
                .build();
            ui.text("workflow.palette.title")
                .position(pad, pad)
                .size(width - pad * 2.0f, kFontSm + kSpace1)
                .text("Palette")
                .fontSize(kFontSm)
                .fontWeight(theme::kWeightSemibold)
                .color(tokens.fgSubtle)
                .build();

            // 即输即筛（§5.2）：过滤框样式字段逐一取 viewer 令牌（DEC-005），
            // 不引入组件主题度量（inset/字号显式给定）。
            components::InputStyle inputStyle;
            inputStyle.background = tokens.input;
            inputStyle.focused = tokens.input;
            inputStyle.border = tokens.inputBorder;
            inputStyle.focusBorder = tokens.brand;
            inputStyle.text = tokens.fg;
            inputStyle.placeholder = tokens.fgSubtlest;
            inputStyle.cursor = tokens.brand;
            inputStyle.shadow = core::Shadow{};
            inputStyle.radius = kRadiusMd;
            components::input(ui, "workflow.palette.filter")
                .position(pad, pad + kFontSm + kSpace2)
                .size(width - pad * 2.0f, inputHeight)
                .bind(state.paletteFilter)
                .placeholder("filter")
                .fontSize(kFontSm)
                .inset(kSpace2)
                .style(inputStyle)
                .build();

            // 目录引用必须两侧同为左值：`cond ? *catalog : NodeCatalog{}` 会因
            // 左值/纯右值混合把左值分支拷贝成临时目录，语句结束时 items 里指向
            // 其节点元素的指针全部悬垂（M5-04 冒烟实证的 M5-03 潜伏缺陷）。
            static const rin::NodeCatalog kEmptyPaletteCatalog{};
            const rin::NodeCatalog& paletteCatalog =
                state.model.catalog != nullptr ? *state.model.catalog
                                               : kEmptyPaletteCatalog;
            const std::string filter = state.paletteFilter.get();

            // 滚动条目区（M7-03）：scrollView 内容为列布局流式行（组标题 +
            // 条目行，行内绝对定位）；contentKey=过滤词驱动重测量。滚动偏移
            // 绑定会话信号，切页保持。内容 compose 会被测量趟额外调用（丢弃
            // 式 UI）——内容仅绘制与登记回调，无状态副作用；条目回调捕获
            // &state/descriptor（目录指针稳定）。滚动条样式字段逐一取 viewer
            // 令牌（DEC-005 组件接线纪律）。
            const float groupTitleHeight = kFontXs + kSpace2;
            const float itemHeight = 26.0f;
            components::ScrollStyle scrollStyle;
            scrollStyle.track = tokens.surface;
            scrollStyle.thumb = tokens.inputBorder;
            scrollStyle.thumbHover = tokens.fgSubtlest;
            scrollStyle.thumbPressed = tokens.brand;
            scrollStyle.radius = kRadiusSm;
            components::scrollView(ui, "workflow.palette.scroll")
                .position(pad, pad + kFontSm + kSpace2 + inputHeight + kSpace2)
                .size(width - pad * 2.0f,
                      height - (pad + kFontSm + kSpace2 + inputHeight + kSpace2) - pad)
                .bind(state.paletteScroll)
                .contentKey(filter)
                .gap(kSpace1)
                .style(scrollStyle)
                .content([&state, engine, &paletteCatalog, &filter, &tokens,
                          itemHeight, groupTitleHeight](eui::Ui& listUi,
                                                        float contentWidth, float) {
                    const std::vector<PaletteGroup> groups =
                        paletteGroups(paletteCatalog, filter);
                    for (const PaletteGroup& group : groups) {
                        listUi.text("workflow.palette.group." + group.title)
                            .size(contentWidth, groupTitleHeight)
                            .text(group.title)
                            .fontSize(kFontXs)
                            .color(tokens.fgSubtlest)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .build();
                        for (const rin::NodeDescriptor* descriptor : group.items) {
                            const std::string base =
                                std::string("workflow.palette.item.") + descriptor->typeId;
                            const eui::Color transparent{0.0f, 0.0f, 0.0f, 0.0f};
                            listUi.stack(base + ".row")
                                .size(contentWidth, itemHeight)
                                .content([&] {
                                    listUi.rect(base + ".rowBg")
                                        .size(contentWidth, itemHeight)
                                        .radius(kRadiusSm)
                                        .color(transparent)
                                        .states(transparent, tokens.menuHover,
                                                tokens.menuHover)
                                        .build();
                                    listUi.text(base + ".label")
                                        .position(kSpace2, 0.0f)
                                        .size(contentWidth - kSpace2, itemHeight)
                                        .text(descriptor->displayName)
                                        .fontSize(kFontCaption)
                                        .color(tokens.fg)
                                        .verticalAlign(eui::VerticalAlign::Center)
                                        .build();
                                    // 拖出创建（§5.2.1）：拖拽越过阈值才进入拖拽
                                    // 态（浮层跟随光标），落在画布内创建、画布外
                                    // 取消；原位单击不触发。
                                    components::mouseArea(listUi, base + ".drag")
                                        .size(contentWidth, itemHeight)
                                        .cursor(eui::CursorShape::Hand)
                                        .onDragStart(
                                            [&state, descriptor](
                                                const components::MouseEvent& event) {
                                                state.paletteDragging = true;
                                                state.paletteDragType = descriptor->typeId;
                                                state.paletteDragPos = {event.globalX,
                                                                        event.globalY};
                                            })
                                        .onDrag([&state](
                                                    const components::MouseDragEvent&
                                                        event) {
                                            state.paletteDragPos = {event.globalX,
                                                                    event.globalY};
                                        })
                                        .onDragEnd([&state, engine](
                                                       const components::MouseDragEvent&
                                                           event) {
                                            state.paletteDragging = false;
                                            if (!state.areaRect.contains(
                                                    {event.globalX, event.globalY})) {
                                                return;  // §5.2.1 落点在画布外 = 取消。
                                            }
                                            const CanvasPoint canvas =
                                                state.globalToCanvas(
                                                    {event.globalX, event.globalY});
                                            const CanvasPoint origin{
                                                canvas.x - kNodeWidth * 0.5f,
                                                canvas.y - kNodeHeaderHeight * 0.5f};
                                            const CanvasOpResult result =
                                                state.model.createNode(state.paletteDragType,
                                                                       origin);
                                            state.feedback = result.ok ? "" : result.error;
                                            state.afterGraphChange(engine);
                                        })
                                        .build();
                                })
                                .build();
                        }
                    }
                })
                .build();
        })
        .build();
}

// --- 节点内嵌参数区（M11/DEC-021 决策 3：ComfyUI/蓝图范式，右栏语义就地化） ---
//
// 行布局与 canvas_model.hpp 的 paramRowHeight/paramSectionHeight 同源（绘制与
// 命中共用度量）；控件尺寸/字号随画布缩放（scaledFont）。枚举=点击循环胶囊
// （决策 6）、ROI 联动约束沿用（有效域映射，M6-06 的"越界从控件不可达"由
// 滑条域内取值保持）。同步拒绝不改变行布局（几何纯函数不含动态错误行）：
// 参数行右上以 destructive "!" 标注、完整原因经画布反馈行（工具栏）显式
// 呈现，不静默（§5.4/§5.5）。控件回调按值捕获节点 id（画布图变更会重排节点
// 数组，不持节点指针）。
inline void composeInlineParams(eui::Ui& ui, WorkflowCanvasState& state,
                                const CanvasNode& node, const rin::NodeDescriptor& descriptor,
                                rin::IWorkflowEngine* engine,
                                const CameraResolutionBinding* cameraResolution,
                                const float s) {
    const theme::ThemeTokens& tokens = theme::dark();
    ensureNodeControls(state, node, descriptor);
    NodeParamControls& entry = state.controls[node.id];
    const rin::NodeId nodeId = node.id;  // 回调按值捕获，不持画布节点指针。

    // ROI 联动约束（M6-06，DEC-017）：输入尺寸取驱动节点最新产物尺寸缓存
    // （pumpDriverSizes 刷新；未知尺寸回退声明范围，apply 期拒绝兜底）。
    rin::NodeId roiDriver = rin::kInvalidNode;
    for (const rin::Connection& connection : state.model.connections) {
        if (connection.to.node == node.id &&
            connection.to.direction == rin::PortDirection::Input &&
            connection.to.index == 0) {
            roiDriver = connection.from.node;
            break;
        }
    }
    RoiConstraint roi = RoiConstraint::unconstrained();
    if (roiDriver != rin::kInvalidNode) {
        if (const auto it = state.driverSizes.find(roiDriver); it != state.driverSizes.end()) {
            roi = RoiConstraint::forInput(it->second.width, it->second.height);
        }
    }

    const float innerX = kParamPadX;
    const float innerW = kNodeWidth - kParamPadX * 2.0f;
    const components::InputStyle inputStyle = inlineInputStyle(tokens);
    // 同步拒绝标注（不占行布局）：参数行标签区右侧 destructive "!"。
    const auto errorMark = [&ui, &tokens, s](const std::string& base, const float x,
                                             const float y, const bool hasError) {
        if (!hasError) {
            return;
        }
        const float mark = 8.0f * s;
        ui.text(base + ".errMark")
            .position(x * s, y * s)
            .size(mark, kParamLabelHeight * s)
            .text("!")
            .fontSize(scaledFont(kFontXs, s, 5.0f))
            .color(tokens.destructive)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .build();
    };
    float rowY = kNodeHeaderHeight + kParamSectionGap;

    // 相机分辨率入口（M6-06 引入；M11 迁入源节点内嵌，DEC-017 决策 4）：
    // 分辨率是相机流全局属性（单 pipeline），与预览页共享档位/状态/命令，
    // 点击循环档位（DEC-021 决策 6）。行高与 paramSectionHeight 同源
    // （isCameraSourceNode）。
    if (isCameraSourceNode(node.typeId) && cameraResolution != nullptr &&
        cameraResolution->labels != nullptr && !cameraResolution->labels->empty()) {
        const std::string base = "workflow.node." + std::to_string(node.id) + ".cameraRes";
        const int count = static_cast<int>(cameraResolution->labels->size());
        const int selectedIndex =
            std::clamp(cameraResolution->selectedIndex, 0, std::max(0, count - 1));
        ui.text(base + ".label")
            .position(innerX * s, rowY * s)
            .size(innerW * 0.55f * s, kParamPillHeight * s)
            .text("Camera resolution")
            .fontSize(scaledFont(kFontXs, s, 6.0f))
            .color(tokens.fgSubtle)
            .verticalAlign(eui::VerticalAlign::Center)
            .maxWidth(innerW * 0.55f * s)
            .build();
        // 下拉触发器（M11 验收反馈：下拉选择；点击打开窗口级选项浮层，
        // composeWorkflowResolutionMenu，由 app.cpp overlay 层合成）。触发用
        // rect.onClick；浮层窗口坐标 compose 期精确计算（画布区矩形 + 节点
        // 屏幕原点 + 触发器屏幕偏移）。勘误（resolve 升级分析）：首版
        // "mouseArea 在此嵌套下不响应"的结论不成立——EUI 输入路径经无头
        // 探针实证正常，真实断点是浮层 composer 从未接线。
        const float pillW = innerW * 0.45f - kSpace1;
        const float pillX = (innerX + innerW - pillW) * s;
        const float pillY = (rowY + 2.0f) * s;
        const float pillH = (kParamPillHeight - 4.0f) * s;
        const CanvasPoint nodeScreen = state.view.toScreen(node.position);
        const float menuX = state.areaRect.x + nodeScreen.x + pillX;
        const float menuY = state.areaRect.y + nodeScreen.y + pillY + pillH;
        ui.rect(base + ".pill")
            .position(pillX, pillY)
            .size(pillW * s, pillH)
            .radius(kRadiusSm * s)
            .color(tokens.input)
            .border(kBorderHairline, tokens.inputBorder)
            .states(tokens.input, tokens.inputBorderHover, tokens.inputBorderHover)
            .onClick([&state, menuX, menuY] {
                state.menuOpen = false;  // 与创建菜单互斥（双 scrim 不可叠加）。
                state.resolutionMenuOpen = true;
                state.resolutionMenuPos = {menuX, menuY};
            })
            .build();
        ui.text(base + ".value")
            .position(pillX + kSpace1 * s, pillY)
            .size((pillW - 14.0f) * s, pillH)
            .text((*cameraResolution->labels)[static_cast<std::size_t>(selectedIndex)])
            .fontSize(scaledFont(kFontXs, s, 6.0f))
            .color(tokens.fg)
            .verticalAlign(eui::VerticalAlign::Center)
            .maxWidth((pillW - 14.0f) * s)
            .build();
        ui.text(base + ".chevron")
            .position(pillX + (pillW - 12.0f) * s, pillY)
            .size(12.0f * s, pillH)
            .text("\u25BE")
            .fontSize(scaledFont(kFontXs, s, 6.0f))
            .color(tokens.brand)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .build();
        rowY += kParamPillHeight + kParamRowGap;
    }

    for (const rin::ParamDescriptor& pd : descriptor.params) {
        ParamControlState& control = entry.controls[pd.id];
        const std::string base =
            "workflow.node." + std::to_string(node.id) + ".param." + pd.id;

        switch (pd.kind) {
            case rin::ParamKind::Boolean: {
                ui.text(base + ".label")
                    .position(innerX * s, rowY * s)
                    .size(innerW * 0.5f * s, kParamPillHeight * s)
                    .text(pd.label)
                    .fontSize(scaledFont(kFontXs, s, 6.0f))
                    .color(tokens.fgSubtle)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .maxWidth(innerW * 0.5f * s)
                    .build();
                errorMark(base, innerX + innerW * 0.5f + kSpace1, rowY,
                          !control.error.empty());
                components::SwitchStyle switchStyle;
                switchStyle.off = tokens.input;
                switchStyle.on = tokens.brand;
                switchStyle.knob = tokens.fg;
                switchStyle.text = tokens.fg;
                switchStyle.rowHover = tokens.menuHover;
                switchStyle.rowPressed = tokens.menuHover;
                ui.stack(base + ".switchWrap")
                    .position((innerX + innerW * 0.5f) * s, (rowY + 2.0f) * s)
                    .size(innerW * 0.5f * s, (kParamPillHeight - 4.0f) * s)
                    .content([&] {
                        components::toggleSwitch(ui, base + ".switch")
                            .size(innerW * 0.5f * s, (kParamPillHeight - 4.0f) * s)
                            .trackSize(std::max(20.0f, 34.0f * s),
                                       std::max(10.0f, 16.0f * s))
                            .fontSize(scaledFont(kFontXs, s, 6.0f))
                            .style(switchStyle)
                            .checked(control.checked.get())
                            .onChange([&state, engine, nodeId, &pd,
                                       &control](const bool value) {
                                if (submitParamAssignment(state, engine, nodeId, pd, value,
                                                          control)) {
                                    control.checked.set(value);
                                }
                            })
                            .build();
                    })
                    .build();
                rowY += kParamPillHeight;
                break;
            }
            case rin::ParamKind::Integer:
            case rin::ParamKind::Real: {
                const rin::ParamValue* current =
                    effectiveParamValue(descriptor, node.params, pd.id);
                // 标签行：label 左，当前生效值右（编辑即生效，值即回显）。
                ui.text(base + ".label")
                    .position(innerX * s, rowY * s)
                    .size(innerW * 0.55f * s, kParamLabelHeight * s)
                    .text(pd.label)
                    .fontSize(scaledFont(kFontXs, s, 6.0f))
                    .color(tokens.fgSubtle)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .maxWidth(innerW * 0.55f * s)
                    .build();
                errorMark(base, innerX + innerW * 0.55f + kSpace1, rowY,
                          !control.error.empty());
                ui.text(base + ".value")
                    .position(innerX * s, rowY * s)
                    .size(innerW * s, kParamLabelHeight * s)
                    .text(current != nullptr ? paramValueText(*current) : "")
                    .fontSize(scaledFont(kFontXs, s, 6.0f))
                    .color(tokens.fg)
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .maxWidth(innerW * s)
                    .build();
                rowY += kParamLabelHeight;
                // 滑条（目录现存标量参数全部 hasRange）：ROI 参数映射到联动
                // 有效域（M6-06），否则声明域；Integer 就近取整。
                const std::optional<std::pair<double, double>> roiRange =
                    roi.effectiveRange(descriptor, node.params, pd);
                const double currentValue =
                    current != nullptr
                        ? paramScalarAsDouble(*current)
                        : (roiRange ? roiRange->first
                                    : paramScalarAsDouble(pd.defaultValue));
                float normalized = 0.0f;
                if (roiRange) {
                    normalized = static_cast<float>(
                        valueToSliderInRange(roiRange->first, roiRange->second, currentValue));
                } else {
                    normalized = static_cast<float>(
                        valueToSlider(pd, current != nullptr ? *current : pd.defaultValue)
                            .value_or(0.0));
                }
                components::SliderStyle sliderStyle;
                sliderStyle.track = tokens.input;
                sliderStyle.fill = tokens.brand;
                sliderStyle.knob = tokens.fg;
                ui.stack(base + ".sliderWrap")
                    .position(innerX * s, rowY * s)
                    .size(innerW * s, kParamSliderHeight * s)
                    .content([&] {
                        components::slider(ui, base + ".slider")
                            .size(innerW * s, kParamSliderHeight * s)
                            .value(normalized)
                            .style(sliderStyle)
                            .onChange([&state, engine, nodeId, &pd, &control,
                                       roiRange](const float t) {
                                std::optional<double> value;
                                if (roiRange) {
                                    value = sliderToValueInRange(
                                        roiRange->first, roiRange->second, t,
                                        pd.kind == rin::ParamKind::Integer);
                                } else {
                                    value = sliderToValue(pd, t);
                                }
                                if (!value) {
                                    return;
                                }
                                if (pd.kind == rin::ParamKind::Integer) {
                                    const auto integer =
                                        static_cast<std::int64_t>(std::llround(*value));
                                    if (submitParamAssignment(state, engine, nodeId, pd,
                                                              integer, control)) {
                                        control.text.set(std::to_string(integer));
                                    }
                                } else if (submitParamAssignment(state, engine, nodeId, pd,
                                                                 *value, control)) {
                                    control.text.set(formatRealText(*value));
                                }
                            })
                            .build();
                    })
                    .build();
                rowY += kParamSliderHeight;
                break;
            }
            case rin::ParamKind::Enumeration: {
                // 点击循环胶囊（DEC-021 决策 6）：小选项集即时遍历；提交路径
                // 同滑条（requestParamUpdate + 模型记账），拒绝就地标注。
                ui.text(base + ".label")
                    .position(innerX * s, rowY * s)
                    .size(innerW * 0.5f * s, kParamPillHeight * s)
                    .text(pd.label)
                    .fontSize(scaledFont(kFontXs, s, 6.0f))
                    .color(tokens.fgSubtle)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .maxWidth(innerW * 0.5f * s)
                    .build();
                errorMark(base, innerX + innerW * 0.5f + kSpace1, rowY,
                          !control.error.empty());
                const rin::ParamValue* current =
                    effectiveParamValue(descriptor, node.params, pd.id);
                const std::string currentOption =
                    current != nullptr ? std::get<std::string>(*current) : std::string();
                const float pillW = innerW * 0.5f - kSpace1;
                ui.rect(base + ".pill")
                    .position((innerX + innerW - pillW) * s, (rowY + 2.0f) * s)
                    .size(pillW * s, (kParamPillHeight - 4.0f) * s)
                    .radius(kParamPillHeight * 0.5f * s)
                    .color(tokens.input)
                    .border(kBorderHairline, tokens.inputBorder)
                    .states(tokens.input, tokens.inputBorderHover, tokens.inputBorderHover)
                    .onClick([&state, engine, nodeId, &pd, &control,
                                              currentOption] {
                        const std::optional<std::string> next =
                            cycleEnumOption(pd.enumOptions, currentOption);
                        if (!next) {
                            return;
                        }
                        if (submitParamAssignment(state, engine, nodeId, pd, *next, control)) {
                            control.text.set(*next);
                        }
                    })
                    .build();
                ui.text(base + ".value")
                    .position((innerX + innerW - pillW) * s, (rowY + 2.0f) * s)
                    .size(pillW * s, (kParamPillHeight - 4.0f) * s)
                    .text(currentOption + " \u203A")
                    .fontSize(scaledFont(kFontXs, s, 6.0f))
                    .color(tokens.fg)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .maxWidth(pillW * s)
                    .build();
                rowY += kParamPillHeight;
                break;
            }
            case rin::ParamKind::RealArray: {
                // 形状行：label + R x C 左；行列步进右（±R/±C，M5-04 矩阵网格
                // 同语义：重排保留行主序数据，截断/零扩展）。
                ui.text(base + ".shape")
                    .position(innerX * s, rowY * s)
                    .size(innerW * 0.4f * s, kParamGridShapeHeight * s)
                    .text(pd.label + " " + std::to_string(control.matrixRows) + "x" +
                          std::to_string(control.matrixCols))
                    .fontSize(scaledFont(kFontXs, s, 6.0f))
                    .color(tokens.fgSubtle)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .maxWidth(innerW * 0.4f * s)
                    .build();
                errorMark(base, innerX + innerW * 0.35f + kSpace1, rowY,
                          !control.error.empty());
                constexpr const char* kSteppers[] = {"R+", "R-", "C+", "C-"};
                const float stepperW = (innerW * 0.55f - kSpace1 * 3.0f) * 0.25f;
                for (int stepper = 0; stepper < 4; ++stepper) {
                    const float sx = innerX + innerW - (4 - stepper) * (stepperW + kSpace1);
                    ui.rect(base + ".step" + std::to_string(stepper))
                        .position(sx * s, rowY * s)
                        .size(stepperW * s, kParamGridShapeHeight * s)
                        .radius(kRadiusSm * s)
                        .color(tokens.input)
                        .border(kBorderHairline, tokens.inputBorder)
                        .states(tokens.input, tokens.inputBorderHover, tokens.inputBorderHover)
                        .onClick([&state, engine, nodeId, &pd, &control, stepper] {
                            RealArrayGrid grid;
                            grid.rows = control.matrixRows;
                            grid.cols = control.matrixCols;
                            grid.values = control.matrixValues;
                            grid.reshape(grid.rows + (stepper == 0 ? 1 : stepper == 1 ? -1 : 0),
                                         grid.cols + (stepper == 2 ? 1 : stepper == 3 ? -1 : 0));
                            control.matrixRows = grid.rows;
                            control.matrixCols = grid.cols;
                            control.matrixValues = grid.values;
                            control.matrixCells.resize(grid.values.size());
                            for (std::size_t i = 0; i < grid.values.size(); ++i) {
                                control.matrixCells[i].set(formatRealText(grid.values[i]));
                            }
                            (void)submitParamAssignment(state, engine, nodeId, pd,
                                                        grid.values, control);
                        })
                        .build();
                    ui.text(base + ".stepText" + std::to_string(stepper))
                        .position(sx * s, rowY * s)
                        .size(stepperW * s, kParamGridShapeHeight * s)
                        .text(kSteppers[stepper])
                        .fontSize(scaledFont(kFontXs, s, 5.0f))
                        .color(tokens.fgSubtle)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .build();
                }
                rowY += kParamGridShapeHeight;
                // 单元网格（行列数可配；单元宽度随列数均分，随缩放）。
                const float cellGap = 2.0f;
                const float cellW =
                    (innerW - static_cast<float>(control.matrixCols - 1) * cellGap) /
                    static_cast<float>(control.matrixCols);
                for (std::size_t r = 0; r < control.matrixRows; ++r) {
                    for (std::size_t c = 0; c < control.matrixCols; ++c) {
                        const std::size_t index = r * control.matrixCols + c;
                        components::input(ui, base + ".cell" + std::to_string(index))
                            .position((innerX + static_cast<float>(c) * (cellW + cellGap)) * s,
                                      rowY * s)
                            .size(cellW * s, kParamGridCellHeight * s)
                            .value(control.matrixCells[index].get())
                            .fontSize(scaledFont(kFontXs, s, 6.0f))
                            .inset(std::max(2.0f, kSpace1 * s))
                            .style(inputStyle)
                            .onChange([&state, engine, nodeId, &pd, &control,
                                       index](const std::string& text) {
                                control.matrixCells[index].set(text);
                                std::vector<double> values;
                                values.reserve(control.matrixCells.size());
                                bool parsed = true;
                                for (std::size_t i = 0; i < control.matrixCells.size(); ++i) {
                                    const std::optional<double> value =
                                        parseRealText(control.matrixCells[i].get());
                                    if (!value) {
                                        parsed = false;
                                        break;
                                    }
                                    values.push_back(*value);
                                }
                                if (!parsed) {
                                    control.error = "values must be finite numbers";
                                    state.feedback = "'" + pd.label + "': " + control.error;
                                    return;
                                }
                                (void)submitParamAssignment(state, engine, nodeId, pd,
                                                            values, control);
                            })
                            .build();
                    }
                    rowY += kParamGridCellHeight;
                }
                break;
            }
        }
        rowY += kParamRowGap;
    }
}

// --- 节点框（§5.3 端口纵向排布 / §4 选中与问题描边；M11：内嵌参数区与监看器
// 预览窗参与节点几何） ---
inline void composeCanvasNode(eui::Ui& ui, WorkflowCanvasState& state, const CanvasNode& node,
                              const std::optional<rin::PortType>& draftType,
                              rin::IWorkflowEngine* engine,
                              const CameraResolutionBinding* cameraResolution) {
    const theme::ThemeTokens& tokens = theme::dark();
    const rin::NodeDescriptor* descriptor = state.model.descriptorFor(node);
    if (descriptor == nullptr) {
        return;  // 目录缺项（契约校验 UnknownNodeType）：跳过绘制，问题走列表。
    }
    const float s = state.view.scale;
    const CanvasPoint origin = state.view.toScreen(node.position);
    const float w = kNodeWidth * s;
    const float h = nodeHeight(*descriptor, node.params) * s;
    const bool selected =
        std::find(state.model.selection.begin(), state.model.selection.end(), node.id) !=
        state.model.selection.end();
    const bool invalid = state.model.nodeHasIssue(node.id);
    // 节点错误可视化（M5-04 §4）：执行失败（NodeFailed）与校验问题同样以
    // destructive 描边标注，失败另加徽标；消息走底部事件列表。
    const bool failed = state.failures.failureOf(node.id) != nullptr;
    const bool monitor = isMonitorNode(node.typeId);
    const std::string base = "workflow.canvas.node." + std::to_string(node.id);

    ui.stack(base)
        .position(origin.x, origin.y)
        .size(w, h)
        .content([&] {
            ui.rect(base + ".card")
                .size(w, h)
                .radius(std::max(2.0f, kRadiusMd * s))
                .color(selected ? tokens.accentSurface : tokens.card)
                .border(kBorderHairline,
                        (invalid || failed) ? tokens.destructive
                                            : (selected ? tokens.brand : tokens.cardBorder))
                .build();
            ui.text(base + ".title")
                .position(kSpace2 * s, 0.0f)
                .size(w - kSpace2 * 2.0f * s, kNodeHeaderHeight * s)
                .text(descriptor->displayName)
                .fontSize(scaledFont(kFontSm, s))
                .color(tokens.fg)
                .verticalAlign(eui::VerticalAlign::Center)
                .maxWidth(w - kSpace2 * 2.0f * s)
                .build();
            if (failed) {
                const float badge = 12.0f * s;
                ui.rect(base + ".failedBadge")
                    .position(w - badge - 3.0f * s, 3.0f * s)
                    .size(badge, badge)
                    .radius(badge * 0.5f)
                    .color(tokens.destructive)
                    .build();
                ui.text(base + ".failedBadge.mark")
                    .position(w - badge - 3.0f * s, 3.0f * s)
                    .size(badge, badge)
                    .text("!")
                    .fontSize(scaledFont(kFontXs, s, 6.0f))
                    .color(tokens.background)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
            }

            // 监看器预览窗（M11/DEC-021）：输入图像经 pumpMonitorViews 上传，
            // contain 适配预览区；meta 行悬浮右上（宽高 · 源帧序号）。
            if (monitor) {
                const float previewY = (kNodeHeaderHeight + kParamSectionGap * 0.5f) * s;
                const float previewH = (kMonitorPreviewHeight - kParamSectionGap * 0.5f) * s;
                const float previewW = (kNodeWidth - kParamPadX * 2.0f) * s;
                const float previewX = kParamPadX * s;
                ui.rect(base + ".previewArea")
                    .position(previewX, previewY)
                    .size(previewW, previewH)
                    .radius(kRadiusSm * s)
                    .color(tokens.surface)
                    .border(kBorderHairline, tokens.border)
                    .build();
                if (const auto it = state.monitors.find(node.id); it != state.monitors.end() &&
                                                                 it->second.view.valid()) {
                    ui.image(base + ".previewImg")
                        .position(previewX, previewY)
                        .size(previewW, previewH)
                        .texture(it->second.view.image(), it->second.view.revision())
                        .contain()
                        .radius(kRadiusSm * s)
                        .build();
                    if (!it->second.meta.empty()) {
                        ui.text(base + ".previewMeta")
                            .position(previewX, previewY)
                            .size(previewW, kParamLabelHeight * s)
                            .text(it->second.meta)
                            .fontSize(scaledFont(kFontXs, s, 6.0f))
                            .color(tokens.fgSubtlest)
                            .horizontalAlign(eui::HorizontalAlign::Right)
                            .verticalAlign(eui::VerticalAlign::Center)
                            .maxWidth(previewW)
                            .build();
                    }
                } else {
                    ui.text(base + ".previewEmpty")
                        .position(previewX, previewY)
                        .size(previewW, previewH)
                        .text("connect & run to preview")
                        .fontSize(scaledFont(kFontXs, s, 6.0f))
                        .color(tokens.fgSubtlest)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .build();
                }
            } else {
                // 内嵌参数区（M11/DEC-021 决策 3）。
                composeInlineParams(ui, state, node, *descriptor, engine, cameraResolution, s);
            }

            // 端口（视觉尺寸 kPortVisualRadius；输入左缘、输出右缘，§5.3）。
            // 监看器输出端口不绘制（M11/DEC-021：透传 sink 的输出在交互面
            // 不存在；portAt 同步跳过，不可拖线）。
            const float r = kPortVisualRadius * s;
            const auto portColor = [&](const rin::PortType type,
                                       const rin::PortDirection direction) {
                if (draftType && direction == rin::PortDirection::Input) {
                    // 拖线中即时类型过滤（§5.3）：兼容高亮 brand，不兼容降次级；
                    // Any 输入（监看器）恒兼容（M11/DEC-021）。
                    return (*draftType == type || type == rin::PortType::Any) ? tokens.brand
                                                                              : tokens.fgSubtlest;
                }
                return theme::portTypeColor(type);
            };
            for (std::size_t i = 0; i < descriptor->inputs.size(); ++i) {
                const rin::PortRef port{node.id, rin::PortDirection::Input,
                                        static_cast<std::uint32_t>(i)};
                const CanvasPoint local =
                    portPosition(node.position, *descriptor, node.params, port) - node.position;
                ui.rect(base + ".in." + std::to_string(i))
                    .position(local.x * s - r, local.y * s - r)
                    .size(r * 2.0f, r * 2.0f)
                    .radius(r)
                    .color(portColor(descriptor->inputs[i], rin::PortDirection::Input))
                    .build();
            }
            if (!monitor) {
                for (std::size_t i = 0; i < descriptor->outputs.size(); ++i) {
                    const rin::PortRef port{node.id, rin::PortDirection::Output,
                                            static_cast<std::uint32_t>(i)};
                    const CanvasPoint local =
                        portPosition(node.position, *descriptor, node.params, port) -
                        node.position;
                    ui.rect(base + ".out." + std::to_string(i))
                        .position(local.x * s - r, local.y * s - r)
                        .size(r * 2.0f, r * 2.0f)
                        .radius(r)
                        .color(portColor(descriptor->outputs[i], rin::PortDirection::Output))
                        .build();
                }
            }
            // 每节点耗时徽标（§5.7；M11 起 last · avg 双值）：底部页脚带呈现，
            // 统一小数位达成等宽对齐（DEC-005 mono 替代纪律）；无统计（未运行/
            // 不在当前生效图）不绘制。
            if (const rin::NodeStats* stats = state.perf.nodeStats(node.id);
                stats != nullptr) {
                ui.text(base + ".cost")
                    .position(kSpace2 * s, h - kNodeFooterHeight * s)
                    .size(w - kSpace2 * 2.0f * s, kNodeFooterHeight * s)
                    .text(std::string("last ") + formatCostMs(stats->lastCostMs) + " \u00B7 avg " +
                          formatCostMs(stats->avgCostMs))
                    .fontSize(scaledFont(kFontXs, s, 6.0f))
                    .color(tokens.fgSubtlest)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .maxWidth(w - kSpace2 * 2.0f * s)
                    .build();
            }
        })
        .build();
}

// --- 节点画布（§5.1 视图变换 / §5.3 连线 / §5.4 选择） ---
//
// 合成次序（M11/DEC-021 决策 4）：area 背景 → 全视口 mouseArea → 连线 → 节点
// （含内嵌交互控件）→ 框选/空态。mouseArea 先合成 = 视觉层之下：内嵌控件
// （interactive）优先命中；节点标题/卡片等非交互视觉与空白穿透至 mouseArea
// （拖动/框选/连线经纯逻辑命中分发，语义不变）。
inline void composeWorkflowCanvas(eui::Ui& ui, WorkflowCanvasState& state,
                                  rin::IWorkflowEngine* engine,
                                  const CameraResolutionBinding* cameraResolution, const float x,
                                  const float y, const float width, const float height) {
    const theme::ThemeTokens& tokens = theme::dark();
    state.viewport = {width, height};
    state.areaRect = {x, y, width, height};
    const std::string dirty = std::to_string(state.revision);

    // 拖线中的即时类型过滤基准（§5.3）：发起输出端口的类型。
    std::optional<rin::PortType> draftType;
    if (state.interaction.connecting()) {
        const rin::PortRef from = state.interaction.connectFromPort();
        if (const CanvasNode* fromNode = state.model.findNode(from.node);
            fromNode != nullptr) {
            if (const rin::NodeDescriptor* fd = state.model.descriptorFor(*fromNode);
                fd != nullptr && from.index < fd->outputs.size()) {
                draftType = fd->outputs[from.index];
            }
        }
    }

    ui.stack("workflow.canvas")
        .position(x, y)
        .size(width, height)
        .clip()
        .content([&] {
            ui.rect("workflow.canvas.area")
                .size(width, height)
                .radius(kRadiusLg)
                .color(tokens.surface)
                .border(kBorderHairline, tokens.border)
                .build();

            // 指针热区（M11 起先合成 = 视觉层之下；节点/连线为非交互视觉，
            // 内嵌控件经组件自身 interactive 元素优先命中，其余指针语义经纯
            // 逻辑命中分发，DEC-015/DEC-021）。
            components::mouseArea(ui, "workflow.canvas.input")
                .size(width, height)
                .acceptedButtons(eui::PointerButton::Left | eui::PointerButton::Middle |
                                 eui::PointerButton::Right)
                .cursor(eui::CursorShape::Arrow)
                .dragThreshold(3.0f)
                .onPress([&state, engine](const components::MouseEvent& event) {
                    state.lastMouse = {event.x, event.y};
                    if (state.menuOpen) {  // 任意画布按下先收起创建菜单。
                        state.menuOpen = false;
                        state.menuFilter.set("");
                        return;
                    }
                    if (state.resolutionMenuOpen) {  // 分辨率浮层同理（scrim 已
                        state.resolutionMenuOpen = false;  // 拦截画布外点击）。
                        return;
                    }
                    const int button =
                        event.button == eui::PointerButton::Middle ? 1
                        : event.button == eui::PointerButton::Right ? 2
                                                                    : 0;
                    const CanvasPoint canvas = state.toCanvas({event.x, event.y});
                    const PressResult result =
                        state.interaction.onPress(state.model, canvas, button,
                                                  event.modifiers.shift,
                                                  event.modifiers.alt);
                    if (!result.message.empty()) {
                        state.feedback = result.message;
                    }
                    if (result.kind == PressResult::Kind::ContextMenu) {
                        // 右键空白处打开创建菜单（§5.2.2；menuPos 为窗口坐标，
                        // composeWorkflowCreateMenu 全窗浮层）。接线修复：自
                        // M5-03 起 menuOpen 仅被消费从未被置位，真实右键从未
                        // 打开过（脚本化验证只直改状态），随 M11 验收排查一并
                        // 修复；与分辨率浮层互斥（双 scrim 不可叠加）。
                        state.menuOpen = true;
                        state.menuPos = {event.globalX, event.globalY};
                        state.resolutionMenuOpen = false;
                    }
                    if (result.graphChanged) {
                        // 右键删除节点等即时变更：与松开路径同一引擎同步出口。
                        state.afterGraphChange(engine);
                    }
                    ++state.revision;
                })
                .onDrag([&state](const components::MouseDragEvent& event) {
                    state.lastMouse = {event.x, event.y};
                    state.interaction.onDrag(state.model, state.view, {event.x, event.y},
                                             state.toCanvas({event.x, event.y}));
                    ++state.revision;
                })
                .onRelease([&state, engine](const components::MouseEvent& event) {
                    const CanvasPoint canvas = state.toCanvas({event.x, event.y});
                    const ReleaseResult result =
                        state.interaction.onRelease(state.model, canvas,
                                                    event.modifiers.shift);
                    if (!result.message.empty()) {
                        state.feedback = result.message;
                    }
                    if (result.graphChanged) {
                        state.afterGraphChange(engine);  // §5.3 画布图与引擎同步。
                    }
                    ++state.revision;
                })
                .onMove([&state](const components::MouseEvent& event) {
                    state.lastMouse = {event.x, event.y};
                    return true;
                })
                .onScroll([&state](const components::MouseScrollEvent& event) {
                    // 滚轮缩放（§5.1：以指针为锚点；上下限在 CanvasView 夹取）。
                    const float factor = std::pow(1.2f, event.stepY);
                    state.view.zoomAt(state.lastMouse, factor);
                    ++state.revision;
                })
                .build();

            // 连线（非交互不挡命中；修订号作脏键逐帧直绘）。
            for (std::size_t i = 0; i < state.model.connections.size(); ++i) {
                const rin::Connection& connection = state.model.connections[i];
                const CanvasNode* fromNode = state.model.findNode(connection.from.node);
                const CanvasNode* toNode = state.model.findNode(connection.to.node);
                if (fromNode == nullptr || toNode == nullptr) {
                    continue;
                }
                const rin::NodeDescriptor* fd = state.model.descriptorFor(*fromNode);
                const rin::NodeDescriptor* td = state.model.descriptorFor(*toNode);
                if (fd == nullptr || td == nullptr ||
                    connection.from.index >= fd->outputs.size() ||
                    connection.to.index >= td->inputs.size()) {
                    continue;
                }
                const CanvasPoint a =
                    portPosition(fromNode->position, *fd, fromNode->params, connection.from);
                const CanvasPoint b =
                    portPosition(toNode->position, *td, toNode->params, connection.to);
                // 贝塞尔带状轮廓（粗细一致曲线；polygon 为填充语义，开放点集
                // 会渲染成弦线与曲线围成的封闭区域，见 canvas_model.hpp wireRibbon）。
                std::vector<eui::Vec2> points;
                points.reserve(50);
                for (const CanvasPoint& p : wireRibbon(a, b, 24, kWireWidth)) {
                    const CanvasPoint screen = state.view.toScreen(p);
                    points.push_back({screen.x, screen.y});
                }
                const bool selected = state.model.selectedConnection == connection;
                ui.polygon("workflow.canvas.wire." + std::to_string(i))
                    .position(0.0f, 0.0f)
                    .size(width, height)
                    .points(std::move(points))
                    .color(selected ? tokens.brand
                                    : theme::portTypeColor(
                                          fd->outputs[connection.from.index]))
                    .dirtyKey(dirty)
                    .build();
            }

            // 拖线预览（§5.3）：发起端口 → 当前指针，类型色。
            if (state.interaction.connecting()) {
                const rin::PortRef from = state.interaction.connectFromPort();
                if (const CanvasNode* fromNode = state.model.findNode(from.node);
                    fromNode != nullptr && draftType) {
                    const rin::NodeDescriptor* fd = state.model.descriptorFor(*fromNode);
                    if (fd != nullptr && from.index < fd->outputs.size()) {
                        const CanvasPoint a =
                            portPosition(fromNode->position, *fd, fromNode->params, from);
                        const CanvasPoint b = state.interaction.connectCurrent;
                        // 零长度守卫：光标停回发起端口时中心线因控制点下限外凸
                        // 成小环（from==to 仅预览可达，已提交连线拒绝自连）。
                        if (canvasDistance(a, b) >= 0.5f) {
                            std::vector<eui::Vec2> points;
                            points.reserve(50);
                            for (const CanvasPoint& p : wireRibbon(a, b, 24, kWireWidth)) {
                                const CanvasPoint screen = state.view.toScreen(p);
                                points.push_back({screen.x, screen.y});
                            }
                            ui.polygon("workflow.canvas.wire.draft")
                                .position(0.0f, 0.0f)
                                .size(width, height)
                                .points(std::move(points))
                                .color(theme::portTypeColor(*draftType))
                                .dirtyKey(dirty)
                                .build();
                        }
                    }
                }
            }

            // 节点（绘制序 = 模型数组序，后到顶；内嵌控件随节点 stack 命中
            // 优先于画布 mouseArea，M11/DEC-021 决策 4）。
            for (const CanvasNode& node : state.model.nodes) {
                composeCanvasNode(ui, state, node, draftType, engine, cameraResolution);
            }

            // 框选矩形（§5.4 拖空白框选）。
            if (state.interaction.marqueeing() && state.interaction.marqueeArmed) {
                const CanvasRect rect = canvasRectBetween(state.interaction.marqueeStart,
                                                          state.interaction.marqueeCurrent);
                const CanvasPoint p0 = state.view.toScreen({rect.x, rect.y});
                ui.rect("workflow.canvas.marquee")
                    .position(p0.x, p0.y)
                    .size(rect.width * state.view.scale, rect.height * state.view.scale)
                    .radius(kRadiusSm)
                    .color(tokens.accentSurface)
                    .border(kBorderHairline, tokens.brand)
                    .dirtyKey(dirty)
                    .build();
            }

            // 空态引导（§4：画布中央引导卡，不弹模态）。
            if (state.model.nodes.empty()) {
                ui.text("workflow.canvas.emptyTitle")
                    .position(0.0f, height * 0.5f - 26.0f)
                    .size(width, theme::kFontBase + kSpace2)
                    .text("No nodes yet")
                    .fontSize(theme::kFontBase)
                    .fontWeight(theme::kWeightMedium)
                    .color(tokens.fgSubtle)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .build();
                ui.text("workflow.canvas.emptyHint")
                    .position(0.0f, height * 0.5f + 2.0f)
                    .size(width, kFontSm + kSpace2)
                    .text("drag nodes from the palette to start")
                    .fontSize(kFontSm)
                    .color(tokens.fgSubtlest)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .build();
            }
        })
        .build();
}

// --- 工作流总览（M5-05 §5.7；M11/DEC-021 自右面板迁至底部区右列）：端到端
// FPS、累计处理/丢弃帧、在飞任务数；丢弃计数非零时 warning 提示（EXEC-07
// 显式丢弃的可观察面，禁止静默）；引擎非 Running 时末次值冻结并标注 stopped
// （§4，stale 数值只在冻结标注下出现）。数值右对齐固定列（DEC-005 mono
// 替代纪律）。 ---
inline void composeWorkflowOverview(eui::Ui& ui, const WorkflowPerfState& perf, const float x,
                                    const float y, const float width, const float height) {
    const theme::ThemeTokens& tokens = theme::dark();
    const float rowHeight = 22.0f;
    const float labelHeight = kFontXs + kSpace1;

    ui.stack("workflow.overview")
        .position(x, y)
        .size(width, height)
        .content([&] {
            ui.text("workflow.overview.title")
                .position(0.0f, 0.0f)
                .size(width, labelHeight)
                .text("Overview")
                .fontSize(kFontXs)
                .color(tokens.fgSubtlest)
                .build();
            const rin::WorkflowStats* stats = perf.stats();
            if (stats == nullptr) {
                ui.text("workflow.overview.empty")
                    .position(0.0f, labelHeight + kSpace1)
                    .size(width, rowHeight)
                    .text("run the workflow to see statistics")
                    .fontSize(kFontXs)
                    .color(tokens.fgSubtlest)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
                return;
            }
            const auto row = [&ui, &tokens, width, rowHeight](
                                 const char* id, const std::string& label,
                                 const std::string& value, const eui::Color valueTint) {
                ui.text(std::string("workflow.overview.") + id + ".label")
                    .position(0.0f, 0.0f)
                    .size(width * 0.5f, rowHeight)
                    .text(label)
                    .fontSize(kFontSm)
                    .color(tokens.fgSubtle)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
                ui.text(std::string("workflow.overview.") + id + ".value")
                    .position(0.0f, 0.0f)
                    .size(width, rowHeight)
                    .text(value)
                    .fontSize(kFontSm)
                    .color(valueTint)
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
            };
            float rowY = labelHeight + kSpace1;
            const auto labeledRow = [&](const char* id, const std::string& label,
                                        const std::string& value,
                                        const eui::Color valueTint) {
                ui.stack(std::string("workflow.overview.") + id)
                    .position(0.0f, rowY)
                    .size(width, rowHeight)
                    .content([&] { row(id, label, value, valueTint); })
                    .build();
                rowY += rowHeight;
            };
            labeledRow("fps", "end-to-end", formatFps(stats->endToEndFps), tokens.fg);
            labeledRow("processed", "processed", std::to_string(stats->processedFrames),
                       tokens.fg);
            const bool dropping = stats->droppedFrames > 0;
            labeledRow("dropped", "dropped", std::to_string(stats->droppedFrames),
                       dropping ? tokens.warning : tokens.fg);
            labeledRow("inFlight", "in flight", std::to_string(stats->inFlight), tokens.fg);
            // 过载丢弃警示（EXEC-07：显式丢弃必须可观察，禁止静默排队）。
            if (dropping) {
                ui.text("workflow.overview.overload")
                    .position(0.0f, rowY)
                    .size(width, labelHeight)
                    .text("overload - frames are being dropped")
                    .fontSize(kFontXs)
                    .color(tokens.warning)
                    .build();
                rowY += labelHeight;
            }
            // 冻结标注（§4 停止/关闭排空）：末次值仅在非活动标注下呈现。
            if (!perf.live()) {
                ui.text("workflow.overview.frozen")
                    .position(0.0f, rowY)
                    .size(width, labelHeight)
                    .text("stopped")
                    .fontSize(kFontXs)
                    .color(tokens.fgSubtlest)
                    .build();
            }
        })
        .build();
}

// --- 底部：校验问题与事件列表（左列）+ 工作流总览（右列，M11 迁入） ---
inline void composeWorkflowIssues(eui::Ui& ui, WorkflowCanvasState& state, const float x,
                                  const float y, const float width, const float height) {
    const theme::ThemeTokens& tokens = theme::dark();
    const float pad = kSpace3;
    const float titleHeight = kFontSm + kSpace1;
    const float rowHeight = 22.0f;
    constexpr std::size_t kMaxRows = 3;
    // 右列总览占约 38% 宽（四行数值列的最小可读宽度）。
    const float overviewWidth = std::max(180.0f, width * 0.38f);
    const float issuesWidth = width - pad * 3.0f - overviewWidth;

    ui.stack("workflow.issues")
        .position(x, y)
        .size(width, height)
        .clip()
        .content([&] {
            ui.rect("workflow.issues.card")
                .size(width, height)
                .radius(kRadiusLg)
                .color(tokens.card)
                .border(kBorderHairline, tokens.cardBorder)
                .build();

            // 右列：工作流总览（M11/DEC-021 自右面板迁入，§5.7 语义不变）。
            // stack 子元素为局部坐标。
            composeWorkflowOverview(ui, state.perf, width - pad - overviewWidth, pad,
                                    overviewWidth, height - pad * 2.0f);

            // 左列：校验与事件（§4：逐条 kind + node + message，点击定位）。
            ui.text("workflow.issues.title")
                .position(pad, pad)
                .size(issuesWidth, titleHeight)
                .text("Validation & Events")
                .fontSize(kFontSm)
                .fontWeight(theme::kWeightSemibold)
                .color(tokens.fgSubtle)
                .build();
            ui.text("workflow.issues.event")
                .position(pad, pad + titleHeight)
                .size(issuesWidth, rowHeight)
                .text(state.lastEvent.empty() ? "engine events appear here" : state.lastEvent)
                .fontSize(kFontXs)
                .color(state.lastEvent.empty() ? tokens.fgSubtlest : tokens.fgSubtle)
                .verticalAlign(eui::VerticalAlign::Center)
                .maxWidth(issuesWidth)
                .build();

            const std::size_t rows =
                std::min(state.model.validation.issues.size(), kMaxRows);
            float rowY = pad + titleHeight + rowHeight;
            for (std::size_t i = 0; i < rows; ++i) {
                const rin::ValidationIssue& issue = state.model.validation.issues[i];
                const std::string base = "workflow.issues.row" + std::to_string(i);
                const std::string label = std::string("[") +
                                          validationKindLabel(issue.kind) + "] " +
                                          issue.message;
                const bool locatable = issue.node != rin::kInvalidNode &&
                                       state.model.findNode(issue.node) != nullptr;
                const eui::Color transparent{0.0f, 0.0f, 0.0f, 0.0f};
                ui.rect(base)
                    .position(pad, rowY)
                    .size(issuesWidth, rowHeight)
                    .radius(kRadiusSm)
                    .color(transparent)
                    .states(transparent, locatable ? tokens.menuHover : transparent,
                            locatable ? tokens.menuHover : transparent)
                    .onClick([&state, issue] {
                        if (issue.node == rin::kInvalidNode) {
                            return;
                        }
                        if (const CanvasNode* node = state.model.findNode(issue.node);
                            node != nullptr) {
                            // 点击定位（§4）：视图定心到问题节点。
                            const rin::NodeDescriptor* descriptor =
                                state.model.descriptorFor(*node);
                            const float h = descriptor != nullptr
                                                ? nodeHeight(*descriptor, node->params)
                                                : 40.0f;
                            centerViewOn(state.view,
                                         {node->position.x + kNodeWidth * 0.5f,
                                          node->position.y + h * 0.5f},
                                         state.viewport.x, state.viewport.y);
                            state.feedback = "located node " + std::to_string(issue.node);
                            ++state.revision;
                        }
                    })
                    .build();
                ui.text(base + ".label")
                    .position(pad + kSpace2, rowY)
                    .size(issuesWidth - kSpace2, rowHeight)
                    .text(label)
                    .fontSize(kFontXs)
                    .color(tokens.warning)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .maxWidth(issuesWidth - kSpace2)
                    .build();
                rowY += rowHeight;
            }
            if (state.model.validation.issues.size() > kMaxRows) {
                ui.text("workflow.issues.more")
                    .position(pad + kSpace2, rowY)
                    .size(issuesWidth - kSpace2, rowHeight)
                    .text("+" +
                          std::to_string(state.model.validation.issues.size() - kMaxRows) +
                          " more")
                    .fontSize(kFontXs)
                    .color(tokens.fgSubtlest)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
            }
        })
        .build();
}

}  // namespace

// --- 右键创建菜单（§5.2.2：浮层 + 即输即筛；自绘，EUI-20260928-001 先例） ---
inline void composeWorkflowCreateMenu(eui::Ui& ui, WorkflowCanvasState& state,
                                      rin::IWorkflowEngine* engine, const float windowWidth,
                                      const float windowHeight) {
    if (!state.menuOpen || state.model.catalog == nullptr) {
        return;
    }
    const theme::ThemeTokens& tokens = theme::dark();
    const float pad = kSpace2;
    const float width = 190.0f;
    const float inputHeight = 28.0f;
    const float groupTitleHeight = kFontXs + kSpace1;
    const float itemHeight = 24.0f;
    const std::vector<PaletteGroup> groups =
        paletteGroups(*state.model.catalog, state.menuFilter.get());
    std::size_t itemCount = 0;
    for (const PaletteGroup& group : groups) {
        itemCount += group.items.size();
    }
    const float height = pad * 2.0f + inputHeight + kSpace2 +
                         static_cast<float>(groups.size()) * groupTitleHeight +
                         static_cast<float>(itemCount) * itemHeight;
    const float x = std::clamp(state.menuPos.x, 8.0f, std::max(8.0f, windowWidth - width - 8.0f));
    const float y =
        std::clamp(state.menuPos.y, 8.0f, std::max(8.0f, windowHeight - height - 8.0f));

    // 全窗口透明阻挡层：菜单外任意点击收起（§5.2.2 菜单是模态浮层）。
    ui.rect("workflow.menu.scrim")
        .position(0.0f, 0.0f)
        .size(windowWidth, windowHeight)
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .onClick([&state] {
            state.menuOpen = false;
            state.menuFilter.set("");
        })
        .build();

    ui.stack("workflow.menu")
        .position(x, y)
        .size(width, height)
        .content([&] {
            ui.rect("workflow.menu.panel")
                .size(width, height)
                .radius(kRadiusLg)
                .color(tokens.menu)
                .border(kBorderHairline, tokens.border)
                .shadow(18.0f, 10.0f, 8.0f, {0.0f, 0.0f, 0.0f, 0.35f})
                .build();
            components::InputStyle inputStyle;
            inputStyle.background = tokens.input;
            inputStyle.focused = tokens.input;
            inputStyle.border = tokens.inputBorder;
            inputStyle.focusBorder = tokens.brand;
            inputStyle.text = tokens.fg;
            inputStyle.placeholder = tokens.fgSubtlest;
            inputStyle.cursor = tokens.brand;
            inputStyle.shadow = core::Shadow{};
            inputStyle.radius = kRadiusMd;
            components::input(ui, "workflow.menu.filter")
                .position(pad, pad)
                .size(width - pad * 2.0f, inputHeight)
                .bind(state.menuFilter)
                .placeholder("filter")
                .fontSize(kFontSm)
                .inset(kSpace2)
                .style(inputStyle)
                .build();

            float rowY = pad + inputHeight + kSpace2;
            for (const PaletteGroup& group : groups) {
                ui.text("workflow.menu.group." + group.title)
                    .position(pad, rowY)
                    .size(width - pad * 2.0f, groupTitleHeight)
                    .text(group.title)
                    .fontSize(kFontXs)
                    .color(tokens.fgSubtlest)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
                rowY += groupTitleHeight;
                for (const rin::NodeDescriptor* descriptor : group.items) {
                    const std::string base =
                        std::string("workflow.menu.item.") + descriptor->typeId;
                    const eui::Color transparent{0.0f, 0.0f, 0.0f, 0.0f};
                    ui.rect(base)
                        .position(pad, rowY)
                        .size(width - pad * 2.0f, itemHeight)
                        .radius(kRadiusSm)
                        .color(transparent)
                        .states(transparent, tokens.menuHover, tokens.menuHover)
                        .onClick([&state, engine, descriptor] {
                            // §5.2.2：选中后在该点创建；创建即选中（模型内）。
                            const CanvasPoint canvas = state.globalToCanvas(state.menuPos);
                            const CanvasOpResult result = state.model.createNode(
                                descriptor->typeId,
                                {canvas.x - kNodeWidth * 0.5f,
                                 canvas.y - kNodeHeaderHeight * 0.5f});
                            state.feedback = result.ok ? "" : result.error;
                            state.menuOpen = false;
                            state.menuFilter.set("");
                            state.afterGraphChange(engine);
                        })
                        .build();
                    ui.text(base + ".label")
                        .position(pad + kSpace2, rowY)
                        .size(width - pad * 2.0f - kSpace2, itemHeight)
                        .text(descriptor->displayName)
                        .fontSize(kFontCaption)
                        .color(tokens.fg)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .build();
                    rowY += itemHeight;
                }
            }
        })
        .build();
}

// --- 调色板拖拽浮层（§5.2.1：类型名跟随光标） ---
inline void composeWorkflowDragGhost(eui::Ui& ui, WorkflowCanvasState& state) {
    if (!state.paletteDragging || state.model.catalog == nullptr) {
        return;
    }
    const theme::ThemeTokens& tokens = theme::dark();
    const rin::NodeDescriptor* descriptor =
        rin::findNodeDescriptor(*state.model.catalog, state.paletteDragType);
    if (descriptor == nullptr) {
        return;
    }
    const float width = 120.0f;
    const float height = 24.0f;
    ui.stack("workflow.ghost")
        .position(state.paletteDragPos.x + 10.0f, state.paletteDragPos.y + 10.0f)
        .size(width, height)
        .content([&] {
            ui.rect("workflow.ghost.card")
                .size(width, height)
                .radius(kRadiusSm)
                .color(tokens.menu)
                .border(kBorderHairline, tokens.brand)
                .build();
            ui.text("workflow.ghost.label")
                .position(theme::kSpace2, 0.0f)
                .size(width - theme::kSpace2 * 2.0f, height)
                .text(descriptor->displayName)
                .fontSize(kFontXs)
                .color(tokens.fg)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
        })
        .build();
}

// --- 相机分辨率下拉浮层（M11 验收反馈：窗口级 overlay，创建菜单同款模式；
// 画布 clip 视口内的组件弹层会被裁剪，浮层由 app.cpp overlay 层最后合成） ---
inline void composeWorkflowResolutionMenu(eui::Ui& ui, WorkflowCanvasState& state,
                                          const CameraResolutionBinding* cameraResolution,
                                          const float windowWidth, const float windowHeight) {
    if (!state.resolutionMenuOpen || cameraResolution == nullptr ||
        cameraResolution->labels == nullptr || cameraResolution->labels->empty()) {
        return;
    }
    const theme::ThemeTokens& tokens = theme::dark();
    const float pad = kSpace2;
    const float width = 176.0f;
    const float titleHeight = kFontXs + kSpace2;
    const float itemHeight = 26.0f;
    const int count = static_cast<int>(cameraResolution->labels->size());
    const float height = pad * 2.0f + titleHeight + static_cast<float>(count) * itemHeight;
    const float x = std::clamp(state.resolutionMenuPos.x, 8.0f,
                               std::max(8.0f, windowWidth - width - 8.0f));
    const float y = std::clamp(state.resolutionMenuPos.y, 8.0f,
                               std::max(8.0f, windowHeight - height - 8.0f));

    // 全窗口透明阻挡层：菜单外任意点击收起（模态浮层）。
    ui.rect("workflow.resMenu.scrim")
        .position(0.0f, 0.0f)
        .size(windowWidth, windowHeight)
        .color({0.0f, 0.0f, 0.0f, 0.0f})
        .onClick([&state] { state.resolutionMenuOpen = false; })
        .build();

    ui.stack("workflow.resMenu")
        .position(x, y)
        .size(width, height)
        .content([&] {
            ui.rect("workflow.resMenu.panel")
                .size(width, height)
                .radius(kRadiusLg)
                .color(tokens.menu)
                .border(kBorderHairline, tokens.border)
                .shadow(18.0f, 10.0f, 8.0f, {0.0f, 0.0f, 0.0f, 0.35f})
                .build();
            ui.text("workflow.resMenu.title")
                .position(pad, pad)
                .size(width - pad * 2.0f, titleHeight)
                .text("Camera resolution")
                .fontSize(kFontXs)
                .color(tokens.fgSubtlest)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            const int selected = std::clamp(cameraResolution->selectedIndex, 0, count - 1);
            float rowY = pad + titleHeight;
            for (int index = 0; index < count; ++index) {
                const std::string base =
                    "workflow.resMenu.item." + std::to_string(index);
                const bool active = index == selected;
                const eui::Color transparent{0.0f, 0.0f, 0.0f, 0.0f};
                ui.rect(base)
                    .position(pad, rowY)
                    .size(width - pad * 2.0f, itemHeight)
                    .radius(kRadiusSm)
                    .color(transparent)
                    .states(transparent, tokens.menuHover, tokens.menuHover)
                    .onClick([&state, cameraResolution, index] {
                        state.resolutionMenuOpen = false;
                        cameraResolution->onPick(index);
                    })
                    .build();
                ui.text(base + ".label")
                    .position(pad + kSpace2, rowY)
                    .size(width - pad * 2.0f - kSpace2 - 14.0f, itemHeight)
                    .text((*cameraResolution->labels)[static_cast<std::size_t>(index)])
                    .fontSize(kFontCaption)
                    .color(active ? tokens.brand : tokens.fg)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
                if (active) {
                    ui.text(base + ".check")
                        .position(width - pad - 14.0f, rowY)
                        .size(14.0f, itemHeight)
                        .text("\u2713")
                        .fontSize(kFontXs)
                        .color(tokens.brand)
                        .horizontalAlign(eui::HorizontalAlign::Center)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .build();
                }
                rowY += itemHeight;
            }
        })
        .build();
}

// --- 工作流页四区组装（M11/DEC-021：右栏移除，五区改四区；几何由
// WorkflowCanvasState 的 docking 布局状态驱动，palette 宽、底部高可经分隔条
// 拖拽调节；底部区最后合成，总览数值列高于画布） ---
inline void composeWorkflowPage(eui::Ui& ui, WorkflowCanvasState& state,
                                rin::IWorkflowEngine* engine,
                                const CameraResolutionBinding* cameraResolution, const float x,
                                const float y, const float width, const float height) {
    const float gap = theme::kSpace3;
    const float toolbarHeight = 44.0f;
    const float paletteWidth = std::clamp(state.paletteWidth, 150.0f, 400.0f);
    const float bottomHeight = std::clamp(state.bottomHeight, 72.0f, 300.0f);
    const float bodyTop = y + toolbarHeight + gap;
    const float bodyHeight = std::max(
        120.0f, height - toolbarHeight - bottomHeight - gap * 2.0f);
    const float canvasWidth = std::max(240.0f, width - paletteWidth - gap * 2.0f);

    composeWorkflowToolbar(ui, state, engine, x, y, width, toolbarHeight);
    composeWorkflowPalette(ui, state, engine, x, bodyTop, paletteWidth, bodyHeight);
    composeWorkflowCanvas(ui, state, engine, cameraResolution, x + paletteWidth + gap, bodyTop,
                          canvasWidth, bodyHeight);
    composeWorkflowIssues(ui, state, x, y + height - bottomHeight, width, bottomHeight);

    // 分隔条（M7-04 引入，M11 余两处；最后合成 = 浮层；捕获 &state 稳定地址
    // ——与画布交互同纪律）。命中区略宽于视觉条；拖拽经 onDragStart 记录
    // 起始、onDrag 按指针增量更新布局值（compose 期夹取生效）。
    // growWithNegative：底部上缘为反向缘（指针负向增量使布局值增大）。
    const float handleHit = 10.0f;
    const eui::Color transparent{0.0f, 0.0f, 0.0f, 0.0f};
    const auto dividerTint = [&state, &transparent](int index) {
        return state.dockDrag == index ? theme::dark().brand : transparent;
    };
    const auto divider = [&](const char* id, int index, float dx, float dy, float dw,
                             float dh, bool vertical, bool growWithNegative,
                             float* dockValue) {
        ui.rect(std::string(id) + ".bar")
            .position(dx, dy)
            .size(dw, dh)
            .radius(2.0f)
            .color(dividerTint(index))
            .build();
        components::mouseArea(ui, std::string(id) + ".drag")
            .position(dx - (vertical ? handleHit * 0.5f : 0.0f),
                      dy - (vertical ? 0.0f : handleHit * 0.5f))
            .size(vertical ? handleHit : dw, vertical ? dh : handleHit)
            .cursor(eui::CursorShape::Hand)
            .onDragStart([&state, index, dockValue, vertical](
                             const components::MouseEvent& event) {
                state.dockDrag = index;
                state.dockDragStartPointer = vertical ? event.x : event.y;
                state.dockDragStartValue = *dockValue;
            })
            .onDrag([&state, index, dockValue, vertical, growWithNegative](
                        const components::MouseDragEvent& event) {
                if (state.dockDrag != index) {
                    return;
                }
                const float pointer = vertical ? event.x : event.y;
                const float delta = pointer - state.dockDragStartPointer;
                *dockValue = growWithNegative ? state.dockDragStartValue - delta
                                              : state.dockDragStartValue + delta;
            })
            .onDragEnd([&state, index](const components::MouseEvent&) {
                if (state.dockDrag == index) {
                    state.dockDrag = -1;
                }
            })
            .build();
    };
    divider("workflow.divider.palette", 0, x + paletteWidth + gap * 0.5f - 2.0f, bodyTop,
            4.0f, bodyHeight, true, false, &state.paletteWidth);
    divider("workflow.divider.bottom", 1, x, y + height - bottomHeight - gap * 0.5f - 2.0f,
            width, 4.0f, false, true, &state.bottomHeight);
}

// --- pump 边界消费（RULE-05：渲染线程只做有界取快照与提交） ---

/// pump 边界的监看器预览消费（M11/DEC-021）：引擎 Running 时逐监看器节点拉取
/// 最新产物并上传预览纹理（GL 当前线程）；非 Running 整体排空释放（§4 停止/
/// 关闭排空，stale 快照不得显示活动状态）。已删除/不再是监看器的节点条目
/// 同步清理（afterGraphChange 只移除条目、GL 释放在此完成——渲染线程纪律）。
/// 返回 true 表示有可见变更（调用方需 requestUpdate）。
inline bool pumpMonitorViews(WorkflowCanvasState& canvas, rin::IWorkflowEngine& engine) {
    const bool running = engine.state() == rin::WorkflowEngineState::Running;

    // 清理不再有效的条目（节点删除/类型不再是监看器）：GL 释放仅渲染线程。
    bool changed = false;
    for (auto it = canvas.monitors.begin(); it != canvas.monitors.end();) {
        const CanvasNode* node = canvas.model.findNode(it->first);
        const bool stale = node == nullptr || !isMonitorNode(node->typeId);
        if (stale) {
            it->second.view.release();
            it = canvas.monitors.erase(it);
            changed = true;
            continue;
        }
        ++it;
    }

    if (!running) {
        for (auto& [id, monitor] : canvas.monitors) {
            if (monitor.view.valid() || !monitor.meta.empty()) {
                monitor.view.release();
                monitor.meta.clear();
                monitor.lastSeen = 0;
                changed = true;
            }
        }
        return changed;
    }

    for (const CanvasNode& node : canvas.model.nodes) {
        if (!isMonitorNode(node.typeId)) {
            continue;
        }
        MonitorView& monitor = canvas.monitors[node.id];
        rin::NodeOutputSnapshot snapshot;
        if (!engine.tryLoadNodeOutput(node.id, monitor.lastSeen, snapshot)) {
            continue;
        }
        rin::Frame frame;
        if (!thumbnailRgbaFromSnapshot(snapshot, kMonitorPreviewMaxDim, frame)) {
            continue;
        }
        monitor.view.update(frame);
        monitor.meta = std::to_string(snapshot.width) + " x " +
                       std::to_string(snapshot.height) + " \u00B7 seq " +
                       std::to_string(snapshot.sourceSequence);
        changed = true;
    }
    return changed;
}

/// pump 边界的驱动尺寸刷新（M11：ROI 联动约束的输入尺寸源）：Running 时为
/// 画布图内每个被消费的驱动节点拉取最新产物尺寸（每驱动一条目，水位推进，
/// 只取元数据不舍弃像素共享）；非 Running 清空（约束回退声明范围）。
inline bool pumpDriverSizes(WorkflowCanvasState& canvas, rin::IWorkflowEngine& engine) {
    if (engine.state() != rin::WorkflowEngineState::Running) {
        const bool changed = !canvas.driverSizes.empty();
        canvas.driverSizes.clear();
        return changed;
    }
    // 当前驱动集合（输入端口 0 的上游；ROI 参数只读首输入）。
    std::vector<rin::NodeId> drivers;
    for (const rin::Connection& connection : canvas.model.connections) {
        if (connection.to.direction != rin::PortDirection::Input || connection.to.index != 0) {
            continue;
        }
        if (std::find(drivers.begin(), drivers.end(), connection.from.node) == drivers.end()) {
            drivers.push_back(connection.from.node);
        }
    }
    bool changed = false;
    for (auto it = canvas.driverSizes.begin(); it != canvas.driverSizes.end();) {
        if (std::find(drivers.begin(), drivers.end(), it->first) == drivers.end()) {
            it = canvas.driverSizes.erase(it);
            changed = true;
            continue;
        }
        ++it;
    }
    for (const rin::NodeId driver : drivers) {
        DriverOutputSize& entry = canvas.driverSizes[driver];
        rin::NodeOutputSnapshot snapshot;
        if (!engine.tryLoadNodeOutput(driver, entry.lastSeen, snapshot)) {
            continue;
        }
        entry.width = snapshot.width;
        entry.height = snapshot.height;
        changed = true;
    }
    return changed;
}

}  // namespace viewer
