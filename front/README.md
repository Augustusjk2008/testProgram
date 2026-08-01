# HWTEST 浏览器遥测控制台

`front/` 是独立的 React 19 + TypeScript + Vite 浏览器前端。它不进入宿主 CMake、不提供后端或 DUT I/O，只通过回环 WebSocket 消费 `hwtest_web` 的应用层快照、样本、只读分析结果和版本化配置文档；该 WebSocket 是宿主控制面，不是产品通讯协议。

[`dut/`](../dut/README.md) 是唯一可信的产品端软件来源，`dut/docs/design/product_protocol_csv/` 是唯一批准的产品协议快照。仓库外副本不构成产品事实或验证证据。

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

打开 `http://127.0.0.1:5173`。默认连接 `ws://127.0.0.1:18765/ws`；需要改后端端口时编辑 `.env.local` 中的 `VITE_HWTEST_WS_URL`。

## 当前功能

- 连接后读取后端配置和 descriptor，提供配置选择、设备准备、配置声明支持的运行模式、算法参数编辑、遥测曲线和诊断。
- `mbddf.di_read` 总览页按当前 descriptor 显示已配置的 DI 激励、回显和诊断；通道数量与标识始终以当前快照为准。
- 支持串口回显辅助端口、DH 参数、数字量输出、板级结果和连续数据展示；设备 I/O、数据保存和安全动作都由后端负责。
- descriptor 声明后处理能力时显示性能页；页面只读取后端 `analysisResult`，不从实时缓存计算权威性能结果。
- “配置”页通过 `configCatalog`、`configDocument` 和带 `expectedRevision` 的 `saveConfig` 请求读取及保存后端批准的配置投影：可编辑测试配置目录启停/排序、testcfg 常用字段与 `executionConfig` JSON，以及 MB_DDF 工位 JSON。冲突和服务端验证错误会保留草稿并显示在页面中。
- 主题、运行参数和图表工作区会使用浏览器本地存储；它们是浏览器偏好或本次运行参数，不是 testcfg 持久化。损坏存储、快照防御、隐藏参数和分析结果重试等当前限制见[WebSocket 前端协议契约的当前实现限制](../docs/design/contracts/websocket-frontend-protocol.md#71-当前浏览器实现限制)。

请求字段、能力协商、兼容、连接和关闭语义只以[WebSocket 前端协议契约](../docs/design/contracts/websocket-frontend-protocol.md)为准；产品协议和硬件证据边界分别见[设备通讯协议契约](../docs/design/contracts/device-communication-protocol.md)与[测试规范](../docs/design/testing/testing-specification.md)。

## 验证

```powershell
npm test
npm run build
```

`npm run build` 生成已内联 JS、CSS 和 Geist 字体的 `front/dist/index.html`，可直接打开，但仍需可用的 `hwtest_web` 后端才能显示实时数据。前端自动化和模拟消息只证明浏览器控制面，不构成 DUT、真实舵机、PXI 或目标板验收。
