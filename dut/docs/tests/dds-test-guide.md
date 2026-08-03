# DDS 测试说明

本文档说明本工程当前测试集的测试内容、环境准备、启动命令、预期结果和异常结果判定。测试遵循 DDS-only、AArch64 交叉编译、目标板执行的原则；Windows 主机只负责编译、上传和收集结果。

## 测试目标

- 验证 `src/MB_DDF/DDS` 的基础数据结构、共享内存、Topic 注册、发布订阅、回调、零拷贝和同步等待行为。
- 在 AArch64 目标板验证 POSIX shared memory、POSIX semaphore、`fork()` 多进程 IPC 和 DDS 生命周期。
- 在目标板输出 DDS 性能指标，包括同进程往返吞吐、零拷贝往返吞吐、等待唤醒延迟和 fork IPC 发布吞吐。
- 不在 Windows 主机执行测试二进制，不新增 host/native 测试路径。

## 测试二进制

### `MB_DDF_v2_Tests`

普通 DDS 单元和集成测试二进制，由 `tests/unit` 下的测试文件组成。

覆盖内容：

- `test_message.cpp`：消息头、消息体、校验和、有效性检查。
- `test_ringbuffer.cpp`：环形缓冲区发布、读取、订阅者状态、覆盖行为、等待唤醒。
- `test_shared_memory.cpp`：POSIX 共享内存创建、打开、映射、读写、信号量获取。
- `test_topic_registry.cpp`：Topic 注册、查找、共享内存布局、容量边界。
- `test_publisher_subscriber.cpp`：同步读、回调订阅、多订阅者、零拷贝、超时等待。
- `test_dds_core.cpp`：DDSCore 初始化、关闭、重启、发布订阅端到端、异常初始化路径。
- `test_semaphore_guard.cpp`：信号量 RAII 守护行为。
- `test_external_endpoint.cpp`：外部端点和端口适配层的 DDS-only 接口行为。

### `MB_DDF_v2_HardwareTests`

目标板实机测试二进制，由 `tests/hardware` 下的测试文件组成。

覆盖内容：

- `test_hardware_smoke.cpp`
  - 目标板存在 `/proc/self/comm`。
  - 目标板挂载 `/dev/shm`。
  - POSIX shared memory 和 semaphore 可创建。
  - DDSCore 可初始化、关闭、重新初始化。
  - 同进程发布订阅可完成一次 payload 往返。

- `test_hardware_ipc.cpp`
  - 父进程发布，fork 子进程订阅并读取。
  - fork 子进程发布，父进程订阅并读取。
  - 子进程超时会被回收，避免测试挂死。

- `test_hardware_stress.cpp`
  - 回调订阅者接收突发消息。
  - 零拷贝消息提交后可被订阅者读取。
  - DDSCore 多轮初始化、发布订阅、关闭后共享状态仍可继续使用。

- `test_hardware_performance.cpp`
  - `SameProcessRoundTripThroughputOnTarget`：64B、1KiB、16KiB payload 的同进程写入和读取往返吞吐。
  - `ZeroCopyRoundTripThroughputOnTarget`：4KiB payload 的零拷贝提交和读取往返吞吐。
  - `WaitForMessageLatencyOnTarget`：订阅者阻塞等待消息时的平均、P95、最大唤醒延迟。
  - `ForkedIpcPublishThroughputOnTarget`：父进程发布、fork 子进程订阅读取时的 IPC 发布吞吐。

性能测试输出指标，不设置固定性能阈值。不同目标板 CPU 频率、负载、内核配置、Debug/coverage 构建都会显著影响数值；这些用例只在功能正确的前提下记录可比较的基线。

## 主机环境准备

Windows 主机需要：

- PowerShell 5.1。
- AArch64 GNU 交叉编译工具链可用。
- CMake 和工程配置的 make 程序可用。
- `ssh`、`scp` 可用。
- `rg` 可用，供 DDS-only 静态检查使用。
- 首次构建时 googletest 源码可获取，或 `.deps` 中已有可用缓存。
- 主机能通过网络访问目标板。

## 目标板环境准备

目标板需要：

- AArch64 Linux 用户态。
- 可通过 `ssh root@<RemoteHost>` 免密登录。
- 目标目录可写、可执行，例如 `/home/sast8/user_tests`。
- `/proc` 可用，至少能读取 `/proc/self/comm`。
- `/dev/shm` 已挂载，并有足够空间。建议至少保留 128MiB 可用空间。
- 支持 POSIX shared memory、POSIX named semaphore、`fork()`、`waitpid()`。
- 运行测试前目标板上没有其它进程正在使用同名 DDS 共享内存。

如怀疑有上次异常中断残留，可在目标板上清理 DDS 测试共享对象：

```powershell
ssh root@192.168.1.29 "rm -f /dev/shm/MB_DDF_V2_SHM /dev/shm/sem.MB_DDF_V2_SHM_sem /dev/shm/mb_ddf_hw_smoke_shm /dev/shm/sem.mb_ddf_hw_smoke_shm_sem"
```

## 启动命令

### 只构建，不部署、不执行

```powershell
.\tests\test-all.ps1 -BuildOnly
```

用途：

- 运行 DDS-only 静态检查。
- 设置 `ENABLE_TESTS=ON`。
- 交叉编译 `MB_DDF_v2_Tests`、`MB_DDF_v2_HardwareTests`、`MB_DDF_HW_Tests`、
  `MB_DDF_DytDebug_Tests` 和 `MB_DDF_HW_Smoke`。
- 确认五个 AArch64 测试二进制存在。

### 全量构建、部署、实机执行

```powershell
.\tests\test-all.ps1 -RemoteHost 192.168.1.29 -RemoteDir /home/sast8/user_tests -SaveResults
```

用途：

- 依次运行两个 DDS 测试二进制、硬件层单元测试和默认只读 XDMA smoke。
- 保存输出到 `tests/results/<timestamp>`。

### 只运行普通 DDS 测试

```powershell
.\tests\test-deploy.ps1 -RemoteHost 192.168.1.29 -RemoteDir /home/sast8/user_tests -TestBinaryName MB_DDF_v2_Tests -SaveResults
```

### 只运行实机测试

```powershell
.\tests\test-deploy.ps1 -RemoteHost 192.168.1.29 -RemoteDir /home/sast8/user_tests -TestBinaryName MB_DDF_v2_HardwareTests -SaveResults
```

### 只运行目标板 DDS 性能测试

```powershell
.\tests\test-deploy.ps1 -RemoteHost 192.168.1.29 -RemoteDir /home/sast8/user_tests -TestBinaryName MB_DDF_v2_HardwareTests -TestFilter "HardwarePerformanceTest.*" -SaveResults
```

### 只运行某类实机测试

```powershell
.\tests\test-deploy.ps1 -TestBinaryName MB_DDF_v2_HardwareTests -TestFilter "HardwareSmokeTest.*"
.\tests\test-deploy.ps1 -TestBinaryName MB_DDF_v2_HardwareTests -TestFilter "HardwareIpcTest.*"
.\tests\test-deploy.ps1 -TestBinaryName MB_DDF_v2_HardwareTests -TestFilter "HardwareStressTest.*"
.\tests\test-deploy.ps1 -TestBinaryName MB_DDF_v2_HardwareTests -TestFilter "HardwarePerformanceTest.*"
```

## 结果文件

使用 `-SaveResults` 时，脚本会创建：

- `tests/results/<timestamp>/test_output.txt`：目标板 gtest 输出。
- `tests/results/<timestamp>/summary.txt`：本轮执行摘要。
- `tests/results/latest.txt`：最近一次结果目录名。

如果使用 `-WithCoverage -SaveResults`，脚本还会尝试收集 coverage 数据并生成 HTML 报告。覆盖率依赖目标板生成 `.gcda`，主机侧还需要 `lcov` 和 `genhtml`。

## 预期结果

### 构建阶段预期

`.\tests\test-all.ps1 -BuildOnly` 成功时应看到：

- `[INFO] Static checks passed`
- `Built target MB_DDF_v2_Tests`
- `Built target MB_DDF_v2_HardwareTests`
- `Built target MB_DDF_HW_Tests`
- `Built target MB_DDF_DytDebug_Tests`
- `Built target MB_DDF_HW_Smoke`
- `[INFO] Build-only mode, skipping deploy`
- `[INFO] All requested test binaries completed successfully`

构建产物应存在：

```powershell
Get-ChildItem -Path .\build\aarch64 -Recurse -File |
    Where-Object { $_.Name -in @("MB_DDF_v2_Tests", "MB_DDF_v2_HardwareTests", "MB_DDF_HW_Tests", "MB_DDF_DytDebug_Tests", "MB_DDF_HW_Smoke") } |
    Select-Object FullName
```

### 目标板执行预期

五个测试二进制都成功时：

- gtest 输出所有用例 `PASSED`。
- `test-deploy.ps1` 输出 `[INFO] All tests PASSED`。
- `test-all.ps1` 输出 `[INFO] All requested test binaries completed successfully`。

性能测试成功时，还会在 gtest 输出中看到类似：

```text
[ PERF ] roundtrip payload=64 bytes ...
[ PERF ] zerocopy payload=4096 bytes ...
[ PERF ] wait_latency messages=256 ...
[ PERF ] forked_ipc_publish payload=256 bytes ...
```

这些数值用于记录和横向比较，不代表固定验收门槛。

## 预期内的失败

以下失败通常属于环境未准备好，不优先判断为 DDS 代码缺陷：

- 主机缺少交叉编译工具链、CMake、make、`rg`、`ssh` 或 `scp`。
- googletest 源码无法获取，且 `.deps` 没有可用缓存。
- 目标板 ping 不通，或 SSH 免密登录失败。
- 目标目录不存在且无法创建，或无执行权限。
- `/dev/shm` 未挂载、权限不足、空间不足。
- 目标板不支持 POSIX shared memory、POSIX named semaphore 或 `fork()`。
- 上次异常中断留下了尺寸不匹配的共享内存对象。
- 性能指标相比上次下降，但所有功能断言仍通过；这可能由 Debug/coverage 构建、CPU 调频、系统负载或后台进程造成。

## 预期外的失败

以下失败更可能表示 DDS 行为需要继续定位：

- DDSCore 初始化、关闭、重新初始化失败。
- 同进程发布订阅读不到刚发布的 payload。
- fork 父子进程间发布订阅超时或 payload 错误。
- 回调订阅在突发发布后完全收不到消息。
- 零拷贝 `begin_message()` 无效、`commit()` 失败，或提交后无法读取。
- `read(timeout)` 在生产者持续发送时持续超时。
- 性能测试中的功能性断言失败，例如消息数量不完整、子进程退出码非 0。

## 定位建议

建议按以下顺序缩小范围：

1. 先运行 `.\tests\test-all.ps1 -BuildOnly`，确认主机构建链路正确。
2. 再运行 `HardwareSmokeTest.*`，确认目标板 POSIX 运行时和 DDS 基础生命周期正确。
3. 如果 smoke 通过，运行 `HardwareIpcTest.*`，确认 fork 后共享内存 IPC 正常。
4. 如果 IPC 通过，运行 `HardwareStressTest.*`，确认突发、零拷贝和反复初始化行为。
5. 最后运行 `HardwarePerformanceTest.*`，记录目标板性能基线。

性能数据需要比较时，尽量保持同一构建配置、同一目标板频率策略、低后台负载和相同远程目录。Debug/coverage 构建下的数值只能作为趋势参考。
