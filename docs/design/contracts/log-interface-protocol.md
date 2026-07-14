# 日志模块接口协议

> 适用项目：多产品通用硬件测试软件（Qt 5.15 / C++17 / Windows）
> 本文定位：日志模型、生产接口、来源约定、HAL/Adapter 映射。
> 原则：五层只生产日志事件，不直接写日志文件、数据库或报告摘要。

---

## 1. 模块定位

日志模块是五层架构旁路基础模块，不属于 UI、业务调度、算法、HAL 或 Adapter。

```text
UI / 业务调度 / 算法
  -> emit logProduced(LogEvent)
  -> ILogService::append(LogEvent)
  -> recent 缓存 / sink 持久化 / UI 展示

HAL / Adapter
  -> emit IHalService::logProduced(HalLogEvent)
  -> hal_log_bridge 映射
  -> ILogService::append(LogEvent)
```

边界：

- 五层可通过 signal 生产日志事件。
- 五层不直接写日志存储。
- 日志模块不反向调用 UI、业务流程、算法、HAL 或 Adapter。
- 报告模块只读取日志摘要，不负责收集日志。
- Adapter C ABI 日志先通过 HAL host 回调进入 HAL，再由 HAL 产出 `source = "adapter"` 的 `HalLogEvent`。
- `monitor.hpp`、`monitor_ui.*`、`monitor_ui_logger.*` 不属于本契约范围。

---

## 2. 日志事件模型

主模型：

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
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
    Off
};
```

约定：

- 文本字段使用大写等级名：`TRACE`、`DEBUG`、`INFO`、`WARN`、`ERROR`、`FATAL`。
- `Off` 只用于 `LogService` 最小等级过滤，不作为事件等级写入。
- 空等级由 `LogService` 补为 `INFO`。
- 空时间戳由 `LogService` 补为当前 UTC Unix epoch 微秒。

字段：

| 字段 | 含义 |
| --- | --- |
| `timestampUs` | Unix epoch 微秒 |
| `level` | 日志级别 |
| `source` | 来源层或模块 |
| `category` | 细分分类，如 `hal.analog.readAd` |
| `message` | 可读信息 |
| `requestId` | 测试项或操作追踪 ID，可空 |
| `durationMs` | 操作耗时；无耗时语义为 `-1` |
| `status` | 操作结果，如 `Ok` / `Timeout` / `IoError` |
| `adapterCode` | Adapter 统一错误码；非硬件日志可空 |
| `context` | 产品、工位、步骤、设备、资源等扩展上下文 |

---

## 3. 生产接口

各层使用一致语义：

```cpp
signals:
    void logProduced(const LogEvent& event);
```

Qt 实现不强制继承统一基类；信号语义一致即可。

禁止把 `ILogger` 或 `logGenerated` 作为项目主契约。若附件或历史代码出现这些名称，只能作为兼容名映射到 `logProduced(LogEvent)`。

---

## 4. 日志服务接口

```cpp
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

约定：

- 各层 `logProduced` 连接到 `ILogService::append`。
- `LogService` 负责等级过滤、有界 recent 缓存、sink 分发和 `logAppended`。
- UI 订阅 `logAppended` 做实时展示。
- `LogService` 不在内部状态锁内调用 sink 或 emit signal，避免回调重入导致死锁。
- recent 缓存按追加顺序保存最近事件，`recent(maxCount)` 返回最多 `maxCount` 条。

sink 约定：

```cpp
class ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual void append(const LogEvent& event) = 0;
    virtual void flush() {}
};
```

当前文件 sink 为 `JsonLineFileSink`，每条日志写为一行紧凑 JSON，不输出 ANSI 颜色。

---

## 5. 来源约定

| source | 来源 |
| --- | --- |
| `ui` | UI 交互层 |
| `flow` | 业务调度层 |
| `algorithm` | 核心测试算法层 |
| `hal` | HAL 硬件抽象层 |
| `adapter` | 硬件适配器层 |
| `system` | 应用框架、启动关闭、全局异常 |

`source` 只描述来源，不描述业务状态；状态写入 `status` 或 `context`。

---

## 6. 追踪链

- 业务调度层为每次测试任务生成一个非空 `requestId`。
- 任务内各步骤以及 UI、业务、算法、HAL、Adapter 同一操作链复用该 `requestId`。
- HAL 调 Adapter 前后计时，补齐 `durationMs`、`status`、`adapterCode`。
- Adapter 不能接收 `requestId` 时，由 HAL 在转发 Adapter 日志时补齐。
- 不适用字段保持空值或默认值。

---

## 7. HAL/Adapter 映射

HAL 内部事件类型为 `HalLogEvent`，定义在 `hal_types.h`。

映射到 `LogEvent`：

| LogEvent 字段 | HAL 来源 |
| --- | --- |
| `timestampUs` | `HalLogEvent.timestampUs` |
| `level` | `HalLogEvent.level` |
| `source` | `HalLogEvent.source`，通常为 `hal` 或 `adapter` |
| `category` | `HalLogEvent.category` |
| `message` | `HalLogEvent.message` |
| `requestId` | `OperationOptions.requestId` |
| `durationMs` | HAL 调用计时 |
| `status` | `HalStatusCode` 字符串 |
| `adapterCode` | `HalStatus.error.adapterCode` |
| `context.deviceId` | `HalLogEvent.deviceId` |
| `context.resourceId` | `HalLogEvent.resourceId` |
| `context.operation` | `HalLogEvent.operation` |

规则：

- `HalLogEvent.source` 为空时补 `hal`。
- `deviceId`、`resourceId`、`operation` 写入 `LogEvent.context`。
- `requestId`、`durationMs`、`status`、`adapterCode` 同时保留在 `LogEvent` 顶层字段和 `context`。
- 原始 `HalLogEvent.context` 字段保留；同名标准字段以 HAL 顶层字段为准。
- `connectHalLogs(IHalService*, ILogService*)` 只负责 signal 连接和映射，不修改 HAL ABI。

兼容信号：

- `IHalService::logProduced(const HalLogEvent&)` 是 HAL 新日志入口。
- `IHalService::logMessage(...)` 只保留给旧 UI/旧日志接入。
- HAL 发 `logProduced` 时可同步桥接 `logMessage`，但不得把 `logMessage` 作为新主契约。

Adapter 日志：

- Adapter C ABI 的 `host.log` 进入 HAL。
- HAL 转换为 `source = "adapter"` 的 `HalLogEvent`。
- 再由日志接入层转为 `LogEvent`。

---

## 8. 验收标准

- 项目主日志模型只有 `LogEvent`。
- 项目主生产语义只有 `logProduced`。
- `ILogger`、`logGenerated` 不作为主契约出现。
- HAL/Adapter 日志可追踪到 `requestId`、耗时、状态、设备和资源。
- 日志模块不承担 CSV 数据记录或报告生成职责。
