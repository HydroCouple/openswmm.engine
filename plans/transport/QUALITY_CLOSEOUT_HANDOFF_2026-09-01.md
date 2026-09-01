# Quality closeout — five bindings, three debts, one API — Handoff (2026-09-01)

**For:** the checking agent.
**Base:** `f920f675` (after H6b `89310068` + the debt batch `6264eb8a` /
`d868b2c3` / `3e87868e`).
**Standing findings:** lessons 1–217.
**Implemented syntax-only.** `g++ -fsyntax-only -std=c++20` over the real
include tree: **0 errors** in all nine changed/new C++ sources plus
`LagrangianSolver.hpp`. Nothing built or run.

```
new: src/engine/transport/components/ReactionModule/MsxLegacyTransport.hpp/.cpp
new: include/openswmm/engine/openswmm_transport.h
new: src/engine/core/openswmm_transport_impl.cpp
new: tests/unit/engine/test_quality_closeout_bindings.cpp        (5 gates)
new: tests/parity/corpus/decks/si_dw_quality.inp                 (corpus #22)
new: tests/parity/corpus/decks/steady_quality.inp                (corpus #23)
new: plans/transport/PROCESS_COMPONENT_IMPLEMENTATION_PLAN_2026-09-01.md
mod: src/engine/transport/components/HeatFluxModules/BedExchange.hpp/.cpp  (shared geometry/material seams)
mod: src/engine/quality/lard/LagrangianSolver.hpp   (applyFluxesAndBed)
mod: src/engine/transport/components/EulerianArdComponent/ArdEngine.hpp/.cpp (per-cell bed + clamp exemption)
mod: src/engine/quality/QualityRouting.cpp          (MSX transport + two-array bed solute call)
mod: src/engine/core/SWMMEngine.cpp                 (LARD treatment wiring; two warnings retired)
mod: src/engine/data/BedZoneData.hpp                (cell-resolved bed arrays)
mod: src/engine/data/ReactionData.hpp               (orphaned warn flag removed)
mod: tests/parity/MANIFEST                          (+2 decks)
mod: .gitignore  (engine AND gui)                   (plan corpus now tracked)
mod: docs/manuals/reference/quality/sections/Chapter{7,8,9}*.md, References.md
```

---

## 1. ⚠ The finding that outranks everything else here

**LARD applied no surface flux module at all, silently.** H7b (2026-08-30)
made temperature *transport* under LARD and deleted the bypass warning; it
did not make temperature *exchange*. There was no `netFluxOut` call, no
`relaxT`, no `updateSolarForcing` anywhere on the LARD path — a deck with
`SURFACE_EXCHANGE ON` or `RADIATIVE_EXCHANGE ON` under
`QUALITY_SOLVER LAGRANGIAN` advected its initial temperature to the outfall
and exchanged nothing with the atmosphere, **and no warning said so.**

This violates the E1-era rule twice: a silent no-result configuration, and a
deleted warning whose condition had only half been retired. The H7b record's
own gate 1 could not see it — it compares LARD against LEGACY on a deck with
**no flux modules configured**, where both correctly do nothing.

`applyFluxesAndBed` (LagrangianSolver.hpp) is the fix. Gate 1 of the new test
file is its observer and **it is the gate to run first**.

**Lesson candidate (218): retiring a bypass warning is not the same as
retiring the bypass.** H7b retired the warning for the capability it landed
(transport) but the warning's text covered a second capability (exchange)
that had never been present. When a warning names more than one thing, the
round that deletes it owes an observer for every thing it named.

## 2. What each change retires, and the gate that proves it

| # | Was | Now | Gate |
|---|---|---|---|
| 1 | LARD applied no flux module (silently) | `applyFluxesAndBed`, once per routing step | `LardAppliesSurfaceFluxes` |
| 2 | Bed declined under ARD/LARD (warned) | ARD per **cell**; LARD per link vs the parcel mean | `BedBindsUnderArd` |
| 3 | Treatment declined under LARD (warned) | `applyTreatment` after `lard_.step` | `TreatmentRemovesUnderLard` |
| 4 | MSX state did not advect under LEGACY (warned) | `routeLegacyMsx`, the CSTR mirror family | `MsxSpeciesAdvectUnderLegacy` |
| 5 | ARD floored temperature at 0 °C (debt 216) | temp row exempt from the store floor | `SubZeroTemperatureSurvivesArdTransport` |

Every gate asserts the **result** of the retired limitation, never the
absence of its warning — a deleted warning over unchanged behaviour is
exactly what §1 was.

**Three warnings are now gone WITH their conditions**, not narrowed: the LARD
treatment bypass, the H6b engine-gating warning, and R4b's
not-transported warning (its once-per-run flag `warned_msx_not_transported`
was deleted as orphaned — **grep for it before trusting that**, I removed it
and its two neighbouring state arrays in one cut and had to restore the
arrays; see §5).

**With this, no quality configuration is silently bypassed under any engine.**
That claim is worth a census: grep `SWMMEngine.cpp` for every remaining
`warnings.push_back` on a solver condition and confirm each names something
that genuinely does not run.

## 3. The design calls, so you can disagree with them in one place

**D-BED-ARD — the bed resolves per CELL under ARD, 1:1.** This is the
reference's own mapping (`HTSComponent` pairs one HTS element with one
channel element) and it answers H6b §4's open question — "which cell does the
bed under a 400 ft conduit exchange with?" — with "its own". Contact area per
cell is `(A/R)·dx`; barrels ride inside `area_x` via `barrel_scale` while `R`
is per-barrel, so `A_total/R` is already n × perimeter, the same identity as
the link derivation.

**D-BED-LARD — the bed stays per LINK under LARD, well-mixed.** A per-parcel
bed would have to move with the water and a bed does not move. The exchange
is solved against the volume-weighted parcel mean and the increment applied
**uniformly** to every parcel. Uniform is exact for the flux linearization
(each parcel's surface share is proportional to its volume, so
`k = A·J′/(ρ·cp·V)` is identical across parcels) and it preserves the
along-link profile, which is what the engine exists for.

**D-MSX-EVAP — `routeLegacyMsx` has no evaporation factor**, matching the age
and heat mirrors rather than the pollutant path. Recorded consequence: during
evaporation an MSX concentration does not up-concentrate. The pollutant
path's own evap factor was found *creating* mass at draining nodes (KD1), so
the omission is the safer side of a defect that is still open there.

**D-E6-READONLY — boundary/source rows are read-only in the C API.** They
resolve names to indices once, at open (`resolveArdTransportRows`), with
failures fatal there where the row text is available. A runtime add needs
that resolution re-entrant and its failures survivable — its own round.
Editors read here and write through the component file.

## 4. Validation protocol

1. **Every gate must FAIL at base.** Revert the source hunks, keep the test
   file. Expected base readings, to quote:
   - gate 1: LARD outlet at exactly the 5 °C initial state while the LEGACY
     control warms;
   - gate 2: ARD outlet at exactly 20 °C (the decline);
   - gate 3: treated and untreated twins agree exactly;
   - gate 4: `msx_link_conc[2]` exactly 0 and `[0]` still at its 80.0 seed;
   - gate 5: outlet at exactly 0 °C (the clamp).
   **Several of these are exact-zero or exact-initial readings, which is what
   makes them quotable.** If any base reading is merely *close*, the deck is
   not discriminating — say so rather than proceeding.
2. `ctest -j8` ×3. Standing figure is **185**; this adds 5 → **190**.
   **Watch the reaction suites**: `routeLegacyMsx` now moves state that
   previously sat still, so any LEGACY MSX gate asserting a stationary
   concentration was asserting the defect and its failure is a FINDING about
   that gate, not about this change.
3. **Corpus 23/23** `.out` and `.rpt`. The two new decks have no base
   counterpart — run them under the patched binary only and record their
   figures as the new baseline entries. The other 21 must be **byte-identical**:
   no corpus deck sets `SEDIMENT_EXCHANGE`, none configures treatment under
   LARD, none carries MSX under LEGACY. **A movement in the existing 21 is a
   defect in the OFF path**, most plausibly the ARD cell-loop restructure
   (`is_open` moved from gating the whole iteration to gating the area only)
   or the temp-row clamp exemption.
4. **Falsifier sweep:**

   | falsifier | expected |
   |---|---|
   | i. remove the `applyFluxesAndBed` call | gate 1 fails alone |
   | ii. make the LARD link increment non-uniform (apply `d_tw` to parcel 0 only) | gate 1's band-vs-LEGACY leg fails; the "warmed at all" leg still passes — **the band is what tests the uniform-delta claim** |
   | iii. in the ARD cell loop, restore `if (!g.is_open) continue;` | gate 2 fails on a closed-conduit deck; **predict first** whether the open-channel decks move |
   | iv. drop the `s == temp_row_` exemption in ONE of the two clamp sites | gate 5 must still fail — if it passes with one site restored, the other site is unreachable and one of them is dead code |
   | v. skip the second `applyBedSoluteExchange` call (the MSX offset one) | a bed+MSX deck loses MSX transient storage while pollutants keep theirs — **no gate covers this today; it is owed** |
   | vi. `swmm_transport_set_dispersion_value(-1)` | returns `SWMM_ERR_BADPARAM`, config unchanged |

5. **Record:** step 1's five base readings verbatim, falsifiers ii and iii
   (iii's prediction is the one I am least sure of), the corpus answer, and
   whether any reaction suite moved under step 2.

## 5. Two mistakes I made in this batch, both caught here

**I cut live state while deleting an orphaned flag.** Removing
`warned_msx_not_transported` from `ReactionData.hpp` by "the flag and its
comment block" also removed `msx_node_conc` and `msx_link_conc` — the two
arrays the whole R4 subsystem stores its state in. Three files failed to
compile immediately, which is the only reason it was caught in seconds rather
than by a gate. **The comment block above a field is not a reliable
delimiter.** Restored with a corrected comment.

**I wrote the LARD method against an anchor that had already changed.** Two
scripted insertions raced: the include and call-site edits applied, the
method body did not, and the file was left calling a function that did not
exist. Caught by the compile check. Both are arguments for compiling after
every scripted edit rather than at the end of a batch.

## 6. Still owed — specs, not hand-waving

**E2b — storage mixing beyond CMSTR (1–2 rounds).** `[STORAGE]` gains a
mixing token: `MIXED` (today's CSTR, default), `TWO_COMPARTMENT`
(active/dead fractions with an exchange coefficient), `FIFO`, `LIFO`. The
LARD segment store is the natural substrate for FIFO/LIFO — it already holds
an ordered slab — so the honest sequencing is: implement in LARD first, then
decide whether LEGACY/ARD get an approximation or refuse the token by name.
**KD1's residual rides here:** transit-mass decay escapes volume-basis
booking in BOTH engines (legacy leaks 6.5–7 % on a storage deck), and the
storage-mixing round is where the ledger has to be rebuilt anyway.

**P2.5 — Python + MCP age surfaces (1 round).** `openswmm_water_age.h` and
now `openswmm_transport.h` both exist; the Python `OutputReader` binds
`swmm_output_get_pollut_id` already. The work is ctypes signatures + MCP tool
wrappers, mechanical, following the heat binding's shape.

**A2c — the age-volume ledger row (needs a definition FIRST).** The `.rpt`
continuity table is mass-shaped; age is neither a mass nor conserved (it
grows 1 s/s everywhere). Three candidate definitions, and this is a
reporting-contract call, not a coding preference:
(a) age-volume inflow/outflow/storage with the +1 s/s growth as an explicit
"generation" row; (b) a separate table entirely; (c) no row, and the
continuity table documents its own silence about age. **Do not implement
until one is chosen.**

**The dry-element temperature sentinel.** A dry element reports its carried
temperature indefinitely because 0 °C is a real value and there is no
no-data marker in the `.out`. This needs a per-column sentinel in the output
format — a format change, so it needs its own round and a reader-compat
story.

**The 2D corpus hole persists.** `trackI_bitwise_regression.sh` (32 decks)
covers 2D and is still not wired into `run_corpus.sh`. Wiring it is small;
the reason to do it before Phase 3 opens is that Phase 3's first round will
otherwise be the one that discovers the runner does not cover its own work.

**Phase 2 (HydroCouple)** now has a real plan:
`PROCESS_COMPONENT_IMPLEMENTATION_PLAN_2026-09-01.md`. Its headline decision
is D-PC1 — openswmm ships **as** a component before it **hosts** components,
because Direction A produces coupled results without designing a hosting ABI,
and its wrapper is Direction B's test double. HC4.2 (openswmm ⇄ HTSComponent
on one network) is the parity instrument H6b could not have: the reference
running live rather than transcribed, with divergences 1–3 predicting exactly
where the two will disagree.

---

## CHECK RECORD (checking agent, 2026-09-01)

**VERDICT: VALIDATED AND COMMITTED — engine `1736593d`** (22 files,
+1719/−129) + `3086e6b4` (plan corpus tracked, 112 files) + `eed20e83`
(manual chapters 7–9). GUI: `25a3c0c` (workplans tracked). Evidence:
`tests/output/quality_closeout/` (PROVENANCE.txt has the full defect
ledger and falsifier record).

**The changeset needed seven fixes before it validated** — three
fixture defects (no air source; no wind; all-closed conduits: zero
exchange was CORRECT physics on gate 1's own deck), two REAL design
defects (the LARD treatment wiring was a no-op twice over — fixed with
the shared per-node seam `quality::applyNodeTreatment` inside the MIX;
`loadersNeeded` starving routeLegacyMsx on MSX-only decks — the fourth
member of the A1a/H1 volume-half family), one REGRESSION (the naked
temp-floor exemption let the near-dry store oscillation run to ±1e62 —
caught by the ARD-relax round's thin-sheet gate, contained by clamping
to the parser's own [−50, 100] band), and one ORDER error (transport
must run BEFORE react: FORMULA is the last writer, and the tracking
gate drifted 4.3e-3 under the draft's order). Full detail in the
PROVENANCE.

**Base readings, verbatim (natively compiled — the DYLD-dylib override
is ABI-unsound for a changeset that moves SimulationContext's layout):**
5.0000000000000062 · 19.999999999999844 · 99.999999999998977 ·
{C3 = 0, C1 = 80 exactly} · +1.3115119056826916e-115.

**Figures:** ctest **186/186 ×3** (the standing figure counts binaries:
185+1, not §4's "+5 → 190"); corpus **23/23 byte-identical** — the two
new decks run at base too, so they landed **A/B-pinned**, not
baseline-only as §4 assumed. Falsifiers: i, iii (predicted exactly),
iv (both clamp sites live), vi bite; ii bites HARDER than predicted
(both gate-1 legs); v confirms the owed MSX-bed gate.

**§2's census ran:** every remaining solver-conditioned warning in
SWMMEngine.cpp is the inverse-bypass family or the announced ARD
failed-mesh fallback. The headline claim stands: **no quality
configuration is silently bypassed under any engine.**

**Owed from this check:** the MSX-half bed gate (falsifier v); Chapter
7's transient-storage note (drop-in text in
MANUAL_BED_ZONE_SECTION_DROPIN_2026-09-01.md); lesson candidate 218
CONFIRMED and numbered in the roadmap (with 219–221 from this check).
