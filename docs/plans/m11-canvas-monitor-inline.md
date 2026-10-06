# M11：监看器节点与画布内嵌参数编辑

> 状态：Complete
> 负责人：Linductor-alkaid（授权 Agent 按工程规范自治执行）
> 所属计划：[Rin 实施总计划](rin-implementation-plan.md)
> 前置：M10
> 立项依据：用户需求（2026-10-06，画布块内 context 就地化 + 监看器 + 去右栏）
> 决策：[DEC-021](../decisions/DEC-021-monitor-node-and-inline-editing.md)
> 更新日期：2026-10-06

## 目标

参照 ComfyUI / Unreal 蓝图范式改造工作流页交互（DEC-021）：

1. 画布节点块内直接显示并编辑自己的参数（内嵌控件，去掉右侧参数面板）；
2. 调色板新增"监看器"节点：任意类型输出线可接入，输入图像实时显示在节点内
   预览窗口，UI 无输出端口；
3. 移除右栏，工作流页五区改四区；总览统计迁底部区，分辨率入口迁源节点，
   中间结果查看由监看器承担。

## 方案冻结（详见 DEC-021）

- `PortType::Any` 通配（仅目录声明位；连线兼容 = 相等或输入端 Any；
  `elementSize(Any)=0` 天然拒绝实际图像格式为 Any）。
- 监看器 = 目录 `viewer`（Any→Any 恒等透传，引擎零改动，产物走既有邮箱）；
  UI 隐藏输出端口（`portAt` 跳过）；预览 = per-monitor `GpuFrameView` +
  `tryLoadNodeOutput` pump + `thumbnailRgbaFromSnapshot`（maxDim 512）。
- 画布指针分发反转：全视口 mouseArea 先合成（视觉层下），内嵌控件后合成
  优先命中（EUI hit test：子先于父、同 zIndex 后合成优先、仅 interactive
  拦截；pinned 4691fc0 源码取证）。
- 节点纵向结构：标题 → 参数区 → 端口区 → 页脚；几何纯函数
  `nodeHeight(descriptor, params)` 族（RealArray 行数随实例形状）。
- 控件映射：Boolean→开关；带范围标量→滑条+值文本；枚举→点击循环胶囊；
  RealArray→紧凑网格；ROI 联动沿用（尺寸源 per-node 驱动缓存）。

## 工作项

- [x] `M11-01` 契约层：`PortType::Any`（枚举/toString/elementSize/端口色）+
      `validateWorkflowGraph` 判据 + `runNodeGraph` Any 防御核对 + 目录
      `viewer` 项 + 恒等节点工厂（`makeDefaultImageNode`）。
- [x] `M11-02` canvas_model 纯逻辑：参数区/预览区几何（nodeHeight/nodeBounds/
      portPosition 签名扩展）、connect Any 判据、portAt 跳过监看器输出端口、
      调色板 View 分组、枚举循环取值助手。
- [x] `M11-03` UI 组装：节点内嵌参数控件（param_panel 重构为内联控件 +
      per-node 控件状态）、监看器节点绘制（预览/meta/隐藏输出端口）、
      四区布局（右栏与 context 分隔条删除、总览迁底部右列、分辨率入口迁
      源节点、页脚 last·avg）、画布 mouseArea 合成次序反转。
- [x] `M11-04` pump 与生命周期：监看器预览拉取上传/排空释放、驱动尺寸缓存
      刷新、节点删除清理、shutdown 排空（GL 引用释放顺序不变）。
- [x] `M11-05` 测试（Independent-Verification-Agent）：契约（Any 判据全矩阵、
      viewer 透传 golden、目录签名）、画布纯逻辑（几何/连线/命中/分组）、
      参数纯逻辑回归、全量 ctest debug/asan/ubsan/tsan。
- [x] `M11-06` 真机冒烟（脚本化交互时序 + 截图存档 `screenshots/m11/`）、
      文档收口（ui_workspace_design/image_workflow_design 补记、CHANGELOG、
      总计划状态）、MR。

## 测试与退出条件

- [x] 契约：Any 输入接受三型、Any 输出连具体输入拒绝、实际格式 Any 拒绝
      （make/wrap）、viewer 恒等零拷贝 golden、防御核对（声明 Any 接受任意
      具体格式产出）。
- [x] 画布：带参节点高度/端口偏移、RealArray 形状变化联动、viewer 几何、
      Any 连线接受/类型过滤、监看器输出端口不可命中、View 分组。
- [x] 参数：既有 param_model 纯逻辑回归全绿（未破坏）；内联新增纯逻辑
      （几何/循环）覆盖。
- [x] 全量 ctest 四预设全绿。
- [x] 真机：拖入监看器 → 任意输出接入 → 启动 → 节点内实时预览随 seq 推进；
      画布内滑条/枚举循环/矩阵网格编辑即时生效（下一帧）；停止排空（预览
      清空、总览冻结标注）；四区布局无右栏。
- [x] 文档同步齐备。

## 验证记录

2026-10-06：立项。可行性调研完成（主循环）：EUI-NEO pinned 4691fc0 命中
测试源码取证（`core/runtime/runtime_input.h`：自顶向下、子先于父、仅
interactive 拦截、兄弟 zIndex 稳定排序）——画布全视口 mouseArea 先合成即可
让内嵌控件优先命中，交互范式迁移可行；引擎产物通道（M7-02 跨代邮箱）对
恒等透传节点零改动可用。方案冻结于 DEC-021。

2026-10-06：`M11-01`..`M11-06` 完成关闭。

**实现**（主循环）：契约层 `PortType::Any`（toString/elementSize=0/校验判据"相等或输入端 Any"/runNodeGraph Any 防御）+ 目录 `viewer`（Any→Any，追加于末项后）+ 恒等节点（`makeDefaultImageNode`，共享像素零拷贝；`singleInput` 格式核对不适配 Any，独立实现输入防御）；`canvas_model` 几何族扩展（`nodeHeight/nodeBounds/portPosition(descriptor, params)`、参数区/预览窗/相机源分辨率行、RealArray 实例形状联动）+ connect Any 判据 + `portAt` 跳过监看器输出端口 + View 分组；UI 重组：`node_canvas.hpp` 吸收 `param_panel.hpp`（右栏删除，四区组装、总览迁底部右列、画布 mouseArea 先合成=内嵌控件优先命中、内嵌参数控件按 ParamKind 映射、枚举点击循环、监看器预览窗、页脚 last·avg）；pump：`pumpMonitorViews`（逐监看器拉取上传/排空释放/条目清理，GL 仅渲染线程）+ `pumpDriverSizes`（ROI 联动尺寸源）+ shutdown 排空序更新。

**测试**（Independent-Verification-Agent，一轮全绿）：三份受影响测试映射新 API（node_canvas 474 / run_control 273 / param_panel 376 项检查，含新增 cycleEnumOption 与监看器常量）；新建 `test_monitor_any` 171 项（Any 校验全矩阵、elementSize/make/wrap 拒绝、目录签名与顺序冻结、恒等透传零拷贝 golden 三格式、runNodeGraph 集成、canvas_model M11 几何/连线/命中/分组、afterGraphChange 清扫）。四预设 ctest 32/32 全绿（tsan 按仓库惯例 `setarch -R`；realsense_hardware 无相机按既有方式跳过）。未发现 M11 产品缺陷。

**真机冒烟**（无相机环境，合成帧源 + 脚本化时序注入既有交互接缝 + glReadPixels 截图，临时补丁已还原）：四张截图存档 `screenshots/m11/`——`m11_graph`（四区布局、source→downscale→viewer 连线、内嵌参数呈现、"graph valid"）；`m11_running_monitor`（监看器实时预览 424x240 · seq 推进、Overview 30.5 fps/processed 推进、内嵌滑条与枚举胶囊）；`m11_param_updated`（scale→0.25 后预览 212x120 即时联动，下一帧生效实证）；`m11_stopped_drained`（停止排空：预览回占位符、总览冻结标注 stopped、Idle + [stopped] 事件）。无右栏、监看器无输出端口（不可拖线）核实。

2026-10-07：**真机复验（D435if）完成**——两轮，第一轮暴露宿主环境缺口并
登记台账，第二轮（视频-only 请求）通过。

**第一轮（默认 enableMotion=true，暴露环境缺口）**：引擎 Running 但零帧
（截图复核实证：Overview fps 0.0/processed 0，监看器占位符）。根因经
Independent-Verification-Agent 受控实验链定位：本机内核 7.0.0-34-generic 的
hid-sensor-hub/IIO 通路不出 IMU 数据（`in_accel_x_raw` 恒 0 + 内核
`No report with id 0xffffffff found` 日志），librealsense 2.58.3 启用运动流后
合成帧同步器等不到运动帧、`wait_for_frames` 永久超时，整条 pipeline 连带
视频被饿死（探针：关运动流后 8 s 得 223/223 帧 ≈27.9 fps，视频链健康）。
**首轮截图曾一度被主循环误读为"实拍 30.4 fps"，经独立验证交叉质疑与放大
裁剪复核更正**（fps 0.0/processed 0），存档证据以裁剪复核为准。登记
[LRS-20261007-001](../dependency_feedback/librealsense/ledger.md)。

**真相机在位条件下的全量测试矩阵**（Independent-Verification-Agent）：五预设
（debug/asan/ubsan/tsan-setarch/release）构建成功，除 `realsense_hardware`
外 31/31 全绿、零消毒器诊断；`realsense_hardware` 五预设一致 FAIL（117 检查
27 失败，全部为同一饿死根因的数据面级联；控制面见证正常：设备枚举/内参/
IMU 目录/停止闭合），分级判定为环境前置缺失（非测试缺陷、非产品回归），
详见 LRS-20261007-001。

**第二轮（视频-only 请求绕行，enableMotion=false）通过**：主循环冒烟
（脚本化时序注入既有交互接缝 + glReadPixels 截图，临时补丁已还原）重跑，
三张截图存档 `screenshots/m11/m11_realcam_*.png`——`running`：双监看器并行
实拍（RGB 链 source→downscale→监看器 424x240 · seq 推进的房间实拍 +
source_depth_jet→监看器 848x480 jet 深度色图直连，"任意输出线均可接入"
真机实证；Overview 端到端 29.1 fps/processed 156/0 丢弃）；`param`：内嵌
滑条路径 scale→0.25 后监看器预览即时 212x120 · seq 351（下一帧生效真机
实证）；`stopped`：停止排空（预览回占位符、总览冻结标注 stopped、
end-to-end 0.0 fps 冻结呈现）。运动流真机验收（M3-08 项）在本机被
LRS-20261007-001 阻断，补跑条件见台账。

2026-10-07：**用户真机验收反馈两项处理完毕**（手动测试 RIN_DISABLE_MOTION=1
会话）。

**反馈 A（交互）：相机源分辨率入口期望下拉选择**。落地：源节点内嵌行改为
下拉触发器（值 + ▾），点击打开窗口级选项浮层（`composeWorkflowResolutionMenu`，
创建菜单同款 overlay 模式：scrim + 面板 + 选中项 ✓，Esc/画布外击收起，选项
点击经既有 onPick 命令路径）；DEC-021 决策 6 相应修订（枚举参数维持点击
循环）。

**反馈 A 追加两轮返修（真实根因经 resolve 专家升级通道定位）**：首版与
二版（mouseArea.onTap → rect.onClick）均"悬停正常、点击无响应"——两次
均为**浮层 composer 从未接线**（app.cpp overlay 层缺失调用；当时接线脚本
中断言失败未写盘，被误判为已落地），回调实际已触发、无人消费状态；
"components::mouseArea 在此嵌套下不响应"的结论不成立（EUI 输入路径经
无头探针 1:1 复刻实证正常，DEC-021 决策 4 分层设计无罪）。连带发现同类
潜在缺口：**右键创建菜单自 M5-03 起 menuOpen 仅被消费从未被置位**（历史
脚本化验证只直改状态，真实右键从未打开过），一并接线。修复四件套：
overlay 调用 / Esc 收起 / 右键置位（PressResult ContextMenu 分支，窗口
坐标）/ 创建菜单与分辨率浮层互斥。验证改用**像素断言**（不再依赖目测，
此前两次目测误读教训）：程序化置位同字段后，分辨率菜单面板检测
352×324 px（=2×176×164 精确匹配）、创建菜单 380×1330、基线无面板；
点击路由侧由升级分析的无头探针证据背书。截图
`screenshots/m11/m11_resolution_menu.png`（本轮真实浮层，含选中 ✓）。

**反馈 B（缺陷）：切换分辨率后相机输入不再更新**。根因链（主循环仪表化
复现 + 裸 librealsense 探针对照定位）：分辨率档位的 `enableMotion` 粘性取自
`kDefaultRequest`（恒 true）——`RIN_DISABLE_MOTION=1` 会话下，restream 命令
把运动流重新带回，在本机坏 IIO 环境（LRS-20261007-001）即合成帧同步器饿死：
restream 分支全部成功返回、服务保持 Streaming，但 `wait_for_frames` 永久超时
（3 次后 Fatal 重试循环），采集零帧。裸探针复刻适配器全部调用序列（默认构造
pipeline/enable_device/独立枚举 context/内参读取）不换档正常、换档亦正常，
排除 librealsense 序列差异，锁定 enableMotion 粘性泄漏。修复：`RIN_DISABLE_MOTION`
升级为会话级标志（`ViewerContext::motionDisabledByEnv`），启动请求与分辨率
档位粘性同源（`rebuildResolutionOptions` 同步消费）。运行验证：切换
848x480→1280x720 后帧计数持续增长（227→632/14s）、引擎保持 Running、
"resolution applied"。

2026-10-07（晚）：**用户验收通过后追加需求：监看器视窗大小可调**。落地：
`CanvasNode::monitorPreviewHeight`（节点级 UI 私有状态，position 先例；默认
kMonitorPreviewHeight=140，范围 [96,640]，拖拽写入即夹取）；几何族
nodeHeight/nodeBounds/portPosition/portsOffsetY 增尾参 monitorPreviewHeight
（默认参数保持旧调用兼容，模型命中/框选/连线全链路携带节点值）；预览窗
右下角缩放手柄（三道斜杠视觉 + mouseArea 拖拽，dockDrag 槽位 2 与页面
分隔条互斥；向下拖增高，画布单位增量随缩放换算），端口锚点随高度联动
（revision 驱动连线重绘）；未运行占位符态亦可缩放。验证（像素差分断言）：
程序化置 height=400 后节点底缘 y=1216=(108+40+460)×2 精确匹配公式，
默认底缘 696=(148+200)×2；拖拽手势归用户验收。

2026-10-07（深夜）：**用户验收追加：视窗宽度亦可调（双向缩放）+ 绘制层
分裂缺陷修复**。① 手柄改双向拖拽（totalX/totalY 累计增量换算画布单位，
宽 [160,800]×高 [96,640] 写入即夹取，`CanvasNode::monitorWidth` 默认
kNodeWidth，仅 viewer 生效）；nodeBounds/portPosition 增宽度尾参（默认
kNodeWidth 兼容旧调用），模型命中/框选/连线与绘制端口圆点/连线终点/
拖线预览全链路同源；预览纹理上限 512→1024（宽窗清晰度）。② 独立验证
（缩放轮）发现绘制层 4 处调用漏传 previewHeight（画/命中分裂：端口圆点
与连线终点停默认高度）——已修复并像素断言（连线终点 y=1177≈1180）。
双向缩放像素断言：预览边框横向 run 356→1156px ≈ 理论 368/1168，起点
x=1310 精确匹配；拖拽手势归用户验收。

**遗留观察**（非 M11 回归）：M10 深度域 typeId（`source_depth_metric`/`depth_*`）不在 `paletteGroupFor` 分组表，调色板落 "Other"（HEAD 同此，M10 遗留呈现缺口，建议择期登记修复）；conv_kernel 的 size 枚举切换不自动重排 kernel 形状（M5-04 既有语义保持，切换后需手动 R+/C+ 步进，否则构造期拒绝经 NodeFailed 呈现）。
