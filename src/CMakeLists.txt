# 分层构建：rin_core（零第三方依赖）→ rin_realsense_adapter（executor + librealsense2）。

add_library(rin_core STATIC
    core/camera_types.cpp
    core/camera_state_machine.cpp
    core/imu_fuser.cpp
    core/pose_math.cpp
    core/pixel_format.cpp
    core/workflow_types.cpp
)
add_library(rin::core ALIAS rin_core)

target_include_directories(rin_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(rin_core PUBLIC cxx_std_20)

# M5-08 契约假引擎（DEC-016）：IWorkflowEngine 的仿真实现，M5 骨架（M5-03..05）
# 对它先行开发调试；位于 Core 契约与 Executor 之间，零设备/窗口依赖；M5-06
# 假换真集成后移除。仿真统计仅为 UI 调试数据，不代表性能结论。
add_library(rin_workflow_fake STATIC
    workflow/fake_engine.cpp
)
add_library(rin::workflow_fake ALIAS rin_workflow_fake)

target_include_directories(rin_workflow_fake
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/workflow>
)
target_compile_features(rin_workflow_fake PUBLIC cxx_std_20)
# executor 作为运行时契约 PUBLIC 传递（与 adapter 同纪律：引擎任务全部受 Executor 管理）。
target_link_libraries(rin_workflow_fake PUBLIC rin::core executor::executor)

add_library(rin_realsense_adapter STATIC
    adapters/realsense/realsense_camera_service.cpp
    adapters/realsense/imu_motion_ingest.cpp
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
