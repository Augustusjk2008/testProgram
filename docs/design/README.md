# 设计文档目录

本文只提供现行事实源索引。协议字段、运行模式、页面能力、硬件点位和测试计数不得在索引中重复定义。

## 状态与事实源

- `[当前实现]`：已由公共 API、CMake、配置或已核对源码确认的行为。
- `[当前实现限制]`：现有代码的已知边界；不表示已经修复或形成安全保证。
- `[目标契约-未实现]`：已批准但尚未落地，不得作为实现或验收证据。
- `dut/` 是唯一可信的 MB_DDF_v2 产品端软件；唯一批准的产品协议快照是 [`dut/docs/design/product_protocol_csv/`](../../dut/docs/design/product_protocol_csv/)。仓库外目录、同名副本和历史导入来源不参与当前事实或验证。

## 目录分组

```text
docs/design/
  overview/        分层、依赖与生产 I/O 边界
  contracts/       当前接口、协议和行为语义
  implementation/ 仍需保留的实现说明或历史入口
  testing/         当前测试清单、命令和证据边界
  history/         已被现行事实源替代的历史快照
```

## 唯一主定义

| 范围 | 主定义 |
| --- | --- |
| 分层、依赖与组合根 | [五层架构](overview/five-layer-architecture.md) |
| BIZ API、运行模式和算法端口 | [业务调度层契约](contracts/business-scheduling-layer.md) |
| HAL、Provider、Adapter、deadline 和安全态 | [HAL 接口协议](contracts/hal-interface-protocol.md) |
| 产品协议、CSV、算法配置和产品流程 | [设备通讯协议契约](contracts/device-communication-protocol.md) |
| WebSocket 请求、快照、错误、顺序和关闭 | [WebSocket 前端协议契约](contracts/websocket-frontend-protocol.md) |
| 日志字段、来源和映射 | [日志接口协议](contracts/log-interface-protocol.md) |
| 测试清单、运行方式和证据等级 | [测试规范](testing/testing-specification.md) |
| DUT 内部设计与当前限制 | [`dut/docs/design/`](../../dut/docs/design/) 与已核对的 [`dut/src/`](../../dut/src/) |

## 当前实现入口

| 范围 | 代码入口 | 说明 |
| --- | --- | --- |
| BIZ | `src/biz/` | `hwtest_biz`；只负责调度、状态、结果和报告 |
| 算法 | `src/algorithm/` | `hwtest_algorithm_mbddf`；消费批准协议快照并负责产品流程、判定和运行参数 Schema |
| HAL | `src/hal/`、`src/adapters/` | `hwtest_hal`、Qt Provider、Mock/C ABI/NI Adapter |
| 日志 | `src/logging/` | `hwtest_log_types` 与 `hwtest_log` |
| 应用与入口 | `src/app/`、`front/` | 共享控制器、runner、TUI、GUI、WebSocket 后端和浏览器控制台 |
| DUT | `dut/src/` | AArch64 产品端、硬件层、DDS、产品测试服务和舵控程序 |
| 测试 | `tests/`、`src/adapters/ni_daqmx/tests/`、`front/`、`dut/tests/` | 目标、统计、命令和结果只以测试规范及 DUT 局部规则为准 |

当前应用启动时扫描 `configs/*.testcfg.json`；配置数量和运行模式矩阵只在[测试规范](testing/testing-specification.md)维护。浏览器按 descriptor 动态显示运行参数和 DI 通道；板级点位、安全态和真机门禁只在 HAL 契约定义。

## 历史文档

`history/`、`../plan/` 和 `../superpowers/` 中的文件保留设计或执行过程，只用于追溯当时决策。其路径、计数、能力和测试记录不得覆盖本页列出的现行事实源。
