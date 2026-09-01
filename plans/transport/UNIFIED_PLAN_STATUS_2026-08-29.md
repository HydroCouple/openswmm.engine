# Unified Transport Program — Status (2026-08-29)

**What this is.** One page showing what the unified plan asked for, what has
landed, and what remains. Superseded documents stay where they are; this one
is the current answer to "where are we".

**Authority.** `IMPLEMENTATION_ROADMAP.md` remains the authority for *why* each
step landed the way it did. `LARD_CLOSEOUT_PLAN_2026-08-24.md` is the authority
for the remaining backlog. This file is derived from both, **re-checked against
the code on 2026-08-29** — see §9 for what is measured and what is inherited.

**⚠ Read §9 before planning from §1.** The last five stock-takes of this
program each found the trackers overstating what was left, never understating
it. The rows below marked *inherited* have not been re-derived.

Legend: ✅ landed and validated · ◐ partly landed · ⬜ not started ·
⛔ blocked or refused

---

## 1. At a glance

| Phase | Scope | Status |
|---|---|---|
| **1 — 1D transport** | ARD engine, MSX reactions, water age, heat, I/O config | ✅ **essentially complete** |
| **5 — LARD (Lagrangian)** | segment store, LTD advection, RWPT dispersion, age | ✅ **delivered except L3** |
| **GUI track** | transport/quality editors, species theming/plotting | ◐ **6 of 8 landed** (G4g shipped 2026-08-31) |
| 2 — HydroCouple | `IModelComponent` wrapper, Composer coupling | ⛔ **refused, not merely unstarted** |
| 3 — 2D surface transport | tracer advection on the marcher, 1D↔2D coupling | ⬜ 0 of 7 |
| 4 — Groundwater | two-zone explicit-LTS | ◐ G0 signed off; 0 of 4 code |

**The 1D water-quality core is done.** What remains in quality is a *binding*
(L3) and an editor (G4g) — not new formulation work. P1.4 closed 2026-08-29
(`4b26aa50` + `0e73f7ea`).

---

## 2. Phase 1 — 1D transport ✅

| Track | Steps | State |
|---|---|---|
| Eulerian ARD engine | E0 · E1 · E2 · E3 · E4 · E5a · E5b | ✅ |
| Species registry | T0a · T0b | ✅ (D-UT10: parallel per-capability accumulators, not a widened tuple) |
| MSX reactions | R1 · R2 · R3 · R4 · R6 | ✅ compiler + flat-pool VM + EUL/RK5/ROS2/BDF2 + Newton EQUIL + FORMULA |
| Water age | A1 · A1b · A2 · A2a · A2b · A3 · A4 | ✅ reserved species `__WATER_AGE__`, seconds internally, hours reported |
| Heat | H1 · H2 · H3 · H4 · H5a · H5b · H6a · **H6b** | ✅ reserved species `__TEMPERATURE__` (°C). **H6b `89310068` (2026-08-31): bed conduction + deep-ground conduction + hyporheic exchange** — [SEDIMENT_EXCHANGE] stops refusing itself; coupled water/bed 2×2 matrix-exponential pair (the "one term in netFluxOut" prediction was WRONG — wetted-perimeter area + second state variable); solutes exchange in (total,difference) coordinates on the LEGACY link store; ARD/LARD decline BY NAME. Reference: HydroCouple HTSComponent |
| I/O + component config | IO1 · IO2 · IO3 | ✅ |
| Snow (unplanned track) | S1 · S2a · S3 | ✅ seven defects; the largest harvest of the program |
| API/CLI divergence | O4 | ✅ resolved — a stale library, not a divergence |
| LARD wiring / API | X1–X6 · Y0 · Z1 | ✅ |

**Quality-ledger units (`5b21f9a6`)** — the mass-conversion seam now converts
at source as legacy does (`mcf` at every booking), the Washoff Summary and the
ledger agree by construction, continuity error gained legacy's third branch,
and the **vendored `landuse.c` parity reference was repaired** (`>=` → stock
`==`, under which any land use with a buildup function washed off nothing,
ever). Validated 2026-08-23; recorded in the roadmap only on 2026-08-29.

**Embedded-section data loss** — `7d43a1ff` (engine) + `040a8de` / `bec6e1d`
(GUI). Saving a deck with an embedded `[REACTION_*]` system destroyed it
silently, in both repos, including on the quit path. Now warned end to end.
**Still a mitigation, not a cure** — IO3's per-component `saveData()` is what
would stop the loss.

### Remaining in Phase 1

| Item | Size | Note |
|---|---|---|
| ~~P1.4 `[TRANSPORT_SOURCES]` negative rows~~ | — | ✅ **CLOSED** `4b26aa50` (extraction + clamp) + `0e73f7ea` (per-clamp warning retired). **Next step is now H7** — §6 |
| P1.5 negative DWF / GW / RDII concentrations | 1 round, optional | plan suggests closing as *won't do* rather than carrying it |

---

## 3. Phase 5 — LARD ✅ except L3

| Step | State |
|---|---|
| L0 SegmentStore · L1–L2 LTD advection · L4 RWPT dispersion · L5 water age | ✅ |
| L6 perf pass · L7 docs / default promotion | ◐ deferred by scope decision |
| **L3 — MSX reactions on segments** | ✅ **COMPLETE 2026-08-31, engine `ec22580a`** — species ride the segments (rowLayout + MIX/passthrough/RELEASE/publish/seed) and react per segment (pipe scope) + node store (tank scope, HRT) through the shared integrator; deferral warning gone; cross-engine vs ARD ratio 0.9751 |

**L3 is a binding, not a new module.** The shared `ReactionSystem` (R1–R4)
already exists; the work is D-L1's stated gather/scatter — collect each
segment's species column into a stack block, integrate, scatter back. Today
`"the LARD reaction binding is not implemented (deferred L3)"` fires at open,
and only first-order KDECAY reacts under LARD. **2–3 rounds.**

---

## 4. GUI track — 5 of 8

| Step | Scope | State |
|---|---|---|
| G1g | Quality & Transport options page | ✅ `ebf28ae` |
| G2g | Reaction System editor + expression stack | ✅ + first observer `11f8ea5`/`99fe650` |
| G3g | Water Age Sources editor | ✅ `f5e0d9b` + reachable `bc4e07c` |
| G5g | Results descriptors — theming, pickers, plots | ◐ `dcc20e6` · `dae4bad` · `7a5f732` · `9e63357`; **per-species units from the `.out` owed** |
| — | Age as a first-class species (D-Y4) | ✅ `94ff3b5` |
| **G4g** | **Heat configuration editor** | ✅ **SHIPPED 2026-08-31, gui `000ae72`** — Model ▸ Heat Configuration…, five tabs (sources+overrides, fluxes, radiative, solar, cloud), writeIfChanged, parser-mirrored limits, COMPUTED gated on sited; 7-claim GUI test; recorded gap: no engine getter for a bound sw/cloud timeseries NAME (rebind-only combos). Old note: ⬜ **UNBLOCKED 2026-08-31** — IO3b (`23c1ddfb`) extended the heat renderer to all five sections; heat edits survive a save on every model. Build against the corrected spec (no `[HEAT_METEOROLOGY]`; `[HEAT_SOURCES]` + existing climate rows + H6a radiative/solar/cloud). Superseded note:  — IO3a landed (`4738bca9`, 2026-08-31): heat + reactions write their own config files, `[HEAT_SOURCES]` edits survive a save. Still owed: the heat renderer does not cover H6a's radiative/solar/cloud sections (it DECLINES to the copy for such models, no truncation, but their API edits still fall back to the step-3 loss). An editor limited to `[HEAT_SOURCES]` + climate rows round-trips today; the full G4g spec needs IO3b. Old note:  `openswmm_heat.h` is complete and validated (H6a + step 3, 2026-08-31: 28 functions, 184/184 ×3, corpus 21/21) — but API edits to `model.heat` do not survive `swmm_model_write` (no per-component `saveData()`), so an editor today would silently lose every edit on save. Spec correction: `[HEAT_METEOROLOGY]` does not exist — G4g = `[HEAT_SOURCES]` + the existing `[TEMPERATURE]` climate rows |
| G6g | 2D transport rendering + ARD profile sidecar | ⬜ gated on Phase 3 |
| G7g | Storage mixing / dispersion / boundary properties + docs | ⬜ |

**Context assist + syntax highlighting: shipped and now observed.**
`ReactionSyntaxHighlighter` + `QCompleter` draw their vocabulary from the live
model (species, coefficients, terms, pollutants) plus engine-supplied hydraulic
variables and functions, with debounced validation through
`swmm_reaction_validate_expression` so the engine stays authoritative. Three
gates as of 2026-08-29; the highlighter's *formatting* and the completer
*popup* remain unobserved (recorded in the round's handoff).

---

## 5. Phases 2, 3, 4

**Phase 2 — HydroCouple ⛔ REFUSED.** Not "unstarted": a library-backed
`[PROCESS_COMPONENTS]` row **hard-errors** at
`ProcessComponentRegistry.cpp:216-223`. Costing this as four ordinary steps
understates it. See `PROGRAM_REVIEW_2026-08-25.md` §3.

**Phase 3 — 2D surface transport ⬜** 0 of 7 (`2D-S1…2D-S7`).
**⚠ The corpus contains no 2D deck** (20 decks, zero mesh), so "corpus green"
says nothing about the surface solver. A bitwise 2D script exists
(`tests/scripts/trackI_bitwise_regression.sh`, 32 decks) but is **not wired
into `run_corpus.sh`**.

**Phase 4 — Groundwater ◐** G0 signed off 2026-08-25; G1 ready to start; 0 of
4 code steps. **D-N1 is a RISK, not a chore** — the 2026-08-26 retraction
found eleven fixed-array sites, not four, and two are **GPU out-of-bounds**
(`ExplicitKokkosSurfaceSolver.hpp:137-138`, `:149`). Conditional on a 2D corpus
deck landing first.

---

## 6. Closeout backlog

| Item | State |
|---|---|
| P0.1 push | ✅ engine 0 unpushed; GUI 1 |
| P0.2 CHANGELOG (CLAUDE.md §5.2) | ✅ engine `2ecdef8a`, gui `d9ad932` |
| P0.3 shared-tree protocol as habit | ◐ written, not yet reflexive |
| P1.1 rs-ladder instrument | ✅ `6566f407` |
| P1.2 dry-hotstart gate | ✅ `test_water_age.cpp:911` |
| P1.3 C-API numeric audit | ✅ `22e55228` |
| **P1.4 negative cell sources** | ✅ `4b26aa50` |
| **P1.4b clamp-warning contract** | ✅ `0e73f7ea` — the contract call is CLOSED (warning retired, summary kept) |
| P1.5 negative DWF/GW/RDII | ⬜ optional |
| P2.1 H7 heat under LARD | ✅ **COMPLETE 2026-08-30** (H7a `f31efd63` + H7b `deb42172`) |
| Step 3 `openswmm_heat.h` (+H6a validation) | ✅ **COMPLETE 2026-08-31, engine `803d5cbc`** — 28 functions incl. the `[HEAT_SOURCES]` table; H6a validated in the same round (fixture bug fixed, Bird verified vs pvlib, `frac()` NaN hole closed). **G4g now waits on IO3 serialization** (see its row) |
| P2.2 L3 MSX on segments | ✅ `ec22580a` (2026-08-31) — one round, not 2–3: H7a/H7b's layout work had already paid the hard part |
| P2.3 treatment interop under LARD | ⬜ 1 round |
| P2.4 storage mixing beyond CMSTR | ⬜ 1–2 rounds |
| P2.5 full A6 — Python + MCP age surfaces | ⬜ 1 round |
| P3 verification breadth | ⬜ laminar RWPT deck · RWPT corpus deck · `swmmvis_core` extraction |

Every P2 item is **warned at open**, so nothing fails silently — the plan's own
guidance is "pick the one your next project needs."

---

## 7. The decision owed before P1.4

D-NS1 (negative sources) is implemented at the **node** seam in all three
engines. ARD's **cell** sources were deliberately left out because they have
their own conservation story, so a negative cell source is unspecified
behaviour today.

**The plan says decide first: does a clamped cell extraction get its own ledger
row, or ride `qual_routing_ex_in`?** The answer differs from the node case
because cell sources are *distributed*. A new row is visible and matches the
"count and summarise" contract but **moves the `.rpt` on every ARD deck with
sources**; riding the existing row keeps the report still but makes the clamp
invisible in the ledger. This is a reporting-contract call, not a coding
preference, which is why it is yours.

---

## 8. Open defects and debts

| Item | Note |
|---|---|
| ~~Kdecay 86 400×~~ **FIXED — KD1 `3aa37c00` (2026-08-31, same day as triage)** | Was: applied RAW against dt-in-seconds everywhere; the common case annihilated the pollutant, unbooked, 100 % continuity error. Now: 1/day at every boundary (INP, GPKG, API — the header's own contract), 1/sec inside like legacy; decay books its mass on every path; junctions no longer decay (legacy parity); mixAtNodes' evap factor no longer creates mass at draining nodes. Cross-engine: Mass Reacted 0.158 vs legacy 0.159. Residual for P2.4 (BOTH engines): transit-mass decay escapes volume-basis booking — legacy leaks 6.5–7 % on the same storage deck. `KDECAY_UNITS_TRIAGE_2026-08-31.md` |
| ~~ARD does not relax~~ **FIXED `6264eb8a` (2026-09-01)** | Both ArdEngine heat sites now call relaxT; temp-row publish holds below legacy ZeroVolume (was 6e+117 degC on a draining deck). Gate: steady warm thin sheet — base reads an impossible 0 degC (Euler crash-landing on the mass clamp), patched warms monotonically. NEW recorded debts from the same dig: ARD advection unbounded under CFL starvation on draining decks; temperature rides the max(0,mass) clamp (0 degC floor). The ARD/LARD bed bindings are now UNGATED |
| **ARD/LARD bed bindings (H6b §4)** | The bed binds to the LEGACY link store only; mapping a per-link bed onto cells/parcels is a modelling decision (recorded in BedZoneData.hpp on the array a successor would widen). Both engines decline BY NAME at open. Needs the ARD-relax fix first |
| ~~InpWriter idempotence drift~~ **FIXED `3e87868e` (2026-09-01)** | SWEEP anchors unified on non-leap 2001 (was: parse leap-2000 / format 2001 — one day's walk per save past Feb 28, default included); [MAP] Units renders legacy mixed case from the uppercase canonical. Gate drives gen3 and pins gen2==gen3 byte-equality |
| ~~No getter for a bound shortwave/cloud timeseries NAME~~ **CLOSED `d868b2c3` (2026-09-01)** | swmm_heat_get_shortwave_timeseries / _get_cloud_timeseries land with a 10-claim gate; GUI wiring of the combos follows in the gui repo |
| **Dry elements report a carried temperature indefinitely** | H1's deliberate no-mask call. 0 °C is a real temperature, so this needs a per-column no-data sentinel in the `.out` |
| **`tests/parity/snow/baseline/` is not tracked** | The one mechanism that detects cross-round drift lives in a single working tree — the condition the round was opened to fix, one level down |
| **A snow parity deck is owed** | Seven defects fixed in a module no corpus deck exercises |
| **Corpus composition** | 20 decks; **0 2D**, 0 SI, 0 STEADY |
| Treatment `mass_lost` is step-dependent | Pre-existing in **both** engines; the fix moves every model's reported Mass Reacted |
| ARD reads TSS 0 at an outfall where LEGACY reads 42 | Pre-existing ARD outfall behaviour |
| `HotStartManager.cpp:246` misaligned CRC load | Pre-existing; one-line memcpy |
| Build hygiene | Builds 40 commits apart both report `6.0.0-alpha.3`; a generated `version.h` dated 2026-06-01 is on the include path. **A version line is not evidence about what code ran** |
| `end()` after `report()` | Returns `SWMM_ERR_WRONG_STATE` and leaves the `.out` unfinalised, silently |
| Every test shares one working directory | The fixture-collision class exists by construction; renaming + a configure-time check is containment, not prevention |
| **`plans/` and `workplans/` are gitignored** | The entire plan corpus of both repos exists in one working tree on one machine. **Decision owed, twice** |

---

## 9. Provenance — what is measured, what is inherited

**Measured 2026-08-29:** unpushed counts (engine 0, GUI 1); corpus 20 decks
with 0 2D; P1.1/P1.2/P1.3 closure; G3g reachability; `5b21f9a6` in HEAD;
the reaction-editor gate result.

**Measured 2026-08-26, not re-derived:** 177 ctest tests · 2,786 gates across
317 files · 296,677 test LOC · 976 program-era gates.

**Inherited, not re-checked:** the Phase 3 step count, the Phase 4 step count,
and every "size in rounds" estimate in §6.

**Method note.** This program's trackers have drifted in one direction — five
rows in the 2026-08-29 pass, five in the 2026-08-25 pass, all overstating
remaining work, because a validated round writes its result into the handoff
that requested it and the authority never learns. Rows above are corrected;
**the mechanism is not**. Until a round ends by writing to the roadmap, the
next stock-take will find the same class of error.
