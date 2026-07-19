# 测试设备与被测件通讯协议契约

> 适用项目：多产品通用硬件测试软件（Qt 5.15 兼容、Qt 6 Core/Network/SerialPort fallback / C++17 / Windows）
> 本文定位：产品协议资产、当前 MB_DDF CSV 解析规则、帧编解码和算法运行期语义。
> 代码事实源：`src/algorithm/include/algorithm/mbddf_protocol.h`、`src/algorithm/src/mbddf_protocol.cpp`、`src/algorithm/src/system_status_executor.cpp`。
> 状态标记：**当前**表示已实现或已由用户确认的资产基线，**目标**表示已确认但尚未实现。

原始建模参考来自 `H:/WorkSpace/PythonWorkspace/openEulerEnvironment/docs/protocol_modeling_workbench_definition.md`。该外部绝对路径只作来源追溯，不是本仓库可复现的发布输入；本项目当前实现与该参考不一致时，以本节明确列出的“当前规则”为准。

---

## 1. 分层边界

本文中的产品协议是测试设备与 DUT 之间的帧语义，不是 HAL 传输协议。

当前代码依赖和测试替身路径：

```text
BIZ -> IAlgorithmExecutor -> hwtest_algorithm_mbddf -> IByteTransport
                                                   -> SystemStatusSimulator（当前成功测试）
                                                   -> HalControlTransport -> hwtest_hal
                                                   -> HalSerialTransport -> hwtest_hal
```

统一生产路径如下；当前 `SYSTEM_STATUS` 控制通道已按该边界落地，通用 Provider 架构仍未完成：

```text
BIZ -> 算法层（CSV、帧、CRC、命令/序号、响应匹配和判定数据）
    -> HAL（逻辑资源、连接、原始 I/O、deadline、传输错误和物理安全态）
    -> providerId 路由
         -> Qt 标准 API Provider
         -> Vendor Adapter Provider
         -> Mock Provider
```

明确禁止：

- BIZ 解释 CSV 字段、构造产品帧或直接调用 HAL；
- HAL 解释产品帧头、长度、CRC、命令、序号、字段和值；
- Adapter 执行测试流程或判定；
- 将产品解析错误映射为 `HalStatusCode::ProtocolError`。

产品协议错误进入算法/BIZ 结果面，例如 `ErrorCode::ProtocolParseError`；HAL 的 `ProtocolError` 只保留给 Provider 或驱动可独立识别的传输层错误。分层总览见 [五层架构](../overview/five-layer-architecture.md)，HAL 原始 I/O 语义见 [HAL 接口协议](hal-interface-protocol.md)。

---

## 2. 当前实现范围

当前仓库已经存在 `hwtest_algorithm_mbddf`，不是纯设计占位：

| 实现 | 当前能力 |
| --- | --- |
| `ProtocolCatalog` | 从目录加载并索引严格 MB_DDF CSV 定义 |
| `encodePayload()` / `decodePayload()` | 按定义编解码 B4 至产品 payload 末尾 |
| `encodeFrame()` / `decodeFrame()` | 处理 `55 AA + LEN + payload + CRC16/XMODEM` |
| `SystemStatusAlgorithmExecutor` | 执行 `mbddf.system_status` 单步算法 |
| `SystemStatusSimulator` | 协议级成功、超时、坏 CRC 和无效响应模拟 |
| `HalControlTransport` | 经 `IControlChannel` 发送原始字节，并累积短读、搜索同步字、按长度分帧及保留剩余帧 |
| `HalSerialTransport` | 将算法字节事务桥接到现有 `ISerialBus` |

当前有两类闭环证据：直连 `SystemStatusSimulator` 验证 golden frame；Qt UDP 测试经“配置 -> BIZ -> 算法 -> `HalControlTransport` -> HAL -> `qt.udp` -> 本机模拟目标 -> 判定”。后者证明控制通道和标准 UDP Provider 集成，但不证明真实 DUT、真实网口或真实串口。`HalSerialTransport` 作为旧兼容桥接保留。

---

## 3. 资产身份与基线

当前 `ProtocolCatalog` 按“一份 CSV 对应一个 `MessageDefinition`”加载。文件名必须以 `_request.csv` 或 `_response.csv` 结尾，去掉 `.csv` 后得到定义名；同方向下 `type_group + sub_type` 不得重复。

用户已确认 `H:/Resources/RTLinux/Demos/MB_DDF_v2/docs/design/product_protocol_csv` 的当前内容为 MB_DDF 协议 CSV 基线。消息名称、字段和当前清单以该目录现状为准；加载器、测试和本文必须随该目录的受控变更同步更新。

该目录尚未形成仓库内可复现快照。交付或冻结版本时至少应记录：

- `baselineId` 和来源版本或哈希；
- CSV 文件名与内容哈希；
- 逻辑定义 ID、方向和命令键；
- 请求/响应配对关系；
- 测试步骤到定义的显式映射；
- 该基线通过的校验与测试记录。

### 3.1 已确认基线与测试对齐

截至 2026-07-19，该目录有 32 个 CSV，包含 `system_status_request.csv` 与 `system_status_response.csv`，不包含 `ad_read_response.csv`。

因此：

- `ProtocolCatalog` 按一文件一定义加载，当前期望为 32 个定义；
- `SYSTEM_STATUS` 所需两份 CSV 当前存在；
- 原测试源码中的 36 项断言和 `ad_read_response` 引用是相对批准基线的陈旧预期，现已按当前定义修正；
- 后续修改该目录时，必须同步协议测试和本节清单；测试结果应记录基线路径、观测时间和实际清单。

基线已批准不等于 manifest、内容哈希或不可变快照已经实现，也不等于 HAL 产品集成或真实硬件验收已经完成。

---

## 4. 当前 CSV 语法

当前解析器采用严格输入，不等同于通用 CSV 工作台设计：

- 编码必须是有效 UTF-8，UTF-8 BOM 可有可无；
- 表头必须逐项等于下列 8 列，额外列或缺列都会失败；
- 每个数据行必须正好 8 列；
- 空文件、只有表头、空行、未闭合引号和非法引号位置都会失败；
- 支持双引号字段和 `""` 转义；字段读取后会 trim；
- `index` 只接受 `B1` 或 `B1-4` 形式；
- `length` 必须是正整数；
- `name_cn`、`name_en` 均不能为空，`name_en` 在单文件内不得重复；
- `is_valid` 当前只接受 `0` 或 `1`，不接受 `true/yes/是` 等文本。

固定表头：

```text
index,length,type,name_cn,name_en,lsb,default,is_valid
```

| 列 | 当前语义 |
| --- | --- |
| `index` | 1 基物理帧字节位置，必须与 `length` 一致 |
| `length` | 普通字段字节数；`BIT` 为位宽 |
| `type` | 第 5 节列出的当前支持类型 |
| `name_cn` | 非空中文说明 |
| `name_en` | 非空且文件内唯一的运行期访问名 |
| `lsb` | 可选的有限正数，编码时除以 `lsb`，解码时乘以 `lsb` |
| `default` | 可选有限数；协议公共字段和 `CONST` 有更严格规则 |
| `is_valid` | `1` 可由调用方赋值，`0` 只能使用默认值；受保护公共字段不可覆盖 |

---

## 5. 当前字段类型

| CSV 类型 | 当前 C++ 类型 | 字节数 | 编解码语义 |
| --- | --- | --- | --- |
| `BIT` | `FieldType::Bit` | 1 个承载字节中的 1..8 位 | 无符号，可应用 `lsb`；同一字节的连续 BIT 必须正好覆盖 8 位 |
| `CONST` | `FieldType::Const` | 1..8 | 无符号常量，解码时必须匹配默认值 |
| `F32` | `FieldType::F32` | 4 | IEEE 754，小端位模式，可应用 `lsb` |
| `RESERVED` | `FieldType::Reserved` | 由 `length` 指定 | 必须为零，解码发现非零即失败 |
| `S16` | `FieldType::S16` | 2 | 小端有符号整数，可应用 `lsb` |
| `S16F` | `FieldType::S16F` | 2 | 小端有符号定标整数 |
| `S32F` | `FieldType::S32F` | 4 | 小端有符号定标整数 |
| `U8` | `FieldType::U8` | 1 | 无符号整数，可应用 `lsb` |
| `U16` | `FieldType::U16` | 2 | 小端无符号整数，可应用 `lsb` |
| `U32` | `FieldType::U32` | 4 | 小端无符号整数，可应用 `lsb` |

通用建模参考中的 `ANY`、`S8`、`U8F`、`S8F`、`U16F`、`U32F`、`S32`、`F64`、结构数组和通用位容器目前不是已实现能力。新增类型必须同步扩展 `FieldType`、解析、编解码、校验、测试和本文，不得仅修改 CSV。

---

## 6. MB_DDF 帧与布局约束

当前解析器不是任意帧格式引擎，而是带固定公共字段的 MB_DDF 解析器。

每份定义的前七个字段必须依次为：

| 位置 | `name_en` | 类型 | 当前约束 |
| --- | --- | --- | --- |
| `B1` | `sync[0]` | `CONST` | 默认字面量 `0x55` |
| `B2` | `sync[1]` | `CONST` | 默认字面量 `0xAA` |
| `B3` | `len` | `U8` | 默认值只能是 48 或 123 |
| `B4` | `version` | `CONST` | 默认字面量 `0x11` |
| `B5` | `type_group` | `U8` | 必须有 8 位无符号默认值 |
| `B6` | `sub_type` | `U8` | 必须有 8 位无符号默认值 |
| `B7-8` | `seq` | `U16` | 由执行器写入，调用方不能覆盖 |

布局规则：

- 字段必须从 B1 开始连续覆盖，不能有空洞或重叠；
- 普通字段的 `index` 跨度必须等于 `length`；
- 同一字节的连续 `BIT` 字段按低位到高位排列，累计必须正好 8 位；
- `len` 表示从 B4 开始的产品 payload 长度，当前只允许 48 或 123；
- 最后一个字段必须名为 `crc`、类型为 `U16`，位于完整帧末尾；
- 完整物理帧为 `55 AA + LEN + payload + CRC_LO CRC_HI`；
- CRC 使用 CRC-16/XMODEM，对 `LEN + payload` 计算，并以小端顺序附加；
- 编解码前会再次验证定义，防止调用方传入被篡改的 `MessageDefinition`。

`encodePayload()` / `decodePayload()` 只处理 B4 至 payload 末尾，不包含 B1-B3 和尾部 CRC；`encodeFrame()` / `decodeFrame()` 负责物理信封。

---

## 7. `SYSTEM_STATUS` 配置与执行

当前 `SystemStatusAlgorithmExecutor` 只支持 `algorithmId = "mbddf.system_status"`。BIZ 将 `TestConfig.executionConfig` 原样传给 `prepare()`；执行器实际收到的 map 形状为：

```json
{
  "protocolAssetRoot": "${MB_DDF_PROTOCOL_CSV_DIR}",
  "protocol": {
    "requestProfileId": "system_status_request",
    "responseProfileId": "system_status_response"
  },
  "transport": {
    "openTimeoutMs": 1000,
    "readChunkBytes": 260
  },
  "initialSequence": 4660
}
```

当前语义：

- `protocolAssetRoot` 可由 `${ENV}` 展开；缺失时回退环境变量 `MB_DDF_PROTOCOL_CSV_DIR`；
- 请求/响应 ID 直接作为 `ProtocolCatalog` 定义名查找，缺省为两份 `system_status_*` 定义；
- `transport.openTimeoutMs` 和 `readChunkBytes` 必须为正整数；
- 串口部署参数由 HAL 资源 `properties` 提供，并按 MB_DDF 当前基线配置为 614400、8E1、无流控；旧 `executionConfig.serial` 仍只作兼容校验输入；
- `initialSequence` 必须是 16 位整数；每次执行后递增；
- 响应必须匹配配置的响应命令，并回显请求序号；
- CRC、命令或序号失败由算法返回 `ProtocolParseError`，传输超时返回 `BusTimeout`。

算法不选择 Provider 或物理端点。`control.resourceId`、资源 `providerId`、串口参数、UDP 端点、设备 match、SDK 和扫描结果只属于 HAL 部署配置；当前样例见 `configs/mbddf_pc_hal.json`。把 `control.resourceId` 设为 `CONTROL_SERIAL` 或 `CONTROL_NETWORK` 即可在 PC 每次运行前选择控制口，不向产品端发送切换命令。

当前 `ProtocolProfile` 列表由 BIZ 保存和透传，但 `SystemStatusAlgorithmExecutor` 没有把它与 `executionConfig.protocol.*ProfileId`、CSV 命令键或 HAL 资源做交叉校验。该绑定仍是未实现项，不能仅凭两个同名 Profile 宣称映射已建立。

目标映射应显式包含：

```text
operationId
  -> requestProfileId / responseProfileId
  -> command key
  -> sequence rule / CRC rule / deadline
  -> HAL logical ResourceId
```

`channelId` 是算法可见的 HAL 逻辑资源。`providerId`、物理端点、SDK、扫描结果以及 Qt/Vendor/Mock 路由只属于 HAL 部署配置，禁止进入协议 CSV。

---

## 8. 流式收发规则

串口和未来 TCP 是字节流，一次 HAL `read` 与一个产品帧不构成一一对应关系。算法传输实现必须在同一 deadline 内：

1. 发送完整请求字节；
2. 累积零个、一个或多个原始字节块；
3. 根据同步字、长度和上限识别候选帧；
4. 完成 CRC、命令、方向、序号和响应配对；
5. 保留剩余字节供后续帧处理。

单次 HAL 读取返回短字节块不是协议错误。只有算法在 deadline 内仍无法形成合法候选帧时，才产生超时或产品协议诊断。

当前 `HalControlTransport` 已实现同步字搜索、长度分帧、短读累积、前导噪声丢弃和剩余帧保留，并以一次事务总预算驱动 HAL 读写。CRC、方向、命令和序号仍由 `SystemStatusAlgorithmExecutor`/协议 codec 校验。旧 `HalSerialTransport` 仍是一次 `transactSerial()` 的兼容骨架，不作为当前产品路径。

CAN/CANFD 的帧边界由总线提供，但 payload 内的 MB_DDF 字段、CRC、命令、序号及响应关系仍由算法解释。

---

## 9. 日志与追踪

协议日志与 HAL 日志分开归属：

| 日志 | 应记录 |
| --- | --- |
| 算法/协议 | 定义名、命令键、序号、候选帧长度、CRC、字段诊断、判定输入 |
| HAL | `ResourceId`、连接、原始读写、deadline、耗时、安全态和归一传输错误 |

两侧复用同一 `requestId`。原始帧默认只记录长度、摘要和受限十六进制片段；完整原始帧需显式调试开关，并遵循数据敏感性要求。结构化字段主定义见 [日志接口协议](log-interface-protocol.md)。

---

## 10. 派生物与扩展

可以从已批准 CSV 基线生成 C++ 辅助类型、Schema 或协议说明，但这些生成物：

- 只属于算法/协议模块，不得进入 HAL 公共头；
- 必须记录输入基线、生成工具版本和输出哈希；
- 不得反向覆盖 CSV 或掩盖测试预期相对批准基线的偏差；
- 在未接入构建和验证前只能标记为扩展点。

若要把当前 MB_DDF 解析器扩展为通用协议工作台，应另行评审可变帧头、字节序、字段表达式、数组、条件字段和版本迁移，不在当前契约中预先冻结未使用的抽象。

---

## 11. 测试与验收

最低验证边界：

- CSV：UTF-8/BOM、精确表头、列数、空行、引号、文件名和重复命令；
- 定义：公共字段、48/123 长度、连续布局、BIT 8 位覆盖和尾部 CRC；
- 编解码：常量、保留字节、定标、符号扩展、F32、小端和 CRC16/XMODEM；
- `SYSTEM_STATUS`：golden request、成功响应、坏 CRC、错误命令、序号不匹配和超时；
- 纯协议单测可直连 Simulator；产品模拟和算法集成必须经过 HAL，并标明是 HAL Mock 或标准 Provider 隔离模拟目标；
- 真实硬件协议测试单独标记，不进入默认 CI。

当前验收限制：批准基线仍是仓库外依赖，尚无 manifest/hash 可复现快照；控制通道 Mock Provider、真实串口和真实 DUT 闭环未实现。Qt UDP 本机模拟目标和流式分帧已有自动化证据。完整证据等级和执行命令统一见 [测试规范](../testing/testing-specification.md)。
