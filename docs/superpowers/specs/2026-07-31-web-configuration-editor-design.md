# Web 配置编辑与工位配置设计

日期：2026-07-31

状态：已于 2026-07-31 实施；当前行为以 `docs/design/` 下的现行事实源和已核对源码为准。

现行事实源仍以 `docs/design/` 下的架构、契约和测试规范为准；本文只保留当时的设计决策，不替代现行事实源或已核对源码。

## 1. 目标

在当前 Web 主链路中提供三个持久化配置入口：

1. 读取、编辑并保存当前 `configs/*.testcfg.json`；
2. 启用、停用并排序多个 testcfg，停用项不作为可选测试；
3. 配置本机串口、波特率、PXI-6259/PXI-6733 身份及其物理端口映射。

配置页面中的保存都必须由后端写入文件。现有“本次运行参数”仍是单次运行覆盖，不属于配置页面，也不回写 testcfg。

## 2. 范围

### 2.1 本期包含

- 当前 `.testcfg.json` 的渐进式表单编辑；
- `executionConfig` 等未表单化扩展对象的高级 JSON 编辑；
- 独立测试目录 `configs/test-config-catalog.json`；
- 独立工位覆盖配置 `configs/mbddf_station.json`；
- WebSocket 配置读取、保存、revision 冲突和保存后重载；
- 前端配置管理页及必要回归测试。

### 2.2 本期不包含

- 创建、删除或重命名 testcfg；
- 编辑 legacy 配置、基础 `mbddf_pc_hal.json`、safe state、Adapter/Provider 类型或任意 HAL 拓扑；
- 完整动态表单引擎、角色权限、远程配置服务、审计数据库或硬件自动发现；
- 清理全部历史兼容字段、前端产品特例或配置模型遗留问题；
- TUI、Qt GUI 的配置编辑界面；
- 真实串口、PXI 或目标板台架验收。

## 3. 持久化边界

| 配置内容 | 持久化文件 | 所有者 |
| --- | --- | --- |
| 测试定义、判据、超时、展示和算法执行配置 | 对应 `*.testcfg.json` | BIZ 解析，算法消费扩展字段，应用组合 |
| 测试启停与顺序 | `test-config-catalog.json` | 应用层测试目录 |
| 串口、板卡身份和物理通道覆盖 | `mbddf_station.json` | 应用层合并，HAL 消费有效配置 |
| 基础逻辑拓扑、Provider/Adapter、safe state | `mbddf_pc_hal.json` | HAL，Web 只读 |

### 3.1 测试目录

目录使用稳定文件名作为 `documentId`，不使用可编辑的 `configId` 作为持久键：

```json
{
  "schemaVersion": "1",
  "entries": [
    {
      "documentId": "mbddf_system_status.testcfg.json",
      "enabled": true,
      "order": 10
    }
  ]
}
```

- 主测试选择区只显示 `enabled=true` 且 testcfg 可加载的项；
- 配置管理页显示全部当前 testcfg，包括停用项和解析失败项；
- 新发现但未登记的 testcfg 默认停用；
- 禁用当前项后选择排序中的下一项；没有可用项时控制器回到空配置态。

### 3.2 工位配置

`mbddf_station.json` 只覆盖基础 HAL 配置中已存在的设备和资源：

```json
{
  "schemaVersion": "1",
  "control": {
    "resourceId": "CONTROL_SERIAL"
  },
  "devices": {
    "ni6259_stimulus": {
      "physicalDeviceName": "Dev1",
      "serialNumber": "123456"
    },
    "ni6733_fixture": {
      "physicalDeviceName": "Dev2",
      "serialNumber": "654321"
    }
  },
  "resources": {
    "CONTROL_SERIAL": {
      "portName": "COM7",
      "baudRate": 614400,
      "dataBits": 8,
      "parity": "Even",
      "stopBits": 1
    },
    "DUT_DI3_STIM": {
      "portNumber": 0,
      "lineNumber": 0
    },
    "HELM_PWM1_SENSE": {
      "physicalIndex": 0
    }
  }
}
```

应用层把白名单字段覆盖到 `mbddf_pc_hal.json`，再把合并后的有效配置交给现有控制器/HAL 校验。工位配置不能新增逻辑设备或资源，也不能覆盖 `adapterId`、`providerId`、模块、方向或 safe state。

## 4. 组件边界

新增一个应用层内部 `ConfigurationService`，统一负责：

- 扫描当前 testcfg；
- 读取和保存测试目录、testcfg、工位配置；
- 计算内容 SHA-256 revision；
- 根据 `documentId` 映射服务端文件；
- 合并工位配置与基础 HAL 配置；
- 调用现有 BIZ、应用和 HAL 校验入口；
- 使用 `QSaveFile` 原子提交。

`TestApplicationController` 仍是所有前端入口的唯一控制边界。WebSocket 只做 JSON DTO 与控制器动作适配，不直接访问 BIZ、算法或 HAL。

前端新增独立配置页，文档草稿保留在页面本地状态，不混入运行参数状态：

- `ConfigPage`：配置列表、启停、排序、测试编辑和硬件编辑；
- `ConfigPage` 本地状态：原始值、草稿、dirty、保存中、错误和 revision；
- `ConfigFormRenderer`：少量通用字段控件；
- `AdvancedJsonEditor`：编辑 Schema 明确开放的扩展对象。

## 5. 编辑模型

首期使用简单、可扩展的字段描述，不建设完整 JSON Schema 系统。后端返回字段路径、标签、类型、可编辑性和可选枚举；前端通用渲染已覆盖字段，其余受支持扩展使用 JSON 编辑。

以下字段只读：

- testcfg 文件名；
- `schemaVersion`；
- `configId`；
- `steps[].algorithmId`。

兼容但当前不生效的字段不在首期表单中主动开放。后续完全表单化时继续扩充字段描述和控件，不改变文档保存协议。

## 6. WebSocket 协议

保留现有 `testConfigs` 和 `selectTest`，其中 `testConfigs` 改为返回目录中启用且可加载的测试。新增三个动作：

### 6.1 `configCatalog`

参数为空，返回目录 revision 和管理项：

```json
{
  "revision": "sha256-hex",
  "items": [
    {
      "documentId": "mbddf_system_status.testcfg.json",
      "configId": "mbddf-system-status",
      "title": "系统状态",
      "enabled": true,
      "order": 10,
      "valid": true,
      "message": ""
    }
  ]
}
```

### 6.2 `configDocument`

参数为服务端目录中的 `documentId`。返回文档种类、revision、JSON 值和简化字段描述。客户端不能传文件路径。

目录和工位配置使用保留 documentId，例如 `test-config-catalog` 与 `mbddf-station`；testcfg 使用文件名。

### 6.3 `saveConfig`

请求包含：

```json
{
  "documentId": "mbddf_system_status.testcfg.json",
  "expectedRevision": "sha256-hex",
  "value": {}
}
```

后端执行 revision、文档结构和现有业务校验，通过后原子写入。需要重载的文档仅在重载成功后响应新 revision 和保存后的规范值；重载失败时，只有磁盘仍是本次候选 revision 才恢复保存前内容，并返回 `config_reload_failed`。

- 保存当前 testcfg 后立即重载；
- 保存非当前 testcfg 后刷新目录；
- 保存目录后应用启停与排序；
- 保存工位配置后重新加载当前配置；
- 准备、运行、暂停、停止、清理或分析期间拒绝保存；读取仍允许。

## 7. 必要保护与错误

本期只保留三项保护：

1. `QSaveFile` 原子写入，失败不破坏原文件；
2. revision 乐观检查，检测本次保存所见内容已经陈旧的情况；
3. 活动测试期间禁止保存，防止运行配置中途变化。

路径始终由服务端 `documentId` 映射，客户端不能提交任意路径。沿用当前回环地址、Origin 和单客户端模型，不新增账号、角色、写开关或远程授权系统。

错误保持简洁：

- `config_not_found`；
- `config_invalid`；
- `config_conflict`；
- `config_reload_failed`；
- `config_save_failed`；
- `invalid_state` / `command_in_progress`。

回复以现有 `code` 和 `message` 为主，能定位字段时可附带单个 JSON Pointer `path`，不建设复杂错误聚合协议。

## 8. 配置版本一致性

控制器从同一轮文件读取中取得当前 testcfg、工位配置和基础 HAL 的内容与 revision，并先在临时配置服务中完成全部校验，成功后才替换存储绑定。UI 保存当前配置后立即重载；`load` 与 `prepare()` 前重新确认目录选择仍启用有效，`prepare()` 还会检查三份源文件漂移。该检查配合 `QSaveFile` 只提供尽力而为的乐观并发保护，不是跨进程文件锁或强 compare-and-swap。

本期不修改 `ITestRunService` 公共虚接口，也不建设完整内存配置快照 API。后续大改可把控制器、算法和 BIZ 统一迁移为同一内存快照，本期 revision 门禁作为过渡实现。

## 9. 最小验证范围

只要求以下关键回归：

- 配置读取、保存、自动重载和重启后仍生效；
- 启用/停用正确改变主测试列表；
- 工位配置正确覆盖串口和板卡字段，基础 HAL 文件不被修改；
- revision 冲突不静默覆盖；
- 非法内容或保存失败不破坏原文件，候选内容重载失败时条件回滚；
- 损坏目录拒绝启动，基础 HAL 漂移会阻止 `prepare()`；
- 一个前端编辑、保存、刷新流程。

修改前端后仍执行 `npm test` 和 `npm run build`；后端运行受影响的 BIZ、应用和 WebSocket 定向测试。Fake、Mock 和回环测试不构成真实串口、PXI 或目标板验收。
