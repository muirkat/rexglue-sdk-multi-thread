# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ReXGlue is a static recompilation SDK for Xbox 360 executables. It reads an Xbox 360 `XEX` (PowerPC) binary and **generates portable C++ source ahead of time** (it does not interpret or JIT at runtime). The generated C++ is then compiled and linked against the ReXGlue **runtime library**, which provides the guest memory model, a reimplemented Xbox 360 kernel, and graphics/audio/input backends so the recompiled game runs natively on Windows and Linux. The approach is rooted in Xenia, XenonRecomp, and rexdex's recompiler (see README credits).

There are two distinct artifacts in this repo:
- **`rexglue`** — the CLI tool that performs analysis + code generation (the SDK's compiler).
- **The runtime** (`rexruntime`, a shared library) — what recompiled games link against at runtime.

## Build, test, lint

The toolchain is **enforced and strict**: Clang **18+** only (GCC/MSVC are hard-errors in CMake), C++23, 64-bit only, x86-64-v3 baseline. Build generator is **Ninja Multi-Config** with `Debug`, `Release`, `RelWithDebInfo` configs in one build tree. Presets: `win-amd64`, `win-arm64`, `linux-amd64`, `linux-arm64`.

The preferred workflow is the **PSReX PowerShell module** (`scripts/PSReX/`), which exposes `rex-*` aliases. On Windows it's loaded via the VS Developer profile at `scripts/vs/rexglue-devprompt.ps1`; run `rex-setup` once to install the pre-commit hook and dev profile.

```powershell
rex-setup                      # install pre-commit hook + verify toolchain (clang++, clang-format, cmake, ninja)
rex-configure                  # cmake --preset <preset>  (preset auto-detected from $env:REXGLUE_PRESET / platform)
rex-build -Config Debug        # cmake --build out/build/<preset> --config <Config>
rex-test                       # ctest --preset <preset>-debug   (requires REXGLUE_BUILD_TESTS=ON at configure time)
rex-format -Modified           # clang-format changed files (-All / explicit paths also supported)
rex-lint                       # clang-tidy over include/ + src/ (needs compile_commands.json from configure)
```

Equivalent raw commands (any platform):
```bash
cmake --preset linux-amd64
cmake --build out/build/linux-amd64 --config Debug
ctest --preset linux-amd64-debug
```

**Tests are OFF by default.** Configure with `-DREXGLUE_BUILD_TESTS=ON` to build them. There are two suites under `tests/`:
- `tests/unit/` — Catch2 unit tests, one `unit_tests` executable (core, codegen, kernel, memory, ppc, system, rexglue).
- `tests/ppc/` — PowerPC instruction tests: each `.s` file in `tests/ppc/asm/` is assembled with a PPC toolchain, recompiled, and turned into a generated Catch2 case (validates instruction translation). Requires a PPC cross-toolchain; see `cmake/ppc_test_pipeline.cmake`.

Run a single test via the built executable + Catch2 filters, e.g. `./out/.../unit_tests "[codegen]"` or `unit_tests -c "specific test name"`.

## Versioning

Do **not** hand-edit the version in `CMakeLists.txt` for routine releases. The project version there is only the **API floor** (`MAJOR.MINOR`); the full version is derived at configure time from git tags by `cmake/rex_version.cmake`. Bump the floor only for public-API changes. Full release process is in [docs/RELEASING.md](docs/RELEASING.md). Branch model: `main` = latest stable tag, `development` = working canary, `release/X.Y.Z` = short-lived release branches.

## Code generation pipeline (`src/codegen/`)

This is the heart of the tool. Codegen runs a fixed sequence of **analysis phases** (declared in `include/rex/codegen/phases.h`, one `phase_*.cpp` each) over a `CodegenContext` before any C++ is emitted:

1. **Register** (`phase_register.cpp`) — seed the `FunctionGraph` from PDATA, config, helpers, imports.
2. **Scan** (`phase_scan.cpp`) — segment the binary into code/data regions; find `bl` targets, thunks, `bctr` sites.
3. **Discover** (`phase_discover.cpp`) — grow function blocks from candidate entry points.
4. **Merge** (`phase_merge.cpp`) — vacancy-based function expansion and sealing.
5. **GapFill** (`phase_gapfill.cpp`) — register leftover uncovered code regions as functions.
6. **Validate** (`phase_validate.cpp`) — confirm every call target resolves.

After analysis, `CodegenWriter` emits C++. The instruction translation layer lives in `src/codegen/ppc/` (disassembler + opcode tables) and `src/codegen/builders/` — one `build_<mnemonic>()` per PPC instruction, routed through `DispatchInstruction()` (`builders.h` / `instruction_dispatch.cpp`). Each builder turns a decoded PPC instruction into C++ statements. Output rendering uses Inja templates (`template_registry.cpp`).

`CodegenPipeline` (`codegen.cpp`) wires this together: it spins up a `Runtime` in **tool mode** (no GPU) to load the XEX and populate memory, builds a `CodegenContext`, runs analysis, then writes. `ProjectRecompiler` (`project_recompiler.cpp`) drives **multi-binary** projects (an entrypoint XEX plus DLLs) from a manifest. Project config is TOML (`tomlplusplus`); there's a legacy-config migration path in `src/rexglue/commands/`.

## Runtime architecture (`src/system/`, `src/kernel/`, `include/rex/`)

`Runtime` (`include/rex/runtime.h`, `src/system/runtime.cpp`) is the top-level object recompiled apps create. It owns all subsystems and is constructed with data-root paths (game/user/update/cache). Key design point: **backends are dependency-injected** via `RuntimeConfig` (graphics/audio/input via the `REX_*_BACKEND` factory macros and `kernel_init`), keeping `rexruntime` decoupled from concrete backend libs. `tool_mode` skips GPU init for analysis tools like `rexglue` itself.

- **Guest memory** — custom mapping at a fixed base (`0x100000000`); this is why ASan is unusable and only UBSan is wired into `REXGLUE_ENABLE_SANITIZERS`.
- **Kernel reimplementation** (`src/kernel/`) — `xboxkrnl`, `xam`, `xbdm`, and CRT shims. `src/system/` holds the `X*` kernel objects (`XThread`, `XEvent`, `XFile`, `XSemaphore`, …), `KernelState`, `FunctionDispatcher` (guest→host call dispatch), and the XEX/ELF module loaders.
- **CMake target map**: `src/core`→`rexcore`, `src/filesystem`→`rexfilesystem`, `src/ui`→`rexui`, `src/input`→`rexinput`, `src/audio`→`rexaudio`, `src/graphics`→`rexgraphics` (OBJECT libs); `src/system`+`src/kernel`→`rexruntime` (SHARED, aliased `rex::runtime`/`rex::system`/`rex::kernel`); `src/codegen`→`rexcodegen` (STATIC); `src/rexglue`→`rexglue` (the CLI executable). Public headers live in `include/rex/<subsystem>/`.

Most platform-specific code is split into `*_win.cpp` / `*_posix.cpp` pairs (memory, threading, filesystem, exceptions, clock, sockets, etc.). When touching such code, change both sides.

## Multi-threading and concurrency

Threading lives almost entirely in the **runtime**, not the codegen tool. Get the boundary right before touching either side:

- **`rexglue` codegen: the embarrassingly-parallel stages (instruction decode, C++ emission) are threaded; the stateful analysis phases are serial.** Both parallel stages use the shared `parallelMap()` helper in `src/codegen/parallel_for.h` — a deterministic index-parallel map (atomic work cursor, results written per-index) whose output is **byte-identical regardless of thread count**. `DecodedBinary::decode()` decodes every instruction in parallel (pure per-word decode), and `CodegenWriter::write()` emits every function's C++ via `emitFunctionsParallel()` (`emitCpp()` is `const`, reads only shared-const `BinaryView`/`RecompilerConfig`/`FunctionGraph`/read-only `ExportResolver`, builds its own `BuilderContext`). Worker count is the `codegen_threads` cvar (`0` = autodetect via `hardware_concurrency()`; set env `REX_CODEGEN_THREADS`). Invariant if you touch emission: graph mutation (rexcrt `setName` rename, import/rexcrt filtering) must stay **before** the parallel loop, and nothing in the emit path may write through the const references.
  - The analysis phases (`phase_*.cpp`) run serially and mutate the shared `FunctionGraph`, so they aren't parallelized — but two of them used to be **O(n²)** and dominated wall-clock on large titles. Both are fixed and worth understanding before editing the graph: (1) `FunctionGraph::notifyFunctionAdded` no longer scans every function on each `addFunction` — an unresolved-jump-target index (`unresolvedWaitersByTarget_`) routes notifications only to functions that can resolve against the new entry; (2) `cleanupAbsorbedGapFills` (`phase_gapfill.cpp`) replaced a pairwise containment scan with an interval sweep line. Per-stage profiling is available at debug level (`--log-level debug`, look for `[timing]`).
  - The remaining serial axis is per-module analysis (`ProjectRecompiler` handles the entrypoint and each DLL sequentially); modules are independent but each briefly stands up a `Runtime` in tool mode, so audit shared global state (`Runtime::instance()`, the template registry) before parallelizing it.

- **The runtime is heavily multi-threaded** — it reproduces the Xbox 360 thread model. Each guest thread is an **`XThread`** (`src/system/xthread.cpp`) kernel object that owns a host **`rex::thread::Thread`** plus a **`runtime::ThreadState`** (`include/rex/system/thread_state.h`). `ThreadState` holds that thread's `PPCContext` and is bound **thread-locally** via `ThreadState::Bind()`; recompiled code and kernel `_entry` shims reach the current thread's context/kernel state through `ThreadState::Get()` and the convenience macros in `kernel_state.h`. The Xbox 360 has 3 cores × 2 hardware threads (6 logical); `XThread::SetAffinity()`/`active_cpu()` model that mapping.

- **Guest threads run on host fibers.** `XThread` keeps a `main_fiber` (`rex::thread::Fiber`, `src/core/fiber_*.cpp`); fiber switching preserves the full C++ call stack so a guest thread can be suspended/resumed mid-function without LR lookup. This is also how APCs and critical regions (`EnterCriticalRegion`/`DeliverAPCs`) are serviced.

- **Locking discipline (from Xenia, read the comment in `include/rex/thread/mutex.h`):** the **`global_critical_region`** is a process-wide singleton mutex that must be the *first* lock acquired and held for the whole critical region — think "disabling guest interrupts." Code reachable only from guest threads can be guarded by it alone; code also touched by non-guest (host) threads needs additional protection. Keep critical regions extremely short — **no I/O** inside them. Guest kernel locks are modeled by `X_KSPINLOCK` fields in `KernelState` (`dispatcher_lock`, `ob_lock`, `tls_lock`, the thread-list spinlock). `KernelState` itself also runs a background dispatch thread (`XHostThread`).

- **Host synchronization primitives** live in `include/rex/thread/` and `rex/thread.h`: `Thread`, `Event`, `Semaphore`, `Mutant`, `Timer` (all `WaitHandle`s), `Fence`, `HighResolutionTimer`, plus `atomic.h`, `mutex.h`, `fiber.h`, `timer_queue.h`. Atomics and several primitives have `*_win.cpp`/`*_posix.cpp` implementations in `src/core/` — change both platforms together. Prefer these `rex::thread` wrappers over raw `std::thread`/OS calls so guest-visible semantics (wait results, alertable waits, suspend counts) stay correct.

## Graphics / backends

Graphics backend is selected at configure time: **D3D12** (Windows, default on) and **Vulkan** (default on Linux, off on Windows). At least one must be enabled (CMake errors otherwise). FidelityFX is an experimental opt-in (`REXGLUE_ENABLE_FIDELITYFX`). Shaders under `src/graphics/shaders/` and `src/ui/shaders/` are compiled via DXC (Windows) / glslang. Tracy profiling and lightweight perf counters are on by default but compiled out in Release builds.

## Conventions

- **Style**: `.clang-format` (Google base, 2-space indent, 100-col, left-aligned pointers). Run `rex-format` before committing — the pre-commit hook enforces it.
- **Naming** (`.clang-tidy`): classes/structs `CamelCase`, the enabled check set is `bugprone-*`, `performance-*`, and selected `readability-*`/`misc-*`. Header filter is `include/rex/.*`.
- **Error handling**: functions return `rex::Result<T>` / `VoidResult` (`include/rex/result.h`) with an `ErrorCategory` rather than throwing. `Err(...)` / `Ok(...)` helpers. Kernel-level code uses `X_STATUS` codes.
- **Third-party** deps are git submodules under `thirdparty/` (vendored sources only, built via `add_subdirectory`). Run `git submodule update --init --recursive` before configuring. Do not edit vendored code.
