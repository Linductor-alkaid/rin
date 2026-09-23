# rin：EUI-NEO 前端应用。eui_neo_configure_app 注入框架入口与运行资源。

add_executable(rin app.cpp)

target_link_libraries(rin PRIVATE rin::realsense_adapter OpenGL::GL)

eui_neo_configure_app(rin)

# EUI-NEO 对 app 目标统一施加 -fno-exceptions/-fno-rtti（非 Debug 配置，框架宿主策略）；
# 但 app.cpp 内联 Executor 的异常式公开 API（task_cancellation.hpp 会抛出取消异常），
# 本编译单元必须启用异常（EUI-20260923-004，台账有对应登记）。
target_compile_options(rin PRIVATE -fexceptions -frtti)
