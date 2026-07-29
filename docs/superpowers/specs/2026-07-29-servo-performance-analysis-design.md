# 舵机连续测试性能分析与伯德图设计

> 状态：已于 2026-07-29 按实施计划落地；本文保留设计决策，现行实现与验证事实以五层架构、WebSocket 契约和测试规范为准。
>
> 本文定义舵机连续测试结束后的后处理设计。当前实现以[五层架构](../../design/overview/five-layer-architecture.md)、[设备通讯协议契约](../../design/contracts/device-communication-protocol.md)、[WebSocket 前端协议契约](../../design/contracts/websocket-frontend-protocol.md)和[测试规范](../../design/testing/testing-specification.md)为事实源；执行记录见[实施计划](../../plan/2026-07-29-0820-plan.md)。前一阶段的采集链路见[舵机远端连续测试设计](./2026-07-28-servo-remote-continuous-test-design.md)；本文只增加后处理，未被本文明确覆盖的前一阶段决策继续有效。

## 1. 目标与已确认口径

在现有 `mbddf.helm_stream` 设备持续测试完成硬件停止后，自动计算四路舵机的性能指标，并在 Web 前端展示计算过程、逐通道结果和伯德图。

已确认的产品口径如下：

- 性能结果只用于计算和展示，不参与现有测试 `Pass/Fail`；采集 verdict、分析状态和测量结果相互独立。
- 覆盖现有五种波形：正弦波、方波、三角波、恒值和连续扫频；伯德图只适用于连续扫频。
- 连续扫频保持用户手动停止。DUT 在 `sweep_duration_s` 后输出零并继续反馈，PC 不自动发送 STOP。
- 沿用参考工程的频响指标，但修正弱激励、相位解包、有效频段和停止竞态等问题，不要求与参考工程逐点数值一致。
- 系统假定舵机流不存在丢样。分析器不补点、不对原始输入样本插值、不重采样，也不因产品帧或 DDS 流水号跳变拒绝分析；既有不连续字段只作为诊断数据展示。派生指标可以在同一连续有效段的相邻估计点间插值，但不得跨无效空洞。扫频估计直接使用每个样本的 DDS 相对时间，不要求为 FFT 构造等间隔网格。实际发生丢样属于本设计不覆盖的外部异常条件。
- 当前性能定义使用反馈帧中的 DDS 指令回显 `ins[i]` 作为输入、角度反馈 `fdb[i]` 作为输出。`ins[i]` 不是舵控程序内部限幅后的实际执行指令，结果不得被表述为限幅后内部闭环的独立测量。

## 2. 范围与非目标

本设计包含：

- 后端独立的分析输入采集、尾样本封存栅栏、异步计算和结果持久化；
- 通用跟踪指标及五种波形的专用指标；
- 扫频频响、-1 dB/-3 dB 带宽、指定频点相位、共振峰和伯德图；
- 应用快照和 WebSocket v1 的追加式分析 DTO；
- Web 性能页、计算进度、通道切换、部分结果和无法计算说明；
- 纯算法 golden、应用/WebSocket 契约、前端和真机对照验证设计。

本设计不包含：

- 性能限值、合格判据或由性能结果派生的测试 verdict；
- 启停、暂停、重启或管理独立 `MB_DDF_v2_HelmControl` 进程；
- 修改 DDS/COM3 产品协议或新增限幅后内部指令字段；
- 参考工程中的负载力矩、BIT、最大舵偏角、零位和依赖预设编号的项目；这些项目存在未说明的标定常数或缺少当前协议输入，后续另行设计；
- TUI 和 Qt GUI 的性能页面。当前项目以 Web 链路为首要基线，公共 DTO 保留未来适配能力；
- 历史运行查询、跨运行对比、手工重新分析或浏览器选择保存路径；
- WebSocket 断线后的后台续算和重连恢复。当前断线仍沿用既有安全 cleanup/shutdown，并取消未完成分析。

## 3. 参考实现评估

外部参考目录为：

```text
H:\Resources\RTLinux\MGJ_APPS\monitor\front_end\source\cyberlink-terminal
```

关键入口为 `components/HelmTest.tsx::stopTest/analyzeData` 和 `utils/servoAnalysis.ts::calculateServoCharacteristics/calculateFrequencyResponse`。参考实现由浏览器在用户停止后同步读取前端全量缓存并计算：

```text
H(f) = Y(f) / U(f)
magnitude(f) = 20 * log10(abs(H(f)))
phase(f) = atan2(imag(H(f)), real(H(f)))
```

可复用内容是 `H=Y/U` 的频响目标、指标集合、四通道展示思路和对数横轴伯德图。参考实现的整段 FFT 比值不直接复用。以下行为不进入本项目契约：

- 前端固定按 1 ms 重建时间，不使用设备时间戳；
- STOP 请求发出后不等 ACK 和尾包就开始分析；
- 输入频谱过小时填充 `0 dB/0°`；
- 在相位解包前对 `[-180°, 180°]` 包裹值做普通平均；
- 先在显示范围外计算峰值或截止频率，再裁剪图形；
- 使用中文项目名称或旧预设编号决定算法；
- 使用无来源说明的 `0.03`、`2 ms`、`0.13` 等常数；
- 在浏览器主线程持有无限样本并执行递归 FFT。

## 4. 方案比较与架构决策

| 方案 | 优点 | 主要问题 | 决策 |
| --- | --- | --- | --- |
| 浏览器在 STOP 后计算 | 接近参考工程，界面接线最少 | 现有前端只有 50,000 点环形缓存；断线、刷新和长时间运行会丢失分析输入；主线程 FFT 会阻塞 UI；结果难以作为可追溯证据 | 不采用 |
| `HelmStreamAlgorithmExecutor::executeStep()` 返回前同步计算 | 算法处于产品算法层，能直接访问样本 | 用户 STOP 的 5 秒时限会同时承担设备 STOP、尾包和分析；计算失败会混入硬件停止语义；当前停止路径还会抑制用户停止任务的结果投影 | 不采用 |
| 应用层编排、算法层计算的异步后处理 | 先完成硬件停止，再独立计算；不依赖浏览器缓存；可显示过程并持久化；产品数学仍留在算法层 | 需要新增分析端口、临时采集、封存栅栏和 DTO | 采用 |

目标数据流为：

```text
HELM_STREAM RawSample
  -> TestApplicationController
       -> ContinuousDataRecorder（用户选择时保存完整 TXT）
       -> PostRunAnalysisCoordinator
            -> IPostRunAnalysisSession（算法层解析并采集舵机最小列）
       -> sampleReceived（继续实时投影到 Web）

用户 STOP
  -> BIZ/算法/HAL 完成 HELM_STOP 和既有安全收尾
  -> 内部尾样本关闭栅栏
  -> 封存不可变分析输入
  -> stopCompleted（不等待性能计算）
  -> 后处理 QThread
       -> HelmPerformanceAnalyzer
       -> 结果 JSON
       -> ApplicationSnapshot.analysis（只含状态和摘要）
       -> WebSocket v1 只读 analysisResult 分通道取曲线
       -> 浏览器性能页
```

分层约束：

- `HelmPerformanceAnalyzer` 位于算法层，负责产品字段解释、预处理、数学计算、指标语义和结果 Schema。
- `PostRunAnalysisCoordinator` 位于应用组合层，只按 `algorithmId` 创建分析会话、路由并复制通用样本、编排线程和投影通用状态；它不得识别 `ins/fdb`、自检、timeout、单位或通道语义。
- BIZ 不新增舵机字段或性能语义，不缓存样本，不承担后处理调度。
- WebSocket 层只序列化应用 DTO；浏览器不重新计算权威指标。
- HAL、Provider、Adapter 和 DUT 不因性能分析发生变化。
- `AnalysisResult` 是应用层持有的 sidecar artifact，不进入 `biz::TestResult`、`MeasurementRecord` 或 BIZ 报告输入。

## 5. 组件边界

### 5.1 通用后处理端口

算法层提供只依赖 Qt Core 的通用端口，概念接口如下：

```cpp
class IPostRunAnalysisSession {
public:
    virtual ~IPostRunAnalysisSession() = default;
    virtual AnalysisAcceptResult append(const AnalysisSample& sample) = 0;
    virtual AnalysisInputSeal seal(const AnalysisTermination& termination) = 0;
};

class IPostRunAnalyzer {
public:
    virtual ~IPostRunAnalyzer() = default;
    virtual AnalysisResult analyze(const AnalysisInputSeal& input,
                                   const AnalysisProgressCallback& progress,
                                   const AnalysisCancelToken& cancel) = 0;
};
```

实际公共结构采用尾部扩展和版本化 Schema，避免把 `ins/fdb` 固化到应用公共 API。`mbddf.helm_stream` 的 `IPostRunAnalysisSession` 实现从不透明 `values/tags` 中解析、校验并版本化舵机专用输入；应用层只管理接口生命周期。

### 5.2 `HelmAnalysisCapture`

职责：

- 只接受当前 `taskId`、`stepId=HELM_STREAM` 的样本；
- 作为算法层会话实现，从通用样本提取 `streamElapsedUs`、`ins[0..3]`、`fdb[0..3]`、自检/timeout 和必要诊断字段；控制器只调用 `append()`，不解释字段；
- 在启动时冻结 `effectiveRunParameters`、配置 ID、算法 ID、运行开始时间和启用位图；
- 使用有缓冲的流式临时文件而不是无界内存，支持用户长时间手动运行；
- 受后端 `maxCaptureBytes`、`maxInputSamples`、`maxAnalysisDurationS` 和最小磁盘余量约束，浏览器不得覆盖；触发限制时停止追加分析输入，最终状态为 `unavailable`、原因码为 `analysis_resource_limit`，但不影响既有采集、STOP、verdict 或可选 TXT；
- STOP 收尾后通过关闭栅栏确认所有此前接受的样本已写入，再关闭写端并产生不可变输入；迟到样本只计数，不得修改已封存输入；
- 成功后删除临时输入；失败时按保留策略保存为诊断输入，并在独立 `diagnosticInputFilePath` 中返回路径；下次启动只清理分析专用目录内超过保留期且不属于活动任务的孤儿文件。

分析采集与“保存全部测量列”互相独立。关闭 TXT 保存不得关闭性能分析输入；浏览器仍无权指定临时或结果路径。

临时分析输入是本功能的启动前置条件。创建分析专用目录或初始输入失败时，控制器在启动 BIZ 任务前返回 `analysis_storage`；运行中写入失败则继续完成既有硬件测试和 STOP，在分析侧终态报告 `failed`。

### 5.3 `HelmPerformanceAnalyzer`

职责：

- 校验必需参数、有限数值、时间单调性、启用通道和波形所需的完整周期；
- 对每个启用通道独立计算，禁用通道返回 `not_applicable`；
- 生成通用指标、波形专用指标、数据说明和绘图点；
- 输出稳定、版本化、与 UI 无关的结果模型；
- 不修改采集 verdict，不调用 HAL 或产品通讯。

扫频使用基于已知 DUT 扫频相位律的局部同步复数估计，不要求把 DDS 时间轴重建为 FFT 等间隔网格。估计器作为独立小单元实现，未来可以替换而不改变分析端口和结果 Schema。

### 5.4 `PostRunAnalysisCoordinator`

职责：

- 根据当前 descriptor/算法注册表判断是否支持后处理；
- 管理单次运行的捕获对象、关闭栅栏、不可变输入、工作线程、取消令牌和 `{taskId, analysisGeneration}` 关联；
- `analysisGeneration` 是 coordinator 私有、单调递增且不复用的 `quint64`，不等同于控制器现有 lifecycle generation；只在启动成功得到新 taskId 后创建新身份并清空旧投影；
- coordinator 始终位于控制器 QObject 亲和线程；工作线程只持有共享且不可变的 `AnalysisRunContext`、封存输入和原子取消令牌，不得访问控制器、BIZ、HAL 或 WebSocket 对象；
- 将进度和结果通过 `Qt::QueuedConnection` 回到控制器亲和线程；样本追加、封存、写入器关闭、进度、JSON commit 和终态发布都必须携带并校验身份对；
- 在分析终态前拒绝启动、重新选择测试和重新准备等写动作，允许快照、配置目录、控制资源和端口等只读动作；
- shutdown 时请求取消并安全 join。禁止 `QThread::terminate()`、detach 或删除运行中的线程；若配置时限内未退出，保留线程及共享上下文并返回 `analysis_shutdown_timeout`，不得释放仍可能被访问的对象；控制器析构前必须确认线程已经终止。

## 6. 生命周期与状态

分析状态独立于 `ApplicationSnapshot.phase` 和 BIZ `TestState`：

| 状态 | 含义 | 是否有结果 |
| --- | --- | --- |
| `none` | 当前配置不支持分析或尚未开始 | 否 |
| `capturing` | 舵机样本正在进入分析采集 | 否 |
| `queued` | 输入已封存，等待工作线程 | 否 |
| `validating` | 检查参数、样本和波形覆盖 | 否 |
| `preprocessing` | 截取、去直流、加窗等处理 | 否 |
| `calculating` | 逐通道计算 | 否 |
| `persisting` | 原子写入结果文件并生成 Web 投影 | 否 |
| `completed` | 所需数据完整，结果已生成 | 是，完整 |
| `partial` | 用户提前停止或仅覆盖部分可计算内容 | 是，明确限定范围 |
| `unavailable` | 数据不足、没有启用通道或无有效激励 | 否 |
| `failed` | 分析代码、临时文件或结果存储失败 | 否 |
| `cancelled` | shutdown 或明确的应用收尾取消了分析 | 否 |

顺序约束：

1. 只有 `startTestWithOptions()` 成功返回新 taskId 后，才递增 `analysisGeneration`、清除上一轮投影并进入 `capturing`；启动失败保留上一轮已完成结果。
2. 分析捕获接入控制器处理 `ITestRunService::sampleProduced` 的内部路径，位于 `suppressedResultTaskId` 的 UI 投影判断之前。RUN 和 STOP 等待 ACK 期间产生的尾样本都先进入当前身份的 `append()`；不得以公开 `sampleReceived`、`resultProduced`、STOP ACK 或 `stopCompleted` 作为分析输入来源。
3. 用户 STOP 仍按现有 `stopAsync -> stopCompleted` 完成 DUT STOP、BIZ 收敛和安全收尾。
4. 每次成功 `append()` 分配内部单调样本序号。收到采集终态后建立包含“最后接受序号”的关闭栅栏；只有此前追加均完成、缓冲写入器已关闭且身份仍匹配，才执行一次 `seal()` 并排队分析。封存后的同任务样本只增加 `lateSampleCount`，不得修改输入。
5. `stopCompleted` 仍只表示 DUT STOP、BIZ 收敛和既有安全收尾完成，不等待关闭栅栏之后的性能计算；STOP reply 不承担分析超时。
6. 采集以正常 `stopped/finished` 结束时执行数学分析；采集以通讯、协议、自检或其他 `error` 结束时仍封存诊断输入，但分析直接归约为 `unavailable`，不把故障前片段包装为正常性能结果。
7. 分析运行期间前端显示进度，但现有采集 phase 保持 `stopped`、`finished` 或 `error`，不伪造新的 BIZ 状态。
8. `completed/partial/unavailable/failed/cancelled` 都是分析终态，不改变 `snapshot.verdict`、`errorCode` 或采集消息。

重复终态、STOP/异常竞争和迟到工作线程回告都以 `taskId + analysisGeneration` 去重。每轮最多封存一次、启动一次分析、发布一个终态。

逐通道只使用 `not_applicable`、`completed`、`partial` 和 `unavailable`。整体状态按下表归约：

| 逐通道情况 | 整体状态 |
| --- | --- |
| 禁用通道 | 不参与归约 |
| 所有启用通道均为 `completed` | `completed` |
| 至少一个启用通道有可发布的 `completed/partial` 结果，但并非全部完整 | `partial` |
| 没有任何启用通道可发布结果 | `unavailable` |
| 临时文件、线程、算法执行或结果提交等基础设施失败 | `failed` |
| shutdown 取消 | `cancelled` |

`failed/cancelled` 不作为逐通道性能状态；这两种整体终态不得携带半成品曲线。

## 7. 输入和持久化契约

### 7.1 输入字段

每个分析样本至少包含：

- 非负 `streamElapsedUs`；
- `ins[0..3]` 与 `fdb[0..3]` 的有限数值；
- `self_check_or_timeout` 及必要的自检/timeout 细分字段；
- 现有产品帧和 DDS 流水诊断字段，但这些字段不参与丢样处理或分析门禁。

一次运行元数据至少包含：

- `taskId`、`configId`、`algorithmId` 和结果 Schema 版本；
- `waveform`、`freq`、`ampl`、`offset`、`start`、`max_freq`、`sweep_duration_s`、`enable` 的生效值；
- 采集开始/结束时间、终止原因、原始样本数；
- 可选原始 TXT 路径和分析输入摘要。

扫频结果还必须保存拟合得到的 `tCommandStartUs`、候选搜索范围、最佳/次佳相关系数、候选重叠时长和选择规则。候选范围为 `[firstSampleUs-T, lastSampleUs]`，每个候选只使用映射到理论 `0..T` 且满足激励门槛的重叠样本。相关系数在数值容差内并列时先选择重叠时长较长者，再选择较早起点；仍不可区分时为 `unavailable/command_model_ambiguous`。

时间轴以 DDS 相对微秒为准，不使用 WebSocket 到达时间或 PC 逐样本时钟重建。`t_n=streamElapsedUs_n/1e6` 直接进入时间域拟合和局部同步估计；相邻时间差只用于报告采样率统计，不用于构造新的等间隔时间轴。时间必须严格单调，产品帧/DDS 流水号只改变诊断计数，不改变数值结果或分析状态。

### 7.2 结果文件

性能分析使用已解析 `dataStorage.directory` 下的固定分析子目录。`saveData` 只控制既有 `ContinuousDataRecorder` 原始 TXT，不影响最小分析输入或性能 JSON。结果使用 `QSaveFile` 原子写入，建议文件名为：

```text
mbddf-helm-stream_<taskId>_<analysisGeneration>_performance.json
```

JSON 至少记录：

- `schemaVersion`、`analyzerId`、`analyzerVersion`；
- taskId、analysisGeneration、运行身份和生效参数；
- 采样率、样本数、有效时长和分析状态；
- 每个通道的通用指标、波形专用指标、单位、有效时间/频段、有效点数、分析阈值和无效原因；
- 扫频完整精度的频率、幅值和相位数组；
- 结果生成时间、原始 TXT 路径（若存在）、规范化输入摘要和 SHA-256、诊断信息；
- `reproducible`：原始 TXT 已保存且其哈希匹配时为 `true`；关闭 TXT 保存并在成功后删除唯一临时输入时为 `false`，结果不得被表述为可重放证据。

临时输入和结果使用 taskId/analysisGeneration 的专用无冲突命名，不复用原始 TXT 的 `.partial` 命名空间。仅在 `QSaveFile::commit()` 成功后设置 `resultFilePath`；诊断输入只进入 `diagnosticInputFilePath`。完整精度保留在结果 JSON，成功持久化的分析结果文件始终保存，原始完整 TXT 仍由现有 `saveData` 决定。

后端同时限制 `maxInputSamples`、`maxResultBytes` 和每次只读曲线回复的 `maxProjectedPoints`/`maxProjectedBytes`。达到输入限制时分析为 `unavailable/analysis_resource_limit`；结果 JSON 或投影超限时为 `failed`，不得生成截断却标称完整的文件。

## 8. 指标定义

### 8.1 统一窗口、激励和状态规则

所有计算使用 `t=streamElapsedUs/1e6` 秒。结果同时记录原始捕获区间和每项指标实际使用的 `analysisStartUs/analysisEndUs/analysisSampleCount`，避免把全程统计与稳定窗口统计混为一谈。

动态波形的实际激励下限为：

```text
absoluteExcitationFloor = 1e-6 degree
relativeExcitationRatio = 10^(-40/20) = 0.01
excitationFloor = max(absoluteExcitationFloor,
                      abs(configuredAmpl) * relativeExcitationRatio)
```

上述值和等号语义进入 `analyzerVersion`：实际输入幅值小于下限时为 `unavailable`，恰好等于下限时可计算但产生 `near_excitation_floor` 警告。方波用半阶跃幅度、三角波用实际半峰峰值代替 `configuredAmpl` 复核；恒值允许目标为零，不应用动态激励门槛。

若输出拟合幅值低于 `absoluteExcitationFloor`，增益只可表示为以上限结尾的衰减值，状态为 `upper_bound`，相位为 `indeterminate`；不得为相位填零。无效点、有效点、上限值和未覆盖点使用显式状态，不以 NaN/Inf 或字符串数值编码。

所有启用通道均输出：

| 指标 | 定义 |
| --- | --- |
| `raw_sample_count/raw_duration_s` | 封存输入的原始样本数和首末 DDS 时间差 |
| `analysis_sample_count/analysis_duration_s` | 实际分析窗口的样本数和时间差 |
| `sampling_frequency_hz` | `(N-1)/(t_last-t_first)`，只作时序摘要 |
| `command_peak/rms` | 分析窗口内 `ins[i]` 的峰值和 RMS |
| `feedback_peak/rms` | 分析窗口内 `fdb[i]` 的峰值和 RMS |
| `mean_error` | `mean(fdb-ins)` |
| `mae` | `mean(abs(fdb-ins))` |
| `rmse` | `sqrt(mean((fdb-ins)^2))` |
| `max_abs_error` | `max(abs(fdb-ins))` |

自检或 timeout 异常不改变采集 verdict，但对应通道的性能分析返回 `unavailable` 并说明异常位置，避免把设备明确报告的异常样本包装为正常测量结果。

### 8.2 正弦波

对配置频率 `freq` 使用带常量项、同一 DDS 时间基准的最小二乘拟合：

```text
x(t) = a*sin(2*pi*f*t) + b*cos(2*pi*f*t) + c
amplitude = sqrt(a^2 + b^2)
phase = atan2(b, a)
```

分别拟合 `ins[i]` 和 `fdb[i]`。定义：

```text
phaseLagDeg = wrapTo[-180, 180)(phaseCommand - phaseFeedback)
principalDelayMs = phaseLagDeg / (360*f) * 1000
```

正值表示反馈在主值范围内滞后。`principalDelayMs` 只在一个周期的模意义下成立，不宣称是绝对运输时延；`0.75/f` 等时延可能与 `-0.25/f` 主值等价。输出幅值比、增益 dB、主值相位滞后、主值时延、稳定段 RMSE 和最大误差。

默认丢弃前两个完整周期，之后至少需要三个完整周期；实际指令拟合幅值必须满足统一激励门槛。数据只够产生部分通用指标时为 `partial`，连通用窗口也不足时为 `unavailable`。

### 8.3 方波

从实际 `ins[i]` 检测平台和边沿，不使用参考工程硬编码的 `10°/0.3°`。每个候选边沿必须在前后各存在不少于配置半周期 20% 的稳定命令平台，平台值分别取其中央 50% 样本的中位数 `u0/u1`，并满足 `abs(u1-u0)/2 >= excitationFloor`。

定义：

- 指令边沿时刻是 `ins` 线性插值穿过 `u0+0.5*(u1-u0)` 的时刻；
- 反馈初值 `y0` 取前平台中位数，10%/90% 阈值为 `y0+0.1*(u1-u0)`、`y0+0.9*(u1-u0)`；穿越时刻均在相邻 DDS 样本间线性插值；
- 响应延迟是指令边沿到反馈首次同方向穿越 10% 阈值的时间；上升/下降时间是反馈 10% 到 90% 的时间；
- 超调以实际命令目标 `u1` 和阶跃幅度 `abs(u1-u0)` 为基准；
- 稳定时间要求反馈进入 `u1 ± 2%*abs(u1-u0)` 后持续到下一指令边沿或观测结束，且有效保持时间不少于半周期的 10%；未稳定返回 `not_settled`；
- 稳态误差使用后平台末尾 20% 区间的 `mean(fdb-u1)`。

分别输出上升沿、下降沿的有效边沿数、平均值和最差值。尾部截断或缺少完整平台的边沿不参与专用指标；仍有其他完整边沿时通道为 `partial`，没有完整边沿时只保留通用指标。

### 8.4 三角波

先按配置频率划分周期，再对实际 `ins[i]` 计算时间导数。导数绝对值低于本周期中位绝对斜率 5% 的样本属于转折死区；连续方向段必须不少于四分之一配置周期。每个方向段排除命令幅度顶部和底部各 10%，使用实际 DDS 时间做加权线性拟合。

输出：

- 正、负方向反馈斜率与指令斜率之比；
- 上升、下降段 RMSE 和最大误差；
- 正负方向斜率不对称性；
- 把实际命令范围等分为 32 个固定分箱，在同时具有正、反方向样本的分箱内计算反馈中位数之差。

最后一项命名为“方向性跟踪差（含动态滞后）”，不宣称为准静态机械回差。至少需要一个完整上升段和一个完整下降段，否则只输出可定义的部分结果。

### 8.5 恒值

要求原始有效时长 `D >= 1 s`。稳定窗口长度为：

```text
W = min(D, max(0.2*D, 0.2 s))
```

窗口为 `[t_last-W, t_last]`，目标值取窗口内 `ins[i]` 中位数。输出窗口平均反馈、`mean(fdb-target)`、总体标准差 `sqrt(mean((fdb-mean(fdb))^2))`、反馈峰峰值和 `max(abs(fdb-target))`。数据不足时为 `unavailable`。

### 8.6 连续扫频与伯德图

#### 8.6.1 DUT 扫频律和观察窗口

本设计使用 2026-07-28 舵机设计已定义的 DUT 相位律。令 `f0=freq`、`f1=max_freq`、`T=sweep_duration_s`、`phi0=start`，对 `0<=tau<=T`：

```text
phase(tau) = 2*pi*f0*f1*T/(f1-f0)
             * ln(f1*T/(f1*T-tau*(f1-f0))) + phi0
f_inst(tau) = f0*f1*T/(f1*T-tau*(f1-f0))
```

该公式支持正向和反向扫频。结果频率数组始终按升序保存。必须满足 `f0>0`、`f1>0`、`T>0` 且最大频率低于由 DDS 时间轴摘要得到的 Nyquist 频率。`f0==f1` 时按单频正弦分析，只输出一个频点及正弦指标，不计算带宽或伯德曲线。

`streamElapsedUs=0` 是首个有效反馈样本，不是 DUT 指令起点。分析器不得直接把 `0..T` 当作扫频窗口，而要按第 7.1 节的候选范围和并列规则拟合时间偏移 `tau=t-tCommandStart`，使去均值后的实际 `ins[i]` 与上述命令模型归一化相关系数最大。初版 `minCommandCorrelation=0.95`，低于该值时通道 `unavailable/command_model_mismatch`。结果记录 `tCommandStartUs`、`observedExcitationStartUs`、`observedExcitationEndUs`、最佳/次佳相关系数和理论/实际覆盖范围。

#### 8.6.2 局部同步复数估计

先在 `maxDelayMs=100 ms` 的版本化有界范围内，用 `ins/fdb` 归一化互相关估计粗时延 `delay0`。对计划频率 `fc`，由 `f_inst(tauc)=fc` 得到命令中心时刻；选择相位跨度为四个周期、中心为 `tauc` 的局部窗口。命令窗口必须完整位于观测到的有效指令区间；反馈系数使用命令窗口整体后移 `delay0` 的样本，但点级可发布性采用更保守的 `maxDelayMs` 保护：已封存反馈区间必须覆盖从命令窗口起点到“命令窗口终点 + maxDelayMs”的完整范围。`delay0` 只用于局部坐标对齐，不缩小完整性保护带。响应尾段不足时，扫频末端对应频点（正扫为高频、反扫为低频）标记 `not_covered`。

对每个实际 DDS 时间样本，使用相位域 Hann 权重 `w_n` 和按相邻 DDS 时间构造的梯形积分权重 `q_n`。命令相位使用 `tau=t_command-tCommandStart`；反馈窗口的样本使用移回粗时延后的 `tau=(t_feedback-delay0)-tCommandStart`。分别计算命令与延迟对齐反馈的局部复系数：

```text
C_x(fc) = sum(q_n*w_n*(x_n-mean_w(x))*exp(-j*phase(tau_n)))
          / sum(q_n*w_n)
H_residual(fc) = C_feedback(fc) / C_command(fc)
H(fc) = H_residual(fc) * exp(-j*2*pi*fc*delay0)
```

此估计直接使用实际 DDS 时间，不要求等间隔 FFT 网格，也不执行丢样补偿。四周期局部积分本身承担平滑作用，首版不再追加跨频率移动平均。`cyclesPerEstimate`、`maxDelayMs`、相关阈值和激励阈值全部记录在 `analyzerVersion` 和结果 JSON 中。

计划频率使用最多 256 个对数等距点。只有局部命令幅值满足统一激励门槛、命令/反馈窗口完整且估计为有限数时才形成有效点。输出幅值为 `20*log10(abs(H))`；相位仅在输出局部幅值可辨识时计算，并在每个连续有效频段内独立解包，禁止跨无效空洞解包或插值。

#### 8.6.3 摘要指标和完整性

- 只有某一连续有效段包含 `plannedUsableBand` 的最低频率锚点，才可用该段最低 10% 对数频率范围且不少于 5 个有效点的幅值中位数作为低频基准 `G0`。反扫提前停止而只观察到高频段时不得把该段下缘冒充低频基准；条件不满足时 `G0`、带宽和共振峰为 `indeterminate`。
- -1 dB/-3 dB 带宽只在包含 `G0` 的同一连续有效频段内，取幅值首次向下穿越 `G0-1 dB`、`G0-3 dB` 的线性插值频率。频段已到达本次观测上限但未穿越时为 `above_observed_range`；被空洞或窗口边界截断时为 `not_covered`。
- 5/10/20 Hz 相位只在目标频率被同一连续有效段的相邻点包围时插值；不得跨空洞或从范围外最近点代替。
- 共振峰只在包含 `G0` 的连续有效频段内取相对 `G0` 的最大幅值及其频率。

理论上完整的扫频仍会因四周期窗口和最大时延保护在首尾形成不可估计保护带。`plannedUsableBand` 由相位律、局部窗口和 `maxDelayMs` 预先计算并写入结果；观察到完整 DUT 激励、响应尾段覆盖该计划带，且所有计划点有效时为 `completed`。提前 STOP、尾段不足或只有部分连续频段可用时为 `partial`，只展示实际有效范围，不推断未覆盖频率。

频率以 Hz 作为协议和结果单位。前端可用 `rad/s=2*pi*f` 做纯显示切换，不改变保存结果和指标计算。

## 9. 应用 DTO 与 WebSocket v1

`TestDescriptor` 在尾部追加能力描述：

```text
postRunAnalysis:
  supported
  analyzerId
  schemaVersion
```

配置或算法注册表不声明时安全回退为 `supported=false`。`ApplicationSnapshot` 在尾部追加可选的通用分析摘要：

```text
analysis:
  supported
  analyzerId
  schemaVersion
  taskId
  analysisGeneration
  state
  progress
  stage
  message
  resultFilePath
  diagnosticInputFilePath
  sourceSummary
  channelSummaries[]
```

`channelSummaries[]` 只包含小型摘要：

```text
channel
enabled
status
warnings[]
commonMetrics
waveformMetrics
bodeAvailable
bodePointCount
```

伯德数组不进入完整快照，避免状态更新和重连时反复广播大载荷。WebSocket v1 增加只读动作：

```json
{"action":"analysisResult","params":{"taskId":"...","analysisGeneration":3,"channel":0}}
```

服务端只允许读取当前分析身份和 `channel=0..3`，不接受客户端路径、分辨率或任意历史标识。分析未到终态返回 `analysis_not_ready`，身份不匹配返回 `stale_analysis_result`。成功 reply 返回该通道的摘要和经过确定性对数抽样的：

```text
bode.frequencyHz[]
bode.magnitudeDb[]
bode.phaseDeg[]
bode.pointStatus[]
```

四个数组长度必须相等。每通道默认不超过 256 点，并由 `maxProjectedBytes` 再次收紧，保证单条紧凑 JSON reply 不超过 16 KiB；抽样必须保留首末有效点、无效空洞边界、-1/-3 dB 交点、5/10/20 Hz 邻点和共振峰。完整精度只在性能 JSON 中保存。

完整快照中的分析摘要同样受固定 Schema 和 `maxAnalysisSummaryBytes` 约束：每通道警告最多 16 条、单条 UTF-8 文本最多 512 字节，指标键集合由 schemaVersion 固定，超出部分只增加 `omittedWarningCount`。摘要不得把完整 JSON 中的任意动态对象原样投影到快照。

兼容规则：

- WebSocket 协议版本仍为 1，descriptor 能力、`analysis` 摘要和只读动作都是追加扩展；旧客户端可以忽略，新客户端收到旧服务端缺失字段时必须回退为 `supported=false`、`state=none`、`analysisGeneration=0` 和空摘要。
- `analysis` 不进入 sample 事件，避免每个样本重复发送分析状态。
- `analysisResult` 只读取后端已完成结果，不触发重新计算；分析仍由后端在舵机流终止后自动启动。
- 进度快照不携带半成品曲线；分析终态先广播摘要，浏览器再按通道顺序读取绘图结果。
- `resultFilePath` 沿用现有后端路径只读投影语义，客户端不能覆盖。
- WebSocket 序列化必须拒绝 NaN/Inf；无效指标使用显式状态或 `null`，不得用字符串数值或伪造的零替代。微秒、样本数和 generation 必须处于 JavaScript 安全整数范围。

## 10. Web 交互设计

前端新增“性能”导航项，仅在 descriptor 声明分析能力时启用。

### 10.1 运行与计算过程

- 运行期间显示“停止后自动计算”和 PC 观察到的设备流时长，但不得把 `streamElapsedUs=0` 当作 DUT 命令起点，也不得在运行期保证已经覆盖完整扫频。达到观察时长 `sweep_duration_s + maxDelayMs` 后只提示“理论时长已达到，可手动停止；完整性将在停止后根据指令回显拟合确认”。PC 始终不自动 STOP，任何时刻手动停止都允许，最终由离线 `tCommandStartUs`、计划带和尾段覆盖决定 `completed/partial`。
- 用户 STOP 成功后自动进入性能页，依次显示“等待尾样本、封存数据、校验、预处理、计算舵1..舵4、保存结果”。同一 WebSocket 会话内可以切换 SPA 页面，后端计算继续。
- 分析非终态时，本条覆盖 2026-07-28 首期设计中“停止后可再次编辑并开始”的规则。控制器在 `start`、`prepare`、`selectTest`、控制/串口选择等写动作入口统一返回既有 `command_in_progress`；Web 不能只靠禁用按钮。`snapshot`、`testConfigs`、`controls`、`ports` 和满足条件的 `analysisResult` 保持只读可用。
- `disconnect`、`quit` 和意外断线继续沿用现有 cleanup/shutdown，并取消未完成分析；本阶段不承诺断线后续算或重连恢复。页面内导航不等同于 WebSocket 断线。
- 不复用 `busyAction=stop` 表示分析，避免让用户误以为硬件仍在停止。分析到任一终态后恢复既有停止后编辑和再次启动行为。
- 新一轮启动成功后才清空旧结果。页面始终显示 taskId、analysisGeneration 和开始时间，旧任务结果不得残留。

### 10.2 结果页面

页面包含：

- 顶部身份区：任务 ID、波形、生效参数、启用通道、采样率、有效时长、分析版本和“仅测量，不参与 Pass/Fail”提示；
- 通道选择：舵1至舵4，可单通道查看或多通道叠加；禁用通道显示“未参与”；
- 通用指标卡和按波形切换的专用指标表；
- 扫频时显示共用对数 Hz 横轴的上下两幅 uPlot：幅值 dB 和相位度；
- -1 dB/-3 dB、5/10/20 Hz 和共振峰标记；弱激励或未覆盖频点留空，不跨空洞连线；
- `partial/unavailable/failed` 的明确原因、实际覆盖频段和可用指标，不把数据不足显示为性能不合格。

现有实时“曲线”页面继续展示样本环形缓存；性能页从 `analysisResult` 逐通道取得后端投影，不读取 `SampleBuffer` 作为分析输入。切换通道时可缓存当前 taskId/generation 的 reply，新身份出现时必须整体清空。

## 11. 错误与取消

| 场景 | 分析状态与行为 |
| --- | --- |
| 无启用通道 | `unavailable`，提示没有分析对象 |
| 必需参数缺失、非有限或时间倒退 | `unavailable`，不生成曲线 |
| 自检或 timeout 异常 | 受影响通道 `unavailable`，保留位置和字段说明 |
| 正弦/方波/三角波周期不足 | `partial` 或 `unavailable`，只保留定义上仍成立的通用指标 |
| 扫频提前停止 | `partial`，仅输出实际覆盖频段 |
| 扫频命令模型不匹配或响应尾段不足 | `unavailable` 或 `partial`，记录相关系数和未覆盖频段 |
| 扫频无有效激励 | 对应频点为空；全频段均无效时通道 `unavailable` |
| 分析输入达到资源上限 | `unavailable/analysis_resource_limit`，不影响采集和可选 TXT |
| 启动前不能创建分析输入 | `start` 返回 `analysis_storage`，不启动 BIZ 或硬件流 |
| 临时文件或结果 JSON 写入失败 | `failed`，不改变采集 verdict，保留诊断路径 |
| 计算超时 | `failed`，错误码 `analysis_timeout`；不发布半成品为有效结果 |
| shutdown/断线取消 | `cancelled`，请求协作取消并安全 join；超时返回 `analysis_shutdown_timeout` 且不释放活动上下文 |
| 新任务与旧结果竞争 | 以 taskId/analysisGeneration 丢弃旧结果，不覆盖新快照 |

分析超时与 shutdown 等待时间属于后端配置，不开放给浏览器。STOP 时限只覆盖既有硬件和任务收尾，不与分析超时共用。

## 12. 验证设计

### 12.1 纯算法 golden

- 使用版本化的原始样本 fixture、运行参数、DDS 时间轴、使能位和独立期望 JSON；fixture 及期望文件记录 SHA-256、生成方法、容差来源和 analyzerVersion，期望生成不得复用待测拟合、局部估计、插值或归约代码；
- 一阶低通、二阶欠阻尼和纯时延合成系统：验证局部同步复数估计、粗时延恢复、幅值、相位、-1 dB/-3 dB、共振峰和指定频点插值；
- 纯时延 fixture 分别提供完整响应尾段和截断尾段，验证扫频末端对应频点不会被错误标成完整；
- 正向与反向扫频分别覆盖完整、提前停止和尾段截断，验证受影响的是扫频末端对应频点且保存频率数组始终升序；
- 四通道使用不同增益、带宽和相位，验证通道隔离；
- 激励为零、低于阈值、恰好等于阈值和刚超过阈值，以及扫频范围外强噪声，验证无效点不会变成 `0 dB/0°` 或污染摘要；
- `-170° -> -190°`、有效段中间空洞和多个连续段，验证只在段内解包和插值；
- 正弦覆盖 `0.25/f` 与 `0.75/f` 延迟的主值歧义；方波覆盖正负阶跃、未稳定和尾部截断；三角波覆盖转折死区和方向性跟踪差；恒值覆盖窗口边界、零目标和已知偏差/噪声；
- 提前停止、周期不足、禁用通道、NaN/Inf、自检和 timeout；
- 命令模型完整、首样本晚于命令起点、相关系数恰在阈值和模型不匹配；
- 逐通道状态的所有组合及整体状态归约真值表。

本轮不设计丢样补偿、插值或因流水号跳变拒绝分析的 golden。必须有一组契约 fixture 只改变产品帧/DDS 流水号而保持时间和值完全相同，断言数值结果和状态不变、仅诊断计数变化。

### 12.2 应用与 WebSocket

- `start -> samples -> STOP 尾反馈 -> 关闭栅栏 -> seal -> analysis states -> terminal result` 的顺序；
- 尾反馈在公开 sample 抑制前仍进入分析；STOP reply 不等待性能计算，硬件停止时限不受后处理影响；
- `seal()` 后输入不可变，重复 STOP、异常终态、封存后迟到样本和迟到线程回告最多发布一个终态；
- 分析期间控制器写动作统一返回 `command_in_progress`，只读动作可用；shutdown/断线取消安全 join，超时不销毁活动上下文；
- taskId/analysisGeneration 的创建、启动失败保留旧结果以及旧身份不能覆盖新任务；
- TXT 保存关闭且采集远超前端 50,000 点时仍可分析，但 JSON 标记 `reproducible=false`；开启 TXT 时校验路径和哈希关联；
- 临时输入、JSON commit、输入/结果资源上限和孤儿清理失败路径；
- WebSocket v1 可选摘要、旧端回退、`analysisResult` 身份/通道白名单、NaN/Inf 拒绝、数组等长、空洞编码和每条 reply 不超过 16 KiB；
- `disconnect/quit/DropCleanup` 取消分析且不承诺重连恢复；
- 非舵机配置保持 `analysis.supported=false`，既有客户端行为不变。

### 12.3 浏览器前端

- descriptor 能力控制导航项；
- 运行、可停止、分析中、完整、部分、不可用、失败和已取消状态；
- 自动进入性能页但不阻断用户导航；
- 启动成功后清除旧结果，taskId/analysisGeneration 显示和四通道按需读取；
- uPlot 对数横轴、幅相同步游标、无效点空洞、Hz/rad/s 显示切换；
- `analysisResult` 抽样保留端点、空洞、交点、指定频率和峰值；新身份清空所有通道缓存；
- 不读取 50,000 点 `SampleBuffer` 计算性能，超过该容量的受控会话仍能展示后端结果；
- 单文件生产构建仍可连接回环 WebSocket。

### 12.4 真机与参考对照

- 在隔离且明确授权的台架按“波形、启用通道、终止方式、负载/工况、运行参数、独立参考、容差、重复次数、归档产物”形成验收矩阵；
- 记录 DDS 时间轴、生效参数、原始 TXT（对照轮次强制开启）、性能 JSON 和前端截图；
- 独立记录源端连续性、DDS 时间差分布、原始样本计数和流水诊断，作为“无丢样”外部前提的证据；分析器仍不因流水号门禁；
- 同一份数据输入参考工程算法和新算法，逐项解释弱激励、相位、频段和停止时序造成的差异，不以逐点一致作为验收门槛；
- 使用独立测量或已知注入信号核对幅值、相位和带宽；
- 本机合成数据和脚本化传输不构成 DDS、真实舵机或目标板性能证据。

### 12.5 需求到证据矩阵

| 设计规则 | 首要测试层 | 固定证据 |
| --- | --- | --- |
| 五种波形数学定义 | 算法 GoogleTest | 版本化 fixture、期望 JSON、容差和哈希 |
| 扫频窗口、粗时延和尾段保护 | 算法 GoogleTest | 完整/截断纯时延 fixture |
| STOP 尾样本与唯一封存 | 应用集成 | 事件顺序和输入样本清单 |
| taskId/generation、取消和资源限制 | 应用集成 | 状态序列、错误码和文件集合 |
| v1 DTO、载荷上限和按通道拉取 | WebSocket 契约 | 紧凑 JSON、字节数和数组不变量 |
| 性能页面和旧结果清除 | Vitest | 状态/交互断言和绘图输入 |
| 无丢样前提与真实指标 | 真机验收 | 原始 TXT、时间/流水统计、性能 JSON、独立参考 |

## 13. 实施拆分

建议按以下独立门禁推进：

1. 纯算法值类型、局部同步扫频估计器、五种波形分析器和 golden；
2. 临时捕获、后处理端口、应用协调器、取消和结果 JSON；
3. descriptor 能力、`ApplicationSnapshot.analysis` 摘要、WebSocket v1 `analysisResult` 和契约测试；
4. Web 性能页、过程交互、伯德图和 Vitest；
5. 全量宿主验证、文档事实源同步和真机对照方案。

本次落地已同步以下事实源；后续行为变更仍须保持一致：

- [五层架构](../../design/overview/five-layer-architecture.md)中的算法后处理与应用编排职责；
- [业务调度层契约](../../design/contracts/business-scheduling-layer.md)中的“不由 BIZ 缓存或解释性能样本”边界；
- [WebSocket 前端协议契约](../../design/contracts/websocket-frontend-protocol.md)的追加 DTO、动作门禁和消息顺序；
- [测试规范](../../design/testing/testing-specification.md)的测试清单、验证边界和执行证据；
- 根规则和设计索引中原有的“当前保存原始样本、不计算性能或伯德图”描述。
