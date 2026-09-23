// rin：EUI-NEO 前端。Executor 生命周期 owner 为 viewer::Context（DEC-002）：
// dslAppConfig() 首调点惰性启动，DslAppConfig::onShutdown（主线程、GPU 销毁前）关闭。
// 视觉层遵循 viewer_theme.hpp 的语义令牌翻译（DEC-005）：布局代码不出现一次性
// 颜色/字号/圆角。热插拔与设备选择见 DEC-006：启动不依赖相机连接，运行中经设备
// 目录自动识别，多设备时用户选择、唯一设备自动选择。3D 位姿视图（M3-06，DEC-011）
// 由 pose_view.hpp 承载：Core 投影纯逻辑 + polygon 有界组装，姿态经 tryLoadPose()
// 最新态消费。

#include "gpu_frame_view.hpp"
#include "pose_view.hpp"
#include "viewer_theme.hpp"

#include <eui_neo.h>

#include <executor/executor.hpp>

#include <realsense_camera_service.hpp>

#include <rin/camera_service.hpp>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace viewer {
namespace {

using namespace viewer::theme;
using rin::FrameKind;
using rin::StreamRequest;

/// 默认流请求（DEC-004 暂定默认值）；设备选择独立于流请求（DEC-006）。M3-06 起
/// 默认使能运动流（SCOPE-07）：IMU 设备上附加 ACCEL/GYRO 供 3D 位姿视图与 IMU
/// 面板消费；无 IMU 设备按契约退化为纯视频流（运动通道保持空，不视为错误）。
constexpr StreamRequest kDefaultRequest{.enableMotion = true};

struct ResolutionUiOption {
    StreamRequest request;
    std::string label;
};

struct DeviceUiOption {
    std::string serial;
    std::string label;
};

struct ViewerContext {
    executor::Executor executor;
    std::shared_ptr<rin::ICameraService> service;
    GpuFrameView rgbView;
    GpuFrameView depthView;

    std::uint64_t lastRgbSequence = 0;
    std::uint64_t lastDepthSequence = 0;
    std::uint64_t lastIntrinsicsSequence = 0;
    std::uint64_t lastCatalogSequence = 0;
    std::uint64_t lastPoseSequence = 0;

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
    /// 深度配色（DEC-007）：0 = Jet，1 = Grayscale；不依赖设备目录，Waiting 可预设。
    eui::Signal<int> paletteIndex{0};
    eui::Signal<bool> paletteOpen{false};

    rin::CameraServiceState statusState = rin::CameraServiceState::Idle;
    std::string statusMessage;
    std::string startError;
    std::string rgbMeta;
    std::string depthMeta;

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

void ensureStarted() {
    static const bool once = [] {
        ViewerContext& ctx = context();
        if (const auto init = ctx.executor.initialize_ex({}); !init) {
            ctx.startError = init.message;
            ctx.statusMessage = "executor init failed";
            return false;
        }
        ctx.service = rin::createRealSenseCameraService(ctx.executor);
        // 启动不依赖相机连接（DEC-006）：无设备时服务进入 Waiting，接入后自动出流。
        const rin::StartOutcome outcome = ctx.service->start(kDefaultRequest);
        if (!outcome.admitted) {
            ctx.startError = outcome.error;
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
    std::snprintf(buffer, sizeof(buffer),
                  "fx %.3f   fy %.3f   cx %.3f   cy %.3f   %ux%u",
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
    const rin::DeviceInfo* device = activeDevice(*this);
    if (device == nullptr) {
        resolutionOptionsForSerial.clear();
        return;  // 未选定设备（等待接入/用户选择）时不提供分辨率档位。
    }
    resolutionOptionsForSerial = device->serial;
    const auto byWidthHeight = [](const rin::ResolutionOption& option) {
        return std::pair<std::uint32_t, std::uint32_t>{option.width, option.height};
    };
    std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
    for (const rin::ResolutionOption& color : device->colorOptions) {
        if (color.fps != kDefaultRequest.colorFps) {
            continue;  // M1：固定 30fps 档位
        }
        if (!seen.insert(byWidthHeight(color)).second) {
            continue;
        }
        bool depthMatch = false;
        for (const rin::ResolutionOption& depth : device->depthOptions) {
            if (depth.width == color.width && depth.height == color.height &&
                depth.fps == kDefaultRequest.depthFps) {
                depthMatch = true;
                break;
            }
        }
        if (!depthMatch) {
            continue;
        }
        ResolutionUiOption option;
        option.request.colorWidth = color.width;
        option.request.colorHeight = color.height;
        option.request.colorFps = color.fps;
        option.request.depthWidth = color.width;
        option.request.depthHeight = color.height;
        option.request.depthFps = kDefaultRequest.depthFps;
        option.label = std::to_string(color.width) + " x " + std::to_string(color.height);
        resolutionOptions.push_back(std::move(option));
    }
}

void ViewerContext::applyResolutionChoice(int index) {
    if (service == nullptr || index < 0 ||
        index >= static_cast<int>(resolutionOptions.size())) {
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
    if (!service->requestDevice(deviceOptions[static_cast<std::size_t>(index)].serial,
                                &error)) {
        statusMessage = "device select rejected: " + error;
    }
}

void ViewerContext::applyPaletteChoice(int index) {
    if (service == nullptr || index < 0 || index > 1) {
        return;
    }
    const auto scheme =
        index == 1 ? rin::DepthColorScheme::Grayscale : rin::DepthColorScheme::Jet;
    std::string error;
    if (!service->requestDepthColorScheme(scheme, &error)) {
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
            // 仅设备变化（首次就绪/切换设备）时回到默认档位 848x480（DEC-004）；
            // 同一设备的目录刷新必须保持用户已选档位。
            for (std::size_t index = 0; index < resolutionOptions.size(); ++index) {
                const ResolutionUiOption& option = resolutionOptions[index];
                if (option.request.colorWidth == kDefaultRequest.colorWidth &&
                    option.request.colorHeight == kDefaultRequest.colorHeight) {
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

    // 姿态通道（M3-06，EXEC-06 最新态语义）：Streaming/Restreaming 中消费最新
    // 快照；其余状态（含 restream 重建窗口——融合器已复位、快照暂停发布）回空态
    // 并复位视图参考，避免陈旧姿态滞留显示。无新快照（false）不触碰现有状态。
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

void ViewerContext::shutdown() {
    if (shutdownDone) {
        return;  // 幂等：初始化失败清理路径也会进入。
    }
    shutdownDone = true;
    if (service != nullptr) {
        service->stop();
        service.reset();
    }
    rgbView.release();    // GPU 设备销毁前释放导入引用（框架 retirement 完成删除）
    depthView.release();
    if (started) {
        (void)executor.shutdown(true);
    }
}

/// 头部：标题（base/semibold）+ 状态点 + 状态文本（语义色）+ 事件消息（次级）。
void composeHeader(eui::Ui& ui, const ViewerContext& ctx, float width) {
    ui.row("header")
        .position(kSpace4, kSpace4)
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
void composeViewCard(eui::Ui& ui, const char* id, float width, float height, const char* label,
                     const std::string& meta, GpuFrameView& view) {
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

/// 控制行标签与提示（下拉触发器本体由 overlay 层最后合成，保证弹层浮顶）。
/// narrow（逻辑宽 < 640）时右锚定组会贴靠 Device 选择器，标签/长提示让位隐藏，
/// 选择器取值文本自描述；paletteX/resolutionX 由 compose() 统一计算传入。
void composeControls(eui::Ui& ui, const ViewerContext& ctx, bool narrow,
                     float paletteX, float resolutionX) {
    ui.text("controls.device.label")
        .position(kSpace4, 65.0f)
        .text("Device")
        .fontSize(kFontSm)
        .fontWeight(kWeightMedium)
        .color(dark().fgSubtle)
        .build();
    if (narrow) {
        return;  // 右锚定组已贴靠，标签/提示与选择器重叠，让位。
    }
    // 分辨率与深度配色两组控件右对齐（响应式：随窗口宽度变化不溢出）；
    // 标签贴在各自选择器左侧，与 Device 选择器（76..286）保持间距不重叠。
    ui.text("controls.resolution.label")
        .position(resolutionX - 98.0f, 65.0f)
        .text("Resolution")
        .fontSize(kFontSm)
        .fontWeight(kWeightMedium)
        .color(dark().fgSubtle)
        .build();
    ui.text("controls.palette.label")
        .position(paletteX - 98.0f, 65.0f)
        .text("Palette")
        .fontSize(kFontSm)
        .fontWeight(kWeightMedium)
        .color(dark().fgSubtle)
        .build();
    if (ctx.hasCatalog && ctx.catalog.activeSerial.empty() &&
        ctx.catalog.devices.size() > 1) {
        ui.text("controls.hint")
            .position(298.0f, 65.0f)
            .text("multiple devices, select one")
            .fontSize(kFontXs)
            .color(dark().warning)
            .build();
    } else if (ctx.hasCatalog && !ctx.catalog.activeSerial.empty() &&
               ctx.catalog.activeIsAuto && ctx.catalog.devices.size() > 1) {
        ui.text("controls.hint")
            .position(298.0f, 65.0f)
            .text("auto-selected, click Device to change")
            .fontSize(kFontXs)
            .color(dark().fgSubtlest)
            .build();
    } else if (!ctx.deviceOptions.empty() && ctx.catalog.activeSerial.empty()) {
        ui.text("controls.hint")
            .position(kSpace4 + 120.0f, 65.0f)
            .text("select device")
            .fontSize(kFontXs)
            .color(dark().warning)
            .build();
    } else if (ctx.hasCatalog && ctx.catalog.devices.empty()) {
        ui.text("controls.hint")
            .position(kSpace4 + 120.0f, 65.0f)
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

    ui.stack(id)
        .position(x, y)
        .size(width, totalHeight)
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
                .text(hasSelection ? items[static_cast<std::size_t>(selectedIndex)]
                                   : placeholder)
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
                    .shadow(18.0f, 10.0f, 8.0f, {0.0f, 0.0f, 0.0f, 0.35f})
                    .build();
                for (int index = 0; index < count; ++index) {
                    const bool active = index == selectedIndex;
                    const float itemY = menuPad + itemHeight * index;
                    ui.rect(std::string(id) + ".item" + std::to_string(index))
                        .position(kSpace1, fieldHeight + kSpace1 + itemY)
                        .size(width - kSpace2, itemHeight)
                        .radius(kRadiusMd)
                        .color(active ? dark().accentSurface
                                      : (dark().menu))
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
    const float pad = kSpace3;
    const float titleHeight = kFontSm + kSpace1;
    const float rowHeight = kFontBase + kSpace2;
    const float valueX = 96.0f;

    ui.stack("intrinsics")
        .position(x, y)
        .size(width, height)
        .content([&] {
            ui.rect("intrinsics.card")
                .size(width, height)
                .radius(kRadiusXl)
                .color(dark().card)
                .border(kBorderHairline, dark().cardBorder)
                .build();
            ui.text("intrinsics.title")
                .position(pad, pad)
                .size(width - pad * 2.0f, titleHeight)
                .text("Intrinsics")
                .fontSize(kFontSm)
                .fontWeight(kWeightSemibold)
                .color(dark().fgSubtle)
                .build();

            const rin::StreamIntrinsics streams[] = {
                ctx.hasIntrinsics ? ctx.intrinsics.color : rin::StreamIntrinsics{},
                ctx.hasIntrinsics ? ctx.intrinsics.depth : rin::StreamIntrinsics{},
            };
            const char* names[] = {"Color", "Depth"};
            const char* ids[] = {"color", "depth"};
            for (int index = 0; index < 2; ++index) {
                const float rowY =
                    pad + titleHeight + kSpace2 + static_cast<float>(index) * rowHeight;
                ui.text(std::string("intrinsics.") + ids[index] + ".name")
                    .position(pad, rowY)
                    .size(valueX - pad - kSpace2, rowHeight)
                    .text(names[index])
                    .fontSize(kFontCaption)
                    .fontWeight(kWeightMedium)
                    .color(dark().fgSubtle)
                    .build();
                ui.text(std::string("intrinsics.") + ids[index] + ".values")
                    .position(valueX, rowY)
                    .size(width - valueX - pad, rowHeight)
                    .text(formatIntrinsicsValues(streams[index]))
                    .fontSize(kFontBase)
                    .color(dark().fg)
                    .build();
                if (streams[index].valid()) {
                    ui.text(std::string("intrinsics.") + ids[index] + ".model")
                        .position(width - pad - 130.0f, rowY)
                        .size(130.0f, rowHeight)
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

void compose(eui::Ui& ui, const eui::Screen& screen) {
    ViewerContext& ctx = context();
    ctx.pump();

    const float pad = kSpace4;  // 16px：标准卡/面板内边距
    const float contentWidth = screen.width - pad * 2.0f;
    // 固定预算：头部 + 控制行 + 内参卡 + 间隙（紧凑操作型布局，宁密勿松）。
    const float headerHeight = 24.0f;
    const float controlsHeight = 38.0f;
    const float intrinsicsHeight = 96.0f;
    const float viewsTop = pad + headerHeight + kSpace3 + controlsHeight + kSpace3;
    const float viewsHeight = std::max(160.0f, screen.height - viewsTop - kSpace3 -
                                                   intrinsicsHeight - pad);
    // 三卡并列：RGB / Depth / Pose 3D（M3-06 集成位；最终卡位与 IMU 面板属 M3-07）。
    const float viewWidth = (contentWidth - kSpace3 * 2.0f) / 3.0f;
    // 右锚定控件组（响应式收敛）：Palette/Resolution 期望右对齐，空间不足时依次
    // 贴靠 Device 选择器（右缘 286 + 12 间距），逻辑宽 < 640 为 narrow（标签让位）。
    const float paletteX = std::max(298.0f, contentWidth - 342.0f);
    const float resolutionX = std::max(paletteX + 162.0f, contentWidth - 180.0f);
    const bool narrowControls = contentWidth < 640.0f;

    // 根用 stack 绝对布局：下拉触发器必须晚于视图/内参卡合成（绘制顺序即层叠顺序，
    // EUI 的 zIndex 不跨父容器），否则弹层被后绘制的兄弟卡片覆盖。
    ui.stack("root")
        .size(screen.width, screen.height)
        .onFrame([](float) { context().pump(); })
        .content([&] {
            composeHeader(ui, ctx, contentWidth);
            composeControls(ui, ctx, narrowControls, paletteX, resolutionX);
            ui.row("views")
                .position(pad, viewsTop)
                .size(contentWidth, viewsHeight)
                .gap(kSpace3)
                .content([&] {
                    composeViewCard(ui, "view.rgb", viewWidth, viewsHeight, "RGB", ctx.rgbMeta,
                                    ctx.rgbView);
                    composeViewCard(ui, "view.depth", viewWidth, viewsHeight, "Depth",
                                    ctx.depthMeta, ctx.depthView);
                    composePoseViewCard(ui, ctx.poseView,
                                        ctx.hasIntrinsics ? &ctx.intrinsics : nullptr,
                                        viewWidth, viewsHeight);
                })
                .build();
            composeIntrinsicsCard(ui, ctx, contentWidth, intrinsicsHeight, pad,
                                  screen.height - pad - intrinsicsHeight);

            // overlay 层（最后合成 = 浮于卡片之上）：设备选择 + 深度配色 + 分辨率。
            std::vector<std::string> deviceLabels;
            deviceLabels.reserve(ctx.deviceOptions.size());
            for (const DeviceUiOption& option : ctx.deviceOptions) {
                deviceLabels.push_back(option.label);
            }
            if (!deviceLabels.empty()) {
                composeSelect(ui, "controls.device", 76.0f, 52.0f, 210.0f,
                              "select device", deviceLabels, ctx.deviceIndex.get(),
                              ctx.deviceOpen.get(), [&] { ctx.deviceOpen.set(!ctx.deviceOpen.get()); },
                              [&ctx](int index) {
                                  ctx.deviceOpen.set(false);
                                  ctx.deviceIndex.set(index);
                                  ctx.applyDeviceChoice(index);
                              });
            }
            // 深度配色（DEC-007）：不依赖设备目录，Waiting 态可预设。
            composeSelect(ui, "controls.palette", paletteX, 52.0f, 150.0f,
                          "select", {"Jet", "Grayscale"}, ctx.paletteIndex.get(),
                          ctx.paletteOpen.get(),
                          [&] { ctx.paletteOpen.set(!ctx.paletteOpen.get()); },
                          [&ctx](int index) {
                              ctx.paletteOpen.set(false);
                              ctx.paletteIndex.set(index);
                              ctx.applyPaletteChoice(index);
                          });
            if (!ctx.resolutionOptions.empty()) {
                std::vector<std::string> resolutionLabels;
                resolutionLabels.reserve(ctx.resolutionOptions.size());
                for (const ResolutionUiOption& option : ctx.resolutionOptions) {
                    resolutionLabels.push_back(option.label);
                }
                composeSelect(ui, "controls.resolution", resolutionX, 52.0f, 180.0f,
                              "select", resolutionLabels, ctx.resolutionIndex.get(),
                              ctx.resolutionOpen.get(),
                              [&] { ctx.resolutionOpen.set(!ctx.resolutionOpen.get()); },
                              [&ctx](int index) {
                                  ctx.resolutionOpen.set(false);
                                  ctx.resolutionIndex.set(index);
                                  ctx.applyResolutionChoice(index);
                              });
            }
        })
        .build();
}

}  // namespace
}  // namespace viewer

namespace app {

const DslAppConfig& dslAppConfig() {
    viewer::ensureStarted();
    static const DslAppConfig config =
        DslAppConfig{}
            .title("Rin")
            .pageId("Rin")
            .clearColor(viewer::theme::dark().background)
            .windowSize(1280, 860)
            .fps(60.0)
            .onKeyEvent([](const eui::KeyEvent& event) {
                if (!event.isDown()) {
                    return;
                }
                const int digit =
                    static_cast<int>(event.key) - static_cast<int>(eui::InputKey::Digit1) + 1;
                if (digit >= 1 && digit <= 9) {
                    viewer::context().applyResolutionChoice(digit - 1);
                }
            })
            .onShutdown([] { viewer::context().shutdown(); });
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) { viewer::compose(ui, screen); }

}  // namespace app
