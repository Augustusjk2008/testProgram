# 舵机远端连续测试设计

> 状态：首期软件实现完成；宿主、前端、DUT 单元/交叉构建证据已记录，目标板与真实舵机联合实测待执行。
> 现行事实源以对应架构、协议和测试规范为准；本文保留本轮设计决策与首期范围。

## 目标

在现有板级测试之外新增 `mbddf.helm_stream` 舵机实测。PC 只负责选择测试、编辑本次运行参数、启动/停止、展示并保存样本；DUT 负责按 1 ms 周期生成连续角度指令，通过 MB_DDF DDS 发送给独立舵控程序，并把舵控反馈经 COM3 主动回告 PC。

首期不计算舵机性能，不生成伯德图。首期必须保存足够完整、带时间与序号的原始数据，使后续性能算法不需要修改采集协议。

## 明确不做

- 不把 `HW_TEST` 与 `helm_control` 的进程生命周期绑定。
- 不由测试程序启动、停止、暂停、重启或管理 `helm_control`。
- 不在舵机连续测试与 `HELM_BOARD_TEST 07/02` 之间增加互斥、占用检查或所有权交接；用户可自行决定远端程序启停与测试并发方式。
- 不在 PC 界面或 DUT 测试桥接中限制舵角、幅值或偏置范围。`helm_control` 当前内部钳位保持不变。
- 不在首期定义性能分析任务、结果 DTO 或伯德图接口。

## 总体链路

```text
Web 动态参数表单
  -> WebSocket start.algorithmParameters
  -> TestApplicationController（合并默认值并调用算法层校验）
  -> BIZ（不解释参数，只透传）
  -> mbddf.helm_stream 执行器
  -> COM3 HELM_START / HELM_FEEDBACK / HELM_STOP
  -> DUT HelmDdsTestBridge（连续指令生成与反馈批量转发）
  -> local:://helm_command
  -> 独立 helm_control 进程
  -> 舵机闭环
  -> local:://helm_feedback
  -> 原路回到 PC
```

`helm_control` 纳入 `dut/` 构建并产出独立可执行文件，但由用户自行启动和停止。DDS Topic 的创建采用与启动顺序无关的方式；任一进程先启动都不得要求另一进程由其管理。

## 算法参数描述

算法层是参数语义和校验的事实源，提供只依赖 Qt Core 的参数描述值类型。每个参数至少声明：

- 稳定 ID、显示名称、说明、类型和单位；
- 默认值、枚举选项、是否必填；
- 条件显示规则；
- schema 版本。

应用层把配置默认值与浏览器运行时覆盖值合并，调用算法层完成最终归一化和校验。BIZ 公共运行选项只增加不透明 `QVariantMap` 参数并原样传到算法执行上下文，不解释舵机或 DH 语义。

Web 根据描述生成通用表单，按 `configId + schemaVersion` 把覆盖值保存到浏览器本地。界面不回写 `.testcfg.json`。本次实际生效参数进入快照、日志和连续数据记录。

首期接入：

- `mbddf.helm_stream`：`waveform`、`freq`、`ampl`、`offset`、`start`、`max_freq`、`sweep_duration_s`、`enable`；
- `mbddf.dh_pulse_config`：`config_enable` 与 `pulse_width[0..22]`。

舵机参数沿用产品协议字段名。波形枚举固定为正弦 `0`、方波 `1`、三角波 `2`、恒值 `3`、连续对数扫频 `4`。舵角相关 F32 只要求可编码为有限数，不设置数值范围；频率和扫频时长必须为正数；通道位图只使用低四位。

## 指令生成语义

- 指令周期固定为 1 ms；四路启用通道使用同一波形值，未启用通道发送零。
- 普通波形持续运行，直到用户停止。
- 对数扫频使用可配置起始频率 `f0`、终止频率 `f1` 和总时长 `T`。相位沿用参考积分公式：

```text
phase(t) = 2*pi*f0*f1*T/(f1-f0) * ln(f1*T/(f1*T-t*(f1-f0))) + start
```

- `f0 == f1` 时退化为定频正弦。
- `t > T` 后四路指令输出零，但测试与反馈流保持活动，直至用户手动停止。
- 指令帧的 `Q=0`、`temp_imu=30`、`temp_ground=30`、`plug_detach=0` 作为首期固定值，不暴露为界面参数。

## DUT 结构

`dut/` 新增两个相互独立的组成部分：

1. 独立 `helm_control` 可执行目标：从 `tmp/helm_control` 提升正式源码，继续使用真实 27 字节 `Helm_ins_frame` 与 41 字节 `Helm_fdb_frame` DDS payload；不修改内部舵角钳位。
2. `HW_TEST` 内的 `HelmDdsTestBridge`：接收 PC START 参数，启动 1 ms 指令生成；发布 `helm_command`，订阅 `helm_feedback`，按产品协议批量上送；收到 STOP 后停止生成并返回 ACK。

桥接不直接访问 PWM 或 AD7606。`HELM_BOARD_TEST 07/02` 继续走现有直接硬件路径，并移除与连续舵控活动状态相关的 `TASK_BUSY` 判断。

## COM3 产品协议

复用现有命令号：

- `HELM_START 07/10`：增加 `sweep_duration_s:F32`，其余字段保持 `waveform/freq/ampl/offset/start/max_freq/enable`。
- `HELM_FEEDBACK 07/01`：改为携带真实 DDS 反馈语义，不再由 HW_TEST 自行读 AD7606。
- `HELM_STOP 07/11`：停止 DUT 指令生成和反馈上送，不管理 `helm_control` 进程。

反馈按最多五个样本批量编码，以适配单字节物理长度和 614400/8E1 带宽。批次携带首样本 DDS 时间戳，每个样本携带时间增量以及完整反馈语义：`serial_b`、`version`、四路 `fdb`、自检位组、`timeout`、`serial_a` 和四路回显 `ins`。不足五个样本时允许低延迟发送短批次。

START 只表示 DUT 生成器和 DDS 端点已就绪，不启动或管理 `helm_control`。运行中长期没有反馈应作为测试数据/通信异常报告，但不得触发对远端进程的控制动作。STOP 必须保持可调用并完成本测试自身的收尾。

## PC 执行与样本契约

`HelmStreamAlgorithmExecutor` 采用与惯测流相同的 START/主动反馈/STOP 生命周期，但输出逐样本事件。每条应用样本至少包含：

- DUT 生成指令关联序号；
- DDS `serial_a`、`serial_b` 与 DDS 时间戳；
- 四路回显指令 `ins[0..3]`；
- 四路角度反馈 `fdb[0..3]`；
- 版本、自检各字段、合并自检与超时标识；
- 产品帧序号、丢帧/不连续信息；
- 本次生效的算法参数或可追溯参数标识。

`ContinuousDataRecorder` 保存配置声明的全部舵机测量列，不因图表选择减少保存列。请求参数和反馈帧回显指令必须分开命名；反馈帧的 `ins` 是 DDS 输入回显，不宣称为舵控内部钳位后的值。

## Web 行为

- 舵机配置只声明 `device_stream`。
- 参数表单在运行期间只读；停止后可再次编辑并开始新一轮。
- 扫频字段仅在 `waveform=4` 时显示；普通波形不使用 `max_freq` 与 `sweep_duration_s`。
- 运行参数保存在浏览器本地，但每次 START 都发送完整有效参数，后端不依赖浏览器历史状态。
- 首期实时曲线展示回显指令和四路反馈；不展示伯德图或性能结论。

## 验证边界

- 参数 schema、默认值合并、条件字段、服务端二次校验和未知字段拒绝的宿主单元测试。
- WebSocket 描述与 START 参数契约测试；前端动态表单、按配置持久化和运行期锁定测试。
- 舵机执行器的 START、批量反馈拆样本、序号/时间戳、STOP、取消和无反馈错误测试。
- DUT 指令生成公式、1 ms 序列、扫频结束归零、通道位图、DDS payload 编解码和五样本批次测试。
- `HELM_BOARD_TEST` 与舵机连续流无互斥的回归测试。
- 独立舵控目标 AArch64 交叉构建；目标板实测记录两个进程的人工启停方式、DDS Topic、COM3 数据率、序号连续性和完整原始数据。
