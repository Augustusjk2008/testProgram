# Demo 使用说明

## 1. Demo 入口

主程序入口是 `src/main.cpp`，它只初始化日志并按编译期画像分派。原教学 Demo 编排位于：

```text
src/MB_DDF_Demo/DemoRunner.cpp
src/MB_DDF_Demo/DdsExamples.cpp
src/MB_DDF_Demo/HardwareExamples.cpp
```

主程序没有命令行参数。DEMO 画像启动后按固定顺序执行全部示例；单个示例失败或抛异常
不会阻止后续示例运行，最后统一输出通过、跳过和失败数量。硬件部分分为默认只读巡检
和显式启用的全能力示例两个区段。

## 2. 三种应用画像

### 2.1 ECHO

```powershell
.\build.ps1 debug
.\build.ps1 release
```

默认画像只编入 COM3 原始回显入口，启用 `MB_DDF_HW`、关闭 DDS Adapter，不编入 Demo
或产品测试处理器。

### 2.2 HW_TEST

```powershell
.\build.ps1 hw_test_debug
.\build.ps1 hw_test_release
```

该画像编入 COM3 产品协议服务、系统测试、硬件测试 provider 和舵机 DDS bridge，启用
硬件层并关闭 DDS Adapter；同一构建另产出用户独立启停的 `MB_DDF_v2_HelmControl`。

`IMU_STREAM` 复用该 HW_TEST 的 COM3 产品协议服务，但不是 `test_pyqt` 的第 12 个页面，
也不进入旧工具的“执行全部”或“连续”功能。宿主工程通过
`configs/mbddf_imu_stream.testcfg.json` 只以 `device_stream` 运行：PC 发送一次 `09/10`，
DUT 从 COM4（偏移 `0x100000`、event 3、`921600/8E1`）读取 FPGA 已处理的 59 字节
payload，经 COM3 主动上送完整 `09/01`，停止时最多发送一次 `09/11`。至少一帧有效反馈
通过，0 帧失败；可选 UTF-8-SIG/TSV 固定列保存以宿主契约为准。流活动期间 COM4 被临时
占用，BUS link 5 返回 `TASK_BUSY`。

`HELM_STREAM` 由根仓 `configs/mbddf_helm_stream.testcfg.json` 以 `device_stream` 运行：
PC 发送一次 `07/10`，DUT 以 1 ms 周期生成四路舵角指令，经
`local:://helm_command`/`local:://helm_feedback` 与独立 `MB_DDF_v2_HelmControl`
交互，并把 1..5 个完整 DDS 反馈样本组成 `07/01` 主动回告，停止时发送一次 `07/11`。
扫频总时长可配，超时后命令归零而反馈继续到 STOP。测试服务不管理舵控程序，二者可由
用户按任意顺序独立启停；连续实测与 `HELM_BOARD_TEST 07/02` 不互斥、不绑定。

### 2.3 DEMO

```powershell
.\build.ps1 hw_debug
.\build.ps1 hw_release
```

该画像定义 `MB_DDF_DEMO_WITH_HARDWARE=1`，主程序会链接 `MB_DDF_HW` 并执行
XDMA 只读检查。默认构建同时链接 `MB_DDF_HW_DDS_Adapter`；全能力示例仍需在目标板
显式设置 `MB_DDF_HW_FULL_DEMO=1`。

## 3. 部署与运行

### 3.1 一键运行 COM3 回显

```powershell
.\debug.ps1 -Run -Com3Echo
# 或
.\debug.bat com3_echo
```

默认部署到：

```text
root@192.168.1.29:/home/sast8/tmp/MB_DDF_v2
```

指定目标板：

```powershell
.\debug.ps1 -Run -Com3Echo `
  -RemoteHost 192.168.1.50 `
  -RemoteDir /home/sast8/tmp
```

### 3.2 一键运行带硬件层的 Demo

推荐直接使用带硬件层的批处理入口：

```powershell
.\debug.bat hw_run
```

该入口把 `-Run -FullHardware` 传给 `debug.ps1`；构建脚本以完整显式参数选择独立的
DEMO Release 画像，并在目标板启动命令中设置 `MB_DDF_HW_FULL_DEMO=1`。随后执行部署和
全硬件能力运行，不会污染 ECHO 或 HW_TEST 的 CMake 缓存。

`hw_run` 会执行硬件写入、ADS1258 清错、主发动机2点火、COM 回环、XDMA DMA
回读，以及 CPU `/dev/spidev0.0` 上的 Micron N25Q512A 读写测试。点火不可撤销，只能
在已隔离且允许执行这些动作的目标板上运行。SPI Flash 测试会永久改写非易失配置以
关闭 `#HOLD`，并临时改写一个 4 KiB 子扇区；子扇区会先完整备份并在测试结束时恢复，
但非易失配置不会自动恢复，测试期间断电仍可能破坏该 4 KiB 区域。

直接调用 `debug.ps1` 的等价方式为：

```powershell
.\debug.ps1 -Run -FullHardware
```

缺省测试地址为 N25Q512A 最后一个 4 KiB 子扇区 `0x03FFF000`。如果板级预留区不同，
应显式传入 4 KiB 对齐地址：

```powershell
.\debug.ps1 -Run -FullHardware -SpiFlashTestAddress 0x03FFF000
```

地址必须位于 `0x00000000..0x03FFF000` 且 4 KiB 对齐，非法值会在发出任何写命令前
失败。旧 `-FlashChipEraseTest/-FlashChipEraseConfirm` 参数只为脚本兼容保留，现已
废弃并忽略；新 Demo 永远不会发送 ChipErase/Bulk Erase。

`debug.bat hw_run` 和 `-FullHardware` 只选择独立的 DEMO 画像并启用全能力区段，不会
污染 ECHO 或 HW_TEST 的 CMake 缓存。对已经部署的 DEMO 画像二进制，也可以手工执行：

```powershell
ssh root@192.168.1.29 `
  "cd /home/sast8/tmp && MB_DDF_HW_FULL_DEMO=1 ./MB_DDF_v2"
```

指定 XDMA 设备前缀和用于备份/回读/恢复的 DMA 设备地址：

```powershell
ssh root@192.168.1.29 `
  "cd /home/sast8/tmp && MB_DDF_XDMA_DEVICE=/dev/xdma1 MB_DDF_HW_FULL_DEMO=1 MB_DDF_HW_DMA_OFFSET=0x100000 ./MB_DDF_v2"
```

### 3.3 手工部署

```powershell
.\build.ps1 hw_release

scp .\build\aarch64\demo\Release\MB_DDF_v2 `
  root@192.168.1.29:/home/sast8/tmp/MB_DDF_v2

ssh root@192.168.1.29 `
  "chmod +x /home/sast8/tmp/MB_DDF_v2 && /home/sast8/tmp/MB_DDF_v2"
```

### 3.4 产品硬件测试服务

板端测试服务使用独立 HW_TEST 画像：

```powershell
.\debug.ps1 -Run -HardwareTest
# 或
.\debug.bat hw_test_run
```

服务固定占用 COM3（`/dev/xdma0`、偏移 `0xC0000`、映射窗口 `0x40000`、event 2、
Level 中断），串口配置固定为 `614400 / 8E1 / 无流控`，只处理产品协议，
不初始化 Adapter 或 Demo；DDS Core 只在舵机 bridge 需要时使用。协议命令可能执行 DIDO、
DH、舵控板级输出、DDS 舵机连续实测和 SPI Flash 写操作；本版不做安全确认、状态恢复、协议级自动重试或断链
清理，只能在已隔离且允许写入硬件的目标板运行。COM TX `Busy` 时服务只会有界重交尚未
被硬件接收的同一帧和同一序号。

硬件测试模式的关键边界：link 0/1 分别在 `192.168.1.29:3003`、
`192.168.7.29:3003` 本机自环；link 2/3/4/5 分别对应 COM1/2/3/4，其中 COM3 控制口
占用 link 4 并返回 `CHANNEL_INVALID`，“执行全部”只包含 link 0/1/2/3/5/6；link 6 LOOP
只读取 JEDEC ID，ECHO 明确失败；“执行全部”先配置 DH0=80 ms、DH1..22=63 ms 的 23 路
脉宽，再按电源使能、回线使能、Multiple 模式和通道位图执行 DH 控制；23 路遥测电压
从 ADS1258 `raw[1]`、`raw[4..25]` 读取，板端完成分段定标后按
`0.001 V/LSB` 回告。DH 控制默认回告 50 次、间隔 2500 us，间隔不得低于 2500 us；板端
按每帧实际采样起点计算下一截止点并在采样后立即发送，串口发送时间不额外叠加到间隔。
当前 PC 导航和“执行全部”使用舵控板级命令
`HELM_BOARD_TEST (07/02)`：B9 低 4 位保留、高 4 位写四路方向，B10-B13 分别下发四路
整数占空比 `0..100%`，读取四路 AD7606，并回读 `pwm_duty_match`、raw duty、peak、方向、
使能及 PWM/AD 状态。板端按 `raw=(uint64_t(peak)*percent+50)/100` 换算占空比；命令结束后
保持输出和 AD7606 状态，不自动恢复。`07/02` 不查询连续 DDS 舵机实测的状态，也不会
因其活动返回 `TASK_BUSY`；peak 为 0 或写后回读不一致时返回
`REG_READ_WRITE_FAILED (0x0201)`。

Web 主基线通过 `HELM_START (07/10)` 运行舵机连续实测：`start` 单位为 rad，
`sweep_duration_s` 指定对数扫频总时长；DUT 每 1 ms 经 DDS 发布一条完整指令，
`HELM_FEEDBACK (07/01)` 每帧批量携带 1..5 个完整 41 字节反馈样本，
`HELM_STOP (07/11)` 只关闭本次 DDS bridge。`MB_DDF_v2_HelmControl` 由用户独立操作，
测试服务不启动、停止或占有它，启动顺序不限。旧 PyQt 导航仍只显示 `07/02` 板级页。
SYSTEM_STATUS 的 `net_init_time` 当前固定为 `0 s`；RK3588 温度来自 `center_thermal`
hwmon，K7 温度来自 XADC 全局 `0x150000` 的局部 `0x200`，缺失实际温度源或其他未确认
硬件映射均返回明确错误。完整合同见
[COM3 产品协议设计](../design/product_protocol_csv/codedesign.md)。

### 3.5 PC 串口工具

Windows 串口工具位于 `test_pyqt`，连接板端 COM3 对应的 Windows 串口。板端 COM3
不表示必须选择 Windows 的 `COM3`，应选择与板端 COM3 物理连接的主机串口。窗口顶部
是紧凑连接状态栏，显示当前 Windows COM、连接状态和解析模式，并提供连接/断开、
执行全部和停止操作；详细串口配置及完整彩色日志集中在“连接与日志”页。启动：

```powershell
.\test_pyqt\run.bat
# 或
.\test_pyqt\run.ps1
```

需要生成免 Python 安装的独立目录时，使用同一个固定 `pyqt5_env` 环境执行：

```powershell
.\test_pyqt\run.bat package
# 或
.\test_pyqt\run.ps1 -Package
```

打包结果位于
`build\test_pyqt\dist\MB_DDF_HW_Test_PC\MB_DDF_HW_Test_PC.exe`，协议 CSV 位于产物内部，
不需要另行复制。当前 `pyqt5_env` 安装的 PyInstaller 6.17.0 尚未在 Windows 7 SP1 x64
实机验证；交付 Win7 前必须补做启动、端口枚举、串口打开和协议收发验证。

工具默认使用 `614400 / 8 / Even / 1 / None`；界面保留通用串口参数，但联调本工程
COM3 画像时必须保持该默认值。发送区只接受十六进制 payload。线序帧为
`[55 AA][LEN][DATA][CRC_LO CRC_HI]`，CRC-16/XMODEM 的校验范围
是长度字节和 payload；单次 payload 为 1..255 字节。半帧超过 500 ms 会复位解析器，
CRC 错误不会被报告为有效回显。

左侧使用单层纵向标签，依次为“连接与日志”“串口回显”“系统状态”“内存”
“SPI Flash”“总线”“DI”“DO”“电气健康”“DH 脉宽配置”“DH 控制”
“舵控板级”和“定时器”。进入“串口回显”页会选择回显解析，进入任一硬件测试页会选择
产品协议；“连接与日志”页不改变当前解析模式。切换标签不会取消在途测试或清空已显示
结果。舵机连续实测由根仓 Web 主基线操作，旧 PyQt 导航不创建该设备流页面。

串口回显页联调前先运行 `debug.bat com3_echo`；任一硬件测试页联调前先运行
`debug.bat hw_test_run`。串口回显支持 1..255 字节 payload；旧 PyQt 可见硬件测试页只
发送其协议规定的 48/123 字节数据段，一次只保留一个普通待响应请求，并把 DH 突发单独路由。
每个硬件测试页的“执行”按钮旁提供“连续”按钮；连续是用户显式开启的重复执行，
每轮请求收到终态后等待 200 ms 再发下一轮，不会与在途请求重叠。再次点击“连续”
或点击顶部“停止”会取消后续轮次；涉及写入的页面仍只能在已隔离且允许写入的目标板上使用。
DH 控制页完整缓存 23 路点火状态和遥测电压，但曲线只绘制本次请求选中的通道；回告较多时
只批量刷新界面，原始回告仍完整缓存，超过 5000 个显示点时均匀抽样绘图。用户可指定保存
目录；完成、失败或停止时，PC 将完整去重回告写入 UTF-8-SIG TSV 长表，文件名固定为
`DH_data_YYYYMMDD_HHMMSS_ffffff.txt`。11 个硬件测试页分别保存各自的参数、状态和最近
结果，每页通过“结果 / 原始字段”视图
展示专用指标、位图、表格或图表以及协议原始字段，不再共用全局响应表。顶部“执行全部”
会收集各页当前参数，其中舵控项执行 `07/02` 板级测试；任一页参数无效时会定位该页且
不发送请求。

界面显示未执行、执行中、执行完成、执行失败和通信失败；后三项是终态。它不根据测量值
判定硬件通过与否。DH 控制页保留多帧报告并允许选择查看；舵控板级页分别设置四路 PWM
整数占空比 `0..100%` 和方向，并显示请求百分比、`pwm_duty_match`、raw duty、peak、
方向回读及四路 AD7606。保留的旧扫频页面仍维持
原有语义：HELM_START/HELM_STOP ACK 不覆盖反馈曲线，停止时等待 HELM_STOP 响应，板端
返回清理错误时不得显示为执行完成。定时器的 TIMER_STOP ACK 不覆盖 START 阶段的抖动
统计。

电气健康页从响应 B31-B32 读取 XADC `value_YX`，只显示 `activate_bits.bit0` 并命名为
“BC激活好”；bit1..7 固定为 0 且不显示。舵控板级页读取 AD7606 四路舵反馈。板端直接
发送 XADC 12 位 ADC code 和 AD7606 有符号 16 位 raw，PC 分别按
`10.09/4096 V/code`、`10/65536 V/code` 换算。
ADS1258 由板端按当前临时定标值换算，完整公式统一见
[COM3 产品协议设计](../design/product_protocol_csv/codedesign.md)；电气健康的
`c_volt/b_volt/v28_5` 按 `0.01 V/LSB` 编码。“BC激活好”对应的 `activate_bits.bit0` 来自 DH
`BatteryStatus` 高字节 `0xAA/0xFF`，其他高字节值返回 `REG_READ_WRITE_FAILED`。
电气健康页选择“连续”后会在页面指定目录累计每轮完整响应；停止、断连或关闭窗口时一次性
保存为 UTF-8-SIG TSV，文件名为 `ElectricalHealth_data_YYYYMMDD_HHMMSS_ffffff.txt`。
每行对应一次响应，包含 PC 接收相对时间（微秒）、序号、status/err_code、10 路已解码电压、
原始 `activate_bits` 和 `bc_activate_good`（bit0）；失败响应的测量字段写为 `NA`。

## 4. DDS 示例

主程序先调用 `DDSCore::initialize()`，默认连接
`/MB_DDF_V2_SHM` 这块 128 MiB 共享内存。`DDSShutdownGuard` 确保 `main`
正常返回或抛异常时都会调用 `shutdown()`。

### 4.1 同步发布和轮询

Topic：

```text
demo://dds/synchronous
```

流程：

1. `create_writer()`。
2. `create_reader()`。
3. `writer->write()` 发布固定结构体。
4. `dds.data_poll(..., latest=true)` 非阻塞读取。
5. 校验字节数和完整 payload。
6. 显式 `unsubscribe()`。

该示例展示最短的共享内存发布/订阅路径。

### 4.2 阻塞读取

Topic：

```text
demo://dds/blocking
```

流程：

1. 主线程先创建 reader。
2. 生产者线程等待 50 ms 后发布。
3. 主线程使用 `ReadStrategy::Blocking`，总等待上限 500 ms。
4. 收到消息后校验内容并 `join` 生产者线程。

它用于演示 condition variable 唤醒和带超时读取。

### 4.3 异步回调

Topic：

```text
demo://dds/callback
```

`create_reader(topic, true, callback)` 会启动 Subscriber 工作线程。主线程使用自己的
`condition_variable` 等待回调完成。

回调内先检查指针、大小和本次运行 ID，再复制数据。回调返回后不继续持有 DDS
共享内存中的消息指针。

### 4.4 零拷贝发布

Topic：

```text
demo://dds/zero-copy
```

该示例依次演示：

- `begin_message()` + `data()` + `commit()`。
- `publish_fill()`。

两种方式都直接在 RingBuffer 写槽内构造 payload。写槽持有 Topic 写锁，实际业务
代码不应在填充期间执行耗时操作。

### 4.5 观察者和本地序列号

Topic：

```text
demo://dds/observer
```

流程：

1. `create_observer()` 建立内部观察订阅。
2. `publish_and_get_sequence()` 发布并取得 DDS 本地序列号。
3. 观察者回调取得同一条消息的 `LocalMessageView.sequence`。
4. 比较发布端和观察端序列号。

该接口主要服务于跨域网关的去重和回灌抑制。

### 4.6 Topic 发现

最后调用 `list_topics()`，输出当前共享内存中所有 Topic 的：

- Topic ID。
- Topic 名。
- RingBuffer 大小。

并检查前五个 Demo Topic 是否全部存在。

## 5. 历史消息处理

DDS 共享内存在进程退出后不会自动删除，因此重复运行 Demo 时可能看到上次运行的
消息。

`DdsExamples.cpp` 给每次运行生成 `run_id`，由进程号、单调时钟计数和本地计数混合
得到。阻塞读和回调只接受本次 `run_id` 的消息，历史消息会被忽略。

如果需要完全清空状态，在目标板停止所有相关进程后执行：

```powershell
ssh root@192.168.1.29 `
  "rm -f /dev/shm/MB_DDF_V2_SHM /dev/shm/sem.MB_DDF_V2_SHM_sem"
```

## 6. 硬件示例

默认只读区段直接构造 `XdmaTransport` 和 Device；全能力区段还会通过 DDS Adapter
执行 COM1 回环。

默认 XDMA 前缀：

```text
/dev/xdma0
```

可以在目标板覆盖：

```sh
MB_DDF_XDMA_DEVICE=/dev/xdma1 ./MB_DDF_v2
```

### 6.1 默认只读巡检

| 设备 | 全局映射偏移 | Demo 行为 |
|---|---:|---|
| PWM | `0x00000` | 通信检查、读取配置和四路输出 |
| AD7606 | `0x10000` | 通信检查、读取八路原始采样 |
| ADS1258 | `0x20000` | 通信检查、读取 32 路数据和错误计数 |
| DH | `0x30000` | 通信检查、读取 48 路反馈和电池状态 |
| COM1 | `0x40000` | 打开 event 0，读取配置和错误状态 |
| COM2 | `0x80000` | 打开 event 1，读取配置和错误状态 |
| COM3 | `0xC0000` | 打开 event 2，读取配置和错误状态 |
| COM4 | `0x100000` | 打开 event 3，读取配置和错误状态 |
| DIDO | `0x140000` | 通信检查、读取输入和输出位图 |
| XADC | `0x150000` | 默认 Demo 不映射；HW_TEST/硬件 smoke 只读 `value_YX` |
| Flash | `0x160000` | 默认 Demo 不映射；硬件 smoke 另行只读检查 |

默认区段只读，不调用：

- PWM 配置或输出更新。
- AD7606/ADS1258 配置、复位或清错。
- DIDO 输出更新。
- DH 点火、脉宽和模式配置。
- COM 发送、配置、清错或复位。
- XADC 触发转换、阈值配置或复位。
- Flash 命令、触发、完成标志清除或数据区读取。

DH 示例只调用 `read_feedback()`，不会调用 `fire()` 或
`fire_multiple()`。

### 6.2 全能力示例

设置 `MB_DDF_HW_FULL_DEMO=1` 后，主程序在只读巡检之后继续执行以下代表性能力。
同构设备或相似接口只调用一个：COM1 代表 COM1 到 COM4，PWM 选择归一化输出而不再
重复演示原始输出，DH 选择主发动机2单通道点火而不再重复演示批量点火。

| 能力组 | 代表性调用 | 示例结束处理 |
|---|---|---|
| PWM | 暂停更新、配置载波、无符号占空比模式、归一化四路输出、恢复更新 | 恢复载波配置、原始输出和更新状态 |
| AD7606 | 保存组合状态、切换滤波配置、复位采集、读取组合状态 | 恢复完整配置 |
| ADS1258 | 读取/写回完整配置、清除错误计数、读取数据快照 | 恢复配置；错误计数无法恢复 |
| DIDO | 翻转 DO0、读取输入输出快照 | 恢复全部 16 路输出 |
| DH | 配置时基、保持 CH2 脉宽、执行主发动机2五步使能与点火、读取反馈 | 反向关闭使能链并恢复时基、模式和 CH2 脉宽；点火结果无法恢复 |
| COM | COM1 配置内部回环和 Level 中断、清错、使能接收、查询 event fd 和 MTU；Adapter 预检后连续校验 16 个唯一 payload | 每轮严格校验长度和全部字节，跑满后恢复配置；正常流程不执行锁存式软件复位 |
| DDS Adapter | 通过 `ComExternalEndpoint` 调用 `send/receive/mtu` | 与 COM1 共用恢复流程 |
| XDMA DMA | DMA0 C2H 备份 64 字节、H2C 写测试图案、C2H 回读校验 | H2C 写回原 64 字节 |
| CPU SPI Flash | `/dev/spidev0.0` 使用 `13h + 四字节地址` 备份 4 KiB、子扇区擦除、写入并读回 16 字节测试图样 | 再次擦除、按 256 B 页恢复备份并完整校验 |

`ComExternalEndpoint` 是 `ExternalEndpointAdapter` 的 COM 语义别名，二者不重复
演示。`CallbackExternalEndpoint` 提供相同的 `ExternalEndpoint` 契约，只是将后端
换成三个回调，因此也不重复调用。

COM1 多轮回环固定发送 16 个不同的 32 字节 payload，不做失败重试。每轮日志包含轮次；
不匹配时还包含实际长度、首个差异及收发十六进制。测试会继续完成余下轮次，但任一失败
都会保留到最终结果。通过摘要必须为 `16/16，odd=8/8，even=8/8`；严格奇偶交替失败是
“一个 RX bank 返回陈旧数据”的特征判据。该判据只覆盖 COM1 内部回环，不能代替 COM3
外部线缆和产品协议验收。`hw_run` 还执行其他能力组，因此整套命令可因 DH、SPI 等独立
故障返回非零；应以 COM1 分组摘要判断本项结果。

全能力区段固定使用以下代表对象：

- PWM 四路输出。
- DIDO DO0。
- DH 主发动机2（CH2）。
- COM1 和 event 0。
- XDMA H2C0/C2H0，传输 64 字节。
- CPU `/dev/spidev0.0` 上的 N25Q512A；可恢复 4 KiB 流程固定在全能力序列末尾。

`MB_DDF_HW_DMA_OFFSET` 接受十进制或 `0x` 开头的十六进制地址，默认值为 `0`。该地址
必须指向 H2C0/C2H0 均可访问且允许短暂改写的同一片设备内存。Demo 会先备份、再
写入、回读并恢复；进程被强制终止或板卡在恢复前掉电时，无法保证恢复。

CPU SPI Flash 测试随 `MB_DDF_HW_FULL_DEMO=1` 自动执行。可选地址变量为：

```text
MB_DDF_HW_FULL_DEMO=1
MB_DDF_HW_SPI_FLASH_TEST_ADDRESS=0x03FFF000
```

Demo 打开设备后先以两个独立 CS 事务发送 `06h` 和 `B1h EFh FFh`，在非易失配置中
永久关闭 `#HOLD`；该设置跨复位和掉电保持，流程结束时不恢复。随后用两条独立 `70h`
（命令间切换 CS）确认两个 die 均为 Ready，并将第一笔内存
数据事务直接发往调用方确认的测试地址。地址读取前还会用 `9Fh` 读取 JEDEC ID；只有
ID 为 Micron N25Q512A 的 `20 BA 20`，并完整读取、备份 4096 字节后才进入擦写。
读、页编程和 4 KiB 擦除分别使用 N25Q512A 专用四字节命令 `13h`、
`12h`、`21h`，不会通过 `B7h/E9h` 改变器件全局地址模式。CPU SPI Flash 仅通过
`/dev/spidev0.0` 访问，不需要额外的 MMIO 选择切换。
每次 Program/Erase 前执行 WREN 并确认 Status Register 的 WEL=1；完成由 Flag Status
bit7=1 判定，同时检查 bits5/4/1 的擦除、编程和保护错误。

流程只有在 4096 字节备份完整成功后才开始修改。擦除后校验全区为 `FF`，写入
`SpiFlash.h` 的 16 字节图样并读回；随后无论测试成功或失败，都再次擦除、按 256 B
页恢复原数据并完整回读校验。若进程被终止或板卡在恢复完成前掉电，软件仍无法保证
恢复；日志会打印准确测试范围，恢复失败时必须停止访问并从外部备份重刷该范围。
若 Status 和两个 die 的 Flag Status 都读到 `FF`，Demo 将其判定为 MISO 保持高电平
或片选/供电/引脚复用未响应，并在首笔内存读取及任何擦写命令前停止。

`SpidevTransport` 会保存并在结束时恢复原 SPI mode、位序、字宽和速率，同时在整个
测试期间持有 advisory `flock`。该锁不能阻止不遵守 `flock` 的其他程序，因此运行
`hw_run` 前必须停止所有其他 `/dev/spidev0.0` 使用者，避免 WREN 与写命令之间被插入
事务或备份内容被并发修改。

ADS1258 清错和 DH 点火也是不可逆动作。启用全能力模式即表示运行环境已隔离，允许
清除当前错误计数并向 DH 主发动机2（CH2）发出一次点火命令。点火依次写入
`0x1 <- 0xA000`、`0x77 <- 0xAAAA`、`0x72 <- 0xBBBB`、`0x75 <- 0xBBBB` 和
`0x13 <- 0xB002`，收到完成反馈后反向关闭两项使能。其他可写
能力即使中途失败也会继续尝试恢复，并在任一操作或恢复失败时将该示例记为 `Failed`。

### 6.3 设备节点要求

至少需要：

```text
/dev/xdma0_user
/dev/xdma0_events_0
/dev/xdma0_events_1
/dev/xdma0_events_2
/dev/xdma0_events_3
```

全能力示例还要求：

```text
/dev/xdma0_h2c_0
/dev/xdma0_c2h_0
/dev/spidev0.0
```

如果任一硬件块打开、通信检查或读取失败，程序仍继续检查其余硬件，最终把硬件示例
记为 `Failed`。

## 7. 输出与退出码

日志默认：

- 级别为 INFO。
- 不显示时间戳。
- 不显示函数名和行号。

每个示例输出统一区段：

```text
========== DDS synchronous publish/poll ==========
[DEMO] ... PASSED
```

最终输出：

```text
========== Demo summary ==========
[DEMO] passed=<n>, skipped=<n>, failed=<n>
```

退出码：

| 退出码 | 含义 |
|---:|---|
| 0 | 所有已执行示例通过；允许存在 `Skipped` |
| 1 | DDS 初始化失败 |
| 2 | `std::exception` 逃逸到 `main` |
| 3 | 未知异常逃逸到 `main` |
| 4 | 至少一个示例失败 |

自动化部署应以退出码和 summary 两者作为判定依据。

## 8. 真实硬件 smoke

主程序 Demo 同时提供只读巡检和显式全能力入口。需要只执行独立 COM 回环检查时，
使用 `MB_DDF_HW_Smoke`：

```powershell
.\build.ps1 hw_tests

.\tests\test-deploy.ps1 `
  -TestBinaryName MB_DDF_HW_Smoke
```

默认仍为只读。显式执行全部 COM 回环：

```powershell
.\tests\test-deploy.ps1 `
  -TestBinaryName MB_DDF_HW_Smoke `
  -TestArguments "--com-loopback --com-index all --timeout-us 1000000"
```

回环程序会保存并恢复 `ComConfig` 覆盖的 COM 持久配置字段，但它会短暂修改设备状态，
只应在允许回环测试的环境中执行。

目标板 XDMA 驱动会在 poll/epoll 报告 event 就绪时消费驱动事件，因此 Transport 直接
返回就绪结果，不再读取 event fd；追加读取会忽略 `O_NONBLOCK` 并等待下一次中断，导致
接收超时失效。COM 接收仍以 Control.D0 判断真实完成；若先遇到旧事件而 D0 尚未完成，
会在调用方给定的总超时内继续等待当前帧。每个 `_events_N` 必须保持单消费者，不得同时
运行两个占用同一 COM event 节点的进程。

XADC 没有 `0xAAAABBBB` 通信签名；smoke 在 `0x150000` 只读 `value_YX`。Flash 使用本次
确认的固定基址 `0x160000`，默认只读取控制器状态和时钟分频，无需额外参数：

```powershell
.\tests\test-deploy.ps1 -TestBinaryName MB_DDF_HW_Smoke
```

该 smoke 不触发 FPGA Flash IP 的 Read/Program/Erase。完整 Demo 不再调用该 IP 的
ChipErase 流程；它改为通过 CPU `/dev/spidev0.0` 测试独立的 N25Q512A，并执行上述
4 KiB 备份和恢复。

## 9. 常见问题

### 需要运行只读硬件 Demo

`debug/release` 现在构建的是 ECHO 画像，不包含 DDS/Demo 编排。请构建 DEMO 画像：

```powershell
.\build.ps1 hw_debug
```

再按[手工部署](#33-手工部署)运行且不要设置 `MB_DDF_HW_FULL_DEMO`。只有在明确允许点火、
清错和写 Flash 等危险动作时才使用 `.\debug.bat hw_run`。

### DDS 示例读取到历史消息

正常情况下 Demo 会按 `run_id` 忽略历史消息。若共享内存版本、大小或内容异常，停止
所有 DDS 进程后清理 `/dev/shm` 对象。

### COM 打开失败

确认对应 `_events_N` 节点存在且当前用户有权限。COM Transport 映射长度是
`0x40000`，不能按其他设备的 `0x10000` 配置。

### 通信签名不匹配

PWM、AD7606、ADS1258、DH、DIDO 的局部 offset 0 应返回
`0xAAAABBBB`。不匹配通常表示：

- FPGA bitstream 与软件地址表版本不同。
- `user_offset` 配置错误。
- 设备节点对应的 FPGA 实例错误。

COM 和 FPGA `FlashDevice` 当前没有固定签名检查，只验证 Transport 已打开；FPGA
Flash 局部 offset `0` 是读 RAM，不能按其他五类设备解释为签名寄存器。CPU
`SpiFlashDevice::read_jedec_id()` 用于完整 Demo 的器件身份准入；通过 `20 BA 20`
校验后，数据读取仍使用明确的四字节 Flash 地址，不把 `9Fh` 响应解释为地址数据。
