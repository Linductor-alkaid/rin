#pragma once

// 相机分辨率档位枚举纯逻辑（M13-01，立项依据 m13-fps-selection.md）：设备能力
// 目录 → 档位下拉选项。彩色与深度按 (宽, 高, fps) 三元组求交集配对（M1"双流
// 成对"契约，camera_types.hpp StreamRequest 注记：单 pipeline 不支持单流），
// 只暴露两路同时支持的档位；排序确定性（高度降序、宽度降序、帧率升序）供
// 下拉/浮层/源节点胶囊三处同源展示与测试断言。
//
// 本头文件不包含 EUI-NEO 类型（纯逻辑单测对象，tests/test_resolution_model.cpp）；
// EUI 组装与命令接线见 app.cpp（ViewerContext::rebuildResolutionOptions）。

#include <rin/camera_types.hpp>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace viewer {

/// 一个分辨率档位的 UI 载体：restream 请求（宽/高/fps 成对覆盖彩色与深度，
/// enableMotion 由调用方注入会话粘性）与下拉展示标签。
struct ResolutionUiOption {
    rin::StreamRequest request;
    std::string label;
};

/// 设备能力目录 → 档位选项列表（M13-01）。`motionEnabled` 注入每个档位的
/// `request.enableMotion`（会话粘性：kDefaultRequest 与 RIN_DISABLE_MOTION
/// 会话级禁用共同决定，见 app.cpp）。输出确定性排序：高度降序、宽度降序、
/// 帧率升序；同三元组只出现一次。
[[nodiscard]] inline std::vector<ResolutionUiOption> buildResolutionUiOptions(
    const rin::DeviceInfo& device, bool motionEnabled) {
    std::set<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> depthModes;
    for (const rin::ResolutionOption& depth : device.depthOptions) {
        depthModes.emplace(depth.width, depth.height, depth.fps);
    }

    std::vector<ResolutionUiOption> options;
    std::set<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> seen;
    for (const rin::ResolutionOption& color : device.colorOptions) {
        const auto key = std::tuple{color.width, color.height, color.fps};
        if (!seen.insert(key).second) {
            continue;
        }
        if (depthModes.find(key) == depthModes.end()) {
            continue;  // 深度无同 (宽, 高, fps) 档：不成对，不提供。
        }
        ResolutionUiOption option;
        option.request.colorWidth = color.width;
        option.request.colorHeight = color.height;
        option.request.colorFps = color.fps;
        option.request.depthWidth = color.width;
        option.request.depthHeight = color.height;
        option.request.depthFps = color.fps;
        option.request.enableMotion = motionEnabled;
        option.label = std::to_string(color.width) + "x" + std::to_string(color.height) +
                       " · " + std::to_string(color.fps) + "fps";
        options.push_back(std::move(option));
    }

    std::sort(options.begin(), options.end(),
              [](const ResolutionUiOption& lhs, const ResolutionUiOption& rhs) {
                  const auto lhsWH = std::tuple{lhs.request.colorHeight, lhs.request.colorWidth};
                  const auto rhsWH = std::tuple{rhs.request.colorHeight, rhs.request.colorWidth};
                  if (lhsWH != rhsWH) {
                      return lhsWH > rhsWH;  // 分辨率观感与既有菜单一致（大在上）。
                  }
                  return lhs.request.colorFps < rhs.request.colorFps;  // 同档位帧率升序。
              });
    return options;
}

}  // namespace viewer
