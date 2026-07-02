# 仓库规范

## 事实源
- `docs/design/overview/five-layer-architecture.md`
- `docs/design/contracts/hal-interface-protocol.md`
- `docs/design/testing/testing-specification.md`
- `src/hal/AGENTS.md` 适用 `src/hal/` 子树

## 当前结构
- 根构建入口: `CMakeLists.txt`
- HAL 产物: `src/hal/` 下的 `hwtest_hal` 静态 Qt Core 库
- 公共头: `src/hal/include/hal/`
- 内部实现: `src/hal/src/`
- 测试: `tests/`，以 GoogleTest 为主，含 HAL 夹具和单测

## 约束
- 遵循 Qt 5.15 / C++17，命名空间用 `hwtest::hal`
- 当前仓库只落地 HAL 核心库和测试，不引入 UI
- 公共 HAL 头视为兼容面；接口或配置结构变更时同步更新设计文档
- 结构体尽量尾部扩展；ABI 变更时同步更新 `hal_adapter_abi.h`

## 构建
- `CMakeLists.txt` 先找 Qt5 Core，失败后转 Qt6 Core
- `src/hal` 必须保持可独立构建
- `tests` 通过 `BUILD_TESTING` 控制
