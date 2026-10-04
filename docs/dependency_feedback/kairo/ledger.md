# kairo 依赖台账（索引镜像）

> 状态：Active
> 说明：kairo 的反馈台账由仓库模板约定承载于固定路径
> [`docs/kairo_feedback/ledger.md`](../../kairo_feedback/ledger.md)。
> 本文件是依赖台账统一索引下的占位镜像，不重复登记条目，所有 `EXE-*` 记录以上述
> canonical 文件为准。

## 依赖身份

- 来源：https://github.com/Linductor-alkaid/kairo
- 锁定：`d9602ea6762806be320b9543e3b36f27f7dd5b1a`（tag v0.6.0，
  `third_party/dependencies.lock`）
- 许可证：MIT（见上游 LICENSE；原 executor 时代为 Apache-2.0，随上游更名变更）
- 类别：external（FetchContent 源码引入 / 本地 pinned clone）
- 沿革：原依赖名 executor（pin `4731b16`，Apache-2.0）；2026-10-04 随上游更名迁移为
  kairo 0.6.0，见 canonical 台账 EXE-20261004-001。
