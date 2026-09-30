# rin：EUI-NEO 前端应用。eui_neo_configure_app 注入框架入口与运行资源。

add_executable(rin app.cpp)

# 工作流页运行控制与真引擎集成（M5-06，DEC-013/DEC-016）：M4-07 真引擎 +
# 相机帧源接缝（workflow_frame_source.hpp）替代 M5-08 契约假引擎（已随本次
# 集成移除，见 src/CMakeLists）。
target_link_libraries(rin PRIVATE rin::realsense_adapter rin::workflow OpenGL::GL)

# 设置页"关于"展示的版本号（根 project(Rin VERSION ...)）。
target_compile_definitions(rin PRIVATE RIN_VERSION="${Rin_VERSION}")

eui_neo_configure_app(rin)

# EUI-NEO 对 app 目标统一施加 -fno-exceptions/-fno-rtti（非 Debug 配置，框架宿主策略）；
# 但 app.cpp 内联 Executor 的异常式公开 API（task_cancellation.hpp 会抛出取消异常），
# 本编译单元必须启用异常（EUI-20260923-004，台账有对应登记）。
target_compile_options(rin PRIVATE -fexceptions -frtti)
