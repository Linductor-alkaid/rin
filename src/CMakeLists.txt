# 分层构建：rin_core（零第三方依赖）→ rin_realsense_adapter（executor + librealsense2）。

add_library(rin_core STATIC
    core/camera_types.cpp
    core/camera_state_machine.cpp
    core/pixel_format.cpp
)
add_library(rin::core ALIAS rin_core)

target_include_directories(rin_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(rin_core PUBLIC cxx_std_20)

add_library(rin_realsense_adapter STATIC
    adapters/realsense/realsense_camera_service.cpp
)
add_library(rin::realsense_adapter ALIAS rin_realsense_adapter)

target_include_directories(rin_realsense_adapter
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/adapters/realsense>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/core
)
target_compile_features(rin_realsense_adapter PUBLIC cxx_std_20)
# executor 作为运行时契约 PUBLIC 传递（viewer 持有 Executor 实例）；设备 SDK 保持 PRIVATE。
target_link_libraries(rin_realsense_adapter
    PUBLIC rin::core executor::executor
    PRIVATE realsense2::realsense2
)
