// rsv_viewer：EUI-NEO 前端。Executor 生命周期 owner 为 viewer::Context（DEC-002）：
// dslAppConfig() 首调点惰性启动，DslAppConfig::onShutdown（主线程、GPU 销毁前）关闭。
// 视觉层遵循 viewer_theme.hpp 的语义令牌翻译（DEC-005）：布局代码不出现一次性
// 颜色/字号/圆角，层级靠文本层级与背景对比表达，阴影仅用于浮层。

#include "gpu_frame_view.hpp"
#include "viewer_theme.hpp"

#include <eui_neo.h>

#include <components/dropdown.h>

#include <executor/executor.hpp>

#include <realsense_camera_service.hpp>

#include <rs_vision/camera_service.hpp>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace viewer {
namespace {

using namespace viewer::theme;
using rsv::FrameKind;
using rsv::StreamRequest;

/// 默认流请求（DEC-004 暂定默认值）。
constexpr StreamRequest kDefaultRequest{};

struct ResolutionUiOption {
    StreamRequest request;
    std::string label;
};

struct ViewerContext {
    executor::Executor executor;
    std::shared_ptr<rsv::ICameraService> service;
    GpuFrameView rgbView;
    GpuFrameView depthView;

    std::uint64_t lastRgbSequence = 0;
    std::uint64_t lastDepthSequence = 0;
    std::uint64_t lastIntrinsicsSequence = 0;
    std::uint64_t lastCapabilitiesSequence = 0;

    rsv::StreamCapabilities capabilities;
    bool hasCapabilities = false;
    rsv::IntrinsicsSnapshot intrinsics;
    bool hasIntrinsics = false;

    std::vector<ResolutionUiOption> resolutionOptions;
    eui::Signal<int> resolutionIndex{0};
    eui::Signal<bool> resolutionOpen{false};

    rsv::CameraServiceState statusState = rsv::CameraServiceState::Idle;
    std::string statusMessage;
    std::string startError;
    std::string rgbMeta;
    std::string depthMeta;

    bool started = false;
    bool shutdownDone = false;

    void applyResolutionChoice(int index);
    void applyKeyboardSelection(int digit);
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
        ctx.service = rsv::createRealSenseCameraService(ctx.executor);
        const rsv::StartOutcome outcome = ctx.service->start(kDefaultRequest);
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

const char* distortionName(rsv::DistortionModel model) {
    switch (model) {
        case rsv::DistortionModel::None:
            return "None";
        case rsv::DistortionModel::ModifiedBrownConrady:
            return "ModBrownConrady";
        case rsv::DistortionModel::InverseBrownConrady:
            return "InvBrownConrady";
        case rsv::DistortionModel::BrownConrady:
            return "BrownConrady";
        case rsv::DistortionModel::FTheta:
            return "FTheta";
        case rsv::DistortionModel::KannalaBrandt4:
            return "KannalaBrandt4";
        case rsv::DistortionModel::Unknown:
            return "Unknown";
    }
    return "Unknown";
}

/// 单流内参数值（等宽内容用统一小数位对齐；无打包等宽字体，见 DEC-005 限制）。
std::string formatIntrinsicsValues(const rsv::StreamIntrinsics& intrinsics) {
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

void ViewerContext::rebuildResolutionOptions() {
    std::vector<ResolutionUiOption> options;
    const auto byWidthHeight = [](const rsv::ResolutionOption& option) {
        return std::pair<std::uint32_t, std::uint32_t>{option.width, option.height};
    };
    std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
    for (const rsv::ResolutionOption& color : capabilities.colorOptions) {
        if (color.fps != kDefaultRequest.colorFps) {
            continue;  // M1：固定 30fps 档位
        }
        if (!seen.insert(byWidthHeight(color)).second) {
            continue;
        }
        bool depthMatch = false;
        for (const rsv::ResolutionOption& depth : capabilities.depthOptions) {
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
        options.push_back(std::move(option));
    }
    std::sort(options.begin(), options.end(),
              [](const ResolutionUiOption& lhs, const ResolutionUiOption& rhs) {
                  const auto areaOf = [](const ResolutionUiOption& option) {
                      return static_cast<std::uint64_t>(option.request.colorWidth) *
                             option.request.colorHeight;
                  };
                  return areaOf(lhs) < areaOf(rhs);
              });
    resolutionOptions = std::move(options);
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

void ViewerContext::applyKeyboardSelection(int digit) {
    // 键盘直达：1-9 选择第 N 个分辨率档位（keyboard-first，操作型 UI 一等输入路径）。
    const int index = digit - 1;
    if (index < 0 || index >= static_cast<int>(resolutionOptions.size())) {
        return;
    }
    resolutionIndex.set(index);
    applyResolutionChoice(index);
}

void ViewerContext::pump() {
    if (service == nullptr) {
        return;
    }

    bool frameUpdated = false;
    rsv::Frame frame;
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
    if (frameUpdated) {
        app::requestUpdate();  // 外部纹理非动画元素，需显式请求重绘
    }

    rsv::IntrinsicsSnapshot snapshot;
    if (service->tryLoadIntrinsics(lastIntrinsicsSequence, snapshot)) {
        intrinsics = std::move(snapshot);
        hasIntrinsics = true;
    }

    rsv::StreamCapabilities caps;
    if (service->tryLoadCapabilities(lastCapabilitiesSequence, caps)) {
        capabilities = std::move(caps);
        hasCapabilities = true;
        rebuildResolutionOptions();
        int defaultIndex = 0;
        for (std::size_t index = 0; index < resolutionOptions.size(); ++index) {
            const ResolutionUiOption& option = resolutionOptions[index];
            if (option.request.colorWidth == kDefaultRequest.colorWidth &&
                option.request.colorHeight == kDefaultRequest.colorHeight) {
                defaultIndex = static_cast<int>(index);
                break;
            }
        }
        resolutionIndex.set(defaultIndex);
    }

    rsv::ServiceEvent event;
    if (service->tryLoadEvent(event)) {
        statusState = event.state;
        statusMessage = event.message;
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
                .text("RealSense Vision")
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
                .text(rsv::toString(ctx.statusState))
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
/// 控制行标签与设备元数据（下拉触发器本体由 overlay 层最后合成，保证弹层浮顶）。
void composeControls(eui::Ui& ui, ViewerContext& ctx, float width) {
    ui.text("controls.label")
        .position(kSpace4, 65.0f)
        .text("Resolution")
        .fontSize(kFontSm)
        .fontWeight(kWeightMedium)
        .color(dark().fgSubtle)
        .build();
    if (!ctx.resolutionOptions.empty()) {
        ui.text("controls.none")
            .position(kSpace4 + 260.0f, 65.0f)
            .text("device options: " + std::to_string(ctx.resolutionOptions.size()))
            .fontSize(kFontXs)
            .color(dark().fgSubtlest)
            .build();
    } else {
        ui.text("controls.none")
            .position(kSpace4 + 260.0f, 65.0f)
            .text(ctx.hasCapabilities ? "no common RGB+depth option" : "detecting device")
            .fontSize(kFontXs)
            .color(dark().fgSubtlest)
            .build();
    }
    if (ctx.hasCapabilities && !ctx.capabilities.deviceName.empty()) {
        ui.text("controls.device")
            .position(width - 240.0f - kSpace4, 65.0f)
            .size(240.0f, kFontSm)
            .text(ctx.capabilities.deviceName)
            .fontSize(kFontXs)
            .color(dark().fgSubtlest)
            .horizontalAlign(eui::HorizontalAlign::Right)
            .build();
    }
}

/// 下拉触发器 + 弹层。作为根 stack 的最后一个兄弟合成（EUI 的 zIndex 不跨父容器，
/// 绘制顺序即层叠顺序），使弹层浮于视图卡与内参卡之上。
void composeResolutionDropdown(eui::Ui& ui, ViewerContext& ctx) {
    if (ctx.resolutionOptions.empty()) {
        return;
    }
    components::DropdownStyle dropdownStyle;
    dropdownStyle.field = dark().input;
    dropdownStyle.fieldHover = dark().input;
    dropdownStyle.fieldPressed = dark().input;
    dropdownStyle.popup = dark().menu;
    dropdownStyle.optionHover = dark().menuHover;
    dropdownStyle.optionPressed = dark().menuHover;
    dropdownStyle.selected = dark().accentSurface;
    dropdownStyle.text = dark().fg;
    dropdownStyle.mutedText = dark().fgSubtlest;
    dropdownStyle.accent = dark().brand;
    dropdownStyle.border = dark().inputBorder;
    dropdownStyle.radius = kRadiusLg;

    ui.stack("controls.dropdown.overlay")
        .position(kSpace4 + 131.0f, 52.0f)
        .size(200.0f, 38.0f)
        .content([&] {
    components::dropdown(ui, "controls.resolution")
        .size(200.0f, 38.0f)
        .items([&] {
            std::vector<std::string> labels;
            labels.reserve(ctx.resolutionOptions.size());
            for (const ResolutionUiOption& option : ctx.resolutionOptions) {
                labels.push_back(option.label);
            }
            return labels;
        }())
        .selected(ctx.resolutionIndex.get())
        .bindOpen(ctx.resolutionOpen)
        .style(dropdownStyle)
        .onOpenChange([&ctx](bool open) { ctx.resolutionOpen.set(open); })
        .onChange([&ctx](int value) {
            ctx.resolutionIndex.set(value);
            ctx.resolutionOpen.set(false);
            ctx.applyResolutionChoice(value);
        })
        .build();
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

            const rsv::StreamIntrinsics streams[] = {
                ctx.hasIntrinsics ? ctx.intrinsics.color : rsv::StreamIntrinsics{},
                ctx.hasIntrinsics ? ctx.intrinsics.depth : rsv::StreamIntrinsics{},
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
    const float viewWidth = (contentWidth - kSpace3) * 0.5f;

    // 根用 stack 绝对布局：下拉触发器必须晚于视图/内参卡合成（绘制顺序即层叠顺序，
    // EUI 的 zIndex 不跨父容器），否则弹层被后绘制的兄弟卡片覆盖。
    ui.stack("root")
        .size(screen.width, screen.height)
        .onFrame([](float) { context().pump(); })
        .content([&] {
            composeHeader(ui, ctx, contentWidth);
            composeControls(ui, ctx, contentWidth);
            ui.row("views")
                .position(pad, viewsTop)
                .size(contentWidth, viewsHeight)
                .gap(kSpace3)
                .content([&] {
                    composeViewCard(ui, "view.rgb", viewWidth, viewsHeight, "RGB", ctx.rgbMeta,
                                    ctx.rgbView);
                    composeViewCard(ui, "view.depth", viewWidth, viewsHeight, "Depth",
                                    ctx.depthMeta, ctx.depthView);
                })
                .build();
            composeIntrinsicsCard(ui, ctx, contentWidth, intrinsicsHeight, pad,
                          screen.height - pad - intrinsicsHeight);
            composeResolutionDropdown(ui, ctx);
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
            .title("RealSense Vision")
            .pageId("realsense_vision")
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
                    viewer::context().applyKeyboardSelection(digit);
                }
            })
            .onShutdown([] { viewer::context().shutdown(); });
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) { viewer::compose(ui, screen); }

}  // namespace app
