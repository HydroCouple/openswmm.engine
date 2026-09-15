# E2 Implementation — Validation & Commit Handoff (2026-08-16)

**For:** the checking agent (same mandate as the E1 handoff: the implementing
sandbox ran `g++ -fsyntax-only` only — every touched TU passes; nothing
linked or executed).
**Base:** validated E1 (`a7824b32`) + `29f1577a` + `08e7900a`. This
changeset builds directly on the ArdEngine as your E1 fixes left it (the
rate-vs-amount load asymmetry, the down-only store resync, and the
`kMinStoreVol` donor guard are all preserved and the structure code follows
the same conventions).
**Plan:** `plans/transport/EULERIAN_ARD_TRANSPORT_PLAN.md` §6 E2 (as
re-scoped there today: storage mixing models and FV direct-state moved to
E2b — no inert-option parsing before semantics exist).

---

## 1. Changeset (uncommitted)

```
mod:  src/engine/transport/components/EulerianArdComponent/ArdEngine.{hpp,cpp}
mod:  tests/unit/engine/test_ard_transport.cpp        (+2 gates, deck helper ext)
mod:  plans/transport/EULERIAN_ARD_TRANSPORT_PLAN.md  (E1 ✅, E2/E2b re-scope)
mod:  tests/unit/engine/data/site_drainage_model.rpt  (NOT mine — pre-existing
      working-tree mod, presumably from your validation runs; commit or
      discard per your own judgement, separately)
```

## 2. What E2 adds

1. **Structure passthrough** (`substep()` step 5, `publish()` tail):
   pumps/orifices/weirs/outlets move constituent mass node-store →
   node-store at the donor's concentration (`donor = upstream by flow
   sign`, `mesh_.struct_n1/n2`), volume moved symmetrically, mass capped at
   donor holdings, donor below `kMinStoreVol` moves water but no mass —
   the same convention as your (iii) fix. Structure links publish the
   donor node's concentration (legacy convention). The E1 "structures not
   transported" init warning is removed.
2. **Persistent user quality-mass-flux forcing** enters the node stores
   (`dt_sub × user_conc_mass_flux`, a rate like `qual_mass_in`). Rationale:
   the engine-side post-quality `conc +=` bump is overwritten by the next
   ARD publish, so forced mass never reached the store. *Known transient
   artifact:* within the step where forcing applies, the engine-side bump
   plus the store path can both be visible in `nodes.conc` for one report
   snapshot; the mass ledger books once. Check it is acceptable (§3.4) —
   if not, the clean fix is gating the engine-side bump on
   `quality_solver == LEGACY`, which I did NOT do (it touches the forcing
   path your E1 validation just stabilized; your call).
3. **Loud CFL clamp:** `nsub > kMaxSubsteps(512)` now warns once per run
   with needed-vs-clamped counts (your §5.3 note).

## 3. Validation protocol

1. Build; zero new warnings from the touched files.
2. `ctest -R test_engine_ard_transport` — now FIVE gates: the three E1
   gates (must stay green — regression check on the structure code paths)
   plus `StructurePassthroughCarriesMass` (orifice deck; kills the
   E1 behavior of C2 pinned at zero) and `CstrLimitTracksLegacy`
   (`FV_MIN_CELLS 1` + `UPWIND` vs LEGACY, 20 %·c0 band — if the band is
   the only failure, record measured deltas in §5 and judge: the two
   models legitimately differ transiently; tighten or loosen with data,
   don't chase equality).
   *Anticipated failure modes:* (a) `[ORIFICES]` deck syntax vs the
   parser's column expectations; (b) orifice hydraulics under DYNWAVE with
   these offsets — if the orifice doesn't flow, the gate starves: check
   `links.flow[O1] > 0` mid-run before debugging transport; (c)
   `FV_MIN_CELLS 1` interacting with `min_cells` floor elsewhere.
3. Full suite — especially that the three E1 gates and the quality/options
   suites are untouched.
4. Smoke per E1 §3.5 on `site_drainage_model` + an added orifice: the E1
   structures warning must be GONE, continuity comparable to E1's numbers
   (DYNWAVE ~2.4 %, FV ~−5.8 %) or better, and the FV smoke should confirm
   the 1e25-class blowup stays dead with structures active.
5. Forcing check (§2.2): a deck driving `nodes_set_quality_mass_flux`
   (persistent) under EULERIAN_ARD — forced mass appears in downstream
   loads within ledger tolerance.
6. Append results to §5; commit with §4's message.

## 4. Commit message

```
feat(transport): ARD structure passthrough, forcing flux, loud CFL clamp (E2)

Structures (pumps/orifices/weirs/outlets) transport constituents as
zero-volume donor passthrough between node stores (mass-capped, empty-donor
guarded, symmetric volume), publishing donor concentration on the structure
link; persistent user quality-mass-flux forcing now reaches the ARD node
stores; the transport subcycle clamp warns loudly with needed-vs-clamped
counts. Plan E2 re-scoped: storage mixing models beyond CMSTR and FV
direct-cell-state move to E2b (no inert options before semantics). Gates:
StructurePassthroughCarriesMass, CstrLimitTracksLegacy (+3 E1 gates).

Plan: plans/transport/EULERIAN_ARD_TRANSPORT_PLAN.md §6 E2.
Validation record: plans/transport/E2_VALIDATION_HANDOFF_2026-08-16.md
```

## 5. Validation results

**Validated 2026-08-16**, Darwin 25.5.0 / arm64, preset `Darwin`
(`build/darwin`, Release). Artifacts, decks and the forcing driver:
`tests/output/e2_validation_2026-08-16/`. Base was `df7bdf12` (the LID
commit that landed on top of E1).

**Outcome: E2 passes.** Compiled first try, all five gates green, no
regressions. One additional ledger gap was found by protocol item 5 and is
fixed here; two related pre-existing defects are recorded in §5.6 rather
than fixed.

### 5.1 Build

Clean, and **zero warnings from the touched files** — the `<string>`
include for `std::to_string` is present and nothing else moved.

### 5.2 Gates — 5/5, and gate 4 has teeth

```
[ OK ] UniformFieldStaysUniform            (13 ms)   E1
[ OK ] FrontIsBoundedMonotoneAndOrdered    (11 ms)   E1
[ OK ] LegacyDefaultAndSteadyStateAgreement(21 ms)   E1
[ OK ] StructurePassthroughCarriesMass     (10 ms)   E2
[ OK ] CstrLimitTracksLegacy               (20 ms)   E2
```

None of the anticipated failure modes materialised: the `[ORIFICES]` deck
parsed, the orifice flowed, and `FV_MIN_CELLS 1` behaved.

Because an E1 gate once passed vacuously (0 ≈ 0 — see the E1 handoff §5.4),
gate 4 was checked against the **pre-E2 binary** rather than trusted. Same
deck, `git stash`-ed structure code, rebuilt:

| link (lbs) | pre-E2 | E2 | EPA legacy engine |
|---|---|---|---|
| C1 (upstream) | 16.762 | 16.762 | 16.692 |
| **O1 (orifice)** | **0.000** | **16.692** | 16.691 |
| **C2 (downstream)** | **0.000** | **16.613** | 16.567 |

Pre-E2 the E1 "not yet transported through" warning fired and nothing
crossed; post-E2 the structure carries the load to within 0.3 % of EPA and
the warning is gone. The gate fails if the passthrough is removed.

`CstrLimitTracksLegacy` passed inside its 20 %·c0 band on the first run —
no band adjustment needed, so no measured-delta judgement call was required.

### 5.3 No-regression — 128/129

`ctest -j6` (`ctest_e2_final.log`). The three E1 gates stayed green through
the structure code paths, and the quality/options suites are untouched. The
single failure is `test_engine_fv_integration` →
`RefiningTheMeshConvergesTowardTheDynwaveHydrograph`, bisected during E0
validation to predate all of this work (E0 handoff §4.3).

### 5.4 Smoke with structures active (§3.4)

`site_drainage_model` + an added orifice (ORX, J1→J5), quality continuity:

| | E1 (no structures) | E2 (structures active) |
|---|---|---|
| DYNWAVE | 2.449 % | **2.419 %** |
| FV | −5.817 % | **−6.538 %** |

Comparable to E1's numbers, the E1 structures warning is gone from both, and
**the 1e25-class FV blowup stays dead** with structures active.

### 5.5 Forcing (§3.5 item 5) — worked, but exposed a missing ledger row

Driver: `forcing_probe.c` (in the artifacts dir), C API, persistent
`swmm_node_set_quality_mass_flux(e, J0, TSS, 1.0)` set once after `start()`.
1.0 mass/s × 21600 s = **1.348 lbs** expected.

| | C1 | C2 | O1 | outflow | continuity |
|---|---|---|---|---|---|
| ARD, no forcing | 16.762 | 16.613 | 16.692 | 16.551 | 0.263 % |
| ARD, forced | 18.103 | 17.942 | 18.027 | 17.875 | **0.262 %** |
| delta | +1.341 | +1.329 | +1.335 | +1.324 | — |

The forced mass reaches the stores and propagates downstream at 1.33 lbs
against 1.348 expected, and the ledger books it exactly once (no double
count with the engine-side bump, which the next publish overwrites as §2.2
predicted). The transient artifact in §2.2 is present and acceptable — it
is one report snapshot, and the ledger is unaffected.

**Fixed here:** the run initially read **−7.717 %**.
`routing_forcing_qual_inflow` has been booked since the forcing API landed
but was consumed by no continuity formula and no report row, so the whole
forced amount read as error the moment E2 made the forcing physically real.
`DefaultReportPlugin` now reports a `User Forced Inflow` row (only when
non-zero) and counts it in `total_in` — hence the 0.262 % above, matching
the unforced 0.263 %.

### 5.6 Found, NOT fixed — recorded deliberately

1. **LEGACY quality forcing is booked but never delivered.** The same probe
   under `QUALITY_SOLVER LEGACY` produces loads *identical* to the unforced
   run (C1 16.715 both), because the engine-side `conc +=` bump is
   overwritten by the next `mixAtNodes`. With the ledger row now honest,
   that deck reports **7.414 %** — the report correctly saying "you booked
   1.348 lbs that never entered". Previously the omission and the phantom
   booking cancelled, hiding it. This is the same shape as the E1
   wet-weather term, and re-hiding it would repeat that mistake. The real
   fix is to route LEGACY forcing through `qual_mass_in` instead of a
   post-hoc concentration bump — a change to the forcing path E1 validation
   stabilised, so it is left for a scoped commit of its own.
2. **The FLOW side has the identical omission.** `routing_forcing_inflow` is
   booked at `SWMMEngine.cpp:3441` and appears in neither
   `MassBalance::routing_error()` nor the flow continuity table, so any deck
   using `swmm_node_set_lateral_inflow` carries a spurious flow continuity
   error. Symmetric to what §5.5 fixed, entirely pre-existing, and untouched
   here only because changing the flow ledger risks perturbing flow goldens
   across the suite for a defect E2 does not create.

### 5.7 Loud CFL clamp (§2.3)

Fires, and reaches the `.rpt`:

```
QUALITY_SOLVER EULERIAN_ARD: transport subcycling clamped at 512 substeps
(needed 2356) — effective CFL exceeds the stability target; shorten
ROUTING_STEP or increase FV_CELL_LENGTH.
```

Verified at `ROUTING_STEP 120` with `FV_CELL_LENGTH 0.25` (needed 2356) and
`0.1` (needed 588); silent on every normal deck, once per run as intended.

### 5.7a Follow-up: §5.6 revisited against SWMM 5.2.4 — one item fixed, one WRONG

Both §5.6 items concern the runtime forcing **API**, so they were re-examined
against the legacy reference. The verdicts differ:

**§5.6 item 2 (flow side) was a WRONG CALL — there is no bug.**
`effectiveUserLatFlow()` is already folded into `sum_ext` → `step_ext_inflow`
→ `routing_external` (`SWMMEngine.cpp`, issue #113), exactly as legacy does
(`routing.c:553`: `q = Node[j].apiExtInflow; … massbal_addInflowFlow(
EXTERNAL_INFLOW, q)`), with `routing_forcing_inflow` documented in-code as a
diagnostic *subset*. Verified: 2 cfs forced onto a 1 cfs deck reports
External Inflow 1.488 acre-ft (exactly 3 cfs × 21600 s) and closes at
−0.204 %. The earlier claim came from grepping the continuity formula without
checking where the term was already summed.

**§5.6 item 1 (quality side) was real and is now fixed**, and the legacy
reference showed the E2 approach was a workaround rather than the mechanism:

```c
/* routing.c addExternalInflows() */
w = Node[j].apiExtQualMassFlux[p];
if (w > 0.0) { Node[j].newQual[p] += w;
               massbal_addInflowQual(EXTERNAL_INFLOW, p, w); }
```

Legacy delivers the forced mass **in the loader stage** and books it as
**EXTERNAL_INFLOW** — no post-hoc concentration bump, no ledger row of its
own. Changes made:

- `QualitySolver::addExtInflowLoads()` now adds `user_conc_mass_flux` (a
  rate, positive only) to `qual_mass_in` and books it into
  `qual_routing_ex_in`. This one path feeds **both** engines, so the ARD
  special case is gone.
- The post-quality `conc +=` bump in `SWMMEngine.cpp` is removed — that was
  the reason the mass never persisted under either engine.
- E2's direct `user_conc_mass_flux` addition inside `ArdEngine::substep()` is
  removed; it would now double the forced mass.
- E2's separate `User Forced Inflow` continuity row is reverted. It was also
  incomplete: there are **two** continuity-error implementations — the report
  plugin's and `swmm_get_quality_continuity_error()`'s — and it only patched
  the first. Both read `qual_routing_ex_in`, so folding into it fixes both.
  `routing_forcing_qual_inflow` survives as the API-visible diagnostic subset,
  mirroring the flow side.

Measured, same probe (1.0 mass/s at J0 = **1.348 lbs** over the run):

| | C1 | External Inflow | continuity |
|---|---|---|---|
| LEGACY, unforced | 16.715 | 16.856 | 0.007 % |
| LEGACY, forced | 18.052 | **18.204** | **0.007 %** |
| ARD, unforced | 16.762 | 16.856 | 0.263 % |
| ARD, forced | 18.103 | **18.204** | **0.263 %** |

External Inflow rises by exactly 1.348 lbs under both engines and neither
continuity error moves — delivered, booked once, no double count. LEGACY
forcing works for the first time.

**Gate:** `ForcedQualityMassFluxRoutesUnderBothEngines` asserts both halves
(mass arrives downstream AND the ledger closes) for LEGACY and EULERIAN_ARD,
because each half fails silently on its own. Verified to fail against the
committed E2 build: LEGACY reported `base 12.4999… -> forced 12.4999…`
(no delivery) and ARD reported a −0.995 ledger error.

Still not fixed, and still not API-related: the `site_drainage_model`
outfall mass creation and the washoff divergence from EPA in §5.6 of the E1
handoff — both solver-parity questions with no API surface.

### 5.8 Notes

- `tests/unit/engine/data/site_drainage_model.rpt` (§1) resolved itself
  during this session and is no longer modified; nothing was committed for
  it. Its content delta had been a version banner reading `alpha.2` against
  the golden's `alpha.3`, i.e. a run against a stale binary.
- `forcing_probe.c` needs a versioned dylib symlink to link:
  `ln -sf libopenswmm.engine.6.0.0.dylib build/darwin/bin/Release/libopenswmm.engine.6.dylib`
  (removed again afterwards). It must call `end()` → `report()` → `close()`,
  or the `.rpt` ends in "[Report interrupted]".
