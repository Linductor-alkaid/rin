#pragma once

#include <chrono>

namespace rin::detail {

/// 单调毫秒时钟唯一实现（M12/CR-24）：事件时间戳 / 速率测量的共用取时。
[[nodiscard]] inline double steadyMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace rin::detail
