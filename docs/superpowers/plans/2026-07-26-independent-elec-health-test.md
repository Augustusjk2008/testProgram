# Independent Electrical Health Test Implementation Plan

> **Execution note:** This plan should be executable either inline in the current session or by a delegated worker. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `ELEC_HEALTH_STATUS` as an independently selectable MB_DDF test that supports one-shot and BIZ-managed PC-periodic execution through the existing HAL control path.

**Architecture:** Keep the current SYSTEM_STATUS configuration and public WebSocket action schema unchanged. Add a dedicated electrical-health algorithm executor and a second single-step configuration; the application controller will select one of the two known executors after validating that the chosen configuration contains exactly one enabled supported step. The executor performs one bounded request-response exchange per invocation, leaving all interval and cycle scheduling to BIZ.

**Tech Stack:** C++17, Qt Core/Network/SerialPort, GoogleTest, existing MB_DDF CSV catalog, HAL `HalControlTransport`, WebSocket v1.

---

### Task 1: Add the Independent Configuration and Controller Acceptance Test

**Files:**
- Create: `configs/mbddf_elec_health.testcfg.json`
- Modify: `tests/app/CMakeLists.txt`
- Modify: `tests/app/test_application_controller_test.cpp`
- Modify: `src/app/src/test_application_controller.cpp`

- [x] **Step 1: Add the electrical-health configuration fixture**

Create `configs/mbddf_elec_health.testcfg.json` with one enabled step. The command has no request parameters and only status/error-code criteria; do not invent voltage thresholds.

```json
{
  "schemaVersion": "1.0",
  "configId": "mbddf-elec-health",
  "productModel": "MB_DDF_v2",
  "productName": "MB_DDF electrical health",
  "configVersion": "1.0.0",
  "steps": [{
    "stepId": "ELEC_HEALTH_STATUS",
    "testItemId": "elec_health_status",
    "name": "Read electrical health",
    "type": "EXCHANGE",
    "algorithmId": "mbddf.elec_health_status",
    "parameters": {"protocol": {"requestValues": {}}},
    "timeoutMs": 2000,
    "retryCount": 0,
    "enabled": true,
    "dependsOn": [],
    "criteria": [
      {"metric": "status", "op": "Equal", "ref": 0, "tol": 0, "passIfMatched": true},
      {"metric": "err_code", "op": "Equal", "ref": 0, "tol": 0, "passIfMatched": true}
    ]
  }],
  "hardwareRequirements": [],
  "protocolProfiles": [
    {"id": "elec_health_status_request", "busType": "CONTROL", "payloadEncoding": "raw", "frameFormat": {"header": "55AA", "lengthBytes": 1, "crc": "CRC-16/XMODEM", "crcByteOrder": "little"}, "timing": {"timeoutMs": 2000}},
    {"id": "elec_health_status_response", "busType": "CONTROL", "payloadEncoding": "raw", "frameFormat": {"header": "55AA", "lengthBytes": 1, "crc": "CRC-16/XMODEM", "crcByteOrder": "little"}, "timing": {"timeoutMs": 2000}}
  ],
  "executionConfig": {
    "protocolAssetRoot": "${MB_DDF_PROTOCOL_CSV_DIR}",
    "protocol": {"requestProfileId": "elec_health_status_request", "responseProfileId": "elec_health_status_response"},
    "transport": {"openTimeoutMs": 1000, "readChunkBytes": 260},
    "initialSequence": 4660
  },
  "safetyPolicy": {"enterSafeStateOnStop": true, "enterSafeStateOnError": true},
  "runtimeConfig": {"parallelEnabled": false, "maxParallel": 1, "defaultTimeoutMs": 2000, "defaultRetryCount": 0, "retryIntervalMs": 0, "taskPriorityDefault": 2, "stopOnFirstFailure": true, "allowResume": false},
  "reportFields": {}
}
```

- [x] **Step 2: Write the failing controller acceptance test**

Expose the fixture to `hwtest_app_tests` as `HWTEST_APP_ELEC_HEALTH_CONFIG`, then add this test before changing the controller:

```cpp
TEST(TestApplicationControllerTest, LoadsAndPreparesElectricalHealthConfiguration)
{
    ensureQtApplication();
    const QString assets = qEnvironmentVariable("MB_DDF_PROTOCOL_CSV_DIR");
    if (!QFileInfo(assets).isDir()) {
        GTEST_SKIP() << "MB_DDF protocol assets are not available";
    }

    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(
        QStringLiteral(HWTEST_APP_ELEC_HEALTH_CONFIG),
        QStringLiteral(HWTEST_APP_HAL_CONFIG)).ok);
    ASSERT_TRUE(controller.selectControl(QStringLiteral("CONTROL_NETWORK")).ok);
    ASSERT_TRUE(controller.prepare().ok);
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("ready"));
    EXPECT_TRUE(controller.shutdown().ok);
}
```

- [x] **Step 3: Run the new test and observe the intended failure**

Run:

```powershell
cmake --build build_vs --config Debug --target hwtest_app_tests --parallel
ctest --test-dir build_vs -C Debug -R "LoadsAndPreparesElectricalHealthConfiguration" --output-on-failure
```

Expected: the test fails at `loadConfigurations()` with `unsupported_algorithm`, proving the test reaches the current one-algorithm gate.

- [x] **Step 4: Update controller validation and executor selection minimally**

Replace the SYSTEM_STATUS-only count with exactly one enabled step whose ID is either `mbddf.system_status` or `mbddf.elec_health_status`. Store the accepted algorithm ID in `Impl`; change its executor member to `std::unique_ptr<hwtest::biz::IAlgorithmExecutor>` and create the matching dedicated executor in `prepare()`.

```cpp
const QSet<QString> supportedAlgorithms{
    QStringLiteral("mbddf.system_status"),
    QStringLiteral("mbddf.elec_health_status"),
};
if (!supportedAlgorithms.contains(step.algorithmId)) {
    return failure(QStringLiteral("unsupported_algorithm"),
                   QStringLiteral("Unsupported MB_DDF algorithm '%1'").arg(step.algorithmId));
}
```

The existing configuration must still require exactly one enabled step; mixed or multi-step plans remain rejected under the chosen independent-configuration design.

- [x] **Step 5: Re-run the controller test**

Run the command from Step 3. Expected: one passing test, with no attempt to transact on UDP during `prepare()`.

### Task 2: Create the Dedicated Electrical-Health Executor by TDD

**Files:**
- Create: `src/algorithm/include/algorithm/elec_health_status_executor.h`
- Create: `src/algorithm/src/elec_health_status_executor.cpp`
- Modify: `src/algorithm/CMakeLists.txt`
- Modify: `tests/algorithm/CMakeLists.txt`
- Modify: `tests/algorithm/system_status_executor_test.cpp`

- [x] **Step 1: Write failing executor tests**

Add `HWTEST_MBDDF_ELEC_HEALTH_CONFIG` in `tests/algorithm/CMakeLists.txt`, include the not-yet-created executor header in `system_status_executor_test.cpp`, and add tests named:

```cpp
TEST(ElecHealthStatusExecutorTest, ConfigBIZScriptedTransportProducesReadOnlyExchange)
TEST(ElecHealthStatusExecutorTest, RejectsDeviceManagedStreamingBeforeOpeningTransport)
TEST(ElecHealthStatusExecutorTest, RemoteStatusErrorBecomesRemoteCommandError)
```

The first test loads the electrical-health config through `ITestRunService`, uses `ScriptedByteTransport`, and asserts exactly one request with command bytes `0x05, 0x01`, sequence `0x1234`, and zero-filled B9-B51. Its scripted response is built with `elec_health_status_response` and values `status=0`, `err_code=0`, `c_volt=28.51`, `external_vol=3.30`, `value_YX=5.045`; it asserts `Pass/Ok`, channel `ELEC_HEALTH_STATUS`, and decoded values in `rawData.responseValues`.

- [x] **Step 2: Run the executor tests and observe RED**

Run:

```powershell
cmake --build build_vs --config Debug --target hwtest_algorithm_tests --parallel
ctest --test-dir build_vs -C Debug -R "ElecHealthStatusExecutorTest" --output-on-failure
```

Expected: compilation fails because `algorithm/elec_health_status_executor.h` does not exist.

- [x] **Step 3: Implement the minimal dedicated executor**

Mirror the bounded request-response lifecycle of `SystemStatusAlgorithmExecutor`, with only command-specific names and semantics changed:

```cpp
class ElecHealthStatusAlgorithmExecutor final : public hwtest::biz::IAlgorithmExecutor {
public:
    explicit ElecHealthStatusAlgorithmExecutor(std::unique_ptr<IByteTransport> transport);
    // prepare, executeStep, requestStop, reset, shutdown
};
```

`prepare()` must resolve only `elec_health_status_request` and `elec_health_status_response`, reject `device_stream`, enforce the existing 614400/8E1 transport options, and preserve initial-sequence parsing. `executeStep()` must send one frame, require command/sequence echo, map status or err_code nonzero to `RemoteCommandError`, emit one `RawSample` with `channelId = "ELEC_HEALTH_STATUS"`, and use log category `mbddf.elec_health_status`. Do not modify `mbddf_protocol`, `HalControlTransport`, `SystemStatusSimulator`, BIZ scheduling, or add a device-side loop.

- [x] **Step 4: Run focused tests to GREEN**

Run the command from Step 2. Expected: all three electrical-health tests pass.

### Task 3: Prove the Application/HAL UDP Path for Single and Periodic Runs

**Files:**
- Modify: `tests/app/support/mbddf_udp_test_peer.h`
- Modify: `tests/app/test_application_controller_test.cpp`

- [x] **Step 1: Write failing application integration tests**

Add tests that use `configs/mbddf_elec_health.testcfg.json`, select a temporary loopback UDP HAL configuration, then verify:

```cpp
TEST(TestApplicationControllerTest, RunsElectricalHealthThroughTheSelectedUdpControlResource)
TEST(TestApplicationControllerTest, ElectricalHealthPcPeriodicForwardsOneSamplePerCycle)
```

The first must assert a `0x05/0x01` request and one `ELEC_HEALTH_STATUS` sample. The second calls `start({"pc_periodic", 10, 2})`, supplies two independent responses, and asserts sample cycle indexes `{1, 2}`, exactly two requests, and terminal `cycleIndex=2`, `sampleCount=2`.

- [x] **Step 2: Run tests and observe RED**

Run:

```powershell
cmake --build build_vs --config Debug --target hwtest_app_tests --parallel
ctest --test-dir build_vs -C Debug -R "ElectricalHealth" --output-on-failure
```

Expected: tests fail because the UDP peer has only a SYSTEM_STATUS response helper or because the selected executor is not yet complete.

- [x] **Step 3: Generalize only the test peer response helper**

Keep `replyToLastRequest()` unchanged for existing tests. Add a helper that accepts response profile name and values, decodes the stored request, copies its sequence, encodes the specified response profile, and returns that one frame. It must not change production code or simulate device streaming.

```cpp
bool replyToLastRequest(const QString& responseProfile,
                        QVariantMap responseValues,
                        QString* error = nullptr);
```

- [x] **Step 4: Re-run focused app tests to GREEN**

Run the command from Step 2. Expected: both tests pass through `TestApplicationController -> BIZ -> executor -> HAL -> qt.udp`.

### Task 4: Complete Regression Coverage, Configuration Documentation, and Real COM3 Validation

**Files:**
- Modify: `docs/design/overview/five-layer-architecture.md`
- Modify: `docs/design/contracts/device-communication-protocol.md`
- Modify: `docs/design/contracts/business-scheduling-layer.md` only if a public semantic changes
- Modify: `docs/design/testing/testing-specification.md`

- [x] **Step 1: Run the algorithm, app, BIZ architecture, and WebSocket suites**

```powershell
cmake --build build_vs --config Debug --parallel
ctest --test-dir build_vs -C Debug --output-on-failure
```

Also run the local Markdown link/terminology checks and `git diff --check`.

- [x] **Step 2: Perform authorized real-hardware smoke tests**

Start only the board `HW_TEST` service and `hwtest_web`; do not start Vite or a browser. Run two separate backend sessions, each using COM3 and the approved external CSV directory:

1. SYSTEM_STATUS configuration: one `single` and `pc_periodic(intervalMs=100,maxCycles=3)`.
2. Electrical-health configuration: one `single` and `pc_periodic(intervalMs=100,maxCycles=3)`.

For electrical health, require response command `0x05/0x01`, one sample per single run, three samples with cycles `1,2,3` for periodic, `Pass/Ok`, `status=0`, and `err_code=0`. Capture backend diagnostics and assert no QThread timer warning. Send `quit`, terminate only the verified remote `MB_DDF_v2` PID with `SIGTERM`, then verify 18765 is free, COM3 opens/closes, and no local/remote service remains.

- [x] **Step 3: Update only current fact sources with observed evidence and limits**

Record the additional test, the exact independent-config boundary, real command/response evidence, warning count, and cleanup result. Preserve the limits: no device-stream support, no voltage threshold product judgment, no long-duration/plug-unplug/running-stop/physical safety acceptance.
