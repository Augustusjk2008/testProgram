# testProgram

当前版本提供 MB_DDF_v2 的十三项配置驱动测试：`SYSTEM_STATUS`、`ELEC_HEALTH_STATUS`、统一的 `SERIAL_TEST`、`MEMPERF_TEST`、`SPI_FLASH_TEST`、`DH_PULSE_CONFIG`、`DH_IGNITE_STREAM`、带 STOP 清理的 `TIMER_JITTER`、`DI_READ`、`DO_WRITE`、`HELM_BOARD_TEST`、`IMU_STREAM` 和 `HELM_STREAM`。`DH_IGNITE_STREAM` 只使用有限 `device_stream`：PC 发送一次可配置请求，DUT 先采 `delay_frames` 帧基线，再点火一次并回告到 `report_count`；受理后不能停止或退出，断开 Web 只分离客户端，任务自然完成。串口测试只测试 COM1/COM2/COM4，并以同一个 `single` 入口选择内部回环或 PC-DUT 外部回显：两种方式都使用 `1..100000` 的“循环次数”，默认 1000；回显在一次运行内逐轮通过用户选择的独立 PC 本地串口完成 114 字节往返，每轮截止时间为 5 秒。COM3 控制口、网口和 SPI Flash 均不进入串口测试链路。一致性结果只投影紧凑摘要，逐轮诊断最多保留前 15 轮和最后一轮，避免 114 字节明细随循环次数无限放大。一次性 runner、分步 TUI、Qt Widgets GUI、本机 WebSocket 后端和浏览器遥测控制台共享同一个应用控制器；当前宿主开发与验证以 Web 链路为主基线。BIZ 支持单次、PC 主动周期和设备主动持续回告三种通用语义。舵机连续实测由 DUT 以 1 ms 周期生成指令，经 DDS 与用户独立启停的 `MB_DDF_v2_HelmControl` 交互；它与 `HELM_BOARD_TEST` 并列存在且没有生命周期、互斥或忙状态绑定。板端当前通讯基线是串口，PC 端仍保留 UDP 控制资源，用于本机模拟和后续网口扩展。串口回显、DH 真实点火、IMU、DDS 舵机和其他新增硬件路径尚无真实板端验收证据。

`HELM_STREAM` 的后处理把性能结果作为采集结果之外的 sidecar：算法层覆盖正弦、方波、三角波、恒值和连续对数扫频，只有扫频产生伯德图；频率结果以 Hz 保存，浏览器可仅改变显示为 rad/s。性能指标不参与 `Pass/Fail`，也不改变 BIZ `TestResult`、采集错误码或硬件 STOP 语义。用户手动 STOP 后，应用层封存包含尾样本的输入，等待既有硬件收尾完成，再异步分析、原子保存版本化 JSON，并由 Web 性能页读取按通道投影；分析从 `queued` 到终态期间拒绝新会话写动作，断线/退出会协作取消。PC 不会为扫频自动 STOP。TUI 和 Qt GUI 本轮不提供性能页面。

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

随后打开 `http://127.0.0.1:5173`。任务、曲线、性能和诊断四页顶部都保留同一个运行控制条；性能页只在 descriptor 声明 `postRunAnalysis.supported` 时可进入，展示 `snapshot.analysis` 摘要并仅通过只读 `analysisResult` 拉取四个舵机通道的曲线。PC 周期模式由后端/BIZ 重复执行“发指令 → 采反馈”，浏览器不会用定时器重复发送 `start`。Web 参数编辑器消费算法层 Schema，当前可修改串口方式/链路/循环次数、舵机连续实测参数、DH 脉宽配置、DH 点火参数和 16 路数字量输出；串口回显的 PC 本地串口来自后端系统端口枚举，独立于主控制口选择。DH 点火的电源/回线使能使用复选框，23 路通道支持全选/全不选，“等待帧数”仍编码为 `delay_frames`。修改值按配置与 Schema 版本保存在当前浏览器，并只随本次 `start` 发送，不改写测试配置文件。DH 点火和其他连续模式都只有显式勾选保存时才写完整 TXT。字段选择、同图/分图/自定义图组和时间窗同样保存在浏览器本机。消息、动作和关闭规则见 [WebSocket 前端协议契约](docs/design/contracts/websocket-frontend-protocol.md)。

`-Port` 只覆盖本次主控制口，不修改配置文件。串口回显的独立 PC 本地串口在 Web 页面从系统端口枚举中选择，不会覆盖主控制口；两者指向同一物理串口时会被拒绝。也可以在独立的 HAL 部署配置中设置对应辅助资源的 `properties.portName`，再用 `-HalConfig` 指定：

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

运行 `.\hwtest.ps1 help` 查看全部动作和参数。算法、协议和浏览器自动化不构成真实舵机性能验收；真机仍须在隔离、明确授权的台架按波形、负载、STOP 时机、原始 TXT、性能 JSON、DDS 时间轴和独立参考量留存证据。架构、接口与验证规则分别见 [五层架构](docs/design/overview/five-layer-architecture.md)、[HAL 契约](docs/design/contracts/hal-interface-protocol.md) 和 [测试规范](docs/design/testing/testing-specification.md)。
