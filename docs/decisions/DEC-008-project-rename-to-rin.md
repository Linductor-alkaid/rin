# DEC-008：项目更名 Rin

> 状态：Accepted
> 日期：2026-09-23
> 负责人：Linductor-alkaid
> 冻结里程碑：M2
> 替代/被替代：无

## 背景与问题

项目原名 realsense-vision（构建名 `realsense_vision`、命名空间 `rsv`、头目录
`include/rs_vision/`、可执行名 `rsv_viewer`、窗口标题 "RealSense Vision"）。GitHub
远端仓库已更名为 `Linductor-alkaid/rin`，本地标识与远端不一致；且原名冗长、与设备
供应商强绑定，不利于后续扩展与分发（包名、命令行、桌面入口都需要简短标识）。

## 决策

项目对外与对内标识统一更名为 **Rin**：

- CMake 工程 `project(Rin)`；版本随本次变更为 0.2.0。
- C++ 命名空间 `rsv` → `rin`（测试辅助 `rsv_test` → `rin_test`）；公开头目录
  `include/rs_vision/` → `include/rin/`。
- 构建目标：`rin_core`（alias `rin::core`）、`rin_realsense_adapter`
  （alias `rin::realsense_adapter`）、可执行 `rin`；CMake 选项前缀 `RSV_` → `RIN_`
  （`RIN_BUILD_TESTS`、`RIN_BUILD_VIEWER`、`RIN_ENABLE_ASAN/UBSAN/TSAN` 等），
  依赖本地源变量 `RIN_<NAME>_SOURCE_DIR`。
- 桌面窗口标题与 UI 头部文案为 "Rin"；deb 包名 `rin`，命令 `rin`，桌面入口
  `rin.desktop`，图标 `rin.png`。
- 文档（AGENTS.md、工程规范、计划、设计、决策、台账、CHANGELOG）同步更名；
  总计划文件更名为 `docs/plans/rin-implementation-plan.md`。
- **不改名的部分**：`src/adapters/realsense/`、`realsense_camera_service.*` 等标识中
  的 "realsense" 指向 Intel RealSense 设备与 librealsense SDK 本身，保留原词。

## 备选方案

- 保留 `rsv` 等内部命名、只改文档与包名：迁移成本低，但同一项目两套标识长期并存，
  分发物（命令 `rsv_viewer`）与仓库名脱节，未采用。
- 更名为其他名称：远端仓库已定 `rin`，以远端为准。

## 影响与风险

- 一次性的全局代码/文档改名，接口语义、状态机、并发模型均不变；依赖本项目的下游
  （当前无）需同步更新 include 路径与命名空间。
- 历史文档中的验证记录在更名前的 commit 上执行，命令中的旧目标名/选项名以当次
  commit 为准，不追溯改写；更名后的规范性文本一律使用新标识。

## 验证方式

- 全仓库 `grep -ri "rsv|rs_vision|realsense-vision"` 零残留（third_party 与 build
  树除外）。
- 干净构建目录 configure + build + ctest 通过（见 M2 验证记录）。

## 关联文档和工作项

[M2 计划](../plans/m2-linux-packaging.md)、[DEC-009](DEC-009-self-contained-deb-distribution.md)
