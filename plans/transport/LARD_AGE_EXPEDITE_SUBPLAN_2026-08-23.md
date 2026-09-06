# Expedite Subplan — Water Age + LARD Transport/Dispersion + GUI (2026-08-23)

> **✅ CLOSED 2026-08-24.** Every round of this subplan AND of Amendment 1
> (D-Y4) is landed — engine X1/X2/X3a/X3b/X4/X5/X6/Y0/Z1/H1, GUI
> Y1/Y2a/Y2b-1/Y2b-2/Y2b-3/Y3/Y3b/Y4 — verified in source, not from commit
> messages. **Remaining work is tracked in
> `LARD_CLOSEOUT_PLAN_2026-08-24.md`**: release hygiene (push, CHANGELOG),
> four correctness debts with recorded defect stories, and the
> scope-deferred capabilities (heat under LARD, L3 reactions, treatment
> interop, storage models, full A6) — each of which warns at open today, so
> nothing fails silently.

**What this is:** a fast-path sequencing overlay for one project need: water
age tracking and pollutant transport + dispersion under **LARD**, with the GUI
elements to configure runs and view results on top of the existing hydraulics
GUI. It re-orders and trims steps that already exist in the vetted plans; it
defines no new strategy. **Authorities per step:**
`IMPLEMENTATION_ROADMAP.md` (tracking), `plans/LAGRANGIAN_QUALITY_STRATEGY.md`
(LARD, §16 amendments binding), `WATER_AGE_TRACKING_PLAN.md` (A5/A6),
`openswmm.gui/workplans/TRANSPORT_QUALITY_GUI_PLAN_2026-08-12.md` (GUI).

**Scope decisions (user, 2026-08-23):**

| Topic | Decision |
|---|---|
| LARD scope | Transport (L0–L2) + RWPT dispersion (L4) + water age (L5) + A5 cross-engine gate. First-order decay only. **Deferred:** L3 MSX-on-segments, storage mixing models beyond CMSTR, L6 perf pass, L7 docs/default promotion |
| GUI depth | Options page (G1g) + result descriptors (G5g) **+ Water Age Sources editor (G3g)** — G3g requires a minimal A6 C API first |
| Staffing | Engine and GUI tracks run in **parallel** as independent implement→validate rounds |
| Negative sources | **D-NS1 (user, 2026-08-23)** — water-age and pollutant sources may be negative (extraction). Warn at parse time; clamp at runtime to available mass and warn on first clamp. Full semantics in §3.1 |
| Age as a species | **D-Y4 (user, 2026-08-23)** — water age is a **first-class species in the UI**: usable where pollutants are, inflows prescribable from the UI, and species timeseries plottable. **Reverses** the GUI plan's 2026-08-12 "not fake pollutants" decision for age (temperature unchanged), and does it **without** making age a `[POLLUTANTS]` row — `np` is load-bearing for parity. Rounds ALL LANDED 2026-08-24 — **Z1 ✅ engine `4639be37` · Y2b-1 ✅ `dae4bad` · Y2b-2 ✅ `7a5f732` · Y2b-3 ✅ `9e63357` · Y4 ✅ `94ff3b5`** (+ Y3b `bc4e07c`); AMENDMENT COMPLETE; full record in `AMENDMENT_1_WATER_AGE_AS_SPECIES_2026-08-23.md` |

---

## 1. What the fast path stands on (already landed — do not rebuild)

- **Shared reaction module R1–R4 ✅** — the LARD strategy's Phase 2 (~4 weeks)
  is largely moot; LARD binds to the shared `ReactionSystem` when L3 is ever
  wanted. First-order decay comes from the existing exact-exponential kdecay
  path (R4 precedent).
- **`QualitySolverKind::LAGRANGIAN` is already reserved** and parses to a
  warning (`SimulationOptions.hpp:88`) — X1 replaces the warning with a
  skeleton dispatch.
- **Water age 1D is essentially complete** (A1a–A4, S1–S4, hotstart, `.out`
  column, dry-mask). What LARD needs is only its own age binding (L5/A5).
- **Species-ID reader `06580dd6` + Python binding `d7ce8efb` ✅** — GUI plan
  §6 prerequisite 4 (dynamic output enumeration) is satisfied; G5g is
  unblocked **today**.
- ~~**Engine option keys for G1g exist** … G1g is unblocked **today**.~~
  **⛔ CORRECTED 2026-08-23 (Y0).** The keys existed in the `[OPTIONS]`
  **parser**; the GUI hydrates through the **C API**, whose key dispatch
  did not know them — `swmm_options_set(e, "QUALITY_SOLVER", …)` returned
  `SWMM_ERR_BADPARAM`. The prerequisite was verified in the wrong layer
  (lesson-26 shape at plan level). **Y0 ✅ `948b2840`** adds the seven keys
  to `swmm_options_get/set`; G1g is unblocked by Y0, not by the parser.
  The same trap waits for `openswmm_heat.h` at G4g.
- **Loader seam is already factored** — the seven loader pathways feed both
  engines (D-UT10 parallel accumulators); LARD consumes the same seam.
  Strategy Phase 0 item 3 ("factor addWetWeatherLoads/addRdiiLoads") is a
  **verify, not a refactor**.

## 2. Step 0 — hygiene gates (before or alongside the first round)

| Item | Why it is on this path |
|---|---|
| Push the 7 unpushed commits (`8b7d1cf7`…`d633c53e`, and the three cascade/run-on fixes) | Parallel tracks mean two agents/two trees; unpushed hydrology-moving commits are exactly how the lesson-71 masking recurs |
| Rebuild the library the MCP server loads; re-run O4 (`o4_mcp.out` byte-identical to `o4_cli.out`) | Lifts the standing rule (PROGRESS §2.8); any MCP-driven validation before this is untrustworthy |
| **Not** gating: gate-count reconciliation, G0 sign-off | G0 gates Phase 4 (groundwater) — out of this subplan's scope |

## 3. Engine track (critical path)

Each step is one implement→validate round per the standing loop (handoff +
falsifier sweep + independent checking agent). Estimates are rounds, not
calendar promises.

| # | Step | Source | Deps | Verify (gate) | Est. |
|---|---|---|---|---|---|
| X1 ✅ `24602eb2` | **L0 wiring:** `LagrangianSolver` skeleton + dispatch in `stepRouting`; `LAGRANGIAN` stops warning, runs as a no-op (zero concentrations) | Strategy §12 Phase 0 (trimmed: enums exist, loaders factored) | — | 18/18 parity decks byte-identical with the option absent; no-op run produces zero quality + closed hydraulics ledger | 1 |
| X2 ✅ `8c141a5e` | **L1/L2:** `SegmentStore` ring-buffer slabs (D-L2) + LTD advection + segment merge (§4.5) + junction mixing in toposort order (§4.3) + flow reversal (§4.4) + storage **CMSTR only** + first-order decay via exact-exp kdecay + **D-NS1 contract on the segment loaders** | Strategy §12 Phase 1; §16 D-L2/D-L5; §3.1 | X1 | **G1** single-species parity vs LEGACY (see §5 tolerance note) + **G2** reverse-flow mass balance ≤1e-9 | 2–3 |
| X3 — **split** (E5a/E5b precedent) — **COMPLETE**: **X3a ✅ `647a3603`** (QUALITY_STEP substepping + MAX_SEGMENTS_PER_LINK + dt-reference instrument + reverse-flow deck; X2.viii recorded OPEN — dtq-axis-blind by §2.3 volume freeze, needs a ROUTING_STEP instrument) · **X3b ✅ `b9852cee`** = RWPT proper (Elder {1.44,1.24,0.96,1.28,1.20} across seeds; fixed-quantum pump 10x found+fixed) | **L4 RWPT dispersion:** `ParticleStore` + counter-based RNG (D-L6, deterministic seed) + velocity profiles + cross-section reflection + §5.2 kernel + segment binning (D-L4) | Strategy §12 Phase 4 | X2 | **G4** Taylor/Shang moments: D_L within 5% for τ>0.5, mean position within 1% | 1–2 |
| X4 ✅ `9f155227` | **L5-age:** `__WATER_AGE__` on segments (§8: reserved species, r=1, hours) + **A5 cross-engine gate** LEGACY vs ARD vs LARD | Strategy §8; water-age plan A5 | X2 (X3 not required) | **G6** dead-end age = travel + residence time; A5 compares **outfall nodes** (A1b: link ages differ by definition; ARD carries a 2.25% dt-independent residual — band accordingly) | 1 |
| X5 ✅ `d7b6c079` — **ENGINE TRACK COMPLETE** (X1–X6 all landed) | **A6-min:** C API subset for the water-age source table (`openswmm_water_age.h`: 8 entry points, hours at the boundary, signed per D-NS1, parser's A1a scope rule enforced, save round-trips through the real parser) + **X6 §5.vi's owed W5 gate** (negative API flux extracts/warns/clamps — the `w <= 0` restoration bites four legs) | Water-age plan A6 (subset); GUI plan §6 prereq 5 | — | 5 gates; 9/9 falsifiers bite (vi needed W3's absence leg asserted on the FILE — a zero row parses back identical to absence); corpus 19/19 | 1 |
| X6 ✅ `d79c8bcf` | **D-NS1 in all THREE engines** (LARD included — its X2 defensive floor became the booked clamp): parse warning, per-step clamp-to-available, first-clamp warning + summary, ledger un-books the shortfall; ARD's silent max(0,·) drop fixed | §3.1 | — | 4 gates + flipped A1a row; 19/19 corpus + the §5.vii probe (forced hot-path nudge moved exactly the 5 quality decks) | 1 |

X3 and X4 are parallel-eligible after X2. X5 is independent of all of X1–X4.

**Deliberate deviation, flagged:** the roadmap's A5 row lists `LARD-4` (RWPT)
as a dependency; this subplan runs X4 after X2 only, with the A5 cross-engine
check executed **dispersion OFF** on all three engines (age needs segments +
mixing, not RWPT). If review disagrees, X4 slides behind X3 at the cost of
one round of parallelism.

### 3.1 D-NS1 — Negative source fluxes (extraction) [user requirement, 2026-08-23]

> **STATUS (X6 ✅ `d79c8bcf`):** landed in all three engines at the
> CONSUMPTION seam (mix/store), v1 scope = [INFLOWS] rows + the runtime API
> flux + [WATER_AGE_SOURCES] values. Deviations from the letter of this
> section, recorded in the X6 handoff §9: clamping is per consumption site
> (not literally "inside the loader seam" — available mass is only known at
> mix time); the first-clamp warning is global, not per (element, species);
> [TRANSPORT_SOURCES] negative rows and negative DWF/GW/RDII concentrations
> remain OWED. The ARD loader's silent max(0,·) drop — a ledger break since
> E1 — was found and fixed in this round.

Applies to `[TRANSPORT_SOURCES]` (pollutant mass-rate rows, E5a),
`[WATER_AGE_SOURCES]`, quality rows on external inflows, and the LARD
equivalents (strategy §9) — **one rule at the loader seam**, so both existing
engines and LARD inherit it identically (the D-UT10 claim: every pathway
contributes at the same seam).

1. **Sign semantics.** A negative pollutant source extracts mass from the
   element. A negative water-age source extracts **age·volume** (age is not a
   mass — the extractable quantity is the accumulator's, and the state floor
   is age ≥ 0). Water *withdrawal* (negative flow) is unchanged: it already
   removes mass/age at ambient value and is not routed through this rule.
2. **Parse-time warning** — one warning per negative row naming element,
   species, and value (`ctx.warnings`; note the R1-carried warnings-channel
   asymmetry: API/GUI subscribers will not see it until IO5 — the .rpt and
   the GUI validation panel via hydration are the visible channels for now).
3. **Runtime clamp** — per substep, extraction is limited to the mass
   (age·volume) held in the control volume: ARD cell, LARD segment, node
   store, or LEGACY element. State floors at exactly 0; the clamp must ride
   **inside** the existing non-negativity seam, not as a second floor (the
   E4/R6 lesson — a swept clamp was this program's defect once already).
4. **Runtime warning + ledger booking** — first clamp per (element, species)
   warns; subsequent clamps are counted, with the count and total shortfall
   reported in the `.rpt`. The ledger books the mass **actually removed**,
   not the requested rate — otherwise continuity breaks by construction.
   Extraction lands in the existing removal row family (beside treatment),
   not a new balance (lesson 147: a third self-consistent balance certifies
   nothing).
5. **Gates the implementing rounds owe** (each verified to fail on the
   defective form): (a) negative source on an **empty/dry** element extracts
   zero and warns — liveness, not vacuity; (b) extraction below available
   matches the analytic depletion curve; (c) a clamped run's ledger still
   closes, with the clamped shortfall visible; (d) the parse warning fires
   exactly once per row; (e) pollutants bitwise-unchanged on decks with no
   negative rows (the inertness claim, measured not argued); (f) for age:
   extraction can never produce a negative age at any dt (max-principle
   check, the 7b2dfaae shape).
6. **API + GUI.** X5's CRUD surface accepts signed values (no unsigned types,
   no silent abs()); Y3's editor accepts negatives with an inline warning
   affordance and surfaces the parse warning in the options validation panel.

**Where it lands:** LEGACY+ARD get it in **X6** (below) so the project can
extract mass before LARD is ready; X2/X4 build LARD's loaders to the same
contract from day one rather than retrofitting.

**Corpus obligation:** when X4 lands, add an `age_lard` parity deck differing
from `age_ard` by one line (the corpus convention from
`CORPUS_AGE_HEAT_DECKS_HANDOFF_2026-08-22.md`), so a movement localises.

## 4. GUI track (openswmm.gui, parallel from day 1)

| # | Step | Source | Deps | Verify | Est. |
|---|---|---|---|---|---|
| Y1 ✅ gui `ebf28ae` (engine base `948b2840`) — dialog widgets still lack an automated observer (five of six falsifier rows empty at link level; harness round owed BEFORE Y3) | **G1g:** Simulation Options "Quality & Transport" category — solver combo (LEGACY/EULERIAN_ARD/**LARD disabled + `gapSliceLabel`** until X1), ARD group, Water Age group, `IGNORE_QUALITY` relocation, preferences defaults, engine-version gating | GUI plan §1.1/§3.1/§7 G1 | engine keys ✅ | hydration-contract rows for every key; combo item disabled + tooltip against an old engine | 1 |
| Y2 — **split** (X3a/X3b precedent): **Y2a ✅ gui `dcc20e6`** (species as themeable map attributes, name-keyed `qual:<name>` tokens per D-G1; 6/6 falsifiers; premise verified on a real .out) · **Y2b** = plots/tabular/stats/.oswp round-trip + warn-on-miss | **G5g:** D-G1 dynamic result descriptors — species/age selectable in map theming, profile/time-series plots, tabular results; persist by species **name** in `.oswp` | GUI plan §3.6/§7 G5 | `06580dd6` ✅ | `.out` with pollutants + age exposes all descriptors in every picker; `.oswp` species theme survives reload; legacy `.out` shows no submenu | 1–2 |
| Y3 ✅ gui `f5e0d9b` — editor complete, 8/8 falsifiers automated (the dependency-light-dialog pattern CLOSES the GUI observer hole); **UNREACHABLE until Y3b wires the menu action** | **G3g:** Water Age Sources editor + options wiring; accepts negative values with inline warning affordance, parse warnings surfaced in the validation panel (D-NS1 §3.1.6) | GUI plan §3.4/§7 G3 | **X5** | `[WATER_AGE_SOURCES]` round-trip incl. a negative row; per-scope pickers resolve after rename | 1 |
| Y4 | **LARD enablement (small):** enable the LARD combo item + Lagrangian group (`DISPERSION Off/RWPT`, `MAX_SEGMENTS_PER_LINK`, shared `QUALITY_STEP`) | GUI plan §1.1 | X1 (combo), X3 (dispersion keys) | hydration rows for the new keys | ½ |

Y1 → Y2 → (Y3 after X5) → Y4 trailing the engine keys. Nothing in the GUI
track waits on X2's heavy round.

## 5. Risks and standing constraints carried into this subplan

- **G1's ≤1e-6-vs-legacy tolerance predates lesson A1b.** A segment scheme's
  link values differ from a CSTR's **by definition** (mixed-tank outlet vs
  in-conduit profile). Expect the G1 gate to need the A1b treatment: strict
  comparison at **nodes/outfalls**, definitional differences at links
  documented, not band-widened. Settle this in the X2 handoff, not after.
- **Quality ledger findings (5)/(6)/(8) are live** (washoff summary ÷453592,
  mixed units, `qual_bmp_removal` never written). All LARD parity gates must
  assert **`.out` values**, never `.rpt` quality summary rows, until those
  are fixed.
- **No LID in LARD gate decks** until issue #131 lands (unit conversion —
  every LID number is provisional).
- **Isolated-worktree counts only** (lesson 71) — mandatory with two
  parallel tracks in flight.
- **MCP-driven validation** is untrustworthy until the Step-0 rebuild lands
  (stale-library rule, PROGRESS §2.8).
- **Fixture hygiene:** new LARD test files must use unique fixture names —
  the configure-time collision check (`b85b802d`) will refuse duplicates.
- **⚠ Shared-tree clobber now eats changeset hunks, not just fixtures
  (X1 round, `24602eb2`).** Two SWMMEngine.cpp hunks written by the
  implementing agent were absent from the validator's tree — overwritten by
  a concurrent session's copy of the same file. Standing rule from this
  round: **the implementing agent greps for its own hunks at handoff time
  and lists the expected match counts in the handoff**; the validator
  greps again before building. A missing hunk is re-applied from the
  handoff's spec, not re-invented.

## 6. Explicitly deferred (not lost — still tracked in the roadmap)

L3 (MSX reactions on segments), storage mixing models beyond CMSTR (shared
token with E2b), L6 perf/parallelism pass, L7 docs + default promotion,
A2c (age ledger row), full A6 (Python/MCP age surfaces), G2g (reaction
editor — needs R5), G4g (heat GUI), G6g (2D), G7g (property/MapCommand
round), R4b, E6.

## 7. Critical path summary

```
Step 0 (hygiene)  ──────────────┐
X1 (1) → X2 (2–3) → X3 (1–2) ───┼→ project-ready engine
                  └→ X4 (1) ────┤   (X3 ∥ X4)
X5 (1, any time) ───────────────┤
X6 (1, any time — D-NS1) ───────┤
Y1 (1) → Y2 (1–2) → Y3 (1) → Y4 ┴→ project-ready GUI
```

Critical path ≈ **5–7 engine rounds** (X1→X2→X3/X4) — X5 and X6 ride the
slack, so D-NS1 does not lengthen it. The GUI track (≈3½–4½ rounds) is fully
absorbed in parallel. The single biggest round is X2; nothing else on the
path is novel machinery.
