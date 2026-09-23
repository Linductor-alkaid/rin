# 分层构建：rsv_core（零第三方依赖）→ rsv_realsense_adapter（executor + librealsense2）。

add_library(rsv_core STATIC
    core/camera_types.cpp
    core/camera_state_machine.cpp
    core/pixel_format.cpp
)
add_library(rsv::core ALIAS rsv_core)

target_include_directories(rsv_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(rsv_core PUBLIC cxx_std_20)

add_library(rsv_realsense_adapter STATIC
    adapters/realsense/realsense_camera_service.cpp
)
add_library(rsv::realsense_adapter ALIAS rsv_realsense_adapter)

target_include_directories(rsv_realsense_adapter
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/adapters/realsense>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/core
)
target_compile_features(rsv_realsense_adapter PUBLIC cxx_std_20)
# executor 作为运行时契约 PUBLIC 传递（viewer 持有 Executor 实例）；设备 SDK 保持 PRIVATE。
target_link_libraries(rsv_realsense_adapter
    PUBLIC rsv::core executor::executor
    PRIVATE realsense2::realsense2
)
