# 日志模块接口协议设计

> 适用项目：多产品通用硬件测试软件（Qt 5.15 / C++ / Windows）  
> 设计范围：独立日志模块与 UI、业务调度、测试算法、HAL、Adapter 五层之间的日志事件边界。  
> 原则：KISS。各层只通过 signal 生产日志事件，不直接写日志文件、数据库或报告摘要。

---

## 1. 模块定位

日志模块是五层架构旁路的独立基础模块，不属于 UI、业务调度、测试算法、HAL 或 Adapter 任一层。

```text
UI / 业务调度 / 测试算法 / HAL / Adapter
  -> emit log event
  -> LogService
```

HAL 侧边界：

```text
测试算法/业务层
  -> IHalService / IHalDevice 调用携带 OperationOptions.requestId
  -> HAL 计时并生成 HalLogEvent
  -> IHalService::logProduced(HalLogEvent)
  -> LogService::append(LogEvent)

Adapter host.log
  -> HAL 接收
  -> HalLogEvent{source="adapter"}
  -> LogService
```

边界约定：

- 五层都可以向日志模块 `emit signal` 生产日志事件。
- 五层不直接写日志文件、数据库、CSV 或报告摘要。
- 日志模块不反向调用 UI、业务流程、测试算法、HAL 或 Adapter。
- UI 可以订阅日志模块输出，用于实时展示。

---

## 2. 日志事件模型

最小日志事件字段：

```cpp
struct LogEvent {
    qint64 timestampUs = 0;
    QString level;       // TRACE / DEBUG / INFO / WARN / ERROR
    QString source;      // ui / flow / algorithm / hal / adapter / system
    QString category;    // 例如 hal.analog.read
    QString message;
    QString requestId;
    qint64 durationMs = -1;
    QString status;
    QString adapterCode;
    QVariantMap context;
};
```

字段说明：

| 字段 | 含义 |
| --- | --- |
| `timestampUs` | 事件发生时间，微秒 |
| `level` | 日志级别 |
| `source` | 日志来源层或模块 |
| `category` | 细分分类 |
| `message` | 可读日志内容 |
| `requestId` | 测试项或操作追踪 ID，可为空 |
| `durationMs` | 操作耗时；无耗时语义时为 `-1` |
| `status` | 操作结果，例如 `Ok` / `Timeout` / `IoError` |
| `adapterCode` | Adapter 统一错误码；非硬件日志可为空 |
| `context` | 产品、工位、步骤、设备、资源等扩展上下文 |

---

## 3. 追踪链约定

- 业务调度层为每个测试步骤生成 `requestId`。
- UI、业务调度、测试算法、HAL、Adapter 产生同一操作链日志时复用同一个 `requestId`。
- HAL 调用 Adapter 前记录开始时间，调用结束后补齐 `durationMs`、`status`、`adapterCode`。
- Adapter 不能接收 `requestId` 时，由 HAL 在转发 Adapter 日志时补齐。
- 不适用的字段保持空值或默认值，不为简单日志强行造字段。

---

## 4. 生产接口

各层通过信号或轻量接口投递日志事件：

```cpp
class ILogProducer {
public:
    virtual ~ILogProducer() = default;

signals:
    void logProduced(const LogEvent& event);
};
```

Qt 实现时不强制所有类继承统一基类；只要信号语义一致即可：

```cpp
emit logProduced(event);
```

---

## 5. 日志服务接口

日志模块对外暴露最小服务接口：

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

使用方式：

- 各层信号连接到 `ILogService::append`。
- `LogService` 负责缓存、持久化、查询和导出；具体存储实现后续再定。
- UI 订阅 `logAppended` 做实时展示。

---

## 6. 来源约定

| source | 来源 |
| --- | --- |
| `ui` | UI 交互层 |
| `flow` | 业务调度层 |
| `algorithm` | 核心测试算法层 |
| `hal` | HAL 硬件抽象层 |
| `adapter` | 硬件适配器层 |
| `system` | 应用框架、启动关闭、全局异常 |

---

## 7. 与现有模块关系

- HAL 当前的 `IHalService::logMessage` 可视为 HAL 到日志模块的兼容事件源。
- HAL 新代码应优先连接 `IHalService::logProduced(const HalLogEvent&)`；`logMessage` 只保留给旧 UI/旧日志接入。
- Adapter C ABI 的 `host.log` 进入 HAL 后，应转换为 `source = "adapter"` 的 `LogEvent`。
- 报告模块只读取日志摘要，不负责收集日志。
- CSV 数据记录是否复用日志模块后续再定，本协议只约束日志事件。

字段映射：

| LogEvent 字段 | HAL 来源 |
| --- | --- |
| `requestId` | `OperationOptions.requestId` |
| `durationMs` | HAL 调用计时 |
| `status` | `HalStatusCode` 字符串 |
| `adapterCode` | `HalStatus.error.adapterCode` |
| `deviceId` / `resourceId` | 放入 `context`，来自设备描述和资源映射 |
