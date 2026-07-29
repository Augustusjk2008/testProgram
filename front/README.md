# HWTEST 浏览器遥测控制台

`front/` 是独立的 React 19 + TypeScript + Vite 浏览器前端。它不进入宿主 CMake、不提供后端或 DUT I/O，只通过回环 WebSocket 消费 `hwtest_web` 的应用层快照、样本和只读分析结果。

## 启动

先从仓库根目录启动后端：

```powershell
.\hwtest.ps1 -Ui web -WebPort 18765
```

再开一个终端启动前端：

```powershell
Set-Location front
Copy-Item .env.example .env.local
npm ci
npm run dev
```

打开 `http://127.0.0.1:5173`。默认连接 `ws://127.0.0.1:18765/ws`；需要改端口时编辑 `.env.local` 中的 `VITE_HWTEST_WS_URL`。

## 当前功能

- 支持深色/浅色主题切换并在浏览器本地保留选择，包含任务、曲线、性能、诊断四页。
- WebSocket 连接后自动读取配置目录并加载默认测试项，正常操作路径为连接设备后开始测试；加载失败时可手动重试。
- 全局运行控制条贯穿所有页面，支持单次和 PC 周期测试。PC 周期只发送一次 `start`，轮间隔可为 `0–3,600,000 ms`；`0` 表示上一轮完整收发结束后立即开始下一轮，仍由 BIZ 严格串行调度。轮数 `0` 仍表示持续运行。
- BUS LOOP 可选择 COM1/COM2/COM4 和内部回环次数；BUS ECHO 只提供 PC 周期模式，每轮完成固定 114 字节往返。页面只消费 `echo_bytes`、`mismatch_count` 等紧凑摘要，不把 114 个字节字段加入实时曲线。
- 连接时若 `hello.capabilities.telemetryBatch` 明确声明批传能力，浏览器会在读取配置和自动加载前协商 `setTelemetryDelivery({mode:"batch"})`；协商失败或旧服务端未声明能力时安全保持旧的单条 `sample` 模式。
- 选择 `mbddf.di_read` 后，首页按后端 descriptor 紧凑显示 16 路 DUT DI 激励开关与 `di_state[0]` 回显状态，并保留只读 `di_state[1]` 诊断位图；快速操作会合并并串行发送带 revision 的请求，失败时回滚到后端权威状态，且可一键恢复安全态。
- 设备主动持续回告保留独立运行语义，只有当前测试配置声明支持时才显示对应模式；当前 SYSTEM_STATUS 不支持该模式，后端算法返回 `CapabilityUnsupported`。
- 自动发现样本中的数值字段，允许全部同图、每项一图或自定义图组；横轴固定为采样时间，曲线配置优先按 `configId`、缺失时按 `algorithmId` 隔离保存在 `localStorage`。
- uPlot Canvas 绘图；浏览器只保留 50,000 点有界环形缓存，批次内样本一次摄入并最多约 10 Hz 提交到 UI。图表直接扫描环形缓存，按字段、像素桶保留极小/极大值与首尾点，不先复制完整窗口；uPlot 实例持续复用并只更新数据。诊断事件最多保留 500 条。
- 运行控制条显示本次累计接收、当前缓存、缓存淘汰数和样本序号状态。批次会严格校验安全整数序号、长度、首尾关系和单一任务；缺口、重复、倒序或重连后无历史重放都会标为不完整。浏览器展示缓存淘汰不等于健康连接中的网络丢样，连续测试的全量事实源仍是后端启用保存后生成的 TXT。
- `mbddf.helm_stream` descriptor 声明 `postRunAnalysis.supported` 时启用“性能”导航。页面读取 `snapshot.analysis` 的小型摘要，按 `{taskId, analysisGeneration}` 缓存四个通道的 `analysisResult`；新身份会清空缓存，迟到 reply 不覆盖当前结果，读取动作不占全局 `busyAction`。旧服务端缺字段时安全回退为不支持分析。
- 性能页只展示后端 `analysisResult` 的通道摘要和伯德投影，不从实时 `SampleBuffer` 计算性能。扫频图使用 uPlot 对数频率轴、`spanGaps=false` 的 `null` 空洞、幅相同步游标；Hz/rad/s 只改变显示，不改变后端保存的 Hz 数据。
- 用户手动 STOP 后，页面首次观察到当前 `{taskId, analysisGeneration}` 进入 `queued` 或后续分析状态时自动跳到性能页一次；用户离开页面不会取消分析，也不会被后续进度强制跳回。分析期间新会话写按钮按后端门禁状态禁用，真正断线、`disconnect` 或 `quit` 会触发后端协作取消。

## 验证

```powershell
npm test
npm run build
```

`npm run build` 会生成已内联 JS、CSS 和 Geist 字体的 `front/dist/index.html`，可以直接双击打开；该单文件仍需可用的 `hwtest_web` WebSocket 后端才能显示实时数据。前端/算法自动化与模拟消息不构成 DDS、真实舵机或目标板性能证据；真机仍需在隔离、明确授权的台架保存波形参数、STOP 时机、原始 TXT、性能 JSON、前端截图与独立参考量。前端协议字段和关闭语义以 [WebSocket 前端协议契约](../docs/design/contracts/websocket-frontend-protocol.md) 为准。
