# 多产品通用硬件测试软件五层架构设计

> 技术栈：Qt 5.15 / C++ / Qt Widgets / Windows 7+  
> 架构目标：通过配置文件驱动测试流程，用一套软件适配多种产品和多种板卡，降低 UI、业务、测试算法、硬件适配之间的耦合。  
> 当前测试范围：AD、DA、DI、DO、串口、CANFD。  
> 日志策略：日志模块是五层旁路的独立基础模块，各层通过 signal 生产日志事件。

---

## 1. 总体架构

软件采用五层架构，日志模块作为旁路独立模块：

```text
┌─────────────────────────────────────────────┐
│ 1. UI 交互层                                │
│    Qt Widgets 页面、配置编辑、测试启停、日志展示 │
└───────────────────────┬─────────────────────┘
                        │ UI 命令 / 展示模型
┌───────────────────────▼─────────────────────┐
│ 2. 业务调度层                                │
│    配置加载、流程编排、任务状态、日志报告、权限 │
└───────────────────────┬─────────────────────┘
                        │ TestPlan / TestContext
┌───────────────────────▼─────────────────────┐
│ 3. 核心测试算法层                            │
│    AD/DA/DI/DO、串口、CANFD 测试逻辑          │
└───────────────────────┬─────────────────────┘
                        │ IHalService / IHalDevice
┌───────────────────────▼─────────────────────┐
│ 4. HAL 硬件抽象层                            │
│    逻辑资源映射、统一错误、参数归一化、安全校验 │
└───────────────────────┬─────────────────────┘
                        │ HAL Adapter ABI / HardwareAdapter
┌───────────────────────▼─────────────────────┐
│ 5. 硬件适配器层 Adapter                      │
│    调用厂家 DLL/lib/SDK/Win32 API，屏蔽厂家差异 │
└─────────────────────────────────────────────┘

五层均可通过 signal 旁路输出：
UI / 业务 / 算法 / HAL / Adapter ──emit log event──▶ 独立日志模块 LogService
```

边界说明：

- 前四层是主程序核心模块。
- 第五层 Adapter 是本软件需要开发和维护的最下层。
- 厂家 DLL、lib、SDK、内核驱动、C 例程是 Adapter 的外部依赖，不作为本软件内部架构层。
- 测试算法层不包含厂家 SDK 头文件，不直接调用厂家函数。
- 更换板卡厂家时，原则上只新增或替换 Adapter，HAL 以上层不改或少改。

---

## 2. 架构设计目标

### 2.1 配置驱动

测试流程、产品型号、硬件资源、测试参数、判定阈值都由 `.testcfg` 配置文件描述。

目标：

- 切换产品时优先改配置，不改代码。
- UI 可视化编辑配置文件。
- 业务调度层根据配置生成测试计划。
- 测试算法层根据配置参数执行具体测试。
- HAL 根据配置把逻辑资源映射到真实板卡通道。

### 2.2 分层解耦

每层只依赖下一层公开接口，不越层调用。

| 层级 | 可以依赖 | 不允许依赖 |
| --- | --- | --- |
| UI 交互层 | 业务调度层的 ViewModel / Service | HAL、Adapter、厂家 SDK |
| 业务调度层 | 测试算法层、配置模型、报告服务、日志事件接口 | Adapter、厂家 SDK |
| 测试算法层 | HAL 对上接口 | UI、厂家 SDK |
| HAL 层 | Adapter 接口、资源配置 | UI、具体厂家业务逻辑 |
| Adapter 层 | 厂家 DLL/lib/SDK/Win32 API | UI、业务调度、测试判定 |

### 2.3 可扩展

后续增加新板卡、新产品、新测试项时，应遵循：

- 新产品：新增 `.testcfg`。
- 新板卡：新增 Adapter。
- 新测试算法：新增测试算法类，不改 HAL 基础模型。
- 新 UI 页面：通过业务层服务访问数据，不直接访问 HAL。

### 2.4 工业稳定性

- 硬件调用必须带超时。
- 所有错误统一映射为系统错误码。
- 关键操作写入日志，可追踪到产品、工位、测试项、硬件资源。
- 输出类操作必须做安全范围校验。
- 低配 Win7 机器上优先保证稳定、简洁、可维护。

---

## 3. 第一层：UI 交互层

### 3.1 职责

UI 交互层负责人与软件的直接交互，包括：

- 登录和权限入口。
- 产品方案选择。
- 测试配置编辑。
- 测试启动、停止、暂停、继续。
- 测试进度展示。
- 实时日志展示。
- 测试结果和报告查看。
- 简单曲线或表格展示。

### 3.2 技术选择

- 使用 Qt Widgets。
- 页面轻量化，避免复杂动画和高负载绘制。
- UI 只处理展示和用户操作，不写硬件业务逻辑。

### 3.3 UI 主要模块

```text
ui/
  main_window
  pages/
    product_select_page
    test_config_page
    test_run_page
    log_page
    report_page
  widgets/
    status_panel
    test_item_table
    log_view
    result_summary
```

### 3.4 UI 与业务层交互

UI 不直接调用测试算法和 HAL，而是通过业务层提供的服务：

```text
用户点击“开始测试”
  -> UI 调用 TestRunService::start(planId)
  -> 业务调度层加载配置并启动流程
  -> UI 订阅测试状态、日志和结果事件
```

典型 UI 命令：

- `loadProductConfig(filePath)`
- `saveProductConfig(config)`
- `startTest(planId)`
- `stopTest()`
- `exportReport(reportId)`

典型 UI 展示模型：

- 当前产品信息。
- 测试项列表。
- 每个测试项状态：等待、执行中、通过、失败、跳过。
- 日志列表。
- 总体结果。

---

## 4. 第二层：业务调度层

### 4.1 职责

业务调度层是整套软件的流程控制中心，负责：

- 配置文件加载和保存。
- 配置合法性校验。
- 测试计划生成。
- 测试流程执行。
- 测试状态管理。
- 失败重试策略。
- 断点续测状态记录。
- 向独立日志模块发送流程日志事件。
- CSV 数据记录。
- 简易测试报告生成。
- 权限控制。

### 4.2 核心对象

```text
TestConfigManager      配置加载、保存、校验
TestPlanBuilder        根据配置生成测试计划
TestRunService         测试启动、停止、状态管理
TestFlowEngine         按顺序执行测试项
TestContext            一次测试运行的上下文
TestResultCollector    收集测试结果
ReportService          生成报告
PermissionService      权限判断
```

### 4.3 测试计划模型

业务层从 `.testcfg` 中生成标准测试计划：

```cpp
struct TestPlan {
    QString planId;
    QString productModel;
    QString configVersion;
    QVector<TestStep> steps;
};

struct TestStep {
    QString stepId;
    QString name;
    QString algorithmId;
    QVariantMap parameters;
    int timeoutMs;
    int retryCount;
    bool enabled;
};
```

### 4.4 执行流程

```text
加载 .testcfg
  -> 校验配置
  -> 生成 TestPlan
  -> 初始化 HAL
  -> 打开所需设备
  -> 按 TestStep 顺序调用测试算法
  -> 收集每项测试结果
  -> emit 流程日志事件 / 写 CSV
  -> 生成报告
  -> 关闭设备 / 进入安全状态
```

### 4.5 与测试算法层交互

业务层不关心具体硬件细节，只通过算法 ID 找到对应测试算法：

```text
TestStep.algorithmId = "analog.loopback"
  -> TestAlgorithmRegistry 创建 AnalogLoopbackTest
  -> algorithm.run(context, parameters)
  -> 返回 TestResult
```

---

## 5. 第三层：核心测试算法层

### 5.1 职责

测试算法层负责具体测试项逻辑，包括：

- AD 采集测试。
- DA 输出测试。
- AD/DA 回环测试。
- DI 输入测试。
- DO 输出测试。
- DI/DO 回环测试。
- 串口收发测试。
- CANFD 收发测试。
- 阈值判断。
- 重试时机判断。
- 测试数据整理。

### 5.2 算法层原则

- 只调用 HAL 对上接口。
- 不包含厂家 SDK 头文件。
- 不关心真实板卡型号。
- 不直接操作 UI。
- 不直接写报告文件，只返回结构化测试结果。

### 5.3 测试算法接口

建议统一测试算法接口：

```cpp
class ITestAlgorithm {
public:
    virtual ~ITestAlgorithm() = default;

    virtual QString id() const = 0;
    virtual QString name() const = 0;

    virtual TestResult run(TestContext& context,
                           const QVariantMap& parameters) = 0;
};
```

`TestContext` 由业务调度层创建，内部持有：

- `IHalService* hal`
- 当前产品信息
- 当前测试项 ID
- requestId
- 日志入口
- 数据记录入口

### 5.4 典型算法示例：AD/DA 回环

```text
读取参数：DA 逻辑资源、AD 逻辑资源、输出值、容差
  -> 调用 HAL writeDa(DA_MAIN_0, 2.5V)
  -> 调用 HAL readAd(AD_MAIN_0)
  -> 比较采样值和期望值
  -> 返回 PASS/FAIL 和测量数据
```

### 5.5 测试结果模型

```cpp
enum class TestVerdict {
    Pass,
    Fail,
    Error,
    Skipped
};

struct TestMeasurement {
    QString name;
    QVariant expected;
    QVariant actual;
    QVariant tolerance;
    QString unit;
};

struct TestResult {
    QString stepId;
    QString algorithmId;
    TestVerdict verdict;
    QString message;
    QVector<TestMeasurement> measurements;
    QVariantMap rawData;
    qint64 startTimeUs;
    qint64 endTimeUs;
};
```

---

## 6. 第四层：HAL 硬件抽象层

### 6.1 职责

HAL 层负责把测试算法中的逻辑硬件资源转换为统一硬件操作。

主要职责：

- 设备扫描。
- 设备打开、关闭、复位、健康检查。
- 逻辑资源映射。
- 参数归一化。
- 输出安全校验。
- 统一错误码。
- 统一时间戳。
- 统一日志上下文。
- 调用 Adapter 接口。

### 6.2 当前 HAL 能力范围

HAL 对上提供以下功能接口：

- `IAnalogIo`：AD/DA。
- `IDigitalIo`：DI/DO。
- `ISerialBus`：串口。
- `ICanFdBus`：CANFD。

顶层入口：

- `IHalService`
- `IHalDevice`

详细接口以 `docs/hal-interface-protocol.md` 为准。

### 6.3 逻辑资源映射

测试算法只使用逻辑资源 ID，例如：

```text
AD_MAIN_0
DA_MAIN_0
DI_POWER_OK
DO_POWER_EN
SERIAL_A
CANFD_A
```

HAL 根据配置把逻辑资源映射到真实硬件：

```text
ResourceId -> DeviceSession -> AdapterDeviceHandle -> physicalIndex -> Adapter API
```

示例配置：

```json
{
  "hardware": {
    "devices": [
      {
        "alias": "main_daq",
        "adapterId": "acme.daq.v1",
        "match": {"serialNumber": "DAQ-001"}
      }
    ],
    "resources": {
      "AD_MAIN_0": {"device": "main_daq", "module": "analog", "direction": "input", "physicalIndex": 0},
      "DA_MAIN_0": {"device": "main_daq", "module": "analog", "direction": "output", "physicalIndex": 0},
      "SERIAL_A": {"device": "main_daq", "module": "serial", "physicalIndex": 0},
      "CANFD_A": {"device": "main_daq", "module": "canfd", "physicalIndex": 0}
    }
  }
}
```

### 6.4 HAL 对上调用示例

```cpp
auto deviceResult = hal->device(sessionId);
if (!deviceResult.ok()) {
    return TestResult::error(deviceResult.status.error.message);
}

auto analog = deviceResult.value->analogIo();
analog->writeDa("DA_MAIN_0", 2.5, writeOptions);
auto sample = analog->readAd("AD_MAIN_0", readOptions);
```

### 6.5 HAL 错误模型

HAL 把 Adapter 和厂家 SDK 错误统一转换为 `HalStatusCode`：

- `Ok`
- `InvalidArgument`
- `InvalidState`
- `NotInitialized`
- `NotFound`
- `NotSupported`
- `Busy`
- `Timeout`
- `SafetyLimitExceeded`
- `DeviceDisconnected`
- `AdapterLoadFailed`
- `AdapterSymbolMissing`
- `AdapterError`
- `IoError`
- `ProtocolError`
- `BufferTooSmall`
- `InternalError`

错误信息必须包含：

- 操作名。
- 设备 ID。
- 逻辑资源 ID。
- Adapter 错误码。
- 厂家原始错误码。
- 关键参数上下文。

---

## 7. 第五层：硬件适配器层 Adapter

### 7.1 职责

Adapter 是本软件实际开发的最下层，用于屏蔽不同厂家硬件 API 差异。

Adapter 负责：

- 加载或链接厂家 DLL/lib/SDK。
- 按厂家例程完成初始化、打开设备、读写通道、关闭设备。
- 把厂家句柄封装为 `HalAdapterDeviceHandle`。
- 把厂家错误码转换为 Adapter 错误码。
- 把厂家数据结构转换为 HAL Adapter ABI 数据结构。
- 向 HAL 提供设备能力描述。

### 7.2 Adapter 与厂家 SDK 的关系

厂家提供的 C 例程不作为软件运行时模块直接使用，但它非常重要：

- 用来确认厂家 API 的调用顺序。
- 用来确认初始化和关闭流程。
- 用来确认通道编号、参数单位和返回值含义。
- 用来确认错误码处理方式。
- 用来验证 Adapter 封装是否正确。

关系如下：

```text
HAL
  -> HardwareAdapter / HalAdapterApiV1
  -> AcmeDaqAdapter
  -> Vendor_Open / Vendor_ReadAd / Vendor_WriteDo / Vendor_Close
  -> 厂家 DLL/lib/SDK/驱动
```

### 7.3 Adapter 形态

Adapter 支持两种实现形态：

| 形态 | 说明 | 适用情况 |
| --- | --- | --- |
| 内置 C++ Adapter | 与主程序一起编译 | 一期单厂家、快速开发 |
| 外部 Adapter DLL | 按 `HalAdapterApiV1` 导出 C ABI | 多厂家、独立替换、编译器 ABI 风险高 |

一期可以先采用内置 C++ Adapter，但接口语义保持和 `HalAdapterApiV1` 一致，后续需要拆成 DLL 时成本最低。

### 7.4 外部 Adapter ABI

外部 Adapter DLL 导出函数：

```cpp
extern "C" __declspec(dllexport)
int hal_adapter_get_api_v1(const HalAdapterHostApiV1* host,
                           HalAdapterApiV1* outApi);
```

HAL 加载 Adapter 后，通过函数表调用：

- `initialize`
- `shutdown`
- `enumerateDevices`
- `openDevice`
- `closeDevice`
- `resetDevice`
- `getCapabilities`
- `analogRead / analogWrite / analogConfigure`
- `digitalRead / digitalWrite / digitalWaitEdge`
- `serialOpen / serialClose / serialWrite / serialRead`
- `canOpen / canClose / canSetFilters / canSend / canReceive`

### 7.5 Mock Adapter

必须提供 Mock Adapter，保证没有真实硬件时也能开发 UI、业务层和测试算法。

Mock Adapter 支持：

- AD 模拟采样。
- DA 输出记录。
- AD/DA 回环。
- DI/DO 回环。
- 串口 echo。
- CANFD loopback。
- 可配置超时。
- 可配置错误码。
- 可配置随机噪声。

---

## 8. 配置文件架构

### 8.1 配置文件职责

`.testcfg` 是产品测试方案的核心输入，负责描述：

- 产品信息。
- 硬件设备匹配规则。
- 逻辑资源映射。
- 测试项启用状态。
- 测试项执行顺序。
- 测试参数。
- 判定阈值。
- 超时和重试次数。
- 安全输出限制。
- 报告字段。

### 8.2 配置文件示例

```json
{
  "schemaVersion": 1,
  "product": {
    "model": "PRODUCT_A",
    "name": "产品A"
  },
  "hardware": {
    "devices": [
      {
        "alias": "main_daq",
        "adapterId": "acme.daq.v1",
        "match": {"serialNumber": "DAQ-001"}
      }
    ],
    "resources": {
      "AD_MAIN_0": {"device": "main_daq", "module": "analog", "direction": "input", "physicalIndex": 0},
      "DA_MAIN_0": {"device": "main_daq", "module": "analog", "direction": "output", "physicalIndex": 0},
      "SERIAL_A": {"device": "main_daq", "module": "serial", "physicalIndex": 0},
      "CANFD_A": {"device": "main_daq", "module": "canfd", "physicalIndex": 0}
    }
  },
  "testFlow": [
    {
      "stepId": "analog_loopback_001",
      "name": "模拟量回环测试",
      "algorithmId": "analog.loopback",
      "enabled": true,
      "timeoutMs": 1000,
      "retryCount": 1,
      "parameters": {
        "da": "DA_MAIN_0",
        "ad": "AD_MAIN_0",
        "value": 2.5,
        "tolerance": 0.05,
        "unit": "V"
      }
    }
  ]
}
```

---

## 9. 主要运行流程

### 9.1 软件启动

```text
启动程序
  -> 初始化日志系统
  -> 加载全局设置
  -> 初始化业务服务
  -> 初始化 UI
  -> 等待用户选择产品配置
```

### 9.2 加载产品配置

```text
用户选择 .testcfg
  -> UI 请求业务层加载配置
  -> TestConfigManager 解析 JSON
  -> 校验 schemaVersion
  -> 校验硬件资源和测试步骤
  -> TestPlanBuilder 生成 TestPlan
  -> UI 展示测试项列表
```

### 9.3 执行测试

```text
用户点击开始
  -> TestRunService 创建 TestContext
  -> IHalService.initialize(halConfig)
  -> HAL 加载/创建 Adapter
  -> HAL 扫描设备并建立资源映射
  -> TestFlowEngine 按 TestPlan 顺序执行 TestStep
  -> 测试算法调用 HAL
  -> HAL 调用 Adapter
  -> Adapter 调用厂家 DLL/lib/SDK
  -> 返回测量数据和状态
  -> 业务层记录结果、日志、CSV
  -> 全部步骤结束后生成报告
```

### 9.4 停止测试

```text
用户点击停止
  -> 业务层标记停止请求
  -> 当前测试步骤返回后停止后续步骤
  -> HAL 输出通道进入安全状态
  -> 关闭设备
  -> 写入停止日志
  -> UI 更新状态
```

---

## 10. 日志事件、数据记录与报告

日志模块是独立基础模块，详细接口见 `docs/design/log-interface-protocol.md`。

五层只通过 signal 生产日志事件，不直接写日志文件、数据库或报告摘要。

### 10.1 CSV 数据记录

CSV 用于保存关键测量数据，便于后续追溯和统计。

建议字段：

```text
time, product_model, serial_no, step_id, item_name, measurement, expected, actual, tolerance, unit, verdict
```

### 10.2 简易报告

报告内容：

- 产品信息。
- 操作员。
- 工位。
- 测试开始/结束时间。
- 总体结果。
- 每个测试项结果。
- 关键测量值。
- 失败原因。
- 日志摘要。

---

## 11. 安全策略

### 11.1 输出安全校验

HAL 在执行输出类操作前必须校验：

- DA 输出值是否超出安全范围。
- DO 输出是否允许切换。
- CANFD 发送帧长度是否合法。
- CANFD 发送频率是否超出配置限制。

### 11.2 安全状态

配置中可声明安全状态：

```json
{
  "safeState": {
    "DA_MAIN_0": 0.0,
    "DO_POWER_EN": "Low",
    "CANFD_A": "closed"
  }
}
```

触发以下情况时进入安全状态：

- 测试停止。
- 测试取消。
- 设备断开。
- Adapter 返回严重 IO 错误。
- 输出参数超过安全限制。
- 应用退出。

---

## 12. 推荐源码结构

```text
src/
  app/
    main.cpp
    application.cpp

  ui/
    main_window.*
    pages/
    widgets/
    viewmodels/

  business/
    config/
      test_config_manager.*
      test_config_schema.*
    flow/
      test_plan_builder.*
      test_flow_engine.*
      test_run_service.*
      test_context.*
    logging/
      log_service.*
    report/
      report_service.*
    permission/
      permission_service.*

  algorithms/
    i_test_algorithm.h
    test_algorithm_registry.*
    analog/
      analog_loopback_test.*
    digital/
      digital_loopback_test.*
    serial/
      serial_transaction_test.*
    canfd/
      canfd_loopback_test.*

  hal/
    include/
      hal_types.h
      i_hal_service.h
      i_hal_device.h
      i_analog_io.h
      i_digital_io.h
      i_serial_bus.h
      i_canfd_bus.h
      hal_adapter_abi.h
    core/
      hal_service.*
      hal_device.*
      resource_mapper.*
      error_mapper.*
      safety_guard.*
    adapters/
      adapter_loader.*
      c_abi_adapter.*
      mock_adapter.*

  adapters/
    acme_daq_adapter/
      acme_daq_adapter.*
      vendor_api_wrapper.*
```

```text
configs/
  products/
    product_a.testcfg
    product_b.testcfg

logs/
reports/
data/
```

---

## 13. 开发分工建议

### 13.1 UI 开发

负责：

- 页面搭建。
- 配置编辑界面。
- 测试运行界面。
- 日志和结果展示。

依赖：业务层服务接口和 ViewModel。

### 13.2 业务开发

负责：

- 配置文件解析。
- 测试计划生成。
- 测试流程执行。
- 日志、CSV、报告。
- 权限和状态管理。

依赖：测试算法注册表和 HAL 服务接口。

### 13.3 测试算法开发

负责：

- AD/DA 测试算法。
- DI/DO 测试算法。
- 串口测试算法。
- CANFD 测试算法。
- 判定规则和测量数据输出。

依赖：HAL 对上接口。

### 13.4 HAL 开发

负责：

- `IHalService` 实现。
- `IHalDevice` 实现。
- 逻辑资源映射。
- 参数归一化。
- 错误映射。
- 安全校验。
- Adapter 调用封装。

依赖：Adapter ABI。

### 13.5 Adapter 开发

负责：

- 阅读厂家 C 例程。
- 封装厂家 DLL/lib/SDK。
- 实现 Adapter 接口。
- 提供设备能力描述。
- 转换厂家错误码。
- 配合 Mock Adapter 做对照测试。

依赖：厂家 SDK 和 HAL Adapter ABI。

---

## 14. 一期交付范围

一期建议交付稳定功能测试版本：

- Qt Widgets 主界面。
- 产品配置加载和保存。
- 测试流程顺序执行。
- AD/DA 测试闭环。
- DI/DO 测试闭环。
- 串口收发测试。
- CANFD 收发测试。
- HAL 对上接口。
- HAL ↔ Adapter 接口。
- 一个真实 Adapter 或 Mock Adapter。
- 日志记录。
- CSV 数据记录。
- 简易报告。
- 安全输出保护。

---

## 15. 架构验收标准

架构落地后应满足：

- UI 层不直接调用 HAL 或 Adapter。
- 测试算法层不包含厂家 SDK 头文件。
- HAL 层不包含具体测试判定逻辑。
- Adapter 层不包含 UI、业务流程、测试判定逻辑。
- 更换厂家板卡时，优先新增或替换 Adapter。
- 新增产品方案时，优先新增 `.testcfg`。
- 所有硬件调用都有超时和统一错误码。
- 输出类操作有安全范围校验。
- 没有真实硬件时，Mock Adapter 能支撑 UI、业务层和算法层联调。
