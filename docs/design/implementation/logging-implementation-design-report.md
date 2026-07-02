# 日志模块实现设计报告

> 范围：`src/logging` 当前实现。
> 技术栈：Qt 5.15 / C++17。
> 目标：说明独立日志模块如何缓存、分发、落盘，并如何接入 HAL 日志。

---

## 1. 定位

日志模块是五层架构旁路基础模块，命名空间为 `hwtest::logging`，构建产物为静态库 `hwtest_log`。

当前职责：

- 定义项目主日志模型 `LogEvent` 和辅助等级 `LogLevel`。
- 提供 `ILogService`、`LogService` 和 `ILogSink`。
- 维护有界 recent 缓存并按最小等级过滤。
- 分发日志到 sink，并通过 `logAppended` 通知 UI 或其他展示层。
- 提供 `JsonLineFileSink`，按 JSONL 持久化结构化日志。
- 提供 `hal_log_bridge`，把 `hwtest::hal::HalLogEvent` 映射为 `LogEvent`。

不做：

- 不引入 UI、monitor UI、ANSI 颜色输出或全局 `LOG_INFO` 宏。
- 不修改 `src/hal/include/hal/` 和 `hal_adapter_abi.h`。
- 不负责 CSV 数据记录、测试报告生成或业务状态判定。

---

## 2. 目录

```text
src/logging/
  CMakeLists.txt
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

测试目录：

```text
tests/log/
  CMakeLists.txt
  log_service_test.cpp
  log_file_sink_test.cpp
  hal_log_bridge_test.cpp
```

---

## 3. 公共类型

`LogEvent` 字段：

```text
timestampUs, level, source, category, message,
requestId, durationMs, status, adapterCode, context
```

`LogLevel`：

```text
Trace, Debug, Info, Warn, Error, Fatal, Off
```

实现约定：

- `registerLogMetaTypes()` 注册 `LogLevel`、`LogEvent` 和 `QVector<LogEvent>`。
- `toString(LogLevel)` 输出大写等级字符串。
- `logLevelFromString()` 支持大小写不敏感解析，并把 `WARNING` 兼容为 `Warn`。
- `Off` 只参与过滤，不作为正常事件等级。

---

## 4. `LogService`

`LogService` 继承 `ILogService`，内部状态：

```text
m_recent        最近日志缓存
m_sinks         非拥有 sink 指针列表
m_recentLimit   缓存上限，默认 1000
m_minimumLevel  最小等级，默认 Trace
m_mutex         保护内部状态
```

`append()` 流程：

```text
接收 LogEvent
  -> 空 timestampUs 补当前 UTC epoch 微秒
  -> 空或可解析 level 规范化为大写等级
  -> 按 minimumLevel 过滤
  -> 写入有界 recent 缓存
  -> 复制 sink 列表
  -> 解锁
  -> sink->append(event)
  -> emit logAppended(event)
```

关键约束：

- sink 回调和 signal emit 不在内部状态锁内执行。
- `recent(maxCount)` 返回按追加顺序排列的最近日志。
- `setRecentLimit(0)` 清空并禁用 recent 缓存。
- sink 生命周期由调用方管理；`LogService` 只保存非拥有指针。

---

## 5. 格式化和文件 sink

`LogFormatter` 提供：

- `toJsonObject(LogEvent)`：转换为 `QJsonObject`。
- `toJsonLine(LogEvent)`：紧凑 JSON + 换行。
- `toTextLine(LogEvent)`：人可读文本格式，当前不作为主持久化格式。

`JsonLineFileSink`：

- 使用 `QFile` 追加写入。
- 每条日志一行 JSON。
- `append()` 在文件未打开时尝试自动打开。
- `flush()` 显式刷新到文件。
- 不写 ANSI 颜色，不直接写 stdout。

---

## 6. HAL 桥接

`hal_log_bridge` 位于日志模块内，是 HAL 和主日志模型之间的边界适配。

接口：

```cpp
LogEvent fromHalLogEvent(const hwtest::hal::HalLogEvent& event);

QMetaObject::Connection connectHalLogs(hwtest::hal::IHalService* halService,
                                       ILogService* logService,
                                       Qt::ConnectionType type = Qt::AutoConnection);
```

映射规则：

- `timestampUs`、`level`、`category`、`message` 直接复制。
- `source` 为空时补 `hal`，否则保留原值，例如 `adapter`。
- `requestId`、`durationMs`、`status`、`adapterCode` 写入顶层字段。
- `deviceId`、`resourceId`、`operation` 写入 `context`。
- `requestId`、`durationMs`、`status`、`adapterCode` 也镜像到 `context`。
- 原始 `HalLogEvent.context` 先保留，同名标准字段以顶层字段覆盖。

HAL 仍只生产 `HalLogEvent`，不依赖 `LogService`。

---

## 7. 测试

测试目标：`hwtest_log_tests`。

覆盖点：

- `LogService`：补齐默认值、过滤、recent 缓存、signal、sink 分发。
- `JsonLineFileSink`：JSONL 结构、context 保留、flush。
- `hal_log_bridge`：字段映射和 signal 连接。

运行：

```powershell
cmake --build build_vs --config Debug --target hwtest_log_tests
cmake --build build_vs --config Release --target hwtest_log_tests
ctest --test-dir build_vs -C Debug --output-on-failure
ctest --test-dir build_vs -C Release --output-on-failure
```
