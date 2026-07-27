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
| 接口契约 | `contracts/websocket-frontend-protocol.md` | `hwtest_web` 的回环 WebSocket JSON、动作、错误、顺序和关闭语义 |
| 实现设计 | `implementation/hal-implementation-design-report.md` | 2026-07-25 HAL 历史快照；已被现行 HAL 契约与总览替代 |
| 实现设计 | `implementation/logging-implementation-design-report.md` | 当前 `src/logging/` 的缓存、sink 和桥接落地 |
| 测试 | `testing/testing-specification.md` | 分层测试规范和运行方式 |
| 测试 | `testing/hal-test-design-report.md` | HAL 历史测试快照；当前清单以测试规范为准 |

## 当前实现入口

| 范围 | 入口 | 说明 |
| --- | --- | --- |
| BIZ | `src/biz/` | `hwtest_biz`；公共头仅直接依赖 Qt Core 和 `hwtest_log_types` |
| 算法 | `src/algorithm/` | `hwtest_algorithm_mbddf`，包含 MB_DDF 协议 CSV、编解码、两个固定命令执行器、配置驱动单步交换和 DI 刺激控制器 |
| HAL | `src/hal/` | `hwtest_hal`；控制资源走 `qt.serial`/`qt.udp`，其他设备按 `adapterId` 惰性路由到 Mock 或 C ABI v1 后端；Vendor C ABI 初始化接收驱动级 Adapter 配置，打开代码使用版本化单设备投影；另有默认关闭的 PXI-6259 NI-DAQmx Adapter 与可选采样任务 ABI |
| 日志 | `src/logging/` | `hwtest_log_types` 与 `hwtest_log` |
| 应用 | `src/app/`、`front/` | `hwtest_app_core` 统一组合生命周期；`hwtest_pc_runner`、`hwtest_tui`、`hwtest_gui` 与回环 `hwtest_web` 是独立 C++ 入口；`front/` 是独立 React/Vite 遥测控制台 |
| 测试 | `tests/hal/`、`tests/log/`、`tests/biz/`、`tests/algorithm/`、`tests/app/` | 七个 GoogleTest 目标，经 CTest 注册 |

`[当前实现]` 仓库已有行式 TUI、Qt Widgets GUI、WebSocket 后端和浏览器遥测控制台。浏览器通过现有配置选择一个独立单步测试；应用层把配置中的展示元数据投影为 WebSocket descriptor，前端据此显示测试名称、支持的运行模式、首页主指标、测量标签/单位及 16 路 DI 刺激/回读，并自动发现样本新增字段。当前配置目录包含七项：`SYSTEM_STATUS`、`ELEC_HEALTH_STATUS`、`MEMPERF_TEST`、`SPI_FLASH_TEST`、`DH_PULSE_CONFIG`、带 STOP 清理的 `TIMER_JITTER` 和 `DI_READ`。电气健康仅判定设备 `status`/`err_code`，不解释电压阈值；SPI Flash 配置明确写入固定隔离测试区且不恢复。BIZ 任务 worker 已迁移为带 Qt dispatcher 的 `QThread` 并有自动化回归。通用 C ABI、多 Adapter、可选任务 ABI 和 PXI-6259 NI-DAQmx 软件路径已实现；`AdapterRouter` 将 Vendor C ABI 初始化输入限定为驱动级配置，设备拓扑与 safe state 通过版本化 open projection 传递，并有动态 fixture 回归。NI 自动化证据仍限仓库 Fake NIDAQmx，尚无真实 NI SDK 或 PXI-6259 板卡验收。TCP Provider 和全面真实硬件验收仍未实现。Qt UDP 已有本机模拟目标闭环；Qt 串口已有既有两个测试项的短时 COM3 smoke，后续五项尚未扩展真实硬件证据。测试目标、源码清单、实机证据和限制统一见 `testing/testing-specification.md`。

外部目录 `H:/Resources/RTLinux/Demos/MB_DDF_v2/docs/design/product_protocol_csv` 的当前内容已批准为 MB_DDF 协议 CSV 基线；`dut/` 已同步来源提交 `982b3f5bbce222aea061e9ce1523ba926c801658` 及其中 32 份 CSV，但尚无 manifest、内容哈希和不可变快照自动机制。宿主运行期仍显式使用外部资产目录，当前清单和约束统一见 `contracts/device-communication-protocol.md`。

## 阅读顺序

- 生产硬件/通讯 I/O 的边界、Simulator 与 HAL Mock 的测试边界见 `overview/five-layer-architecture.md`。
- BIZ 的 API、配置迁移和 `executionConfig` 透传见 `contracts/business-scheduling-layer.md`。
- `LogEvent` 字段、来源和 HAL/Adapter 映射只看 `contracts/log-interface-protocol.md`。
- 协议 CSV 字段与物理帧规则只看 `contracts/device-communication-protocol.md`。
- WebSocket 请求、配置白名单选择、快照、错误与关闭顺序只看 `contracts/websocket-frontend-protocol.md`。
