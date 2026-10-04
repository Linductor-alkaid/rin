# 分层构建：rin_core（公开契约零第三方类型，RULE-01；kissfft 为实现私有链接，
# DEC-012/M4-06）→ rin_realsense_adapter（kairo + librealsense2）。

add_library(rin_core STATIC
    core/camera_types.cpp
    core/camera_state_machine.cpp
    core/imu_fuser.cpp
    core/pose_math.cpp
    core/pixel_format.cpp
    core/workflow_types.cpp
    core/image_types.cpp
    core/image_node.cpp
    core/image_ops.cpp
    core/depth_preproc.cpp
    core/node_graph.cpp
)
add_library(rin::core ALIAS rin_core)

target_include_directories(rin_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(rin_core PUBLIC cxx_std_20)
# FFT 后端（DEC-012）：调用面收敛在 core/image_ops.cpp 单一实现内，私有链接不进
# 公开头；STATIC 库的 PRIVATE 依赖以 LINK_ONLY 传递给消费者链接，不外泄头文件。
target_link_libraries(rin_core PRIVATE kissfft::kissfft)

# M4-07 工作流真引擎（DEC-013 执行模型）：按 M4-09 契约实现 IWorkflowEngine，
# 帧泵采样 + 最新帧快照 + 有界在飞显式丢弃；经 runNodeGraph 求值真实算子。
# M5-08 契约假引擎（rin_workflow_fake）已完成历史使命（M5-03..05 骨架先行调试，
# DEC-016），随 M5-06 假换真集成移除。零设备/窗口依赖。
add_library(rin_workflow STATIC
    workflow/engine.cpp
)
add_library(rin::workflow ALIAS rin_workflow)

target_include_directories(rin_workflow
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/workflow>
)
target_compile_features(rin_workflow PUBLIC cxx_std_20)
# kairo 作为运行时契约 PUBLIC 传递（与 adapter 同纪律：引擎任务全部受 Executor 管理）。
target_link_libraries(rin_workflow PUBLIC rin::core kairo::kairo)

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
# kairo 作为运行时契约 PUBLIC 传递（viewer 持有 Executor 实例）；设备 SDK 保持 PRIVATE。
target_link_libraries(rin_realsense_adapter
    PUBLIC rin::core kairo::kairo
    PRIVATE realsense2::realsense2
)
