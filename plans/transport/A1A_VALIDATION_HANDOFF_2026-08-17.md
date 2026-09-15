# A1a Implementation — Validation & Commit Handoff (2026-08-17)

**For:** the checking agent (sandbox: `g++ -fsyntax-only` only).
**Base:** `721ae60c` (post-E5b).
**Plan:** `WATER_AGE_TRACKING_PLAN.md` §1–§2, §7 A1 — **split decision:**
A1a = the age species on the ARD mesh + the waterage component + per-source
loader wiring (this shot); A1b = the LEGACY CSTR age mirror (warned until
then); A2 = hotstart + ledger row; A3/A4 = watershed/LID states. Same risk
logic as the E5 split.
**Standing findings:** lessons 1–25; especially 20 (gate 1 IS the
motivating pure-age deck), 15 (gate 4 is the symmetric-row razor for the
new row class), 14 (see §2.3 — the guard fix it predicted).

---

## 1. Changeset (uncommitted)

```
mod:  src/engine/core/SimulationOptions.hpp     (bool water_age)
mod:  src/engine/input/handlers/OptionsHandler.cpp ([OPTIONS] WATER_AGE)
new:  src/engine/data/WaterAgeData.hpp
      (WaterAgeSource enum — 7 pathways; WaterAgeConfigData: GLOBAL ages
       (hours→SECONDS at parse) + NODE overrides for DWF/EXTERNAL_INFLOW,
       source_age() lookup; WaterAgeState: node_age_vol_in RATE
       (age·ft³/s, the qual_mass_in analogue) + published node/link ages)
mod:  src/engine/core/SimulationContext.hpp     (water_age_config/_state
       members + reset() hygiene)
new:  src/engine/transport/components/WaterAgeModule/WaterAgeComponent.{hpp,cpp}
      (org.hydrocouple.openswmm.waterage: [WATER_AGE_SOURCES] GLOBAL+NODE
       constant hours; TIMESERIES/SUBCATCH/EDGE_BC precise deferrals;
       never-half-apply; bypass warnings both directions)
mod:  src/engine/quality/QualityRouting.cpp
      (loadersNeeded += water_age — the pure-age deck needs the volume
       half at np == 0, the E5a lesson-20 shape pre-empted; addAgeVolume
       helper; SEVEN injection sites: ext-inflow/washoff/LID-drain (as
       RAINFALL until A4)/RDII/DWF/GW/iface each add q·age_source;
       assembleExternalLoads sizes+zeroes the accumulator per step)
mod:  src/engine/core/SWMMEngine.cpp            (registerWaterAgeComponent;
       __WATER_AGE__ RESERVED_AGE registry row when ON; B5 stays alive for
       pure-age decks; WATER_AGE-under-LEGACY warns naming A1b)
mod:  src/engine/data/SpeciesRegistry.hpp       (transported_count +=
       RESERVED_AGE)
mod:  src/engine/transport/components/EulerianArdComponent/ArdEngine.{hpp,cpp}
      (age_row_ = np+nm LAST row; ns += 1; cell/store init from
       INITIAL_STATE; substep: age-volume load integrates like
       qual_mass_in; step(): exact aging — cells += dt, stores +=
       dt·vol; publish routes the age row to water_age_state (links,
       nodes, structure passthrough); sidecar names it __WATER_AGE__)
mod:  src/engine/transport/components/ReactionModule/ReactionArdBinding.cpp
      (ns_total guard: equality → `< np+nm` — see §2.3)
new:  tests/unit/engine/test_water_age.cpp      (6 gates)
mod:  tests/unit/engine/CMakeLists.txt
```

All TUs pass `g++ -std=c++20 -fsyntax-only`. Reconfigure: one new engine
`.cpp` + one test TU.

## 2. Design decisions to review

1. **Age lives as the LAST state row** (after pollutants and MSX):
   advection/dispersion/FCT and volume-weighted junction mixing come free
   from the shared kernels; the transported quantity is age-volume, so
   mixing is conservative by construction (plan §1). Internal unit:
   SECONDS (config speaks hours).
2. **Aging is the exact integral** — cells += dt, store mass += dt·vol —
   once per routing step, Lie-split like every stage. No integrator runs
   for the age row (d(age)/dt = 1 has a closed form; the R4 exponential
   argument again).
3. **Per-source ages ride the loaders**: each of the seven pathways adds
   `q · age_source(kind, node)` as a RATE. Dispersion of age is ON if the
   deck configures dispersion (age is a transported species like any
   other) — flag if you think age should be exempt.
4. **LID drain water counts as RAINFALL age until A4** (documented at the
   injection site).
5. **A1b/A2/A3 deferrals are loud**: LEGACY+ON warns naming A1b;
   TIMESERIES/SUBCATCH rows error naming their phases.

### 2.3 The guard the age row nearly tripped (lesson-14 shape, pre-empted)

reactArdStage validated `ns_total != np + nm` and returned — a layout
consistency check written for E4's two-block state. Adding the age row
makes ns_total = np+nm+1, so the OLD guard would have SILENTLY skipped
every MSX reaction on any deck with WATER_AGE ON. Changed to
`ns_total < np + nm`; gate 4's MSX leg (a REACTING species, bitwise
across ON/OFF) is the observer — falsifier iii restores the equality.

## 3. Validation protocol

1. Reconfigure, build, zero new warnings.
2. `ctest -R test_engine_water_age` — six gates.
   *Anticipated failure modes, likelihood order:*
   (a) **Gate 1's V/Q band (±40%)** — the 1-h horizon may not fully
   equilibrate the age front, and V includes near-stagnant storage.
   Measure the ratio and tighten; extending END_TIME is the deck fix if
   equilibration is the issue.
   (b) **Gate 2's 5% shift band** — requires the aged inflow to have
   fully flushed the chain (same physics gate 1 needs). If the shift
   lands below 21600, check equilibration BEFORE suspecting the wiring:
   the shift converges to exactly 21600 from below.
   (c) **Gate 3's level pool** — same FIXED-stage micro-flow caveat as
   E4; equal ages make mixing harmless, so only VOLUME loss paths could
   perturb it (should not, at 0.1%).
   (d) **Gate 4's bitwise claim** — age is the last row; every per-row
   loop indexes by explicit row, loads are np-strided, msx rows [np,
   np+nm). A failure is a REAL stride/guard defect (locate, don't
   tolerance).
3. **Falsifier sweep** (verified restoration; table in §5):

   | falsifier | expected failing gates |
   |---|---|
   | i. comment the aging stage in ArdEngine::step | 1, 3 (ages stay ~0 / initial) |
   | ii. comment addAgeVolume in addExtInflowLoads | 2 (no shift); 1 unaffected (age-0 inflow) |
   | iii. restore the ns_total equality guard in reactArdStage | 4 (MSX leg — X stops reacting under ON) |
   | iv. drop the loadersNeeded water_age term | 1 (pure-age deck: no volume assembly → no dilution-side mixing; measure which assert trips) |
   | v. drop the hours→seconds ×3600 | 2 (shift = 6 s not 21600 s) |
   | vi. drop a bypass warning | 6 (its leg) |
   | vii. skip the publish age routing | 1/3 (published ages stay 0 while state ages) |
4. **Prior suites all green** — every existing deck has WATER_AGE off ⇒
   age_row_ = -1 and every new branch is dead; bit-identity across the 14
   benchmarks must hold EXACTLY. The E4 suite additionally re-runs the
   reactArdStage guard change with na = 0 (`ns_total == np+nm` satisfies
   `<` trivially). Sanitizers over the new suite.
5. **Pre-existing gaps observed, NOT fixed:** QUALITY_SOLVER and
   WATER_AGE are absent from InpWriter's [OPTIONS] round-trip (both
   engine-selection keys share the gap; recorded for IO-later).
6. Append results to §5; commit with §4.

## 4. Commit message

```
feat(transport): water age tracking on the ARD mesh (A1a)

[OPTIONS] WATER_AGE ON adds the reserved __WATER_AGE__ species
(RESERVED_AGE) as the last ARD state row: zero-order aging integrated
exactly (cells += dt, stores += dt*vol per routing step), volume-weighted
mixing/advection/dispersion free from the shared kernels, published to
water_age_state (seconds). Per-source initial ages configure through the
waterage component ([WATER_AGE_SOURCES] in model.age: GLOBAL for
RAINFALL/DWF/GW/RDII/EXTERNAL_INFLOW/IFACE/INITIAL_STATE + NODE overrides
for DWF/EXTERNAL_INFLOW, constant hours in A1a) and ride the seven
QualitySolver loader pathways as q*age_source age-volume rates; the
loader predicate now includes water_age so the PURE-AGE model (no
[POLLUTANTS] - the motivating configuration, lesson 20) assembles its
volumes. reactArdStage's ns_total equality guard relaxed to `<` - the
old check would have silently skipped ALL MSX reactions with the age row
present (lesson-14 shape, pre-empted; gate 4 observes it). LEGACY age is
phase A1b and warns loudly until then; TIMESERIES/SUBCATCH ages defer
with precise phase names. Gates: tests/unit/engine/test_water_age.cpp
(6: residence-time on the pure-age deck, the exact 6-h source-age shift,
exact level-pool aging, the symmetric-row bitwise razor, config errors,
bypass warnings).

Plan: WATER_AGE_TRACKING_PLAN.md sections 1-2, 7 A1 (A1a half).
Validation record: plans/transport/A1A_VALIDATION_HANDOFF_2026-08-17.md
```

## 5. Validation results

*(appended by the checking agent)*

### 5.0 Outcome

**Committed.** 5 of 6 delivered gates passed on arrival; the two failures
were a gate-deck fault and a parse-ordering defect. Validation additionally
found a **crash** on a plausible config typo, an **unreachable deferral**,
a **silent save-as data-loss path**, and — via gate 2's diagnosis — a
**pre-existing ARD transport defect that is the largest finding in this
phase and is NOT fixed here** (§5.2).

Final: 7/7 A1a gates (6 delivered + 1 added), suite **138/139** (only the
known pre-existing `FvEngine.RefiningTheMeshConvergesTowardTheDynwave-
Hydrograph`), **14/14 bit-identical** vs a `721ae60c` worktree build,
ASan/UBSan **0 findings across 54 tests** in six transport suites.
Falsifier sweep 11/11 caught, every case restored and re-verified.

### 5.1 Gate 2 — the shift was 15246 s, not 21600. The age row was innocent.

The handoff's anticipated cause (b) was incomplete flushing, with the
instruction to check equilibration before suspecting the wiring. It is not
equilibration:

| END_TIME | 1 h | 2 h | 4 h | 8 h | 12 h | 24 h |
|---|---|---|---|---|---|---|
| shift at C5 (s) | 15246.835 | 15246.835 | 15246.835 | 15246.835 | 15246.835 | 15246.835 |

Bit-identical at every horizon — a **plateau, not a convergence**. C1, which
flushes in seconds, showed the same 15246.835. So the deficit is not the
front failing to arrive; 29.4% of the source age never entered the model.

The decisive next measurement was **not** more age instrumentation. A
CONCEN pollutant and an EXTERNAL_INFLOW age enter the same node through the
same loader shape (`q·c` and `q·age`, both rates integrated over `dt_sub`),
so putting both on one deck asks whether the age row is broken or whether
it is faithfully tracking a broken carrier:

```
rs=5  | TSS: C1=70.594 (0.7059 of 100) | age: C1=15360.957 (0.7112 of 21600)
rs=1  | TSS: C1=100.000 (1.0000)       | age: C1=21712.038 (1.0052)
```

The pollutant loses the same fraction. **The age row was never the defect.**
See §5.2 for what is.

Gate 2 now runs at `ROUTING_STEP 1`, where the shift is **exactly
21600.000** — so the band tightened from ±1080 s (5%) to ±5 s. A 5% band
passed nothing here; it *failed* a run that was 29% short, but it would
equally have passed a run that was 4% short for a real reason.

Gate 1's `±40%` band was tightened to `±12%`. The residence-time theorem
gives mean outlet age = V/Q exactly at steady state; measured 0.9426 (the
5.7% is cell discretization plus the volume-weighted publish, stable across
ROUTING_STEP and cell length). The delivered band admitted 0.6 — wide enough
to pass a deck whose aging stage ran at half rate.

### 5.2 The finding this phase did not go looking for: ARD loses (and manufactures) external-inflow mass above ROUTING_STEP 2

Steady `CONCEN 100 mg/L` at J0, no decay, so every element reads 100.00 when
transport is exact:

| ROUTING_STEP | 1 | 2 | 3 | 4 | 5 | 10 | 20 |
|---|---|---|---|---|---|---|---|
| ARD, WATER_AGE **OFF** — C1 | 100.000 | 100.000 | 97.316 | 78.697 | **70.594** | 58.539 | 53.933 |
| ARD, WATER_AGE **ON** — C1 | 100.000 | 100.000 | 97.316 | 78.697 | **70.594** | 58.539 | 53.933 |
| ARD — C5 | 100.00 | 100.00 | 97.32 | 78.70 | 70.59 | **502.95** | **7730.23** |
| **LEGACY** — C1 | 100.000 | 100.000 | 100.000 | 100.000 | 100.000 | 100.000 | 100.000 |

Three things this establishes:

1. **It predates A1a entirely.** The OFF and ON columns are identical to
   every printed digit. Every A1a branch is gated on `ctx.options.water_age`.
2. **It is an ARD defect, not deck physics.** LEGACY delivers 100.000 at
   every step on the same deck. Running the reference engine separated
   "the model really is 70.594" from "this engine gets it wrong" in one run
   — the same move that settled E5b's treatment gate.
3. **Above rs≈10 it stops losing mass and starts creating it**: C5 reads
   **7730 mg/L from a 100 mg/L inflow**, 77×. A quality result that is 77×
   its own source is worse than a wrong number; it is a number no reviewer
   would trust the engine after seeing.

Not driven by the mesh: `FV_CELL_LENGTH` 25 vs 250 (20 cells vs 4) gives
70.594 either way, so this is not CFL subcycling or spatial resolution. It
is the routing step alone. Leading suspect is the node-store path in
`ArdEngine::step`/`substep` — the resync at lines 802–832 zeroes or scales
store mass against `ctx.nodes.volume` while `substep` adds
`load_frac · qual_vol_in` volume and `dt_sub · qual_mass_in` mass, and the
face flux at stage 3 reads the store from before stage 4's load. **Not
diagnosed further and NOT fixed** — it is outside A1a's changeset, it moves
every existing EULERIAN_ARD result, and it needs its own parity round
against LEGACY. Recorded here as the highest-value item found this phase.

Consequence accepted for now: the A1a gate deck pins `ROUTING_STEP 1` with a
comment saying why, so the gates measure the age row rather than this. The
pin is load-bearing; raising it silently re-breaks gate 2.

### 5.3 A config typo crashed the engine, and the TIMESERIES deferral was unreachable

`WaterAgeComponent.cpp` bound the value token **before** checking the row
had that column:

```cpp
const bool has_name = (scope == "NODE");
const std::string& vtok = toks[has_name ? 3 : 2];   // toks.size() >= 3 only
if ((has_name && toks.size() != 4) || ...)          // the check comes AFTER
```

`DWF NODE J0` — a NODE row with the age omitted — reads `toks[3]` on a
3-element vector. Rebuilt under ASan with the delivered ordering and run:

```
libc++abi: terminating due to uncaught exception of type
           std::length_error: basic_string          (exit -6, SIGABRT)
```

A hard abort, not a silent misread, on a plausible hand-edit of a config
file.

The same ordering made the **TIMESERIES deferral unreachable in the only
spelling anyone would write.** Plan §2 documents
`EXTERNAL_INFLOW NODE N4 TIMESERIES age_ts` — the series name makes the row
one column wider than a constant row, so the arity test rejected it as
"malformed row" before the deferral could name its phase. The deferral was
reachable only for a bare, undocumented `... TIMESERIES` with no name. The
delivered gate 5 case used `GW GLOBAL TIMESERIES ts1`, which is also 4
tokens, which is why gate 5 failed on arrival.

Fixed by binding the token only after a bounds check and testing TIMESERIES
before exact arity. Two gate-5 cases added: the documented TIMESERIES
spelling, and the 3-token NODE row.

### 5.4 Saving a water-age model silently turned it off — scope extended by one pre-existing key

`InpWriter` emits neither `WATER_AGE` nor `QUALITY_SOLVER`. A saved
EULERIAN_ARD + WATER_AGE model reopened as **LEGACY with age tracking off**,
with no warning. The handoff recorded both as pre-existing and deferred to
"IO-later", but only `QUALITY_SOLVER` is pre-existing — **`WATER_AGE` is a
key this changeset introduces**, so the changeset creates half of this
data-loss path itself.

Both are now written, using the `IGNORE_2D` convention already in that
function: an OpenSWMM extension key is emitted **only when set**, so no
existing model's `[OPTIONS]` block moves and no round-trip baseline shifts.
They are written **together deliberately** — `WATER_AGE ON` without its
`QUALITY_SOLVER EULERIAN_ARD` line produces a deck that opens with the
"no age is tracked this simulation" warning, which is worse than dropping
both. This is a deliberate one-key extension beyond the changeset and is
flagged as such rather than folded in quietly.

Gate 7 (added) round-trips through the writer and **re-opens the result**,
asserting on the reopened `options` rather than on the file text — a key
spelled in a way the parser rejects would pass a text assertion.

### 5.5 Falsifier sweep — 11 cases, all caught

| # | falsification | gates that failed | predicted |
|---|---|---|---|
| i | comment the aging stage in `ArdEngine::step` | 1, 3, 4 | 1, 3 ✓ |
| ii | comment `addAgeVolume` in `addExtInflowLoads` | 2 | 2 ✓ |
| iii | restore the `ns_total` **equality** guard | 4 | 4 ✓ |
| iv | drop the `loadersNeeded` water_age term | 2 | **1 — wrong** |
| v | drop the hours→seconds ×3600 | 2, 3 | 2 ✓ |
| vi | drop the WATER_AGE-OFF bypass warning | 6 | 6 ✓ |
| vii | skip the **link** publish age routing | 1, 2, 3, 4 | 1/3 ✓ |
| viii | skip the **node** publish age routing | 3 | *added* |
| ix | restore arity-before-TIMESERIES ordering | 5 | *added* |
| x | drop the InpWriter `WATER_AGE` line | 7 | *added* |
| xi | drop the InpWriter `QUALITY_SOLVER` line | 7 | *added* |

**Falsifier iv contradicts §2's claim that gate 1 observes the loader
predicate.** Gate 1 passes with the term dropped. The pure-age deck's aging
does not depend on `loadersNeeded` at all — the node store's volume comes
from the per-step resync against `ctx.nodes.volume`, not from
`qual_vol_in`. What the predicate actually gates is the *source-age
delivery*, so gate 2 is its only observer. The fix is right and still
needed; the reasoning given for it named the wrong gate.

**Falsifier viii was added because vii could not distinguish the halves.**
Neutering the link publish fails four gates; neutering the node publish
fails exactly one (gate 3's `age_node_final` assert). Without viii, a
regression in the store publish would have been covered only incidentally.

### 5.6 Prior suites, bit-identity, sanitizers

- `ctest` **138/139**. The single failure is
  `FvEngine.RefiningTheMeshConvergesTowardTheDynwaveHydrograph`, the known
  pre-existing baseline failure.
- **14/14 bit-identical** `.out` across the ten E0 hydraulics decks and the
  four E2 quality decks, against a CLI built from a `721ae60c` worktree.
  This is the load-bearing check for A1a: `age_row_ = -1` on every one of
  them, and the sweep traverses `loadersNeeded`'s new `||` term, an
  `addAgeVolume` call in each of the seven loaders, all four ArdEngine
  entry points, and the relaxed `ns_total` guard.
- ASan+UBSan, 0 findings across 54 tests: water_age (7), ard_e5b (7),
  ard_transport_bcs (10), ard_dispersion (11), reaction_ard_binding (9),
  reaction_legacy_binding (10).

### 5.7 Also verified, and open items

Checked and **clean**, no action needed:

- **Reopen hygiene.** `species_registry.clear()` runs at open before the
  age row is added, and `add()` returns −1 on a name collision, so a
  reopened model cannot accumulate duplicate `__WATER_AGE__` rows.
- **Dry-cell aging.** Cells age unconditionally, including dry ones, and a
  permanently-dry cell's age grows without bound. Its contribution on
  rewetting is `a_old · phi` with `a_old ≤ kDryArea = 1.0e-12`, so the
  artifact is ~1e-12 × age — negligible, not worth a guard.
- **Newly reachable path.** `WATER_AGE ON` is a third way into
  `stepRouting`'s quality branch, so a LEGACY deck with **no**
  `[POLLUTANTS]` now reaches `QualitySolver::execute` at `np == 0` where it
  never could before. Gate 6's LEGACY leg was strengthened from open-only
  to a full run, and additionally asserts the deferral warning is *true*
  (no age is published, not merely announced).

Open, recorded not fixed:

- **§5.2's ARD external-inflow defect** — the priority item.
- `SpeciesRegistry::transported_count()` now counts `RESERVED_AGE`, but the
  function **has no callers anywhere in `src/` or `tests/`**. The edit is
  documentation-only in A1a and no gate can observe it; falsifying it fails
  nothing. Correct for when a caller arrives.
- The registry registers `__WATER_AGE__` with units `"hours"` while
  `water_age_state` publishes **seconds**. Nothing consumes the units string
  today; plan §1 wants hours in the report, so this is a trap for whoever
  wires reporting.
- `SimulationContext::reset()` still does not clear `ctx.reactions`
  (carried from E3/E4/E5a; A1a's own `water_age_config`/`_state` are
  cleared). Recorded for IO5.
- Artifacts: `tests/output/a1a_validation_2026-08-17/` — `a1a_probe*.cpp`
  and their logs, `falsifiers.sh` + per-case logs, `run_decks.sh`,
  `bit_identity.log`, `asan_run.log`, `asan/oob_delivered.log`,
  `ctest_full.log`.

---

## 6. Follow-up: the §5.2 ARD node-store defect, FIXED

§5.2 recorded this as the priority item and left it. Fixed here on request.

### 6.1 Root cause — one ordering error, two symptoms

`ArdEngine::substep` read the node store's donor concentration BEFORE
mixing in what arrived that substep. Both the external load and the face
inflows were applied afterwards. That is a forward-Euler CSTR, and a
junction's own volume is far smaller than what passes through it, so it ran
past its stability bound at ordinary settings and broke in both directions.

**Loss (ROUTING_STEP 3–9).** Traced directly:

```
RESYNC nd=0 v_old=25.000 v_new=15.987 ratio=0.639 m_before=1880.29 -> 1202.40
   SUB nd=0 faceflux_out=-1818.73 dvol=-4.836 v_preload=0.000000 v=25.000
           load_vol=25.000 m=1883.68 loadmass=2500.00 conc=75.347
```

`nsub = 1`, so the whole step is one substep. The store resynced to
15.99 ft³; the faces debited 5 s × 4.836 cfs = 24.18 ft³ — more than the
store held — and `std::max(0.0, …)` clamped the volume to zero. The 25 ft³
of inflow then landed on an empty store, so it ended at 25 ft³ instead of
16.8. The mass debit stood in full. `conc = mass/volume` locked in at
75.347 and stayed there. `v_preload = 0.000000` in that trace is the
silent sink, printed.

**Manufacture (ROUTING_STEP ≥ 10).** Same ordering, further past the bound:
the store's residence time on that deck is V/q = 16/5 = 3.2 s, so
`dt·q/V` = 3.1 at rs=10 and 6.25 at rs=20. An explicit CSTR oscillates
above 1 and diverges above 2. The floor at zero clipped the negative half
of the oscillation, and the receiving cell had already been handed the full
oversized flux — mass from nothing. It compounded per junction: C1 exact,
C2 3–4×, C3 8–12×.

### 6.2 The fix — mix, then discharge

Three stages moved ahead of the donor read, so that everything arriving in
a substep is mixed in before the store decides what leaves:

- **1b** external loads (`qual_vol_in` / `qual_mass_in`, and A1a's age
  volume),
- **1b(ii)** E5a transport-boundary mass, which rides the same
  `qual_vol_in` water and must stay beside the volume it rides on,
- **1c** face inflows.

Stage 4 keeps only the outflow half. Total per-substep change is unaltered,
so the exchange stays conservative by construction.

This is not a tuning constant, it is a property: the donor becomes a
**weighted average** of what the store held and what just arrived, so it
can never exceed the larger of the two. The maximum principle holds at any
step size with no extra substeps. It also cannot drive the store negative —
the demand `dt·Qout·M/(V0 + Qin·dt)` stays below `M` whenever
`Qin ≥ Qout`, which is every junction not draining its own storage. And it
makes a zero-volume junction behave the way legacy's `findNodeQual` does:
what enters in a step leaves in that step at the mixed concentration.

### 6.3 Result

Steady 5 cfs / 100 mg/L, no decay, so every element reads 100.00:

| ROUTING_STEP | 1 | 2 | 5 | 10 | 20 | 30 | 60 | 120 |
|---|---|---|---|---|---|---|---|---|
| ARD before — C1 | 100.0 | 100.0 | 70.6 | 58.5 | 53.9 | — | — | — |
| ARD before — C5 | 100.0 | 100.0 | 70.6 | 503 | 7730 | — | — | — |
| **ARD after** | **100.000** | **100.000** | **100.000** | **100.000** | **100.000** | **100.000** | **100.000** | **100.000** |
| LEGACY | 100.000 | 100.000 | 100.000 | 100.000 | 100.000 | 100.000 | 100.000 | 100.000 |

Exact from 1 s to 120 s, and unchanged across `FV_CELL_LENGTH` 25/250.

### 6.4 Two independent confirmations that this is right

Neither is a deck written for this fix.

**E5b's own ledger deck**, which that phase recorded as "narrows but does
not close":

| term (lb) | before | after |
|---|---|---|
| External Inflow | 22.474 | 22.474 |
| External Outflow | 6.776 | 8.586 |
| Mass Reacted | 10.093 | 13.510 |
| Final Stored Mass | 2.592 | 3.675 |
| **Continuity Error (%)** | **25.048** | **0.751** |

33×. E5b's leading suspect was "the store resync scales `node_mass_` down
with volume and that mass leaves unbooked". The resync was downstream of
the real cause and is unchanged by this fix.

**A refinement study on `sdm_struct_dw_ard`**, whose reported continuity
error ROSE (2.419 → 4.573) and needed explaining. LEGACY is unusable as the
reference on that deck — it reports 9.224 lb out of a 0.080 lb inflow,
−11384% — so the dt→0 limit is the arbiter:

| ROUTING_STEP | before: outflow / err | after: outflow / err |
|---|---|---|
| 5 s | 0.078 / 2.419 | 0.077 / **4.573** |
| 2 s | 0.078 / 3.366 | 0.077 / 4.534 |
| 1 s | 0.077 / 4.424 | 0.077 / 4.546 |
| 0.5 s | 0.077 / **4.564** | 0.077 / 4.563 |

Both converge to 0.077 / ~4.56%. The fixed engine is AT the converged
answer at ROUTING_STEP 5; the old one needed 0.5 s. **The rise is the
honest number** — the old store's mass loss was partly cancelling the
ledger's known unbooked-mass hole and flattering the total. `Final Stored
Mass` reads 0.000 in both, so the 4.56% residual is that pre-existing hole,
now visible at an ordinary routing step instead of masked.

### 6.5 Gates

New suite `tests/unit/engine/test_ard_node_store.cpp` (3), each verified to
FAIL on the pre-fix engine — **0 of 3 passed** against `7c322a6c`:

1. `SteadyInflowIsTransportedExactlyAtEveryRoutingStep` — the ten steps
   above, to 1e-6. Asserts LEGACY first on the same deck, so a broken deck
   premise cannot hide behind it.
2. `NoElementEverExceedsTheInflowConcentration` — the maximum principle,
   sampled at EVERY step rather than at the end, because manufacture is a
   transient that can relax away before a run finishes.
3. `StoreStaysBoundedWhenThroughputDwarfsStoreVolume` — `dt·q/V ≈ 40`
   deliberately.

### 6.6 What else moved, and one threshold recalibrated

Full suite **139/140** — only the known pre-existing
`FvEngine.RefiningTheMeshConvergesTowardTheDynwaveHydrograph`.

Two ARD suites failed on the first pass and were resolved, not tolerated:

- `ArdTransportBcsTest.BoundaryValueFeedsInflow` / `…MsxOnlyModel`
  overshot their boundary (8.0128 against 8.0). Cause: stage 5b added the
  boundary MASS after the donor read while the VOLUME it rides on had moved
  ahead of it, so each substep read a store holding the boundary water but
  not its mass. Fixed by moving 5b beside its volume as stage 1b(ii) —
  the same invariant, applied consistently. Not a threshold change.
- `ArdDispersionEngineTest.SiUnitsDeckActivatesDispersion` — see §6.7. It
  was first re-floored from 1% to 0.2%, then replaced outright.

ASan/UBSan: **0 findings across 47 tests** in six ARD suites.

Bit-identity over the 14 E0/E2 decks: `.out` 14/14 identical. Two `.rpt`
files differ (`force_ard`, `sdm_struct_dw_ard`) — both ARD decks with
pollutant loading, i.e. exactly the decks that should move, and both
analysed above.


### 6.7 Replacing the SI dispersion gate rather than re-picking its constant

The first pass at this lowered that gate's floor from 1% to 0.2%, reasoning
from measurement: on the corrected deck the true separation is 0.657%
against 0.062% with the ucf² conversion falsified, and 0.2% is the geometric
mean. The arithmetic is fine and the gate would have worked.

It was still the wrong repair. The gate asked "did the trajectory move by at
least 1% of the base signal?" — a magnitude — and that bar had been
calibrated while an unrelated defect was diverging on its deck: the base
signal integrated to **856626** before this fix and **2224.8** after (the
same order as the US deck's 1685). The bar was 1% of a blown-up number.
Re-picking the constant from whatever the code produces next leaves the same
failure mode armed for the next person.

What the gate actually claims is that DISPERSION in m²/s means the same
physics as the equivalent number in ft²/s. It now asserts that directly, as
an **invariance**. `write_chain_deck(..., "CMS")` could not support this —
it flips FLOW_UNITS and leaves the numbers alone, so it describes a 500 m
chain carrying 5 m³/s, a different and much larger system, and a magnitude
floor was the only thing such a deck could support. The new fixture
`write_chain_deck_si()` is the EXACT metric transcription of the US deck
(1 ft = 0.3048 m and 1 ft³ = 0.028316846592 m³ are both exact, so every
number is a finite decimal). 100 ft²/s and 9.290304 m²/s are then the same
coefficient on the same model, and the two runs must agree.

Every bound is derived from the run itself:

| quantity | measured | role |
|---|---|---|
| transcription floor (dispersion OFF) | 0.0019568 mg/L | the acceptance bar |
| with dispersion, correct conversion | 0.0019841 mg/L (**1.014×** floor) | must stay under 1.5× |
| with dispersion, ucf² dropped | **266.6×** floor | must exceed 10× |

Nothing in the gate is a number taken from observed output. The pass/fail
separation is ~175×, against ~10× for the magnitude form.

Three properties the old gate did not have:

- **It fails on the defect it exists for**: 266.6× against a 1.5× bound.
- **It is indifferent to the store defect**: it PASSES unchanged against the
  pre-fix engine, because that defect moved both unit systems identically.
  A gate that cannot be knocked over by an unrelated bug will not need
  re-calibrating after one is fixed — which is the whole complaint.
- **It cannot pass vacuously**: it asserts the two unit systems do NOT agree
  exactly with dispersion off (otherwise the SI deck is not being read as
  SI and every later comparison is empty), and it re-runs the SI side with a
  deliberately unconverted coefficient to prove the pair can observe the
  conversion at all.

The US-unit activation gate is unchanged and still carries the "dispersion
does something" claim (4.47% → 4.46% across this fix; that deck never hit
the store defect).

### 6.8 The regime the fix's argument does NOT cover — checked, and gated

§6.2's guarantee is conditional: the donor is a bounded weighted average
only while `Qin ≥ Qout`. A **storage node emptying with no inflow** is the
excluded case, so it was tested rather than assumed. Deck: 1000 ft²
FUNCTIONAL storage, `InitDepth 8 ft`, `Cinit 100 mg/L`, draining through two
conduits to a free outfall, nothing entering.

**Concentrations are sound.** Peak anywhere in the model, at every routing
step: exactly 100.000 mg/L. The maximum principle holds in this regime too.
Against the pre-fix engine the same deck gives:

| ROUTING_STEP | 1 | 5 | 20 | 60 |
|---|---|---|---|---|
| peak conc before (mg/L) | 296.8 | 4569.4 | 5553.1 | 3674.6 |
| peak conc after | 100.0 | 100.0 | 100.0 | 100.0 |

Note **296.8 at ROUTING_STEP 1** — the defect was broader than §5.2
characterised it. On a draining storage node it manufactured concentration
at *every* step size, including the ones where the steady-inflow decks were
already exact. That is why §5.2's "above ROUTING_STEP 2" describes the
symptom on one deck shape rather than the bound of the bug.

**The mass-balance LEDGER does not close here, in either engine.** Quality
continuity error on the same deck:

| ROUTING_STEP | before | after | LEGACY |
|---|---|---|---|
| 60 | −186.8 | **−2.4** | −27.2 |
| 20 | −192.2 | **−3.5** | −26.6 |
| 5 | −534.0 | **+11.3** | −9.4 |
| 1 | −2.7 | **+13.1** | −7.5 |

Two things to read here honestly. The fix is a large improvement at every
step (−534% → +11.3% at rs 5), and it beats LEGACY at 60 and 20 — but it is
**not** uniformly better: at rs 1 the old engine's −2.7% is nearer zero than
the new +13.1%. Given the old column swings from −2.7 to −534 with the sign
flipping, that is a cancellation rather than better behaviour, but it is a
real cell in the table and is not being hidden.

The residual is **not** a transport error — every concentration is bounded
and correct. It does not converge under refinement in either engine, which
is the signature of ledger terms that are never written rather than a
discretization error, and it matches the pre-existing hole already recorded
at E5b (`qual_routing_flood`/`seep` have no write sites; the storage terms
are incomplete). Closing it is a mass-balance bookkeeping job, separate
from this fix and shared with the legacy engine.

Gate 4, `DrainingStorageNodeNeverExceedsItsInitialConcentration`, asserts
the property that actually holds — the maximum principle across four
routing steps — and deliberately does NOT assert the ledger, which would
gate this suite on someone else's bug. It also asserts the deck carries
pollutant at all (`peak > 0.5·Cinit`), because a storage node with
`InitDepth 0` holds none: `initQuality` discards `Cinit` on dry elements,
which is the E3 trap, and it caught the first draft of this very deck.
Verified to fail on the pre-fix engine at all four step sizes.
