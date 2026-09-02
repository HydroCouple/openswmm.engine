# CPU (OpenMP) and GPU (Kokkos) Performance Review Plan — 2026-09-02

Status: PLAN (not started). Static review; no GPU hardware in the authoring environment.
Scope: 1D (`src/engine/hydraulics/`) and 2D (`src/engine/2d/`) parallel paths.

## 0. Premise correction

The request was to review "the CPU OpenMP and GPU/CUDA implementation" of both solvers.
Code survey establishes:

| Solver | CPU parallel path | GPU path |
|---|---|---|
| 1D DynamicWave | Persistent-team OpenMP (`DynamicWave.cpp:1328`) | **None.** Prototype in `plans/prototypes/dynwave_kokkos/` measured 0.06–0.81× serial at every size (`plans/1d/1D_DYNWAVE_GPU_KOKKOS_STRATEGY.md`) — offload ruled out. |
| 1D ExplicitFv | 6 per-phase `parallel for` regions inside the substep loop | **None.** `NetworkSolverFactory.cpp:146-175` dlopens for `openswmm_make_gpu_network_solver`; no plugin exports it. Factory always returns `ExplicitFvSolver`. |
| 2D ExplicitInertial | 14 `parallel for` regions + serial rebuild/publish | `ExplicitKokkosSurfaceSolver` (omp/cuda/hip/sycl plugin) |

So the GPU review is the 2D Kokkos solver only. The 1D review is an OpenMP-structure review.
The factory comment `SurfaceSolverFactory.cpp:241-244` already records the Kokkos-OMP plugin
as **15–20× slower than the serial marcher at 25k cells**; the CUDA path has never been profiled
(`plans/2d/perf_review_2026-08-22/2D_FV_PERFORMANCE_CRITICAL_REVIEW.md` F10 was code inspection only).
The impression that the GPU underperforms is consistent with the code as written.

## 1. Prior work this plan builds on (do not re-plan)

- `plans/2d/perf_review_2026-08-22/` — F1–F11 findings; F8 (renumbering), F9 (region fusion), F10 (GPU) still open.
- `plans/FV1D_PERF_PLAN_REVISED_2026-08-20.md` — Phase 4 (threading) deliberately quotes no speedup; persistent-team hypothesis stated.
- `plans/PHASE2_PERF_HANDOFF.md` — 1D OpenMP measured net-negative pre-persistent-team (303 s → 520 s at 8T).
- `CHANGELOG.md` L1823-1827 — DW persistent team + CSR gather: Bellinge 204 s → 56.5 s (T=8).
- `plans/2d/2D_GPU_DEFAULTS.md` — selection gates and numerics expectations (omp ≈1e-13, cuda 1e-4..1e-6 vs serial).

## 2. Hypotheses (ranked by expected payoff)

### 2D Kokkos solver (`src/engine/2d/gpu/ExplicitKokkosSurfaceSolver.cpp`)

| # | Hypothesis | Evidence (file:line) | Expected effect on CUDA |
|---|---|---|---|
| G1 | Host↔device traffic per `advance` dominates at default `COUPLING_SYNC` (one advance per routing step) | `pushForcings` :1114 (4×nt in), `pushNodeState` :1145 (3×nn in), `publishAndCopyBack` :1192-1206 (~7×nt out) | ~20 MB/step at 228k cells over PCIe; serialises the pipeline every step |
| G2 | Single-thread device kernels launched per substep | `fireBc` :862 and `fireExchange` :1006 both `RangePolicy(0,1)` | Each is a full launch + serial loop on one CUDA thread; 16 of 46 launches per macro cycle |
| G3 | Blocking host round-trips inside the marching loop | `cfl_min` :513, `cfl_refresh` :625 (`Min` reduce), 2K `parallel_scan` per rebuild :580/:594, 2 per tail :667/:676; control state `dt0_`, `n_active_`, `tier_off_` host-resident (`advance` :1223-1251) | GPU idles at every decision point; with K=4 that is ≥9 syncs per rebuild |
| G4 | Memory traffic regressions vs CPU path | `d_sign_` stored as `double` (:191-197) where CPU uses `int8_t`; `cell_arm_x/y` not mirrored, arm recomputed in `fireCells` :795; `publish_flux` walks full `ne` :1165 where CPU walks `active_faces_` | Hot kernels are memory-bound; 8× sign traffic |
| G5 | Kernel launch count and closure size | 46 launches per macro cycle; `fireFaces`/`fireCells` capture ~17–20 Views each | Launch latency ~5–10 µs each; large param space |
| G6 | Per-advance host allocation | `:1127-1134` rebuilds 4 `std::vector` of size nbc every advance | Minor, but per-step heap churn |
| G7 | Divergent/expensive device math | `tier_assign` uses `std::log2` (:537; CPU uses `ilogb`); VFR safeguarded Newton up to 64 iters (`VfrClosure.hpp:211-221`) | Warp divergence under VFR; minor under FLAT |
| G8 | Selection gates uncalibrated | `SurfaceSolverFactory.cpp:245-256` device floor 10k, omp floor 50k; `OPENSWMM_2D_MIN_PARALLEL_CELLS` never swept | Wrong backend chosen at mid sizes |
| G9 | Kokkos-OMP plugin loses to the native OpenMP marcher | Factory comment 15–20× slower at 25k; one `fence` total (:1190) but every reduce/scan is an implicit barrier | OMP backend may never win; the plugin only justifies itself on device |

Functional gaps found alongside (not perf, but block adoption): transport/species entirely absent from the Kokkos solver (`SurfaceRouter2D.cpp:794-797` constructs the plugin without the transport gate); head-ramp only reaches the serial marcher (`SurfaceRouter2D.cpp:1059` `dynamic_cast`); `SolverOptions2D.hpp:52-53` vs `:184` contradict on VFR support.

### 2D CPU marcher (`src/engine/2d/solver/ExplicitInertialSolver.cpp`)

| # | Hypothesis | Evidence |
|---|---|---|
| C1 | Serial rebuild stretches cap scaling | halo loop :455-459 over all `ne`; compaction `push_back` :460-462; tier assignment :507-524; face tiers :526-537 |
| C2 | Serial publish once per advance | :1243-1269 whole edge-flux publish serial |
| C3 | Static schedule over full mesh for sparse work | `lazySourcesOnly` :354, `syncAndRebuild` step 1 :394 — early-continue on active cells |
| C4 | Inconsistent min-reduction idioms | :478 per-thread array vs :574 `omp critical` |
| C5 | Fork/join per region (F9, open) | ~30 regions per macro cycle; scaling 2.79× on 10 cores |
| C6 | No mesh renumbering (F8, open) | scattered 2-cell gathers in `fireFaces`; 1.3–2× on real meshes per prior estimate |

### 1D ExplicitFv (`src/engine/hydraulics/fv/ExplicitFvSolver.cpp`)

| # | Hypothesis | Evidence |
|---|---|---|
| F1 | Fork/join per phase inside the substep loop, 3–5 forks per substep, thousands of substeps per step | `refreshDepths` :332, `computeFluxes` :1094, `computeUfGradients` :1490, `updateTpaFlags` :1514, `updateCells` :1633, `fireFaces` :3102 |
| F2 | FV never receives the active wait policy DW gets | `SWMMEngine.cpp:6163-6167` gates `OMP_WAIT_POLICY`/`KMP_BLOCKTIME` on `RouteModel::DYNWAVE` only |
| F3 | Serial phases between forks are comparable in cost to the parallel ones | `relaxNodeFluxes` :1329-1345, `limitPositivity` :1256-1323, `updateNodes` :2207-2255, `reconstructState` :840-909, `censusDt` :515-750 (serial min over active faces, twice per accepted step), `rebuildActiveLists` :403-509 |
| F4 | Thresholds uncalibrated | `kOmpMinFaces`/`kOmpMinCells = 4096` :29-37, compile-time, same for light and heavy bodies |
| F5 | `FvGeometry` is a >3 KB AoS struct indirected on every face/cell op | `NetworkMeshData.hpp:77-171`; hot scalars not co-located |
| F6 | No SIMD, FP-contract off caps autovectorisation | `src/engine/CMakeLists.txt:227`; no `omp simd` in FV |
| F7 | Superlinear scaling in network size (500→2000 conduits = 13.4× time) | `plans/FV1D_PERF_BASELINE_2026-08-20.md` — unexplained; suspect `rebuildActiveLists` halo loop or node CSR walks |
| F8 | LTS phase timers double-count | same baseline doc — must be fixed before any FV attribution is trusted |

### 1D DynamicWave — reference design, low priority

Persistent team with 8–10 barriers per Picard iteration; `updateNodeFlows` serial by choice; Apple P-core clamp. Review only for barrier count and the `omp single` serial structures phase (:3371). No Kokkos.

## 3. Review phases

Each phase has a deliverable and a verify step. Phases A–C are static and can run now. Phase D needs the CUDA box.

### Phase A — Instrumentation fixes (prerequisite; Tier A bit-identical)
1. Fix FV LTS timer double-counting (F8) → verify: `[PERF-FV]` phase percentages sum to 100 ± 1% on `fv_perf_baseline.py`.
2. Add per-kernel Kokkos timing to `ExplicitKokkosSurfaceSolver` behind `OPENSWMM_PERF=1`: launch counts, fence time, `deep_copy` bytes per advance → verify: counters printed, zero effect on `.out` sha256.
3. Wire `Kokkos::Tools` profiling hooks so `nsys`/kernel-logger attribute by kernel label (labels already exist).

### Phase B — Static code review (deliverable: `plans/CPU_GPU_PERF_REVIEW_FINDINGS_2026-09-XX.md`)
For each hypothesis G1–G9, C1–C6, F1–F7: confirm/refute by reading code, estimate cost analytically (bytes moved, launches, syncs per step), classify fix as Tier A (bit-identical), B (FP-reorder only), or C (numerics change), and estimate effort. Specific checks:

- G1: count bytes crossing PCIe per advance as a function of nt, nn, nbc, np. Decide which arrays can stay device-resident across advances (volume/head/depth only needed on host at output cadence; `edge_flux` only for coupling/render).
- G2: rewrite plan for `fireBc`/`fireExchange` as `RangePolicy(0,nbc)`/`(0,np)` with atomics or a per-point private accumulator; check whether the serial-order contract (`ExplicitInertialSolver.cpp:1006-1093`) is a numerics contract or an implementation accident.
- G3: enumerate every device→host scalar per macro cycle; design a device-side control path (dt0 kept on device, tier offsets computed by one scan over K tiers instead of K scans; or a 2-level scan).
- G4: mirror `int8_t` sign and `cell_arm_x/y`; gate `publish_flux` on compacted active faces.
- G8/G9: define the crossover experiment for Phase D.
- C5/F1: compare region count per substep against a persistent-team restructure (as DW did); estimate barrier count.
- F3: sum serial-phase extents vs parallel-phase extents per substep; rank which serial phase to parallelise first (likely `censusDt` and `limitPositivity`, both face-extent).
- F2: cost of extending the wait-policy gate to FV — one-line change, measure first.

### Phase C — CPU measurements (can run on macOS/Linux now)
Harness: `tests/benchmarks/scripts/fv_perf_baseline.py` (1D), `plans/2d/perf_review_2026-08-22/make_bench2d.py` + `tools/bench_2d/run_one.py` (2D). All outputs under `tests/benchmarks/generated/perf_review_2026-09/` (reviewable, not temp).

1. 2D: `bench2d_79202.inp` at T=1,2,4,8 with `OPENSWMM_2D_BACKEND=cpu` vs `=omp` (plugin). Record wall, scaling, and `.out` sha256 / max HDF5 diff. Establishes G9 and the C-series baseline.
2. 1D: FV configs at `OMP_NUM_THREADS=1,4,8` on Example1, TwinOaks-v2 (5086 cells), and the 2000-conduit synthetic. Then repeat with `OMP_WAIT_POLICY=active` exported by hand (F2). Bit-identity gate via sha256.
3. Sampling profile (existing `pmprof.c` or `perf`) of the 2D omp plugin and the FV 8T run to attribute barrier/futex time.

Success criteria: reproduce the 15–20× omp-plugin slowdown or refute it; quantify FV 1T→8T scaling and the wait-policy delta; confirm/refute F7 superlinearity by timing `rebuildActiveLists` in isolation.

### Phase D — CUDA measurements (Windows RTX 2000 Ada, preset `Windows-cuda`; user-run)
Runbook to be written in Phase B; outline:
1. `bench2d_{19602,79202}.inp` + a ~250k-cell variant from `make_bench2d.py`, `OPENSWMM_2D_BACKEND=cuda` vs `cpu` T=8. Record wall, `nsys` timeline (kernel vs memcpy vs idle), `ncu` on `fireFaces`/`fireCells` (achieved occupancy, DRAM throughput, registers).
2. Same with `COUPLING_SYNC 10` to isolate per-advance transfer cost (G1).
3. Crossover sweep for G8: 5k, 10k, 20k, 50k, 100k, 250k cells.
4. Numerics: cuda vs cpu max |Δ| on head/volume; expect 1e-4..1e-6 per `2D_GPU_DEFAULTS.md`.

Success criteria: a measured breakdown of GPU wall into {kernel, transfer, sync/idle, launch}. If transfer + idle > 50%, G1–G3 are confirmed and are the implementation plan.

### Phase E — Recommendation
Rank fixes by (measured cost × confidence) / effort. Expected ordering from static evidence: G1+G3 (device residency + control state), G2 (parallel bc/exchange), G4, then F1+F2 (FV persistent team + wait policy), then C1/C5. Write as an implementation plan with per-phase Tier A/B/C gates matching the existing `.out` sha256 and 18-deck parity corpus (`tests/parity/run_corpus.sh`).

## 4. Out of scope
- Implementing any fix (this is a review plan).
- 1D GPU offload (negative result already recorded).
- Metal/Apple GPU (no fp64; separate strategy doc).
- CVODE-era 2D preconditioner mirror to Kokkos (`2D_SOLVER_STEPPING_PERFORMANCE_PLAN.md` Phase 1) — unless the explicit marcher is abandoned.

## 5. Open questions for the owner
1. Which model triggered the impression? Cell count and `COUPLING_SYNC` value determine whether G1 or G3 dominates. Also confirm the CUDA plugin was actually selected: below 10k cells `auto` never tries cuda (`SurfaceSolverFactory.cpp:249-254`), and a probe failure silently falls back to the CPU marcher (:229-232). Phase D step 0: run with `OPENSWMM_2D_BACKEND=cuda` and capture the stderr backend line.
2. Is the serial order of `fireExchange` over coupling points a numerics contract (must match CPU bit-for-bit) or incidental? Decides whether G2 is Tier A or B.
3. Is the transport gap in the Kokkos solver known/accepted? It affects whether GPU results are comparable on quality-enabled decks.
