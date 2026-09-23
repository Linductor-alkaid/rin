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

# M3-03 IMU 运动数据契约（仅公开头）：MotionSample/ImuSnapshot/MotionSourceStats
# 的 valid() 与字段语义、StreamRequest.enableMotion 相等语义（restream 去重）、
# DeviceInfo/DeviceCatalog IMU 能力字段。
add_executable(test_motion_contracts test_motion_contracts.cpp)
target_link_libraries(test_motion_contracts PRIVATE rin::core)
target_include_directories(test_motion_contracts PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME motion_contracts COMMAND test_motion_contracts)
set_tests_properties(motion_contracts PROPERTIES LABELS "unit")

# M3-04 IntrinsicsSnapshot 契约扩展（仅公开头）：Extrinsics/MotionIntrinsics
# 的 valid() 语义（全零无效可观察、非有限拒绝）与快照"纯视频/完整"装配形态。
add_executable(test_motion_intrinsics test_motion_intrinsics.cpp)
target_link_libraries(test_motion_intrinsics PRIVATE rin::core)
target_include_directories(test_motion_intrinsics PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME motion_intrinsics COMMAND test_motion_intrinsics)
set_tests_properties(motion_intrinsics PROPERTIES LABELS "unit")

# M3-04 ImuFuser 接缝（Core 纯逻辑）：IdentityImuFuser 假实现契约（初态不可用、
# 推进后恒等姿态、reset 幂等）、多态/不可拷贝接缝与 createImuFuser 工厂可用性
# （刻意不锁定工厂返回的具体实现，M3-05 切换 Mahony 不破坏本测试）。
add_executable(test_imu_fuser test_imu_fuser.cpp)
target_link_libraries(test_imu_fuser PRIVATE rin::core)
target_include_directories(test_imu_fuser PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
add_test(NAME imu_fuser COMMAND test_imu_fuser)
set_tests_properties(imu_fuser PROPERTIES LABELS "unit")

# M3-05 MahonyImuFuser 数值与语义（Core 纯逻辑，DEC-010 冻结验证方式 1-8）：
# 静态重力对齐收敛、60s 已知旋转序列跟踪误差上界、yaw 漂移披露、零偏估计残差、
# 门限突发精度、复位幂等（memcmp 逐位重放）与确定性；GYRO 单调守卫/非有限时间戳/
# 零偏补偿/精确指数映射、ACCEL 一次性对齐/1g 门限/PI 修正单步解析、Params 注入
# 消毒、工厂 Mahony 真身，及 EXEC-06 热路径无堆分配（全局 operator new 计数）。
# 单线程纯逻辑：无跨上下文状态/关闭路径（DOD-02 矩阵不适用，见文件头说明）。
add_executable(test_imu_fuser_mahony test_imu_fuser_mahony.cpp)
target_link_libraries(test_imu_fuser_mahony PRIVATE rin::core)
target_include_directories(test_imu_fuser_mahony PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
add_test(NAME imu_fuser_mahony COMMAND test_imu_fuser_mahony)
set_tests_properties(imu_fuser_mahony PROPERTIES LABELS "unit")

# M3-04 MotionIngest（rs2-free 离线行为，EXEC-06 采集分支语义）：有效/无效采样
# 分支、会话序号单调与最新态邮箱语义、EMA 源频率（已知 dt/收敛/异常时间戳/长间隙）、
# resetStreamState（restream 语义：融合器复位 + 序号/累计样本连续）、ImuFuser 接缝
# （测试桩验证推进次数/原始采样/姿态门控），以及 Executor 生产者 + 主线程消费者的
# 跨上下文发布/读取与 shutdown 收敛（tsan 预设复跑目标）。
add_executable(test_motion_ingest test_motion_ingest.cpp)
target_link_libraries(test_motion_ingest PRIVATE rin::realsense_adapter)
target_include_directories(test_motion_ingest PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
add_test(NAME motion_ingest COMMAND test_motion_ingest)
set_tests_properties(motion_ingest PROPERTIES LABELS "unit")

# M3-06 3D 位姿视图投影数学（Core 纯逻辑，DEC-011 冻结验证方式 1）：四元数规范化/
# 共轭/Hamilton 乘积（复合方向契约）、四元数→矩阵（正交性、det=+1、与 Hamilton
# 旋转参考逐点一致、pitch≈±90° 无表示奇异）、矩阵转置/乘法/列主序外参布局、
# orbit look-at 语义（原点深度=distance、世界上方=视图上方、±89° 仰角截断、
# 非有限/distance<=0 消毒）、透视投影 y 向下屏幕约定与 <=nearZ 剔除边界、
# 线段→等宽四边形（法向半宽/点序/退化剔除）、失败路径不写出参；golden 端到端
# （已知位姿 + 固定观察相机 → 期望屏幕点集，double 参考独立推导，0.05 px 容差）
# 与"世界系固定/姿态驱动旋转"不变量。单线程纯逻辑：无跨上下文状态/关闭路径
# （DOD-02 矩阵不适用，见文件头说明）。
add_executable(test_pose_math test_pose_math.cpp)
target_link_libraries(test_pose_math PRIVATE rin::core)
target_include_directories(test_pose_math PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME pose_math COMMAND test_pose_math)
set_tests_properties(pose_math PROPERTIES LABELS "unit")

# M3-06 viewer 位姿视图状态语义（DEC-011 风险 3 重锚定契约）：PoseViewState
# 空态初值、update 保存快照且不动 baseline、Reset 以当前姿态重锚定显示参考
# （conj(baseline)⊗current，幂等）、空态 Reset no-op、clear 复位恒等与会话重
# 锚定；显示公式与 double 参考逐数值一致。包含 apps/viewer/pose_view.hpp 需
# EUI-NEO 公开头包含路径（经 eui_neo 目标用法属性注入，不链接其库）；compose
# 绘制路径需活动 EUI 运行时，headless 不可测（真机验收归 M3-08，见文件头说明）。
add_executable(test_pose_view_state test_pose_view_state.cpp)
target_link_libraries(test_pose_view_state PRIVATE rin::core)
target_include_directories(test_pose_view_state PRIVATE ${CMAKE_SOURCE_DIR}/apps/viewer)
target_include_directories(test_pose_view_state SYSTEM PRIVATE
    "$<TARGET_PROPERTY:eui_neo,INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:eui_neo,INTERFACE_INCLUDE_DIRECTORIES>")
target_compile_definitions(test_pose_view_state PRIVATE
    "$<TARGET_PROPERTY:eui_neo,INTERFACE_COMPILE_DEFINITIONS>")
add_test(NAME pose_view_state COMMAND test_pose_view_state)
set_tests_properties(pose_view_state PROPERTIES LABELS "unit")

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
