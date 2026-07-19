# MB_DDF Standard Control Channel Implementation Plan

> **Execution note:** This plan should be executable either inline in the current session or by a delegated worker. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build one configuration-driven `SYSTEM_STATUS` PC runner whose selected control resource uses either Qt SerialPort or Qt UDP directly inside HAL, without traversing a Vendor Adapter.

**Architecture:** BIZ remains hardware-independent and the MB_DDF algorithm continues to own CSV loading, frame assembly, CRC, command/sequence matching, and verdicts. HAL exposes one raw `IControlChannel`; each configured logical control resource selects an internal `qt.serial` or `qt.udp` Provider. The PC opens only the resource named by `control.resourceId`; the DUT-side protocol and simultaneous availability of both physical links are unchanged.

**Tech Stack:** C++17, Qt 5.15 Core/Network/SerialPort with Qt 6 fallback, GoogleTest, CMake/CTest.

---

### Task 1: Add the raw HAL control-channel contract and Qt dependencies

**Files:**
- Modify: `CMakeLists.txt`
- Create: `src/hal/include/hal/i_control_channel.h`
- Modify: `src/hal/include/hal/i_hal_device.h`
- Modify: `src/hal/src/hal_device.h`
- Modify: `src/hal/src/hal_device.cpp`
- Modify: `src/hal/CMakeLists.txt`
- Test: `tests/hal/hal_device_test.cpp`

- [ ] **Step 1: Write a failing public-contract test**

Add a test that obtains `device.controlChannel()` and verifies a missing `control` resource returns `NotFound`. The intended API is:

```cpp
class IControlChannel {
public:
    virtual ~IControlChannel() = default;
    virtual HalStatus openControl(const ResourceId&, const OperationOptions&) = 0;
    virtual HalStatus closeControl(const ResourceId&, const OperationOptions&) = 0;
    virtual HalStatus writeControl(const ResourceId&, const QByteArray&, const OperationOptions&) = 0;
    virtual HalResult<QByteArray> readControl(const ResourceId&, int, const OperationOptions&) = 0;
};
```

- [ ] **Step 2: Run the HAL test target and confirm RED**

Run:

```powershell
cmake --build build_codex --config Debug --target hwtest_hal_tests --parallel
```

Expected: compilation fails because `IHalDevice::controlChannel()` does not exist.

- [ ] **Step 3: Add the minimal public API and Qt module targets**

Find Core, Network, and SerialPort from the same Qt major and define:

```cmake
set(HWTEST_QT_CORE_TARGET Qt5::Core)
set(HWTEST_QT_NETWORK_TARGET Qt5::Network)
set(HWTEST_QT_SERIALPORT_TARGET Qt5::SerialPort)
```

Use the corresponding Qt 6 targets only when the complete Qt 5 component set is unavailable. Add `IControlChannel* controlChannel()` to `IHalDevice`; make `HalDevice` implement it without changing existing serial/CAN/analog APIs.

- [ ] **Step 4: Rebuild and confirm the contract test reaches runtime**

Expected: the test builds and fails only because no control Provider is implemented yet.

### Task 2: Route configured control resources to standard Qt Providers

**Files:**
- Create: `src/hal/src/control_io_provider.h`
- Create: `src/hal/src/qt_serial_control_provider.h`
- Create: `src/hal/src/qt_serial_control_provider.cpp`
- Create: `src/hal/src/qt_udp_control_provider.h`
- Create: `src/hal/src/qt_udp_control_provider.cpp`
- Create: `src/hal/src/control_channel_manager.h`
- Create: `src/hal/src/control_channel_manager.cpp`
- Modify: `src/hal/src/resource_mapper.h`
- Modify: `src/hal/src/resource_mapper.cpp`
- Modify: `src/hal/src/hal_device.h`
- Modify: `src/hal/src/hal_device.cpp`
- Modify: `src/hal/CMakeLists.txt`
- Create: `tests/hal/control_channel_test.cpp`
- Modify: `tests/hal/CMakeLists.txt`

- [ ] **Step 1: Write failing configuration and UDP loopback tests**

Cover these behaviors independently:

```text
missing providerId             -> InvalidArgument
unknown providerId             -> NotSupported
qt.serial without portName     -> InvalidArgument
qt.udp without remote endpoint -> InvalidArgument
qt.udp loopback                -> exact raw request/response bytes
read timeout                   -> HalStatusCode::Timeout
close then read                -> InvalidState
```

- [ ] **Step 2: Run only the new tests and confirm RED**

Run:

```powershell
cmake --build build_codex --config Debug --target hwtest_hal_tests --parallel
ctest --test-dir build_codex -C Debug -R "^ControlChannelTest\." --output-on-failure
```

Expected: the new tests fail because configured Providers are not available.

- [ ] **Step 3: Parse `providerId` and implement the Provider boundary**

Extend internal `ResourceBinding` with `providerId`. Only resources with `module: "control"` use this field. The Provider interface remains raw-byte-only:

```cpp
class ControlIoProvider {
public:
    virtual ~ControlIoProvider() = default;
    virtual HalStatus open(const QVariantMap&, const OperationOptions&) = 0;
    virtual HalStatus close(const OperationOptions&) = 0;
    virtual HalStatus write(const QByteArray&, const OperationOptions&) = 0;
    virtual HalResult<QByteArray> read(int, const OperationOptions&) = 0;
};
```

`ControlChannelManager` creates `qt.serial` or `qt.udp` strictly from the selected resource. Missing or unknown identifiers fail closed; it never falls back to Mock or Adapter.

- [ ] **Step 4: Implement `QSerialPort` and `QUdpSocket` Providers**

`qt.serial` requires all physical settings in resource `properties`: `portName`, `baudRate`, `dataBits`, `parity`, `stopBits`, `flowControl`.

`qt.udp` requires `remoteAddress` and `remotePort`; `localAddress` and `localPort` are explicit optional bind settings. Each write sends one complete MB_DDF physical frame as one datagram. No IP address or port is compiled into the implementation.

- [ ] **Step 5: Confirm GREEN and run all HAL tests**

Expected: loopback bytes round-trip; serial invalid configuration fails without scanning or opening a real COM port; existing HAL tests remain green.

### Task 3: Add a HAL control transport with MB_DDF stream framing

**Files:**
- Modify: `src/algorithm/include/algorithm/mbddf_transport.h`
- Modify: `src/algorithm/src/mbddf_transport.cpp`
- Modify: `src/algorithm/src/system_status_executor.cpp`
- Test: `tests/algorithm/system_status_executor_test.cpp`

- [ ] **Step 1: Write failing transport tests**

Add a real `IControlChannel` test double and cover:

```text
split frame across reads       -> one complete frame returned
two concatenated frames        -> first returned, second buffered
noise before 55 AA sync        -> noise discarded
deadline exhausted             -> TransportResult::Timeout
completed transaction          -> control resource closed
```

- [ ] **Step 2: Run the focused tests and confirm RED**

Expected: compilation fails because `HalControlTransport` is not defined.

- [ ] **Step 3: Implement `HalControlTransport`**

The constructor accepts only `IHalDevice*` and a logical `ResourceId`. `transact()` writes once, accumulates raw reads under one remaining deadline, and extracts frames as:

```text
sync(55 AA) + payload_length(U8) + payload + crc16(2 bytes)
total bytes = 2 + 1 + payload_length + 2
```

CRC and command/sequence validation remain in `mbddf_protocol` and `SystemStatusAlgorithmExecutor`.

- [ ] **Step 4: Move transport lifetime into one execution attempt**

`prepare()` validates CSV/configuration but does not open I/O. `executeStep()` opens the selected control resource, performs one transaction, and closes it on every success/error/cancel path. This keeps Qt I/O objects on the BIZ worker thread and makes retries reopen a clean channel.

- [ ] **Step 5: Confirm GREEN and retain Simulator golden tests**

Expected: existing Simulator/golden tests still pass unchanged in purpose.

### Task 4: Prove `SYSTEM_STATUS` over the Qt UDP HAL Provider

**Files:**
- Modify: `tests/algorithm/system_status_executor_test.cpp`
- Modify: `tests/algorithm/CMakeLists.txt`

- [ ] **Step 1: Write a failing end-to-end test**

Start a test-only UDP peer on `127.0.0.1` with an ephemeral port. It decodes the request sequence using the approved CSV catalog and returns a valid `system_status_response` datagram. Assemble:

```text
HAL service -> openDevice -> HalControlTransport
-> SystemStatusAlgorithmExecutor -> ITestRunService
```

Assert the golden request bytes, decoded measurements, `Pass`, provider identity in HAL logs, and closed channel.

- [ ] **Step 2: Confirm RED before wiring the transport**

Expected: the test cannot construct or run the selected control channel.

- [ ] **Step 3: Wire the resource-driven transport and confirm GREEN**

Add separate no-response and bad-CRC cases. HAL reports timeout/I/O only; the algorithm reports bad CRC as `ProtocolParseError`.

### Task 5: Add a minimal configuration-driven PC runner

**Files:**
- Create: `src/app/CMakeLists.txt`
- Create: `src/app/main.cpp`
- Modify: `CMakeLists.txt`
- Create: `configs/mbddf_system_status.testcfg.json`
- Create: `configs/mbddf_pc_hal.json`

- [ ] **Step 1: Add a runner smoke test through CTest or a focused config-loader test**

The runner requires `--test-config` and `--hal-config`; malformed/missing files return nonzero without opening hardware.

- [ ] **Step 2: Implement the composition root**

The HAL deployment file owns the PC-only selection:

```json
{
  "control": {
    "deviceId": "mbddf_dut",
    "resourceId": "CONTROL_SERIAL"
  }
}
```

Both `CONTROL_SERIAL` (`qt.serial`) and `CONTROL_NETWORK` (`qt.udp`) remain defined under `hardware.resources`. Switching the one `resourceId` field changes the PC control link; no product protocol or DUT configuration changes.

The runner loads both files, initializes HAL, opens the configured device, creates `HalControlTransport`, runs the single configured `mbddf.system_status` step through BIZ, prints one compact JSON result, and shuts down in this order:

```text
BIZ shutdown -> executor/transport destruction -> HAL closeDevice -> HAL shutdown
```

- [ ] **Step 3: Build and run invalid-config smoke tests**

Expected: the executable builds; invalid selections fail before any Provider opens.

### Task 6: Synchronize contracts and verify the deliverable

**Files:**
- Modify: `AGENTS.md`
- Modify: `src/hal/AGENTS.md`
- Modify: `docs/design/overview/five-layer-architecture.md`
- Modify: `docs/design/contracts/hal-interface-protocol.md`
- Modify: `docs/design/contracts/device-communication-protocol.md`
- Modify: `docs/design/implementation/hal-implementation-design-report.md`
- Modify: `docs/design/testing/testing-specification.md`
- Modify: `docs/design/testing/hal-test-design-report.md`

- [ ] **Step 1: Update current implementation facts**

Document `IControlChannel`, `qt.serial`, `qt.udp`, PC-only resource selection, UDP datagram framing, Qt dependencies, and the fact that Vendor Adapter is bypassed only for these standard Providers. Keep TCP, UI, other MB_DDF tests, and real serial hardware marked unimplemented.

- [ ] **Step 2: Run complete verification**

```powershell
cmake -S . -B build_codex -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build_codex --config Debug --parallel
$env:MB_DDF_PROTOCOL_CSV_DIR = "H:\Resources\RTLinux\Demos\MB_DDF_v2\docs\design\product_protocol_csv"
ctest --test-dir build_codex -C Debug --output-on-failure
cmake --build build_codex --config Release --parallel
ctest --test-dir build_codex -C Release --output-on-failure
git diff --check
```

Expected: both configurations build, all non-hardware tests pass with the approved 32-file CSV directory, and no whitespace errors remain.
