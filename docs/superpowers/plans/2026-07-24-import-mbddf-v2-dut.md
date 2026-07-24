# MB_DDF_v2 Embedded DUT Import Implementation Plan

> **Execution note:** This plan is being executed in the current workspace. The imported embedded project remains an isolated DUT snapshot and is not linked into the host Qt build.

**Goal:** Copy the complete versioned MB_DDF_v2 embedded project into this repository as a self-contained device-under-test tree and update the repository instructions with its source, build, test, and boundary facts.

**Architecture:** Place the snapshot under `dut/mb_ddf_v2/` so its AArch64/C++20/CMake project, Python PC tool, protocol CSV, tests, and deployment scripts cannot be confused with the host Qt/C++17 application. Preserve the source tree's relative paths and project documentation; record the exact source commit and excluded generated artifacts in an import metadata file.

**Tech Stack:** Git archive snapshot, C++20/CMake/AArch64 cross build, Python/PyQt5 host tool, PowerShell/batch scripts, MB_DDF protocol CSV.

---

### Task 1: Capture source snapshot facts

**Files:**
- Create: `dut/mb_ddf_v2/IMPORT_METADATA.md`
- Source: `H:/Resources/RTLinux/Demos/MB_DDF_v2`

- [x] Record the source repository path, commit `32d961fbaccc3411378241dd1fa850d662354e4c`, observation time, tracked file count, and byte count.
- [x] Record that `.git`, build/install directories, `.deps`, caches, Python bytecode, logs, and test result outputs are excluded.

### Task 2: Import the embedded project

**Files:**
- Create: `dut/mb_ddf_v2/**` from the source repository's versioned tree

- [x] Copy all source-controlled embedded project files while preserving paths and filenames, including `src/`, `tests/`, `docs/`, protocol CSV, `test_pyqt/`, `tools/`, `build.ps1`, `debug.ps1`, `tests/test-*.ps1`, and batch wrappers.
- [x] Do not copy `.git` or generated/build output.
- [x] Keep the imported CMake project independent; do not add it as a host `add_subdirectory`.

### Task 3: Update repository instructions

**Files:**
- Modify: `AGENTS.md`

- [x] Declare `dut/mb_ddf_v2/` as the embedded DUT subtree and state that its C++20/AArch64 build is separate from the host Qt project.
- [x] Document the imported commit, source path, protocol location, build/test/debug entry points, and hardware safety boundary.
- [x] State that DUT results do not become host application or real-hardware evidence unless explicitly run and reported with target-board prerequisites.

### Task 4: Verify import integrity

- [x] Compare source-controlled file count and relative path set against `dut/mb_ddf_v2`, excluding only the documented metadata file.
- [x] Check protocol CSV count and representative build/test scripts.
- [x] Run `git diff --check` and verify the host CMake file was not changed to build the DUT implicitly.
- [x] Report that no target-board build is claimed unless the AArch64 toolchain and sysroot are present.
