# 第三方依赖解析与锁定校验（工程规范 9.1：lock 清单 + CMake 拉取）。
#
# 每个 external 依赖按顺序解析：
# 1. 若给定 RSV_<NAME>_SOURCE_DIR 且该目录 git HEAD 等于 pinned_commit，直接使用本地源码；
# 2. 否则 FetchContent 按 pinned_commit 拉取（FETCHCONTENT_FULLY_DISCONNECTED 时要求本地源目录）。
# class=system 依赖用 find_package 校验版本（expected_commit 字段承载最低版本号）。
#
# 离线构建：-DFETCHCONTENT_FULLY_DISCONNECTED=ON，并为每个 external 依赖提供本地源目录。

include(FetchContent)

set(RSV_DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/../third_party/dependencies.lock")

function(_rsv_verify_commit source_dir expected_commit dep_name)
    if(NOT EXISTS "${source_dir}/.git")
        message(FATAL_ERROR "realsense-vision deps: ${dep_name} 源目录不是 git 仓库: ${source_dir}")
    endif()
    execute_process(
        COMMAND git -C "${source_dir}" rev-parse HEAD
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _head
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "realsense-vision deps: 无法读取 ${dep_name} HEAD (${source_dir})")
    endif()
    if(NOT _head STREQUAL expected_commit)
        message(FATAL_ERROR
            "realsense-vision deps: ${dep_name} commit 不匹配锁清单。\n"
            "  期望: ${expected_commit}\n"
            "  实际: ${_head}\n"
            "  源目录: ${source_dir}")
    endif()
endfunction()

function(_rsv_lock_field line index out_var)
    string(REPLACE "|" ";" fields "${line}")
    list(GET fields "${index}" value)
    set(${out_var} "${value}" PARENT_SCOPE)
endfunction()

# _rsv_declare_external(name url pinned_commit)
function(_rsv_declare_external dep_name dep_url dep_commit)
    string(TOUPPER "${dep_name}" dep_upper)
    string(REPLACE "-" "_" dep_upper "${dep_upper}")
    set(local_var "RSV_${dep_upper}_SOURCE_DIR")
    set(source_dir "")
    if(DEFINED ${local_var} AND EXISTS "${${local_var}}")
        set(source_dir "${${local_var}}")
        _rsv_verify_commit("${source_dir}" "${dep_commit}" "${dep_name}")
        message(STATUS "realsense-vision deps: ${dep_name} 使用本地源 ${source_dir} (commit 校验通过)")
        set("FETCHCONTENT_SOURCE_DIR_${dep_upper}" "${source_dir}")
    endif()

    if(FETCHCONTENT_FULLY_DISCONNECTED)
        message(FATAL_ERROR
            "realsense-vision deps: 离线构建要求 -D${local_var}=<pinned 源目录>（${dep_name}）")
    endif()

    message(STATUS "realsense-vision deps: ${dep_name} 按 pinned commit 拉取 ${dep_commit}")
    FetchContent_Declare(${dep_name}
        GIT_REPOSITORY "${dep_url}"
        GIT_TAG "${dep_commit}"
    )
    FetchContent_MakeAvailable(${dep_name})
endfunction()

file(READ "${RSV_DEPENDENCIES_LOCK}" lock_lines)
string(REPLACE "\n" ";" lock_lines "${lock_lines}")
foreach(line IN LISTS lock_lines)
    if(line STREQUAL "" OR line MATCHES "^#")
        continue()
    endif()
    _rsv_lock_field("${line}" 0 dep_name)
    _rsv_lock_field("${line}" 1 dep_url)
    _rsv_lock_field("${line}" 2 dep_commit)
    _rsv_lock_field("${line}" 3 dep_expected)
    _rsv_lock_field("${line}" 5 dep_class)
    if(dep_class STREQUAL "external")
        _rsv_declare_external("${dep_name}" "${dep_url}" "${dep_commit}")
    elseif(dep_class STREQUAL "system")
        find_package(${dep_name} ${dep_expected} REQUIRED)
        message(STATUS "realsense-vision deps: ${dep_name} 系统版本 ${dep_expected} 校验通过")
    else()
        message(FATAL_ERROR "realsense-vision deps: 未知依赖类别 ${dep_class} (${dep_name})")
    endif()
endforeach()
