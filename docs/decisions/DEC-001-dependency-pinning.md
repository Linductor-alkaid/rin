# DEC-001：依赖锁定与引入方式

> 状态：Accepted
> 日期：2026-09-23
> 负责人：Linductor-alkaid
> 冻结里程碑：M1
> 替代/被替代：librealsense2 的 system 类引入被 [DEC-009](DEC-009-self-contained-deb-distribution.md)
> 取代（2026-09-23，改为 external 源码构建）；executor / eui-neo 部分继续有效

## 背景与问题

Rin 依赖 executor（C++20 并发库）、EUI-NEO（UI 框架）与 librealsense2
（设备 SDK）。工程规范 9.1 允许两种锁定方式（submodule+锁文件 / lock 清单+CMake 拉取），
需要为三个依赖选择统一且可离线校验的方案。

## 决策

采用 **lock 清单 + CMake 拉取**（`third_party/dependencies.lock` + `cmake/Dependencies.cmake`）：

- `executor`：external，pinned `4731b16493ae996311a9e85f55bd137aee69418a`，
  FetchContent 源码引入，`EXECUTOR_BUILD_TESTS=OFF`。
- `eui-neo`：external，pinned `782c56993dc1890e0589e2100cfa74322bb0e0bf`，
  FetchContent 源码引入。
- `librealsense2`：system，`find_package(realsense2 2.58.3)` 校验（本机
  `/usr/local` 安装版本 v2.58.3-6-g7c3ee3fb7，与锁清单 pinned commit 同源）；源码
  级引入成本过高（全量构建十余分钟），当前阶段不打包。

external 依赖优先接受 `RIN_<NAME>_SOURCE_DIR` 指向的本地 clone（configure 时
`git rev-parse HEAD` 必须等于 pinned commit），否则按 URL 拉取；离线构建
（`FETCHCONTENT_FULLY_DISCONNECTED=ON`）必须提供本地源目录。

## 备选方案

- git submodule + `dependencies.lock.json`：clone 体验依赖网络且 submodule 状态易漂移；
  本项目依赖数量少，锁清单已足够，未采用。
- librealsense2 也 FetchContent 源码构建：可复现性最强，但显著增加配置/构建时间；
  作为 `POST` 项，出现无系统安装的部署目标时再立项。

## 影响与风险

- 换机构建要求系统预装 librealsense2 2.58.3（或改走 `POST` 路线），已在 librealsense
  台账记录。
- executor/eui-neo pinned 于开发分支 commit，上游 API 可能演进；升级须按工程规范
  10.7 走独立变更并记录差异。

## 验证方式

`cmake --preset debug` configure 输出显示三个依赖校验通过；篡改本地源 commit 时
configure 失败（负向验证）。

## 关联文档和工作项

`M1-07`、[依赖台账索引](../dependency_feedback/README.md)、
[librealsense 台账](../dependency_feedback/librealsense/ledger.md)
