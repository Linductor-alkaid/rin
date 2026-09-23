# M2：Rin 更名与 Linux 自包含分发

> 状态：In Progress
> 负责人：Linductor-alkaid
> 所属计划：[Rin 实施总计划](rin-implementation-plan.md)
> 前置：M1
> 建议发布点：v0.2.0
> 更新日期：2026-09-23

## 目标

项目标识统一更名为 Rin（远端仓库已定名），并提供"安装即用"的 Linux（deb）分发：
捆绑 librealsense2 运行库与 udev 规则，用户无需任何预装即可构建、安装和使用；
GitHub Actions 产出安装包工件，tag 触发 Release 上传。

## 范围与非目标

范围：

- 全局更名（命名空间、目标、选项、窗口标题、文档、包名）。
- librealsense2 锁定行 system → external（pinned commit 不变）。
- CPack DEB 打包（可执行 + 捆绑库 + udev 规则 + 桌面入口/图标）与组件隔离。
- GitHub Actions 工作流（构建、测试、打包、发布工件）。

非目标：

- Windows/Android 打包（`POST-03`）。
- AppImage/Flatpak 等其他分发格式。
- 与系统 librealsense2 共存的变体包（如 `rin+system-lrs`）。

## 设计与决策依据

- [DEC-008](../decisions/DEC-008-project-rename-to-rin.md)：更名范围与不改名清单。
- [DEC-009](../decisions/DEC-009-self-contained-deb-distribution.md)：自包含 deb 分发
  与组件隔离。
- [LRS-20260923-002](../dependency_feedback/librealsense/ledger.md)：源码构建的
  C++ 标准隔离绕行。
- [EUI-20260923-004](../dependency_feedback/eui-neo/ledger.md)：app 目标异常编译
  选项绕行。

## 工作项

- [x] `M2-01` 全局更名：`rsv`→`rin`、`rs_vision`→`rin`、`RSV_`→`RIN_`、目标/可执行/
      窗口标题/文档更名，总计划文件更名为 `rin-implementation-plan.md`；全仓库旧标识
      零残留（DEC-008）。
- [x] `M2-02` librealsense2 改 external 源码构建：锁定行、构建裁剪选项（CACHE FORCE
      + 恢复）、C++17 域隔离、`realsense2::realsense2` 别名补齐（DEC-009、
      LRS-20260923-002）。
- [x] `M2-03` CPack DEB 打包：`cmake/Packaging.cmake` 安装规则 + 组件隔离 +
      desktop/图标/udev 规则/postinst；deb 内容与 RUNPATH 本地验证。
- [x] `M2-04` 台账与决策同步：DEC-001 部分替代标注、DEC-008/009 立档、
      librealsense 台账 -001 关闭/-002 登记、EUI-NEO 台账 -004 登记。
- [x] `M2-05` GitHub Actions 工作流：push/PR 构建测试并上传 deb 工件；`v*` tag
      上传 Release。（run 35888435555 全绿，见验证记录 2026-09-23 第三条）
- [x] `M2-06` 独立验证：干净环境 configure/build/ctest、deb 安装面检查、更名残留
      检查（Independent-Verification-Agent 回报证据，见验证记录 2026-09-23 第二条）。
- [ ] `M2-07` 真机冒烟：deb 安装到本机，D435if 出流验证（本机无免密 sudo，agent
      不可代行；待维护者执行 `sudo apt install ./rin_0.2.0_amd64.deb` 后复验；
      当前源码树真机硬件用例已通过，见验证记录 2026-09-23 第二条）。

## 风险与阻塞

- GitHub 网络在本机间歇不可达：configure 拉取（librealsense、上游 json）可能失败，
  重试即可；CI 环境不受影响。
- deb 在 Ubuntu 22.04 及更早版本的依赖命名差异（如 `libcurl4t64` 为 24.04 包名）：
  当前按 ubuntu-24.04 构建分发，兼容性证据在 M3 前不外延。

## 测试与退出条件

- [x] 干净构建目录 Release configure + build 通过（无系统 librealsense2 依赖）。
- [x] `cpack -G DEB` 产出 `rin_0.2.0_amd64.deb`；`dpkg-deb -c/-I` 内容与依赖符合
      DEC-009 验证方式描述。
- [x] 解包后 `readelf -d` RUNPATH `$ORIGIN/../lib/rin`，`ldd` 全部可解析且捆绑库
      从私有目录加载。
- [x] `cmake --install --component rin` 暂存树不含依赖文件（组件隔离）。
- [ ] debug/asan/ubsan 预设 ctest 通过（独立验证代理执行）。
- [x] CI 在 GitHub 上运行通过并产出 deb 工件（run 35888435555 与 tag run
      35890171762）。
- [ ] 真机（D435if）经 deb 安装后出流验证。

## 验证记录

### 2026-09-23：M2-01/02/03/04 本地实施与打包验证

- 范围：更名、librealsense2 external 源码构建、CPack DEB、决策/台账同步。
- 依据：DEC-008、DEC-009、LRS-20260923-001/-002、EUI-20260923-004。
- 环境：Ubuntu 24.04（内核 7.0.0-31），GCC 13.3，CMake 3.28，Ninja。
- 命令与结果（干净目录 `build/pkg`，依赖源目录复用 pinned clone）：
  - `cmake -B build/pkg -G Ninja -DCMAKE_BUILD_TYPE=Release -DRIN_BUILD_TESTS=OFF ...`
    configure 通过；裁剪后 cache 中 `BUILD_EXAMPLES/BUILD_TOOLS/BUILD_ROSBAG2/...=OFF`，
    `BUILD_SHARED_LIBS` 仅在 realsense2 阶段 ON 并恢复。
  - `cmake --build build/pkg` 通过（`apps/viewer/rin` 2.9MB）。
  - `cmake --install build/pkg --prefix /tmp/rin-stage --component rin`：仅
    `bin/rin`、`lib/rin/librealsense2.so.2.58(.3)`、udev 规则、desktop、icon 五类，
    RUNPATH `$ORIGIN/../lib/rin`，无依赖文件泄漏。
  - `cpack -G DEB`：首次产出 `rin_0.1.0_amd64.deb`（7.9MB），版本升 0.2.0 后复验产出
    `rin_0.2.0_amd64.deb`。`dpkg-deb -I` Depends 自动生成且不含 librealsense2；解包
    `ldd` 无 `not found`，捆绑库从 `/usr/lib/rin` 解析。
- 限制：`rin` 为 GUI 应用，打包验证未覆盖设备出流（M2-07 补跑）；ctest 全量回归
  与 CI 运行由 M2-05/06 补齐。
- 同步：DEC-008/009、两份台账、总计划、CHANGELOG、README。

### 2026-09-23：M2-06 独立验证（Independent-Verification-Agent）

- 范围：干净构建目录全流程复核（V1 更名残留 / V2 debug 构建+全量测试 / V4 deb
  安装面 / V5 组件隔离负向 / V6 desktop 校验）。
- 环境：同上；全新构建目录，依赖全部由锁定 commit 干净拉取（未用本地源覆盖）。
- 结果（全部 PASS，日志 `/tmp/iva-*.log`）：
  - V1：六类旧标识中仅 CHANGELOG 更名条目自身含 `rsv`/`rs_vision` 各 1 处（更名
    记录必须指称新旧名，按 keep-a-changelog 惯例豁免）；代码/配置/其余文档零残留。
  - V2：debug configure/build 零 error；ctest 4/4 通过，**hardware 用例真机跑通**
    （D435IF，serial 261922074392，固件 5.15.1.55，requestDevice + 持续收帧）。
  - V4：`rin_0.2.0_amd64.deb`（7,934,180 字节）；Depends 无 librealsense2；内容恰为
    DEC-009 五类文件；RUNPATH `$ORIGIN/../lib/rin`；解包 ldd 无 `not found` 且
    librealsense2 解析到私有目录；postinst/postrm 可执行。
  - V5：`--component rin` 暂存树仅五类路径，依赖文件零泄漏。
  - V6：`desktop-file-validate` 退出码 0、无告警。
- 限制：V3（asan/ubsan 预设本地复跑）未在独立验证中执行，由 CI 矩阵
  （debug/asan/ubsan）在 push 时覆盖；M2-07 deb 安装真机冒烟待执行。
- 同步：本文件勾选 M2-06。

### 2026-09-23：M2-05 CI 运行通过（GitHub Actions run 35888435555）

- 范围：ubuntu-24.04 上 debug/asan/ubsan 三预设构建 + ctest 全量；release 构建 +
  cpack 产出 deb + 包内验证 + artifact 上传。
- 结果：全绿（
  [run 35888435555](https://github.com/Linductor-alkaid/rin/actions/runs/35888435555)）：
  - `build & test (debug|asan|ubsan) => success`：ctest 全部通过（runner 无设备，
    hardware 用例按设计 SKIP）；DOD-03 的 asan/ubsan 门禁自此由 CI 承载。
  - `deb package => success`：`rin_0.2.0_amd64.deb` 产出并上传 artifact
    （`rin-deb`，7,933,079 字节）；包内验证（Depends 无 librealsense2、五类内容、
    RUNPATH `$ORIGIN/../lib/rin`、ldd 全解析且 librealsense2 从 `/usr/lib/rin`
    加载）在 runner 上通过。
- 迭代记录（CI 环境差异修复，均以 `ci:` 前缀提交）：
  1. runner 缺 `wayland-scanner`（EUI-NEO 捆绑 GLFW 3.4 默认启用 Wayland）→ 补装
     `libwayland-dev wayland-protocols libxkbcommon-dev libegl-dev`。
  2. EUI-NEO `find_package(CURL)` 必需 → 补装 `libcurl4-openssl-dev`。
  3. 验证脚本 `test -x` 的 glob 展开错误 → 改 `find` 断言 + ldd 私有目录解析断言。
- 限制：M2-07 真机 deb 冒烟待维护者执行（无免密 sudo）。
- 同步：本文件勾选 M2-05。

### 2026-09-24：v0.2.0 发布（tag run 35890171762）

- 范围：应用图标替换为 rin_v2 同尺寸新作（1254x1254 RGBA，`docs/rin.png` 内容
  替换，打包路径不变）；CHANGELOG 定版 0.2.0；打 `v0.2.0` tag 触发发布流程。
- 结果：tag run 全绿（
  [run 35890171762](https://github.com/Linductor-alkaid/rin/actions/runs/35890171762)），
  Release 自动创建并附着 `rin_0.2.0_amd64.deb`（7,002,010 字节，
  https://github.com/Linductor-alkaid/rin/releases/tag/v0.2.0 ）。tag 触发的
  Release 上传路径自此实测通过。
- 本地同步复验：`build/iva-release/dist/rin_0.2.0_amd64.deb` 重建后包内
  `usr/share/icons/hicolor/512x512/apps/rin.png` 与 `docs/rin.png` MD5 一致
  （2e92a6392f24b6cf8da7a9e162b71d59）。
- 限制：M2-07 真机 deb 冒烟仍待维护者执行；tag 打在 feat 分支 HEAD，合入
  master 的 MR 由维护者安排。
- 同步：CHANGELOG 定版、本文件退出条件勾选。
