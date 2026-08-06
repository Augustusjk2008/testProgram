# MB_DDF_v2

> 项目版本：`2.0.0`
> 文档最后更新：`2026-08-06`

共享库 ABI v2 客户端必须重新编译；旧客户端应继续使用真实的 `.so.1`，不得将 `.so.1` 软链接到 `.so.2`。
ABI v1 与 v2 也不得装入同一进程；需要同时保留两代时，应使用进程边界隔离。

MB_DDF_v2 是面向 AArch64 Linux 的 C++20 工程，包含共享内存 DDS、XDMA 硬件抽象层、
产品硬件测试服务和只读硬件 Demo。Windows 主机
负责编译、部署和结果收集；生成的 C++ 二进制只在目标板运行。

## 主要功能

- 共享内存发布/订阅、Topic 注册和跨域网关。
- XDMA Transport、PWM、AD7606、ADS1258、XADC、DIDO、DH、COM，以及只读的 UPDATE image IP version / FPGA update state Device。
- CPU `/dev/spidev0.0` 上的 `SpiFlashDevice` 与独立 `SPI_FLASH_TEST`。
- 相互隔离的 COM3 回显、产品硬件测试和 DDS/硬件 Demo 三种应用画像；HW_TEST 另构建用户独立启停的 `MB_DDF_v2_HelmControl`。
- CSV 驱动的产品协议描述、板端编解码和 PC 端异步串口收发，含舵控板级 AD/PWM/方向测试与 DDS 舵机连续实测桥接。
- ADS1258 分段定标、XADC 原始码上报、电气健康采样和 23 路 DH 电压遥测。
- AArch64 单元测试、目标板硬件测试和默认只读 smoke test。

## 运行环境

Windows 主机需要 PowerShell 5.1+、能识别 `cxx_std_20` 的 CMake、`make.exe` 或
`mingw32-make.exe`、AArch64 GNU 工具链、匹配目标板的 sysroot，以及 `ssh`/`scp`。
工程不提供 C++ host/native 构建路径。

当前根 `CMakeLists.txt` 与测试 CMake 仍声明最低版本 3.10，但目标同时请求
`cxx_std_20`；因此 3.10 只是现有声明，不是已验证可用的最低版本。构建记录必须写明实际
CMake 版本，文档不得把任一更高版本写成已经由工程门禁保证的下限。

目标板需要 AArch64 Linux、SSH、`/dev/shm` 和对应的 XDMA 设备节点；CPU SPI Flash
完整能力测试还需要 `/dev/spidev0.0`。部署目标由 `build.ps1`、`debug.ps1` 的参数指定；
完整环境变量和工具链参数见
[编译、运行与调试指南](docs/guides/build-and-debug-guide.md)。

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

### 舵机控制程序

`MB_DDF_v2_HelmControl` 随 `HW_TEST` 画像一起构建。仅编译 Release 版本时，在 `dut/`
目录执行：

```powershell
.\build.bat hw_test_release
# 或
.\build.ps1 hw_test_release
```

构建产物为：

```text
build\aarch64\hw_test\Release\MB_DDF_v2_HelmControl
```

当前 `debug.bat`/`debug.ps1` 没有独立的舵控快捷入口；`debug.bat hw_test_run` 只部署并
运行产品测试服务 `MB_DDF_v2`。`MB_DDF_v2_HelmControl` 由用户独立部署、独立启停；产品
服务只管理本次 DDS bridge，`HELM_STOP` 不结束舵控进程。连续舵机实测的主机交互和生命周期
边界见[设备通信协议契约](../docs/design/contracts/device-communication-protocol.md)。

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

`MB_DDF_HW_Smoke` 默认只读，不启用 COM 回环、产品硬件测试服务或 DH 点火；它对
UPDATE image IP version 与 FPGA update state 只做通信检查和快照读取，不写入或触发更新。

## 协议兼容与限制

- 产品端软件行为以当前 [`src/`](src) 实现为唯一事实；字段布局以
  [`docs/design/product_protocol_csv`](docs/design/product_protocol_csv) 中的 CSV 为准。
  命令、错误码、定标和硬件边界的完整定义见[产品协议设计](docs/design/product_protocol_csv/codedesign.md)。
- 原始寄存器事实源为 [`docs/design/xxm_ip_addr/origin_v4`](docs/design/xxm_ip_addr/origin_v4)；
  `GOLDEN_image_IP_VERSION IP核通用型地址分配表（公开） .xlsx` 暂不进入导出或实现。
  已实现 Device、只读 smoke 边界和 CPU SPI 路径以[硬件层详细设计](docs/design/hardware-layer-architecture.md)为准。
- 主机侧职责、运行模式和协议事务边界只在[设备通信协议契约](../docs/design/contracts/device-communication-protocol.md)定义；BUS、设备流和板级事务的产品端细节只在产品协议设计定义。
- `MB_DDF_v2_HelmControl` 与产品测试服务独立启停；`HELM_BOARD_TEST` 与连续舵机实测
  不互斥，也不共享生命周期。舵机、DH、DI/DO 和寄存器映射的当前软件行为以
  `src/` 实现及产品协议设计为准。
- 当前实现的已知限制见[产品协议设计](docs/design/product_protocol_csv/codedesign.md)中的
  “已知实现限制”；该节记录现状，不表示问题已修复。

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
