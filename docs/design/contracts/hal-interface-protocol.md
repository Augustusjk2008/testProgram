# HAL 层接口协议

> 适用项目：多产品通用硬件测试软件（Qt 5.15 兼容、Qt 6 Core/Network/SerialPort fallback / C++17 / Windows）
> 本文定位：HAL 公共接口、生产态 I/O 边界、资源与安全语义、Provider 路由目标以及 Vendor Adapter C ABI。
> 代码事实源：`src/hal/include/hal/`、`src/hal/src/`；公共头与本文冲突时，先判断代码是否违反本契约，再修正文档或代码。
> 状态标记：**当前**表示仓库已实现，**目标**表示已确认但尚未实现，**扩展点**表示尚未冻结接口。

---

## 1. 边界

生产态中，凡涉及测试设备或被测件（DUT）的硬件访问与通讯 I/O，均必须经过 HAL。配置、协议资产、日志和报告的文件 I/O 不属于这条规则。

目标调用链：

```text
算法层
  -> HAL 公共接口（逻辑 ResourceId、原始数据）
  -> HAL 内部 providerId 路由
       -> Qt 标准 API Provider
       -> Vendor Adapter Provider
       -> Mock Provider
  -> 测试设备或 DUT
```

当前控制调用链：

```text
算法层 HalControlTransport
  -> IHalDevice / IControlChannel
  -> ControlChannelManager
       -> qt.serial -> QSerialPort
       -> qt.udp -> QUdpSocket
```

该路由只覆盖 `module = "control"` 的资源，Qt 标准接口不经过 Vendor C ABI。非控制设备资源当前走另一条已实现的内部链路：

```text
HalService -> AdapterRouter（按设备 adapterId）
           -> MockAdapter（mock.* / mock.adapter.v1）
           -> CAbiAdapter -> 配置的外部 ABI v1 DLL
```

`AdapterRouter` 只在首次 `openDevice()` 时构造并初始化该设备的后端；同一 Adapter 的最后一个会话关闭后调用后端 `shutdown()` 并释放实例。它不是控制通道的通用 `providerId` Router。TCP、控制通道 Mock Provider、控制通道通用 Router 和其他厂家 SDK Adapter 仍未接入；`hwtest_adapter_ni_daqmx` 已是可选的原生 ABI v1 后端，但源码/CMake/Fake 证据不等于真实设备证据。

职责分配：

| 能力 | 所有者 |
| --- | --- |
| CSV 加载、帧边界、产品 CRC、命令/序号、请求响应匹配 | 算法/协议层 |
| 测试步骤、依赖、状态、步骤级重试、结果与报告编排 | BIZ |
| 逻辑资源、连接对象、原始 I/O、操作 deadline、归一错误 | HAL |
| 输出范围校验、物理安全态执行、连接关闭 | HAL |
| 厂家 SDK、DLL、驱动调用 | Vendor Adapter |
| Qt 串口、UDP 的标准 API 调用 | HAL 内部 Qt 控制 Provider（当前） |
| TCP 标准 API 调用 | Qt Provider（目标，未实现） |
| 协议字段解释、测试判定 | 不属于 HAL、Provider 或 Adapter |

算法可以请求 HAL 初始化、打开、关闭和安全收尾，但具体连接对象及物理动作由 HAL 持有并执行。BIZ 不得直接依赖 HAL。

---

## 2. 公共兼容面

命名空间为 `hwtest::hal`。公共头位于 `src/hal/include/hal/`，使用 Qt/C++ 类型和逻辑资源 ID，不暴露厂家句柄或物理通道。

| 公共接口 | 当前职责 |
| --- | --- |
| `IHalService` | 初始化、扫描、能力查询、设备会话和日志信号 |
| `IHalDevice` | 聚合设备描述、能力及各类 I/O 接口 |
| `IAnalogIo` | AD/DA 配置、采样和输出 |
| `IDigitalIo` | DI/DO 读写及边沿等待 |
| `ISerialBus` | 串口配置与原始字节收发 |
| `ICanFdBus` | CAN/CANFD 配置与原始总线帧收发 |
| `ISampleTaskIo` | 可选采样任务的创建、启动、读写、状态、停止和释放；覆盖 AI/AO/DI/DO 与计数器任务 |
| `IControlChannel` | 按逻辑资源打开/关闭控制通道，并执行原始字节读写 |
| `availableSerialPorts()` | 只读枚举宿主串口并返回稳定排序的 `SerialPortDescriptor` 值列表 |

`IHalDevice::controlChannel()` 返回当前控制通道接口，`IHalDevice::sampleTasks()` 以默认返回 `nullptr` 的可选入口暴露 `ISampleTaskIo`。公共方法签名以对应头文件为准；没有新增或冻结 `INetworkBus`，`IControlChannel` 不向算法暴露 `QSerialPort`、`QUdpSocket` 或 UDP 数据报类型。TCP 仍待真实用例、deadline 和流语义明确后评审。

`availableSerialPorts()` 定义在 `hal_factory.h`，内部使用 `QSerialPortInfo::availablePorts()`，只返回端口名、说明、厂家、序列号和系统位置，不创建 HAL 会话、不打开端口，也不等价于 `IHalService::scanDevices()`。应用层将其映射为自身 DTO，TUI 不直接依赖 HAL 或 Qt SerialPort 类型。空列表表示当前未发现宿主串口；端口是否被占用、是否有权限以及 DUT 是否响应，只能在后续 HAL 打开和通讯时确定。

### 2.1 `IHalService` 语义

- `initialize(const QVariantMap&)`：加载资源、安全配置，验证多设备资源映射并登记 `AdapterRouter` 条目；不加载厂商 DLL。控制资源的 `providerId` 在首次打开时由 `ControlChannelManager` 解析。
- `scanDevices()` / `queryCapabilities()`：返回 HAL 归一化描述。当前结果来自 `ResourceMapper` 配置，不是物理扫描。
- `openDevice()`：按目标设备 `adapterId` 惰性取得/初始化后端，构造单设备 `AdapterDeviceOpenSpec`，并将其 `schema=hwtest.adapter-device-open`、`version=1` JSON 原样作为 C ABI `openOptionsJson` 传给 Adapter；返回全局不冲突的公开 `SessionId`。连接、底层 handle 和 Adapter 租约归 HAL 所有。
- `closeDevice()` / `shutdown()`：先停止并释放该会话已跟踪的采样任务，再尽力执行物理安全态、关闭连接并释放相应 Adapter 租约/后端；不得构造产品协议停机命令。
- `device(sessionId)`：返回受该 HAL 会话生命周期约束的聚合设备接口。

### 2.2 原始通讯语义

`ISerialBus` 只负责端口配置与原始字节收发。当前 `transactSerial()` 的实现等价于一次 `writeSerial()` 加一次有界 `readSerial()`：

- 不保证一次读取获得完整产品响应；
- 不累积流式字节，不搜索 terminator；
- 当前不执行 `expectedPrefix`、`readMinBytes` 或产品 CRC 校验；
- 不匹配产品命令、序号或请求响应关系。

多段读取、产品帧组装和响应匹配由算法层完成。`SerialTransaction` 中相关字段是现有兼容字段，不能据此宣称相应能力已实现。

`ICanFdBus` 负责 CAN/CANFD 配置、过滤和原始 `CanFdFrame` 收发。HAL 可校验总线帧的 ID、DLC 和 payload 上限，但不得解释 payload 中的产品字段。

---

## 3. 公共类型与错误边界

主定义文件为 `src/hal/include/hal/hal_types.h`。`DeviceId`、`AdapterId`、`ResourceId`、`RequestId` 和 `SessionId` 均为现有兼容类型；`SerialPortDescriptor` 是宿主串口发现的纯值类型，不携带或转移连接对象所有权。

`HalStatusCode` 的现有顺序和值不得随意调整：

```text
Ok, InvalidArgument, InvalidState, NotInitialized, NotFound,
NotSupported, PermissionDenied, Busy, Timeout, Cancelled,
SafetyLimitExceeded, DeviceDisconnected, AdapterLoadFailed,
AdapterSymbolMissing, AdapterError, IoError, ProtocolError,
CrcMismatch, DataMismatch, BufferTooSmall, InternalError
```

三个易混淆状态的使用边界：

| 状态 | 允许语义 | 禁止语义 |
| --- | --- | --- |
| `ProtocolError` | Provider/驱动明确报告的链路协议或传输帧化错误 | MB_DDF 帧头、长度、命令、序号、字段或响应匹配错误 |
| `CrcMismatch` | 驱动或链路层已报告的底层 CRC 错误 | HAL 自行计算产品 CRC |
| `DataMismatch` | HAL 自检或回环完整性错误的兼容值 | DUT 字段值、测试阈值或业务判定不一致 |

来源不明的 Vendor 错误应映射为 `AdapterError` 并保留原始诊断，不能猜测为产品协议错误。

### 3.1 操作选项

`OperationOptions.timeoutMs` 的目标语义是一次 HAL 操作的总 deadline 预算。内部 Provider 重试只能消耗剩余预算，且不得根据产品响应内容重试。

当前 `HalControlTransport` 使用单调时钟把一次请求的剩余预算分别传给控制写和后续读取；Qt 串口写入也在同一 HAL 写操作预算内排空待写字节。整个 HAL 尚未统一完成该语义：`retryCount` / `retryIntervalMs` 未执行，旧 `transactSerial()` 仍把同一个 `timeoutMs` 分别交给写和读，`readAdBatch()`、`writeDaBatch()` 和 `readDiBatch()` 还会为每个通道重复使用完整 timeout。Qt UDP 只在没有待处理报文时检查剩余预算，持续到达的异源报文可能使读取超过总 deadline。步骤级重试由 BIZ 编排。

### 3.2 日志类型

HAL 通过 `HalLogEvent` 和 `IHalService::logProduced` 暴露硬件上下文；`logMessage` 仅作兼容信号。HAL 不依赖完整日志服务，字段映射和 `requestId` 规则统一见 [日志接口协议](log-interface-protocol.md)。

---

## 4. 控制通道 Provider 路由（当前，局部实现）

Provider 是 HAL 内部后端分类，不是新的业务层，也不向 BIZ 暴露。当前只冻结两个控制资源注册值：

| `providerId` | 连接与原始 I/O | 是否经过 Vendor C ABI |
| --- | --- | --- |
| `qt.serial` | `QSerialPort`；端口名、波特率、数据位、校验、停止位和流控来自资源 `properties` | 否 |
| `qt.udp` | `QUdpSocket`；本地绑定地址/端口和远端地址/端口来自资源 `properties`，接收时忽略非配置远端的数据报 | 否 |

`providerId` 只在 HAL 部署配置和内部路由中解析；算法和 BIZ 只使用 `ResourceId`。控制资源缺少 `providerId` 返回 `InvalidArgument`，未知值返回 `NotSupported`，不得静默回退到 Mock。当前资源链为：

```text
ResourceId
  -> ResourceBinding(module = control, device alias, properties)
  -> providerId = qt.serial | qt.udp
  -> HAL 按 ResourceId 持有的独立 QSerialPort | QUdpSocket 会话
```

同一 `HalDevice` 可以同时打开多个不同 `ResourceId` 的控制资源；每个资源持有独立 Provider 会话，读、写和单资源关闭只作用于该资源。已打开的同一 `ResourceId` 再次打开返回 `Busy`。管理器直接在调用线程同步调用 Provider，不建立工作线程、排队转发或跨资源 I/O 复用。

`HalDevice::close()` 会按 `ResourceId` 的升序逐个关闭所有仍打开的控制会话；即使某个 Provider 关闭失败，也会消费该会话并继续尝试其余会话，最终返回第一个关闭失败。这样关闭顺序可复现，且单个失败不会阻止其余资源收尾。

对于同一 manager 内已经成功打开的 `qt.serial` 会话，新的不同资源若解析出相同非空 `properties.portName`，会在打开第二个 Provider 前返回 `Busy`，错误 detail 包含请求的 `portName` 和 `conflictingResourceId`。端口名先去首尾空白；Windows 上再做大小写折叠，其他平台保持大小写。该检查不跨 `HalDevice`，也不把 `\\.\COM3`、`COM03` 等未经 Qt 明确规范化的别名擅自视为同一端口；空 `portName` 仍由 `qt.serial` Provider 返回既有的 `InvalidArgument`。

可运行配置见 `configs/mbddf_pc_hal.json`。PC 端通过 `control.resourceId` 选择同一份配置中的串口或 UDP 资源；可按运行需要同时使用多个已声明资源，这不向 DUT 发送“切换控制口”命令。远端 IP/端口属于部署事实，不从 MB_DDF 板端网口自环测试地址推断。

控制通道的通用 Router、Provider 级设备扫描、`Mock Provider`、TCP 和其他厂家 SDK Provider 仍是目标能力。当前设备 `adapterId` 已用于选择惰性多 Adapter 后端，仍不能与控制资源 `providerId` 混为一谈；`ni.daqmx` 已有可选原生 Adapter，其他厂家 SDK Provider 仍未实现。

HAL 部署配置与 BIZ 的产品测试配置是不同边界：BIZ 新配置使用 `executionConfig` 向算法透传产品执行参数，不得把 Provider、SDK、物理端点或扫描结果塞入 `ProtocolProfile`、`ExchangeAction` 或协议 CSV。

`[当前实现]` Web 工位配置持久化在 `configs/mbddf_station.json`，由应用层在读取基础 `mbddf_pc_hal.json` 后投影到内存中的最终 HAL map；基础文件不被改写。覆盖只匹配基础配置中已经存在的控制资源、PXI-6259/PXI-6733 设备 alias 和资源 ID：可选择既有主控制资源，可设置串口属性、两张 NI 卡的物理设备名/序列号、6259 数字端口/线号，以及 6259 AI、6733 AO 的资源 `physicalIndex`。未知设备/资源或试图修改 Adapter、Provider、module、direction、设备型号、connector、量程或根 `safeState` 会在进入 HAL 前拒绝。模拟通道路由以资源顶层 `physicalIndex` 为准；`properties.channel` 只是基础配置中的说明字段，不是工位覆盖入口。

---

## 5. 资源、参数和安全

HAL 对外只接受逻辑资源 ID，并负责：

- 校验资源存在、模块和方向匹配；
- 依据设备能力校验物理索引和功能支持；
- 归一化模拟量、数字量、串口和 CAN/CANFD 参数；
- 对输出执行范围、电平和 payload 上限校验；
- 在会话关闭前按已配置 safe state 尽力进入物理安全态。

HAL 可以转换工程单位和厂家单位，但不得把产品字段转换或测试阈值判定伪装成硬件归一化。

`[当前实现]` `ResourceMapper` 拒绝重复设备 alias；多设备配置中的每个资源必须显式指定已知设备，资源缺省 `adapterId` 时继承设备值，显式值不一致会失败；同一设备、模块、方向和物理索引的重复映射也会失败。单设备未声明 `device` 的既有配置仍兼容。

`[当前实现限制]` `physicalIndex` 缺失或无法转换为整数时会得到 `0`，只在结果为负数时拒绝；根 `safeState` 中没有对应资源的键会在设备投影时静默丢弃。部署配置必须在进入 HAL 前保证物理索引是显式非负整数，并保证每个安全态键对应同一设备的可写输出资源。

`[当前实现]` `HalDevice::close()` 在关闭底层设备前尽力应用安全态。对于同一 HAL 设备的已配置数字输出，HAL 先按 `physicalIndex` 汇总，再只调用一次底层 `writeDigitalBatch()`；模拟输出、串口和 CAN 仍按各自资源处理。这是 HAL 批处理语义，不承诺厂商 Adapter 已按端口 bank 实现原子整幅写入，也不构成物理安全验收。

`[当前实现限制]` 模拟输出安全检查只比较上下界，没有拒绝 `NaN`/`Inf` 或非法量程；非有限值可能继续传给后端。文档中的“范围校验”不能据此解释为已经形成完整的非有限数门禁。

`[当前实现]` BIZ `SafetyPolicy.enterSafeStateOnStop` 与 `enterSafeStateOnError` 仅被解析、保存和透传，HAL 与应用层均不按其布尔值选择分支。应用控制器的显式停止会无条件尝试复位 DI 刺激，错误终态不会因这两个字段立即触发额外复位；最终会话关闭仍执行上述 HAL safe state。该 safe state 只作用于 HAL/PXI 资源，不会向 DUT 发送产品 `DO_WRITE`；已批准的数字量输出测试在完成、停止和断开时都保留 DUT 最后状态。不得把兼容字段或 PXI 关闭写成已复位产品 DO。

`[当前实现]` `AdapterDeviceOpenSpec` 是 HAL 私有 DTO，不是公共 HAL 头或 ABI v1 的破坏性修改。它从 `ResourceMapper` 只投影一个设备的逻辑/物理身份、资源通道、该设备 safe state 和 `taskProfiles`；物理名优先来自 `hardware.devices[].properties.vendor.ni.deviceName`。`HalService` 将同一份已筛选 safe state 交给 `HalDevice`，并作为 `openOptionsJson` 交给厂家 Adapter。投影可以携带 `taskProfiles`，但当前 NI Adapter 只校验其数组结构，并不据此创建或执行任务；运行时身份、通道和 safe state 由 open parser 解析，任务仍由 `ISampleTaskIo` 的显式创建调用配置。

`[当前实现]` 可选 `hwtest_adapter_ni_daqmx` 支持 **NI PXI-6259** 与 **NI PXI-6733** profile。其驱动初始化只读取驱动级 `settings.timeoutSeconds`，拒绝把 `hardware`、`safeState` 或 open schema 作为初始化配置；设备身份、资源和 safe state 由 `openDevice()` 的版本化投影解析。投影解析、拓扑校验和厂商 I/O 实现分离：前者位于 `src/adapters/ni_daqmx/ni_daqmx_config.*`，后者位于 `ni_daqmx_adapter.cpp`。打开时 Adapter 以投影物理名发现设备，并用 `DAQmxGetSysDevNames`、`DAQmxGetDevProductType`、`DAQmxGetDevSerialNum` 核验投影型号和非占位序列号；`serialNumber=CONFIGURE_ME` 必须在投影解析时被拒绝，防止模板误开。

`[当前实现]` `AdapterRouter::configForAdapter()` 对 Vendor C ABI 后端只返回对应 Adapter 的驱动级配置，不再把全局 `hardware`/`safeState` 交给 `CAbiAdapter::initialize()`；Mock 后端继续接收其进程内拓扑。动态 C ABI fixture 会主动拒绝初始化 JSON 中的 `hardware`/`safeState`，`HalServiceTest.PassesOnlyDriverSettingsToVendorAdapterInitialization` 锁定初始化边界；`HalServiceTest.RoutesDriverSettingsAndDeviceProjectionIntoNiAdapter` 再经生产 NI parser 与 Fake DAQmx 锁定完整组合链。

`[当前实现限制]` `HalService::initialize()` 会先调用但忽略上一轮 `shutdown()` 的失败，随后仍可重新配置；因此重新初始化成功不证明前一会话已完成安全态或原生资源收尾。

PXI-6259 配置拓扑为 AI `ai0..ai31`（硬件能力还报告最多 16 路 differential）、AO `ao0..ao3`、DIO `port0/line0..31` 或 `port1..2/line0..7`（共 48 线）以及 `ctr0..ctr1`。Adapter 已实现按需 AI/AO/DI/DO，以及 AI/AO/DI/DO 的有限和连续任务；采样任务可选择内部默认或外部时钟、start/reference/pause 触发，并支持计数边沿输入和频率脉冲输出。任务式路径对 PXI-6259 施加 AI 单通道 1.25 MS/s、AI 多通道总 1 MS/s、AO 1--2 通道 2.86 MS/s / 3 通道 1.54 MS/s / 4 通道 1.25 MS/s、硬件定时 DIO 10 MHz 且仅 port0、计数器脉冲频率不高于 20 MHz 的限制；按需多样本 AI 路径当前只要求采样率大于零，没有复用同一上限校验。PXI-6733 profile 不声明 AI、DIO 或计数器，只接受 `ao0..ao7` 的单样本按需模拟输出（能力投影的最大 AO 速率为 1 MS/s），并要求每路 AO safe state 为 `0.0 V`。Adapter 短读以实际每通道样本数返回，部分写入、overflow/underflow 和 DAQmx 错误均归一到 HAL 状态/诊断；板级 fixture 在此基础上进一步要求通道数与每通道样本数精确等于请求值且全部为有限数，短读或 `NaN`/`Inf` 直接按 `DataMismatch` 拒绝。

NI 的 on-demand 数字路径按 `portNumber`、方向和连续 `lineNumber` 分组，使用 `DAQmxCreateDOChan`/`DAQmxCreateDIChan` 的 `ChanForAllLines` task 和 `DAQmxWriteDigitalU32`/`DAQmxReadDigitalU32` 处理完整配置线段。设备关闭先停止并释放已跟踪的采样任务，再写 DO/AO safe state；安全写使用的临时 DAQmx task 在各自调用内清理，不调用会影响整卡其他任务的 `DAQmxResetDevice`。上述 NI 语义目前只有 Fake NIDAQmx 自动化证据。

### 5.1 板级夹具部署映射

本节是 `mbddf.do_write` 与 `mbddf.helm_board_test` 的物理夹具端点、部署模板和真机门禁的唯一主定义。产品步骤和判据见 [设备通讯协议契约](device-communication-protocol.md)，结果 DTO/浏览器呈现见 [WebSocket 前端协议](websocket-frontend-protocol.md)。

| 用途 | 逻辑资源 | 板卡物理端点 | 端子 | 量程/说明 |
| --- | --- | --- | --- | --- |
| DI 刺激 | `DUT_DI3_STIM`、`DUT_DI1_STIM`、`DUT_DI2_STIM` | PXI-6259 P0.0、P0.1、P0.2 | 52、17、49 | 数字输出；DI 刺激专用 |
| DO_WRITE 发送使能读回 | `DUT_TX_ENABLE_SENSE` | PXI-6259 P0.3 | 47 | 数字输入 |
| DO_WRITE 衰减器读回 | `DUT_ATTENUATOR_SENSE` | PXI-6259 P0.4 | 19 | 数字输入 |
| PWM 采样 | `HELM_PWM1..4_SENSE` | PXI-6259 `ai0..ai3` | 68、33、65、30 | RSE，0..5 V |
| 方向采样 | `HELM_DIR1..4_SENSE` | PXI-6259 `ai4..ai7` | 28、60、25、57 | RSE，0..5 V |
| 反馈激励 | `HELM_FK1..4_STIM` | PXI-6733 `ao0..ao3` | 22、21、57、25 | 0..5 V；safe state 全部为 0.0 V |

应用组合根在 `prepare` 阶段打开当前配置无条件需要的夹具：DI 刺激和 `mbddf.do_write` 都打开并保持 PXI-6259，使 Adapter DLL、设备身份、资源和 safe state 错误统一在 Web“连接设备”阶段返回；`start` 不得首次发现 DO fixture 缺失。`mbddf.helm_board_test` 的夹具需求仍由 `test_mode` 决定：`automatic` 使用 PXI-6259/PXI-6733，`manual` 不打开两张卡。自动舵机测试在开始前和任何终态都尽力将四路 AO 清零；清零会逐路尝试 FK1..FK4，即使某路失败也继续其余通道并保留首个错误。HAL 关闭也按已配置 safe state 尽力收尾。这些是软件路径，不保证台架已达到物理零电压，也不改变 DUT DO 保留语义。

`configs/mbddf_pc_hal.json` 当前只是一份部署模板：两张卡的 `serialNumber` 及 `hardware.devices[].properties.vendor.ni.deviceName` 都为 `CONFIGURE_ME`。投影解析明确拒绝占位 serial；占位 MAX 设备名也不会匹配已安装板卡。真机前必须在隔离、明确授权的台架提供实际 `HWTEST_NI_DAQMX_ADAPTER_PATH`/NI SDK、匹配 PXI-6259/PXI-6733 的 MAX 设备名和非占位序列号，并核对上述端点接线、共地、电平、量程与隔离。须记录 PXI-6259 P0.3/P0.4 读回和 AI 采样参数、PXI-6733 AO0..AO3 的输出及正常/异常收尾清零，并以独立参考测量、`rawData.boardTest` 与前端截图形成真机证据。Fake NI、模板解析或构建成功均不满足这些前置条件。

目标生命周期：

```text
initialize
  -> 校验 HAL 部署配置
  -> 建 Provider 路由
  -> 扫描/匹配设备并建立 ResourceId 映射
  -> 加载安全边界

openDevice
  -> Provider 创建并持有连接
  -> HAL 返回 SessionId / IHalDevice

I/O
  -> 查资源和会话
  -> 校验参数、安全和剩余 deadline
  -> Provider 原始 I/O
  -> 归一错误与日志

closeDevice / shutdown
  -> 执行物理安全态
  -> 关闭连接
  -> 释放 Provider
```

若产品需要发送业务级停机命令，应由算法在 HAL 物理安全收尾之前明确编排；HAL 的最终安全动作不得依赖产品协议成功。

---

## 6. Vendor Adapter C ABI（当前兼容面）

主定义文件为 `src/hal/include/hal/hal_adapter_abi.h`。外部 Vendor Adapter DLL 的入口为：

```cpp
int HAL_ADAPTER_CALL hal_adapter_get_api_v1(const HalAdapterHostApiV1* host,
                                            HalAdapterApiV1* outApi);
```

核心 ABI 版本仍为 `HAL_ADAPTER_ABI_VERSION == 1`。基础函数表保持生命周期、模拟量、数字量、串口和 CAN/CANFD 操作；`openDevice` 原有的 `openOptionsJson` 参数现在承载 HAL 生成的单设备投影。采样任务函数没有被插入或重排进核心 `HalAdapterApiV1`。

`[当前实现]` `CAbiAdapter` 通过 `AdapterLoader`/`QLibrary` 解析 `hal_adapter_get_api_v1`，校验 ABI 版本及函数表 `structSize`，并实际调用外部 DLL 的初始化、设备生命周期、能力、模拟/数字、串口和 CAN 函数。缺失函数指针返回 `NotSupported`；厂家状态码、原始 code 和诊断文本按现有 `HalStatusCode` 归一化并保留。它不再委托 `MockAdapter`。`IHalService::scanDevices()` 和 `queryCapabilities()` 当前仍返回配置映射，不是通过已加载 DLL 动态发现。

`[当前实现限制]` 核心函数表允许 `openDevice` 存在而 `closeDevice` 为空。此时设备能够打开，但按会话关闭返回 `NotSupported`；`HalService` 仍消费公开会话并释放 lease。最后一次 Adapter `shutdown()` 是否回收厂家设备句柄取决于 DLL，不能由宿主当前实现保证。

兼容规则：

- C ABI 只使用固定宽度整数、POD、opaque handle 和调用方分配缓冲区；
- 字符串为 UTF-8 且以 `\0` 结尾；
- 阻塞函数必须尊重传入的 `timeoutMs`；
- `closeDevice` 一经调用即消费 opaque device handle；即使安全写、Stop/Clear 或其他清理返回错误，宿主也必须移除会话且不得用同一 handle 二次关闭；
- 当前 `CAbiAdapter` 在进程内以递归互斥串行化同一 Adapter 实例的 ABI 调用，避免 I/O 与 close/shutdown 并发进入 DLL；
- 不支持的函数指针可为 `nullptr`，HAL 调用前返回 `NotSupported`；
- 新函数只能追加到函数表尾部，并以 `structSize` 判断兼容；
- 改变已有字段语义必须升级 ABI 主版本。

该 ABI 只服务 Vendor Adapter Provider。Qt Provider 和 Mock Provider 为进程内实现，不经过此 ABI。

### 6.1 可选采样任务 ABI v1

`hal_adapter_task_abi.h` 定义独立的 `HAL_ADAPTER_TASK_ABI_VERSION == 1`、opaque task handle 和 `HalAdapterTaskApiV1`。它覆盖 AI/AO/DI/DO、计数输入/输出，按需/有限/连续模式，采样时钟、start/reference/pause 触发、计数器配置、块缓冲和任务状态，以及 create/start/read/write/status/stop/close 生命周期。

厂商 DLL 可选导出：

```cpp
int HAL_ADAPTER_CALL hal_adapter_get_task_api_v1(
    const HalAdapterHostApiV1* host,
    HalAdapterTaskApiV1* outApi);
```

`AdapterLoader` 仅在核心 ABI 已成功加载后尝试解析该符号；符号缺失、版本不符或结构过小不会卸载核心 ABI。`CAbiAdapter` 因而保持既有 ABI v1 Adapter 可加载；对没有 task API 的 Adapter，任务操作返回 `NotSupported`。公共 `ISampleTaskIo` 通过 `IHalDevice::sampleTasks()` 的可选入口暴露同一 create/start/read/write/status/stop/close 语义，HAL 负责逻辑资源到物理索引的校验、会话所有权和 task ABI 映射。

当前 Adapter 状态码映射保持兼容；其中 `HAL_ADAPTER_PROTOCOL_ERROR` 只有在厂家明确说明为传输层错误时才映射 `ProtocolError`，否则使用 `AdapterError` 并保留厂家码。

`[当前实现]` 仓库有 `src/adapters/ni_daqmx/` 原生 **PXI-6259/PXI-6733** Adapter。根 CMake 的 `HWTEST_ENABLE_NI_DAQMX` 默认关闭；开启时先使用显式 `NI_DAQMX_INCLUDE_DIR`/`NI_DAQMX_LIBRARY`，未提供时通过 `find_path`/`find_library` 搜索 `NIDAQmx.h` 和 `NIDAQmx`/`nicaiu` 导入库，仍缺任一项则配置失败，并生成共享 DLL `hwtest_adapter_ni_daqmx`。它导出核心 `hal_adapter_get_api_v1` 和可选 `hal_adapter_get_task_api_v1`；`AdapterRouter -> CAbiAdapter` 使用驱动级初始化配置加载该 DLL，单设备投影随后由 `openDevice()` 传入。

该 Adapter 对 PXI-6259 声明 `analog`、`digital`、`counter` 模块，支持按需 AI/AO/DI/DO 和基于可选 task ABI 的有限/连续 AI/AO/DI/DO、计数边沿输入、频率脉冲输出；PXI-6733 仅声明单样本按需模拟输出。`DAQmxCfgSampClkTiming` 接收空时钟源作为内部默认时钟，非空源按外部时钟传入；start/reference/pause 触发分别映射到相应 DAQmx 调用。读取保留实际短读样本数，任务状态报告已启动/完成、可用样本、累计样本和 overflow/underflow；DAQmx 错误保留厂家码并归一化超时、设备移除、忙和 I/O 错误。通道、速率、safe state、部署模板和真机门禁只在第 5 节定义，本 ABI 章节不再复制。

`[当前实现]` 这里只证明 PXI-6259/PXI-6733 NI-DAQmx 软件路径和 ABI 接线已经存在；当前自动化仍是 Fake NIDAQmx。真实板卡前置条件与证据边界分别见 5.1 和[测试规范](../testing/testing-specification.md)。

---

## 7. 日志与诊断

HAL 关键生命周期和 I/O 操作应产生结构化事件，至少可追踪：

- `requestId`、操作名、耗时和归一状态；
- `deviceId`、`resourceId`；
- 目标 Provider 路由落地后的 `providerId`、`providerKind`；
- Vendor 分支的 `adapterCode` 和原始厂家诊断。

只有 Vendor 分支涉及 DLL 加载/卸载日志；Qt/Mock 分支记录进程内 Provider 创建、连接和释放。日志字段的唯一主定义见 [日志接口协议](log-interface-protocol.md)。

---

## 8. Mock 与测试边界

纯协议编解码单元测试可以直接使用 `ScriptedByteTransport` 或 `SystemStatusSimulator`，因为这类测试不宣称验证 HAL。

产品模拟、算法集成和端到端测试必须经过 HAL。HAL Mock 的目标链路为：

```text
算法 -> HAL（providerId 指向 Mock）-> Mock Provider -> 模拟设备行为
```

也可以使用标准 Provider 连接隔离模拟目标，但必须单独标为 Qt Provider 证据，不能冒充 HAL Mock 或真实硬件证据。

当前自动化分别覆盖协议级 Simulator、Qt UDP 隔离目标、MockAdapter、动态 Fake C ABI 和生产 NI Adapter + Fake NIDAQmx。它们只能证明各自软件边界；测试目标、当前清单和执行证据统一见[测试规范](../testing/testing-specification.md)，不得在本契约重复维护计数或用例列表。

---

## 9. 当前差距与验收

| 能力 | 当前 | 目标验收 |
| --- | --- | --- |
| 后端选择 | 控制资源按 `providerId` 路由；非控制设备按 `adapterId` 经 `AdapterRouter` 惰性选择 Mock 或配置的 C ABI DLL | 补控制通道通用 Router、Provider 扫描和更多已验收后端 |
| Qt 串口 | `qt.serial` 已实现宿主端口枚举、配置、打开、原始读写和关闭；当前契约不预设任何一次真机运行结果 | 补自动化 hardware target，并完成长时、超时、拔插、运行中停止和物理收尾验收 |
| UDP/TCP | `qt.udp` 已实现并有本机闭环；持续异源报文可能绕过总读取 deadline；TCP 未实现 | 明确现场 UDP 端点并验证有界读取；另行评审 TCP |
| Vendor Adapter | 通用 `CAbiAdapter` 已实际加载并调用核心 ABI v1 与可选 task ABI；可选 `hwtest_adapter_ni_daqmx` 已实现 PXI-6259 按需/任务式模拟、数字和计数器 I/O，以及 PXI-6733 AO-only profile；自动化使用动态 Fake DLL 与 Fake NIDAQmx | 补其他原生厂家 Adapter；PXI-6259/PXI-6733 真机验收与 `hardware` CTest 标签均未实现 |
| 设备发现 | HAL 设备来自配置；宿主串口可独立只读枚举 | Provider 设备扫描并与配置 match |
| deadline | 部分方法传递 timeout；批量通道操作、旧串口事务和异源 UDP 路径尚未共享严格总预算 | 一次 HAL 操作共享总预算 |
| 产品级 Mock | Simulator 绕过 HAL；另有 Qt UDP 隔离模拟目标 | 增加控制通道 Mock Provider 闭环 |
| 生产安全 | 关闭前尽力应用 safeState；当前仍存在非有限模拟值未拒绝、错误 safeState 键静默丢弃、重新初始化吞掉上次 shutdown 失败，以及 open/close 函数不成对时原生句柄回收不确定 | 异常、停止和关闭路径均有可验证物理收尾；真实 PXI-6259/PXI-6733 和其他厂商端口语义另行验收 |

在代码达到目标前，文档和测试报告必须继续保留“未实现”标记，不得以 Mock echo、Simulator 或已存在的接口声明替代实现证据。
