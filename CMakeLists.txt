cmake_minimum_required(VERSION 3.25)

project(realsense_vision
    VERSION 0.1.0
    DESCRIPTION "RealSense vision library with an executor-backed runtime and an EUI-NEO viewer"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(RSV_BUILD_TESTS "Build realsense-vision tests" ON)
option(RSV_BUILD_VIEWER "Build the EUI-NEO viewer application" ON)
option(RSV_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# 依赖构建裁剪：只引入库本体，不构建依赖的自测/示例/托盘。
set(EXECUTOR_BUILD_TESTS OFF)
set(EXECUTOR_BUILD_EXAMPLES OFF)
set(EUI_BUILD_APPS OFF)
set(EUI_BUILD_USER_APPS OFF)
set(EUI_BUILD_TEST_FIXTURES OFF)
set(EUI_ENABLE_TRAY OFF)

# 依赖解析与锁定校验（executor / eui-neo / librealsense2）。
# 需在定义目标前完成：EUI-NEO 的 eui_neo_configure_app() 依赖其配置阶段注册的入口源。
include(Dependencies)

find_package(Threads REQUIRED)
find_package(OpenGL REQUIRED)  # viewer 外部 GPU 图像绕行（EUI-20260923-003）需要 GL 上传

# Sanitizer 预设（工程规范 11：asan/ubsan 常规，tsan 按需）。
# 仅作用于本仓库目标（声明位于依赖引入之后），第三方静态库不被插桩。
if(RSV_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
endif()
if(RSV_ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=undefined)
endif()
if(RSV_ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
endif()

add_subdirectory(src)

if(RSV_BUILD_VIEWER)
    add_subdirectory(apps/viewer)
endif()

if(RSV_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
