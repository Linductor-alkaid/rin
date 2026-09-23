# 单元测试。标签：unit；真机（hardware）用例由验收流程单独执行，不进默认 ctest。

add_executable(test_camera_state_machine test_camera_state_machine.cpp)
target_link_libraries(test_camera_state_machine PRIVATE rin::core)
target_include_directories(test_camera_state_machine PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
add_test(NAME camera_state_machine COMMAND test_camera_state_machine)
set_tests_properties(camera_state_machine PROPERTIES LABELS "unit")

add_executable(test_pixel_format test_pixel_format.cpp)
target_link_libraries(test_pixel_format PRIVATE rin::core)
target_include_directories(test_pixel_format PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
add_test(NAME pixel_format COMMAND test_pixel_format)
set_tests_properties(pixel_format PROPERTIES LABELS "unit")

# 公开头第三方类型边界（RULE-01）：本目标只拿到 include/，不链接任何第三方；
# 若公开头引入 rs2/eui 类型将因找不到头文件而编译失败。
add_executable(test_public_boundary test_public_boundary.cpp)
target_link_libraries(test_public_boundary PRIVATE rin::core)
target_include_directories(test_public_boundary PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME public_boundary COMMAND test_public_boundary)
set_tests_properties(public_boundary PROPERTIES LABELS "unit")

# 真机冒烟（hardware 标签）：D435if 在位时执行完整采集链路；无设备时测试自身
# 打印 SKIP 并返回 77，ctest 经 SKIP_RETURN_CODE 记为跳过（不计失败）。
# 链 rin::realsense_adapter（PUBLIC 传递 rin::core 与 executor 头/库）。
add_executable(test_realsense_hardware test_realsense_hardware.cpp)
target_link_libraries(test_realsense_hardware PRIVATE rin::realsense_adapter)
add_test(NAME realsense_hardware COMMAND test_realsense_hardware)
set_tests_properties(realsense_hardware PROPERTIES
    LABELS "hardware"
    SKIP_RETURN_CODE 77
    TIMEOUT 120)
