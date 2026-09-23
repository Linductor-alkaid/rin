// rsv_viewer：EUI-NEO 前端。Executor 生命周期 owner 为 viewer::Context（DEC-002）：
// dslAppConfig() 首调点惰性启动，DslAppConfig::onShutdown（主线程、GPU 销毁前）关闭。

#include "eui_neo.h"

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

using rsv::FrameKind;
using rsv::StreamRequest;

using eui::ImageStream;

/// 默认流请求（DEC-004 暂定默认值）。
constexpr StreamRequest kDefaultRequest{};

struct ResolutionUiOption {
    StreamRequest request;
    std::string label;
};

struct ViewerContext {
    executor::Executor executor;
    std::shared_ptr<rsv::ICameraService> service;
    std::shared_ptr<ImageStream> rgbStream = std::make_shared<ImageStream>(2);
    std::shared_ptr<ImageStream> depthStream = std::make_shared<ImageStream>(2);

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

    std::string statusText = "starting";
    std::string startError;

    bool started = false;
    bool shutdownDone = false;

    void applyResolutionChoice(int index);
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
            ctx.statusText = "executor init failed";
            return false;
        }
        ctx.service = rsv::createRealSenseCameraService(ctx.executor);
        const rsv::StartOutcome outcome = ctx.service->start(kDefaultRequest);
        if (!outcome.admitted) {
            ctx.startError = outcome.error;
            ctx.statusText = "start failed";
            return false;
        }
        ctx.started = true;
        ctx.statusText = "opening device";
        return true;
    }();
    (void)once;
}

void submitToStream(const std::shared_ptr<ImageStream>& stream, const rsv::Frame& frame) {
    if (!frame.valid()) {
        return;
    }
    const eui::ImageFrame image{frame.pixels, frame.width, frame.height, frame.stride,
                                eui::ImagePixelFormat::RGBA8, frame.sequence};
    (void)stream->submit(image);
}

std::string formatIntrinsics(const char* label, const rsv::StreamIntrinsics& intrinsics) {
    if (!intrinsics.valid()) {
        return std::string(label) + ": n/a";
    }
    const char* model = "?";
    switch (intrinsics.model) {
        case rsv::DistortionModel::None:
            model = "None";
            break;
        case rsv::DistortionModel::ModifiedBrownConrady:
            model = "ModBrownConrady";
            break;
        case rsv::DistortionModel::InverseBrownConrady:
            model = "InvBrownConrady";
            break;
        case rsv::DistortionModel::BrownConrady:
            model = "BrownConrady";
            break;
        case rsv::DistortionModel::FTheta:
            model = "FTheta";
            break;
        case rsv::DistortionModel::KannalaBrandt4:
            model = "KannalaBrandt4";
            break;
        case rsv::DistortionModel::Unknown:
            model = "Unknown";
            break;
    }
    char buffer[192];
    std::snprintf(buffer, sizeof(buffer),
                  "%s: %ux%u  fx=%.3f  fy=%.3f  cx=%.3f  cy=%.3f  [%s]", label,
                  intrinsics.width, intrinsics.height, static_cast<double>(intrinsics.fx),
                  static_cast<double>(intrinsics.fy), static_cast<double>(intrinsics.cx),
                  static_cast<double>(intrinsics.cy), model);
    return buffer;
}

void ViewerContext::rebuildResolutionOptions() {
    std::vector<ResolutionUiOption> options;
    const auto byHeightThenFps = [](const rsv::ResolutionOption& option) {
        return std::pair<std::uint32_t, std::uint32_t>{option.width, option.height};
    };
    std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
    for (const rsv::ResolutionOption& color : capabilities.colorOptions) {
        if (color.fps != kDefaultRequest.colorFps) {
            continue;  // M1：固定 30fps 档位
        }
        if (!seen.insert(byHeightThenFps(color)).second) {
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
        statusText = "resolution rejected: " + error;
    }
}

void ViewerContext::pump() {
    if (service == nullptr) {
        return;
    }

    rsv::Frame frame;
    if (service->tryLoadFrame(rsv::FrameKind::Rgb, lastRgbSequence, frame)) {
        submitToStream(rgbStream, frame);
    }
    if (service->tryLoadFrame(rsv::FrameKind::Depth, lastDepthSequence, frame)) {
        submitToStream(depthStream, frame);
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
        statusText = std::string(rsv::toString(event.state)) +
                     (event.message.empty() ? "" : (" - " + event.message));
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
    if (started) {
        (void)executor.shutdown(true);
    }
}

void composeViewPanel(eui::Ui& ui, const char* id, float width, float height,
                      const char* label, const std::shared_ptr<ImageStream>& stream) {
    ui.stack(id)
        .size(width, height)
        .content([&] {
            ui.rect(std::string(id) + ".bg")
                .size(width, height)
                .color({0.08f, 0.09f, 0.11f, 1.0f})
                .radius(10.0f)
                .build();
            ui.image(std::string(id) + ".img")
                .size(width, height)
                .stream(stream)
                .fit(eui::ImageFit::Contain)
                .build();
            ui.text(std::string(id) + ".label")
                .position(10.0f, 8.0f)
                .text(label)
                .fontSize(16.0f)
                .color({0.92f, 0.94f, 0.97f, 1.0f})
                .build();
        })
        .build();
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    ViewerContext& ctx = context();
    ctx.pump();

    const float padding = 24.0f;
    // 预留头部/控制/内参文本的固定高度，防止小窗口下内参面板被裁掉。
    const float viewsHeight = std::max(200.0f, screen.height - 350.0f);
    const float viewWidth = (screen.width - padding * 2.0f - 16.0f) * 0.5f;

    ui.column("root")
        .size(screen.width, screen.height)
        .padding(padding)
        .gap(12.0f)
        .onFrame([](float) { context().pump(); })
        .content([&] {
            ui.text("app.title")
                .text("RealSense Vision")
                .fontSize(24.0f)
                .build();
            ui.text("app.status")
                .text(ctx.statusText + (ctx.startError.empty() ? "" : (" | " + ctx.startError)))
                .fontSize(14.0f)
                .color({0.65f, 0.70f, 0.78f, 1.0f})
                .build();

            ui.row("views")
                .size(screen.width - padding * 2.0f, viewsHeight)
                .gap(16.0f)
                .content([&] {
                    composeViewPanel(ui, "view.rgb", viewWidth, viewsHeight, "RGB",
                                     ctx.rgbStream);
                    composeViewPanel(ui, "view.depth", viewWidth, viewsHeight, "Depth",
                                     ctx.depthStream);
                })
                .build();

            ui.row("controls")
                .size(screen.width - padding * 2.0f, 44.0f)
                .gap(12.0f)
                .content([&] {
                    ui.text("controls.label")
                        .text("Resolution")
                        .fontSize(15.0f)
                        .build();
                    if (!ctx.resolutionOptions.empty()) {
                        components::dropdown(ui, "controls.resolution")
                            .size(220.0f, 38.0f)
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
                            .onChange([&ctx](int value) {
                                ctx.resolutionIndex.set(value);
                                ctx.resolutionOpen.set(false);
                                ctx.applyResolutionChoice(value);
                            })
                            .build();
                    } else {
                        ui.text("controls.none")
                            .text(ctx.hasCapabilities ? "no common RGB+depth option"
                                                      : "detecting device")
                            .fontSize(14.0f)
                            .build();
                    }
                })
                .build();

            ui.text("intrinsics.color")
                .text(formatIntrinsics("Color",
                                       ctx.hasIntrinsics ? ctx.intrinsics.color
                                                         : rsv::StreamIntrinsics{}))
                .fontSize(15.0f)
                .build();
            ui.text("intrinsics.depth")
                .text(formatIntrinsics("Depth",
                                       ctx.hasIntrinsics ? ctx.intrinsics.depth
                                                         : rsv::StreamIntrinsics{}))
                .fontSize(15.0f)
                .build();
        })
        .build();
}

}  // namespace
}  // namespace viewer

namespace app {

const DslAppConfig& dslAppConfig() {
    viewer::ensureStarted();
    static const DslAppConfig config = DslAppConfig{}
                                           .title("RealSense Vision")
                                           .pageId("realsense_vision")
                                           .clearColor({0.05f, 0.06f, 0.08f, 1.0f})
                                           .windowSize(1280, 860)
                                           .fps(60.0)
                                           .onShutdown([] { viewer::context().shutdown(); });
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) { viewer::compose(ui, screen); }

}  // namespace app
