# 仓库规范

最终答复使用中文。

## 范围与事实源

- 本仓库同时维护 Windows/Qt 宿主测试程序与 `dut/` 中的 MB_DDF_v2 产品端软件。根规则覆盖宿主工程，`src/hal/AGENTS.md` 与 `dut/AGENTS.md` 分别覆盖对应子树。
- `dut/` 是产品端软件、产品协议实现和协议 CSV 的唯一可信来源；批准的协议目录固定为 `dut/docs/design/product_protocol_csv/`。
- 仓库外目录、同名副本和历史导入来源均不参与当前设计、构建、测试或验收事实。需要设置 `MB_DDF_PROTOCOL_CSV_DIR` 时，它必须解析到上述仓库内目录；其他取值不形成有效证据。
- 现行事实源按职责划分：
  - 分层与依赖：`docs/design/overview/five-layer-architecture.md`；
  - BIZ、HAL、设备通讯、WebSocket 和日志语义：`docs/design/contracts/`；
  - 当前测试清单、命令和证据边界：`docs/design/testing/testing-specification.md`；
  - 产品端内部设计与当前限制：`dut/docs/design/` 及已核对的 `dut/src/`；
  - 公共 API、CMake 目标、配置和已核对源码共同描述当前实现。
- `docs/history/`、`docs/plan/`、`docs/superpowers/` 与 `dut/docs/plan/` 是历史记录，不作为当前事实源；历史原文不用于推导当前行为。
- 搜索和统计默认过滤 `tmp/`、`build*/`、`cmake-build*/`、`out/` 和 `.git/`。`tmp/helm_control/` 仅作保留资产，不得作为当前产品端或协议事实源，也不得随一般临时内容清理。

## 当前基线与分层

- 当前宿主开发与验证以 `hwtest_web` 和 `front/` 的 Web 链路为主；TUI 和 Qt GUI 只在任务明确要求时同步扩展。
- 当前配置入口以 `configs/mbddf_*.testcfg.json` 的实际扫描结果为准，算法注册表可以保留没有当前配置入口的兼容 ID；不要在入口文档重复维护配置清单和精确协议参数。
- 依赖方向固定为：

```text
浏览器 Web UI / TUI / Qt GUI / batch CLI
  -> hwtest_app_core::TestApplicationController
  -> hwtest_biz
  -> biz::IAlgorithmExecutor（算法层实现）
  -> hwtest_hal
  -> Provider / Adapter
```

- 应用层是组合根，统一管理配置、HAL 会话、算法、BIZ、连续数据和关闭顺序；入口不得绕过控制器。
- BIZ 只负责任务、状态、重试、结果和报告，不解释产品协议或访问硬件。
- 算法层负责产品协议、运行参数语义、流程和判定，并通过 HAL 请求 I/O。
- HAL 负责资源映射、连接、原始 I/O、deadline、错误归一化和已配置安全态，不解释产品字段。
- 日志是旁路模块，不反向驱动业务或硬件。

## 代码与兼容

- 宿主使用 C++17、Qt 5.15 兼容 API 并保持 Qt 6 fallback；DUT 使用 C++20 和 AArch64 Linux 工具链。
- 公共 HAL/BIZ 头、Adapter ABI、配置字段、状态和错误语义属于兼容面；结构体优先尾部扩展，既有枚举值不得静默改义。
- 厂家 Adapter 初始化只接收驱动级配置；设备身份、资源和安全态通过版本化单设备 open projection 传递。
- BIZ 保持硬件无关；产品协议和运行参数校验不得上移到 UI、BIZ 或 HAL。
- 行为缺陷先用回归测试复现，再修改实现。当前任务若只要求文档，不得顺带修改代码、测试、配置或脚本。

## 文档规则

- 一个概念只保留一个主定义：总览写职责，契约写语义，DUT 设计写产品端内部实现，测试规范写验证边界，入口文档只写操作和链接。
- 文档按当前实现描述；已知实现缺陷若不修复，明确标为“当前实现限制”，不得改写为目标能力或已验证安全保证。
- 不在 README、AGENTS、用户指南和测试流水账中复制协议字节、完整参数表、硬件点位或测试计数。
- 修改公共 API、配置字段、状态语义、错误码、构建依赖或测试边界时，同步唯一事实源与契约测试。
- 说明性文档改动至少执行当前文档的本地链接/术语检查和 `git diff --check`。

## 本地服务与前端

- `hwtest_web`、Vite 和其他常驻服务必须由用户在当前对话明确授权；常规验证使用会自行退出的测试和 smoke test。
- WebSocket 后端默认监听 `127.0.0.1:18765`。手动运行命令与浏览器入口见根 `README.md` 和 `front/README.md`。
- 修改 `front/` 源码后，在 `front/` 运行 `npm test` 和 `npm run build`。

## 构建与验证

- Windows 推荐入口为 `./hwtest.ps1 test`；正式区分 Debug/Release 证据时使用独立构建树，避免 GoogleTest discovery 被另一配置覆盖：

```powershell
$env:MB_DDF_PROTOCOL_CSV_DIR = (Resolve-Path ".\dut\docs\design\product_protocol_csv").Path

cmake -S . -B build_vs_debug -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build_vs_debug --config Debug --parallel
ctest --test-dir build_vs_debug -C Debug --output-on-failure

cmake -S . -B build_vs_release -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build_vs_release --config Release --parallel
ctest --test-dir build_vs_release -C Release --output-on-failure
```

- `MB_DDF_PROTOCOL_CSV_DIR` 必须保持为仓库内 DUT 协议目录；测试输出应记录提交、配置、命令、跳过项和未验证硬件边界。
- 宿主与 DUT 使用独立构建树和入口。进入 `dut/` 后遵循局部 `AGENTS.md` 与 `README.md`。
- Fake/Mock、Simulator、主机侧测试和交叉构建均不等于目标板、真实 PXI、DDS 舵机、DH 或其他物理台架验收。
- 真实硬件写入、部署和长时运行只在隔离且经用户明确授权的环境执行。

## CodeGraph

- 跨模块修改、架构分析、重构和影响面分析优先使用可用的 CodeGraph 工具；未覆盖细节再用 `rg` 和源码阅读。
- 变更后以测试、日志和 `git diff` 验证；大改动后运行 `codegraph sync .` 刷新索引。
