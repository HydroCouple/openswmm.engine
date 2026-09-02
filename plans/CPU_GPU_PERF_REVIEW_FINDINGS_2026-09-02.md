# CPU (OpenMP) and GPU (Kokkos) Performance Review — Findings — 2026-09-02

Status: Phase B COMPLETE (static review). Phase A instrumentation landed. Phase C
measurements in progress (results appended in §7). Phase D (CUDA) is user-run —
runbook in §8.
Parent plan: `plans/CPU_GPU_PERF_REVIEW_PLAN_2026-09-02.md`.
Base commit reviewed: `bbd13775` (measurements from a detached worktree at that
commit — the live tree carries a peer session's in-progress transport edits).

Tier legend (acceptance gates): **A** = bit-identical `.out`; **B** = FP
reordering only (parity corpus within tolerance); **C** = numerics change
(needs its own validation).

## 0. Phase A status

| Item | Status |
|---|---|
| A1 FV LTS timer double-count (F8) | **Already fixed** before this review — commit `7fb60748` introduced `GatedTimer`/`nested_wall` self-time accounting (`PerfTimers.hpp:252-296`); all 16 FV phase timers use it. Verified empirically in Phase C (§7). |
| A2 Kokkos launch/fence/deep_copy counters | **Implemented this round**: `src/engine/2d/gpu/KokkosPerfCounters.hpp` (new) + hooks in `ExplicitKokkosSurfaceSolver.cpp`. Rides in-process `Kokkos::Tools` callbacks, so every launch/fence/deep_copy is counted with zero code in the dispatch sites. Gated on `OPENSWMM_PERF`; dumps `[PERF-2D-KOKKOS]` + per-kernel lines at solver finalize. |
| A3 Kokkos::Tools wiring | **Implemented**: `Kokkos::Profiling::pushRegion` phase regions (`openswmm_2d_advance_push/march/publish`) in `advance()`; the A2 counters self-disable when `KOKKOS_TOOLS_LIBS`/`KOKKOS_PROFILE_LIBRARY` is set so external tools (nvtx-connector for nsys, kernel-logger) receive the callbacks instead. Kernel labels already existed. |

## 1. 2D Kokkos solver (G1–G9) — `src/engine/2d/gpu/ExplicitKokkosSurfaceSolver.cpp`

### G1 — Host↔device traffic per advance. CONFIRMED (analytic). Tier A fix.

Per `advance()` (line refs pre-A2-edit):
- in: `pushForcings` :1114 — 4×nt doubles (rain, coup, evap, infil) = 32·nt B, plus 4 boundary arrays ≈ 28·nbc B;
- in: `pushNodeState` :1145 — 3×nn doubles = 24·nn B;
- out: `publishAndCopyBack` :1192-1206 — volume, head, depth (3·nt), edge_flux (3·nt slots), infil_applied (nt) = 7·nt doubles = 56·nt B, plus exch (np).

**Total ≈ 88·nt + 24·nn B per advance.** At 228k cells that is **20.1 MB/advance**;
at `COUPLING_SYNC 1` with a 1 s routing step, ~1.7 TB/day of PCIe traffic —
~145 s/day at 12 GB/s effective, all of it serializing the pipeline (the
`Kokkos::fence()` at :1190 sits in front of the D2H block).

Residency decision (what actually must cross per advance):
- `coup` (windowed coupling flux) and the 3 node arrays — genuinely per-advance, but nn ≪ nt.
- `rain/evap/infil` — change on the wet-weather/climate cadence, not per advance. Push only on change (dirty flag host-side).
- `volume/head/depth` — needed on host only at output cadence and for coupling consumers; publish on demand or at report cadence.
- `edge_flux` — coupling/render only; same.
- `exch` (np doubles) — the only per-advance mandatory D2H.

Fix: dirty-flag forcings; move `publishAndCopyBack` to an explicit
`publish()` the router calls at output/coupling cadence. Tier A (identical
values, fewer copies). Effort: 2–3 days (router contract touch).

### G2 — Single-thread device kernels per tier-0 fire. CONFIRMED. Parallel rewrite is NOT Tier A.

`fireBc` :862 and `fireExchange` :1006 are `RangePolicy(0,1)` — one CUDA
thread walks all nbc / np points. 16 of the 46 launches per macro cycle
(8 tier-0 `fireCells` × 2).

**The serial order is a numerics contract, not an accident** (plan open
question 2, answered): in the CPU reference (`ExplicitInertialSolver.cpp`,
current lines :872-1004 boundary, :1011-1093 exchange) later iterations read
state earlier iterations mutate — `volume[i]` (a cell with 2–3 boundary edges
appears 2–3× in `bc_cell_`; the availability floor and stage equilibrium clamp
bind against the post-first-edge volume), `head/depth[i]`, `qcx_/qcy_[i]`, and
the first-come-first-served node budget `node_drawn_[ni]` (:1033/:1037).
EIS:1079-1082 documents the order-dependence explicitly.

Rewrite plan (recorded for Phase E):
1. Tier A now: fuse `fireBc`+`fireExchange` into one launch (halves the
   single-thread launches, order preserved). Trivial.
2. Tier A structural: group points by cell (bc) / by (cell, node) component
   (exchange) host-side at initialize — the overwhelming majority of groups
   have one member; launch one thread per group, each thread walking its group
   in serial order. Bit-identical if within-group order and FP expressions are
   unchanged. Effort: 2–3 days.
3. Tier C: full two-pass request/scale-to-budget. Only if profiling shows
   groups are large; changes budget allocation.

### G3 — Blocking host round-trips in the marching loop. CONFIRMED. Tier A/B fix.

Device→host scalars per macro-cycle window (K = 4):
- rebuild (1 in 5 cycles): `cfl_min` Min-reduce → dt0 (:513-528), then **2K = 8**
  blocking `parallel_scan` count results (:580-601) = **9 syncs per rebuild**;
- non-rebuild cycles (4 in 5): `cfl_refresh` Min-reduce (:625-637) = 1 sync;
- tail landing (once per advance, and per the CPU marcher's comment the tail
  fires in *every* window under per-routing-step coupling): `collapseToGlobalDt`
  = 2 blocking scans (:667-683).
- loop control `dt0_`, `n_active_`, `tier_off_[]`, `ftier_off_[]` all
  host-resident (:1223-1251) — every `while` iteration decision drains the
  device pipeline.

Amortized ≥ 2.6 blocking syncs per macro cycle *before* the tail. Design
sketch: keep `dtcell` min in a device scalar; replace the K cell scans + K edge
scans with ONE scan each over a (tier,index) key (offsets fall out per tier);
fetch {dt0, n_active, offsets} once per rebuild in a single 80-byte D2H instead
of 9 round-trips. The `nsub·dt0 > remaining` decision needs dt0 on host once
per cycle — unavoidable, but 1 sync/cycle, not 9. Tier A if scan order per tier
is preserved (compaction order is ascending either way). Effort: 3–4 days.

### G4 — Memory-traffic regressions vs the CPU path. CONFIRMED. Tier A.

The CPU marcher already has all three optimizations; the Kokkos port dropped them:
- `d_sign_` stored as `double` (:191-197, comment even cites the CPU's int8
  "gather-traffic diet") vs `int8_t` in `InertialEdges.hpp:100` → 8× sign bytes
  in `fireCells`/`settle` CSR walks;
- `cell_arm_x/y` (`InertialEdges.hpp:105`, used EIS:773-777) not mirrored —
  `fireCells` :826-830 recomputes `mx(e)-cx(i)` per CSR entry: 2 gathers + sub
  where the CPU does 1 sequential load;
- `publish_flux` :1165-1178 walks all `ne` where the CPU publish walks the
  compacted `active_faces_` (EIS:1251) — plus the full-`ne` deep_copy zero
  at :1155.

Effort: 1 day. Bit-identical.

### G5 — Launch count and closure size. CONFIRMED (static); measure in Phase D.

46 launches per macro cycle at K=4/nsub=8: 15 `fireFaces` + 15 `fireCells` +
16 single-thread bc/exchange. `fireFaces` captures ~20 Views, `fireCells` ~24.
At 5–10 µs launch latency → 0.25–0.5 ms per macro cycle floor before any work.
The A2 counters print launches/advance to make this exact per deck. Reduction
levers: G2 fusion (−8), graph/stream capture on device backends (Phase D
decision), K sweep (LTS_TIERS trades launches against work).

### G6 — Per-advance host allocation. CONFIRMED, minor. Tier A, trivial.

`pushForcings` :1127-1141 rebuilds 4 `std::vector`(nbc) per advance. Hoist to
members sized once at initialize. 30 minutes, do alongside G1.

### G7 — Divergent/expensive device math. CONFIRMED, minor.

- `tier_assign` :545 uses `std::log2`; the CPU switched to `std::ilogb` with a
  measured rationale (EIS:514-519: "std::log2 measured 1.8% of the run") and a
  documented ULP edge where they disagree — so this is both a perf and a
  **potential omp-backend bit-parity** divergence vs the serial marcher.
  Fix: use `ilogb` on device too. Tier A (it is the exactly-correct one).
- VFR safeguarded Newton up to 64 iters (`VfrClosure.hpp:211`) → warp
  divergence under VFR on device; FLAT decks unaffected. No action until
  Phase D profiles a VFR deck.

### G8 — Selection gates uncalibrated. CONFIRMED.

`SurfaceSolverFactory.cpp:249-256`: device floor 10k (`OPENSWMM_2D_MIN_PARALLEL_CELLS_DEVICE`),
omp floor 50k (`OPENSWMM_2D_MIN_PARALLEL_CELLS`); neither ever swept.
Crossover experiment defined in Phase D runbook (§8, step 3).

### G9 — Kokkos-OMP plugin vs native OpenMP marcher. Measured in Phase C (§7).

Static expectation stands: the plugin's every reduce/scan is an implicit
barrier, the marcher amortizes fences over an advance; factory comment records
15–20× slower at 25k cells. If Phase C reproduces it, the omp plugin's only
role is CI coverage of the Kokkos source — the 50k auto floor should probably
become "never" (device backends keep their own floor).

### Functional gaps (adoption blockers, not perf)

1. **Transport/species entirely absent from the Kokkos solver — and nothing
   gates it.** `SurfaceRouter2D.cpp:794-796` constructs via
   `makeSurfaceSolver(options_, nullptr, n_triangles)`; the factory checks env,
   backend and cell floors only. `grep transport|cell_mass` over the Kokkos
   solver: zero hits. A `[POLLUTANTS]`/water-age/heat deck on a ≥10k-cell mesh
   with a device plugin **silently transports no species**. Fix: factory gate
   (fall back to CPU marcher when transport is enabled) + one warning line.
   Must land before any GPU default-on.
2. Head-ramp reaches the serial marcher only (`SurfaceRouter2D.cpp:1053-1059`
   `dynamic_cast`); `OPENSWMM_2D_HEAD_RAMP=1` is a silent no-op on plugins.
3. `SolverOptions2D.hpp` contradicts itself on VFR: :51-53/:60 say "CPU solver
   only; Kokkos degrades to FLAT", :183-185 say "fully implemented on all
   backends". **Code inspection says the Kokkos solver DOES implement VFR**
   (`cell_closure == VFR` drives `etaDepthScalar`/`faceFlowDepthVfr`
   throughout), so :52-53/:60 are stale — but no test pins omp-backend VFR
   parity, which is how the two texts could drift.

## 2. 2D CPU marcher (C1–C6) — `src/engine/2d/solver/ExplicitInertialSolver.cpp`

All six CONFIRMED (current-tree line numbers; file is 1343 lines).

| # | Verdict | Key numbers | Fix tier / effort |
|---|---|---|---|
| C1 | CONFIRMED (halo :455-459, compaction :460-462, tier assign :507-524, face tiers :527-537 all serial) | Serial rebuild work ≈ 2·ne+nt+na ≈ 4·nt+na element visits — **>50% of the rebuild's traffic, Amdahl-capped at ~2× regardless of threads**, and it touches full nt/ne even when the active set is tiny | B (scan-based compaction reorders push_back only if kept ascending — keep ascending → A). 2–3 days |
| C2 | CONFIRMED (publish :1243-1269 serial; already narrowed to `active_faces_`) | One serial pass per advance: `fill` over 3·nt + \|active_faces\| iterations, 2 scattered stores each; iterations independent — the missing piece is literally one `parallel for` | A. Half a day |
| C3 | CONFIRMED (:354, :394 — `schedule(static)` over full nt with early-continue on the *inactive* complement; no dynamic/guided anywhere in the file) | Worst imbalance exactly in the normal regime (small clustered active set) | A (schedule change) after C1 compaction gives an inactive-list to iterate. 1 day |
| C4 | CONFIRMED (:476-504 per-thread partial array — false-sharing 8-byte stride; :557-578 `omp critical`) | `refreshDt0` critical entered nthr× on 4 of 5 cycles | A. Hours — pad partials to cache lines, use same idiom in both |
| C5 | CONFIRMED — 9 parallel regions; **≈31.6 fork/joins per macro cycle** (~3.95/base substep): 15 fireFaces + 15 fireCells + amortized 1.6 rebuild/refresh | DW's persistent team does ONE fork per routing step with 9-10 barriers/Picard — the restructure precedent exists in-repo. A persistent-team marcher ≈ 30 barriers/cycle worst case, ~15 with `nowait` staging | B (FP unchanged; scheduling changes only if idioms kept). 4-5 days, after C1/C2 land |
| C6 | CONFIRMED — no renumbering pass anywhere in 2D mesh build (`InertialEdges.cpp:40-93` raw triangle-major, no sort); `fireFaces` does up to 8 scattered double loads per face (:595-629) | Prior estimate 1.3–2× on real meshes stands | A (pure permutation at build). 3-4 days incl. permutation of state arrays + coupling maps |

## 3. 1D ExplicitFv (F1–F8) — `src/engine/hydraulics/fv/ExplicitFvSolver.cpp`

### F1 — Fork/join per phase. CONFIRMED, and worse than hypothesized under LTS.

Exactly 6 `parallel for` regions (:333, :1095, :1491, :1515, :1634, :3103), no
persistent team, and **zero OpenMP in the transport kernels**
(`src/engine/transport/fvkernels/`).

- Non-LTS: 2–4 fork/joins per accepted substep (`takeSubstep` :2534: TpaFlags →
  fluxes → [UF gradients] → cell update); ×2 under RK2; ×attempts under the
  retry loop (≤8).
- **LTS: exactly 1 fork per base substep** — `fireFaces` :3103 only.
  `computeUfGradients` takes its serial branch under LTS (:1487),
  `fireCells`/`fireNodes` (:3232/:3310) have no OpenMP at all, and the fork is
  gated on the *due-subset* size ≥ 4096 — tier-0 substeps with small due sets
  never parallelize. **LTS mode runs ~80% serial; LTS and threading are close
  to mutually exclusive as written.**

### F2 — Wait policy gated to DYNWAVE. CONFIRMED.

`SWMMEngine.cpp:6161-6180`: `rm == RouteModel::DYNWAVE` strict equality guards
`OMP_WAIT_POLICY=active` / `KMP_BLOCKTIME=infinite` / `kmp_set_blocktime`. FV
runs passive-wait: per the code's own comment ~5–30 µs per barrier vs 0.5–3 µs
→ ~20–500 µs pure futex sleep/wake per substep at F1's fork counts. Fix is a
one-line gate extension; Phase C measures the delta first via env (§7).
Tier A. Minutes.

### F3 — Serial phases. CONFIRMED for all six; ranking for Phase E.

Heavy serial, face-extent, fires 2×/substep: **`censusDt`** (:515; also
`anyPressurizedCell` :2478 is a serial O(nc) scan called at :3538 and per retry
attempt). Heavy serial, node-CSR: **`relaxNodeFluxes`** (:1329).
Light-but-wide: `limitPositivity` (:1256, 3 passes), `updateNodes` (:2207 —
walks the same CSR range twice, :2216 vs :2225, trivially fusable),
`reconstructState` (:821 — chain loop :850 embarrassingly parallel over
chains, left serial), `rebuildActiveLists` (see F7).

Parallelize-first ranking: (1) censusDt, (2) limitPositivity, (3)
reconstructState chains, (4) updateNodes (fuse walks first). All Tier A if
reductions use deterministic per-thread partials.

### F4 — Thresholds uncalibrated. CONFIRMED.

`kOmpMinFaces = kOmpMinCells = 4096`, compile-time, file-local (:28-37), no env
override. The "same order per-item work" premise is refuted by the bodies: one
`depthOfArea` lookup (refreshDepths) vs a full Riemann solve
(computeFluxes/fireFaces) — plausibly 20–50×. One threshold is simultaneously
too low for the light loops and far too high for the heavy ones. Sub-4096
models run 100% single-threaded regardless of THREADS; TwinOaks-v2 (5086
cells) barely clears it. Fix: per-loop thresholds + env override for sweeping.
Tier A. Half a day.

### F5 — FvGeometry AoS. CONFIRMED, bigger than the plan said.

`NetworkMeshData.hpp:77-171`: **sizeof ≈ 3416 B (~53 cache lines), 91% lookup
tables** (`i1_tbl` 2064 B + `h_tbl` 1032 B). Hot scalars sit at offsets
152–248 behind a 144-B cold `XSectParams`, with documented-cold `slope`/
`culvert_curve` wedged between them and the tables. ~9 geometries fit in a
32 KB L1. Access is a dependent double indirection
(`geom[cell_geom[uc]]`) at 25 hot sites. In-file corroboration
(`NetworkMeshData.hpp:163`): depthOfArea + area lookups = 87% of solver time
on a Δx=20 ft run. Fix: split hot scalars into an SoA (or a 64-B hot header),
keep tables behind a pointer. Tier A. 2–3 days.

### F6 — FP contract / no SIMD. CONFIRMED with a correction.

The plan's citation (`src/engine/CMakeLists.txt:227`) is inside an
OFF-by-default option block — the flag actually binds via **CMakePresets**
`CMAKE_CXX_FLAGS` on every platform preset (`-fno-fast-math -ffp-contract=off
-fno-math-errno`). So FMA is forbidden engine-wide by policy (bit-parity
rationale), even though `-mavx2 -mfma` is requested on x86. No `omp simd`,
`__restrict`, or vectorize pragmas anywhere in FV. Any SIMD/FMA work is
**Tier B by definition** and belongs behind the existing bit-exact-flags
build split (`build/darwin` FP flags memory). Not a quick win; note for Phase E.

### F7 — Superlinear scaling suspects. Two concrete mechanisms found.

1. **`rebuildActiveLists` :403-509 is O(total network) regardless of active
   set**: 4 O(nf) fills + all-cells seed + all-faces seed + up to 8 halo levels
   × O(nf) + O(nc) copy per level + final all-faces compaction ≈ **14·nf + 9·nc
   per call, serial**, every 8 substeps → amortized ≈ 1.75·nf/substep — the
   cost of a whole extra flux pass. A 1%-wet model pays the same as 100%-wet.
2. **`fireFaces` pays Ω(n_nodes) per base substep** (:3115-3128): `touched`
   assign + full scan over all node slots even when the due set is 3 faces —
   defeats LTS's entire premise on large networks.

Both are load-growth mechanisms (cost per substep grows with N while substep
count also grows with N via CFL), which is exactly the shape of the 500→2000
conduit 13.4×/4× superlinearity. Phase C times rebuild in isolation (the
[PERF-FV] `rebuild=` field now attributes it correctly post-F8-fix).
Fixes: incremental rebuild from the wet frontier (Tier A, 2-3 days);
per-substep touched-node *list* instead of flag-scan (Tier A, 1 day).

### F8 — Timer double-count. FIXED pre-review (commit `7fb60748`). Verified §7.

## 4. 1D DynamicWave (reference)

Persistent team confirmed at :1328 (one fork per routing step). Barriers per
Picard iteration: **9 unconditional** (10 from iteration 2, up to 13 with slot
mode + virtual junctions) — 3 explicit (:1780, :2134, :3538) + implicit
worksharing barriers. Serial structures phase is the `omp single` at :1371
(pumps/orifices/weirs + outfalls + AA skip flags; the plan's :3371 was the
callee `computeAASkipFlags`). This is the design FV/2D restructures should
copy; no further action.

## 5. Answers to the plan's open questions

1. *Which model / was CUDA selected?* — Still owed by the owner; note the
   `auto` path never tries cuda below 10k cells and probe failure falls back
   silently. Phase D step 0 captures the stderr backend line.
2. *Is fireExchange's serial order a numerics contract?* — **Yes** (§1 G2):
   shared-cell drains and the `node_drawn_` first-come-first-served budget are
   order-dependent by construction; EIS's own comments say so. G2's parallel
   rewrite is Tier A only in the group-by-cell form.
3. *Transport gap known/accepted?* — It is worse than a comparability caveat:
   nothing prevents selecting the Kokkos solver on a quality deck (§1
   functional gaps). Needs a factory gate regardless of this review's outcome.

## 6. Phase E preview — ranked fix list (static confidence; re-rank after C/D)

| Rank | Fix | Tier | Effort | Expected effect |
|---|---|---|---|---|
| 1 | G1+G6 device residency + dirty forcings + on-demand publish | A | 2–3 d | Removes ~88·nt B/advance PCIe + the fence serialization — the static #1 CUDA suspect |
| 2 | G3 single-scan compaction + batched control D2H | A | 3–4 d | 9→1 syncs/rebuild; GPU stops idling at decision points |
| 3 | G2 fuse bc+exchange, then group-parallel | A | 0.5 + 2–3 d | Removes 16 single-thread launches/cycle |
| 4 | G4 int8 sign + arm mirror + active-face publish | A | 1 d | Cuts hot-kernel gather bytes ~8× on sign path |
| 5 | F2 wait-policy gate extension | A | minutes | Measured first in Phase C; up to 500 µs/substep back |
| 6 | F1 FV persistent team (+ parallel censusDt/limitPositivity from F3) | A | 4–5 d | Only lever that makes LTS+threads compose |
| 7 | C1+C2 parallel rebuild + publish | A/B | 2–3 d | Lifts the 2.79×@10-core ceiling toward ~5× |
| 8 | F7 incremental rebuild + touched-list | A | 3–4 d | Kills the O(N) floor behind the superlinearity |
| 9 | C5 2D persistent team | B | 4–5 d | After C1/C2; DW precedent |
| 10 | C6 mesh renumbering | A | 3–4 d | 1.3–2× on real meshes (prior estimate) |
| — | G7 ilogb, C4 idiom unify, F4 per-loop thresholds, updateNodes fuse | A | hours each | Housekeeping batch |

Gates: every Tier A phase must hold `.out` sha256 on the Phase C decks and the
18-deck parity corpus (`tests/parity/run_corpus.sh`); Tier B phases get the
corpus-tolerance gate instead.

## 7. Phase C — CPU measurements (arm64 macOS, Apple Silicon; worktree @ bbd13775 + A2 counters)

All inputs/outputs under `tests/benchmarks/generated/perf_review_2026-09/`.
Caveat: desktop machine with other sessions live; wall clocks are best-of-run,
ratios are the finding, not the absolute seconds.

### 7.1 2D backend/thread matrix — `bench2d_79202.inp` (79,202 cells, 3600 advances)

| T | CPU marcher | Kokkos-OMP plugin |
|---|---|---|
| 1 | 19.45 s | 46.39 s |
| 2 | **11.72 s** | 23.55 s |
| 4 | 23.11 s | **16.35 s** |
| 8 | 84.38 s | 17.85 s |

- **G9 REFUTED at this size/machine.** The plugin is 2.4× slower than the
  marcher at T=1 (not 15–20×) and it *scales* — 2.8× to T=4 — while the
  native marcher **collapses past T=2** (T=8 is 4.3× slower than serial, all
  of it inside `2D-advance`: 82.2 s of the 84.1 s window). At any fixed T ≥ 4
  the plugin wins. The factory's "15–20×@25k" note is either size-specific,
  machine-specific (it was measured on Bellinge), or stale.
- The marcher's collapse is C5 (≈31.6 fork/joins per macro cycle) amplified by
  macOS libomp passive wait + efficiency cores — the same pathology F2
  documents for 1D. The prior "2.79× on 10 cores" scaling figure came from a
  different platform and does not transfer to macOS.
- Determinism: each backend is bit-identical across T=1..8 (cpu
  `0bd5cbc0…`, omp `f3e4bb9f…`) — the any-thread-count contract holds
  per backend.
- **NEW FINDING (N1): the omp plugin does NOT match the CPU marcher** on this
  deck — external inflow 0.079 vs 0.070 ML (+12%), outflow 0.765 vs 0.680 ML
  at continuity level. That is physical divergence, not ULP noise, despite the
  solver header's "bit-identical to the serial marcher" claim. The G7
  `std::log2`-vs-`std::ilogb` tier split is one candidate (it changes LTS
  tiering, hence firing cadence). Root cause of the coverage hole: the only
  omp-backend tests (`test_engine_2d_omp_default`/`_vfr`,
  `tests/unit/engine/CMakeLists.txt:429-464`) assert
  `PASS_REGULAR_EXPRESSION "using GPU backend: omp"` — **they gate that the
  plugin loads, not that it matches the CPU marcher numerically**. There is no
  cross-backend parity test anywhere. **Must be root-caused (and a parity gate
  added) before the plugin is trusted anywhere** — it now outranks every 2D
  perf item (a fast wrong answer is not a speedup).
- A2 counters at T=8: 47.0 launches/advance (static estimate 46 ✓);
  deep_copy traffic 34.2 GB / 3600 advances ≈ 9.5 MB/advance at 79k cells —
  matches the §1 G1 analytic (88·nt ≈ 7.0 MB + boundary + fills). On the OMP
  backend this is host-memcpy, cheap; on CUDA it is the PCIe bill.

### 7.2 1D FV thread scaling + wait policy

Decks: `Example1`, generated `reach_uniform_2000` (fv_default ≈ 8,000 cells),
5 configs each, `OMP_NUM_THREADS` ∈ {1,4,8}, then 8+`OMP_WAIT_POLICY=active`.

- **Thread scaling: NONE, anywhere.** reach_2000/fv_default: 24.70 / 25.88 /
  24.76 s at T=1/4/8. nolts: 15.25 / 19.85 / 15.07. Example1/fv_1cell gets
  *slower* at T=8 (0.90 → 1.47 s). F1's structural analysis explains it: under
  LTS only `fireFaces` ever forks (gated on the due subset ≥ 4096), and the
  non-LTS forks are light bodies whose fork cost exceeds their work at these
  sizes (F4).
- **F2 wait-policy delta: ≈ 0** on every row (t8active ≈ t8). The lever is
  real only once FV actually spins on barriers — i.e. after an F1 restructure.
  Refuted as a standalone quick win; keep as part of the F1 package.
- **Bit-identity across thread counts: holds on every (deck, config)** —
  `deterministic=true`, same `.out` sha at T=1/4/8. The Tier A gate is
  mechanical for FV threading work.
- **A1/F8 fix verified**: unattributed time is positive on every LTS row now
  (+4.8% Example1/fv_default, +6.2% reach2000/fv_default, +17.9% fv_1cell) —
  phases no longer sum past the step.
- **The profile re-ranks the 1D plan** (reach_2000/fv_default, T=1):
  `refreshdepths` **54.6%** + `cellupdate` 21.2% dominate; `census` is only
  6.2% and `rebuild` **0.4%**. n.invert = 60.2M `depthOfArea` calls. So:
  - **F5 (FvGeometry AoS / depthOfArea table gathers) is the #1 1D lever**,
    corroborating the in-code 87% note — not the fork/join structure.
  - The plan's F3 parallelize-first ranking (censusDt first) is demoted;
    refreshDepths/updateCells — already parallel-gated but useless without
    F5's memory-layout fix — are the hot path.
  - **F7's rebuild hypothesis is REFUTED as the superlinearity mechanism**
    (rebuild = 0.09 s of 24.25 s): the halo loop is cheap in practice at this
    size. The 500→2000 superlinearity remains unexplained — re-measure on an
    idle box before acting; the Ω(n_nodes) `fireFaces` floor (F7b) is still
    real for LTS on large networks (see TwinOaks).
- LTS vs no-LTS on the uniform reach: LTS **costs** 1.6× (24.70 vs 15.25 s) —
  on a uniformly-CFL-bound deck tiering is pure overhead (expected); the
  refreshdepths share rises from 30% to 55% under LTS (settle-path
  re-inversions).

### 7.3 TwinOaks-v2 (real network, 5086 cells, 15 h sim)

| Config | Wall |
|---|---|
| dynwave | 236.7 s (continuity error 76.3% — the deck is pathological under DW; treat as reference-with-asterisk) |
| fv_1cell | **≥ 1800 s (timeout)** |
| fv_default | **≥ 1800 s (timeout)** |
| fv_default_nolts | solo rerun in progress (killed at 269.7 s still running in the batch — earlier "completion" figures for nolts/routingstep in the batch output are kill artifacts, exit −15/−9, and must not be cited) |

FV is ≥ 7.6× slower than DW on this real network at default settings —
consistent with the standing FV/East-Boston gap. If the LTS-off solo run
finishes in O(DW) time, LTS's fixed per-base-substep floors (F7b) are the
blow-up mechanism on real networks and become the top FV work item alongside F5.

### 7.4 Phase C verdicts against the plan's success criteria

1. 15–20× omp-plugin slowdown: **refuted** at 79k cells on this machine (2.4×
   at T=1, crossover in the plugin's favor at T≥4) — but superseded by N1
   (divergent answers).
2. FV 1T→8T scaling: **flat to negative**; wait-policy delta **zero**.
3. F7 superlinearity: rebuild **exonerated by measurement**; cause still open.

### 7.5 Re-ranked recommendation (supersedes §6 where they differ)

1. **N1 root-cause** — omp plugin vs CPU marcher divergence (start with G7
   ilogb, then diff tier/active traces on a small deck). Blocks everything GPU.
2. Transport gate in the factory (§1 functional gap 1) — one-day fix, closes a
   silent-wrong-results hole.
3. **F5** FvGeometry hot/cold split + SoA for the depthOfArea path — the
   measured 55%+21% of FV step time.
4. G1+G3+G6 device residency + batched control sync (unchanged — CUDA-side,
   Phase D confirms magnitude).
5. C5-for-macOS: the 2D marcher's thread collapse is a shipping regression on
   Apple hardware (T=8 is 4.3× slower than serial) — either persistent-team
   restructure or clamp default 2D threads to ≤2 on Darwin until it lands.
6. F1 persistent team for FV — still right long-term; F2 rides along; but at
   current sizes it buys nothing until F5 removes the memory-bound floor.
7. G2/G4, C1/C2, F4, C4/G7-hygiene — as §6.

## 8. Phase D — CUDA runbook (Windows RTX 2000 Ada, preset `Windows-cuda`; user-run)

0. **Selection sanity**: run any 2D deck with `OPENSWMM_2D_BACKEND=cuda` and
   capture stderr — confirm the backend line names the cuda plugin, not a
   silent fallback (probe failure falls back to the CPU marcher without error).
1. **Counters first** (needs A2, this round): `set OPENSWMM_PERF=1`, run
   `bench2d_{19602,79202}.inp` + a ~250k variant from
   `plans/2d/perf_review_2026-08-22/make_bench2d.py`, backend cuda vs cpu
   (T=8). Record wall, the `[PERF-2D-KOKKOS]` line (launches/advance,
   fence_s, h2d/d2h bytes/advance), and `.out` vs cpu max |Δ| on head/volume
   (expect 1e-4..1e-6 per `2D_GPU_DEFAULTS.md`).
2. **Timeline**: `nsys profile` with the Kokkos nvtx connector
   (`KOKKOS_TOOLS_LIBS=<kp_nvtx_connector>`; do NOT set OPENSWMM_PERF's
   counters — they yield to the external tool automatically). Split wall into
   {kernel, memcpy, idle}; the `openswmm_2d_advance_push/march/publish`
   regions (A3) bracket the phases. Then `ncu` on `fireFaces`/`fireCells`:
   achieved occupancy, DRAM throughput, registers/thread (closure size G5).
3. **G1 isolation**: same decks with `COUPLING_SYNC 10` — if wall drops
   ~pro-rata with advance count, transfer+fence dominates (G1 confirmed).
4. **G8 crossover sweep**: 5k, 10k, 20k, 50k, 100k, 250k cells × {cpu T=8,
   cuda}; plot; set `OPENSWMM_2D_MIN_PARALLEL_CELLS_DEVICE` from the crossing.
5. Success criterion: a measured breakdown of GPU wall into
   {kernel, transfer, sync/idle, launch}. transfer+idle > 50% ⇒ G1–G3 are the
   implementation plan, in that order.
