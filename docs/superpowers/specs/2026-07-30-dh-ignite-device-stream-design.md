# DH 点火有限设备流设计

> 状态：设计已确认并实现；自动化验证记录见测试规范，真实 DH 写入和目标板验收不在默认验证范围。
> 现行事实最终以架构、协议契约、测试规范和已核对实现为准。

## 目标

在保留既有 `mbddf.dh_pulse_config` 单次脉宽配置测试的前提下，新增独立的
`mbddf.dh_ignite_stream` 点火测试。PC 发送一次 `06/02 dh_control` 请求，DUT 先采集指定
帧数的点火前基线，再执行一次点火控制，继续回告到指定总帧数并自然结束。

该测试只支持 `device_stream`。有限流由算法收到配置数量的回告后自然完成，不新增第四种
通用运行模式，也不使用 `intervalMs` 或 `maxCycles` 表达产品帧间隔和帧数。

## 明确不做

- 不修改或复用 `mbddf.dh_pulse_config` 的测试语义。
- 不新增 DH STOP/ABORT 产品命令；请求被 DUT 接受后不可取消。
- 不在正常完成或错误结束后自动复位 `FireEnable`、`ReturnEnable`、点火模式或通道状态。
- 不由 PC 根据字段名、状态位或遥测值推导“物理点火成功/失败”。
- 不在默认自动化中执行真实 DH 硬件写入。

## 参数与默认值

复用 `dh_control_request` 的帧布局与 `06/02` 命令号。B23-B24 从 `delay_us` 更名为
`delay_frames`，原“延迟首帧采样”的时间语义删除。仓库内 DUT、根宿主和旧 PyQt 客户端
同步升级；旧版本客户端不再具备语义兼容性。

Web 每次运行前可编辑：

- `power_enable`、`return_enable`；
- 23 路点火通道选择，编码到 `channel[0].bit0..22`；
- `report_count`、`interval_us`、`delay_frames`。

`channel[1]` 固定编码为 0，不投影到界面。安全默认值为 power/return 关闭、通道全不选、
`report_count=50`、`interval_us=2500`、`delay_frames=5`。

PC 只校验类型及协议编码范围，不解释 DUT 业务范围。DUT 在任何硬件写入前一次性校验：

- `report_count` 为 `1..65535`；
- `interval_us` 为 `2500..65535`；
- `delay_frames` 为 U16 且小于 `report_count`；
- `channel[0]` 至少选择一路且不得使用 bit23..31；
- `channel[1] == 0`；
- power/return 均为 0 或 1。

power/return 为 0 是合法测试输入。选中通道时 DUT 仍执行通道和触发命令，用于观察硬件门控
是否阻止物理点火。

## DUT 时序

```text
解析并完整校验请求
  -> 按 interval_us 采集并发送 delay_frames 个只读基线帧
  -> 一次执行 FireEnable / ReturnEnable / RepeatMode / FireMode / 通道批次 / MultiTrigger
  -> 继续按最小采样起点间隔回告
  -> 总计 report_count 帧后自然结束
```

`delay_frames=N` 表示前 N 帧为点火前基线，点火在下一帧采样前执行；N=0 时在第一帧前
点火。不存在固定的第 5/6 帧逻辑。

`interval_us` 是相邻采样起点的最小间隔。每个下一期限从本帧实际采样起点计算；硬件读取、
锁竞争或发送较慢时允许顺延，不追赶、不丢弃预定帧，也不承诺硬实时等间隔。

基线阶段只读，不改变任何 DH 寄存器。基线采集或发送任一失败时立即结束且不得点火。点火
之后，单帧采集失败以错误响应占用该帧位置并继续剩余回告；发送链路失效等无法继续回告的
错误才提前终止。正常、错误和发送失败路径都保持既有“不恢复 DH 状态”行为。

## 宿主执行与结果

新增专用 `DhIgniteStreamAlgorithmExecutor`，一轮只发送一个 `dh_control_request`，持续读取
`dh_control_response`，校验响应命令及 `response_seq = request_seq + frame_index`（U16
回绕），并逐帧发布全部回读值。

PC 以本轮第一个有效回告建立 UTC 锚点，并生成理想时间轴：

```text
streamElapsedUs = 已接收帧索引 * interval_us
timestampUs = UTC 锚点 + streamElapsedUs
```

该轴不使用逐帧到达时间，也不证明 DUT 实际采样严格等间隔。样本保留 power/return 回读、
23 路状态、23 路遥测、响应序号和基线/点火后阶段标记。

流程判定只要求收到 `report_count` 个响应且所有响应 `status/err_code` 正常。点火后的错误帧
仍保存并使最终结果失败；不增加物理点火效果阈值或业务判据。

## 不可停止 capability

DH 点火流是不可取消的有限 `device_stream`：

- 运行中 Web 隐藏暂停/恢复/停止并禁用配置切换；
- 后端对运行中的 stop/quit 返回不支持，不改变测试；
- 显式 `disconnect`、浏览器意外断开和该连接背压关闭都只分离客户端，后端继续本轮直到 DUT 自然完成并允许新客户端重连；
- 不向 DUT发送 STOP，也不把断开解释为取消；
- 强制终止进程或断电属于协议外行为，不能由软件保证收尾。

该能力是应用生命周期的显式例外，不是新的 BIZ 运行模式。BIZ 仍执行一次 `executeStep()`，
算法自然返回后完成任务。

## 数据保存与 Web

只有 Web 启动时显式 `saveData=true` 才写 TXT；否则仅实时投影。保存内容包含完整 descriptor
测量列、正常/错误帧、估算时间轴、生效请求参数和基线/点火后分界。图表选择不得减少保存列。

参数通过算法运行参数 Schema 投影；浏览器可做 U16/布尔/通道选择的编码级校验，但不得复制
DUT 的 `delay_frames < report_count`、通道非空或 `interval_us >= 2500` 等业务判定。

## 验证边界

- DUT 服务测试先锁定参数错误零写入、动态 `delay_frames`、点火前只读、一次触发、采样起点
  最小间隔、点火前失败不触发、点火后错误继续、发送失败终止和不复位状态。
- 产品协议生成器、DUT/宿主/旧 PyQt codec golden 锁定 B23-B24 的 `delay_frames` 字段与
  现有命令号、帧长和 CRC。
- 宿主算法测试覆盖 U16 回绕、可变帧数/间隔、估算时间轴、完整 23 路数据、短帧、错误帧、
  自然结束和无 STOP 写入。
- 应用/Web/浏览器测试覆盖只允许 `device_stream`、参数表单与安全默认值、DUT 业务错误透传、
  不可停止 capability、客户端掉线后继续、显式保存及完整 TXT。
- Fake、脚本化传输、主机侧 PyQt 和 AArch64 交叉构建均不能作为真实点火证据。真实 DH 验收
  必须在隔离台架和明确授权下另行记录接线、电平、通道、输入参数、50/动态帧数据、触发时刻、
  保持状态和人工恢复结果。
