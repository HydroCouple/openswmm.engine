# Subcatchment PET Prescription — Implementation Plan

**Date:** 2026-06-10
**Status:** IMPLEMENTED (2026-06-10) — pending local rebuild + test run
**Scope:** Legacy engine + refactored engine + Python bindings (both APIs)

---

## 1. Objective

Allow callers to prescribe a potential evapotranspiration (PET) **rate** per
subcatchment at runtime, in **both** the legacy and refactored APIs. The
prescribed rate overrides the default climate-derived evaporation rate
(`Evap.rate` / `ClimateState.evap_rate`) for that subcatchment and is consumed
by the normal evaporation pathways so that mass balance is tracked correctly
(actual evaporation capped to available water; losses flow into the existing
runoff continuity totals).

### Design decisions (confirmed 2026-06-10)

| Decision | Choice |
|---|---|
| Semantics | Prescribe a PET **rate** fed to the solvers (not a post-hoc loss-volume override) |
| Input path | Runtime API setter, mirroring the `apiRainfall` / forcing-channel patterns |
| PET scope | **All** subcatchment evap consumers: surface/depression storage, LID units, groundwater upper-zone evap |
| Units | User units — in/day (US) or mm/day (SI), converted via `UCF(EVAPRATE)` / `ucf::EVAPRATE` |
| Precedence | A prescribed rate is used **as-is**: it bypasses `DRY_ONLY` suppression and monthly `Adjust.evap[]` factors. Adjustment logic is **deferred to the caller** via the runtime getter/setter pair: read the current climate-derived rate (new read-only getter, tasks L9/R8), apply any caller-side factors, set the prescribed rate |
| Out of scope | Conveyance evap (`link.c:1362`), storage-node evap (`node.c:1037`), INP-file/timeseries configuration |

---

## 2. Current State & Gap Analysis

### 2.1 Legacy engine

* `Evap.rate` is a **global scalar** (ft/sec) computed in
  `src/legacy/engine/climate.c` (`climate_setState`, lines ~1229–1255,
  including `Adjust.evap[mon-1]`).
* Subcatchment consumers of `Evap.rate`:
  * `src/legacy/engine/subcatch.c:662–663` — surface/depression-storage evap
    in `subcatch_getRunoff()` (honors `Evap.dryOnly`).
  * `src/legacy/engine/gwater.c:523` — `MaxEvap = Evap.rate * FracPerv` for
    upper-zone groundwater evap.
  * `src/legacy/engine/lid.c:1644` — `EvapRate = Evap.rate` for LID units.
* Mass balance is automatic: evap volumes accumulate through `Vevap`/`Vpevap`
  in `subcatch_getRunoff()` → `massbal_updateRunoffTotals()` (`massbal.c`,
  `RunoffTotals.evap`).
* **Gap:** there is no per-subcatchment evap setter. `swmm_SUBCATCH_EVAP`
  (`include/openswmm/legacy/engine/openswmm_solver.h:134`) is **get-only**
  (`swmm5.c:1828` returns `subcatch->evapLoss * UCF(EVAPRATE)`).
* **Pattern to mirror:** `swmm_SUBCATCH_API_RAINFALL` →
  `Subcatch[].apiRainfall`, settable while the simulation is running
  (`swmm5.c` `setSubcatchValue`, lines ~1425–1432), consumed in
  `subcatch.c:757`.

### 2.2 Refactored engine

* `ClimateState.evap_rate` (`src/engine/hydrology/Climate.hpp`) is a scalar
  broadcast to three solvers from `src/engine/core/SWMMEngine.cpp`:
  * `runoff_.execute(ctx_, dt_runoff, ctx_.climate_state.evap_rate, ...)` — line 1062
  * `lid_.execute(ctx_, dt_runoff, 0.0, ctx_.climate_state.evap_rate)` — line 1129
  * `groundwater_.execute(ctx_, dt_runoff, ctx_.climate_state.evap_rate, ...)` — line 1841
* `RunoffSolver::execute` already expands the scalar into an internal
  per-subcatchment vector `evap_rate_[ui]` applying `DRY_ONLY`
  (`src/engine/hydrology/Runoff.cpp:259–264`) — a natural injection point.
* Mass balance is automatic: `Runoff.cpp:539` writes
  `ctx.subcatches.evap_loss[ui]` and accumulates `stat_evap_vol[ui] += Vevap`.
* **Defect found during exploration:** the existing forcing channel
  `swmm_forcing_subcatch_evap` (`src/engine/core/openswmm_forcing_impl.cpp:147–160`)
  is **ineffective**. `applyForcings()` writes `ctx_.subcatches.evap_loss[ui]`
  (`SWMMEngine.cpp:3239–3243`) *before* `stepRunoff()` (call order at
  `SWMMEngine.cpp:718` → `723`), and `Runoff.cpp:539` then overwrites
  `evap_loss[ui]` with the computed value. The forced value never affects
  physics, `stat_evap_vol`, or reported results. This plan repurposes that
  channel into the PET-rate prescription, fixing the defect.

---

## 3. Public API Design

### 3.1 Legacy C API

New subcatchment property enum (in `include/openswmm/legacy/engine/openswmm_solver.h`,
appended to the `swmm_SUBCATCH_*` block to preserve existing enum values):

```c
/*! \brief Externally prescribed potential evapotranspiration rate
 *         (in/day or mm/day). Settable while the simulation is running.
 *         Set a negative value to clear and revert to climate-derived evap. */
swmm_SUBCATCH_API_PET,
```

Usage: `swmm_setValueExpanded(swmm_SUBCATCH, swmm_SUBCATCH_API_PET, idx, -1, -1, value)`.
Getter returns the currently prescribed rate in user units, or a negative
value when not prescribed.

### 3.2 Refactored C API

Keep the existing exported symbol — repurposed, value semantics corrected:

```c
/**
 * @brief Prescribe a potential evapotranspiration rate on a subcatchment.
 *
 * The prescribed rate replaces (OVERRIDE) or augments (ADD) the
 * climate-derived evaporation rate for surface, LID, and groundwater
 * upper-zone evaporation. Actual evaporation remains capped by available
 * water, so mass balance is tracked through the normal continuity totals.
 *
 * @param engine   Engine handle.
 * @param idx      Subcatchment index.
 * @param value    PET rate in user units (in/day for US, mm/day for SI).
 * @param mode     SWMM_FORCING_OVERRIDE or SWMM_FORCING_ADD.
 * @param persist  SWMM_FORCING_RESET (auto-clear each step) or SWMM_FORCING_PERSIST.
 * @returns SWMM_OK or error code.
 */
SWMM_ENGINE_API int swmm_forcing_subcatch_evap(
    SWMM_Engine engine, int idx, double value, int mode, int persist);
```

Unit conversion (user → ft/sec) happens at the C API boundary so the forcing
channel stores internal units, consistent with the legacy setter convention.
(Previous documented behavior was non-functional — see §2.2 — so this is a
fix, not a breaking change; CHANGELOG.md entry required.)

### 3.3 Python APIs

Legacy (`python/openswmm/legacy/engine/_subcatchments.py`), mirroring
`set_api_rainfall` including the `ExternalForcingLog` hook:

```python
def set_api_pet(self, value, log=None):
    """Prescribe a potential evapotranspiration rate for this subcatchment.

    Overrides the climate-derived evaporation rate. Actual evaporation is
    capped to available water; losses appear in the runoff totals under
    ``evaporation``. Persists until cleared with L{clear_api_pet}.

    @param value: PET rate in user units (in/day or mm/day).
    @type value: float
    @param log: Optional external forcing log for audit.
    @type log: ExternalForcingLog or None
    @return: None
    @rtype: None
    """
```

Plus `get_api_pet()` and `clear_api_pet()` (sets the negative sentinel).

Refactored (`python/openswmm/engine/_forcing.pyx`): the existing
`Forcing.subcatchment_evap(sub, value, *, mode=..., persist=...)` keeps its
signature; docstring updated to document PET-rate semantics and in/day–mm/day
units (epytext style).

---

## 4. Legacy Engine Implementation

| # | Task | Files | Verify |
|---|---|---|---|
| L1 | Add `double apiEvapRate;` to `TSubcatch` (`// externally prescribed PET (ft/sec); MISSING when not set`) | `src/legacy/engine/objects.h` | builds |
| L2 | Initialize `apiEvapRate = MISSING` where `apiRainfall` is initialized (`subcatch.c:426` block) and on `swmm_start` | `src/legacy/engine/subcatch.c` | L7 tests |
| L3 | Add helper `double subcatch_getEvapRate(int j)`: returns `Subcatch[j].apiEvapRate` if `!= MISSING`, else applies legacy logic (`Evap.dryOnly` check + `Evap.rate`) | `src/legacy/engine/subcatch.c`, declare in `funcs.h` | L7 tests |
| L4 | Replace the three consumers: `subcatch.c:662–663` (use helper; the dryOnly branch moves into the helper), `gwater.c:523` (`MaxEvap = subcatch_getEvapRate(j) * FracPerv` — gwater has the subcatchment index in scope), `lid.c:1644` (`EvapRate = subcatch_getEvapRate(j)`) | `subcatch.c`, `gwater.c`, `lid.c` | L7 + mass-balance test |
| L5 | Add `swmm_SUBCATCH_API_PET` enum; `setSubcatchValue` runtime case: `value >= 0 → apiEvapRate = value / UCF(EVAPRATE)`, `value < 0 → apiEvapRate = MISSING` (allowed both before start and while running, like `API_RAINFALL`); `getSubcatchValue` case returning `apiEvapRate == MISSING ? -1.0 : apiEvapRate * UCF(EVAPRATE)` | `openswmm_solver.h`, `src/legacy/engine/swmm5.c` | L7 tests |
| L6 | Mass balance: no new accounting needed — prescribed rate flows through `Vevap`/`Vpevap` → `massbal_updateRunoffTotals()`. Confirm runoff continuity error stays < 0.5 % in tests | `massbal.c` (read-only) | L7 continuity assertions |
| L7 | Legacy Python binding: `SP.API_PET` enum in `_solver.pyx` enum block (`@cvar API_PET: ...`, near `API_RAINFALL` at line ~211/260), `set_api_pet` / `get_api_pet` / `clear_api_pet` in `_subcatchments.py` with `ExternalForcingLog` support (`mass_balance_category="runoff.evaporation"`), epytext docstrings | `python/openswmm/legacy/engine/_solver.pyx`, `_subcatchments.py` | unit tests §6 |
| L8 | `.pyi` stubs for L7 | `python/openswmm/legacy/engine/_solver.pyi` | `mypy`/stubtest pass |
| L9 | Read-only current climate evap rate: add `swmm_SYSTEM_EVAP_RATE` to `swmm_SystemProperty` (`openswmm_solver.h`, lines ~330–417); `getSystemValue()` case returning `Evap.rate * UCF(EVAPRATE)` (post-adjustment value the engine would use). Expose as `System.get_evap_rate()` in the legacy Python binding + `.pyi` stub. Enables caller-side adjustment composition (§1 precedence) | `openswmm_solver.h`, `swmm5.c`, `python/openswmm/legacy/engine/_system.py`, stubs | unit test: getter matches reported system evap; composition round-trip |

`.pyi` stub addition (L8):

```python
class Subcatchment:
    def set_api_pet(
        self, value: float, log: Optional[ExternalForcingLog] = None,
    ) -> None: ...
    def get_api_pet(self) -> float: ...
    def clear_api_pet(self) -> None: ...
```

---

## 5. Refactored Engine Implementation

| # | Task | Files | Verify |
|---|---|---|---|
| R1 | Rename forcing channel fields for clarity: `subcatch_evap_{mode,value,persist}` → keep names (surgical; only semantics doc changes). `value` now stores PET rate in **ft/sec** | `src/engine/data/ForcingData.hpp` (comments only) | builds |
| R2 | C API boundary conversion: in `swmm_forcing_subcatch_evap` (`openswmm_forcing_impl.cpp:147–160`), convert `value` from user units to ft/sec via the engine's unit system (`ucf::EVAPRATE`); update the header doc block (§3.2) | `openswmm_forcing_impl.cpp`, `include/openswmm/engine/openswmm_forcing.h` | unit test (SI + US) |
| R3 | **Delete** the ineffective `evap_loss` override block in `applyForcings()` (`SWMMEngine.cpp:3235–3245`) | `src/engine/core/SWMMEngine.cpp` | R7 tests |
| R4 | Surface evap: in `RunoffSolver::execute` per-subcatch rate loop (`Runoff.cpp:259–264`), after the broadcast/`DRY_ONLY` assignment, apply forcing: `OVERRIDE → evap_rate_[ui] = forced` (bypasses dryOnly, per §1), `ADD → evap_rate_[ui] += forced`. Read via `ctx.forcing` | `src/engine/hydrology/Runoff.cpp` | R7 tests |
| R5 | LID + groundwater: add a small shared inline helper, e.g. `double effective_evap_rate(const SimulationContext&, std::size_t ui, double broadcast)` in `ForcingData.hpp`; LID call site (`SWMMEngine.cpp:1129` / `LID.cpp:383–391`) and GW call site (`SWMMEngine.cpp:1841` / `Groundwater.cpp:237`, scalar `max_evap`) resolve per-subcatchment rates through it. GW `execute` changes its scalar `max_evap` parameter to per-subcatch lookup (it already receives per-subcatch arrays, so the signature change is small) | `LID.cpp/.hpp`, `Groundwater.cpp/.hpp`, `SWMMEngine.cpp` | R7 tests incl. GW/LID cases |
| R6 | Mass balance: no new accounting — forced rate flows through `Vevap` → `evap_loss[ui]`, `stat_evap_vol[ui]` (`Runoff.cpp:539–543`) and the runoff continuity totals. Keep the SIMD-friendly structure: forcing lookup happens once per subcatchment per step, outside the subarea hot loop | — | continuity assertions |
| R7 | Refactored Python binding: update `Forcing.subcatchment_evap` docstring (units, PET semantics, OVERRIDE/ADD, persist) in `_forcing.pyx`; `.pyi` already declares the method (`_forcing.pyi:41–44`) — confirm signature unchanged, refresh doc comment | `python/openswmm/engine/_forcing.pyx`, `_forcing.pyi` | unit tests §6 |
| R8 | Read-only current climate evap rate: new C API getter `swmm_climate_get_evap_rate(SWMM_Engine, double* value)` returning `ctx_.climate_state.evap_rate` in user units (post-adjustment); expose in the Python `Solver` API + `.pyi` stub. Enables caller-side adjustment composition (§1 precedence) | `include/openswmm/engine/` header, impl in `src/engine/core/`, Python binding + stubs | unit test: getter matches model evap; composition round-trip |

Note on persistence: refactored semantics stay as-is (`RESET` auto-clears via
`ctx_.forcing.clear_reset_entries()` at `SWMMEngine.cpp:736`; `PERSIST` holds
until `Forcing.clear()`). Legacy semantics persist until explicitly cleared
(sentinel), matching `apiRainfall`. The cross-engine difference already exists
for rainfall and is documented, not changed.

---

## 6. Unit Tests (definition of done for every item above)

All tests run against the real handle-based `openswmm.engine.Solver` /
legacy solver — **no engine mocks**. Test input and report files are written
to the repository test data/output folders (`python/tests/data`,
`python/tests/<engine>/output/`), not temp directories, so artifacts are
reviewable.

New files:

* `python/tests/engine/test_forcing_pet.py` (refactored)
* `python/tests/legacy/test_subcatch_api_pet.py` (legacy)
* `python/tests/test_pet_parity.py` (cross-engine parity)

Required cases (both engines unless noted):

1. **Override takes effect** — prescribe a high PET on a wet subcatchment;
   per-step evap loss reflects the prescribed rate, not the climate rate.
2. **Capping** — prescribe PET far exceeding available ponded water; actual
   evap loss ≤ available volume; no negative depths.
3. **Mass balance** — full run with persistent prescription; runoff
   continuity error < 0.5 %; `stat_evap_vol` / `RunoffTotals.evap`
   consistent with summed per-step losses.
4. **Clear/revert** — prescribe, step, clear (negative sentinel / `clear()`),
   step; evap returns to climate-derived value.
5. **ADD mode** (refactored only) — evap rate = climate + delta.
6. **Persist vs reset** (refactored only) — `RESET` affects exactly one step.
7. **DryOnly bypass** — model with `DRY_ONLY` evap and active rainfall;
   prescribed PET still evaporates (documented precedence).
8. **Groundwater + LID consumption** — model with GW and an LID unit;
   prescribed rate changes upper-zone GW evap and LID surface evap.
9. **Units** — same prescription on a US (in/day) and SI (mm/day) model
   produces internally consistent ft/sec rates (compare evap volumes).
10. **Parity** — identical model + prescription schedule in both engines;
    per-subcatchment evap volumes agree within the established legacy-parity
    tolerance.
11. **Getter round-trip** (legacy) — `set_api_pet(x)`; `get_api_pet() == x`
    in user units; after `clear_api_pet()`, getter returns negative.
12. **Error paths** — bad index, simulation-not-open, bad mode/persist.
13. **Adjustment composition round-trip** — read the climate-derived rate
    via the L9/R8 getter, scale it (e.g., ×0.8), prescribe the result;
    actual evap loss reflects the scaled rate, and the engine applied no
    additional adjustment of its own.

Stub checks: run the existing stubtest/typing job over the updated `.pyi`
files (`python/tests/typing`).

---

## 7. Implementation Order

1. L1–L6 (legacy engine) → legacy C-level behavior testable via existing
   pytest harness through the binding once L7 lands.
2. L7–L8 (legacy Python + stubs) → run `python/tests/legacy` suite.
3. R1–R6 (refactored engine; includes deleting the broken override block).
4. R7 (refactored Python docs/stubs) → run `python/tests/engine` suite.
5. §6 test files, parity test last.
6. CHANGELOG.md entry: new legacy `swmm_SUBCATCH_API_PET`; fixed +
   redefined `swmm_forcing_subcatch_evap` (previously ineffective).

## 8. Follow-up (separate repo, not in this plan's scope)

`openswmm.mcp`: `tools/forcing.py` already maps
`("subcatchment", "evap")` → `subcatchment_evap`; only the tool description
needs a units/semantics update after R2 lands. No schema change.

## 9. Open Questions / Decision Log

* **Resolved:** rate semantics, runtime-setter-only, all consumers, user
  units, prescribed-rate precedence over `DRY_ONLY` and monthly adjustments
  (see §1).
* **Resolved (2026-06-10):** adjustments / INP-based configuration — no
  engine-side adjustment of prescribed PET and no INP `[ADJUSTMENTS]`-style
  or timeseries assignment. Deferred entirely to the runtime getter/setter:
  the engine applies the prescribed rate verbatim; callers wanting monthly
  factors or other scaling read the climate-derived rate (L9/R8 getters),
  apply their own logic, and set the result. New test case 13 covers this
  composition round-trip.
* **Resolved (2026-06-10):** snowmelt interaction — verified that neither
  engine's snow module consumes the evaporation rate (`src/legacy/engine/snow.c`
  and `src/engine/hydrology/Snow.cpp` contain no `Evap.rate` / evap /
  sublimation references; snowmelt is temperature-driven and enters runoff
  only as net precip, `Runoff.cpp:279`). Therefore the prescribed PET
  correctly matches existing behavior with no snow-specific work: melt water
  on the surface is subject to the prescribed rate through the normal surface
  pathway, and no snowpack sublimation pathway exists to extend.
