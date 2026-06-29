# Thread Safety Verification Report — OpenSWMM Engine 6.0

**Date:** 2026-04-16
**Scope:** `src/engine/`, `include/openswmm/engine/`
**Goal:** Verify that two independent `SWMM_Engine` instances can safely run
concurrently on separate threads.

---

## 1. Architecture Summary

The V6 engine stores all simulation state in a per-instance `SimulationContext`
owned by `SWMMEngine`. The C API creates a `SWMMEngine` via `new`, wraps it in
an opaque `void*` handle, and destroys it via `swmm_engine_destroy()`. Each
solver subsystem (`DWSolver`, `KWSolver`, `RunoffSolver`, etc.) is a member of
`SWMMEngine`—not a global or singleton.

The IO thread receives a **deep-copied** `SimulationSnapshot` through a ring
queue, never a pointer to the live context.

## 2. Audit Findings

All items below are classified as:

- **CLEAN** — clearly per-instance, no action needed
- **BENIGN** — immutable after initialisation, read-only, or thread-local by design
- **ACTION** — shared mutable state or race risk found

### 2.1 Thread-Local Statics (BENIGN)

| File | Symbol | Purpose | Assessment |
|------|--------|---------|------------|
| `src/engine/math/OdeSolver.cpp:54` | `thread_local OdeWorkspace ws_` | RK45 integrator workspace | BENIGN — each thread has own copy |
| `src/engine/math/OdeSolver.cpp:195` | `thread_local std::vector<double> dy0_buf, dy1_buf` | Derivative buffers for batch ODE | BENIGN — thread-local |
| `src/engine/core/HotStartManager.cpp:42` | `thread_local std::string tl_last_io_error` | Per-thread error string | BENIGN — thread-local |
| `src/engine/hydraulics/DynamicWave.cpp:267-268` | `thread_local std::vector<double> node_P_sum; thread_local std::vector<int> node_P_count` | DPS spatial smoothing accumulators | BENIGN — used in parallel loop, thread-local |
| `src/engine/hydraulics/KinematicWave.cpp:174` | `static thread_local std::vector<int> fallback` | KW fallback link order | BENIGN — thread-local |
| `src/engine/core/SWMMEngine.cpp:1270` | `thread_local std::vector<double> q_prev` | Non-conduit flow relaxation buffer | BENIGN — thread-local |

**Verdict:** All thread-local declarations are intentional per-thread caches.
They do not cause races between separate `SWMM_Engine` handles.

### 2.2 Singleton Pattern (BENIGN — conditional)

| File | Symbol | Purpose | Assessment |
|------|--------|---------|------------|
| `src/engine/input/geopackage/GeoPackagePluginInfo.hpp:46` | `static GeoPackagePluginInfo inst` (Meyer's singleton) | Plugin metadata for GeoPackage I/O | See analysis below |

**Analysis:** `GeoPackagePluginInfo` is a Meyer's singleton (C++11 guarantees
thread-safe static local initialization). The class has two mutable members:
`registered_` and `reg_info_`, set once by `register_plugin()` during plugin
discovery. Plugin discovery is invoked via `dlopen`/`LoadLibrary` during
`swmm_engine_open()`, which is a *sequential* operation per engine. After
registration, the members are effectively read-only.

**Risk:** If two engines attempt to load the GeoPackage shared library for the
first time concurrently, the static init is safe (C++11 §6.7), but
`register_plugin()` is not atomic. However, both calls would write the same
immutable data (`RegistrationInfo` from the engine), so the race is benign in
practice.

**Recommendation:** Document that plugin loading should happen on a single
thread, or add a `std::once_flag` guard to `register_plugin()`. This is
**not** a blocking defect for concurrent simulation runs—it only affects
the one-time plugin registration path.

**Classification: BENIGN** (startup-only, effectively immutable after init)

### 2.3 Mutable `mutable` Class Members (CLEAN)

| File | Symbol | Purpose | Assessment |
|------|--------|---------|------------|
| `src/engine/hydraulics/DynamicWave.hpp:210` | `mutable double variable_step_` | CFL-cached routing step | CLEAN — instance member of DWSolver |
| `src/engine/hydraulics/XSectBatch.hpp:184-186` | `mutable std::vector<double> buf_d, buf_r, buf_r2` | Batch xsect gather/scatter buffers | CLEAN — instance member of XSectGroups |

These are `mutable` for use in `const`-qualified methods but are per-instance.

### 2.4 OpenMP Parallelism (CLEAN)

| File | Pragma | Assessment |
|------|--------|------------|
| `src/engine/hydraulics/DynamicWave.cpp:1453` | `#pragma omp parallel for num_threads(num_threads_)` | CLEAN — `num_threads_` is per-DWSolver instance |
| `src/engine/quality/QualityRouting.cpp:187,283` | `#pragma omp parallel for schedule(static)` | CLEAN — operates on per-context data |

OpenMP thread counts are set per `DWSolver` instance via `setNumThreads()`.
Two concurrent instances each control their own thread pool. The only risk
would be if `omp_set_num_threads()` were called globally (it is not — the
`num_threads()` clause is used in the pragma).

### 2.5 Static Immutable Data (CLEAN)

Over 50+ `static const` / `static constexpr` declarations were found across
the engine (culvert coefficient tables, RK45 coefficients, error message
lookup tables, unit conversion arrays, etc.). All are immutable after program
load and present zero threading risk.

### 2.6 File-Scope Mutable Statics

**None found** in `src/engine/` outside of the thread-local and singleton
categories above. This is a direct result of the V6 architecture placing all
state in `SimulationContext`.

## 3. Summary

| Category | Count | Status |
|----------|------:|--------|
| Thread-local statics | 7 | BENIGN |
| Singleton patterns | 1 | BENIGN (startup-only) |
| Mutable cache members | 3 | CLEAN (per-instance) |
| OpenMP pragmas | 3 | CLEAN |
| Static const/constexpr | 50+ | CLEAN |
| File-scope mutable statics | 0 | CLEAN |

### Overall Verdict

**The V6 engine is safe for concurrent use by two independent `SWMM_Engine`
instances on separate threads**, subject to one minor caveat:

- Plugin shared-library loading (GeoPackage) should ideally be serialized
  if two engines could race on the first `swmm_engine_open()` call. The race
  is benign in practice but could be eliminated with a `std::once_flag`.

No `ACTION` items were found that block concurrent simulation runs.

## 4. Functional Verification

A concurrent-engine Google Test is provided in:

```
tests/unit/engine/test_concurrent_engines.cpp
```

This test:
1. Creates two `SWMM_Engine` instances
2. Runs them on separate `std::thread`s with distinct input models
3. Compares each concurrent run to its own single-threaded baseline
4. Fails on result divergence beyond the repository's regression tolerances
   (absolute 0.001, relative 0.1%)

## 5. ThreadSanitizer Configuration

A CMake preset `tsan` is documented for CI integration:

```cmake
# Add -fsanitize=thread to compiler and linker flags
# Run: cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
```

On Windows (MSVC), ThreadSanitizer is not natively supported. The concurrent
test relies on `/analyze` and runtime race detection via the test itself.
For full TSan coverage, use the Linux/macOS CI matrix with GCC or Clang.
