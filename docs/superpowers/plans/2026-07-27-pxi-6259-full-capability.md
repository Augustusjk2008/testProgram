# PXI-6259 Full Capability Implementation Plan

> **Current facts:** See `docs/design/overview/five-layer-architecture.md`, `docs/design/contracts/hal-interface-protocol.md`, and `docs/design/testing/testing-specification.md`.
>
> **Execution note:** This plan should be executable either inline in the current session or by a delegated worker. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the USB-6259-specific digital adapter with a PXI-6259 adapter that supports versioned device configuration, analog and digital input/output, finite and continuous hardware-timed tasks, sampling clocks, triggers, counters, and deterministic safe shutdown.

**Architecture:** HAL remains the authority for logical devices, resource mappings, and safe state. It projects one validated `AdapterDeviceOpenSpec` per device through the existing ABI v1 `openOptionsJson`. Advanced acquisition uses a separate optional task ABI so existing adapter ABI v1 binaries remain loadable; `HalDevice` exposes the task lifecycle and stops active tasks before applying safe state.

**Tech Stack:** C++17, Qt 5.15/Qt 6 fallback, NI-DAQmx C API, C ABI shared libraries, GoogleTest/CTest, repository Fake NIDAQmx.

---

### Task 1: Versioned PXI Device Projection

**Files:**
- Create: `src/hal/src/adapter_device_open_spec.h`
- Create: `src/hal/src/adapter_device_open_spec.cpp`
- Modify: `src/hal/src/CMakeLists.txt`
- Modify: `src/hal/src/hal_service.cpp`
- Modify: `src/hal/src/c_abi_adapter.cpp`
- Modify: `configs/mbddf_pc_hal.json`
- Modify: `configs/mbddf_di.testcfg.json`
- Test: `tests/hal/resource_mapper_test.cpp`
- Test: `tests/hal/hal_service_test.cpp`

- [x] **Step 1: Write failing projection tests**

  Assert that a PXI device projects `schema=hwtest.adapter-device-open`, `version=1`, logical/physical identity, channels, task profiles, and resource-scoped safe state, while duplicate physical channels and mismatched identities are rejected.

- [x] **Step 2: Verify RED**

  Run `cmake --build build_vs --config Debug --target HalServiceTest ResourceMapperTest` followed by focused CTest; expect missing projection fields or helper symbols.

- [x] **Step 3: Implement the private DTO**

  Define `AdapterDeviceOpenSpec` with these concrete fields:

  ```cpp
  struct AdapterDeviceOpenSpec {
      int version = 1;
      DeviceDescriptor device;
      QString physicalDeviceId;
      QVector<ResourceBinding> channels;
      QVariantMap safeState;
      QVariantList taskProfiles;
      QVariantMap toVariantMap() const;
  };
  ```

  Build it from `ResourceMapper` output in `HalService::openDevice()` and pass it unchanged to `HardwareAdapter::openDevice()`.

- [x] **Step 4: Migrate configuration**

  Set the template model to `PXI-6259`, retain `CONFIGURE_ME` serial protection, move MAX device name to `hardware.devices[].properties.vendor.ni.deviceName`, add AI/AO/DIO/counter resources and task profiles, and update the DI hardware requirement.

- [x] **Step 5: Verify GREEN**

  Run focused HAL tests and parse both JSON files with PowerShell `ConvertFrom-Json`.

### Task 2: Optional Task ABI and Public HAL Task Types

**Files:**
- Create: `src/hal/include/hal/hal_adapter_task_abi.h`
- Create: `src/hal/include/hal/i_sample_task_io.h`
- Modify: `src/hal/include/hal/hal_types.h`
- Modify: `src/hal/include/hal/i_hal_device.h`
- Modify: `src/hal/src/adapter_loader.h`
- Modify: `src/hal/src/adapter_loader.cpp`
- Modify: `src/hal/src/hardware_adapter.h`
- Modify: `src/hal/src/c_abi_adapter.h`
- Modify: `src/hal/src/c_abi_adapter.cpp`
- Modify: `src/hal/src/mock_adapter.h`
- Modify: `src/hal/src/mock_adapter.cpp`
- Test: `tests/hal/adapter_loader_test.cpp`
- Test: `tests/hal/c_abi_adapter_test.cpp`
- Test fixture: `tests/hal/fixtures/digital_adapter_fixture.cpp`

- [x] **Step 1: Write failing optional-extension tests**

  Verify that an adapter without `hal_adapter_get_task_api_v1` still loads, while a fixture with the symbol exposes create/start/read/write/status/stop/close.

- [x] **Step 2: Verify RED**

  Run `AdapterLoaderTest` and `CAbiAdapterTest`; expect unresolved task API types and methods.

- [x] **Step 3: Add concrete ABI types**

  Add `HalAdapterTaskKind`, `HalAdapterTaskMode`, `HalAdapterSampleType`, clock/trigger/counter structs, task configuration, block buffer, status, opaque task handle, and `HalAdapterTaskApiV1`. Export the optional symbol `hal_adapter_get_task_api_v1` without changing `HalAdapterApiV1`.

- [x] **Step 4: Add public HAL task DTOs**

  Add `SampleTaskConfig`, `SampleTaskBlock`, `SampleTaskStatus`, `SampleTaskId`, clock/trigger/counter enums, and `ISampleTaskIo` with:

  ```cpp
  createTask(const SampleTaskConfig&, const OperationOptions&)
  startTask(const SampleTaskId&, const OperationOptions&)
  readTask(const SampleTaskId&, int maxSamplesPerChannel, const OperationOptions&)
  writeTask(const SampleTaskId&, const SampleTaskBlock&, const OperationOptions&)
  taskStatus(const SampleTaskId&, const OperationOptions&)
  stopTask(const SampleTaskId&, const OperationOptions&)
  closeTask(const SampleTaskId&, const OperationOptions&)
  ```

- [x] **Step 5: Implement loader and adapter forwarding**

  Resolve the optional task symbol after the base ABI loads, copy all variable strings and arrays during `createTask`, preserve all channel-major samples, and normalize vendor errors.

- [x] **Step 6: Verify GREEN**

  Run focused loader/C ABI tests and existing missing-symbol tests.

### Task 3: HalDevice Task Lifecycle and Safe Shutdown

**Files:**
- Modify: `src/hal/src/hal_device.h`
- Modify: `src/hal/src/hal_device.cpp`
- Test: `tests/hal/hal_device_test.cpp`
- Test: `tests/hal/hal_service_test.cpp`

- [x] **Step 1: Write failing mapping and shutdown tests**

  Verify resource IDs become one physical-index vector, all samples survive channel-major projection, active tasks stop and close before analog/digital safe state, and device close follows last.

- [x] **Step 2: Verify RED**

  Run focused `HalDeviceTest` and `HalServiceTest`; expect `sampleTasks()` and lifecycle methods to be absent.

- [x] **Step 3: Implement `ISampleTaskIo` in `HalDevice`**

  Validate direction/module bindings, keep a map from public task IDs to backend task IDs, and perform close ordering `stop tasks -> close tasks -> safe state -> backend close`.

- [x] **Step 4: Verify GREEN**

  Run all HAL device/service tests.

### Task 4: PXI-6259 Parser and Device Profile

**Files:**
- Create: `src/adapters/ni_daqmx/ni_daqmx_config.h`
- Create: `src/adapters/ni_daqmx/ni_daqmx_config.cpp`
- Modify: `src/adapters/ni_daqmx/CMakeLists.txt`
- Modify: `src/adapters/ni_daqmx/ni_daqmx_adapter.cpp`
- Test: `src/adapters/ni_daqmx/tests/ni_daqmx_adapter_fake_test.cpp`

- [x] **Step 1: Write failing PXI identity/topology tests**

  Cover PXI-6259 identity, P0 `0..31`, P1/P2 `0..7`, AI `0..31`, AO `0..3`, counter `0..1`, resource uniqueness, and legacy full-HAL JSON rejection after migration.

- [x] **Step 2: Verify RED**

  Run `NiDaqmxAdapterFakeTest`; expect USB identity or legacy parser behavior.

- [x] **Step 3: Split parsing from production I/O**

  Move the JSON parser and `NiDaqmxConfig` into dedicated files. Parse only `AdapterDeviceOpenSpec` in `openDevice()`. Keep adapter initialization limited to driver-level settings and runtime DAQmx discovery.

- [x] **Step 4: Add the PXI profile**

  Validate exact model/serial/MAX identity and channel topology without a USB-specific branch; capabilities report analog, digital, counter, timing, trigger, and route limits.

- [x] **Step 5: Verify GREEN**

  Run the Fake adapter test and configuration tests.

### Task 5: Fake NIDAQmx Full Task Surface

**Files:**
- Modify: `src/adapters/ni_daqmx/tests/fake/NIDAQmx.h`
- Modify: `src/adapters/ni_daqmx/tests/fake_nidaqmx_control.h`
- Modify: `src/adapters/ni_daqmx/tests/fake_nidaqmx.cpp`
- Modify: `src/adapters/ni_daqmx/tests/ni_daqmx_adapter_fake_test.cpp`

- [x] **Step 1: Add failing AI/AO/task tests**

  Cover AI/AO channel creation, exact range/terminal configuration, finite and continuous sample clock calls, digital/analog start triggers, external clock routes, counter input/output, short reads, overflow, underflow, timeout, disconnect, and Stop/Clear ordering.

- [x] **Step 2: Verify RED**

  Build and run `NiDaqmxAdapterFakeTest`; expect missing DAQmx symbols.

- [x] **Step 3: Implement deterministic Fake state**

  Add per-task kind, channels, timing, trigger, counter, input queue, output blocks, error injection, and call-order logging. Implement the exact DAQmx functions used by production.

- [x] **Step 4: Verify GREEN**

  Re-run the Fake test with no real SDK.

### Task 6: NI AI/AO, Hardware-Timed DIO, and Counter Tasks

**Files:**
- Modify: `src/adapters/ni_daqmx/ni_daqmx_adapter.cpp`
- Test: `src/adapters/ni_daqmx/tests/ni_daqmx_adapter_fake_test.cpp`

- [x] **Step 1: Implement v1 on-demand analog functions after RED**

  Use `DAQmxCreateAIVoltageChan`/`DAQmxCreateAOVoltageChan`, `DAQmxReadAnalogF64`, and `DAQmxWriteAnalogF64`; export analog ABI slots and preserve full multi-sample channel-major data.

- [x] **Step 2: Implement task creation**

  Create AI/AO/DI/DO/counter channels, configure `DAQmxCfgSampClkTiming`, finite/continuous mode, buffer size, start/reference/pause triggers, and counter edge/pulse modes from `HalAdapterTaskConfig`.

- [x] **Step 3: Implement task I/O and lifecycle**

  Support chunked analog/digital/counter read/write, partial reads, actual sample counts, timestamps, task status, stop, clear, and close-time cancellation.

- [x] **Step 4: Implement safe state**

  Stop streams first, write AO and DO safe values, then clear tasks. Preserve the first cleanup error while completing all cleanup steps.

- [x] **Step 5: Verify GREEN**

  Run every NI Fake test plus C ABI/HalDevice task tests.

### Task 7: Contracts, Builds, and Evidence

**Files:**
- Modify: `docs/design/contracts/hal-interface-protocol.md`
- Modify: `docs/design/contracts/device-communication-protocol.md`
- Modify: `docs/design/testing/testing-specification.md`
- Modify: `docs/design/overview/five-layer-architecture.md`
- Modify: `AGENTS.md`

- [x] **Step 1: Document current implemented semantics**

  Record the versioned open spec, task ABI, channel-major blocks, clock/trigger/counter configuration, shutdown order, optional symbol compatibility, and Fake-versus-real-hardware evidence boundary.

- [x] **Step 2: Run focused verification**

  Configure with `-DBUILD_TESTING=ON`, build Debug, and run NI Fake, HAL, app configuration, and ABI tests.

- [x] **Step 3: Run full verification**

  Run Debug and Release builds/CTest, JSON parsing, ABI export inspection, `rg DAQmxResetDevice`, link/term checks, and `git diff --check`.

- [x] **Step 4: Record real-hardware gate**

  Keep the template serial as `CONFIGURE_ME`. Classify the result as software/Fake evidence until PXI chassis, MAX identity, cable/pin map, common ground, levels, isolation, rates, triggers, long-duration streaming, and safe shutdown are recorded on hardware.
