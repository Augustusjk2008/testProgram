# 日志模块实现设计报告

> `[当前实现]` 本文只记录 `src/logging/` 已落地代码，不定义未来 Provider、网络、UI 或真实厂家日志能力。日志模型、来源和 HAL/Adapter 映射以 `../contracts/log-interface-protocol.md` 为主定义。

## 1. 构建与范围

当前日志代码位于 `src/logging/`，命名空间为 `hwtest::logging`，使用 C++17 和 Qt Core。

| 目标 | 当前内容 | 直接依赖 |
| --- | --- | --- |
| `hwtest_log_types` | `LogEvent`、`LogLevel`、字符串转换和元类型注册 | Qt Core |
| `hwtest_log` | `ILogService`、`LogService`、sink、formatter、HAL 日志桥接 | Qt Core、`hwtest_hal`、`hwtest_log_types` |

`hwtest_log_types` 是给 BIZ 等只交换 `LogEvent` 值的 HAL-free 边界；`hwtest_log` 才包含服务和 `hal_log_bridge`。本仓库当前没有 UI 订阅方。

## 2. 当前文件结构

```text
src/logging/
  include/logging/
    log_global.h
    log_types.h
    i_log_service.h
    log_service.h
    log_formatter.h
    log_file_sink.h
    hal_log_bridge.h
  src/
    log_types.cpp
    log_service.cpp
    log_formatter.cpp
    log_file_sink.cpp
    hal_log_bridge.cpp
```

当前测试目录为：

```text
tests/log/
  log_service_test.cpp
  log_file_sink_test.cpp
  hal_log_bridge_test.cpp
```

## 3. 类型与 `LogService`

`registerLogMetaTypes()` 注册 `LogLevel`、`LogEvent` 和 `QVector<LogEvent>`。`toString()` 输出大写等级；`logLevelFromString()` 大小写不敏感并兼容 `WARNING`，无法识别时使用调用方给定的默认等级。

`LogService` 的当前内部状态：

```text
m_recent        最近日志缓存
m_sinks         非拥有 sink 指针列表
m_recentLimit   缓存上限，默认 1000
m_minimumLevel  最小等级，默认 Trace
m_mutex         保护内部状态
```

`append()` 当前执行顺序：

```text
接收 LogEvent
  -> timestampUs <= 0 时补当前 UTC epoch 微秒
  -> 规范化 level
  -> 按 minimumLevel 过滤
  -> 写入有界 recent 缓存并复制 sink 列表
  -> 解锁
  -> 逐个 sink->append(event)
  -> emit logAppended(event)
```

实现不会在内部状态锁内调用 sink 或发射 `logAppended`。`recent(maxCount)` 返回追加顺序中最多 `maxCount` 条最近事件；`setRecentLimit(0)` 清空并禁用 recent 缓存。

## 4. 格式化与文件 sink

`LogFormatter` 当前提供：

- `toJsonObject(LogEvent)`：构造 `QJsonObject`。
- `toJsonLine(LogEvent)`：输出紧凑 JSON 并追加换行。
- `toTextLine(LogEvent)`：输出人可读文本，当前不是主持久化格式。

`JsonLineFileSink` 使用 `QFile` 追加写入。`append()` 在文件未打开时尝试打开，单次写入一行 JSON；`flush()` 显式刷新文件。文件 sink 不写 ANSI 颜色，也不直接写 stdout。

## 5. HAL 日志桥接

当前 `hal_log_bridge` 提供：

```cpp
LogEvent fromHalLogEvent(const hwtest::hal::HalLogEvent& event);

QMetaObject::Connection connectHalLogs(hwtest::hal::IHalService* halService,
                                       ILogService* logService,
                                       Qt::ConnectionType type = Qt::AutoConnection);
```

`connectHalLogs()` 将 HAL 的 `logProduced(HalLogEvent)` 连接到 `ILogService::append()`，中间调用 `fromHalLogEvent()`。HAL 仍只生产 `HalLogEvent`，不依赖 `LogService`。字段来源、上下文覆盖和 Adapter 来源规则不在本报告重复，统一见 `../contracts/log-interface-protocol.md` 第 5 节。

## 6. 当前验证

测试目标为 `hwtest_log_tests`，当前包含 7 个源级 `TEST*` 用例，覆盖：

- `LogService` 的默认值规范化、过滤、recent 缓存、signal 和 sink 分发。
- `JsonLineFileSink` 的 JSONL 结构、context 保留和 `flush()`。
- `hal_log_bridge` 的字段映射和 signal 连接。

运行示例：

```powershell
cmake --build build_vs --config Debug --target hwtest_log_tests
ctest --test-dir build_vs -C Debug --output-on-failure
```
