# Web 配置编辑与工位配置实施计划

> **状态：** 已于 2026-07-31 实施。长期事实已同步到 `docs/design/` 下的架构、契约和测试规范；本文件仅保留实施清单。

**目标：** 在 Web 主链路中交付 testcfg 编辑、测试启停/排序和工位硬件配置，并由后端原子持久化到仓库内配置目录。

**架构：** 应用层 `ConfigurationService` 管理三个配置边界；`TestApplicationController` 继续作为唯一前端控制入口；WebSocket 只适配 DTO；前端使用渐进式表单和高级 JSON 编辑。

**约束：** 不编辑 legacy、基础 HAL/safe state 或公共协议 CSV；不新增权限系统、远程服务、硬件扫描或完整 Schema 引擎；不改变 `ITestRunService` 虚接口。

---

## 阶段 1：配置文件模型与服务

- [x] 添加 `configs/test-config-catalog.json`，登记当前 testcfg 的启用状态和顺序。
- [x] 添加 `configs/mbddf_station.json`，初始覆盖与基础模板等效并保持现有 HAL 行为。
- [x] 为 `TestConfigManager` 增加从 JSON 对象/字节解析并校验的非破坏性入口，复用现有文件加载逻辑。
- [x] 新增应用内部 `ConfigurationService`，实现扫描、documentId 映射、revision、工位覆盖和 `QSaveFile` 保存。
- [x] 添加聚焦测试：目录过滤、未知文件默认停用、工位覆盖、冲突和失败不破坏原文件。

## 阶段 2：控制器集成与版本门禁

- [x] 在应用 DTO 中增加配置目录项、文档、简化字段描述和保存结果。
- [x] 给 `TestApplicationController` 增加目录读取、文档读取和保存动作。
- [x] 保存当前 testcfg 或工位配置后自动重新加载；保存其他 testcfg 后只刷新目录。
- [x] 禁用当前测试时选择下一可用测试，无可用项时回到空配置态。
- [x] 配置加载以临时服务完成校验，失败不改变先前存储绑定；目录禁用项在 `load`/`prepare()` 前不会复活。
- [x] 从同一轮读取记录当前 testcfg、工位和基础 HAL revision，并在 `prepare()` 前拒绝外部版本漂移。
- [x] 当前配置保存后若自动重载失败，只在候选 revision 未再次变化时恢复原文件并返回 `config_reload_failed`。
- [x] 添加控制器聚焦测试，锁定阶段门禁、自动重载和 revision 行为。

## 阶段 3：WebSocket 配置动作

- [x] 扩展 `web_protocol` 动作白名单和 DTO，加入 `configCatalog`、`configDocument`、`saveConfig`。
- [x] 保持 `testConfigs`、`selectTest` 向后兼容，并改为使用最新启用目录；动态选择后再次 `load` 保持当前项。
- [x] 排队到达的 `selectTest` 与 `load` 以控制器当前选择为准，不使用过期启动路径覆盖。
- [x] 在 WebSocket server 中通过 queued invocation 调用控制器，不直接操作文件或下层服务。
- [x] 校验 `documentId`、`expectedRevision` 和 `value` 类型，拒绝客户端路径。
- [x] 添加协议与集成测试：正常读取/保存、冲突、损坏目录、活动阶段拒绝、保存后重载和失败回滚。

## 阶段 4：前端配置管理页

- [x] 在协议类型和 `HwtestClient` 中增加配置目录、文档和保存解析。
- [x] 新增配置 feature/store，管理草稿、dirty、saving、错误和 revision。
- [x] 新增配置页：测试启停/排序、testcfg 基础表单、`executionConfig` 高级 JSON、工位硬件表单。
- [x] 锁定文件名、`schemaVersion`、`configId` 和 `algorithmId`。
- [x] 保存成功后使用服务端返回值重置草稿，并刷新主测试选择和当前 descriptor。
- [x] 添加最小交互测试：编辑保存、启停过滤、冲突保留草稿和硬件字段持久化。
- [x] 运行 `npm test` 与 `npm run build`。

## 阶段 5：收口当前配置驱动缺口

- [x] 确认配置管理页不会把兼容但不生效的字段展示为普通可编辑能力。
- [x] 确认现有运行参数 localStorage 与配置文件保存具有不同标签和入口。
- [x] 确认工位配置只覆盖基础 HAL 中允许的设备/资源属性，不改变 safe state 或路由类型。
- [x] 用当前实际扫描结果检查所有启用 testcfg 均可进入主测试目录。

## 阶段 6：事实源与验证

- [x] 更新五层架构中的应用配置职责与三个持久化边界。
- [x] 更新 BIZ、HAL 和 WebSocket 契约；当前实现只写已实现能力和限制。
- [x] 更新测试规范、根 README 与前端 README 的操作入口和证据边界。
- [x] 设置仓库内 `MB_DDF_PROTOCOL_CSV_DIR`，运行受影响的 BIZ、应用和 WebSocket 定向测试。
- [x] 运行前端测试/构建、配置 JSON 解析、本地链接/术语检查和 `git diff --check`。
- [x] 大改完成后运行 `codegraph sync .`；若 CodeGraph 服务仍不可用，记录该限制并以源码、测试和 diff 为准。
- [x] 明确记录未执行真实串口、PXI-6259/PXI-6733 或目标板验收。
