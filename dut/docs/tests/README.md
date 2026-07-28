# MB_DDF_v2 测试

板端测试分为 DDS 单元/集成测试、目标板 DDS 实机测试、硬件层单元测试和 XDMA
smoke test；另有一类 Windows PC 串口工具测试。Windows 主机交叉编译 C++，AArch64
目标板执行 C++ 二进制，PC 的 PyQt5 测试使用下述固定 Python 环境。

## 快速开始

```powershell
# 静态检查 + 构建
.\tests\test-dds-only.ps1 -BuildOnly

# 只交叉构建 DDS 测试目标
.\build.ps1 dds_tests

# 构建 + 部署 + 运行
.\tests\test-dds-only.ps1

# 只跑部分 gtest
.\tests\test-dds-only.ps1 -TestFilter "RingBuffer*"
.\tests\test-dds-only.ps1 -TestFilter "DDSCore*"

# 全量构建，不部署不执行
.\tests\test-all.ps1 -BuildOnly

# 全量构建 + 部署 + 实机执行
.\tests\test-all.ps1

# 只运行实机测试二进制
.\tests\test-deploy.ps1 -TestBinaryName MB_DDF_v2_HardwareTests

# 只运行目标板 DDS 性能测试
.\tests\test-deploy.ps1 -TestBinaryName MB_DDF_v2_HardwareTests -TestFilter "HardwarePerformanceTest.*"

# 产品协议和硬件测试服务单元测试
.\tests\test-deploy.ps1 -TestBinaryName MB_DDF_HW_Tests `
  -TestFilter 'ProductProtocol*:*HardwareTestService*'

# 惯测协议、COM4 配置、payload 映射和主动反馈
.\tests\test-deploy.ps1 -TestBinaryName MB_DDF_HW_Tests `
  -TestFilter 'ProductProtocolTest.*Imu*:HardwareTestServiceTest.*Imu*:HardwareTestProviderTest.*Imu*'
```

`test-all.ps1` 依次覆盖 `MB_DDF_v2_Tests`、`MB_DDF_v2_HardwareTests`、
`MB_DDF_HW_Tests` 和 `MB_DDF_HW_Smoke`。前两者使用 DDS-only 测试配置，后两者使用完整
硬件测试配置；两种配置都在 `build/aarch64/tests/Debug` 重新显式配置，不复用应用画像。

## DDS 单元与集成测试

- `test_message.cpp`
- `test_ringbuffer.cpp`
- `test_shared_memory.cpp`
- `test_topic_registry.cpp`
- `test_publisher_subscriber.cpp`
- `test_dds_core.cpp`
- `test_semaphore_guard.cpp`
- `test_external_endpoint.cpp`
- `test_gateway_envelope.cpp`
- `test_domain_gateway.cpp`

## 实机测试

- `hardware/test_hardware_smoke.cpp`
- `hardware/test_hardware_ipc.cpp`
- `hardware/test_hardware_stress.cpp`
- `hardware/test_hardware_performance.cpp`

## 硬件层测试

- `hw_unit/test_hw_os.cpp`
- `hw_unit/test_hw_transport.cpp`
- `hw_unit/test_*_device.cpp`
- `hw_unit/test_spi_flash_device.cpp`：验证永久关闭 `#HOLD` 使用两个独立事务 `06h`、`B1h EFh FFh`。
- `hw_unit/test_xadc_device.cpp`：验证 `Data[15:4]`、`value_YX` 定标、`5V_JS=0x240` 和局部 `0x260` 只读访问。
- `hw_unit/test_dds_adapter.cpp`
- `hw_unit/test_product_protocol.cpp`
- `hw_unit/test_hardware_test_service.cpp`
- `hw_unit/test_hardware_test_provider.cpp`
- `hw_unit/test_system_test_provider.cpp`
- `hardware/test_mb_ddf_hw_smoke.cpp`

`MB_DDF_HW_Smoke` 默认在目标板只读 XADC `0x150000` 的 `value_YX`，并在
PCIe Flash 新基址 `0x160000` 读取控制器状态和时钟分频；不触发 XADC 配置或 Flash
Read/Program/Erase。

## 脚本说明

- `test-dds-only.ps1`：先做静态检查，再调用 `test-deploy.ps1`。
- `test-deploy.ps1`：使用独立的 `build/aarch64/tests/<Config>` 目录构建指定测试二进制，
  可选部署到目标板执行。
- `test-all.ps1`：全量入口，先做 DDS-only 静态检查，再依次调用 `test-deploy.ps1` 运行
  `MB_DDF_v2_Tests`、`MB_DDF_v2_HardwareTests`、`MB_DDF_HW_Tests` 和
  `MB_DDF_HW_Smoke`。
- `test-deploy.ps1 -TestBinaryName <name>`：选择部署和执行的测试二进制，默认 `MB_DDF_v2_Tests`。

详细说明见 [DDS 测试说明](dds-test-guide.md)。

## 前置条件

- AArch64 交叉编译工具链可用。
- Windows `ssh`、`scp` 可用。
- 目标板已配好免密 SSH。
- 若要编测试，主机需能拉取 googletest 源码。

## PC 串口工具测试

PC 端 `test_pyqt` 是 Windows 例外测试入口，使用固定 PyQt 环境和
`QtSerialPort` 假后端测试物理帧、产品协议目录、硬件测试会话、发送队列和协议专用
多标签页界面：

```powershell
$Py = 'C:\Users\JiangKai\.conda\envs\pyqt5_env\python.exe'
$env:QT_QPA_PLATFORM = 'offscreen'
& $Py -m pytest -q .\test_pyqt\tests
```

协议 CSV 在 PC 和板端之间共享。提交前还应单独验证 37 份布局：

```powershell
& $Py .\tools\generate_product_protocol.py --check `
  .\docs\design\product_protocol_csv
```

产品协议测试覆盖 1..255 字节数据段（当前 48/123/232）、小端字段、默认值、非零 RESERVED 拒绝、非有限
F32 拒绝、请求序号回显和回绕、无双重物理组帧、普通响应、错误响应、DH 多帧和舵反馈路由。
当前 7 个 DUT 源级 IMU 用例还覆盖 `09/10`/`09/01`/`09/11` 命令号、COM4 event 3 与
921600/8E1 配置、59 字节 payload 的 12 个 F32 全映射、所有 F32 位置的 NaN/Inf 拒绝、
START/STOP ACK 序号、主动反馈发送序号、空闲不消耗序号和终止错误反馈。
界面测试覆盖顶部紧凑连接状态栏，以及“连接与日志”“串口回显”和 11 个硬件测试页
组成的西侧纵向标签；还覆盖每页参数序列化、“结果 / 原始字段”视图、响应只更新对应页、
执行全部采用各页当前参数、停止、彩色日志和微软雅黑。复合流程重点验证 DH 默认
50 次/2500 us、2500 us 下限、逐帧真实间隔、多帧报告选择查看、完整去重文本保存，
以及每页“连续”请求在终态后 200 ms 再次发送、停止时取消待发轮次，电气健康连续响应
的完整 UTF-8-SIG TSV 保存，HELM_START/HELM_STOP ACK 不覆盖舵反馈曲线、TIMER_STOP ACK 不覆盖
START 阶段的抖动统计。界面只区分执行状态，不根据测量值设置硬件通过阈值。

板端协议测试还覆盖请求序号回显、DH `请求序号+i`、采样后立即发送且串口耗时不额外叠加
到回告间隔、发送锁下的 `Busy` 同帧有界流控、
UDP 固定自环端点、SPI 仅 JEDEC LOOP/拒绝任意 ECHO、DH 三项使能和状态映射、舵控 rad
相位与可配置时长扫频公式、通道位图、27/41 字节帧和最多 5 样本打包，以及
SYSTEM_STATUS 的 CPU 计数解析和 XDMA PCI BDF 事实链。板端 `SYSTEM_STATUS` 当前直接读取
`center_thermal` hwmon、`/proc/uptime` 和 XDMA PCI BDF；`net_init_time` 按当前实现固定为
`0 s`，K7 温度直接读取 XADC 局部 `0x200`。串口、网口、SPI、DH 和舵控硬件仍需在已隔离
目标板上联调，单元测试不会把缺失映射视作成功。

上述 IMU 与舵机 DDS bridge 用例是源级/交叉构建证据，不等于 COM4、DDS 或舵机真机验收；
当前 smoke 不注入惯测 payload，也不运行舵控程序。宿主 `device_stream` 的
START/反馈/STOP、至少一帧通过、单次 STOP 和 UTF-8-SIG/TSV 固定列保存证据位于根仓
算法/应用测试。`test_pyqt` 当前没有 IMU/舵机设备流会话，不得把旧工具测试写成该功能已覆盖。

运行工具使用 `test_pyqt\run.bat`（或 `run.ps1`）；该入口会拒绝 base/system
Python，且不会改变 AArch64 C++ 的构建和部署路径。
