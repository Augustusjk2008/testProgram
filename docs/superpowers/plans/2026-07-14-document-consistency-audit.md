# Repository Documentation Consistency Audit Implementation Plan

> **Execution note:** This plan should be executable either inline in the current session or by a delegated worker. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 更新仓库级 `AGENTS.md`，并在忽略 `tmp/` 与构建目录的前提下消除代码/文档冲突、文档间冲突和不必要的重复定义。

**Architecture:** 以已落地公共 API、CMake 目标和测试注册为代码事实，以五层架构、BIZ/HAL/日志/设备协议契约和测试规范为文档事实源。实现合理时文档跟随代码；发现代码违反严格分层或公共契约时先修代码和回归测试，再同步文档。

**Tech Stack:** Qt 5.15 / Qt 6 Core fallback、C++17、CMake、GoogleTest、CTest、Markdown、PowerShell

---

### Task 1: Refresh Repository Instructions

**Files:**
- Modify: `AGENTS.md`

- [ ] 更新当前模块、产物、公共头、测试入口和命名空间说明。
- [ ] 将 BIZ 严格分层、HAL 独立构建、日志类型边界和文档同步规则写成可执行约束。
- [ ] 明确审查/搜索默认忽略 `tmp/`、`build*/`、`.git/`，并保留 `src/hal/AGENTS.md` 的子树优先级。

### Task 2: Build An Evidence-Based Conflict Matrix

**Files:**
- Inspect: `CMakeLists.txt`, `src/**`, `tests/**`, `docs/**`

- [ ] 用 `rg --files` 生成显式排除 `tmp/`、`build*/`、`.git/` 的审查清单。
- [ ] 对照公共头、实现、CMake 和 CTest，记录带文件行号的代码/文档冲突。
- [ ] 对照 `docs/design/**` 的事实源、实现报告和历史计划，记录文档冲突与重复定义。

### Task 3: Reconcile Design And Implementation Documents

**Files:**
- Modify as required: `docs/design/overview/five-layer-architecture.md`
- Modify as required: `docs/design/contracts/*.md`
- Modify as required: `docs/design/implementation/*.md`
- Modify as required: `docs/design/testing/*.md`
- Modify as required: `docs/design/README.md`

- [ ] 以实际公共 API 和构建依赖修正文档中的签名、默认值、已实现状态和模块职责。
- [ ] 每个跨模块概念只保留一个主定义，其余文档改为简短引用，避免复制接口或规则。
- [ ] 保留合理的未来设计并明确标注“未实现/扩展点”，不得写成当前能力。

### Task 4: Quarantine Superseded Plans

**Files:**
- Modify as required: `docs/plan/*.md`
- Modify as required: `docs/superpowers/plans/*.md`

- [ ] 对仍描述旧式 BIZ 直连 HAL 的历史计划添加“已被替代”状态和现行事实源链接，不重写历史执行内容。
- [ ] 清除会被误认为当前规范的重复验收说明，保留必要的历史背景。

### Task 5: Verify The Repository Facts

**Files:**
- Verify: `AGENTS.md`, `docs/**`, `src/**`, `tests/**`

- [ ] 运行 Markdown 本地链接检查和严格分层关键词扫描，扫描范围显式排除 `tmp/` 与构建目录。
- [ ] 运行 `git diff --check` 并审查最终变更范围。
- [ ] 重新配置并构建 Debug/Release，分别运行完整 CTest；只有实际输出为零失败时才更新完成状态。

