# 仓库规范

## 当前事实来源
- `docs/design/overview/five-layer-architecture.md` 与 `docs/design/contracts/hal-interface-protocol.md` 定义了 HAL 合约与架构。
- 代码变更须与上述文档保持一致；当接口或配置结构发生变化时，同步更新文档。

## 模块布局
- 公共 HAL 头文件位于 `src/hal/include/hal/` 下。
- 内部实现位于 `src/hal/src/` 下。
- 本仓库保持无 UI；HAL 模块仅为核心库。

## 代码风格
- 遵循 Qt 5.15 / C++17 规范，使用 `hwtest::hal` 命名空间。
- 注释力求精简且有用：仅说明意图、边界情况或不变量。
- 优先使用小而专注的类与辅助工具，避免大而全的文件。

## API 稳定性
- 将公共 HAL 头文件视为兼容性表面。
- 尽可能在结构体尾部扩展字段，避免破坏枚举语义。
- 若适配器 ABI 发生变更，同步更新 `hal_adapter_abi.h` 与设计文档。

## 构建
- 根构建入口为 `CMakeLists.txt`。
- HAL 模块应继续作为独立的 Qt Core 库进行构建。
