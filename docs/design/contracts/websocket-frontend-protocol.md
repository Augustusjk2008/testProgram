# WebSocket 前端协议契约

## 1. 范围与边界

本文定义 `hwtest_web` 与浏览器前端之间的版本 1 JSON 协议。`hwtest_web` 是与 TUI、Qt Widgets GUI 并列的独立应用进程；它只把 WebSocket 消息适配为 `TestApplicationController` 动作、快照和样本，不解释产品协议，也不直接访问 BIZ、算法、HAL 或 DUT 通讯对象。

`[当前实现]` 服务器使用 Qt WebSockets，仅监听 IPv4 回环地址 `127.0.0.1`，默认端口为 `18765`，唯一资源路径为 `/ws`。它不提供 HTTP、静态文件、TLS、数据库、登录或远程访问。仓库根目录的 `front/` 已提供独立的 React/Vite 遥测控制台；开发服务器或静态文件服务与 `hwtest_web` 分开启动，浏览器仍只连接回环 WebSocket。

## 2. 连接规则

- 连接地址为 `ws://127.0.0.1:<port>/ws`。
- 服务器只允许一个活跃客户端。第二个已完成 WebSocket 握手的客户端先收到 `server_busy` 连接级错误，再以关闭码 `1008`（Policy Violation）关闭；当前客户端不受影响。
- 请求路径不是 `/ws` 的连接以 `1008` 关闭，且不成为活跃客户端。
- Origin 为空时允许连接。非空 Origin 必须是有效的 `http` 或 `https` URL，主机名必须为 `localhost` 或 `127.0.0.1`；比较主机名时不区分大小写。其他 Origin 在握手阶段被拒绝，因此没有 WebSocket 关闭帧。
- 单条文本消息以其 UTF-8 编码长度计不得超过 `16384` 字节。超限时服务器发送 `message_too_large` 连接级错误，并以 `1009`（Message Too Big）关闭。
- 二进制消息不属于本协议。服务器发送 `invalid_envelope` 连接级错误，并以 `1003`（Unsupported Data）关闭。
- 客户端执行 `disconnect` 后，服务器完成安全收尾并以 `1000`（Normal Closure）关闭该连接，随后继续监听新客户端。
- 客户端执行 `quit` 后，服务器完成同样的安全收尾、回复请求、以 `1000` 关闭连接，然后停止监听并排队退出进程。
- 浏览器异常掉线时无法发送 reply；服务器仍须执行第 7 节的安全收尾，完成后继续监听。

连接级错误无法对应某个合法请求时，使用普通 reply 结构，但 `id` 固定为空字符串。

## 3. 通用编码

- 客户端只发送 UTF-8 JSON 文本消息；JSON 顶层必须是对象。
- 所有消息都包含整数 `v`，本契约固定为 `1`。
- 字段名区分大小写。协议未定义的顶层字段应被忽略，以便尾部扩展；已定义字段的类型必须严格符合本文。
- 服务器发送紧凑 JSON，不依赖空白或对象成员顺序。
- 每个有效请求恰好产生一个相同 `id` 的 reply。快照和样本是独立异步事件，可以出现在请求与 reply 之间。

## 4. 消息结构

### 4.1 请求

```json
{"v":1,"type":"request","id":"req-1","action":"start","params":{}}
```

字段约束：

| 字段 | 类型 | 约束 |
| --- | --- | --- |
| `v` | integer | 必须为 `1` |
| `type` | string | 必须为 `request` |
| `id` | string | 去除首尾空白后非空；服务器原样用于关联 reply |
| `action` | string | 必须是第 6 节列出的动作之一 |
| `params` | object | 必须存在；无参数动作使用空对象 `{}` |

### 4.2 回复

```json
{"v":1,"type":"reply","id":"req-1","ok":true,"code":"","message":"","data":{}}
```

`ok`、`code` 和 `message` 直接投影 `ActionResult`。成功时 `code`、`message` 通常为空；控制器失败时错误码和消息原样返回。协议层错误使用第 5 节的固定错误码。`data` 总是对象；没有附加数据时为空对象。

### 4.3 握手问候

```json
{"v":1,"type":"hello","server":"hwtest_web","protocolVersion":1}
```

连接成为活跃客户端后，服务器首先发送 `hello`，随后发送当前完整快照。客户端不应把 `hello` 当作请求 reply。

### 4.4 快照事件

```json
{"v":1,"type":"snapshot","seq":12,"snapshot":{"phase":"running","progress":25}}
```

`seq` 是服务器进程生命周期内单调递增的非负整数。服务器启动时缓存控制器当前快照且 `seq` 为 `0`；每收到一次 `snapshotChanged` 就先递增 `seq`，再广播完整快照。新客户端收到的是当前 `seq` 和完整缓存，而不是增量补丁。

`snapshot` 必须包含 `ApplicationSnapshot` 的全部公共字段：

| JSON 字段 | 类型 |
| --- | --- |
| `phase`、`testState`、`controlResourceId`、`providerId`、`serialPortName` | string |
| `taskId`、`stepId`、`testItemId`、`algorithmId` | string |
| `progress` | integer |
| `progressStep` | string |
| `hasResult` | boolean |
| `verdict`、`errorCode`、`message` | string |
| `attempts` | integer |
| `rawData` | object |
| `runMode` | string；`single`、`pc_periodic` 或 `device_stream` |
| `intervalMs` | integer |
| `maxCycles`、`cycleIndex`、`sampleCount` | non-negative integer |

`rawData` 使用 Qt 的 JSON-compatible QVariant 转换规则；其嵌套 map/list、布尔值、数值、字符串和空值保持对应 JSON 类型。

### 4.5 样本事件

```json
{"v":1,"type":"sample","seq":42,"sample":{"taskId":"...","stepId":"SYSTEM_STATUS","channelId":"SYSTEM_STATUS","timestampUs":1785000000123456,"cycleIndex":7,"values":{"cpu_usage":12.5,"cpu_temp":44.2},"tags":{}}}
```

`sample.seq` 所在的顶层 `seq` 是样本事件在服务器进程生命周期内的独立单调递增序号；它不与快照 `seq` 共用计数器。`timestampUs` 是 UTC epoch 微秒，当前数值范围可由 JavaScript `Number` 精确表达。`values` 和 `tags` 使用与 `rawData` 相同的 JSON-compatible QVariant 转换规则。服务器不缓存或重放历史样本；新客户端从连接后产生的样本开始接收。

## 5. 协议层错误码

| `code` | 条件 | 连接是否保持 |
| --- | --- | --- |
| `invalid_json` | 文本不是合法 JSON 对象 | 是 |
| `invalid_envelope` | 消息类型或已定义字段类型不合法，或收到二进制消息 | 一般保持；二进制消息关闭 |
| `unsupported_version` | `v` 不是整数 `1` | 是 |
| `unknown_action` | `action` 不在固定动作集合中 | 是 |
| `missing_field` | 必填字段缺失、字符串为空，或动作必需参数缺失 | 是 |
| `server_busy` | 已有活跃客户端 | 否，关闭码 `1008` |
| `message_too_large` | UTF-8 文本超过 `16384` 字节 | 否，关闭码 `1009` |
| `command_in_progress` | 正在异步停止或安全收尾时又收到写动作 | 是 |
| `invalid_run_mode` | `start.mode` 不是固定三种模式之一 | 是 |
| `ParameterRangeError` | `start.intervalMs` 或 `start.maxCycles` 超出范围 | 是 |

能够安全读取请求 `id` 时，协议错误 reply 使用该 id；否则使用空字符串。错误输入不得触发控制器动作。

## 6. 动作

| `action` | `params` | 行为及 reply `data` |
| --- | --- | --- |
| `load` | `{}` | 使用进程启动时解析好的 `FrontendLaunchOptions` 调用 `configureController`；客户端不能提供或覆盖文件路径 |
| `snapshot` | `{}` | 从 Web 层缓存读取；`data` 为 `{ "seq": n, "snapshot": { ... } }` |
| `controls` | `{}` | 在控制器亲和线程读取；`data.controls` 为 `{resourceId, providerId}` 数组 |
| `ports` | `{}` | 在控制器亲和线程读取；`data.ports` 为完整 `SerialPortInfo` 对象数组 |
| `selectControl` | `{"resourceId":"CONTROL_SERIAL"}` | 调用 `selectControl`；`resourceId` 必须是非空字符串 |
| `selectSerialPort` | `{"portName":"COM7"}` | 调用 `selectSerialPort`；`portName` 必须是非空字符串 |
| `prepare` | `{}` | 调用 `prepare` |
| `start` | `{}` 或 `{"mode":"pc_periodic","intervalMs":500,"maxCycles":0}` | 调用 `start(TestRunOptions)`；空对象保持单次兼容。`mode` 只允许 `single`、`pc_periodic`、`device_stream`；`intervalMs` 为 `10..3600000` 的整数；`maxCycles` 为 `0..1000000000` 的整数，`0` 表示 PC 周期不限轮数 |
| `pause` | `{}` | 调用 `pause` |
| `resume` | `{}` | 调用 `resume` |
| `stop` | `{}` | 调用 `stopAsync`；发起成功时只在 `stopCompleted` 后发送 reply，初始调用失败时立即回复且不等待不存在的完成信号 |
| `disconnect` | `{}` | 必要时先异步停止，再调用 `shutdown`；最后回复并关闭当前连接，服务器继续监听 |
| `quit` | `{}` | 执行与 `disconnect` 相同的安全收尾，随后关闭服务器并退出进程 |

除 `start`、`selectControl` 和 `selectSerialPort` 外，无参数动作不得从 `params` 读取行为配置。`load` 尤其不得读取 `testConfigPath`、`halConfigPath` 或其他客户端路径字段。`start` 只接受上表三个可选字段，未知字段按 `invalid_envelope` 拒绝。

## 7. 异步、线程与安全收尾

- WebSocket 回调不得直接跨线程调用控制器。所有控制器动作和读取都通过 queued invocation 投递到控制器的 QObject 亲和线程；禁止 `BlockingQueuedConnection`。
- Web 层不得调用 `waitForTerminal()`。运行进度和终态只通过 `snapshotChanged` 观察。
- `sampleReceived` 直接形成 sample 事件；Web 层不解释、聚合或绘制字段，也不为连续测试建立定时器。
- `stop` 保存请求 id，调用 `stopAsync()` 后保持事件循环运行；发起成功后收到 `stopCompleted` 才回复。若 `stopAsync()` 因状态、超时参数或已有停止而立即失败，则直接返回该控制器错误并清除 Web 层 pending 状态。
- 异步停止或断开收尾期间，`snapshot`、`controls` 和 `ports` 三个只读动作仍允许；其他新动作回复 `command_in_progress`，不得再次触发控制器写动作。
- `disconnect`、`quit` 和异常掉线在状态为 `running` 或 `paused` 时按 `stopAsync -> stopCompleted -> shutdown` 顺序执行；其他状态直接尝试 `shutdown`。
- `shutdown` 的失败必须通过对应 reply 返回；异常掉线时没有 reply，但服务器仍清理会话并恢复到可接纳下一客户端的状态。
- 服务器停止监听或客户端对象销毁，不得先于已经排队的安全收尾。

## 8. 消息顺序保证

1. 活跃连接建立后依次发送 `hello`、当前完整 `snapshot`。
2. 同一事件循环队列中的普通请求按接收顺序投递；每个请求最多一个 reply。
3. `snapshotChanged` 和 `sampleReceived` 分别立即形成完整 snapshot/sample 消息，因此它们可以先于触发该变化的动作 reply 到达；客户端必须按 `type` 分流，不能假定 start reply 是运行后的第一条消息。
4. `stop`、`disconnect`、`quit` 的 reply 必须晚于 `stopCompleted`（若需要停止）和 `shutdown`（若需要收尾）。
5. `disconnect`/`quit` 的最终 reply 必须先于正常关闭帧；`quit` 的关闭帧必须先于进程退出。
