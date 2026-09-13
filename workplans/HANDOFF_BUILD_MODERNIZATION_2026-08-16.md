# Handoff — SWMM 5.2.4 Build Modernization (build-v5.2.4)

**Date:** 2026-08-16
**Branch:** `build-v5.2.4` (sibling clone `openswmm.engine.5.2.4`)
**Implements:** `openswmm.engine/plans/ENGINE_524_BUILD_MODERNIZATION_PLAN_2026-08-16.md`
(= Phase 1 step 0 of `openswmm.gui/workplans/MULTI_ENGINE_VERSION_SUPPORT_PLAN_2026-08-01.md`)

**Your job:** verify this compiles and tests clean on **Windows, macOS arm64, macOS x86_64** (Linux is already verified — see §3), fix whatever breaks, and report back. Nothing is committed; the working tree holds all changes.

---

## 1. What changed and why

| File | Change |
|---|---|
| `CMakeLists.txt` | `cmake_minimum_required` 3.13 → **3.21** (presets v3 needs it; 3.13 deprecated under CMake 4.x). Added `SWMM_WITH_OPENMP` option (default ON) doing `find_package(OpenMP REQUIRED)` **plus an explicit `TARGET OpenMP::OpenMP_C` assertion**. `BUILD_TESTS` help text now says GoogleTest. |
| `src/solver/CMakeLists.txt` | Removed the broken OpenMP genex (see §2.1). Removed hardcoded `/GL /fp:fast` + `/LTCG` (see §2.2). Moved `find_package(OpenMP)` up to the top level. OpenMP now linked under `if(TARGET OpenMP::OpenMP_C)`. |
| `cmake/HomebrewOpenMP.cmake` | **New.** Adapted from `openswmm.engine/cmake/FindOpenMP.cmake` but deliberately renamed (see §2.3). Included only on APPLE. *(Later superseded — now `cmake/FindOpenMP.cmake`, verbatim engine copy; see constraint 3 in §6.)* |
| `CMakePresets.json` | **New.** `Windows` / `Linux` / `Darwin`, each with `-debug` and `-tests`, plus build+test presets. FP policy mirrors `openswmm.engine`. |
| `vcpkg.json` | **New.** Trimmed manifest; `tests` feature pulls `gtest`. No hdf5/kokkos/sqlite3, no overlays. |
| `tests/CMakeLists.txt` | Boost → `find_package(GTest CONFIG REQUIRED)`. `add_test` now uses `$<TARGET_FILE:...>` (see §2.4). Added `LABELS "unit"`. |
| `tests/outfile/CMakeLists.txt` | Links `GTest::gtest` / `GTest::gtest_main`; C++17; fixed a stale include path (`../../outfile/include` → `../../src/outfile/include`). |
| `tests/outfile/test_output.cpp` | Ported Boost.Test → GoogleTest. All 17 cases, reference data and tolerances carried over unchanged. |
| `extern/boost.cmake` | **Deleted** (directory removed). |
| `.github/workflows/build-and-test.yml` | Rewritten: 4-platform matrix, vcpkg + presets, OpenMP linkage gate, smoke run. |
| `tools/verify_524_parity.sh` | **New.** Byte-identical `.out` parity gate vs stock `v5.2.4`. |

---

## 2. Bugs found and fixed — read this before touching the CMake

### 2.1 The OpenMP generator expression was always-true (upstream bug)

```cmake
# BEFORE — src/solver/CMakeLists.txt
$<$<BOOL:OpenMP_C_FOUND>:OpenMP::OpenMP_C>
```

`$<BOOL:...>` evaluates the **literal string** `OpenMP_C_FOUND`, not the variable. A non-empty, non-false string is `1`, so this was unconditionally true. Consequences: the target always tried to link `OpenMP::OpenMP_C`, and where `find_package(OpenMP)` failed (macOS without libomp) CMake **aborted at generate time** instead of degrading. Now gated on `if(TARGET OpenMP::OpenMP_C)`.

### 2.2 `/fp:fast` removed — deliberate, do not restore

Upstream pinned `/fp:fast` for MSVC Release. This branch adopts `openswmm.engine`'s `/fp:precise` + `-ffp-contract=off` + `-fno-fast-math`, so that 5.2.4 vs 5.3.0 vs v6 benchmark differences are attributable to engine changes rather than compiler math. **This means results differ in the last bits from EPA's shipped Windows binary — that is intended.** All FP policy now lives in `CMakePresets.json`; do not reintroduce per-target FP flags.

### 2.3 The Homebrew OpenMP shim shadowed CMake's builtin module (bug I introduced, then fixed — do not undo)

Copying `openswmm.engine/cmake/FindOpenMP.cmake` verbatim onto `CMAKE_MODULE_PATH` **shadows CMake's builtin `FindOpenMP`**, because the module path is searched first. That file's body is `if(APPLE ...)` only, so on Linux/Windows it is a silent no-op: `find_package(OpenMP)` resolves to it, finds nothing, sets nothing, and **the build links no OpenMP runtime at all** while still configuring and compiling successfully.

This was reproduced empirically on Linux: `libswmm5.so` had no `libgomp` dependency. Fixed by renaming to `cmake/HomebrewOpenMP.cmake` (cannot shadow) and `include()`-ing it only on APPLE, before the real `find_package(OpenMP REQUIRED)`.

> **Worth checking upstream:** `openswmm.engine/cmake/FindOpenMP.cmake` has the same shape and is on that project's module path. It may be masked there (vcpkg/Kokkos supply OpenMP through their own config packages), but it is worth confirming the main repo's Linux/Windows builds genuinely link an OpenMP runtime. **Not changed as part of this work.**

### 2.4 `add_test` path was wrong for single-config generators

`${CMAKE_BINARY_DIR}/bin/$<CONFIGURATION>/test_output` only resolves under multi-config generators. Under Ninja (Linux/Darwin presets) the binary lands in `bin/` with no config subdirectory, so ctest looked at a nonexistent path. Now `$<TARGET_FILE:test_output>`.

---

## 3. What is already verified (Linux aarch64, GCC 11.4, CMake 4.4.2)

Done in a sandbox **without vcpkg**, configuring directly rather than through presets:

- ✅ Configure + build clean; `runswmm`, `libswmm5.so`, `libswmm-output.so` all produced.
- ✅ `OpenMP C found: version 4.5, flags -fopenmp`; `ldd libswmm5.so` → `libgomp.so.1`. **This is the assertion that failed before the §2.3 fix.**
- ✅ `-DSWMM_WITH_OPENMP=OFF` warns about serial builds and still configures.
- ✅ GoogleTest port builds and **17/17 tests pass** (same count as the Boost suite); `ctest -L unit` green.
- ✅ End-to-end: `runswmm Example1.inp ex1.rpt ex1.out` completes and writes a 332 KB `.out`.
- ✅ `CMakePresets.json` + `vcpkg.json` parse as valid JSON; workflow YAML parses; `bash -n` clean on the parity script.

**Not verified anywhere yet:** the preset path itself (needs `VCPKG_ROOT`), Windows, macOS, and `tools/verify_524_parity.sh` end-to-end.

---

## 4. What you need to verify

Set `VCPKG_ROOT` first. Per platform:

```bash
cmake --preset <OS>-tests          # OS ∈ Windows | Linux | Darwin
cmake --build build/<os>-tests --config Release
ctest --test-dir build/<os>-tests -C Release -L unit --output-on-failure
```

macOS needs `brew install libomp` first; pass `-DCMAKE_OSX_ARCHITECTURES=arm64` or `x86_64` per leg.

Checklist:

1. [ ] Configure succeeds on all four legs (`Linux`, `Windows`, `Darwin` arm64, `Darwin` x86_64).
2. [ ] `OpenMP C found:` appears in configure output on every leg.
3. [ ] OpenMP runtime genuinely linked — `otool -L` / `ldd` / `dumpbin /dependents` shows `libomp` / `libgomp` / `vcomp*.dll`. **Do not accept a green build without this.**
4. [ ] `ctest -L unit` → 17/17 pass.
5. [ ] `runswmm` runs a model end-to-end.
6. [ ] `tools/verify_524_parity.sh` reports all PASS (stock-vs-stock at this point, since no backports have landed — a failure means the script is wrong, not the engine).
7. [ ] Push a branch and confirm the 4-leg CI matrix is green.

---

## 5. Likely failure points, with fixes

| Symptom | Cause | Fix |
|---|---|---|
| `Could NOT find OpenMP` on macOS | libomp missing | `brew install libomp`. If still failing, check `cmake/FindOpenMP.cmake` paths (`/opt/homebrew/opt/libomp` Apple Silicon, `/usr/local/opt/libomp` Intel). |
| `find_package(OpenMP) succeeded but OpenMP::OpenMP_C was not defined` | The §2.3 shadowing class of bug has reappeared | Verify no file named `FindOpenMP.cmake` is on `CMAKE_MODULE_PATH`. This FATAL_ERROR is intentional — it is the guard. |
| MSVC: `/GL` + `/LTCG` mismatch warnings | Presets set `/GL` in `CMAKE_C_FLAGS_RELEASE` and `/LTCG` in linker flags; `src/solver` still adds `/Zi` for Release | Harmless. If the linker objects, drop `/Zi` from `src/solver/CMakeLists.txt`. |
| Windows: `vcomp` check in CI fails though OpenMP is on | The `findstr` import probe is crude | Replace with `dumpbin /dependents` (needs the MSVC dev shell) and grep `vcomp`. Flagged as the least-tested step in the workflow. |
| `GTest::gtest not found` | `VCPKG_MANIFEST_FEATURES=tests` not applied | Use the `-tests` preset (it sets both the feature and `BUILD_TESTS=ON`); confirm `VCPKG_ROOT` is exported. |
| `test_output` fails to find `Example1.out` | Working directory | `add_test` sets `WORKING_DIRECTORY tests/outfile/data`; run via ctest, not the binary directly. |
| `-Wall -Wextra` noise from upstream C | Upstream EPA code is not warning-clean (`fread` unused-result etc.) | **Leave it.** Warnings are not errors here on purpose; do not "fix" upstream solver source — the parity gate requires byte-identical numerics. |
| Parity script: `git worktree add` fails | `.parity/stock-v5.2.4` exists from a previous run | `git worktree remove .parity/stock-v5.2.4 --force`, rerun. |

---

## 6. Hard constraints — do not violate

1. **No solver-path source edits.** The GUI plan requires numerics bit-identical to stock EPA 5.2.4; only reporting/IO/API surface may change, and none of that is in scope here. This handoff changed **zero** `.c`/`.h` solver files — verify with `git diff --stat` before committing.
2. **Do not restore `/fp:fast`** (§2.2).
3. ~~**Do not rename `HomebrewOpenMP.cmake` back to `FindOpenMP.cmake`** (§2.3).~~
   **SUPERSEDED by user decision (2026-08-16, verification session):** the file is
   now `cmake/FindOpenMP.cmake`, copied **verbatim** from
   `openswmm.engine/cmake/FindOpenMP.cmake` so both repos share one OpenMP
   strategy. The §2.3 shadowing hazard is neutralized differently, mirroring how
   `openswmm.engine/src/engine/CMakeLists.txt` itself consumes the file: it is
   `include()`d by **full path**, APPLE-only, and `cmake/` is kept **off**
   `CMAKE_MODULE_PATH` (the upstream append was vestigial — stock v5.2.4 has no
   `cmake/` directory). New constraint: keep the file byte-identical to the
   engine's copy, and never re-add `${PROJECT_SOURCE_DIR}/cmake` to
   `CMAKE_MODULE_PATH`.
4. **The parity script must build both trees with identical flags.** It copies this branch's presets/manifest into the stock worktree for exactly that reason. If you change how either side is configured, change both.

---

## 7. State and follow-ups

- Working tree is **uncommitted** on `build-v5.2.4`. A pre-existing local edit to `.github/workflows/build-and-test.yml` (branch triggers → `lew-develop`) was overwritten by the rewrite — intended.
- `git diff --stat` should show only: root + solver + tests CMake, new presets/manifest/cmake/tools files, the ported test, and the workflow. Plus deleted `extern/boost.cmake`.
- **Not done, sequenced next:** GUI plan Phase 1 steps 1–6 (unknown-section skip+warn, unknown-option skip+warn, `swmm_setWarningCallback`, `swmm_getRunningMassBalErr`, `src/worker/`, tag `v5.2.4-swmmvis.1`).
- **Blocked on this work:** the 5.2.4 registry entry in `openswmm.engine/plans/BENCHMARK_REGRESSION_CI_PLAN_2026-08-16.md`.
- **Open question for the human:** amendments 1–3 in the GUI plan header (GoogleTest, FP policy, 4 CI legs) are recorded but not formally accepted. Amendment 2b matters most — GUI Phase 2 compiles the solver with GUI-side flags, so the shipped worker will *not* inherit this branch's FP policy unless `add_legacy_engine_worker()` applies it.
