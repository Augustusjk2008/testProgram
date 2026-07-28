# testProgram

当前版本提供 MB_DDF_v2 的 `SYSTEM_STATUS`、`ELEC_HEALTH_STATUS`、`MEMPERF_TEST`、`SPI_FLASH_TEST`、`DH_PULSE_CONFIG` 和带 STOP 清理的 `TIMER_JITTER` 配置驱动测试，以及一次性 runner、分步 TUI、Qt Widgets GUI、本机 WebSocket 后端和浏览器遥测控制台。四个 C++ 入口共享同一个应用控制器和生命周期；BIZ 支持单次与 PC 主动周期测试，并保留设备主动持续回告的独立语义。板端当前通讯基线是串口，PC 端仍保留 UDP 控制资源，用于本机模拟和后续网口扩展。新增四项尚无真实板端验收证据，SPI Flash 仅可在已隔离且允许写入的目标板执行。

从仓库根目录执行一条命令即可配置、构建和启动：

```powershell
.\hwtest.ps1 ports
.\hwtest.ps1 -Ui tui
.\hwtest.ps1 -Ui gui
.\hwtest.ps1 -Ui web -WebPort 18765
.\hwtest.ps1 start -Ui gui -Port COM7
.\hwtest.ps1 run -Port COM7
.\hwtest.ps1 test
```

第一次使用命令行界面时，先阅读 [TUI 使用指南](docs/user/tui-usage-guide.md)。其中包含可直接照抄的串口流程、全部命令和状态说明、常见错误恢复，以及后续新增测试项目时的操作兼容规则。

`tui`、`gui` 和 `web` 是兼容别名，例如 `.\hwtest.ps1 tui -Port COM7`、`.\hwtest.ps1 gui -Port COM7` 与 `.\hwtest.ps1 web -WebPort 18765`。无参数执行脚本只显示帮助，不启动前端。

`hwtest_web` 只监听 `127.0.0.1`，默认暴露 `ws://127.0.0.1:18765/ws`，启动后打印机器可读的 `ready wsUrl=...`。它是控制器的 WebSocket 适配后端，不提供 HTTP 或静态文件服务。浏览器控制台是 [front/](front/README.md) 中独立启动的 Vite 工程：

```powershell
# 终端 1：WebSocket 后端
.\hwtest.ps1 -Ui web -WebPort 18765

# 终端 2：浏览器前端
Set-Location front
npm ci
npm run dev
```

随后打开 `http://127.0.0.1:5173`。三张页面顶部都保留同一个运行控制条；PC 周期模式由后端/BIZ 重复执行“发指令 → 采反馈”，浏览器不会用定时器重复发送 `start`。字段选择、同图/分图/自定义图组和时间窗保存在浏览器本机。消息、动作和安全关闭规则见 [WebSocket 前端协议契约](docs/design/contracts/websocket-frontend-protocol.md)。

`-Port` 只覆盖本次进程，不修改配置文件。也可以在独立的 HAL 部署配置中设置 `hardware.resources.<ResourceId>.properties.portName`，再用 `-HalConfig` 指定：

```powershell
.\hwtest.ps1 run -HalConfig .\configs\my_pc_hal.json
```

进入 TUI 后，串口分步流程为：

```text
ports
load
controls
use CONTROL_SERIAL
port COM7
prepare
run
wait 5000
result
disconnect
quit
```

GUI 启动后先显示配置和可用操作，不会自动加载、准备或运行。点击“加载配置”后可选择控制资源和可编辑的串口，再依次执行“准备”和“运行”；运行、停止和终态结果均由控制器快照事件驱动，不阻塞 GUI 事件循环。

运行 `.\hwtest.ps1 help` 查看全部动作和参数。架构、接口与验证规则分别见 [五层架构](docs/design/overview/five-layer-architecture.md)、[HAL 契约](docs/design/contracts/hal-interface-protocol.md) 和 [测试规范](docs/design/testing/testing-specification.md)。
