# 日志模块接口协议

> 本文是 `LogEvent`、日志来源、追踪链以及 HAL/Adapter 到主日志模型映射的唯一主定义。当前实现细节见 `../implementation/logging-implementation-design-report.md`；分层边界见 `../overview/five-layer-architecture.md`。
>
> `[当前实现]` 表示 `src/logging/` 和当前 HAL 日志接口已提供的能力。`[目标契约-未实现]` 的 UI、真实厂家链或 Provider 日志来源不因本契约而视为已经落地。

## 1. 模块边界

日志是五层架构的旁路基础模块。各层生产事件，日志服务缓存、分发和持久化；日志模块不反向调用 UI、BIZ、算法、HAL 或 Adapter。

```text
生产者
  -> logProduced(LogEvent)
  -> ILogService::append(LogEvent)
  -> recent 缓存 / sink / logAppended

HAL
  -> IHalService::logProduced(HalLogEvent)
  -> fromHalLogEvent
  -> ILogService::append(LogEvent)
```

- `[当前实现]` `hwtest_log_types` 仅提供值类型，`hwtest_log` 提供服务、JSONL sink 和 HAL 桥接。
- 五层不直接写日志存储；报告只读取日志摘要，不负责收集日志。
- 配置、日志和报告文件 I/O 不属于生产硬件/通讯 I/O 边界。

## 2. 主日志模型

```cpp
struct LogEvent {
    qint64 timestampUs = 0;
    QString level;       // TRACE / DEBUG / INFO / WARN / ERROR / FATAL
    QString source;      // ui / flow / algorithm / hal / adapter / system
    QString category;
    QString message;
    QString requestId;
    qint64 durationMs = -1;
    QString status;
    QString adapterCode;
    QVariantMap context;
};
```

辅助等级：

```cpp
enum class LogLevel {
    Trace, Debug, Info, Warn, Error, Fatal, Off
};
```

| 字段 | 语义 |
| --- | --- |
| `timestampUs` | UTC Unix epoch 微秒；空或非正值由 `LogService` 补齐 |
| `level` | 大写等级；空或无法识别时规范化为 `INFO` |
| `source` | 来源层或模块，不表达业务状态 |
| `category`、`message` | 细分分类和可读信息 |
| `requestId` | 同一操作链的追踪 ID；不适用时为空 |
| `durationMs` | 操作耗时；不适用时为 `-1` |
| `status`、`adapterCode` | 结果状态和适配器错误码；非硬件事件可为空 |
| `context` | 产品、工位、步骤、设备、资源等扩展上下文 |

`Off` 只用于 `LogService` 最小等级过滤，不能作为事件等级写入。

## 3. 生产与服务接口

生产语义统一为：

```cpp
signals:
    void logProduced(const LogEvent& event);
```

Qt 实现不强制继承统一基类，但新代码不得以 `ILogger` 或 `logGenerated` 作为主契约。历史名称只能在兼容层映射到 `logProduced(LogEvent)`。

```cpp
class ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual void append(const LogEvent& event) = 0;
    virtual void flush() {}
};

class ILogService : public QObject {
    Q_OBJECT
public:
    virtual ~ILogService() = default;
    virtual void append(const LogEvent& event) = 0;
    virtual QVector<LogEvent> recent(int maxCount) const = 0;

signals:
    void logAppended(const LogEvent& event);
};
```

`[当前实现]` `LogService` 负责等级过滤、有界 recent 缓存、sink 分发和 `logAppended`。它在释放内部锁后调用 sink 和发射信号；sink 生命周期由调用方管理。

## 4. 来源与追踪约定

| `source` | 语义 |
| --- | --- |
| `ui` | UI 交互层 |
| `flow` | BIZ 调度层 |
| `algorithm` | 核心测试算法层 |
| `hal` | HAL 硬件抽象层 |
| `adapter` | Adapter 或 Provider 边界 |
| `system` | 应用启动关闭、全局异常等框架事件 |

该词表是契约，不表示所有来源均已有生产者；当前仓库没有 UI 或真实厂家链。

- BIZ 为一次测试任务创建非空 `requestId`；同一操作链的生产者应复用该值。
- HAL 产生事件时应提供已知的耗时、状态、设备、资源和操作上下文。
- 不适用字段保持空值或默认值；不得以虚构值填充追踪字段。

## 5. HAL/Adapter 映射

`[当前实现]` HAL 通过 `IHalService::logProduced(const HalLogEvent&)` 产生内部事件；日志模块通过下列接口桥接：

```cpp
LogEvent fromHalLogEvent(const hwtest::hal::HalLogEvent& event);

QMetaObject::Connection connectHalLogs(hwtest::hal::IHalService* halService,
                                       ILogService* logService,
                                       Qt::ConnectionType type = Qt::AutoConnection);
```

映射由 `fromHalLogEvent()` 定义：

| `LogEvent` 字段 | `HalLogEvent` 来源 |
| --- | --- |
| `timestampUs` | `timestampUs` |
| `level` | `level` |
| `source` | `source`；为空时补 `hal` |
| `category` | `category` |
| `message` | `message` |
| `requestId` | `requestId` |
| `durationMs` | `durationMs` |
| `status` | `status` |
| `adapterCode` | `adapterCode` |
| `context.deviceId` | `deviceId`，非空时写入 |
| `context.resourceId` | `resourceId`，非空时写入 |
| `context.operation` | `operation`，非空时写入 |

上下文合并规则：

- 先保留 `HalLogEvent.context`。
- 当 `requestId`、`status`、`adapterCode` 非空时，分别镜像到 `context`；当 `durationMs >= 0` 时镜像到 `context.durationMs`。
- `deviceId`、`resourceId`、`operation` 非空时写入 `context`。
- 空的标准顶层字段不得覆盖原有 `context` 同名键。顶层字段始终保留其原始值。
- `connectHalLogs()` 只连接信号并调用此映射，不修改 HAL ABI。

`[目标契约-未实现]` 当 Provider 或真实 Vendor Adapter 通过其宿主回调产生日志时，HAL 应将其归一化为 `source = "adapter"` 的 `HalLogEvent`，再按本节映射。当前没有真实厂家链，不能把该路径视为已验证。

兼容信号 `IHalService::logMessage(...)` 只服务旧接入；新代码以 `logProduced(HalLogEvent)` 和本节桥接为主。

## 6. 验收边界

- 项目主日志模型是 `LogEvent`，主生产语义是 `logProduced`。
- HAL/Adapter 日志映射只以本文件第 5 节为主定义；总览、BIZ 契约和实现报告只引用本节。
- 日志服务不承担 CSV 数据记录、产品判定或报告生成。
- UI、Provider 或真实厂家日志的存在与否应按对应模块的当前实现另行验证。
