# 依赖反馈台账索引

realsense-vision 要求所有第三方依赖分别建立独立台账（AGENTS.md“依赖独立台账”节）。
一个依赖一份台账，记录依赖身份、集成决策、问题、缺口与上游反馈状态；纪律与 Executor
台账一致（工程规范 9.4）。

| 依赖 | 台账位置 | 编号前缀 | 锁定版本（commit/版本号） | 类别 |
| --- | --- | --- | --- | --- |
| executor | [../../executor_feedback/ledger.md](../../executor_feedback/ledger.md)（模板固定路径） | `EXE-` | `4731b16493ae996311a9e85f55bd137aee69418a` | external |
| executor（索引镜像） | [executor/ledger.md](executor/ledger.md) | — | 同上 | external |
| librealsense2 | [librealsense/ledger.md](librealsense/ledger.md) | `LRS-` | `2.58.3`（源 `7c3ee3fb7c640e9f315e663907208cb56c4febfd`） | system |
| eui-neo | [eui-neo/ledger.md](eui-neo/ledger.md) | `EUI-` | `782c56993dc1890e0589e2100cfa74322bb0e0bf` | external |

新增依赖时：在本表追加一行，并创建 `docs/dependency_feedback/<dep>/ledger.md`。
锁定信息以 `third_party/dependencies.lock` 为准。
