# 依赖反馈台账索引

Rin 要求所有第三方依赖分别建立独立台账（AGENTS.md“依赖独立台账”节）。
一个依赖一份台账，记录依赖身份、集成决策、问题、缺口与上游反馈状态；纪律与 Executor
台账一致（工程规范 9.4）。

| 依赖 | 台账位置 | 编号前缀 | 锁定版本（commit/版本号） | 类别 |
| --- | --- | --- | --- | --- |
| kairo | [../../kairo_feedback/ledger.md](../../kairo_feedback/ledger.md)（模板固定路径） | `EXE-` | `d9602ea6762806be320b9543e3b36f27f7dd5b1a`（tag v0.6.0） | external |
| kairo（索引镜像） | [kairo/ledger.md](kairo/ledger.md) | — | 同上 | external |
| librealsense2 | [librealsense/ledger.md](librealsense/ledger.md) | `LRS-` | `2.58.3`（源 `7c3ee3fb7c640e9f315e663907208cb56c4febfd`） | system |
| eui-neo | [eui-neo/ledger.md](eui-neo/ledger.md) | `EUI-` | `4691fc0a5c1fde6f3e22f1ac454ed87c7a17f722`（上游 dev） | external |
| kissfft | [kissfft/ledger.md](kissfft/ledger.md) | `KIS-` | `131.2.0`（源 `7bce4153c6bc8aba2db0e889e576f9d00505cbe1`） | external |

新增依赖时：在本表追加一行，并创建 `docs/dependency_feedback/<dep>/ledger.md`。
锁定信息以 `third_party/dependencies.lock` 为准。
