# WebSocket 前端协议契约

## 1. 范围与边界

本文定义 `hwtest_web` 与浏览器前端之间的版本 1 JSON 协议。`hwtest_web` 是与 TUI、Qt Widgets GUI 并列的独立应用进程；它只把 WebSocket 消息适配为 `TestApplicationController` 动作、快照和样本，不解释产品协议，也不直接访问 BIZ、算法、HAL 或 DUT 通讯对象。

`[当前实现]` 服务器使用 Qt WebSockets，仅监听 IPv4 回环地址 `127.0.0.1`，默认端口为 `18765`，唯一资源路径为 `/ws`。它不提供 HTTP、静态文件、TLS、数据库、登录或远程访问。仓库根目录的 `front/` 已提供独立的 React/Vite 遥测控制台；前端既可使用开发服务器，也可使用构建后的单文件 HTML，二者都与 `hwtest_web` 分开运行，浏览器仍只连接回环 WebSocket。

`[当前实现]` 浏览器源码已将数字刺激协议类型、WebSocket transport、SessionProvider 和 `DigitalStimulusPanel` 接入总览页。面板仅在快照声明可用、恰有 16 路且全部 `dutBit` 为 0..15 时显示；它以 32 ms 同开关合并、单飞行串行队列发送动作，显示 active-low 物理电平、`di_state[0]` 回读、`di_state[1]` 诊断和 settling 状态。连续模式参数区另提供“保存全部测量列”复选框，只把布尔选择随 `start` 发送；浏览器不能提交保存路径或字段清单。图表配置优先按 `configId`、缺失时按 `algorithmId` 隔离保存。此浏览器控制面实现不等价于 PXI-6259 真机已连接或已验收。

## 2. 连接规则

- 连接地址为 `ws://127.0.0.1:<port>/ws`。
- 服务器只允许一个活跃客户端。第二个已完成 WebSocket 握手的客户端先收到 `server_busy` 连接级错误，再以关闭码 `1008`（Policy Violation）关闭；当前客户端不受影响。
- 请求路径不是 `/ws` 的连接以 `1008` 关闭，且不成为活跃客户端。
- Origin 为空或为浏览器从 `file://` 单文件页面发送的 opaque 值 `null` 时允许连接。其他非空 Origin 必须是有效的 `http` 或 `https` URL，主机名必须为 `localhost` 或 `127.0.0.1`；比较主机名时不区分大小写。其他 Origin 在握手阶段被拒绝，因此没有 WebSocket 关闭帧。
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
- `setDigitalStimulus`、`resetDigitalStimulus`、`start.saveData` 和快照中的数据保存状态是版本 1 的追加式扩展；旧客户端可忽略新增快照字段，未识别动作不能自行推断为可用。

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

`ok`、`code` 和 `message` 直接投影 `ActionResult`。成功时 `code`、`message` 通常为空；控制器失败时错误码和消息原样返回。协议层错误使用第 5 节的固定错误码。`data` 总是对象；没有附加数据时为空对象。`setDigitalStimulus` 和 `resetDigitalStimulus` 的 reply 无论成功或控制器失败都附带 `data.digitalStimulus`，其形状与快照字段相同。

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
| `dataSaveEnabled` | boolean；当前任务是否启用了后端连续数据保存 |
| `dataFilePath`、`dataSaveError` | string；后端目标/完成文件路径与保存错误，未启用或无错误时为空 |
| `digitalStimulus` | object；当前数字刺激状态，字段见下表 |

`rawData` 使用 Qt 的 JSON-compatible QVariant 转换规则；其嵌套 map/list、布尔值、数值、字符串和空值保持对应 JSON 类型。

`descriptor` 是后端从当前已验证测试配置投影出的展示元数据，前端不得读取或解析原始配置文件、协议 CSV 或 `executionConfig`：

| JSON 字段 | 类型 | 语义 |
| --- | --- | --- |
| `configId`、`productModel`、`productName`、`configVersion` | string | 当前测试配置身份 |
| `stepId`、`testItemId`、`algorithmId` | string | 当前唯一启用步骤身份 |
| `title`、`description` | string | 当前测试的展示名称和说明 |
| `supportedRunModes` | string[] | 当前配置声明的运行模式；元素只能是 `single`、`pc_periodic` 或 `device_stream`，且不得同时包含 `pc_periodic` 与 `device_stream`；字段缺失时后端只投影 `single` |
| `measurements` | object[] | 待测量元数据；每项包含 `id`、`label`、`unit`、`primary` |

`measurements` 只描述展示标签、单位和首页主指标候选，不改变算法判定或硬件安全语义。首条样本可能包含 descriptor 未列出的数值字段，前端仍应自动发现并显示该字段；descriptor 缺失时，兼容客户端可以使用空 descriptor 和样本字段回退。

`digitalStimulus` 是受 WebSocket v1 数值边界约束的应用 DTO 公开投影，不暴露 `resourceId`、设备 alias、`adapterId`、端口、厂家设置或 DLL 路径：

WebSocket v1 的刺激通道边界固定为最多 16 路且 `dutBit` 只能是 0..15。控制器/算法可在其他前端保留 64 路内部能力；若当前快照包含高位通道、高位 mask 或非 JavaScript 安全整数 revision，Web 投影必须返回 `available=false`、空 switches、零 mask/revision 和 `CapabilityUnsupported`，刺激动作回复 `capability_unsupported`，不得把 `quint64` 高位转换为 JSON `double`。

| JSON 字段 | 类型 | 语义 |
| --- | --- | --- |
| `available`、`configured` | boolean | 已加载配置声明刺激 / 已完成 DI 准备和控制器配置 |
| `switches` | object[] | 每项只含 `switchId`、`dutBit`、`label`、`activeLevel`（`High` 或 `Low`） |
| `appliedMask`、`revision` | number | WebSocket v1 只承载低 16 位逻辑激活位图和 JavaScript 安全整数 revision；写入成功后 revision 递增 |
| `lastWriteTimestampUs`、`settlingMs` | number | 最近一次成功写入的 UTC epoch 微秒与配置的稳定等待毫秒 |
| `errorCode`、`message` | string | 最近一次刺激操作的 HAL 归一化错误或诊断；成功时为空 |

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
| `test_config_not_found` | `selectTest.configId` 不在后端启动时发现的配置白名单中 | 是 |
| `invalid_run_mode` | `start.mode` 不是固定三种模式之一 | 是 |
| `CapabilityUnsupported` | `start.mode` 未在当前配置的 `supportedRunModes` 中声明，或刺激快照超出 WebSocket v1 的 16 位投影 | 是 |
| `capability_unsupported` | `setDigitalStimulus`/`resetDigitalStimulus` 面对超出 v1 范围的刺激配置 | 是 |
| `ParameterRangeError` | `start.intervalMs` 或 `start.maxCycles` 超出范围 | 是 |

能够安全读取请求 `id` 时，协议错误 reply 使用该 id；否则使用空字符串。错误输入不得触发控制器动作。

数字刺激参数的类型、缺失或未知字段都属于 `invalid_envelope`，不会进入控制器。通过协议校验后，DI 未准备时的 `invalid_state`、未知配置开关的 `NotFound`、陈旧 revision 的 `DataMismatch` 等是控制器/HAL 归一化 `ActionResult`，不是新增的 WebSocket 协议错误码；协议中没有 `revision_conflict` 码。

## 6. 动作

| `action` | `params` | 行为及 reply `data` |
| --- | --- | --- |
| `load` | `{}` | 使用进程启动时解析好的 `FrontendLaunchOptions` 调用 `configureController`；客户端不能提供或覆盖文件路径 |
| `testConfigs` | `{}` | 返回后端固定 `configs/` 目录中已验证的测试配置摘要；`data` 为 `{ "selectedConfigId": "...", "configs": [{"configId":"...","title":"...","description":"...","algorithmId":"..."}] }`，不返回本地路径 |
| `selectTest` | `{"configId":"mbddf-elec-health"}` | 只接受 `testConfigs` 返回的白名单 `configId`；服务器映射到本地路径并重新加载配置，若当前处于可运行终态则先安全关闭当前会话；运行中或停止中拒绝切换 |
| `snapshot` | `{}` | 从 Web 层缓存读取；`data` 为 `{ "seq": n, "snapshot": { ... } }`，其中包含当前配置 descriptor |
| `controls` | `{}` | 在控制器亲和线程读取；`data.controls` 为 `{resourceId, providerId}` 数组 |
| `ports` | `{}` | 在控制器亲和线程读取；`data.ports` 为完整 `SerialPortInfo` 对象数组 |
| `selectControl` | `{"resourceId":"CONTROL_SERIAL"}` | 调用 `selectControl`；`resourceId` 必须是非空字符串 |
| `selectSerialPort` | `{"portName":"COM7"}` | 调用 `selectSerialPort`；`portName` 必须是非空字符串 |
| `prepare` | `{}` | 调用 `prepare` |
| `start` | `{}` 或 `{"mode":"pc_periodic","intervalMs":500,"maxCycles":0,"saveData":true}` | 调用 `start(TestRunOptions)`；空对象保持单次兼容。`mode` 只允许 `single`、`pc_periodic`、`device_stream`，且必须由当前 descriptor 声明；`intervalMs` 为 `10..3600000` 的整数，`maxCycles` 为 `0..1000000000` 的整数且 `0` 表示 PC 周期不限轮数，两者只有 `pc_periodic` 具备调度含义，其他模式由控制器归一为 `1000/1`；`saveData` 只能是 boolean，`pc_periodic` 与 `device_stream` 均可启用保存，`single` 强制不保存 |
| `pause` | `{}` | 调用 `pause` |
| `resume` | `{}` | 调用 `resume` |
| `setDigitalStimulus` | `{"switchId":"di0","active":true,"expectedRevision":0}` | 必须且只能包含这三个字段；`switchId` 为非空 string，`active` 为 boolean，`expectedRevision` 为 0..9007199254740991 的非负安全整数。未知字段（包括 `resourceId`、`adapterId`、端口或路径）一律 `invalid_envelope`；控制器再按已加载配置白名单验证 `switchId`。reply 的 `data.digitalStimulus` 返回当前状态 |
| `resetDigitalStimulus` | `{}` | 只能使用空对象；任何参数均为 `invalid_envelope`。调用控制器复位，reply 的 `data.digitalStimulus` 返回当前状态 |
| `stop` | `{}` | 调用 `stopAsync`；发起成功时只在 `stopCompleted` 后发送 reply，初始调用失败时立即回复且不等待不存在的完成信号 |
| `disconnect` | `{}` | 必要时先异步停止，再调用 `shutdown`；最后回复并关闭当前连接，服务器继续监听 |
| `quit` | `{}` | 执行与 `disconnect` 相同的安全收尾，随后关闭服务器并退出进程 |

除 `start`、`selectTest`、`selectControl`、`selectSerialPort` 和 `setDigitalStimulus` 外，无参数动作不得从 `params` 读取行为配置。`load` 尤其不得读取 `testConfigPath`、`halConfigPath` 或其他客户端路径字段；`selectTest` 只读取白名单标识，不读取客户端路径。`start` 与 `setDigitalStimulus` 都只接受上表列出的字段，未知字段按 `invalid_envelope` 拒绝。`start` 不接受客户端保存目录、文件名或测量字段；这些内容不能借 `saveData` 绕过后端配置和 descriptor 白名单。

### 6.1 连续模式数据文件

`[当前实现]` `saveData=true` 且运行模式为 `pc_periodic` 或 `device_stream` 时，`TestApplicationController` 在后端记录配置 descriptor 定义的全部测量列，而不是任意 `ApplicationSample.values` 字段或浏览器当前勾选的曲线字段。`pc_periodic` 记录 PC 每轮单发单回产生的样本；`device_stream` 记录一次启动后设备持续回告产生的全部样本，应用层不会为它重复发起 PC 请求。默认 HAL/应用组合配置为：

```json
{"dataStorage":{"directory":"../data"}}
```

绝对目录直接使用；相对目录按 HAL 配置文件所在目录解析。字段缺失时同样回退 `../data`。客户端无权覆盖目录。目录或初始文件不能创建时，`start` 返回控制器错误 `data_storage`；运行中写入或终态提交失败时，完整快照的 `dataSaveError` 返回诊断。

最终文件固定为带 UTF-8 BOM 的 `.txt`：顶部是一行标题和 `# key=value` 元数据，至少包含 `started_at`、`finished_at`、`final_status`、`final_detail`、`sample_count`、`run_mode`、`repeat_delay_ms`、`config_id`、`algorithm_id` 与 `max_cycles`；`device_stream` 的两个 PC 周期字段固定写 `NA`。空行之后是制表符分隔、换行符为 LF 的固定表头和数据行。文件名使用后端开始时间；电气健康保持 PyQt 参考名 `ElectricalHealth_data_YYYYMMDD_HHMMSS_ffffff.txt`。

电气健康表头保持参考实现的固定顺序：

```text
report_index  sample_time_us  seq  response_status  err_code  c_volt_V  b_volt_V  external_vol_V  core_vol_V  assist_vol_V  v28_5_V  js_5V_V  dyt_5V_V  power_24V_V  value_YX_V  activate_bits  bc_activate_good
```

实际分隔符是 TAB。其他连续模式配置使用同样的前五列，后续列由已验证 descriptor 的全部 `measurements` 固定投影，并排除已在前缀中的 `status`/`err_code`；单位非空时写入列名后缀。前端图表的显示/隐藏、组合、时间窗和降采样均不改变该列集合。响应 `status != 0` 或 `err_code != 0` 时仍保存状态和错误码，测量列写 `NA`。

运行期数据逐样本刷新到同目录的 `.partial` 文件，避免不限 PC 周期或设备持续回告在内存累积；正常完成、用户停止或错误终态时，后端使用 `QSaveFile` 把元数据和 TSV 原子提交为最终 TXT，再删除 `.partial`。提交失败时保留 `.partial` 供恢复。未勾选或 `single` 均不创建数据文件。

## 7. 异步、线程与安全收尾

- WebSocket 回调不得直接跨线程调用控制器。所有控制器动作和读取都通过 queued invocation 投递到控制器的 QObject 亲和线程；禁止 `BlockingQueuedConnection`。
- Web 层不得调用 `waitForTerminal()`。运行进度和终态只通过 `snapshotChanged` 观察。
- `sampleReceived` 直接形成 sample 事件；Web 层不解释、聚合或绘制字段，也不为连续测试建立定时器或写文件。连续数据文件由共享应用控制器在形成应用 DTO 后记录，TUI/GUI/WebSocket 适配器都不持有 recorder。
- `setDigitalStimulus` 和 `resetDigitalStimulus` 只在控制器处于 `ready`、`running`、`paused`、`finished` 或 `stopped`，且 DI 已准备时才会执行；Web 层不持有或传递物理资源/Adapter 参数。
- `stop` 保存请求 id，调用 `stopAsync()` 后保持事件循环运行；发起成功后收到 `stopCompleted` 才回复。若 `stopAsync()` 因状态、超时参数或已有停止而立即失败，则直接返回该控制器错误并清除 Web 层 pending 状态。
- 异步停止或断开收尾期间，`snapshot`、`controls` 和 `ports` 三个只读动作仍允许；其他新动作回复 `command_in_progress`，不得再次触发控制器写动作。
- `disconnect`、`quit` 和异常掉线在状态为 `running` 或 `paused` 时按 `stopAsync -> stopCompleted -> shutdown` 顺序执行；其他状态直接尝试 `shutdown`。DI 配置的停止/收尾会在 BIZ 停止后尽力 `resetDigitalStimulus`，随后按刺激设备会话、DUT 会话、HAL 的顺序释放；这不是进程崩溃、主机掉电或未验证台架的物理安全保证。
- `shutdown` 的失败必须通过对应 reply 返回；异常掉线时没有 reply，但服务器仍清理会话并恢复到可接纳下一客户端的状态。
- 服务器停止监听或客户端对象销毁，不得先于已经排队的安全收尾。

## 8. 消息顺序保证

1. 活跃连接建立后依次发送 `hello`、当前完整 `snapshot`。
2. 同一事件循环队列中的普通请求按接收顺序投递；每个请求最多一个 reply。
3. `snapshotChanged` 和 `sampleReceived` 分别立即形成完整 snapshot/sample 消息，因此它们可以先于触发该变化的动作 reply 到达；`setDigitalStimulus`/`resetDigitalStimulus` 的 reply 也携带当时的 `data.digitalStimulus`。客户端必须按 `type` 分流，不能假定 reply 先于相应 snapshot，且应以序号更高的后续完整 snapshot 更新状态。
4. `stop`、`disconnect`、`quit` 的 reply 必须晚于 `stopCompleted`（若需要停止）和 `shutdown`（若需要收尾）。
5. `disconnect`/`quit` 的最终 reply 必须先于正常关闭帧；`quit` 的关闭帧必须先于进程退出。
