# Unified Transport Program — Progress Report

**As of:** 2026-08-22 · **HEAD:** `55a70839` (unpushed against
`hydrocouple/swmm6_rel`) · **Program start:** 2026-08-15 (planning) /
2026-08-16 (first code commit `08e7900a`)

**What this document is:** a readable snapshot of the work list and what has
been accomplished, across all five phases and the GUI track. It is derived
from `IMPLEMENTATION_ROADMAP.md`, which remains the authority — the roadmap
carries the full validation detail per step, this file carries the shape.

Status legend: ✅ landed and validated · 🔄 in validation · ⬜ not started ·
⛔ blocked.

---

## 1. At a glance

| Phase | Scope | Status | Steps landed |
|---|---|---|---|
| **1 — 1D transport** | ARD engine, MSX reactions, water age, heat, I/O config | **ACTIVE** | **~40 of ~45** |
| 2 — HydroCouple | `IModelComponent` wrapper, Composer coupling | ⛔ **refused, not merely unstarted** | 0 of 4 |
| 3 — 2D surface transport | tracer advection on the marcher, 1D↔2D coupling | ⬜ not started | 0 of 7 (`2D-S1…2D-S7`) |
| 4 — Groundwater | two-zone explicit-LTS | **G0 ✅ signed off 2026-08-25**; G1 ready to start | 0 of 4 code |
| **5 — LARD (Lagrangian)** | segment store, LTD advection, RWPT dispersion, age | **MOSTLY DELIVERED** | **L0, L1–L2, L4 ✅ · L5–L7 ◐ · L3 ⬜** |
| GUI track | transport/quality editors, 2D+GW visualization | **PARTIAL** | **G1g, G2g, G3g ✅ · G5g ◐ · G4g, G6g, G7g ⬜** |

> **⚠ This table was wrong in both directions until 2026-08-25**, and every
> planning decision in the preceding stretch was taken against it. Phase 5 read
> "not started, 0 of 4" while `SegmentStore.hpp` (288 lines),
> `LagrangianSolver.hpp` (576), `RwptDispersion.hpp` (356) and 2,213 lines of
> LARD test suites were in the tree; the GUI read "0 of 14" with an 1150-line
> reaction editor shipped. Meanwhile Phase 2 read as ordinary future work when
> the code **actively rejects** a library-backed process component.
>
> The cause: this file was written from one session's line of sight and never
> learned about the **X / Y / Z / closeout tracks** that landed 2026-08-23 →
> 08-25 in parallel sessions — which until 2026-08-25 appeared **nowhere in
> `IMPLEMENTATION_ROADMAP.md` either** (`grep X4|Y0|Z1` → zero hits). Full
> verification, against the code rather than the documents:
> **`PROGRAM_REVIEW_2026-08-25.md`**.

**✅ MEASURED 2026-08-26 — the lower bounds below are retired.** The gap open
since 2026-08-22 ("someone runs `ctest -N` and `wc` and reconciles") is closed;
figures are from `build/darwin` at `a0dbc04b`. Method and caveats:
`CHECK_RECONCILIATION_G0_2026-08-26.md` §8.

| quantity | measured | previously claimed |
|---|---|---|
| registered ctest tests | **177** (175 `unit`, 1 `regression`, 1 unlabelled) | 160 |
| gtest gates, tree-wide | **2,786** across **317** test source files | — |
| test LOC, tree-wide | **296,677** | — |
| gates in test files added since 2026-06-01 | **976** across **119** files | "216+ across 25+" |
| test LOC, same window | **48,400** | — |
| parity corpus decks | **19** (0 are 2D) | 15 |

The program-era figures are **4.5× the gate count and 4.8× the file count**
previously carried. **60+ commits** stands (not re-derived).

Standing verification each round: full C++ suite — **177/177 as of 2026-08-26**
(Track I's 2D infiltration failure was fixed by `002b8c0a`; the suite now has
no standing failure, though `test_engine_report_timing` flakes intermittently
under `-j8` and passes standalone) — reference decks byte-identical where the
change is meant to be inert (corpus in tree since `d633c53e`, ~100 s for a full
before/after run), and ASan/UBSan clean.

**⚠ The corpus contains no 2D deck.** A bitwise 2D gate exists as a standalone
script (`tests/scripts/trackI_bitwise_regression.sh`, 32 qualifying decks
discovered dynamically) but is not wired into `run_corpus.sh` — so "corpus
green" says nothing about the surface solver.

**⚠ That 159/160 is a `-j8` number now, and it was not stable until
`97bfa512`'s round.** `test_engine_heat_watershed` and `test_engine_heat_lid`
both wrote `_h5b.inp`/`_h5b.heat` into the shared `data/` working directory,
so under parallelism one suite parsed the other's deck. **It was caught by a
real `ctest -j8`** — that is the citation that counts, and the deliberate
"8 failures in 8" reproduction that got quoted first is the weaker of the
two. The failure presents as `SURFACE_EXCHANGE did not parse`, a content
error, which is why the sibling instance was written off as a flake a round
earlier; that string can only come from the LID suite's config. Both suites
passed alone the whole time. A sweep of all
**865 distinct fixture literals across 147 `test_*.cpp`** then found a
second, unlocked instance — `_out.inp`, shared by `test_object_deletion_ext`
and `test_quality_roundtrip`. Both renamed (`b85b802d`), and a configure-time
check now refuses a third.

**The second one is prophylactic, and saying otherwise was an overclaim.**
Run the way ctest runs — one copy of each, 12 paired rounds — `_out.inp`
gives **0 failures in 24**. The "8 failures in 16" that made it look live came
from a stress harness, and 8 copies of `test_engine_object_deletion_ext`
*alone* fail 4 of 8 on an unrelated assertion: **that suite races itself.**
Harmless under ctest, recorded, not fixed. The `_h5b` finding is untouched —
it came from a real `ctest -j8` before any harness existed.

**The snow track closed its continuity ledger (`0ad28685`).** Runoff
continuity on a snow deck moved **−8.193 % → +0.407 %**, and the residual is
now *explained* rather than absorbed: `Snow Removed` of 0.122 in lands on the
one subcatchment with `Fout = 0.20`, and the reported pack carries its free
water (0.340 in) rather than SWE alone (0.323). A hand reconciliation and the
ledger agree to the third decimal — **two independent routes, which is what
retires an "unexplained loss"**, not the smaller number by itself.
**14/14 decks are byte-identical in BOTH `.out` and `.rpt`** — the first
round to check the `.rpt`, which is precisely where a report-row change
would show.

**The long-standing FV mesh-convergence failure is GONE (lesson 89).** It had
been carried in every handoff for six rounds as the expected standing
failure; at `c292b8eb` it passes at base, because a foreign commit rewrote
`test_fv_engine_integration.cpp`. A standing exemption has to be
re-verified, not inherited — this one outlived its cause by an unknown
number of rounds.

**A caveat on the suite figure, established at `5b2b7418` (lesson 71):** a
count from the **main tree** is not attributable. The A4 implementation round
reported 150/150 from the main tree with foreign edits present; the identical
changeset in an **isolated worktree** gives 149/150, the bistable FV gate
failing to the digit and matching what the A3 round measured at its own base.
The foreign edits had been **masking** a pre-existing failure, not causing
one. Counts in this document are the isolated-worktree numbers where the
round produced them; earlier rounds' main-tree numbers should be read with
this in mind.

**⚠ Two stacked gaps found at `d7ee70be`, and the first is an ENGINE defect
older than this program.** `SnowSolver::setMeltCoeffs` had **no caller
anywhere in `src/engine/`**, so `dhm` stayed zero and **degree-day snowmelt
had never fired** — only rain-on-snow, which needs 0.02 in/hr. `test_snow.cpp`
has 35 gates and every one of them calls `setMeltCoeffs` itself before
stepping, so the function was exercised and the engine's *use* of it never
was. Fixed separately as `274b6506`.

That is also why the transport half survived four phases: no gate used a
`[SNOWPACKS]` deck, **and** even with one there would have been no melt to
expose it. Either gap alone would have kept it invisible.

**⚠ The corpus was not in the repository, and nobody checked for fifty
rounds (`d633c53e`).** Every handoff has reported "14/14 reference decks
byte-identical". **`git ls-tree -r HEAD tests/output` returns zero** — the
decks lived in two rounds' scratch directories from August 16, and the runner
existed as ten-plus copies of `run_decks.sh`, each hardcoding the repo root.
Now `tests/parity/`: 15 decks, a `MANIFEST` recording what each one reaches,
one `run_corpus.sh`, and a `git archive HEAD | tar -x` clean-clone check that
runs 15/15. **The snow deck is entry 15**, closing the register §4 item.

Three corrections came out of validating it, and two were mine:

- **`openswmm` is a thin driver over the engine dylib**, so an engine-only
  changeset leaves the CLI **byte-identical** — my runner hashed the CLI, so
  its "this run cannot fail" guard was silent in exactly the case it exists
  for. It hashes the resolved *library* now. Two binaries means two build
  **directories**.
- **The snow deck costs 0.30 s, not "~15–24 s".** ~50× out, in the direction
  nobody guessed — it is among the *fastest* decks. The three `sdm_fv_*`
  decks are **96 % of the corpus** (47.1 s of 49.3 s). The step count was
  right; the wall time had never been measured on a CLI build, and came from
  the same stale-library era O4 traced.
- **Release vs Debug, same source: 15/15 identical.** The matching-build-type
  warning stays as prudence, now labelled as such.

**⛔ THE NEW DECKS FOUND TWO ENGINE DEFECTS ON THEIR FIRST RUN (`1da1d7ca`).**

**(1) The runoff ledger double-counts cascaded run-on — ✅ FIXED `421e95c2`,
landing on legacy's number exactly.**
Same hydrology, two engines: precipitation 6.960 in and infiltration 4.620 in
agree to the digit, but **surface runoff reads 3.976 in against legacy's
2.348 in** and runoff continuity **−23.667 % against −0.271 %**. The gap is
exactly S1's own 1.628 in — booked once when S1 sheds it, again when S2
discharges it. `SWMMEngine.cpp:2357` added every subcatchment unconditionally;
legacy guards it at `subcatch.c:761-765`. **Not one of the fifteen previous
corpus decks routes a subcatchment onto another subcatchment**, and cascading
is ordinary in real models. Measured after the fix: **−0.271 %, legacy 5.x's
figure to three decimals**, with **18/18 `.out` byte-identical** — the
ledger-only claim verified rather than argued.

**(3) Self-routed subcatchments recirculate their own runoff — ✅ FIXED
`69467241`, correct as delivered.** Found while writing the falsifier fixture for (1).
A self-routed subcatchment books **2.328 in against legacy's 0.417** — 5.6×,
**−265 % continuity** — while direct, 2-deep, 3-deep and all-direct cascades
all match legacy to the digit. Legacy carries `!= subcatchIndex` in **three**
places — run-on distribution (`subcatch.c:546-548`), the ledger (`763`), and
washoff (`surfqual.c:363`) — and `421e95c2` gave us only the ledger. So the
water genuinely recirculates and the ledger faithfully reports a hydrology
that is already wrong (lesson 142). The fix closes both remaining sites, and
the run-on guard covers **three seams at once** — `addRunonAge` and
`addRunonTemperature` ride the same branch, so a self-route was feeding its
own age and its own heat back to itself too. **First of the three findings
that changes routed water.** The gate asserts **equality** with a directly
connected subcatchment rather than inequality with the broken value: a
self-route is a no-op in legacy's model, so equality is the real claim. After
the fix `selfroute` joins the other four decks at legacy's **0.417 acre-feet**
(in inches 2.794 → 0.500, continuity **−265.245 % → −1.160 %**), and the whole
Runoff Quantity block diffs clean against legacy on all five.

**(4) The node injection double-counts run-on — ✅ FIXED `55a70839`, right as
delivered, and the biggest of the four.** `SWMMEngine.cpp:2183` feeds the node `q_runoff + q_runon`;
legacy's `subcatch_getWtdOutflow` returns **runoff alone**, because run-on is
already inside the receiver's `newRunoff`. Cascade: legacy 0.218 against our
**0.511**; three-deep 0.318 against **0.536**. The excess is 0.293 acre-feet
against the donor's own 0.294. **Our conveyance receives 2.3× what our own
runoff ledger says left the surface, and neither continuity check notices** —
two self-consistent balances, each certifying the other (lesson 147).

**The per-contributor question resolves uniformly, and structurally.**
`assembleRunon` sums every contributor — cascade, LID drain, outfall return —
into **one** `runon_inflow[]`, and `Runoff.cpp:333` consumes it **wholesale**:
the solver cannot distinguish contributors, it reads one lump. So no
contributor makes the node addition legitimate. The gate asserts
**correspondence across the seam** (`ROUTING_WET_WEATHER == RUNOFF_RUNOFF` on
a deck where nothing can be lost between them) rather than adding a third
balance — both existing balances closed while the defect was live, which is
precisely why a third would have been useless.

All five fixtures now match legacy (cascade 0.511→**0.218**, three_deep
0.536→**0.318**), and the corpus moved **15/18** — exactly the three transport
decks, with the config guard silent on its first real round, so the movement
is attributable. **The two contributors I could only reason about are now
measured**: LID drain 1.9412→1.0000 and outfall return 2.0759→0.9986, both
landing on legacy. **The outfall case was manufacturing water, not just
mis-booking it** — 10.020 → 0.705 acre-feet against legacy's 0.779, because
the doubled node inflow fed the outfall that fed it back (lesson 150).

**My gate's tolerance was below the achievable floor** and failed at base and
after. The two totals integrate on different clocks; the floor was *measured*
by sweeping the wet step (5.1e-5 → 2.2e-6, monotone: quadrature, not a leak)
and the assertion is now a ratio rather than a constant — cascade error ≤ 10×
the control's, which reads 1.5× fixed and **26175.9×** broken (lesson 149).

**🔄 (5), (8), (9), (10), (11) — the quality-ledger units cluster — fix in
validation (`QUALITY_LEDGER_UNITS_FIX_HANDOFF_2026-08-23.md`).** The audit's
known-mass deck refuted my first trace: the washoff accumulator mixed THREE
unit systems, and **both** printed readings were wrong — ledger 16057× legacy
(1,815,717 against 113.082 lbs), summary 28× under. The fix is legacy's
design end to end: one internal convention, `mcf` applied once at every
ledger booking, summary prints raw, EXPON's per-hour coefficient finally
`/3600`, `qual_bmp_removal` written for the first time, the continuity error
gets legacy's missing third branch (mass leaving an empty system no longer
prints 0.000), and the **vendored legacy control** (`landuse.c:633`, `>=`
since `03ed283a` where stock EPA has `==`) restored — under it any land use
with a buildup function washed off nothing, corrupting the reference every
quality comparison rides on. **(6) needed no fix** — legacy's 0.000 was
correct; the deck lacked an antecedent dry period.

**The superseded first reading, kept for the record
(`QUALITY_LEDGER_UNITS_AUDIT_2026-08-22.md`):** Our washoff mass is in **mg**;
legacy applies `mcf` at source so its is in **lbs**. **So Finding 5 inverts:
the summary is correct and the ledger row is wrong** — my first instinct
would have "fixed" the one right reading in the report. Finding 6 partly
dissolves (our washoff 1.369 mg ≈ 3.0e-6 lbs against legacy's 0.000) and
partly does not (buildup legacy **0.885 lbs** against our 2.500), which points
at the ledger **mixing units across its own terms**. And **(8):
`qual_bmp_removal` has ZERO write sites** — declared, resized, enumerated,
written nowhere, with a report row rendered from it. **Fourth instance of F8's
family.**

**⛔ (7) The LID deck sheds 34× legacy's water** — 15.482 against 0.456
acre-feet, unchanged by (4)'s fix. The issue #131 unit family, surfacing on
the first deck ever built to exercise `DrainTo`, and the measurement that
justifies deferring the LID corpus deck.

**⛔ (5) and (6), both blocking cross-engine quality comparison and both
predating everything above.** The Subcatchment Washoff Summary divides by
**453592** while the ledger row prints the same mass variable raw — ratio
measured at exactly 453592.0 — so it prints **0.000 on every ordinary deck**.
And buildup/washoff already diverge from legacy on the simplest deck: legacy
**0.885 / 0.000** against our **2.500 / 1.369**. Unscoped.

**⛔ A defect in my own corpus runner, found the same round and fixed.** A run
reported **four moved decks — precisely the quality decks, during a
washoff-guard round**, which is exactly what the defect under test would look
like. It was two build directories with different CMake options. The runner's
vacuity guard **pointed the wrong way**: it warned when both sides were the
*same* build, when the dangerous direction is two secretly *different* ones.
It now diffs the build configurations and says so before the deck table
(lesson 146) — and the first real run showed that filter was too **wide**,
not too narrow: version strings drift between any two build directories, so
the guard would have fired on every honest before/after, which is the same
failure one level up (lesson 148).

**(2) The subcatchment `__TEMPERATURE__` column is never written — ✅ FIXED
`29cbc361`. The writer was right; both my gates were wrong.** Nodes and links carry live temperature; **every subcatchment
reads exactly 0.0 all run.** There is an age writer at
`SWMMEngine.cpp:4645` and no temperature sibling, so the column keeps its
`assign(…, 0.0)`. Third instance of F8's family, and the deck handoff's own
column-presence check **would have passed** — only reading the values caught
it (lesson 139). After the fix the corpus moved `heat_parity` by **2003 of
51520 bytes** against 3 subcatchments × 4 bytes × ~167 periods = 2004 — one
float column, exactly — and the values land where a surface should
(S1 −4.147…19.59 °C against nodes/links at −4.147…17.66: shared minimum,
subcatchment maxima slightly above the piped water).

**Both gates I wrote for it were defective, and the second is the worse
mistake.** Both misread `swmm_output_get_subcatch_attribute`, whose second
parameter is an **object index, not an attribute code** (the reader that takes
a variable is `..._get_subcatch_result`); I copied a call's shape from another
suite without running it, and got `-1` every period. Worse, once fixed, gate 2
**passed under the falsifier it existed for**: the writer's "dry"
(`runoff != 0.0`, an exact double that essentially never turns false) and the
`.out` column's "dry" (rounds to `0.0f` early) are **different predicates**, so
all 44 sampled periods were dry to the reader and wet to the writer. Fixed by
making the fixture dry by construction rather than by waiting (lesson 143).

**🔄 The reserved-column hole is being closed — three decks in validation.**
The caveat opened while validating `b5be8ec3` and stood until now: the corpus
had one `[SNOWPACKS]` deck and **no water-age deck, no heat deck**, after
roughly fifteen rounds of building both. In validation
(`CORPUS_AGE_HEAT_DECKS_HANDOFF_2026-08-22.md`): `age_legacy` and `age_ard`,
**differing by one line** so a movement localises to shared age machinery or
to one engine's binding, both with **np = 0** — the configuration E5a found
broken in all six loaders and nothing has re-checked since; and
`heat_parity`, the only deck reaching the **`np + age + heat`** stride where
D-UT10's parallel-accumulator decision is load-bearing. Corpus 15 → 18.

**Still zero after that:** no LID under either reserved species (**blocked on
issue #131** — a deck written today bakes in pre-#131 behaviour), no heat
under ARD, no SI deck, no STEADY deck. Composition table:
`tests/parity/README.md` §4.

The honest summary: **Phase 1 is roughly half delivered and the other four
phases have not been started.** What exists is a working Eulerian
advection–reaction–dispersion engine with multispecies reactions and
end-to-end water age, on 1D existing formulations.

---

## 2. Phase 1 — what shipped

### 2.1 Eulerian ARD engine (`EULERIAN_ARD_TRANSPORT_PLAN.md`)

| # | Delivered | Commit |
|---|---|---|
| E0 | FV species kernels promoted to `transport/fvkernels/` — verbatim, bitwise | `08e7900a` |
| — | Four pre-existing quality faults found during E1 validation (INFLOWS pollutant rows, Cinit, ledger init/final, wet-weather double count) | `29f1577a` |
| E1 | Projection + `ArdEngine` tracer + `QUALITY_SOLVER` option + junction stores | `a7824b32` |
| E2 | Structure passthrough + loud CFL clamp; orifice load within 0.3 % of EPA | `04340084` |
| — | Runtime-API forced quality mass moved into the loader stage — feeds **both** engines; LEGACY forcing worked for the first time | `3f56e47a` |
| E3 | Dispersion under all solvers (per-conduit D + FISCHER, implicit per-chain) | `7684af53` |
| E4/R6 | Reaction hook on a Lie split per routing step + exact-exponential kdecay; MSX species transported on the mesh | `4df5cc0f` |
| E5a | `[TRANSPORT_BOUNDARIES]` + `[TRANSPORT_SOURCES]` + full `model.ard` option aliases | `cbb9d321` |
| E5b | Treatment interop at ARD node stores, reacted-ledger booking, CSV detail sidecar, `TARGET_DX` | `721ae60c` |

**Still open here:** E2b (storage mixing models, FV direct-cell-state, tidal
reverse-flow BC), E6 (C/Python/MCP transport API), E7 (→ Phase 2).

### 2.2 Species registry

`T0a` ✅ `756afa6e` — species kinds (POLLUTANT / RESERVED_AGE /
RESERVED_TEMPERATURE / MSX_BULK / MSX_WALL).

**`T0b` — superseded by D-UT10 (2026-08-17).** The plan called for widening
the loader tuple to `(mass, vol, age_vol, enthalpy)`; A1a shipped the age
channel as a *parallel* accumulator instead (`node_age_vol_in`, a rate
filled by the same seven loader pathways). The decision follows what
shipped: parallel per-capability accumulators, not a widened tuple — the
contract's guarantee is that every pathway contributes at the same seam, and
the seam is the loader set, not the C++ type. Age half delivered; the
enthalpy half now lands **with H1**.

### 2.3 Shared MSX reaction module

| # | Delivered | Commit |
|---|---|---|
| R1 | `ReactionSystem` registry + `[REACTION_*]` parsers in `model.rxn` + duplicate-id refusal | `756afa6e`, `9d0dbbff` |
| R2 | Expression compiler + Tier-1 VM (flat token pool, pre-resolved indices) | `352638e6` |
| R3 | Integrators (EUL/RK5/ROS2/BDF2) + EQUIL Newton + FORMULA + stiffness ladder | `a3fbc78b`, `7c2c151b`, `eca08593` |
| D-R10 | Default `SOLVER = RK5` — decided on measurement, not preference | `eca08593` |
| R4 | LEGACY `qualroute` binding: exact-exp kdecay, MSX element state, failure containment | `326b595c` |

**Still open:** R4b (MSX transport under LEGACY), R5 (reaction APIs +
authoring surface), R6's `[REACTION_SUBCATCHMENTS]` hook.

### 2.4 Water age — the most recent track, essentially complete for 1D

| # | Delivered | Commit |
|---|---|---|
| ⚠ | **ARD node-store ordering defect — "mix before discharge".** A forward-Euler CSTR read its donor *before* that substep's arrivals mixed in; ordinary junctions exceed the `dt·q/V ≤ 2` stability bound at routing step ≥ 3. Fixed conservatively; also collapsed E5b's flowing-deck continuity error 25.048 % → 0.751 % | `7b2dfaae`, `4be378de`, `fa9babba` |
| A1a | Reserved `__WATER_AGE__` species on the ARD mesh + `waterage` component + age source through all seven loader pathways | `7c322a6c` |
| A1b | LEGACY CSTR age mirror (pollutants stay bitwise identical) | `d2f003e6` |
| A2a | Age hotstart persistence — native format V3, both engines seed from loaded state | `f704b83d` |
| ⚠ | **`SimulationSnapshot`'s quality vectors had no writer anywhere** — every pollutant column in every binary `.out` was written as zero while the header advertised the columns. Engine state was correct throughout, which is why every transport gate passed over it | `957a1d62` |
| A2b | Age reports as a trailing `__WATER_AGE__` column in the `.out`, in hours | `d4889329` |
| — | `swmm_output_get_pollut_id` + `OutputReader` parses the fourth ID list the writer always emitted — the name is the only way to tell an hours column from a concentration | `06580dd6` |
| — | A dry element reports no water age (state keeps aging; the mask is at the report boundary) | `584d1065` |
| — | Python `OutputReader.pollutant_ids` + 3 gates — accepted unchanged; closes the half-bound `.pxd` surface | `d7ce8efb` |
| A3 | Subcatchment water age — per-subarea state, run-on carrying the donor's age (a defect, not a gap), A2b's placeholder column retired | `b5be8ec3` |
| A4 | LID layer age — per-layer complete-mix on inflows newly published from the LID solver; **found and fixed an A3 defect** (run-on age counted one of three contributors, giving water younger than anything entering the model) | `5b2b7418` |
| S2a | **Meltwater arrives at the freezing point**, not at the configured RAINFALL temperature. Arriving water under a pack is two different waters — melt and rain-through — and `snow_net_*` is their sum; `snow_melt_*` now publishes the split under the identical area blend. Measured: arriving **0 °C** against 20, and on a mixed deck **7.235 / 7.235 / 11.389 °C** — a per-subarea spread the single-scalar form cannot produce at all | `8b7d1cf7` |
| S1 | **The mixing volume under a snowpack is not the rainfall.** A3 and H5a read `subcatches.rainfall` (rain + snow*fall*) where the solver uses `snow_net_imperv`/`snow_net_perv` (`imelt + rain·(1 − asc)`, per subarea). Measured on a melting pack with zero rain: subarea ages read **3600 s — the elapsed run time exactly**, what a surface reads when nothing mixes in. Correct: 624.67 / 787.37 / 0 s | `d7ee70be` |
| — | `DryElementHotstartCarriesTheAgedState` — the observer the state/report separation lacked | ⚠ **never landed** — working tree only |
| S3 | **The snowpack water balance both loses and creates water.** Three divergences from legacy `routeSnowmelt`: SWE reduced by the drained excess rather than the melt (mass creation — a pack melting slower than its capacity never depleted), rain on the covered fraction discarded outright, and free-water capacity taken from pre-melt SWE. Plus S2a's instant-melt discard. Measured: SWE 0.5 → 0.48642 ft in an hour where it had been **exactly 0.5**; 1.0 % of runoff volume on a ripe melting deck | `c316c83e` |
| S4 | **The deck's `SD100` was never read, and `awe` initialised wrong.** `si` was pinned to the initial pack depth, so `wsnow >= si` held forever and **every snow deck sat at `asc = 1`** — no rain reached the ground under a pack. Reading `SD100` was necessary and not sufficient: `awe` started at 0 where legacy uses 1.0, pinning cover a second, independent way. Also **retracted D1**, this program's own false claim that legacy's seasonal constant was wrong | `2992f7c5` |

**Still open:** A2c (age-volume ledger row — needs its own definition, since
age is neither a mass nor conserved), A5 (LARD binding + cross-engine check),
A6 (Python/C/MCP age surface). A3 carries owed items of its own: hotstart
persistence, the `WATER_AGE_SNOW` question (untouched *and* undeferred), and
a cascade gate with enough age contrast to catch a donor/receiver swap. A4
carries one: a **transient** LID deck able to observe a mix-order error
(reading the donor layer's new age instead of its old one moves the result by
~`dt`, which is 1.7 % against a 15 % band at steady state — a narrower bound
on a steady deck cannot see it, only a moving one can).

**⚠ Every LID number in this program is provisional until issue #131 lands.**
A conventional `[LID_CONTROLS]` block reaches the solver unconverted — a soil
layer given in inches arrives as 18 ft with a 0.5 ft/s conductivity, 43,200×
too fast. A4's gate decks are written in feet and ft/s with a warning saying
so; they are *expected* to fail when the conversion lands, and the right
response then is to convert the decks, not to widen the bands.

**✅ RESOLVED — the forward-Euler divergence (D-H5d, `5cc83f94`).** The
surface energy balance had no stability limit, and heat capacity is `ρ·cp·V`,
so a thin film has almost none: a 0.52 ft³ film over 27,226 ft² took a
**+862 °C step in 60 s** and diverged to NaN through `subcatch_runoff_temp`
into the node temperatures and the report. All four bindings now integrate
semi-implicitly — linearize the net outward flux, step
`ΔT = (J₀/J′)·expm1(−k·dt)` — which cannot overshoot equilibrium at any `dt`.
Answers moved slightly where they should (H2's storage pool +4.5e-3 °C, the
subarea decks 0.05–0.15 °C) and a `dt` sweep confirms the gap halves with the
step: both schemes converge to the same limit, relaxation nearer it at every
step size.

**✅ RESOLVED — operator-split order dependence (D-H5e, `c292b8eb`).**
Relaxations do not commute, so the two node/link entry points each relaxed
fully toward their own module's equilibrium and the answer depended on module
order. One `applyHeatFluxes` now sums every enabled family before a single
relaxation, matching what the ARD cells and the subareas already did. On the
gate deck the split parked at −2.8384 °C — the surface-only equilibrium is
+2.2364, radiative-only −30.1352, combined −0.3942, so it landed on **none of
the three**. The merged answer is `dt`-independent: exact at steady state for
any timestep.

### 2.5 Heat transport — started

**H1 ✅ `4767aabb`** — `__TEMPERATURE__` as a reserved species, transported
and mixed by the LEGACY CSTR engine, reported as a trailing `.out` column in
°C, with a `[HEAT_SOURCES]` component and D-UT10's temperature-volume
accumulator through all seven loader pathways (this absorbed T0b's remaining
half). **Transport only** — nothing adds or removes energy yet.

It arrived with three defects and crashed on its own first gate: a
name-based special case in the `.out` writer that a second reserved species
turned into a null dereference, two stage-level guards that the new flag
never reached, and an expected constant borrowed from another gate's deck.
All three are recorded as lessons 51–53.

**H2 ✅ `221c5dac`** — latent and sensible exchange at the free surface: the
first terms that make temperature *change* rather than merely move, and the
first writer `ClimateState::humidity` has ever had. The area question H2 was
scoped to answer has an answer that is not new — **heat exchanges exactly
where evaporation already does**, which is also what keeps the module alive
under STEADY and KINWAVE. ρw·cp are load-bearing here, measured to 2e-4.

**H3 ✅ `7038bea9`** — the four radiative terms (net shortwave, back /
atmospheric / land-cover longwave) on the same free surfaces H2 uses.
Reading `RHEComponent` directly rather than the plan summary caught two
omissions in the plan text, either of which would have been silent physics:
the sky-view factor splits the longwave budget between sky and canopy, and
Brunt's emissivity takes vapour pressure in pascals — in kPa it is wrong by
√1000 and still looks like a plausible emissivity.

**H4 ✅ `8b5b3ef5`** — temperature is a mesh row, inheriting advection,
dispersion and mixing from the shared kernels, with per-cell surface fluxes.
Six of eleven falsifiers escaped as delivered; the reasons became lessons
59–63, the richest harvest of the program.

**H5a ✅ `65cae8a8`** — temperature on subcatchment surfaces: three per
subcatchment, one per ponded subarea, with the surface energy balance pulled
forward from H6 (D-H5a) because the plan's own H5 verify criterion was
unreachable without it. Run-on carries a (temperature-volume, rate) **pair**
rather than a numerator alone, so the LID underdrain arriving in H5b averages
over less water instead of dragging the mean toward 0 °C — the defect shape
A3 shipped. `DRY_ELEMENT_TEMPERATURE HOLD|AIR|DEFAULT` makes the dry-element
value the deck's choice (D-H5c): A4's "no water, no age" zero is wrong for a
temperature. Five of seven gates failed on arrival; lessons 74–79.

**D-H5d ✅ `5cc83f94`** — the surface balance integrates semi-implicitly at
all four bindings. See the resolved-hazard note in §2.4 above; the
order-dependence it introduced between the two node/link modules is the one
open item.

**H5b ✅ `1c78e9dd`** — temperature through the LID layer stack on A4's
existing block: one more species row, no second array. Vertical conduction
(D-H5b) is solved WITH the atmospheric flux as one implicit tridiagonal
system per column, because conduction is a second operator on the same state
and applying it sequentially would reproduce D-H5e's defect inside one
phase. A4's age values are **bit-unchanged** under the widened stride.

**The dry-but-present layer defect is fixed (`815f0e8e`).** `live[k]` — "does
this layer hold water" — was standing in for "has thermal mass" at two sites:
a drained layer dropped out of the conduction system, *and* fell under the
D-H5c dry policy, which reset a real physical state to a constant every step.
Both now use `mass[k]`; a buried matrix conducts whether or not its voids are
full, while a dry surface layer genuinely has nothing.

**The `dt`-refinement instrument closed four carried falsifiers at once
(`0e8e57df`).** A4 iii, H5a vi, D-H5e's linearization caveat and H5b ii were
each a `dt`-order error, where the correct and defective forms converge to
the *same* limit — so no fixed-step assertion could separate them. Each gate
runs one deck at three timesteps and asserts a contracting sequence plus a
coarse-step error small relative to the deck's own source spread. Every band
sits between a **measured** correct-form and defective-form error; the
separations are 2.3× to 3.4×, and gate 3's defect does not converge slowly at
all — it diverges to NaN. Sweep 5 of 5, 39–46 ms.

**⚠ What the heat track still lacks.** Conduction remains the one part with
**no external parity reference** — CSH and RHE model a streambed, not a
layered LID — so property gates stand in for parity and are weaker. And the
instrument's second leg is **one-sided by construction**: a defect whose sign
opposes the discretization error reads as *better* convergence (measured:
correct 0.001362, defective 0.000292). Two-sidedness would require a
reference value, which is the thing the instrument exists to avoid.

**H6–H7 ⬜.** H6 carries the HTS sediment layer, which is also where H3's
deliberately-omitted shortwave bed split belongs — **and it adds a fifth flux
family, which is the deadline on the node/link merge decision.**

### 2.6 I/O and component config

`IO1` + `IO2` ✅ `64c831d6` — `[PROCESS_COMPONENTS]` handler, registry
resolution, component-file section reader and path rules. `IO4`–`IO6` ⬜.

**Discrepancy to resolve:** the roadmap lists `IO3` as ⬜ in §1.6 while the
E5b row records "IO3 save-as carry-alongside" as delivered in `721ae60c`.
Both are probably right about different halves — the carry-alongside landed,
the InpWriter pointer section and round-trip gate may not have. Worth one
look before IO3 is either claimed or re-implemented.

### 2.7 Snow — an unplanned track, and the largest defect harvest of the program

Not on the roadmap. It opened when S1 went looking for the mixing volume under
a snowpack and found that `SnowSolver::setMeltCoeffs` **had no caller anywhere
in `src/engine/`**, so degree-day snowmelt had never fired in this engine's
history. Seven defects have now been fixed behind that one:

| # | What was wrong | Landed |
|---|---|---|
| F1 | `setMeltCoeffs` never called — `dhm` stayed zero, so only rain-on-snow above 0.02 in/hr could melt anything | `274b6506` |
| F2 | SWE reduced by the **drained excess**, not the melt — melted snow counted twice, as snow *and* free water | `c316c83e` |
| F3 | Rain on the covered fraction discarded — excluded from the ground by `rain·(1 − asc)` and never added to the pack | `c316c83e` |
| F4 | Free-water capacity measured against **pre-melt** SWE | `c316c83e` |
| F5 | The instant-melt branch's water assigned over and lost | `c316c83e` |
| F6 | The deck's `SD100` never read — `asc` pinned at 1 on every deck | `2992f7c5` |
| F7 | `awe` initialised to 0 where legacy uses 1.0 — `asc` pinned at 1 a second way | `2992f7c5` |
| D1 | ~~legacy's seasonal constant is a botched `2π/365`~~ — **RETRACTED.** 364 = 4 × 91 puts the melt peak exactly on the solstice; ours was the divergence | `2992f7c5` |

**Every divergence found in this module has been the engine's.** Nothing is
reportable upstream to EPA — which was the question S4 was opened to answer.

**The pattern, which is the transferable part.** F2–F5 were unreachable until
F1 gave melt something to mis-account. F7 was unreachable until F6 stopped
pinning `si`. And three of S3's four gates *passed with their own defect fully
restored*, because the shared deck writer starts every pack **ripe** — a store
already at capacity drains every drop the instant it appears, so "SWE −= melt"
and "SWE −= excess" are arithmetically the same number. Gate 16 is the
counter-example worth copying: it asserted the **observable** (`asc < 1`)
rather than the mechanism, so it failed on the half of S4's changeset that was
still wrong instead of ratifying it.

**⬜ S2b — the pack age model — is the next step and is now unblocked.** It was
deliberately held behind the water balance: an age model that complete-mixes
over the pack's water inherits every hole in that balance, and calling that an
approximation is lesson 64. Scoped in `S2A_MELT_TEMPERATURE_HANDOFF §7`.

**⬜ A snow parity deck is owed** — see `SNOW_DIVERGENCE_REGISTER.md` §4. Seven
defects have been fixed in a module the 14-deck corpus cannot observe at all.

### 2.8 O4 — the API/CLI divergence, narrowed to one remaining variable

O4 measured **7.25 in** of precipitation delivered to the ground through the
API against **12.98 in** through the CLI, same deck, same rainfall. The
premise turned out to be worth checking: `lifecycle_open_model` and
`lifecycle_run_simulation` are **MCP tool names, not engine entry points** —
there is no `lifecycle_run_simulation` anywhere in `src/` — so the API
measurement came through the MCP server, a third variable the differential
was never controlling for.

`src/tools/o4_differential` removes it (`3bdc30a2`, off by default behind
`OPENSWMM_BUILD_O4_DIFFERENTIAL`): one deck, one process, one binary, through
the C API five ways, one variable each — a reopen before stepping, `start(0)`,
`report()` before `end()`, and a working directory away from the deck. The
`cli` variant is byte-for-byte `src/cli/main.cpp` and is the control.

**Outcome A, and the control passed** — `o4_cli.out` byte-identical to a real
`openswmm` run. All five variants agree on **every hydrology number**: 8,640
steps, continuity 0.407 %, snow cover 1.500 / 0.340. **The C API is
exonerated. The MCP server is the only variable left**, and that re-run needs
a session with the openswmm MCP tools.

**The instrument shipped with a defect of its own (`97bfa512`).** It discarded
`report()`, `end()` and `close()`'s return codes, so the round recorded
`report()` before `end()` as "legal and silently lossy — no error code". There
is an error code. **The correction was then half wrong too**: it named
`report()`'s `state != ENDED` guard from a code read, but `step()` has already
set `ENDED` by then, so that guard never fires — `report()` succeeds and sets
`REPORTED`, and **`end()`** is the refused call, code 6. A falsifier settled
which explanation of the artefacts was right: **both, for different files.**
The `.out` truncation is `end()`'s absence (letting `end()` accept `REPORTED`
closes it, 190446 → 190470, byte-identical to `cli`); the `.rpt` omissions —
`All links are stable.` for `Link C1 (0)`, a flat timestep histogram for the
real `300.000 → 0.500` ladder — are a report that ran early.

**✅ RESOLVED 2026-08-22 — and it was never an execution path.** The re-run
through the MCP server reproduces O4 to the digit: infiltration 3.674 in,
runoff 3.573 in, continuity **+39.543 %**. The `.out` parts from `o4_cli.out`
at **period 120, 2026-01-06 01:00** — the first hour of the deck's first thaw
— header, IDs and properties byte-identical. Plotting melt against the deck's
own forcing named it in one pass: **the API run melts snow only in the
rain-on-snow window (01-15→01-18, 38 °F, 0.05 in/hr) and never by degree-day —
zero melt across eight consecutive days at 47 °F.** That is F1's signature,
`setMeltCoeffs` uncalled and `dhm == 0`, fixed in `274b6506` on 08-20. **The
library the MCP server loads was built 2026-08-03** — seventeen days and ~40
commits stale.

**Nothing in the engine needs fixing.** The three snow commits O4 appeared to
indict are correct, and the standing rule stays in force only until the
library is rebuilt and the re-run comes back byte-identical — it is now cheap
to lift rather than open-ended. **One link is unverified**: the server's
configuration was not read, so the load path is inferred behaviourally.
Account: `O4_RESOLVED_STALE_MCP_LIBRARY_2026-08-22.md`; artefacts and the
analysis script in `tests/output/o4_mcp_2026-08-22/`.

**Two build-hygiene defects surfaced on the way, both live.** The Aug 3 and
Aug 21 libraries **both report `6.0.0-alpha.3`**, and
`build/install-prefix/include/openswmm/version.h` is a generated header dated
**2026-06-01** still saying `alpha.2` — which is why `o4_cli.rpt`, produced
from current source, prints alpha.2. **The report header's version line is
not evidence about what code ran**, in either direction.

---

## 3. Test evidence

| Suite | Gates |
|---|---|
| `test_water_age.cpp` | 16 |
| `test_heat_transport.cpp` | 9 |
| `test_heat_surface_exchange.cpp` | 7 |
| `test_heat_radiative_exchange.cpp` | 7 |
| `test_heat_ard_binding.cpp` | 6 |
| `test_water_age_watershed.cpp` | 8 |
| `test_water_age_lid.cpp` | 6 |
| `test_heat_watershed.cpp` | 10 |
| `test_heat_integrator.cpp` | 8 |
| `test_heat_lid.cpp` | 9 |
| `test_transport_dt_reference.cpp` | 4 |
| `test_transport_snow.cpp` | 18 |
| `test_ard_dispersion.cpp` | 11 |
| `test_ard_transport_bcs.cpp` | 10 |
| `test_reaction_legacy_binding.cpp` | 10 |
| `test_reaction_ard_binding.cpp` | 9 |
| `test_reaction_integrators.cpp` | 9 |
| `test_output_quality.cpp` | 8 |
| `test_reaction_expressions.cpp` | 8 |
| `test_ard_e5b.cpp` | 7 |
| `test_reactions_config.cpp` | 7 |
| `test_ard_transport.cpp` | 6 |
| `test_process_components.cpp` | 5 |
| `test_ard_node_store.cpp` | 4 |
| `python/tests/engine/test_output_species_ids.py` | 3 |
| **Total** | **206** |

**⚠ Three gate counts in this document disagree and none of them has been
recounted from source.** §1 says 216, this table says 206, and the rows above
sum to **205**. The rows are also stale — `test_transport_dt_reference.cpp`
and `test_transport_snow.cpp` have both grown since they were entered. Nobody
has run `ctest -N` and reconciled. **Treat every figure here as approximate
until someone does**; the suite pass/fail count (159/160) is measured each
round and is the reliable one.

**A caveat on the Python figure, found while validating `d7ce8efb`:** all 28
engine test modules skip themselves on `ImportError` at module scope, so a
run against a non-editable install reports `collected 0 items / 1 skipped` —
which reads as a pass. A green Python run only means something if the
collection count is checked too.

---

## 4. How the work was validated, and what that caught

Every step follows the same loop: implement in one shot in a syntax-only
sandbox → write a validation handoff naming the design decisions, the
anticipated failure modes in likelihood order, and a falsifier sweep table
→ an independent checking agent builds, runs, falsifies and commits →
findings recorded as numbered lessons (**201 to date**).

The loop is not ceremonial. Rounds that arrived with real defects:

| Round | What the checking agent found |
|---|---|
| R3 | As-delivered failed **0/6** on four numerics defects (time advanced by the *proposed* step; wrong ROS2 weights; BDF2 had no error control and invalid coefficients under variable h; a COUPLING NONE publish bug) |
| E4/R6 | A loop narrowing swept the store non-negativity clamp with it — MSX store masses went to −273 at an emptying node |
| E5a | All six `QualitySolver` loaders guarded `if (np <= 0) return`, blocking external-inflow **volume** as well as mass, so MSX-only decks got zero boundary injection — and every gate deck had pollutants enabled, so the motivating configuration was not in the matrix |
| E5b/IO3 | The config copy used `overwrite_existing` and silently destroyed a different pre-existing file, warnings = 0 |
| A1a | A value token was bound before the arity check — a three-token typo aborted the engine; and a save-as silently reopened as LEGACY with age off |
| A2a | **Both delivered gates were vacuous** — they called `hotstart_apply` from the wrong lifecycle state and exited before the first assertion |
| A2b | The "stride razor" was blind: it read columns by fixed index and never asserted a name, while the design had made the name the sole discriminator |
| dry-element mask | The mask was **inert on links** — a dry conduit never reports depth 0 (the router floors it at `FUDGE` = 1e-4 ft) against a 1e-9 threshold. The node branch worked, so half the feature was live and the dead half was the half the defect was filed on |

The two most recent rounds (`d7ce8efb`, `f37f7dde`) were accepted unchanged,
so the table is not the whole picture — but the loop still earned its keep on
both. On the GeoPackage round the *changeset* was correct and my **falsifier
criterion** was wrong: I asked which legs fail when the collision check is
removed, and ruled that if only the return-code leg failed the corruption was
theoretical. It isn't — the fix makes `update()` unreachable, so that gate
can never see the consequence. Measured directly on a tolerate build, a
pollutant concentration lands in the node depth series at 99999.0.

Three findings generalize: a gate that cannot fail is worse than no gate; a
claimed defence needs an observation path or it is not tested code; and when
a defence prevents its own consequence, no falsifier can measure that
consequence — it needs a separate probe.

---

## 5. Open items

| Item | Owner / what closes it |
|---|---|
| **⚠ THE COUNTS IN §1 AND §3 ARE LOWER BOUNDS** | This file is blind to the X/Y/Z/closeout tracks' tests and decks. Corpus is **19**, not 15. `ctest -N` + a `wc` sweep closes it; §3 has asked since 2026-08-22 |
| ~~G0 sign-off (D-N1–N5)~~ | ✅ **CLOSED 2026-08-25.** D-N1 approved conditional on a 2D corpus deck landing first — **the risk→chore re-rating is RETRACTED (2026-08-26): eleven fixed sites, not four, and two are GPU out-of-bounds**; D-N2 deferred to step 18 by decision; rest approved. **Found during sign-off: §11's named protection gate does not exist** — 19 corpus decks, none 2D |
| **Phase 2 is refused, not unstarted** | a library-backed `[PROCESS_COMPONENTS]` row hard-errors (`ProcessComponentRegistry.cpp:216-223`). Costing it as four ordinary steps understates it — see `PROGRAM_REVIEW_2026-08-25.md` §3 |
| ~~SAVING SILENTLY DESTROYED EMBEDDED REACTION DATA~~ ✅ **FIXED `7d43a1ff`** — engine side. 19/19 `.out` and `.rpt`, 177/177 ×3. ✅ **GUI half LANDED `040a8de`** (openswmm.gui) — `saveAs` captures the per-save warning delta, routes it to the log panel and raises a modal on data loss. **⚠ Residual: the queued notice is never delivered on the QUIT path** (save-on-close then exit) — fixed in the tree, premise unmeasured, revert-if-refuted; see `SAVE_WARNING_QUIT_PATH_HANDOFF_2026-08-27.md` | Open a deck with embedded `[REACTION_*]`, edit, save: **the reaction system is gone with no message anywhere, GUI included.** The warning at `InpWriter.cpp:2580-2586` is gated on an optional sink and **all three production callers pass nothing**; the test that certifies it calls the writer directly **with** a sink, so it structurally cannot catch this. My earlier claim that "the engine warns" was wrong (2026-08-26) |
| **G0 sign-off** — groundwater plan §10 decisions (D-N1–N5) | Review work, sitting since 2026-08-15; cheap, and it gates all of Phase 4 |
| **⬜ A snow parity deck is owed** | Seven defects (F1–F7) fixed in a module the 14-deck corpus cannot observe — no deck has `[SNOWPACKS]`. One deck with a nonzero `SD100` and a real `ADC` curve turns "byte-identical because nothing exercised it" into a real result. **Hard prerequisite once S2b lands** (`SNOW_DIVERGENCE_REGISTER.md` §4) |
| ~~⛔ O4 — re-run through the MCP server~~ | ✅ **Done 2026-08-22. Not a divergence — a stale library** (§2.8). Remaining: **confirm the server's load path** (not read; the inference is behavioural), **rebuild/reinstall** whatever it imports, and re-run — `o4_mcp.out` should come back byte-identical to `o4_cli.out`. **The standing rule holds until that passes** |
| ~~⛔ the git index~~ | ✅ **RESOLVED 2026-08-22.** Cause was a **zero-byte `.git/index.lock` stuck since 08-21 05:58**, freezing the main index for a day while HEAD advanced seven commits via `GIT_INDEX_FILE` — not "drift". No work was ever at risk: all 34 phantom deletions existed on disk, 33 byte-identical to HEAD. **Removing the lock emptied the index** (65 bytes, `git write-tree` → the empty tree, 1850 phantom deletions); `git reset` rebuilt it. All 1850 HEAD paths checksummed before and after — **aggregate identical, not one byte changed**. Record: `tests/output/index_repair_2026-08-22/` |
| **⚠ `tests/parity/snow/baseline/` is not tracked** | `d633c53e` tracked the deck, generator and README and left the generated baseline out. So the one mechanism in that directory that detects **cross-round drift lives in a single working tree** — the condition the round was opened to fix, surviving one level down. `README.md` §3 carries the warning; either regenerate from `gen_snow_parity.py` plus a recorded build, or drop the claim. **Owed, not decided** |
| **Corpus composition gaps** | ~~0 water-age, 0 heat~~ ✅ **both landed `1da1d7ca`** (age_legacy, age_ard, heat_parity). Still 0 SI, 0 STEADY, **0 2D**; corpus is 19 decks. Each addition needs its own justification — a deck reaching nothing new makes every run slower and proves nothing |
| **Build hygiene — two live defects** | (a) Builds 40 commits apart both report `6.0.0-alpha.3`; (b) `build/install-prefix/include/openswmm/version.h` is a generated header dated 2026-06-01 still saying `alpha.2`, and it is on the `build/` tree's include path. **A version line is not evidence about what code ran.** Found via O4; neither fixed |
| **C API contract: `end()` after `report()`** | Returns `SWMM_ERR_WRONG_STATE` and leaves the `.out` **unfinalised**, with nothing forcing the caller to notice. Either the state machine should let `end()` close a `REPORTED` run, or the truncation should be documented. Separable from O4; recorded, not fixed |
| **⚠ Seven commits unpushed** | `8b7d1cf7`, `c316c83e`, `2992f7c5`, `3bdc30a2`, `97bfa512`, `b85b802d`, `d633c53e`. Two of them move hydrology on every deck with snow |
| **Every test shares one working directory** | `data/`, so the fixture-collision class exists by construction. Renaming plus the configure-time check is containment; **per-test working directories would make it impossible**. Not attempted — it touches every `add_test` and the fixtures some tests legitimately read from `data/` |
| **Gate counts unreconciled** | §3's three figures disagree (216 / 206 / 205) and the per-file rows are stale. One `ctest -N` closes it |
| ~~**⚠ The dry-link hotstart gate never landed**~~ ✅ **CLOSED — it DID land.** `DryElementHotstartCarriesTheAgedState` is at `tests/unit/engine/test_water_age.cpp:911` with the bone-dry deck, `INITIAL_STATE 6 h`, saved link age > 21600 s and the bit round-trip, and its comment records the mask-the-state falsifier. The row below was stale from the A2b era; verified 2026-08-29 |
| **⚠ Dry elements report a carried temperature indefinitely** | The consequence of H1's deliberate no-mask call. Because 0 °C is a real temperature this cannot be fixed inside the current `.out` format — it needs a per-column no-data sentinel, scoped before H2 fills the column with flux-driven values |
| ~~⚠ Git index drift~~ | ✅ **Resolved during the H2 round** — it had grown to seven phantom staged deletions. Fixed by checksumming all 184 listed paths, running `git reset`, and confirming not one byte changed |
| Unexplained, seen once | 3 extra failures in `test_files_iface_gaps.py` on one Python run during `d7ce8efb` validation; did not reproduce across two full runs and an isolated run. Recorded as a first sighting, not a diagnosis |
| Treatment `mass_lost` is step-dependent | Pre-existing legacy defect in **both** engines; needs its own parity round — the fix moves every model's reported Mass Reacted |
| ~~GeoPackage emits **no** species rows~~ | ✅ **Fixed `f37f7dde`** — `prepare()` registers the reported species for NODE/LINK/SUBCATCH. Was dropping ALL quality, pollutants included |
| `HotStartManager.cpp:246` misaligned CRC load | Pre-existing (blamed to `4e29c8869`); one-line memcpy |
| ARD reads TSS 0 at an outfall where LEGACY reads 42 | Pre-existing ARD outfall behaviour, verified independent of water age |
| Manual note owed | LEGACY and ARD link ages differ 6.5–15.2 % **by definition** (mixed-tank outlet vs volume-weighted cell mean); the comparable quantity is the outfall node |
| A1a rs-unpin | ~~Owed~~ done in A1b |
| Dry-cell source-share accounting | Sidecar candidate, recorded not scoped |

---

## 6. What is not started

**⚠ THIS SECTION'S HEADLINE WAS FALSE and is corrected here (2026-08-29).**
Phase 5 (LARD) is *mostly delivered* and the GUI track has **eight** landed
commits (Y1 `ebf28ae` · Y2a `dcc20e6` · Y2b-1 `dae4bad` · Y2b-2 `7a5f732` ·
Y2b-3 `9e63357` · Y3 `f5e0d9b` · **Y3b `bc4e07c`** · Y4 `94ff3b5`). What
remains unstarted is **Phase 2 (refused), Phase 3 (2D transport), Phase 4
code, and GUI G4g/G6g/G7g** — not "the bulk of the program".
The authority for what is left is `LARD_CLOSEOUT_PLAN_2026-08-24.md`. The near-term sequencing decision already made is:
**1D transport on existing formulations first → HydroCouple interfacing → 2D
→ groundwater**, with G0 decision-closure pulled early because it is cheap
review work that unblocks a whole phase.

Within Phase 1, the roadmap's own recommended order puts **H1–H3 (heat)**
next, with A3–A4 later — *after* H4, not before it. Tracing the order
against what landed also surfaced that two steps ran ahead of their slot
(A1–A2 before T0b, E5+IO3 before H1–H3), which is how the T0b discrepancy
came to light and produced D-UT10.
