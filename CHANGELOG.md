# 更新日志

本项目的版本遵循语义化版本（工程规范 10.5）；tag 与里程碑"建议发布点"一一对应。

## [Unreleased]

## [0.5.4] - 2026-10-09（预览策略深度可选输出与界面字号设置）

### 新增

- 预览深度可选输出（[PR #50](https://github.com/Linductor-alkaid/rin/pull/50)，
  用户需求"整个 policy 输出工作流（不带历史分片）的输出进预览深度可选
  输出"，参考 roboparty E3-Parkour 部署语义）。预览页 Depth 卡新增
  Raw ⇄ Policy 循环切换：Policy 输出 = 米制深度通道 → O6 冻结单帧组合
  （无效填充 → 64×36 raw 网格面积降采样 → 裁切 (18,0,16,16) → 高斯模糊 →
  [0,2.5 m] 归一化，`PolicyDepthConfig` 默认值即 E3 部署参考冻结值）→
  最近邻放大 ≤512 → [0,1]×255 灰度（无效填充呈白，不随全局配色命令变化）。
  新组件 `viewer::PolicyDepthOutput`（apps/viewer/policy_depth_output.hpp，
  无 EUI 类型可 headless 单测）：Executor 20ms 软调度周期 tick
  （DEC-019 决策 3 纪律：弱引用闭包/busy 跳过/失败显式丢弃计数），
  渲染线程 pump 非阻塞消费上传；**组件必须 shared_ptr 持有**，`start()`
  对 unique/栈持有显式拒绝（见修复项）。决策记录
  [DEC-022](docs/decisions/DEC-022-preview-depth-source-and-font-scale.md)。
- 设置页字号档位（用户需求）：Preferences 新增 Font size 四档
  （小 0.85 / 标准 1.0 / 大 1.15 / 特大 1.3），运行期写应用配置
  `uiScale`——EUI 每帧读取 effectiveScale（输入/布局/绘制同一路径），
  变更即整页等比重排，避免纯文本缩放的行高裁切；会话级，EUI 契约无变更。

### 修复

- 工作流画布下拉失效（用户真机反馈："接入非监看器下游模块后所有视频源
  下拉没法点开"）。根因（EUI-20261009-001，真机 A/B 取证）：EUI 命中与
  绘制按子树最大 zIndex 排序，组件滑条内部 ".hit" 热区自带 zIndex(10)
  ——画布内存在任一滑条参数节点（深度域模块等，监看器无滑条故此前未
  暴露）时页面子树 z 上限被抬到 10，默认 z=0 的根级浮层（分辨率下拉
  scrim/面板、右键创建菜单、拖拽跟随）落到页面之下，点击全部穿透。
  修复：浮层显式 `kOverlayZIndex=1000`（面板/scrim 分级）；上游化
  三步走：issue sudoevolve/EUI-NEO#95 → 上游修复 PR sudoevolve/EUI-NEO#96
  （含回归测试，上游 ctest 34/34）→ 锁清单 eui-neo 升至 fork 修复提交
  791cb46（基 dev c444e53；上游合入后回迁）。Rin 侧显式 zIndex 保留为
  防御性声明。IVA 判别性对照：归零 Rin 侧声明后 S1 仍通过，上游修复
  独立生效。
- 设置页双下拉纵向重叠区互扰（用户真机反馈两轮）：配色菜单展开区与
  字号字段重叠（菜单高 110px、字段距 40px），同 z tie 下点配色选项被
  字号字段截胡/菜单被遮挡；先开字号再开配色时双菜单同层互扰。修复：
  composeSelect 展开中整栈再升一级 + 同页下拉互斥（展开任一先收起其它，
  预览页 Device⇄Resolution 同款获益）。
- PolicyDepthOutput 周期 tick 静默空转（IVA 真机发现，单测漏网案例）：
  tick 闭包经 weak_from_this 捕获而生产接线 make_unique——弱引用恒空，
  米制通道健康但策略显示帧永不产生。修复：强制 shared_ptr 持有 +
  `start()` 显式拒绝无主持有；回归用例固化（unique 持有拒绝、start()
  周期路径真实产出）。

### 测试

- 新增 `test_policy_depth_output`（37 检查）：常值帧端到端量化、无效像素
  填充白、最近邻放大尺寸与索引映射、邮箱门控、O6 拒绝/无效源帧显式
  丢弃、持有契约拒绝、start() 周期路径真实产出；`fake_camera_service`
  去 final 支持用例级通道特化。IVA 真机矩阵：策略输出 seq 推进
  dropped=0、Raw⇄Policy 切换、字号四档含缩放后命中一致性、双下拉两
  方向互扰与互斥、S1-S5 工作流回归 5/5。

## [0.5.3] - 2026-10-08（M13 帧率档位选择与相机热插拔恢复）

### 新增

- 帧率档位选择（M13，用户需求：手动选择输出帧率）。分辨率档位下拉从 M1 的
  "固定 30fps"放开为按设备能力枚举全部 (宽, 高, fps) 组合——彩色与深度成对
  交集（双流成对契约不变，不成对档位如 960x540、848x480@90 不出现），标签
  "宽x高 · 帧率fps"（如 `848x480 · 60fps`），排序高度/宽度降序、同档位帧率
  升序。预览页下拉、预览页数字键 1-9 速选、工作流页源节点胶囊浮层三处同源
  同选；默认档仍为 848x480@30（DEC-004），设备变化时按完整默认视频字段回选。
  枚举逻辑下沉 `apps/viewer/resolution_model.hpp` 纯函数（契约测试
  `test_resolution_model`，独立验证 339 checks 全绿）；源节点胶囊布局
  0.40/0.60 加宽适配 fps 标签（标签列更名 "Resolution"）。真机 D435IF 验收：
  848x480 档 30→60→30 切换实测 30.28→59.51→29.84 Hz，视频-only 请求
  （LRS-20261007-001 台账绕行），新增 hardware 用例 `realsense_fps_hardware`；
  截图存档 `screenshots/m13/`。

### 修复

- 相机热插拔恢复（用户真机反馈：瞬间拔线后视频流终止不恢复）。真因（独立
  验证裁定）：pinned librealsense 的阻塞式 `wait_for_frames` 在设备移除时
  超时后走 hub 自动重连分支，可阻塞 ~15s 或悬挂——检查点不可达，状态停留
  Streaming、画面留末帧。修复：采集等待改为 `poll_for_frames`（契约级非
  阻塞）+ 10ms 自旋，1s deadline 与 3 次失败计数由循环自律执行（50ms 版本
  实证丢 ~25-35% 帧集后收紧）——拔线 ~2s 转 Waiting，插回恢复。
  配套：帧序号改为服务生命周期单调计数（`nextFrameSequence`）——修复引擎
  `committedSeq_` 混域水位在重连序号重置下每 tick 重复捕获的缺陷（独立
  验证实证：旧式重置下一次重连引发 140 个重复帧任务；预览/监看器消费者走
  邮箱内部计数域本不受影响）。viewer 在 Waiting/Failed 稳态排空陈旧画面
  （预览回空态、监看器回占位符）。

### 变更

- M12 代码复用收敛（[PR #45](https://github.com/Linductor-alkaid/rin/pull/45)）：
  全仓库代码复用审查台账 48 项整合 36 项（公共 helper / 内部头 / table 驱动
  下沉，公开 API 形状不变、冻结数值语义不变）；行为唯一允许的变化为修复
  CR-15 Integer 参数端点精度分叉（>2⁵³ 边界两路径量化判定相反）与 CR-35
  监看器排空水位差异，均有对应回归。

## [0.5.2] - 2026-10-07（M11 监看器节点与画布内嵌参数编辑）

### 新增

- 监看器节点（M11，[DEC-021](docs/decisions/DEC-021-monitor-node-and-inline-editing.md)）：
  调色板 View 分组新增 `viewer`（"监看器"），任意类型输出线均可接入
  （`PortType::Any` 通配输入，契约加性扩展：连线兼容 = 两端相等或输入端
  Any；实际图像格式永不为 Any，`elementSize(Any)=0` 天然拒绝）；节点内嵌
  预览窗实时显示输入图像（meta "宽 x 高 · seq N"，最长边 ≤512 降采样，
  每监看器一枚 GPU 纹理；引擎非 Running 整体排空释放）；UI 无输出端口
  （不可拖线）。契约层为 Any→Any 恒等透传（共享像素零拷贝，引擎零改动，
  产物走既有 per-node 邮箱）。
- 画布内嵌参数编辑（M11，ComfyUI/蓝图范式）：节点框内直接显示并编辑自己的
  参数——Boolean 开关、带范围标量滑条 + 当前值回显、枚举点击循环胶囊、
  RealArray 紧凑矩阵网格（R±/C± 步进 + 单元输入）；同步拒绝以参数行 "!"
  标注 + 画布反馈行显式呈现；ROI 联动约束（M6-06）沿用，尺寸源改为
  per-node 驱动产物缓存；控件尺寸/字号随画布缩放。
- 画布指针分发次序反转（M11 可行性基础，pinned EUI-NEO 4691fc0 命中测试
  源码取证）：全视口 mouseArea 先合成（视觉层之下），内嵌交互控件优先
  命中，非交互节点视觉穿透——拖动/框选/连线语义不变，滚轮缩放不受影响。

### 变更

- 监看器视窗大小可调（用户验收追加需求）：右下角手柄**双向**拖拽——宽
  （默认 200、范围 [160,800]）与高（默认 140、范围 [96,640]）节点级独立
  调整，端口圆点/连线终点/命中检测全链路联动，未运行占位符态亦可调；
  预览纹理上限提升至 1024。像素断言验证几何精确（连线终点 1177≈1180、
  边框宽 356→1156≈理论 368/1168）；独立验证发现的绘制层漏传缺陷
  （画/命中分裂）已修复。
- 用户验收反馈（2026-10-07）：相机源分辨率入口改为**下拉浮层选择**（窗口级
  overlay + 选中项 ✓，Esc/外击收起；DEC-021 决策 6 修订——枚举参数维持
  点击循环）。下拉功能经两轮返修：真实根因是**浮层 composer 未接线**
  （resolve 专家升级通道定位：EUI 输入路径无头探针实证正常，此前"mouseArea
  不响应"结论勘误撤销）；连带修复**右键创建菜单自 M5 起从未能打开**的同类
  接线缺口（menuOpen 从未被置位）。修复以像素断言验证（菜单面板
  352×324 精确匹配 2×176×164，基线无面板）。
- 修复**切换分辨率后相机输入停更**：分辨率档位的 enableMotion 粘性恒取
  默认请求 true，`RIN_DISABLE_MOTION=1` 会话下 restream 命令把运动流重新
  带回，在坏 IIO 宿主（LRS-20261007-001）即合成帧饿死、采集零帧；现该
  环境变量为会话级（启动请求与档位粘性同源），切换后帧流持续（实测
  848x480→1280x720 帧计数持续增长）。
- 工作流页五区改四区（右栏移除）：参数编辑就地化（如上）、中间结果由
  监看器承担、工作流总览（FPS/处理/丢弃/在飞 + 过载警示 + 停止冻结标注）
  迁至底部区右列、相机分辨率全局入口迁入相机源节点内嵌（点击循环档位，
  与预览页共享状态与命令）、节点页脚耗时徽标升级 last·avg 双值；docking
  分隔条余调色板宽/底部高两处。
- 节点宽度 148→200（画布坐标），节点高度由参数区/预览窗/实例 RealArray
  形状参与计算（`nodeHeight(descriptor, params)` 纯函数族，绘制与命中共用）。

### 测试

- 独立验证（Independent-Verification-Agent）一轮 PASS：三份 UI 测试映射
  新 API（node_canvas 474 / run_control 273 / param_panel 376 项检查）、
  新建 `test_monitor_any` 171 项（Any 校验全矩阵、恒等透传零拷贝 golden、
  目录顺序冻结、几何族、连线/命中/分组、afterGraphChange 清扫）；四预设
  ctest 32/32 全绿（tsan `setarch -R`；无相机硬件项按既有方式跳过）。
- 真机冒烟（合成帧源 + 脚本化时序 + 截图 `screenshots/m11/`）：内嵌编辑、
  监看器预览随 scale 参数 0.5→0.25 联动（424x240→212x120）、停止排空；
  真相机（D435if）复验：视频-only 请求下双监看器并行实拍（RGB 链 424x240 +
  深度伪彩 848x480 直连，端到端 29.1 fps）、参数联动 212x120 与停止排空实证
  （`m11_realcam_*.png`）；默认 enableMotion 请求在本机被宿主内核 IIO/hid-sensor
  缺口饿死（环境问题，登记 LRS-20261007-001；realsense_hardware 五预设一致
  FAIL 为同一根因，非回归）。

## [0.5.1] - 2026-09-30（M8 深度策略预处理算子；含 M9 米制通道、M10 工作流深度域）

### 新增

- 米制深度策略预处理算子（M8，`M8-02`）：`rin::DepthFrameF32`（float32 单通道
  米制深度帧：共享不可变像素、16 MiB 单帧字节预算、元素 stride 语义，镜像
  `ImageU8` 纪律）与 Core 纯函数算子集——`fillDepthInvalid`（无效深度填充）、
  `resizeDepthArea`（面积加权箱式降采样，仅缩小）、`cropDepth`（裁切）、
  `gaussianBlurDepth`（可分离高斯，reflect-101 边界，σ=0 δ 核恒等）、
  `clipNormalizeDepth`（裁切归一化）、`preprocessPolicyDepthFrame`（冻结顺序
  组合：填充 → 降采样到 raw 网格 → 裁切 → 模糊 → 归一化）与
  `PolicyDepthHistory`（`historyLength` 有界环形历史 + CHW 时序抽样，默认
  37 帧 ring 抽 8 帧输出 (8, 18, 32) float32，欠帧首帧填充）。数值语义以
  外部视觉策略仿真部署参考为唯一权威冻结（[DEC-018](docs/decisions/DEC-018-depth-policy-preproc-operators.md)、
  `docs/design/depth_policy_preproc_design.md`），用于把 RealSense 深度流
  确定性复刻为策略 `depth_encoder` 输入；不进入工作流节点面（DEC-018）。
- golden/交叉验证测试（M8，`M8-03`，Independent-Verification-Agent 两轮）：
  `test_depth_preproc` 309 项检查（逐算子手推 golden + 测试内双精度独立
  参考交叉 ≤1e-6 + 全部拒绝路径）与 `test_public_boundary` 新头文件见证；
  debug/asan/ubsan 三预设全量 ctest 27/27 全绿，零消毒器诊断。

- 米制深度通道贯通与策略深度预览（M9，`M9-02`..`M9-04`）：相机契约加性
  扩展 `DepthMetricSample`/`ICameraService::tryLoadDepthMetric`（float32 米制
  distance_to_image_plane rendition，Z16 × depth_scale，无效像素 0.0）；adapter
  采集 worker 新增米制转换与发布（`depth_metric_convert.hpp` 纯函数）；Core
  预览模型 `PolicyDepthPreviewModel`（M8 冻结管线 + 2×4 历史网格渲染，
  [DEC-019](docs/decisions/DEC-019-metric-depth-channel.md)）；viewer 预览页
  新增 "Policy Depth" 卡片（Executor 周期 tick 驱动冻结管线，流重启自动
  复位历史，最新态邮箱交付渲染线程）。真机米制通道冒烟待用户验收。

- 工作流深度域（M10，[DEC-020](docs/decisions/DEC-020-workflow-depth-domain.md)）：
  `PortType::Depth32F` 端口类型（ImageU8 字节容器承载 float32 米制深度）与
  七个画布节点——`source_depth_metric` 相机米制源、`depth_fill_invalid`/
  `depth_resize`/`depth_crop`/`depth_gaussian_blur`/`depth_normalize`
  （默认参数链 ≡ M8 冻结管线）与有状态 `depth_history`（37 帧环抽 8 帧
  竖直堆叠，欠帧首帧填充；引擎对含状态节点图自动切换串行在飞执行保证
  帧序）；帧源米制路由、Depth32F 缩略图逐帧 P99 显示、深度域端口色。
  M9 固定预览卡片移除（被画布组合能力取代，DEC-020 决策 6）；米制通道
  与契约保留。

## [0.5.0] - 2026-09-29（M4 图像工作流与 M5 工作台 UI；含 M6 输入扩展、M7 可用性）

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
- 工作流输入扩展（M6，[DEC-017](docs/decisions/DEC-017-workflow-camera-sources.md)，
  用户真机验收反馈）：相机源四型——RGB 与深度伪彩（Rgba8）/深度灰度/深度自适应
  （Gray8）固定 rendition，语义与预览配色解耦（相机契约加性扩展
  `FrameKind::DepthJet` 与 `GrayFrame`/`tryLoadGrayFrame`）；灰度域新增
  `crop_gray`/`downscale_gray`（与 Rgba8 版同参数 schema、同冻结数值语义）；
  裁切 ROI 控件联动夹取防呆（已知输入尺寸时 x/y/width/height 按互约束有效域
  夹取并呈现，M4-03 apply 期拒绝保持兜底）；相机源面板分辨率入口（与预览
  选择器共享同一档位/状态/命令，全局 restream 明示）；采集侧深度灰度
  rendition 以单趟双输出转换发布（与单 rendition 函数逐字节等价）。
- 工作流运行控制与假换真集成（M5，`M5-06`，[DEC-016](docs/decisions/DEC-016-contract-first-workbench-order.md)
  交错策略终点）：工作台工作流页改接 `M4-07` 真引擎（`createWorkflowEngine`），
  新增相机帧源接缝（RGB 彩色流"上次已见序号"非阻塞过滤读取 + RGBA8 共享缓冲
  零拷贝接管，多源图各源节点独立水位）；工具栏 Start/Stop 运行控制（启动可用
  条件 = 图校验通过且引擎 Idle/Failed，Failed 先停后重启，准入错误显式反馈；
  停止幂等有界排空；Stopping 全禁用、无暂停）与 Running 下图结构变更"待生效"
  标注（`GraphApplied` 帧边界应用后清除）；契约假引擎（`rin_workflow_fake`，
  `M5-08`）按既定计划随集成移除，`M4-09` 契约与共用测试套件由真引擎延续。
- 工作台导航壳与页面框架（M5，`M5-02`，[DEC-014](docs/decisions/DEC-014-workbench-information-architecture.md)）：
  单窗口四页导航（预览 / 位姿 / 图像工作流 / 设置；左窄边导航栏按台账
  EUI-20260928-001 以 rect/text + viewer 令牌自绘）；页面切换保持各页 UI
  状态，相机服务运行态全局共享；工作流页五区骨架；深度配色选择移入设置页
  （DEC-014 决策 3）；M1 数字键分辨率速选收窄到预览页。
- 节点编辑器画布（M5，`M5-03`，[DEC-015](docs/decisions/DEC-015-node-editor-implementation-path.md)）：
  EUI-NEO 原语自研节点画布——调色板拖出创建（目录分组 + 即输即筛）与右键
  搜索创建菜单、端口拖拽连线（即时类型过滤、悬空输入为搭图中间态只标注不
  阻塞、目标输入已有入边时替换）、贝塞尔带状连线渲染、画布平移缩放（指针
  锚点）与帧全图、框选 / 组拖动 / `Del` 删除 / `Alt+点击` 删线 / `Esc` 取消；
  画布图经 `applyGraph` 与引擎同步（UI 预检与引擎准入共用 `validateWorkflowGraph`
  唯一判据，拒绝原因显式反馈）；交互几何全部下沉为平台无关纯逻辑（单测对象）。
- 参数面板与中间结果查看（M5，`M5-04`）：选中节点的类型化参数编辑
  （Boolean→开关、Integer/Real→滑条+输入、Enumeration→下拉、RealArray→自研
  矩阵网格），编辑经 `requestParamUpdate` 逐参数提交、同步拒绝就地报错
  （"参数下一帧生效"常驻提示）；节点输出缩略图（每节点仅最新一幅、有界 LRU
  缓存、降采样后 GpuFrameView 上屏，显示分辨率与源帧序号）；节点执行失败
  可视化（`NodeFailed` → 画布/面板 destructive 徽标 + 事件列表条目）。
- 性能面板（M5，`M5-05`）：工具栏引擎状态徽标（四态语义色）；节点框底部
  耗时徽标（滚动窗口均值）；右面板工作流总览（端到端 FPS / 累计处理 / 丢弃 /
  在飞，数值取自引擎与 `executor::comm` 统计；丢弃非零以 warning 行显式呈现，
  禁止静默；停止/失败后末次值冻结呈现并标注 stopped）。
- M4 测试矩阵与吞吐实测记录（M4，`M4-08`）：`tools/workflow_bench` 实测工具
  （Release 构建，`RIN_BUILD_TOOLS` 门控，不进 ctest）与记录
  [docs/benchmarks/workflow-throughput-848x480.md](docs/benchmarks/workflow-throughput-848x480.md)
  ——848×480 典型链（grayify→gaussian_blur→fft_lowpass→hist_eq）真引擎吞吐：
  30 fps 相机速率三次独立运行 18 轮 29.94–30.04 fps、零丢弃（余量约 3.4 倍，
  最强可复现结论）；标称频率态饱和 ≈102 fps，FFT 低通为链主导成本
  （10.4–14.9 ms/帧）；降分辨率前置变体把 FFT 降至 1.8–4.3 ms/帧；只记录
  实测值与方法，无性能目标声明；M4 全部工作项与退出条件闭合。

### 修复

- 工作流运行中改图/热更新参数后节点产物停留陈旧值（M7，2026-09-29）：产物
  邮箱为代内设施、每代新建使发布序号从头计，UI"上次已见序号"水位跨代过滤
  掉全部新快照（表现为节点缩略图尺寸/分辨率在改图后不再更新）；引擎产物
  邮箱改为同 id 节点跨代共享（序号连续）并跳过过代帧的邮箱发布（迟到的旧代
  产物不得以更高序号写回），选中节点产物即时刷新。
- 工作流相机源面板分辨率下拉不生效（M7，2026-09-29）：绑定对象在 compose
  栈上构造，下拉回调被 retained 节点持有、点击时才执行，捕获的栈指针悬垂；
  绑定与档位标签移入 ViewerContext 稳定存续，onPick 捕获静态单例地址。
- 工作台可用性（M7，2026-09-29）：调色板条目区改滚动容器（小窗口可下滑）；
  工作流页 palette 宽 / context 宽 / 底部高 / Context 输出块高四处分隔条
  拖拽可调（布局为 UI 私有会话状态）。
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

### 依赖

- kissfft pinned `7bce4153c6bc8aba2db0e889e576f9d00505cbe1`（upstream
  131.2.0，BSD-3-Clause，external 源码构建）：`rin_core` FFT 族节点私有
  后端（DEC-012 基准裁决；master 溢出预检误报缺陷见
  `docs/dependency_feedback/kissfft/ledger.md`）。

### 已知限制

- 六轴 IMU 位姿 yaw 长期漂移（无磁力计的传感器物理限制，界面与文档披露，
  v0.3.0 起持续存在）。
- tsan 插桩 × 真机硬件组合下 `realsense_hardware` 的 IMU 频率恢复断言对
  宿主时序敏感（插桩侵蚀相机/IMU 交付速率，比值贴近断言下限；debug/asan/
  ubsan 确定性通过，M5-05/M6 验证记录）。

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
