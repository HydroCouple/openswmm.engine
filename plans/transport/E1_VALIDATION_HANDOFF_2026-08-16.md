# E0+E1 Implementation — Validation & Commit Handoff (2026-08-16)

**For:** the checking agent (full build toolchain; the implementing sandbox
could only run `g++ -fsyntax-only` — every TU passes it, but nothing here
has been linked, executed, or regression-run).
**Plans:** `plans/transport/EULERIAN_ARD_TRANSPORT_PLAN.md` §6 phases E0–E1;
`UNIFIED_TRANSPORT_MASTER_PLAN.md` D-UT1/D-UT6.
**Supersedes:** `E0_VALIDATION_HANDOFF_2026-08-16.md` §2–3 (its E0 protocol
is folded into §3 below; its §1 inventory of commit `3ca9f4ed` still
applies).

---

## 1. State of the tree

- **Committed:** `3ca9f4ed` — pre-existing HGL/node-head checkpoint (see the
  E0 handoff §1; validate its tests as part of the full suite).
- **Uncommitted, TWO DISJOINT changesets** (no file overlaps — commit
  separately, in order):

**Changeset A — E0 kernel promotion:**
```
new:  src/engine/transport/fvkernels/SpeciesTransportKernels.{hpp,cpp}
mod:  src/engine/hydraulics/fv/ExplicitFvSolver.{cpp,hpp}
mod:  src/engine/CMakeLists.txt                    (transport/ globs)
```

**Changeset B — E1 Eulerian ARD engine:**
```
new:  src/engine/transport/components/EulerianArdComponent/ArdEngine.{hpp,cpp}
new:  tests/unit/engine/test_ard_transport.cpp
mod:  src/engine/core/SimulationOptions.hpp        (QualitySolverKind + field)
mod:  src/engine/input/handlers/OptionsHandler.cpp (QUALITY_SOLVER key)
mod:  src/engine/quality/QualityRouting.{hpp,cpp}  (assembleExternalLoads split)
mod:  src/engine/core/SWMMEngine.{hpp,cpp}         (ard_ member + B5 branch)
mod:  tests/unit/engine/CMakeLists.txt             (test_engine_ard_transport)
```

Untracked bystanders to leave alone: `.claude/`, `CLAUDE.md`, `cliff.toml`,
`paper/`, `SWMM_5.3-6.0_User_Testing_Questionnaire.docx`.

## 2. What E1 does (review orientation)

`[OPTIONS] QUALITY_SOLVER EULERIAN_ARD` routes quality through
`transport::ArdEngine` instead of the legacy CSTR stages:

- **Mesh:** `fv::buildNetworkMesh` cell mesh, built lazily on the first
  routing step (needs Router::init's `mod_length`); build failure warns and
  falls back to LEGACY — never a silent no-quality run.
- **Loads:** `QualitySolver::assembleExternalLoads` (a behavior-preserving
  split of `execute()`'s first stage — washoff/RDII/DWF/GW/iface into
  `qual_mass_in`/`qual_vol_in`) feeds both engines identically.
- **Projection** (plan §3.2): per conduit, face flux ramps
  `F_i = Q + (dV/dt)·(1/2 − i/n)` so every cell receives an equal share of
  the conduit's volume change; splice faces average the two conduits' end
  values; `sstar = flux` gives sign-of-flux upwinding through the promoted
  kernels (`hllc=true` path).
- **Transport:** CFL-subcycled (0.9, cap 512/step) calls into
  `fvk::reconstructScalars` (MUSCL default via the `FV_SCALAR_SCHEME`/
  `FV_LIMITER` options) + FCT; node-boundary faces override the kernels'
  zero-gradient ghost with the node store as inflow donor; cell mass/area
  update from the same fluxes; node CSTR stores (mass + volume) advance
  from face exchange + prorated external loads, volumes resyncing to the
  solver's each routing step.
- **Publish:** volume-weighted cell→link conc into `links.conc`,
  node stores into `nodes.conc`, `*_old` maintained — reporting unchanged.

**Deliberate E1 limitations (each warns where relevant; verify the warnings
fire, do NOT "fix" these — they are later plan phases):** structures
(pumps/orifices/weirs) not transported through (E2); kdecay/treatment not
applied under ARD (E4, warning on init); dispersion off (E3); FV hydraulics
also use the projection rather than direct cell state (E2); no
mass-balance-ledger rows yet (E5); species registry deferred —
`n_species = n_pollutants` (first consumer is the reactions module, T1).

## 3. Validation protocol (in order; stop at first failure)

1. **Commit A first, in isolation:**
   ```
   git stash push -m e1 -- src/engine/transport/components tests/unit/engine/test_ard_transport.cpp \
       src/engine/core/SimulationOptions.hpp src/engine/input/handlers/OptionsHandler.cpp \
       src/engine/quality src/engine/core/SWMMEngine.hpp src/engine/core/SWMMEngine.cpp \
       tests/unit/engine/CMakeLists.txt
   ```
   Build; run the full unit suite (the five species gates in
   `test_fv_solver_network` are the E0 gates); then the **bitwise check**:
   run the FV benchmark decks (site_drainage + FV parity models,
   `FLOW_ROUTING FV`) against a pre-E0 baseline (`git stash` E0 too, or
   build `3ca9f4ed` in a worktree) — outputs must be byte-identical
   (species are wired to zero in production; any diff = refactor defect).
   Commit with the message in §4-A. Then `git stash pop`.
2. **Changeset B build** (all platforms you routinely build): zero new
   warnings beyond the intended runtime ones.
3. **New gates:** `ctest -R test_engine_ard_transport` — three tests:
   UniformFieldStaysUniform (explicitly asserts NO LEGACY fallback),
   FrontIsBoundedMonotoneAndOrdered, LegacyDefaultAndSteadyStateAgreement.
   *Anticipated failure modes worth checking before debugging deep:* (a)
   initial `links.conc` not seeded from `Cinit` before the first routing
   step — the ArdEngine snapshots `links.conc` at lazy init; (b) the
   conduit face-side convention (ArdEngine.cpp `projectHydraulics`,
   side==0 ⇒ face at the cell's axis-positive end — verified against the
   FCT gather, but a runtime disagreement would show as immediate uniform-
   field failure); (c) `[INFLOWS] CONCEN` semantics in the generated deck.
4. **No-regression:** full unit suite — especially quality tests (LEGACY
   path must be bit-identical: `assembleExternalLoads` is a pure split) and
   the options hydration/parsing tests (new `QUALITY_SOLVER` key must not
   perturb unknown-key handling).
5. **Manual smoke:** any real quality deck of your choosing with
   `QUALITY_SOLVER EULERIAN_ARD` under DYNWAVE and under FV: runs to
   completion, warnings present as specified, concentrations plausible,
   `.rpt` continuity not wildly off (ledger rows are E5 — expect the
   quality continuity section to reflect the legacy accounting only).
6. Append all results to §5; commit B with the §4-B message.

## 4. Commit messages

**A (E0):** use the message block in `E0_VALIDATION_HANDOFF_2026-08-16.md`
§3 verbatim.

**B (E1):**
```
feat(transport): QUALITY_SOLVER EULERIAN_ARD — solver-agnostic ARD engine (E1)

Adds transport::ArdEngine: the promoted FV species kernels driven over the
NetworkMeshBuilder cell mesh under ANY routing model, with
continuity-consistent projected face fluxes (per-conduit dV ramp, splice
averaging), sign-of-flux upwinding, MUSCL/QUICKEST + Zalesak FCT, node-store
junction mixing fed by the shared QualitySolver load assembly
(assembleExternalLoads split, behavior-preserving), CFL subcycling, and
publication into the legacy links/nodes conc arrays. Selected by
[OPTIONS] QUALITY_SOLVER (LEGACY default; EULERIAN_ARD; LAGRANGIAN reserved).
Lazy init with warned fallback to LEGACY. E1 scope: conservative transport;
structures/decay/treatment/dispersion/FV-direct-state follow plan phases
E2-E5 (warnings emitted). Gates: tests/unit/engine/test_ard_transport.cpp.

Plan: plans/transport/EULERIAN_ARD_TRANSPORT_PLAN.md §6 E1.
Validation record: plans/transport/E1_VALIDATION_HANDOFF_2026-08-16.md
```

## 5. Validation results

**Validated 2026-08-16**, Darwin 25.5.0 / arm64, preset `Darwin`
(`build/darwin`, Release, unit + regression tests). Artifacts, decks and
scripts: `tests/output/e1_validation_2026-08-16/`. Changeset A (E0)
committed first as `08e7900a` — see
`E0_VALIDATION_HANDOFF_2026-08-16.md` §4.

**Outcome: E1 did not work as delivered, and could not have.** Two defects
in ArdEngine and one more found later under FV; and *four* pre-existing
engine faults that made the gates unreachable no matter how correct E1 was.
All are fixed. Gates 1–3 now pass as originally written. Commits:

- `29f1577a` — `fix(quality): deliver [INFLOWS] pollutant loads, seed Cinit,
  close the mass ledger` (the pre-existing faults)
- `<E1>` — `feat(transport): QUALITY_SOLVER EULERIAN_ARD` (§4-B message)

### 5.1 Build

Changeset B compiled first try. One new warning attributable to E1 —
`ArdEngine.cpp:420 unused variable 'uns'` — which the substep fix below
consumed. Everything else in the warning set is pre-existing and in
untouched files. Log: `build_e1.log`.

### 5.2 Pre-existing faults that blocked the gates

Anticipated failure modes (a) and (c) in §3 were both real, and both in the
engine rather than in the deck. Established against the **EPA legacy engine
on the identical deck**, and proved to predate E1 by rebuilding with
changeset B shelved — the LEGACY quality path is **byte-identical with and
without E1** (4/4 `.out`, `legacyq_noE1/` vs `legacyq_withE1/`), which also
discharges §3.4.

| | EPA | new engine (before) | new engine (after) |
|---|---|---|---|
| Flow external inflow (acre-ft) | 0.496 | **6.694** | 0.496 |
| Quality external inflow (lbs) | 16.843 | **0.000** | 16.856 |
| Initial stored mass (lbs) | 0.382 | **0.000** | 0.382 |
| Final stored mass (lbs) | 0.403 | **0.000** | 0.403 |
| Quality continuity error | 0.139 % | **−11383 % / 51 %** | −0.031 % |

1. **`[INFLOWS]` pollutant rows were inert and corrupting.** `ExtInflowSoA`
   carried no constituent, so every row was summed into the node's flow: a
   `TSS … CONCEN … 12.5` row injected 12.5 **cfs** of phantom water — 13.5×
   the intended inflow — while delivering no mass. This corrupted the
   *hydraulics* of any deck with pollutant inflows, independent of quality.
2. **`Cinit` was never applied.** Parsed, stored, written back out by
   InpWriter, exposed on the C API — and never seeded into any state.
3. **`qual_routing_init` / `qual_routing_final` were never written.** Read
   by the report and the C API only, so the storage terms were always 0 and
   the continuity error carried the whole of the stored mass.
4. **Wet-weather quality was booked from the wrong quantity, in the wrong
   place** — `lat_flow × node concentration`, for every node with lateral
   flow, in `updateRoutingMassBalance()`. That is the node's *resulting*
   concentration, and `lat_flow` lumps runoff with DWF/GW/RDII/direct
   inflows, so every one of those was counted a second time as "wet
   weather". Invisible only while direct inflows delivered nothing: fixing
   (1) lit it up as a 2× double count.

### 5.3 Defects in E1 itself

**(i) External loads landed a factor of `dt_step` too small.**
`substep()` added `load_frac * qual_mass_in`. `qual_vol_in` is an AMOUNT
(the loaders add `q*dt`) so prorating it is right, but `qual_mass_in` is a
RATE — `mixAtNodes` multiplies it by `dt`. The adjacent-line asymmetry is
what made this easy to miss. Confirmed before fixing by sweeping the step:
the load deficit factor **equals `ROUTING_STEP` in seconds** (4.98× at 5 s,
9.95× at 10 s). Fix: integrate over `dt_sub`.

**(ii) Boundary nodes accumulated mass they could not shed.** Water leaving
the system at an outfall rides no mesh face, so the store never saw it go,
while `step()` resynced the store's volume to the solver's (≈0 at a FREE
outfall) and *preserved mass* — `conc = mass/vol` diverged. Confirmed by
run length: input grew linearly (2.809 → 5.619 → 11.237 → 16.856 lbs at
1/2/4/6 h) while reported outflow grew **quadratically** (144.99 → 689.57 →
2992.32 → 6913.22 lbs), the signature of a store filling without an outlet.
Continuity error **−34,078 %**. Fix: a well-mixed store discharges at its
own concentration, so the mass scales with the water at the resync —
scaling **down only**, since a store the solver reports as larger has
gained water the store did not track.

**(iii) A near-empty node store donated an astronomical concentration.**
The node-boundary donor override divided by `max(node_vol_, 1e-12)`. Found
only in the §3.5 smoke test: `site_drainage_model` under **FV** produced an
outfall load of **1.6e25 lbs** while the same deck under DYNWAVE was fine,
and the same deck at `ROUTING_STEP 1 s` was fine — the injection needed the
larger step to run away. Fix: below the store-empty threshold the donor
contributes nothing, which is both stable and what "the node holds no
water" means.

*Note for later phases:* `nsub` is still silently clamped at
`kMaxSubsteps = 512`, so a deck fine enough to need more substeps violates
CFL without saying so. It no longer blows up, but the cap should become
loud (or adaptive) in E2/E5.

### 5.4 Gates (§3.3) — 3/3 PASS, as originally written

```
[ OK ] ArdTransportTest.UniformFieldStaysUniform            (17 ms)
[ OK ] ArdTransportTest.FrontIsBoundedMonotoneAndOrdered    (13 ms)
[ OK ] ArdTransportTest.LegacyDefaultAndSteadyStateAgreement (25 ms)
```

Gate 1's `ASSERT_FALSE(fell_back)` holds — the ARD engine is genuinely
active, not silently on the legacy path. Worth recording that gate 3
**passed even when the engine was completely broken**: with no pollutant
anywhere, 0 ≈ 0 satisfied it. Its agreement band is only meaningful once
the other two pass.

### 5.5 No-regression (§3.4) — 128/129

`ctest -j6`, log `ctest_e1_final.log`. Quality-path tests all pass
(`test_engine_quality_routing`, `test_engine_quality_roundtrip`,
`test_engine_massbalance`, `test_engine_treatment`), as do the options
hydration/parsing tests with the new `QUALITY_SOLVER` key.

The single failure is `test_engine_fv_integration` →
`RefiningTheMeshConvergesTowardTheDynwaveHydrograph`, the pre-existing
failure bisected during E0 validation: bit-identical error values at E0, at
HEAD, and at `3ca9f4ed^`, so it predates all of this work. See the E0
handoff §4.3.

### 5.6 Manual smoke (§3.5)

`QUALITY_SOLVER EULERIAN_ARD` under both routing models, quality continuity:

| deck | DYNWAVE | FV |
|---|---|---|
| 3-conduit DWF feed (16.856 lbs in) | **0.257 %** | **−0.082 %** |
| `site_drainage_model` (184 subcatchments) | **2.449 %** | **−5.817 %** |

Both scope warnings fire verbatim as specified — kdecay (verified with
`Kdecay 0.5`) and structures (verified with an added orifice). Link loads
on the DWF deck: ARD 16.770/16.626/16.477 vs EPA 16.692/16.547/16.418
(≤0.5 %).

### 5.7 Open, NOT fixed — pre-existing, and worth its own change

On `site_drainage_model` the **LEGACY** quality path reports 9.224 lbs of
outfall load against 0.080 lbs in — mass creation, unchanged by any of this
work (9.224 before and after). EPA reports 0.000 for every quality row on
that deck, i.e. it generates no washoff there at all, so the new engine
also diverges from EPA on washoff itself. Two separate pre-existing parity
questions, both outside this changeset. Note the contrast: **ARD conserves
on the same deck** (0.078 out against 0.080 in) where the legacy CSTR path
does not.

Also note `links.old_volume` is correctly rolled per routing step by
`save_state()` (a `std::copy`, easy to miss when grepping for assignment) —
an early suspicion here was wrong.
