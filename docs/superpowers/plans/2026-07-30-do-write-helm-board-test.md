# DO_WRITE 与舵机板级测试实施计划

> **现行事实：** 设计见 `docs/superpowers/specs/2026-07-30-do-write-helm-board-test-design.md`；完成后由架构、协议契约和测试规范承接长期事实。

**目标：** 在 Web 主链路中交付 DO_WRITE 物理闭环和 HELM_BOARD_TEST 手动/全自动板级测试，提供可停止、安全收尾和直观结果。

**架构：** 专用算法执行器通过小型板级夹具接口使用 HAL；应用组合根按规范化运行模式延迟打开 6259/6733；NI Adapter 以型号 profile 支持双板；Web 沿既有 snapshot/sample 契约展示版本化结果。

**技术栈：** C++17、Qt 5.15/Qt 6 fallback、GoogleTest/CTest、NI-DAQmx Fake、React/Vite/Vitest。

---

### 任务 1：锁定协议与测量算法

- [ ] 先添加 DO/舵机协议编解码、PWM 波形测量和结果模型失败测试。
- [ ] 运行聚焦测试并确认失败原因是缺少新行为。
- [ ] 实现最小编解码、PWM 测量、容差与结果 DTO。
- [ ] 运行聚焦测试至通过。

### 任务 2：专用执行器与板级夹具

- [ ] 先添加 DO 五步、方向矩阵、36 个 PWM 点、44 个反馈点、手动零板卡访问、Fail 继续、Error 中断、停止与 AO 清零测试。
- [ ] 运行测试确认 RED。
- [ ] 实现 `DoWriteAlgorithmExecutor`、`HelmBoardTestAlgorithmExecutor` 与 `IBoardTestFixture`。
- [ ] 运行算法测试确认 GREEN，并重构重复流程。

### 任务 3：双 NI 板与配置

- [ ] 在 NI Fake 测试中先锁定 6733 profile、能力和未知型号拒绝。
- [ ] 实现型号感知的 `ni.daqmx`，保持 C ABI v1 与 6259 回归。
- [ ] 新增两份测试配置，修正 DI 真实三路刺激映射，添加 6733 门禁配置。
- [ ] 解析全部 JSON 并运行 NI/HAL 聚焦测试。

### 任务 4：应用层按模式绑定

- [ ] 先添加手动模式不打开板卡、自动模式缺板失败、终态释放与结果快照测试。
- [ ] 扩展运行参数 Schema 的非持久化标记和 Helm 手动参数。
- [ ] 在 `start()` 规范化参数后打开并绑定需要的辅助设备，终态释放。
- [ ] 运行应用与 Web 契约测试。

### 任务 5：板级结果界面

- [ ] 先添加结果防御性解析、导航和非持久化参数测试。
- [ ] 实现板级测试页、矩阵、曲线与最差点摘要。
- [ ] 运行 `npm test` 与 `npm run build`。

### 任务 6：事实源与最终证据

- [ ] 同步架构、HAL/设备通讯契约、根说明与测试规范。
- [ ] 运行 Debug 聚焦/全量测试、Release 构建与测试、前端测试/构建、JSON 解析和 `git diff --check`。
- [ ] 刷新 CodeGraph（如可用）并检查工作树只包含本次变更。
- [ ] 在测试规范记录 Fake/软件证据及真实 PXI 台架待验收项。
