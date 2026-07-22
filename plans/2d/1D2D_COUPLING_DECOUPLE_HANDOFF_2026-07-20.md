# 1D/2D Coupling Decouple — Handoff & Open Conservation Bug (2026-07-20)

**Status:** ~~WIP, **uncommitted**, **NOT conservative** — do not ship as-is.~~
**RESOLVED 2026-07-21 — see Addendum below. P1 and the "solver spike" are root-caused and
fixed (still uncommitted); repro now 0.000% on all three variants; ctest 101/102 (the one
failure is a 1D-DW test-model artifact, not coupling).**
**Author of this handoff:** prior agent session. Everything below is measured, not assumed.
**Engine HEAD when written:** `b2a4a5e6` (working tree dirty; see "What changed").

---

## ADDENDUM 2026-07-21 — Root causes found, fixed, verified

Three independent mechanisms, none of them the ones §3 suspected:

**(P1) The −13% leak = the deviation's ε-apparatus was never armed — a setup-ORDER bug.**
`SurfaceRouter2D::initialize` called `solver_->initialize()` (which sizes the augmented
state `nc_` from `state.coupling_series`) BEFORE publishing `state_.coupling_series` →
`nc_ == 0` on the held path. Consequences: no `ydot[nt+k]` slots existed
(`ncap = min(ncp, nc_) = 0`), the deviation was still scattered into the cells every RHS
eval, `last_coupling_exchange()` was an empty vector, and the reconciliation loop iterated
ZERO times. `eps ≈ 0.0000` in §3's measurements was **structural, not evidence of
conservation** — the accumulator it printed never existed. The deviation's BDF-quadrature
integral (exactly the leak the slots were designed to capture, per the comment at
`CvodeSurfaceSolver.cpp` nc_-sizing block) landed in the cells uncorrected. On budget-capped
flat windows the sampled series is front-loaded (q → 0 by window end), so
`dev(t_end) = +V/dt` is a pure end-of-window source; CVODE takes ~one BDF2 step there and
books `h·β₀·dev` with β₀ = 2/3 — the domain retains exactly ~⅔ of each booked drain,
telescoping into the next window's budget (the observed `drain_{n+1} = RESID_n` pattern).
**Fix:** publish `coupling_series_` on the state BEFORE `solver_->initialize()` (same rule
the live path already documented). Do NOT delete the reconciliation block — §3's "it's a
no-op, consider deleting" was observing the disarmed state.

**(P2a) The "+432 m³ 2D-solver spike" is NOT a solver/mesh blow-up — it is a CVODE
clock desync from the quiescence skip.** Quiescent windows skip `solver_->advance()` but
still run `sim_time_ += dt`. CVode always integrates from its INTERNAL time, so the next
live window integrated the freshly-held forcing over the whole accumulated gap:
0.4 m³/s held rain × 1080 s of lag = 432.001 m³, exactly the NO_INTERP spike (verified
with per-advance instrumentation: `tin=120.5` on a `span=[1180.5,1200.5]` advance).

**(P2b) Same family: the failed-window path resyncs off by one window.** On failure,
`solver_->reinitialize(sim_time_)` runs BEFORE `sim_time_ += dt`, pinning the CVODE clock
at the failed window's START while the router moves to its END — every failed window adds
−dt of clock lag (this, not booking, flipped the tight-tolerance A/B to +13.9%; per-window
`DEFECT = src_rate × lag` exactly). On Bellinge, 770k h=hmin failures ⇒ this compounds;
the −252% number should be re-measured with the fixes before drawing any mesh conclusions.
**Fix for both:** clock-resync guard at the top of `CvodeSurfaceSolver::advance()` — if
`CVodeGetCurrentTime != t_current`, `CVodeReInit(t_current, y_)` keeping the CURRENT y
(no head-reseed: a head reseed would zero negative-volume debt and CREATE water) and
invalidate preconditioner caches.

**Verification (all with `OPENSWMM_2D_DEBUG_COUPLE=1` + new `OPENSWMM_2D_DEBUG_SINK=1`):**
repro default −13.107% → **0.000%**; NO_INTERP −1055.078% → **0.000%**; tight-tolerance
(REL 1e-6/ABS 1e-8) +13.878% → **0.000%**. 1D flow-routing continuity 0.633%. Full suite
101/102 (`build/darwin-tests-release`); the two §3 unit tests' coupling assertions
(1D-received == 2D-given <1%, no overdraw, 2D continuity) now PASS — the remaining
assertion failure is 1D DW continuity −17.9% on the deliberately ill-conditioned test model
(pond-capable coupled junction, exchange area ≈14× pipe, engine-warned, ~12.5%
non-converging steps): a test-model artifact, not a coupling-ledger error.

**Also root-caused (not bugs):** the repro's "rain missing mid-hyetograph" is legacy-parity
gage step semantics — a 1-min INTENSITY gage with sparse series entries delivers rain only
for [entry, entry+interval), i.e. [0,60) and [1200,1260) ≈ 48 m³ intended, 40.2 delivered
(window-end hold clips one tail; ledger-consistent, contributes nothing to the error).

**Implication for §0:** with the fixes, BOTH the mean-only path and the deviation path are
conservative. But the deviation remains numerically hostile (kinks every routing step ⇒
~2× or worse CVODE step counts; its quadrature residual on this repro was ~18% of the
exchange, corrected post-hoc through the ε-queue one window late). That strengthens
option (A)-minus: keep per-step accumulation + TIME-window gating + mean-rate injection,
and make the deviation/ε apparatus opt-in or delete it. Option (B) live-RHS remains the
by-construction endgame if its preconditioner/speed problem is solved.

New instrumentation left in (env-gated, mirrors the §3 convention):
`OPENSWMM_2D_DEBUG_SINK=1` → per-advance `[sink]` line in `CvodeSurfaceSolver::advance`
(internal clock, span, Σcells/Σslots deltas, applied source rate, invariant DEFECT).
Files changed by this addendum's fixes: `SurfaceRouter2D.cpp` (wiring order),
`CvodeSurfaceSolver.cpp` (clock guard + [sink] debug). ArkodeSurfaceSolver likely has the
same desync exposure (not audited); the Kokkos CVODE backend sizes nc_ the same way — check
before enabling either with decoupled coupling.

---

---

## 0. Read this first — framing from the human

The human's words: *"I feel like the 2D model has grown too complicated and unwieldy."*

Take that seriously. The coupling path has accreted several overlapping mechanisms that now
interact in ways that are hard to reason about (held-flux vs. live-RHS vs. per-step-accumulate,
a per-cell `coupling_flux` field, a per-node `coupling_volume`/`coupling_queue` delivery system,
a per-point augmented CVODE accumulator, a provisional-drawdown budget, an interpolated
deviation series, plus a stability guard that halves/doubles the window). This session ADDED to
that pile trying to decouple the timesteps, and hit a conservation bug I could not close.

**Before writing more code, seriously evaluate consolidation.** Two viable framings:

- **(A) Simplify to ONE conservative mechanism.** The pre-session held path (`computeCouplingExchange`,
  single evaluation per window against the frozen 2D state, same capped Q booked to both sides)
  WAS conservative. Its weaknesses were (i) step-count gating collapsing with the 1D variable step,
  and (ii) window-boundary sampling of a transient. Both are fixable WITHOUT the per-step
  accumulate machinery: keep single-eval-per-window but (i) gate on a TIME window (already done,
  see Phase 2 below — that part is clean) and (ii) evaluate against a short trailing average of the
  1D head rather than the instantaneous value. This deletes a lot of the new complexity.

- **(B) Make the exchange a first-class CVODE quantity.** The live-RHS path
  (`computeNodeCouplingQ` inside `rhs_fn`, `ydot[nt+k]=Q`) is conservative BY CONSTRUCTION — the
  same Q drives the cell derivative and the ∫Q dt accumulator, so booked ≡ removed regardless of
  quadrature or solver behavior. Its only problem per the code comments is SPEED (stiff orifice,
  preconditioner doesn't capture its Jacobian). If the mesh stiffness is fixed (see §5), the live
  path may simply be fast enough, and the entire held/accumulate/interpolate apparatus can be
  deleted. **This is my recommended direction if a coupling-aware preconditioner or a good mesh
  makes it viable.**

Do not just keep patching the per-step-accumulate path. It is the source of the open bug and it
is the "unwieldy" the human is reacting to.

---

## 1. The goal that was requested

1. Decouple the 1D and 2D timesteps so each proceeds at its own tempo; accumulate the exchange
   fluxes and apply them, **interpolating the temporally misaligned fluxes** (human's words).
2. Map legacy `COUPLING_INTERVAL` to a physical time window (retire step-count gating).
3. (GUI, separate) Fix depth-profile rendering showing water climbing adverse slopes.
4. Regenerate the Bellinge 2D mesh with the fixed meshing pipeline (needs the GUI; NOT done).

Full plan: `~/.claude/plans/it-is-gone-the-humming-hanrahan.md` (may not be readable by you; the
essentials are reproduced here).

---

## 2. What changed (all uncommitted, engine repo)

`git diff --stat` of the files THIS session touched (other dirty files — `SWMMEngine.hpp`,
`NodeData.hpp` doc comment, `python/tests/...`, various `plans/2d/*.md`, `docs/2D_MODEL_AND_COUPLING_REVIEW.md`
— predate this session; do not attribute them here):

```
src/engine/2d/SurfaceRouter2D.cpp           | 392 +++--   core: per-step accumulate, window fire, ledger, reconcile
src/engine/2d/SurfaceRouter2D.hpp           |  39 +-     accumulators, series, resetWindowAccumulators()
src/engine/2d/coupling/NodeCoupling.cpp     | 255 ++--    computeCouplingExchangeStep, accumulateOutfallDischargeStep,
                                                          injectAccumulatedExchange, budgetDraw/Credit, provisional drawdown
src/engine/2d/coupling/NodeCoupling.hpp     | 139 +-     CouplingForcingSeries struct, new fn decls, provisional_vol arg
src/engine/2d/data/SolverOptions2D.hpp      |  10 +-     coupling_interval→window doc
src/engine/2d/data/SurfaceStateData.hpp     |  11 +      coupling_series pointer + fwd decl
src/engine/2d/solver/CvodeSurfaceSolver.cpp |  74 +-     RHS deviation term + augmented dev accumulator, nc_ sizing
src/engine/core/SWMMEngine.cpp              |  69 +-     assembleLateralInflows consumes per-step coupling_volume
tests/unit/engine/CMakeLists.txt            |   5 +      registers new test
```

Plus **new file** `tests/unit/engine/test_2d_decoupled_stepping.cpp` (3 cases; 1 passes,
2 fail on the conservation bug — see §3).

### 2a. Architecture as it stands now (the thing to simplify)

Per routing step, in `SurfaceRouter2D::advancePostRouting`:
- `computeCouplingExchangeStep` evaluates the junction orifice against the FROZEN 2D state with a
  **provisional-drawdown** head (driving head re-derived from the per-cell withdrawal budget so Q
  self-limits as the window's committed drain grows — this made the window total independent of the
  number of routing steps in it). Books `Q·dt` to `nodes.coupling_volume` (consumed next step by
  `assembleLateralInflows`) AND accumulates the SI volume into `window_exchange_accum_[k]`.
- `accumulateOutfallDischargeStep` does the same for outfalls into `window_outfall_accum_[k]`.
- One sample row `(t, q_k)` is appended to `coupling_series_` (net 2D-source rate per point).

When the window fires (`fireAdvanceWindow`):
- The MEAN rate `V_accum/window_dt` is injected into the per-cell held `state_.coupling_flux` via
  `injectAccumulatedExchange` → `scatterCouplingFlux` (frozen upwind weights).
- The sampled series is finalized into a zero-mean **deviation** `dev_k(t)=scale·lerp(t)−mean`,
  scaled by the TRAPEZOID integral so ∫dev=0; the CVODE RHS adds it via `scatterCouplingToYdot`
  and integrates its exact value into augmented state `ydot[nt+k]=dev`.
- After advance, a **reconciliation** block folds the deviation's CVODE integral ε_k back into the
  booked volumes. `accumulateMassBalance` books the (corrected) accumulators to the 2D ledger.
- `resetWindowAccumulators()` re-seeds the budget from `state_.volume` and clears everything.

Failed windows: re-queue `-window_exchange_accum_` to `nodes.coupling_queue` (give the 1D back what
the frozen 2D never moved). Quiescent windows: no exchange (guaranteed zero accumulators).

**This is a lot of moving parts for one physical coupling. That is the core problem.**

---

## 3. The open bug — a ~13% coupling-booking leak (NOT the deviation)

The 2D ledger books MORE drain than the 2D domain actually loses. On Bellinge this compounds to a
catastrophic **−252% 2D continuity error** (drain 233,251 m³ booked vs only 93,867 m³ ever entering
the surface). On a clean small model it is a steady **−13%**.

### Reproducer (fast, CLI, gage-driven — no API forcing needed)

`~/Downloads/7_SWMM/overdraw_repro/repro.inp` — 3 shallow junctions (MaxDepth 0.3) under a 6-vertex
/ 4-triangle 2D patch (bed z=1.0, above the crowns so it drains immediately), rain gage 1800 mm/hr
for 20 min then off, `COUPLING_INTERVAL 5` (→ 20 s window), `RAINFALL_MODE SYSTEM`. Run:

```
cd ~/Downloads/7_SWMM/overdraw_repro
~/Documents/Projects/cbuahin_github/openswmm.engine/build/darwin/src/cli/openswmm repro.inp r.rpt r.out
grep -A14 "2D Surface Routing Continuity" r.rpt   # → Continuity Error ~ -13%
```

CLI binary is `build/darwin/src/cli/openswmm` (build dir `build/darwin`, install prefix
`install/Darwin` — GUI consumes the install, so `cmake --install build/darwin` + re-codesign
`install/Darwin/bin/libomp.dylib` after any engine change the GUI must see).

### Debug instrumentation (env-gated, already in the code — LEAVE or remove when done)

- `OPENSWMM_2D_DEBUG_COUPLE=1` → per-window stderr line in `fireAdvanceWindow`:
  `[couple] t=.. dVol=.. rain=.. drain=.. spill=.. outf=.. eps=.. RESID=..`
  where `RESID = dVol − (rain − net_booked_out)`. **RESID>0 ⇒ the 2D removed LESS than booked.**
- `OPENSWMM_2D_NO_INTERP=1` → disables the deviation (mean-rate only).
- The `[couple]` block and `eps_sum` read live in `SurfaceRouter2D::fireAdvanceWindow` right before
  `sim_time_ += dt`. Grep `OPENSWMM_2D_DEBUG_COUPLE`.

### What the measurements PROVE (do not re-litigate these — evidence attached)

1. **The interpolation deviation is NOT the leak.** With `OPENSWMM_2D_DEBUG_COUPLE=1`, `eps≈0.0000`
   on every window — ∫dev dt is conservative under CVODE. The reconciliation block I added corrects
   a leak that isn't there (it's a no-op; consider deleting it). Hypothesis "trapezoid≠CVODE
   quadrature leaks volume" was **wrong**.
2. **The provisional drawdown is NOT the leak.** `OPENSWMM_2D_NO_DRAWDOWN` A/B (gate since removed)
   gave −13.1% vs −12.2% — no material difference.
3. **Mean-rate-only is per-window conservative WHEN STABLE.** With `NO_INTERP=1`, most windows show
   `RESID≈0`. BUT one window (`t=1196.5` in the repro) shows `dVol=+432, rain=8, drain=0` — the 2D
   solver **created 432 m³ from nothing** with zero coupling. That is a **2D-solver numerical spike**,
   independent of coupling. It is why `NO_INTERP` reports −1055%: not a booking bug, a solver blow-up.
   The deviation happens to smooth the forcing enough to avoid the spike, which is why default
   (deviation ON) is "only" −13%.
4. **So there are TWO independent problems:**
   - **(P1)** A ~13% coupling-booking leak: the constant mean-rate sink removes LESS than its exact
     integral V_accum. **This is the mystery.** A constant sink integrated by CVODE-BDF should
     remove exactly V_accum, yet at `t=36.5` the ledger books 4.951 m³ drain and the 2D volume only
     drops by ~4.223 m³. I could not explain this. Leading unproven suspects:
       - `scatterCouplingFlux` uses FROZEN upwind weights chosen at injection; cells it targets may
         dry out / go negative mid-window while sibling stencil cells keep water. Total *should*
         still be V_accum (weights sum to 1), but verify the sink isn't silently clamped when a
         reconstructed depth hits the dry floor, or when active-set/VFR interacts.
       - Check whether `t_reached < t_target` on "successful" windows (partial advance booked as
         full). Add `t_reached` to the `[couple]` print.
       - Instrument the ACTUAL ∫(area·coupling_flux)dt the solver applied vs V_accum directly (add a
         second augmented accumulator that integrates the MEAN sink, not just the deviation, and
         compare to V_accum). If they differ, the constant-sink-is-exact assumption is false and you've
         found it.
   - **(P2)** 2D-solver volume spikes under stiff forcing (the 432 m³) and the 770k `h=hmin`
     CVODE failures on Bellinge. **Largely the MESH** (see §5).

### Failing tests (expected — they guard the unfinished invariant)

`tests/unit/engine/test_2d_decoupled_stepping.cpp`:
- `IntervalMapsToTimeWindow` — FAILS: 2D storage overdrawn −2.3 m³/55, 1D continuity −54% under
  API-forced rainfall (gage-driven repro is cleaner, which is itself a clue — the API forcing path
  may double-apply or mis-time).
- `ExplicitLargeWindow` — FAILS same way.
- `FailedWindowsRedeliver` — PASSES (net 1D receipt ≈ 0 when every window fails; the redelivery
  queue works).

Run: `ctest --test-dir build/darwin-tests-release -R decoupled --output-on-failure`
(build dir with tests: `build/darwin-tests-release`, 102 tests, all others green).

---

## 4. What IS solid and worth keeping (verified)

- **Phase 2: `COUPLING_INTERVAL` → time-window mapping.** In `SurfaceRouter2D::initialize` window
  resolution block: AUTO(−1) with `coupling_interval>1` now resolves `window = interval ×
  ROUTING_STEP`; step-count gating deleted from `advancePostRouting`. Clean, self-contained,
  behavior-preserving for healthy models. Keep this regardless of what happens to the rest.
- **Per-step 1D booking** gives good 1D continuity (0.000 on the gage repro). The 1D SIDE of the
  decoupling is fine; it's the 2D-side booking (P1) that leaks.
- **GUI rendering fixes from earlier this session are COMMITTED** on the GUI repo branch
  `swmm6_gui` (`d1b6421` NN-interp Triangle `-N` crash + `aba21bd` mesh DTM-CRS/mesh-CRS unit
  stall). Unrelated to this coupling work; do not revert.

---

## 5. The confound you cannot ignore: the mesh

The Bellinge mesh (`~/Downloads/7_SWMM/BellingeSWMM_v021_nopervious.2dm`, 5369 v / 10664 t / 1020
vertex-coupled nodes) was generated BEFORE this session's meshing fixes, with a **0.14 mm boundary
buffer** (a DTM-CRS/mesh-CRS unit bug, since fixed in the GUI). That produced sliver cells along
boundaries — a textbook cause of the `h=hmin` CVODE grinding (P2). **Baselines:**

| | old engine (pre-session) | new engine (this session, decoupled) |
|---|---|---|
| wall time | 51 min | 15.8 h (partly a duplicate-process artifact; distrust) |
| CVODE `h=hmin` failures | 150,039 | 769,994 |
| 1D flow-routing continuity | −19.3% | 16.8% |
| 2D continuity | n/a | **−252%** (the P1 leak at scale) |

**Regenerate the mesh in the GUI (fixed pipeline: ~12 m boundary buffer, working Poisson filter,
working natural-neighbour elevation) and re-baseline BEFORE trusting any coupling measurement on
Bellinge.** This needs a human GUI session — it is blocked on the human, and it is the single
highest-leverage action. Debugging coupling on a sliver-ridden mesh conflates P1 and P2.

Baseline artifacts and scripts: `~/Downloads/7_SWMM/decoupling_baseline/` (baseline_asis.* = old
engine; newengine_decoupled.* = this session; control/ = gage-driven CLI A/B at windows 6 vs 30).

---

## 6. Concrete next steps, in priority order

1. **Decide the architecture (see §0).** Recommend evaluating whether the live-RHS path (option B)
   is viable on a REGENERATED mesh before investing more in the per-step-accumulate path. If yes,
   delete the held/accumulate/interpolate/deviation/reconcile apparatus entirely — it is the
   "unwieldy."
2. **If keeping per-step-accumulate: close P1 first.** Add the mean-sink augmented accumulator
   (integrate ∫area·coupling_flux dt per point in the RHS, compare to V_accum). This will either
   confirm the constant sink removes V_accum (⇒ the leak is elsewhere — measurement/rain path) or
   expose exactly where it doesn't. This is the decisive experiment I ran out of session to do.
3. **Regenerate the mesh** (human) and re-run the baseline. Separates P1 from P2.
4. **Then** the two failing tests should be revisited — several assertions may be measuring P1+P2
   together and need splitting.
5. Profile-rendering fix (GUI, Phase 3) is untouched and independent — a separate work item, do not
   entangle it with coupling.

## 7. If you decide to revert instead

The pre-session held path was conservative. To get back there: `git checkout` the 9 files in §2 and
delete `test_2d_decoupled_stepping.cpp` + its CMake registration. Keep ONLY the Phase-2 window
resolution change if you want the interval→time mapping (it's a ~10-line island in
`SurfaceRouter2D::initialize` + the `advancePostRouting` gating simplification). The GUI commits are
separate and stay.

## 8. Gotchas (all verified this session)

- Engine `install/Darwin` was stale from Jun 12; rebuild `build/darwin` → `cmake --install
  build/darwin` → `codesign --force --sign - install/Darwin/bin/libomp.dylib` or the GUI SIGKILLs.
- Two test build dirs: `build/darwin-tests` (Debug, 100 tests) and `build/darwin-tests-release`
  (Release, 102). Use the release one for coupling runs (Debug is slow).
- CLI has no rainfall-forcing API; drive 2D wetting via a `[RAINGAGES]` + `[TIMESERIES]` +
  `RAINFALL_MODE SYSTEM` (as the repro does), NOT `swmm_2d_force_rainfall_uniform` (that's C-API only).
- `swmm_2d_get_mass_balance` arg order: `(eng, init_storage, final_storage, rainfall_in,
  coupling_1d_to_2d_in, coupling_2d_to_1d_out, outfall_in, outfall_out, boundary_in, boundary_out,
  evap_out)` — easy to misorder (I did).
- Do not `git push`; commit locally only when the human asks. No Claude attribution lines in commit
  messages (repo convention).
