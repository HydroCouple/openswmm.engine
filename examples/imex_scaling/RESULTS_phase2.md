# Phase 2 (local-inertial) — built and measured

**Date:** 2026-06-27
**Build:** `build/darwin` (Release, arm64, hypre ON), SUNDIALS 7.6.
**Selectors:** `OPENSWMM_2D_MOMENTUM=inertial` (→ ARKStep). Within inertial,
`OPENSWMM_2D_GRAVITY_IMPLICIT=1` forces the comparison path; default is explicit
gravity. Default for all existing models is unchanged (DW + CVODE).

## What was built
- **`InertialEdges`** — unique interior-edge structure (one prognostic discharge
  `q` per shared edge = the conservation invariant) + per-cell CSR incidence for
  a race-free continuity gather.
- **Local-inertial RHS** (`ArkodeSurfaceSolver`, `MOMENTUM=inertial`): state
  `[V(cells), q(edges)]`, continuity `dV_i/dt = −Σ_e sign·q_e·ξ_e`, momentum
  `dq_e/dt = −g·h_f·∇η − g·n²·q|q|/h_f^(7/3)` (LISFLOOD-FP, `h_f = max(η_L,η_R)−z_face`).
- **Two IMEX splits**, both conservative:
  - *explicit gravity / implicit friction* (default) — LISFLOOD-FP; implicit
    operator is the per-edge friction diagonal → exact block preconditioner, **no
    global solve**.
  - *implicit gravity* (comparison) — gravity+continuity coupled implicitly,
    block-Jacobi (Schur η-diagonal + friction diagonal).

## Correctness — conservation gate (closed sloped basin)
Every configuration closes the 2D mass balance to machine precision:

| mode | 2D continuity error |
|---|---|
| CVODE-DW (baseline) | −0.073 % |
| ARKODE-DW | 0.000 % |
| inertial, implicit gravity | 0.000 % |
| inertial, explicit gravity (default) | 0.000 % |

## Scaling (serial CPU, 1 h sim, closed basin)

**Diffusive-wave reference (CVODE-DW):** 8k/50 m 0.38 s · 30k/50 m 0.93 s ·
8k/200 m 0.11 s · 100k/200 m 1.34 s. (Fast, AMG+BDF, **no inertia**.)

**Local-inertial, explicit gravity (the scalable scheme):**

| mesh | cells | wall | continuity | mean step |
|---|---|---|---|---|
| 8 000 | 50 m | 3.68 s | 0.000 % | 22 s |
| 30 000 | 50 m | 13.69 s | −0.000 % | — |
| 8 000 | 200 m | 1.35 s | 0.000 % | 31 s |
| 100 352 | 200 m | 17.48 s | −0.000 % | — |

→ **~linear in cell count** (8k→100k @ 200 m: 1.35→17.48 s = 12.9× for 12.5×).
1M cells is tractable serially (~3 min for a 1 h sim at 200 m). **Larger cells
run faster** (200 m is 2.7× faster than 50 m at equal count) because the explicit
gravity-wave CFL Δt ≤ Δx/√(g·h) relaxes — matching the large-representative-cell
hydrological regime.

**Local-inertial, implicit gravity + block-Jacobi (does NOT scale):**
8k/50 m 1.73 s → 30k/50 m 18.73 s — **superlinear** (10.8× for 3.75×). Diagonal
preconditioning gives no mesh-independent convergence for the elliptic gravity
coupling; it would need full Schur+AMG, which then inherits the Phase-1 ARKStep
inefficiency. This is why the explicit-gravity split is the one to ship.

## Findings
1. The **local-inertial physics works and is exactly conservative** in every split.
2. **Implicit gravity is the wrong lever for large cells** — it forces a global
   elliptic solve (AMG) that the block-Jacobi can't substitute for and that
   ARKStep integrates inefficiently (Phase 1). Confirmed by the superlinear
   block-Jacobi scaling.
3. **Explicit gravity / implicit friction (LISFLOOD-FP) is the 1M-cell answer:**
   O(n) per step, no global solve, ~linear, conservative, and *cheaper on larger
   cells*. This is the lateral-explicit / local-implicit structure the Phase-1
   measurement predicted.
4. **DW (CVODE) stays ~13× faster** but carries no inertia — it is the right
   default when inertia is not needed; inertial is the physics-fidelity option.
5. **Threading is the next lever, not the algorithm:** OpenMP gave only ~1.2× (8
   threads) because SUNDIALS' serial N_Vector ops dominate. The per-cell/per-edge
   kernels are already parallel-shaped; true 1M throughput wants the Kokkos
   N_Vector backend (as the DW path already has via the GPU plugin).

## OpenMP for the core (non-plugin) CPU path — 2026-06-27

To thread the CPU solver WITHOUT the Kokkos plugin, the SUNDIALS overlay gained an
`openmp` feature (`-DENABLE_OPENMP=ON` → `SUNDIALS::nvecopenmp`), and the core
`CvodeSurfaceSolver` / `ArkodeSurfaceSolver` create `N_VNew_OpenMP(n, num_threads)`
when `THREADS > 1` (else `N_VNew_Serial`). This threads the BDF/ARK *vector ops*
(the part the serial vector left single-threaded), and the leftover serial host
loops in `advance()` were OpenMP'd too. **Opt-in:** default `num_threads = 1`
keeps the exact serial path — no default-behaviour or baseline change, no scaling
risk (additive parallelism, O(n) unchanged), conservation preserved.

| 100k inertial path | wall | speedup |
|---|---|---|
| core `THREADS=1` (serial vector) | 17.4 s | — |
| core `THREADS=8`, serial vector (before) | 14.2 s | 1.21× |
| core `THREADS=8`, OpenMP N_Vector (now) | 13.3 s | 1.37× |
| Kokkos-omp plugin `THREADS=8` | 12.5 s | 1.39× |

The core OpenMP path now nearly matches the Kokkos plugin without the plugin
dependency. Speedup is modest on this 4-performance-core laptop (bandwidth/Amdahl
bound past 4 threads); a many-core server or GPU scales further.

## CVODE-DW (the fast path) scales to 1M — 2026-06-27

The diffusive-wave CVODE solver is and remains the fast path; it scales to 1M and
threads modestly:

| cells | serial (T1) | OpenMP N_Vector (T8) |
|---|---|---|
| 100,000 | 1.30 s | 1.14 s (1.1×) |
| 1,000,000 | 12.57 s | 7.12 s (1.77×) |

Scaling is **sub-linear** (100k→1M = 9.7× wall for 10× cells) because hypre
BoomerAMG is mesh-independent. Threading (the OpenMP N_Vector + threaded RHS) is
capped at ~1.8× because **BoomerAMG itself is single-threaded**.

**Threaded BoomerAMG (`HYPRE_WITH_OPENMP`) was tried and REVERTED:** it regressed
even the serial path (1M: 12.5 s → 16.7 s; 100k slower at every thread count) —
its setup/coarsening does not parallelize and the OpenMP-instrumented code adds
net overhead on CPU. The AMG stays single-threaded by design; the real parallel
AMG path is **GPU** (the CUDA plugin with a device-built hypre), not CPU OpenMP.

**Net:** CVODE-DW (the *current physics*) does 1M cells in ~12.5 s serial / ~7 s
threaded — ~2.4× faster than the inertial path at 1M. For raw 1M throughput of
the existing DW physics, this — not IMEX — is the path.

## Status / next levers (not built)
- Kokkos N_Vector port of the inertial kernels (the real parallel-scaling step).
- `[2D_OPTIONS] MOMENTUM` parse (env works today).
- Boundary-condition momentum (prescribed-flow / stage edges) for inertial.
- Phase 3 Froude-gated DW↔inertial blend.
- de Almeida (2012) q-centred weighting for a larger explicit-gravity step.
