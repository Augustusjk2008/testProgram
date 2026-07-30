# testProgram

本仓库维护 Windows/Qt 宿主测试程序，以及 `dut/` 中的 MB_DDF_v2 产品端软件。`dut/` 是产品端软件和产品协议 CSV 的唯一可信基线，批准的协议快照位于 `dut/docs/design/product_protocol_csv/`；仓库外目录及其副本不构成产品事实、协议基线或验证证据。

当前宿主开发与验证以 `hwtest_web` 和 `front/` 组成的 Web 链路为主。配置、产品协议、运行模式和生命周期见[设备通讯协议契约](docs/design/contracts/device-communication-protocol.md)，WebSocket 动作和关闭语义见[WebSocket 前端协议契约](docs/design/contracts/websocket-frontend-protocol.md)，测试与硬件证据见[测试规范](docs/design/testing/testing-specification.md)。

## 快速开始

从仓库根目录查看脚本帮助或执行一次自退出验证：

```powershell
.\hwtest.ps1 help
.\hwtest.ps1 ports
.\hwtest.ps1 test
```

手动启动 Web 链路需要两个终端：

```powershell
# 终端 1：回环 WebSocket 后端
.\hwtest.ps1 -Ui web -WebPort 18765

# 终端 2：浏览器前端
Set-Location front
npm ci
npm run dev
```

随后打开 `http://127.0.0.1:5173`。后端只监听 `127.0.0.1`，默认 WebSocket 地址为 `ws://127.0.0.1:18765/ws`，不提供 HTTP 或静态文件服务。浏览器启动、页面能力和当前限制见[前端说明](front/README.md)。

## 其他入口

```powershell
.\hwtest.ps1 -Ui tui
.\hwtest.ps1 -Ui gui
.\hwtest.ps1 run -Port COM7
```

TUI 的命令、状态和当前限制见[TUI 使用指南](docs/user/tui-usage-guide.md)。GUI、TUI、runner 和 WebSocket 后端共享 `TestApplicationController`；入口文档不重复产品协议或硬件流程。

## 文档导航

- [设计文档目录](docs/design/README.md)：当前事实源索引。
- [五层架构](docs/design/overview/five-layer-architecture.md)：分层、依赖和生产 I/O 归属。
- [HAL 契约](docs/design/contracts/hal-interface-protocol.md)：资源、Adapter、deadline、安全态和夹具边界。
- [设备通讯协议契约](docs/design/contracts/device-communication-protocol.md)：CSV、帧、算法配置和产品流程。
- [WebSocket 前端协议契约](docs/design/contracts/websocket-frontend-protocol.md)：浏览器控制面协议。
- [测试规范](docs/design/testing/testing-specification.md)：当前测试清单、命令和证据等级。
- [DUT 说明](dut/README.md)：产品端构建、运行和当前实现限制。

Fake/Mock、Simulator、浏览器自动化、主机侧测试和交叉构建都不等于真实 COM、DH、DDS 舵机、PXI 或目标板验收。真实硬件操作必须在隔离且经明确授权的环境执行。
