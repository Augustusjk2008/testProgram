# 设计文档目录

`[当前实现]` 表示已由源码、CMake 或测试注册核对的事实；`[目标契约-未实现]` 表示已批准但尚未落地的边界。不要把后者作为实现或验收已通过的证据。

## 目录分组

```text
docs/design/
  overview/        总览、分层边界、跨层依赖
  contracts/       对外接口、协议、契约
  implementation/  当前实现设计和落地说明
  testing/         测试规范、测试设计报告
  history/         被现行事实源替代的历史快照
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
| 历史入口 | `implementation/hal-implementation-design-report.md` | 指向 `history/` 中的 2026-07-25 HAL 快照 |
| 实现设计 | `implementation/logging-implementation-design-report.md` | 当前 `src/logging/` 的缓存、sink 和桥接落地 |
| 测试 | `testing/testing-specification.md` | 分层测试规范和运行方式 |
| 历史入口 | `testing/hal-test-design-report.md` | 指向 `history/` 中的 HAL 测试快照；当前清单以测试规范为准 |

## 当前实现入口

| 范围 | 入口 | 说明 |
| --- | --- | --- |
| BIZ | `src/biz/` | `hwtest_biz`；公共头仅直接依赖 Qt Core 和 `hwtest_log_types` |
| 算法 | `src/algorithm/` | `hwtest_algorithm_mbddf`，包含 MB_DDF 协议 CSV、编解码、固定命令执行器、配置驱动单步交换、惯测/舵机设备流执行器、运行参数 Schema 和 DI 刺激控制器 |
| HAL | `src/hal/` | `hwtest_hal`；控制资源走 `qt.serial`/`qt.udp`，其他设备按 `adapterId` 惰性路由到 Mock 或 C ABI v1 后端；Vendor C ABI 初始化接收驱动级 Adapter 配置，打开代码使用版本化单设备投影；另有默认关闭的 PXI-6259 NI-DAQmx Adapter 与可选采样任务 ABI |
| 日志 | `src/logging/` | `hwtest_log_types` 与 `hwtest_log` |
| 应用 | `src/app/`、`front/` | `hwtest_app_core` 统一组合生命周期；`hwtest_pc_runner`、`hwtest_tui`、`hwtest_gui` 与回环 `hwtest_web` 是独立 C++ 入口；`front/` 是独立 React/Vite 遥测控制台，并消费后处理 capability、摘要和按通道结果接口 |
| 测试 | `tests/hal/`、`tests/log/`、`tests/biz/`、`tests/algorithm/`、`tests/app/` | 七个 GoogleTest 目标，经 CTest 注册 |

`[当前实现]` 仓库已有行式 TUI、Qt Widgets GUI、WebSocket 后端和浏览器遥测控制台，当前开发验证以 Web 链路为主基线。浏览器通过现有配置选择一个独立测试；应用层把配置展示元数据和算法层运行参数 Schema 投影为 WebSocket descriptor，前端据此显示测试名称、支持的运行模式、首页主指标、测量标签/单位、可编辑运行参数及 16 路 DI 刺激/回读，并自动发现样本新增字段。当前配置目录包含九项：`SYSTEM_STATUS`、`ELEC_HEALTH_STATUS`、`MEMPERF_TEST`、`SPI_FLASH_TEST`、`DH_PULSE_CONFIG`、带 STOP 清理的 `TIMER_JITTER`、`DI_READ`、只支持设备持续模式的 `IMU_STREAM` 和 `HELM_STREAM`。惯测由 PC 经 COM3 发送一次 START，DUT 持续读取 COM4 并主动回告；舵机实测由 DUT 以 1 ms 周期生成指令，经 DDS 与用户独立启停的 `MB_DDF_v2_HelmControl` 交互并批量回告。两类设备流都由 PC 发送 STOP 后结束，配置 descriptor 定义的全部测量列可由应用层按固定表头保存。现有自动化尚不构成 COM4、DDS 舵机或目标板实机验收。其他硬件证据和限制统一见 `testing/testing-specification.md`。

### 舵机后处理状态

`[当前实现]` 算法层已有独立的后处理端口、五种舵机波形分析、连续扫频伯德估计和版本化结果序列化；应用 DTO/WebSocket v1 已追加 `TestDescriptor.postRunAnalysis`、`ApplicationSnapshot.analysis` 和只读 `analysisResult`，浏览器已有性能页、四通道身份缓存和 uPlot 显示。`mbddf.helm_stream` 当前声明 analyzer `mbddf.helm.performance`、schema `1`。这些接口把结果明确隔离在 BIZ `TestResult`、采集 verdict 和连续 TXT 保存之外。

`[当前实现]` 应用组合层在 BIZ start 前预检分析目录并创建随机 pending 捕获，只有任务启动成功才递增 `analysisGeneration` 并绑定 `{taskId, analysisGeneration}`。控制器按“连续 TXT → 分析捕获 → 公开样本”的顺序追加，STOP 时封存尾样本并进入 `queued`，待 `stopCompleted` 硬件收尾栅栏满足后才启动一次性工作线程；自然完成则只等待封存。协调器执行资源限制、进度投影、原始 TXT SHA-256、结果原子提交、失败诊断保留和协作取消/join；从 `queued` 到终态的新会话写操作受门禁，只读查询保持可用。终态摘要和按通道曲线均校验当前身份，旧线程迟到回告不能覆盖新任务。

无论算法、WebSocket 或前端测试是否通过，五种波形、正反扫频、提前 STOP、四通道差异及伯德指标仍须在隔离、明确授权的真机台架结合原始 TXT、性能 JSON、DDS 时间轴、前端截图和独立参考测量验收；自动化不能替代该证据。

外部目录 `H:/Resources/RTLinux/Demos/MB_DDF_v2/docs/design/product_protocol_csv` 是原始来源追溯；导入基线为来源提交 `982b3f5bbce222aea061e9ce1523ba926c801658` 的 32 份 CSV，2026-07-27 又同步加入 5 份惯测流 CSV。当前仓库内 `dut/docs/design/product_protocol_csv/` 保留 37 份定义，并已更新舵机 START/反馈布局；宿主脚本默认使用这份可复现快照，显式设置 `MB_DDF_PROTOCOL_CSV_DIR` 时才改用其他受控目录。尚无 manifest、内容哈希和不可变快照自动机制。

## 阅读顺序

- 生产硬件/通讯 I/O 的边界、Simulator 与 HAL Mock 的测试边界见 `overview/five-layer-architecture.md`。
- BIZ 的 API、配置迁移和 `executionConfig` 透传见 `contracts/business-scheduling-layer.md`。
- `LogEvent` 字段、来源和 HAL/Adapter 映射只看 `contracts/log-interface-protocol.md`。
- 协议 CSV 字段与物理帧规则只看 `contracts/device-communication-protocol.md`。
- WebSocket 请求、配置白名单选择、快照、错误与关闭顺序只看 `contracts/websocket-frontend-protocol.md`。
