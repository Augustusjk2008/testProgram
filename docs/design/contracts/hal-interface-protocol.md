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

该路由只覆盖 `module = "control"` 的资源，Qt 标准接口不经过 Vendor C ABI。其他现有资源仍走 `HalService -> CAbiAdapter -> MockAdapter`；因此不能描述为通用 Provider Router 已完成。TCP、控制通道 Mock Provider、Vendor Provider 和真实厂家 Adapter 尚未接入。

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
| `IControlChannel` | 按逻辑资源打开/关闭控制通道，并执行原始字节读写 |
| `availableSerialPorts()` | 只读枚举宿主串口并返回稳定排序的 `SerialPortDescriptor` 值列表 |

`IHalDevice::controlChannel()` 返回当前控制通道接口。公共方法签名以对应头文件为准；没有新增或冻结 `INetworkBus`，`IControlChannel` 不向算法暴露 `QSerialPort`、`QUdpSocket` 或 UDP 数据报类型。TCP 仍待真实用例、deadline 和流语义明确后评审。

`availableSerialPorts()` 定义在 `hal_factory.h`，内部使用 `QSerialPortInfo::availablePorts()`，只返回端口名、说明、厂家、序列号和系统位置，不创建 HAL 会话、不打开端口，也不等价于 `IHalService::scanDevices()`。应用层将其映射为自身 DTO，TUI 不直接依赖 HAL 或 Qt SerialPort 类型。空列表表示当前未发现宿主串口；端口是否被占用、是否有权限以及 DUT 是否响应，只能在后续 HAL 打开和通讯时确定。

### 2.1 `IHalService` 语义

- `initialize(const QVariantMap&)`：加载资源、安全配置并创建既有 `CAbiAdapter` 会话后端；控制资源的 `providerId` 在首次打开时由 `ControlChannelManager` 解析。
- `scanDevices()` / `queryCapabilities()`：返回 HAL 归一化描述。当前结果来自 `ResourceMapper` 配置，不是物理扫描。
- `openDevice()`：返回 `SessionId`；连接及底层 handle 归 HAL 所有。
- `closeDevice()` / `shutdown()`：先尽力执行物理安全态并关闭连接，再释放后端；不得构造产品协议停机命令。
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

当前 `HalControlTransport` 使用单调时钟把一次请求的剩余预算分别传给控制写和后续读取；Qt 串口写入也在同一 HAL 写操作预算内排空待写字节。整个 HAL 尚未统一完成该语义：`retryCount` / `retryIntervalMs` 未执行，旧 `transactSerial()` 仍把同一个 `timeoutMs` 分别交给写和读。步骤级重试由 BIZ 编排。

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
  -> HAL 持有的 QSerialPort | QUdpSocket
```

可运行配置见 `configs/mbddf_pc_hal.json`。PC 端通过 `control.resourceId` 在同一份配置中的串口和 UDP 资源之间选择；这只是 PC 每次运行前的选择，不向 DUT 发送“切换控制口”命令。远端 IP/端口属于部署事实，不从 MB_DDF 板端网口自环测试地址推断。

通用 Router、Provider 级设备扫描、`Mock Provider`、`Vendor Adapter Provider` 和 TCP 仍是目标能力。当前设备 `adapterId` 仍用于建立兼容会话，不能与控制资源 `providerId` 混为一谈。

HAL 部署配置与 BIZ 的产品测试配置是不同边界：BIZ 新配置使用 `executionConfig` 向算法透传产品执行参数，不得把 Provider、SDK、物理端点或扫描结果塞入 `ProtocolProfile`、`ExchangeAction` 或协议 CSV。

---

## 5. 资源、参数和安全

HAL 对外只接受逻辑资源 ID，并负责：

- 校验资源存在、模块和方向匹配；
- 依据设备能力校验物理索引和功能支持；
- 归一化模拟量、数字量、串口和 CAN/CANFD 参数；
- 对输出执行范围、电平和 payload 上限校验；
- 在关闭、停止或异常路径按策略进入物理安全态。

HAL 可以转换工程单位和厂家单位，但不得把产品字段转换或测试阈值判定伪装成硬件归一化。

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

当前 ABI 版本为 `HAL_ADAPTER_ABI_VERSION == 1`。函数表覆盖设备生命周期、模拟量、数字量、串口和 CAN/CANFD 操作。

兼容规则：

- C ABI 只使用固定宽度整数、POD、opaque handle 和调用方分配缓冲区；
- 字符串为 UTF-8 且以 `\0` 结尾；
- 阻塞函数必须尊重传入的 `timeoutMs`；
- 不支持的函数指针可为 `nullptr`，HAL 调用前返回 `NotSupported`；
- 新函数只能追加到函数表尾部，并以 `structSize` 判断兼容；
- 改变已有字段语义必须升级 ABI 主版本。

该 ABI 只服务 Vendor Adapter Provider。Qt Provider 和 Mock Provider 为进程内实现，不经过此 ABI。

当前 Adapter 状态码映射保持兼容；其中 `HAL_ADAPTER_PROTOCOL_ERROR` 只有在厂家明确说明为传输层错误时才映射 `ProtocolError`，否则使用 `AdapterError` 并保留厂家码。

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

当前保留直连 `SystemStatusSimulator` 的 golden 测试，并新增“算法 -> HAL -> `qt.udp` -> 本机隔离模拟目标”的 `SYSTEM_STATUS` 集成测试。后者证明 Qt UDP Provider 路径，但不是 HAL Mock Provider、真实网口或真实 DUT 证据。当前 `MockAdapter` 的串口 echo 和 CAN loopback 仍只证明基础原始 I/O。

完整测试分层、证据等级和真实硬件隔离要求见 [测试规范](../testing/testing-specification.md)。

---

## 9. 当前差距与验收

| 能力 | 当前 | 目标验收 |
| --- | --- | --- |
| 后端选择 | 控制资源按 `providerId` 路由；其他资源固定 `CAbiAdapter -> MockAdapter` | 扩展为通用 Router |
| Qt 串口 | `qt.serial` 已实现宿主端口枚举、配置、打开、原始读写和关闭；无实机证据 | 真实 Windows 串口隔离联调 |
| UDP/TCP | `qt.udp` 已实现并有本机闭环；TCP 未实现 | 明确现场 UDP 端点；另行评审 TCP |
| Vendor Adapter | Loader 和 ABI 已有，未接入调用链 | 真实 DLL 经 Vendor Provider 使用 ABI v1 |
| 设备发现 | HAL 设备来自配置；宿主串口可独立只读枚举 | Provider 设备扫描并与配置 match |
| deadline | 部分方法传递 timeout | 一次 HAL 操作共享总预算 |
| 产品级 Mock | Simulator 绕过 HAL；另有 Qt UDP 隔离模拟目标 | 增加控制通道 Mock Provider 闭环 |
| 生产安全 | 有基础 safeState | 异常、停止和关闭路径均有可验证物理收尾 |

在代码达到目标前，文档和测试报告必须继续保留“未实现”标记，不得以 Mock echo、Simulator 或已存在的接口声明替代实现证据。
