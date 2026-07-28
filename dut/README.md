# MB_DDF_v2

> 项目版本：`1.0.0`
> 文档最后更新：`2026-07-17`

MB_DDF_v2 是面向 AArch64 Linux 的 C++20 工程，包含共享内存 DDS、XDMA 硬件抽象层、
产品硬件测试服务、只读硬件 Demo，以及配套的 Windows PyQt5 串口工具。Windows 主机
负责编译、部署和结果收集；生成的 C++ 二进制只在目标板运行。

## 主要功能

- 共享内存发布/订阅、Topic 注册和跨域网关。
- XDMA Transport、PWM、AD7606、ADS1258、XADC、DIDO、DH、COM 和 Flash Device。
- 相互隔离的 COM3 回显、产品硬件测试和 DDS/硬件 Demo 三种应用画像；HW_TEST 另构建用户独立启停的 `MB_DDF_v2_HelmControl`。
- CSV 驱动的产品协议描述、板端编解码和 PC 端异步串口收发，含舵控板级 AD/PWM/方向测试与 DDS 舵机连续实测桥接。
- ADS1258 分段定标、XADC 原始码上报、电气健康采样和 23 路 DH 电压遥测。
- AArch64 单元测试、目标板硬件测试和默认只读 smoke test。

## 运行环境

Windows 主机需要 PowerShell 5.1+、CMake 3.10+、`make.exe` 或
`mingw32-make.exe`、AArch64 GNU 工具链、匹配目标板的 sysroot，以及 `ssh`/`scp`。
工程不提供 C++ host/native 构建路径。

目标板需要 AArch64 Linux、SSH、`/dev/shm` 和对应的 XDMA 设备节点；CPU SPI Flash
完整能力测试还需要 `/dev/spidev0.0`。默认目标板地址为 `192.168.1.29`，测试目录为
`/home/sast8/user_tests`。完整环境变量和工具链参数见
[编译、运行与调试指南](docs/guides/build-and-debug-guide.md)。

PC 串口工具支持 Windows 7 SP1 x64 和 Python 3.8.18，依赖锁定在
[`test_pyqt/requirements-win7.txt`](test_pyqt/requirements-win7.txt)。工具使用 PyQt5
`QtSerialPort`，不依赖 `pyserial`。

## 快速开始

查看构建帮助：

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1 help
```

选择应用画像构建并运行：

| 画像 | 构建 | 运行 | 用途 |
|---|---|---|---|
| COM3 回显 | `.\build.ps1 debug` | `.\debug.ps1 -Run -Com3Echo` | 固定 COM3 链路回显 |
| DDS/硬件 Demo | `.\build.ps1 hw_debug` | `.\debug.ps1 -Run -FullHardware` | 默认只读的 DDS 与硬件演示 |
| 产品硬件测试 | `.\build.ps1 hw_test_debug` | `.\debug.ps1 -Run -HardwareTest` | 按产品协议执行硬件测试命令；同时产出独立 `MB_DDF_v2_HelmControl` |

`build.bat` 和 `debug.bat` 是兼容包装；完整动作和参数见构建指南。

## 目标板测试

```powershell
.\tests\test-dds-only.ps1 -BuildOnly
.\tests\test-dds-only.ps1
.\tests\test-deploy.ps1 -TestFilter "RingBuffer*"
.\tests\test-all.ps1 -BuildOnly
.\tests\test-all.ps1
.\tests\test-deploy.ps1 -TestBinaryName MB_DDF_HW_Tests
.\tests\test-deploy.ps1 -TestBinaryName MB_DDF_HW_Smoke
```

`MB_DDF_HW_Smoke` 默认只读，不启用 COM 回环、产品硬件测试服务或 DH 点火。

## PC 串口工具

安装锁定依赖后，从根目录启动：

```powershell
.\test_pyqt\run.bat
```

使用固定的 `pyqt5_env` 环境打包为独立目录：

```powershell
.\test_pyqt\run.bat package
```

产物入口为
`build\test_pyqt\dist\MB_DDF_HW_Test_PC\MB_DDF_HW_Test_PC.exe`。打包脚本会将产品协议
CSV 一并收录。当前环境中的 PyInstaller 6.17.0 产物尚未在 Windows 7 SP1 x64 实机验证，
交付前仍需在目标系统完成启动、串口枚举及 `614400 / 8E1` 联调。

与板端回显或产品协议服务联调时，板端链路固定为 COM3，配置为
`614400 / 8E1 / 无流控`。PC 工具保留通用串口控件，但联调配置必须与板端一致。

PC 测试是 Windows 主机运行 Python 测试的明确例外：

```powershell
$Py = 'C:\Users\JiangKai\.conda\envs\pyqt5_env\python.exe'
$env:QT_QPA_PLATFORM = 'offscreen'
& $Py -m pytest -q .\test_pyqt\tests
```

## 协议兼容与限制

- 产品协议字段布局以 [`docs/design/product_protocol_csv`](docs/design/product_protocol_csv)
  下的 37 份 CSV 为准。DH 脉宽配置使用 `0x06/0x01`，控制与多帧遥测使用
  `0x06/0x02`；旧 `dh_report_response` 不再使用。
- 旧 PyQt 舵控页面及“执行全部”使用 `HELM_BOARD_TEST 0x07/0x02`：B9 低 4 位保留、
  高 4 位设置四路方向，B10-B13 分别设置四路整数占空比 `0..100%`，并回读
  `pwm_duty_match`、raw duty、peak、方向、使能状态及四路 AD7606。Web 主基线使用
  `HELM_START 0x07/0x10`、`HELM_FEEDBACK 0x07/0x01`、`HELM_STOP 0x07/0x11`：DUT
  以 1 ms 周期生成指令，经 DDS 与独立 `MB_DDF_v2_HelmControl` 交互，最多 5 个完整反馈
  样本组成一帧回告。扫频总时长可配置，结束后指令归零、反馈继续到 STOP。
- `MB_DDF_v2` 测试服务与 `MB_DDF_v2_HelmControl` 可由用户按任意顺序、独立启停；测试
  服务不管理舵控进程。`HELM_BOARD_TEST` 与连续实测并列存在，不互斥、不返回对方导致的
  `TASK_BUSY`，也不共享生命周期状态。
- 板级寄存器以 `origin_v3` 为现行事实源。XADC 全局基址为 `0x150000`，PCIe SPI Flash
  为 `0x160000`；外部集成不得继续沿用旧映射。
- `ELEC_HEALTH_STATUS` 在 B31-B32 传输 XADC `value_YX` 的 FPGA 原始 ADC code，PC
  按 CSV 的 `10.09/4096 V/code` 换算；四路 AD7606 由 `HELM_BOARD_TEST` 回传并按
  `10/65536 V/code` 换算。ADS1258 电气健康和 DH 遥测由板端完成分段定标后量化，PC
  不重复应用运放公式。ADS1258 的当前换算系数
  是临时定标值，完整公式以 `codedesign.md` 和 `origin_v3` 生成表为准。
- DH 控制默认回告 50 次、间隔 2500 us；`report_count` 范围为 `1..65535`，
  `interval_us` 范围为 `2500..65535`。板端每帧到期采样后立即发送，下一采样截止点从本帧
  实际采样起点计算，串口发送时间只占用周期余量，不额外叠加到请求间隔。
- PC 页面只绘制本次请求选中的点火状态和遥测电压曲线，并对高频回告批量刷新；完整去重
  回告会在完成、失败或停止时保存到用户指定目录，文件名为
  `DH_data_YYYYMMDD_HHMMSS_ffffff.txt`。
- 电气健康只显示 `activate_bits.bit0`，名称为“BC激活好”；该位来自 DH
  `BatteryStatus` 高 8 位（`0xAA=1`、`0xFF=0`），bit1..7 固定为 0 且不显示，非法高字节
  返回 `REG_READ_WRITE_FAILED`。`5V_JS` 只有 XADC 电压采样，没有 DIDO 控制输出。
- PC 的 DI/DO 页面按 `origin_v3` 标注已知信号：DI0 联锁/电气弹动、DI1 引信报警、
  DI2 引信起爆指令、DI3 锁相环锁定指示、DI8 投放允许；DO0 舵锁使能、DO1
  数控衰减器控制、DO2 数遥发送使能、DO3 `24V_EN`、DO4 `DYT_5V_EN`、DO5
  `DI_EN1`、DO6 `DO_EN`。未在地址表中命名的通道保持通道号。

## 安全边界

- DDS/硬件 Demo 和默认 smoke test 保持只读。
- `HW_TEST` 画像包含 DIDO、DH、舵控和 SPI Flash 写操作。所有写操作只能在已隔离且
  明确允许硬件写入的目标板运行。
- `HELM_BOARD_TEST` 会保持写入后的 PWM、方向及相关使能状态，不自动恢复测试前状态；
  回读不一致返回 `REG_RW_FAILED (0x0201)`。
- N25Q512A `#HOLD` 使用非易失配置永久关闭，不是仅对本次上电有效的临时设置。

## 文档索引

- [文档索引](docs/README.md)
- [编译、运行与调试指南](docs/guides/build-and-debug-guide.md)
- [Demo 使用说明](docs/guides/demo-usage.md)
- [DDS 使用说明](docs/guides/dds-standalone-usage.md)
- [DDS 详细设计](docs/design/dds-detailed-design.md)
- [硬件层详细设计](docs/design/hardware-layer-architecture.md)
- [产品协议设计](docs/design/product_protocol_csv/codedesign.md)
- [寄存器汇总](docs/design/xxm_ip_addr/generated/registers.md)
- [测试快速说明](docs/tests/README.md)
- [DDS 测试说明](docs/tests/dds-test-guide.md)
