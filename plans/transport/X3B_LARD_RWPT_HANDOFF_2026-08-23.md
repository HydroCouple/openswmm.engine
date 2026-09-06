# X3b Validation Handoff — RWPT Dispersion under LARD

**Date:** 2026-08-23 · **Author:** implementing agent (syntax-only; nothing
executed — every numeric claim is a design claim until you run it) ·
**Step:** subplan X3b = strategy §5/§12 Phase 4 under D-L4/D-L6, with
**D-X3b1** (new, §2.1) · **Base:** `647a3603` (X3a).

This is the LAST engine round of the user's original ask (age ✅ X4,
transport ✅ X2, dispersion — this round).

---

## 0. Hunk-presence check

| grep (repo root) | expected |
|---|---|
| `grep -c "rwpt\|RWPT" src/engine/quality/lard/RwptDispersion.hpp` | **26** |
| `grep -c "rwpt_\|RwptDispersion\|release_vol_" src/engine/quality/lard/LagrangianSolver.hpp` | **11** |
| `grep -c "DISPERSION\|RWPT_SEED" src/engine/input/handlers/OptionsHandler.cpp` | **3** |
| `grep -c "DISPERSION\|RWPT_SEED" src/engine/core/InpWriter.cpp` | **3** |
| `grep -c "DISPERSION RWPT is set" src/engine/core/SWMMEngine.cpp` | **1** |
| `grep -c "set_seg_conc" src/engine/quality/lard/SegmentStore.hpp` | **1** |
| `grep -c "^TEST(" tests/unit/engine/test_lard_rwpt.cpp` | **5** |
| `grep -c "lard" tests/unit/engine/CMakeLists.txt` | **5** |

## 1. Changeset

| File | Change |
|---|---|
| `src/engine/quality/lard/RwptDispersion.hpp` | **NEW.** D-L6 counter RNG (splitmix64 hash, pure function of (seed, link, substep counter, particle, draw)); profile kernels as free functions (unit-gateable): log-law/parabola mean-free deviations, Rouse D_t + Itô drift, circular-exact R_h; persistent (ζ, η) particle field in VOLUME coordinates; per-substep upwind-carried inter-segment exchange with the **D-X3b1 equalizing limiter** |
| `src/engine/quality/lard/SegmentStore.hpp` | `set_seg_conc` (the exchange's write path) |
| `src/engine/quality/lard/LagrangianSolver.hpp` | phase 3b between RELEASE and DECAY: per-conduit hydraulics (ū = Q·L/V, h, R_h, the conduit table's own n) → `rwpt_.disperse`; `release_vol_[l]` recorded at RELEASE (the ζ-advection shift); `substep_counter_` (RNG key); lazy `rwpt_.resize` |
| `SimulationOptions/OptionsHandler/InpWriter/SWMMEngine` | `DISPERSION RWPT\|OFF` + `RWPT_SEED` — parsed, saved (A1a rule), warned under non-LARD solvers (inverse bypass; message distinguishes ARD's transport.ard machinery) |
| `test_lard_rwpt.cpp` | **NEW** — 5 gates, prefix `_lr_` |
| `CMakeLists.txt` | registered |

## 2. Design decisions (challenge in this order)

1. **D-X3b1 — particles carry no mass; crossings carry upwind mass with an
   equalizing limiter** (`take ≤ min(donor mass, (c_j − c_n)·v_n)`). Bought:
   maximum principle and zero spurious drift are STRUCTURAL (gate 3 is
   strict, not banded); uniform fields are exact no-ops; mass conserves
   exactly. Cost: the limiter damps exchange where gradients invert within
   one event — a second-order effect on the emergent D_L, absorbed by the
   Elder measurement. If you find the limiter halving D_meas, record it and
   we revisit — do not remove it (that trades a measured bias for an
   unbounded overshoot).
2. **Volume coordinates make bulk advection an exact shift** (ζ += V_in of
   the substep, recorded at RELEASE). Particles that ride out the back are
   recycled at the front as fresh water with a fresh η. No inter-link
   particle transfer (reflection at both ends) — the boundary node's CSTR
   already mixes; recorded.
3. **Elder (5.93·u*·h) is the reference, deliberately** — vertical shear is
   exactly what the kernel resolves. Fischer's field-scale D_L (transverse
   shear) is NOT claimed and would be wrong to compare against.
4. **u\* from Manning at the link's own n** (S_f = (nū)²/2.208R^{4/3}),
   R_h exact for CIRCULAR, ≈ h otherwise (wide-channel v1, recorded).
5. **The age row disperses like every species** — mixing moves age,
   physically. A5's suite regressing untouched is the observer that this
   didn't corrupt the age machinery at DISPERSION OFF.
6. **2000 particles/link, constant** (strategy wants 5000 adaptive — L6
   perf-pass item, recorded).

## 3. ⛔ The X2.viii decision (owed by X3a's round)

X2.viii (mix reads `nodes.volume` instead of `old_volume`) remains OPEN:
its observer needs a ROUTING_STEP-refinement instrument, and this round
deliberately does not build one — RWPT is dtq-axis machinery, and mixing
rs-refinement into this changeset would blur which axis a movement belongs
to. **Recorded decision: the rs-instrument is its own small round after
X3b** (recipe: I1's deck at ROUTING_STEP {40,20,10} with dtq = rs, the
heat-instrument caveat back in force — contraction asserted, ratio
reported). If your round disagrees, build it now and say so.

## 4. Anticipated failure modes, likelihood order

1. ⚠ **Gate 1's Elder band (log-factor 3) is the round's real measurement.**
   MC noise at 2000 particles, the limiter's damping, numerical front
   width, and the wide-channel R_h all push D_meas around. Measure across
   RWPT_SEED {7, 8, 9}; pin at the observed spread ×3, refuse past
   factor 5. If D_meas is SYSTEMATICALLY low, check the limiter (§2.1)
   before the profiles.
2. ⚠ **Gate 1's σ_on > 2σ_off liveness.** σ_off is floored at dt/2; if the
   plug front is wider than expected (merge tolerance), the factor
   shrinks — lengthen the conduit (more spreading distance), don't lower
   the factor.
3. **Gate 3's 1% ledger band** — RWPT adds no ledger terms (exchange is
   internal); if it fails beyond X2's measured envelope, the exchange is
   leaking — check the limiter's debit/credit pairing first.
4. **Gate 2 same-seed bit-identity** — if it fails, something stateful
   crept into the RNG path (D-L6's whole point); suspect `substep_counter_`
   differing between runs (it must count identically — it does not reset
   between runs of the same process? IT DOES NOT RESET — **each `run_deck`
   creates a fresh engine, and `substep_counter_` is a solver member, so
   it resets with the solver. Verify this holds: two engines in one
   process must not share the counter** (they don't — it's per-instance;
   if the gate fails here, that assumption broke).
5. **Turbulent/laminar switch untested by deck** (Re ≈ 1e6 here — always
   turbulent). The laminar branch is pinned only at unit level (gate 4).
   Recorded; a laminar DECK is future work (a trickle-flow deck is
   fragile).
6. **Performance**: 2000 particles × links × substeps is fine at test
   scale; if a gate times out under sanitizers, cut kRwptParticlesPerLink
   in the TEST via MAX_SEGMENTS…? No — it's a constant; record the timing
   and we option it in the perf pass.

## 5. Falsifier sweep

| # | Falsifier | Must fail |
|---|---|---|
| i | zero the shear deviation (`rwpt_u_dev` returns 0) | R1 (σ_on collapses to σ_off; D_meas → 0) |
| ii | drop the Itô drift term | R1's Elder band (particles pile at the walls, D_meas biased) — if R1 absorbs it, record the measured shift; gate 4's FD identity also reddens if you falsify the GRADIENT function instead |
| iii | remove the D-X3b1 limiter (raw upwind take) | R3 (max principle — a tiny receiving segment overshoots) |
| iv | break debit/credit pairing (credit `take/2`) | R3 (ledger) |
| v | seed the RNG from `std::rand` | R2 (same-seed identity) |
| vi | ignore RWPT_SEED (hardwire 0) | R2 (new-seed liveness leg) |
| vii | run RWPT when `lard_rwpt` is false | R5 + the three standing LARD suites (bit-inertness) |
| viii | drop the InpWriter hunk | wiring round-trip is NOT extended this round — **expected empty unless you extend gate W1 with the two keys (recommended, one line each); do it and cite here** |

## 6. Standing verification

Full suite isolated worktree; **the four existing LARD suites must pass
untouched** (DISPERSION defaults OFF — the bit-inertness claim). Corpus
**19/19** (no corpus deck sets the keys — an `age_lard`-style RWPT deck is
NOT owed until the Elder band is pinned; record as future). ASan/UBSan over
all six LARD suites — the RNG/Box–Muller path is new floating-point code;
UBSan on `log(u1)` needs u1 > 0, guaranteed by the +0.5/2^53 construction —
verify, don't assume. Zero new warnings.

## 7. On acceptance

Commit; roadmap **L4 → ✅** (completing Phase 5's engine scope for this
subplan: L0–L2 ✅ X2, L5-age ✅ X4, L4 ✅ here; deferred L3/L6/L7 stand);
subplan X3b row updated; pin the Elder band from measurement; record
lessons; report gates/falsifiers/measurements. **After this round the
engine side of the user's ask is complete** — remaining subplan work is
X5/X6 (parallel, small) and the GUI track Y1–Y4.

---

## 8. Validation results (2026-08-23, validating agent)

**Committed `b9852cee`** on `647a3603`, branch `swmm6_rel`. Ten files;
`InpWriter.cpp` a clean blob (HEAD + the RWPT emission hunk only), built
and run alone: 26/26 across the five LARD suites. All eight §0 greps
passed before building.

### Two engine defects found and fixed

1. **The exchange quantum rectified into a numerical pump — D at 10×
   Elder.** Every boundary crossing moved a fixed V/N (~7.3 cf) while the
   D-X3b1 limiter zeroed the up-gradient legs, so random-walk
   RE-crossings each pumped a full share down-gradient. §4.1's suspect
   ordering pointed at the limiter for a LOW D; the measured HIGH D traced
   to the quantum instead. Fix: quantum = upwind conc × the PENETRATION
   past the boundary, capped at V/N — flux ~ displacement, dt-robust.
   10× → the pinned band. The limiter stays, per §2.1.
2. **`kEtaMin` clamped the laminar parabola too** — gate 4's mean-free
   quadrature failed at 1.5e-6·ū vs the 1e-6·ū band. The floor now
   belongs to the log-law alone (ln 0 is the only thing it guards).

### Gate 1 redesigned, for measured reasons

The prescribed immediate-injection 3000 ft deck cannot host the
instrument: the startup hydraulic transient sheds merge-scale parcels
that arrive as a ~47 s staircase (16/84 quantiles were plateau-vs-level
coin flips — σ_off read 47 s or 25 s on whether one plateau sat above
84%), and at dtq = routing step the η-walk teleports across the depth
(T_mix ≈ 80 s vs dt 5 s), making the emergent D a dt artifact — measured:
the RWPT signal at dtq=1 vanished under the staircase (σ_on 46.98 vs
σ_off 47.11). The redesign: step injection at t = 1000 s (steady
hydraulics; the OFF front is then 5.4 s sharp over 6000 ft), QUALITY_STEP
1 (X3a's key, resolving T_mix), truncated rising-mass second moments, and
D from the EXCESS variance over the OFF control.

### The Elder measurement (§4.1) — the placeholder IS the pin

Seeds {7, 8, 9, 11, 13}: D_meas/D_elder = **{1.44, 1.24, 0.96, 1.28,
1.20}** (σ_on 33.9/31.5/27.9/32.1/31.0 s vs σ_off 5.38 s). Max |ln ratio|
0.364; 3× that is 1.09 ≈ ln 3 — the factor-3 placeholder is exactly the
measured pin, kept, citation in the test. Systematically ~20% high
(wide-channel R_h, the kEtaMin floor, finite-dt walk) — recorded, within
band, not chased. The liveness factor sits at 6.3× against the asserted 2×.

### Falsifier sweep — 7/8 bite, one absorbed with its observer demonstrated

| # | result |
|---|---|
| i | **BITES** — σ_on collapses to σ_off (5.38), D_meas = 0; liveness fails |
| ii | **ABSORBED, as §5 allows**: drift-drop shifts D 1.44 → 1.80 (+24%), inside the band — recorded. The gradient side HAS a standing observer: falsifying `rwpt_d_eta_grad` (ii-b, run for the record) reddens gate 4's FD identity loudly |
| iii | **BITES** — max principle: "a link exceeded the source" (and D collapses to 0.19 — the raw take rectifies the other way) |
| iv | **BITES TWICE** — ledger out/in = 0.41 (R3) and D = 112 (R1) |
| v | **BITES** — same-seed identity fails (engine-instance address as seed) |
| vi | **BITES** — new-seed liveness fails (seed-8 trace ≡ seed-7) |
| vii | **BITES, but not where §5 predicted**: R5 is structurally BLIND to unconditional-on (omit and OFF are corrupted identically, so they still match bitwise). The catchers: gate 1's own control leg (σ_off jumps to 33.57 — RWPT ran in the control) and the dt-reference suite (washout + age gates fail). Recorded: R5 pins the OFF *spelling*, the cross-suite inertness observers are the dt-reference gates |
| viii | **BITES** — W1 round-trip fails. Gate W1 WAS extended with both keys per §5's recommendation (DISPERSION RWPT + RWPT_SEED 42 round-trip), cited here |

### Standing verification

ctest full ×3: standing `test_engine_2d_infil_integration` only. Corpus
**19/19 bit-identical** (DISPERSION defaults OFF — inert in fact). The
four prior LARD suites pass untouched through every falsifier build.
ASan/UBSan over all six LARD-touching suites clean — the Box–Muller
`u1 > 0` guarantee (+0.5/2^53) verified under UBSan per §6, not assumed.
Zero warnings from X3b TUs (a `crossing_time` orphaned by the gate-1
redesign was removed).

## 9. Open after this round

- **X2.viii** — still OPEN; the ROUTING_STEP-axis instrument is its own
  small round (§3's recorded decision, unchanged by X3b).
- An RWPT corpus deck — NOT owed until now; the Elder band is pinned, so
  it becomes ordinary future work (§6).
- L6 perf pass: particle-count adaptivity (2000 constant), maintained-sort
  locate. Suite timing at 2000: gate 1 ≈ 0.3 s, whole suite < 1 s — no
  sanitizer-timeout concern (§4.6).
- A laminar DECK (trickle-flow) — gate 4 pins the laminar branch at unit
  level only (§4.5).
- X4.vii dry-hotstart gate — still owed program-wide.
