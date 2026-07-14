# HAL 层接口协议

> 适用项目：多产品通用硬件测试软件（Qt 5.15 / C++17 / Windows）
> 本文定位：HAL 对上接口、Adapter ABI、资源映射、错误映射、日志链路。
> 当前范围：AD、DA、DI、DO、串口、CANFD。
> 公共头文件以 `src/hal/include/hal/` 为准；本文与代码需保持一致。

---

## 1. 边界

```text
核心测试算法层
  -> IHalService / IHalDevice / IAnalogIo / IDigitalIo / ISerialBus / ICanFdBus
  -> HAL
  -> HardwareAdapter / HalAdapterApiV1
  -> Adapter
  -> 厂家 DLL / lib / SDK / Win32 API
```

HAL 负责：

- 设备发现、打开、关闭、复位、健康检查。
- 逻辑资源到真实设备、通道、总线端口的映射。
- 参数归一化。
- 输出安全校验。
- Adapter 和厂家错误到 `HalStatusCode` 的统一映射。
- 操作耗时、状态和硬件上下文日志。

HAL 不负责：

- UI 展示。
- 业务流程编排。
- 测试项判定。
- 报告生成。
- 厂家业务规则。

---

## 2. 对上协议原则

- 命名空间：`hwtest::hal`。
- 对上使用 Qt/C++ 类型：`QString`、`QByteArray`、`QVector`、`QVariantMap`。
- 硬件调用必须携带 `OperationOptions` 或等价选项，至少含 `timeoutMs`。
- 返回值统一为 `HalStatus` 或 `HalResult<T>`。
- 调用方只传逻辑资源 ID，不传厂家句柄或物理通道。
- 新代码连接 `IHalService::logProduced(const HalLogEvent&)`；`logMessage` 只作兼容信号。

---

## 3. 公共类型

主定义文件：`src/hal/include/hal/hal_types.h`。

```cpp
using DeviceId = QString;
using AdapterId = QString;
using ResourceId = QString;
using RequestId = QString;
using SessionId = QString;
```

### 3.1 状态码

```cpp
enum class HalStatusCode {
    Ok = 0,
    InvalidArgument,
    InvalidState,
    NotInitialized,
    NotFound,
    NotSupported,
    PermissionDenied,
    Busy,
    Timeout,
    Cancelled,
    SafetyLimitExceeded,
    DeviceDisconnected,
    AdapterLoadFailed,
    AdapterSymbolMissing,
    AdapterError,
    IoError,
    ProtocolError,
    CrcMismatch,
    DataMismatch,
    BufferTooSmall,
    InternalError
};
```

### 3.2 状态与结果

```cpp
struct HalError {
    HalStatusCode code = HalStatusCode::Ok;
    QString message;
    QString adapterCode;
    QString deviceId;
    QString resourceId;
    QString operation;
    QVariantMap detail;
};

struct HalStatus {
    HalStatusCode code = HalStatusCode::Ok;
    HalError error;
    bool ok() const noexcept;
};

template <typename T>
struct HalResult {
    HalStatus status;
    T value {};
    bool ok() const noexcept;
};
```

### 3.3 操作选项

```cpp
struct OperationOptions {
    int timeoutMs = 1000;
    int retryCount = 0;
    int retryIntervalMs = 50;
    RequestId requestId;
    QVariantMap tags;
};
```

### 3.4 HAL 日志事件

```cpp
struct HalLogEvent {
    qint64 timestampUs = 0;
    QString level;
    QString source;
    QString category;
    QString message;
    RequestId requestId;
    qint64 durationMs = -1;
    QString status;
    QString adapterCode;
    DeviceId deviceId;
    ResourceId resourceId;
    QString operation;
    QVariantMap context;
};
```

`HalLogEvent` 只定义 HAL 内部硬件上下文。日志模块主模型仍是 `LogEvent`，映射规则见 `log-interface-protocol.md`。不得把 `LogService` 或 `LogEvent` 放入 `src/hal/include/hal/`，日志模块接入不得修改 `hal_adapter_abi.h`。

### 3.5 设备与能力

```cpp
struct DeviceDescriptor {
    DeviceId deviceId;
    AdapterId adapterId;
    QString vendor;
    QString model;
    QString serialNumber;
    QString location;
    QString firmwareVersion;
    QVariantMap properties;
};

struct ChannelDescriptor {
    ResourceId resourceId;
    QString module;
    QString direction;
    int physicalIndex = -1;
    QVariantMap properties;
};

struct DeviceCapabilities {
    DeviceDescriptor device;
    QVector<ChannelDescriptor> channels;
    QStringList supportedModules;
    QVariantMap limits;
};
```

---

## 4. HAL 对上接口

### 4.1 `IHalService`

主定义文件：`src/hal/include/hal/i_hal_service.h`。

```cpp
class IHalService : public QObject {
    Q_OBJECT
public:
    virtual HalStatus initialize(const QVariantMap& halConfig) = 0;
    virtual HalStatus shutdown() = 0;

    virtual HalResult<QVector<DeviceDescriptor>> scanDevices(const OperationOptions& options) = 0;
    virtual HalResult<DeviceCapabilities> queryCapabilities(const DeviceId& deviceId,
                                                            const OperationOptions& options) = 0;

    virtual HalResult<SessionId> openDevice(const DeviceId& deviceId,
                                            const OperationOptions& options) = 0;
    virtual HalStatus closeDevice(const SessionId& sessionId,
                                  const OperationOptions& options) = 0;
    virtual HalStatus resetDevice(const SessionId& sessionId,
                                  const OperationOptions& options) = 0;
    virtual HalStatus healthCheck(const SessionId& sessionId,
                                  const OperationOptions& options) = 0;

    virtual HalResult<IHalDevice*> device(const SessionId& sessionId) = 0;

signals:
    void deviceChanged(const DeviceDescriptor& device, const QString& state);
    void hardwareEvent(const QString& eventType, const QVariantMap& payload);
    void logProduced(const HalLogEvent& event);
    void logMessage(const QString& level,
                    const QString& category,
                    const QString& message,
                    const QVariantMap& context);
};
```

约定：

- `initialize` 建立 Adapter、设备能力、资源映射和安全配置。
- `scanDevices` 返回 HAL 归一化设备描述。
- `openDevice` 返回 `SessionId`，后续用 `device(sessionId)` 取得聚合设备接口。
- `shutdown` 需关闭设备、释放 Adapter，并尽量让输出进入安全状态。

### 4.2 `IHalDevice`

主定义文件：`src/hal/include/hal/i_hal_device.h`。

```cpp
class IHalDevice {
public:
    virtual DeviceDescriptor descriptor() const = 0;
    virtual DeviceCapabilities capabilities() const = 0;

    virtual IAnalogIo* analogIo() = 0;
    virtual IDigitalIo* digitalIo() = 0;
    virtual ISerialBus* serialBus() = 0;
    virtual ICanFdBus* canFdBus() = 0;
};
```

### 4.3 模拟量 `IAnalogIo`

主定义文件：`src/hal/include/hal/i_analog_io.h`。

```cpp
class IAnalogIo {
public:
    virtual HalStatus configureAd(const ResourceId& channel,
                                  const AnalogRange& range,
                                  const OperationOptions& options) = 0;
    virtual HalResult<AnalogSample> readAd(const ResourceId& channel,
                                           const AnalogReadOptions& options) = 0;
    virtual HalResult<QVector<AnalogSample>> readAdBatch(const QVector<ResourceId>& channels,
                                                         const AnalogReadOptions& options) = 0;

    virtual HalStatus configureDa(const ResourceId& channel,
                                  const AnalogRange& range,
                                  const OperationOptions& options) = 0;
    virtual HalStatus writeDa(const ResourceId& channel,
                              double value,
                              const AnalogWriteOptions& options) = 0;
    virtual HalStatus writeDaBatch(const QMap<ResourceId, double>& values,
                                   const AnalogWriteOptions& options) = 0;
};
```

约定：算法传工程值，HAL 负责单位转换、安全量程校验和 Adapter 调用。

### 4.4 数字量 `IDigitalIo`

主定义文件：`src/hal/include/hal/i_digital_io.h`。

```cpp
class IDigitalIo {
public:
    virtual HalResult<DigitalSample> readDi(const ResourceId& channel,
                                            const OperationOptions& options) = 0;
    virtual HalResult<QVector<DigitalSample>> readDiBatch(const QVector<ResourceId>& channels,
                                                          const OperationOptions& options) = 0;

    virtual HalStatus writeDo(const ResourceId& channel,
                              DigitalLevel level,
                              const DigitalWriteOptions& options) = 0;
    virtual HalStatus writeDoBatch(const QMap<ResourceId, DigitalLevel>& values,
                                   const DigitalWriteOptions& options) = 0;

    virtual HalResult<DigitalSample> waitEdge(const ResourceId& channel,
                                              DigitalLevel targetLevel,
                                              const OperationOptions& options) = 0;
};
```

### 4.5 串口 `ISerialBus`

主定义文件：`src/hal/include/hal/i_serial_bus.h`。

串口接口只负责端口配置和原始字节收发。测试设备与被测件之间的字段级帧结构、CSV 建模、打包和解析规则见 `device-communication-protocol.md`。

```cpp
class ISerialBus {
public:
    virtual HalStatus openSerial(const ResourceId& port,
                                 const SerialConfig& config,
                                 const OperationOptions& options) = 0;
    virtual HalStatus closeSerial(const ResourceId& port,
                                  const OperationOptions& options) = 0;
    virtual HalStatus flushSerial(const ResourceId& port,
                                  const OperationOptions& options) = 0;

    virtual HalStatus writeSerial(const ResourceId& port,
                                  const QByteArray& data,
                                  const OperationOptions& options) = 0;
    virtual HalResult<QByteArray> readSerial(const ResourceId& port,
                                             int maxBytes,
                                             const OperationOptions& options) = 0;
    virtual HalResult<SerialTransactionResult> transactSerial(const ResourceId& port,
                                                              const SerialTransaction& transaction) = 0;
};
```

### 4.6 CANFD `ICanFdBus`

主定义文件：`src/hal/include/hal/i_canfd_bus.h`。

CANFD 接口只负责 CANFD 总线配置和原始帧收发。CANFD payload 中承载的字段级协议结构见 `device-communication-protocol.md`。

```cpp
class ICanFdBus {
public:
    virtual HalStatus openCan(const ResourceId& bus,
                              const CanFdConfig& config,
                              const OperationOptions& options) = 0;
    virtual HalStatus closeCan(const ResourceId& bus,
                               const OperationOptions& options) = 0;
    virtual HalStatus setCanFilters(const ResourceId& bus,
                                    const QVector<CanFdFilter>& filters,
                                    const OperationOptions& options) = 0;
    virtual HalStatus sendCan(const ResourceId& bus,
                              const CanFdFrame& frame,
                              const OperationOptions& options) = 0;
    virtual HalResult<CanFdFrame> receiveCan(const ResourceId& bus,
                                             const OperationOptions& options) = 0;
    virtual HalResult<QVector<CanFdFrame>> receiveCanBatch(const ResourceId& bus,
                                                           int maxFrames,
                                                           const OperationOptions& options) = 0;
};
```

---

## 5. Adapter ABI

主定义文件：`src/hal/include/hal/hal_adapter_abi.h`。

外部 Adapter DLL 导出：

```cpp
int HAL_ADAPTER_CALL hal_adapter_get_api_v1(const HalAdapterHostApiV1* host,
                                            HalAdapterApiV1* outApi);
```

当前 ABI：

```cpp
#define HAL_ADAPTER_ABI_VERSION 1
```

`HalAdapterApiV1` 函数表：

```text
getInfo
initialize / shutdown / enumerateDevices
openDevice / closeDevice / resetDevice / getCapabilities
analogConfigure / analogRead / analogWrite
digitalRead / digitalWrite / digitalWaitEdge
serialOpen / serialClose / serialWrite / serialRead
canOpen / canClose / canSetFilters / canSend / canReceive
```

ABI 规则：

- C ABI 只使用固定宽度整数、POD、调用方分配缓冲区和 opaque handle。
- 所有字符串为 UTF-8、`\0` 结尾。
- 阻塞函数必须尊重 `timeoutMs`。
- 不支持能力的函数指针可为 `nullptr`；HAL 调用前检查并返回 `NotSupported`。
- 新增函数只能追加到函数表尾部，并用 `structSize` 判断兼容。
- 修改字段语义必须升级 ABI 主版本。

---

## 6. 资源映射

`.testcfg` 使用逻辑资源，不暴露物理通道：

```json
{
  "hardware": {
    "devices": [
      {
        "alias": "main_daq",
        "adapterId": "acme.daq.v1",
        "match": {"serialNumber": "DAQ-001"}
      }
    ],
    "resources": {
      "AD_MAIN_0": {"device": "main_daq", "module": "analog", "direction": "input", "physicalIndex": 0},
      "DA_MAIN_0": {"device": "main_daq", "module": "analog", "direction": "output", "physicalIndex": 0},
      "SERIAL_A": {"device": "main_daq", "module": "serial", "physicalIndex": 0},
      "CANFD_A": {"device": "main_daq", "module": "canfd", "physicalIndex": 0}
    }
  }
}
```

HAL 初始化后建立：

```text
ResourceId -> DeviceSession -> AdapterDeviceHandle -> physicalIndex -> Adapter API
```

HAL 必须校验：

- 资源是否存在。
- 模块和方向是否匹配。
- 设备能力是否支持。
- 物理索引是否有效。
- 安全范围是否允许。

---

## 7. 参数归一化

HAL 调 Adapter 前统一处理：

- 模拟量单位和量程转换。
- DI/DO 电平值转换。
- 串口校验位、停止位、流控转换。
- CANFD DLC、payload 长度、ID 类型、bitrate 配置校验。
- 默认超时、重试和 requestId 透传。
- 输出安全边界校验。

---

## 8. 错误映射

Adapter 到 HAL 映射：

| Adapter code | HAL code |
| --- | --- |
| `HAL_ADAPTER_OK` | `Ok` |
| `HAL_ADAPTER_INVALID_ARGUMENT` | `InvalidArgument` |
| `HAL_ADAPTER_NOT_FOUND` | `NotFound` |
| `HAL_ADAPTER_NOT_SUPPORTED` | `NotSupported` |
| `HAL_ADAPTER_BUSY` | `Busy` |
| `HAL_ADAPTER_TIMEOUT` | `Timeout` |
| `HAL_ADAPTER_IO_ERROR` | `IoError` |
| `HAL_ADAPTER_PROTOCOL_ERROR` | `ProtocolError` |
| `HAL_ADAPTER_DEVICE_DISCONNECTED` | `DeviceDisconnected` |
| `HAL_ADAPTER_BUFFER_TOO_SMALL` | `BufferTooSmall` |
| `HAL_ADAPTER_INTERNAL_ERROR` | `InternalError` |

HAL 返回错误时必须尽量携带：

- `operation`
- `deviceId`
- `resourceId`
- `adapterCode`
- 底层 message
- 关键参数上下文
- 厂家原始错误码，放入 `HalError.detail["vendorCode"]`

---

## 9. 日志链路

HAL 每次关键硬件调用生成 `HalLogEvent`：

```text
OperationOptions.requestId
  -> HAL 调用计时
  -> Adapter 调用
  -> HalStatus / adapterCode / durationMs
  -> emit IHalService::logProduced(HalLogEvent)
  -> hwtest::logging::fromHalLogEvent(...)
  -> ILogService::append(LogEvent)
```

字段来源：

| 字段 | 来源 |
| --- | --- |
| `requestId` | `OperationOptions.requestId` |
| `durationMs` | HAL 调用前后计时 |
| `status` | `HalStatusCode` |
| `adapterCode` | `HalStatus.error.adapterCode` |
| `deviceId` | 设备描述或会话 |
| `resourceId` | 逻辑资源映射 |
| `source` | `hal` 或 `adapter` |

`host.log` 边界：

- Adapter C ABI 可通过 `HalAdapterHostApiV1::log` 输出日志。
- HAL 接收后转换为 `HalLogEvent{source="adapter"}`。
- 当前 ABI 不向 Adapter 传 `requestId`；HAL 在调用上下文中补齐。
- HAL 不依赖 `hwtest_log`；`src/logging/hal_log_bridge.*` 同时链接 HAL 与日志模块，负责边界映射。

---

## 10. 生命周期

### 初始化

```text
IHalService.initialize(halConfig)
  -> 加载或创建 Adapter
  -> adapter.initialize(configJson)
  -> adapter.enumerateDevices()
  -> 建立 DeviceDescriptor / DeviceCapabilities
  -> 建立 ResourceMapper
  -> 加载安全配置
```

### 调用

```text
算法传 ResourceId
  -> HAL 查映射
  -> 校验能力和安全范围
  -> 参数归一化
  -> 调 Adapter API
  -> 映射数据、错误和日志
  -> 返回 HalResult<T> 或 HalStatus
```

### 关闭

```text
shutdown / closeDevice
  -> 停止采样或监听
  -> 输出进入安全状态
  -> closeDevice
  -> adapter.shutdown
  -> unload adapter DLL
```

---

## 11. Mock Adapter

Mock Adapter 是默认开发路径，必须支持：

- AD 模拟采样。
- DA 输出记录和 AD/DA 回环。
- DI/DO 回环。
- 串口 echo。
- CANFD loopback。
- 可配置超时、错误码、随机噪声。

Mock 行为仍须走 HAL 接口和 Adapter 抽象，测试算法不得绕过 HAL。

---

## 12. 验收标准

- 算法层不包含厂家 SDK 头文件即可编译。
- 更换厂家只新增或替换 Adapter，不改算法。
- 所有硬件操作有超时、统一错误、日志和 `requestId`。
- 输出操作先校验安全范围再下发。
- 外部 Adapter 遵守 `HAL_ADAPTER_ABI_VERSION == 1`。
- 无真实硬件时 Mock 能跑通当前范围基础流程。
