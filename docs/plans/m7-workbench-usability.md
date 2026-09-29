# M7：工作台可用性修复——运行中改图陈旧产物、分辨率入口、调色板滚动与面板可调

> 状态：Complete
> 负责人：Linductor-alkaid（授权 Agent 按工程规范自治执行）
> 所属计划：[Rin 实施总计划](rin-implementation-plan.md)
> 前置：M6
> 立项依据：用户真机使用反馈（2026-09-29，M6 合并后）
> 建议发布点：v0.5.0（随 M5）
> 更新日期：2026-09-29

## 目标

修复用户真机反馈的三个问题：① 相机源面板分辨率下拉不生效；② 运行中改图后
节点产物（缩略图尺寸等）停留陈旧值——"先降低分辨率后高斯再降低分辨率，高斯
的输出不是第一次降低分辨率的输出的分辨率"；③ 调色板在小窗口下无法看到全部
选项（需滚动），且五区 docking 大小固定（Context 图像输出太小，应可调）。

## 根因与方案（冻结）

1. **分辨率下拉不生效（Bug A）**：M6 的 `CameraResolutionBinding` 在
   `app::compose` 栈上构造，下拉回调（retained 节点持有回调副本，点击时才
   执行）捕获该栈临时量指针——点击时悬垂（UB）。M5-04 同类下拉均捕获
   ViewerContext 稳定地址故未触发。修复：labels 向量与 binding 对象移入
   `ViewerContext`（稳定存续），compose 仅刷新 `selectedIndex` 后传稳定指针；
   onPick 捕获 `&ctx`（稳定）。
2. **运行中改图产物陈旧（Bug B）**：M4-07 产物邮箱为代内设施
   （`buildGeneration` 每代逐节点新建），UI 侧 `NodeOutputCache` 的 per-node
   "上次已见序号"水位跨代比较无意义——换代后新代邮箱序号从 1 重新计，全部
   新快照被旧水位过滤，面板永久显示旧代陈旧产物（尺寸/分辨率即用户观察到的
   错误值；参数热更新换代同样命中且无 GraphApplied 事件）。修复（引擎侧）：
   `Generation::outputs` 改 shared_ptr，`buildGeneration` 对同 id 节点**沿用
   上一代邮箱**（发布序号跨代连续，水位语义成立）；`publishOutputs` 对非当前
   生效代的过代帧**跳过邮箱发布**（统计照记）——共享邮箱下迟到的旧代帧不得
   以更高序号把旧图产物写回。被移除节点邮箱随旧代释放，新节点取全新邮箱。
3. **调色板滚动（UI C-1）**：采用 pinned EUI-NEO 既有
   `components::scrollView` 组件（bind 偏移信号 + token 主题），调色板内容
   改列布局流式排布；不满足"组件能力缺口"台账条件（框架有现成组件）。
4. **docking 可调（UI C-2）**：五区几何改为 `WorkflowCanvasState` 持有的
   UI 私有布局状态（palette 宽 / context 宽 / 底部高 / Context 输出块高），
   分隔条以 `components::mouseArea` 拖拽调节（范围夹取）；会话级，不持久化
   （DEC-014"画布布局为 UI 私有状态"延伸）。

## 工作项

- [x] `M7-01` Bug A 修复（分辨率绑定稳定化）。
- [x] `M7-02` Bug B 修复（代际边界产物消费水位重置）。
- [x] `M7-03` 调色板滚动（scrollView 组件化）。
- [x] `M7-04` 五区 docking 可调大小（palette/context 宽、底部高、输出块高）。
- [x] `M7-05` 测试（Independent-Verification-Agent）、真机冒烟、文档收口。

## 测试与退出条件

- [x] Bug A/B 回归单测（绑定稳定性见证 / GraphApplied 清空语义）。
- [x] 布局纯逻辑单测（分隔条拖拽夹取、调色板行测量）。
- [x] 全量 ctest debug/asan/ubsan/tsan 单测全绿。
- [x] 真机冒烟：运行中改图后缩略图尺寸即时刷新、分辨率下拉生效（预览同步）、
      调色板滚动、拖拽调节四处分隔条。
- [x] 文档同步：ui_workspace_design.md §3/§5 补记、M7 计划验证记录、CHANGELOG。

## 验证记录

2026-09-29：立项。用户真机反馈三项：① 相机源面板分辨率下拉不生效；② 运行中
改图后节点产物停留陈旧值（"先降低分辨率后高斯再降低分辨率，高斯的输出不是
第一次降低分辨率的输出的分辨率"）；③ 调色板小窗口放不下且五区 docking 大小
固定（Context 图像输出太小）。根因走查（主循环代码审查）：① 为 M6 引入的
retained 回调悬垂（绑定对象在 compose 栈上构造，下拉回调持有其指针，点击时
已悬垂）；② 为 M4-07 产物邮箱代内设施与 M5-04 UI 消费水位的交互缺口（运行中
图变更换代后新代邮箱序号从头计，旧水位过滤掉全部新快照）；③ 为 UI 能力项
（pinned EUI-NEO 有现成 scrollView 组件，不构成能力缺口）。修复方案冻结于
上文"根因与方案"。实施顺序 `M7-01`..`M7-05`，测试归
Independent-Verification-Agent。

2026-09-29：`M7-01`..`M7-05` 完成关闭。**Bug A（分辨率下拉不生效）**：M6 的
绑定对象在 compose 栈上构造，下拉回调被 retained 节点持有、点击时才执行，
捕获的栈指针悬垂（真机冒烟脚本化绕过了下拉回调故 M6 未暴露）。修复：labels
与绑定对象移入 ViewerContext 稳定存续，onPick 捕获静态单例。**Bug B（运行中
改图产物陈旧——"高斯的输出不是第一次降低分辨率的输出的分辨率"）**：引擎
产物邮箱改同 id 跨代共享（序号连续）+ 过代帧跳过邮箱发布；主循环真机冒烟
实证：运行中插入 downscale 后选中节点缩略图即时刷新为 424×240（修复前永久
停留 848×480），分辨率下拉经绑定 onPick 路径切换生效（预览选择器同步）。**UI**：
调色板 scrollView 滚动、四处分隔条拖拽可调（截图 `screenshots/m7/`）。
测试（Independent-Verification-Agent）：三条判别性回归（跨代水位图替换/参数
热更新/过代帧不回写共享邮箱）——**在旧实现下确定性失败、修复后全绿**（临时
还原 HEAD 引擎实证，tsan 下 5/5 失败于三处判据）；并实证既有套件对新旧实现
均通过（水位恰低不构成防线），新回归补上空档；UI 布局状态默认值与夹取语义
见证单测。全量 ctest debug/asan/ubsan/tsan **26/26 全绿**（tsan 含真机
hardware 本轮通过）。主循环真机冒烟另验证 Bug A 与 docking（分辨率切换
"resolution applied"、四分隔条呈现）。环境：x86_64 Linux（GNOME/XWayland），
GCC 13。`M5-07`（M5 验收收口）为下一工作项。
