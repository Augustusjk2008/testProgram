# Continuous Telemetry Web Implementation Plan

> **Execution note:** This plan should be executable either inline in the current session or by a delegated worker. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add host-driven periodic test execution, preserve device-managed streaming semantics, stream typed samples over WebSocket, and build a configurable high-performance telemetry frontend for SYSTEM_STATUS.

**Architecture:** BIZ owns the interruptible PC-periodic loop, while a device-stream run remains one algorithm invocation that may emit many samples. The application controller projects run/session DTOs, the Qt WebSocket adapter validates and transports them, and a React/uPlot frontend batches samples into bounded time-series buffers.

**Tech Stack:** C++17, Qt 5.15 Core/Network/WebSockets, GoogleTest/CTest, React 19, TypeScript, Vite, Tailwind CSS 4, uPlot, Vitest.

---

### Task 1: Add run-mode and sample contracts in BIZ

**Files:**
- Modify: `src/biz/include/biz/biz_types.h`
- Modify: `src/biz/include/biz/i_test_run_service.h`
- Modify: `src/biz/src/biz_types.cpp`
- Test: `tests/biz/test_run_service_test.cpp`
- Test: `tests/biz/algorithm_executor_contract_test.cpp`

- [ ] **Step 1: Write failing contract tests for the new options and events**

```cpp
RunOptions options;
options.mode = RunMode::PcPeriodic;
options.intervalMs = 10;
options.maxCycles = 3;
const auto task = service->startTestWithOptions(options);
ASSERT_TRUE(task.ok());
EXPECT_TRUE(test::waitUntil([&] { return executor.executeCallCount() == 3; }, 1000));
```

- [ ] **Step 2: Run the focused tests and confirm RED**

```powershell
cmake --build build_vs --config Debug --target hwtest_biz_tests --parallel
ctest --test-dir build_vs -C Debug -R "^(AlgorithmExecutorContractTest|TestRunServiceTest)\." --output-on-failure
```

Expected: compilation fails because `RunMode`, `RunOptions`, `startTestWithOptions`, `cycleStarted`, and `sampleProduced` do not exist.

- [ ] **Step 3: Add the public tail extensions**

```cpp
enum class RunMode { Single = 0, PcPeriodic, DeviceStream };

struct HWTEST_BIZ_EXPORT RunOptions {
    RunMode mode = RunMode::Single;
    int intervalMs = 1000;
    quint64 maxCycles = 1;
};

struct RawSample {
    qint64 timestampUs = 0;
    QString channelId;
    QVariantMap values;
    QVariantMap tags;
    quint64 cycleIndex = 1;
};
```

Add `TestResult::cycleIndex`, `runModeToString()`, `runModeFromString()`, the new service entry point, and the two signals without changing existing enum numeric values or `startTest()` behavior.

- [ ] **Step 4: Build and confirm the contract tests now compile**

```powershell
cmake --build build_vs --config Debug --target hwtest_biz_tests --parallel
```

Expected: build succeeds; behavior tests may remain red until Task 2.

### Task 2: Implement the interruptible BIZ periodic loop

**Files:**
- Modify: `src/biz/src/test_run_service.cpp`
- Modify: `tests/biz/test_support.h`
- Test: `tests/biz/test_run_service_test.cpp`

- [ ] **Step 1: Add RED tests for finite cycles, stop during wait, and device-stream single invocation**

```cpp
TEST(TestRunServiceTest, PcPeriodicWaitIsInterruptible)
{
    RunOptions options{RunMode::PcPeriodic, 1000, 0};
    ASSERT_TRUE(service->startTestWithOptions(options).ok());
    ASSERT_TRUE(test::waitUntil([&] { return executor.executeCallCount() == 1; }, 500));
    EXPECT_TRUE(service->stopTest(500).ok());
    QThread::msleep(50);
    EXPECT_EQ(executor.executeCallCount(), 1);
}
```

- [ ] **Step 2: Run and observe the expected behavior failures**

```powershell
ctest --test-dir build_vs -C Debug -R "^TestRunServiceTest\.(PcPeriodic|DeviceStream|SingleRun)" --output-on-failure
```

- [ ] **Step 3: Refactor one plan cycle and add the outer mode loop**

```cpp
enum class CycleOutcome { Completed, Stopped, Error };
CycleOutcome executeCycle(const TestPlan& plan, const TestContext& context, quint64 cycleIndex);
bool waitForRunInterval(int intervalMs) const;
```

Before every cycle clear only the dependency-result map, set `m_currentCycleIndex`, emit `cycleStarted`, and keep the full result/sample vectors for reporting. `PcPeriodic` repeats after the interruptible interval; `Single` and `DeviceStream` execute one cycle.

- [ ] **Step 4: Forward samples with timestamp and cycle metadata**

```cpp
void onSample(const StepId& stepId, const RawSample& incoming) override
{
    RawSample sample = incoming;
    sample.cycleIndex = m_currentCycleIndex;
    if (sample.timestampUs == 0)
        sample.timestampUs = QDateTime::currentMSecsSinceEpoch() * 1000;
    emit sampleProduced(m_taskId, stepId, sample);
}
```

- [ ] **Step 5: Run all BIZ tests GREEN**

```powershell
ctest --test-dir build_vs -C Debug -R "^(AlgorithmExecutorContractTest|BizArchitectureTest|TestRunServiceTest)\." --output-on-failure
```

### Task 3: Project run/session data through the application controller

**Files:**
- Modify: `src/app/include/app/test_application_controller.h`
- Modify: `src/app/src/test_application_controller.cpp`
- Modify: `src/algorithm/src/system_status_executor.cpp`
- Test: `tests/app/test_application_controller_test.cpp`
- Test: `tests/algorithm/system_status_executor_test.cpp`

- [ ] **Step 1: Write failing controller tests for periodic start and sample forwarding**

```cpp
TestRunOptions options;
options.mode = QStringLiteral("pc_periodic");
options.intervalMs = 10;
options.maxCycles = 2;
QSignalSpy samples(&controller, &TestApplicationController::sampleReceived);
ASSERT_TRUE(controller.start(options).ok);
EXPECT_TRUE(waitForSnapshot(controller, [](const auto& s) { return s.cycleIndex == 2; }));
EXPECT_GE(samples.count(), 2);
```

- [ ] **Step 2: Verify RED**

```powershell
cmake --build build_vs --config Debug --target hwtest_app_tests hwtest_algorithm_tests --parallel
```

Expected: missing DTOs, overload, fields, and signal.

- [ ] **Step 3: Add application DTOs and queued signal projection**

```cpp
struct TestRunOptions {
    QString mode = QStringLiteral("single");
    int intervalMs = 1000;
    quint64 maxCycles = 1;
};

struct ApplicationSample {
    QString taskId;
    QString stepId;
    QString channelId;
    qint64 timestampUs = 0;
    quint64 cycleIndex = 1;
    QVariantMap values;
    QVariantMap tags;
};
```

Keep `start()` and implement it as `start(TestRunOptions{})`. Append run fields to `ApplicationSnapshot`, register metatypes, and connect `cycleStarted`/`sampleProduced` using the controller generation guard.

- [ ] **Step 4: Preserve device-stream semantics in the algorithm**

```cpp
if (context.tags.value(QStringLiteral("runMode")) == QStringLiteral("device_stream")) {
    return makeStatus(ErrorCode::CapabilityUnsupported,
                      QStringLiteral("SYSTEM_STATUS does not define a device stream start/stop command"),
                      QStringLiteral("mbddf.prepare"));
}
```

- [ ] **Step 5: Run application and algorithm tests GREEN**

```powershell
ctest --test-dir build_vs -C Debug -R "^(TestApplicationControllerTest|SystemStatusExecutorTest)\." --output-on-failure
```

### Task 4: Extend the WebSocket protocol with periodic start and samples

**Files:**
- Modify: `src/app/web/web_protocol.h`
- Modify: `src/app/web/web_protocol.cpp`
- Modify: `src/app/web/web_socket_frontend_server.cpp`
- Test: `tests/app/web_protocol_test.cpp`
- Test: `tests/app/web_controller_integration_test.cpp`

- [ ] **Step 1: Write RED protocol tests**

```cpp
ApplicationSample sample;
sample.channelId = QStringLiteral("SYSTEM_STATUS");
sample.values.insert(QStringLiteral("cpu_usage"), 12.5);
const QJsonObject json = makeSample(7, sample);
EXPECT_EQ(json.value(QStringLiteral("type")).toString(), QStringLiteral("sample"));
EXPECT_EQ(json.value(QStringLiteral("seq")).toInt(), 7);
```

- [ ] **Step 2: Add an integration RED test that observes multiple UDP requests**

Send `start` with `{mode:"pc_periodic", intervalMs:10, maxCycles:3}`, reply to three UDP requests, require three `sample` messages with cycle indices `1..3`, then require a terminal snapshot.

- [ ] **Step 3: Verify RED**

```powershell
cmake --build build_vs --config Debug --target hwtest_web_tests --parallel
ctest --test-dir build_vs -C Debug -R "^(WebProtocolTest|WebSocketUdpIntegrationTest)\." --output-on-failure
```

- [ ] **Step 4: Implement boundary validation and sample broadcasting**

```cpp
QJsonObject makeSample(quint64 sequence, const ApplicationSample& sample);
```

`start {}` remains valid. Validate `mode` as `single|pc_periodic|device_stream`, integer `intervalMs`, and integer `maxCycles`; call the controller only through queued invocation. Connect `sampleReceived` to a monotonically increasing sample sequence and broadcast only to the active client.

- [ ] **Step 5: Run all WebSocket tests GREEN**

```powershell
ctest --test-dir build_vs -C Debug -L websocket --output-on-failure
```

### Task 5: Scaffold and test the telemetry frontend core

**Files:**
- Create: `front/package.json`
- Create: `front/package-lock.json`
- Create: `front/vite.config.ts`
- Create: `front/tsconfig.json`
- Create: `front/tsconfig.app.json`
- Create: `front/index.html`
- Create: `front/.env.example`
- Create: `front/src/shared/protocol.ts`
- Create: `front/src/shared/config.ts`
- Create: `front/src/shared/ws/HwtestClient.ts`
- Create: `front/src/features/telemetry/sample-buffer.ts`
- Create: `front/src/features/telemetry/series-config.ts`
- Test: `front/src/features/telemetry/sample-buffer.test.ts`
- Test: `front/src/features/telemetry/series-config.test.ts`
- Test: `front/src/shared/ws/HwtestClient.test.ts`

- [ ] **Step 1: Create the package manifest and install verified dependencies**

```json
{
  "scripts": {"dev":"vite --host 127.0.0.1","build":"tsc -b && vite build","test":"vitest run"},
  "dependencies": {"react":"19.2.8","react-dom":"19.2.8","uplot":"1.6.32","@phosphor-icons/react":"2.1.10","@fontsource-variable/geist":"5.3.0"},
  "devDependencies": {"vite":"8.1.5","vitest":"4.1.10","typescript":"7.0.2","tailwindcss":"4.3.3","@tailwindcss/vite":"4.3.3"}
}
```

- [ ] **Step 2: Write RED tests for bounded storage, flattening, and grouping**

```ts
it('keeps only the newest configured sample points', () => {
  const buffer = new SampleBuffer(3)
  ;[1, 2, 3, 4].forEach((value) => buffer.append(sample(value)))
  expect(buffer.values('cpu_usage')).toEqual([2, 3, 4])
})
```

- [ ] **Step 3: Run and observe RED**

```powershell
Set-Location front
npm install
npm test
```

- [ ] **Step 4: Implement typed protocol, ring buffer, field discovery, time windows, min/max downsampling, and grouping**

```ts
export type ChartLayout = 'single' | 'separate' | 'custom'
export interface SeriesAssignment { path: string; enabled: boolean; groupId: string }
export function downsampleMinMax(points: Point[], pixelWidth: number): Point[]
```

- [ ] **Step 5: Run frontend core tests GREEN**

```powershell
npm test
```

### Task 6: Build the dark telemetry control-room UI

**Files:**
- Create: `front/src/main.tsx`
- Create: `front/src/index.css`
- Create: `front/src/app/App.tsx`
- Create: `front/src/app/AppShell.tsx`
- Create: `front/src/features/session/SessionProvider.tsx`
- Create: `front/src/features/session/RunControlBar.tsx`
- Create: `front/src/features/telemetry/TelemetryChart.tsx`
- Create: `front/src/features/telemetry/ChartConfigurator.tsx`
- Create: `front/src/pages/OverviewPage.tsx`
- Create: `front/src/pages/ChartsPage.tsx`
- Create: `front/src/pages/DiagnosticsPage.tsx`
- Modify: `front/README.md`

- [ ] **Step 1: Implement the persistent shell and connection states**

The shell must expose loading, connected, reconnecting, empty, and error states. Every page renders the same `RunControlBar`; no page owns a test timer.

- [ ] **Step 2: Implement the run-mode controls**

Provide Single and PC Periodic controls. Display Device Stream as a preserved mode with a SYSTEM_STATUS capability explanation. Validate interval and cycle count before sending `start`.

- [ ] **Step 3: Implement the uPlot chart leaf**

```ts
const chart = new uPlot(options, data, host)
return () => chart.destroy()
```

Update through `requestAnimationFrame` at no more than 10 Hz, use Canvas, call `setData()` imperatively, and never place individual points in React state.

- [ ] **Step 4: Implement flexible chart placement**

Support one combined plot, one plot per selected quantity, and custom groups. Persist assignments, time window, and selected fields in `localStorage` under the current channel id.

- [ ] **Step 5: Build and test**

```powershell
Set-Location front
npm test
npm run build
```

Expected: tests pass and `front/dist` is produced without TypeScript or bundler errors.

### Task 7: Synchronize contracts, facts, and launch guidance

**Files:**
- Modify: `docs/design/contracts/business-scheduling-layer.md`
- Modify: `docs/design/contracts/websocket-frontend-protocol.md`
- Modify: `docs/design/overview/five-layer-architecture.md`
- Modify: `docs/design/testing/testing-specification.md`
- Modify: `docs/design/README.md`
- Modify: `README.md`
- Modify: `AGENTS.md`
- Modify: `.gitignore`

- [ ] **Step 1: Document one authoritative definition per concept**

BIZ contract owns the three run modes and stop semantics; WebSocket contract owns JSON fields; architecture overview only links and states boundaries; testing specification records actual discovered counts after build.

- [ ] **Step 2: Remove the former “front contains README only” fact**

State that a browser telemetry demo exists, uses a separate Vite development/static build, and still connects only to the loopback WebSocket backend.

- [ ] **Step 3: Add generated frontend output ignores**

```gitignore
front/node_modules/
front/dist/
front/coverage/
```

### Task 8: Final verification

- [ ] **Step 1: Run frontend verification**

```powershell
Set-Location front
npm test
npm run build
Set-Location ..
```

- [ ] **Step 2: Run focused C++ verification**

```powershell
$env:MB_DDF_PROTOCOL_CSV_DIR = "H:\Resources\RTLinux\Demos\MB_DDF_v2\docs\design\product_protocol_csv"
cmake --build build_vs --config Debug --parallel
ctest --test-dir build_vs -C Debug -R "^(TestRunServiceTest|TestApplicationControllerTest|SystemStatusExecutorTest|WebProtocolTest|WebSocket)\." --output-on-failure
```

- [ ] **Step 3: Run full Debug and Release**

```powershell
ctest --test-dir build_vs -C Debug --output-on-failure
cmake --build build_vs --config Release --parallel
ctest --test-dir build_vs -C Release --output-on-failure
```

- [ ] **Step 4: Run static and workspace checks**

```powershell
rg -n "<(hal|biz|algorithm)/|QSerialPort|QUdpSocket|QTcpSocket|waitForTerminal\(|BlockingQueuedConnection" src/app/web front/src
rg -n "setInterval\(.*start|unsplash|picsum|placeholder|placehold|via\.placeholder|lorem\.space|dummyimage" front/src
git diff --check
git status --short
```

Expected: Web/front production sources contain no forbidden layer references or browser-owned repeat-start timer, no placeholder media is present, and diff checking reports no errors.

