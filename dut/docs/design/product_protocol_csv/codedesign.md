# 产品端-上位机通信协议 V1.1

> 版本：V1.1（2026-07-30）
> 适用范围：AArch64 Linux 产品端硬件测试服务与上位机协议端。产品端软件行为以当前
> `dut/src/` 实现为唯一事实；主机职责和运行模式见[设备通信协议契约](../../../../docs/design/contracts/device-communication-protocol.md)。
> 字段布局唯一事实源：本目录 37 份 CSV。本文只定义传输、时序、测试映射和硬件边界。

## 1. 传输基线

- 控制口固定为板端 COM3：`/dev/xdma0`、全局偏移 `0xC0000`、映射窗口 `0x40000`、
  XDMA event 2。
- 串口固定为 `614400 / 8E1 / 无流控`，中断使用 Level 模式。
- FPGA 负责识别 `55 AA`、长度和 CRC。板端 C++ 只接收和发送从 B4 `version`
  开始的 1..255 字节数据段，不重复解析物理帧，也不重复计算 CRC；当前定义使用
  48、123 和舵反馈的 232 字节数据段。
- 全部多字节整数和 IEEE-754 F32 字段均按小端线序编码。
- COM3 产品帧不暴露 DDS 格式；舵机连续实测在产品端内部经 DDS 连接独立
  `MB_DDF_v2_HelmControl`，其 27/41 字节帧以当前 `src/HelmControl/ProtocolModel`
  实现为准。

## 2. 公共帧

| 字节 | 字段 | 类型 | 说明 |
|---|---|---|---|
| B1 | `sync[0]` | CONST | `0x55` |
| B2 | `sync[1]` | CONST | `0xAA` |
| B3 | `len` | U8 | 从 B4 起的数据段长度，范围 1..255 |
| B4 | `version` | CONST | `0x11` |
| B5 | `type_group` | U8 | 类型组 |
| B6 | `sub_type` | U8 | 子类型 |
| B7-8 | `seq` | U16 | 请求序号；响应规则见 2.2 |
| B9.. | payload | - | 具体字段以对应 CSV 为准 |
| 末 2B | `crc` | U16 | CRC-16/XMODEM，低字节先发 |

固定帧档如下：

| `len` | 数据段 | payload | 完整帧 |
|---:|---:|---:|---:|
| 48 | B4-B51 | 43 字节 | 53 字节 |
| 123 | B4-B126 | 118 字节 | 128 字节 |
| 232 | B4-B235 | 227 字节 | 237 字节 |

完整帧长度必须严格等于 `len + 5`。CSV 的 `index`、`length` 和字段累计偏移必须同时
成立；任何不一致都属于协议资产错误，不允许按行序容错。

### 2.1 CRC

- 算法：CRC-16/XMODEM。
- 多项式：`0x1021`。
- 初值：`0x0000`。
- 输入/输出均不反射，异或输出：`0x0000`。
- 覆盖范围：从 B3 `len` 到 payload 最后一个字节，不含同步字和 CRC 自身。
- 线上顺序：`CRC_LO CRC_HI`。
- 物理层已知向量：payload `4D 42 31` 的完整帧为
  `55 AA 03 4D 42 31 FC 89`。

### 2.2 序号

- PC 维护 U16 请求序号，每发送一帧递增，`0xFFFF` 后回到 `0`。
- 普通响应回显请求 `seq`；`error_response.seq` 与 `orig_seq` 都使用触发错误的请求序号。
- DH 控制的第 `i` 帧响应使用 `请求 seq+i`（`i=0..report_count-1`，U16 回绕）。
- HELM_FEEDBACK 没有对应请求：HELM_START 先返回回显请求序号的 ACK，随后反馈帧使用
  板端独立递增的 U16 主动上送序号，直至 HELM_STOP。
- IMU_STREAM_FEEDBACK 同样使用板端独立递增的 U16 主动上送序号；其中 `source_seq`
  是 COM4 payload 的普通数据字段，不参与产品协议匹配，也不做连续性判定。
- PC 同一时刻只保留一个普通待响应请求，按 `(type_group, sub_type, seq)` 匹配；DH 按
  本次请求序号和报告索引匹配，舵反馈按活动会话路由。
- 所有响应和主动上送共用同一发送锁。序号在进入发送器前已经确定，发送锁覆盖完整数据段
  的提交；COM TX Busy 时只等待硬件 ready 后重新提交尚未接受的同一帧：间隔 100 us，
  最多 1000 次，不改变序号。达到上限即报告通信失败；这是有界的 Transport ready
  流控，不属于协议超时自动重发。

## 3. CSV 合同与生成器

CSV 列名顺序固定为：

```text
index,length,type,name_cn,name_en,lsb,default,is_valid
```

合同规则：

- `index` 使用 `B1` 或 `B7-8` 格式；普通字段的字节跨度必须等于 `length`。
- 支持 `CONST`、`RESERVED`、`BIT`、`U8/U16/U32`、`S16`、`S16F/S32F` 和 `F32`。
- `CONST` 必须给出能装入字段宽度的整数默认值。
- `RESERVED.default` 为空时按 `0` 处理；若显式填写则只能为 `0`，发送时全部置零。
- `S16F/S32F.lsb` 为空时按 `1` 处理；显式填写时必须是正数。该默认值只确定线序换算，
  不代表未知硬件采样映射已经确认。
- `BIT.length` 表示位宽。共享同一 `Bn` 的 BIT 行按 CSV 行序从低位到高位排列，
  每个字节必须恰好累计 8 bit。`dh_status` 即采用四个 2-bit 字段共享一个字节。
- `is_valid` 只能是 `0` 或 `1`；`name_en` 在单份 CSV 内必须唯一。
- 协议资产集合必须严格等于本目录列出的 37 份 CSV，缺失或多余文件均失败。

校验：

```powershell
python .\tools\generate_product_protocol.py --check `
  .\docs\design\product_protocol_csv
```

生成 C++17 描述头文件：

```powershell
python .\tools\generate_product_protocol.py `
  --output .\build\generated\ProductProtocolGenerated.h `
  .\docs\design\product_protocol_csv
```

生成头提供稳定 CSV stem、Request/Response 方向、帧相对偏移、B4 数据段相对偏移、
BIT 位偏移、LSB 和默认值。输出不包含源目录或机器路径；构建系统应把输出放在构建目录，
不得提交临时生成物。

## 4. 指令集

| tg/st | 指令 | 请求主要字段 | 响应主要字段 | 帧长（请求/响应） |
|---|---|---|---|---:|
| `01/01` | SYSTEM_STATUS | 无 | CPU、内存、频率、PCIe、启动时间、温度、上电秒数 | 53/53 |
| `02/01` | MEMPERF_TEST | `memperf_type:U8`, `length:U32`, `seed:U32` | 错误计数、首错地址、耗时、读写带宽 | 53/53 |
| `02/02` | SPI_FLASH_TEST | 无 | `sjl_result:F32`，固定区擦写读回耗时秒 | 53/53 |
| `03/01` | BUS_LOOP_TEST | `link_id:U8`, `total_count:U32` | 链路、错误数、总数、耗时 | 53/53 |
| `03/02` | BUS_ECHO_TEST | `link_id:U8`, `data[114]:U8` | 链路和 114 字节数据 | 128/128 |
| `04/01` | DI_READ | 无 | `di_state[2]:U32` | 53/53 |
| `04/02` | DO_WRITE | `channel[2]:U32` | `applied_state[2]:U32` | 53/53 |
| `05/01` | ELEC_HEALTH_STATUS | 无 | C/B 组、`activate_bits`、各路电压及 B31-B32 `value_YX:S16F` | 53/53 |
| `06/01` | DH_PULSE_CONFIG | 配置使能、23 路 U16 毫秒脉宽 | 23 路脉宽读回 | 128/128 |
| `06/02` | DH_CONTROL | 电源/回线使能、23 位通道、次数、间隔和延时 | 使能读回、23 路 2-bit 状态及 23 路 `S32F` 遥测 | 53/128 |
| `07/02` | HELM_BOARD_TEST | B9 低 4 位保留、高 4 位方向；B10-B13 四路 U8 百分比 | `pwm_duty_match`、方向、raw duty、peak、四路 AD7606 和使能状态 | 53/53 |
| `07/10` | HELM_START | `waveform:U32`、`freq/ampl/offset/start/max_freq/sweep_duration_s:F32`、`enable:U32` | `helm_version=0x01`、状态和错误码 | 53/53 |
| `07/11` | HELM_STOP | 无 | 状态和错误码 | 53/53 |
| `07/01` | HELM_FEEDBACK | 无请求 | 状态、错误码、首样本 DDS 时间戳和 1..5 组完整 41 字节舵反馈 | -/237 |
| `08/01` | TIMER_JITTER_START | `mode:U32` | `buckets[8]:U32`, 平均/最大抖动 F32 | 53/53 |
| `08/02` | TIMER_JITTER_STOP | 无 | 状态和错误码 | 53/53 |
| `09/10` | IMU_STREAM_START | 无 | 状态和错误码 | 53/53 |
| `09/01` | IMU_STREAM_FEEDBACK | 无请求 | 状态、错误码、源序列、12 个 F32、温度、自检/工作状态、软件版本原始值和源保留字段 | -/128 |
| `09/11` | IMU_STREAM_STOP | 无 | 状态和错误码 | 53/53 |
| `FF/00` | ERROR | 无请求 | 原 tg/st/seq、错误码和 detail | -/53 |

已知指令执行失败时，使用该指令响应中的 `status/err_code`。未知指令、版本错误、固定
长度错误或无法解析的参数使用 `FF/00`；其中 `orig_seq` 保留请求序号。字段顺序必须按
CSV 读取，不能假定所有响应都以 `status/err_code` 开头；例如 HELM_START 响应先发送
`helm_version`。

## 5. 特殊流程

### 5.1 DH 脉宽配置与控制

1. `DH_PULSE_CONFIG.config_enable=1` 时，板端向 `0x1E0` 写 `0xAAAA`，依次写入并回读
   23 路 U16 毫秒脉宽，配置使能保持开启；为 `0` 时向该寄存器写 `0xFFFF`，忽略请求中
   的 `pulse_width[]`，只执行失能和当前脉宽读回。
2. `DH_CONTROL` 只接受 `channel[0]` 的 bit 0..22；bit 23..31 或非零 `channel[1]`
   均返回 `PARAM_OUT_OF_RANGE`，不得静默忽略。
3. 控制顺序固定为：向 `0x1DC` 写电源使能（`0xAAAA/0xFFFF`），向 `0x04` 写回线使能
   （`0xA000/0x00A0`），配置 Multiple 点火模式，然后提交所选通道。即使任一使能值为
   `0`，非空通道仍提交点火命令，由硬件使能状态决定实际输出。所选通道去重后每四路
   一批；末批不足四路时重复最后一个有效通道填满，避免含 `0xFF` 的尾批在实板不响应。
4. `report_count=N` 表示响应总帧数，范围 `1..65535`；`interval_us` 范围为
   `2500..65535`；`delay_frames` 为 U16 且必须小于 `report_count`，默认值分别为 50、
   2500 us 和 5。power/return 只允许 0/1，二者为 0 仍是合法测试输入；通道位图必须非空、
   只使用 `channel[0].bit0..22` 且 `channel[1]=0`。全部业务参数在任何硬件写入前验证，
   非法时只发送一帧 `PARAM_OUT_OF_RANGE`。
5. `delay_frames=N` 表示先采集并发送 N 帧只读基线，再在下一帧采样前恰好提交一次点火；
   N=0 时在首帧前提交。基线采集或发送失败立即结束且不得点火；点火后采集错误以错误响应
   占用当前帧并继续剩余回告，发送失败终止。协议不提供 STOP/ABORT，结束后不自动复位。
6. 第 `i` 帧响应的 `seq=请求 seq+i`（U16 回绕）。板端按单调时钟等待截止点，记录本帧
   实际采样起点、采样并立即发送；下一截止点使用“本帧实际采样起点 + interval_us”。串口
   发送只占当前周期余量，不额外叠加到请求间隔；发送超过周期时下一帧不会追赶压缩，实际
   采样起点间隔仍不小于 `interval_us`。本版不要求突发期间并行处理其他命令，也不做协议级
   自动重发。
7. 电源读回按 `0xAAAA/0xFFFF` 映射为逻辑 1/0；回线寄存器虽然写入
   `0xA000/0x00A0`，读回低 8 位按 `0xAA/0xFF` 映射为 1/0。每路点火状态按寄存器
   `0xFFFF/0xAAAA/0xBBBB` 映射为 `0/1/2`（未 DH/成功/失败）。
8. 每帧报告读取同一份 ADS1258 快照。`telemetry[0]=raw[1]`（局部 `0x84`，热电池
   激活），`telemetry[1..22]=raw[4..25]`（局部 `0x90..0xE4`，22 路点火电压）。板端
   完成按通道定标后以 `0.001 V/LSB` 编码 `S32F`；读取或编码失败时整帧返回执行错误，
   不把脉宽寄存器或部分字段伪装成遥测。

### 5.2 舵控板级测试

1. `HELM_BOARD_TEST` 是普通单次请求/响应。请求 B9 bit0..3 为
   `pwm_command_reserved`，必须为 0；bit4..7 依次为 `direction[0..3]`；B10-B13 依次为
   `pwm_duty_percent[0..3]:U8`，只接受整数 `0..100`。
2. 板端读取当前 `peak_value`，使用 64 位中间值按
   `raw=(uint64_t(peak_value)*percent+50)/100` 四舍五入为寄存器 duty，方向位独立写入。
   四路 PWM 使能位图固定写为 `0x0F`，提交输出后打开 PWM update；同时打开 AD7606
   采集和滤波。百分比越界返回 `PARAM_OUT_OF_RANGE`，不得执行硬件写入。
3. 响应 B12 低四位回传 `pwm_duty_match[0..3]`，每一位表示对应 raw duty 是否与本次
   百分比换算值精确一致；高四位回传方向。另外回传四路 raw duty、peak、四路 AD7606
   原始码、PWM 使能位图、PWM update 状态以及 AD7606 采集/滤波状态。AD7606 字段按 CSV 的
   `10/65536 V/code` 由 PC 解码。
4. PWM peak、duty、方向、四路使能、update 或 AD7606 采集/滤波任一回读不符合预期，
   返回 `REG_RW_FAILED (0x0201)`。该命令不查询连续舵机 DDS bridge 的活动状态，也不会
   因其运行而返回 `TASK_BUSY`。
5. 该命令不恢复测试前状态，成功或中途失败时已经完成的 PWM、方向和使能写入均可能
   保持。只能在已隔离且明确允许硬件写入的目标板运行。

### 5.3 DDS 舵机连续实测

1. 波形和使能位图是 U32；频率、幅值、偏置、起始相位、扫频上限和扫频总时长是 F32，
   其中 `start` 单位为 rad。波形取 0..4、使能只取低四位，频率/上限/时长为有限正数；
   DUT 与 Web 不限制幅值或偏置，独立舵控程序保留自身内部限幅。
2. 正弦、方波和三角波共用 `phase = 2*pi*freq*t + start`，恒值波只输出 offset。连续
   对数扫频在 `f0 != f1` 时使用
   `2*pi*f0*f1*T/(f1-f0)*ln(f1*T/(f1*T-t*(f1-f0))) + start`，`T` 取请求中的
   `sweep_duration_s`；`f0 == f1` 时退化为定频。超过 T 后命令归零，但反馈保持到 STOP。
3. START 只打开 `HelmDdsTestBridge`：create-or-get `local:://helm_command` writer，并以
   已提交可见的当前 DDS sequence 为原子边界创建 `local:://helm_feedback` reader；reader
   跳过边界前仍保留的历史反馈，并补读快照到注册完成之间的新反馈，不清 Topic 或共享
   RingBuffer，也不为快照等待 Topic 写锁。bridge 在 reader 打开前先进入接收态，因此端点
   建立期间到达的边界后反馈会进入私有队列；START 只有在首帧指令发布成功后才返回成功。
   随后以 1 ms 周期发布四路共用波形，未启用通道为零。
   writer 发送严格 27 字节
   `Helm_ins_frame`；B27/U8 名为 `helm_unlock`，`0x00` 表示无请求，连续测试的所有指令
   均固定为 `0xFF` 请求解锁。该字段不属于 COM3 产品 CSV，不由 PC 设置；现行线序以
   当前 `src/HelmControl/ProtocolModel` 实现为准。reader 只接受严格 41 字节
   `Helm_fdb_frame`。
4. 每个 `HELM_FEEDBACK` 使用 232 字节数据段，包含首个 DDS 微秒时间戳和 1..5 个完整
   样本；每样本保留 U16 相对时间、`serial_a/serial_b`、版本、四路 `ins/fdb`、六项
   2-bit 自检及 timeout。反馈使用产品端统一发送器取得产品帧序号。
5. STOP 先停止并 `join` 本次指令线程，再于 DDS 端点关闭前发布一帧四路零位且
   `helm_unlock=0xFF` 的尾帧，然后关闭端点并清空反馈队列。它不访问板级
   `07/02` PWM/AD7606 路径，也不发送关舵锁或禁止 PWM 语义。
   bridge 内的 START 与 STOP 生命周期操作互斥执行，端点打开失败、首帧失败或启动异常
   都会退出接收态、关闭端点并清空该轮私有反馈。
   同一流的 START ACK、主动反馈和 STOP ACK 保持线序：已开始发送的反馈先完成，STOP ACK
   之后不得再发送该会话旧反馈。DDS 端点或发布失败返回 `HELM_DDS_FAILED (0x0301)`；同一
   bridge 重复 START 可返回 `TASK_BUSY`，但 `HELM_BOARD_TEST 07/02` 与该状态无关。
6. `MB_DDF_v2_HelmControl` 是用户独立启停的进程，测试服务不创建、终止、探测、等待或
   占有它；DDS create-or-get 允许任意启动顺序。舵控进程在打开 PWM transport 后先直接将
   四路 enable 写为全关，随后关闭 update gate 并写入零 duty；首次 `helm_unlock=0xFF` 由控制线程
   将 DIDO DO0 置高，从写入成功时刻起使用单调时钟至少等待 30 ms 才使能 PWM。
   成功后该状态在进程内单向锁存，后续 `0x00`、重复 `0xFF` 或 STOP 均不会关舵锁、禁止
   PWM 或重置 30 ms 计时。连续实测与 `07/02` 板级测试没有生命周期、互斥、忙状态
   或其他绑定，`07/02` 不包含舵锁流程。

### 5.4 定时器

- START 的 `mode` 是 U32，且只允许 `0` 或 `1`。`mode=0` 不叠加额外负载，作为基线；
  `mode=1` 在统计期间叠加只读 XDMA C2H0 负载。大于 `1` 返回
  `PARAM_OUT_OF_RANGE`。
- 两种模式都固定按 250 us 周期采样 250 个周期。八个桶是 U32 计数，平均和最大抖动
  是 F32 测量值；START 同步完成全部采样并只返回一帧统计，不产生周期或异步回告。
- mode 1 使用 `XdmaConfig{"/dev/xdma0", 0, 0x1000, -1, 0, -1}`：只打开 C2H0，
  不打开 H2C，不执行 DMA 写入；从设备偏移 `0` 每 12 ms 读取一次 64 KiB，只有完整
  返回 65536 字节才计为成功。这里的偏移 `0` 是 C2H 读取地址。
- mode 1 在采样前启动负载，采样完成后先停止并 `join` 线程，再关闭 Transport 和发送
  START 响应。缺少负载执行器、打开失败、DMA 失败、短读或线程启动失败均返回
  `TASK_EXEC_FAILED`；已有负载尚未清理时返回 `TASK_BUSY`。
- 收到成功 START 响应后，PC 自动发送 STOP。STOP 是采样结束后的幂等清理 ACK，不能
  中途取消同一接收线程中同步执行的 START；STOP ACK 的成败通过测试状态和日志展示，
  不覆盖界面中 START 返回的桶、平均值和最大值。
- 当前 `dma_read()` 基于阻塞 `pread`，没有超时或跨线程取消能力；其收尾风险见
  “5.6 已知实现限制”。

### 5.5 惯测连续流

1. PC 经 COM3 发送一次 `IMU_STREAM_START 09/10`；板端完成 COM4 打开、配置、清错和接收
   使能后先回 START ACK，随后才允许主动反馈。
2. COM4 固定使用 `/dev/xdma0`、`user_offset=0x100000`、`map_length=0x40000`、event 3、
   `921600 / 8E1`、波特率计数 `0x0086`。该计数由
   `round((0x00CA+1)*614400/921600-1)=134` 推得。格式寄存器 `0xB0` 让 FPGA 处理
   2 字节 CRC；接收帧头配置为 `AA 1A`、1 字节长度，C++ 只收到 59 字节 payload。
3. payload 为小端：`source_seq:U16`、12 个 F32、`temperature:S16F`（0.1 ℃/LSB）、
   `self_test_status:U16`、`work_status:U8`、`software_version:U16`、
   `source_reserved:U16`。12 个 F32 必须有限；其他状态/范围值不拦截。
4. 每个有效 payload 立即编码为 `09/01` 128 字节反馈并尝试经 COM3 发送，不插入固定
   sleep、不主动抽样。400 Hz 不要求软件保证无损；COM3 614400/8E1 的理论占用约 91.7%。
5. PC 对同一会话最多发送一次 `IMU_STREAM_STOP 09/11`。板端停止读取并关闭 COM4 后再回
   STOP ACK；ACK 超时或错误只影响本次结果，PC 收尾不得再次发送 STOP。COM4 流活动时，
   使用同一硬件的 BUS link 3 返回 `TASK_BUSY`。
6. 主机侧的设备流运行模式、停止和保存语义见
   [设备通信协议契约](../../../../docs/design/contracts/device-communication-protocol.md)。

### 5.6 已知实现限制

以下为当前源码已知限制，不表示已修复：

- `ProductProtocol::parse_request()` 对仅含合法 `version`、长度为 1 或 2 的数据段，在长度
  防护前访问 `data_segment[1]`/`[2]`，存在越界访问风险。
- `MB_DDF_v2_HelmControl` 的 ADC `read_snapshot()` 失败后继续使用上次反馈值（首次为 0），
  PWM 写失败也只记录告警并继续循环；DDS 命令只缓存最新帧，没有新鲜度 watchdog，命令源
  中断后控制仍会使用旧命令。
- 舵机连续实测只能跳过 reader 快照前已经提交的历史反馈。若上一轮尾指令的反馈在下一轮
  快照后才写入 DDS，它具有新的 DDS sequence；当前 27/41 字节舵控帧没有会话 epoch，bridge
  无法仅凭时间戳或 `serial_a/serial_b` 可靠识别并过滤该晚到反馈。
- 舵机连续实测的 reader 注册仍使用 DDS 全局 named semaphore；该既有注册锁没有 deadline。
  若另一个存活进程永久占有该 semaphore，`HELM_START` 可能无法有界返回。
- `TIMER_JITTER_START` 的 `mode=1` 使用无超时、不可跨线程取消的阻塞 `pread`；停止路径在
  关闭 transport 前 `join` 工作线程，若读取不返回，`join` 可无限等待。
- `SystemTimer` 创建并写入 stop eventfd，但 worker 实际只等待实时信号；启动失败且
  `running_` 尚未置位时，`stop()` 的早退路径不会关闭该 eventfd。该机制不提供额外取消保证，
  并存在失败路径文件描述符泄漏。

## 6. 测试项映射

“执行全部”顺序固定如下，本文已合并全部映射，不依赖其他映射文档：

| 顺序 | 测试项 | 请求/响应 CSV | 默认参数 |
|---:|---|---|---|
| 1 | 系统状态 | `system_status_request/response` | 无 |
| 2 | 内存 | `memperf_test_request/response` | 类型 0-6；`length=65536 KB`, `seed=0x5A5A5A5A` |
| 3 | SPI Flash | `spi_flash_test_request/response` | 固定隔离测试区，见 7.4 |
| 4 | 总线 | `bus_loop_test_*` 或 `bus_echo_test_*` | 可选 link 0/1/3；默认 link 0，`total_count=1000`；echo `4D 42 31` |
| 5 | DI | `di_read_request/response` | 无 |
| 6 | DO | `do_write_request/response` | 两组位图默认 0，仅 bit 0-15 有效 |
| 7 | 电气健康 | `elec_health_status_request/response` | 无 |
| 8 | DH 脉宽配置 | `dh_pulse_config_request/response` | 使能；DH0=80 ms，DH1..22=63 ms |
| 9 | DH 控制 | `dh_control_request/response` | 电源/回线使能；`0x007FFFFF/0`，count 50，interval 2500 us，delay_frames 5 |
| 10 | 舵控板级 | `helm_board_test_request/response` | 四路 PWM 百分比=0，四路方向=0 |
| 11 | 定时器 | `timer_jitter_start/stop_*` | mode 0/1，默认 0；250 us x 250 周期 |

主机侧“执行全部”、连续运行、超时、保存和设备流会话语义见
[设备通信协议契约](../../../../docs/design/contracts/device-communication-protocol.md)。本文只保留
产品端的命令、字段和硬件行为。

## 7. 硬件映射与边界

### 7.1 总线

| link_id | 链路 | 固定参数/行为 |
|---:|---|---|
| 0 | COM1 | 可测试；BUS_LOOP 使用内部 `loopback=true`，BUS_ECHO 使用外部原样回送 |
| 1 | COM2 | 可测试；BUS_LOOP 使用内部 `loopback=true`，BUS_ECHO 使用外部原样回送 |
| 2 | COM3 | 产品协议控制口；在打开任何硬件前返回 `CHANNEL_INVALID` |
| 3 | COM4 | 可测试；IMU_STREAM 活动时先返回 `TASK_BUSY`，不打开该硬件 |
| 其他 | - | 返回 `CHANNEL_INVALID` |

BUS_LOOP 的 `total_count` 只允许 `1..100000`。每轮发送后均以 5 秒作为接收截止时间；
只有实际完成次数严格等于请求且 `error_count=0` 才返回成功，否则命令失败。

BUS_ECHO 的每个 DUT `03/02` 请求只执行一轮：DUT 在所选 COM 以 `loopback=false` 发送请求中的
全部 114 字节，独立外部 PC/夹具必须在 5 秒内原样回发。DUT 严格比较接收长度和每一个字节；
任何不一致都失败，响应中的 `data[114]` 回传实际接收字节供宿主核对。多轮由根宿主
`mbddf.serial_test` 在同一次 BIZ `single` 的一个 `executeStep()` 内顺序复合，每轮均为独立
`03/02` 与 5 秒回显事务；旧 PyQt 仅经 COM3 控制口下发单次请求，不能充当外部 ECHO 对端。

BUS 不再包含网口或 SPI Flash 路径。SPI Flash 仅由独立 `SPI_FLASH_TEST` 在固定隔离测试区
执行，不承载任意 BUS payload。

### 7.2 系统状态事实源

- CPU 使用率来自两次 `/proc/stat` 采样，内存来自 `/proc/meminfo`，`power_on_sec` 来自
  `/proc/uptime`。小核/大核当前频率优先来自 CPU0/CPU4 的标准 cpufreq；目标内核未注册
  cpufreq policy 时，分别读取 debugfs 的 `scmi_clk_cpul/clk_rate` 和
  `scmi_clk_cpub01/clk_rate`，按 Hz 转 MHz。该回退要求 HW_TEST 以 root 运行且 debugfs
  已挂载。
- CPU/RK 温度来自 `center_thermal` hwmon（CPU 温度可回退到名称含 `cpu`/`soc` 的
  thermal zone）。当前 `net_init_time` 固定回传 `0 s`；K7 温度直接读取 XADC 全局
  `0x150000` 的局部 `0x200`，按 `ADC_code*503.975/4096-273.15` 换算。
- PCIe 速率/宽度只跟随 XDMA 事实链：优先解析
  `/sys/class/xdma/xdma0_user/device`；若该入口不存在，则由 `/dev/xdma0_user` 的字符
  设备 major/minor 跟随 `/sys/dev/char/<major>:<minor>/device`。旧 `xdma0` 入口仅作兼容
  回退。解析得到真实 BDF 后读取其 `current_link_speed/current_link_width`，不得扫描并
  猜测其他 PCIe 控制器，也不在运行时调用和解析 `lspci`。
- 任一必需来源缺失，SYSTEM_STATUS 整体返回 `TASK_EXEC_FAILED`；响应中的默认零值不得
  被解释为有效测量。
- `MEMPERF_TEST` 只接受 type 0..6。type 0/1/2 分别使用固定 seed、seed/反码交替、
  seed 异或字索引三种图样，均测试当前进程分配的内存，不代表 FLASH/RAM/DDR 三种
  物理介质；type 3/4/5/6 分别测内存读、写、拷贝和 NT Store 带宽。type 7 以上返回
  `PARAM_OUT_OF_RANGE`。

### 7.3 电气健康、舵反馈和 DH 遥测

- `DO_WRITE` 只使用 `channel[0]` 低 16 位，并以更新掩码 `0xFFFF` 整幅写入 DO0..15；
  `channel[1]` 被忽略，响应 `applied_state[1]` 固定为 0。位值 1 命令 DIDO 输出高、
  位值 0 命令输出低；DO3/DO4 是物理低使能，所以这两路 bit=0 才表示功能使能。
- DIDO 地址表仍确认 DO3=`24V_EN`、DO4=`DYT_5V_EN`，两路均为物理低使能；DO5 是
  `DI_EN1`、DO6 是 `DO_EN`，均不得占用作 5V_JS。当前产品协议不提供独立电源测试；
  健康采样侧的 XADC VAUX0/`0x240` 仍可读取。
- 板端直接发送 FPGA 原始码。`value_YX` 为 XADC VAUX8/局部 `0x260` 的 `Data[15:4]`，
  位于 `ELEC_HEALTH_STATUS` B31-B32，CSV LSB=`10.09/4096 V/code`；四路
  `helm_AD_value[0..3]` 位于 `HELM_BOARD_TEST`，为 AD7606 通道 0..3 的有符号 16 位 raw，
  CSV LSB=`10/65536 V/code`。PC 解码时乘 LSB 得到伏特值。
- ADS1258 使用 v4 的两芯片诊断定标：每次快照必须读取并解码所属芯片的
  `OFFSET/VCC/TEMP/GAIN/VREF`，VCC/TEMP 参与诊断有效性检查，样本再按
  OFFSET/GAIN/VREF 修正；全局通道 1/2/3 另应用外部偏置和 `19.18` 增益，其余通道按
  当前分段拟合换算。`HardwareTestProvider` 使用
  `calibrated_channel_voltage()`，不使用旧的无状态换算。诊断寄存器、完整公式、拟合
  边界和“仅为当前实现、尚非真机标定”的限制以
  [硬件层详细设计](../hardware-layer-architecture.md) 为唯一主定义。
- ELEC_HEALTH 已确认 `c_volt=raw[0]`/`0x80`、`b_volt=raw[2]`/`0x88`、
  `v28_5=raw[3]`/`0x8C`，板端换算后按 `0.01 V/LSB` 编码；同时确认 XADC 的
  `external_vol`、`core_vol`、`assist_vol`、`js_5V=0x240`、`dyt_5V` 和 `power_24V`
  通道及定标。PC 将 `activate_bits.bit0` 显示为“BC激活好”；该位读取 DH
  `BatteryStatus` 高 8 位，`0xAA` 表示已激活、`0xFF` 表示未激活；bit1..7 恒为 0 且不显示，
  低 8 位点火状态不参与该字段。其他高字节值经
  `status_error()` 映射为 `REG_READ_WRITE_FAILED`；不根据电压阈值推导位图。
- DH 点火状态来自 DH_ctrl；23 路 `telemetry[]` 使用 ADS1258 `raw[1]` 与 `raw[4..25]`，
  板端换算后按 `0.001 V/LSB` 编码。PC 只按 CSV LSB 还原伏特值，不执行运放分段公式。
- DH 已确认寄存器为：回线使能 `0x04`（开/关 `0xA000/0x00A0`）、电源使能
  `0x1DC`（开/关 `0xAAAA/0xFFFF`）和脉宽配置使能 `0x1E0`
  （开/关 `0xAAAA/0xFFFF`）。
- HW_TEST 启动经使能/状态回退保护应用 ADS1258 v4 运行时覆盖（Config0、Config1、
  SYSRED、WORK_COUNT、SPI divider 与 C/B 激活阈值），以建立内部 OFFSET 和双芯片诊断
  前置状态，随后恢复两片使能；两项阈值保持当前源码值、等待最终标定，不应在产品协议层
  表述为已完成硬件标定或自动通过判据。

### 7.4 SPI Flash

- 设备固定为 `/dev/spidev0.0`。
- 测试区固定为 `0x03FFF000` 起始的 4 KiB，仅允许在可重刷的隔离板执行。
- HW_TEST 会擦除、写入并读回该区域，本版不备份、不恢复；进程终止、掉电或失败均可能
  留下测试数据。这一行为与带恢复流程的原 Demo 不同。

## 8. 结果与错误

测试模式只报告：

- 执行完成：命令已执行并返回可解析结果。
- 执行失败：板端返回 `status != 0` 或 `err_code != 0`。
- 通信失败：超时、断链、物理层丢帧或无法完成响应。

测量值、错误计数、位图和带宽只展示，不配置硬件通过/失败阈值，不生成“测试通过”结论。
本版也不实现安全确认、状态恢复、自动重试、夹具检测或报告导出。

这里的“不自动重试”指 PC/板端不在超时后重新发起产品协议请求；COM TX `Busy` 时对尚未
被硬件接收的同一数据段做上述有界重交不产生新协议帧或新序号。

| err_code | 名称 | 含义 |
|---:|---|---|
| `0x0000` | OK | 执行完成 |
| `0x0001` | CMD_UNKNOWN | 指令或版本不支持 |
| `0x0002` | LEN_MISMATCH | 固定长度不匹配 |
| `0x0003` | CRC_ERROR | 预留给物理层诊断；C++ 不重复校验 |
| `0x0101` | CHANNEL_INVALID | 通道或 link_id 无效 |
| `0x0102` | PARAM_OUT_OF_RANGE | 参数越界 |
| `0x0103` | SAFETY_LIMIT | 预留安全限制；当前实现未返回 |
| `0x0201` | REG_RW_FAILED | 寄存器访问或硬件回读异常 |
| `0x0202` | MEM_ACCESS_FAILED | 存储器不可访问 |
| `0x0203` | TASK_EXEC_FAILED | 任务流程、超时或数据编码失败 |
| `0x0204` | TASK_BUSY | 同一连续流重复 START、定时器/DH 任务忙；IMU 活动时 BUS link 3 返回该错误；舵机连续流不限制 `07/02` |
| `0x0301` | HELM_DDS_FAILED | 舵机 DDS 端点建立或指令发布失败 |

## 9. CSV 资产

| 请求 CSV | 响应 CSV | tg/st |
|---|---|---|
| `system_status_request.csv` | `system_status_response.csv` | `01/01` |
| `memperf_test_request.csv` | `memperf_test_response.csv` | `02/01` |
| `spi_flash_test_request.csv` | `spi_flash_test_response.csv` | `02/02` |
| `bus_loop_test_request.csv` | `bus_loop_test_response.csv` | `03/01` |
| `bus_echo_test_request.csv` | `bus_echo_test_response.csv` | `03/02` |
| `di_read_request.csv` | `di_read_response.csv` | `04/01` |
| `do_write_request.csv` | `do_write_response.csv` | `04/02` |
| `elec_health_status_request.csv` | `elec_health_status_response.csv` | `05/01` |
| `dh_pulse_config_request.csv` | `dh_pulse_config_response.csv` | `06/01` |
| `dh_control_request.csv` | `dh_control_response.csv` | `06/02` |
| `helm_board_test_request.csv` | `helm_board_test_response.csv` | `07/02` |
| `helm_start_request.csv` | `helm_start_response.csv` | `07/10` |
| `helm_stop_request.csv` | `helm_stop_response.csv` | `07/11` |
| - | `helm_feedback_response.csv` | `07/01` |
| `timer_jitter_start_request.csv` | `timer_jitter_start_response.csv` | `08/01` |
| `timer_jitter_stop_request.csv` | `timer_jitter_stop_response.csv` | `08/02` |
| `imu_stream_start_request.csv` | `imu_stream_start_response.csv` | `09/10` |
| - | `imu_stream_feedback_response.csv` | `09/01` |
| `imu_stream_stop_request.csv` | `imu_stream_stop_response.csv` | `09/11` |
| - | `error_response.csv` | `FF/00` |
