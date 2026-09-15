# OdeWorkspace thread_local leak — falsifier results

Date: 2026-09-09
Subject: `src/engine/math/OdeSolver.cpp`, `thread_local OdeWorkspace ws_`

## The defect

`ensureWorkspace()` allocates six `std::calloc` buffers (`y`, `yscal`, `dydx`,
`yerr`, `ytemp`, `ak`) into a `thread_local OdeWorkspace`, and frees them only
on the grow path (`if (ws_.nmax >= n) return;` — the steady state never frees).

`OdeWorkspace` had only `int` and `double*` members with constant initializers
and no user-declared destructor, so it was **trivially destructible**. That is
the crux: no `__cxa_thread_atexit` handler is registered for a trivially
destructible thread_local, so thread exit reclaims the TLS block holding the
*pointers* while the buffers they point at stay allocated and unreachable.

Legacy has the matching teardown — `odesolve_close()` at
`src/legacy/engine/odesolve.c:67`, called from `src/legacy/engine/runoff.c:135`
and paired with `odesolve_open()` at `runoff.c:97`. The C++ port kept the open
half and dropped the close half, while the comment above the struct claimed
"open/close match legacy odesolve_open/close".

## Why the existing sanitizer build never caught it

Not reachability — measured directly:

```
$ ASAN_OPTIONS=detect_leaks=1 ./lsanprobe
==56890==AddressSanitizer: detect_leaks is not supported on this platform.
```

LeakSanitizer does not run on Darwin arm64, so `build/darwin-asan`
(`-fsanitize=address,undefined`) has never performed leak detection here at all.
Hence this falsifier measures the allocator directly instead.

## Method

`main.cpp` runs sequential thread create/integrate/join cycles and reads
`mstats().bytes_used` (exact allocator accounting) plus task RSS around them.

The production call sites use n = 1 (runoff ponded depth, `Runoff.cpp:149`) and
n = 2 (groundwater, `Groundwater.cpp:349`), i.e. ~160 bytes per thread — far
below allocator noise. The probe drives n = 65536 so the workspace is
80·n = 5.00 MiB per thread, over 16 threads: 80.00 MiB expected if leaking.

## Results

| Build | heap delta (mstats) | rss delta | verdict |
|---|---|---|---|
| before fix (HEAD) | **80.00 MiB** | 80.94 MiB | LEAKING |
| after fix | **0.00 MiB** | 0.06–6.08 MiB | CLEAN |

The before figure is exactly 16 × 5.00 MiB — every thread stranded its whole
workspace, to the byte. The after figure is zero; the residual RSS wobble is
allocator page retention, not held allocations.

## Fix

`OdeWorkspace` gained a destructor that frees all six buffers and then resets
the pointers and `nmax` to the constructed state, mirroring `odesolve_close()`.
The reset is not cosmetic: `ensureWorkspace()` early-returns while
`nmax >= n`, so leaving `nmax` set would hand freed pointers to any
`integrate()` running after teardown, since thread_local destruction order is
not guaranteed.

A `static_assert(!std::is_trivially_destructible<OdeWorkspace>::value)` pins the
property, so removing or `=default`-ing the destructor fails the build rather
than silently restoring the leak on a platform with no leak checker.

## Scope

Leaks once per **thread that ever calls `integrate()`**, not once per OpenMP
worker: neither call site sits inside a parallel region (the non-legacy
`#pragma omp parallel` sites are DynamicWave, the FV/2D solvers and
VertexReconstruction). The GUI-relevant vector is the engine running on a
pooled thread, so each run landing on a fresh worker stranded a fresh
workspace.

At the real n of 1–2 that is ~160 bytes per thread, so this is a genuine leak
and a genuine latent leak-checker finding, but it is **not** a plausible cause
of GUI long-run memory growth — reaching 100 MB would take on the order of a
million distinct threads.

## Reproduce

```sh
tests/manual/ode_workspace_leak/build.sh
tests/manual/ode_workspace_leak/obj/ode_workspace_leak   # exit 0 = clean, 1 = leaking
```
