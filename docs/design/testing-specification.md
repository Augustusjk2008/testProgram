# 测试规范

> 适用项目：多产品通用硬件测试软件（Qt 5.15 / C++17 / Windows）
> 当前落地范围：HAL 层 GoogleTest / CTest。
> 本文定位：测试目录、分层边界、用例范围、运行方式。

---

## 1. 构建目录

- `build_vs/`：CMake + Visual Studio 17 2022 x64 生成目录。当前 HAL 测试在此构建和运行。

推荐命令：

```powershell
cmake -S . -B build_vs -G "Visual Studio 17 2022" -A x64
cmake --build build_vs --config Debug --target hwtest_hal_tests
ctest --test-dir build_vs -C Debug --output-on-failure
```

---

## 2. 测试框架

- C++ 单元测试使用 GoogleTest。
- CMake 负责拉取、构建和注册测试。
- CTest 负责统一执行测试。

不用 qmake 的原因：

- 仓库事实来源已指定根入口为 CMake。
- HAL 已作为 CMake Qt Core 库构建。
- GoogleTest、CTest、FetchContent、CI 集成更直接。
- qmake 更适合旧式 Qt 工程，不适合作为多模块测试主入口。

---

## 3. 测试目录

推荐结构：

```text
tests/
  ui/
  business/
  algorithms/
  hal/
  adapters/
  integration/
```

命名规则：

- 测试文件：`*_test.cpp`。
- 测试目标：`<module>_tests`。
- 测试名：`对象_行为_结果` 或 `ClassName.BehaviorResult`。
- 测试数据优先内联小样本；大样本放 `tests/data/`。

---

## 4. 当前 HAL 测试范围

已覆盖：

- `HalErrorMapper`：Adapter 状态码到 `HalStatusCode` 映射、未知错误、错误字段。
- `ResourceMapper`：默认 Mock 资源、自定义设备和资源、能力、安全状态配置。
- `SafetyGuard`：模拟量安全钳位、禁止越界输出、数字量/串口/CANFD 参数校验。
- `HalService`：未初始化错误、初始化、扫描、打开、日志 `requestId`、AD/DA 回环、DI/DO 回环、串口 echo、CANFD loopback、关闭和重开安全状态。

验收命令：

```powershell
ctest --test-dir build_vs -C Debug --output-on-failure
ctest --test-dir build_vs -C Release --output-on-failure
```

---

## 5. HAL 后续补测

优先补：

- `HalDevice`：各接口空指针、会话失效、能力不匹配。
- `MockAdapter`：超时、错误码注入、噪声、批量读写边界。
- `CAbiAdapter`：函数指针缺失、缓冲区不足、ABI 版本不匹配。
- `AdapterLoader`：DLL 加载失败、符号缺失、路径错误。
- 参数归一化：模拟量单位/量程、串口参数、CANFD DLC 和 payload。
- 生命周期：重复初始化、重复关闭、异常 shutdown、设备断开。
- 日志：失败路径也必须携带 operation、resourceId、deviceId、requestId。

真实硬件测试单独标记为 `hardware`，默认 CI 不运行。

---

## 6. 五层测试边界

单元测试只测本层职责，不跨层验证实现细节。

| 层级 | 单元测试允许依赖 | 禁止 |
| --- | --- | --- |
| UI | ViewModel、业务接口假对象、日志假对象 | HAL、Adapter、厂家 SDK |
| 业务调度 | 算法假对象、HAL 假对象、配置样本 | UI 控件、Adapter、厂家 SDK |
| 核心算法 | `IHalService` / `IHalDevice` 假对象、测试上下文 | UI、业务流程实现、厂家 SDK |
| HAL | Mock Adapter、C ABI 假对象、资源配置 | UI、业务判定、测试项结论 |
| Adapter | 厂家 SDK 假对象或仿真 DLL、ABI Host 假对象 | UI、业务、算法判定 |

跨层测试只允许两类：

- 契约测试：相邻层公共接口、错误码、日志字段、配置字段。
- 集成测试：业务 -> 算法 -> HAL -> Mock Adapter 的主流程。

端到端测试默认使用 Mock Adapter；真实硬件端到端测试必须独立开关、独立机器、独立报告。

---

## 7. 测试准入

新增或修改接口时：

- 公共头文件变化，同步协议文档和契约测试。
- Adapter ABI 变化，同步 `hal_adapter_abi.h`、协议文档和 ABI 兼容测试。
- 新增错误码，同步错误映射测试。
- 新增资源类型，同步资源映射、安全校验、Mock Adapter 测试。
- 修复缺陷，先补回归测试，再修实现。

提交前最低要求：

- 相关单元测试通过。
- Debug 和 Release 至少各跑一次 HAL 测试。
- 无真实硬件环境时，Mock 主流程必须通过。
