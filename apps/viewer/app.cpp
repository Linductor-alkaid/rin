// rin：EUI-NEO 前端（工作台壳，M5-02）。Executor 生命周期 owner 为 viewer::Context
// （DEC-002）：dslAppConfig() 首调点惰性启动，DslAppConfig::onShutdown（主线程、
// GPU 销毁前）关闭。信息架构按 DEC-014：单窗口四页导航（预览 / 位姿 / 图像工作
// 流 / 设置），左窄边导航栏见 navigation.hpp（控件选型结论见台账 EUI-20260928-001）；
// 页面切换只推进 NavigationState，各页 UI 状态由 ViewerContext 持有（切页保持），
// 相机服务运行态全局共享（pump 不分页）。工作流页节点编辑器见 node_canvas.hpp、
// 参数面板与中间结果缩略图见 param_panel.hpp、性能面板统计管道见 perf_model.hpp、
// 运行控制（§5.8 启动/停止 + 待生效标注）随工具栏落地（M5-06 假换真集成：
// M4-07 真引擎 createWorkflowEngine + 相机帧源接缝 workflow_frame_source.hpp，
// DEC-013/DEC-016）。视觉层遵循 viewer_theme.hpp 的语义令牌翻译（DEC-005）：布局
// 代码不出现一次性颜色/字号/圆角。热插拔与设备选择见 DEC-006：启动不依赖相机
// 连接，运行中经设备目录自动识别，多设备时用户选择、唯一设备自动选择。3D 位姿
// 视图（M3-06，DEC-011）由 pose_view.hpp 承载：Core 投影纯逻辑 + polygon 有界
// 组装，姿态经 tryLoadPose() 最新态消费；IMU 状态面板（M3-07，imu_panel.hpp）
// 消费同一份快照呈现源频率与姿态数值；onShutdown 在服务停止后排空 UI 侧姿态
// 状态（关闭顺序回归 M3-07）。

#include "gpu_frame_view.hpp"
#include "imu_panel.hpp"
#include "navigation.hpp"
#include "node_canvas.hpp"
#include "policy_depth_output.hpp"
#include "pose_view.hpp"
#include "resolution_model.hpp"
#include "viewer_components.hpp"
#include "viewer_theme.hpp"

#include <eui_neo.h>

#include <kairo/executor.hpp>

#include <realsense_camera_service.hpp>

#include <rin/camera_service.hpp>

#include "workflow_frame_source.hpp"

#include "engine.hpp"

#include <cstdio>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

/// 字号档位写入（定义见文件尾 app 命名空间；设置页 overlay 回调消费）。
namespace app {
void setUiScale(float value);
}

namespace viewer {
namespace {

using namespace viewer::theme;
using rin::FrameKind;
using rin::StreamRequest;

/// 默认流请求（DEC-004 暂定默认值）；设备选择独立于流请求（DEC-006）。M3-06 起
/// 默认使能运动流（SCOPE-07）：IMU 设备上附加 ACCEL/GYRO 供 3D 位姿视图与 IMU
/// 面板消费；无 IMU 设备按契约退化为纯视频流（运动通道保持空，不视为错误）。
/// 视频字段（含 fps）同时是设备变化时档位回选的默认档判据（M13-01）。
constexpr StreamRequest kDefaultRequest{.enableMotion = true};

/// 字号档位（DEC-022）：设置页四档界面缩放，运行期写入应用配置 uiScale
///（EUI 每帧读取 effectiveScale = dpiScale × uiScale，变更即整页重排重绘）。
constexpr float kFontScales[] = {0.85f, 1.0f, 1.15f, 1.3f};
constexpr const char* kFontScaleLabels[] = {"小", "标准", "大", "特大"};

struct DeviceUiOption {
    std::string serial;
    std::string label;
};

struct ViewerContext {
    kairo::Executor executor;
    std::shared_ptr<rin::ICameraService> service;
    GpuFrameView rgbView;
    GpuFrameView depthView;
    /// 策略深度可选输出（DEC-019 决策 3 的"无历史分片"变体）：米制深度通道 →
    /// 冻结管线 O6 单帧组合 → 灰度显示帧；tick 组件随服务 start 准入后创建。

    std::uint64_t lastRgbSequence = 0;
    std::uint64_t lastDepthSequence = 0;
    std::uint64_t lastPolicySequence = 0;
    std::uint64_t lastIntrinsicsSequence = 0;
    std::uint64_t lastCatalogSequence = 0;
    std::uint64_t lastPoseSequence = 0;

    /// shared_ptr 持有（PolicyDepthOutput tick 闭包经 weak_from_this 捕获，
    /// enable_shared_from_this 契约——unique 持有会被 start() 显式拒绝）。
    std::shared_ptr<viewer::PolicyDepthOutput> policyDepth;
    GpuFrameView policyDepthView;
    std::string policyDepthMeta = "waiting for metric depth";

    rin::DeviceCatalog catalog;
    bool hasCatalog = false;
    rin::IntrinsicsSnapshot intrinsics;
    bool hasIntrinsics = false;
    /// 3D 位姿视图状态（M3-06）：pump() 消费最新姿态快照，Reset 重新锚定显示参考。
    PoseViewState poseView;

    std::vector<DeviceUiOption> deviceOptions;
    eui::Signal<int> deviceIndex{0};
    eui::Signal<bool> deviceOpen{false};
    std::vector<ResolutionUiOption> resolutionOptions;
    /// 档位列表对应的设备序列号；仅设备变化时才重置默认选中（否则会覆盖用户选择）。
    std::string resolutionOptionsForSerial;
    eui::Signal<int> resolutionIndex{0};
    eui::Signal<bool> resolutionOpen{false};
    /// 分辨率档位标签（M7-01：与 resolutionOptions 同步重建的稳定副本——
    /// 工作流源面板的分辨率下拉回调被 retained 节点持有、点击时才执行，
    /// 捕获地址必须稳定，不得指向 compose 栈临时量）。
    std::vector<std::string> resolutionLabels;
    /// 相机分辨率入口绑定（M6-06 引入，M7-01 移入本上下文）：对象与 labels
    /// 均稳定存续，onPick 捕获 &ctx（静态单例）；selectedIndex 每次 compose
    /// 前从 resolutionIndex 刷新（仅 compose 期读取）。
    viewer::CameraResolutionBinding cameraResolutionBinding;
    /// 会话级运动流禁用（RIN_DISABLE_MOTION=1，LRS-20261007-001 宿主绕行）：
    /// 启动请求与分辨率档位的 enableMotion 粘性都以此为准——否则分辨率
    /// restream 命令会把运动流重新带回（坏 IIO 宿主上即合成帧饿死）。
    const bool motionDisabledByEnv = getenv("RIN_DISABLE_MOTION") != nullptr;
    /// 深度配色（DEC-007）：0 = Jet，1 = Grayscale，2 = AdaptiveGrayscale；
    /// 不依赖设备目录，Waiting 可预设。
    eui::Signal<int> paletteIndex{0};
    eui::Signal<bool> paletteOpen{false};
    /// 预览深度可选输出（DEC-022）：0 = 相机深度 rendition，1 = 策略输出。
    eui::Signal<int> depthSourceIndex{0};
    /// 字号档位（DEC-022，界面缩放实现）：0..3 → kFontScales，会话级。
    eui::Signal<int> fontScaleIndex{1};
    eui::Signal<bool> fontScaleOpen{false};

    rin::CameraServiceState statusState = rin::CameraServiceState::Idle;
    std::string statusMessage;
    std::string startError;
    std::string rgbMeta;
    std::string depthMeta;

    /// 工作流引擎（M5-06 假换真集成：M4-07 真引擎 + 相机帧源接缝，DEC-013/
    /// DEC-016）。目录指针随实例存续，构建期确定。
    std::shared_ptr<rin::IWorkflowEngine> workflow;
    /// 帧源路由快照（M6-05，DEC-017）：source 节点 → rendition，画布图变更
    /// 即整体换新（原子共享，帧泵 tick 只读）；pump 以画布修订号为脏标记刷新。
    viewer::WorkflowSourceRouter sourceRouter;
    std::uint64_t sourceRouterRevision = 0;
    /// 工作流页画布会话状态（node_canvas.hpp；DEC-014 决策 4：切页保持；
    /// M11/DEC-021 起含内嵌参数控件、监看器预览与驱动尺寸缓存）。
    WorkflowCanvasState workflowCanvas;

    /// 工作台导航状态（M5-02）：四页模型与当前页；页面 UI 状态由本上下文各字段
    /// 持有，导航不触碰（页面切换状态保持，navigation.hpp）。
    NavigationState nav;

    bool started = false;
    bool shutdownDone = false;

    void applyResolutionChoice(int index);
    void applyDeviceChoice(int index);
    void applyPaletteChoice(int index);
    void rebuildDeviceOptions();
    void rebuildResolutionOptions();
    void pump();
    void shutdown();
};

ViewerContext& context() {
    static ViewerContext instance;
    return instance;
}


/// 重建帧源路由快照（M6-05，DEC-017）：source 节点 typeId → rendition。
/// ensureStarted 与 pump（画布修订号脏标记）调用；整体换新对帧泵 tick 原子
/// 可见，未知节点由路由回退 RGB。
void refreshSourceRouter(ViewerContext& ctx) {
    auto routes = std::make_shared<viewer::WorkflowSourceRouter::RouteMap>();
    routes->reserve(ctx.workflowCanvas.model.nodes.size());
    for (const CanvasNode& node : ctx.workflowCanvas.model.nodes) {
        (*routes)[node.id] = viewer::renditionForSourceType(node.typeId);
    }
    ctx.sourceRouter.replace(std::move(routes));
    ctx.sourceRouterRevision = ctx.workflowCanvas.revision;
}

void ensureStarted() {
    static const bool once = [] {
        ViewerContext& ctx = context();
        if (const auto init = ctx.executor.initialize({}); !init) {
            ctx.startError = init.message;
            ctx.statusMessage = "executor init failed";
            return false;
        }
        ctx.service = rin::createRealSenseCameraService(ctx.executor);
        // 工作流引擎（M5-06 真引擎，DEC-013 执行模型）：目录为构建期数据，画布
        // 模型直接引用（引擎存续期由本上下文持有）；帧源按路由快照分发源节点
        // rendition（M6-05，DEC-017：RGB/深度伪彩/深度灰度/深度自适应），零拷贝
        // 包装（workflow_frame_source.hpp），以 shared_ptr 持有相机服务——关闭
        // 顺序中服务先停、引擎后回收，帧源闭包不悬垂；router 为本上下文成员，
        // 存续覆盖引擎。
        rin::WorkflowEngineConfig workflowConfig;
        workflowConfig.frameSource = viewer::makeCameraFrameSource(ctx.service, ctx.sourceRouter);
        ctx.workflow = rin::createWorkflowEngine(ctx.executor, std::move(workflowConfig));
        ctx.workflowCanvas.model.catalog = &ctx.workflow->catalog();
        ctx.workflowCanvas.afterGraphChange(ctx.workflow.get());
        refreshSourceRouter(ctx);
        // 相机分辨率入口绑定（M7-01）：全部捕获稳定地址（labels 为本上下文
        // 成员、onPick 捕获静态单例 &ctx、open 指向面板成员），一次性接线；
        // selectedIndex 由 compose 每帧从 resolutionIndex 刷新。
        ctx.cameraResolutionBinding.labels = &ctx.resolutionLabels;
        ctx.cameraResolutionBinding.onPick = [&ctx](int index) {
            ctx.resolutionIndex.set(index);
            ctx.applyResolutionChoice(index);
        };
        // 启动不依赖相机连接（DEC-006）：无设备时服务进入 Waiting，接入后自动出流。
        // RIN_DISABLE_MOTION=1：跳过运动流（会话级，含后续分辨率档位粘性；
        // LRS-20261007-001 宿主绕行，见 ViewerContext::motionDisabledByEnv）。
        rin::StreamRequest startRequest = kDefaultRequest;
        startRequest.enableMotion = kDefaultRequest.enableMotion && !ctx.motionDisabledByEnv;
        const rin::StartOutcome outcome = ctx.service->start(startRequest);
        if (!outcome.admitted) {
            ctx.startError = outcome.error;
            ctx.statusMessage = "start failed";
            return false;
        }
        // 策略深度可选输出 tick（DEC-019 决策 3 纪律：服务 start 准入通过后
        // 启动；onShutdown 在服务 stop 之后、executor shutdown 之前析构停止）。
        // 准入失败路径不创建，tick 不会落在未初始化 executor 上。
        ctx.policyDepth = std::make_shared<viewer::PolicyDepthOutput>(ctx.service);
        if (!ctx.policyDepth->start(ctx.executor)) {
            ctx.startError = "policy depth tick rejected";
            ctx.statusMessage = "start failed";
            return false;
        }
        ctx.started = true;
        ctx.statusMessage = "opening device";
        return true;
    }();
    (void)once;
}

const rin::DeviceInfo* activeDevice(const ViewerContext& ctx) {
    if (!ctx.hasCatalog) {
        return nullptr;
    }
    for (const rin::DeviceInfo& device : ctx.catalog.devices) {
        if (device.serial == ctx.catalog.activeSerial) {
            return &device;
        }
    }
    return nullptr;
}

std::string shortSerial(const std::string& serial) {
    return serial.size() <= 5 ? serial : serial.substr(serial.size() - 5);
}

const char* distortionName(rin::DistortionModel model) {
    switch (model) {
        case rin::DistortionModel::None:
            return "None";
        case rin::DistortionModel::ModifiedBrownConrady:
            return "ModBrownConrady";
        case rin::DistortionModel::InverseBrownConrady:
            return "InvBrownConrady";
        case rin::DistortionModel::BrownConrady:
            return "BrownConrady";
        case rin::DistortionModel::FTheta:
            return "FTheta";
        case rin::DistortionModel::KannalaBrandt4:
            return "KannalaBrandt4";
        case rin::DistortionModel::Unknown:
            return "Unknown";
    }
    return "Unknown";
}

/// 单流内参数值（统一小数位对齐；无打包等宽字体，见 DEC-005 限制）。
std::string formatIntrinsicsValues(const rin::StreamIntrinsics& intrinsics) {
    if (!intrinsics.valid()) {
        return "n/a";
    }
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "fx %.3f   fy %.3f   cx %.3f   cy %.3f   %ux%u",
                  static_cast<double>(intrinsics.fx), static_cast<double>(intrinsics.fy),
                  static_cast<double>(intrinsics.cx), static_cast<double>(intrinsics.cy),
                  intrinsics.width, intrinsics.height);
    return buffer;
}

void ViewerContext::rebuildDeviceOptions() {
    std::vector<DeviceUiOption> options;
    options.reserve(catalog.devices.size());
    for (const rin::DeviceInfo& device : catalog.devices) {
        DeviceUiOption option;
        option.serial = device.serial;
        option.label = device.name + " · " + shortSerial(device.serial);
        options.push_back(std::move(option));
    }
    deviceOptions = std::move(options);
}

void ViewerContext::rebuildResolutionOptions() {
    resolutionOptions.clear();
    resolutionLabels.clear();
    const rin::DeviceInfo* device = activeDevice(*this);
    if (device == nullptr) {
        resolutionOptionsForSerial.clear();
        return;  // 未选定设备（等待接入/用户选择）时不提供分辨率档位。
    }
    resolutionOptionsForSerial = device->serial;
    // 档位枚举唯一实现（M13-01，resolution_model.hpp）：彩色∩深度按
    // (宽, 高, fps) 三元组配对，M1"固定 30fps 档位"范围收缩就此放开——
    // 设备支持的全部帧率档位（如 D435IF 848x480@60）均可手动选择。
    // enableMotion 粘性保持（StreamRequest 契约，camera_types.hpp）：分辨率档位
    // 只覆盖视频字段，运动流意图沿用会话默认（RIN_DISABLE_MOTION 会话级
    // 禁用时为 false——否则 restream 命令重新带回运动流，坏 IIO 宿主上
    // 即 LRS-20261007-001 合成帧饿死的第二入口）。
    resolutionOptions = buildResolutionUiOptions(
        *device, kDefaultRequest.enableMotion && !motionDisabledByEnv);
    resolutionLabels.reserve(resolutionOptions.size());
    for (const ResolutionUiOption& option : resolutionOptions) {
        resolutionLabels.push_back(option.label);
    }
}

void ViewerContext::applyResolutionChoice(int index) {
    if (service == nullptr || index < 0 || index >= static_cast<int>(resolutionOptions.size())) {
        return;
    }
    std::string error;
    if (!service->requestResolution(resolutionOptions[static_cast<std::size_t>(index)].request,
                                    &error)) {
        statusMessage = "resolution rejected: " + error;
    }
}

void ViewerContext::applyDeviceChoice(int index) {
    if (service == nullptr || index < 0 || index >= static_cast<int>(deviceOptions.size())) {
        return;
    }
    std::string error;
    if (!service->requestDevice(deviceOptions[static_cast<std::size_t>(index)].serial, &error)) {
        statusMessage = "device select rejected: " + error;
    }
}

void ViewerContext::applyPaletteChoice(int index) {
    // 下拉项与配色一一对应（DEC-007）：0=Jet、1=Grayscale、2=AdaptiveGrayscale。
    static constexpr rin::DepthColorScheme kSchemes[] = {
        rin::DepthColorScheme::Jet,
        rin::DepthColorScheme::Grayscale,
        rin::DepthColorScheme::AdaptiveGrayscale,
    };
    if (service == nullptr || index < 0 || index >= static_cast<int>(std::size(kSchemes))) {
        return;
    }
    std::string error;
    if (!service->requestDepthColorScheme(kSchemes[index], &error)) {
        statusMessage = "palette rejected: " + error;
    }
}

void ViewerContext::pump() {
    if (service == nullptr) {
        return;
    }

    bool frameUpdated = false;
    rin::Frame frame;
    if (service->tryLoadFrame(FrameKind::Rgb, lastRgbSequence, frame)) {
        rgbView.update(frame);  // UI/渲染线程上传（EUI-20260923-003 绕行）
        rgbMeta = std::to_string(frame.width) + " x " + std::to_string(frame.height);
        frameUpdated = true;
    }
    if (service->tryLoadFrame(FrameKind::Depth, lastDepthSequence, frame)) {
        depthView.update(frame);
        depthMeta = std::to_string(frame.width) + " x " + std::to_string(frame.height);
        frameUpdated = true;
    }

    rin::IntrinsicsSnapshot snapshot;
    if (service->tryLoadIntrinsics(lastIntrinsicsSequence, snapshot)) {
        intrinsics = std::move(snapshot);
        hasIntrinsics = true;
    }

    rin::DeviceCatalog newCatalog;
    if (service->tryLoadCatalog(lastCatalogSequence, newCatalog)) {
        catalog = std::move(newCatalog);
        hasCatalog = true;
        rebuildDeviceOptions();
        const std::string previousOptionsForSerial = resolutionOptionsForSerial;
        rebuildResolutionOptions();
        if (resolutionOptionsForSerial != previousOptionsForSerial) {
            // 仅设备变化（首次就绪/切换设备）时回到默认档位 848x480@30（DEC-004；
            // M13-01 起帧率同为判据——同宽高存在多帧率档位，必须整体匹配默认
            // 视频字段）；同一设备的目录刷新必须保持用户已选档位。
            for (std::size_t index = 0; index < resolutionOptions.size(); ++index) {
                const ResolutionUiOption& option = resolutionOptions[index];
                if (option.request.colorWidth == kDefaultRequest.colorWidth &&
                    option.request.colorHeight == kDefaultRequest.colorHeight &&
                    option.request.colorFps == kDefaultRequest.colorFps &&
                    option.request.depthFps == kDefaultRequest.depthFps) {
                    resolutionIndex.set(static_cast<int>(index));
                    break;
                }
            }
        }
    }

    rin::ServiceEvent event;
    if (service->tryLoadEvent(event)) {
        statusState = event.state;
        statusMessage = event.message;
    }

    // 无设备稳态排空（2026-10-07 热插拔反馈）：Waiting/Failed 下不显示陈旧
    // 画面——预览卡回空态、监看器预览回占位符（序号水位保持，重连后经服务
    // 生命周期帧序号恢复推进）；Opening/Restreaming 为瞬态，保留末帧避免闪烁。
    // 监看器排空走 viewer::drainMonitorViews 唯一实现（M12/CR-35），
    // resetWatermark=false：相机服务帧序号为服务生命周期连续，与新帧序号
    // 比较的水位必须保留（引擎侧非 Running 排空才复位，见 node_canvas.hpp）。
    if (statusState == rin::CameraServiceState::Waiting ||
        statusState == rin::CameraServiceState::Failed) {
        if (rgbView.valid() || depthView.valid()) {
            rgbView.release();
            depthView.release();
            rgbMeta = "waiting for device";
            depthMeta = "waiting for device";
            frameUpdated = true;
        }
        if (policyDepthView.valid()) {
            policyDepthView.release();
            policyDepthMeta = "waiting for metric depth";
            frameUpdated = true;
        }
        if (viewer::drainMonitorViews(workflowCanvas, /*resetWatermark=*/false)) {
            app::requestUpdate();
        }
    }

    // 策略深度可选输出（DEC-022）：最新态消费显示帧（tick 侧已完成 O6 组合
    // 与灰度量化，此处仅上传）。无论卡片当前选择哪个输出都保持消费——邮箱
    // 单槽换新，切换输出即时呈现最新帧。
    if (policyDepth != nullptr) {
        rin::Frame policyFrame;
        if (policyDepth->tryLoadFrame(lastPolicySequence, policyFrame)) {
            policyDepthView.update(policyFrame);
            policyDepthMeta = "policy " + std::to_string(policyFrame.width) + " x " +
                              std::to_string(policyFrame.height) + " \u00B7 seq " +
                              std::to_string(policyFrame.sequence);
            frameUpdated = true;
        }
    }

    // 工作流引擎事件（底部事件行 + 节点失败标注；契约 tryLoad* 非阻塞最新态
    // 语义，RULE-05 渲染线程只做有界消费）。NodeFailed 事件驱动画布/面板
    // destructive 徽标（M5-04 §4），Started/Stopped 清空（会话级）。GraphApplied
    // 事件清除待生效标注（M5-06 §4：Running 下图结构变更 → 帧边界应用确认）。
    if (workflow != nullptr) {
        // 帧源路由刷新（M6-05）：画布图变更（修订号推进）即重建 rendition
        // 快照，原子换新对帧泵 tick 可见。
        if (sourceRouterRevision != workflowCanvas.revision) {
            refreshSourceRouter(*this);
        }
        // 引擎事件消费：pumpWorkflowEvents 唯一实现（M12/CR-37；测试同源消费）。
        if (pumpWorkflowEvents(workflowCanvas, *workflow)) {
            app::requestUpdate();
        }
        // 监看器预览消费（M11/DEC-021 §5.6 替代）：Running 逐监看器拉取最新
        // 产物上传节点内嵌预览；非 Running 整体排空释放（§4 停止/关闭排空）。
        // 驱动尺寸刷新（ROI 联动约束的输入尺寸源）同址有界消费。
        if (pumpMonitorViews(workflowCanvas, *workflow) |
            pumpDriverSizes(workflowCanvas, *workflow)) {
            app::requestUpdate();
        }
        // 性能统计消费（M5-05 §5.7）：sequence 推进拉取最新快照 + 活动/冻结
        // 标志同步（停止后末次值冻结呈现，§4）。返回 true 需重绘。
        if (workflowCanvas.perf.consume(*workflow)) {
            app::requestUpdate();
        }
    }

    // 姿态通道（M3-06/07，EXEC-06 最新态语义）：Streaming/Restreaming 中消费最新
    // 快照（3D 位姿视图与 IMU 状态面板共用）；其余状态（含 restream 重建窗口——
    // 融合器已复位、快照暂停发布）回空态并复位视图参考，避免陈旧姿态滞留显示。
    // 无新快照（false）不触碰现有状态。
    bool poseUpdated = false;
    if (statusState == rin::CameraServiceState::Streaming ||
        statusState == rin::CameraServiceState::Restreaming) {
        rin::ImuSnapshot pose;
        if (service->tryLoadPose(lastPoseSequence, pose)) {
            poseView.update(pose);
            poseUpdated = true;
        }
    } else if (poseView.available) {
        poseView.clear();
        poseUpdated = true;
    }

    if (frameUpdated || poseUpdated) {
        app::requestUpdate();  // 外部纹理/最新态快照非动画元素，需显式请求重绘
    }
}

/// 关闭顺序（EXEC-04，M3-07 含姿态通道排空语义）：停止命令生产者（服务 stop =
/// request_stop + worker 回收，返回后全部通道不再有新发布）→ 排空 UI 侧跨上下文
/// 姿态状态（PoseViewState 回空态，陈旧快照不跨 shutdown 存活）→ 工作流引擎
/// stop + 释放（引擎生命周期纪律：实例先于 executor shutdown 停止或析构，
/// engine.hpp；帧源以 shared_ptr 持有相机服务，服务已停后引擎侧残余 tick 的
/// 取帧恒为"无新帧"，闭包不悬垂）→ 工作流面板 UI 侧排空（M5-04 §4：缩略图
/// 清空、控件绑定与失败标注复位；M5-05：性能统计消费态清空；M5-06：待生效
/// 标注随引擎释放复位）→ GPU 设备销毁前释放导入引用 → executor.shutdown(true)。
/// 全部在主线程 onShutdown 内完成；幂等。
void ViewerContext::shutdown() {
    if (shutdownDone) {
        return;  // 幂等：初始化失败清理路径也会进入。
    }
    shutdownDone = true;
    if (service != nullptr) {
        service->stop();
        service.reset();
    }
    // 策略深度 tick 停止（DEC-019 顺序：服务 stop 之后、executor shutdown
    // 之前；通道已无新发布，析构排空 LatestMailbox）。
    policyDepth.reset();
    policyDepthView.release();
    poseView.clear();  // 姿态通道 UI 侧排空（M3-07）：通道已无新发布，消费态归零。
    if (workflow != nullptr) {
        workflow->stop();  // 幂等；Idle 快路径。画布 UI 状态不跨 shutdown 复活
        workflow.reset();  // （契约 stop 排空语义：stale 数据不得恢复活动状态）。
        workflowCanvas.graphPending = false;
    }
    // 工作流画布 UI 侧排空（M5-04 §4 语义随 M11/DEC-021 迁移）：监看器预览
    // GL 引用释放（GPU 设备销毁前）、内嵌控件与驱动尺寸缓存清空、失败标注
    // 清空——stale 产物与控件态不跨 shutdown 存活。性能统计消费态同址排空
    // （M5-05 §4：clear 后 stale 统计不跨 shutdown 复活）。
    for (auto& [id, monitor] : workflowCanvas.monitors) {
        (void)id;
        monitor.view.release();
    }
    workflowCanvas.monitors.clear();
    workflowCanvas.controls.clear();
    workflowCanvas.driverSizes.clear();
    workflowCanvas.failures.clear();
    workflowCanvas.perf.clear();
    rgbView.release();  // GPU 设备销毁前释放导入引用（框架 retirement 完成删除）
    depthView.release();
    if (started) {
        (void)executor.shutdown(true);
    }
}

/// 头部（全局，跨页共享相机运行态）：标题（base/semibold）+ 状态点 + 状态文本
/// （语义色）+ 事件消息（次级）。
void composeHeader(eui::Ui& ui, const ViewerContext& ctx, float x, float y, float width) {
    ui.row("header")
        .position(x, y)
        .size(width, 24.0f)
        .gap(kSpace2)
        .content([&] {
            ui.text("header.title")
                .text("Rin")
                .fontSize(kFontBase)
                .fontWeight(kWeightSemibold)
                .color(dark().fg)
                .build();
            ui.rect("header.dot")
                .size(kSpace2, kSpace2)
                .radius(kSpace2)
                .color(stateColor(ctx.statusState))
                .build();
            ui.text("header.state")
                .text(rin::toString(ctx.statusState))
                .fontSize(kFontSm)
                .fontWeight(kWeightMedium)
                .color(stateColor(ctx.statusState))
                .build();
            if (!ctx.statusMessage.empty()) {
                ui.text("header.message")
                    .text(ctx.statusMessage)
                    .fontSize(kFontSm)
                    .color(dark().fgSubtlest)
                    .build();
            }
            if (!ctx.startError.empty()) {
                ui.text("header.error")
                    .text(ctx.startError)
                    .fontSize(kFontSm)
                    .color(dark().destructive)
                    .build();
            }
        })
        .build();
}

/// 画面卡：card 底 + cardBorder 1px + 圆角 xl；内嵌 surface 画面区圆角 md；
/// 角标为 caption 次级标签与 xs 元数据（文字层级表达密度，不加多余描边）。
/// toggleText 非空时在标签右侧绘制输出切换胶囊（DEC-022：深度卡 Raw/Policy
/// 可选输出；点击回调由调用方注入，样式与工作流枚举胶囊同源）。
void composeViewCard(eui::Ui& ui, const char* id, float width, float height, const char* label,
                     const std::string& meta, GpuFrameView& view, const char* toggleText = nullptr,
                     const std::function<void()>& toggleOnClick = {}) {
    const float pad = kSpace3;
    const float labelHeight = kFontCaption + kSpace1;
    const float areaY = pad + labelHeight + kSpace1;
    const float areaHeight = height - areaY - pad;

    ui.stack(id)
        .size(width, height)
        .content([&] {
            ui.rect(std::string(id) + ".card")
                .size(width, height)
                .radius(kRadiusXl)
                .color(dark().card)
                .border(kBorderHairline, dark().cardBorder)
                .build();
            ui.text(std::string(id) + ".label")
                .position(pad, pad)
                .size(width - pad * 2.0f, labelHeight)
                .text(label)
                .fontSize(kFontCaption)
                .fontWeight(kWeightMedium)
                .color(dark().fgSubtle)
                .build();
            if (toggleText != nullptr) {
                constexpr float kToggleWidth = 76.0f;
                ui.rect(std::string(id) + ".toggle")
                    .position(pad + 52.0f, pad - 1.0f)
                    .size(kToggleWidth, labelHeight)
                    .radius(kRadiusSm)
                    .color(dark().input)
                    .border(kBorderHairline, dark().inputBorder)
                    .states(dark().input, dark().inputBorderHover, dark().inputBorderHover)
                    .onClick(toggleOnClick)
                    .build();
                ui.text(std::string(id) + ".toggleText")
                    .position(pad + 52.0f, pad - 1.0f)
                    .size(kToggleWidth, labelHeight)
                    .text(toggleText)
                    .fontSize(kFontXs)
                    .color(dark().fg)
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
            }
            ui.text(std::string(id) + ".meta")
                .position(pad, pad)
                .size(width - pad * 2.0f, labelHeight)
                .text(meta)
                .fontSize(kFontXs)
                .color(dark().fgSubtlest)
                .horizontalAlign(eui::HorizontalAlign::Right)
                .build();
            ui.rect(std::string(id) + ".area")
                .position(pad, areaY)
                .size(width - pad * 2.0f, areaHeight)
                .radius(kRadiusMd)
                .color(dark().surface)
                .build();
            ui.image(std::string(id) + ".img")
                .position(pad, areaY)
                .size(width - pad * 2.0f, areaHeight)
                .texture(view.image(), view.revision())
                .contain()
                .radius(kRadiusMd)
                .build();
        })
        .build();
}

/// 预览页控制行标签与提示（下拉触发器本体由 overlay 层最后合成，保证弹层浮顶）。
/// 深度配色选择已按 DEC-014 收纳至设置页。narrow（内容宽 < 640）时右锚定的
/// Resolution 会贴靠 Device 选择器，标签/长提示让位隐藏，选择器取值文本自描述；
/// ox 为内容左缘，resolutionX 由 composePreviewPage 统一计算传入。
void composeControls(eui::Ui& ui, const ViewerContext& ctx, float ox, bool narrow,
                     float resolutionX, float rowY) {
    const float labelY = rowY + 13.0f;
    ui.text("controls.device.label")
        .position(ox, labelY)
        .text("Device")
        .fontSize(kFontSm)
        .fontWeight(kWeightMedium)
        .color(dark().fgSubtle)
        .build();
    if (narrow) {
        return;  // 右锚定组已贴靠，标签/提示与选择器重叠，让位。
    }
    ui.text("controls.resolution.label")
        .position(resolutionX - 98.0f, labelY)
        .text("Resolution")
        .fontSize(kFontSm)
        .fontWeight(kWeightMedium)
        .color(dark().fgSubtle)
        .build();
    if (ctx.hasCatalog && ctx.catalog.activeSerial.empty() && ctx.catalog.devices.size() > 1) {
        ui.text("controls.hint")
            .position(ox + 282.0f, labelY)
            .text("multiple devices, select one")
            .fontSize(kFontXs)
            .color(dark().warning)
            .build();
    } else if (ctx.hasCatalog && !ctx.catalog.activeSerial.empty() && ctx.catalog.activeIsAuto &&
               ctx.catalog.devices.size() > 1) {
        ui.text("controls.hint")
            .position(ox + 282.0f, labelY)
            .text("auto-selected, click Device to change")
            .fontSize(kFontXs)
            .color(dark().fgSubtlest)
            .build();
    } else if (!ctx.deviceOptions.empty() && ctx.catalog.activeSerial.empty()) {
        ui.text("controls.hint")
            .position(ox + 60.0f + 210.0f + kSpace3, labelY)
            .text("select device")
            .fontSize(kFontXs)
            .color(dark().warning)
            .build();
    } else if (ctx.hasCatalog && ctx.catalog.devices.empty()) {
        ui.text("controls.hint")
            .position(ox + 60.0f + 210.0f + kSpace3, labelY)
            .text("no camera connected - plug in and it appears here")
            .fontSize(kFontXs)
            .color(dark().fgSubtlest)
            .build();
    }
}

/// 轻量选择控件（viewer 内自研，绕开 components::dropdown 的弹层缺陷
/// EUI-20260923-003/004）：字段 + 展开式菜单面板，全部使用主题令牌；
/// 由根 stack 最后合成，菜单自然浮于卡片之上。onClick 只在字段/菜单行上。
void composeSelect(eui::Ui& ui, const char* id, float x, float y, float width,
                   const std::string& placeholder, const std::vector<std::string>& items,
                   int selectedIndex, bool open, const std::function<void()>& toggleOpen,
                   const std::function<void(int)>& onPick) {
    const float fieldHeight = 38.0f;
    const float itemHeight = 34.0f;
    const float menuPad = kSpace1;
    const int count = static_cast<int>(items.size());
    const bool hasSelection = selectedIndex >= 0 && selectedIndex < count;
    const float menuHeight = menuPad * 2.0f + itemHeight * count;
    const float totalHeight = fieldHeight + (open ? kSpace1 + menuHeight : 0.0f);

    // 根级下拉浮层显式抬层（kOverlayZIndex）：与 composeFloatingPanel 同理——
    // 页面内容若含 zIndex 抬升元素（滑条 .hit），默认 z=0 的下拉会在根级兄弟
    // 命中/绘制排序中落到页面之下，弹层点击穿透（见 viewer_components.hpp）。
    ui.stack(id)
        .position(x, y)
        .size(width, totalHeight)
        .zIndex(kOverlayZIndex)
        .content([&] {
            ui.rect(std::string(id) + ".field")
                .size(width, fieldHeight)
                .radius(kRadiusLg)
                .color(dark().input)
                .border(kBorderHairline, dark().inputBorder)
                .onClick([toggleOpen] { toggleOpen(); })
                .build();
            ui.text(std::string(id) + ".value")
                .position(kSpace3, 0.0f)
                .size(width - kSpace3 * 2.0f - kSpace4, fieldHeight)
                .text(hasSelection ? items[static_cast<std::size_t>(selectedIndex)] : placeholder)
                .fontSize(kFontBase)
                .fontWeight(kWeightMedium)
                .color(hasSelection ? dark().fg : dark().fgSubtlest)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            ui.text(std::string(id) + ".chevron")
                .position(width - kSpace4, 0.0f)
                .size(kSpace4, fieldHeight)
                .text(open ? "\uF077" : "\uF078")
                .fontSize(kFontXs)
                .color(dark().brand)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();

            if (open && count > 0) {
                ui.rect(std::string(id) + ".menu")
                    .position(0.0f, fieldHeight + kSpace1)
                    .size(width, menuHeight)
                    .radius(kRadiusLg)
                    .color(dark().menu)
                    .border(kBorderHairline, dark().border)
                    .shadow(kOverlayShadowOffsetX, kOverlayShadowOffsetY, kOverlayShadowBlur, kOverlayShadowColor)
                    .build();
                for (int index = 0; index < count; ++index) {
                    const bool active = index == selectedIndex;
                    const float itemY = menuPad + itemHeight * index;
                    ui.rect(std::string(id) + ".item" + std::to_string(index))
                        .position(kSpace1, fieldHeight + kSpace1 + itemY)
                        .size(width - kSpace2, itemHeight)
                        .radius(kRadiusMd)
                        .color(active ? dark().accentSurface : (dark().menu))
                        .onClick([index, onPick] { onPick(index); })
                        .build();
                    ui.text(std::string(id) + ".itemText" + std::to_string(index))
                        .position(kSpace3, fieldHeight + kSpace1 + itemY)
                        .size(width - kSpace3 * 2.0f, itemHeight)
                        .text(items[static_cast<std::size_t>(index)])
                        .fontSize(kFontBase)
                        .color(active ? dark().brand : dark().fg)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .build();
                }
            }
        })
        .build();
}

void composeIntrinsicsCard(eui::Ui& ui, const ViewerContext& ctx, float width, float height,
                           float x, float y) {
    ui.stack("intrinsics")
        .position(x, y)
        .size(width, height)
        .content([&] {
            // 卡片底 + 标题 + 名称/值行列表：共享脚手架（M12/CR-33、CR-38）。
            const CardGeometry card =
                composeCardShell(ui, dark(), "intrinsics", "Intrinsics", width, height);

            const rin::StreamIntrinsics streams[] = {
                ctx.hasIntrinsics ? ctx.intrinsics.color : rin::StreamIntrinsics{},
                ctx.hasIntrinsics ? ctx.intrinsics.depth : rin::StreamIntrinsics{},
            };
            const char* names[] = {"Color", "Depth"};
            const char* ids[] = {"color", "depth"};
            for (int index = 0; index < 2; ++index) {
                const float rowY =
                    card.contentTop + static_cast<float>(index) * (kFontBase + kSpace2);
                ui.text(std::string("intrinsics.") + ids[index] + ".name")
                    .position(card.pad, rowY)
                    .size(kLabeledRowsValueX - card.pad - kSpace2, kFontBase + kSpace2)
                    .text(names[index])
                    .fontSize(kFontCaption)
                    .fontWeight(kWeightMedium)
                    .color(dark().fgSubtle)
                    .build();
                ui.text(std::string("intrinsics.") + ids[index] + ".values")
                    .position(kLabeledRowsValueX, rowY)
                    .size(width - kLabeledRowsValueX - card.pad, kFontBase + kSpace2)
                    .text(formatIntrinsicsValues(streams[index]))
                    .fontSize(kFontBase)
                    .color(dark().fg)
                    .build();
                if (streams[index].valid()) {
                    ui.text(std::string("intrinsics.") + ids[index] + ".model")
                        .position(width - card.pad - kLabeledRowsMetaWidth, rowY)
                        .size(kLabeledRowsMetaWidth, kFontBase + kSpace2)
                        .text(std::string("[") + distortionName(streams[index].model) + "]")
                        .fontSize(kFontXs)
                        .color(dark().fgSubtlest)
                        .horizontalAlign(eui::HorizontalAlign::Right)
                        .build();
                }
            }
        })
        .build();
}

// --- 页面（DEC-014 四页；选择控件本体由 compose() overlay 层最后合成） ---

/// 预览页：设备/分辨率控制行 + RGB/Depth 画面卡 + 内参卡（DEC-014 决策 3：沿用
/// 现有卡片式组织；深度配色已按决策收纳至设置页）。
void composePreviewPage(eui::Ui& ui, ViewerContext& ctx, float ox, float y, float width,
                        float height) {
    const float controlsHeight = 38.0f;
    const float panelHeight = 136.0f;
    const float viewsTop = y + controlsHeight + kSpace3;
    const float viewsHeight =
        std::max(160.0f, height - controlsHeight - kSpace3 * 2.0f - panelHeight);
    const float viewWidth = (width - kSpace3) / 2.0f;
    const float resolutionX = ox + width - 180.0f;
    const bool narrowControls = width < 640.0f;

    composeControls(ui, ctx, ox, narrowControls, resolutionX, y);
    ui.row("views")
        .position(ox, viewsTop)
        .size(width, viewsHeight)
        .gap(kSpace3)
        .content([&] {
            composeViewCard(ui, "view.rgb", viewWidth, viewsHeight, "RGB", ctx.rgbMeta,
                            ctx.rgbView);
            // 深度可选输出（DEC-022）：Raw = 相机深度 rendition（配色跟随全局
            // 配色命令）；Policy = 策略输出（O6 冻结组合单帧，灰度、无历史
            // 分片）。胶囊显示当前输出，点击循环切换。
            const bool policySource = ctx.depthSourceIndex.get() != 0;
            composeViewCard(ui, "view.depth", viewWidth, viewsHeight, "Depth",
                            policySource ? ctx.policyDepthMeta : ctx.depthMeta,
                            policySource ? ctx.policyDepthView : ctx.depthView,
                            policySource ? "Policy" : "Raw", [&ctx] {
                                ctx.depthSourceIndex.set((ctx.depthSourceIndex.get() + 1) % 2);
                            });
        })
        .build();
    composeIntrinsicsCard(ui, ctx, width, panelHeight, ox, y + height - panelHeight);
}

/// 位姿页：3D 位姿视图卡 + IMU 状态面板（M3-07 卡片对迁移为独立页面，DEC-014
/// 决策 3）。位姿卡占满页面高；IMU 面板固定信息行高度（内容自适应，余下留白）。
void composePosePage(eui::Ui& ui, ViewerContext& ctx, float ox, float y, float width,
                     float height) {
    const float imuPanelWidth = std::clamp(width * 0.38f, 320.0f, 480.0f);
    const float poseWidth = width - imuPanelWidth - kSpace3;
    composePoseViewCard(ui, ctx.poseView, ctx.hasIntrinsics ? &ctx.intrinsics : nullptr, poseWidth,
                        height, ox, y);
    composeImuPanelCard(ui, ctx.poseView, imuPanelWidth, std::min(height, 160.0f),
                        ox + poseWidth + kSpace3, y);
}

/// 设置页：深度配色切换（DEC-007 命令通道；DEC-014 决策 3 收纳至设置页）+
/// 关于/版本信息。选择控件本体在 overlay 层合成（composeSelect，见 compose()）。
void composeSettingsPage(eui::Ui& ui, float ox, float y, float width) {
    const theme::ThemeTokens& tokens = dark();
    const float pad = kSpace3;
    const float titleHeight = kFontSm + kSpace1;
    const float preferencesHeight = 172.0f;
    const float aboutHeight = 150.0f;

    ui.stack("settings.preferences")
        .position(ox, y)
        .size(width, preferencesHeight)
        .content([&] {
            // 卡片底 + 标题：共享脚手架（M12/CR-33；同函数两卡同款）。
            composeCardShell(ui, tokens, "settings.preferences", "Preferences", width,
                             preferencesHeight);
            ui.text("settings.preferences.paletteLabel")
                .position(pad, 48.0f)
                .size(150.0f, 38.0f)
                .text("Depth palette")
                .fontSize(kFontBase)
                .color(tokens.fg)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            ui.text("settings.preferences.fontLabel")
                .position(pad, 88.0f)
                .size(150.0f, 38.0f)
                .text("Font size")
                .fontSize(kFontBase)
                .color(tokens.fg)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            ui.text("settings.preferences.paletteHint")
                .position(pad, 136.0f)
                .size(width - pad * 2.0f, kFontSm + kSpace1)
                .text("depth palette applies to the live stream - font size scales the "
                      "whole interface")
                .fontSize(kFontSm)
                .color(tokens.fgSubtlest)
                .build();
        })
        .build();

    ui.stack("settings.about")
        .position(ox, y + preferencesHeight + kSpace3)
        .size(width, aboutHeight)
        .content([&] {
            composeCardShell(ui, tokens, "settings.about", "About", width, aboutHeight);
            ui.text("settings.about.name")
                .position(pad, 46.0f)
                .size(width - pad * 2.0f, kFontLg + kSpace2)
#ifdef RIN_VERSION
                .text("Rin v" RIN_VERSION)
#else
                .text("Rin")
#endif
                .fontSize(kFontLg)
                .fontWeight(kWeightSemibold)
                .color(tokens.fg)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            ui.text("settings.about.description")
                .position(pad, 78.0f)
                .size(width - pad * 2.0f, kFontSm + kSpace1)
                .text("RealSense depth-camera preview and image workflow workbench")
                .fontSize(kFontSm)
                .color(tokens.fgSubtle)
                .build();
            ui.text("settings.about.stack")
                .position(pad, 100.0f)
                .size(width - pad * 2.0f, kFontXs + kSpace1)
                .text("librealsense2 · EUI-NEO · executor")
                .fontSize(kFontXs)
                .color(tokens.fgSubtlest)
                .build();
        })
        .build();
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    ViewerContext& ctx = context();
    ctx.pump();

    const float pad = kSpace4;             // 16px：标准卡/面板内边距
    const float ox = kNavRailWidth + pad;  // 内容左缘（导航栏右移一个内边距）
    const float contentWidth = screen.width - kNavRailWidth - pad * 2.0f;
    const float headerHeight = 24.0f;
    const float pageTop = pad + headerHeight + kSpace3;
    const float pageHeight = std::max(160.0f, screen.height - pageTop - pad);
    // 预览页 overlay 选择器几何（与 composePreviewPage/composeControls 共享；
    // 分辨率严格右锚定于内容区，标签让位由 narrow 分支处理）。
    const float deviceFieldX = ox + 60.0f;
    const float resolutionFieldX = ox + contentWidth - 180.0f;

    // 根用 stack 绝对布局：overlay 选择器必须晚于页面卡片合成（绘制顺序即层叠
    // 顺序，EUI 的 zIndex 不跨父容器），否则弹层被后绘制的兄弟卡片覆盖。
    ui.stack("root")
        .size(screen.width, screen.height)
        .onFrame([](float) { context().pump(); })
        .content([&] {
            composeNavRail(ui, ctx.nav, screen.height,
                           [&ctx](WorkbenchPage page) { ctx.nav.switchTo(page); });
            composeHeader(ui, ctx, ox, pad, contentWidth);

            switch (ctx.nav.current) {
                case WorkbenchPage::Preview:
                    composePreviewPage(ui, ctx, ox, pageTop, contentWidth, pageHeight);
                    break;
                case WorkbenchPage::Pose:
                    composePosePage(ui, ctx, ox, pageTop, contentWidth, pageHeight);
                    break;
                case WorkbenchPage::Workflow: {
                    // 相机分辨率入口绑定（M7-01 稳定化）：绑定对象与 labels 均
                    // 为 ViewerContext 成员（retained 回调持有安全），仅
                    // selectedIndex 每帧从共享信号刷新。
                    ctx.cameraResolutionBinding.selectedIndex = ctx.resolutionIndex.get();
                    composeWorkflowPage(ui, ctx.workflowCanvas, ctx.workflow.get(),
                                        &ctx.cameraResolutionBinding, ox, pageTop,
                                        contentWidth, pageHeight);
                    break;
                }
                case WorkbenchPage::Settings:
                    composeSettingsPage(ui, ox, pageTop, contentWidth);
                    break;
            }

            // overlay 层（最后合成 = 浮于卡片之上）：当前页的选择控件与工作流页
            // 浮层（右键创建菜单、调色板拖拽跟随）。
            if (ctx.nav.current == WorkbenchPage::Workflow) {
                composeWorkflowCreateMenu(ui, ctx.workflowCanvas, ctx.workflow.get(), screen.width,
                                          screen.height);
                composeWorkflowDragGhost(ui, ctx.workflowCanvas);
                ctx.cameraResolutionBinding.selectedIndex = ctx.resolutionIndex.get();
                composeWorkflowResolutionMenu(ui, ctx.workflowCanvas, &ctx.cameraResolutionBinding,
                                              screen.width, screen.height);
            } else if (ctx.nav.current == WorkbenchPage::Preview) {
                std::vector<std::string> deviceLabels;
                deviceLabels.reserve(ctx.deviceOptions.size());
                for (const DeviceUiOption& option : ctx.deviceOptions) {
                    deviceLabels.push_back(option.label);
                }
                if (!deviceLabels.empty()) {
                    composeSelect(
                        ui, "controls.device", deviceFieldX, pageTop, 210.0f, "select device",
                        deviceLabels, ctx.deviceIndex.get(), ctx.deviceOpen.get(),
                        [&] { ctx.deviceOpen.set(!ctx.deviceOpen.get()); },
                        [&ctx](int index) {
                            ctx.deviceOpen.set(false);
                            ctx.deviceIndex.set(index);
                            ctx.applyDeviceChoice(index);
                        });
                }
                if (!ctx.resolutionOptions.empty()) {
                    std::vector<std::string> resolutionLabels;
                    resolutionLabels.reserve(ctx.resolutionOptions.size());
                    for (const ResolutionUiOption& option : ctx.resolutionOptions) {
                        resolutionLabels.push_back(option.label);
                    }
                    composeSelect(
                        ui, "controls.resolution", resolutionFieldX, pageTop, 180.0f, "select",
                        resolutionLabels, ctx.resolutionIndex.get(), ctx.resolutionOpen.get(),
                        [&] { ctx.resolutionOpen.set(!ctx.resolutionOpen.get()); },
                        [&ctx](int index) {
                            ctx.resolutionOpen.set(false);
                            ctx.resolutionIndex.set(index);
                            ctx.applyResolutionChoice(index);
                        });
                }
            } else if (ctx.nav.current == WorkbenchPage::Settings) {
                // 深度配色（DEC-007）：不依赖设备目录，Waiting 态可预设；与卡片
                // 内 "Depth palette" 标签同行（标签行 y=48，见 composeSettingsPage）。
                composeSelect(
                    ui, "settings.preferences.palette", ox + 170.0f, pageTop + 48.0f, 170.0f,
                    "select", {"Jet", "Grayscale", "Adaptive"}, ctx.paletteIndex.get(),
                    ctx.paletteOpen.get(), [&] { ctx.paletteOpen.set(!ctx.paletteOpen.get()); },
                    [&ctx](int index) {
                        ctx.paletteOpen.set(false);
                        ctx.paletteIndex.set(index);
                        ctx.applyPaletteChoice(index);
                    });
                // 字号档位（DEC-022）：界面缩放实现，运行期写应用配置 uiScale
                //（框架每帧读取 effectiveScale，变更即整页重排）；会话级。
                composeSelect(
                    ui, "settings.preferences.font", ox + 170.0f, pageTop + 88.0f, 170.0f,
                    "select", std::vector<std::string>(std::begin(kFontScaleLabels),
                                                       std::end(kFontScaleLabels)),
                    ctx.fontScaleIndex.get(), ctx.fontScaleOpen.get(),
                    [&] { ctx.fontScaleOpen.set(!ctx.fontScaleOpen.get()); },
                    [&ctx](int index) {
                        ctx.fontScaleOpen.set(false);
                        ctx.fontScaleIndex.set(index);
                        app::setUiScale(kFontScales[static_cast<std::size_t>(
                            std::clamp(index, 0,
                                       static_cast<int>(std::size(kFontScales)) - 1))]);
                    });
            }
        })
        .build();
}

}  // namespace
}  // namespace viewer

namespace app {

// deb 安装布局的运行时字体目录（cmake/Packaging.cmake 随包安装；DEC-009 的
// deb 前缀固定 /usr）。EUI 默认按"可执行文件旁 assets/"解析字体：开发布局由
// eui_neo_configure_app 把 assets 拷到可执行文件旁、解析命中；安装布局
// /usr/bin 旁无 assets，退化为系统回退——中文与 Font Awesome 图标码位在常见
// 系统字体中无覆盖（本机 Ubuntu 24.04 桌面实测：中文无 CJK 路径命中、图标
// 静默消失；strace 证据见 M2 计划验证记录 2026-09-29）。该目录存在时显式
// 指定两个默认字体；文件名与 EUI 默认字体一致（eui core/render/text.cpp
// kDefaultUiFontFile/kDefaultIconFontFile），更换 EUI 字体时须同步本处与
// Packaging.cmake。
constexpr const char* kInstalledUiFont =
    "/usr/share/rin/fonts/JingNanJunJunTi-JinNanJunJunTi-Bold-2.ttf";
constexpr const char* kInstalledIconFont = "/usr/share/rin/fonts/Font Awesome 7 Free-Solid-900.otf";

DslAppConfig makeDslAppConfig() {
    viewer::ensureStarted();
    DslAppConfig config =
        DslAppConfig{}
            .title("Rin")
            .pageId("Rin")
            // 桌面标识（上游 dev d7b15ea，issue #77）：X11 WM_CLASS 与 Wayland
            // app_id；未设置时 WM_CLASS 为空、任务栏图标缺失。
            .appId("rin")
            .clearColor(viewer::theme::dark().background)
            // 工作台壳默认版面（M5-02）：四页与工作流五区骨架（DEC-014）需要比
            // M1 单页 viewer 更大的最小版面。注意 EUI 的 Screen 为逻辑尺寸（X11/
            // XWayland 下 framebuffer 不随监视器缩放放大，逻辑视口 ≈ 窗口像素 /
            // 监视器缩放），本机 2x 面板上为 960x600 逻辑——布局按逻辑视口自适应。
            .windowSize(1920, 1200)
            .fps(60.0)
            .onKeyEvent([](const eui::KeyEvent& event) {
                if (!event.isDown()) {
                    return;
                }
                auto& ctx = viewer::context();
                // 工作流页画布快捷键（§3/§5.1/§5.4；每项均有鼠标等价路径：
                // Fit 按钮 / 右键删除 / 点击空白收起菜单）。F=帧全图，
                // Del=删除选中（节点+关联边、选中连线），Esc=取消拖拽/收起菜单。
                if (ctx.nav.current == viewer::WorkbenchPage::Workflow && ctx.workflow != nullptr) {
                    viewer::WorkflowCanvasState& canvas = ctx.workflowCanvas;
                    if (event.key == eui::InputKey::F) {
                        canvas.view.fit(canvas.model.graphBounds(), canvas.viewport.x,
                                        canvas.viewport.y);
                        ++canvas.revision;
                        return;
                    }
                    if (event.key == eui::InputKey::Delete) {
                        canvas.feedback = canvas.interaction.deleteSelection(canvas.model);
                        canvas.afterGraphChange(ctx.workflow.get());
                        ++canvas.revision;
                        return;
                    }
                    if (event.key == eui::InputKey::Escape) {
                        if (canvas.menuOpen) {
                            canvas.menuOpen = false;
                            canvas.menuFilter.set("");
                        }
                        canvas.resolutionMenuOpen = false;
                        // 取消进行中的拖拽（框选/连线/平移；§5.3 空白松开语义）。
                        canvas.interaction.mode = viewer::InteractionMode::None;
                        canvas.interaction.draggedNode = rin::kInvalidNode;
                        canvas.paletteDragging = false;
                        ++canvas.revision;
                        return;
                    }
                }
                // 数字键 = 预览页分辨率档位速选（M1 快捷键；随工作台壳收窄到
                // 预览页——分辨率控件位于预览页，其他页不响应）。
                if (ctx.nav.current != viewer::WorkbenchPage::Preview) {
                    return;
                }
                const int digit =
                    static_cast<int>(event.key) - static_cast<int>(eui::InputKey::Digit1) + 1;
                if (digit >= 1 && digit <= 9 &&
                    digit - 1 < static_cast<int>(ctx.resolutionOptions.size())) {
                    // 与下拉路径同序（先 set 再 apply）：Resolution 选择器由
                    // resolutionIndex 驱动，缺 set 会显示 stale 档位；越界档位
                    // 保持 no-op（与 applyResolutionChoice 拒绝语义一致）。
                    ctx.resolutionIndex.set(digit - 1);
                    ctx.applyResolutionChoice(digit - 1);
                }
            })
            .onShutdown([] { viewer::context().shutdown(); });

    // 安装态字体捆绑生效（见上方说明）：显式路径由 EUI resolveFontFilePath
    // 直接命中，开发布局目录不存在、保持 EUI 默认 exeDir/assets 解析。
    std::error_code fsError;
    if (std::filesystem::exists(kInstalledUiFont, fsError) &&
        std::filesystem::exists(kInstalledIconFont, fsError)) {
        config.textFont(kInstalledUiFont).iconFont(kInstalledIconFont);
    }
    return config;
}

/// 应用配置存储（DEC-022）：uiScaleValue 运行期可变——字号档位经 setUiScale
/// 写入，EUI 每帧读取 effectiveScale = dpiScale × uiScale，变更即整页重排；
/// 其余字段仅初始化期消费。读写同在 UI 线程，无跨线程竞争。
DslAppConfig& appConfigStorage() {
    static DslAppConfig config = makeDslAppConfig();
    return config;
}

void setUiScale(float value) {
    appConfigStorage().uiScaleValue = value > 0.0f ? value : 1.0f;
    requestUpdate();
}

const DslAppConfig& dslAppConfig() {
    return appConfigStorage();
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    viewer::compose(ui, screen);
}

}  // namespace app
