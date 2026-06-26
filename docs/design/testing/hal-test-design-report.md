# HAL 测试设计报告

> 范围：`tests/hal` 当前测试。
> 目标：不读测试代码也能理解测试怎么组织、覆盖什么、如何运行、还缺什么。

---

## 1. 测试定位

HAL 测试验证 `src/hal` 作为独立 Qt Core 库的行为，重点是：

- 公共接口契约。
- 资源映射。
- 安全校验。
- 错误映射。
- 会话生命周期。
- Mock 后端主流程。
- Adapter DLL 加载器基本契约。

测试不依赖 UI、业务调度、算法层、真实硬件、厂家 SDK。

---

## 2. 目录

```text
tests/
  CMakeLists.txt
  hal/
    CMakeLists.txt
    test_support.h
    adapter_loader_test.cpp
    hal_device_test.cpp
    hal_error_mapper_test.cpp
    hal_service_test.cpp
    hal_types_test.cpp
    mock_adapter_test.cpp
    resource_mapper_test.cpp
    safety_guard_test.cpp
    fixtures/
      adapter_loader_fixture.cpp
      adapter_loader_missing_symbol_fixture.cpp
```

`tests/CMakeLists.txt` 只注册 `hal/`。HAL 测试统一在仓库根 `tests/hal/` 下。

---

## 3. 构建接入

根 `CMakeLists.txt`：

```text
include(CTest)
add_subdirectory(src/hal)
if (BUILD_TESTING)
  add_subdirectory(tests)
endif()
```

`tests/hal/CMakeLists.txt`：

- 用 `FetchContent` 拉取 GoogleTest v1.15.2。
- 关闭 gmock 和 gtest install。
- 构建测试目标 `hwtest_hal_tests`。
- 链接 `hwtest_hal` 和 `GTest::gtest_main`。
- 通过 `gtest_discover_tests(hwtest_hal_tests)` 注册 CTest。
- 构建 2 个 DLL fixture 给 `AdapterLoaderTest` 使用。

测试目标 include：

```text
tests/hal
src/hal/include
src/hal/src
```

因此测试可访问公共头，也可测试 HAL 内部类，如 `ResourceMapper`、`SafetyGuard`、`MockAdapter`、`AdapterLoader`、`HalDevice`。

---

## 4. 运行方式

推荐 Debug：

```powershell
cmake -S . -B build_vs -G "Visual Studio 17 2022" -A x64
cmake --build build_vs --config Debug --target hwtest_hal_tests
ctest --test-dir build_vs -C Debug --output-on-failure
```

推荐 Release：

```powershell
cmake --build build_vs --config Release --target hwtest_hal_tests
ctest --test-dir build_vs -C Release --output-on-failure
```

当前已验证 Debug：

```text
22/22 passed
0 failed
```

---

## 5. 测试支撑配置

主文件：`test_support.h`。

提供两个配置构造函数：

### 5.1 `defaultHalConfig()`

生成 1 个设备：

```text
deviceId: main_daq
adapterId: mock.adapter.v1
vendor: MockVendor
model: MockDevice
serialNumber: DAQ-001
location: rack-1
firmwareVersion: 1.0.0
match.serialNumber: DAQ-001
```

生成 6 个逻辑资源：

```text
AD_MAIN_0    analog  input          physicalIndex=0
DA_MAIN_0    analog  output         physicalIndex=0
DI_POWER_OK  digital input          physicalIndex=0
DO_POWER_EN  digital output         physicalIndex=0
SERIAL_A     serial  bidirectional  physicalIndex=0
CANFD_A      canfd   bidirectional  physicalIndex=0
```

这些资源共用 physicalIndex=0，用于验证 Mock 回环：

- DA 写入后 AD 读回。
- DO 写入后 DI 读回。
- 串口写入后读回。
- CAN 发送后接收。

### 5.2 `safeStateHalConfig()`

在默认配置上增加：

```text
safeState.DA_MAIN_0 = 0.0
safeState.DO_POWER_EN = "Low"
```

用于验证关闭设备时输出回到安全状态。

---

## 6. Adapter Loader Fixture

`AdapterLoaderTest` 需要真实 DLL 路径，所以测试构建两个动态库。

### 6.1 `hal_adapter_fixture`

源文件：`fixtures/adapter_loader_fixture.cpp`。

导出：

```text
hal_adapter_get_api_v1(host, outApi)
```

行为：

- host 或 outApi 为空时返回 -1。
- 否则填：
  - `abiVersion = HAL_ADAPTER_ABI_VERSION`
  - `structSize = sizeof(HalAdapterApiV1)`
- 返回 0。

用途：验证 `AdapterLoader` 正常加载、解析符号、读取 ABI。

### 6.2 `hal_adapter_missing_symbol_fixture`

源文件：`fixtures/adapter_loader_missing_symbol_fixture.cpp`。

只导出无关函数，不导出 `hal_adapter_get_api_v1`。

用途：验证 `AdapterLoader` 能识别缺入口符号。

CMake 用宏把 DLL 路径传给测试：

```text
HAL_TEST_ADAPTER_FIXTURE_PATH
HAL_TEST_ADAPTER_MISSING_SYMBOL_FIXTURE_PATH
```

---

## 7. 测试文件覆盖

### 7.1 `hal_types_test.cpp`

覆盖：

- `registerHalMetaTypes()` 可调用。
- 枚举转字符串：
  - `HalStatusCode::Ok`
  - `AnalogUnit::RawCount`
  - `DigitalLevel::Unknown`
  - `SerialParity::Even`
  - `SerialStopBits::OneAndHalf`
  - `SerialFlowControl::Software`

目的：保证公共类型能注册进 Qt 元对象系统，基础字符串映射稳定。

### 7.2 `hal_error_mapper_test.cpp`

覆盖：

- Adapter timeout 映射到 `HalStatusCode::Timeout`。
- Adapter message、vendorCode、deviceId、resourceId、detail 被复制到 `HalError`。
- 未知 Adapter code 映射到 `AdapterError`。
- `makeError()` 同步填顶层 code 和嵌套 error code。

目的：保证错误模型对上稳定。

### 7.3 `resource_mapper_test.cpp`

覆盖：

- 空配置生成默认 Mock 设备和默认 6 个资源。
- 自定义配置解析设备字段、match 属性、资源绑定。
- 能力生成包含 6 个通道和模块列表。
- `safeState` 可读取。

目的：保证 `.testcfg` 风格配置能转成设备、资源、能力、安全态。

### 7.4 `safety_guard_test.cpp`

覆盖：

- 模拟量越上限时，`safeClamp=true` 会钳位到最大值。
- 模拟量越下限且 `safeClamp=false` 返回 `SafetyLimitExceeded`。
- 禁止对 input 数字资源写输出。
- 禁止写 `DigitalLevel::Unknown`。
- 串口 baudRate 非法返回 `InvalidArgument`。
- CANFD payload 超过 64 字节返回 `SafetyLimitExceeded`。

目的：保证输出类操作的基本安全边界。

### 7.5 `mock_adapter_test.cpp`

覆盖：

- 未初始化时 `enumerateDevices()` 失败。
- 初始化默认配置后可枚举设备。
- 可打开 `main_daq`。
- 模拟量写后读回。
- 数字量写后读回。
- 串口写后读出非空。
- CAN 发送后接收同一帧。

目的：保证默认 Mock 后端能支撑无硬件主流程。

### 7.6 `hal_device_test.cpp`

覆盖：

- 关闭后的设备再读 AD、写 DO、开串口均返回 `InvalidState`。
- 资源模块错配：
  - 用 analog 接口读数字资源返回 `NotSupported`。
  - 用 digital 接口读模拟资源返回 `NotSupported`。
- AD 批量读成功，返回 2 个 sample，channel 改写为逻辑资源 ID。
- close 时执行 safeState，并使 `isOpen()` 变 false。

目的：验证 `HalDevice` 的资源绑定、状态机、类型校验、批量封装和关闭语义。

### 7.7 `hal_service_test.cpp`

覆盖：

- 未初始化扫描设备返回 `NotInitialized`。
- 初始化默认配置。
- openDevice 产生日志，保留 `requestId`。
- 能通过 session 取得 `IHalDevice*`。
- AD/DA 回环：
  - 写 `DA_MAIN_0`
  - 读 `AD_MAIN_0`
- DI/DO 回环：
  - 写 `DO_POWER_EN`
  - 读 `DI_POWER_OK`
- 串口 echo：
  - open / write / read
- CANFD loopback：
  - open / send / receive
- close 后重新 open，验证安全态已生效：
  - AD 读回 0.0
  - DI 读回 Low
- 缺 session 的 `device`、`closeDevice`、`resetDevice`、`healthCheck` 返回错误。
- `createHalService()` / `destroyHalService()` 成对可用。

目的：覆盖最接近对上接口的 HAL 主流程。

### 7.8 `adapter_loader_test.cpp`

覆盖：

- 空路径、空 outApi 参数被拒绝。
- 正常 fixture DLL 可加载，`isLoaded()` 为 true，保存 `libraryPath`，返回 ABI 版本和结构体大小。
- 缺入口符号的 DLL 加载失败，并保留错误字符串。

目的：验证外部 Adapter DLL 加载契约的基本行为。

---

## 8. 覆盖矩阵

| 模块 | 测试文件 | 覆盖重点 |
| --- | --- | --- |
| `hal_types` | `hal_types_test.cpp` | 元类型注册、枚举字符串 |
| `hal_error_mapper` | `hal_error_mapper_test.cpp` | Adapter 状态码映射、错误字段 |
| `ResourceMapper` | `resource_mapper_test.cpp` | 默认配置、自定义配置、能力、安全态 |
| `SafetyGuard` | `safety_guard_test.cpp` | 模拟量钳位、数字量、串口、CAN 安全边界 |
| `MockAdapter` | `mock_adapter_test.cpp` | 初始化、打开、AD/DA、DI/DO、串口、CAN 回环 |
| `HalDevice` | `hal_device_test.cpp` | 关态、类型错配、批量、safeState |
| `HalService` | `hal_service_test.cpp` | 初始化、open、session、日志、主流程、安全重开 |
| `AdapterLoader` | `adapter_loader_test.cpp` | 参数校验、DLL 加载、缺符号 |

---

## 9. 当前测试规模

当前 CTest 自动发现 22 个用例：

```text
AdapterLoaderTest        3
HalDeviceTest            3
HalErrorMapperTest       3
HalServiceTest           4
HalTypesTest             1
MockAdapterTest          1
ResourceMapperTest       4
SafetyGuardTest          3
```

Debug 最近一次结果：

```text
100% tests passed
0 tests failed out of 22
```

---

## 10. 测试边界

当前测试允许：

- 使用 HAL 内部类。
- 使用 Mock 后端。
- 使用最小 DLL fixture。
- 使用 `QVariantMap` 构造配置。
- 通过 CTest 统一运行。

当前测试禁止或未涉及：

- UI 控件。
- 业务调度。
- 算法判定。
- 真实硬件。
- 厂家 SDK。
- 线程并发压力。
- 长时间 IO 超时。

---

## 11. 仍需补测

高优先级：

- `CAbiAdapter` 真实 ABI 桥接完成后补函数指针缺失、缓冲区不足、ABI mismatch。
- `AdapterLoader` 补入口返回失败、ABI 版本错误、`structSize` 过小、无效 DLL 路径。
- `HalDevice` 补 backend 为空的所有 IO 错误分支。
- `HalDevice` 补串口 `transactSerial` 读失败、写失败分支。
- `HalDevice` 补 `flushSerial()`、`setCanFilters()`、`receiveCanBatch()` 日志策略。
- `HalService` 补 `queryCapabilities()` 未初始化、设备不存在、正常返回。
- `MockAdapter` 补关闭 session 后操作失败、reset 默认状态、CAN 空队列 timeout、batch 读取。
- 安全校验补普通 CAN 8 字节限制、串口 dataBits 上下界、模拟量默认 range 属性。

中优先级：

- 日志失败路径字段完整性：operation、deviceId、resourceId、requestId、status。
- 多设备、多资源、多物理通道配置。
- 资源引用不存在设备时回落首设备的行为。
- `safeState` 对串口和 CAN close 的覆盖。
- Mock 配置项：
  - `analogLoopback=false`
  - `digitalLoopback=false`
  - `serialEcho=false`
  - `canLoopback=false`
  - `analogNoiseAmplitude`

低优先级：

- `toString()` 所有枚举值。
- Qt signal 兼容信号 `logMessage`。
- 重复初始化、重复 shutdown。
- 空 batch 行为。

---

## 12. 维护规则

新增 HAL 公共接口时：

1. 先更新 `../contracts/hal-interface-protocol.md`。
2. 补契约测试。
3. Mock 后端必须能支撑无硬件测试。
4. Debug 和 Release 至少各跑一次 HAL 测试。

新增 Adapter ABI 字段时：

1. 只在 `HalAdapterApiV1` 尾部追加。
2. 更新 ABI 文档。
3. 补 loader/ABI 兼容测试。
4. 保留旧 `structSize` 判断。

修复缺陷时：

1. 先写能复现缺陷的单测。
2. 再修实现。
3. 用 `ctest --test-dir build_vs -C Debug --output-on-failure` 验证。
