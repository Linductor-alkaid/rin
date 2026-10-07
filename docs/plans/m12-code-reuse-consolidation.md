# M12：代码复用收敛

> 状态：In Progress
> 负责人：Linductor-alkaid（授权 Agent 按工程规范自治执行）
> 所属计划：[Rin 实施总计划](rin-implementation-plan.md)
> 前置：M11
> 立项依据：用户需求（2026-10-07，全仓库代码复用问题审查并按序修复）
> 更新日期：2026-10-07

## 目标

消除 2026-10-07 全仓库代码复用审查（三路并行分析 + 机械克隆检测交叉证实）发现的
重复代码与被绕过的既有抽象，重点是 4 处已经产生语义分叉的"危险重复"。整合方式
限于抽取公共 helper/内部头/模板/table 驱动，不改变任何公开契约、冻结数值语义与
事件文案；行为唯一允许的变化是修复审查确认的缺陷（CR-15 Integer 精度分叉、
CR-35 监看器排空 lastSeen 差异，见各工作项）。

## 范围与非目标

范围：

- `src/core/`、`src/adapters/realsense/`、`src/workflow/`、`apps/viewer/`、`tests/`、
  `tools/` 内审查登记的全部 CR 条目（见问题台账）。
- 每项整合附带受影响测试的回归；测试执行委派 Independent-Verification-Agent。

非目标：

- 不修改 `third_party/`（含 pinned kairo、kissfft、librealsense2、EUI-NEO）。
- 不改公开 API 形状（`include/rin/` 头文件对外签名不变；新增内部 helper 不外泄）。
- 不整合 `pose_math.cpp` float 四元数与 `imu_fuser.cpp` double 四元数（CR-13）：
  两者分属公开层与 detail 层、均为冻结公式，跨层模板化需先做分层决策，收益一般，
  暂缓并保留台账记录。
- 不做性能优化或纯格式化（与复用无关的顺手项不纳入）。

## 设计与决策依据

- [工程规范](../project/project-standards.md)第 5、6、7、8 节（完成定义、模板、验证证据、文档同步）。
- 审查证据：2026-10-07 会话内审查报告（三路 general-purpose 分析 agent 全文阅读
  审范围内全部文件 + 归一化 9 行滑窗克隆检测 61 个 ≥11 行克隆区交叉证实）。
- 冻结数值语义出处：M4-04（reflect-101、quantizeU8）、M8（深度预处理算子与
  O7 环形历史）、DEC-013（参数校验单一事实源）、DEC-021（监看器节点）。
- `image_node.cpp` 的 `typedParam<T>` 与 `depth_metric_convert.hpp` 为既有收敛
  范例，新 helper 沿用其形态（内部命名空间 + `[[nodiscard]]`）。

## 问题台账（CR）

编号 `CR-NN` 为本里程碑内的稳定引用；一经引用不得复用。严重度沿用审查定义：
P1（明显应立即整合）/ P2（值得整合）/ P3（可顺手做）。标注"已分叉"的条目表示
两份副本已出现行为不一致，除整合外还需裁决正确语义。

| 编号 | 严重度 | 位置 | 问题 | 关联工作项 |
| --- | --- | --- | --- | --- |
| CR-01 | P1 | `src/core/pixel_format.cpp:71-123 / 280-331 / 372-423` | Z16 P99 分位计算整段三份拷贝（~160 行），注释自认同语义 | M12-03 |
| CR-02 | P1 | `src/core/image_ops.cpp:286-299` vs `src/core/depth_preproc.cpp:44-57` | reflect-101 冻结公式双份且守卫漂移（`size<=1` vs `size==1`） | M12-04 |
| CR-03 | P2 | `src/core/image_ops.cpp:546-563` vs `src/core/depth_preproc.cpp:284-298` | 一维高斯核构造双份（同一冻结公式） | M12-04 |
| CR-04 | P2 | `src/core/image_ops.cpp` 9 处（68-87、154-176、308-330、407-412、453-472、798-802） | required 参数读取样板 ×9（~55 行） | M12-05 |
| CR-05 | P2 | `src/core/image_ops.cpp:873-974` | 5 个深度域节点包装器结构克隆（~90 行） | M12-06 |
| CR-06 | P2 | `src/core/image_ops.cpp:994-1056` vs `src/core/depth_preproc.cpp:133-150、399-429` | O7 环形历史抽样下标/入环推进/约束校验三段双份 | M12-07 |
| CR-07 | P2 | `src/core/pixel_format.cpp:180-188 / 237-245 / 273-278 / 360-368` | Z16 入口参数校验样板 ×4 | M12-03 |
| CR-08 | P3 | `src/core/image_ops.cpp:1113-1176` | `makeDefaultImageNode` 工厂 18 连 if 可表驱动 | M12-18 |
| CR-09 | P3 | `src/core/image_ops.cpp:257-259` vs `61-64` | bilinear 内联重写已有 `quantizeU8` | M12-18 |
| CR-10 | P3 | `src/core/pixel_format.cpp:260-262 / 344-345 / 440-447` | Gray8 ramp 公式三处内联（纯函数已在 20-38 行） | M12-03 |
| CR-11 | P3 | `include/rin/camera_types.hpp:85-88、103-106`；`src/core/workflow_types.cpp:128-137`；`src/core/depth_preproc.cpp:96-99` | 帧元数据 `valid()` 四处平行实现 | M12-18 |
| CR-12 | P3 | `src/core/imu_fuser.cpp:20-36` | `allFinite` 对 `array<double,4>`/`array<double,3>` 双胞胎 | M12-18 |
| CR-13 | P3 | `src/core/pose_math.cpp:18-44` vs `src/core/imu_fuser.cpp:39-61` | 四元数乘/归一化 float/double 两套——**暂缓**（分层决策，见非目标） | 暂缓 |
| CR-14 | P3 | `src/core/depth_preproc.cpp:154-168` | `policyWidth/policyHeight` 孪生函数 | M12-18 |
| CR-15 | P1（已分叉） | `src/core/workflow_types.cpp:141-182` vs `src/workflow/param_check.hpp:14-61` | 参数校验规则双实现；`param_check.hpp:11` 自称"单一事实源（DEC-013）"但 Integer 边界一处 `double` 量化、一处 `long double`，边界 >2⁵³ 时两路径判定相反 | M12-01 |
| CR-16 | P1 | `src/adapters/realsense/realsense_camera_service.cpp:782-797 / 806-823 / 1133-1135(+706-719)` | 流重建序列（stop→startPipeline→motion 门控复位→重发内参）三处复制 | M12-08 |
| CR-17 | P2 | `src/adapters/realsense/realsense_camera_service.cpp:1031-1094` | 三个 `request*` 命令方法 ~75% 相同，错误串逐字 ×3，白/黑名单写法不统一 | M12-09 |
| CR-18 | P2 | `src/adapters/realsense/realsense_camera_service.cpp:654-684` vs `723-757` | 命令/热插拔消费逻辑双份，事件文案逐字重复 | M12-09 |
| CR-19 | P2 | `src/adapters/realsense/realsense_camera_service.cpp:766-772` vs `865-871` | "设备移除→Waiting"块 6 行 100% 相同 ×2 | M12-09 |
| CR-20 | P2 | `src/workflow/engine.cpp:405-421` vs `428-441` | buildNodeGraph 调用+失败消息格式化双份 | M12-10 |
| CR-21 | P2 | `src/workflow/engine.cpp:199-206` vs `221-228` | requestParamUpdate 两分支预编译块 8 行 ×2 | M12-10 |
| CR-22 | P2 | `src/adapters/realsense/realsense_camera_service.cpp:468-523` | publishFrame 两个重载 + publishGrayFrame 同骨架互不复用 | M12-09 |
| CR-23 | P2 | `tests/workflow_engine_contract_suite.hpp:66-91`、`tests/test_workflow_depth_engine.cpp:81-92`、`tests/test_run_control.cpp:72-150`、`tests/test_perf_panel.cpp:69-80`、`tests/test_param_panel.cpp:83-94`、`tools/workflow_bench/workflow_bench.cpp:211-224` | `pollUntil` ×6、`quietFor` ×2 手抄；bench 变体语义略异需注释互指 | M12-11 |
| CR-24 | P3 | `src/adapters/realsense/realsense_camera_service.cpp:40-44` vs `src/workflow/engine.cpp:39-43` | `steadyMs()` 逐字 ×2 | M12-10 |
| CR-25 | P3 | `src/workflow/engine.cpp:114-119` vs `src/core/node_graph.cpp:31-33` | maxNodes 检查重复（预检与内部检查） | M12-10 |
| CR-26 | P3 | `tools/fft_bench/fft_bench.cpp:376-457` | 三后端基准块脚手架 ×3 + 容差字面量散布（1e-3 ×3、5e-3 ×2） | M12-19 |
| CR-27 | P3 | `src/workflow/engine.cpp:37` vs `tools/workflow_bench/workflow_bench.cpp:432-433` | 统计窗口 32 的知识跨文件硬编码 | M12-19 |
| CR-28 | P3 | `src/workflow/default_catalog.hpp`（~15 处 hasRange/min/max 三连；131-147 vs 313-329） | 参数描述符样板冗长 + blur schema 重复 | M12-19 |
| CR-29 | P3 | `src/adapters/realsense/imu_motion_ingest.hpp:61-68`、`.cpp:28-59` | gyro/accel 镜像字段与 bool 三元分派 | M12-19 |
| CR-30 | P3 | `src/workflow/engine.cpp:309-325 / 799-832 / 851-870` | future 排空捕获位重复 ~8 行；catch 链平行（处置语义不同，仅抽 consumeFuture） | M12-10 |
| CR-31 | P3 | `src/adapters/realsense/realsense_camera_service.cpp:924 / 967` | 同一指针重复 `reinterpret_cast` | M12-09 |
| CR-32 | P1 | `apps/viewer/node_canvas.hpp:320-332` vs `623-632`、`1921-1930` | `InputStyle` 三处逐字段复制；helper 已存在却仅 1 处使用 | M12-14 |
| CR-33 | P1 | `apps/viewer/imu_panel.hpp:95-112`；`apps/viewer/app.cpp:784-801、893-910、929-946`；`apps/viewer/node_canvas.hpp:606-619、1772-1792`（同族变体 app.cpp:604-617、pose_view.hpp:237-242+283-290、node_canvas.hpp:474-488） | "卡片底+标题栏"面板脚手架 ×8（合计 ~120-160 行），仅 id/标题/圆角档不同 | M12-14 |
| CR-34 | P1 | `tests/test_run_control.cpp:159-273` vs `tests/test_workflow_depth_source.cpp:57-127`（另有 test_public_boundary.cpp:22 NullService） | FakeCameraService 双副本 ~150 行，stop 排空/水位语义各测一半 | M12-12 |
| CR-35 | P2（已分叉） | `apps/viewer/app.cpp:425-436` vs `apps/viewer/node_canvas.hpp:2216-2226`（shutdown 份 app.cpp:533-537） | 监看器排空循环三份；活动排空两份除 `monitor.lastSeen = 0` 一行外逐字相同——需裁决哪份正确后统一 | M12-02 |
| CR-36 | P2 | `apps/viewer/node_canvas.hpp:1895-1920` vs `2041-2064`；阴影 `shadow(18,10,8,{0,0,0,0.35})` ×3（app.cpp:751、node_canvas.hpp:1919、2063） | 模态浮层壳（钳制+scrim+面板+阴影）×2；viewer_theme.md 明定"阴影只用于浮层"却无阴影令牌 | M12-15 |
| CR-37 | P2 | `apps/viewer/app.cpp:449-468` vs `tests/test_run_control.cpp:508-528` | pump 工作流事件消费块留在 app.cpp 不可测，测试内联复刻生产逻辑（注释自认），生产一改测试不失败 | M12-16 |
| CR-38 | P2 | `apps/viewer/imu_panel.hpp:90-93、130-147、155-162` vs `apps/viewer/app.cpp:779-782、809-836` | 名称/值行列表布局与样式整段复制；魔法数 `96.0f`/`130.0f` 双份 | M12-17 |
| CR-39 | P2 | `runSection` ×5（test_node_canvas.cpp:180、test_param_panel.cpp:73、test_monitor_any.cpp:67、test_perf_panel.cpp:63、test_run_control.cpp:117）；`nearF/nearD/nearPoint/kTol` ×4；`hasKind/hasIssueOn` ×4 | 测试断言 helper 散落（test_util.hpp 仅含 3 个宏） | M12-11 |
| CR-40 | P2 | `makeRgba`（histogram:180-193 ≡ geometry:266-279）、`makeGray`（histogram:196-208 ≡ fft:230-241 ≡ convolution:262-273）、`ByteLcg`（fft:243-251 ≡ histogram:270-278；depth_preproc.cpp:227 内联同常量） | 图像测试夹具跨 5 文件逐字复制 ~80 行 | M12-13 |
| CR-41 | P2 | `tests/test_workflow_engine.cpp:100-118` vs `tests/test_perf_panel.cpp:149-165` | 确定性图案帧夹具 ×2（仅尺寸不同） | M12-13 |
| CR-42 | P2 | 透明色字面量 ×7（navigation.hpp:140、node_canvas.hpp:690、1814、1903、1955、2050、2079、2145）；悬停行 ×5；下拉选择语义两套并行（app.cpp:702-775 composeSelect vs node_canvas.hpp:2027-2116） | 主题层缺 `transparent` 令牌被绕过；composeSelect 绕行原因（EUI-20260923-003）仅记录在 app.cpp 一处 | M12-15 |
| CR-43 | P3 | `perf_model.hpp:29-40`、`imu_panel.hpp:57-81`、`param_model.hpp:51-55`、`app.cpp:251-261` | snprintf 格式化样板 ×6 | M12-20 |
| CR-44 | P3 | `apps/viewer/app.cpp:325-360`、`1041-1078` | applyXChoice 三胞胎 + overlay 开合/选择闭包样板 ×3 | M12-20 |
| CR-45 | P3 | `apps/viewer/app.cpp:563-573` vs `node_canvas.hpp:506-520`；`node_canvas.hpp:483-586` | 状态徽标 ×2；工具栏魔法 x 偏移（344/452/120）同函数内多点硬编码 | M12-20 |
| CR-46 | P3 | `apps/viewer/workflow_frame_source.hpp:113-180`；makeSnapshot 变体 ×5（test_param_panel.cpp:97-113、test_workflow_contracts.cpp、test_workflow_depth_source.cpp:136-154、test_pose_view_state.cpp:90、test_imu_panel.cpp:111） | 包装尾部重复；快照构造可带默认参 builder | M12-20 |

机械克隆检测（归一化 9 行滑窗，≥11 行克隆区）独立证实的高置信条目：CR-01、CR-23、
CR-34、CR-39、CR-40、CR-41（tests/ 占 61 个克隆区中的绝大多数）。

### 状态注记（2026-10-07 首轮修复收口）

已整合（36 条）：CR-01..07、09、12、14、15、16..25、27、30..42、47（登记）、
48（删除）。暂缓/保留（10 条 + 3 变体）：

- CR-08（表驱动工厂）、CR-26（fft_bench 基准 helper）、CR-28（default_catalog
  参数工厂）、CR-29（imu ingest 对称字段）、CR-43（formatText）、CR-44
  （requestOrReport）、CR-45（状态徽标/工具栏偏移）、CR-46（帧源包装/快照
  builder）：P3 遗留，归属 M12-18/19/20 未完成部分，维持 Planned。
- CR-11（帧元数据 valid 四处平行）：**不整合**——四处分布在不同公共/内部头，
  共享 helper 需扩大公开面或新建公共头，成本大于 4 行/处模式的收益；各处
  minStride 逻辑本有格式差异。
- CR-13（四元数 float/double 两套）：暂缓（见非目标；解除条件不变）。
- CR-33 变体：issues 卡（标题宽不同）、工具栏卡（kFontBase 标题）、viewCard
  （双标签叠加）为有意布局变体，不套 composeCardShell。
- CR-42 部分：悬停行脚手架五处差异大（条件悬停/不同捕获），不强行抽
  composeHoverRow；透明色字面量已全部令牌化。composeSelect 下沉未做。
- CR-47（验证轮新发现）：Integer 参数端点为 double，声明 >2^53 端点即量化；
  如需精确端点须 DEC 变更公开契约（int64 端点），不在 M12 范围。

## 工作项

修复顺序即下列编号顺序（与审查报告建议顺序一致）。每个工作项完成后由
Independent-Verification-Agent 独立回归，并在"验证记录"追加条目。

- [x] `M12-01`（Integer 边界统一 long double；端点 double 量化契约限制登记 CR-47） CR-15：把参数值匹配规则下沉为 Core 唯一实现（`workflow_types.cpp` 导出
  `paramValueMatches`，`param_check.hpp` 转发或删除），`validateWorkflowGraph` 与两个
  引擎的 `requestParamUpdate`/帧边界复核共用；Integer 边界统一为 `long double` 语义
  （以 `param_check.hpp` 注释的精度意图为准），消除 >2⁵³ 边界两路径判定相反。
- [x] `M12-02`（裁决：差异为有意语义（相机服务水位保留 vs 引擎复位），参数化统一 + 注释互指） CR-35：核实 app.cpp Waiting/Failed 排空与 node_canvas 非 Running 排空的
  `lastSeen` 复位差异哪份符合 DEC-021/M11 冻结语义，统一为单一
  `drainMonitorViews(canvas, resetWatermark)` 并让三处调用共用；若差异为缺陷按修复回归，
  若为有意行为在两处补注释并保留参数。
- [x] `M12-03` CR-01、CR-07、CR-10：pixel_format.cpp 批量收敛——抽
  `computeFrameQuantile()`（P99 三份）、`validateDepth16Args()`（入口校验 ×4）、
  `grayLevel()`（ramp ×3）；数值行为逐位不变，由 test_pixel_format 回归保证。
- [x] `M12-04`（守卫统一 size<=1→0（size<=0 分叉不可达，已论证）） CR-02、CR-03：新建 `src/core/` 内部头（仿 `pixel_format.hpp` 先例）承载
  reflect-101 下标与一维高斯核构造唯一实现，image_ops/depth_preproc 两边引用；
  reflect 守卫统一为 `size <= 1 → 0`（两侧对 size≤0 的旧行为分叉按 image_ops 侧防御
  语义统一，size≥1 行为不变）。
- [x] `M12-05` CR-04：image_ops.cpp 抽 `requiredParam<T>`（内部走 `typedParam`），
  9 处"缺失/种类错位即抛"样板收敛为薄包装；范围校验保留调用点。
- [x] `M12-06` CR-05：深度域五个无状态节点包装器收敛为单一模板基类
  （构造注入算子 callable），`DepthHistoryNode` 保持独立。
- [x] `M12-07`（分组消息保留 + historyParamsValid 总判据兜底） CR-06：环形历史抽样下标公式、入环推进与历史约束校验抽为
  depth_preproc 共享函数，`DepthHistoryNode` 与 `PolicyDepthHistory` 共用。
- [x] `M12-08`（首次打开路径生命周期不同保持独立（注释互指），收敛范围为准 restream/设备切换两处） CR-16：realsense_camera_service 抽 `rebuildStream()`（stop/startPipeline
  motion 门控/resetStreamState/重发内参），restream、设备切换、首次打开三处调用。
- [x] `M12-09` CR-17、CR-18、CR-19、CR-22、CR-31：adapter 小抽取——
  `sendControlCommand()`（统一白名单）、`consumeCommand()`/`drainHotPlug()`、
  `handleDeviceRemoved()`、publishFrame 模板化并让右值版委托 shared_ptr 版、
  复用 `depthData` 局部变量。
- [x] `M12-10`（CR-25 按预案取注释互指（预检使拒绝先于状态分支可见，不删）） CR-20、CR-21、CR-24、CR-25、CR-30：engine/adapter 小抽取——
  `formatBuildFailure()`、`checkedParamApply()`、`steadyMs()` 入 src 内部小头、
  maxNodes 预检处理（信任 buildNodeGraph 唯一判据或注释互指）、`consumeFuture()`。
- [x] `M12-11`（hasKind 拆 validation_test_util.hpp 保持 test_util 零依赖；bench/hardware 变体保留互指） CR-23、CR-39：`pollUntil`/`quietFor`/`runSection`/`nearF` 族/`hasKind`
  族下沉 `tests/test_util.hpp`，6 个手抄副本改引共享版；workflow_bench 变体保留并
  注释互指来源与语义差异。
- [x] `M12-12` CR-34：新建 `tests/fake_camera_service.hpp`，单一可配置槽位
  （Frame/GrayFrame/DepthMetric 可选启用）的 FakeCameraService，stop 排空与水位
  语义唯一化，test_run_control/test_workflow_depth_source 改用。
- [x] `M12-13` CR-40、CR-41：新建 `tests/image_test_util.hpp`（makeRgba/makeGray/
  ByteLcg/确定性图案帧），5 个 test_image_ops_* 与 test_workflow_engine/
  test_perf_panel 改用。
- [x] `M12-14`（6 处同构站点收敛；issues 卡/工具栏/viewCard 有意变体保留（见台账注记）） CR-32、CR-33：viewer 抽 `composeCardShell()`（卡片+标题栏，返回内容区
  几何）与 `inlineInputStyle` 加 radius 参数，8 处卡片脚手架与 2 处 InputStyle 内联
  副本收敛。
- [ ] `M12-15` CR-36、CR-42：viewer_theme 补 `kOverlayShadow`/`transparent` 令牌与
  `composeFloatingPanel()`、`composeHoverRow()`；浮层壳 ×2、透明色 ×7、悬停行 ×5
  收敛；composeSelect 下沉共享并集中记录 EUI-20260923-003 绕行依据。（部分完成：令牌/浮层壳/透明色已收敛，composeSelect 下沉未做——绕行依据注释已集中于实现处。）
- [x] `M12-16` CR-37：app.cpp pump 工作流事件消费块提为 node_canvas.hpp 的
  `pumpWorkflowEvents()`（平台无关、可测），app.cpp 与 test_run_control 共同消费
  真实现，删除测试内联复刻。
- [x] `M12-17` CR-38：抽 `composeLabeledRows()`，`96.0f`/`130.0f` 收进具名常量，
  imu_panel 与内参卡共用。
- [x] `M12-18`（部分完成：CR-09/12/14 已整合；CR-11 暂缓（公开头膨胀成本大于 4 行收益）；CR-08 未做） CR-08、CR-09、CR-11、CR-12、CR-14：core P3 杂项——表驱动工厂、
  bilinear 复用 `quantizeU8`、帧元数据 `valid()` 公共判定、`allFinite` 模板化、
  `policyWidth/Height` 合并。
- [x] `M12-19`（部分完成：CR-27 已整合；CR-26/28/29 未做） CR-26、CR-27、CR-28、CR-29：bench/catalog/ingest P3 杂项——fft_bench
  基准 helper 与容差常量、`kStatsWindow` 导出、default_catalog 参数工厂、
  imu ingest 对称字段按 kind 索引。
- [ ] `M12-20` CR-43、CR-44、CR-45、CR-46：viewer P3 杂项——`formatText()`、
  `requestOrReport()`、`composeStateBadge()`、工具栏偏移局部常量、
  workflow_frame_source 包装收敛、快照 builder。

CR-13（四元数跨层模板化）暂缓，不设工作项；解除条件：出现跨 float/double 复用的
实际需求并经分层决策确认。

## 风险与阻塞

- 冻结数值语义（M4-04/M8/DEC-013）的整合必须逐位等价，否则破坏 golden 测试与
  已发布行为的可追溯性。缓解：每项由既有测试全量回归 + 独立验证。
- CR-35 的 `lastSeen` 裁决若确认 app.cpp 侧为缺陷，属于行为修复，需在验证记录
  单独标注（不是纯重构）。
- viewer 层 helper 抽取涉及 EUI-NEO 布局细节，抽取前后几何必须一致；以
  test_node_canvas/param_panel/perf_panel/imu_panel 回归与真机冒烟为界。
- 测试夹具下沉会触碰十余个测试文件，属纯等价改写；以全量 ctest 四预设为界。

## 测试与退出条件

- [ ] 全部 CR 条目状态为"已整合/已统一/暂缓（记录理由）"，无遗留未登记条目。
- [x] 每个完成的工作项有 Independent-Verification-Agent 的独立回归证据（通过测试
  清单与输出）。
- [x] 全量 `ctest` 在 debug/asan/ubsan/tsan 预设 0 失败（32/32 × 4；tsan 需
  `setarch -R` 禁 ASLR——本机内核 mmap 随机化超 TSAN 支持范围，环境限制）。
- [x] golden/契约测试（image_contracts、workflow_contracts、motion_contracts、
  test_pixel_format、test_depth_preproc、test_public_boundary）逐位通过，证明冻结
  数值语义未漂移。
- [x] 台账中标注"已分叉"的条目（CR-15、CR-35）有裁决结论与回归证据。
- [x] 受影响设计文档（如需）与总计划索引已同步；验证记录完整。

## 验证记录

2026-10-07：审查落盘。三路分析 agent 全文阅读 src/core、src/adapters、src/workflow、
apps/viewer、tests 抽样与 tools；机械克隆检测 61 个 ≥11 行克隆区交叉证实（tests 占
绝大多数；src 侧 pixel_format.cpp 三份克隆被独立检出）。发现 46 条（P1×7、P2×20、
P3×19，其中 2 条已分叉）登记为 CR-01..CR-46，成立 M12。尚未开始代码修复。

2026-10-07：首轮修复收口（36/46 条整合，分支 refactor/code-reuse-m12，提交
101e101..cd42a60）。独立验证（Independent-Verification-Agent）四轮：

1. M12-01 轮：发现验收标准错误——minValue/maxValue 契约为 double，声明 2^53+1
   即量化为 2^53，原断言不可满足；改为契约文档化断言 + 判别用例（2^54−1 对
   min=2^54 由误接受改正确拒绝）。登记 CR-47。debug/asan/ubsan 32/32，
   workflow_contracts 332 断言 0 失败。
2. 核心批次轮（CR-01..07、09）：纸面逐项比对 + 11 个冻结数值判据测试三预设
   全绿，判定"逐位等价"；同时发现 adapter 批次 publishFrame 重载委托先于声明
   的编译错误（已修复：shared_ptr 重载前移）与孤儿测试
   test_depth_policy_preview.cpp（被测对象已随 a03066d 删除、从未注册 CMake，
   删除并登记 CR-48）。
3. 修复后 debug 全量 32/32 连续 5 轮全绿；workflow_depth_engine 在构建高负载
   下出现过 2 次时序抖动（5s 轮询死限），单跑复跑即绿，非本批引入。
4. 收尾轮（全 11 提交）：debug/asan/ubsan/tsan 四预设 clean rebuild + ctest
   **32/32 × 4**；tsan 裸跑因本机内核 mmap 随机化超 TSAN 支持范围失败，
   `setarch -R` 下 32/32（环境限制，建议 CI 固化该说明）。第一方代码零告警
   （kairo 既有 34 条第三方告警除外）。diff 审查（44 文件 +1813/−1994）：公开头
   零改动（include/ 无 diff），viewer 重构 id/几何逐位一致，新符号均有调用方；
   报告-only 残留 include（7 个测试文件的 <map>/<mutex> 等）留待后续清理轮。

行为对齐点（非纯重构，均已在对应提交注明）：CR-15 Integer 边界精度统一；
CR-35 排空时"仅水位非零"边角补复位；CR-22 首次打开路径保持独立（生命周期
不同）；CR-25 保留 applyGraph 预检（注释互指）。
