# 设计文档目录

## 目录分组

```text
docs/design/
  overview/        总览、分层边界、跨层流程
  contracts/       对外接口、协议、契约
  implementation/  当前实现设计和落地说明
  testing/         测试规范、测试设计报告
```

## 文档索引

| 分组 | 文档 | 内容 |
| --- | --- | --- |
| 总览 | `overview/five-layer-architecture.md` | 五层架构、模块边界、跨层依赖 |
| 接口契约 | `contracts/business-scheduling-layer.md` | 业务调度层接口契约 |
| 接口契约 | `contracts/hal-interface-protocol.md` | HAL 对上接口、Adapter ABI、资源和错误映射 |
| 接口契约 | `contracts/log-interface-protocol.md` | 日志模型、来源约定、HAL/Adapter 日志映射 |
| 实现设计 | `implementation/hal-implementation-design-report.md` | 当前 HAL 实现、执行流程、限制、扩展点 |
| 测试 | `testing/testing-specification.md` | 分层测试规范、运行方式 |
| 测试 | `testing/hal-test-design-report.md` | 当前 HAL 测试结构、覆盖矩阵、缺口 |
