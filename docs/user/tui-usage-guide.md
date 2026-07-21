# TUI 使用指南

本文面向第一次使用 `hwtest_tui` 的操作人员。TUI 是逐行输入命令的控制台界面；每输入一条命令并按 Enter，程序就返回一行或多行结果。

> 当前能力边界：应用控制器只支持 `algorithmId = "mbddf.system_status"`，并要求测试配置中恰好有一个启用的 `SYSTEM_STATUS` 步骤。本文后半部分的“新增项目兼容规则”是后续接入约定，不表示当前已经可以运行任意测试项目。

## 1. 最短上手流程

在仓库根目录打开 PowerShell，先查看串口名称：

```powershell
.\hwtest.ps1 ports
```

假设目标串口是 `COM7`，启动 TUI：

```powershell
.\hwtest.ps1 -Ui tui -Port COM7
```

看到 `hwtest>` 提示符后，依次输入：

```text
load
status
prepare
run
wait 5000
status
result
disconnect
quit
```

这组命令使用脚本传入的默认测试配置和 HAL 配置。`-Port COM7` 只覆盖本次会话，不会修改 JSON 文件。

判断执行是否成功时，先看每条命令的首个单词：

- `ok <action>`：该命令动作已成功；`ok run` 只表示测试已启动，不表示结果为 Pass。
- `error <code> <message>`：动作失败；先执行 `status` 查看当前阶段，再按第 6 节处理。
- `result=unavailable`：当前还没有可显示的测试结果，不等同于 Pass。

## 2. 完整串口操作流程

需要显式确认控制资源和串口时，使用下面的流程：

```text
load
controls
use CONTROL_SERIAL
ports
port COM7
status
prepare
run
wait 5000
result
disconnect
quit
```

关键分界如下：

```text
读取配置        选择会话参数        打开设备        执行与观察        释放资源
load       -> controls/use/port -> prepare    -> run/wait/result -> disconnect/quit
```

`load`、`controls`、`ports`、`use` 和 `port` 都不会打开串口。`prepare` 才会初始化 HAL，并尝试打开当前选中的控制资源。

如果配置路径不是默认值，可在启动时指定：

```powershell
.\hwtest.ps1 -Ui tui `
  -TestConfig .\configs\mbddf_system_status.testcfg.json `
  -HalConfig .\configs\mbddf_pc_hal.json `
  -Control CONTROL_SERIAL `
  -Port COM7
```

也可以在 TUI 内重新加载一对配置。路径中有空格时使用双引号：

```text
load "D:\Test Configs\product.testcfg.json" "D:\Test Configs\station-hal.json"
```

`load` 只能不带参数，或同时提供测试配置和 HAL 配置两个参数。

## 3. 命令速查

| 命令 | 何时使用 | 作用 | 设备影响 |
| --- | --- | --- | --- |
| `help` 或 `?` | 任意时刻 | 显示内置命令清单 | 无 |
| `load [test-config hal-config]` | 初始或已断开 | 读取并校验两份配置，再应用启动时的控制口/串口覆盖 | 只读文件，不打开设备 |
| `controls` | `load` 后 | 列出 `resource=<id> provider=<id>` | 无 |
| `ports` | 任意时刻 | 枚举 Windows 串口信息 | 只枚举，不打开端口 |
| `use <ResourceId>` | `configured` | 选择本次会话使用的控制资源 | 只改内存配置 |
| `port <port-name>` | 已选择 `qt.serial` 且为 `configured` | 设置本次会话的串口名 | 只改内存配置 |
| `prepare` | `configured` | 初始化 HAL、算法和 BIZ，并打开所选设备 | 会尝试打开设备 |
| `run` | `ready`、`finished` 或 `stopped` | 启动当前测试 | 使用已打开的设备 |
| `pause` | 测试正在运行 | 请求暂停 | 不新开设备；短测试可能已先结束 |
| `resume` | 测试已暂停 | 请求继续 | 使用现有会话 |
| `stop [timeout-ms]` | 测试正在运行或暂停 | 停止当前任务；省略时超时为 5000 ms | 停止任务，但保留已准备会话 |
| `status` | 任意时刻 | 显示阶段、状态、选择、进度和错误 | 无 |
| `wait [timeout-ms]` | 测试正在运行、暂停、停止中或已经终止 | 等待 `finished`、`stopped` 或 `error`；省略时使用 HAL 配置的运行超时 | 不创建新操作 |
| `result` | 产生结果后 | 显示判定和原始数据 | 无 |
| `disconnect` | 不再使用当前设备时 | 按顺序关闭 BIZ 和 HAL 资源 | 会关闭设备；成功后保留已加载配置 |
| `quit` 或 `exit` | 结束操作时 | 先执行有序收尾，再退出 TUI | 会关闭设备；收尾失败时进程返回 2 |

`stop`、`disconnect` 和 `quit` 不是同一个动作：

- `stop` 只结束当前测试，成功后通常可以再次 `run`。
- `disconnect` 关闭已准备的业务和硬件资源，但 TUI 仍然运行，可重新选端口并再次 `prepare`。
- `quit` 尝试完成与 `disconnect` 相同的收尾，然后退出进程。出现收尾错误时，不应把设备视为已安全释放。

## 4. 看懂 `status`

示例：

```text
phase=running state=Running control=CONTROL_SERIAL provider=qt.serial serial=COM7 progress=0 task=... step=... error= message=
```

字段含义：

| 字段 | 含义 |
| --- | --- |
| `phase` | 应用控制器阶段，决定下一步可执行动作 |
| `state` | BIZ 测试状态 |
| `control` / `provider` | 当前控制资源及其 Provider |
| `serial` | 当前串口；选择 UDP 时可以为空 |
| `progress` / `step` | 当前进度和正在处理的步骤 |
| `task` | 当前运行任务 ID |
| `error` / `message` | 最近的错误码和说明 |

常见 `phase` 与下一步：

| `phase` | 含义 | 常用下一步 |
| --- | --- | --- |
| `empty` | 尚未加载配置 | `load` |
| `configured` | 配置已加载，设备未连接 | `controls`、`use`、`port`、`prepare` |
| `preparing` | 正在建立业务和硬件会话 | `status` |
| `ready` | 已准备，尚未运行 | `run` |
| `running` | 正在执行 | `status`、`wait`、`pause`、`stop` |
| `paused` | 已暂停 | `resume` 或 `stop` |
| `stopping` | 正在停止 | `status` 或 `wait` |
| `finished` | 正常到达终态 | `result`、再次 `run` 或 `disconnect` |
| `stopped` | 已由停止请求终止 | 再次 `run` 或 `disconnect` |
| `error` | 运行进入错误终态 | `status`、可能的 `result`、`disconnect` |
| `shutdown_failed` | 收尾未完整成功 | 记录 `error/message`，不要宣称设备已安全释放 |

## 5. 看懂 `result`

有结果时输出三行：

```text
step=SYSTEM_STATUS item=system_status algorithm=mbddf.system_status attempts=1
verdict=Pass error= message=...
rawData={...}
```

- `verdict` 是最终判定，例如 `Pass` 或 `Fail`。
- `attempts` 是实际尝试次数。
- `error` 和 `message` 解释失败或错误原因。
- `rawData` 是算法返回的紧凑 JSON 原始结果，排查问题和比对前端结果时应保留。

只看到 `result=unavailable` 时，先执行 `status`。常见原因是尚未运行、运行尚未结束，或在产生业务结果前就发生了准备/通讯错误。

## 6. 常见问题与恢复

| 现象 | 含义或检查项 | 建议操作 |
| --- | --- | --- |
| `ports=none` | Windows 当前未枚举到串口 | 检查线缆和驱动后再次执行 `ports`；该命令不会占用端口 |
| `error parse ...` | 命令名、参数数量、引号或超时值不合法 | 输入 `help`，按命令原型重新输入 |
| `error invalid_state ...` | 当前阶段不允许该动作 | 执行 `status`，按第 4 节回到正确阶段 |
| `error control_not_found ...` | 配置中不存在该资源 ID | 先执行 `load` 和 `controls`，复制实际的 `resource=` 值 |
| `error control_not_serial ...` | 当前控制资源不是 `qt.serial` | 先执行 `use CONTROL_SERIAL`，再执行 `port COM7` |
| `prepare` 报端口打开失败 | 端口名错误、被占用、驱动异常或设备不可用 | 确认 `status` 已回到 `configured`，修正 `port` 后重试；必要时先 `disconnect` |
| `error run_timeout ...` | 等待期内未到达终态 | 执行 `status`；若仍在运行则 `stop 5000`，随后查看 `result` 并 `disconnect` |
| `unsupported_algorithm` 或“Exactly one enabled...” | 配置超出当前控制器能力 | 改用当前 SYSTEM_STATUS 配置；不能通过换一条 TUI 命令绕过 |

启动脚本找不到协议 CSV 时，显式传入已批准的资产目录：

```powershell
.\hwtest.ps1 -Ui tui -ProtocolCsvDir "H:\Resources\RTLinux\Demos\MB_DDF_v2\docs\design\product_protocol_csv"
```

## 7. 启动参数与配置保存

推荐入口是 `.\hwtest.ps1 -Ui tui`；`.\hwtest.ps1 tui` 是保留的兼容别名。常用参数如下：

| 参数 | 用途 |
| --- | --- |
| `-TestConfig <path>` | 选择测试配置 |
| `-HalConfig <path>` | 选择 HAL 部署配置 |
| `-Control <ResourceId>` | 覆盖本次会话的控制资源 |
| `-Port <port-name>` | 覆盖本次会话的串口 |
| `-ProtocolCsvDir <path>` | 指定 MB_DDF 协议 CSV 目录 |
| `-Configuration Debug\|Release` | 选择构建配置 |
| `-SkipBuild` | 使用已有可执行文件，不重新构建 |

`-Control`、`-Port` 以及 TUI 内的 `use`、`port` 都只修改进程内状态，不回写 HAL JSON。需要长期保存站点配置时，应维护独立 HAL 配置文件，再用 `-HalConfig` 选择它。

## 8. 新增项目时如何保持使用习惯连续

这里的“项目”指新的测试配置及其算法能力。用户习惯由稳定的生命周期保证，而不是由某个产品名或某组固定结果字段保证：

```text
load -> select(use/port) -> prepare -> run -> wait/status -> result -> disconnect
```

后续项目接入必须遵守以下兼容规则：

1. **项目由配置选择。** 继续使用 `-TestConfig` 和 `-HalConfig`，不要为每个产品新增一套 `run-product-x` 命令。
2. **既有命令不改义。** `load` 仍只加载配置，`prepare` 才能打开设备，`run` 只启动测试，`disconnect` 负责有序释放资源。
3. **状态和输出保持可解析。** 保留 `ok`、`error`、`phase=`、`verdict=` 和 `rawData=` 等既有前缀；新字段只追加，不改变旧字段的含义。
4. **新能力采用追加式演进。** 必须新增命令时使用新名称；不能复用旧命令承载不兼容语义。废弃命令需保留兼容别名和明确迁移周期。
5. **前端共享同一控制器契约。** TUI、GUI 和 batch 入口继续调用同一应用控制器，项目生命周期和判定不能在前端各自实现。
6. **回归测试作为合入门禁。** 既有 `TuiShellTest`、`HwtestTuiScriptedSession` 和 `FrontendEquivalenceTest` 必须继续通过；每个新项目还要补一条覆盖 `load -> prepare -> run -> terminal -> result -> disconnect` 的工作流测试。
7. **文档示例随项目增加。** 每个项目只增加“配置组合、资源选择、结果字段”示例，不重复发明基础操作流程。

因此，未来变化的是配置、可选资源、测试步骤和 `rawData` 内容；操作人员已经形成的命令顺序、动作含义和错误处理方式保持不变。正式准入规则记录在[测试规范](../design/testing/testing-specification.md)中。
