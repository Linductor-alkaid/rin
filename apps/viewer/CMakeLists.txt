# rsv_viewer：EUI-NEO 前端应用。eui_neo_configure_app 注入框架入口与运行资源。

add_executable(rsv_viewer app.cpp)

target_link_libraries(rsv_viewer PRIVATE rsv::realsense_adapter)

eui_neo_configure_app(rsv_viewer)
