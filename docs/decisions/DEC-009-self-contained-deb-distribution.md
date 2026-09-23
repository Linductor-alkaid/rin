# DEC-009：自包含 deb 分发（捆绑 librealsense2）

> 状态：Accepted
> 日期：2026-09-23
> 负责人：Linductor-alkaid
> 冻结里程碑：M2
> 替代/被替代：取代 [DEC-001](DEC-001-dependency-pinning.md) 中 librealsense2 的
> system 类引入方式；其余依赖锁定方式不变

## 背景与问题

此前 librealsense2 为 system 类依赖：用户或构建机必须先按上游文档安装 librealsense2
（含 udev 规则）才能构建与使用，end user 安装 Rin 后常因缺少设备库/udev 规则无法使用
相机。需要一种"安装即用"的分发方式，且可在 CI 中无系统 librealsense2 的环境下产出
安装包。

## 决策

1. **librealsense2 改为 external 源码构建**：`third_party/dependencies.lock` 锁定行
   改为 external（pinned commit 不变 `7c3ee3fb7...`，即上游 2.58.3），FetchContent
   拉取构建；构建面裁剪为仅运行时库（见 librealsense 台账"集成决策"）。锁定校验、
   本地源目录与离线构建纪律与其他 external 依赖一致。
2. **deb 自包含分发**（CPack DEB，`cmake/Packaging.cmake`）：
   - `/usr/bin/rin`（RUNPATH `$ORIGIN/../lib/rin`）；
   - `/usr/lib/rin/librealsense2.so.2.58(.3)`：捆绑的 librealsense2 运行库，私有目录
     + rpath 定位，不与系统安装冲突，不带开发 namelink；
   - `/usr/lib/udev/rules.d/99-realsense-libusb.rules`：来自 pinned 源码的设备规则，
     postinst 中 `udevadm control --reload-rules && udevadm trigger`（失败不阻塞安装）；
   - `/usr/share/applications/rin.desktop` 与 hicolor 图标（`docs/rin.png`）；
   - 依赖声明由 `dpkg-shlibdeps` 自动生成（libusb-1.0-0、libudev1、libcurl4、
     libopengl0、libx11-6 等），不硬编码。
3. **组件隔离**：本项目安装规则全部归属 `rin` 组件；依赖（executor / eui-neo /
   librealsense2）的 install 规则在 `cmake/Dependencies.cmake` 中被归入
   `deps-<name>` 组件；CPack 只安装 `rin` 组件，依赖的头文件/静态库/cmake config
   不进入 deb。
4. **CI 导出**：GitHub Actions 在 push/PR 上构建测试并产出 deb 工件；打 `v*` tag 时
   将 deb 附着到 GitHub Release。

## 备选方案

- deb 依赖系统 librealsense2（Depends: librealsense2）：用户仍需第三方源/手动安装，
  违背"安装即用"，未采用。
- 安装 librealsense2 至标准库目录并 ldconfig：与系统 librealsense2 包文件冲突，
  且会连带安装上游 install 规则的头文件与 cmake config，未采用。
- AppImage/Flatpak：分发面更广但打包与维护成本更高；当前目标平台为 Ubuntu 系
  （deb），留作后续扩展。
- 仅 CI 中从系统包构建 deb（不切 external）：本地与 CI 两条依赖路径，且受制于
  Intel apt 源的可用性，未采用。

## 影响与风险

- 所有构建（含本地开发）都会构建 librealsense2 源码：首次 configure/构建耗时增加
  （拉取 + 编译），FetchContent 缓存后无额外成本；裁剪后构建面可控。
- nlohmann/json 由上游 configure 期按分支 tag 拉取，不在本仓库锁清单校验范围
  （LRS-20260923-002）。
- 捆绑库的 CVE 跟进依赖升级 pinned commit，按工程规范 10.7 走独立变更。
- 私有库目录 + RUNPATH 属于发行版打包的保守做法（避免文件冲突），不做 ldconfig 注册。

## 验证方式

- 干净构建目录：configure（无系统 librealsense2 也可）→ build → `cpack -G DEB`
  产出 `rin_0.2.0_amd64.deb`。
- `dpkg-deb -c/-I`：内容为上述五类文件，Depends 无 librealsense2；解包后
  `readelf -d` RUNPATH 为 `$ORIGIN/../lib/rin`，`ldd` 从私有目录解析捆绑库、
  无 `not found`。
- `cmake --install --component rin` 暂存目录不含任何依赖文件（组件隔离负向验证）。

## 关联文档和工作项

[M2 计划](../plans/m2-linux-packaging.md)、[librealsense 台账](../dependency_feedback/librealsense/ledger.md)、
[EUI-NEO 台账 EUI-20260923-004](../dependency_feedback/eui-neo/ledger.md)
