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

# M3-07 IMU 状态面板纯函数（apps/viewer/imu_panel.hpp）：四元数→ZYX 欧拉角读数
# （独立 double 四元数直分解参考：恒等/单轴/DEC-011 golden 30-20-40/角度网格往返/
# yaw ∈ (−180°,180°] 折叠/万向锁 ±90° 有限/退化输入消毒/非单位输入归一）、三行
# 文本组装精确串（源频率、姿态、四元数）、面板空态门控输入（available && valid()，
# 含 clear 排空后 stale 快照不放行）。compose 卡片路径需活动 EUI 运行时，headless
# 不可测（真机视觉验收归 M3-08，见文件头说明）；纯函数单线程值语义（DOD-02 矩阵
# 不适用，跨上下文/关闭排空由 shutdown_drain 覆盖）。
add_executable(test_imu_panel test_imu_panel.cpp)
target_link_libraries(test_imu_panel PRIVATE rin::core)
target_include_directories(test_imu_panel PRIVATE ${CMAKE_SOURCE_DIR}/apps/viewer)
target_include_directories(test_imu_panel SYSTEM PRIVATE
    "$<TARGET_PROPERTY:eui_neo,INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:eui_neo,INTERFACE_INCLUDE_DIRECTORIES>")
target_compile_definitions(test_imu_panel PRIVATE
    "$<TARGET_PROPERTY:eui_neo,INTERFACE_COMPILE_DEFINITIONS>")
add_test(NAME imu_panel COMMAND test_imu_panel)
set_tests_properties(imu_panel PROPERTIES LABELS "unit")

# M3-07 关闭路径回归（里程碑判据"关闭路径测试 + tsan 通过（跨上下文姿态数据）"，
# tsan 预设复跑目标）：Executor 生产者独占 MotionIngest（采集 worker 模型）+
# LatestMailbox 姿态通道 + 消费侧 PoseViewState 按 app.cpp:326-336 门控语义泵送。
# 覆盖：正常完成（序号推进/统计一致/单位姿态）、停止后无新发布（stop() 契约发布
# 半边）、排空语义（clear 后 stale 快照不复活视图、序号不回退、幂等）、任务异常
# （future 传播 + 失败路径排空/shutdown 收敛）、shutdown(true) Completed、排空后
# 新会话仅随新快照复活（resetStreamState 序号连续）。app.cpp ViewerContext 属
# 匿名命名空间且绑定 EUI 运行时，消费门控以同构模型锁定（文件头说明）。
add_executable(test_shutdown_drain test_shutdown_drain.cpp)
target_link_libraries(test_shutdown_drain PRIVATE rin::realsense_adapter)
target_include_directories(test_shutdown_drain PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/apps/viewer)
target_include_directories(test_shutdown_drain SYSTEM PRIVATE
    "$<TARGET_PROPERTY:eui_neo,INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:eui_neo,INTERFACE_INCLUDE_DIRECTORIES>")
target_compile_definitions(test_shutdown_drain PRIVATE
    "$<TARGET_PROPERTY:eui_neo,INTERFACE_COMPILE_DEFINITIONS>")
add_test(NAME shutdown_drain COMMAND test_shutdown_drain)
set_tests_properties(shutdown_drain PROPERTIES LABELS "unit")

# 公开头第三方类型边界（RULE-01）：本目标只拿到 include/，不链接任何第三方；
# 若公开头引入 rs2/eui 类型将因找不到头文件而编译失败。
add_executable(test_public_boundary test_public_boundary.cpp)
target_link_libraries(test_public_boundary PRIVATE rin::core)
target_include_directories(test_public_boundary PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME public_boundary COMMAND test_public_boundary)
set_tests_properties(public_boundary PROPERTIES LABELS "unit")

# 真机冒烟（hardware 标签）：D435if 在位时执行完整采集链路；无设备时测试自身
# 打印 SKIP 并返回 77，ctest 经 SKIP_RETURN_CODE 记为跳过（不计失败）。
# M3-08：IMU 出流频率验收（EMA 稳态 × 墙钟窗口交叉验证，交付频率对设备档位
# 只记录不断言，DEC-010 真机无数值阈值）与 restream 后源频率恢复量级断言。
# 链 rin::realsense_adapter（PUBLIC 传递 rin::core 与 executor 头/库）。
add_executable(test_realsense_hardware test_realsense_hardware.cpp)
target_link_libraries(test_realsense_hardware PRIVATE rin::realsense_adapter)
add_test(NAME realsense_hardware COMMAND test_realsense_hardware)
set_tests_properties(realsense_hardware PROPERTIES
    LABELS "hardware"
    SKIP_RETURN_CODE 77
    TIMEOUT 120)

# M4-09 工作流视图契约（DEC-016，仅公开头 + Core 纯函数）：契约类型 valid()
# 语义（ParamDescriptor 全分支 / NodeDescriptor / NodeCatalog 唯一性 /
# NodeOutputSnapshot stride 与容量边界）、paramKindOf 与三组 toString、
# findNodeDescriptor、validateWorkflowGraph 全部 ValidationIssueKind 反例与
# 正例（空图/链式缺省参数/扇出/全参数种类/闭区间边界）、间接环与多问题累积。
# 单线程纯逻辑：无跨上下文状态/关闭路径（DOD-02 并发矩阵不适用，见文件头）。
add_executable(test_workflow_contracts test_workflow_contracts.cpp)
target_link_libraries(test_workflow_contracts PRIVATE rin::core)
target_include_directories(test_workflow_contracts PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    # M6 目录扩展检查（第 15 节）引用默认目录单一事实源（仅头文件，无 src 链接）。
    ${CMAKE_SOURCE_DIR}/src/workflow)
add_test(NAME workflow_contracts COMMAND test_workflow_contracts)
set_tests_properties(workflow_contracts PROPERTIES LABELS "unit")

# M5-08 契约假引擎测试随假引擎移除（M5-06 假换真集成，DEC-016）：契约面由
# test_workflow_engine 对真引擎继续运行共用套件（workflow_engine_contract_suite.hpp）。

# M5-06 工作流运行控制 + 假换真集成测试（ui_workspace_design.md §5.8 / DEC-016，
# Independent-Verification-Agent）：相机帧源接缝（workflow_frame_source.hpp 的
# makeCameraFrameSource 对脚本化假 ICameraService：最新帧/零拷贝/多源独立水位/
# 无效帧与空服务防御/帧种类绑定）、运行控制状态机（WorkflowCanvasState::
# afterGraphChange 对真引擎：Idle 同步生效无待生效、Running 入队置 graphPending、
# app.cpp pump 契约复刻清除、校验拒绝不置位、stop 清除与重启衔接）、引擎-UI
# 端到端（假相机服务 → 帧源接缝 → 真引擎 → NodeOutputCache/WorkflowPerfState/
# pumpNodeOutput 消费：格式与 sourceSequence 透传、参数热更新下一帧生效、stop
# 排空语义、onShutdown 关闭顺序同构全序回归、Running 中直接 stop 数轮收敛）、
# 关停竞态防御（executor.shutdown(false) 后 stop 有界收敛）。compose 绘制路径
# （含工具栏按钮可用条件与待生效标注视觉）需活动 EUI 运行时，headless 不可测
# （真机视觉验收归 M5-07，见文件头说明）。tsan 预设复跑目标（跨上下文通道与
# 关闭顺序）。
add_executable(test_run_control test_run_control.cpp)
# eui_neo 本体仅满足 param_panel.hpp 内联组件函数引用的框架符号（剪贴板/文本
# 度量/GPU 导入；headless 不调用，不触窗口系统初始化）；其 -fno-exceptions 为
# PRIVATE 属性，不传播到本测试。
target_link_libraries(test_run_control PRIVATE rin::workflow rin::core OpenGL::GL
    eui_neo)
target_include_directories(test_run_control PRIVATE
    ${CMAKE_SOURCE_DIR}/apps/viewer
    ${CMAKE_SOURCE_DIR}/include)
target_include_directories(test_run_control SYSTEM PRIVATE
    "$<TARGET_PROPERTY:eui_neo,INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:eui_neo,INTERFACE_INCLUDE_DIRECTORIES>")
target_compile_definitions(test_run_control PRIVATE
    "$<TARGET_PROPERTY:eui_neo,INTERFACE_COMPILE_DEFINITIONS>")
add_test(NAME run_control COMMAND test_run_control)
set_tests_properties(run_control PROPERTIES
    LABELS "unit"
    TIMEOUT 300)  # 有界轮询/静默窗设计 <60s；挂看门狗兜底关闭竞态检查不悬挂。

# M5-02 工作台导航壳纯逻辑与主题令牌扩展（apps/viewer/navigation.hpp、
# viewer_theme.hpp，DEC-014）：元数据表完整性（页数 4、表序=枚举声明序、四枚举
# 双射、id/label/icon 非空唯一、id 稳定）、workbenchPageIndex 逐枚举与越界归 0
# （编译期 static_assert + 运行期）、workbenchPageId 与表项一致、NavigationState
# 初值/4x4 切换矩阵（异页 true、同页 no-op false 且状态不变）/往返/selectedIndex
# 与表双向一致、值语义拷贝与"只拥有当前页"结构锁定（sizeof + 页面状态见证逐位
# 不变），viewer_theme 的 workflowStateColor 逐状态令牌一致且四结果互异、
# portTypes/portTypeColor 逐字段一致不透明且 Gray8/Rgba8 互异。compose 绘制路径
# （composeNavRail/composeWorkflowShell）需活动 EUI 运行时，headless 不可测
# （真机视觉验收归 M5-07，见文件头说明）；单线程纯逻辑值语义（DOD-02 矩阵
# 不适用）。
add_executable(test_navigation test_navigation.cpp)
target_link_libraries(test_navigation PRIVATE rin::core)
target_include_directories(test_navigation PRIVATE ${CMAKE_SOURCE_DIR}/apps/viewer)
target_include_directories(test_navigation SYSTEM PRIVATE
    "$<TARGET_PROPERTY:eui_neo,INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:eui_neo,INTERFACE_INCLUDE_DIRECTORIES>")
target_compile_definitions(test_navigation PRIVATE
    "$<TARGET_PROPERTY:eui_neo,INTERFACE_COMPILE_DEFINITIONS>")
add_test(NAME navigation COMMAND test_navigation)
set_tests_properties(navigation PROPERTIES LABELS "unit")

# M5-03 节点编辑器画布纯逻辑（apps/viewer/canvas_model.hpp，DEC-015 交互几何
# 下沉；ui_workspace_design.md §7 测试清单）：视图变换（screen↔canvas 往返、
# 锚点缩放与上下限夹取、帧全图拟合、视图定心）、命中检测（节点顶层优先/端口
# 扩大命中区/连线采样距离/框选集合）、画布图同步（创建/删除连带边/连线替换边/
# 成环与类型不匹配拒绝——validateWorkflowGraph 唯一判据、悬空输入非阻塞、
# 校验标注缓存）、交互状态机（平移/节点拖动/框选与 Shift 加选/连线拖拽与取消/
# 右键删除/Alt 删线/Del 删选中）、调色板分组与即输即筛。单线程纯逻辑：无跨
# 上下文状态/关闭路径（DOD-02 矩阵不适用，见文件头说明）；compose 绘制路径
# 需活动 EUI 运行时，headless 不可测（真机视觉验收归 M5-07）。
add_executable(test_node_canvas test_node_canvas.cpp)
target_link_libraries(test_node_canvas PRIVATE rin::core)
target_include_directories(test_node_canvas PRIVATE
    ${CMAKE_SOURCE_DIR}/apps/viewer
    ${CMAKE_SOURCE_DIR}/include)
add_test(NAME node_canvas COMMAND test_node_canvas)
set_tests_properties(node_canvas PROPERTIES LABELS "unit")

# M5-04 参数面板与中间结果查看纯逻辑（apps/viewer/param_model.hpp 与
# canvas_model.hpp 参数赋值变换，DEC-015 决策 2；ui_workspace_design.md
# §5.5/§5.6/§7 测试清单）：控件-参数绑定（严格文本解析、值对声明校验——种类/
# 范围闭区间/枚举选项/RealArray 有限性、滑条归一化映射与取整、生效值解析、
# 控件初值文本）、RealArray 矩阵网格（扁平推断形状/重排尺寸保留数据）、节点
# 产物有界缓存（对工作流引擎拉取、每节点最新一幅、容量驱逐、停止排空）、缩略
# 图降采样（Gray8/Rgba8 按格式、最近邻只缩不放、stride 处理、有界幅面、
# sourceSequence 透传）、节点失败标注事件语义（NodeFailed 置位、Started/Stopped
# 清空）、画布 setParam（替换同 paramId/追加/toGraph 携带/无效赋值经校验标注）。
# 单线程纯逻辑（缓存拉取走引擎契约面，owner 线程直接调用，无并发；DOD-02
# 矩阵不适用，见文件头说明）；compose 绘制路径需活动 EUI 运行时，headless
# 不可测（真机视觉验收归 M5-07）。
add_executable(test_param_panel test_param_panel.cpp)
target_link_libraries(test_param_panel PRIVATE rin::workflow)
target_include_directories(test_param_panel PRIVATE
    ${CMAKE_SOURCE_DIR}/apps/viewer
    ${CMAKE_SOURCE_DIR}/include)
add_test(NAME param_panel COMMAND test_param_panel)
set_tests_properties(param_panel PROPERTIES LABELS "unit")

# M5-05 性能面板统计管道纯逻辑（apps/viewer/perf_model.hpp；ui_workspace_design.md
# §5.7/§4/§7 测试清单）：统计数值文本（统一小数位 + 固定后缀的 DEC-005 mono 替代
# 纪律）、WorkflowPerfState 统计管道（对工作流引擎的 sequence 推进消费、最新态
# 快照合并、节点统计查找、停止/失败后末次值冻结与 live 标志切换、重启衔接、
# clear 关闭排空后 stale 统计不复活）。单线程纯逻辑（消费走引擎契约面，owner
# 线程直接调用，无并发；DOD-02 矩阵不适用，见文件头说明）；compose 绘制路径
# （工具栏状态徽标/节点耗时徽标/右面板总览）需活动 EUI 运行时，headless 不可测
# （真机视觉验收归 M5-07）。
add_executable(test_perf_panel test_perf_panel.cpp)
target_link_libraries(test_perf_panel PRIVATE rin::workflow)
target_include_directories(test_perf_panel PRIVATE
    ${CMAKE_SOURCE_DIR}/apps/viewer
    ${CMAKE_SOURCE_DIR}/include)
add_test(NAME perf_panel COMMAND test_perf_panel)
set_tests_properties(perf_panel PROPERTIES LABELS "unit")

# M4-02 Core 图像契约（仅公开头 + Core 纯逻辑，image_workflow_design.md §8 前两行）：
# elementSize、ImageU8::make/wrap 正负例（零宽高/未知格式/stride 不足/缓冲过短/
# 超预算）、64 位乘法回绕防护反例（width=2^30 Rgba8、stride×height ≥ 2^32）、
# kMaxImageBytes 恰等/超界、row 越界与 padding 行距、拷贝共享像素与 const 不可变
# 类型面、参数模型三态生效值与五种类型化读取（含运行期种类错位防御）。
# 单线程纯逻辑值语义：无跨上下文状态/关闭路径（DOD-02 并发矩阵不适用，见文件头）。
add_executable(test_image_contracts test_image_contracts.cpp)
target_link_libraries(test_image_contracts PRIVATE rin::core)
target_include_directories(test_image_contracts PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME image_contracts COMMAND test_image_contracts)
set_tests_properties(image_contracts PROPERTIES LABELS "unit")

# M4-02 Core 图编译与单帧求值（Core 纯逻辑，image_workflow_design.md §8 后两行）：
# buildNodeGraph（校验透传/节点数准入含默认 64 边界/工厂异常与 typeId 不符显式化/
# 稳定拓扑序：链式·分叉·菱形多轮/入边执行序下标与输出端口/空图/find/移动语义）与
# runNodeGraph（注入语义全分支/防御性输出核对/异常原样传播含非 std 异常/像素共享
# 零拷贝链与扇出/空图）。节点桩内联实现 IImageNode，自建小目录。单线程纯逻辑：
# 无跨上下文状态/关闭路径（DOD-02 并发矩阵不适用，见文件头）。
add_executable(test_node_graph test_node_graph.cpp)
target_link_libraries(test_node_graph PRIVATE rin::core)
target_include_directories(test_node_graph PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME node_graph COMMAND test_node_graph)
set_tests_properties(node_graph PROPERTIES LABELS "unit")

# M4-03 几何算子节点 golden 数值测试（Core 纯逻辑，image_workflow_design.md §7 冻结
# 数值语义）：工厂 makeDefaultImageNode（source→nullptr、crop/downscale 实现签名一致、
# 未实现类型抛 invalid_argument）；crop 像素级 golden（4×3 @(1,1,2,2)、全图恒等、
# padding stride 输入输出紧凑不泄漏）、越界/退化/默认参数/无效输入/数量/格式/参数
# 种类错位拒绝；downscale nearest/bilinear 小图 golden（含 x.5 round-half-up 半值、
# 中心像素、alpha 不预乘）、floor 尺寸公式（5×3、848×480）、scale=1.0 双插值恒等、
# 常值图、线性渐变 |out−理想|≤1、37×23 与测试内独立 double 参考交叉验证（nearest
# 精确、bilinear ≤1）、退化输出/默认参数/构造期（scale 0/负/1.5/NaN/inf、非法枚举、
# 种类错位、参数缺失）拒绝；图集成（source→crop→downscale 链逐级 golden、图内越界
# 原样抛、未实现类型 buildNodeGraph 显式 BadParam）。golden 期望由 §7 冻结公式独立
# 手推/测试内参考实现推导，不参考实现代码。单线程纯逻辑：无跨上下文状态/关闭路径
# （DOD-02 并发矩阵不适用，见文件头）。
add_executable(test_image_ops_geometry test_image_ops_geometry.cpp)
target_link_libraries(test_image_ops_geometry PRIVATE rin::core)
target_include_directories(test_image_ops_geometry PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME image_ops_geometry COMMAND test_image_ops_geometry)
set_tests_properties(image_ops_geometry PROPERTIES LABELS "unit")

# M4-04 卷积算子节点 golden 数值测试（Core 纯逻辑，image_workflow_design.md §7 冻结
# 数值语义）：工厂 makeDefaultImageNode（gaussian_blur/conv_kernel 实现签名一致、
# 未实现类型 fft_* 抛 invalid_argument 且错误消息冻结为 M4-06 FFT；M4-05 起
# grayify/hist_eq 已实现并归 test_image_ops_histogram.cpp）；
# conv_kernel 构造期拒绝全分支（size 非法枚举/种类错位、kernel 缺失/种类错位/长度
# ≠K²/系数非有限、border 非法枚举/种类错位）、默认参数生效、单位核恒等（K=1/3/5 ×
# 三边界策略）、已知核响应 golden（非对称核 1..9 在 4×4 图全 16 字节手推、单点移位
# 核 clamp/reflect/zero 逐策略 golden、2× 核饱和/负响应截 0/x.5 round-half-up/
# Laplacian 和=0）、1×N/N×1 窄图 K=5 reflect-101 折返（含 n=1 图）、37×23 噪声图与
# 测试内独立 double 参考交叉验证（dyadic 核逐字节）、padding stride 输入、防御路径、
# 图集成（source→conv_kernel golden、kernel 长度不符 buildNodeGraph 显式 BadParam）；
# gaussian_blur 构造期拒绝（radius/sigma 越界/种类错位/声明缺失）、默认参数生效、
# sigma=0 δ 核恒等、常值图、核系数 golden（[0.274068619, 0.451862761,
# 0.274068619]）、角点脉冲与列阶梯逐字节手推 golden（质量守恒 255 自检）、可分离 vs
# 直接 2D 参考 9 组 radius×sigma 交叉验证（|diff| ≤ 1）、stride 紧凑/padding、防御
# 路径、图集成（sigma=0 恒等链）。golden 期望由 §7 冻结公式独立手推/测试内参考实现
# 推导，不参考实现代码。单线程纯逻辑：无跨上下文状态/关闭路径（DOD-02 并发矩阵不
# 适用，见文件头）。
add_executable(test_image_ops_convolution test_image_ops_convolution.cpp)
target_link_libraries(test_image_ops_convolution PRIVATE rin::core)
target_include_directories(test_image_ops_convolution PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME image_ops_convolution COMMAND test_image_ops_convolution)
set_tests_properties(image_ops_convolution PROPERTIES LABELS "unit")

# M4-05 直方图均衡与灰度化算子节点 golden 数值测试（Core 纯逻辑，image_workflow_design.md
# §7 冻结数值语义）：工厂 makeDefaultImageNode（grayify/hist_eq 实现签名一致、无参数
# 声明直接实例化、未实现类型 fft_lowpass 抛 invalid_argument 且错误消息冻结为 M4-06
# FFT）；grayify 通道错位判别（R/G/B 单通道极值 1×1 → 77/149/29 手推，BGR 误读可判别）、
# round-half-up 半值边界（S≡128 mod 256 → 18.5→19）、全黑/全白极值、4×2 混合小图全 8
# 字节手推 golden、alpha 不参与（同 RGB 不同 alpha 输出相同）、行尾 padding 输入（输出
# 紧凑 stride==宽、padding 不泄漏）、37×23 噪声图与测试内独立参考实现逐字节交叉验证、
# 防御路径（数量 0/2、无效图、Gray8 喂 Rgba8 声明）、图集成（source→grayify == 直接
# apply）；hist_eq 已知直方图 golden（6×4 hist{2:3,5:5,100:9,200:7}，cdf_min=3、
# den=21，出现值 → 0/61/170/255 手推含 num/den 中间量，padding 变体）、极值映射
# （v_min→0、v_max→255）、常值图恒等（全 7/全 255/全 0/1×1，非全白全黑，新紧凑缓冲）、
# 全量程均匀直方图精确恒等（256×2 每级 2 次，out[v]=v）、子区间均匀线性重映射
# （[40,47] 每级 3 次 → round-half-up(255j/7) 手推、[10,40] 半值锁 8.5→9 排除银行家
# 舍入）、不共享源像素（共享指针与数据指针核对）、防御路径（Rgba8 喂 Gray8 声明）、
# 亮度域一致性图集成（source→grayify→hist_eq 链路 == 参考亮度 Gray8 图直接经
# hist_eq == 独立参考）、37×23 噪声图参考交叉验证。golden 期望由 §7 冻结公式独立
# 手推/测试内参考实现推导，不参考实现代码。单线程纯逻辑：无跨上下文状态/关闭路径
# （DOD-02 并发矩阵不适用，见文件头）。
add_executable(test_image_ops_histogram test_image_ops_histogram.cpp)
target_link_libraries(test_image_ops_histogram PRIVATE rin::core)
target_include_directories(test_image_ops_histogram PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME image_ops_histogram COMMAND test_image_ops_histogram)
set_tests_properties(image_ops_histogram PROPERTIES LABELS "unit")

# M4-06 FFT 滤波节点族 golden 数值测试（Core 纯逻辑，image_workflow_design.md §7
# 2026-09-29 M4-06 冻结数值语义）：工厂 makeDefaultImageNode（fft_lowpass/
# fft_highpass/fft_bandpass 实现签名一致、未知类型抛 invalid_argument 且消息冻结为
# 无后缀新口径）；构造期拒绝全分支（cutoff/lowCut/highCut 缺失/种类错位/NaN/±inf/
# −0.1/1.1、bandpass lowCut ≥ highCut（含相等）空通带矛盾构造拒绝、0/1 边界
# 合法）、默认参数生效（单频探针夹逼 0.2/0.2+0.6）；正弦注入单频滤除/保留（512×512
# 原生路径谱线精确落 bin → 解析期望：低通滤除→均值、高通/带通保留→输入−均值、
# 全阻→0；848×480 填充路径零填充截断谱扩展属冻结语义→测试内自写 double 参考；
# 二维对角单频 fx=fy=1/16）；填充/裁剪往返（cutoff ≥ √0.5 低通恒等：848×480/
# 512×512/5×3 pad 8×4/1×5 pad 2×8 下界 2、c=√0.5 角点 bin 边界）；掩膜代数
# （LP+HP 掩膜补相加≈恒等、BP≡LP(hi)−LP(lo) 掩膜差按 8bit 饱和语义、实现内部
# 饱和感知形式）；DC 语义（原生常值图低通恒等/高通全黑精确、c=0 低通=均值、
# 斜坡 c=0 高通=斜坡−均值逐像素解析、填充路径 c=0 单 bin：848×480 常值 128 →
# Σ/524288 = 99.375 → 99 均匀精确、c=0 高通 28.625 → 29）；归一化频率跨尺寸不变
# （64²/128²/848×480 同 f 同 cutoff）；防御路径（数量 0/2、无效图、Rgba8 喂
# Gray8 声明）；stride padding 输入/紧凑输出/不共享源像素；掩膜边界 ≤/> 语义
# （c=0.125 恰在谱线上：低通保留、高通移除、BP 严格下界/闭上界）与 lowCut ==
# highCut 空通带构造拒绝（empty passband）；图集成（source→fft_lowpass == 直接
# apply == 参考、lowCut>highCut 经工厂异常显式化 BadParam）。golden 期望由 §7 冻结
# 公式独立手推/测试内自写
# double 参考实现（自含 radix-2 FFT，参考自检先行）推导，不参考实现代码。
# 单线程纯逻辑：无跨上下文状态/关闭路径（DOD-02 并发矩阵不适用，见文件头）。
add_executable(test_image_ops_fft test_image_ops_fft.cpp)
target_link_libraries(test_image_ops_fft PRIVATE rin::core)
target_include_directories(test_image_ops_fft PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME image_ops_fft COMMAND test_image_ops_fft)
set_tests_properties(image_ops_fft PROPERTIES LABELS "unit")

# M4-07 工作流真引擎独立验证（DEC-013 执行模型，Independent-Verification-Agent）：
# 以真引擎 fixture 运行两引擎共用契约套件（workflow_engine_contract_suite.hpp，
# DEC-016），加真引擎特有检查：工厂校验三分支抛 invalid_argument
# （paramQueueCapacity==0 按 1 处理）、默认目录单一事实源（makeDefaultImageNodeCatalog）、
# 真实算子数值链（source→grayify BT.601 定点亮度逐像素
# golden 手推）、参数"下一帧生效"（downscale scale 0.5→1.0 输出 64×48→128×96）、
# EXEC-07 过载显式丢弃（maxInFlight=1 + 20ms 慢算子 → droppedFrames>0 且
# processedFrames 增长且 inFlight≤1）、帧源空闲静默（门控关闭 300ms 无帧且保持
# Running）、Running 图替换消费式语义（GraphApplied 不逐帧重发——对照已移除
# 假引擎的 peek 缺陷探针 + 参数真实生效 floor(128×0.9)=115）、sourceSequence 传播（同帧两节点
# 相等 + 运行期非递减）、运行期算子失败归因（crop 越界 ROI → NodeFailed node=2
# + Failed + lastError，多会话采样）、关停竞态防御（executor.shutdown(false) 后
# stop 有界收敛）与会话复位（processedFrames 复位、统计通道序号实例内单调）。
# tsan 预设复跑目标（跨上下文通道与关闭顺序）。
add_executable(test_workflow_engine test_workflow_engine.cpp)
target_link_libraries(test_workflow_engine PRIVATE rin::workflow)
target_include_directories(test_workflow_engine PRIVATE
    ${CMAKE_SOURCE_DIR}/src/workflow
    ${CMAKE_SOURCE_DIR}/include)
add_test(NAME workflow_engine COMMAND test_workflow_engine)
set_tests_properties(workflow_engine PROPERTIES
    LABELS "unit"
    TIMEOUT 300)  # 套件 + 特有检查设计 <60s；挂看门狗兜底关停/关闭竞态检查不悬挂。

# M8 米制深度策略预处理算子 golden 数值测试（Core 纯逻辑，DEC-018，
# depth_policy_preproc_design.md §5 冻结公式 / §6 golden 项）：DepthFrameF32 契约
# （make/wrap/valid/byteSize/row、stride 语义、16 MiB 字节预算恰等/超限、共享只读）、
# PolicyDepthConfig 默认值逐字段与 valid() 全约束反例/边界正例（delay 恰好上界、
# historyLength 4096/4097、u64 回绕对抗）；O1 无效填充（NaN/+Inf/−Inf/0/负值混合、
# invalidBelow 非默认含恰等与负阈值、farValue 非有限拒绝）；O2 面积降采样（2×2→1×1、
# 3×2→2×1 分数覆盖手推、8×8→2×2 整数倍恒比、同尺寸恒等含 padding 输入、848×480→64×36
# 等四组与独立 double 参考交叉 ≤1e-6、放大/零尺寸/无效帧拒绝）；O3 裁切（64×36
# (18,0,16,16)→18×32 索引逐位含 padding stride、退化/越界全分支拒绝、贴边合法）；
# O4 高斯模糊（核系数 golden 经脉冲响应读出、σ=0 δ 核恒等、常值图恒等、角点脉冲
# reflect-101 折返手推 + 能量守恒、1×6 窄图手推、radius×sigma 九组直接 2D 与可分离
# 双参考交叉 ≤1e-6、padding 一致、越界/无效帧拒绝）；O5 裁切归一化（双侧 clip、
# 非零 near、near≥far 拒绝）；O6 冻结顺序全链（合成 848×480 混合无效像素 vs 测试内
# 按冻结顺序独立 double 参考逐像素 ≤1e-6、输出恰 18×32 值域 [0,1]、config/输入无效
# 拒绝）；O7 时序历史（空历史全 0、append 1/5/36 帧欠帧首帧填充逐位、满环 37 帧与
# 滚动 40 帧抽样下标 {1,6,…,36}、CHW oldest→newest、delay=1 下标 {0,5,…,35}、非默认
# 小配置、historyLength=1 微型环、reset、尺寸不符/无效帧/padding stride 对抗、非法
# config 构造抛）。golden 期望由冻结公式独立手推/测试内参考实现推导，不参考实现代码。
# 单线程纯逻辑：无跨上下文状态/关闭路径（DOD-02 并发矩阵不适用，见文件头）。
add_executable(test_depth_preproc test_depth_preproc.cpp)
target_link_libraries(test_depth_preproc PRIVATE rin::core)
target_include_directories(test_depth_preproc PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME depth_preproc COMMAND test_depth_preproc)
set_tests_properties(depth_preproc PROPERTIES LABELS "unit")

# M9-04 米制深度转换纯单测（DEC-019，Independent-Verification-Agent；头文件
# src/adapters/realsense/depth_metric_convert.hpp header-only 零 SDK 依赖）：
# 码值×scale 精确换算（scale 1.0 恒等/0.5 倍频/典型 0.001 毫米→米）、无效像素
# 0 保持 0.0（含 +0 符号位）、非零最小/最大码值（1/65535）端点、strideUnits ==
# width 紧凑与 > width 行距 + padding 哨兵不泄漏、全零图全 0.0、全分支拒绝
# （nullptr/0 尺寸/stride 不足/scale 0/−0/负/NaN/±Inf）且不部分写出、out 复用
# 重置语义、848×480 规模全量 double 参考交叉 + 冻结公式 float 形式精确互证。
# 单线程纯函数：无跨上下文状态/关闭路径（DOD-02 并发矩阵不适用，见文件头）。
add_executable(test_depth_metric_convert test_depth_metric_convert.cpp)
target_include_directories(test_depth_metric_convert PRIVATE
    ${CMAKE_SOURCE_DIR}/src/adapters/realsense)
add_test(NAME depth_metric_convert COMMAND test_depth_metric_convert)
set_tests_properties(depth_metric_convert PROPERTIES LABELS "unit")

# M10 深度域节点 golden 数值测试（DEC-020，Independent-Verification-Agent；数值
# 判据 = depth_policy_preproc_design.md §5 冻结公式）：工厂分派全分支（真实默认
# 目录：source_depth_metric → nullptr 注入型、5 个深度算子实现非状态、
# depth_history 实现 + IStatefulImageNode 标记、未知类型抛）；O1 填充（NaN/±Inf/
# 0/负混合逐位 golden、invalid_below 恰等 ≤ 边界、16×12 double 参考交叉 ≤1e-6、
# 显式 stride padding 哨兵不泄漏输出紧凑、格式/无效帧/输入数量错位 apply 抛）；
# O2 面积降采样（2×2→1×1、3×2→2×1 分数覆盖手推、848×480→64×36 double 参考交叉、
# 同尺寸恒等含 padding、放大/零尺寸拒绝）；O3 裁切（64×36 (18,0,16,16)→32×18
# 索引逐位、退化/越界/全图拒绝）；O4 模糊（σ=0 恒等、常值恒等、核系数经内部
# 脉冲响应与解析式互证、reflect-101 角点折返手推 + 能量守恒、radius/sigma 越界
# apply 抛）；O5 归一化（双侧 clip、恒比精确、非零 near、near≥far 抛）；容器
# 纪律（elementSize/depthF32Row 有效性/make 最小行宽/输出紧凑 byteSize）；参数
# 纪律（缺失/种类错位构造抛、history_length 0/4097/抽样超限抛、恰等上界合法）；
# 图集成（真实目录 source_depth_metric→fill→resize→crop→blur→normalize→history
# 全链 runNodeGraph 双帧 golden：常量 1.25→逐级核对→欠帧填充全 0.5，第二帧
# idx={1,6,…,31,36} 抽样 7 平面首帧 + 底平面 0.8）。
# 单线程纯逻辑：无跨上下文状态/关闭路径（DOD-02 并发矩阵不适用，见文件头）。
add_executable(test_image_ops_depth test_image_ops_depth.cpp)
target_link_libraries(test_image_ops_depth PRIVATE rin::core)
target_include_directories(test_image_ops_depth PRIVATE
    ${CMAKE_SOURCE_DIR}/src/workflow
    ${CMAKE_SOURCE_DIR}/include)
add_test(NAME image_ops_depth COMMAND test_image_ops_depth)
set_tests_properties(image_ops_depth PROPERTIES LABELS "unit")

# M10 引擎有状态节点执行模型测试（DEC-020 决策 3，Independent-Verification-Agent）：
# IStatefulImageNode 串行在飞门（buildGeneration 编译期探测 + tick 门——跳过提交
# 不计过载丢弃、stats.inFlight ≤ 1、持续供帧 + 慢算子下历史平面按应用序严格递增
# 堆叠）、DepthHistoryNode staged 旧帧重提交去重（节点级同缓冲对象逐位不变 +
# 引擎级多源图 staged {S1: C} 反复重提交保持 [A,B,C]）、生成代复位（参数热更新
# 重建生成代清空历史 → 欠帧首帧填充可观测）、无状态图对照（慢 fill 图
# droppedFrames > 0，门不误触发）、输入尺寸中途变化 → NodeFailed/Failed 终态。
# 全部有界轮询（5s 死限）；owner 顺序 engine stop → executor shutdown。
# tsan 预设复跑目标（跨上下文帧泵/任务与关闭顺序）。
add_executable(test_workflow_depth_engine test_workflow_depth_engine.cpp)
target_link_libraries(test_workflow_depth_engine PRIVATE rin::workflow)
target_include_directories(test_workflow_depth_engine PRIVATE
    ${CMAKE_SOURCE_DIR}/src/workflow
    ${CMAKE_SOURCE_DIR}/include)
add_test(NAME workflow_depth_engine COMMAND test_workflow_depth_engine)
set_tests_properties(workflow_depth_engine PROPERTIES
    LABELS "unit"
    TIMEOUT 300)

# M10 帧源米制路由与 Depth32F 缩略图测试（DEC-020 决策 4/5，Independent-Verification-
# Agent；workflow_frame_source.hpp 与 param_model.hpp 纯逻辑，无 EUI 类型）：
# renditionForSourceType 六映射 + 未知回退 RGB、WorkflowSourceRouter 回退/整体换新、
# makeCameraFrameSource 对 source_depth_metric（脚本化假服务：水位过滤、float 逐元素
# 精确、sourceSequence、无新帧/无效采样/服务失效 false、stop 排空、表外节点回退
# RGB）、thumbnailRgbaFromSnapshot Depth32F 分支（P99 手推 golden：0→黑、round-half-
# up 半值 127.5→128、≥P99 截断 0、负值同 0、全无效帧全黑、padding stride 不泄漏、
# sourceSequence 透传、Gray8/Rgba8 不回归）。
# 单线程纯逻辑：无跨上下文状态/关闭路径（DOD-02 并发矩阵不适用，见文件头）。
add_executable(test_workflow_depth_source test_workflow_depth_source.cpp)
target_link_libraries(test_workflow_depth_source PRIVATE rin::workflow)
target_include_directories(test_workflow_depth_source PRIVATE
    ${CMAKE_SOURCE_DIR}/apps/viewer
    ${CMAKE_SOURCE_DIR}/src/workflow
    ${CMAKE_SOURCE_DIR}/include)
add_test(NAME workflow_depth_source COMMAND test_workflow_depth_source)
set_tests_properties(workflow_depth_source PROPERTIES LABELS "unit")

