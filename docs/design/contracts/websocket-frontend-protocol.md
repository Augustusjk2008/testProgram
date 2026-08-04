# WebSocket 前端协议契约

## 1. 范围与边界

本文定义 `hwtest_web` 与浏览器前端之间的版本 1 JSON 协议。`hwtest_web` 是与 TUI、Qt Widgets GUI 并列的独立应用进程；它只把 WebSocket 消息适配为 `TestApplicationController` 动作、快照和样本，不解释产品协议，也不直接访问 BIZ、算法、HAL 或 DUT 通讯对象。

`[当前实现]` 服务器使用 Qt WebSockets，仅监听 IPv4 回环地址 `127.0.0.1`，默认端口为 `18765`，唯一资源路径为 `/ws`。它不提供 HTTP、静态文件、TLS、数据库、登录或远程访问。仓库根目录的 `front/` 已提供独立的 React/Vite 遥测控制台；前端既可使用开发服务器，也可使用构建后的单文件 HTML，二者都与 `hwtest_web` 分开运行，浏览器仍只连接回环 WebSocket。

`[当前实现]` 浏览器按 descriptor 动态投影配置、运行模式、算法参数、数字刺激、板级结果、连续遥测和舵机后处理；只通过本协议调用应用控制器，不解释产品帧或直接访问硬件。页面功能与操作说明见 [`front/README.md`](../../../front/README.md)，产品流程和物理证据边界分别见[设备通讯契约](device-communication-protocol.md)与[HAL 契约](hal-interface-protocol.md)，本文不复制页面清单和硬件参数。

`[当前实现]` 服务器投影后处理 capability/摘要并接受只读 `analysisResult` 请求；应用控制器已接通 STOP 尾样本封存、硬件收尾双栅栏、`queued` 到终态的后台分析、分析期间写门禁及运行中 worker 协作取消。算法层提供五种波形与扫频伯德计算端口。`capturing` 只表示输入正在采集，只有带匹配 `{taskId, analysisGeneration}` 的分析终态才表示该轮结果可查询。

## 2. 连接规则

- 连接地址为 `ws://127.0.0.1:<port>/ws`。
- 服务器只允许一个活跃客户端。第二个已完成 WebSocket 握手的客户端先收到 `server_busy` 连接级错误，再以关闭码 `1008`（Policy Violation）关闭；当前客户端不受影响。
- 请求路径不是 `/ws` 的连接以 `1008` 关闭，且不成为活跃客户端。
- Origin 为空或为浏览器从 `file://` 单文件页面发送的 opaque 值 `null` 时允许连接。其他非空 Origin 必须是有效的 `http` 或 `https` URL，主机名必须为 `localhost` 或 `127.0.0.1`；比较主机名时不区分大小写。其他 Origin 在握手阶段被拒绝，因此没有 WebSocket 关闭帧。
- 单条文本消息以其 UTF-8 编码长度计不得超过 `16384` 字节。超限时服务器发送 `message_too_large` 连接级错误，并以 `1009`（Message Too Big）关闭。
- 二进制消息不属于本协议。服务器发送 `invalid_envelope` 连接级错误，并以 `1003`（Unsupported Data）关闭。
- 客户端执行 `disconnect` 后，服务器通常完成安全收尾并以 `1000`（Normal Closure）关闭该连接，随后继续监听新客户端。不可停止有限流活动时是显式例外：回复成功后仅关闭并分离该客户端，不停止或 shutdown 当前任务；新客户端可重连观察当前快照和后续样本。
- 客户端执行 `quit` 后，服务器通常完成同样的安全收尾、回复请求、以 `1000` 关闭连接，然后停止监听并排队退出进程。不可停止有限流活动时 `quit` 返回 `invalid_state`，连接、监听和任务均保持。
- 浏览器异常掉线时无法发送 reply；服务器通常执行第 7 节的安全收尾，完成后继续监听。不可停止有限流活动时只分离客户端，不调用 stop/shutdown，任务自然完成后仍可接纳重连。
- “数字量输出”的已批准语义是显式例外：完成、停止、`disconnect`、`quit` 或异常掉线收尾均不发送额外 DUT `DO_WRITE` 复位帧，产品保持最后一次已应用输出；关闭 PXI/HAL 会话不得被描述为已经复位 DUT DO。

连接级错误无法对应某个合法请求时，使用普通 reply 结构，但 `id` 固定为空字符串。

## 3. 通用编码

- 客户端只发送 UTF-8 JSON 文本消息；JSON 顶层必须是对象。
- 所有消息都包含整数 `v`，本契约固定为 `1`。
- 字段名区分大小写。协议未定义的顶层字段应被忽略，以便尾部扩展；已定义字段的类型必须严格符合本文。
- 服务器发送紧凑 JSON，不依赖空白或对象成员顺序。
- 每个有效请求恰好产生一个相同 `id` 的 reply。快照和样本是独立异步事件，可以出现在请求与 reply 之间。
- 所有对浏览器可见的事件序号必须是 `0..9007199254740991` 内的整数。服务端不得把超出 JavaScript 安全整数范围的 `snapshot.seq`、`sample.seq`、`sampleBatch.firstSeq` 或 `sampleBatch.lastSeq` 转成 JSON number 后发送。
- `selectAuxiliarySerialPort`、`snapshot.auxiliarySerialPortName`、`setDigitalStimulus`、`resetDigitalStimulus`、`start.saveData`、`start.dataDirectory`、`start.dataFileName`、`setTelemetryDelivery`、descriptor 中的 `stoppable`/`postRunAnalysis`/`runParameters[].persistValues`、快照 `rawData.boardTest`、快照中的 `analysis` 和只读 `analysisResult` 都是版本 1 的追加式扩展；旧客户端可忽略新增快照字段，未识别动作不能自行推断为可用。旧客户端省略两个保存目标字段时继续使用 6.1 节默认规则；不认识它们的旧服务端会按严格参数白名单返回 `invalid_envelope`，客户端不得删除用户指定的目标后静默重试。新客户端收到旧服务端缺失的辅助串口字段时回退为空字符串，缺失 `stoppable` 时兼容回退为 true；缺失后处理字段时安全回退为 `supported=false`、`state=none`、`analysisGeneration=0` 和空摘要。显式 `stoppable` 与 `persistValues` 必须为 boolean，其他类型是协议边界错误。

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
{"v":1,"type":"hello","server":"hwtest_web","protocolVersion":1,"capabilities":{"telemetryBatch":{"version":1,"maxSamples":64,"maxBytes":32768,"maxLatencyMs":20,"snapshotIntervalMs":100}}}
```

连接成为活跃客户端后，服务器首先发送 `hello`，随后发送当前完整快照。客户端不应把 `hello` 当作请求 reply。

`capabilities.telemetryBatch` 是协商式批量遥测能力；字段中的数值是该连接实际可用的上限。默认值为 `version=1`、`maxSamples=64`、`maxBytes=32768`、`maxLatencyMs=20`、`snapshotIntervalMs=100`。缺失该 capability 的旧服务端只支持旧的单条 `sample` 投影；客户端必须保持 `single`，不得仅因协议版本为 1 推断批量能力。

### 4.4 快照事件

```json
{"v":1,"type":"snapshot","seq":12,"snapshot":{"phase":"running","progress":25}}
```

`seq` 是服务器进程生命周期内单调递增、位于 JavaScript 安全整数范围内的非负整数。服务器启动时缓存控制器当前快照且 `seq` 为 `0`；每收到一次 `snapshotChanged` 都先递增源序号，再更新完整缓存。新客户端收到的是当前 `seq` 和完整缓存，而不是增量补丁。

在默认 `single` 投递模式，服务器逐次投影快照，保持既有行为。在已协商的 `batch` 模式，普通 `phase="running"` 快照可只保留最新值并最多每 `snapshotIntervalMs` 投影一次；因此客户端看到的快照 `seq` 可以跳号，跳号只表示状态投影合并，不能推断样本丢失。首个运行快照、暂停/恢复、`errorCode`、`dataSaveError` 或数字刺激安全状态变化、分析状态变化、以及 `finished`、`stopped`、`error` 等非运行终态均不得合并或延迟。

`snapshot` 必须包含 `ApplicationSnapshot` 的全部公共字段：

| JSON 字段 | 类型 |
| --- | --- |
| `phase`、`testState`、`controlResourceId`、`providerId`、`serialPortName`、`auxiliarySerialPortName` | string；后者是统一串口回显当前选择的独立 PC 本地串口，未选择或旧服务端时为空 |
| `taskId`、`stepId`、`testItemId`、`algorithmId` | string |
| `progress` | integer |
| `progressStep` | string |
| `hasResult` | boolean |
| `verdict`、`errorCode`、`message` | string |
| `attempts` | integer |
| `rawData` | object |
| `effectiveRunParameters` | object；本次启动经算法 Schema 合并和校验后的完整运行参数，未启动或当前算法无 Schema 时为空 |
| `runMode` | string；`single`、`pc_periodic` 或 `device_stream` |
| `intervalMs` | integer |
| `maxCycles`、`cycleIndex`、`sampleCount` | non-negative integer |
| `dataSaveEnabled` | boolean；当前任务是否启用了后端连续数据保存 |
| `dataFilePath` | string；服务端归一化后的绝对路径。未启用保存时为空；显式文件名在运行中投影当前候选最终目标，默认名因结束时间未知而保持为空；终态保存成功后是已原子占位并提交的最终 TXT，失败且存在可恢复工作文件时是 `.partial` |
| `dataSaveError` | string；保存错误，未启用或无错误时为空 |
| `digitalStimulus` | object；当前数字刺激状态，字段见下表 |
| `analysis` | object；后处理 capability、当前分析身份、进度和通道摘要，字段与可用性见 4.4.1；不含完整伯德数组 |

`rawData` 使用 Qt 的 JSON-compatible QVariant 转换规则；其嵌套 map/list、布尔值、数值、字符串和空值保持对应 JSON 类型。`rawData.boardTest` 存在时遵循 4.4.2 的版本化结果投影；它不是 `analysisResult` 或连续数据文件。

`descriptor` 是后端从当前已验证测试配置投影出的展示元数据，前端不得读取或解析原始配置文件、协议 CSV 或 `executionConfig`：

| JSON 字段 | 类型 | 语义 |
| --- | --- | --- |
| `configId`、`productModel`、`productName`、`configVersion` | string | 当前测试配置身份 |
| `stepId`、`testItemId`、`algorithmId` | string | 当前唯一启用步骤身份 |
| `title`、`description` | string | 当前测试的展示名称和说明 |
| `supportedRunModes` | string[] | 当前配置声明的运行模式；元素只能是 `single`、`pc_periodic` 或 `device_stream`，且不得同时包含 `pc_periodic` 与 `device_stream`；字段缺失时后端只投影 `single` |
| `stoppable` | boolean | 当前测试在活动态是否允许 pause/resume/stop/shutdown；配置缺失时为 true，false 只允许恰好支持 `device_stream` 的配置，当前用于 DH 点火有限流 |
| `measurements` | object[] | 待测量元数据；每项包含 `id`、`label`、`unit`、`primary` |
| `runParameterSchemaVersion` | string | 算法层运行参数 Schema 版本；无可编辑参数时为空 |
| `runParameters` | object[] | 参数定义；每项包含 `id`、`label`、`description`、`kind`、`unit`、`required`、独占边界标志和 `choices`，可选 `minimum`、`maximum`、`visibleWhen`、`persistValues`。后者缺失时按 true 兼容；显式 false 时浏览器不得读写该项 localStorage，但仍发送本次有效参数 |
| `runParameterDefaults` | object | 当前配置值覆盖算法 Schema 默认值后得到的完整默认参数 |
| `postRunAnalysis` | object | `{supported, analyzerId, schemaVersion}`；仅当前测试支持后处理时 `supported=true`，旧服务端/非支持配置安全回退为 false 和空字符串 |

`measurements` 只描述展示标签、单位和首页主指标候选，不改变算法判定或硬件安全语义。`runParameters` 由算法层定义语义，`kind` 只能是 `integer`、`number`、`boolean` 或 `choice`；`visibleWhen` 只控制界面显示，隐藏字段仍按 Schema 校验并随完整参数传递。首条样本可能包含 descriptor 未列出的数值字段，前端仍应自动发现并显示该字段；descriptor 缺失时，兼容客户端可以使用空 descriptor 和样本字段回退。

`digitalStimulus` 是受 WebSocket v1 数值边界约束的应用 DTO 公开投影，不暴露 `resourceId`、设备 alias、`adapterId`、端口、厂家设置或 DLL 路径：

WebSocket v1 的刺激通道边界固定为最多 16 路且 `dutBit` 只能是 0..15。控制器/算法可在其他前端保留 64 路内部能力；若当前快照包含高位通道、高位 mask 或非 JavaScript 安全整数 revision，Web 投影必须返回 `available=false`、空 switches、零 mask/revision 和 `CapabilityUnsupported`，刺激动作回复 `capability_unsupported`，不得把 `quint64` 高位转换为 JSON `double`。

| JSON 字段 | 类型 | 语义 |
| --- | --- | --- |
| `available`、`configured` | boolean | 已加载配置声明刺激 / 已完成 DI 准备和控制器配置 |
| `switches` | object[] | 每项只含 `switchId`、`dutBit`、`label`、`activeLevel`（`High` 或 `Low`） |
| `appliedMask`、`revision` | number | WebSocket v1 只承载低 16 位逻辑激活位图和 JavaScript 安全整数 revision；写入成功后 revision 递增 |
| `lastWriteTimestampUs`、`settlingMs` | number | 最近一次成功写入的 UTC epoch 微秒与配置的稳定等待毫秒 |
| `errorCode`、`message` | string | 最近一次刺激操作的 HAL 归一化错误或诊断；成功时为空 |

### 4.4.1 舵机后处理 capability 与摘要

`descriptor.postRunAnalysis` 是 `mbddf.helm_stream` 的后处理能力声明。当前注册的 analyzer 为 `mbddf.helm.performance`，schema 为 `"1"`；其他配置的 `supported=false`。客户端不得从算法 ID、中文标题或配置路径推断该能力。

`snapshot.analysis` 使用以下追加式对象，所有整数必须位于 JavaScript 安全整数范围；`analysisGeneration` 为协调器私有的单调身份，不能复用控制器已有 lifecycle generation：

| 字段 | 类型 | 语义 |
| --- | --- | --- |
| `supported`、`analyzerId`、`schemaVersion` | boolean、string、string | 复制当前 descriptor 的后处理能力 |
| `taskId`、`analysisGeneration` | string、non-negative integer | 当前分析身份 `{taskId, analysisGeneration}`；只有启动成功获得新 taskId 后才可创建新身份和清空旧投影 |
| `state` | string | `none`、`capturing`、`queued`、`validating`、`preprocessing`、`calculating`、`persisting`、`completed`、`partial`、`unavailable`、`failed` 或 `cancelled` |
| `progress`、`stage`、`message`、`reasonCode` | integer、string、string、string | 小型进度、阶段、用户可读诊断和稳定原因码 |
| `resultFilePath`、`diagnosticInputFilePath` | string | 已原子提交的结果路径或保留的诊断输入路径；浏览器只读，不能覆盖 |
| `sourceSummary` | object | 固定 Schema 的小型输入/时序摘要；不承载原始样本或完整性能 JSON |
| `channelSummaries` | object[] | 四个最多通道的小型摘要；字段见下表 |

每个 `channelSummaries[]` 元素为 `{channel, enabled, status, warnings, omittedWarningCount, commonMetrics, waveformMetrics, bodeAvailable, bodePointCount}`。`channel` 只能是 `0..3`；通道状态只能为 `not_applicable`、`completed`、`partial` 或 `unavailable`。metric 统一为 `{key,label,unit,status,value,detail}`，其中 `value` 是有限 number 或 `null`，绝不以字符串、`NaN`、`Inf` 或伪造的 `0` 表示无效结果。

摘要不携带完整伯德数组，避免每次快照或重连重复广播大载荷。摘要在结果提交前按 `maxAnalysisSummaryBytes` 验证，每通道警告最多 16 条、单条 UTF-8 最多 512 字节，超出部分只反映为 `omittedWarningCount`；无法满足限制时整体进入 `failed`。`failed`/`cancelled` 只能是整体基础设施终态，不得携带半成品曲线。

分析状态独立于 `snapshot.phase` 和 BIZ `TestState`：性能结果不改变采集 `verdict`、`errorCode`、`TestResult` 或 STOP 硬件语义。`[当前实现]` 控制器已接通 STOP 尾样本封存、`queued` 起写门禁、硬件收尾栅栏、后台分析/持久化/取消和终态投影；Web 只按当前 `{taskId, analysisGeneration}` 暴露摘要与只读查询结果。

### 4.4.2 板级测试结果投影

`mbddf.do_write` 与 `mbddf.helm_board_test` 的终态或中途失败结果可由当前完整快照的 `snapshot.rawData.boardTest` 取得。其 v1 顶层对象固定为：

```json
{
  "schema": "hwtest.mbddf-board-test-result",
  "version": 1,
  "kind": "do_write",
  "mode": "automatic",
  "completedPoints": 1,
  "totalPoints": 1,
  "summary": {},
  "doSteps": [],
  "pwmPoints": [],
  "directionPoints": [],
  "feedbackPoints": []
}
```

| 字段 | 类型与约束 |
| --- | --- |
| `schema`、`version` | 必须分别为 `"hwtest.mbddf-board-test-result"` 与整数 `1` |
| `kind` | `"do_write"` 或 `"helm_board_test"` |
| `mode` | `"automatic"` 或 `"manual"` |
| `completedPoints`、`totalPoints` | 非负整数；Error 或取消时列表可以是部分结果 |
| `summary` | object；可为空，automatic 当前可提供最大 PWM/反馈误差和最差点摘要 |
| `doSteps`、`pwmPoints`、`directionPoints`、`feedbackPoints` | array；元素是后端逐点记录，页面只依赖已知字段，允许其尾部扩展 |
| `manualResponse` | 可选 object；manual 的一次 `07/02` 响应 |

`do_write` 为保持 v1 DTO 兼容继续使用 `mode="automatic"`，但它不再表示固定自动五步；浏览器必须显示为“用户配置”。`helm_board_test` 的 `automatic`/`manual` 语义保持不变。

总体 `Pass`/`Fail`/`Error` 仍以 `snapshot.verdict`、`errorCode` 和 `message` 为准，不在 `boardTest` 顶层复制。客户端必须防御性解析：未知 schema/version/kind/mode、非对象根、错误类型或缺失列表均不得导致页面异常；应安全降级为不显示板级结果或以空对象、空数组、零计数呈现，并忽略未知字段。逐点结果中不可用的数值以 JSON `null` 表示，服务端不得发送 `NaN`/`Inf` 或伪造为零。

前端只在两个上述 algorithmId 显示“板级测试”导航。DO_WRITE 显示本次完整 16 位指令与 DUT `applied_state`；只有 bit2/bit1 额外显示 PXI-6259 指令/实测/一致性，其他位必须标为“未配置外部回读”，不能宣称全 16 路物理闭环。HELM automatic 显示方向 0/1 矩阵、PWM 与反馈的指令/实测/误差/容差图形或等价清晰表格，以及最大误差/最差点摘要；manual 显示一次响应表。浏览器重连会从服务器收到仍在内存中的当前完整 snapshot，因而可恢复当前结果视图；新的任务会清空该 `rawData`，进程重启后也不承诺保留。`single` 不保存连续 TXT，`boardTest` 也不构成独立磁盘结果工件或真机验收记录。

### 4.5 样本事件

```json
{"v":1,"type":"sample","seq":42,"sample":{"taskId":"...","stepId":"IMU_STREAM","channelId":"IMU_STREAM","timestampUs":1785000000123456,"streamElapsedUs":2500,"cycleIndex":1,"values":{"delta_angle_x":0.1},"tags":{}}}
```

`sample.seq` 所在的顶层 `seq` 是样本事件在服务器进程生命周期内的独立、JavaScript 安全整数范围内的单调递增序号；它不与快照 `seq` 共用计数器。每个新连接默认使用此消息，保持旧客户端完全兼容。`timestampUs` 继续是 UTC epoch 微秒，必须是 `0..9007199254740991` 范围内的整数。算法提供非负流内相对时间时，服务追加同一范围内的可选整数 `streamElapsedUs`；其原点是本次设备流第一条有效样本，任意负值均按不可用处理并省略字段，字段缺失保持旧客户端兼容。前端拒绝负数、小数和非 JavaScript 安全整数；服务端也在最终 Web 投影处防御，遇到负或超限 `timestampUs`、超限 `streamElapsedUs` 时丢弃该 Web 事件并记录警告，不发送舍入或不合约 JSON。当前 IMU 使用 `0, 2500, 5000, ...` 的已发布样本理想轴，HELM 使用原始 DDS 时间相对首样本的实际差值；二者的 `timestampUs` 都只用一次 PC UTC 锚点加相对时间生成。HELM 的 `values.dds_timestamp_us` 也受同一安全整数上限约束。`values` 和 `tags` 的其他字段使用与 `rawData` 相同的 JSON-compatible QVariant 转换规则。服务器不缓存或重放历史样本；新客户端从连接后产生的样本开始接收。

### 4.5.1 批量样本事件

协商 `setTelemetryDelivery.mode="batch"` 成功后，服务端以如下消息替代后续的单条 `sample` 帧：

```json
{"v":1,"type":"sampleBatch","firstSeq":101,"lastSeq":102,"samples":[{"taskId":"...","stepId":"IMU_STREAM","channelId":"IMU_STREAM","timestampUs":1785000000123456,"cycleIndex":1,"values":{},"tags":{}},{"taskId":"...","stepId":"IMU_STREAM","channelId":"IMU_STREAM","timestampUs":1785000000125956,"cycleIndex":1,"values":{},"tags":{}}]}
```

- `samples` 必须非空，最多包含 capability `maxSamples` 所声明的数量，且 v1 上限为 64。
- `firstSeq`、`lastSeq` 都是 JavaScript 安全整数，且严格满足 `lastSeq == firstSeq + samples.length - 1`。样本在进入服务端批量器时分配序号，flush 时不得重编或重排；跨 batch 的序号也不得重置。
- batch 内样本顺序与 `sampleReceived` 顺序一致；每个元素的字段形状与旧 `sample.sample` 完全一致，不含单独的 `seq` 字段。
- 一个 batch 不得跨 `taskId`。任务变化前必须先 flush；不得丢字段、降采样或合并相邻样本。
- 到达样本数、紧凑 JSON 软字节上限、首样本最大等待时间、任务变化、暂停、停止、错误、终态、断线或 `disconnect`/`quit` 收尾时必须 flush。单个样本本身超过软字节上限时，允许完整发送一个超过软上限的单元素 batch，不得截断或静默丢弃。

健康连接期间，批量投影仍包含每一条可投影样本；浏览器环形显示缓存淘汰旧点不等于网络样本丢失。断线前历史不会被缓存或重放，完整长期记录仍以后端启用保存后的 TXT 为事实源。

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
| `telemetry_backpressure` | 活跃客户端的出站 FIFO 加 Qt socket 待写字节超过 `maxQueuedOutputBytes` 硬上限 | 否；记录诊断并以 `1011` 关闭。普通任务执行现有安全停止/收尾；不可停止有限流只分离连接，任务继续自然运行，绝不把背压解释为 STOP |
| `command_in_progress` | `[当前实现]` 正在异步停止、安全收尾，或分析处于 `queued` 到终态之间时又收到新会话写动作 | 是 |
| `test_config_not_found` | `selectTest.configId` 不在后端启动时发现的配置白名单中 | 是 |
| `config_not_found` / `config_invalid` | 配置文档 ID 不存在，或草稿不能通过对应结构/业务校验 | 是 |
| `config_conflict` | `saveConfig.expectedRevision` 与当前文件 SHA-256 revision 不一致，未写入 | 是 |
| `config_reload_failed` | 候选内容提交后无法重载，服务端已恢复保存前文件并保留先前有效配置；不返回候选 revision | 是 |
| `config_save_failed` | 原子写入无法提交，或重载失败后的条件回滚也无法完成；后一种情况的消息会明确磁盘状态可能已变化 | 是 |
| `invalid_run_mode` | `start.mode` 不是固定三种模式之一 | 是 |
| `CapabilityUnsupported` | `start.mode` 未在当前配置的 `supportedRunModes` 中声明，或刺激快照超出 WebSocket v1 的 16 位投影 | 是 |
| `capability_unsupported` | `setDigitalStimulus`/`resetDigitalStimulus` 面对超出 v1 范围的刺激配置 | 是 |
| `ParameterRangeError` | `start.intervalMs`、`start.maxCycles`、算法运行参数或连续数据保存目标的值/范围不合法；已定义字段的非预期 JSON 类型仍使用 `invalid_envelope` | 是 |
| `data_storage` | 保存目标参数合法，但目录、工作文件或最终文件无法创建、写入、刷新或原子提交 | 是 |
| `analysis_not_ready` | 请求身份是当前分析，但状态尚非 `completed`/`partial`，或该通道尚无可读投影 | 是 |
| `stale_analysis_result` | `analysisResult` 的 taskId 或 analysisGeneration 不是当前身份 | 是 |
| `analysis_projection_invalid` | 后端保存的投影不满足点数、数组、有限数或空洞编码不变量 | 是 |
| `analysis_projection_too_large` | 成功 `analysisResult` 的完整紧凑 reply 超过独立 16 KiB 限制 | 是 |

能够安全读取请求 `id` 时，协议错误 reply 使用该 id；否则使用空字符串。错误输入不得触发控制器动作。

数字刺激参数的类型、缺失或未知字段都属于 `invalid_envelope`，不会进入控制器。通过协议校验后，DI 未准备时的 `invalid_state`、未知配置开关的 `NotFound`、陈旧刺激 revision 的 `DataMismatch` 等是控制器/HAL 归一化 `ActionResult`；配置文档的乐观锁则使用上表固定的 `config_conflict`，协议中没有 `revision_conflict` 码。配置 revision 是内容 SHA-256 的陈旧写检测 token；保存前会再次比较以缩小竞争窗口，`QSaveFile` 只保证单次替换原子性，不构成跨进程文件锁或强 compare-and-swap。

## 6. 动作

| `action` | `params` | 行为及 reply `data` |
| --- | --- | --- |
| `load` | `{}` | legacy 静态 allowlist 与目录模式都优先按控制器当前 descriptor 重新解析服务端路径；前者映射静态 allowlist，后者映射最新 catalog，只有初次加载才使用启动项。客户端不能提供或覆盖文件路径，已禁用项不会被后续 `load` 复活；目录没有任何可选项时只初始化配置存储并保持 `empty`，以便在配置页启用测试 |
| `testConfigs` | `{}` | 返回目录中 `enabled=true` 且可加载的测试配置摘要；`data` 为 `{ "selectedConfigId": "...", "configs": [{"configId":"...","title":"...","description":"...","algorithmId":"..."}] }`，不返回本地路径 |
| `selectTest` | `{"configId":"mbddf-elec-health"}` | 只接受 `testConfigs` 返回的白名单 `configId`；服务器映射到本地路径并重新加载配置，若当前处于可运行终态则先安全关闭当前会话；运行中或停止中拒绝切换 |
| `configCatalog` | `{}` | 返回 `{revision, items}`；管理项包含服务端 `documentId`、展示信息、`enabled`、`order`、`valid` 和校验消息，既包括停用项也包括当前无法加载的 testcfg |
| `configDocument` | `{"documentId":"mbddf_system_status.testcfg.json"}` | 只接受服务端文档 ID；返回 `{documentId,kind,revision,value,schema}`。保留 ID 为 `test-config-catalog`、`mbddf-station`，其余仅为当前目录中的 `*.testcfg.json` 文件名。station/testcfg 的 `schema` 是不落盘的产品工程师表单描述，见下文 |
| `saveConfig` | `{"documentId":"...","expectedRevision":"sha256-hex","value":{}}` | 先比较 revision，再验证并用 `QSaveFile` 原子提交；当前 testcfg 或工位配置仅在自动重载成功后返回保存后的文档，重载失败则条件回滚并返回 `config_reload_failed`。目录保存后立即应用启停和顺序。只有 `empty`/`configured` 可保存，准备完成、运行、暂停、停止收尾或分析期间拒绝 |
| `snapshot` | `{}` | 从 Web 层缓存读取；`data` 为 `{ "seq": n, "snapshot": { ... } }`，其中包含当前配置 descriptor |
| `setTelemetryDelivery` | `{"mode":"single"}` 或 `{"mode":"batch"}` | Web 层本地协商动作，不调用应用控制器、不占用全局硬件 busy；每个新连接默认为 `single`。只能在没有 `preparing`/`running`/`paused`/`stopping` 活动测试、没有待批样本、没有待发输出且不在 cleanup 时切换；成功 `data` 为 `{ "mode": "single" | "batch" }`。缺少 `mode` 为 `missing_field`，非 string、未知 mode 或额外字段为 `invalid_envelope`，状态不满足为 `invalid_state`。 |
| `analysisResult` | `{"taskId":"...","analysisGeneration":3,"channel":0}` | 只读；参数必须且只能为这三个字段，taskId 非空，generation 为 `1..9007199254740991` 的安全整数，channel 为 `0..3`。只允许读取当前身份且 `completed`/`partial` 的通道；不匹配返回 `stale_analysis_result`，未就绪返回 `analysis_not_ready`。成功 `data` 为 `{ "analysisResult": { "channelSummary": { ... }, "bode": { "frequencyHz": [], "magnitudeDb": [], "phaseDeg": [], "pointStatus": [] } } }` |
| `controls` | `{}` | 在控制器亲和线程读取；`data.controls` 为 `{resourceId, providerId}` 数组 |
| `ports` | `{}` | 在控制器亲和线程读取；`data.ports` 为完整 `SerialPortInfo` 对象数组 |
| `hardwareOptions` | `{}` | 只读返回 `{state,message,allowManualEntry,devices}`。`devices[]` 包含 NI `deviceName`、`deviceId`、`model`、`serialNumber` 和 `supportedModules`；`state` 为 `available`、`unavailable` 或 `error`。服务端只按基础 HAL 中 `adapters["ni.daqmx"]` 的驱动级配置执行 Adapter `initialize -> enumerateDevices -> shutdown`，不得打开设备、查询需要打开设备的能力、创建任务或应用安全输出；Adapter、驱动或板卡不可用仍返回成功 reply 和 `allowManualEntry=true` |
| `selectControl` | `{"resourceId":"CONTROL_SERIAL"}` | 调用 `selectControl`；`resourceId` 必须是非空字符串 |
| `selectSerialPort` | `{"portName":"COM7"}` | 调用 `selectSerialPort`；`portName` 必须是非空字符串 |
| `selectAuxiliarySerialPort` | `{"portName":"COM8"}` | 只允许当前 `mbddf.serial_test` 在配置态选择 `ports` 枚举中的非空串口；保存到 `snapshot.auxiliarySerialPortName`。回显启动时必须已选择，且不得与主控制串口相同；回环忽略该端口 |
| `prepare` | `{}` | 调用 `prepare` |
| `start` | `{}` 或 `{"mode":"device_stream","saveData":true,"dataDirectory":"D:\\captures\\MB_DDF","dataFileName":"run-001.txt","algorithmParameters":{"waveform":4,"ampl":3.5}}` | 调用 `start(TestRunOptions)`；空对象保持单次兼容。`mode` 只允许 `single`、`pc_periodic`、`device_stream`，且必须由当前 descriptor 声明；`intervalMs` 为 `0..3600000` 的整数，`0` 表示上一轮完整收发结束后不增加额外等待，所有步骤仍严格串行；`maxCycles` 为 `0..1000000000` 的整数且 `0` 表示 PC 周期不限轮数。两者只有 `pc_periodic` 具备调度含义，其他模式由控制器归一为 `1000/1`；`saveData` 只能是 boolean，`pc_periodic` 与 `device_stream` 均可启用保存，`single` 强制不保存。`dataDirectory`、`dataFileName` 是可选 string，只在连续保存生效时解释；空白值分别回退配置目录和默认文件名，非空值遵循 6.1 节。`algorithmParameters` 必须是 object，键和值再由当前算法 Schema 校验 |
| `pause` | `{}` | 调用 `pause`；活动 descriptor `stoppable=false` 时返回 `CapabilityUnsupported`，不调用控制器动作 |
| `resume` | `{}` | 调用 `resume`；活动 descriptor `stoppable=false` 时返回 `CapabilityUnsupported`，不调用控制器动作 |
| `setDigitalStimulus` | `{"switchId":"di0","active":true,"expectedRevision":0}` | 必须且只能包含这三个字段；`switchId` 为非空 string，`active` 为 boolean，`expectedRevision` 为 0..9007199254740991 的非负安全整数。未知字段（包括 `resourceId`、`adapterId`、端口或路径）一律 `invalid_envelope`；控制器再按已加载配置白名单验证 `switchId`。reply 的 `data.digitalStimulus` 返回当前状态 |
| `resetDigitalStimulus` | `{}` | 只能使用空对象；任何参数均为 `invalid_envelope`。调用控制器复位，reply 的 `data.digitalStimulus` 返回当前状态 |
| `stop` | `{}` | 调用 `stopAsync`；发起成功时只在 `stopCompleted` 后发送 reply，初始调用失败时立即回复且不等待不存在的完成信号。活动 descriptor `stoppable=false` 时立即返回 `CapabilityUnsupported`，不调用控制器动作 |
| `disconnect` | `{}` | 通常必要时先异步停止，再调用 `shutdown`；最后回复并关闭当前连接，服务器继续监听。不可停止有限流活动时回复成功后仅分离连接，不停止、不 shutdown，允许重连 |
| `quit` | `{}` | 通常执行与 `disconnect` 相同的安全收尾，随后关闭服务器并退出进程。不可停止有限流活动时返回 `invalid_state`，不得关闭连接、监听或进程 |

station/testcfg 表单描述使用轻量契约 `{"contractVersion":1,"mode":"form","sections":[]}`，不是通用 JSON Schema。`sections[]` 按产品工程师可理解的主题组织 `fields[]` 和 `lists[]`：字段以 JSON Pointer `path` 定位，`kind` 当前只允许 `text`、`multiline`、`integer`、`number`、`boolean`、`choice`、`choiceOrText`、`scalar`，并可携带单位、范围、静态选项、动态 `optionsSource`、条件显示、只读标志和 `defaultValue`；`scalar` 只用于兼容判据参考值的文本、数字或开关标量。列表声明固定列、默认新行以及是否允许增删。testcfg 只描述标题、说明、步骤显示名、超时/重试、算法 Schema 中已有的默认测试参数、批准的执行时序、判定条件、结果显示字段和 DI 通道映射；station 只描述现有控制资源、串口属性、PXI-6259/PXI-6733 identity 及批准的端口/通道叶子。协议 Profile、算法 ID、Adapter/Provider、资源拓扑和 `safeState` 不进入表单。

表单 Schema 不写入配置文件，也不参与配置 revision；客户端只能在完整 `value` 草稿上修改 Schema 声明路径，必须原样保留未显示字段。文档未显式写某个字段时，`defaultValue` 只作为界面显示、条件判断和保存前校验的继承值；工程师实际修改该控件后才把值写入草稿。判据比较字段的 `acceptOptionIndex=true` 只兼容旧配置中的枚举整数，工程师重新选择后写入稳定字符串名。`optionsSource=serialPorts` 使用 `ports` 结果；板卡型号对应的 NI 设备名和序列号选项使用 `hardwareOptions`，但始终允许手工输入。目录文档由固定的启停/排序控件编辑，不使用该表单契约。未知契约版本或缺少表单描述时，浏览器不得回退成原始配置编辑器。

除 `start`、`selectTest`、`configDocument`、`saveConfig`、`selectControl`、`selectSerialPort`、`selectAuxiliarySerialPort` 和 `setDigitalStimulus` 外，无参数动作不得从 `params` 读取行为配置。`load` 尤其不得读取 `testConfigPath`、`halConfigPath` 或其他客户端路径字段；`selectTest` 只读取白名单标识，`configDocument`/`saveConfig` 只读取服务端文档 ID，三者都不读取客户端路径。testcfg 的文件名、`schemaVersion`、`configId` 和 `steps[].algorithmId` 是当前只读标识；“本次运行参数”继续是单次启动覆盖，不因配置页面保存而回写。`selectSerialPort` 保留既有非空端口名覆盖能力；`selectAuxiliarySerialPort` 则必须命中本次系统枚举。`start`、`saveConfig` 与 `setDigitalStimulus` 都只接受上表列出的字段，未知顶层字段按 `invalid_envelope` 拒绝；`algorithmParameters` 内的未知字段由应用/算法边界以 `ParameterRangeError` 拒绝。`start.dataDirectory` 与 `start.dataFileName` 是受信任本机连续数据文件的一次性输出覆盖，不进入 `ConfigurationService`、testcfg、工位覆盖、HAL、Provider/Adapter、设备端点或分析请求；除这两个字段外，`start` 不接受客户端路径、任意测量字段或其他保存配置。

### 6.1 连续模式数据文件

`[当前实现]` `saveData=true` 且运行模式为 `pc_periodic` 或 `device_stream` 时，`TestApplicationController` 在后端记录配置 descriptor 定义的全部测量列，而不是任意 `ApplicationSample.values` 字段或浏览器当前勾选的曲线字段。`pc_periodic` 记录 PC 每轮单发单回产生的样本；`device_stream` 记录一次启动后设备持续回告产生的全部样本，应用层不会为它重复发起 PC 请求。`dataDirectory` 与 `dataFileName` 只覆盖本次连续数据 TXT，不回写配置，也不改变日志、报告、HAL、设备通讯或后处理的存储目录。默认 HAL/应用组合配置为：

```json
{"dataStorage":{"directory":"../data"}}
```

`dataDirectory` 缺失或 trim 后为空时使用合并 HAL/应用配置的 `dataStorage.directory`；配置中的绝对目录直接使用，相对目录按 HAL 配置文件所在目录解析，字段缺失时回退 `../data`。非空 `dataDirectory` 必须是服务端主机可识别的完全限定绝对文件系统路径；Windows 盘符相对路径（如 `H:output`）、裸盘符和当前盘根相对路径都拒绝，UNC 路径至少包含非空的 server 与 share 两段。服务端不把有效绝对路径限制在配置目录内，也不展开 URI、环境变量或 `~`。映射盘、UNC、符号链接和 junction 按主机文件系统及服务端进程权限处理。这是用户确认的受信任回环客户端能力：获得 WebSocket 会话的本机客户端可让服务端进程在其权限范围内创建任意绝对目录和连续数据文件，不得把该权限外推到配置、HAL 或其他文件接口。

`dataFileName` 缺失或 trim 后为空时，服务端在终态生成 `<safeProject>_<startedAt>-<finishedAt>.txt`。`safeProject` 取 trim 后的 `reportFields.title`，空时回退 `configId`，除 Unicode 字母/数字、`-`、`_` 外的字符统一替换为 `_`；两个时间均按 `Asia/Shanghai` 格式化为 `yyyyMMdd_HHmmss_ffffff`。非空 `dataFileName` 必须是 basename，不得含 `/`、`\`、Windows 非法字符、ASCII 控制字符、尾随点/空格、`.`、`..` 或保留设备名；无扩展名时自动补 `.txt`，大小写不敏感的 `.txt` 统一为小写，其他扩展名拒绝。文件名 stem 最长 220 个字符。最终名冲突时在 `.txt` 前追加最小可用的 `_N`，显式名和默认名使用同一规则。工作 `.partial` 以排他创建避免协作实例互相截断；终态再以排他占位选择最终名，因此不会覆盖在运行期间新出现的同名文件。

最终文件固定为带 UTF-8 BOM 的 `.txt`：顶部是一行标题和 `# key=value` 元数据，至少包含 `started_at`、`finished_at`、`final_status`、`final_detail`、`sample_count`、`run_mode`、`repeat_delay_ms`、`config_id`、`algorithm_id` 与 `max_cycles`；`device_stream` 的两个 PC 周期字段固定写 `NA`。`started_at`、`finished_at` 使用 `Asia/Shanghai` 的 `yyyy-MM-dd HH:mm:ss.ffffff+08:00`，不依赖服务端系统本地时区；样本的 UTC epoch `timestampUs` 及相对 `sample_time_us` 语义不变。空行之后是制表符分隔、换行符为 LF 的固定表头和数据行。样本提供非负 `streamElapsedUs` 时，`sample_time_us` 直接使用该相对值；任意负值或字段不可用时保持 `timestampUs - started_at` 的兼容计算并钳制为非负值。

电气健康表头保持参考实现的固定顺序：

```text
report_index  sample_time_us  seq  response_status  err_code  c_volt_V  b_volt_V  external_vol_V  core_vol_V  assist_vol_V  v28_5_V  js_5V_V  dyt_5V_V  power_24V_V  value_YX_V  activate_bits  bc_activate_good
```

实际分隔符是 TAB。其他连续模式配置使用同样的前五列，后续列由已验证 descriptor 的全部 `measurements` 固定投影，并排除已在前缀中的 `status`/`err_code`；单位非空时写入列名后缀。前端图表的显示/隐藏、组合、时间窗和降采样均不改变该列集合。响应 `status != 0` 或 `err_code != 0` 时仍保存状态和错误码，测量列写 `NA`。协议 F32 在内部保留来源类型，TXT 使用能够往返为同一 F32 的最短十进制表示；真实 Double 继续使用既有双精度格式，不统一截断为 F32 位数。

运行期数据逐样本刷新到同目录排他创建的 `.partial` 文件，避免不限 PC 周期或设备持续回告在内存累积。显式文件名在工作文件成功打开后即可通过 `dataFilePath` 投影当前候选最终目标；若终态前出现同名文件，最终路径可按 `_N` 规则调整。默认名依赖结束时间，运行中 `dataFilePath` 保持为空。正常完成、用户停止或错误终态时，后端先排他占位最终名，再使用 `QSaveFile` 把元数据和 TSV 原子提交为最终 TXT；成功终态投影最终绝对路径，并尽力删除已无用途的 `.partial`。写入或提交失败时保留 `.partial` 供恢复，将其绝对路径投影到 `dataFilePath` 并在 `dataSaveError` 返回诊断；后续启动不得删除该恢复文件。目录、工作文件不能创建时 `start` 返回 `data_storage`；目标值不合法时在打开夹具或启动任务前返回 `ParameterRangeError`。未勾选或 `single` 均不解释两个目标字段且不创建数据文件。

### 6.2 `analysisResult` 投影不变量

`analysisResult` 不触发重新计算、不接受客户端路径、分辨率、历史身份或任意筛选条件；它只读取后端已完成的当前 sidecar artifact。成功投影的 `bode.frequencyHz`、`magnitudeDb`、`phaseDeg`、`pointStatus` 四个数组必须等长，最多 256 点：频率为正有限 Hz；幅值/相位为有限 number 或 `null`；无效点以 `null + pointStatus` 表示且不得跨空洞连线。`pointStatus` 使用稳定状态字符串（当前算法端口包括 `valid`、`upper_bound`、`indeterminate`、`not_covered`、`weak_excitation`、`invalid`）；前端不得用 `0 dB/0°` 代替缺失值。

该 action 有独立的出站限制：服务端先校验内层投影的紧凑 JSON 不超过 `16384` 字节，再校验包含标准 reply envelope 的完整紧凑 JSON 仍不超过 `16384` 字节；超限回复 `analysis_projection_too_large`。这不能由第 2 节的入站消息限制替代。点数、数组、有限数、空洞和双重 16 KiB 校验在协议投影层执行；配置中的 `maxProjectedPoints`/`maxProjectedBytes` 也在分析发布终态和提交结果前参与实际投影边界验证，无法满足时不静默截断为完整结果。

浏览器按 `{taskId,analysisGeneration}` 缓存四个通道，身份变化时整体清空并丢弃迟到回复；`analysisResult` 不占全局硬件动作 busy。性能页只使用此只读投影，连续遥测 `SampleBuffer` 只服务实时曲线。Hz/rad/s 是显示切换，不能改变协议/结果文件中的 Hz、指标计算或后端数组。

## 7. 异步、线程与安全收尾

- WebSocket 回调不得直接跨线程调用控制器。所有控制器动作和读取都通过 queued invocation 投递到控制器的 QObject 亲和线程；禁止 `BlockingQueuedConnection`。
- Web 层不得调用 `waitForTerminal()`。运行进度和终态只通过 `snapshotChanged` 观察。
- `sampleReceived` 只消费已形成的应用 DTO；默认 `single` 直接形成旧 `sample` 事件，已协商 `batch` 时进入每连接私有 `WebTelemetryBatcher`。批量器只按数目、字节、时间和 task 边界打包，不解释、合并或绘制业务字段，也不为连续测试写文件。连续数据文件在 Web 投影前已由共享应用控制器记录，TUI/GUI/WebSocket 适配器都不持有 recorder。`analysis` 不进入 sample 事件；伯德数组只能经 `analysisResult` 读取，避免每个样本重复携带大载荷。
- 活跃客户端的样本 batch、关键快照和控制 reply 进入同一个按调用顺序的出站 FIFO。WebSocket、批量器定时器、快照合并定时器、FIFO 和 `bytesToWrite()` 只在 WebSocket server 亲和线程访问；不得跨线程发送或阻塞等待网络 drain。
- 默认出站限制为：`maxBatchSamples=64`、`maxBatchBytes=32768`、`maxBatchLatencyMs=20`、`snapshotIntervalMs=100`、`socketHighWaterBytes=1048576`、`socketLowWaterBytes=262144`、`maxQueuedOutputBytes=4194304`。`bytesToWrite()` 到达高水位后暂停继续提交到 Qt socket；在 `bytesWritten()` 使其降至低水位后恢复。达到硬上限时，服务端记录 `telemetry_backpressure`、停止向旧连接投影新遥测并关闭旧连接；普通任务执行既有安全停止/收尾，不可停止有限流只分离连接且继续自然运行。不保留历史、不重放，也不得把旧 epoch 的 FIFO 内容交给重连客户端。
- `setDigitalStimulus` 和 `resetDigitalStimulus` 只在控制器处于 `ready`、`running`、`paused`、`finished` 或 `stopped`，且 DI 已准备时才会执行；Web 层不持有或传递物理资源/Adapter 参数。
- `stop` 保存请求 id，调用 `stopAsync()` 后保持事件循环运行；发起成功后收到 `stopCompleted` 才回复。若 `stopAsync()` 因状态、超时参数或已有停止而立即失败，则直接返回该控制器错误并清除 Web 层 pending 状态。
- `[当前实现]` 异步停止、断开收尾或后处理处于 `queued` 到终态期间，`snapshot`、`analysisResult`、`testConfigs`、`controls`、`ports` 和 `hardwareOptions` 等只读动作仍允许；其他新会话写动作回复 `command_in_progress`，不得再次触发控制器写动作。当前捕获任务 STOP 和所有 cleanup 始终允许。
- `disconnect`、`quit`、异常掉线和服务器内部 `DropCleanup` 在普通测试状态为 `running` 或 `paused` 时按 `stopAsync -> stopCompleted -> shutdown` 顺序执行；其他普通状态直接尝试 `shutdown`。活动 descriptor `stoppable=false` 时是显式例外：`disconnect`/异常掉线/背压只 detach，`quit` 拒绝，均不调用 stop、shutdown 或 DI 复位。DI 配置的普通停止/收尾会在 BIZ 停止后尽力 `resetDigitalStimulus`，随后按刺激设备会话、DUT 会话、HAL 的顺序释放；这不是进程崩溃、主机掉电或未验证台架的物理安全保证。shutdown 同时请求后处理协作取消并在配置时限内 join，不使用 `terminate()` 或 detach；join 超时返回 `analysis_shutdown_timeout` 并保留仍被线程引用的对象，服务器继续保持 cleanup 门禁并重试。SPA 页面内导航不等于断线，不取消分析。
- `shutdown` 的失败必须通过对应 reply 返回；异常掉线时没有 reply，但服务器仍清理会话并恢复到可接纳下一客户端的状态。
- 服务器停止监听或客户端对象销毁，不得先于已经排队的安全收尾。

### 7.1 当前浏览器实现限制

以下是现有 `front/` 的实现事实，不是服务端协议放宽：

- 图表工作区从 localStorage 读取 JSON 后直接按当前结构使用，没有逐字段迁移或防御校验；损坏或旧结构可能使图表页渲染失败。
- `parseSnapshot()` 只严格解析 descriptor、数字刺激、保存状态、运行参数和分析扩展，其他基础快照字段仍通过整体类型断言接收；类型错误可能在页面格式化阶段触发异常。
- 参数编辑器只在浏览器侧校验当前可见字段，隐藏字段仍保留在 `start.algorithmParameters`；非法隐藏值最终由后端算法 Schema 拒绝。
- 性能页在已选通道的 `analysisResult` 持续读取失败时没有错误锁存门禁；loading 清除后可能继续自动请求，直到结果成功、身份/选择变化或页面卸载。

这些限制不得被测试报告描述为已具备损坏存储迁移、完整不可信快照防御或有界分析结果重试。

## 8. 消息顺序保证

1. 活跃连接建立后依次把 `hello`、当前完整 `snapshot` 放入该连接 FIFO；每个新连接都从 `single` 模式开始。
2. 同一事件循环队列中的普通请求按接收顺序投递；每个请求最多一个 reply。样本、关键快照和 reply 通过同一 FIFO 提交，客户端必须按 `type` 分流，不能假定 reply 先于相应 snapshot。
3. `single` 模式维持逐样本、逐快照兼容投影。`batch` 模式只允许合并普通运行态快照；样本序号和样本字段绝不合并、降采样或改序。批量器在关键快照和控制 reply 前先 flush 已收样本，确保健康连接中批内及跨批样本序号连续。
4. 暂停、停止、错误、终态和安全状态变化时，顺序固定为：必要的尾部 `sampleBatch`（或已发送的单条 sample）→ 最终完整 `snapshot` → `stop`/`disconnect`/`quit` reply → 必要时关闭帧。`disconnect`/`quit` 不得在 FIFO 中的 reply 提交前关闭 socket；`quit` 的关闭帧必须先于进程退出。
5. `stop`、`disconnect`、`quit` 的 reply 必须晚于 `stopCompleted`（若需要停止）和 `shutdown`（若需要收尾）。高水位只暂停继续提交，不能重排 FIFO；硬背压失败不承诺普通遥测或 reply 投影，但仍执行安全收尾并关闭受影响连接。
6. `[当前实现]` `HELM_STREAM` 的成功 STOP reply 不等待性能计算：捕获封存先发布 `analysis.state=queued` 以建立写门禁，`stopCompleted` 硬件收尾完成后再排队启动分析线程，随后发布 validating/preprocessing/calculating/persisting/终态摘要；浏览器收到终态摘要后再按通道请求 `analysisResult`。旧身份的进度、文件提交和 reply 不得覆盖新 `{taskId,analysisGeneration}`。
