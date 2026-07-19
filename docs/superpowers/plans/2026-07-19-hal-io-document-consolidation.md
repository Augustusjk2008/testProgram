# HAL I/O Architecture Documentation Consolidation Implementation Plan

> **Status:** Completed on 2026-07-19. This is an execution record, not a current interface fact source; use `docs/design/` contracts and testing specification for current behavior.
>
> **Superseding decision:** The later user decision designates the current contents of `H:/Resources/RTLinux/Demos/MB_DDF_v2/docs/design/product_protocol_csv` as the approved CSV baseline. References below to the 32/36 mismatch record the then-stale test expectation; current facts are defined by the device communication contract. Manifest/hash reproducibility remains unimplemented.
>
> **Execution note:** This plan should be executable either inline in the current session or by a delegated worker. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Consolidate the repository documentation around the rule that all production DUT/test-equipment I/O crosses HAL, while accurately separating current implementation, target contracts, and unimplemented extensions.

**Architecture:** BIZ remains hardware-free and invokes `IAlgorithmExecutor`; algorithms own product protocol construction, parsing, sequencing, and verdicts; HAL owns production resource routing, connection objects, raw I/O, deadlines, normalized errors, and physical safe-state cleanup. `[Target, unimplemented]` HAL routes each configured device by explicit `providerId` to a Qt standard-API provider, vendor Adapter, or Mock provider. Current code only provides an optional `HalSerialTransport` bridge and the fixed `CAbiAdapter -> MockAdapter` HAL path; successful `SYSTEM_STATUS` tests still use a direct Simulator. TCP/UDP and Qt serial providers remain unfrozen target extensions.

**Tech Stack:** Markdown, Qt 5.15/C++17 repository contracts, CMake/CTest source facts, PowerShell verification.

---

### Task 1: Refresh Governance, Index, And Architecture Overview

**Files:**
- Modify: `AGENTS.md`
- Modify: `docs/design/README.md`
- Modify: `docs/design/overview/five-layer-architecture.md`

- [x] **Step 1: Correct current repository facts**

Record `src/algorithm`, `tests/algorithm`, `hwtest_algorithm_mbddf`, the implemented `SYSTEM_STATUS` vertical slice, the absence of UI/Qt SerialPort/Qt Network/real vendor integration, and the unresolved mismatch between 32 visible external CSV files and the test source's 36-definition expectation.

- [x] **Step 2: State the unique cross-layer rule once**

Define production I/O as communication or hardware access involving the DUT or test equipment. Keep configuration, report, and log file I/O outside this rule. State the flow as `UI -> BIZ -> IAlgorithmExecutor -> HAL -> provider/backend`, with Qt standard API, vendor Adapter, and Mock as HAL-internal backend categories rather than new architectural layers.

- [x] **Step 3: Remove duplicated contract detail from the overview**

Replace copied API, test-count, logging-map, and implementation-detail sections with concise responsibilities and links to their unique contract or implementation fact source.

### Task 2: Reconcile BIZ, HAL, Protocol, And Logging Contracts

**Files:**
- Modify: `docs/design/contracts/business-scheduling-layer.md`
- Modify: `docs/design/contracts/hal-interface-protocol.md`
- Modify: `docs/design/contracts/device-communication-protocol.md`
- Modify: `docs/design/contracts/log-interface-protocol.md`

- [x] **Step 1: Keep BIZ hardware-free**

Remove claims that algorithms own concrete Socket objects or physical safe-state execution. Preserve algorithm responsibility for protocol/test orchestration and HAL lifecycle requests, while assigning concrete connections, raw I/O, deadlines, and physical cleanup to HAL. Clarify that BIZ validates safety configuration structure/ranges only as schema data and never enforces physical output limits.

- [x] **Step 2: Define HAL provider routing as a target contract**

Document explicit `providerId` routing for logical devices, with `qt.serial`, `qt.tcp`, `qt.udp`, `vendor.*`, and `mock.*` as illustrative identifiers rather than frozen registry values. Keep existing public HAL headers and Adapter ABI as current compatibility surfaces; mark Qt standard providers and the network public interface as unimplemented and unfrozen.

- [x] **Step 3: Keep product protocol errors in the algorithm result surface**

Limit HAL to transport/provider framing errors it can observe without interpreting MB_DDF fields. Assign product header, length, CRC, command, sequence, CSV-field, and verdict errors to the algorithm/protocol layer. Remove nonexistent `IFrameBuilder`/`IFrameParser` cross-references.

- [x] **Step 4: Align the CSV contract with the implemented subset**

Separate the current strict MB_DDF parser rules from future generic CSV extensions. Describe the current external path, the one-file/one-definition loader, the 32/36 asset-test mismatch, the `SYSTEM_STATUS` lookup via `executionConfig`, and the currently unimplemented `ProtocolProfile`-to-runtime binding checks.

- [x] **Step 5: Centralize logging semantics**

Keep `LogEvent`, HAL/provider source mapping, and `requestId` semantics only in the logging contract. Other documents link to this section rather than duplicating the mapping table.

### Task 3: Reduce Implementation Reports To Evidence-Based Snapshots

**Files:**
- Modify: `docs/design/implementation/hal-implementation-design-report.md`
- Modify: `docs/design/implementation/logging-implementation-design-report.md`

- [x] **Step 1: Separate current code from the target architecture**

Describe the current `HalService -> CAbiAdapter -> MockAdapter` path, configuration-derived device discovery, current serial transaction limitations, missing Qt modules, missing real Adapter bridge, and the algorithm simulator that bypasses HAL only for unit tests. Put Qt providers, network support, provider routing, and product-level HAL Mock integration in an explicitly unimplemented migration section.

- [x] **Step 2: Remove contract copies**

Replace copied public declarations, stable error definitions, log-field mappings, and test requirements with links to the corresponding contract or testing specification. Retain only internal object relationships, concrete call paths, verified limitations, and extension impact.

- [x] **Step 3: Keep logging implementation details local**

Document the actual `hwtest_log_types`/`hwtest_log` split, recent buffer, JSONL sink, and HAL bridge without restating the logging contract.

### Task 4: Consolidate Test Evidence And Historical Status

**Files:**
- Modify: `docs/design/testing/testing-specification.md`
- Modify: `docs/design/testing/hal-test-design-report.md`
- Modify: `docs/plan/2026-06-25-1412-plan.md`
- Modify: `docs/plan/2026-07-02-1331-plan.md`
- Modify: `docs/superpowers/plans/2026-07-13-biz-integration.md`
- Modify: `docs/superpowers/plans/2026-07-14-document-consistency-audit.md`

- [x] **Step 1: Establish the current source-level test inventory**

Record four test targets and 76 source-level GoogleTest definitions: HAL 22, logging 7, BIZ 35, algorithm/protocol 12. Explain that final CTest registration is verified after build with `ctest -N`.

- [x] **Step 2: Make conditional assets explicit**

State that external MB_DDF CSV-dependent and `tmp` attachment-dependent tests may skip, and that a skipped test does not prove the associated capability. Distinguish protocol simulator, HAL Mock integration, Qt provider integration, vendor Adapter integration, and real-hardware acceptance.

- [x] **Step 3: Compress the HAL test report**

Retain only the HAL-specific coverage matrix, fixtures, known gaps, and dated verification evidence; link all shared commands, test-layer rules, and acceptance requirements to `testing-specification.md`.

- [x] **Step 4: Mark completed or superseded plans as historical records**

Add concise status banners and current fact-source links without rewriting historical plan bodies. Keep file paths stable.

### Task 5: Verify Correctness, Links, Terminology, And Scope

**Files:**
- Verify: `AGENTS.md`
- Verify: `docs/**/*.md`
- Verify: `CMakeLists.txt`
- Verify: `src/**/CMakeLists.txt`
- Verify: `tests/**/CMakeLists.txt`

- [x] **Step 1: Check target and test-count facts from source**

Run:

```powershell
rg -n "add_library|add_executable|add_subdirectory|gtest_discover_tests" CMakeLists.txt src tests -g "CMakeLists.txt"
rg -n "TEST(?:_F|_P)?\(" tests -g "*.cpp"
```

Expected: the documented four test targets and 22/7/35/12 source-level split match the source tree.

- [x] **Step 2: Check prohibited stale terminology**

Run:

```powershell
rg -n "没有具体算法层实现|算法层具体实现仍由完整系统提供|算法层内部拥有.*Socket|Adapter 只封装厂家库或系统 API|只注册 HAL" AGENTS.md docs/design -g "*.md"
```

Expected: no unqualified stale statement remains in current fact-source documents; historical quotations are explicitly marked historical.

- [x] **Step 3: Validate local Markdown links**

Run the repository-local PowerShell Markdown link checker over `AGENTS.md` and `docs/**/*.md`, resolving relative file targets while ignoring HTTP URLs and in-document anchors.

Expected: zero missing local file targets.

- [x] **Step 4: Run deterministic documentation audit and Git checks**

Run:

```powershell
python C:\Users\JiangKai\.codex\skills\military-software-agent\scripts\msa.py doctor
python C:\Users\JiangKai\.codex\skills\military-software-agent\scripts\msa.py run . --out build_msa_doc_audit
git diff --check
git status --short
```

Expected: audit prerequisites are available, `git diff --check` reports no whitespace errors, and only intended documentation files are modified.

- [x] **Step 5: Review the complete diff against the approved design**

Verify every new target statement is labeled unimplemented, every current claim has a code/CMake/test source, no public API is accidentally frozen, historical records remain intact, and no source or build file changed.
