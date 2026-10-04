// Synthetic ImageStream probe: replicates Rin's display path
// (ui.image().stream(std::make_shared<ImageStream>(2)).fit(Contain),
// RGBA8 submit every pump) with a fully synthetic animated pattern.
// If the on-screen pattern does not follow the moving gradient while the
// seq counter advances, the pinned eui-neo build exhibits issue #71
// independently of camera content.
#include <eui_neo.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr int kW = 640;
constexpr int kH = 360;

std::shared_ptr<eui::ImageStream> g_stream = std::make_shared<eui::ImageStream>(2);
std::vector<std::uint8_t> g_pixels(static_cast<std::size_t>(kW) * kH * 4);
std::uint64_t g_seq = 0;

void renderPattern(double seconds) {
    const double phase = seconds * 120.0;  // 120 px/s horizontal sweep
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const std::size_t o = (static_cast<std::size_t>(y) * kW + x) * 4;
            const int band = (x + static_cast<int>(phase)) % 160;
            const bool stripe = band < 80;
            g_pixels[o + 0] = static_cast<std::uint8_t>(stripe ? 40 + (x * 215) / kW : (y * 255) / kH);
            g_pixels[o + 1] = static_cast<std::uint8_t>(stripe ? 220 : 40);
            g_pixels[o + 2] = static_cast<std::uint8_t>((x * 255) / kW);
            g_pixels[o + 3] = 255;
            if (x < 4) {  // fixed reference column, always bright red
                g_pixels[o + 0] = 255;
                g_pixels[o + 1] = 0;
                g_pixels[o + 2] = 0;
            }
        }
    }
}

}  // namespace

const app::DslAppConfig& app::dslAppConfig() {
    static const app::DslAppConfig config =
        app::DslAppConfig{}
            .title("ISPROBE")
            .pageId("isprobe")
            .appId("isprobe")
            .clearColor({0.05f, 0.05f, 0.05f, 1.0f})
            .windowSize(900, 640)
            .fps(60.0);
    return config;
}

void app::compose(eui::Ui& ui, const eui::Screen& screen) {
    const double seconds = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   std::chrono::steady_clock::now().time_since_epoch())
                                                   .count()) /
                           1000.0;
    renderPattern(seconds);
    auto pixels = std::make_shared<const std::vector<std::uint8_t>>(g_pixels);
    const eui::ImageFrame frame{pixels, kW, kH, kW * 4, eui::ImagePixelFormat::RGBA8,
                                ++g_seq};
    const bool submitted = g_stream->submit(frame);
    ui.stack("probe.root")
        .size(screen.width, screen.height)
        .content([&] {
            ui.text("probe.seq")
                .position(12.0f, 8.0f)
                .size(300.0f, 20.0f)
                .text("submitted seq " + std::to_string(g_seq) +
                      (submitted ? "" : " (submit REJECTED)"))
                .fontSize(14.0f)
                .color({0.9f, 0.9f, 0.9f, 1.0f})
                .build();
            ui.image("probe.img")
                .position(12.0f, 40.0f)
                .size(screen.width - 24.0f, screen.height - 52.0f)
                .stream(g_stream)
                .fit(eui::ImageFit::Contain)
                .build();
        })
        .build();
    app::requestUpdate();  // damage every frame so compose keeps running
}
