# TUI 使用指南

本文只说明 `hwtest_tui` 的当前操作。产品协议与配置能力见[设备通讯协议契约](../design/contracts/device-communication-protocol.md)，调度与停止状态见[业务调度层契约](../design/contracts/business-scheduling-layer.md)，测试证据见[测试规范](../design/testing/testing-specification.md)。

> TUI 的 `run` 只使用 `single` 和配置默认参数，不提供运行模式或算法参数编辑。只声明 `device_stream` 的配置不能由当前 TUI 启动；这些入口使用 Web 控制面。

## 1. 启动

先查看串口，再启动 TUI：

```powershell
.\hwtest.ps1 ports
.\hwtest.ps1 -Ui tui -Port COM7
```

常用启动参数：

| 参数 | 当前作用 |
| --- | --- |
| `-TestConfig <path>` | 选择仓库内测试配置 |
| `-HalConfig <path>` | 选择 HAL 部署配置 |
| `-Control <resource-id>` | 选择控制资源 |
| `-Port <port-name>` | 覆盖本次进程的主控制串口，不写回配置 |

批准的产品协议 CSV 固定为仓库内 `dut/docs/design/product_protocol_csv/`。脚本兼容的资产覆盖参数不改变这一事实，也不得用于形成当前产品或验收证据。

## 2. 推荐流程

```text
ports
load
controls
use CONTROL_SERIAL
port COM7
prepare
run
wait 5000
status
result
disconnect
quit
```

每条命令只在相应阶段有效。`ok run` 只表示任务已启动，不表示最终为 Pass；`result=unavailable` 表示当前没有可显示结果。

## 3. 命令

| 命令 | 前置条件 | 当前行为 |
| --- | --- | --- |
| `help` / `?` | 任意 | 显示命令列表 |
| `load [test-config hal-config]` | 初始或已断开 | 读取并校验配置，不打开设备 |
| `controls` | 已加载 | 列出控制资源 |
| `ports` | 任意 | 枚举宿主串口，不占用端口 |
| `use <resource-id>` | `configured` | 选择本次控制资源 |
| `port <port-name>` | 已选择串口资源且为 `configured` | 设置本次主控制串口 |
| `prepare` | `configured` | 初始化应用、算法、BIZ 和 HAL，并打开所需设备 |
| `run` | `ready`、`finished` 或 `stopped` | 以 `single` 和当前配置默认参数启动 |
| `pause` | `running` 且 `stoppable=true` | 请求暂停 |
| `resume` | `paused` 且 `stoppable=true` | 请求继续 |
| `stop [timeout-ms]` | `running`/`paused` 且 `stoppable=true` | 请求停止；省略时为 5000 ms |
| `wait [timeout-ms]` | 活动或终态 | 等待 `finished`、`stopped` 或 `error` |
| `status` | 任意 | 显示阶段、进度、选择和错误 |
| `result` | 已产生结果 | 显示 verdict、错误、消息和 `rawData` |
| `disconnect` | 已准备或终态 | 调用控制器 `shutdown()`；失败时保持 TUI 运行 |
| `quit` / `exit` | 任意 | 尝试 `shutdown()` 后退出；失败时退出码为 2 |

`disconnect` 与 `quit` 不等价：前者成功后保留 TUI 进程和已加载配置，后者会结束进程。关闭失败时，不得把设备描述为已经安全释放。

## 4. 状态与结果

| `phase` | 含义 | 常用下一步 |
| --- | --- | --- |
| `empty` | 尚未加载 | `load` |
| `configured` | 已加载，设备未连接 | `controls`、`use`、`port`、`prepare` |
| `preparing` | 正在建立会话 | `status` |
| `ready` | 已准备 | `run` |
| `running` | 正在执行 | `status`、`wait`；仅 `stoppable=true` 时可 `pause`、`stop` |
| `paused` | 已暂停 | `resume` 或 `stop` |
| `stopping` | 正在停止 | `status`、`wait` |
| `finished` | 正常终态 | `result`、再次 `run` 或 `disconnect` |
| `stopped` | 停止终态 | `result`、再次 `run` 或 `disconnect` |
| `error` | 错误终态 | `status`、可能的 `result`、`disconnect` |
| `shutdown_failed` | 收尾未完整成功 | 保存错误信息，不宣称安全释放 |

`result` 中的 `verdict` 是业务判定，`attempts` 是实际尝试次数，`rawData` 是算法返回的紧凑结果。准备或通讯在产生业务结果前失败时，`result` 可以继续为 unavailable。

## 5. 不可停止任务的当前限制

`mbddf.dh_ignite_stream` 是当前唯一声明 `stoppable=false` 的配置。请求受理后，控制器拒绝 pause、resume、stop 和 shutdown，不发送 STOP/ABORT，任务按产品协议自然完成。

当前 TUI 不能直接以 `device_stream` 启动该配置；但控制器限制仍适用于任何已经进入该状态的会话。存在以下入口层现状：

- `disconnect` 会返回 `CapabilityUnsupported` 并保持进程运行；
- `quit`/`exit` 在 shutdown 失败后仍退出，退出码为 2；
- 标准输入 EOF 同样尝试 shutdown，失败后仍退出码 2，不等待自然终态。

因此，关闭终端、发送 EOF 或看到进程退出都不能作为任务已停止、设备已断开或资源已安全释放的证据。

## 6. 常见问题

| 现象 | 处理 |
| --- | --- |
| `ports=none` | 检查线缆和驱动后重新执行 `ports` |
| `control_not_found` | 先执行 `load`、`controls`，使用实际资源 ID |
| `control_not_serial` | 选择串口类型控制资源后再设置 `port` |
| `prepare` 打开端口失败 | 确认端口名、占用和驱动；回到 `configured` 后重试 |
| `run_timeout` | 执行 `status`；仅 `stoppable=true` 时可 `stop 5000`，不可停止任务等待自然终态 |
| `unsupported_algorithm` / `CapabilityUnsupported` | 当前配置、模式或 TUI 能力不支持该动作；改用 Web 入口或受支持配置 |
| `shutdown_failed` | 保存错误与日志，不要继续宣称设备已安全释放 |

运行 `.\hwtest.ps1 help` 查看脚本参数。页面或命令说明不得替代上述契约和测试事实源。
