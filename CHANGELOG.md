# 更新日志

本项目的版本遵循语义化版本（工程规范 10.5）；tag 与里程碑"建议发布点"一一对应。

## [Unreleased]

### 新增

- 图像工作流 Core 契约（M4，`M4-02`）：`rin::ImageU8`（Gray8/Rgba8、共享不可变
  像素、16 MiB 单图字节预算、stride 语义）、`rin::IImageNode` 同步节点契约与
  类型化参数模型（构造期定型，运行期热更新经帧边界重建实现"下一帧生效"）、
  `rin::NodeGraph` 图编译（`validateWorkflowGraph` 唯一判据、节点数准入、稳定
  拓扑序）与 `runNodeGraph` 单帧同步求值（注入型源节点、算子输出防御性契约
  核对）；设计文档 `docs/design/image_workflow_design.md`（M4 节点目录与逐算子
  golden 测试项要求，DEC-012 的 FFT 内部 2 幂填充 + 归一化频率掩膜约束 golden 化）。
- 几何算子节点（M4，`M4-03`）：`rin::makeDefaultImageNode` 默认节点工厂与
  `crop`/`downscale` 实现——裁切 ROI 退化/越界在执行期显式拒绝、输出为紧凑
  新缓冲；降分辨率输出尺寸 floor(in×scale)，nearest 面积覆盖采样与 bilinear
  中心对齐插值（逐通道独立、round-half-up 量化），scale=1.0 逐像素恒等；
  数值语义冻结于 `docs/design/image_workflow_design.md` §7。
- 卷积/高斯算子节点（M4，`M4-04`）：`rin::makeDefaultImageNode` 工厂扩展
  `conv_kernel`（K ∈ {1,3,5} 自定义行主序核，clamp/reflect/zero 边界填充可配，
  相关语义核不翻转，double 累加 round-half-up 饱和量化）与 `gaussian_blur`
  （半径语义 K=2·radius+1、sigma ∈ [0,10]，可分离两趟 + 固定 clamp 边界，
  sigma=0 为 δ 核恒等输出，与直接卷积量化后逐像素差 ≤ 1）；工作流目录 schema
  为 `conv_kernel` 增加 `border` 参数（假引擎目录同步），数值语义冻结于
  `docs/design/image_workflow_design.md` §7。
- 直方图均衡与灰度化节点（M4，`M4-05`）：`rin::makeDefaultImageNode` 工厂扩展
  `hist_eq`（Gray8 直方图均衡：cdf_min 映射 + 整数 round-half-up，最大/最小出现
  灰度精确映射 255/0，常值图恒等输出，全量程均匀直方图逐像素恒等）与 `grayify`
  （Rgba8 → Gray8 BT.601 定点亮度 Y = (77·R + 150·G + 29·B + 128) >> 8，alpha
  不参与）；目录 schema 无变更，数值语义冻结于
  `docs/design/image_workflow_design.md` §7。
- FFT 滤波节点族（M4，`M4-06`）：`rin::makeDefaultImageNode` 工厂扩展
  `fft_lowpass`/`fft_highpass`/`fft_bandpass`（Gray8 → Gray8，理想锐截止频域
  滤波：内部零填充到 2 幂（宽高各自，848×480 → 1024×512），掩膜按归一化频率
  （每像素周期数，∈ [0,1]）在填充分辨率上构造——高通为低通逐点补、带通为
  低通掩膜差，IFFT 显式 1/(padW·padH) 归一化后裁回原尺寸、round-half-up 饱和
  量化；FFT 后端为 pinned kissfft 131.2.0 float，`rin_core` 私有链接、公开头
  零第三方类型）；目录 schema 无变更，数值语义冻结于
  `docs/design/image_workflow_design.md` §7。
- 工作流真引擎（M4，`M4-07`，DEC-013 执行模型冻结）：`rin_workflow` 库的
  `createWorkflowEngine` 按契约 `IWorkflowEngine` 实现——帧泵采样 + 最新帧
  快照 + 有界在飞显式丢弃（帧泵为 Executor 周期 tick，引擎不拥有线程设施；
  无新帧不提交；在飞满载显式丢弃计数），帧输入经 `WorkflowFrameSource` 接缝
  （相机帧 → ImageU8 转换归适配/应用层），经 `buildNodeGraph`/`runNodeGraph`
  求值真实算子，逐节点耗时与失败归属经 `runNodeGraph` 新增的逐节点观测接缝
  实测（`NodeStats` 滚动窗口 32 帧），产物每节点最新一幅且发布按帧提交序
  有序化；Running 下图替换为消费式语义（仅实际变化时换代并发布一次
  `GraphApplied`，不复刻假引擎 peek 缺陷）、参数热更新受理期预编译校验 +
  帧边界生效；M4 节点目录构建提取为两引擎共享单一事实源（假引擎委托），
  `runNodeGraph` 扩展为加性默认参数（既有调用不受影响）。
- 图像工作流执行模型决策（M4，`M4-07`）：[DEC-013](docs/decisions/DEC-013-workflow-execution-model.md)
  冻结（帧泵采样 + 最新帧快照 + 有界在飞显式丢弃；相机 worker 推送驱动、
  任务期拉帧、容量 1 DropOldest 图通道与假引擎 peek 模式经论证否决）。
- 工作流视图契约（M4，`M4-09`，[DEC-016](docs/decisions/DEC-016-contract-first-workbench-order.md)
  契约先行）：`include/rin/workflow_types.hpp`（工作流图模型、节点目录与类型化
  参数 schema、`validateWorkflowGraph` 结构校验、逐节点耗时与端到端统计、
  产物快照与事件语义）与 `include/rin/workflow_engine.hpp`（`IWorkflowEngine`
  运行控制 + 有界最新态数据通道）——UI 与引擎间的唯一契约面，公开头零第三方
  类型；`M5-08` 假引擎与 `M4-07` 真引擎实现同一契约并共用契约测试套件，完成
  判据随 `M4-08` 关闭全部满足。
- M4 测试矩阵与吞吐实测记录（M4，`M4-08`）：`tools/workflow_bench` 实测工具
  （Release 构建，`RIN_BUILD_TOOLS` 门控，不进 ctest）与记录
  [docs/benchmarks/workflow-throughput-848x480.md](docs/benchmarks/workflow-throughput-848x480.md)
  ——848×480 典型链（grayify→gaussian_blur→fft_lowpass→hist_eq）真引擎吞吐：
  30 fps 相机速率三次独立运行 18 轮 29.94–30.04 fps、零丢弃（余量约 3.4 倍，
  最强可复现结论）；标称频率态饱和 ≈102 fps，FFT 低通为链主导成本
  （10.4–14.9 ms/帧）；降分辨率前置变体把 FFT 降至 1.8–4.3 ms/帧；只记录
  实测值与方法，无性能目标声明；M4 全部工作项与退出条件闭合。

### 修复

- deb 安装后 UI 字体丢失（2026-09-29）：deb 未随包携带 EUI-NEO 运行时字体，
  安装后可执行文件旁无 `assets/`，UI 文本与图标退化为系统字体回退——中文
  （JingNanJunJunTi）与 Font Awesome 图标码位在常见系统字体中无覆盖（本机
  Ubuntu 24.04 桌面 strace 实证：仅 NotoSans/Symbols2/Emoji/DejaVu 被加载，
  CJK 回退路径不匹配实际安装位置），精简系统上文字整体不渲染；字体亦无法经
  shlibdeps 声明依赖。现 `cmake/Packaging.cmake` 随包安装两个运行时字体到
  `/usr/share/rin/fonts/`，viewer 启动时探测该目录并经
  `DslAppConfig::textFont/iconFont` 显式指定（开发布局不受影响，仍用 exe 旁
  assets）；[DEC-009](docs/decisions/DEC-009-self-contained-deb-distribution.md)
  同步增补。

## [0.3.0] - 2026-09-24（M3：IMU 位姿视图与 20.04 适配）

### 新增

- IMU 姿态通路（M3，DEC-010）：采集支持 ACCEL/GYRO 运动流（`StreamRequest::enableMotion`
  契约开关，viewer 默认开启）；Core 新增 Mahony 六轴姿态融合 `ImuFuser`（四元数、
  PI 反馈 + 陀螺零偏在线估计、1g 重力参考门限；六轴无磁力计，yaw 长期漂移为
  传感器物理限制，界面与文档如实披露）；`ICameraService` 新增 `tryLoadMotion()` /
  `tryLoadPose()` 最新态通道；适配器读取出厂 motion intrinsics 与 gyro→color
  外参，restream/设备切换后融合复位并重新收敛。
- viewer 3D 位姿视图（M3，DEC-011）：固定世界坐标系（坐标轴 + 地面网格）中实时
  呈现相机视锥与相机轴，姿态驱动旋转；Reset 重锚定显示参考，姿态不可用时空态。
- viewer IMU 状态面板（M3）：实时显示 IMU 源频率与姿态读数（ZYX 欧拉角、四元数）。
- Ubuntu 20.04 (focal) 适配（v0.3.0 起）：CI 的 deb 打包改在 `ubuntu:20.04` 容器
  内构建（GCC 10、pip 安装 CMake ≥ 3.25/Ninja；executor 公开头使用 concepts/requires
  表达式，最低 GCC 10），产物按 focal glibc 2.31 链接并经 objdump 护栏校验，
  随 Release 分发的 deb 可在 20.04 安装运行。

### 修复

- 3D 位姿视图冻结在初始姿态（M3，EUI-20260924-001）：EUI-NEO retained layer
  绘制签名不含 polygon 点集，位姿卡场景点集逐帧变化而缓存层永不失效，视图冻结
  在首次烘焙的姿态（真机插桩 + 像素差分取证，见依赖台账）。绕行：位姿场景
  polygon 携带姿态通道序号作显式脏键（`dirtyKey`），脱离缓存层逐帧直接绘制；
  上游修复后回归移除。
- 运动流打开失败不再拖死视频流（M3）：无 root 运行时 HID/IIO 设备节点权限前置会
  使含 ACCEL/GYRO 的 pipeline 打开失败，此前服务经有界重试后进入 Failed，视频与
  IMU 全部无输出；现适配器在运动流打开失败时一次性降级为纯视频配置重试，降级原因
  以事件显示于界面状态行，之后每次流重建（restream/设备切换/重开）自动重试运动流；
  纯视频打开失败保持既有重试/Failed 语义。
- 更换分辨率后 IMU 数据停更（M3）：viewer 分辨率档位构造 `StreamRequest` 时丢失
  `enableMotion` 位（契约默认 false），restream 命令触发适配器按"无运动流"重建
  pipeline，姿态通道停止发布、IMU 面板与 3D 视图停留陈旧快照；现分辨率档位沿用
  默认请求的运动流意图（该位为请求级粘性，跨分辨率切换保持）。

## [0.2.0] - 2026-09-24（M2：Rin 更名与 Linux 自包含分发）

### 新增

- Linux 自包含 deb 分发（DEC-009）：`cpack -G DEB` 产出 `rin_<版本>_amd64.deb`，
  含 `/usr/bin/rin`、捆绑的 librealsense2 运行库（`/usr/lib/rin`，RUNPATH 定位）、
  RealSense udev 规则（安装后免 root 使用设备）、桌面入口与 hicolor 图标；deb
  依赖由 `dpkg-shlibdeps` 自动生成。安装规则组件化（`rin` 组件），依赖的 install
  规则不进入分发包。
- GitHub Actions CI（M2-05）：push/PR 构建并运行测试、构建 Release 并导出 deb
  工件；`v*` tag 触发将 deb 附着到 GitHub Release。
- 深度图配色运行时可选（DEC-007）：viewer 控制行新增 Palette 下拉，可在 jet 伪彩
  与灰度黑白（近白远黑）间即时切换；切换仅影响后续帧转换、不重流；`Waiting` 态
  可预设，接入后按所选配色出流。公共契约新增 `DepthColorScheme` 与
  `ICameraService::requestDepthColorScheme`（等待/出流状态有效、粘性，终态拒绝）。
- 相机热插拔与多设备选择（DEC-006）：启动不依赖相机连接（`Waiting` 设计稳态）；
  运行中经 `rs2::context` 设备变化回调自动感知插拔并刷新在线设备目录；唯一设备
  自动选中，多设备时由用户在 Device 下拉中选择（`requestDevice`，选择意图粘性
  跨插拔保留）；活动设备被移除自动回到等待态并可在重插后恢复出流。
- 键盘 1-9 直选分辨率档位。

### 变更

- 项目更名为 **Rin**（DEC-008）：C++ 命名空间 `rsv`→`rin`、公开头目录
  `include/rs_vision/`→`include/rin/`、构建目标 `rin_core`/`rin_realsense_adapter`/
  `rin`、CMake 选项前缀 `RIN_`、窗口标题 "Rin"；设备与 SDK 相关标识保留
  "realsense"。仓库远端 `Linductor-alkaid/rin`。
- librealsense2 由 system 类依赖改为 external 源码构建（DEC-009，pinned commit
  `7c3ee3fb7...` 即上游 2.58.3 不变）：不再要求系统预装；构建面裁剪为仅运行时库，
  并在源码构建域内以 C++17 编译（rsutils 头与 GCC13 C++20 不兼容，
  LRS-20260923-002）。
- viewer 视觉层按 ZCode Design System 令牌翻译重构（DEC-005）：语义色/字阶/圆角
  层级/间距节奏集中于 `apps/viewer/viewer_theme.hpp`；画面卡化、状态语义色、
  下拉令牌化样式；控制行上移为工具栏位，下拉弹层经根 stack 末位合成浮于卡片之上。
- 新增键盘切换分辨率（数字键 1-9 直选档位）。

### 修复

- viewer 视频画面仅左缘更新、其余全黑（EUI-20260923-003）：绕行 EUI-NEO ImageStream
  动态纹理上传缺陷（官方 dynamic_texture 示例同环境可复现），改经外部 GPU 图像接口
  （`GpuFrameView`：UI 线程自有 GL 纹理上传 + `importGpuImage` 导入）。真机复验
  完整画面输出正常。

## [0.1.0] - 2026-09-23（M1：viewer 基础能力）

### 新增

- 仓库骨架：AGENTS.md 协作约定、项目管理与工程规范、CMakePresets（debug/release/
  asan/ubsan/tsan）、依赖锁清单 `third_party/dependencies.lock` 与 configure 时 commit
  校验（`cmake/Dependencies.cmake`）。
- `rin_core`：相机服务契约（`rin::ICameraService`、`StreamRequest`、`Frame`、
  `IntrinsicsSnapshot`、`StreamCapabilities`、`ServiceEvent`）、显式相机状态机
  （Idle/Opening/Streaming/Restreaming/Stopping/Failed）、RGB8→RGBA8 与 Z16→jet
  RGBA8 像素转换；公开头零第三方类型。
- `rin_realsense_adapter`：librealsense2 设备枚举/能力/内参读取、双流采集（executor
  blocking worker 承载）、协作取消与关闭收敛、`LatestMailbox` 帧与事件通道、分辨率
  切换（pipeline 重建）。
- `rin`：EUI-NEO 实时预览应用——RGB/深度双画面（`ImageStream`）、设备能力驱动
  的分辨率下拉、彩色/深度内参面板；Executor owner 挂 `dslAppConfig()` 首调点，
  关闭经 `DslAppConfig::onShutdown` 闭合。
- 测试：状态机全矩阵（238 checks）、像素转换（274 checks）、公开头边界、真机
  hardware 冒烟（41 checks，标签 `hardware`，无设备时 SKIP）。

### 依赖

- executor pinned `4731b16493ae996311a9e85f55bd137aee69418a`（external）
- eui-neo pinned `782c56993dc1890e0589e2100cfa74322bb0e0bf`（external）
- realsense2 系统 2.58.3（system；源 commit `7c3ee3fb7c640e9f315e663907208cb56c4febfd`）
- 各依赖独立反馈台账见 `docs/dependency_feedback/`（executor 在
  `docs/executor_feedback/ledger.md`）。

### 已知限制

- tsan 预设在本机内核（7.0.0-31，高熵 ASLR）下需 `setarch -R` 旁路运行。
- librealsense2 为系统依赖（system 类），无系统安装的机器 configure 失败
  （LRS-20260923-001）。
