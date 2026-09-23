# deb 打包（DEC-009）：自包含分发 —— 可执行文件 + 捆绑的 librealsense2 运行库
# + udev 规则（设备免 root 可用）+ 桌面入口与图标。
#
# 组件模型：本项目安装规则全部归属 `rin` 组件；依赖（executor / eui-neo / librealsense2）
# 的 install 规则在 Dependencies.cmake 中被归入 deps-<name> 组件。CPack 只打包 rin 组件，
# 依赖的启发性安装规则不会进入 deb。仅 Linux 目标提供本打包层。

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()

if(NOT TARGET rin)
    return()
endif()

set(RIN_PACKAGE_NAME "rin")
set(RIN_PACKAGE_VERSION "${PROJECT_VERSION}")
set(RIN_PACKAGE_MAINTAINER "Linductor-alkaid <202200171251@mail.sdu.edu.cn>")
set(RIN_PACKAGE_HOMEPAGE "https://github.com/Linductor-alkaid/rin")

install(TARGETS rin
    RUNTIME
    DESTINATION bin
    COMPONENT rin
)
# 私有运行库目录：$ORIGIN 定位随包捆绑的 librealsense2，不与系统安装冲突。
set_target_properties(rin PROPERTIES
    INSTALL_RPATH "$ORIGIN/../lib/rin"
)

# 捆绑 librealsense2 运行库（NAMELINK_SKIP：不带开发用 namelink 符号链接）。
# 仅源码构建（external 锁定）产生可安装目标；若改回 system 类依赖则跳过并由
# deb Depends 声明系统 librealsense2。
if(TARGET realsense2)
    get_target_property(_rin_rs2_imported realsense2 IMPORTED)
    if(NOT _rin_rs2_imported)
        install(TARGETS realsense2
            LIBRARY
            DESTINATION lib/rin
            NAMELINK_SKIP
            COMPONENT rin
        )
    endif()
endif()

# RealSense 设备的 udev 规则（来自 pinned librealsense 源码），免 root 访问设备。
if(RIN_REALSENSE2_SOURCE_DIR AND EXISTS "${RIN_REALSENSE2_SOURCE_DIR}/config/99-realsense-libusb.rules")
    install(FILES "${RIN_REALSENSE2_SOURCE_DIR}/config/99-realsense-libusb.rules"
        DESTINATION lib/udev/rules.d
        COMPONENT rin
        RENAME 99-realsense-libusb.rules
    )
endif()

install(FILES "${CMAKE_CURRENT_LIST_DIR}/../packaging/linux/rin.desktop"
    DESTINATION share/applications
    COMPONENT rin
)
install(FILES "${CMAKE_CURRENT_LIST_DIR}/../docs/rin.png"
    DESTINATION share/icons/hicolor/512x512/apps
    RENAME rin.png
    COMPONENT rin
)

set(CPACK_PACKAGE_NAME "${RIN_PACKAGE_NAME}")
set(CPACK_PACKAGE_VERSION "${RIN_PACKAGE_VERSION}")
set(CPACK_PACKAGE_VENDOR "Linductor-alkaid")
set(CPACK_PACKAGE_CONTACT "${RIN_PACKAGE_MAINTAINER}")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${RIN_PACKAGE_MAINTAINER}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Rin — real-time Intel RealSense RGB/depth preview")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_GENERATOR "DEB")
set(CPACK_OUTPUT_FILE_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/dist")
set(CPACK_COMPONENTS_ALL "rin")
set(CPACK_DEB_COMPONENT_INSTALL ON)

# 组件化 deb 的元数据（前缀 = CPACK_DEBIAN_<组件大写>_）。
set(CPACK_DEBIAN_RIN_PACKAGE_NAME "${RIN_PACKAGE_NAME}")
set(CPACK_DEBIAN_RIN_PACKAGE_MAINTAINER "${RIN_PACKAGE_MAINTAINER}")
set(CPACK_DEBIAN_RIN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_RIN_PACKAGE_HOMEPAGE "${RIN_PACKAGE_HOMEPAGE}")
set(CPACK_DEBIAN_RIN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_RIN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_RIN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_LIST_DIR}/../packaging/linux/deb/postinst")
# dpkg 架构名与 CMake 处理器名不同（x86_64 → amd64）；其余架构按需补充映射。
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    set(_rin_dpkg_arch "amd64")
else()
    set(_rin_dpkg_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(CPACK_DEBIAN_RIN_FILE_NAME
    "${RIN_PACKAGE_NAME}_${RIN_PACKAGE_VERSION}_${_rin_dpkg_arch}.deb")

include(CPack)
