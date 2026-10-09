# DEC-022：预览深度可选输出（策略单帧）与字号档位

> 状态：Accepted
> 日期：2026-10-09
> 负责人：Linductor-alkaid
> 关联：DEC-019（米制深度通道）、DEC-020 决策 4（移除 M9 固定策略卡片）、
> roboparty E3-Parkour 部署参考（E3_PARKOUR_VISION_ANALYSIS.md）

## 背景与问题

两项用户需求：

1. **策略输出进预览**：把"整个 policy 输出工作流（不带历史分片）"的输出——
   即 O6 冻结单帧组合 `preprocessPolicyDepthFrame`（填充 → 降采样到 raw
   网格 64×36 → 裁切 (18,0,16,16) → 高斯模糊 → [0,2.5 m] 归一化，输出
   18×32 ∈ [0,1]；数值语义即 roboparty E3-Parkour 的策略深度预处理，参数
   = `PolicyDepthConfig` 默认冻结值）——放进预览页的深度显示，作为与相机
   深度 rendition 并列的可选输出。M9 的固定策略卡片（含 O7 8 帧历史网格）
   已随 DEC-020 决策 4 移除；本轮为按需求边界的回归：**只取单帧管线输出，
   不含 O7 历史分片**。
2. **字号设置**：设置页提供字号档位。

## 决策

1. **预览深度卡内切换，而非控制行第三选择器**：Depth 卡片标签行右侧新增
   循环切换胶囊（Raw ⇄ Policy，工作流枚举胶囊同款交互与令牌）。深度输出
   是"该卡片的显示源"属性，贴卡呈现比占据预览页控制行更符合
   DEC-014 的信息架构；Raw 输出继续跟随全局深度配色命令（DEC-007），
   Policy 输出为冻结灰度（[0,1]×255，near 黑 → far 白，无效填充呈白），
   不随配色命令变化——它呈现的是"策略看到的帧"，显示语义即数据语义。
2. **O6 在 Executor 周期 tick 内执行（DEC-019 决策 3 纪律不变）**：新组件
   `viewer::PolicyDepthOutput`（apps/viewer/policy_depth_output.hpp，无 EUI
   类型可 headless 单测）——20 ms 软调度周期 tick：`tryLoadDepthMetric` →
   O6 → 最近邻放大（最长边 ≤ 512）→ [0,1]×255 灰度 RGBA8 →
   `kairo::comm::LatestMailbox`；渲染线程 pump 仅做"上次已见序号"非阻塞
   消费与纹理上传（RULE-05/07）。与 O7 历史分片无跨帧状态，流重启检测
   随之不适用。失败显式化：无效源帧与 O6 拒绝（如源小于 raw 网格的放大
   拒绝）计入 `droppedFrames`，不静默（AGENTS 规则 10）。生命周期 owner
   为 ViewerContext：服务 start 准入通过后 start，onShutdown 在服务 stop
   之后、executor shutdown 之前析构停止（准入失败路径不创建，tick 不会
   落在未初始化 executor 上）。**组件必须由 shared_ptr 持有**（tick 闭包
   经 weak_from_this 捕获，enable_shared_from_this 契约）——unique/栈持有
   会使弱引用恒空、周期回调静默空转，`start()` 对此显式拒绝返回 false
   （独立验证发现的缺陷类，回归用例 test_policy_depth_output 8b/9）。
3. **字号 = 应用配置 uiScale 的运行期档位**：EUI 每帧读取
   `effectiveScale = dpiScale × uiScale`，变更即整页重排重绘——文字与
   布局同步缩放，避免"字号变大但行高/间距固定"的裁切问题（viewer 布局
   度量与字号强耦合，纯文本缩放需要全面积重构，收益不成比例）。
   设置页 Preferences 新增 Font size 四档（0.85/1.0/1.15/1.3，默认标准），
   经 `app::setUiScale` 写入应用配置存储（读写同在 UI 线程，无跨线程
   竞争）；会话级，不持久化（与既有设置面一致）。EUI 契约无变更
   （`uiScaleValue` 为既有配置字段，框架每帧消费）。
4. **预览消费与排空**：无论卡片当前选择哪个输出，policy 邮箱都保持消费
   （单槽换新，切换即时呈现最新帧）；服务 Waiting/Failed 稳态下 policy
   视图与 RGB/Depth 同步排空回占位文案。

## 备选方案

- **策略输出走工作流节点**（source_policy_depth）：O6 无跨帧状态本可节点
  化，但需求指向"预览页深度可选输出"（面向真机快速对照策略视角），且
  DEC-018 已论证策略预处理不进画布节点面（保持与 O7 同族的处理边界）。
  否决。
- **O7 历史网格一并回归**：需求明确"不带历史分片"；历史网格的画布内
  组合已可经 `depth_history` + 监看器表达。否决。
- **纯文本字号缩放**（theme 字号令牌运行期化）：全部布局度量与字号常量
  强耦合，改动面与回归面远超收益。否决（以界面缩放等价实现"字号设置"，
  文字与版面等比）。

## 影响与风险

- 米制通道消费新增一路（PolicyDepthOutput tick）：LatestMailbox 单槽换新，
  显示帧 ≤ 512×288×4 有界；O6 单帧成本与 M9 实测同量级（tick 周期 ≫ 单帧
  成本）。
- uiScale 运行期变更触发整页重排：EUI 逐帧读取该值属既有设计；档位切换
  后指针命中坐标与布局同步换算，真机场景矩阵验证覆盖。

## 验证方式

- `test_policy_depth_output`（headless，tick 直接驱动）：常值帧端到端量化、
  无效像素填充呈白、最近邻放大尺寸与索引映射、序列号/时间戳透传、邮箱
  门控、O6 拒绝与无效源帧显式丢弃、无服务防御、gray 钳制、start/stop 冒烟。
- 全量 ctest 回归 + 真机场景（Independent-Verification-Agent）：深度卡
  Raw/Policy 切换出图、字号档位切换后布局与命中正确。
