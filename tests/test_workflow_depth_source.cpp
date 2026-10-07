// M10 帧源米制路由与 Depth32F 缩略图独立验证（DEC-020 决策 4/5，
// Independent-Verification-Agent）：apps/viewer/workflow_frame_source.hpp 与
// apps/viewer/param_model.hpp 纯逻辑（无 EUI 类型，headless 可测）。
//
// 被测面与范围：
// 1) renditionForSourceType 六映射（RGB/伪彩/灰度/自适应/米制 + 未知回退 RGB）；
// 2) WorkflowSourceRouter：初态/未知节点回退 RGB、replace 整体换新对读方生效
//    （M10 仅验米制路由接线；四 rendition 完整路由归 test_run_control.cpp）；
// 3) makeCameraFrameSource 对 source_depth_metric 的读取（脚本化假相机服务）：
//    tryLoadDepthMetric "上次已见序号"过滤、DepthFrameF32 float 载荷 → Depth32F
//    字节容器（float 值逐元素精确）、sourceSequence = sample.sequence、水位
//    推进、无新帧 false 且出参不动、无效采样 false、服务指针失效 false；
// 4) thumbnailRgbaFromSnapshot Depth32F 分支（DEC-020 决策 5）：逐帧 P99（v>0
//    的 99 分位）为基准——v=0 → 黑、v<P99 → 255×(1−v/P99) round-half-up
//    （127.5→128 见证）、v ≥ P99 → 截断为 0、负值/无效同 0；全无效帧全黑；
//    snapshot 显式 stride（padding 哨兵）不泄漏；sourceSequence 透传；Gray8/
//    Rgba8 既有行为不回归（灰度复制/RGBA 直拷 + alpha）。
//
// golden 独立性：P99 由测试内独立排序推导；灰度映射按 DEC-007/DEC-020 公式
// 手推（0.25→223、0.5→191、1.0→128、1.5→64、≥P99→0，全部 float 精确运算）。
// 单线程纯逻辑：无跨上下文状态/关闭路径（DOD-02 并发矩阵不适用，见文件头）。
#include "test_util.hpp"

#include "fake_camera_service.hpp"

#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <rin/camera_service.hpp>
#include <rin/camera_types.hpp>
#include <rin/image_types.hpp>
#include <rin/workflow_types.hpp>

#include "engine.hpp"
#include "param_model.hpp"
#include "workflow_frame_source.hpp"

namespace {

using rin::DepthFrameF32;
using rin::DepthMetricSample;
using rin::Frame;
using rin::FrameKind;
using rin::GrayFrame;
using rin::GrayFrameKind;
using rin::ImageU8;
using rin::NodeOutputSnapshot;
using rin::PortType;
using viewer::WorkflowSourceRendition;
using viewer::WorkflowSourceRouter;

/// 脚本化假相机服务（M12/CR-34）：rin_test::FakeCameraService 唯一实现见
/// tests/fake_camera_service.hpp（米制单槽 + RGB 分槽 + rgbReads 计数；stop()
/// 后全部通道排空）。本文件以别名保持脚本面命名。
using rin_test::FakeCameraService;

DepthFrameF32 makeMetricFrame(const std::vector<float>& values, std::uint32_t width,
                              std::uint32_t height) {
    auto pixels = std::make_shared<const std::vector<float>>(values);
    return DepthFrameF32::wrap(width, height, width, std::move(pixels));
}

/// 米制深度快照（可指定字节 stride，padding 哨兵见证用）。
NodeOutputSnapshot makeDepthSnapshot(const std::vector<float>& values, std::uint32_t width,
                                     std::uint32_t height, std::uint32_t strideBytes,
                                     std::uint64_t sequence, std::uint8_t padSentinel = 0) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(strideBytes) * height, padSentinel);
    for (std::uint32_t y = 0; y < height; ++y) {
        std::memcpy(bytes.data() + static_cast<std::size_t>(strideBytes) * y,
                    values.data() + static_cast<std::size_t>(y) * width,
                    static_cast<std::size_t>(width) * 4u);
    }
    NodeOutputSnapshot snapshot;
    snapshot.node = 7;
    snapshot.format = PortType::Depth32F;
    snapshot.width = width;
    snapshot.height = height;
    snapshot.stride = strideBytes;
    snapshot.sourceSequence = sequence;
    snapshot.pixels = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
    return snapshot;
}

}  // namespace

int main() {
    // ---- 1) renditionForSourceType 六映射 + 未知回退 --------------------------
    {
        RIN_CHECK(viewer::renditionForSourceType("source") == WorkflowSourceRendition::RgbColor);
        RIN_CHECK(viewer::renditionForSourceType("source_depth_jet") ==
                  WorkflowSourceRendition::DepthJet);
        RIN_CHECK(viewer::renditionForSourceType("source_depth_gray") ==
                  WorkflowSourceRendition::DepthGray);
        RIN_CHECK(viewer::renditionForSourceType("source_depth_adaptive") ==
                  WorkflowSourceRendition::DepthAdaptiveGray);
        // M10：米制深度源映射 DepthMetric。
        RIN_CHECK(viewer::renditionForSourceType("source_depth_metric") ==
                  WorkflowSourceRendition::DepthMetric);
        RIN_CHECK(viewer::renditionForSourceType("source_depth_metric_typo") ==
                  WorkflowSourceRendition::RgbColor);
        RIN_CHECK(viewer::renditionForSourceType("source_depth") ==
                  WorkflowSourceRendition::RgbColor);
    }

    // ---- 2) WorkflowSourceRouter：回退与整体换新 ------------------------------
    {
        WorkflowSourceRouter router;
        RIN_CHECK(router.renditionFor(11) == WorkflowSourceRendition::RgbColor);  // 初态回退。
        {
            auto map = std::make_shared<WorkflowSourceRouter::RouteMap>();
            map->insert_or_assign(11, WorkflowSourceRendition::DepthMetric);
            router.replace(std::move(map));
        }
        RIN_CHECK(router.renditionFor(11) == WorkflowSourceRendition::DepthMetric);
        RIN_CHECK(router.renditionFor(12) == WorkflowSourceRendition::RgbColor);  // 表外回退。
        {
            auto map = std::make_shared<WorkflowSourceRouter::RouteMap>();
            map->insert_or_assign(11, WorkflowSourceRendition::DepthJet);
            router.replace(std::move(map));
        }
        RIN_CHECK(router.renditionFor(11) == WorkflowSourceRendition::DepthJet);  // 换新生效。
    }

    // ---- 3) makeCameraFrameSource：source_depth_metric 米制路由 ---------------
    {
        auto service = std::make_shared<FakeCameraService>();
        WorkflowSourceRouter router;
        {
            auto map = std::make_shared<WorkflowSourceRouter::RouteMap>();
            map->insert_or_assign(1, WorkflowSourceRendition::DepthMetric);
            map->insert_or_assign(2, WorkflowSourceRendition::RgbColor);
            router.replace(std::move(map));
        }
        rin::WorkflowFrameSource source = viewer::makeCameraFrameSource(service, router);

        // 服务指针失效：恒 false。
        {
            rin::WorkflowFrameSource nullSource = viewer::makeCameraFrameSource(nullptr, router);
            std::uint64_t watermark = 0;
            rin::WorkflowFrameInput out;
            RIN_CHECK(!nullSource(1, watermark, out));
            RIN_CHECK_EQ(watermark, std::uint64_t{0});
        }

        // 无帧：false 且出参/水位不动。
        {
            std::uint64_t watermark = 41;
            rin::WorkflowFrameInput out;
            RIN_CHECK(!source(1, watermark, out));
            RIN_CHECK_EQ(watermark, std::uint64_t{41});
            RIN_CHECK(!out.image.valid());
            RIN_CHECK_EQ(out.sourceSequence, std::uint64_t{0});
        }

        // 无效采样：false（帧源契约：包装失败按"无新帧"，不进入执行）。
        {
            service->publishMetric(DepthMetricSample{});  // frame 无效。
            std::uint64_t watermark = 0;
            rin::WorkflowFrameInput out;
            RIN_CHECK(!source(1, watermark, out));
        }

        // 发布米制帧（含无效 0 值混合）：float 逐元素精确 + 元数据。
        const std::vector<float> metricValues = {0.5f, 1.25f, 0.0f, 2.0f, 0.25f, 1.5f, 0.75f, 0.0f};
        {
            DepthMetricSample sample;
            sample.sequence = 5;
            sample.deviceTimestampMs = 33.0;
            sample.frame = makeMetricFrame(metricValues, 4, 2);
            service->publishMetric(std::move(sample));

            std::uint64_t watermark = 0;
            rin::WorkflowFrameInput out;
            RIN_CHECK(source(1, watermark, out));
            RIN_CHECK(out.image.valid());
            RIN_CHECK(out.image.format() == PortType::Depth32F);
            RIN_CHECK_EQ(out.image.width(), std::uint32_t{4});
            RIN_CHECK_EQ(out.image.height(), std::uint32_t{2});
            RIN_CHECK_EQ(out.image.stride(), std::uint32_t{16});  // 紧凑（width×4）。
            RIN_CHECK_EQ(out.sourceSequence, std::uint64_t{5});
            RIN_CHECK_EQ(watermark, std::uint64_t{5});  // 水位推进到本帧序号。
            for (std::uint32_t y = 0; y < 2; ++y) {
                const float* row = rin::depthF32Row(out.image, y);
                RIN_CHECK(row != nullptr);
                if (row == nullptr) {
                    continue;
                }
                for (std::uint32_t x = 0; x < 4; ++x) {
                    const float expected = metricValues[static_cast<std::size_t>(y) * 4 + x];
                    RIN_CHECK_MSG(
                        std::memcmp(&row[x], &expected, 4) == 0,
                        ("metric route: (" + std::to_string(x) + "," + std::to_string(y) + ")")
                            .c_str());
                }
            }

            // 同水位再读：无新帧 false（"上次已见序号"过滤）。
            std::uint64_t sameWatermark = 5;
            rin::WorkflowFrameInput stale;
            RIN_CHECK(!source(1, sameWatermark, stale));
            RIN_CHECK_EQ(sameWatermark, std::uint64_t{5});
        }

        // 更新序号：新值可读。
        {
            DepthMetricSample sample;
            sample.sequence = 6;
            sample.frame = makeMetricFrame(std::vector<float>(8, 2.5f), 4, 2);
            service->publishMetric(std::move(sample));
            std::uint64_t watermark = 5;
            rin::WorkflowFrameInput out;
            RIN_CHECK(source(1, watermark, out));
            RIN_CHECK_EQ(out.sourceSequence, std::uint64_t{6});
            RIN_CHECK_EQ(rin::depthF32Row(out.image, 0)[0], 2.5f);
            RIN_CHECK_EQ(watermark, std::uint64_t{6});
        }

        // 未知节点回退 RGB 路由（引擎覆盖检查不容忍源静默缺帧）：读 RGB 通道。
        {
            Frame frame;
            frame.kind = FrameKind::Rgb;
            frame.width = 2;
            frame.height = 1;
            frame.stride = 8;
            frame.sequence = 9;
            frame.pixels = std::make_shared<const std::vector<std::uint8_t>>(
                std::vector<std::uint8_t>{1, 2, 3, 255, 5, 6, 7, 255});
            service->publish(std::move(frame));

            const int readsBefore = service->rgbReads();
            std::uint64_t watermark = 0;
            rin::WorkflowFrameInput out;
            RIN_CHECK(source(99, watermark, out));  // 表外节点 → RGB。
            RIN_CHECK(service->rgbReads() > readsBefore);
            RIN_CHECK(out.image.format() == PortType::Rgba8);
            RIN_CHECK_EQ(out.sourceSequence, std::uint64_t{9});
        }

        // stop 排空：返回后米制通道不再有新发布。
        {
            service->stop();
            std::uint64_t watermark = 0;
            rin::WorkflowFrameInput out;
            RIN_CHECK(!source(1, watermark, out));
        }
    }

    // ---- 4) thumbnailRgbaFromSnapshot：Depth32F 分支 --------------------------
    {
        // 4×2 快照：v = [0, 0.5, 1.0, 1.5 / 2.0, -1.0, 2.0, 0.25]。
        // 有效集（v>0）= {0.25, 0.5, 1.0, 1.5, 2.0, 2.0}，n=6，idx = 6·99/100 =
        // 5 → P99 = 排序第 6 = 2.0。手推（255×(1−v/2) + 0.5 截断）：
        //   0 → 0（无效黑）   0.25 → 223   0.5 → 191   1.0 → 128（127.5 半值
        //   round-half-up 见证）   1.5 → 64   2.0 → 0（≥P99 截断）  -1.0 → 0。
        const std::vector<float> values = {0.0f, 0.5f, 1.0f, 1.5f, 2.0f, -1.0f, 2.0f, 0.25f};
        const std::uint32_t strideBytes = 5u * 4u;  // 显式 stride（padding 哨兵）。
        NodeOutputSnapshot snapshot = makeDepthSnapshot(values, 4, 2, strideBytes, 77, 0xCC);
        RIN_CHECK(snapshot.valid());

        Frame thumb;
        RIN_CHECK(viewer::thumbnailRgbaFromSnapshot(snapshot, 256, thumb));
        RIN_CHECK(thumb.valid());
        RIN_CHECK_EQ(thumb.width, std::uint32_t{4});  // 只缩不放：小图原样。
        RIN_CHECK_EQ(thumb.height, std::uint32_t{2});
        RIN_CHECK_EQ(thumb.stride, std::uint32_t{16});
        RIN_CHECK_EQ(thumb.sequence, std::uint64_t{77});  // sourceSequence 透传。
        RIN_CHECK_EQ(thumb.kind, FrameKind::Rgb);

        const std::uint8_t expected[8] = {0, 191, 128, 64, 0, 0, 0, 223};
        const std::vector<std::uint8_t>& pixels = *thumb.pixels;
        for (std::uint32_t i = 0; i < 8; ++i) {
            const std::uint8_t* px = pixels.data() + std::size_t{i} * 4u;
            RIN_CHECK_MSG(px[0] == expected[i] && px[1] == expected[i] && px[2] == expected[i],
                          ("depth thumb: pixel " + std::to_string(i) + " = " +
                           std::to_string(px[0]) + ", expected " + std::to_string(expected[i]))
                              .c_str());
            RIN_CHECK_EQ(px[3], std::uint8_t{255});  // 不透明。
        }

        // 全无效帧（全 0）：P99 无有效像素 → 全黑。
        {
            NodeOutputSnapshot invalid =
                makeDepthSnapshot(std::vector<float>(8, 0.0f), 4, 2, 4u * 4u, 3);
            Frame black;
            RIN_CHECK(viewer::thumbnailRgbaFromSnapshot(invalid, 256, black));
            const std::vector<std::uint8_t>& blackPixels = *black.pixels;
            bool allBlack = true;
            for (std::size_t i = 0; i < blackPixels.size(); i += 4) {
                if (blackPixels[i] != 0 || blackPixels[i + 3] != 255) {
                    allBlack = false;
                }
            }
            RIN_CHECK(allBlack);
        }

        // 下采样 + P99 全帧预扫描：8×2 → maxDim 4 → 4×1；P99 按整帧（含未采样
        // 行）计算；采样行 sourceY = y·H/thumbH = 0。两行同图案 [0,1,1.5,2,2,2,
        // 1,0.5]：有效集 14 个，idx = 14·99/100 = 13 → P99 = 2.0。采样源列
        // x*8/4 = {0,2,4,6} → v = {0, 1.5, 2, 1} → 灰度 = {0, 64, 0, 128}。
        {
            const std::vector<float> pattern = {0.0f, 1.0f, 1.5f, 2.0f, 2.0f, 2.0f, 1.0f, 0.5f};
            std::vector<float> tall;
            tall.reserve(16);
            tall.insert(tall.end(), pattern.begin(), pattern.end());
            tall.insert(tall.end(), pattern.begin(), pattern.end());
            NodeOutputSnapshot wide = makeDepthSnapshot(tall, 8, 2, 8u * 4u, 5);
            Frame small;
            RIN_CHECK(viewer::thumbnailRgbaFromSnapshot(wide, 4, small));
            RIN_CHECK_EQ(small.width, std::uint32_t{4});
            RIN_CHECK_EQ(small.height, std::uint32_t{1});
            const std::uint8_t expectedSmall[4] = {0, 64, 0, 128};
            for (std::uint32_t x = 0; x < 4; ++x) {
                RIN_CHECK_EQ((*small.pixels)[std::size_t{x} * 4u], expectedSmall[x]);
            }
        }

        // Gray8 既有行为不回归：亮度复制三通道 + alpha 255。
        {
            NodeOutputSnapshot gray;
            gray.node = 1;
            gray.format = PortType::Gray8;
            gray.width = 2;
            gray.height = 1;
            gray.stride = 2;
            gray.sourceSequence = 11;
            gray.pixels = std::make_shared<const std::vector<std::uint8_t>>(
                std::vector<std::uint8_t>{30, 200});
            Frame grayThumb;
            RIN_CHECK(viewer::thumbnailRgbaFromSnapshot(gray, 256, grayThumb));
            RIN_CHECK_EQ(grayThumb.sequence, std::uint64_t{11});
            RIN_CHECK_EQ((*grayThumb.pixels)[0], std::uint8_t{30});
            RIN_CHECK_EQ((*grayThumb.pixels)[1], std::uint8_t{30});
            RIN_CHECK_EQ((*grayThumb.pixels)[2], std::uint8_t{30});
            RIN_CHECK_EQ((*grayThumb.pixels)[3], std::uint8_t{255});
            RIN_CHECK_EQ((*grayThumb.pixels)[4], std::uint8_t{200});
        }

        // Rgba8 既有行为不回归：RGBA 直拷。
        {
            NodeOutputSnapshot rgba;
            rgba.node = 1;
            rgba.format = PortType::Rgba8;
            rgba.width = 1;
            rgba.height = 1;
            rgba.stride = 4;
            rgba.sourceSequence = 12;
            rgba.pixels = std::make_shared<const std::vector<std::uint8_t>>(
                std::vector<std::uint8_t>{10, 20, 30, 40});
            Frame rgbaThumb;
            RIN_CHECK(viewer::thumbnailRgbaFromSnapshot(rgba, 256, rgbaThumb));
            RIN_CHECK_EQ((*rgbaThumb.pixels)[0], std::uint8_t{10});
            RIN_CHECK_EQ((*rgbaThumb.pixels)[1], std::uint8_t{20});
            RIN_CHECK_EQ((*rgbaThumb.pixels)[2], std::uint8_t{30});
            RIN_CHECK_EQ((*rgbaThumb.pixels)[3], std::uint8_t{40});
        }

        // 无效快照 / maxDim = 0：false。
        {
            NodeOutputSnapshot invalid;
            Frame out;
            RIN_CHECK(!viewer::thumbnailRgbaFromSnapshot(invalid, 256, out));
            RIN_CHECK(!viewer::thumbnailRgbaFromSnapshot(snapshot, 0, out));
        }
    }

    return rin_test::exitStatus();
}
