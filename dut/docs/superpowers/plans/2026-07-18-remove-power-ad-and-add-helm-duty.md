# 删除电源/AD 测试并扩展舵控占空比 Implementation Plan

**Goal:** 删除 POWER_SWITCH 与 AD_READ 双端测试入口，将 `value_YX` 并入电气健康响应，并让舵控板级四路 PWM 支持 0%..100% 整数占空比。

**Architecture:** 产品协议继续使用固定 53 字节帧。删除两组命令 CSV；电气健康复用响应保留区传输 `value_YX`。舵控 `07/02` 保持方向位在 B9 bit4..7，B9 bit0..3 改为必须为零的兼容保留位，B10..13 传四路百分比；响应保留 raw duty/peak，并用四个匹配位报告精确回读结果。

**Tech Stack:** C++20/AArch64 交叉构建、CSV 生成协议描述、Python 3.8/PyQt5。

---

### Task 1: 固化协议与回归预期

- 更新 C++/Python 协议测试，预期目录从 36 份 CSV 变为 32 份。
- 删除 POWER_SWITCH、AD_READ 请求/响应 CSV。
- 在电气健康响应 B31..32 新增 `value_YX:S16F`。
- 将舵控请求改为 B9 兼容保留位/方向位和 B10..13 四路百分比，响应低四位改为 duty 匹配位。

### Task 2: 修改板端处理

- 删除电源与独立 AD 的服务映射、分发、处理器和辅助函数。
- 电气健康读取 XADC `value_YX` 原始 ADC code 并写入新字段。
- 舵控校验百分比 0..100，以 64 位整数换算 raw duty，保持方向先写、duty 后写和现有使能/回读校验。

### Task 3: 修改 PC 工具

- 删除电源、AD 导航页和“执行全部”条目。
- 电气健康增加“引信检测遥测”，判读位只显示 bit0“BC激活好”。
- 舵控四路 PWM 改为 0..100% 数值控件，发送新字段，并根据 raw duty/peak 显示实际占空比。

### Task 4: 同步文档与验证

- 更新 README、AGENTS、设计、使用和协议文档，删除旧命令语义。
- 运行协议生成检查、Python 测试、AArch64 `hw_test_debug`/`hw_tests` 交叉构建与静态差异检查。
- 目标板测试若当前连接可用，运行相关聚焦测试；否则明确记录未执行。
