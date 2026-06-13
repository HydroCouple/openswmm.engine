# Runtime Forcing — Phase 4 Parameter-Surface Audit Outcomes

**Started:** 2026-06-12
**Scope:** `docs/RUNTIME_FORCING_PHASE4_HANDOFF.md` §2 / `RUNTIME_FORCING_API_GAP_PLAN.md` §12.

Per the §2.1 protocol, each setter group gets **one** recorded outcome:
* **Sound** → documented runtime contract + mid-run test, **and** the legacy
  parity setter so both engines share the contract.
* **Unsound mid-run** → `CHECK_RUNNING`-style guard + pre-start-only contract.

Tests: refactored audits in `python/tests/engine/test_param_runtime.py`;
legacy parity in `python/tests/legacy/test_param_runtime.py`.

---

## Wave B1 — P6 time patterns, P4 street sweeping

### P6 — time-pattern factors → **SOUND (with a cache-refresh fix)**

**Refactored.** `swmm_pattern_set_factors` writes `ctx.patterns.factors`.
Per-step consumers split two ways:
* Groundwater-evap patterns read `ctx.patterns` **live** (`Groundwater.cpp`),
  so they already saw mid-run edits.
* DWF / external-inflow patterns read a **cached copy** that
  `InflowSolver::init` makes once at start (`Inflow.cpp`), so a mid-run edit
  was silently ignored — confirmed empirically (DWF inflow ratio 1.0 after a
  10× factor edit before the fix).

Phase 4's purpose is runtime calibration, so the resolution is to **make the
edit propagate**, not to guard it: added `InflowSolver::refreshPatterns(ctx)`
(re-copies factors into the cache), exposed `SWMMEngine::inflowSolver()`, and
`swmm_pattern_set_factors` now calls it. Contract: **a pattern factor edit
takes effect on the next step.**

**Legacy.** `Pattern[i].factor[k]` is looked up afresh each step
(`inflow.c`/`dwf`), so it is live with no cache. Added the parity setter:
`swmm_TIME_PATTERN` object type + `swmm_PatternProperty`
(`FACTOR`/`COUNT`/`TYPE`), `set/getPatternValue` in `swmm5.c` (factor index
via `subIndex`; factors non-negative; settable pre-start and running).

### P4 — street sweeping → **SOUND (no fix needed)**

**Refactored.** `swmm_landuse_set_sweep_interval` / `_removal` write the live
`ctx.landuses.sweep_interval` / `sweep_removal` vectors, read each step where
sweeping is evaluated (`SWMMEngine.cpp`). No cache; trivially sound.

**Legacy.** `Landuse[i].sweepInterval` / `sweepRemoval` are read per step in
the sweeping evaluation. Added the parity setter: `swmm_LANDUSE` object type +
`swmm_LanduseProperty` (`SWEEP_INTERVAL`/`SWEEP_REMOVAL`), `set/getLanduseValue`
in `swmm5.c` (interval ≥ 0; removal in [0, 1]; settable pre-start and running).

**Bindings:** refactored bindings (`Pattern.set_factors`,
`Landuse.sweep_interval/_removal`) already existed. Legacy gains
`SWMMPatternProperties` / `SWMMLandUseProperties` enums (+ `.pyi`); enum
coverage in `test_phase1_enum_coverage.py` (pattern 3 / landuse 2).

---

## Wave B2 — P2 buildup / washoff function coefficients → **SOUND (with a cache-refresh fix)**

**The accumulated buildup pool is preserved.** `swmm_buildup_set` /
`swmm_washoff_set` only change the *function* (type + coefficients); the
accumulated mass (`surface_quality_.buildup`, legacy
`Subcatch[].landFactor[].buildup[]`) is never reset or rescaled by the edit —
the new function only governs how buildup evolves from the next step. (Note: the
buildup integrator maps current mass → equivalent days → new mass each step, so
lowering the max-buildup coefficient below the current mass clamps it on the
next step, by construction.)

**Refactored fix (same class as P6).** The per-step path
(`SWMMEngine::stepSurfaceQuality`) reads `landuse_solver_.buildup_params` /
`washoff_params`, a cache derived from `ctx.buildup`/`ctx.washoff` at start, so
`swmm_buildup_set`/`swmm_washoff_set` (which write `ctx`) were ignored mid-run.
Extracted the start-up transfer into `SWMMEngine::refreshLanduseParams()`
(recomputing `max_days`); both setters now call it → edit takes effect next
step. Pool untouched.

**Legacy parity.** Buildup/washoff functions are read live each step
(`surfqual.c`), so they are runtime-settable. Extended `swmm_LanduseProperty`
with `BUILDUP_FUNC`/`COEFF1..3`/`NORMALIZER` and
`WASHOFF_FUNC`/`COEFF`/`EXPON`/`SWEEP_EFFIC`/`BMP_EFFIC` (pollutant index via
`subIndex`); `set/getLanduseValue` gained the `subIndex` arg. Buildup edits
recompute `TBuildup.maxDays` via `recomputeBuildupMaxDays`, mirroring
`landuse_readBuildup`. Efficiencies bounded to [0, 1]; coefficients ≥ 0.

**Bindings:** refactored (`quality.set_buildup`/`set_washoff`) already existed;
legacy `SWMMLandUseProperties` extended to 12 members (+ `.pyi`, enum coverage).

---

## Wave B3 — P1 infiltration parameters → **PRE-START-ONLY (already guarded)**

**Correction to the handoff premise.** The gap matrix (§12.1 P1) states the
refactored infiltration setters have "no running guard." They do: every
`swmm_subcatch_set_infil_horton` / `_green_ampt` / `_curve_number` begins with
`CHECK_GEOMETRY(ctx)`, which permits only the editable states
(`BUILDING`/`OPENED`) and returns `SWMM_ERR_LIFECYCLE` while running. Verified
empirically — calling a setter mid-run raises `LifecycleError`.

**Why pre-start-only is the right contract.** Each subcatchment's infiltration
*state* (Horton decay clock `tp`/`Fe`, Green-Ampt `F`/`Fu`/`Lu`/`T`, Curve-Number
`S`/`Se`) is initialized once at `start()` from the parameters and then evolves;
the params live *inside* the per-subcatchment state struct
(`RunoffSolver::horton_states_`/`grnampt_states_`/`curvenum_states_`), not in a
separately-read table. Mutating parameters mid-event has no single correct
meaning (reset the decay clock, or continue it on a shifted curve?), and the
engine authors guarded it accordingly. This is the §2.1 **"unsound mid-run →
guard + pre-start-only contract"** outcome — and the guard already exists, so no
engine change is made.

**No legacy parity setter.** Per the plan (§12.2.2), legacy parity is added only
for groups whose audit concluded mid-run mutation is *sound*. P1 is not, so the
legacy engine likewise keeps infiltration parameters as a pre-start (input)
concern; no `setSubcatchValue` infiltration cases are added.

**Tests** (`test_param_runtime.py::TestInfiltrationParams`): a pre-start Horton
edit takes effect and round-trips through the run; a mid-run setter call raises
`LifecycleError`. No new bindings.

---

## Wave B4 — P5 pollutant kinetics → **SOUND (kdecay/co/snow); init_conc pre-start-guarded**

**Refactored.** The kinetics setters are unguarded and write `ctx.pollutants.*`
directly. Consumption:
* `k_decay` is read live each step in quality routing
  (`QualityRouting.cpp:346,420`, `ctx.pollutants.k_decay[p]`).
* `co_pollut`/`co_frac` feed the per-step co-pollutant washoff
  (`Landuse::applyCoPollutant`); `snow_only` gates buildup.
* `init_conc` has **no per-step consumer** — it only seeds the conveyance
  network at `start()` (parsed in `QualityHandler`, written by `InpWriter`,
  never read in the step loop). A mid-run edit would silently no-op, so it is
  now guarded with `CHECK_GEOMETRY` (raises `LifecycleError` while running).

So kdecay/co-pollutant/snow-only are **sound mid-run** (live, no cache, no fix);
init_conc is **pre-start-only** (newly guarded).

**Legacy.** Mirror confirmed: `Pollut[p].kDecay` (`qualrout.c:430,603`),
`coPollut`/`coFraction` (`surfqual.c:469,473`), and `snowOnly`
(`surfqual.c:121`) are all read live; `initConcen` seeds state at start.
Extended `swmm_PollutProperty` with `KDECAY`/`CO_POLLUTANT`/`CO_FRACTION`/
`SNOW_ONLY`/`INIT_CONCEN`; `set/getPollutValue` handle them with per-case
bounds (the old blanket `value < 0` reject would have refused a `-1`
co-pollutant index). **Units:** the API takes `kDecay` in 1/day (INP units);
legacy stores 1/sec, so the setter divides by `SECperDAY` and the getter
multiplies — matching `landuse_readBuildup`/`inputrpt.c`. `INIT_CONCEN` returns
`ERR_API_IS_RUNNING` while the simulation is running, matching the refactored
guard.

**Bindings:** refactored `Pollutant.kdecay`/`init_conc`/`snow_only`/
`co_pollutant` already existed; legacy `SWMMPollutantProperties` extended 4→9
(+ `.pyi`, enum coverage). Tests: `TestKineticsRuntime` (engine),
`TestLegacyKinetics` (legacy).

---

## Wave B5 — P7/P8 external-inflow & DWF baselines/scale → **SOUND (with a cache-refresh fix); refactored direct setters added**

**Refactored (same cache class as P6).** `InflowSolver::init` caches the
ext/DWF definitions (`ext_inflows_`/`dwf_inflows_`) at start; the per-step
`computeAll` reads the cache, so editing `ctx.ext_inflows`/`ctx.dwf_inflows`
(via `swmm_ext_inflow_add`/`swmm_dwf_add`) was stale mid-run. Added direct
setters `swmm_ext_inflow_set_scale`/`_set_baseline` and `swmm_dwf_set_baseline`
(the gap-plan's suggested "direct `set_scale`"), and a
`refresh_inflows_if_running` helper that rebuilds the cache via
`InflowSolver::init(ctx)` (idempotent — touches no node state). The add/remove
paths now refresh too, so the documented "remove + re-add" edit also takes
effect mid-run. Bindings: `Inflows.set_external_scale/_baseline`,
`set_dwf_baseline` (+ `.pyi`, `_common.pxd`). Tests:
`TestInflowBaselineRuntime` (DWF baseline edit changes node inflow next step;
ext baseline/scale round-trip).

**Legacy parity — partially deferred (data-model gap, flagged).** The legacy
inflow model is per-node linked lists (`Node[j].extInflow` of `TExtInflow` by
constituent; `Node[j].dwfInflow` of `TDwfInflow`) with **no flat entry index**
to mirror the refactored `entry_idx` API. Runtime inflow *control* already
exists in legacy via `swmm_NODE_LATFLOW` (`apiExtInflow`, the Phase-1 lateral
inflow override), so mid-run inflow adjustment is achievable today; what is not
yet exposed is editing the persistent `[INFLOWS]`/`[DWF]` baseline/scale
definitions by a node-keyed setter. That is a larger, separate task (new
`setNodeValue` cases + linked-list traversal for the FLOW constituent) and is
deferred with this note rather than rushed on a core routing path.

---

## Wave B6 — P3 treatment expressions → **SOUND (with a cache-refresh fix); legacy parity functions added**

**Refactored (same cache class as P6/P2/P7).** The step loop
(`QualitySolver::applyTreatment`) evaluates the **compiled** expression cache
(`ctx.treatment.compiled` + per-node `has_treatment` flags), which
`initQuality()` builds once at start. `swmm_treatment_set`/`_clear` wrote only
the expression *string*, so a mid-run edit was silently inert. Added
`SWMMEngine::refreshTreatment(node, pollutant)` — recompiles just the edited
cell (Shunting-yard parse, microseconds; no thread-safety concern since API
calls sit between steps) and recomputes the node's `has_treatment` flag. Both
setters call it: **a treatment edit/replace/clear takes effect on the next
step**. A failed parse is rejected (`SWMM_ERR_BADPARAM` → `BadParamError`) and
the previous expression is restored, so a bad edit can't silently disable
treatment. The start-up cyclic co-treatment check (Gap #85) is not re-run for
runtime edits — introducing an R_POLLUT cycle mid-run is the caller's
responsibility (evaluation degrades to the legacy-compatible per-step order,
it does not hang).

**Legacy parity.** Treatment is an *expression*, not a double, so it cannot
ride `setNodeValue`; added dedicated exported functions instead:
`swmm_setTreatment(node, pollutant, "R = ..."/"C = ...")` and
`swmm_clearTreatment(node, pollutant)` (`swmm5.c`). The setter re-uses the
`[TREATMENT]` input parser (`treatmnt_readExpression`) for exact input-file
semantics, freeing any prior `MathExpr` first so runtime replaces don't leak;
clear frees the equation (`treatmnt_treat` applies zero removal for NULL
equations). Callable pre-start and running; evaluated from the next routing
step.

**Bindings:** refactored `quality.set/get/clear_treatment` already existed;
legacy gains `Solver.set_treatment`/`clear_treatment` (pxd + pyx + `.pyi`).
No new enums (dedicated functions), so enum coverage is unchanged. Tests:
`TestTreatmentRuntime` (engine: mid-run "R = 0.95" cuts the loaded node's
quality >2×, clear recovers, round-trip, bad-expression rejection preserves
the previous expression) and `TestLegacyTreatment` (legacy: same effect test
on a DWF-quality fixture, name resolution, garbage/bad-index rejection).

---

## Wave B7 — P11 LID layer parameters → **SPLIT: drain SOUND mid-run; surface/soil/storage PRE-START-ONLY (stubs made real)**

**The premise was worse than a cache:** all four refactored setters
(`swmm_lid_set_surface/_soil/_storage/_drain`) were **silent no-op stubs** —
they returned `SWMM_OK` without writing anything ("TODO: implement when LID
SoA store is expanded"), the worst kind of middle ground. The Python bindings
(`infrastructure.lids.set_*`) already called them, so callers were being lied
to. All four now actually write `ctx.lid_controls.*` (the same arrays the
`[LID_CONTROLS]` parser fills, raw input-file units).

**Per-layer outcome.** `LIDSolver::init()` (start) builds per-unit SoA copies
of the layer params **and seeds per-unit state from them** (soil moisture from
`wp + initSat·(porosity − wp)`, storage depth from `initSat·thick`), and the
water-balance initial volumes. So:
* **Surface / soil / storage → pre-start-only.** Mid-run mutation has no
  single correct meaning (state seeded from the params; θ > porosity etc.);
  guarded with the BUILDING/OPENED state check (`LifecycleError` while
  running), the same class as P1 infiltration. Basic physical bounds enforced
  (porosity/void ∈ (0,1], fc < porosity, wp < fc, non-negatives).
* **Drain (coeff/expon/offset) → sound mid-run.** Pure flux coefficients
  evaluated each step against current head — the classic RTC/calibration knob
  (cistern/rain-barrel drain control). The step loop reads the LID solver's
  per-unit copies, so `swmm_lid_set_drain` calls the new
  `SWMMEngine::refreshLIDDrainParams()` (re-copies the drain columns for every
  unit; touches no state; no-op before start).

**Legacy parity (drain only, per protocol).** Legacy `TLidProc.drain.*` is
read live each step (`lidproc.c`), so the parity setter is live with no cache:
`lid_setDrainParams` in `lid.c` (mirrors `readDrainData` units — coeff in
in/hr / mm/hr raw, offset ÷ `UCF(RAINDEPTH)`) wrapped by the exported
`swmm_setLidDrain`; binding `Solver.set_lid_drain` (accepts index or name via
`swmm_LID`). Surface/soil/storage stay an input-file concern in legacy,
matching the refactored guard.

**Tests.** Fixture: site_drainage + 10 rain barrels on S1 with a closed drain
(coeff 0), so the barrels fill in the storm and hold; by 3 h outlet J1 has
receded to ~0.005 cfs. Refactored (`TestLidParamsRuntime`): paired
deterministic runs (WET_STEP = ROUTING_STEP, VARIABLE_STEP 0) are
bit-identical until the mid-run drain edit and divergent after (J1 jumps to
~21 cfs); layer guards raise `LifecycleError` mid-run; pre-start edits
accepted with bad values rejected. Legacy (`TestLegacyLidDrain`): the same
mid-run edit raises J1 from ~0.004 to ~1.0 cfs next steps; bad index/negative
coeff rejected.

**Flagged follow-up (pre-existing, not addressed here):** the refactored LID
module consumes `ctx.lid_controls` values raw with no US⇄SI/in⇄ft conversion
anywhere in `LID.cpp` (legacy converts at parse/use, e.g. `coeff/UCF(RAINFALL)`,
`offset/UCF(RAINDEPTH)`), and the post-edit drain magnitudes differ between
engines (~21 cfs vs ~1 cfs for the same fixture). The mid-run *contract* (edit
applies next step) is verified for both engines independently of that gap, but
LID unit alignment with legacy deserves its own audit.
