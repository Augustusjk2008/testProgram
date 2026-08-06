# 编译、运行与调试指南

## 1. 适用范围

本工程只支持以下工作流：

```text
Windows 主机
  ├─ PowerShell / build.bat
  ├─ CMake + Unix Makefiles
  └─ AArch64 GNU 交叉工具链
          |
          v
AArch64 Linux 目标板
  ├─ 运行 Demo
  ├─ 运行测试二进制
  └─ gdbserver 远程调试
```

Windows 主机只负责配置、编译、部署和启动调试，不运行生成的 AArch64
二进制。工程没有 C++ host/native 构建路径。

## 2. 主机与目标板准备

### 2.1 Windows 主机

必须安装或准备：

- PowerShell 5.1 或更高版本。
- CMake。
- `make.exe` 或 `mingw32-make.exe`。
- AArch64 GNU 工具链：
  - `aarch64-none-linux-gnu-gcc`
  - `aarch64-none-linux-gnu-g++`
  - `aarch64-none-linux-gnu-gdb`
- 与目标板运行环境匹配的 AArch64 sysroot。
- Windows OpenSSH Client：`ssh`、`scp`、`ssh-keygen`。
- VS Code C/C++ 扩展，用于图形化远程调试。
- `rg`，用于测试脚本中的静态检查。

### 2.2 目标板

目标板需要：

- AArch64 Linux 用户态。
- SSH 登录可用，默认用户为 `root`。
- `/dev/shm` 已挂载并有足够空间。
- 支持 POSIX shared memory、named semaphore、pthread process-shared
  mutex/condition variable。
- 调试时可执行工程内提供的 `tools/gdbserver`。
- 运行硬件 Demo 时存在对应 XDMA 设备节点；完整硬件流程还需要 CPU
  `/dev/spidev0.0` 且当前用户具有读写权限。

默认连接参数为：

| 参数 | 默认值 |
|---|---|
| 目标板 IP | `192.168.1.29` |
| 用户 | `root` |
| 调试/运行目录 | `/home/sast8/tmp` |
| 测试目录 | `/home/sast8/user_tests` |
| gdbserver 端口 | `1234` |

## 3. 工具链参数解析

`build.ps1` 优先使用命令行参数，其次读取环境变量，最后尝试从 `PATH`
和工具链目录推导。

常用参数：

| 参数 | 作用 |
|---|---|
| `-ProjectName` | CMake 项目名和可执行文件名，默认 `MB_DDF_v2` |
| `-Sysroot` | AArch64 sysroot |
| `-ToolchainBin` | AArch64 工具链 `bin` 目录 |
| `-MakePath` | `make.exe` 路径 |
| `-Arm64LibsPrefix` | AArch64 第三方库前缀 |
| `-Jobs` | 并行编译任务数 |

优先使用的环境变量：

```powershell
$env:CROSS_CXX_COMPILER = "D:\toolchain\bin\aarch64-none-linux-gnu-g++.exe"
$env:TOOLCHAIN_BIN = "D:\toolchain\bin"
$env:MAKE_PATH = "D:\tools\make.exe"
```

也可以全部通过参数指定：

```powershell
.\build.ps1 debug `
  -Sysroot "D:\sysroots\aarch64" `
  -ToolchainBin "D:\toolchain\bin" `
  -MakePath "D:\tools\make.exe" `
  -Jobs 12
```

## 4. 构建入口

查看帮助：

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1 help
```

`build.bat` 只是兼容包装，会把全部参数转发给 `build.ps1`：

```powershell
.\build.bat debug
.\build.bat hw_release
```

### 4.1 构建动作

| 动作 | 配置 | 主要产物 |
|---|---|---|
| `clean` | 清理 | 删除 `build`，清除 gdbserver 上传标记 |
| `debug` | COM3 回显，Debug | `build\aarch64\echo\Debug\MB_DDF_v2` |
| `release` | COM3 回显，Release | `build\aarch64\echo\Release\MB_DDF_v2` |
| `source_debug` | 递归源码调试，Debug | `build\aarch64\source_debug\Debug\MB_DDF_v2` |
| `lib_debug` | DDS 静态/动态库，Debug | `install\libs`、`install\include` |
| `lib_release` | DDS 静态/动态库，Release | `install\libs`、`install\include` |
| `dds_tests` | DDS-only 测试，Debug | `build\aarch64\tests\Debug\tests\MB_DDF_v2_Tests` 等 |
| `hw_debug` | 原 DDS/硬件 Demo，Debug | `build\aarch64\demo\Debug\MB_DDF_v2` |
| `hw_release` | 原 DDS/硬件 Demo，Release | `build\aarch64\demo\Release\MB_DDF_v2` |
| `hw_test_debug` | 产品硬件测试服务，Debug | `build\aarch64\hw_test\Debug\MB_DDF_v2` |
| `hw_test_release` | 产品硬件测试服务，Release | `build\aarch64\hw_test\Release\MB_DDF_v2` |
| `hw_tests` | 硬件层和板端测试，Debug | `build\aarch64\tests\Debug\tests\MB_DDF_HW_Tests` 等 |

常用命令：

```powershell
.\build.ps1 clean
.\build.ps1 debug
.\build.ps1 release
.\build.ps1 source_debug
.\build.ps1 lib_debug
.\build.ps1 lib_release
.\build.ps1 dds_tests
.\build.ps1 hw_debug
.\build.ps1 hw_release
.\build.ps1 hw_test_debug
.\build.ps1 hw_test_release
.\build.ps1 hw_tests
```

### 4.2 CMake 开关

脚本以显式 CMake `-D` 参数选择画像和可选模块，并把兼容用环境变量同步成相同值，避免
旧调用环境与当前构建目录冲突：

| 变量 | 含义 |
|---|---|
| `MB_DDF_APP_MODE` | 应用画像，只允许 `ECHO`、`HW_TEST`、`DEMO` |
| `BUILD_SOURCE_DEBUG` | 仅供 `source_debug` 使用，递归构建源码调试组合 |
| `ENABLE_TESTS` | 构建测试 |
| `BUILD_HARDWARE_LAYER` | 构建 `MB_DDF_HW` |
| `BUILD_HW_DDS_ADAPTER` | 构建硬件 DDS Adapter |
| `ENABLE_HW_UNIT_TESTS` | 构建硬件层单元测试 |
| `ENABLE_HW_SMOKE_TESTS` | 构建真实硬件 smoke 程序 |

`ECHO` 和 `HW_TEST` 固定启用硬件层并关闭 DDS Adapter；`DEMO` 固定启用 DDS Core、
硬件层和 Adapter。CMake 会按单一 `MB_DDF_APP_MODE` 归一这些组合，非法画像值会直接
失败，不能通过运行时环境变量切换入口。

`source_debug` 是独立的 Debug-only 构建组合，入口为 `src/main.cpp` 的导引头调试分支。
它通过 `CONFIGURE_DEPENDS` 递归收集 `src/MB_DDF/`、`src/MB_DDF_DEBUG/` 和
`src/MB_DDF_HW/` 中的常见 C++ 源码与头文件；后续在这些目录新增文件无需修改清单，
再次构建时 CMake 会自动重新配置。该组合把硬件层实现直接编入可执行文件，不会改变
日常 ECHO、HW_TEST、DEMO 或库目标的显式源码清单。

导引头调试入口会初始化 COM1，启动后先发送一帧 A，持续接收 B 帧并在每四帧后回发
一帧 A。每个有效 B 帧通过 `SelfDescribingLog` 立即追加并刷新到当前工作目录的
`dyt_frame_b.sdlog`；已有同模式文件会继续追加。该入口包含真实串口收发，只能在已隔离且
明确允许 COM1 写入的目标板运行。

### 4.3 构建输出

主要目录：

```text
build/aarch64/echo/Debug/
build/aarch64/echo/Release/
build/aarch64/source_debug/Debug/
build/aarch64/hw_test/Debug/
build/aarch64/hw_test/Release/
build/aarch64/demo/Debug/
build/aarch64/demo/Release/
build/aarch64/tests/Debug/
build/aarch64/lib/Debug/
build/aarch64/lib/Release/
install/libs/
install/include/
.vscode/compile_commands.json
```

每次成功配置后，`build.ps1` 会把构建目录中的
`compile_commands.json` 复制到 `.vscode`。

Debug 构建使用 `-g -O0`；Release 构建使用 `-O3 -DNDEBUG`，并启用
LTO/strip 相关选项。测试 Debug 构建还会加入 coverage 选项。

## 5. 运行应用画像

### 5.1 COM3 原始回显

```powershell
.\debug.ps1 -Run -Com3Echo
# 或
.\debug.bat com3_echo
```

该命令会：

1. 执行 Release 构建。
2. 配置 SSH 免密登录。
3. 同步目标板时间。
4. 上传程序到 `/home/sast8/tmp/MB_DDF_v2`。
5. 通过 SSH 在当前终端直接运行。

指定目标板：

```powershell
.\debug.ps1 -Run -Com3Echo `
  -RemoteHost 192.168.1.50 `
  -RemoteUser root `
  -RemoteDir /home/sast8/tmp
```

回显画像只启动 `XdmaTransport + ComDevice`，固定使用 `/dev/xdma0`、COM3 偏移
`0xC0000`、窗口 `0x40000`、event 2、Level 中断和 `614400 / 8E1 / 无流控`。FPGA负责帧头、
长度和 CRC，C++ 只原样回显有效 payload；该画像不初始化 DDS、测试服务或 Demo，
也不保存和恢复原 COM 配置。

### 5.2 运行原 DDS/硬件 Demo

使用批处理入口时直接执行：

```powershell
.\debug.bat hw_run
```

它会构建独立的 `DEMO` 画像，并在目标板设置
`MB_DDF_HW_FULL_DEMO=1`，运行包含主发动机2点火在内的全硬件能力流程。直接调用
`debug.ps1` 时的等价方式为：

```powershell
.\debug.ps1 -Run -FullHardware
```

全硬件流程包含不可逆动作，只能在已隔离且允许执行硬件写入和点火的目标板上运行。
只需硬件只读巡检时，先执行 `build.ps1 hw_release`，再按
[Demo 使用说明的手工部署步骤](demo-usage.md#33-手工部署)运行 DEMO 画像，不设置
`MB_DDF_HW_FULL_DEMO`。不带 `-FullHardware` 的 `debug.ps1` 选择的是 ECHO，不是 DEMO。

完整硬件 Demo 的存储测试只使用 CPU `/dev/spidev0.0` 上的 Micron N25Q512A 可恢复
4 KiB 测试；`hw_run` 会自动执行该 CPU SPI 流程：
先读取并校验 JEDEC ID `20 BA 20`，再用 `13h + 四字节地址` 直接读取并完整备份测试
子扇区，然后擦除、写入、读回，最后按页恢复并校验原内容。缺省地址为最后一个子扇区
`0x03FFF000`；板级预留地址不同时可指定：

```powershell
.\debug.ps1 -Run -FullHardware -SpiFlashTestAddress 0x03FFF000
```

地址只接受十进制或 `0x` 十六进制，必须 4 KiB 对齐且不大于 `0x03FFF000`。
`debug.ps1` 会将其转为远端环境变量 `MB_DDF_HW_SPI_FLASH_TEST_ADDRESS`。

备份恢复只能防止正常流程破坏原数据；测试期间进程被终止或板卡掉电仍可能留下被擦除
或部分恢复的 4 KiB。运行前仍应确认日志打印的范围可从外部备份重刷。v4 寄存器事实源、
两个 update 只读 Device 与 smoke 范围见
[硬件层详细设计](../design/hardware-layer-architecture.md)。

SPI Transport 会持有 advisory `flock`，并在退出前恢复原 mode、位序、字宽和速率；
显式恢复失败会使 Demo 失败。`flock` 不能约束不合作的进程，运行前仍须停止其他
`/dev/spidev0.0` 使用者。

也可以先执行 `.\build.ps1 hw_release`，再手工 `scp` 到目标板运行。
硬件 Demo 的设备路径和只读行为见
[Demo 使用说明](demo-usage.md)。

### 5.3 产品硬件测试服务

测试服务使用独立的 `HW_TEST` 画像和同一个 COM3 控制口，不初始化 Adapter 或 Demo；
DDS Core 仅供舵机连续实测 bridge 使用，同一构建另产出用户独立启停的
`MB_DDF_v2_HelmControl`。使用：

```powershell
.\debug.ps1 -Run -HardwareTest
# 或
.\debug.bat hw_test_run
```

`-Com3Echo`、`-HardwareTest` 和 `-FullHardware` 三个运行选项互斥。测试服务只报告
执行完成、执行失败或通信失败，不根据测量值设置硬件通过阈值。本版不提供安全确认、
状态恢复、自动重试、夹具检测或报告导出；只能在已隔离且允许协议所含写操作的目标板运行。
总线 link 0/1/2/3 分别对应 COM1/2/3/4；COM3 控制口占用 link 2，该 link 必须在打开
硬件前返回 `CHANNEL_INVALID`，只有 link 0/1/3 可测。BUS_LOOP 使用内部回环；BUS_ECHO
由独立 PC/夹具在被测 COM 原样回发 119 字节完整物理帧，DUT 比较其中的 114 字节 payload。

`debug.ps1 -Run -HardwareTest` 只部署并运行产品测试服务，不代替用户启动或停止
`MB_DDF_v2_HelmControl`。两者可按任意顺序独立运行，`HELM_BOARD_TEST 07/02` 与
`HELM_STREAM 07/10/01/11` 不互斥、不共享生命周期状态。

## 6. 测试

### 6.1 DDS 测试

```powershell
# 只交叉构建 DDS 测试目标，不部署
.\build.ps1 dds_tests

# 静态检查和交叉编译
.\tests\test-dds-only.ps1 -BuildOnly

# 构建、部署、执行
.\tests\test-dds-only.ps1

# 只执行部分 gtest
.\tests\test-deploy.ps1 -TestFilter "RingBuffer*"
.\tests\test-deploy.ps1 -TestFilter "DomainGateway*"
```

### 6.2 全量目标板测试

```powershell
.\tests\test-all.ps1 -BuildOnly
.\tests\test-all.ps1
```

`test-all.ps1` 依次运行：

- `MB_DDF_v2_Tests`：DDS 单元/集成测试。
- `MB_DDF_v2_HardwareTests`：目标板 POSIX、IPC、压力和性能测试。
- `MB_DDF_HW_Tests`：硬件层、产品协议和硬件测试服务单元测试。
- `MB_DDF_DytDebug_Tests`：导引头帧收发节奏、COM1 配置投影和自描述日志单元测试。
- `MB_DDF_HW_Smoke`：默认只读的真实 XDMA smoke。

这里的 `MB_DDF_v2_HardwareTests` 是“在目标板运行的 DDS 实机测试”，不等同于
访问 XDMA IP 核的 `MB_DDF_HW_Smoke`。

### 6.3 硬件层测试

```powershell
.\build.ps1 hw_tests
.\tests\test-deploy.ps1 -TestBinaryName MB_DDF_HW_Tests
.\tests\test-deploy.ps1 -TestBinaryName MB_DDF_DytDebug_Tests
.\tests\test-deploy.ps1 -TestBinaryName MB_DDF_HW_Smoke
.\tests\test-deploy.ps1 -TestBinaryName MB_DDF_HW_Tests `
  -TestFilter 'ProductProtocol*:*HardwareTestService*'
```

真实硬件 smoke 程序默认只读。需要执行 COM 回环时可通过
`-TestArguments` 传参：

```powershell
.\tests\test-deploy.ps1 `
  -TestBinaryName MB_DDF_HW_Smoke `
  -TestArguments "--com-loopback --com-index all --timeout-us 1000000"
```

详细测试范围见 [DDS 测试说明](../tests/dds-test-guide.md)。

提交前至少应依次交叉构建 `dds_tests`、`hw_tests` 和三个应用画像；构建动作及产物目录
见[构建动作](#41-构建动作)。产品协议 CSV、PC 测试和目标板过滤测试命令见
[测试快速说明](../tests/README.md)。

## 7. 远程调试

### 7.1 启动 gdbserver

```powershell
.\debug.ps1
```

常用参数：

```powershell
.\debug.ps1 -RemoteHost 192.168.1.50
.\debug.ps1 -RemoteGdbPort 2345
.\debug.ps1 -SourceDebug
.\debug.ps1 -ForegroundGdbserver
.\debug.ps1 -DryRun
```

默认流程：

1. 执行 Debug 构建。
2. 更新 `.vscode/launch.json`：
   - `program`
   - `miDebuggerPath`
   - `set sysroot`
3. 首次上传 `tools/gdbserver`。
4. 上传本地 Debug 可执行文件。
5. 终止目标板上的旧程序和旧 gdbserver。
6. 在目标板启动 `gdbserver :1234`。
7. 提示用户在 VS Code 中按 `F5`。

`debug.bat` 会额外传入 `-ForegroundGdbserver`：

```powershell
.\debug.bat
.\debug.bat source_debug
.\debug.bat run
.\debug.bat com3_echo
.\debug.bat hw_test_run
.\debug.bat hw_run
```

`run`/`com3_echo` 使用 ECHO Release 画像，`hw_test_run` 使用 HW_TEST Release
画像，`hw_run` 使用 DEMO Release 画像并在目标板启用全硬件能力流程。未带子命令时
使用 ECHO Debug 画像进入 gdbserver 调试流程。

`-SourceDebug`（或 `debug.bat source_debug`）选择递归源码调试组合并保持 Debug 配置，
默认部署后启动 gdbserver；需要跳过 gdbserver、直接运行 `src/main.cpp` 入口时使用
`.\debug.ps1 -SourceDebug -Run`。该参数与 `-Com3Echo`、`-HardwareTest`、
`-FullHardware` 互斥。

### 7.2 VS Code 检查项

启动调试前确认 `.vscode/launch.json` 中：

- `program` 指向本地 Debug ELF。
- `miDebuggerPath` 指向 AArch64 GDB。
- `miDebuggerServerAddress` 与目标板 IP、端口一致。
- `set sysroot` 指向目标板匹配的 sysroot。

`debug.ps1` 会更新前三类本地路径配置，但
`miDebuggerServerAddress` 仍应在更换 IP/端口后人工核对。

### 7.3 gdbserver 上传缓存

`.vscode/.gdbserver_uploaded` 表示已经上传过 gdbserver。更新
`tools/gdbserver` 后执行：

```powershell
.\build.ps1 clean
```

或手工删除该标记文件，再运行 `debug.ps1`。

## 8. 常见问题

### 找不到交叉编译器

设置 `CROSS_CXX_COMPILER` 或 `TOOLCHAIN_BIN`，并确认工具链前缀是
`aarch64-none-linux-gnu-`。

### 找不到 sysroot

显式传入：

```powershell
.\build.ps1 debug -Sysroot "D:\sysroots\aarch64"
```

### CMake 找不到 make

设置：

```powershell
$env:MAKE_PATH = "D:\tools\make.exe"
```

### 部署失败

依次检查：

```powershell
ping 192.168.1.29
ssh root@192.168.1.29
scp .\README.md root@192.168.1.29:/tmp/
```

### DDS 初始化失败

常见原因是旧进程仍在运行、共享内存大小不一致或共享内存版本不一致。
停止相关进程后，在目标板清理：

```powershell
ssh root@192.168.1.29 "rm -f /dev/shm/MB_DDF_V2_SHM /dev/shm/sem.MB_DDF_V2_SHM_sem"
```

### 硬件 Demo 打不开设备

确认：

- `BUILD_HARDWARE_LAYER=ON`。
- `/dev/xdma0_user` 存在。
- COM 检查时 `/dev/xdma0_events_0` 到 `_3` 存在。
- 当前用户具有设备节点读写权限。
- FPGA 地址布局与硬件详细设计中的映射一致。
