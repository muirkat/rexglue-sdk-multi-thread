# Handoff: harden the demand-driven GapFill fix so it can re-land on `main`

You are picking up a **reverted** codegen fix. This branch (`codegen/gapfill-demand-driven`,
tip `631d501`) holds the fix; it was removed from `main` because it **flaky-crashes on a real
title**. Your job: make it crash-free on the real project manifest, keep its benefit, then it can
be re-merged to `main`.

---

## TL;DR of the task

`src/codegen/phase_gapfill.cpp` gained a **demand-driven pass** (`fillUnresolvedBranchTargets` +
a fixpoint loop in `phases::GapFill`). It registers a function at each unresolved `b`/`bl` branch
target that no entry owns, so tail-call thunks that previously emitted a runtime `REX_FATAL`
resolve to real functions.

- ✅ On the SDK test fixture it works: Fable 2 testdata went **8 unresolved → 0**, deterministic,
  151 files, output diff = only the intended new functions.
- ❌ On the **real Fable2ReX project manifest** it crashes **silently and flakily (~50%)**.

**Goal:** eliminate the crash on the real manifest while keeping the testdata 8→0 result, then
re-land on `main`.

---

## Crash evidence (already gathered — do not redo from scratch)

Tested `rexglue.exe -f codegen <manifest>` repeatedly:

| SDK | Manifest | Crashes |
| --- | --- | --- |
| pre-fix `081ae4c` (no demand pass) | Fable2ReX | **0 / 6** |
| with-fix `631d501` (this branch)   | Fable2ReX | **3 / 6** |
| with-fix `631d501`                 | SDK testdata `fable2_rexglue.toml` | 0 (stable, many runs) |

- Crash is **silent**: exit code 127, no assert / no "terminate" / no message. The log ends
  abruptly mid-Write-phase emission, around functions in the `0x83117xxx` region.
- Crash reproduces **even single-threaded** (`REX_CODEGEN_THREADS=1`) — it is **not** a
  parallel-emit race. It is non-deterministic run-to-run → almost certainly reading memory whose
  contents vary (uninitialized guest memory / mis-decoded bytes).
- When it *doesn't* crash, it produces complete output (150 `.cpp`).

### Prime suspects (the manifest-specific trigger)

The Fable2ReX manifest has unresolved targets that the testdata does not, notably **`0x83040DB8`,
`0x83117FB8`, `0x83040E28`, `0x83040FC0`**. These show up in the log as **both**:

- `Unresolved conditional branch to 0xX from 0xY`  ← a `bc` target = an **interior label**, and
- `Unresolved b target 0xX from 0xZ`               ← also reached by a `b`.

The current pass skips a target only if `getFunctionContaining(t)` is non-null (inside another
function's **blocks**). But these addresses sit in a function's **bounds-but-not-blocks gap**
(`discover()` only grows `size_`, so a function over-claims bounds past its real blocks), so
`getFunctionContaining` returns null and the pass **registers a function there anyway**. Block
discovery then starts from a non-function-start address and walks into garbage → silent crash.

The testdata's 8 targets are *pure* `b` tail-call thunk tails (never `bc` targets), which is why
the simple fixture never trips this.

---

## Root-cause hypothesis & required hardening

**Distinguishing factor:** a safe target is a *real function entry* reached only by `b`/`bl`. An
unsafe target is an *interior label* (also reached by `bc`) that lives inside an over-claiming
neighbor's bounds. Bounds-containment alone can't separate them (the testdata thunks are *also*
inside a neighbor's bounds — that's the over-claim). The reliable signal is: **is this address
also a conditional-branch / interior-label target?**

Implement at least these guards in `fillUnresolvedBranchTargets` and re-test:

1. **Exclude interior-label targets.** Build the set of all addresses that appear as a
   *conditional* (`bc`) branch target across the graph (and/or any function's `labels()`), and
   **skip** registering a function at any such address. (Right now only the `bc` *jump record
   itself* is skipped; the same address arriving via a `b` record is still registered.)
2. **Validate the target is a plausible function start before registering.** Options, cheapest
   first:
   - reject if the address is within any function's `isWithinBounds()` **and** appears as that
     function's label;
   - do a *bounded* trial block-discovery and reject the target if discovery would run past a sane
     instruction cap or hit an invalid/`0x00000000` word before a terminator;
   - require the first decoded instruction to be a sane opcode (not data).
3. **Keep the existing guards** (not already an entry/import, executable, not in
   `invalidInstructions`, not in import/export range).

Be conservative: it is fine to leave a few genuinely-ambiguous targets unresolved (they stay as
the pre-existing managed FATALs) — correctness/no-crash beats resolving every last thunk.

If guards alone don't stop the crash, the bug may be downstream in **block discovery / emission of
a bad gap function** — in that case add a bounds/iteration guard there too (the silent crash is
likely an out-of-range read while decoding from a bad start).

---

## How to reproduce & verify (this is the acceptance gate)

Build env (Windows, this machine): see memory `rexglue-build-and-test-env.md`. Quick form:

```bash
export PATH="$PATH:/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja:/c/LLVM/bin"
cd D:/Development/rexglue-sdk-multi-thread
git checkout codegen/gapfill-demand-driven
cmake out/build/win-amd64 -DREXGLUE_BUILD_TESTS=OFF        # reconfigure
cmake --build out/build/win-amd64 --target rexglue --config Release   # -> out/win-amd64/Release/rexglue.exe
```

**Gate 1 — testdata still fixed (must stay 8→0, deterministic):**
```bash
cd D:/Development/rexglue-sdk-multi-thread/testdata/fable2
rexglue.exe -f codegen fable2_rexglue.toml         # expect "Done in", 0 "Unresolved call from" in rexglue_out
grep -rho "Unresolved call from" rexglue_out | wc -l   # expect 0
```

**Gate 2 — real manifest is crash-free (the whole point). Use a SCRATCH out dir so you never
clobber the project's real `generated/default`:**
```bash
cd D:/Development/Fable2ReX/Fable2ReX
sed 's#out_directory_path = "generated/default"#out_directory_path = "generated/_test"#' \
    fable2rex_manifest.toml > /tmp/fable2rex_TEST.toml
PFX=D:/Development/rexglue-sdk-multi-thread/out/win-amd64/Release/rexglue.exe
c=0; for i in $(seq 1 12); do "$PFX" -f codegen /tmp/fable2rex_TEST.toml >/dev/null 2>&1 || c=$((c+1)); done
echo "crashes: $c/12"     # MUST be 0/12
rm -rf generated/_test /tmp/fable2rex_TEST.toml
```
(Exit code 127 = the crash. Run ≥12 times since it's ~50% flaky — a couple of clean runs is not
proof.)

**Gate 3 — `[codegen]` unit tests pass:** `unit_tests.exe "[codegen]"` (build target `unit_tests`;
binary lands in `out/win-amd64/Release/`). Run tags in isolation (full suite has an unrelated
teardown assert).

Debugging aid: the crash is silent, so a sanitizer build helps — **UBSan only** (`ASan is unusable`
here; guest memory is mapped at a fixed base `0x100000000`). Wire via `REXGLUE_ENABLE_SANITIZERS`.

---

## Re-landing on `main`

Once all three gates pass:
- Rebase/squash this branch onto current `main` (note `main` moved: `081ae4c` is `v0.8.3`, plus a
  PPC-test flag fix `75554e5`).
- Re-run the testdata A/B diff so the only output changes are the intended new functions.
- The version is git-tag-derived; if this ships as a release, tag `v0.8.4` (patch, no API change) —
  do **not** hand-edit the floor. See `docs/RELEASING.md`.
- The consumer (`D:/Development/Fable2ReX/Fable2ReX`) gets it via
  `cmake --install out/build/win-amd64 --config Release --prefix .../Fable2ReX/sdk`.

## Key references
- Fix code: `src/codegen/phase_gapfill.cpp` (`fillUnresolvedBranchTargets`, `phases::GapFill`).
- Resolution authority: `FunctionGraph::resolveBranch` / `classifyTarget` / `getFunction` /
  `getFunctionContaining` / `isWithinBounds` (`src/codegen/function_graph.cpp`,
  `include/rex/codegen/function_node.h`).
- Unresolved-jump records: `node->unresolvedJumps()` (`UnresolvedJump{site,target,isCall,isConditional}`).
- Original commit message: `git show 631d501`.
