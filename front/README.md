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

- 支持深色/浅色主题切换并在浏览器本地保留选择，包含任务总览、曲线工作台、报文与诊断三页。
- 全局运行控制条贯穿所有页面，支持单次和 PC 周期测试。PC 周期只发送一次 `start`，由 BIZ 按“上一轮完成 → 等待轮间隔 → 再发指令并采反馈”调度。
- 设备主动持续回告仍作为独立模式展示；当前 SYSTEM_STATUS 没有设备流启动/停止命令，因此 UI 说明能力限制，后端算法也返回 `CapabilityUnsupported`。
- 自动发现样本中的数值字段，允许全部同图、每项一图或自定义图组；横轴固定为采样时间，配置按 SYSTEM_STATUS 通道保存在 `localStorage`。
- uPlot Canvas 绘图；每通道最多保留 50,000 点，样本最多 10 Hz 批量提交到 React/图表，绘制前按像素宽度做 min/max 降采样。诊断事件最多保留 500 条。

## 验证

```powershell
npm test
npm run build
```

生产构建输出到已忽略的 `front/dist/`。前端协议字段和关闭语义以 [WebSocket 前端协议契约](../docs/design/contracts/websocket-frontend-protocol.md) 为准。
