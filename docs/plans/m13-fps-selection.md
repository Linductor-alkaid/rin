# M13：帧率档位选择（分辨率档位放开 fps）

> 状态：Completed
> 负责人：Linductor-alkaid（授权 Agent 按工程规范自治执行）
> 所属计划：[Rin 实施总计划](rin-implementation-plan.md)
> 前置：M1
> 立项依据：用户需求（2026-10-08："当前只有 30 帧的输出，能不能手动选择输出帧数？"）
> 更新日期：2026-10-08

## 目标

viewer 的分辨率档位下拉从"M1 固定 30fps"放开为按设备能力枚举全部 (宽, 高, fps)
组合，用户可手动选择输出帧率（如 D435IF 的 848x480@60）。默认流请求不变
（DEC-004：848x480@30）；核心 `StreamRequest.colorFps/depthFps`、adapter 能力
枚举（`ResolutionOption.fps`）与 SDK 配置链路已存在且经真机验证，本里程碑只
收敛 viewer 侧的枚举/展示/回选逻辑并补齐契约测试。

## 范围与非目标

范围：

- 新增 `apps/viewer/resolution_model.hpp`：档位枚举纯函数（彩色∩深度按
  (宽, 高, fps) 三元组配对、去重、确定性排序、档位标签），从 `app.cpp` 的
  `rebuildResolutionOptions()` 下沉（M12 同款"下沉即测试"路径）。
- `apps/viewer/app.cpp` 接线：移除 30fps 过滤；设备变化时的默认回选从"只匹配
  宽高"改为匹配完整默认视频字段（含 fps）。
- `apps/viewer/node_canvas.hpp` 源节点 Camera resolution 胶囊布局加宽，适配
  含 fps 后缀的档位标签。
- 契约测试 `tests/test_resolution_model.cpp`（测试委派
  Independent-Verification-Agent）与既有单测回归。
- 文档同步：总计划里程碑索引、README/README_zh、CHANGELOG、本计划验证记录。

非目标：

- 不改 `ICameraService`/`StreamRequest` 公开契约（fps 字段已存在）。
- 不改 adapter 采集/重流逻辑（`buildConfig` 直传 fps 已覆盖）。
- 不提供彩色/深度独立帧率（两路仍按同 (宽, 高, fps) 成对——M1"双流成对"契约，
  见"设计与决策依据"）。
- 不做高帧率下的软件转换性能优化（jet/RGBA CPU 转换开销经性能面板可观测；
  60fps 实测开销见验证记录）。

## 设计与决策依据

- [DEC-004](../decisions/DEC-004-default-stream-request.md)：默认 848x480@30
  不变；本里程碑放开的是"可选项"，与默认值决策正交。
- M1"彩色与深度成对（不支持单流）"契约（`camera_types.hpp` StreamRequest 注记）：
  档位必须是彩色与深度同时支持的 (宽, 高, fps) 三元组，因此采用单一列表求交集，
  而非"分辨率 + 帧率"两个独立下拉（独立下拉会暴露不可用组合，需二次校验与
  拒绝路径，收益为负）。
- M1 遗留范围收缩的出处：`app.cpp` `rebuildResolutionOptions()` 内
  "M1：固定 30fps 档位"过滤注释（313d3bc 首版 viewer 引入）；M1 计划目标
  "允许在设备支持的分辨率档位间切换"本就未限定帧率，本次属放开范围收缩而非
  契约变更。
- 设备能力事实（真机 D435IF，serial 261922074392，firmware 5.15.1.55，
  `rs-enumerate-devices` 2026-10-08）：彩色 RGB8 与深度 Z16 的 (宽, 高, fps)
  交集档位含 1280x720@{30,15,6}、848x480@{60,30,15,6}、640x480@{60,30,15,6}、
  640x360@{60,30,15,6}、424x240@{60,30,15,6} 等（960x540 无深度档不成对）。
- 档位标签格式 `"<宽>x<高> · <fps>fps"`（如 `848x480 · 60fps`）：与内参卡片的
  紧凑 `848x480` 风格一致；排序为高度降序、宽度降序、帧率升序（分辨率排序
  观感与既有菜单一致，同档位帧率自低到高）。
- 无效选择（如 USB2 带宽下设备拒绝 60fps）路径已有闭环：`pipeline.start`
  抛错 → 服务转 `Failed` 并发布事件 → UI 状态行可见（`test_camera_state_machine`
  已覆盖失败可见性）。

## 工作项

- [x] `M13-01` viewer 档位枚举放开 fps：`resolution_model.hpp` 纯函数
  （交集配对/去重/排序/标签）+ `app.cpp` 接线（含默认回选匹配 fps）+
  `node_canvas.hpp` 胶囊布局适配。验收：下拉出现设备支持的帧率档位；默认
  回选 848x480@30；预览页/工作流页/源节点入口三处同源同选。
- [x] `M13-02` 契约测试与回归（测试委派 Independent-Verification-Agent）：
  `tests/test_resolution_model.cpp` 覆盖交集配对、无深度档不成对、去重、
  排序稳定性、标签格式、enableMotion 粘性注入；既有单测全量回归
  （debug/asan/ubsan/tsan 预设）。
- [x] `M13-03` 真机验收与文档同步：D435IF 上选择 848x480@60，流建立且实测
  帧率 ≈60（性能面板/硬件用例）；README/README_zh/CHANGELOG/总计划索引/
  本计划验证记录更新。

## 风险与阻塞

- 高帧率 × 高分辨率的 CPU 转换开销上升（M1 软件转换）：UI 不设限，性能面板
  可观测；由用户按设备/带宽自行选择档位。
- USB2 连接下高帧率档位可能被设备/SDK 拒绝：走既有 Failed 事件路径可见，
  不做静默回退。
- 源节点胶囊宽度有限（kNodeWidth=200）：加宽后超宽标签（1920x1080 · 30fps）
  仍可能贴近胶囊右缘；下拉与浮层展示完整标签，风险接受为外观级。

## 测试与退出条件

1. `test_resolution_model` 新增契约全过（debug + asan + ubsan + tsan）。
2. 既有单测全量回归通过（同上预设）。
3. 真机 D435IF：选择 848x480@60 后流建立、帧率实测 ≈60、切回 30 正常
   （restream 收敛）。
4. 三处入口（预览页下拉、工作流浮层、源节点胶囊）展示同一档位列表与同一
   选中项（代码同源 + 截图证据）。
5. 文档同步完成（README ×2、CHANGELOG、总计划索引、本计划验证记录）。

## 验证记录

2026-10-08：`M13-01`..`M13-03` 完成关闭。

**实现**（主循环）：新增 `apps/viewer/resolution_model.hpp`
（`buildResolutionUiOptions`：彩色∩深度 (宽,高,fps) 三元组配对、去重、
高度/宽度降序 + 同档位帧率升序、标签 `"宽x高 · 帧率fps"`；enableMotion
由调用方注入会话粘性）；`app.cpp` `rebuildResolutionOptions()` 改为消费
唯一实现（移除 M1 "固定 30fps 档位"过滤，313d3bc 引入），设备变化默认
回选判据扩为完整默认视频字段（848x480 + 双路 30fps）；`node_canvas.hpp`
源节点胶囊布局 0.55/0.45→0.40/0.60、标签 "Camera resolution"→"Resolution"
（fps 后缀标签加宽适配）。

**契约测试**（Independent-Verification-Agent，一轮全绿）：新建
`tests/test_resolution_model.cpp`，8 分节 339 checks / 0 failures——
D435IF 真机档位表交集逐项手推（19 档；960x540 与 848x480@90 正确排除）、
双侧配对必要性与同帧率约束、去重、乱序输入排序稳定性、标签逐字节
（U+00B7 = C2 B7）、enableMotion 两态注入与六视频字段成对相等、空目录
退化、输入排列无关性。回归：debug/asan/ubsan 全量通过；tsan 全量通过
（本机需 `setarch -R` 禁 ASLR——内核 7.0 高熵 ASLR 与 TSan 运行时的
预存环境问题，影响所有测试，非本改动引入；补跑条件：调低
`vm.mmap_rnd_bits` 或内核修复）。验证过程修正测试自身两处期望表/偏移
笔误，未发现实现缺陷。

**真机验收**（D435IF，serial 261922074392，Independent-Verification-Agent）：

- 新增 hardware 用例 `tests/test_realsense_fps_hardware.cpp`（视频-only，
  LRS-20261007-001 台账绕行按设计执行）：848x480 档 30→60→30 切换，实测
  墙钟口径 30.28 → **59.51** → 29.84 Hz（设备时间戳口径 30.01 → 59.05 →
  30.01 Hz，≥2s 窗口），全程分辨率 848x480 不漂移、帧序号严格单调；
  886 checks / 0 failures，复跑两次一致（10.78s / 10.79s）。无设备 SKIP 77
  语义与既有 hardware 用例一致。
- viewer UI 自动化截图（Xephyr 嵌套 X + XTest 注入，产品零改动），存档
  `screenshots/m13/`：下拉浮层 19 档含 fps 标签与选中高亮（与手推排序
  逐项一致）；切 60fps 后选择器 "848x480 · 60fps"、状态 "Streaming
  resolution applied"、双卡 848 x 480 在流；工作流页源节点胶囊
  "Resolution" + "848x480 · 60fps"（0.40/0.60 新布局，放大复核无截断），
  与预览页同源同值。默认回选 848x480@30 截图同证（证据 A）。

**回归与预设矩阵**：release 全量 34/34（unit 32 + hardware 2）；debug
同构 34 项。`realsense_hardware`（运动流全链路）本日多次真机执行通过
（M13 真机验收轮 1 次 + 收尾台账审查取证轮连续 5 次），与 LRS-20261007-001
记录的"本机必然饿死"不符——经 owner 授权的台账审查确认未复现（内核不变下
IIO 三轴读数健康 + 运动流全链路 5/5），该条目当日裁定 Resolved（取证与
重开条件见台账）；M13-03 验收按台账绕行以视频-only 请求执行，
`test_realsense_fps_hardware` 与运动流路径解耦，作为常驻回归不受影响。

**未验证项与补跑条件**：

- 60fps 长时间稳定性（>2s 窗口）与全档位循环切换未测（验收范围限定
  30→60→30 单循环）；负责人：Linductor-alkaid；补跑条件：后续真机
  巡检时执行。
- 位姿页/设置页未截图（不在本里程碑验收面内）。
- 工作流页仅验证源节点胶囊呈现，未运行完整处理图（M13 不改引擎，帧率
  上限由相机源档位决定，引擎吞吐基线见 docs/benchmarks/）。
