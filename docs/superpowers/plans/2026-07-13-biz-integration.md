# Strict Business Layer Integration Implementation Plan

> 历史记录（已完成，非现行事实源）：本计划记录 BIZ 严格分层与集成落地；当前模块边界、接口语义、测试清单和验证准入以 [BIZ 契约](../../design/contracts/business-scheduling-layer.md) 和 [测试规范](../../design/testing/testing-specification.md) 为准。

> **Execution note:** This plan should be executable either inline in the current session or by a delegated worker. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将附件中的业务能力按严格五层边界迁入 `hwtest_biz`，确保业务目标不包含 HAL 类型、头文件、链接或调用。

**Architecture:** `TestRunService` 与 BIZ 流程引擎只依赖业务层拥有的逐步骤 `IAlgorithmExecutor` 端口，算法层实现该端口并独立持有 HAL。业务层保留配置、计划、依赖排序、重试、状态、结果编排和报告；测量、协议编解码、Socket、硬件会话与安全输出执行留在算法/HAL 层。

**Tech Stack:** Qt 5.15 / Qt 6 Core fallback、C++17、CMake、GoogleTest、CTest、MSVC 2022 x64

---

### Task 1: Create The Strict Public Boundary

**Files:**
- Create: `src/biz/CMakeLists.txt`
- Create: `src/biz/include/biz/biz_global.h`
- Create: `src/biz/include/biz/biz_types.h`
- Create: `src/biz/include/biz/i_algorithm_executor.h`
- Create: `src/biz/include/biz/i_test_run_service.h`
- Create: `src/biz/include/biz/i_report_generator.h`
- Create: `src/biz/include/biz/biz_factory.h`

- [x] Define `IAlgorithmExecutor` with `prepare/executeStep/requestStop/reset/shutdown`; define `IRunControl` and `IAlgorithmObserver` so BIZ owns scheduling while algorithms own one-step execution.
- [x] Define `ITestRunService` with the documented UI-facing lifecycle methods and `hwtest::logging::LogEvent` signal.
- [x] Keep `TestContext` business-only: task/request/product/operator/station/tags; no HAL or socket pointers.
- [x] Link `hwtest_biz` only to Qt Core and the HAL-free `hwtest_log_types` target; enable `HWTEST_BIZ_STATIC` and `/utf-8` on MSVC.

### Task 2: Write And Observe Strict-Layer RED Tests

**Files:**
- Create: `tests/biz/CMakeLists.txt`
- Create: `tests/biz/test_support.h`
- Create: `tests/biz/strict_layering_test.cpp`
- Create: `tests/biz/test_config_manager_test.cpp`
- Create: `tests/biz/test_plan_builder_test.cpp`
- Create: `tests/biz/test_run_service_test.cpp`
- Create: `tests/biz/report_generator_test.cpp`

- [x] Add an architecture test that scans `src/biz` and fails on forbidden cross-layer dependencies.
- [x] Add configuration round-trip tests for current `TestConfig`, `SafetyPolicy`, `RuntimeConfig`, `ProtocolProfile`, and `reportFields`.
- [x] Add service/flow tests with a fake `IAlgorithmExecutor`, including ordered step execution, dependency skip, retry, request ID continuity, pause/resume, stop failure propagation, reset, report, and observer forwarding.
- [x] Run the focused RED tests before implementing behavior.

### Task 3: Implement Business Services To GREEN

**Files:**
- Create: `src/biz/include/biz/test_config_manager.h`
- Create: `src/biz/include/biz/test_plan_builder.h`
- Create: `src/biz/src/biz_types.cpp`
- Create: `src/biz/src/test_config_manager.cpp`
- Create: `src/biz/src/test_plan_builder.cpp`
- Create: `src/biz/src/test_run_service.cpp`
- Create: `src/biz/src/report_generator.cpp`

- [x] Parse, validate, save, and round-trip every documented configuration field without silent loss.
- [x] Build enabled plans, apply runtime defaults, reject missing dependencies and dependency cycles.
- [x] Implement `TestRunService` as a stateful owner of BIZ scheduling over `IAlgorithmExecutor`; generate task/request IDs before `prepare`, iterate the plan in BIZ, and never access HAL.
- [x] Propagate algorithm `requestStop/shutdown` failures without publishing false `Idle` state.
- [x] Generate HTML, CSV, TXT, or XML reports from business `TestResult` snapshots without hardware access.
- [x] Run focused BIZ tests until GREEN.

### Task 4: Merge Into The Root Build

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `src/logging/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [x] Add `src/biz` after `src/logging`; do not change `src/hal/**` or logging implementation behavior.
- [x] Add `hwtest_log_types` as a small HAL-free static target containing the logging value types and metatype functions; keep `hwtest_log` compatible.
- [x] Add `tests/biz` and reuse the repository's existing GoogleTest target; do not import attachment `3rdparty/` or binaries.
- [x] Configure with Qt5 first and preserve the existing Qt6 Core fallback.

### Task 5: Correct The Facts And Verify Both Configurations

**Files:**
- Modify: `docs/design/overview/five-layer-architecture.md`
- Modify: `docs/design/contracts/business-scheduling-layer.md`
- Modify: `docs/design/testing/testing-specification.md`
- Modify: `docs/design/README.md`

- [x] Remove every statement that permits BIZ to own `IHalService`, `IHalDevice`, HAL lifecycle, socket transport, measurement implementation, or safety output execution.
- [x] Document `IAlgorithmExecutor` as the sole business-to-algorithm port and place HAL lifecycle inside its algorithm-layer implementation.
- [x] Document the landed `hwtest_biz` target and strict-layer architecture test.
- [x] Run:

```powershell
cmake -S . -B build_vs -G "Visual Studio 17 2022" -A x64
cmake --build build_vs --config Debug
ctest --test-dir build_vs -C Debug --output-on-failure
cmake --build build_vs --config Release
ctest --test-dir build_vs -C Release --output-on-failure
```

Expected: both configurations build successfully; all HAL, logging, BIZ, and architecture tests pass.
