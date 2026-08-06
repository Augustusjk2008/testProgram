# 测试设备与被测件通讯协议契约

> 适用项目：多产品通用硬件测试软件（Qt 5.15 兼容、Qt 6 Core/Network/SerialPort fallback / C++17 / Windows）
> 本文定位：产品协议资产、当前 MB_DDF CSV 解析规则、帧编解码和算法运行期语义。
> 宿主代码事实源：`src/algorithm/` 中的协议目录加载、编解码、运行参数和各算法执行器。
> 产品端事实源：`dut/src/` 与 `dut/docs/design/product_protocol_csv/`；仓库外副本不参与当前设计或验证。
> 状态标记：**当前**表示已实现或已由用户确认的资产基线，**目标**表示已确认但尚未实现。

---

## 1. 分层边界

本文中的产品协议是测试设备与 DUT 之间的帧语义，不是 HAL 传输协议。

当前代码依赖和测试替身路径：

```text
BIZ -> IAlgorithmExecutor -> hwtest_algorithm_mbddf -> IByteTransport
                                                   -> SystemStatusSimulator（协议级测试替身）
                                                   -> DhIgniteStreamAlgorithmExecutor（DH 点火有限设备流）
                                                   -> ImuStreamAlgorithmExecutor（设备持续流）
                                                   -> HelmStreamAlgorithmExecutor（舵机设备持续流）
                                                   -> BusEchoTransport（控制口 + 辅助 COM 双通道协调）
                                                   -> HalControlTransport -> hwtest_hal
                                                   -> HalSerialTransport -> hwtest_hal
```

统一协议生产路径如下；当前十三个 MB_DDF 配置均经该边界进入控制通道：

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
| 串口复合执行器 / `BusEchoTransport` | 在同一个 BIZ `single` 中按模式执行一次 `03/01` 内部回环或多轮 `03/02` 外部回显；每轮回显由 `BusEchoTransport` 在 5 秒事务预算内协调产品控制资源和用户选择的辅助串口，短读累积、校验并原样回写一帧 119 字节物理帧（114 字节 payload 加 5 字节帧封装）后读取控制响应 |
| `DhIgniteStreamAlgorithmExecutor` | 执行一次 `06/02` 请求和有限多帧回告；投影 `delay_frames` 基线/点火后分界、U16 回绕序号和理想相对时间轴，受理后不发送 STOP/ABORT |
| `ImuStreamAlgorithmExecutor` / `HelmStreamAlgorithmExecutor` | 分别执行惯测与舵机 `device_stream` 的一次 START、持续主动反馈和一次 STOP |
| `RunParameterSchema` | 由算法 ID 声明运行期可编辑字段、默认值、显示条件和语义边界；合并配置值与本次覆盖，拒绝未知、非有限或越界值 |
| `DoWriteAlgorithmExecutor` / `HelmBoardTestAlgorithmExecutor` | 执行用户配置的一次 DO_WRITE 与两路 PXI-6259 物理验证，以及 HELM_BOARD_TEST 的 manual/automatic 板级流程；夹具资源由应用层按模式绑定，物理端点由 HAL 配置解析 |
| `DiStimulusController` | 解析 `executionConfig.digitalStimulus`，按配置白名单和 revision 构造完整 DI 输出批次；不调用厂家 SDK |
| `SystemStatusSimulator` | 协议级成功、超时、坏 CRC 和无效响应模拟 |
| `HalControlTransport` | 经 `IControlChannel` 发送原始字节，并累积短读、搜索同步字、按长度分帧及保留剩余帧 |
| `HalSerialTransport` | 将算法字节事务桥接到现有 `ISerialBus` |

自动化证据分为协议级 Simulator、经 HAL 的 Qt Provider 隔离目标、Fake/Mock Adapter、DUT 主机侧测试和 AArch64 交叉构建；各自可证明的范围与当前执行结果只在[测试规范](../testing/testing-specification.md)维护。它们都不等于真实 COM、DH、DDS 舵机、PXI 或目标板验收。`HalSerialTransport` 作为旧兼容桥接保留。

---

## 3. 资产身份与基线

当前 `ProtocolCatalog` 按“一份 CSV 对应一个 `MessageDefinition`”加载。文件名必须以 `_request.csv` 或 `_response.csv` 结尾，去掉 `.csv` 后得到定义名；同方向下 `type_group + sub_type` 不得重复。

唯一批准的协议资产目录是 `dut/docs/design/product_protocol_csv/`。宿主、生成器和测试都必须使用该目录；即使实现仍允许通过环境展开路径，任何其他目录的内容和测试结果都不构成当前项目证据。

当前仓库快照尚未形成 manifest/hash 自动机制。交付或冻结版本时至少应记录：

- `baselineId` 和来源版本或哈希；
- CSV 文件名与内容哈希；
- 逻辑定义 ID、方向和命令键；
- 请求/响应配对关系；
- 测试步骤到定义的显式映射；
- 该基线通过的校验与测试记录。

### 3.1 已确认基线与测试对齐

当前批准目录有 37 个 CSV，仍不包含 `ad_read_response.csv`；舵机 START 包含 `sweep_duration_s`，反馈为最多 5 个完整 DDS 样本的 232 字节 payload。精确文件清单由该目录本身和生成器校验结果给出，不在其他入口文档复制。

因此：

- `ProtocolCatalog` 按一文件一定义加载，当前期望为 37 个定义；
- 十三个当前配置所需的协议 CSV 当前存在；
- 后续修改该目录时，必须同步生成器检查和协议测试；测试结果应记录批准目录、观测时间和实际清单。

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
| `B3` | `len` | `U8` | 默认值为 1..255 的产品 payload 长度 |
| `B4` | `version` | `CONST` | 默认字面量 `0x11` |
| `B5` | `type_group` | `U8` | 必须有 8 位无符号默认值 |
| `B6` | `sub_type` | `U8` | 必须有 8 位无符号默认值 |
| `B7-8` | `seq` | `U16` | 由执行器写入，调用方不能覆盖 |

布局规则：

- 字段必须从 B1 开始连续覆盖，不能有空洞或重叠；
- 普通字段的 `index` 跨度必须等于 `length`；
- 同一字节的连续 `BIT` 字段按低位到高位排列，累计必须正好 8 位；
- `len` 表示从 B4 开始的产品 payload 长度，必须为 1..255；
- 最后一个字段必须名为 `crc`、类型为 `U16`，位于完整帧末尾；
- 完整物理帧为 `55 AA + LEN + payload + CRC_LO CRC_HI`；
- CRC 使用 CRC-16/XMODEM，对 `LEN + payload` 计算，并以小端顺序附加；
- 编解码前会再次验证定义，防止调用方传入被篡改的 `MessageDefinition`。

`encodePayload()` / `decodePayload()` 只处理 B4 至 payload 末尾，不包含 B1-B3 和尾部 CRC；`encodeFrame()` / `decodeFrame()` 负责物理信封。

---

## 7. MB_DDF 单步配置与执行

当前应用控制器的十三份配置分别使用十三个算法 ID；注册表另保留 `mbddf.bus_loop`、`mbddf.bus_echo` 两个旧 ID，使受控旧配置仍可加载。每份配置只能启用一个步骤。`TIMER_JITTER` 的 STOP 仍只是单轮跟随清理；统一串口算法在一个 BIZ `single` 中把 `test_mode=0` 路由为一次携带全部次数的 `03/01`，或把 `test_mode=1` 路由为指定轮数的完整 `03/02` 事务；三种设备流都在一次 `executeStep()` 中产生多帧回告，其中只有 IMU/HELM 使用 STOP 收尾，DH 点火按配置帧数自然完成。

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

- `protocolAssetRoot` 可由 `${ENV}` 展开；缺失时回退环境变量 `MB_DDF_PROTOCOL_CSV_DIR`。当前项目只批准其解析到仓库内 `dut/docs/design/product_protocol_csv/`；
- 请求/响应 ID 直接作为 `ProtocolCatalog` 定义名查找，缺省为两份 `system_status_*` 定义；
- `transport.openTimeoutMs` 和 `readChunkBytes` 必须为正整数；
- 串口部署参数由 HAL 资源 `properties` 提供，并按 MB_DDF 当前基线配置为 614400、8E1、无流控；旧 `executionConfig.serial` 仍只作兼容校验输入；
- `initialSequence` 必须是 16 位整数；每次执行后递增；
- 控制通道在一次 BIZ 任务的 `prepare()` 中打开，在该任务的步骤重试、PC 周期各轮及可选跟随请求之间复用，并在同一 worker 的 `finishRun()` 中关闭；不同 `start` 任务仍按各自专用 worker 建立独立连接；
- 响应必须匹配配置的响应命令，并回显请求序号；
- 若 DUT 返回 `error_response`，且原命令与当前请求一致、原序号不是当前序号、错误码为 `0x0102` 且 `detail=0`，算法将其识别为陈旧 RX bank 回放，并以相同序号原帧重发一次；该内部重发不增加 BIZ `attempts`。当前序号错误响应、CRC、其他命令、其他错误码、超时及第二次失败均保持原有错误语义，不做宽泛自动重试；
- CRC、命令或序号失败由算法返回 `ProtocolParseError`，传输超时返回 `BusTimeout`。

`ELEC_HEALTH_STATUS` 使用独立的 `configs/mbddf_elec_health.testcfg.json`，配置只把 Profile 替换为 `elec_health_status_request` / `elec_health_status_response`。请求命令为 `type_group=0x05`、`sub_type=0x01`，其保留填充字节由 CSV 定义；响应解码为 `status`、`err_code`、`c_volt`、`b_volt`、`activate_bits`、`external_vol`、`core_vol`、`assist_vol`、`v28_5`、`js_5V`、`dyt_5V`、`power_24V` 和 `value_YX`。当前判定标准只有 `status == 0` 且 `err_code == 0`，电压和模拟量仅作为样本输出，不在配置中推导或增加未经批准的阈值。该命令没有设备侧 START/STOP 配对，因此支持单次和 PC 周期，不支持 `device_stream`。

各配置的命令映射如下：

| 配置 | algorithmId | 请求/响应 | 当前参数和收尾语义 |
| --- | --- | --- | --- |
| `configs/mbddf_serial_test.testcfg.json` | `mbddf.serial_test` | `03/01` 回环与 `03/02` 回显请求/响应 | 只允许 link 0/1/3（COM1/COM2/COM4），`test_mode` 为 0/1，统一 `cycle_count=1..100000`、默认 1000且只支持 `single`。回环一次请求把次数映射为 `total_count`，DUT 使用 `loopback=true`，完成/失败数取 DUT 回告；回显逐轮发送固定 114 字节 payload，PC 从系统枚举中选择独立本地串口，短读累积、校验并原样回写对应的 119 字节完整物理帧，每轮再校验控制响应与双向 payload，deadline 为 5 秒，轮间检查停止。应用层拒绝未枚举的辅助串口和主辅同口；最终结果保留完整聚合计数，但逐轮诊断最多保存前 15 轮和最后一轮 |
| `configs/mbddf_memperf.testcfg.json` | `mbddf.memperf` | `memperf_test_request/response` | `memperf_type`、`length`、`seed` 由 `step.parameters.protocol.requestValues` 提供；响应 `error_count` 默认要求为 0 |
| `configs/mbddf_spi_flash.testcfg.json` | `mbddf.spi_flash` | `spi_flash_test_request/response` | 空请求；DUT 擦写固定隔离 4 KiB 测试区，不备份、不恢复，配置仅支持单次 |
| `configs/mbddf_dh_pulse_config.testcfg.json` | `mbddf.dh_pulse_config` | `dh_pulse_config_request/response` | `config_enable` 与 23 路 `pulse_width[]` 写入后逐项回读并判定 |
| `configs/mbddf_dh_ignite_stream.testcfg.json` | `mbddf.dh_ignite_stream` | `dh_control_request/response`（`06/02`） | 只支持有限 `device_stream`；PC 发送一次可配 count/interval/delay/使能/通道请求，DUT 先采 `delay_frames` 帧基线、点火一次并回告到 `report_count`；`stoppable=false`，无 STOP/ABORT/自动复位 |
| `configs/mbddf_timer_jitter.testcfg.json` | `mbddf.timer_jitter` | `timer_jitter_start_request/response`，随后 `timer_jitter_stop_request/response` | START 的 `mode` 当前默认为 0；八桶依次表示 `[0,2)`、`[2,4)`、`[4,8)`、`[8,16)`、`[16,32)`、`[32,64)`、`[64,100)`、`[100,+∞)` µs；`executionConfig.lifecycle` 声明 STOP Profile 和 deadline，STOP ACK 失败会使本轮失败 |
| `configs/mbddf_di.testcfg.json` | `mbddf.di_read` | `di_read_request/response`（`DI_READ`） | 读取 `di_state[0]`、`di_state[1]`；任务面板按配置拆分显示 DI0..DI15，当前支持 `single` 和 `pc_periodic`，判定只检查 `status == 0` 与 `err_code == 0` |
| `configs/mbddf_do_write.testcfg.json` | `mbddf.do_write` | `do_write_request/response`（`DO_WRITE 04/02`） | 只支持 `single`；用户配置一次 16 位输出，完整校验 DUT `applied_state`，并由 PXI-6259 对 bit2/bit1 做外部物理验证，见 7.4 |
| `configs/mbddf_helm_board_test.testcfg.json` | `mbddf.helm_board_test` | `helm_board_test_request/response`（`HELM_BOARD_TEST 07/02`） | 只支持 `single`；`test_mode` 的 `automatic`/`manual` 是算法参数，流程与夹具边界见 7.4 |
| `configs/mbddf_imu_stream.testcfg.json` | `mbddf.imu_stream` | `09/10` START、`09/01` 主动反馈、`09/11` STOP | 只支持 `device_stream`；PC 各发送一次 START/STOP，中间只读主动反馈；至少一帧有效反馈才通过；`executionConfig.stream.hostTimestampIntervalUs` 只用于宿主合成样本时间轴，不进入产品帧 |
| `configs/mbddf_helm_stream.testcfg.json` | `mbddf.helm_stream` | `07/10` START、`07/01` 主动反馈、`07/11` STOP | 只支持 `device_stream`；DUT 以 1 ms 周期生成四路舵角指令并经 DDS 交互，最多 5 个完整反馈样本组成一帧；测试程序不管理独立舵控程序的启停 |

### 7.1 DH 点火有限设备流

`DH_IGNITE_STREAM` 复用 `DH_CONTROL 06/02` 布局，但不复用 `mbddf.dh_pulse_config` 的单次脉宽配置语义。B23-B24 当前字段名为 `delay_frames`，旧 `delay_us` 时间延迟语义已经删除。算法 Schema 投影 `power_enable`、`return_enable`、23 个通道布尔值、`report_count`、`interval_us` 和 `delay_frames`；PC 仅校验 U8/U16/布尔编码范围，允许把业务上无效但可编码的值发给 DUT。`channel[1]` 固定编码为 0。默认 power/return 均为 0、通道全不选、count 为 50、interval 为 2500 us、delay 为 5；这些安全默认值可能被 DUT 以业务错误拒绝，不会在 PC 端伪装成合法点火请求。

DUT 在任何硬件写入前完整校验：count 为 `1..65535`，interval 为 `2500..65535`，delay 小于 count，power/return 为 0 或 1，`channel[0]` 至少一位且仅 bit0..22，`channel[1]=0`。power/return 为 0 是合法测试输入；只要通道非空，DUT 仍提交点火命令，由硬件门控决定实际输出。`delay_frames=N` 表示前 N 帧只读，点火在第 N+1 帧采样前执行一次；N=0 表示首帧前点火。不存在固定第 5/6 帧分支。首帧若返回 DUT 的 `PARAM_OUT_OF_RANGE`，宿主将其标记为 `ignition_phase=request_rejected` 并立即结束；正常帧按 `baseline`/`post_ignition` 投影。

相邻采样起点至少间隔 `interval_us`，下一截止点从本帧实际采样起点计算；慢读取或发送导致顺延时不追赶。基线采集/发送失败立即结束且不得点火；点火后的采集错误仍发送当前错误帧并继续剩余帧，发送失败终止。第 i 帧响应序号为 `request_seq+i` 并按 U16 回绕。宿主收到第一帧有效响应时只取一次 PC UTC 锚点，公开 `streamElapsedUs=i*interval_us` 和 `timestampUs=锚点+streamElapsedUs`；这是理想索引轴，不证明 DUT 实际采样等间隔。

请求写入成功后本轮不可取消：算法忽略后续 BIZ run-control，不发送 STOP/ABORT，`finishRun()` 只在自然终态关闭传输，也不复位 DH 状态。结果只依据是否收到 count 帧以及每帧 `status/err_code`；不根据 23 路状态或遥测推导物理点火效果。只有启动时显式 `saveData=true` 才保存全部 descriptor 列；未显式启用时不创建文件。

### 7.2 惯测设备流

`IMU_STREAM` 的 DUT 输入来自 COM4。FPGA 已消费 `AA 1A` 帧头、`3B` 长度和 CRC，板端软件只读取 59 字节小端 payload：源序列号、12 个 IEEE-754 F32、`temperature:S16F`（`0.1 ℃/LSB`）、自检状态、工作状态、软件版本原始 U16 和源保留 U16。12 个浮点必须有限；状态或范围字段的非零值仍原样转发。源序列号只作为数据保存，不做连续性或丢帧判定。

DUT 经 COM3 发送 128 字节 `imu_stream_feedback_response`：B9 为状态、B10-B11 为错误码、B12-B70 为上述完整字段、B71-B126 保留、B127-B128 为 CRC。反馈使用 DUT 自己递增的产品协议序号。400 Hz 不要求软件保证无损，但实现不得固定 sleep 或主动抽样；有有效帧即尝试发送。COM3 在 614400/8E1 下发送 128 B × 400 Hz 约占 91.7% 线速率。

宿主的 `HalControlTransport` 为该模式提供分离的 `writeFrame()`/`readFrame()`。执行器发送 START 并校验 ACK 后持续读取 `09/01`，每帧完整解码并上报样本；本次流第一个已发布样本的 `streamElapsedUs` 为 0，之后按测试配置 `executionConfig.stream.hostTimestampIntervalUs` 递增。该字段必须是正整数微秒，当前惯测配置显式设为 2500；旧配置缺失时为兼容既有 400 Hz 时间轴仍回退到 2500 微秒。该相对轴按“成功解码并已发布的样本序号”生成，不依据 `source_seq` 或产品 U16 `seq`，因此会压缩宿主未观察到的丢帧空洞，不能外推为设备绝对采样时钟。每次 `executeStep()` 的首样本只读取一次 PC UTC 作为新锚点，公开 `timestampUs = UTC 锚点 + streamElapsedUs`，不再逐帧读取 PC 到达时间；乘法和公开时间整数均不得超过 JavaScript 安全整数上限 `9007199254740991`。

`hostTimestampIntervalUs` 是宿主算法的本地时间标注参数，不是 DUT 输出周期配置。它不得写入 START/STOP 的 `requestValues`、运行参数 Schema、协议 CSV、HAL 写操作或产品帧，也不得改变 START/STOP 次数、PC 发包频率、DUT 采样/输出节拍、判定或 verdict。`imu_stream_start_request` 仍只有序号与保留填充字段。舵机流已有协议携带的 DDS 权威时间，配置不声明该宿主估算字段，继续按真实 DDS 增量生成时间轴。

`requestStop()` 只设置原子停止标志，worker 在最长 20 ms 读超时后发送 STOP 并等待 ACK。等待 STOP ACK 时到达的有效反馈仍会上报。同一会话 START/STOP 各最多发送一次；STOP 写失败、ACK 超时、解析失败、序号不匹配或远端错误只使本次结果失败，`finishRun()` 只关闭传输，不再补发 STOP。不允许 BIZ 以 `pc_periodic` 重复 START。停止时零有效帧判为 `SampleFail`，一帧及以上通过；传输、CRC、命令、ACK 序号或远端错误保持类型化失败。

### 7.3 舵机设备流

`HELM_STREAM` 的运行参数由 `mbddf.helm_stream` Schema 定义：`waveform`、`freq`、`ampl`、`offset`、`start`、`max_freq`、`sweep_duration_s` 和低四位 `enable`。波形为正弦、方波、三角、恒值或连续对数扫频；频率和扫频时长必须为有限正数，通道位图为 `0..15`。测试程序的 DUT 与 Web 界面不限制角度幅值或偏置；舵控程序自身现有的内部限幅保持独立。`max_freq` 与 `sweep_duration_s` 只按波形条件显示，但隐藏时仍执行协议语义校验。DH 的 `config_enable` 和 23 路 `pulse_width[]` 使用同一算法 Schema/运行覆盖机制；配置使能时，23 路回读判据以本次实际下发值为准，关闭配置使能时脉宽只读、不参与本次写入一致性判定。

PC 经 COM3 发送一次 `HELM_START 07/10`，DUT `HelmDdsTestBridge` 建立 `local:://helm_command` writer 与 `local:://helm_feedback` reader。START 先发布一帧四路零位解锁指令；27 字节 DDS 指令的 B27/U8 名为 `helm_unlock`，连续测试首帧、运行帧和 STOP 回零尾帧均固定为 `0xFF`，它不属于 COM3 CSV 或界面参数。DUT 按 1 ms 周期生成一个共用波形，启用位对应的舵通道使用该值，未启用通道发送零；连续对数扫频从 `freq` 过渡到 `max_freq`，超过 `sweep_duration_s` 后指令归零，但反馈转发与测试会话保持活动，直到 PC 手动 STOP。STOP 停止指令线程后在关闭 DDS 端点前发布“四路零位 + `helm_unlock=0xFF`”尾帧，不发送关舵锁或禁止 PWM 语义。每个 `HELM_FEEDBACK 07/01` 使用 232 字节 payload，包含首个 DDS 时间戳和 1..5 个完整 41 字节反馈样本；宿主拆成逐样本事件，并记录产品帧、`serial_a`、`serial_b` 的不连续与缺失计数、批内索引和全部生效参数。

DDS 时间来自设备单调时钟而非 UTC。宿主保留每条原始值为 `dds_timestamp_us`，以本次 `executeStep()` 第一条有效 DDS 样本为 0 生成 `streamElapsedUs`，并用一次 PC UTC 锚点映射公开 `timestampUs`；后续样本使用协议携带的真实 DDS 增量，不强制重建为 1 ms 等间隔。DDS 原始值、相对值和 UTC 映射值当前统一限制在 `0..9007199254740991`，确保 WebSocket v1 的 JSON number 可无损表达，超限或跨样本倒退均视为协议错误。U16 连续性采用半环规则：前进距离 `2..0x7FFF` 才计缺失数，连续、自然回绕、重复、反向/重启候选和恰好半环均不制造巨额缺失计数。

独立目标 `MB_DDF_v2_HelmControl` 使用 `dut/src/HelmControl/ProtocolModel` 定义的 DDS 主题和 27/41 字节协议模型，用户可按任意顺序独立启动或停止。舵控进程打开 PWM 后首先直接禁止四路输出，随后关闭 update gate 并写入零 duty；首次解锁请求将高有效 DIDO DO0 置高，从写入成功起使用单调时钟至少等待 30 ms 才使能 PWM。成功后在进程内单向锁存，重复请求、字段回落或 STOP 都不重新上锁、禁止 PWM 或重置 30 ms 计时。DUT 服务只启停本次 DDS bridge，不创建、终止、探测或占有舵控进程；`HELM_BOARD_TEST 07/02` 继续走直接板级硬件路径且不包含舵锁流程，与设备流没有生命周期、互斥、忙状态或其他形式的绑定。DDS 端点使用 create-or-get 语义，启动顺序不构成协议约束。

宿主算法校验 START ACK 后持续读取 `07/01`；请求停止后发送一次 STOP 并校验 ACK。DUT 反馈协议中的六项自检及保留位继续由协议 CSV 定义和解码，但 PC 不把它们投影到应用样本、descriptor、连续 TXT 或后处理捕获，也不计算自检 OR 或“自检或超时”派生值。`status`、`err_code`、`timeout`、指令与反馈偏差以及连续性统计仍作为观测数据保留，不作为 PC 端中断采集或拒绝后处理计算的有效性门禁。停止时零样本判为 `SampleFail`，一条及以上可解码样本通过采集；帧无法解码、时间无法安全表示、START/STOP 命令失败和存储损坏等协议或基础设施错误仍保持类型化失败。完整应用样本与生效参数进入独立的后处理 sidecar，通道会先计算公共误差指标，再按波形计算可形成的性能指标；反馈不跟踪指令本身不得把通道改为 `device_reported_error`/`invalid_input`。波形覆盖或数学前提不足仍可形成对应的 partial/unavailable 指标状态。性能计算、伯德图和只读结果投影不改变采集 verdict、`TestResult` 或 STOP 硬件语义，具体 DTO 和查询边界见 [WebSocket 前端协议](websocket-frontend-protocol.md)。

正弦、方波和三角波的每个启用通道只分析实际指令中的第一个完整周期，公共指标使用该周期窗口，波形专用指标也只绑定该周期，后续重复周期不再累计。正弦以相邻两次上行穿越偏置点定位周期；方波要求闭合的两平台交替边沿，只统计一次上升沿和一次下降沿，可读取紧邻首边沿的前置平台作为基线；三角波以三个交替极值界定一个周期。正式波形前的零位解锁平台、开头或结尾的不完整周期均不进入计算；若整轮没有形成一个完整周期，才按数学覆盖不足返回 partial/unavailable。恒值和真正的连续扫频保持各自原有窗口语义；起止频率相等的扫频回退到上述正弦首周期语义。

### 7.4 板级单次测试

`DO_WRITE` 与 `HELM_BOARD_TEST` 都只声明 BIZ `single`；后者的 `automatic`/`manual` 是 `mbddf.helm_board_test` 的运行参数，不是额外的 BIZ run mode。

- `mbddf.do_write` 的运行参数投影 16 个原始输出位。DO5/DO6 必须为 0；低有效电源控制 DO3/DO4 默认原始位为 1，其余默认 0。每轮只发送一次用户掩码，要求 DUT `applied_state[0]` 完整等于指令且 `applied_state[1]==0`；成功应答后固定等待 `100 ms`，再读取 PXI-6259 的 `DUT_TX_ENABLE_SENSE` 与 `DUT_ATTENUATOR_SENSE`（物理 P0.4/P0.5），分别验证 bit2 与 bit1。其他位只有 DUT 回读证据，不得描述为外部物理闭环。协议、DUT 或夹具 I/O 错误为 `Error`，任一受检回读不一致为 `Fail`。该流程在运行完成、停止、断开或退出时均不发送额外 DUT DO 复位，产品保持最后一次已应用状态。
- `mbddf.helm_board_test` 的 `manual` 只编码四路 PWM/方向参数并交换一次 `HELM_BOARD_TEST 07/02`。它不打开、读取或写入 PXI-6259/PXI-6733，不采样、不写 AO；结果只有这次协议响应及单点完成状态。
- `automatic` 必须绑定两张夹具卡，按互相独立的方向掩码 `0`、`1`、`2`、`4`、`8` 执行 5 点方向检查；随后对四个 PWM 通道各执行 `1`、`5`、`10`、`25`、`50`、`75`、`90`、`95`、`99 %` 九点检查，共 36 点。PWM 由 PXI-6259 以 `1.25 MS/s`、每通道 `7500` 样本采集，指令/实测占空比误差容差为 `±1 pp`。
- 自动反馈检查由 PXI-6733 的 AO0..AO3 逐通道驱动 `0.0..5.0 V`、`0.5 V` 步进，共 4 × 11 = 44 点；每个点记录指令、DUT 读回、误差和 `±0.05 V` 容差。自动模式的总点数为 `5 + 36 + 44 = 85`。
- 自动流程开始前和完成、Fail、停止或 Error 的终态均尽力把四路 AO 写回 `0 V`；清零写失败升级为 `Error`。这是宿主软件的收尾语义，不是物理安全验收。精确端点、safe state、`CONFIGURE_ME` 部署门禁和真机前置条件只在 [HAL 接口协议](hal-interface-protocol.md) 定义；结果快照与浏览器呈现只在 [WebSocket 前端协议](websocket-frontend-protocol.md) 定义。

### 7.5 DI 刺激

`mbddf.di_read` 的 `executionConfig.digitalStimulus` 是算法层消费的执行配置，不进入 CSV 或产品帧。`DiStimulusController` 要求 1..64 个通道、唯一且非空的 `switchId`/`resourceId`、唯一的 0..63 `dutBit`、合法的 `High`/`Low` `activeLevel` 与 0..60000 ms 的 `settlingMs`。设置时必须带与当前状态精确相等的 revision；陈旧 revision 返回既有 HAL `DataMismatch`，未知 `switchId` 返回 `NotFound`，二者都不得触发输出写入。每次成功设置或复位都为全部已配置资源构造完整 `writeDoBatch()` 映射；复位的逻辑 mask 为 0，低有效通道仍写其 inactive 物理电平。写成功后才递增 revision、更新 applied mask 和写入时间，写失败保留已应用状态并记录 HAL 错误。算法内部的 64 路能力不外推到 WebSocket v1；该 JSON 协议只公开 `dutBit` 0..15，超出范围时投影为不可用。

当前 DI 配置仅声明实际接线的 `DUT_DI3_STIM`、`DUT_DI1_STIM`、`DUT_DI2_STIM`，对应开关 `di3`、`di1`、`di2`。`reportFields.taskMeasurements` 从 `di_state[0]` 配置拆分 DI0..DI15，任务面板隐藏两个原始位图；用户标记的 DI0/DI1/DI2/DI3/DI8 使用地址分配表中的业务名称，其余只显示位号。DI0 与 DI8 只通过 DUT 总线回读，不由 PXI 刺激。控制器加载时还要求每个刺激资源属于配置的 stimulus 设备、是数字输出，且 HAL `safeState` 等于该通道的逻辑 inactive 电平；不一致会以 `stimulus_safe_state_mismatch` 拒绝，尚未打开硬件。刺激与 DUT 回读不一致不是该测试的产品判定条件；产品判定仍只依据 `status` 和 `err_code`。DI 刺激使用可选的 PXI-6259 原生 NI-DAQmx Adapter；Router 用驱动级配置初始化 Vendor Adapter，HAL 的 `openDevice()` 再把单设备身份、该设备全部已配置资源和 safe state 放入版本化 open projection；动态 fixture 回归锁定了两阶段 JSON 边界。`DiStimulusController` 本身不创建 NI 采样任务；板级 automatic 才通过其专用 fixture 使用 PXI-6259/PXI-6733。两张卡的部署模板、物理映射和 `CONFIGURE_ME` 拒绝语义见 [HAL 接口协议](hal-interface-protocol.md)，Fake 覆盖不能替代真实 I/O 验收。

### 7.6 单步清理与展示元数据

`executionConfig.lifecycle` 只允许在需要清理 ACK 的单步中使用：请求/响应 Profile 必须成对提供，执行器先完成主请求/响应，再以递增序号发送 follow-up 请求并校验响应命令、序号、`status` 和 `err_code`。这不是设备持续回告能力；当前定时器 START 在 DUT 端同步完成 250 us x 250 周期统计，STOP 只负责幂等清理确认。

十三个当前配置的 `reportFields` 还包含应用层展示元数据：`title`、`description`、`supportedRunModes`、`measurements`、可选 `taskMeasurements` 和可选 `stoppable`。这些字段只由应用层投影为 WebSocket descriptor；通用测量的 `id`、`label`、`unit`、`primary`、`taskVisible` 及任务面板派生项不能改变算法判定、协议编解码或 HAL 安全行为。设备流保存仍固定使用通用 `measurements` 列，曲线选择和 `taskMeasurements` 不改变保存格式。`stoppable` 缺失时兼容回退为 true；显式 false 只允许恰好声明 `device_stream`，当前仅 DH 点火有限流使用。串口回显的 114 个协议数据字段只在算法内部逐字节判定，公开样本和测量只包含状态、链路、轮次、字节数和不一致摘要，完整请求/响应仍以帧十六进制及数据 SHA-256 保留在结果诊断中。可编辑运行参数不在 `reportFields` 定义，而由算法 ID 对应的 Schema 唯一声明；应用层只把 Schema 字段作为本次覆盖，固定协议字段仍留在配置请求中，再将规范化结果透传 BIZ。Schema 可为 descriptor 的每个 `runParameters[]` 项声明 `persistValues`；其浏览器兼容与 localStorage 语义只以 [WebSocket 前端协议](websocket-frontend-protocol.md) 为准。

算法不选择 Provider 或物理端点。`control.resourceId`、资源 `providerId`、串口参数、UDP 端点、设备 match、SDK 和扫描结果只属于 HAL 部署配置；当前样例见 `configs/mbddf_pc_hal.json`。把 `control.resourceId` 设为 `CONTROL_SERIAL` 或 `CONTROL_NETWORK` 即可在 PC 每次运行前选择控制口，不向产品端发送切换命令。串口回显额外消费一个 `role=auxiliary-link` 的 `qt.serial` 资源；Web 只允许从系统 `ports` 枚举中选择其本次 `portName`，不允许客户端提供协议、端点或任意 HAL 配置路径。WebSocket `start.dataDirectory` 是应用层本次连续数据输出路径的独立例外，不选择 Provider、端点、资源或 HAL 配置。主控制资源与辅助资源由同一 `HalDevice` 独立打开，不能映射到同一物理串口。

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

两侧复用同一 `requestId`。`[当前实现]` 算法日志对请求和响应分别记录 `FrameLength`、`FrameSha256` 与最多 16 字节的 `FrameHexPreview`；只有 `runtimeConfig.tags.logFullFrames=true` 时才附加完整 `FrameHex`。结果 `rawData` 和样本投影属于独立诊断面，不等同于日志 sink。结构化字段主定义见 [日志接口协议](log-interface-protocol.md)。

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

验证必须覆盖资产结构、帧编解码、十三个当前配置的算法语义、运行参数、停止/清理和错误分类。`DO_WRITE` 的当前边界是一次用户掩码、一次 DUT 完整回读、固定等待 100 ms 后只验证两路外部物理读回，并且完成、停止或关闭均为 **0 次额外 DUT 复位帧**；其唯一详细定义见 7.4。

具体测试目标、当前数量、命令、执行记录和证据等级只在[测试规范](../testing/testing-specification.md)维护。DUT 内部实现与已知限制见其[代码设计](../../../dut/docs/design/product_protocol_csv/codedesign.md)。真实硬件协议测试必须独立授权和标记，不进入默认自动化证据。
