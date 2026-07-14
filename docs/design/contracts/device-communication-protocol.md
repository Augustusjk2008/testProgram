# 测试设备与被测件底层通讯协议设计

> 适用项目：多产品通用硬件测试软件（Qt 5.15 / C++17 / Windows）
> 本文定位：测试软件运行过程中，测试设备与被测件之间的底层通讯协议定义、CSV 建模格式、字段布局和校验规则。
> 采纳来源：`H:/WorkSpace/PythonWorkspace/openEulerEnvironment/docs/protocol_modeling_workbench_definition.md` 中的协议定义、CSV 存储格式、字段定义、字段类型、命名表达式、布局规则和校验规则。
> 建议来源：同源文档中的 C++ 代码生成、Schema 生成和导出物规则只作为工程实现建议，不构成本项目当前接口强制面。

---

## 1. 边界与关系

本文定义的“底层通讯协议”用于描述测试设备与被测件之间传输的原始帧结构。协议帧可承载在串口、CANFD 或后续新增总线之上，但协议字段语义独立于具体硬件厂家 SDK。

在五层架构中的位置：

```text
核心测试算法层
  -> 根据协议 CSV 构造命令帧、解析响应帧、执行字段级校验
  -> IHalService / ISerialBus / ICanFdBus
  -> HAL 负责总线资源、超时、错误映射和原始字节收发
  -> Adapter
  -> 测试设备
  -> 被测件
```

职责边界：

- 协议 CSV 定义帧结构、字段类型、字节布局、位布局和有效字段。
- 核心测试算法层负责选择协议、填充业务字段、检查响应字段和判定测试结果。
- HAL 负责串口、CANFD 等传输通道的资源映射、参数归一化、超时、重试、日志和错误映射。
- Adapter 只封装厂家库或系统 API，不解释本协议字段业务含义。
- 协议解析失败、帧长度不匹配、字段值不符合协议约束时，应在调用链中记录为协议类错误；进入 HAL 错误面时映射为 `HalStatusCode::ProtocolError`。

与现有事实源关系：

| 文档 | 关系 |
| --- | --- |
| `overview/five-layer-architecture.md` | 定义协议所在层级和跨层依赖 |
| `contracts/business-scheduling-layer.md` | 定义 `ProtocolProfile`、`ExchangeAction`、`IFrameBuilder` 和 `IFrameParser` |
| `contracts/hal-interface-protocol.md` | 定义原始字节或 CANFD 帧如何通过 HAL 发送和接收 |
| `contracts/device-communication-protocol.md` | 定义测试设备与被测件之间的协议帧建模规则 |
| `testing/testing-specification.md` | 定义协议契约测试和集成测试边界 |

---

## 2. 协议资产模型

一个协议等价于一个可保存、可审查、可生成辅助代码的帧结构定义。

| 定义项 | 规则 |
| --- | --- |
| 协议资产文件 | 一个 `.csv` 文件描述一个帧结构 |
| 协议资产目录 | 由产品配置或工程配置指定，保存协议 CSV 和可选生成物 |
| 协议配置入口 | 产品配置中的 `ProtocolProfile` 仅引用协议 CSV；算法层读取并执行该引用 |
| 协议文件名 | 只允许普通文件名，不允许携带路径 |
| 帧名 | CSV 文件名去掉 `.csv` 后得到的名称 |
| 字段列表 | CSV 行顺序排列的一组字段定义 |
| 原始布局 | 由字段行顺序、字段类型、长度和位字段分组推导 |
| 有效字段 | 由 `is_valid` 决定是否参与运行期打包、解析和字段 Schema |

运行期约定：

- 每个命令帧、响应帧、事件帧应分别建模为独立 CSV。
- 产品测试配置引用协议资产时，应使用稳定的协议文件名或帧名，不使用 CSV 的显示序号作为字段 ID。
- 测试算法不得在代码中手写与 CSV 冲突的字节偏移和位偏移。
- 协议 CSV 是字段布局的事实源；辅助 C++ 类型、Schema 或文档导出物均应由 CSV 派生或人工保持一致。

---

## 3. CSV 存储格式

协议 CSV 使用 UTF-8 with BOM 读写，列定义固定为：

```text
index,length,type,name_cn,name_en,lsb,default,is_valid
```

| CSV 列 | 字段属性 | 说明 |
| --- | --- | --- |
| `index` | `FieldSpec.index` 或字节序显示 | 保存时可写入自动计算的字节序，例如 `B1`、`B1-4`；加载时若无法解析为数字，则按行号生成逻辑序号 |
| `length` | `FieldSpec.length` | 普通字段表示字节数，`BIT` 表示位数，`RESERVED` 表示预留字节数 |
| `type` | `FieldSpec.field_type` | 字段类型，必须在支持类型列表中 |
| `name_cn` | `FieldSpec.name_cn` | 中文说明，用于界面、导出文档、注释和展示 Schema |
| `name_en` | `FieldSpec.name_en` | 英文建模名，用于运行期字段访问、数组、结构数组和位字段分组 |
| `lsb` | `FieldSpec.lsb` | 定标系数，主要用于 `U8F/S8F/U16F/S16F/U32F/S32F` |
| `default` | `FieldSpec.default` | 默认值字符串，运行期或生成代码可按整数或浮点解析 |
| `is_valid` | `FieldSpec.is_valid` | 是否参与运行期解析、打包和 Schema；`BIT` 字段强制视为有效 |

读取规则：

- 空行忽略。
- 额外列忽略。
- 列缺失按无效协议处理，保存、导入或运行前校验必须失败。
- `is_valid` 支持 `1/true/yes/y/是` 和 `0/false/no/n/否` 等布尔文本。
- `RESERVED` 字段保存时应清空 `lsb` 和 `default`。

---

## 4. 字段定义

字段是协议的最小建模单元，对应如下结构：

```text
FieldSpec(
    index: int,
    length: int,
    field_type: str,
    name_cn: str,
    name_en: str,
    lsb: Optional[float],
    default: Optional[str],
    is_valid: bool,
)
```

| 属性 | 必填 | 定义 |
| --- | --- | --- |
| `index` | 是 | 逻辑行序号。界面或 CSV 中显示的 `B1`、`B1-4` 不应被业务当作稳定字段 ID |
| `length` | 是 | 字段长度。普通定长类型可填 `0` 表示使用类型默认长度；`BIT` 和 `RESERVED` 必须大于 `0` |
| `field_type` | 是 | 类型编码，例如 `U16`、`F32`、`BIT`、`RESERVED` |
| `name_cn` | 否 | 中文说明，优先用于文档和 Schema 展示；为空时回退到 `name_en` |
| `name_en` | 是 | 英文字段名或表达式，必须满足本文命名规则 |
| `lsb` | 否 | 定标系数。定标整数类型填写后必须为正数；为空时按 `1.0` 处理 |
| `default` | 否 | 默认值。能解析为整数或浮点数时可用于帧默认构造或命令初值 |
| `is_valid` | 是 | 有效字段参与运行期帧大小、打包、解析和 Schema；无效普通字段只作为建模备注保留 |

---

## 5. 字段类型

| 类型 | 名称 | 长度 | 运行期值类型 | 说明 |
| --- | --- | --- | --- | --- |
| `CONST` | 定值字节 | 1 字节 | `UInt8` | 可配置默认值；运行期建议校验接收值是否等于期望常量 |
| `ANY` | 不定字节 | 1 字节 | `UInt8` | 字节占位，不做固定值约束 |
| `U8` | 无符号 1 字节 | 1 字节 | `UInt8` | 无符号整数 |
| `S8` | 有符号 1 字节 | 1 字节 | `Int8` | 有符号整数 |
| `U16` | 无符号 2 字节 | 2 字节 | `UInt16` | 小端读写 |
| `S16` | 有符号 2 字节 | 2 字节 | `Int16` | 小端读写 |
| `U32` | 无符号 4 字节 | 4 字节 | `UInt32` | 小端读写 |
| `S32` | 有符号 4 字节 | 4 字节 | `Int32` | 小端读写 |
| `U8F` | 无符号 1 字节定标 | 1 字节 | `Float64` | 原始无符号整数乘以 `lsb` 得到物理量 |
| `S8F` | 有符号 1 字节定标 | 1 字节 | `Float64` | 原始有符号整数乘以 `lsb` 得到物理量 |
| `U16F` | 无符号 2 字节定标 | 2 字节 | `Float64` | 小端读写，带定标 |
| `S16F` | 有符号 2 字节定标 | 2 字节 | `Float64` | 小端读写，带定标 |
| `U32F` | 无符号 4 字节定标 | 4 字节 | `Float64` | 小端读写，带定标 |
| `S32F` | 有符号 4 字节定标 | 4 字节 | `Float64` | 小端读写，带定标 |
| `F32` | 单精度浮点 | 4 字节 | `Float32` | 以 IEEE 754 原始位模式小端读写 |
| `F64` | 双精度浮点 | 8 字节 | `Float64` | 以 IEEE 754 原始位模式小端读写 |
| `BIT` | 位字段 | `length` 位 | 位字段 | 连续 `BIT` 行打包到 8/16/32 位容器 |
| `RESERVED` | 预留字节 | `length` 字节 | 无业务值 | 原始帧占位，不进入业务字段 Schema |

所有多字节整数和浮点字段默认按小端序读写。若某个外部设备必须使用其他字节序，应在协议资产层新增显式字段约束，并同步更新本文和相关解析实现。

---

## 6. 英文名表达式

`name_en` 是运行期字段访问、辅助代码生成和 Schema 生成的关键标识。

| 形式 | 示例 | 含义 |
| --- | --- | --- |
| 标量字段 | `temperature` | 普通字段 |
| 位字段分组 | `flags.ready` | `BIT` 字段使用点号前缀作为位组名，点号后为位域名 |
| 无分组位字段 | `ready` | `BIT` 字段未写分组时自动归入 `bitGroupN` |
| 标量数组 | `data[0]`、`data[1]` | 显式数组字段 |
| 结构数组 | `points[0].x`、`points[0].y` | 结构数组字段 |

命名约束：

- 普通标识符必须匹配 `^[A-Za-z_][A-Za-z0-9_]*$`。
- 数组下标必须是从 `0` 开始的非负整数。
- 数组下标必须连续，不允许缺口。
- 结构数组字段名必须是合法标识符。
- 非数组字段最多允许一个点号。
- 非 `BIT` 字段中的点号不表示嵌套结构，运行期访问名应归一化为下划线。
- 归一化后的字段名不得重复。
- 位字段组名、数组基名、预留字段名和普通标量名不得在顶层发生类别冲突。

建模要求：

- 数组必须使用显式下标，例如 `data[0]`、`data[1]`，不得依赖连续同名字段推断数组。
- 位字段应使用稳定分组前缀，例如 `status.ready`、`status.mode`。
- `name_en` 一旦进入产品测试配置或报告字段，应视为兼容字段，重命名需同步迁移配置和报告解析。

---

## 7. 布局规则

### 7.1 字节布局

协议按字段行顺序计算原始字节序：

- 普通定长类型使用类型默认字节数。
- 普通字段 `length=0` 时使用类型默认字节数。
- `RESERVED` 使用 `length` 字节。
- 连续 `BIT` 字段先组成位字段组，再按容器大小占用 1、2 或 4 字节。
- 字节序可显示为 `B1`、`B1-4` 等 1 基位置。

### 7.2 位字段布局

连续的 `BIT` 行组成一个位字段 run：

- 位组名优先取第一行 `name_en` 的点号前缀，例如 `flags.ready` 得到 `flags`。
- 没有点号前缀时自动生成 `bitGroup1`、`bitGroup2`。
- 每个 `BIT` 字段的 `length` 表示位数。
- 单个 `BIT` 字段有效位数最大为 32。超过 32 位应产生警告，实际布局按 32 位处理。
- 位容器按总位数选择 8、16 或 32 位。
- 超过 32 位的连续位字段应拆成多个位组。
- 位偏移在实现中使用 0 基；导出文档可使用 1 基展示。

位容器未填满时，剩余位应视为预留位。运行期解析不得把未定义位解释为业务字段。

### 7.3 有效字段布局

`is_valid` 影响运行期解析，不影响原始字节序展示：

- 有效普通字段参与帧大小、打包、解析和 Schema。
- 无效普通字段不参与运行期解析链路。
- `BIT` 字段在加载和校验时强制设置为有效。
- 有效 `RESERVED` 占用帧大小，但不产生业务值。
- 无效 `RESERVED` 不占用运行期帧大小，也不产生业务值。

---

## 8. 校验规则

保存 CSV、导入协议、运行测试、生成辅助代码和导出文档前都必须执行字段校验。

错误会阻断操作：

- 字段类型不在支持列表中。
- `name_en` 不是合法标识符表达式。
- 归一化后的字段名重复。
- 顶层成员名在标量、数组、位组、预留字段之间发生类别冲突。
- `RESERVED.length <= 0`。
- `BIT.length <= 0`。
- 定标整数类型的 `lsb` 存在且小于等于 `0`。
- 定长类型的 `length` 不是 `0` 且不等于类型固定长度。
- 数组下标重复。
- 数组混用标量元素和结构体字段。
- 数组下标不从 `0` 开始、不连续或顺序不正确。
- 结构数组的某个字段没有覆盖所有数组下标。
- 运行期接收帧长度小于协议要求的有效帧大小。

警告不阻断操作，但必须进入日志或校验报告：

- `BIT.length > 32`，后续布局和解析按 32 位截断处理。
- `CONST` 字段缺少默认值。
- 有效字段缺少 `name_cn`，展示时将回退到 `name_en`。

---

## 9. 运行期处理流程

### 9.1 命令帧打包

```text
测试步骤参数
  -> 选择协议 CSV
  -> 校验 FieldSpec
  -> 根据 default 生成初始字段值
  -> 写入测试步骤字段值
  -> 按字段顺序和小端规则打包 QByteArray
  -> 通过 HAL 串口或 CANFD 接口发送
```

要求：

- 打包前必须确认协议 CSV 已通过校验。
- 定标字段写入物理量时，应按 `raw = value / lsb` 编码，并检查原始整数范围。
- `RESERVED` 字节默认填 `0`，除非协议资产显式定义其他填充值。
- `CONST` 字段发送时使用默认值。

### 9.2 响应帧解析

```text
HAL 接收 QByteArray / CanFdFrame
  -> 按协议 CSV 校验帧长度
  -> 按字段顺序和小端规则解析
  -> 应用 lsb 得到物理量
  -> 生成字段 Schema 或键值结果
  -> 测试算法执行字段级判定
```

要求：

- 响应帧长度不足必须视为协议错误。
- 响应帧长度超出协议要求时，默认只解析协议定义长度；是否允许尾部扩展由具体协议资产或产品配置声明。
- 接收 `CONST` 字段时建议校验其值与默认值一致；不一致时记录协议错误。
- 未定义位、无效字段和预留字段不得进入测试结果判定。

### 9.3 日志与追踪

协议运行期日志应复用业务层生成的 `requestId`，并至少记录：

- 协议名或帧名。
- 传输资源 ID，例如 `SERIAL_A`、`CANFD_A`。
- 操作类型，例如 `pack`、`send`、`receive`、`unpack`、`validate`。
- 帧长度。
- 校验状态和错误字段名。

原始帧日志应控制体积和敏感性。默认记录摘要、长度和十六进制截断片段；完整原始帧只在调试开关启用时记录。

---

## 10. `ProtocolProfile` 接入

产品配置可用 `ProtocolProfile` 引用协议资产，并用 `ExchangeAction` 携带逻辑动作描述。BIZ 只解析、保存和透传这些中性配置；算法层负责选择协议资产、解释动作并绑定 HAL 逻辑资源：

```json
{
  "protocolProfiles": [
    {
      "id": "read_status_request",
      "busType": "SERIAL",
      "payloadEncoding": "hex",
      "frameFormat": {
        "protocolCsv": "read_status_request.csv"
      },
      "timing": {
        "timeoutMs": 1000
      },
      "responseRules": {},
      "fieldMapping": {}
    },
    {
      "id": "read_status_response",
      "busType": "SERIAL",
      "payloadEncoding": "hex",
      "frameFormat": {
        "protocolCsv": "read_status_response.csv"
      },
      "timing": {
        "timeoutMs": 1000
      },
      "responseRules": {},
      "fieldMapping": {}
    }
  ],
  "exchange": {
    "stimulus": {
      "source": "HalSerial",
      "busType": "SERIAL",
      "channelId": "SERIAL_A",
      "operation": "send",
      "protocolProfileId": "read_status_request"
    },
    "acquisition": {
      "source": "HalSerial",
      "busType": "SERIAL",
      "channelId": "SERIAL_A",
      "operation": "receive",
      "protocolProfileId": "read_status_response"
    }
  }
}
```

约定：

- `frameFormat.protocolCsv` 指向协议资产目录下的 CSV 文件。
- `ExchangeAction.channelId` 使用 HAL 逻辑资源 ID。
- `ProtocolProfile.busType` 与 `ExchangeAction.busType` 必须一致。
- 同一测试步骤可以引用一个命令协议和一个响应协议。
- 协议资产目录、产品配置和报告字段应一起纳入版本管理或发布包。

---

## 11. C++ 与代码生成建议

以下内容来自源规范中的 C++ 和代码生成部分，在本项目中作为建议项：

- 可以从协议 CSV 生成 `<帧名>_protocol.h`，文件名示例为 `read_status_response_protocol.h`。
- 生成物可以包含帧结构体、默认值构造函数、`FRAME_SIZE`、小端读写辅助函数、定标编码/解码函数、`packFrame()`、`unpackFrame()` 和 `buildSchema()`。
- 生成代码建议放入独立命名空间，例如 `hwtest::protocol` 或项目明确的协议命名空间。
- 生成物不得成为 HAL 公共头的一部分，避免把产品协议字段扩散到 HAL 兼容面。
- 生成物应由 CSV 再生成，不建议手工长期维护。
- 若生成物参与构建，应将生成输入、生成工具版本和输出路径写入构建或发布记录。

当前强制要求仍是 CSV 协议资产和运行期校验规则。是否采用 C++ 代码生成，由后续协议运行库或产品项目实现决定。

---

## 12. 测试要求

协议相关测试属于契约测试和集成测试：

- CSV 读取：UTF-8 with BOM、空行、额外列、布尔文本、缺列失败。
- 字段校验：类型、长度、`lsb`、命名表达式、重复字段、数组连续性、位字段分组。
- 布局计算：普通字段、定标字段、浮点字段、`BIT` run、`RESERVED`、有效和无效字段。
- 打包解析：小端整数、IEEE 754 浮点、定标编码解码、默认值、`CONST` 校验。
- 传输集成：算法层通过 HAL Mock Adapter 完成串口 echo、CANFD loopback 或产品协议样例；BIZ 单测只使用 `FakeAlgorithmExecutor`。
- 错误路径：帧长度不足、字段非法、协议 CSV 无效、传输超时和协议错误日志。

真实硬件协议测试必须单独标记，不进入默认 CI。

---

## 13. 验收标准

- 每个底层通讯帧都有对应 CSV 协议资产。
- CSV 列、字段类型、命名、布局和校验规则符合本文。
- 多字节字段按小端序处理，除非设计文档显式扩展字节序规则。
- 测试算法不手写与 CSV 冲突的字节偏移。
- HAL 不解释协议字段业务含义，只负责传输和统一错误面。
- 协议解析错误能被定位到协议名、字段名、资源 ID 和 `requestId`。
- C++ 生成物和文档导出物只作为 CSV 的派生产物，不反向覆盖 CSV 事实源。
