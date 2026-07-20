# testProgram

当前版本提供 MB_DDF_v2 `SYSTEM_STATUS` 的一次性 runner 和分步 TUI。板端当前通讯基线是串口；PC 端仍保留 UDP 控制资源，用于本机模拟和后续网口扩展。

从仓库根目录执行一条命令即可配置、构建和启动：

```powershell
.\hwtest.ps1 ports
.\hwtest.ps1 tui
.\hwtest.ps1 tui -Port COM7
.\hwtest.ps1 run -Port COM7
.\hwtest.ps1 test
```

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

运行 `.\hwtest.ps1 help` 查看全部动作和参数。架构、接口与验证规则分别见 [五层架构](docs/design/overview/five-layer-architecture.md)、[HAL 契约](docs/design/contracts/hal-interface-protocol.md) 和 [测试规范](docs/design/testing/testing-specification.md)。
