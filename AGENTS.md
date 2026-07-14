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
- 公共 API、CMake 目标和测试注册是“当前已实现行为”的代码事实；文档可以描述未来设计，但必须明确标注“未实现”或“扩展点”。代码与文档冲突时，优先判断代码是否违反分层、安全或公共契约；代码合理则同步文档，代码明显有缺陷则修代码并补回归测试。
- `docs/plan/`、`docs/superpowers/plans/` 是历史执行记录，不是现行接口事实源。
- 审查、搜索和统计默认忽略 `tmp/`、`build*/`、`cmake-build*/`、`out/` 和 `.git/`；不得把附件、生成物或旧构建结果当作仓库实现事实。

## 当前结构

- 根构建入口：`CMakeLists.txt`，先查找 Qt 5.15 Core，失败后使用 Qt 6 Core。
- HAL：`src/hal/`，产物 `hwtest_hal`；公共头位于 `src/hal/include/hal/`，内部实现位于 `src/hal/src/`。
- 日志类型：`src/logging/` 中的 `hwtest_log_types`，只包含 `LogEvent` 等值类型和元类型函数，仅依赖 Qt Core，不得依赖 HAL。
- 日志服务：`src/logging/` 中的 `hwtest_log`，包含日志服务、文件 sink 和 HAL 日志桥接，可以依赖 `hwtest_hal`。
- 业务调度：`src/biz/`，产物 `hwtest_biz`；公共头位于 `src/biz/include/biz/`，实现位于 `src/biz/src/`。
- 测试：`tests/hal/`、`tests/log/`、`tests/biz/`，使用 GoogleTest 并通过 CTest 注册；由 `BUILD_TESTING` 控制。
- 当前仓库不包含 UI，也没有具体算法层实现；`IAlgorithmExecutor` 是供算法层实现的边界端口。

## 分层约束

```text
UI -> hwtest_biz -> biz::IAlgorithmExecutor（算法层实现）
   -> hwtest_hal -> Adapter -> 厂家库/驱动
```

- BIZ 负责配置、计划、稳定拓扑排序、运行状态、重试、结果编排和报告。
- BIZ 只能直接依赖 Qt Core、`hwtest_log_types` 和自身公共模型；禁止 include、link、call 或持有 HAL、Adapter、Socket、codec、测量基类/工厂或安全输出执行对象。
- 算法层负责单步算法、协议编解码、通讯连接、HAL 生命周期和硬件安全收尾；通过 `IAlgorithmExecutor` 向 BIZ 提供执行能力。
- HAL 负责设备发现、逻辑资源、参数/安全校验、统一错误和 Adapter 调用；不得包含业务流程、产品协议字段解释或测试判定。
- Adapter 只封装厂家 DLL/lib/SDK/驱动，不包含 UI、业务流程或算法判定。
- 日志是旁路基础模块；BIZ 使用 `hwtest::logging::LogEvent`，不得为了日志而传递依赖完整 `hwtest_log`。

## 配置与兼容

- 新配置使用 `executionConfig` 作为交给算法层的不透明执行配置；`halConfig` 仅允许在旧配置读取迁移时出现，新写出不得继续使用。
- BIZ 可以保存和透传 `ProtocolProfile`、`SafetyPolicy` 与硬件需求，但不得解释协议字段或执行硬件安全动作。
- 公共 HAL 和 BIZ 头文件均视为兼容面；结构体优先尾部扩展，避免改变既有枚举数值和语义。
- Adapter ABI 变化时必须同步 `src/hal/include/hal/hal_adapter_abi.h`、HAL 契约和 ABI 测试。

## 代码与文档约束

- 使用 C++17 和 Qt 5.15 兼容 API；Qt 6 fallback 必须保持可编译。
- 命名空间分别使用 `hwtest::hal`、`hwtest::logging`、`hwtest::biz`。
- 优先沿用现有模块边界和工厂接口，不把算法或硬件执行逻辑重新塞回 BIZ。
- 修改公共 API、配置字段、状态语义、错误码、构建依赖或测试边界时，同步相应事实源文档和契约测试。
- 跨文档概念只保留一个主定义：总览写职责和依赖，契约写接口和语义，实现报告写落地细节，测试规范写验证边界；其他文档使用链接引用，不复制大段定义。
- 历史计划若与当前实现不一致，应保留历史内容并在顶部标注“已被替代”及现行事实源，不把旧计划改写成当前设计。

## 构建与验证

```powershell
cmake -S . -B build_vs -G "Visual Studio 17 2022" -A x64
cmake --build build_vs --config Debug --parallel
ctest --test-dir build_vs -C Debug --output-on-failure
cmake --build build_vs --config Release --parallel
ctest --test-dir build_vs -C Release --output-on-failure
```

- `src/hal` 必须继续支持独立构建和无真实硬件的 Mock 路径。
- 修改 BIZ 时必须运行 BIZ 契约/架构测试，确认 `src/biz/` 和 `tests/biz/` 没有越层依赖。
- 修复行为缺陷时先补能够复现问题的回归测试；仅修改说明性文档时至少运行链接/术语检查和 `git diff --check`。
