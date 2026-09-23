#pragma once

#include <executor/executor.hpp>

#include "rin/camera_service.hpp"

namespace rin {

/// 工厂（实现在 adapter；viewer 只经工厂创建，不接触 librealsense 类型）。
/// executor 必须已 initialize；引用由服务持有，服务必须先于 executor 析构停止。
[[nodiscard]] std::shared_ptr<ICameraService> createRealSenseCameraService(
    executor::Executor& executor);

}  // namespace rin
