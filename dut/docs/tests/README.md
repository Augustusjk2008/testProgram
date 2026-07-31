# MB_DDF_v2 测试

本文只保留 DUT 测试入口和证据边界。具体用例以当前 CMake 与测试源码为准；产品字段、硬件行为和已知实现限制分别以协议 CSV、产品协议设计和当前 `dut/src/` 为准。

## 快速开始

在 `dut/` 目录运行：

```powershell
# DDS 静态检查与交叉构建
.\tests\test-dds-only.ps1 -BuildOnly

# DDS 构建、部署与目标板执行
.\tests\test-dds-only.ps1
.\tests\test-dds-only.ps1 -TestFilter "RingBuffer*"

# 全部测试目标：只构建，或构建后部署执行
.\tests\test-all.ps1 -BuildOnly
.\tests\test-all.ps1

# 单独部署并执行指定目标
.\tests\test-deploy.ps1 -TestBinaryName MB_DDF_v2_HardwareTests
.\tests\test-deploy.ps1 -TestBinaryName MB_DDF_HW_Tests
.\tests\test-deploy.ps1 -TestBinaryName MB_DDF_HW_Smoke
```

`test-all.ps1` 当前编排以下目标：

| 目标 | 主要边界 |
| --- | --- |
| `MB_DDF_v2_Tests` | DDS 单元与进程内集成 |
| `MB_DDF_v2_HardwareTests` | 目标板 DDS、IPC、压力与性能 |
| `MB_DDF_HW_Tests` | 硬件抽象、产品协议和产品测试服务 |
| `MB_DDF_HW_Smoke` | 默认只读硬件巡检 |

脚本使用独立的 `build/aarch64/tests/<Config>` 构建树，不复用应用画像。目标、筛选参数和部署选项以 `tests/test-deploy.ps1` 的当前帮助及[详细 DDS 测试说明](dds-test-guide.md)为准。

## PC 串口工具测试

Windows PyQt5 工具使用 `test_pyqt/requirements-win7.txt` 锁定依赖。激活对应 Python 环境后运行：

```powershell
$env:QT_QPA_PLATFORM = 'offscreen'
python -m pytest -q .\test_pyqt\tests
python .\tools\generate_product_protocol.py --check `
  .\docs\design\product_protocol_csv
```

`test_pyqt/run.bat`（或 `run.ps1`）是工具运行入口，会拒绝 base/system Python。PC 假后端和界面测试不覆盖宿主 Web 的 `device_stream` 生命周期，也不构成 COM、DDS、舵机或目标板实测。

## 前置条件

- AArch64 GNU 交叉工具链和匹配 sysroot 可用；
- Windows `ssh`、`scp` 可用，目标板部署参数已配置；
- 构建测试时主机能取得项目声明的 googletest 依赖；
- 真实硬件写入只在隔离且经明确授权的目标板执行。

## 证据边界

- 交叉构建成功只证明可生成目标二进制；只有部署并在目标板执行后才形成板端运行证据。
- `MB_DDF_HW_Smoke` 默认只读，不覆盖 COM 回环、DIDO、DH、舵控或 CPU SPI Flash 写入；
  它对 UPDATE image IP version 与 FPGA update state 仅做通信检查和快照读取，不写入或触发
  更新。完整硬件边界见[硬件层详细设计](../design/hardware-layer-architecture.md)。
- PyQt 断言通过若伴随异步 Qt/Matplotlib teardown 异常，必须单独记录，不能称为完全干净门禁。
- 协议资产只使用 `dut/docs/design/product_protocol_csv/`；其他目录的结果不构成当前协议证据。
- 串口、网口、SPI、DH、DDS、舵机和其他硬件结论必须记录目标身份、工具链、命令、日志、退出状态与收尾结果。

宿主 CTest、浏览器测试和跨层证据等级见根仓[测试规范](../../../docs/design/testing/testing-specification.md)。
