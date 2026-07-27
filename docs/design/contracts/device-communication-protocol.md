# 测试设备与被测件通讯协议契约

> 适用项目：多产品通用硬件测试软件（Qt 5.15 兼容、Qt 6 Core/Network/SerialPort fallback / C++17 / Windows）
> 本文定位：产品协议资产、当前 MB_DDF CSV 解析规则、帧编解码和算法运行期语义。
> 代码事实源：`src/algorithm/include/algorithm/mbddf_protocol.h`、`src/algorithm/src/mbddf_protocol.cpp`、`src/algorithm/src/system_status_executor.cpp`、`src/algorithm/include/algorithm/mbddf_exchange_executor.h`、`src/algorithm/src/elec_health_status_executor.cpp` 和 `src/algorithm/src/di_stimulus_controller.cpp`。
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

统一协议生产路径如下；当前七个 MB_DDF 单步配置均经该边界进入控制通道：

```text
BIZ -> 算法层（CSV、帧、CRC、命令/序号、响应匹配和判定数据）
    -> HAL（逻辑资源、连接、原始 I/O、deadline、传输错误和物理安全态）
    -> 控制资源 providerId 路由（qt.serial / qt.udp）
    -> 或设备 adapterId 路由（MockAdapter / 通用 C ABI DLL / 可选 NI-DAQmx DLL）
```

`mbddf.di_read` 的 `DI_READ` 帧交换仍走上述控制通道；其外部 DI 刺激不是产品协议字段，而是独立的应用动作链：`TestApplicationController -> DiStimulusController -> IHalDevice / IDigitalIo::writeDoBatch -> 设备 adapterId 后端`。`DiStimulusController` 不含 NI 类型或 SDK；BIZ 只保存和透传不透明 `executionConfig`，不解析刺激通道，也不直接访问 HAL。严格 WebSocket 输入格式与动作时序见 [WebSocket 前端协议](websocket-frontend-protocol.md)。

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
| `SystemStatusAlgorithmExecutor` | 执行 `mbddf.system_status`，并提供固定命令执行的共享生命周期 |
| `ElecHealthStatusAlgorithmExecutor` | 执行 `mbddf.elec_health_status` 单步算法 |
| `MbdDfExchangeAlgorithmExecutor` | 按配置执行 `MEMPERF_TEST`、`SPI_FLASH_TEST`、`DH_PULSE_CONFIG` 等单步请求/响应；可按配置追加一个清理请求（当前用于 `TIMER_JITTER_STOP`） |
| `DiStimulusController` | 解析 `executionConfig.digitalStimulus`，按配置白名单和 revision 构造完整 DI 输出批次；不调用厂家 SDK |
| `SystemStatusSimulator` | 协议级成功、超时、坏 CRC 和无效响应模拟 |
| `HalControlTransport` | 经 `IControlChannel` 发送原始字节，并累积短读、搜索同步字、按长度分帧及保留剩余帧 |
| `HalSerialTransport` | 将算法字节事务桥接到现有 `ISerialBus` |

当前有三类闭环证据：直连 `SystemStatusSimulator` 验证 golden frame；Qt UDP 测试经“配置 -> BIZ -> 算法 -> `HalControlTransport` -> HAL -> `qt.udp` -> 本机模拟目标 -> 判定”；2026-07-26 的受控实机 smoke 经同一宿主链路改走 `qt.serial -> COM3 -> MB_DDF_v2`，分别完成 `SYSTEM_STATUS` 和 `ELEC_HEALTH_STATUS` 的单次及三轮 PC 周期。其余五项目前有配置、协议、脚本化执行器、Mock/Fake 或本机模拟证据；PXI-6259 NI-DAQmx 专用 Adapter 已有源码、可选 CMake 与 Fake NIDAQmx 回归，但没有真实板端、真实 NI SDK 运行或 PXI-6259 结论。早期 SYSTEM_STATUS 实机执行每轮出现 Qt 工作线程计时器警告；BIZ worker 迁移为 QThread 并补 dispatcher/计时器回归后，两个既有测试项均再次成功，后端完整诊断未再出现该警告。长时和异常收尾仍未覆盖，因此不等于全面真实硬件验收。`HalSerialTransport` 作为旧兼容桥接保留。

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

截至 2026-07-19，该目录有 32 个 CSV，包含 `system_status_request.csv`、`system_status_response.csv`、`elec_health_status_request.csv` 与 `elec_health_status_response.csv`，不包含 `ad_read_response.csv`。

因此：

- `ProtocolCatalog` 按一文件一定义加载，当前期望为 32 个定义；
- 七个当前配置所需的 14 份请求/响应 CSV 当前存在；
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

## 7. MB_DDF 单步配置与执行

当前应用控制器通过统一注册表接受七个已知的独立单步算法：`mbddf.system_status`、`mbddf.elec_health_status`、`mbddf.memperf`、`mbddf.spi_flash`、`mbddf.dh_pulse_config`、`mbddf.timer_jitter` 和 `mbddf.di_read`。每份配置只能启用其中一个步骤；不支持把两个项目拼成一个多步骤生产测试。前两项使用专用执行器，其余五项使用同一份有界请求/响应交换执行器；`TIMER_JITTER` 在成功 START 后按配置再发送 STOP 清理请求，PC 周期由 BIZ 调度。

`SYSTEM_STATUS` 的 `executionConfig` 形状为：

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

`ELEC_HEALTH_STATUS` 使用独立的 `configs/mbddf_elec_health.testcfg.json`，配置只把 Profile 替换为 `elec_health_status_request` / `elec_health_status_response`。请求命令为 `type_group=0x05`、`sub_type=0x01`，其保留填充字节由 CSV 定义；响应解码为 `status`、`err_code`、`c_volt`、`b_volt`、`activate_bits`、`external_vol`、`core_vol`、`assist_vol`、`v28_5`、`js_5V`、`dyt_5V`、`power_24V` 和 `value_YX`。当前判定标准只有 `status == 0` 且 `err_code == 0`，电压和模拟量仅作为样本输出，不在配置中推导或增加未经批准的阈值。该命令没有设备侧 START/STOP 配对，因此支持单次和 PC 周期，不支持 `device_stream`。

新增配置的命令映射如下：

| 配置 | algorithmId | 请求/响应 | 当前参数和收尾语义 |
| --- | --- | --- | --- |
| `configs/mbddf_memperf.testcfg.json` | `mbddf.memperf` | `memperf_test_request/response` | `memperf_type`、`length`、`seed` 由 `step.parameters.protocol.requestValues` 提供；响应 `error_count` 默认要求为 0 |
| `configs/mbddf_spi_flash.testcfg.json` | `mbddf.spi_flash` | `spi_flash_test_request/response` | 空请求；DUT 擦写固定隔离 4 KiB 测试区，不备份、不恢复，配置仅支持单次 |
| `configs/mbddf_dh_pulse_config.testcfg.json` | `mbddf.dh_pulse_config` | `dh_pulse_config_request/response` | `config_enable` 与 23 路 `pulse_width[]` 写入后逐项回读并判定 |
| `configs/mbddf_timer_jitter.testcfg.json` | `mbddf.timer_jitter` | `timer_jitter_start_request/response`，随后 `timer_jitter_stop_request/response` | START 的 `mode` 当前默认为 0；`executionConfig.lifecycle` 声明 STOP Profile 和 deadline，STOP ACK 失败会使本轮失败 |
| `configs/mbddf_di.testcfg.json` | `mbddf.di_read` | `di_read_request/response`（`DI_READ`） | 读取 `di_state[0]`、`di_state[1]`；当前配置支持 `single` 和 `pc_periodic`，判定只检查 `status == 0` 与 `err_code == 0` |

`mbddf.di_read` 的 `executionConfig.digitalStimulus` 是算法层消费的执行配置，不进入 CSV 或产品帧。`DiStimulusController` 要求 1..64 个通道、唯一且非空的 `switchId`/`resourceId`、唯一的 0..63 `dutBit`、合法的 `High`/`Low` `activeLevel` 与 0..60000 ms 的 `settlingMs`。设置时必须带与当前状态精确相等的 revision；陈旧 revision 返回既有 HAL `DataMismatch`，未知 `switchId` 返回 `NotFound`，二者都不得触发输出写入。每次成功设置或复位都为全部已配置资源构造完整 `writeDoBatch()` 映射；复位的逻辑 mask 为 0，低有效通道仍写其 inactive 物理电平。写成功后才递增 revision、更新 applied mask 和写入时间，写失败保留已应用状态并记录 HAL 错误。

当前 DI 配置声明 16 路 `DUT_DI*_STIM`、`di0` 至 `di15` 以及 `di_state[0]`/`di_state[1]` 展示字段。控制器加载时还要求每个刺激资源属于配置的 stimulus 设备、是数字输出，且 HAL `safeState` 等于该通道的逻辑 inactive 电平；不一致会以 `stimulus_safe_state_mismatch` 拒绝，尚未打开硬件。刺激与 DUT 回读不一致不是该测试的产品判定条件；产品判定仍只依据 `status` 和 `err_code`。配置中的 `ni.daqmx` 对应可选的 PXI-6259 原生 NI-DAQmx Adapter：Router 用驱动级配置初始化 Vendor Adapter，HAL 的 `openDevice()` 再把单设备身份、该设备全部已配置资源和 safe state 放入版本化 open projection；动态 fixture 回归锁定了两阶段 JSON 边界。MAX 名称位于 `hardware.devices[].properties.vendor.ni.deviceName`（当前模板为 `PXI1Slot2`），`serialNumber=CONFIGURE_ME` 会在投影解析时阻止误开。任务档案可随投影携带，但当前产品协议和 `DiStimulusController` 不创建 NI 采样任务。Fake 覆盖不能替代真实 I/O 验收；必须填入真实 DLL/身份并在隔离台架验收，不能由此推导真实输出或真实回读已验证。

`executionConfig.lifecycle` 只允许在需要清理 ACK 的单步中使用：请求/响应 Profile 必须成对提供，执行器先完成主请求/响应，再以递增序号发送 follow-up 请求并校验响应命令、序号、`status` 和 `err_code`。这不是设备持续回告能力；当前定时器 START 在 DUT 端同步完成 250 us x 250 周期统计，STOP 只负责幂等清理确认。

七个当前配置的 `reportFields` 还包含应用层展示元数据：`title`、`description`、`supportedRunModes` 和 `measurements`。这些字段只由应用层投影为 WebSocket descriptor，`measurements` 的 `id`、`label`、`unit`、`primary` 不能改变算法判定、协议编解码或 HAL 安全行为；缺少展示元数据时，前端回退到步骤身份和样本字段。

算法不选择 Provider 或物理端点。`control.resourceId`、资源 `providerId`、串口参数、UDP 端点、设备 match、SDK 和扫描结果只属于 HAL 部署配置；当前样例见 `configs/mbddf_pc_hal.json`。把 `control.resourceId` 设为 `CONTROL_SERIAL` 或 `CONTROL_NETWORK` 即可在 PC 每次运行前选择控制口，不向产品端发送切换命令。

当前 `ProtocolProfile` 列表由 BIZ 保存和透传，但 MB_DDF 执行器仍没有把它与 `executionConfig.protocol.*ProfileId`、CSV 命令键或 HAL 资源做完整交叉校验。该绑定仍是未实现项，不能仅凭同名 Profile 宣称映射已建立。

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

当前 `HalControlTransport` 已实现同步字搜索、长度分帧、短读累积、前导噪声丢弃和剩余帧保留，并以一次事务总预算驱动 HAL 读写。CRC、方向、命令和序号仍由 MB_DDF 固定命令执行器/协议 codec 校验。旧 `HalSerialTransport` 仍是一次 `transactSerial()` 的兼容骨架，不作为当前产品路径。

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
- `ELEC_HEALTH_STATUS`：独立配置加载、`0x05/0x01` 请求、响应字段解码、`status/err_code` 判定和经 HAL 的 UDP/串口路径；
- `MEMPERF_TEST`、`SPI_FLASH_TEST`、`DH_PULSE_CONFIG`：配置驱动请求/响应、CSV 字段编解码、响应判定和错误路径；
- `TIMER_JITTER_START/STOP`：START 统计响应、递增序号的 STOP 清理 ACK 和清理失败判定；
- `DI_READ`：`0x04/0x01` 请求、响应字段 `di_state[0]`/`di_state[1]`、序号/CRC、单样本与 `status`/`err_code` 判定；
- 数字刺激：配置白名单、重复/范围拒绝、active-low 映射、完整批量写、revision 冲突无写入、写失败状态保留和复位；这些用 `IHalDevice` Fake 或 Mock 验证，不是厂商 Adapter 或真机证据；
- 纯协议单测可直连 Simulator；产品模拟和算法集成必须经过 HAL，并标明是 HAL Mock 或标准 Provider 隔离模拟目标；
- 真实硬件协议测试单独标记，不进入默认 CI。

当前验收限制：`dut/` 已保存来源提交 `982b3f5bbce222aea061e9ce1523ba926c801658` 的 32 份 CSV 同步副本，但宿主运行期仍使用仓库外批准基线，且尚无 manifest/hash 自动机制；控制通道 Mock Provider 未实现。Qt UDP 本机模拟目标和流式分帧已有自动化证据；BIZ QThread dispatcher 回归已补齐，真实 COM3/DUT 也已有迁移后成功且无目标计时器警告的短时复测，但长时、自动化异常路径和物理安全收尾尚未完成。通用 C ABI、多 Adapter 和原生 PXI-6259 NI-DAQmx Adapter 的自动化都只使用 Fake/fixture；NI-DAQmx 软件路径已实现但没有真实 PXI-6259 验收。完整证据等级和执行命令统一见 [测试规范](../testing/testing-specification.md)。
