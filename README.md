# testProgram

当前版本提供 MB_DDF_v2 `SYSTEM_STATUS` 的一次性 runner、分步 TUI 和 Qt Widgets GUI。TUI 与 GUI 共享同一个应用控制器和生命周期；板端当前通讯基线是串口，PC 端仍保留 UDP 控制资源，用于本机模拟和后续网口扩展。

从仓库根目录执行一条命令即可配置、构建和启动：

```powershell
.\hwtest.ps1 ports
.\hwtest.ps1 -Ui tui
.\hwtest.ps1 -Ui gui
.\hwtest.ps1 start -Ui gui -Port COM7
.\hwtest.ps1 run -Port COM7
.\hwtest.ps1 test
```

第一次使用命令行界面时，先阅读 [TUI 使用指南](docs/user/tui-usage-guide.md)。其中包含可直接照抄的串口流程、全部命令和状态说明、常见错误恢复，以及后续新增测试项目时的操作兼容规则。

`tui` 和 `gui` 仍是兼容别名，例如 `.\hwtest.ps1 tui -Port COM7` 与 `.\hwtest.ps1 gui -Port COM7`。无参数执行脚本只显示帮助，不启动前端。

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
