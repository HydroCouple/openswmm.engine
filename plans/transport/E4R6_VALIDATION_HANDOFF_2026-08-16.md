# E4/R6 Implementation — Validation & Commit Handoff (2026-08-16)

**For:** the checking agent (sandbox: `g++ -fsyntax-only` only; nothing
linked/executed).
**Base:** `7684af53` (post-E3).
**Plans:** `EULERIAN_ARD_TRANSPORT_PLAN.md` §6 E4;
`MULTISPECIES_REACTIONS_MSX_PLAN.md` §5 R6.
**Standing findings:** reconfigure (two new `.cpp`); falsifier sweep as a
table with verified restoration; observation path per claimed defense;
liveness asserts on every divided-by signal (lesson 9 — gate decks reuse
your wet-junction fix); bypass enumeration in BOTH directions (lesson 10);
empty falsifier row ⇒ suspect defense redundancy first (lesson 11);
sanitizers where state plumbing changed.

---

## 1. Changeset (uncommitted)

```
new:  src/engine/transport/components/ReactionModule/ReactionArdBinding.{hpp,cpp}
      (reactArdStage: exact-exp kdecay on cells+stores, MSX per cell (pipe)
       and per store (tank, HRT), PUSH_POLLUT per element, R4-style failure
       containment naming "ARD cell/node store <index>")
new:  tests/unit/engine/test_reaction_ard_binding.cpp   (8 gates)
mod:  src/engine/transport/components/EulerianArdComponent/ArdEngine.{hpp,cpp}
      (state rows = pollutants then MSX when the component is active —
       ns_total = np + rx.n_species(); WALL species refuse with warning →
       LEGACY fallback; cell/store init: pollutant rows np-STRIDED from the
       legacy arrays, MSX rows from init_global; loads loop is np-strided
       (the audit item); reactArdStage call after the subcycle, before
       publish (Lie split per routing step — lesson 13 decision, documented);
       publish splits: pollutant rows → links/nodes.conc (np), MSX rows →
       rx.msx_link_conc/msx_node_conc ([element*nm+m], sized here, R4
       reporting continuity, structures passthrough included); E1 kdecay
       warning REMOVED)
mod:  src/engine/transport/components/ReactionModule/ReactionLegacyBinding.{hpp,cpp}
      (EULERIAN_ARD bypass warning RETIRED — ARD now runs its own binding;
       IGNORE_QUALITY branch kept)
mod:  src/engine/transport/components/ReactionModule/ReactionsComponent.cpp
      (pollutant-kinetics deferral message: phase E4/R6 → R4b, matching the
       roadmap's R4b row; the R4 gate's needle "pollutant kinetics" is
       unchanged)
mod:  src/engine/transport/components/EulerianArdComponent/ArdConfig.cpp
      (no-pollutants dispersion bypass warning now also requires no
       reactions component — MSX species disperse under ARD; tested via
       process_component_specs, which is ORDER-INDEPENDENT w.r.t. the two
       components' apply order)
mod:  src/engine/data/SpeciesRegistry.hpp   (transported_count() counts
       POLLUTANT + MSX_BULK; header note updated)
mod:  tests/unit/engine/test_reaction_legacy_binding.cpp
      (gate 10 EULERIAN_ARD leg FLIPPED: asserts the retired warning is
       GONE; positive coverage moved to the new suite. Gate-4 comment
       phase name updated)
mod:  tests/unit/engine/CMakeLists.txt
```

All TUs pass `g++ -std=c++20 -fsyntax-only`.

## 2. Design decisions to review

1. **Reaction stage cadence: once per ROUTING step**, not per CFL substep —
   the integrator substeps adaptively inside, kinetics see the same dt the
   LEGACY binding uses (so the two bindings agree at the CSTR limit), and
   per-substep integration would multiply the dominant cost by nsub for no
   accuracy gain. Splitting is LIE (first-order), documented per lesson 13;
   the revisit trigger is the LEGACY-vs-ARD convergence data this suite
   starts collecting.
2. **Ordering matches R4**: kdecay (exact exponential) first, then MSX —
   FORMULA sees post-decay pollutants in both bindings.
3. **MSX inflow concentration is ZERO** (sources/BCs are E5): sustained
   clean inflow dilutes MSX stores and the dilution front advects — this is
   the documented default AND the transport observable gate 2 rides on.
4. **WALL species → LEGACY fallback** (no transport semantics for attached
   species; R4 runs them element-locally). Flag if you'd rather hard-error.
5. **Dry cells still react** (carried concentration evolves, negligible
   mass) — matches LEGACY's decay-everything and avoids a wet-mask
   behavioral cliff at kDryArea.
6. **Stores below kMinStoreVol skip MSX tank integration** (no meaningful
   concentration; mass-form kdecay still applies).
7. **The stagnant gate decks are LEVEL POOLS** (flat inverts, FIXED outfall
   stage at the water surface, no inflow): a merely-uninflowed sloped chain
   DRAINS, and draining contaminates the scope gate — stores receive
   pipe-decayed inflow and no longer track their own tank exponential.
   Zero-flow equilibrium isolates every element.

## 3. Validation protocol

1. **Reconfigure**, build, zero new warnings from touched files.
2. `ctest -R test_engine_reaction_ard_binding` — eight gates — plus the
   FULL R4 suite (`test_engine_reaction_legacy_binding`) with its flipped
   gate 10.
   *Anticipated failure modes, likelihood order:*
   (a) **Level-pool decks may not be perfectly static** under DYNWAVE
   (micro-flows from the FIXED-stage boundary or wetting tolerance). Gates
   1/3 tolerate small exchange inside their 2–5% bands; if they miss, probe
   the actual flows first — re-leveling the deck (outfall stage exactly at
   the surface) is legitimate, widening bands is a decision to record.
   (b) **Gate 3's store leg** assumes J1's store stays above kMinStoreVol
   for 2 minutes — check node volume if it fails; switching to another
   junction is fine.
   (c) **Gate 8's failure recipe** (EUL + RATE 1e6·Y) assumes EUL detects
   the blow-up (non-finite or substep cap). If EUL survives it, reuse the
   exact recipe your R4 FailureIsLoudActionableAndNonFatal gate settled on.
   (d) **Gate 2's front timing**: 5 cfs for 1 h should flush the 2500-ft
   chain many times over; if C5's MSX has not dropped below 0.9·8, measure
   the head-store dilution first (is qual_vol_in populated for pure FLOW
   inflow? if not, dilution arrives only via the volume resync — slower but
   still present).
   (e) **Gate 5's bitwise claim**: pollutant arithmetic is per-species
   independent everywhere I audited (kernels loop species independently,
   loads/init/publish are np-strided, resync ratio is volume-only). A
   mismatch here is a REAL stride or ordering defect — do not downgrade to
   a tolerance without locating it.
3. **Falsifier sweep** (verified restoration; record the table — an empty
   row means an unobserved defense):

   | falsifier | expected failing gates |
   |---|---|
   | i. force nm = 0 in ArdEngine::init (MSX off the mesh) | 2, 3, 4, 6 (msx arrays unpopulated/-1) |
   | ii. comment the reactArdStage call | 1 (no decay), 3, 4, 8 |
   | iii. swap the tank flags in reactArdStage | 3 (both legs, values crossed) |
   | iv. pass nullptr pollutants in the cell loop | 4 |
   | v. re-break a stride (use uns for qual_mass_in) | 5 (bitwise), likely 2 |
   | vi. remove the WALL guard | 7 |
   | vii. remove containFailure (silent failure) | 8 |
   | viii. restore the legacy EULERIAN_ARD bypass warning | R4 suite gate 10 |
4. **Prior suites all green** — E3 dispersion suite (disp decks have no
   reactions component: reactArdStage runs kdecay-only with k = 0 ⇒ the
   loop `continue`s, publish np-path identical), E1/E2 ARD gates
   (bit-identity claim: no component + kdecay 0 ⇒ stage is a no-op; decks
   WITH kdecay now DECAY — any E1/E2 deck with nonzero kdecay changes
   results BY DESIGN, check none exists), FV suite, full reactions suites.
   Sanitizers over the new suite (fresh strided plumbing — prime ASan
   territory).
5. **Bit-identity:** benchmark decks without a reactions component and with
   kdecay = 0 vs base. Decks WITH kdecay under LEGACY are untouched (R4
   paths unchanged); under EULERIAN_ARD kdecay decks now decay — expected
   diff, record it.
6. **nh2cl network parity vs EPANET-MSX** (deferred at R4 §3.6 "until
   species are transported" — that is NOW, under EULERIAN_ARD): a small
   network with the nh2cl kinetics, QUALITY_SOLVER EULERIAN_ARD, vs
   EPANET-MSX's published results. This is the E4 plan-verify. If the
   comparison stands up, record tolerances; if you defer again, say what
   remains missing (likely [TRANSPORT_BOUNDARIES] for the inlet BC — E5).
7. **D-R10 profiling obligation** (carried from R3): with reactions now on
   the mesh (cost × n_cells), profile RK5-default vs ROS2/BDF2 on a real
   MSX deck (nh2cl is fine) and record whether RK5 stays the default.
8. Append results to §5; commit with §4.

## 4. Commit message

```
feat(reactions,transport): ARD reaction binding + MSX transport on the mesh (E4/R6)

The Eulerian ARD engine now runs reactions: exact-exponential pollutant
kdecay on every cell and node store (retiring the E1 "kdecay not yet
applied" warning) plus MSX species integration per cell (pipe scope) and
per node store (tank scope, HRT populated) through ReactionIntegrator,
with pollutants readable per element (PUSH_POLLUT). One reaction stage
per routing step, Lie-split after the advection-dispersion subcycle
(first-order splitting, documented; roadmap lesson 13). MSX species are
TRANSPORTED on the ARD mesh - the state carries pollutant rows (np-
aligned) then the MSX rows - so the R4b element-local limitation is now
LEGACY-only; MSX results publish into the R4 element-state arrays for
reporting continuity. Inflow water carries zero MSX until E5's
sources/BCs (documented; dilution fronts are the transport evidence).
WALL species fall back to LEGACY with a warning. The R4-era EULERIAN_ARD
bypass warning is retired (its gate flipped); the pollutant-kinetics
deferral now names R4b; SpeciesRegistry::transported_count() counts
POLLUTANT + MSX_BULK. Gates: tests/unit/engine/test_reaction_ard_binding
.cpp (8, incl. the dilution-front transport razor, the np/ns stride
razor, and level-pool kinetics-isolation decks).

Plans: EULERIAN_ARD_TRANSPORT_PLAN.md §6 E4; MULTISPECIES_REACTIONS_MSX_
PLAN.md §5 R6.
Validation record: plans/transport/E4R6_VALIDATION_HANDOFF_2026-08-16.md
```

## 5. Validation results

**Committed as `4df5cc0f`.** 5/8 delivered gates passed; the three failures led
to **one real production defect** plus two wrong gate premises. Final: 9/9
(one gate added), full suite 135/136, bit-identity 14/14, ASan/UBSan 0.

### 5.1 PRODUCTION DEFECT — MSX store masses lost their non-negativity floor

The single most important finding, and none of the eight delivered gates
could see it.

The E4/R6 stride audit correctly narrowed the node-store load loop from `ns`
to `np`, because `qual_mass_in` is a pollutant array. But the loop body also
contained the store's **non-negativity clamp**, which is a property of a
store, not of a pollutant — and it was narrowed along with the load. MSX
store masses were left unfloored.

Consequence, measured: a node that repeatedly empties — J4, the junction just
upstream of the outfall — accumulated a large oscillating **negative** MSX
mass (`v=0.00000  m0=0.000000  m1=-273.146778`, flipping to +296, −241, +276
across substeps) while its pollutant row stayed correctly clamped at 0. The
node boundary-face override then donated that garbage into the first cell of
the adjoining conduit. Symptom: a **0.67 mg/L** divergence confined to the
outfall-adjacent conduit's MSX rows, oscillating with period 2 and decaying
as the network stabilised. Every pollutant row stayed bit-identical
throughout, which is exactly why gate 5 — the stride razor — could not see
it: gate 5 compares POLLUTANT trajectories with and without MSX present.

Fixed by running the clamp over all `ns` rows. Measured worst |TSS − X| on
an identically seeded inert pair: **6.70e-01 → 1.78e-15**.

The diagnosis path is the reusable part:

1. The failing assertion said "MSX initial seeding on the mesh is wrong". It
   was not a seeding bug at all. **Printing the POLLUTANT beside the MSX row
   settled in one run what reading could not**: at step 0 `TSS/10` and `X/8`
   were both 0.92942 — identical — so the deck simply is not static at t=0.
2. Removing the MSX kinetics entirely reproduced the divergence unchanged ⇒
   not the reaction stage.
3. Two pollutants agreed bit-for-bit while the MSX row diverged ⇒ not generic
   multi-row transport.
4. Two MSX rows diverged equally ⇒ "MSX rows", not "the last row".
5. Six conduits moved the divergence to C6 ⇒ it follows the OUTFALL.
6. ASan clean ⇒ logic, not memory.
7. Dumping the last conduit's cells showed only its FIRST cell differing ⇒
   the upstream node-boundary face ⇒ dump the stores ⇒ found.

### 5.2 Gates 2 and 6 asserted a premise the deck does not satisfy

Both required the last conduit to still hold the MSX initial value at the
first recorded step ("pre-front"). The sloped chain is not static at t=0 — it
drains and mixes immediately, and step 0 already shows ~7% dilution at C5
(7.435 vs 8.0, against a 5% band). This is the E3 lesson again in a new
costume: a gate deck premise, not the binding.

Gate 2 now compares against the pollutant instead —
`c5_msx.front()/GLOBAL == c5_tss.front()/Cinit` to 1e-6 — which is both true
and a stronger seeding claim than "≈ 8.0". Gate 6 (np = 0) has no pollutant
to calibrate against, so its first-step check is now an explicitly labelled
liveness bracket, not a seeding assertion.

### 5.3 Gate 8's failure recipe cannot fail (§3.2(c) anticipated this)

`SOLVER EUL` + `RATE Y 1e6*Y` returned **ok=1, y=4e7, substeps=1**: EUL takes
one explicit step with no error control, so it never reports failure. §3.2(c)
said to fall back to the R4 recipe if this happened; done — default solver
(RK5) with `RATE Y -1000000.0 * Y`, measured ok=0 with the actionable cap
message.

### 5.4 Gate 8 was also redundant with itself (falsifier vii, lesson 11)

Removing the cell-loop `containFailure` left every gate green. `reactArdStage`
contains failures at two call sites (cells, then node stores) and the warning
latches after the first, so the bare needle `"Reaction step failed at ARD"`
let either site cover for the other. Tightened to `"...at ARD cell"`; cells
are integrated first, so the wording is deterministic. Falsifier vii now
fails.

### 5.5 Falsifier sweep (`falsifiers.sh`, one case per invocation)

| falsifier | gates that fail | vs predicted |
|---|---|---|
| i. force `nm = 0` in init | 2, 3, 4, 6, 8, 9 | predicted 2,3,4,6 (+8, +9) |
| ii. remove the `reactArdStage` call | 1, 3, 4, 8 | as predicted |
| iii. swap the tank flags | 3 | as predicted |
| iv. nullptr pollutants in the cell loop | **SIGSEGV, no report** | see below |
| v. re-break the stride (`uns` for `qual_mass_in`) | 5 | as predicted |
| vi. remove the WALL guard | 7 | as predicted |
| vii. remove `containFailure` | 8 — **only after §5.4** | predicted, unobservable as delivered |
| viii. restore the legacy ARD bypass warning | R4 suite gate 10 | as predicted |
| ix. re-narrow the store clamp (§5.1) | 9 | new — gate 9 is its only observer |

Falsifier iv crashes rather than reporting: `evalReactionExpression`
dereferences `env.pollutants` without a null check, so a PUSH_POLLUT token
with a null block segfaults. **Not a shipping defect** — the compiler only
emits PUSH_POLLUT when pollutant names exist, and the binding passes the
block whenever `np > 0`, so production cannot reach the state the falsifier
manufactures. Recorded rather than guarded (CLAUDE.md §2).

Two mechanical notes on the sweep itself, both caught by the R4 discipline:
the post-patch check must compare file CONTENT, not "anchor is gone" — an
insert-style patch whose replacement contains its own anchor tripped the
assertion and left `ReactionLegacyBinding.cpp` patched (repaired, verified
against the diffstat). And `git checkout --` remains unusable as a restore
while the changeset is uncommitted.

### 5.6 Suites, parity, sanitizers

- **9/9** E4/R6 gates; **10/10** R4 suite with its flipped gate 10; full suite
  **135/136** (only the known pre-existing
  `FvEngine.RefiningTheMeshConvergesTowardTheDynwaveHydrograph`).
- **Bit-identity 14/14** vs a `7684af53` worktree build. §3.5 asked whether
  any benchmark deck would legitimately change: both ARD decks
  (`force_ard`, `sdm_struct_dw_ard`) carry `Kdecay 0.0`, so E4's kdecay
  activation cannot move them and no expected-diff row is needed.
- **ASan + UBSan**: 0 findings across the ARD binding, legacy binding and ARD
  transport suites (25 tests).

### 5.7 D-R10 profiling (§3.7) — RK5 stays the default, with data

Chloramine-decay deck (NH2CL with a TOC-mediated term + a FORMULA
by-product), `FV_CELL_LENGTH 10` ⇒ ~250 cells, 6 h. First attempt at the
default cell length put all four solvers inside 43–56 ms — entirely in the
hydraulic noise floor, a real measurement that answered nothing — so the mesh
and horizon were scaled until the reaction stage was visible.

| solver | wall (best of 3) | vs RK5 | max rel. diff vs RK5 |
|---|---|---|---|
| RK5 (default) | 0.731 s | — | — |
| ROS2 | 0.530 s | 0.73× | 0.0 |
| BDF2 | 0.509 s | 0.70× | 0.0 |
| EUL | 0.295 s | 0.40× | 0.0 |

All four agree exactly. Using EUL as the floor, the reaction stage is ≈0.44 s
under RK5 and ≈0.24 s under ROS2 — RK5 costs ~1.9× on the stage, ~1.4×
end to end.

This does not contradict R3's substep table, it reframes it: R3 counted
SUBSTEPS, and here k·dt ≈ 1.8e-3, so every solver accepts a single substep
and cost collapses to per-step function evaluations — Cash-Karp's 6 stages
against ROS2's 2 stages plus a cached 1×1 factorisation. **Recommendation:
keep RK5.** The penalty is ~1.4× on one synthetic deck; RK5's failure mode is
loud and actionable (the substep-cap message) while an implicit solver that
converges to a wrong answer is silent; and swapping a default R3 validated,
on the strength of a single deck, is the sort of unvetted change §5.0 warns
against. Flagged for a broader sweep at E5.

### 5.8 §3.6 nh2cl network parity — DEFERRED AGAIN, with the blockers named

Two independent blockers, not a scheduling choice:

1. **No runnable EPANET-MSX on this machine** — only the EPANET-MSX 2.0 user
   manual PDF. There is no reference to compare against.
2. Even with it, the comparison would be between different problems. An MSX
   network case specifies an INLET CONCENTRATION; under E4/R6 inflow water
   carries zero MSX by design until `[TRANSPORT_BOUNDARIES]`/
   `[TRANSPORT_SOURCES]` land (E5). Reproducing the published case needs that
   BC.

So the R4-era condition ("until species are transported") is satisfied, but a
second one is not. E5 is the earliest phase where this verify can actually
run.

### 5.9 Left alone deliberately

- `reactArdStage` returns silently when `ns_total != n_pollut + nm`. The
  invariant holds by construction (both come from `ArdEngine::init`), but a
  silent return on a broken invariant is the shape this program keeps
  finding. Not changed — no reachable path, and no gate could observe it.
- `SimulationContext::reset()` still does not clear `ctx.reactions` (carried
  from E3 §5.9, recorded for IO5).
- The `EXPECT_FALSE(warned(...))` in the flipped R4 gate 10 is a weak
  assertion by nature — it passes if the ARD binding does nothing at all.
  Acceptable only because the new suite carries the positive coverage; noted
  so nobody reads gate 10 as evidence the binding works.
