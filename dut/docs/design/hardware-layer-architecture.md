# 硬件层详细设计

## 1. 设计目标

`src/MB_DDF_HW` 将 Linux/XDMA 资源访问、寄存器访问和具体 FPGA IP 核语义从
DDS Core 中分离。

设计原则：

- DDS Core 不包含任何 XDMA 或具体设备头文件。
- Device 不直接管理 fd、`mmap`、`epoll`。
- Transport 不解释 PWM、ADC、DIDO、点火或 COM 协议。
- 所有可失败操作返回 `Result<T>`，不依赖异常作为常规错误通道。
- 构造函数只保存依赖和配置，资源申请由 `open()` 完成。
- 真实硬件与 `NullTransport`/测试 Transport 共用同一 Device 实现。
- DDS 与硬件层通过独立 Adapter 组合。

## 2. 分层与依赖

```text
应用 / Demo
   |                  |
   |                  +----------------------+
   v                                         v
具体 Device API                         DDS Publisher/Subscriber
   |                                         ^
   v                                         |
ITransport / ISpiTransport / IByteEndpoint   MB_DDF_HW_DDS_Adapter
   |                                         ^
   v                                         |
XdmaTransport / SpidevTransport / NullTransport
   |                                         |
   +-----------------------------------------+
   v
Os::Fd / MmapRegion / Epoll / Poll
   |
   v
Linux XDMA/spidev 驱动与 FPGA/CPU SPI 外设
```

目录：

```text
src/MB_DDF_HW/
├─ Core/        Status、Result、Buffer、Timeout
├─ Os/          fd、mmap、poll、epoll RAII
├─ Transport/   寄存器、事件、DMA 和 SPI 全双工抽象
├─ Device/      具体 FPGA IP 与 CPU SPI 器件语义
├─ Endpoint/    通用字节端点
└─ DdsAdapter/  硬件端点到 DDS ExternalEndpoint 的桥接
```

## 3. Core 层

### 3.1 Status

`Status` 包含：

```text
StatusCode code
int errno_value
std::string message
```

当前状态码：

| 状态 | 典型含义 |
|---|---|
| `Ok` | 成功 |
| `InvalidArgument` | 参数、通道、偏移或对齐非法 |
| `NotOpen` | Transport/资源未打开 |
| `OpenFailed` | 设备节点或 epoll 创建失败 |
| `MapFailed` | `mmap` 失败 |
| `IoError` | `read/write/pread/pwrite/epoll` 失败 |
| `Timeout` | 设备层显式超时 |
| `Unsupported` | 未配置 event/DMA 或实现不支持 |
| `Busy` | 设备忙，例如 COM 发送器忙 |
| `BufferTooSmall` | 接收缓冲区不足 |
| `ProtocolError` | 帧长度或硬件错误状态异常 |
| `HardwareFault` | 通信签名不匹配 |

### 3.2 Result

`Result<T>` 保存成功值或失败 `Status`；`Result<void>` 只保存状态。

```cpp
auto result = device.read_state();
if (!result) {
    const auto& status = result.status();
    // status.code / errno_value / message
    return;
}
use(result.value());
```

当前接口约定调用者先判断结果，再访问 `value()`。

### 3.3 Buffer 与 Timeout

Buffer 是非持有视图：

```cpp
BufferView          // const uint8_t* + size
MutableBufferView   // uint8_t* + size
```

调用期间底层内存必须保持有效。

Timeout：

```cpp
Timeout::poll();           // 立即返回
Timeout::after_us(1000);   // 1000 us
Timeout::forever();        // 无限等待
```

## 4. OS 资源封装

### 4.1 Fd

`Os::Fd` 对 Linux fd 采用唯一所有权：

- 析构自动 `close`。
- 支持移动构造和移动赋值。
- `reset()` 释放旧 fd 后接管新 fd。
- `valid()`/`get()` 用于状态检查和系统调用。

### 4.2 MmapRegion

`Os::MmapRegion::map(fd, length, offset)` 执行共享读写映射。对象析构或
`reset()` 时自动 `munmap`。

`user_offset` 必须符合目标驱动和 Linux `mmap` 的页对齐要求；不符合时由
`mmap` 返回 `MapFailed`。

### 4.3 Poll 与 Epoll

`Os::Poll::wait_readable()` 提供简单 fd 等待。

`Os::Epoll` 用于 XDMA event：

1. `epoll_create1(EPOLL_CLOEXEC)`。
2. 把 event fd 以 `EPOLLIN` 注册。
3. 按 `Timeout` 转换为毫秒等待。
4. `EINTR` 时继续，并保持原始截止时间。

## 5. Transport 层

### 5.1 ITransport

`ITransport` 继承 `RegisterAccessor`，统一表达：

```cpp
open()
close()
is_open()

read8/read16/read32
write8/write16/write32

event_fd()
wait_event(timeout)

dma_write(channel, data, device_offset)
dma_read(channel, buffer, device_offset)
```

`event_fd` 和 DMA 有默认 `Unsupported` 实现；`wait_event` 必须由具体
Transport 实现。

寄存器 offset 始终相对于当前 Transport 映射窗口，而不是 FPGA 的全局绝对地址。

### 5.2 XdmaConfig

```cpp
struct XdmaConfig {
    std::string device_path{"/dev/xdma0"};
    uint64_t user_offset{0};
    size_t map_length{4096};
    int h2c_channel{-1};
    int c2h_channel{-1};
    int event_number{-1};
};
```

`device_path` 是前缀。根据配置，`XdmaTransport::open()` 打开：

```text
<device_path>_user
<device_path>_h2c_<channel>
<device_path>_c2h_<channel>
<device_path>_events_<event_number>
```

通道或 event 设为 `-1` 表示不启用该能力。

### 5.3 XdmaTransport 打开与关闭

打开顺序：

```text
close 旧资源
  -> open _user
  -> mmap(user_offset, map_length)
  -> 可选 open _h2c_N
  -> 可选 open _c2h_N
  -> 可选 open _events_N
  -> 为 event fd 创建 Epoll
```

任一步失败都会调用 `close()` 回滚已获得资源。

关闭顺序：

```text
epoll -> event fd -> c2h fd -> h2c fd -> mmap -> user fd
```

Device 只保存 `ITransport&`，不拥有 Transport，所以 Transport 生命周期必须覆盖
Device。

### 5.4 MMIO 访问

访问前统一检查：

- Transport 已打开。
- 16 位 offset 为 2 字节对齐。
- 32 位 offset 为 4 字节对齐。
- `offset + width` 不超过映射窗口。

实际访问使用原生宽度的单次 `volatile` 读写：

- 读后执行 acquire fence。
- 写前执行 release fence。

这避免把 32 位寄存器拆成四次字节访问，适合写触发、写一清零等 MMIO 语义。
当前实现按目标 AArch64/FPGA 的原生小端布局解释数值，不做额外字节序转换。

### 5.5 Event

`wait_event(timeout)`：

1. 通过 epoll 等待 `_events_N` 可读。
2. 超时返回 0。
3. 可读后直接返回 epoll 事件数，不再读取 event fd。

目标板实际加载的 XDMA 2020.2.2 驱动会在 `poll` 报告可读时清零 `events_irq`；event
`read()` 又不检查 `O_NONBLOCK`，而是等待 `events_irq` 再次非零。因此 epoll 就绪后
追加 `read()` 会等待下一次中断，使调用方传入的 `Timeout` 失效。Transport 只负责等待
通知；Device 仍须回读业务状态确认真实完成，并通过设备命令清除硬件中断源和重新使能。

同一个 `_events_N` 必须只有一个消费者，避免多个 poll/epoll 等待者竞争驱动事件。
该约定针对当前目标板驱动；若替换为 poll 不消费 `events_irq` 的 XDMA 驱动，必须同步
调整 Transport、测试和本文档，不能直接复用当前等待实现。

### 5.6 DMA

DMA 使用一次 `pwrite`/`pread`：

- 调用 channel 必须等于配置 channel。
- 未配置对应通道时返回 `Unsupported`。
- `device_offset` 直接作为文件偏移。
- 短读/短写按成功返回实际长度，不自动循环补齐。

需要完整传输语义的 Device 或应用必须检查返回长度。

### 5.7 NullTransport

`NullTransport` 使用进程内字节数组模拟寄存器窗口：

- 需要显式 `open()`。
- 执行同样的对齐和越界检查。
- `wait_event()` 返回 0。
- 不支持真实 DMA 和中断。

它适合验证最终寄存器值。需要验证访问顺序时，测试使用
`tests/hw_unit/support/RecordingTransport.h`。

## 6. Device 层

### 6.1 公共规则

大多数设备的 `check_communication()` 读取局部 offset `0`，要求值为
`0xAAAABBBB`，不匹配返回 `HardwareFault`。

COM 和 XADC 是例外：二者局部 offset `0` 分别是接收 RAM 和 XADC 软件复位写寄存器。
`ComDevice::check_communication()` 只验证 Transport 已打开；`XadcDevice` 不提供签名检查，
调用方打开窗口后直接执行只读采样。其余当前已实现且具有通信寄存器的 Device（包括两个
update 只读 Device）均在局部 offset `0` 校验 `0xAAAABBBB`。

寄存器常量集中在 `Device/Registers`，来源于：

- `docs/design/xxm_ip_addr/origin_v4`：现行原始 Excel 寄存器契约；
  `GOLDEN_image_IP_VERSION IP核通用型地址分配表（公开） .xlsx` 暂不纳入 generated CSV、
  Device API 或实现范围。
- `docs/design/xxm_ip_addr/generated/registers.md`：合并后的完整寄存器文档。
- `Device/Registers/*.h`：代码使用的局部字节偏移。

### 6.2 FPGA 地址窗口

当前代码定义的 FPGA 地址窗口如下；UPDATE image IP version 与 FPGA update state 当前由
硬件 smoke 打开，DEMO 与 HW_TEST 不构造这两个 Device：

| 设备 | `user_offset` | `map_length` | event |
|---|---:|---:|---:|
| PWM | `0x00000` | `0x10000` | 无 |
| AD7606 | `0x10000` | `0x10000` | 无 |
| ADS1258 | `0x20000` | `0x10000` | 无 |
| DH | `0x30000` | `0x10000` | 无 |
| COM1 | `0x40000` | `0x40000` | 0 |
| COM2 | `0x80000` | `0x40000` | 1 |
| COM3 | `0xC0000` | `0x40000` | 2 |
| COM4 | `0x100000` | `0x40000` | 3 |
| DIDO | `0x140000` | `0x10000` | 无 |
| XADC | `0x150000` | `0x10000` | 无 |
| UPDATE image IP version | `0x160000` | `0x10000` | 无 |
| FPGA update state | `0x170000` | `0x10000` | 无 |

例如 AD7606 全局地址 `0x10024` 在 Device 中使用局部 offset `0x24`。

应用可执行文件按编译期画像隔离：

| `MB_DDF_APP_MODE` | 编入的入口 | 硬件层 | DDS Adapter |
|---|---|---|---|
| `ECHO` | COM3 payload 原始回显 | 开 | 关 |
| `HW_TEST` | COM3 产品协议与硬件测试处理器 | 开 | 关 |
| `DEMO` | 原 DDS/硬件教学 Demo | 开 | 开 |

三个画像使用 `build/aarch64/echo`、`hw_test`、`demo` 独立目录。`main.cpp` 只负责
Logger 初始化和编译期入口分派，DDS Core 不依赖测试服务或 `MB_DDF_HW`。

### 6.3 PwmDevice

能力：

- 配置载波计数、峰值和锯齿/三角波。
- 以强类型 `PwmDirectionMode` 配置并回读有符号占空比的方向模式：局部 `0x3C` 的默认值为
  `PositiveToZero=0xAAAA`（正占空比方向 0、负占空比方向 1），可选
  `PositiveToOne=0xBBBB`。
- 以强类型 `PwmChannelMapping` 配置并回读局部 `0x40` 低 16 位的通道映射；默认
  `0x3210`，从低到高四个 nibble 对应逻辑舵 1 至 4 的实际 PWM/方向通道。
- 可只写 `set_channel_mapping()` 调整局部 `0x40`，无需连带覆盖载波、峰值、波形和方向模式。
- 设置无符号占空比模式。
- 开关统一更新。
- 不经 peak/方向/duty 读改流程，直接把四路 enable 写为全关，用于启动安全态。
- 写入四路原始输出。
- 把 `[-1.0, 1.0]` 归一化值转换为方向和占空比。
- 读取完整状态。

关键顺序：

1. 校验每路 duty 不超过当前 peak。
2. 先写四路方向。
3. 再写四路 duty。
4. 最后按需更新 enable。

先方向后占空比用于避免方向切换瞬间产生错误输出。`disable_outputs()` 是启动
关断例外：它直接向 Enable 寄存器写 `0xFFFF`，保证舵控程序在其他 PWM 配置之前
尽早禁止四路输出，随后再关闭 update gate 并写入零 duty。

方向模式只在有符号占空比下有效。`configure()` 写 Carrier、Peak、Waveform、
DirectionMode 和 ChannelMapping，刻意不写 `DutyMode`；需要分离方向/占空比的当前安全
路径仍须显式调用 `set_duty_mode_unsigned()` 写 `0xFFFF`。回读到非 `0xAAAA/0xBBBB` 的
方向模式会返回 `HardwareFault`。

### 6.4 Ad7606Device

能力：

- 采集开关、滤波开关。
- 过采样、时钟和转换时序配置。
- 采集计数配置。
- 配置并回读 32 位 AD 通道映射。
- 通过“关闭再开启采集”复位。
- 读取八路原始采样和配置状态。

v4 的 `Ad7606Config` 默认时序依次为：过采样 `0`、时钟周期 `24`、转换低电平 `3`、
转换完成等待 `35`、复位周期 `5`、采集计数 `3904`。局部 `0x44` 是 32 位通道映射，
默认 `0x76543210`；从低到高每个 nibble 对应逻辑通道 1 至 8 的实际 AD 通道。
`configure()` 写入该映射，`read_state()` 回读该映射。采样寄存器低 16 位按 `int16_t`
解释，读取是立即快照，不等待下一次转换完成。`raw(signed) / 65536 * 10 V` 是寄存器
换算契约，不构成真机量程验收；舵控主动反馈仍使用下文单独说明的参考仿射换算，二者
不能混用。

独立舵控程序使用板级反序关系：PWM 局部 `0x40` 写 `0x0123`，AD7606 局部 `0x44`
写 `0x76540123`。因此控制循环始终按逻辑下标执行 `AD[i] -> controller[i] -> PWM[i]`，
实际通道 4、3、2、1 分别对应逻辑舵 1、2、3、4；不得再在软件中执行 `3-i`，否则会
形成双重反序。AD7606 未用于舵反馈的逻辑通道 5 至 8 保持默认映射 4、5、6、7。

### 6.5 Ads1258Device

能力：

- 连续写入 21 个基础配置寄存器，并读写局部 `0x68` 的 DRDY 延时读取计数。
- 读取完整配置、32 路原始数据、两片诊断数据与 9 个 FPGA 维护的错误计数。
- 写 `0xFF` 清错误计数。

`read_snapshot()` 保留原始 32 位数据。32 路采样从局部 `0x80 + 4*i` 读取；全局通道
`0..15` 使用芯片 1 诊断，`16..31` 使用芯片 2 诊断。两片的
`OFFSET/VCC/TEMP/GAIN/VREF` 分别位于 `0x100..0x110` 与 `0x114..0x124`，每项均为低
24 位有效且高 8 位必须为 0。当前实现的每片诊断换算为：

```text
VREF   = raw_vref / 0xC0000
VCC    = raw_vcc / 0xC0000
GAIN   = raw_gain / 0x780000
OFFSET = sign_extend_24(raw_offset) * VREF / 0x780000
TEMP   = (raw_temp * VREF / 0x780000 * 1_000_000 - 168_000) / k + 25 °C
```

`k` 在自由空气模式为 `394`，在温控板/芯片与 PCB 同温模式为 `563`。VCC 与 TEMP 会随
每片快照解码并参与诊断有效性检查（当前明确拒绝零 TEMP、非正 VCC 及保留高位非零），
但 v4 契约没有给出温漂修正项或更窄的允许范围，因此二者不直接进入通道电压公式。
普通采样先计算：

```text
sample    = sign_extend_24(raw_sample) * VREF / 0x780000
corrected = (sample - OFFSET) / GAIN
```

全局通道 1、2、3（热电池激活、28.5V(B)、28.5V(1)）使用
`V = (corrected - external_bias) * 19.18`，外部偏置依次为 `0.500 V`、`0.325 V`、
`0.500 V`。通道 0 与 4..31 在 `0 <= corrected < 3` 使用
`corrected * (-0.1594*corrected^2 + 0.843*corrected + 15.1)`，在
`corrected >= 3` 使用 `corrected * 16.23`；负 `corrected` 是当前 v4 拟合域外错误。
产品路径使用 `calibrated_channel_voltage()`，而非旧的无状态 `channel_voltage()`。

电气健康中 `c_volt=raw[0]` 走普通通道定标，`b_volt=raw[2]` 与 `v28_5=raw[3]` 走上述
三路特殊定标；结果再按 `0.01 V/LSB` 写入协议 `S16F`。C/B 激活阈值不映射为协议响应。
HW_TEST 在使能/状态回退保护下设置 Config0=`0x02`、Config1=`0x82`、SYSRED=`0x3D`、
WORK_COUNT=`0xAA`、SPI divider=`0x20` 和两项阈值，再恢复两片使能；其中 Config0 固定
内部 OFFSET 语义，WORK_COUNT 固定双芯片模式。两项阈值当前均保持 `0x21EC35`、等待最终
标定，不能将 generated CSV 中的设计说明值表述为当前已应用或已完成真机标定。

`configure()` 同样先保存两路使能、关闭一路并打开状态回退，完成全部配置后关闭状态回退
并恢复原使能值；中途失败会尽力关闭状态回退并保持两路失能，避免带着部分配置重新采集。
v4 表没有明确采样与 OFFSET 的
S24/U24 文字，当前实现延续既有低 24 位二补码解释，并将校正值用于现有通用通道拟合；
这是当前实现约定，不扩展为已完成真机标定的保证。

### 6.6 XadcDevice

XADC 使用全局窗口 `0x150000`。`value_YX` 来自 VAUX8 的局部 offset `0x260`：读取
32 位 `Data` 后取 `ADC_code=Data[15:4]`，按单极性公式
`voltage = ADC_code / 4096 * 10.09 V` 换算。Device 只读该寄存器，不触发转换、不复位
XADC，也不读取 offset `0` 做签名检查。产品协议由 `ELEC_HEALTH_STATUS` 响应 B31-B32
上传该原始 ADC code，PC 按 `10.09/4096 V/code` 解码。

同一地址表还确认电气健康的 XADC 来源：`external_vol`=VAUX1/`0x244`（系数 10.09）、
`core_vol`=VCCINT/`0x204`（系数 3）、`assist_vol`=VCCAUX/`0x208`（系数 3）、
`power_24V`=VAUX2/`0x248`（系数 35.09）、`dyt_5V`=VAUX9/`0x264`（系数 10.09）、
`js_5V`=VAUX0/`0x240`（系数 10.09）。

### 6.7 DidoDevice

能力：

- `set_outputs()` 按 `update_mask` 修改指定输出；位值 1 写 `0xAAAA`（输出高），位值 0
  写 `0xFFFF`（输出低）。
- 读取 16 路输出和 16 路输入。
- `read_outputs()` 按 DIDO 寄存器分组返回位图：DO0..7 物理高为 1，DO8..15 物理低为 1。

极性处理：

- DO0-DO7 高有效。
- DO8-DO15 低有效。
- FPGA 输入寄存器低字节为 `0xAA` 表示有效。

该位图不能一概解释成板级功能“已使能”。特别是 DO3=`24V_EN` 和
DO4=`DYT_5V_EN` 为物理低使能，因此其命令/回读 bit=0 才表示使能，bit=1 表示失能。

`origin_v4` 已命名的板级通道如下；未列出的 DI/DO 只保留协议通道号：

| 通道 | 板级信号 | 局部偏移 | 说明 |
|---|---|---:|---|
| DI0 | 联锁、电气弹动 | `0x80` | 高有效 |
| DI1 | 引信报警 | `0x84` | 高有效 |
| DI2 | 引信起爆指令 | `0x88` | 高有效 |
| DI3 | 锁相环锁定指示 | `0x8C` | 高有效 |
| DI8 | 投放允许 | `0xA0` | 低有效 |
| DO0 | 舵锁使能 | `0x04` | 高有效 |
| DO1 | 数控衰减器控制 | `0x08` | 高有效 |
| DO2 | 数遥发送使能 | `0x0C` | 高有效 |
| DO3 | `24V_EN` | `0x10` | 物理低使能 |
| DO4 | `DYT_5V_EN` | `0x14` | 物理低使能 |
| DO5 | `DI_EN1` | `0x18` | 地址表标注无需控制、恒低 |
| DO6 | `DO_EN` | `0x1C` | 地址表标注无需控制、恒低 |

地址表的“无需控制、恒低”是板级使用约束；当前协议实现仍按 16 位 `DO_WRITE` 位图
写入，未在 C++ 中硬编码屏蔽 DO5/DO6。PC 页面已禁用这两个控件并保留地址表提示，
避免测试人员误选；手工构造协议位图时仍需遵守板级约束。

### 6.8 DhController

能力：

- 配置时间基准。
- 配置 48 路脉宽。
- 单通道点火。
- 多通道批量点火。
- 设置单发/多发、一次/可重复模式。
- 显式设置全局点火使能（`FireEnable=0x1DC`，使能 `0xAAAA`，关闭 `0xFFFF`）。
- 读取 48 路反馈和电池状态。

单通道命令按通道范围编码为 `0xBxxx`、`0xCxxx`、`0xDxxx`。
多通道接口先去重，每批编码四个槽位，然后写 `MultiTrigger=0xAAAA`。`origin_v4`
允许通道重复；为兼容实板对含 `0xFF` 尾批不响应的行为，末批不足四路时仍用本批最后一个
有效通道重复填满，不填充 `0xFF`，也不引入未选择通道。

该类只做参数和寄存器编码，不包含业务级武器/危险动作授权、双人确认、互锁或状态机。
应用层必须在调用 `fire`/`fire_multiple` 前实现完整安全控制。工程 Demo 的默认巡检只读
反馈；仅显式启用全能力模式时才执行受控点火流程。

### 6.9 ComDevice

`ComDevice` 同时实现 `IByteEndpoint`，MTU 为 65536 字节。

配置能力：

- 字节格式和收发帧头/帧尾。
- 长度字段宽度。
- 接收使能、超时行为和回环。
- 中断输出使能、脉冲/电平中断模式。
- 收发波特率计数。

发送流程：

1. 检查 Transport 和 payload；空 payload 直接返回 0，不访问硬件。
2. 读取 Control，发送器忙时返回 `Busy`。
3. 按发送帧配置在 payload 前加入 0/1/2 字节小端长度字段，再按 32 位小端字写入
   Send RAM，尾部补零。
4. 仅在无帧内长度字段时写入 16 位发送长度。
5. 完整帧全部写完后，最后写 `Control=0x81`，只触发发送。接收是否允许由模式
   寄存器 D5 控制；发送路径不得用 `0x80` 同时切换 RX RAM。

接收流程：

1. `wait_event(timeout)` 等待 XDMA event。
2. 超时时，普通接收返回成功值 0；若当前对象已显式配置内部回环且刚成功
   发送一帧，允许一次 RAM 降级读取，用于兼容未接出 COM 中断的 bitstream。
3. 回读 Control 和 Error；event 只作通知，D0 才是接收完成判据。
4. Error 低 8 位非零时写 `0xCCCC` 清错，再写 `Control=0x82` 丢弃错误帧并重新
   接收，最后返回 `ProtocolError`。Error 高 24 位无效，必须忽略。
5. 无错误但 D0 为 0 时，将本次通知视为旧事件或伪唤醒：轮询调用立即返回 0；有限/永久
   阻塞调用保留内部回环 pending 状态，并在同一个总截止时间内继续等待，直到 D0=1。
6. 无帧内长度字段时，在重新接收前先锁存 `ReceivedBytes`。
7. 写 `Control=0x82`，切换乒乓 RAM、清除 Level 中断并重新使能下一帧接收。
8. 有帧内长度字段时从完成 bank 的长度前缀获取帧长，然后校验 MTU、用户缓冲区并
   按 32 位读取 Receive RAM。

降级读取使用单次 pending 标记：一次发送最多允许一次超时后回环读取，防止旧
bank 在后续超时中被重复交付。

COM 中断直接接入 XDMA `usr_irq_req` 时必须使用 Level 模式，并由成功/错误接收路径
写 `0x82` 清源。`reset()` 会向 Control 写 0；实际 FPGA 中该软件复位可能锁存到下一次
PCIe/AXI 全局复位，因此正常配置和回环流程不得调用它。

#### 双 RX bank 风险、兼容路径与验收边界

COM RX RAM 是两个乒乓 bank 复用同一 `ReceiveRam=0x00000` 读窗口。正常时，一个 bank
接收线上下一帧，另一个 bank 保存刚完成的帧；软件看到 event、`Control.D0=1` 且 Error
低 8 位为 0 后，先锁存独立长度寄存器（无帧内长度字段时），再写 `Control=0x82`。
该写入必须同时完成三件事：切换 CPU 可见的完成 bank、清除 Level 中断、重新使能另一
bank 接收。随后从 `ReceiveRam` 读到的长度前缀和 payload 必须属于刚完成的同一帧。

宿主当前对一种窄特征陈旧回放提供兼容：`error_response` 必须引用同一命令、不同序号，且
错误严格为 `0x0102/detail=0`，算法才以相同序号原帧重发一次。该路径只能缓解陈旧 bank
回放，不能证明 FPGA bank 写入或选择逻辑已经修复；第二次失败及任何其他错误仍按协议失败。
宿主事务语义以[设备通信协议契约](../../../docs/design/contracts/device-communication-protocol.md)为准。

`debug.bat hw_run` 的 COM1 内部回环提供独立的软件验收门禁：Adapter 预检后，
在同一次 Transport 打开和同一份 COM 配置下连续发送 16 个 32 字节 payload。每个 payload
在偏移 13 编码一基轮次并带不同测试图样；测试禁用重试，逐轮严格比较长度和全部字节，
失败时记录轮次、首个差异以及收发十六进制，并在失败后继续跑满 16 轮。最终必须满足
`16/16`，且奇数轮与偶数轮均为 `8/8`；任一轮失败都使 COM 能力组失败。轮次奇偶只表示
本次启动后的切换顺序，不等同于固定物理 bank 编号。
该门禁的任一次通过都只覆盖所运行的 COM1 内部回环环境，不能单独证明 COM3 外部 RX
或其他 bitstream 已通过；实际结果必须作为独立运行证据记录，不能写死在设计文档中。

#### COM3 外部回显边界

PC 串口工具与板端 COM3 的外部链路协议固定为：帧头 `55 AA`，1 字节有效长度，
1..255 字节 payload，CRC-16/XMODEM（多项式 `0x1021`、初值和异或输出均为 `0`、
不反射），CRC 输入为长度字节加 payload，线上低字节先发。完整帧为
`[55 AA][LEN][DATA...][CRC_LO CRC_HI]`，已知向量 `4D 42 31` 对应
`55 AA 03 4D 42 31 FC 89`。

该协议由 FPGA 处理帧头、长度和 CRC；板端 ECHO 画像只把验证通过的 payload 交给
`ComDevice::send()`，不在 C++ 重新实现物理帧解析，也不把日志写入 COM3。回显模式
固定使用 `/dev/xdma0`、`user_offset=0xC0000`、`map_length=0x40000`、event 2、Level
中断、`614400 / 8E1 / 无流控` 和 `loopback=false`。该画像不读取、保存或恢复原
`ComConfig`，也不编入 DDS、
测试处理器或 Demo 编排。

#### COM3 产品协议服务边界

HW_TEST 画像复用同一个 COM3 物理口，接收 FPGA 已校验的 1..255 字节数据段；当前 CSV
实际使用 48、123 和舵反馈的 232 字节 payload。
字段布局由 `docs/design/product_protocol_csv/*.csv` 唯一定义，构建时由
`tools/generate_product_protocol.py` 校验并在构建目录生成描述头；C++ 按描述偏移做
小端读写，不依赖结构体内存布局，也不重复计算 CRC。

`HardwareTestService` 只有一个接收循环，全部发送路径共享一个带锁发送器。普通响应和
错误响应回显请求序号，DH 第 `i` 帧使用 `请求序号+i`（U16 回绕），无对应请求的舵反馈
和惯测反馈使用板端独立递增序号；惯测 `source_seq` 只是数据字段。未知命令、版本或长度
错误走 `0xFF/0x00`；硬件执行错误写入原命令
响应的 `status/err_code`。总线 link 0/1/2/3 分别对应
COM1/2/3/4；COM3 是控制口，link 2 在打开任何硬件前拒绝，只有 link 0/1/3 可测。

序号在进入发送器前确定，发送锁覆盖完整数据段的提交。若 `ComDevice::send()` 返回
`Busy`，表示发送器尚未接收该帧：服务每隔 100 us 重交相同字节，最多 1000 次；期间
不改变序号。该行为是有界的 Transport ready 流控，不是请求超时后的协议自动重发。

该服务是显式的硬件写入画像。本版不保存或恢复 DIDO、DH、PWM、SPI Flash 或
COM 配置，不做安全确认、自动重试、夹具检测、断链清理和结果阈值判定；调用者只能把
返回值解释为执行完成、执行失败或通信失败。DIDO 地址表确认 DO3=`24V_EN`、
DO4=`DYT_5V_EN`、DO5=`DI_EN1`、DO6=`DO_EN`，但没有 5V_JS 控制输出。当前产品协议和
PC 不提供独立电源测试入口；上述 DIDO 映射仍是 `DidoDevice` 与只读状态核对的板级事实，
不得据此猜测 5V_JS 控制信号。

`ELEC_HEALTH_STATUS` 在 B31-B32 上传 XADC VAUX8/局部 `0x260` 的 12 位
`ADC_code=Data[15:4]`，LSB=`10.09/4096 V/code`。`HELM_BOARD_TEST` 的
`helm_AD_value[0..3]` 上传 AD7606 通道 0..3 的有符号 16 位 raw，LSB=`10/65536 V/code`。
PC 按 CSV LSB 换算工程值，板端不先转成电压后取整，因而不损失 ADC 分辨率。

舵控板级测试使用普通单次请求/响应 `HELM_BOARD_TEST`（`07/02`）。请求 B9 bit0..3
保留且必须为 0，bit4..7 分别选择四路方向，B10-B13 分别为四路 U8 整数占空比
`0..100%`。板端先使能 AD7606 采集和滤波，读取当前 PWM `peak_value`，关闭统一更新并
切换到无符号占空比模式，然后按
`raw=(uint64_t(peak_value)*percent+50)/100` 换算并写入四路 duty，方向位独立写入，四路
PWM 全部使能，最后重新打开统一更新。响应 B12 低四位为 `pwm_duty_match[0..3]`、高四位
为方向回读，另回传四路 raw duty、当前 peak、PWM 使能位图、PWM 更新状态、AD7606
采集/滤波状态及 AD7606 通道 0..3 的有符号原始采样；AD 字段继续按 CSV 的
`10/65536 V/code` 在 PC 端换算。

`07/02` 不启动反馈线程，也不读取或限制连续舵机 DDS bridge 的状态。PWM peak 为 0，
或者 duty、方向、四路使能、更新、AD 采集/滤波任一回读与本次写入不一致时返回
`REG_READ_WRITE_FAILED (0x0201)`。底层寄存器访问错误按统一状态映射返回。该命令执行后
保持 PWM 和 AD7606 状态，不自动清零或恢复，只能在已隔离且允许写入硬件的目标板运行。

`ELEC_HEALTH_STATUS` 的模拟量来源和定标已经确认，`value_YX` 固定在响应 B31-B32。
PC 只显示 `activate_bits.bit0` 并命名为“BC激活好”；该位读取 DH
`BatteryStatus`（局部 `0x1D8`）高 8 位：`0xAA` 为已激活、`0xFF` 为未激活，其他值
返回 `REG_READ_WRITE_FAILED`。bit1..7 恒为 0 且不显示，低 8 位点火状态不参与该字段；
不根据电压阈值猜测位图。

总线处理器的边界如下：

- BUS 只使用 link 0=COM1、1=COM2、3=COM4。link 2=COM3 控制口在打开硬件前返回
  `CHANNEL_INVALID`，其余 link 也返回该错误；IMU_STREAM 活动时 link 3 先返回
  `TASK_BUSY`。
- BUS_LOOP 为内部 `loopback=true` 收发，`total_count` 只允许 `1..100000`。每轮接收
  最多等待 5 秒，只有完成次数严格等于请求且错误数为 0 才成功。
- BUS_ECHO 为 `loopback=false` 的单次外部回显：DUT 发送请求中的 114 字节，独立 PC/夹具
  必须在 5 秒内原样回发。长度或任一字节不符均失败，响应保留实际接收字节。连续轮次由
  根宿主 `mbddf.serial_test` 在同一次 BIZ `single` 运行的 `executeStep()` 中依次发起。
- BUS 不再包含 UDP 或 SPI Flash；独立 SPI Flash 测试仍走固定隔离测试区的
  `SPI_FLASH_TEST`。
- DH 脉宽配置先向 `0x1E0` 写配置使能（`0xAAAA/0xFFFF`）；使能时写入并回读 23 路，
  失能时忽略请求脉宽并只失能、回读。DH 控制依次写电源使能 `0x1DC`
  （`0xAAAA/0xFFFF`）、回线使能 `0x04`（`0xA000/0x00A0`）、配置 Multiple 模式并提交
  通道。只接受 bit 0..22；所选通道去重后按四路分批，末批重复最后一个有效通道填满。
  点火状态按 `0xFFFF/0xAAAA/0xBBBB` 映射为未 DH/成功/失败。默认参数为
  `report_count=50`、`interval_us=2500`、`delay_frames=5`；板端在任何硬件写入前校验总帧数、
  最小间隔、基线帧数、两项使能值和通道位图。`delay_frames=N` 表示先采集并发送 N 帧只读
  基线，再在下一帧采样前点火一次；N=0 时首帧前点火。基线采集或发送失败不得点火，点火后
  采集错误以错误帧继续，发送失败终止；协议无 STOP/ABORT，结束后不自动恢复 DH 状态。
  回线使能从同地址读回时低 8 位使用 `0xAA/0xFF` 表示开/关。每帧到期后先记录实际采样
  起点、读取报告并立即发送，下一截止点为本帧实际采样起点加 `interval_us`。因此串口发送
  耗时只占周期余量，不额外叠加到间隔，也不会压缩相邻实际采样起点间隔。每帧读取一份
  ADS1258 快照：`telemetry[0]=raw[1]`（局部 `0x84`，热电池激活），
  `telemetry[1..22]=raw[4..25]`（局部 `0x90..0xE4`，22 路点火电压）。板端按上述通道
  公式换算，并以 `0.001 V/LSB` 写入协议 `S32F`；读取或定点编码失败时该帧返回明确错误，
  不提交部分响应字段。

SYSTEM_STATUS 只使用可追溯事实源：CPU/内存/上电时间来自 `/proc`。CPU0/CPU4 当前频率
优先读取标准 cpufreq；实板 5.10.0-openeuler 未注册 cpufreq policy 时，分别读取
`/sys/kernel/debug/clk/scmi_clk_cpul/clk_rate` 和
`/sys/kernel/debug/clk/scmi_clk_cpub01/clk_rate`，按 Hz 转 MHz。该回退要求 debugfs 已挂载
且 HW_TEST 进程具有 root 读取权限。CPU/RK 温度来自 `center_thermal` hwmon（CPU 温度可
回退到名称含 `cpu`/`soc` 的 thermal zone）。当前 `net_init_time` 固定回传 `0 s`；K7
温度直接读取 XADC 全局 `0x150000` 的局部 `0x200`，按
`ADC_code*503.975/4096-273.15` 换算。PCIe 不扫描任意控制器，而先解析
`/sys/class/xdma/xdma0_user/device` 的真实路径；若该事实链不存在，再由
`/dev/xdma0_user` 字符设备的 major/minor 跟随 `/sys/dev/char/<major>:<minor>/device`，
最终只读取对应 BDF 的 `current_link_speed` 和 `current_link_width`。旧 `xdma0` 入口仅作
兼容回退；`lspci -vvv` 只用于人工核对。任一必需来源缺失时整条 SYSTEM_STATUS 返回
`TASK_EXEC_FAILED`，不以零值伪装成功。

#### DDS 舵机连续实测边界

舵机连续实测使用 `HELM_START (07/10)`、异步 `HELM_FEEDBACK (07/01)` 和
`HELM_STOP (07/11)`，与 `07/02` 板级测试并列。START 参数为波形 U32、`freq`、`ampl`、
`offset`、`start`、`max_freq`、`sweep_duration_s` 六个 F32 和四路 `enable` 位图；`start`
是弧度相位。参数只要求波形/位图合法、频率与时长为有限正数，其余 F32 有限；DUT 不设置
角度幅值或偏置边界。

`HelmDdsTestBridge` 在 START 时 create-or-get `local:://helm_command` writer 和
`local:://helm_feedback` reader，先发布一帧四路零位且 `helm_unlock=0xFF` 的解锁指令，
成功后以单调时钟每 1 ms 生成一条四路舵角指令。普通波形使用
`phase = 2*pi*freq*t + start`；连续对数扫频在 `f0 != f1` 时使用
`phase = 2*pi*f0*f1*T/(f1-f0)*ln(f1*T/(f1*T-t*(f1-f0))) + start`，其中 `T` 来自
`sweep_duration_s`，`f0 == f1` 时退化为定频。超过 T 后四路指令归零，但 DDS 反馈订阅和
COM3 主动回告继续运行到 STOP。启用通道共用该波形，未启用通道固定为零。

DDS 指令严格使用当前 `src/HelmControl/ProtocolModel` 定义的 27 字节 `Helm_ins_frame`；B27/U8
为 `helm_unlock`，`0x00` 表示无解锁请求，连续测试的每帧固定为 `0xFF`。独立
`MB_DDF_v2_HelmControl` 回送 41 字节 `Helm_fdb_frame`。bridge 完整保留 `serial_a`、
`serial_b`、版本、四路 `ins/fdb`、六项 2-bit 自检和 timeout，并以首样本 DDS 微秒时间戳
加 U16 相对时间打包；每个 232 字节 `07/01` payload 包含 1..5 个完整样本。队列有界，
发送失败通过 `HELM_DDS_FAILED` 上报；同一流的 START ACK、反馈和 STOP ACK 严格保序，
已进入发送的反馈先于 STOP ACK，ACK 后不再补发旧反馈。STOP 停止指令线程后先发布
“四路零位 + `helm_unlock=0xFF`”尾帧，再关闭本次 DDS 端点并清空队列。

独立舵控程序将 AD7606 的有符号 16 位原始反馈码按现有路径扩展为 `int32_t data`，再按
`actual_angle = (2.048 - static_cast<int16_t>(data) * 10.0 / 65535.0) * 3.0 * 115.0 / (4 * 4.096)`
换算舵机实际角度；该角度同时参与闭环控制反馈并填入 41 字节 DDS 反馈帧。此公式只属于
`MB_DDF_v2_HelmControl` 的连续舵控实际角度换算，不替代或改变独立 `HELM_BOARD_TEST 07/02`
上传 AD7606 有符号原始码后由 PC 按 `10/65536 V/code` 执行的通用换算。

`MB_DDF_v2_HelmControl` 由用户独立启动或停止，内部保留自身舵角限幅。程序打开
PWM transport 后的首个控制写是 `disable_outputs()` 直接禁止四路 PWM，随后关闭 update gate、
写入零 duty，再写 PWM 反序映射；AD7606 初始化时写对应反馈反序映射。未收到解锁请求时
不允许输出。首次 `helm_unlock=0xFF` 使控制线程将全局基址
`0x140000` 的 DIDO DO0 置为高有效，从成功写入起使用 `steady_clock` 至少等待 30 ms，
到期后才开启 update gate 和四路 PWM。该状态在进程内不可逆；重复请求不重置计时，字段
回落或 STOP 不关闭 DO0/PWM，四路零位指令由闭环继续回零。任一首次关闭、解锁或使能
硬件动作失败时进入粘滞故障且不标记 PWM 已使能。HW_TEST 不创建、
终止、探测、占有或等待该进程；两个程序启动顺序不限。`07/02` 不查询 bridge，bridge 也
不访问 `07/02` 的 PWM/AD7606 路径，二者没有生命周期、互斥、忙状态或其他形式的绑定；
`07/02` 不包含舵锁流程。

#### COM4 惯测设备流边界

`09/10` 成功后，HW_TEST 独占 COM4：`/dev/xdma0`、`user_offset=0x100000`、
`map_length=0x40000`、event 3、Level 中断、`921600 / 8E1`、波特率计数 `0x0086`。
FPGA 消费 `AA 1A`、1 字节长度和 CRC；软件使用 255 字节接收缓冲辨别异常长度，但只接受
恰好 59 字节的 payload。长度异常或 12 个 F32 任一非有限时丢弃，不产生 `09/01`；其他
状态字段和源序列号原样保留。

每个有效 payload 立即完整映射为 COM3 的 `09/01` 128 字节帧，不固定 sleep、不限频、
不主动抽样。`09/11` 先使会话失活并关闭 COM4，再返回 STOP ACK；同一 PC 会话最多发送
一次 STOP，ACK 异常不触发收尾重发。流活动期间 BUS link 3 返回 `TASK_BUSY (0x0204)`。
400 Hz 不要求软件保证无损，实际吞吐与丢帧只能由授权真机验收记录。

### 6.10 FPGA MMIO Flash IP

FPGA MMIO Flash IP 及 `FlashDevice` 已从当前实现删除；它不再拥有 Device API、寄存器定义、单元测试或 smoke 范围。

### 6.11 UpdateImageIpVersionDevice 与 FpgaUpdateStateDevice

`UpdateImageIpVersionDevice` 使用 `0x160000/0x10000` 窗口，只做局部 offset `0` 的通信
签名检查和版本快照读取：软件类型/状态、型号软件版本/日期，以及 DIDO、DH、AD7606、
PWM、ADS1258、XADC、COM、FPGA update 的基址/版本/日期。软件类型可能报告
update/golden image 标记，但这不等同于导出或实现独立的 GOLDEN image 寄存器图。

`FpgaUpdateStateDevice` 使用 `0x170000/0x10000` 窗口，只做通信签名检查和状态快照读取：
擦除、写入、读取状态，读/写数据 CRC 及 CRC 校验状态。两个 Device 均不公开写入、擦除、
编程或触发 API，也不写地址表中标为“无”或“暂时不用”的位置；这些读取接口不能表述为
FPGA 更新能力或真机验收证据。

### 6.12 CPU SpidevTransport 与 SpiFlashDevice

CPU SPI Flash 使用独立 `/dev/spidev0.0`，不经过 XDMA user BAR。`ISpiTransport`
与 MMIO `ITransport` 分离，因为 SPI 的关键语义是“命令、地址和数据必须位于同一次
片选周期”。`SpidevTransport` 缺省配置为 Mode 0、MSB first、8 bit、1 MHz：

1. 使用 `O_RDWR | O_CLOEXEC` 打开节点，并持有非阻塞 `flock(LOCK_EX)` 到关闭。
2. 保存原 mode、bit order、bits-per-word 和 speed，再写入并读回测试配置。
3. 每个事务只调用一次 `SPI_IOC_MESSAGE(1)`，TX/RX 必须等长且完整返回。
4. 配置 ioctl 可在 `EINTR` 后重试；数据事务不盲目重试，因为无法确认 Program/Erase
   是否已经被器件接收。
5. 正常结束时恢复打开前保存的四项 SPI 配置；显式恢复失败会使 Demo 失败，析构路径
   仍会 best-effort 尝试后关闭 fd。

`flock` 是进程间协作锁，只能排斥同样遵守锁的访问者；完整测试期间仍必须停掉其他
直接操作 `/dev/spidev0.0` 的程序。同一 `SpiFlashDevice` 也不得被多个线程并发调用。

`SpiFlashDevice` 面向 Micron N25Q512A83G1240F，固定几何和标识为：

| 项目 | 值 |
|---|---:|
| JEDEC ID | `20 BA 20` |
| 容量 | 64 MiB |
| die | 2 × 32 MiB |
| page | 256 B |
| subsector | 4 KiB |

驱动不进入全局四字节地址模式，而使用该器件的专用四字节 opcode：Read `13h`、
Page Program `12h`、4 KiB Subsector Erase `21h`。因此不会通过 `B7h/E9h` 留下影响
其他进程的地址模式状态。读取按不超过 256 B 分片，并在 32 MiB die 边界再次拆分。
JEDEC READ ID `9Fh` 没有地址字段；线上 `9F FF FF FF` 中后三字节只是产生 24 个
数据输出时钟的 TX don't-care，ID 从对应 RX 的后三字节取得。

完整 Demo 打开 `/dev/spidev0.0` 后、读取任何状态或 JEDEC ID 前，先用两个独立 CS
事务发送 WREN `06h` 和 WRNVCR `B1h EFh FFh`，在非易失配置寄存器中永久关闭
`#HOLD` 功能；两条写指令之间不插入 RDSR。该修改跨复位和掉电保持，Demo 不自动恢复，
因此每次运行全能力画像前都必须确认允许改写该器件的非易失配置。

每次 Program/Erase 前先发送 CLFSR `50h` 清除历史粘滞错误，再发送 WREN `06h` 并
用 RDSR `05h` 确认 WEL(bit1)=1。操作后轮询 RDFSR `70h`，只有 bit7=1 且
bits5/4/1 均为 0 才成功，再确认 WEL 已自动清零。Page Program 拒绝跨 256 B 页，
Subsector Erase 拒绝非 4 KiB 对齐地址，所有接口检查 64 MiB 容量边界。
上电或接管未知状态与单个 Program/Erase 的完成轮询不同：首次内存读取前分别发送
两条独立 `70h`，在两条命令间切换 CS，只有两个 die 的 bit7 都为 1 才继续。
若前置阶段失败，WRDI `04h` 可显式清除并核验 WEL；`last_mutation_command_attempted()`
区分“尚未发送数据命令”和“ioctl 返回失败但命令可能已被接收”，供恢复状态机决策。

完整 Demo 缺省测试最后一个子扇区 `0x03FFF000..0x03FFFFFF`，可通过
`MB_DDF_HW_SPI_FLASH_TEST_ADDRESS` 指定其他 4 KiB 对齐区域。Demo 在首次内存读取前
用 `9Fh` 校验 JEDEC ID 为 `20 BA 20`；第一笔内存数据事务再使用
`13h + 该四字节地址` 完整备份 4096 B。随后擦除并校验全 `FF`、写入来源
`SpiFlash.h` 的 16 B 图样、读回校验、再次擦除、按页恢复备份、完整恢复校验。备份
不完整时不会发送擦写命令；第一次擦除调用后的任何失败都进入恢复路径。进程终止或
掉电仍会打断恢复，因此测试范围必须可外部重刷。
Status 和两个 die 的 Flag Status 都为 `FF` 时按 MISO 高电平或片选/供电/引脚复用
未响应处理，在首次内存读取及任何擦写命令前终止。

## 7. DDS Adapter

### 7.1 IByteEndpoint

```cpp
send(BufferView)
receive(MutableBufferView, Timeout)
mtu()
```

它用于表达原始字节链路，`ComDevice` 是当前真实实现。

### 7.2 ExternalEndpointAdapter

Adapter 持有非拥有型 `IByteEndpoint&`：

```text
DDS ExternalEndpoint::send
  -> IByteEndpoint::send

DDS ExternalEndpoint::receive(timeout_us)
  -> Timeout::after_us
  -> IByteEndpoint::receive
```

映射规则：

- 成功返回实际字节数。
- `StatusCode::Timeout` 映射为 0。
- 其他错误映射为 `-1`。

Adapter 和 Device 都不拥有底层对象，推荐生命周期顺序：

```cpp
XdmaTransport transport(config);
transport.open();

ComDevice com(transport);
auto endpoint = std::make_shared<ExternalEndpointAdapter>(com);

auto writer = dds.create_writer("external://com1/tx", endpoint);
auto reader = dds.create_reader("external://com1/rx", endpoint);
```

`CallbackExternalEndpoint` 可用回调桥接不实现 `IByteEndpoint` 的设备。

## 8. 构建与链接

构建 DDS-only 库或 DDS-only 目标板测试（不编入硬件层）：

```powershell
.\build.ps1 lib_debug
.\build.ps1 lib_release
.\build.ps1 dds_tests
```

构建 ECHO、HW_TEST 和硬件 Demo：

```powershell
.\build.ps1 debug
.\build.ps1 release
.\build.ps1 hw_test_debug
.\build.ps1 hw_test_release
.\build.ps1 hw_debug
.\build.ps1 hw_release
```

构建硬件层测试和 smoke：

```powershell
.\build.ps1 hw_tests
```

CMake 目标：

| 目标 | 内容 |
|---|---|
| `MB_DDF_HW` | Core、Os、Transport、Device |
| `MB_DDF_HW_DDS_Adapter` | DDS Adapter |
| `MB_DDF_HW_Tests` | Null/RecordingTransport 单元测试 |
| `MB_DDF_HW_Smoke` | 真实 XDMA 设备 smoke |

上层目标链接：

```cmake
target_link_libraries(my_application PRIVATE MB_DDF_HW)
```

需要 DDS Adapter 时还应链接：

```cmake
target_link_libraries(my_application PRIVATE MB_DDF_HW_DDS_Adapter)
```

## 9. 测试设计

### 9.1 无板卡测试

`tests/hw_unit` 覆盖：

- fd/mmap/poll RAII。
- NullTransport 的打开、关闭、对齐和越界。
- PWM 的 v4 方向模式、通道映射、写入顺序和范围校验。
- AD7606 的 v4 默认时序、通道映射和原始数据解释。
- ADS1258 的两片诊断读取、v4 定标、外部偏置、拟合边界、DH 通道映射和 `0x68` 配置读写。
- UPDATE image IP version 与 FPGA update state 的通信签名、完整快照和零写入约束。
- XADC `Data[15:4]` 提取、`value_YX` 定标和只读访问。
- DIDO 极性转换。
- DH 命令编码、批量去重和不足四路尾批的重复填充。
- COM RAM 打包、XDMA event 就绪后不二次读取、旧事件防御等待、超时和缓冲区不足。
- CPU SPI Flash 的四字节命令帧、WEL/Flag Status、页/子扇区/容量边界和跨 die 拆分。
- CPU SPI Flash 完整工作流的明确地址读取、4 KiB 正常恢复、测试写失败恢复，以及
  恢复首轮失败后重试但整体仍判失败。
- DDS Adapter 的 timeout/payload 映射。
- 产品协议 1..255 字节 payload（当前 48/123/232）布局、大小端、默认值/RESERVED、工程量 LSB 有界编码、错误响应
  和独立发送序号。
- 产品测试服务的普通响应、DH 多帧、`07/02` 舵控板级写入与回读、DDS 舵机反馈、发送锁、
  Busy 有界同帧流控，以及 BUS link 映射、COM3 控制口预检、严格完成统计和实际回显保留。
- 惯测 `09/10`/`09/01`/`09/11` 路由、COM4 921600/8E1 配置、59 字节全字段映射、全部
  12 个 F32 非有限拒绝、异常长度丢弃、空闲不消耗发送序号和终止错误反馈。
- 系统状态的 CPU 计数解析和 XDMA sysfs BDF 事实链，以及 BUS 的 COM 内部回环/外部回显
  配置、5 秒接收截止、舵控弧度相位、可配置扫频公式、27/41 字节帧和最多五样本打包。

### 9.2 真实硬件 smoke

`MB_DDF_HW_Smoke` 默认：

- 打开全部 MMIO 窗口。
- 检查 PWM、AD7606、ADS1258、DH、DIDO 通信。
- 读取各设备状态/快照。
- 在 `0x150000` 只读 XADC `value_YX`。
- 在 `0x160000` 验证 UPDATE image IP version 通信签名并读取完整版本快照。
- 在 `0x170000` 验证 FPGA update state 通信签名并读取状态与 CRC 快照。
- 打开 COM1-COM4，读取配置和错误状态。

该 smoke 不向 COM4 注入 59 字节惯测 payload，也不验证 `09/01`、921600 时序、400 Hz
吞吐或 STOP 收尾；也不写入或触发任何 FPGA 更新操作。这些能力必须在授权隔离台架
另行验收。

默认 `--read-only` 行为不修改设备。当前 smoke 的读取范围与交叉构建均不构成目标板
验收证据。

显式指定 `--com-loopback` 时：

1. 保存 COM 原配置。
2. 开启回环、接收和 Level 中断。
3. 发送固定 payload。
4. 等待 event 并校验回读。
5. 恢复 `ComConfig` 覆盖的持久配置字段。

## 10. 扩展新硬件

### 10.1 新设备使用现有 XDMA

新增：

```text
Device/MyDevice.h
Device/MyDevice.cpp
Device/Registers/MyDeviceRegisters.h
```

Device 构造函数接受 `ITransport&`，把寄存器时序翻译成
`read*/write*/wait_event/dma_*` 调用。不要在 Device 中直接 `open` 或 `mmap`。

### 10.2 新 Linux 驱动

如果设备使用独立 `/dev/my_device`、`ioctl` 或特殊映射协议，新增
`MyDeviceTransport` 并实现 `ITransport`。Device 层保持不变。

### 10.3 新字节链路接 DDS

优先实现 `IByteEndpoint`，然后复用 `ExternalEndpointAdapter`。只有接口无法自然
映射时才使用 `CallbackExternalEndpoint`。

### 10.4 变更清单

新增设备或寄存器时同步：

1. 原始地址表和 `generated/registers.md`。
2. `Device/Registers/*.h`。
3. Device 实现。
4. `src/MB_DDF_HW/CMakeLists.txt`。
5. `tests/hw_unit`。
6. 必要时更新 `MB_DDF_HW_Smoke` 和 Demo。
7. 更新本详细设计。

## 11. 已知约束

- 当前没有 Device Factory、配置文件加载或自动发现。
- Device 和 Adapter 使用引用，不管理对象生命周期。
- MMIO 字节序按当前 AArch64/FPGA 小端环境处理。
- DMA 短读短写不会自动重试。
- COM 接收依赖 XDMA event 节点；未配置 event 时返回 `Unsupported`。
- 每个 XDMA `_events_N` 只支持一个消费者；同一节点不得由多个进程或线程竞争读取。
- COM 通信检查当前只验证 Transport 已打开，没有固定签名寄存器。
- UPDATE image IP version 与 FPGA update state 当前只支持通信签名和快照读取；没有写入、
  擦除、编程、校验触发或更新编排，因此不能作为 FPGA 更新成功或真机功能验收证据。
- CPU `SpiFlashDevice` 按 N25Q512A 手册实现命令集；完整 Demo 用 `20 BA 20` ID
  校验器件身份，数据路径使用明确四字节地址，并仅通过 `/dev/spidev0.0` 访问。
- `NullTransport` 不模拟真实异步事件和 DMA。
- Device API 不是线程安全保证；同一设备的配置、发送和控制并发需要应用层协调。
- 危险控制接口只做编码和参数检查，不替代系统级安全策略。
