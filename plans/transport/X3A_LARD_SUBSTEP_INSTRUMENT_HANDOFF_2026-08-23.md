# X3a Validation Handoff — QUALITY_STEP Substepping + the dt-Refinement Instrument + the Reverse-Flow Deck

**Date:** 2026-08-23 · **Author:** implementing agent (syntax-only; nothing
executed) · **Step:** subplan X3 **split** (the E5a/E5b precedent): X3a =
options + substepping + the instrument owed by four falsifier rows + the
reverse-flow deck owed since X2 §7. **X3b = RWPT proper**, next round, on
the substep structure this round lands. · **Base:** `9f155227` (X4).

---

## 0. Hunk-presence check

| grep (repo root) | expected |
|---|---|
| `grep -c "QUALITY_STEP\|MAX_SEGMENTS" src/engine/input/handlers/OptionsHandler.cpp` | **2** |
| `grep -c "QUALITY_STEP\|MAX_SEGMENTS" src/engine/core/InpWriter.cpp` | **2** |
| `grep -c "quality_step\|max_segments_per_link" src/engine/core/SimulationOptions.hpp` | **3** |
| `grep -c "substep\|publish(ctx" src/engine/quality/lard/LagrangianSolver.hpp` | **10** |
| `grep -c "QUALITY_STEP is set\|MAX_SEGMENTS_PER_LINK is set" src/engine/core/SWMMEngine.cpp` | **2** |
| `grep -c "^TEST(" tests/unit/engine/test_lard_dt_reference.cpp` | **4** |
| `grep -c "lard" tests/unit/engine/CMakeLists.txt` | **4** |

## 1. Changeset

| File | Change |
|---|---|
| `SimulationOptions.hpp` | `quality_step` (s, 0 = routing step), `max_segments_per_link` (default 100) |
| `OptionsHandler.cpp` | both keys parsed (`parse_time_seconds` / int, the house patterns) |
| `InpWriter.cpp` | both written conditionally (non-default), beside `QUALITY_SOLVER` — the A1a save-as rule |
| `SWMMEngine.cpp` | inverse-direction bypass warnings (E3 lesson 10): either key set under a non-LARD solver warns |
| `LagrangianSolver.hpp` | step() = reversal detect (once) → `nsub = ceil(dt_routing/dtq)` equal substeps → publish (once). Body factored into `substep(ctx, dt, frac)` — **the phase text is MOVED, not rewritten**; the only semantic change inside is the mix denominator consuming `qual_vol_in · frac`. `publish(ctx, dt_routing)` holds the link means + conc_old + empty-slab aging. Slab cap from the option, floored at 2 |
| `test_lard_wiring.cpp` | gate 1: both keys round-trip through save-as |
| `test_lard_dt_reference.cpp` | **NEW** — 4 gates, prefix `_ld_` |
| `CMakeLists.txt` | registered |

## 2. Design decisions (challenge in this order)

1. **Equal substeps** (`dt = dt_routing/nsub`), not a dtq-sized tail — every
   substep identical, composition of exact-exp decay is exact, age totals
   exactly dt_routing.
2. **External volume scales by `frac`, rates by `dt`.** The steady fixed
   point is then substep-count-independent EXACTLY (c* = rate/q_ext at any
   nsub) — gate 3's claim. Consuming full `qual_vol_in` per substep is the
   falsifier and moves the fixed point by nsub.
3. **`v_old` and `links.volume` are frozen across substeps** (the routing
   step's endpoints). The instrument measures the resulting order; do not
   interpolate volumes mid-round.
4. **Reversal detection once per routing step** — the flow solution is
   constant within it.
5. **`nsub` degenerate path (dtq absent/≥ routing step) is one substep with
   `frac = 1` — bit-identical to the X4 engine by construction.** The
   existing three LARD suites are the observers of that claim: they must
   pass UNTOUCHED.
6. **Cap floored at 2 silently.** If you think this should warn, add the
   warning AND a gate leg — not the warning alone (a defence needs an
   observer).

## 3. Anticipated failure modes, likelihood order

1. ⚠ **The two ⚠ PLACEHOLDER bands in gates 1–2** (0.15 × source spread)
   are unmeasured. Your round MUST measure: run each gate's ladder with the
   correct engine and with each constituent falsifier (X2.ii, X2.vi,
   X2.viii, X4.ii — §4 below) and set the band between the correct-form and
   the smallest defective-form error (the `0e8e57df` procedure). Report all
   eight numbers.
2. ⚠ **Gate 4's reversal deck premise.** FIXED stage 11 over dry inverts
   9/10 should flood backward, then forward under 5 cfs. The LEGACY-control
   ASSERTs make a premise failure read as a deck problem; tune stage/spans,
   not the claims. If DW chatter makes `min_flow` noisy around zero,
   deepen the reversal (raise the stage), don't shrink the -1e-3 threshold.
3. **Gate 4's 5% ledger band placeholder** — apply the T3 conventions
   (residual freeze between checkpoints if a flat band is unachievable);
   past 5% is refused.
4. **Gate 1/2 contraction could plateau** if 20 min lands the washout too
   near a rail at some dtq — the liveness ASSERTs catch the rails; adjust
   END_TIME, not the bands.
5. **Instrument ratio**: hydraulics frozen ⇒ the ratio should sit near the
   scheme's true order. Reported, not asserted — if you measure a stable
   ratio across falsifier builds, consider pinning it and record the
   decision either way.
6. **`std::printf` in the instrument** needs `<cstdio>` on some
   toolchains — add the include if the build objects.

## 4. Falsifier sweep

The four owed rows are now this suite's constituency — run each against
gates 1–2 (the falsifier defines which observable moves):

| # | Falsifier (from prior rounds) | Must fail |
|---|---|---|
| i | X2.ii — release at the upstream node's OLD conc | I1 (larger coarse error / broken contraction) |
| ii | X2.vi — passthrough delivers last-substep conc | I1 on an orifice variant if I1 is blind — record which |
| iii | X2.viii — mix reads `nodes.volume` not `old_volume` | I1 |
| iv | X4.ii — age AFTER transport | I2 |
| v | consume FULL `qual_vol_in` every substep (drop `frac`) | I3 (fixed point moves ×8), likely I1/I2 too |
| vi | substep loop runs once regardless of dtq (`nsub = 1` hardwired) | I1/I2 (ladder degenerates: d1 = d2 = 0 → contraction EXPECT fails on equality) — verify this reads as a FAILURE, not vacuous pass; if d1 == d2 == 0 passes EXPECT_GT(d1,d2) is false, good |
| vii | drop the InpWriter hunk | W1 (round-trip) |
| viii | drop the non-LARD warnings | no gate — **expected empty**; W-suite leg owed if you disagree with §2.6's silence |

Any of i–iv that does NOT bite: that constituent stays open and X3b's RWPT
round must not proceed on top of it without a recorded decision.

## 5. Standing verification

Full suite isolated worktree — **the three existing LARD suites must pass
untouched** (§2.5, the degenerate-path claim). Corpus **19/19** (no corpus
deck sets the new keys). ASan/UBSan over all five LARD-touching suites.
Zero new warnings.

## 6. Not claimed / next

RWPT (X3b — ParticleStore, D-L6 counter RNG, velocity profiles, reflection,
G4 Taylor moments — on this round's substep loop). `QUALITY_STEP` under
ARD/LEGACY (warned, deliberately unconsumed). Mid-substep volume
interpolation (§2.3). The X4.vii dry-hotstart gate (still owed program-wide).

## 7. On acceptance

Commit; roadmap L4 row → note X3a landed (options/instrument; RWPT
outstanding); subplan X3 row split recorded; measured bands replace the two
⚠ placeholders IN THE TEST FILE with the measurement cited in comments;
report gates/falsifiers/ratios/counts.

---

## 8. Validation results (2026-08-23, validating agent)

**Committed `647a3603`** on `4005dfce`, branch `swmm6_rel`. Eight files;
`InpWriter.cpp` committed as a clean blob (HEAD + the §1 hunk only — the
2D session's five uncommitted [2D_INFILTRATION*] hunks stay out), built
and run alone: 21/21 across the four LARD suites.

All seven §0 greps passed before building. One configure-time stop: the
shared-fixture guard flagged the literal `"_src.age"` (also in
`test_lard_age.cpp`); the new suite's sidecar is now `tag + ".age"`.

### Gate repairs, all under the handoff's decision rules

- **Gate 1's premise was broken, not its bands** (§3.4's family): at
  END_TIME 20:00 the washout is FINISHED (outfall 1.2e-7 — the liveness
  floor caught it exactly as designed). Measured the front: arrival ~min
  15, through by min 18. `END_TIME 00:17:00` sits mid-front. A ramped-
  inflow variant was probed and REJECTED: the sharper front breaks
  contraction on the correct engine (d1 < d2 at every feasible horizon).
- **Falsifier ii was blind on the conduit deck as §4 anticipated** (the
  passthrough branch never runs). The pre-authorized orifice variant is
  now a second leg INSIDE gate 1 (`DeckSpec.orifice`: C3 → `O3 SIDE`,
  END_TIME 15:00): correct ratio 3.506 / spread 8.178; the stale
  passthrough gives 1.475 / 14.502.
- **Gate 4 passed at the 5% placeholder; pinned from measurement** per
  §3.3: out/in = 1.003449 (the X2 T3 transient-residual family) → band
  1.5% (4.4× measured, 3.3× under the refusal line). The reversal premise
  held on the FIRST deck — no stage tuning needed (LEGACY control
  min_flow < −1e-3 and forward recovery both asserted, live).

### The eight numbers (§3.1), bands at the geometric mean

| leg | correct | defective (which) | band pinned |
|---|---|---|---|
| I1 conduit washout | ratio 4.655, spread **19.057** | **27.206** (v, full qual_vol_in) | **22.8** |
| I1 orifice leg | 3.506, **8.178** | **14.502** (ii, stale passthrough; ratio →1.475) | **10.9** |
| I2 age washout | 2.534, **8.031 s** | **20.552 s** (iv, age-after; ratio →0.393) | **12.8 s** |
| I3 steady invariance | max node move < 1e-5 rel | fixed point ×nsub (v) | 1e-5 rel (as authored) |

Contraction ratios sit at the reported-first stance (§3.5): 4.655/3.506
(pollutant, sharp-front superlinear locally; dtq=5 continues 13.35, ratio
→2.3 ≈ first order) and 2.534 (age). Not pinned — the washout observable
sits on a moving front and the ratio is deck-geometry-sensitive; the
bands carry the discrimination. Decision recorded here per §3.5.

### Falsifier sweep — 6/8 bite, one cannot, one expected-empty

| # | result |
|---|---|
| i | **BITES** — liveness rails at 80.000 ("washout never started"): under LARD `nodes.conc_old` is never advanced, so release-at-old-conc feeds the slabs the INITIAL concentration forever |
| ii | **BITES** the orifice leg's band (14.502 > 10.9); blind on the conduit leg as predicted — recorded: the orifice leg is the observer |
| iii | **CANNOT BITE — X2.viii stays OPEN.** The instrument refines dtq under §2.3's frozen volume endpoints, so old-vs-new volume is dtq-INDEPENDENT by construction: measured spread 19.057→17.952 (smaller!), ratio 4.655→5.288, steady leg exactly invariant (volume == old_volume at steady state). A ramped-inflow deck was probed too: answer moves ≤0.2 mg/L, still no ladder signature. This constituent needs a ROUTING_STEP-refinement instrument (the heat-instrument axis, hydraulic caveat and all). **X3b must not proceed on top of X2.viii without this recorded decision** (§4 rule) — carried to §6. |
| iv | **BITES** — I2 contraction inverts (2.534 → 0.393), spread 8.031 → 20.552 |
| v | **BITES TWICE** — I3 fails (steady fixed point moves ×8) AND the conduit band (27.206 > 22.8) |
| vi | **BITES as a FAILURE, not vacuously** (§4's exact concern): d1 = d2 = 0 on identical ladders → `EXPECT_GT(0,0)` fails on BOTH I1 legs and I2, message prints the three identical answers |
| vii | **BITES** — `LagrangianOptionRoundTripsThroughSaveAs` fails |
| viii | **EMPTY as §4 expects.** §2.6's silence accepted: the floor-at-2 clamp is documented contract in `SimulationOptions.hpp`; no warning without a gate leg. |

### Standing verification

- ctest full suite ×3: only the standing `test_engine_2d_infil_integration`
  (the 2D session's untracked-file issue), all three runs.
- **The three existing LARD suites passed UNTOUCHED through every
  falsifier build and the final form** (§2.5's degenerate-path claim:
  wiring 5, transport 6, age 6).
- Corpus **19/19 bit-identical** including `age_lard` (base = engine files
  at HEAD rebuilt in `build/darwin`, patched = `build/darwin-tests-release`,
  matched configs, guard silent). The dtq-absent path is bit-identical in
  fact, not just by construction.
- ASan/UBSan (`build/darwin-asan`): all five LARD-touching suites clean
  (wiring 5, transport 6, age 6, dt-reference 4, massbalance 15).
- Zero warnings from X3a TUs (the TableData/GeoPackage/Controls warnings
  in the rebuild log are pre-existing, other files).

## 9. Open after this round

- **X2.viii** — OPEN, needs the ROUTING_STEP-axis instrument (see §8
  falsifier iii). The X3b gate for it must accept the hydraulic caveat the
  QUALITY_STEP axis was designed to avoid.
- X3b: RWPT proper on this round's substep loop.
- X4.vii dry-hotstart gate — still owed program-wide.
- `QUALITY_STEP` under ARD/LEGACY: warned, deliberately unconsumed (§6).
