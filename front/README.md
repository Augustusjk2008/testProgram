# HWTEST 浏览器遥测控制台

`front/` 是独立的 React 19 + TypeScript + Vite 浏览器前端。它不进入宿主 CMake、不提供后端或 DUT I/O，只通过回环 WebSocket 消费 `hwtest_web` 的应用层快照和样本。

## 启动

先从仓库根目录启动后端：

```powershell
.\hwtest.ps1 -Ui web -WebPort 18765
```

再开一个终端启动前端：

```powershell
Set-Location front
Copy-Item .env.example .env.local
npm install
npm run dev
```

打开 `http://127.0.0.1:5173`。默认连接 `ws://127.0.0.1:18765/ws`；需要改端口时编辑 `.env.local` 中的 `VITE_HWTEST_WS_URL`。

## 当前功能

- 支持深色/浅色主题切换并在浏览器本地保留选择，包含任务、曲线、诊断三页。
- WebSocket 连接后自动读取配置目录并加载默认测试项，正常操作路径为连接设备后开始测试；加载失败时可手动重试。
- 全局运行控制条贯穿所有页面，支持单次和 PC 周期测试。PC 周期只发送一次 `start`，由 BIZ 按“上一轮完成 → 等待轮间隔 → 再发指令并采反馈”调度。
- 选择 `mbddf.di_read` 后，首页按后端 descriptor 紧凑显示 16 路 DUT DI 激励开关与 `di_state[0]` 回显状态，并保留只读 `di_state[1]` 诊断位图；快速操作会合并并串行发送带 revision 的请求，失败时回滚到后端权威状态，且可一键恢复安全态。
- 设备主动持续回告保留独立运行语义，只有当前测试配置声明支持时才显示对应模式；当前 SYSTEM_STATUS 不支持该模式，后端算法返回 `CapabilityUnsupported`。
- 自动发现样本中的数值字段，允许全部同图、每项一图或自定义图组；横轴固定为采样时间，配置按 SYSTEM_STATUS 通道保存在 `localStorage`。
- uPlot Canvas 绘图；每通道最多保留 50,000 点，样本最多 10 Hz 批量提交到 React/图表，绘制前按像素宽度做 min/max 降采样。诊断事件最多保留 500 条。

## 验证

```powershell
npm test
npm run build
```

`npm run build` 会生成已内联 JS、CSS 和 Geist 字体的 `front/dist/index.html`，可以直接双击打开；该单文件仍需可用的 `hwtest_web` WebSocket 后端才能显示实时数据。前端协议字段和关闭语义以 [WebSocket 前端协议契约](../docs/design/contracts/websocket-frontend-protocol.md) 为准。
