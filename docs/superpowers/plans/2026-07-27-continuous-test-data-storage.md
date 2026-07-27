# Continuous Test Data Storage Implementation Plan

> 本文是已完成的历史执行记录，保留当时仅覆盖 `pc_periodic` 的原始范围；当前三种运行模式边界和连续数据保存事实以 [BIZ 调度契约](../../design/contracts/business-scheduling-layer.md) 与 [WebSocket 前端契约](../../design/contracts/websocket-frontend-protocol.md) 为准。

> **Execution note:** This plan should be executable either inline in the current session or by a delegated worker. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 Web 端 `pc_periodic` 连续测试增加可选的后端 TXT 数据保存，保持单次测试永不保存，并兼容 PyQt 连续采集文件的 UTF-8-SIG/元数据/TSV 结构。

**Architecture:** 浏览器只在 `start` 请求中发送 `saveData` 布尔值，WebSocket 边界做严格类型与白名单校验，`TestApplicationController` 强制仅 `pc_periodic` 可启用。应用层内部 recorder 将每条完整样本增量写入临时 TSV，终态时补齐元数据并原子生成 `.txt`；列来自配置 descriptor，而不是前端曲线选择。

**Tech Stack:** C++17、Qt 5.15/Qt 6 fallback、Qt WebSockets、React 19、TypeScript、Vitest、GoogleTest。

---

### Task 1: 固化应用层数据文件契约

**Files:**
- Create: `src/app/src/continuous_data_recorder.h`
- Create: `src/app/src/continuous_data_recorder.cpp`
- Modify: `src/app/CMakeLists.txt`
- Test: `tests/app/test_application_controller_test.cpp`

- [x] **Step 1: 写连续保存回归测试**

  用本机 UDP peer 执行不限轮次的电气健康 `pc_periodic`，接收两轮后手动停止，传入 `saveData=true`，断言只生成一份 `ElectricalHealth_data_*.txt`，文件以 UTF-8 BOM 开始，包含 `started_at`、`finished_at`、`final_status=用户停止`、`sample_count=2`、`repeat_delay_ms=1000`，并使用 PyQt 的固定电气健康 TSV 表头。再以 `single + saveData=true` 执行一次，断言目录中不生成 TXT。

- [x] **Step 2: 实现增量 recorder**

  定义以下内部接口并让每个样本立即写入 `.partial` 文件：

  ```cpp
  class ContinuousDataRecorder {
  public:
      ActionResult begin(const QString& directory,
                         const TestDescriptor& descriptor,
                         int intervalMs,
                         quint64 maxCycles);
      void setTaskId(const QString& taskId);
      ActionResult append(const ApplicationSample& sample);
      ActionResult finish(const QString& finalStatus,
                          const QString& finalDetail);
      void cancel();
      bool active() const;
      QString outputPath() const;
  };
  ```

  电气健康输出完全采用参考列：`report_index`、`sample_time_us`、`seq`、`response_status`、`err_code`、十路电压、`activate_bits`、`bc_activate_good`。其他配置使用同一前五列和 descriptor 中除 `status/err_code` 外的全部固定测量列。

- [x] **Step 3: 原子完成文件**

  终态使用 `QSaveFile` 写入 BOM、元数据和 `.partial` 中的 TSV，再提交并删除临时文件；失败时保留 `.partial` 并返回 `data_storage` 错误，不在内存无限缓存连续样本。

### Task 2: 接入统一应用控制器

**Files:**
- Modify: `src/app/include/app/test_application_controller.h`
- Modify: `src/app/src/test_application_controller.cpp`
- Modify: `configs/mbddf_pc_hal.json`
- Modify: `.gitignore`

- [x] **Step 1: 追加 DTO 字段**

  在 `TestRunOptions` 尾部增加 `bool saveData = false`；在 `ApplicationSnapshot` 尾部增加 `dataSaveEnabled`、`dataFilePath`、`dataSaveError`，保持既有公共字段语义不变。

- [x] **Step 2: 强制运行模式边界**

  `start()` 仅当 `mode == PcPeriodic && options.saveData` 时启动 recorder；`single` 和 `device_stream` 即使收到 `saveData=true` 也保持不保存。保存目录读取后端 HAL 配置 `dataStorage.directory`，相对路径按 HAL 配置文件目录解析。

- [x] **Step 3: 接入样本与终态**

  在 `sampleProduced` 投影为 `ApplicationSample` 后先交给 recorder，再决定是否向 UI 转发；在 `Finished/Error/Idle-after-stop` 时完成文件，并将路径或错误写回快照。`shutdown()` 对仍活动的 recorder 做明确收尾。

### Task 3: 扩展 WebSocket 契约与浏览器控制

**Files:**
- Modify: `src/app/web/web_socket_frontend_server.cpp`
- Modify: `src/app/web/web_protocol.cpp`
- Modify: `tests/app/web_controller_integration_test.cpp`
- Modify: `tests/app/web_protocol_test.cpp`
- Modify: `front/src/shared/protocol.ts`
- Modify: `front/src/shared/ws/HwtestClient.test.ts`
- Modify: `front/src/features/session/RunControlBar.tsx`
- Modify: `front/src/index.css`

- [x] **Step 1: 严格解析 `saveData`**

  把 `saveData` 加入 `start` 参数白名单，仅接受 JSON boolean；字符串、数字或未知字段返回 `invalid_envelope`。快照追加投影保存状态和后端文件路径。

- [x] **Step 2: 增加连续保存勾选框**

  仅在 `pc_periodic` 参数区显示“保存完整数据”复选框，运行中禁用；开始单次测试时显式发送 `saveData:false`。该值可保存在既有运行选项 localStorage 中，但不得读取曲线字段选择。

- [x] **Step 3: 锁定前端请求**

  Vitest 断言周期请求携带 `saveData:true`，单次请求归一化为 `saveData:false`；构建检查 TypeScript DTO 与 C++ WebSocket 字段一致。

### Task 4: 同步事实源并验证

**Files:**
- Modify: `docs/design/contracts/websocket-frontend-protocol.md`
- Modify: `docs/design/overview/five-layer-architecture.md`
- Modify: `docs/design/testing/testing-specification.md`

- [x] **Step 1: 更新契约**

  记录 `saveData` 仅适用于 `pc_periodic`、客户端不得传路径、后端保存目录、UTF-8-SIG 元数据加 TSV 格式、完整 descriptor 字段与曲线选择无关、单次永不保存。

- [x] **Step 2: 执行验证**

  运行 `cmake --build build_vs --config Debug --parallel`，随后针对 `TestApplicationControllerTest`、`WebProtocolTest`、`WebSocketControllerIntegrationTest` 执行 CTest；在 `front/` 运行 `npm test` 和 `npm run build`。

- [x] **Step 3: 静态与索引检查**

  运行 `git diff --check`、前端占位资源扫描，并在大改动完成后执行 `codegraph sync .`；将实际新增测试定义数和最新验证结果写回测试规范。
