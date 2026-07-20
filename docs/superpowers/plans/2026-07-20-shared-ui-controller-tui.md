# Shared UI Controller and TUI Implementation Plan

> **Execution note:** This plan should be executable either inline in the current session or by a delegated worker. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Completed 2026-07-20:** The implemented TUI uses a worker input thread so the Qt event loop remains live; this supersedes the simplified blocking-loop sketch below. The device protocol contract was unchanged because no protocol field or framing rule changed.

**Goal:** Build a Qt Core application controller shared by future TUI, Qt GUI, and Web UI front ends, then add a line-oriented TUI that operates the current MB_DDF `SYSTEM_STATUS` test in explicit stages.

**Architecture:** Add `hwtest_app_core` above BIZ and below presentation code. It owns configuration, HAL/BIZ/algorithm/log composition and exposes UI-safe action results and snapshots; it does not move protocol or hardware behavior out of their existing layers. `hwtest_tui` parses text commands and invokes this controller, while `hwtest_pc_runner` is refactored to use the same controller for its one-shot mode.

**Tech Stack:** C++17, Qt 5.15/Qt 6 Core, existing `hwtest_biz`, `hwtest_algorithm_mbddf`, `hwtest_hal`, `hwtest_log`, GoogleTest/CTest.

---

### Task 1: Freeze the shared application boundary

**Files:**
- Create: `src/app/include/app/test_application_controller.h`
- Create: `src/app/src/test_application_controller.cpp`
- Modify: `src/app/CMakeLists.txt`
- Create: `tests/app/test_application_controller_test.cpp`
- Create: `tests/app/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [x] **Step 1: Write the failing controller lifecycle test**

```cpp
TEST(TestApplicationControllerTest, LoadsSelectsPreparesAndRunsUdpSystemStatus)
{
    TestApplicationController controller;
    ASSERT_TRUE(controller.loadConfigurations(testConfigPath(), temporaryUdpHalConfig()).ok);
    ASSERT_TRUE(controller.selectControl(QStringLiteral("CONTROL_NETWORK")).ok);
    ASSERT_TRUE(controller.prepare().ok);
    ASSERT_TRUE(controller.start().ok);
    // The configured QUdpSocket peer echoes the valid SYSTEM_STATUS frame.
    ASSERT_TRUE(controller.waitForTerminal(3000).ok);
    EXPECT_EQ(controller.snapshot().phase, QStringLiteral("finished"));
    EXPECT_EQ(controller.snapshot().verdict, QStringLiteral("Pass"));
}
```

- [x] **Step 2: Build the app test to verify RED**

Run: `cmake --build build_codex --config Debug --target hwtest_app_tests --parallel`

Expected: compilation fails because `TestApplicationController` and the app test target do not exist.

- [x] **Step 3: Implement the UI-safe public model and controller**

```cpp
namespace hwtest::app {

struct ActionResult {
    bool ok = true;
    QString code;
    QString message;
};

struct ApplicationSnapshot {
    QString phase = QStringLiteral("empty");
    QString testState = QStringLiteral("Uninitialized");
    QString controlResourceId;
    QString providerId;
    QString taskId;
    int progress = 0;
    QString progressStep;
    bool hasResult = false;
    QString verdict;
    QString errorCode;
    QString message;
    QVariantMap rawData;
};

class TestApplicationController final : public QObject {
    Q_OBJECT
public:
    ActionResult loadConfigurations(const QString& testPath, const QString& halPath);
    QStringList availableControls() const;
    ActionResult selectControl(const QString& resourceId);
    ActionResult prepare();
    ActionResult start();
    ActionResult pause();
    ActionResult resume();
    ActionResult stop(int timeoutMs = 5000);
    ActionResult waitForTerminal(int timeoutMs);
    ActionResult shutdown();
    ApplicationSnapshot snapshot() const;
signals:
    void snapshotChanged(const hwtest::app::ApplicationSnapshot& snapshot);
};

} // namespace hwtest::app
```

Implementation rules:

```text
loadConfigurations: parse and validate both files without opening hardware
selectControl: accept only a configured module=control resource while disconnected
prepare: initialize HAL, open device, create HalControlTransport/executor/BIZ/logging, load BIZ config
start/pause/resume/stop: delegate only to ITestRunService and normalize errors
waitForTerminal: nested QEventLoop plus a single-shot timeout
shutdown: BIZ shutdown -> executor destruction -> HAL close/shutdown -> clear state
```

- [x] **Step 4: Run the focused controller tests to verify GREEN**

Run: `ctest --test-dir build_codex -C Debug -R '^TestApplicationControllerTest\.' --output-on-failure`

Expected: lifecycle, invalid-order and configured-control selection tests pass.

### Task 2: Add a scriptable line-oriented TUI shell

**Files:**
- Create: `src/app/include/app/tui_shell.h`
- Create: `src/app/src/tui_shell.cpp`
- Create: `tests/app/tui_shell_test.cpp`
- Modify: `src/app/CMakeLists.txt`
- Modify: `tests/app/CMakeLists.txt`

- [x] **Step 1: Write failing command parsing and state-guard tests**

```cpp
TEST(TuiShellTest, ExposesTheStagedSystemStatusWorkflow)
{
    EXPECT_EQ(parseTuiCommand(QStringLiteral("load")).type, TuiCommandType::Load);
    EXPECT_EQ(parseTuiCommand(QStringLiteral("use CONTROL_NETWORK")).arguments,
              QStringList{QStringLiteral("CONTROL_NETWORK")});
    EXPECT_EQ(parseTuiCommand(QStringLiteral("prepare")).type, TuiCommandType::Prepare);
    EXPECT_EQ(parseTuiCommand(QStringLiteral("run")).type, TuiCommandType::Run);
    EXPECT_EQ(parseTuiCommand(QStringLiteral("wait 3000")).type, TuiCommandType::Wait);
    EXPECT_EQ(parseTuiCommand(QStringLiteral("result")).type, TuiCommandType::Result);
}

TEST(TuiShellTest, RejectsUnknownCommandWithHelpHint)
{
    const TuiCommand command = parseTuiCommand(QStringLiteral("fire"));
    EXPECT_EQ(command.type, TuiCommandType::Invalid);
    EXPECT_FALSE(command.error.isEmpty());
}
```

- [x] **Step 2: Run the parser tests to verify RED**

Run: `cmake --build build_codex --config Debug --target hwtest_app_tests --parallel`

Expected: compilation fails because `tui_shell.h` is missing.

- [x] **Step 3: Implement the command set without terminal-specific dependencies**

```text
help
load [test-config] [hal-config]
controls
use <ResourceId>
prepare
run
pause
resume
stop [timeout-ms]
status
wait [timeout-ms]
result
disconnect
quit
```

`TuiShell::execute()` returns printable lines and a `quit` flag. It contains no HAL, algorithm, socket or CSV logic; every operational command calls `TestApplicationController`.

- [x] **Step 4: Run the focused TUI tests to verify GREEN**

Run: `ctest --test-dir build_codex -C Debug -R '^TuiShellTest\.' --output-on-failure`

Expected: parser, invalid argument and staged command tests pass.

### Task 3: Add the TUI executable and reuse the controller in one-shot mode

**Files:**
- Create: `src/app/tui_main.cpp`
- Modify: `src/app/main.cpp`
- Modify: `src/app/CMakeLists.txt`

- [x] **Step 1: Add the two executable targets**

```cmake
add_library(hwtest_app_core STATIC ${APP_PUBLIC_HEADERS} ${APP_SOURCES})
add_executable(hwtest_pc_runner main.cpp)
add_executable(hwtest_tui tui_main.cpp)
target_link_libraries(hwtest_pc_runner PRIVATE hwtest_app_core ${HWTEST_QT_CORE_TARGET})
target_link_libraries(hwtest_tui PRIVATE hwtest_app_core ${HWTEST_QT_CORE_TARGET})
```

- [x] **Step 2: Implement the TUI console loop**

```cpp
while (std::getline(std::cin, line)) {
    QCoreApplication::processEvents();
    const TuiReply reply = shell.execute(QString::fromUtf8(line));
    for (const QString& outputLine : reply.lines) {
        std::cout << outputLine.toStdString() << '\n';
    }
    if (reply.quit) {
        break;
    }
    std::cout << "hwtest> " << std::flush;
}
```

- [x] **Step 3: Refactor `hwtest_pc_runner` to call the shared controller**

One-shot sequence:

```text
loadConfigurations -> prepare -> start -> waitForTerminal -> serialize snapshot -> shutdown
```

Keep existing `--test-config`, `--hal-config`, JSON output and exit-code semantics.

- [x] **Step 4: Verify a scripted TUI UDP loopback session**

Run the local one-shot UDP echo peer and pipe:

```text
load
controls
use CONTROL_NETWORK
prepare
run
wait 3000
result
disconnect
quit
```

Expected: output contains `ok wait`, `verdict=Pass`, and a clean disconnect; `status` after `wait` reports `phase=finished`.

### Task 4: Synchronize evidence and run release gates

**Files:**
- Modify: `AGENTS.md`
- Modify: `docs/design/README.md`
- Modify: `docs/design/overview/five-layer-architecture.md`
- Modify: `docs/design/testing/testing-specification.md`
- Modify: `docs/design/contracts/device-communication-protocol.md`

- [x] **Step 1: Document the shared controller and TUI as current implementation**

Record that TUI is line-oriented, Qt GUI/Web UI are not implemented, all three are expected to call `hwtest_app_core`, and only `mbddf.system_status` is available.

- [x] **Step 2: Update the unique test inventory from source definitions**

Run: `rg -n '^TEST(_F)?\(' tests/hal tests/log tests/biz tests/algorithm tests/app`

Expected: the documentation count equals the actual source definitions and test source files.

- [x] **Step 3: Run full Debug and Release verification**

```powershell
$env:MB_DDF_PROTOCOL_CSV_DIR = "H:\Resources\RTLinux\Demos\MB_DDF_v2\docs\design\product_protocol_csv"
cmake --build build_codex --config Debug --parallel
ctest --test-dir build_codex -C Debug --output-on-failure
cmake --build build_codex --config Release --parallel
ctest --test-dir build_codex -C Release --output-on-failure
git diff --check
```

Expected: both configurations build, all discovered tests pass without skips when the approved CSV directory is present, and `git diff --check` reports no whitespace errors.
