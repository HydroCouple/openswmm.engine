# X6 Validation Handoff — D-NS1 Negative Sources (Extraction) in All Three Engines

**Date:** 2026-08-23 · **Author:** implementing agent (syntax-only; nothing
executed) · **Step:** subplan X6 = §3.1 D-NS1, the user's own requirement
(2026-08-23) · **Base:** `b9852cee` (X3b).

**⚠ This round touches LEGACY and ARD code paths for the first time in the
subplan.** Every clamp lives inside a branch taken only when a load is
negative — the bit-inertness argument for the 19-deck corpus is
*untaken branches*, and §5.vii exists to verify it rather than trust it.

---

## 0. Hunk-presence check

| grep (repo root) | expected |
|---|---|
| `grep -c "D-NS1\|NegativeSource" src/engine/quality/NegativeSources.hpp` | **7** |
| `grep -c "bookNegativeSourceClamp\|NegativeSources.hpp" src/engine/quality/QualityRouting.cpp` | **2** |
| `grep -c "bookNegativeSourceClamp\|NegativeSources.hpp" src/engine/transport/components/EulerianArdComponent/ArdEngine.cpp` | **2** |
| `grep -c "bookNegativeAgeClamp\|NegativeSources.hpp" src/engine/transport/components/WaterAgeModule/WaterAgeLegacy.cpp` | **2** |
| `grep -c "bookNegative" src/engine/quality/lard/LagrangianSolver.hpp` | **2** |
| `grep -c "negsrc" src/engine/core/SimulationContext.hpp` | **1** |
| `grep -c "EXTRACTS age-volume\|is not an age in hours" src/engine/transport/components/WaterAgeModule/WaterAgeComponent.cpp` | **2** |
| `grep -c "^TEST(" tests/unit/engine/test_negative_sources.cpp` | **4** |

## 1. Changeset

| File | Change |
|---|---|
| `src/engine/quality/NegativeSources.hpp` | **NEW.** One clamp-bookkeeping seam for all three engines: count, un-book the shortfall from `qual_routing_ex_in` (the ledger carries what ACTUALLY left), first-clamp warning, end-of-run summary |
| `SimulationContext.hpp` | `NegativeSourceStats negsrc` beside `mass_balance` |
| `QualityRouting.cpp` | (a) mixAtNodes: extraction clamps to `c_old·v_old`, branch untaken on non-negative decks; (b) **user API flux accepts negative** (`w == 0` skip, was `w <= 0`) — a DELIBERATE deviation from legacy's positive-only rule, per D-NS1, first negative warns |
| `ArdEngine.cpp` | **the round's engine finding, fixed:** the loader-consumption `std::max(0.0, …)` silently DROPPED negative loads while the ledger booked the full request — a silent ledger break sitting in ARD since E1. Now: signed apply, clamp to store mass, shortfall booked |
| `WaterAgeLegacy.cpp` | age mix clamps extraction to `a_old·v_old`, counted not ledgered (no age row until A2c) |
| `LagrangianSolver.hpp` | X2's defensive floor upgraded to the booked clamp (pollutant rows un-book; age row counts) |
| `WaterAgeComponent.cpp` | negative hours now PARSE (extraction) with a warning; refusal reserved for non-numbers — error text updated |
| `SWMMEngine.cpp` | open(): per-row parse warning for negative `[INFLOWS]` quality baselines; end(): summary before the report renders; `negsrc.reset()` beside `mass_balance.reset()` |
| `test_water_age.cpp` | **DELIBERATE FLIP in ANOTHER suite's gate** (the H1-inversion precedent): A1a's `_a1_e_bad` row expected `GW GLOBAL -1` to be REFUSED — D-NS1 makes it legal-with-warning. The row now uses a non-numeric value to keep the refusal claim, comment cites this handoff. **Verify the flip is surgical: the other 8 rows must be untouched** |
| `test_negative_sources.cpp` | **NEW** — 4 gates, prefix `_nx_` |
| `CMakeLists.txt` | registered |

## 2. Design decisions (challenge in this order)

1. **Clamp at CONSUMPTION, not at the loaders** — the store's available
   mass is only known at mix time, and one seam per engine keeps the
   contract identical (the shared helper enforces identical semantics).
2. **Un-booking goes to `qual_routing_ex_in`** — v1 scope: EXTERNAL_INFLOW
   rows and the API flux are the admitted negative pathways, and both book
   there. Negative DWF/GW/RDII concentrations are OUT of scope (their
   loaders were not touched) — recorded, not silently included.
3. **Age clamps count + warn but are not ledgered** (no age continuity row
   until A2c — inventing one here would be a third self-consistent balance,
   lesson 147).
4. **LEGACY clamp precedes `c_in`/`c_max`** so all downstream expressions
   see the clamped mass — and the branch is untaken when `mass_in ≥ 0`, so
   every existing deck's float sequence is untouched.
5. **[TRANSPORT_SOURCES] negative rows are NOT in this round** — ARD cell
   sources have their own conservation story; the node seam is the shared
   contract D-NS1 names. Owed, recorded.
6. **Timeseries that go negative at runtime** get no parse warning (only
   configured baselines are scanned) — the runtime clamp warning covers
   them. Recorded.

## 3. Anticipated failure modes, likelihood order

1. ⚠ **Gate 1's MASS-row unit assumption.** The extraction rate −100 is
   sized against a throughflow of q·C = 500 internal units assuming the
   MASS row's value lands unconverted in `ext_qual_mass`. If legacy MASS
   units conversion (lbs/day etc.) applies upstream, the 20%-extraction
   arithmetic is off — recalibrate the ROW VALUE from a measured control
   (print `mb_ex_in` for a positive row first), don't touch the bands'
   structure.
2. ⚠ **Gate 2's 5% closure placeholder** — clamp-period bookkeeping is
   quantized by the routing step; measure per engine, pin per the standing
   rule, refuse past 5%. If LEGACY closes at 1e-6 and ARD at 3%, pin them
   SEPARATELY and record why.
3. **Gate 1's clamp_events == 0 expectation at 20% extraction**: transient
   startup (first steps, near-empty stores) might legitimately clamp a few
   times before the chain wets. If so, relax to "no clamps after minute 10"
   (a windowed count needs a small accessor — or assert
   `shortfall_mass < 0.1% of extraction`), and record.
4. **ARD gate 1**: the fixed max(0,·) means ARD extraction WORKS now —
   if ARD's outfall matches the control exactly, the new signed path is
   not being reached (wrong consumption site?).
5. **Gate 4's age deltas** ride the X4 seeding machinery — if base
   `outfall_age < 60`, the deck premise broke (travel time), not the
   claim; re-time per the X4 gate-4 pattern.
6. **The A1a flip** (§1): if any OTHER `test_water_age` gate reddens, stop
   — that suite is landed property and this round may only touch the one
   row.

## 4. Gates

N1 NegativeInflowExtractsUnderAllThreeEngines · N2
OverExtractionClampsWarnsAndStaysNonNegative · N3
WarningsFireExactlyOnNegativeConfigs · N4
NegativeAgeSourceLowersAgeAndFloorsAtZero — plus the flipped
`_a1_e_bad` row in test_water_age.cpp.

## 5. Falsifier sweep

| # | Falsifier | Must fail |
|---|---|---|
| i | restore ARD's `std::max(0.0, …)` (drop the signed path) | N1 (ARD leg: outfall == control) |
| ii | clamp without un-booking (drop the `ex_in += shortfall`) | N2 (ledger diverges — THE load-bearing row) |
| iii | un-book WITHOUT clamping (book but leave mass negative) | N2 (min_conc < 0) |
| iv | drop the first-clamp warning | N2 (warning leg) |
| v | drop the end() summary | N2 (summary leg) |
| vi | restore the `w <= 0` API skip | no deck-level gate (API flux needs a runtime call) — **expected empty**; the API-driven gate is owed to X5's round where the C API surface is in hand; record |
| vii | make the LEGACY clamp branch unconditional (`if (true)`) | corpus movement / suite diffs — this is the bit-inertness probe: run the full suite + corpus with the branch forced and CONFIRM movement, then restore (proves the untaken-branch argument is what protects the corpus) |
| viii | drop the age-source parse warning | N3(a) |
| ix | refuse negative hours again (restore `out >= 0`) | N4 (deck fails to open) + the flipped A1a row |

## 6. Standing verification

Full suite isolated worktree — **especially `test_engine_water_age` (the
flipped row) and every prior LARD suite**. Corpus **19/19 byte-identical**
— the strongest claim of the round: LEGACY's mixAtNodes and ARD's loader
consumption are HOT PATHS on every quality deck, and the argument is
untaken branches (§5.vii is its probe). ASan/UBSan over the new suite +
water_age + ard_node_store + lard_transport. Zero new warnings.

## 7. Not claimed / owed

[TRANSPORT_SOURCES] negative rows (ARD cells) · negative DWF/GW/RDII
concentrations · the API-flux gate (X5, where the C surface is in hand) ·
GUI affordances (Y3's editor shows the warnings — the GUI round) · A2c age
ledger row. The `.rpt` summary line placement (warnings block) is
implementation-verified only if the summary appears in a gate deck's .rpt —
check one manually and note it.

## 8. On acceptance

Commit; subplan X6 row → ✅; update §3.1's status note; record lessons
(the ARD silent-drop finding deserves one — it predates this subplan);
report gates/falsifiers/corpus/pins. Remaining subplan work after X6:
**X5** (A6-min age C API — carries §5.vi's owed API-flux gate) and the
GUI track Y1–Y4.

---

## 9. Validation results (2026-08-23, validating agent)

**Committed `d79c8bcf`** on `b9852cee`, branch `swmm6_rel`. Eleven files, no
clean-blob needed this round (InpWriter untouched; SWMMEngine's three hunks
are all X6's). All eight §0 greps passed. `test_engine_water_age` 17/17 —
the A1a flip is surgical.

### Gate repairs, each on a measurement

- **§3.1 hit exactly as predicted**: the −100 MASS row moved the outfall
  0.71%, identical across all three engines — the row value divides by
  **28.316846592** (MASS rows are mg/s; internal mass is mg/L·ft³). Gate 1
  now extracts a calibrated 100 internal units/s (20%); gate 2 a true 10×.
- **§3.3's startup clamps are real physics** (J2 is dry-store until the
  front arrives ~min 7; 10/46/88 events measured): gate 1 pins
  "no clamps after minute 10" via a mid-run counter capture.
- **Gate 2's out/in was ill-conditioned at true 10×** — the net booked
  inflow is nearly zero (LEGACY netted 184k of 7.2e6 gross), so a 4.6k
  absolute error read as a fake 2.5% (and LARD as 11%). The closure is now
  **|out−in| / gross inflow** (the deck pins gross exactly): measured
  0.00064 LEGACY / 0.0033 ARD / 0.0033 LARD, band 1% = 3× the worst.
- **Gate 4's premise was structurally weak, not mistimed**: extraction at
  J0 against an INITIAL_STATE seed measured −3.3 s at the outfall — J0's
  own age washes to ~0 in seconds, so there is nothing left to extract
  when its water transits. The age-volume loaders apply EXTERNAL_INFLOW as
  q·age at nodes WITH inflow, so the deck now runs DWF (+2 h) concurrent
  with the external row (−0.5 h): J0 mixes 3600 s vs 2700 s — a 900 s
  signal, asserted at half.

### Falsifier sweep — 8/9 bite, one expected-empty, two rows corrected

| # | result |
|---|---|
| i | **BITES** N1 + N2 (ARD legs) |
| ii | **BITES** — but only after gate 2's recalibration. At the original −5000 (really 0.35×) the un-booking was a ~1% effect hiding inside the band; at true 10× the full request leaves `in = −64.8e6` and the ASSERT names the diagnosis |
| iii | **BITES at the ARD site** (miss = 0.97 — the store goes negative and mass vanishes). **The LEGACY site is UNOBSERVABLE for this row, by algebra**: its mix ends in `c_new = max(c_new, 0)`, and clamp-to-−avail vs floor-negative-numerator are bitwise identical there (both give exactly 0). The falsifier was re-targeted and the finding recorded — the LEGACY clamp's real content at that site is the BOOKKEEPING, which the floor does not do |
| iv | **BITES** N2 (warning leg) |
| v | **BITES** N2 (summary leg) |
| vi | **EMPTY as predicted** — the API-flux gate rides X5's C-surface round, recorded |
| vii | **THE PROBE, run as §5 demanded**: a hot-path float nudge (×1.000001 in the mix, the branch's literal `if(true)` would have been vacuous — taking the branch executes no float ops on positive loads) moved **exactly the 5 quality decks** (sdm_fv_o1/o2/o2_superbee 8.8–9.8 kB, force_legacy + orif_legacy 536 B) and none of the hydraulics/age decks. The untaken-branch argument is now proven, not assumed |
| viii | **BITES** N3(a) |
| ix | **BITES THREE WAYS** — N4, N3, and `WaterAgeTest.ConfigErrorsArePrecise` (the flipped row's suite) |

One process slip worth recording: the first sweep's last variant (ix) was
left applied while gate 2 was being recalibrated — gates 3/4 reddened from
contamination until the sha-verified restore caught it. The runner now ends
every sweep with an explicit `variants.py pristine`.

### Cross-engine observation (recorded, not a defect)

At 10× over-extraction the three engines ACHIEVE different extraction
(net ex_in: LEGACY 184k ≈ LARD 213k, ARD 4.6e6 — ARD's store-level clamp
caps harder). Each closes its own books; the contract (clamp, count,
un-book, close) holds in all three. The achieved-extraction difference is
inherent to consumption-site clamping.

### Standing verification

ctest full ×3: standing `test_engine_2d_infil_integration` only. Corpus
**19/19 byte-identical** — the round's strongest claim, and §5.vii's probe
proves the protection is real. ASan/UBSan clean over negative_sources,
water_age, ard_node_store, lard_transport. Zero new warnings (the
QualityRouting IVDEP-pragma notes pre-exist at HEAD). The D-NS1 summary
line verified present in `_nx_over_LEGACY.rpt` (§7's manual check).

## 10. Open after this round

[TRANSPORT_SOURCES] negative rows · negative DWF/GW/RDII concentrations ·
the API-flux gate (X5) · per-(element,species) first-clamp warnings (v1
warns once globally) · A2c age ledger row · X2.viii rs-instrument ·
X4.vii dry-hotstart gate. Remaining subplan: **X5** + GUI Y1–Y4.

---

**§9 addendum (X5 round, 2026-08-23): falsifier vi's owed observer landed
and bites.** `test_water_age_api` gate W5 drives a negative runtime flux
through `swmm_node_set_quality_mass_flux`; restoring the `w <= 0` skip
(X5 falsifier vii) fails FOUR legs at once — the moderate leg reads
`mod == base == 100.0` exactly ("a negative API mass flux had NO effect"),
the warning leg, the extreme-clamp counter (0), and the monotonicity leg.
Committed in `d7b6c079`.
