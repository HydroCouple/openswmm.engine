# H7b — temperature rides the LARD segments — Round record (2026-08-30)

**Step:** `FINALIZATION_SEQUENCE_2026-08-29.md` step 2, round 2 of 2 — **H7
complete**; step 3 (`openswmm_heat.h`) and G4g unblock.
**Base:** `ee7494ea`. Implemented and checked in one session, in an isolated
worktree; every number below is from that tree. Artifacts:
`tests/output/h7b_lard_heat/` (falsifier logs, corpus, patch).

## What landed

`rowLayout()` assigns `temp_row` after age (the slot H7a reserved), and the
row takes every generic stage — drain, mix, passthrough, release — sourced
from `node_temp_vol_in` (the D-UT10 twin, all seven loaders; routeLegacyHeat's
convention, consumed by the LARD mix instead), published to `heat_state`.
`publish()` joins `rowLayout()`, closing the fourth layout-aware site H7a
flagged. The bypass warning is DELETED, not narrowed.

**Decisions, recorded:**
- **RWPT dispersion: temperature participates, at the solute coefficient** —
  the ARD engine's deliberate choice for its own temperature row
  (`ArdEngine.cpp:107`), adopted for cross-engine consistency (user decision
  2026-08-30). Gate 4 is that decision's observer.
- **Temperature is exempt from the D-NS1 non-negativity clamp** — sub-zero
  degC is an ordinary state; the same reasoning as the report boundary's
  deliberate no-mask.
- **It neither ages nor decays** — +dt is the age row's law; the decay
  stride is pollutant-rows-only. Falsifiers v/vi below observe both.
- **Seeding = routeLegacyHeat's**: INITIAL_STATE once (`legacy_seeded`
  shared, so a LEGACY fallback never re-seeds), then `__TEMPERATURE__`
  overrides; link temperatures collapse to one segment per link (lesson 37).

## Two of the sequence's assumptions were wrong, and the gates say so

1. **Gate (c) "hotstart round-trips the temperature row bit-identically" has
   no substrate**: `HotStartNodeRecord` carries an `age` field and NO
   temperature field — no engine persists temperature (LEGACY and ARD
   included). A restarted run re-seeds from INITIAL_STATE, identically
   across engines. Gate 3 pins that shared behaviour so the debt stays
   visible: the day the record gains the field, the gate fails and its
   replacement asserts the round-trip. **Widening the hotstart format is
   its own round** (the A2a precedent), owed to step 9's debt list.
2. **Gate (b) "dry segments keep their state while the report masks it" is
   half wrong for temperature**: there is deliberately NO dry-element mask
   on temperature (0 degC is an ordinary value — documented at the snapshot
   builder). And the empty-slab HOLD itself has **no reachable observer**:
   DYNWAVE never lands a link volume on exactly zero mid-run, and a
   bone-dry deck skips the routing — and with it the quality stage —
   entirely, so even a falsifier that zeroed the held state could not be
   seen (measured: the state-mask falsifier left the bone-dry gate green).
   The hold is write-nothing by construction; the gate slot went to the
   decay-stride claim instead, which IS observable (falsifier vi).

## Gates (test_lard_heat, 4)

1. `TemperatureAdvancesAndMatchesTheLegacyControl` — 30 degC inflow into a
   10 degC chain; LARD lands on LEGACY within 1 % at every link, both at
   the inflow temperature by steady state.
2. `TemperatureDoesNotDecayWithThePollutant` — TSS at k = 1e-3 1/s decays
   (premise ASSERT), the outfall stays at 30 degC. NOTE: this engine's
   `[POLLUTANTS]` Kdecay is applied as 1/SECOND (no parse conversion;
   `_lt_decay_lard` uses 1e-3 the same way) — legacy's field is 1/day; a
   parity question for its own round, recorded here because the first
   draft of this gate assumed legacy units and got 99.9 % decay.
3. `HotstartReseedsTemperatureFromInitialState` — via the hotstart C API
   (the `[FILES] USE HOTSTART` route did not restore in-process; the
   test_water_age convention used instead).
4. `RwptDispersionMovesTemperature` — RWPT on-vs-off separates the
   mid-chain trajectory; both settle at the inflow temperature.

Plus the wiring FLIP: `BypassWarningFires…`'s heat leg now asserts the
warning's ABSENCE (lesson 21).

## Falsifiers, all observed

| # | change | bite |
|---|---|---|
| i | bypass warning restored | wiring gate fails ✓ |
| iii | INITIAL_STATE reseed broken (fill 0) | gate 3 fails ✓ |
| v | temperature row aged (+dt) | gate 1: LARD 122–500 degC vs LEGACY 30 ✓ |
| vi | decay stride widened to the row | gate 2: outfall at 12.97 degC ✓ |
| ii (state mask) | empty-slab reset to 0 | **no gate can see it** — the branch is unreachable; recorded above, not gated vacuously |

## Corpus and suite

- `heat_lard.inp` joins the corpus (heat_parity + `QUALITY_SOLVER
  LAGRANGIAN`, the age_lard convention; shares the .heat sidecar). A/B vs
  base `ee7494ea`: **20/21 identical, only heat_lard moves**; its base
  `.rpt` carries the bypass warning, the patched does not, and **every
  continuity line is identical** — the movement is the temperature columns
  lighting up. Corpus census is now **21**.
- ctest **180/181 ×3** (test_lard_heat registered; the one red is
  `fv_tpa_closure`, the peer's, since `47c00ae3`). Standing figure:
  **180/181 at this commit**.

## Owed onward

- Hotstart temperature field (all engines) — step 9.
- `[POLLUTANTS]` Kdecay units parity (1/s here vs legacy 1/day).
- Step 3: `openswmm_heat.h`; G4g heat editor (GUI) unblocks.
