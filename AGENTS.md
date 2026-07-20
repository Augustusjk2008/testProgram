# 仓库规范

最终答复使用中文。

## 适用范围与事实源

- 本文件适用于整个仓库；`src/hal/AGENTS.md` 对 `src/hal/` 子树具有更高优先级。
- 架构总览：`docs/design/overview/five-layer-architecture.md`。
- BIZ 契约：`docs/design/contracts/business-scheduling-layer.md`。
- HAL 契约：`docs/design/contracts/hal-interface-protocol.md`。
- 设备通讯契约：`docs/design/contracts/device-communication-protocol.md`。
- 日志契约：`docs/design/contracts/log-interface-protocol.md`。
- 测试规范：`docs/design/testing/testing-specification.md`。
- `[当前实现]` 只陈述公共 API、CMake 目标、测试注册和已核对源码事实；`[目标契约-未实现]` 是已批准的边界，不得写成已落地行为。
- 公共 API、CMake 目标和测试注册是“当前已实现行为”的代码事实。代码与文档冲突时，先判断代码是否违反分层、安全或公共契约；代码合理则同步文档，代码明显有缺陷则修代码并补回归测试。
- `docs/plan/`、`docs/superpowers/plans/` 是历史执行记录，不是现行接口事实源。
- 审查、搜索和统计默认忽略 `tmp/`、`build*/`、`cmake-build*/`、`out/` 和 `.git/`；不得把附件、生成物或旧构建结果当作仓库实现事实。

## 当前实现

- 根构建入口：`CMakeLists.txt`，先查找 Qt 5.15 Core/Network/SerialPort，失败后使用同一 Qt 6 组件集。
- HAL：`src/hal/`，产物 `hwtest_hal`；公共头位于 `src/hal/include/hal/`，内部实现位于 `src/hal/src/`。
- 日志类型：`src/logging/` 中的 `hwtest_log_types`，只包含 `LogEvent` 等值类型和元类型函数，仅依赖 Qt Core，不得依赖 HAL。
- 日志服务：`src/logging/` 中的 `hwtest_log`，包含日志服务、文件 sink 和 HAL 日志桥接，可以依赖 `hwtest_hal`。
- 业务调度：`src/biz/`，产物 `hwtest_biz`；公共头位于 `src/biz/include/biz/`，实现位于 `src/biz/src/`。
- 算法：`src/algorithm/` 已提供 `hwtest_algorithm_mbddf`、MB_DDF CSV 协议编解码和 `SYSTEM_STATUS` 执行器，命名空间为 `hwtest::algorithm::mbddf`。
- 应用：`src/app/` 提供共享 `hwtest_app_core`、一次性 JSON 入口 `hwtest_pc_runner` 和分步操作入口 `hwtest_tui`；两种入口均通过 `TestApplicationController` 从 BIZ 测试配置和 HAL 部署配置组装当前唯一的 `mbddf.system_status` 测试。
- 测试：`tests/hal/`、`tests/log/`、`tests/biz/`、`tests/algorithm/`、`tests/app/` 使用 GoogleTest 并通过 CTest 注册；当前清单和统计只以 `docs/design/testing/testing-specification.md` 为准。
- 当前已有行式 TUI，但没有 Qt GUI、Web UI、TCP Provider、真实厂家链或真实硬件验收。HAL 控制通道已实现 `qt.serial` 和 `qt.udp`，其中 UDP 已有经应用控制器/BIZ/算法/HAL 的本机模拟目标闭环；真实串口尚未联调。`SystemStatusSimulator` 仍只作为纯协议替身。
- `H:/Resources/RTLinux/Demos/MB_DDF_v2/docs/design/product_protocol_csv` 的当前内容是已批准的 MB_DDF 协议 CSV 基线，当前清单为 32 个 CSV；代码、测试和文档应以该目录现状为准。该目录不在仓库内，交付时仍须记录观测时间和清单；manifest、内容哈希和不可变快照尚未实现。

## 分层与 I/O 边界

```text
TUI / batch CLI（当前实现）；Qt GUI / Web UI（未实现）
  -> hwtest_app_core::TestApplicationController
  -> hwtest_biz
  -> biz::IAlgorithmExecutor（算法层实现）
  -> hwtest_hal
  -> Provider / Adapter
```

- BIZ 负责配置、计划、稳定拓扑排序、运行状态、重试、结果编排和报告，保持硬件无关。它只能直接依赖 Qt Core、`hwtest_log_types` 和自身公共模型；禁止 include、link、call 或持有 HAL、Adapter、Socket、codec、测量基类/工厂或安全输出执行对象。
- TUI、未来 Qt GUI 和 Web UI 只能消费应用层 DTO、动作结果和快照事件；HAL 会话、算法执行器、BIZ 服务及其收尾顺序统一由 `hwtest_app_core` 组合，不得在各前端复制。控制器动作和快照只能在其 QObject 亲和线程调用，其他线程必须排队投递。
- `[目标契约-未实现]` 算法层负责产品协议 CSV、编解码、序列/流程和判定，并通过 HAL 请求设备生命周期；它不持有具体连接对象，也不直接执行生产原始 I/O、deadline 或物理 safe state。
- `[目标契约-未实现]` 面向测试设备或 DUT 的所有生产态硬件和通讯 I/O 必须统一经 HAL。HAL 持有具体连接对象，执行原始 I/O 和 deadline，归一化错误，并执行物理 safe state。
- `[当前实现]` `module = "control"` 的资源按显式 `providerId` 路由 `qt.serial` 或 `qt.udp`，直接使用 Qt 标准 API 并绕过 Vendor Adapter；其他资源仍走现有 `CAbiAdapter -> MockAdapter`。通用 Provider Router、TCP、控制通道 Mock Provider 和 Vendor Provider 尚未实现。
- 配置、日志和报告文件 I/O 不属于上述生产硬件/通讯 I/O 规则。
- 纯协议或 golden 单测可以直接使用 Simulator；产品模拟和集成测试必须经过 HAL，可使用 HAL Mock，或使用标准 Provider 连接隔离模拟目标。不得以直连 Simulator 作为 HAL 集成证据。
- `[当前实现]` 现有“配置 -> BIZ -> 算法 -> `SystemStatusSimulator`”用例是遗留的非 HAL 跨层替身回归，可以保留为 golden-frame 证据，但不是产品模拟或集成验收范式。
- 日志是旁路基础模块；BIZ 使用 `hwtest::logging::LogEvent`，不得为了日志而传递依赖完整 `hwtest_log`。事件来源和 HAL/Adapter 映射仅由 `docs/design/contracts/log-interface-protocol.md` 主定义。

## 配置与兼容

- 新写出的 BIZ 测试配置仅使用 `executionConfig` 作为交给算法层的不透明执行配置；旧根字段 `halConfig` 仅允许在读取迁移时出现。该规则不改变当前 HAL 公共函数既有的参数/API 兼容面。
- BIZ 可以保存和透传 `ProtocolProfile`、`SafetyPolicy` 与硬件需求，但不得解释协议字段或执行硬件安全动作。
- 公共 HAL 和 BIZ 头文件均视为兼容面；结构体优先尾部扩展，避免改变既有枚举数值和语义。
- Adapter ABI 变化时必须同步 `src/hal/include/hal/hal_adapter_abi.h`、HAL 契约和 ABI 测试。

## 代码与文档约束

- 使用 C++17 和 Qt 5.15 兼容 API；Qt 6 fallback 必须保持可编译。
- 命名空间分别使用 `hwtest::hal`、`hwtest::logging`、`hwtest::biz`、`hwtest::algorithm::mbddf`、`hwtest::app`。
- 协议解释、产品判定留在算法层；具体连接、原始 I/O、deadline 和物理安全态按目标边界留在 HAL。不要把它们重新塞回 BIZ。
- 修改公共 API、配置字段、状态语义、错误码、构建依赖或测试边界时，同步相应事实源文档和契约测试。
- 跨文档概念只保留一个主定义：总览写职责和依赖，契约写接口和语义，实现报告写当前落地细节，测试规范写验证边界；其他文档使用链接引用，不复制大段定义。
- 历史计划若与当前实现不一致，应保留历史内容并在顶部标注“已被替代”及现行事实源，不把旧计划改写成当前设计。

## 构建与验证

```powershell
cmake -S . -B build_vs -G "Visual Studio 17 2022" -A x64
cmake --build build_vs --config Debug --parallel
ctest --test-dir build_vs -C Debug --output-on-failure
cmake --build build_vs --config Release --parallel
ctest --test-dir build_vs -C Release --output-on-failure
```

- `hwtest_hal` 必须保持可由宿主/根工程作为 Qt Core/Network/SerialPort 库目标构建，并保留无真实硬件的 Mock 路径。当前 `src/hal/` 没有独立 CMake 自举入口，不得宣称支持直接执行 `cmake -S src/hal`。
- 修改 BIZ 时必须运行 BIZ 契约/架构测试，确认 `src/biz/` 和 `tests/biz/` 没有越层依赖。
- 修改算法层时运行 `tests/algorithm/`；产品模拟或集成验证必须经过 HAL，而不是直连 Simulator。
- 修复行为缺陷时先补能够复现问题的回归测试；仅修改说明性文档时至少运行链接/术语检查和 `git diff --check`。
