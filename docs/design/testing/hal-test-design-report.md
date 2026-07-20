# HAL 测试设计报告

> 范围：tests/hal/ 的当前测试覆盖快照。
>
> 本文不定义全局构建命令、CTest 规则、测试准入或硬件证据等级；这些唯一以 [测试规范](testing-specification.md) 为准。

## 1. 当前快照

- 测试目标：hwtest_hal_tests。
- 测试源文件：9 个；源级 GoogleTest 定义：30 个。
- CTest 通过 gtest_discover_tests 在构建后发现。30 是源级定义数，不是已执行结果；实际 CTest 条数和结果必须按 [测试规范](testing-specification.md) 完整构建后执行 ctest -N 和 ctest 确认。
- tests/CMakeLists.txt 还注册日志、BIZ、算法和应用测试；本文只描述 HAL，不能据此推断整个仓库的 CTest 范围。

HAL 测试目标链接 hwtest_hal 与 GTest::gtest_main，并构建两个最小 DLL fixture：hal_adapter_fixture 和 hal_adapter_missing_symbol_fixture。fixture 仅用于 AdapterLoaderTest，不是厂家 Adapter 或真实硬件。

## 2. 测试支撑

tests/hal/test_support.h 提供默认 Mock 配置：一个 main_daq 设备和六个逻辑资源。

| 资源 | 模块与方向 | 用途 |
| --- | --- | --- |
| AD_MAIN_0 | analog / input | Mock AD 回读 |
| DA_MAIN_0 | analog / output | Mock DA 写入和安全态 |
| DI_POWER_OK | digital / input | Mock DI 回读 |
| DO_POWER_EN | digital / output | Mock DO 写入和安全态 |
| SERIAL_A | serial / bidirectional | 内存串口 buffer |
| CANFD_A | canfd / bidirectional | 内存 CAN 接收队列 |

safeStateHalConfig() 为 DA_MAIN_0 设置 0.0，为 DO_POWER_EN 设置 Low。所有回环均是 Mock 行为。

## 3. 覆盖快照

| 测试源文件 | 源级定义数 | 当前覆盖 |
| --- | ---: | --- |
| adapter_loader_test.cpp | 3 | 空参数、最小 fixture DLL、缺入口符号 |
| control_channel_test.cpp | 7 | 显式 providerId、未知 Provider、串口/UDP 配置、UDP 原始回环、远端来源过滤、timeout 和关闭态 |
| hal_device_test.cpp | 4 | 关闭态、资源类型错配、批量包装、关闭流程和控制资源类型校验 |
| hal_error_mapper_test.cpp | 3 | Adapter 状态映射和错误上下文 |
| hal_service_test.cpp | 4 | 初始化、会话、日志 requestId、Mock IO 回环、重开后的安全态 |
| hal_types_test.cpp | 1 | 元类型注册和枚举字符串 |
| mock_adapter_test.cpp | 1 | 初始化与 AD/DA、DI/DO、串口、CAN 基础回环 |
| resource_mapper_test.cpp | 4 | 默认设备、资源、能力和 safeState |
| safety_guard_test.cpp | 3 | 模拟量钳位、数字量、串口和 CANFD 输入边界 |

在对应 CTest 实际通过后，当前 HAL 覆盖可证明 HAL 公共接口、内存 Mock 和本机 Qt UDP Provider 的基础行为；源级用例存在本身不能证明通过，也不能证明真实串口、真实网口、厂家 SDK、厂商 Adapter 或真实目标板的行为。

## 4. 证据边界

- 默认 HalService 后端经 CAbiAdapter 使用 MockAdapter。CAbiAdapter 当前不是实际的 C ABI 厂家桥接。
- 串口 echo 是 MockAdapter 内存 buffer 的读写；CANFD loopback 是内存队列。另有 `qt.udp` 的真实 `QUdpSocket` 本机回环，但它不是物理网口或 DUT 证据。
- AdapterLoader 的 DLL fixture 只验证加载器最小契约，不能证明 Vendor Adapter 集成。
- `SYSTEM_STATUS` 的 Qt UDP 成功/超时/坏 CRC 跨层闭环位于算法测试目标；本报告只把它引用为 Provider 集成证据，不重复其断言。
- Qt 串口 Provider 已编译并覆盖配置拒绝路径，但未在真实或虚拟串口上联调。TCP、Vendor Adapter、真实硬件测试和 CTest hardware label 均未实现。

## 5. 当前缺口

下列均为未实现或未验证项，不能作为当前交付能力：

- CAbiAdapter 接入真实函数表、厂家 DLL 和 SDK 的 ABI 兼容验证。
- 可配置的 Mock 超时和错误码注入，以及相应失败日志覆盖。
- HalDevice 的空 backend、串口 transact 读写失败、flushSerial、setCanFilters、receiveCanBatch 和失败日志分支。
- 多设备、多资源、物理通道、safeState 串口/CAN 关闭和并发/长时 IO 验证。
- SYSTEM_STATUS -> IControlChannel -> HAL Mock Provider 的正向事务闭环。
- Qt 串口实机、TCP、Vendor Adapter 和真实硬件五级证据中的更高级别测试。

新增 HAL 公共接口、ABI、资源、安全规则或 Mock 行为时，按 [测试规范](testing-specification.md) 的全局准入同步契约、回归测试和证据等级。
