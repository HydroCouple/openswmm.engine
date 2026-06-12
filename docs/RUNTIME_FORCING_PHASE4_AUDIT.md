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
