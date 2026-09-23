# 第三方依赖解析与锁定校验（工程规范 9.1：lock 清单 + CMake 拉取）。
#
# 每个 external 依赖按顺序解析：
# 1. 若给定 RIN_<NAME>_SOURCE_DIR 且该目录 git HEAD 等于 pinned_commit，直接使用本地源码；
# 2. 否则 FetchContent 按 pinned_commit 拉取（FETCHCONTENT_FULLY_DISCONNECTED 时要求本地源目录）。
# class=system 依赖用 find_package 校验版本（expected_commit 字段承载最低版本号）。
#
# 离线构建：-DFETCHCONTENT_FULLY_DISCONNECTED=ON，并为每个 external 依赖提供本地源目录。

include(FetchContent)

set(RIN_DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/../third_party/dependencies.lock")

function(_rin_verify_commit source_dir expected_commit dep_name)
    if(NOT EXISTS "${source_dir}/.git")
        message(FATAL_ERROR "Rin deps: ${dep_name} 源目录不是 git 仓库: ${source_dir}")
    endif()
    execute_process(
        COMMAND git -C "${source_dir}" rev-parse HEAD
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _head
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "Rin deps: 无法读取 ${dep_name} HEAD (${source_dir})")
    endif()
    if(NOT _head STREQUAL expected_commit)
        message(FATAL_ERROR
            "Rin deps: ${dep_name} commit 不匹配锁清单。\n"
            "  期望: ${expected_commit}\n"
            "  实际: ${_head}\n"
            "  源目录: ${source_dir}")
    endif()
endfunction()

function(_rin_lock_field line index out_var)
    string(REPLACE "|" ";" fields "${line}")
    list(GET fields "${index}" value)
    set(${out_var} "${value}" PARENT_SCOPE)
endfunction()

# _rin_declare_external(name url pinned_commit)
function(_rin_declare_external dep_name dep_url dep_commit)
    string(TOUPPER "${dep_name}" dep_upper)
    string(REPLACE "-" "_" dep_upper "${dep_upper}")
    set(local_var "RIN_${dep_upper}_SOURCE_DIR")
    set(source_dir "")
    if(DEFINED ${local_var} AND EXISTS "${${local_var}}")
        set(source_dir "${${local_var}}")
        _rin_verify_commit("${source_dir}" "${dep_commit}" "${dep_name}")
        message(STATUS "Rin deps: ${dep_name} 使用本地源 ${source_dir} (commit 校验通过)")
        set("FETCHCONTENT_SOURCE_DIR_${dep_upper}" "${source_dir}")
    endif()

    if(FETCHCONTENT_FULLY_DISCONNECTED)
        message(FATAL_ERROR
            "Rin deps: 离线构建要求 -D${local_var}=<pinned 源目录>（${dep_name}）")
    endif()

    message(STATUS "Rin deps: ${dep_name} 按 pinned commit 拉取 ${dep_commit}")

    # librealsense2 源码构建裁剪（lrs_options.cmake 默认值面向完整上游发行）：
    # 只保留运行时库本体，禁用示例/工具/更新检查/录制与静态捆绑，控制构建面与体积。
    # 注意：librealsense 的 cmake_minimum_required 为 3.10（CMP0077 OLD），普通变量会被
    # 其 option() 覆盖，必须写 cache FORCE 才能生效；cache 是全局持久的，配置完成后必须
    # 恢复原值，否则会污染后续（或重配置时先行的）executor / eui-neo 构建面。
    if(dep_name STREQUAL "realsense2")
        foreach(_opt IN
                ITEMS BUILD_SHARED_LIBS BUILD_EXAMPLES BUILD_GRAPHICAL_EXAMPLES
                BUILD_GLSL_EXTENSIONS BUILD_TOOLS BUILD_UNIT_TESTS BUILD_PYTHON_BINDINGS
                BUILD_ROSBAG2 BUILD_RS2_ALL BUILD_WITH_DDS CHECK_FOR_UPDATES)
            if(DEFINED CACHE{${_opt}})
                set("_rin_saved_${_opt}" "${${_opt}}")
            else()
                set("_rin_saved_${_opt}" "")
                set("_rin_saved_${_opt}__undefined" TRUE)
            endif()
        endforeach()
        set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
        set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(BUILD_GRAPHICAL_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(BUILD_GLSL_EXTENSIONS OFF CACHE BOOL "" FORCE)
        set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
        set(BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
        set(BUILD_PYTHON_BINDINGS OFF CACHE BOOL "" FORCE)
        set(BUILD_ROSBAG2 OFF CACHE BOOL "" FORCE)
        set(BUILD_RS2_ALL OFF CACHE BOOL "" FORCE)
        set(BUILD_WITH_DDS OFF CACHE BOOL "" FORCE)
        set(CHECK_FOR_UPDATES OFF CACHE BOOL "" FORCE)
        # librealsense 2.58 rsutils 头使用类内构造函数冗余模板实参（`Q<T>(unsigned)`），
        # GCC13 C++20 下报错而 C++17 合法；源码构建域内固定 C++17。
        set(CMAKE_CXX_STANDARD 17)
    endif()

    # 依赖的 install 规则归属独立组件：deb 打包只安装 rin 组件（见 cmake/Packaging.cmake），
    # 依赖的头文件/静态库/cmake config 不进入安装面。
    set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME "deps-${dep_name}")
    FetchContent_Declare(${dep_name}
        GIT_REPOSITORY "${dep_url}"
        GIT_TAG "${dep_commit}"
    )
    FetchContent_MakeAvailable(${dep_name})
    set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME "rin")
    set(CMAKE_CXX_STANDARD 20)

    # 恢复 librealsense2 裁剪期间强制的 cache 值（见上方说明）。
    if(dep_name STREQUAL "realsense2")
        foreach(_opt IN
                ITEMS BUILD_SHARED_LIBS BUILD_EXAMPLES BUILD_GRAPHICAL_EXAMPLES
                BUILD_GLSL_EXTENSIONS BUILD_TOOLS BUILD_UNIT_TESTS BUILD_PYTHON_BINDINGS
                BUILD_ROSBAG2 BUILD_RS2_ALL BUILD_WITH_DDS CHECK_FOR_UPDATES)
            if("_rin_saved_${_opt}__undefined")
                unset("${_opt}" CACHE)
            elseif(DEFINED "_rin_saved_${_opt}")
                set("${_opt}" "${_rin_saved_${_opt}}" CACHE BOOL "" FORCE)
            endif()
            unset("_rin_saved_${_opt}")
            unset("_rin_saved_${_opt}__undefined")
        endforeach()
    endif()

    # librealsense 源码构建只定义 realsense2 目标（命名空间别名仅在其安装导出中存在）；
    # 项目代码统一链接 realsense2::realsense2，这里补齐别名。
    if(dep_name STREQUAL "realsense2")
        if(TARGET realsense2 AND NOT TARGET realsense2::realsense2)
            add_library(realsense2::realsense2 ALIAS realsense2)
        endif()
        set(RIN_REALSENSE2_SOURCE_DIR "${${dep_name}_SOURCE_DIR}" CACHE INTERNAL
            "librealsense2 pinned 源目录（udev 规则随包分发）")
    endif()
endfunction()

file(READ "${RIN_DEPENDENCIES_LOCK}" lock_lines)
string(REPLACE "\n" ";" lock_lines "${lock_lines}")
foreach(line IN LISTS lock_lines)
    if(line STREQUAL "" OR line MATCHES "^#")
        continue()
    endif()
    _rin_lock_field("${line}" 0 dep_name)
    _rin_lock_field("${line}" 1 dep_url)
    _rin_lock_field("${line}" 2 dep_commit)
    _rin_lock_field("${line}" 3 dep_expected)
    _rin_lock_field("${line}" 5 dep_class)
    if(dep_class STREQUAL "external")
        _rin_declare_external("${dep_name}" "${dep_url}" "${dep_commit}")
    elseif(dep_class STREQUAL "system")
        find_package(${dep_name} ${dep_expected} REQUIRED)
        message(STATUS "Rin deps: ${dep_name} 系统版本 ${dep_expected} 校验通过")
    else()
        message(FATAL_ERROR "Rin deps: 未知依赖类别 ${dep_class} (${dep_name})")
    endif()
endforeach()
