# 设计文档目录

`[当前实现]` 表示已由源码、CMake 或测试注册核对的事实；`[目标契约-未实现]` 表示已批准但尚未落地的边界。不要把后者作为实现或验收已通过的证据。

## 目录分组

```text
docs/design/
  overview/        总览、分层边界、跨层依赖
  contracts/       对外接口、协议、契约
  implementation/  当前实现设计和落地说明
  testing/         测试规范、测试设计报告
```

## 文档索引

| 分组 | 文档 | 主定义或范围 |
| --- | --- | --- |
| 总览 | `overview/five-layer-architecture.md` | 分层边界、生产 I/O 归属、当前落地范围 |
| 接口契约 | `contracts/business-scheduling-layer.md` | BIZ 的对上服务、数据模型、计划调度与算法端口 |
| 接口契约 | `contracts/hal-interface-protocol.md` | HAL 对上接口、Adapter ABI、资源和错误语义 |
| 接口契约 | `contracts/log-interface-protocol.md` | 日志模型、来源、追踪链和 HAL/Adapter 日志映射的唯一主定义 |
| 接口契约 | `contracts/device-communication-protocol.md` | 测试设备与 DUT 底层通讯协议、CSV 建模和字段布局 |
| 实现设计 | `implementation/hal-implementation-design-report.md` | 当前 HAL 实现、限制和扩展点 |
| 实现设计 | `implementation/logging-implementation-design-report.md` | 当前 `src/logging/` 的缓存、sink 和桥接落地 |
| 测试 | `testing/testing-specification.md` | 分层测试规范和运行方式 |
| 测试 | `testing/hal-test-design-report.md` | 当前 HAL 测试结构、覆盖矩阵和缺口 |

## 当前实现入口

| 范围 | 入口 | 说明 |
| --- | --- | --- |
| BIZ | `src/biz/` | `hwtest_biz`；公共头仅直接依赖 Qt Core 和 `hwtest_log_types` |
| 算法 | `src/algorithm/` | `hwtest_algorithm_mbddf`，包含 MB_DDF 协议 CSV、编解码和 `SYSTEM_STATUS` 执行器 |
| HAL | `src/hal/` | `hwtest_hal`；控制资源可走 `qt.serial`/`qt.udp`，其他资源仍为 `CAbiAdapter -> MockAdapter` 兼容路径 |
| 日志 | `src/logging/` | `hwtest_log_types` 与 `hwtest_log` |
| 应用 | `src/app/` | `hwtest_app_core` 统一组合生命周期；`hwtest_pc_runner` 一次运行，`hwtest_tui` 分步操作当前 `SYSTEM_STATUS` |
| 测试 | `tests/hal/`、`tests/log/`、`tests/biz/`、`tests/algorithm/`、`tests/app/` | 五个 GoogleTest 目标，经 CTest 注册 |

`[当前实现]` 仓库已有行式 TUI，但没有 Qt GUI、Web UI、TCP Provider、真实厂家链或真实硬件验收。Qt SerialPort/Network 已用于控制通道，UDP 已有本机模拟目标闭环，真实串口尚未联调。测试目标、源码清单和统计口径统一见 `testing/testing-specification.md`。

外部目录 `H:/Resources/RTLinux/Demos/MB_DDF_v2/docs/design/product_protocol_csv` 的当前内容已批准为 MB_DDF 协议 CSV 基线，但尚未形成仓库内可复现快照；当前清单和约束统一见 `contracts/device-communication-protocol.md`。

## 阅读顺序

- 生产硬件/通讯 I/O 的边界、Simulator 与 HAL Mock 的测试边界见 `overview/five-layer-architecture.md`。
- BIZ 的 API、配置迁移和 `executionConfig` 透传见 `contracts/business-scheduling-layer.md`。
- `LogEvent` 字段、来源和 HAL/Adapter 映射只看 `contracts/log-interface-protocol.md`。
- 协议 CSV 字段与物理帧规则只看 `contracts/device-communication-protocol.md`。
