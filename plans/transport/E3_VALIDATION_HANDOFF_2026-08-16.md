# E3 Implementation — Validation & Commit Handoff (2026-08-16)

**For:** the checking agent (sandbox: `g++ -fsyntax-only` only; nothing
linked/executed).
**Base:** `326b595c` (post-R4).
**Plan:** `EULERIAN_ARD_TRANSPORT_PLAN.md` §6 E3 (dispersion activation:
per-conduit D + FISCHER under all hydraulic solvers), §4 placement per
D-UT8.
**Standing findings:** reconfigure (two new `.cpp`); falsifier sweep as a
TABLE with verified restoration between cases (R4 §5 lesson — do NOT use
`git checkout --` while the changeset is uncommitted); observation-path
check per claimed defense; EXPECT-not-ASSERT in table loops, ASSERT where
later statements depend on the operation; sanitizers where state plumbing
changed.

---

## 1. Changeset (uncommitted)

```
new:  src/engine/data/ArdConfigData.hpp             (ArdDispersionMode + ArdConfigData)
new:  src/engine/transport/components/EulerianArdComponent/ArdConfig.{hpp,cpp}
      (transport.ard component apply hook: [TRANSPORT_OPTIONS] DISPERSION
       OFF|FISCHER|value, [CONDUIT_DISPERSION] name/value rows resolved to
       link indices; E5/E2b deferral errors; never-half-apply reset;
       bypass warnings)
new:  tests/unit/engine/test_ard_dispersion.cpp     (10 gates: 4 kernel + 6 engine)
mod:  src/engine/core/SimulationContext.hpp         (include + ard_config member;
       reset() clears ard_config — reopen-without-component hygiene)
mod:  src/engine/transport/fvkernels/SpeciesTransportKernels.{hpp,cpp}
      (SpeciesKernelView.cell_dispersion; dispersionSolve honours per-cell
       D via face-mean 0.5*(D_i+D_j) — see §2.1 bitwise argument)
mod:  src/engine/transport/components/EulerianArdComponent/ArdEngine.{hpp,cpp}
      (init reads ard_config with ucf_len² conversion mirroring
       Router::initFv; updateDispersion() per routing step — override wins,
       VALUE broadcast, FISCHER D = 0.011 v²B²/(Y·U*), U* = √(gYS), slope
       floor 1e-5, depth floor 0.01 ft, dry/stagnant ⇒ 0; substep step 6:
       dispersionSolve after advection+stores+structures, Strang split)
mod:  src/engine/core/SWMMEngine.cpp                (registerArdComponent();
       LEGACY-fallback warning notes lost dispersion when configured)
mod:  tests/unit/engine/CMakeLists.txt
```

All TUs pass `g++ -std=c++20 -fsyntax-only`.

## 2. Design decisions to review

### 2.1 Kernel extension preserves the FV solver bitwise

`dispersionSolve` gains an optional per-cell coefficient array. Faces use
`0.5*(D_i + D_j)`. With the array null (the FV solver's path — it still
passes only the scalar) both operands are `v.dispersion`, and
`0.5*(D + D) == D` EXACTLY in binary floating point (×2 and ×0.5 are
exponent shifts), after which the expression evaluates in the pre-E3
order. So the FV solver's transport is bit-identical by argument — and
gate `UniformArrayBitwiseMatchesScalar` makes the argument executable
(EXPECT_EQ on doubles). Your 14-deck sha256 discipline is the deck-level
version. The scalar early-out (`v.dispersion <= 0`) is unchanged when the
array is absent, so `ExplicitFvSolver` never enters the new code.

### 2.2 Configuration placement (D-UT8) and what E3 does NOT consume

Dispersion config lives ONLY in `model.ard` (the transport.ard process
component). `[OPTIONS] FV_DISPERSION` still feeds only the FV solver's
in-solver path and keeps its `WARN_FV_OPTION_INERT`; alias unification of
the `ARD_*`/`FV_*` keys is E5/E6 surface work. If you think FV_DISPERSION
should already flow into the ARD engine as a back-compat alias, flag it —
that is a one-line change in ArdEngine::init but a semantics decision.

### 2.3 Engine semantics

- `updateDispersion` runs once per ROUTING step (hydraulics are per-step
  constants under the projection); the SOLVE runs once per SUBSTEP with
  `dt_sub` (matching the FV solver's per-substep call; implicit ⇒ no
  stability coupling either way).
- Per-conduit override wins over the global model in its conduit,
  including `DISPERSION OFF` + overrides = dispersion only there.
- Zero-D cells produce identity rows in the chain tridiagonal, so
  inactive conduits are BITWISE untouched even while the solve runs over
  the whole mesh (gate `HeterogeneousDIsPerCell` + the override gate's
  upstream EXPECT_EQ rest on this).
- `disp_active_ == false` (no component / no dispersion rows) passes a
  null array and the kernel early-outs — pre-E3 ARD decks bit-identical
  (your E1/E2 gates must pass unchanged).
- FISCHER guards: slope floored at 1e-5, depth floor 0.01 ft, dry or
  stagnant conduit ⇒ D = 0. D is deliberately NOT capped above: the
  implicit solve is unconditionally stable and monotone, so the failure
  mode of a huge Fischer value is over-mixing, never overshoot
  (`HugeDIsStableAndBounded` is the plan's implicit-step-restriction
  verify).

### 2.4 Known characteristic (pre-existing, not E3's)

The promoted kernel's tridiagonal conserves exactly on uniform dx; across
splice faces with differing cell dx there is a small flux asymmetry
(each row divides by its OWN dx). Verbatim from the FV solver (E0);
noting it here so the moments gate's uniform-dx choice is understood as
deliberate.

### 2.5 Pre-existing hazard observed, NOT fixed (CLAUDE.md §3)

`SimulationContext::reset()` does not clear `ctx.reactions`: a model with
a reactions component closed and a component-free model opened on the
SAME engine instance would inherit `reactions.configured/compiled` (the
apply hook resets only when a component exists). Same stale-on-reopen
class E3 just closed for `ard_config`. All current gates create fresh
engines per run, so nothing observes it today — your call whether to fix
alongside (one line in reset()) or record for IO5.

## 3. Validation protocol

1. **Reconfigure** (two new `.cpp`), build, zero new warnings from
   touched files.
2. `ctest -R test_engine_ard_dispersion` — ten gates.
   *Anticipated failure modes, likelihood order:*
   (a) **The [INFLOWS] row** `J0 FLOW "" FLOW 1.0 1.0 5` may not parse as
   a constant 5 cfs baseline (column/empty-timeseries convention). If the
   base run's C5 signal is zero (the `norm > 0` ASSERT), fix the deck row
   — do not weaken the metric.
   (b) **Link index assumption** kC1..kC5 = declaration order 0..4 —
   verify; switch to name lookup if wrong.
   (c) **Override gate's upstream BITWISE claim** — DYNWAVE micro
   reverse flow at J2 could advect C3 state upstream, breaking EXPECT_EQ
   with tiny late-step diffs. If so, inspect face_q_ signs at J2 first.
   Downgrading C1/C2 to a tolerance changes the gate from
   mapping-razor to leak-detector — a decision to record, not a silent
   tolerance bump.
   (d) **FISCHER separation below 1e-6·norm** — measure the computed D
   (probe print in updateDispersion) before touching the threshold;
   raising the deck's inflow is the legitimate fix.
   (e) Variance gate 1e-9 band assumes support far from chain ends;
   check spread vs the 100-cell clearance if it fails marginally.
   (f) The CMS deck reuses US geometry numbers as metres — physically
   fine, hydraulically different; if it floods/dries pathologically,
   rescale the deck geometry, not the gate.
3. **Falsifier sweep** (verified restoration between cases; record the
   full table in §5 — an empty row means an unobserved defense):

   | falsifier | expected failing gates |
   |---|---|
   | i. kernel ignores the per-cell array (cellD → `v.dispersion`) | HeterogeneousDIsPerCell; GlobalValue…, SiUnits…, PerConduitOverride… (downstream leg), Fischer… (engine passes scalar 0) |
   | ii. comment `if (disp_active_) updateDispersion(ctx);` | the four engine dispersion gates; kernel gates stay green |
   | iii. comment `fvk::dispersionSolve(v, dt_sub);` in substep | same observers as ii (shared — acceptable, both are engine-plumbing defenses; note it) |
   | iv. skip the override link→conduit inversion loop in init | PerConduitOverride… downstream leg only (upstream EXPECT_EQ stays green) |
   | v. drop the ucf² division in ArdEngine::init | CFS decks blind (ucf=1); the SI gate's measured separation shifts ≈10.8× — record measured metric before/after, that ratio IS the conversion evidence |
   | vi. remove the bypass-warning branch in ArdConfig | BypassConfigurationsWarnLoudly |
   | vii. comment the reset-on-error in applyArdSections | ConfigErrors… `configured == false` assert |

4. **Prior suites all green** — especially `test_engine_ard_transport`
   (disp-inactive bit-identity), the FV solver transport tests
   (kernel-level bitwise contract), and the full reactions suites.
   Sanitizer pass over the new test (fresh per-cell plumbing).
5. **Bit-identity:** all 14 benchmark decks WITHOUT a transport.ard
   component vs base — production lines touched outside the component
   path are: the two ArdEngine call sites (gated on `disp_active_`), the
   kernel (argument in §2.1), and the SWMMEngine fallback-warning string
   (gated on `any_dispersion()`).
6. **Taylor-moments cross-check (plan verify, optional):** the LARD G4
   harness comparison arrives with the LARD phase; the kernel variance
   gate covers the discrete-coefficient identity today. Record as
   deferred if you skip it.
7. Append results to §5; commit with §4.

## 4. Commit message

```
feat(transport): ARD dispersion activation (E3)

Dispersion now runs in the Eulerian ARD engine under all hydraulic
solvers, configured through the transport.ard process component
(model.ard, D-UT8 placement): [TRANSPORT_OPTIONS] DISPERSION
OFF|FISCHER|<value> plus [CONDUIT_DISPERSION] per-conduit overrides
(overrides win, including over OFF). FISCHER evaluates the Fischer et
al. (1979) coefficient D = 0.011 v²B²/(Y·U*), U* = sqrt(gYS) per conduit
per routing step from link hydraulics (slope/depth floors; dry or
stagnant => 0). The shared dispersionSolve kernel gains an optional
per-cell coefficient array (face mean 0.5*(Di+Dj)); the FV solver still
passes the scalar and is bit-identical by the 0.5*(D+D)==D exactness
argument, gated executable in the new suite. Solve is Strang-split after
advection each substep; zero-D cells are identity rows, so inactive
conduits stay bitwise untouched. E5/E2b sections and the remaining
[TRANSPORT_OPTIONS] keys refuse with precise deferral errors; configs
that parse but cannot take effect (LEGACY, IGNORE_QUALITY, no
pollutants) warn at open; a failed apply never half-applies. Gates:
tests/unit/engine/test_ard_dispersion.cpp (10: 4 kernel incl. exact
2*D*dt variance growth and per-cell heterogeneity, 6 engine incl.
per-conduit override containment and bypass warnings).

Plan: EULERIAN_ARD_TRANSPORT_PLAN.md §6 E3.
Validation record: plans/transport/E3_VALIDATION_HANDOFF_2026-08-16.md
```

## 5. Validation results

**Committed as `7684af53` after four fixes.** The R4 lessons landed: the
observation-path discipline in §2/§3 is why seven of the eight falsifiers
mapped cleanly on the first sweep, and the two that did not are recorded
below rather than papered over. 11/11 gates (10 delivered + 1 added), full
suite 134/135, bit-identity 14/14, ASan/UBSan 0.

### 5.1 The four engine gates all failed on a dead deck (§3.2(a), wrong cause)

All four dispersion gates failed the same `ASSERT_GT(norm, 0.0)`. §3.2(a)
predicted this and blamed the `[INFLOWS]` row; the row is fine (measured
external inflow 0.413 acre-ft). The real cause is the JUNCTION INITIAL DEPTH
column, which the deck left at 0.

`initQuality()` seeds Cinit only into elements already WET (legacy
qualrout_init's 1 mm test). With every junction dry at t=0 the links are dry,
Cinit is discarded, and the steady inflow is clean — so the deck held **zero
TSS for the entire run** and every front-based metric divided by zero.
Measured initial stored mass: **0.000 lb at depth 0, 3.491 lb at 1.5 ft**.
Fixed in the deck (`InitDepth` 1.5 ft), no metric weakened; the reason is now
a comment on `write_chain_deck` because the column reads like decoration.

### 5.2 FV_DISPERSION under EULERIAN_ARD is silent — §2.2's premise is wrong

§2.2 states that `[OPTIONS] FV_DISPERSION` "keeps its `WARN_FV_OPTION_INERT`".
It does not. That warning is raised in `Routing.cpp`'s **FV routing** init
arm, so it fires only under `FLOW_ROUTING FV`. Measured on a
DYNWAVE + EULERIAN_ARD deck with `FV_DISPERSION 100`: separation from base
**exactly 0.000000**, and **zero warnings of any kind**.

This is the same silent-bypass class R4 closed, arriving from the opposite
direction: ArdConfig's bypass enumeration only fires for models that HAVE a
model.ard, and this user has no component at all. E3 is what makes it
misleading rather than merely inert — before E3 no engine did dispersion, so
an ignored dispersion key was honest; now the selected engine supports it
under a different spelling.

Answering §2.2's question: **do not alias.** Silently redirecting an
`[OPTIONS]` FV key into the ARD engine would change what the key means for FV
users and pre-empt E5's surface unification. Warning is not a semantics
decision, so that is what was added — `warnIfFvDispersionKeyIgnored`, called
at open next to R4's `warnIfLegacyBindingBypassed`, naming model.ard and E5.
Gate 11 covers it (falsifier viii).

### 5.3 "Strang split" is Lie splitting — three places

The substep runs one full advection step then one full dispersion solve.
That is sequential (Lie/Godunov) splitting with O(dt) splitting error; Strang
means the symmetrized half-step scheme and implies O(dt²). Corrected in
`ArdEngine.hpp` (×2), `ArdEngine.cpp` and the commit message. The scheme is
unchanged — first-order splitting is a reasonable choice when the substep is
already Courant-limited by advection and the dispersion half is
unconditionally stable — but claiming second-order accuracy the code does not
have would mislead anyone tuning the substep. Symmetrizing is an E5 option,
noted in the comment.

### 5.4 The never-half-apply gate could not observe its defense

Falsifier vii (remove the reset-on-error in `applyArdSections`) left **every
gate green**. `EXPECT_FALSE(ctx.ard_config.configured)` cannot see that
defense: `applyArdSections` also resets wholesale on ENTRY, and the error path
returns before `configured = true`, so the flag is false either way. What the
reset actually protects is rows that parsed BEFORE the bad one —
`_e3_err_scheme`'s `DISPERSION 5`, `_e3_err_dup`'s first `C3` row — which
matter under a LENIENT (editor) open, where the engine survives with errors
recorded and a caller can read a half-parsed config back. The gate now asserts
`dispersion_mode == OFF` and `conduit_disp_link.empty()`; falsifier vii then
fails.

Same shape as R4's link-side finding: a gate that names a defense in its
comment but reads a field the defense does not move.

### 5.5 Override containment was not a mapping razor

The override gate proved C1/C2 bitwise-identical upstream and C5 changed
downstream — but an override that landed on C4 instead of C3 also changes C5,
so the pair could not distinguish them. Added an assertion on **C3 itself**
(measured 3.7% of its own signal). The declared-but-unused `kC3i` constant
was the only new build warning, and it was pointing at exactly this hole.

### 5.6 Falsifier sweep (`falsifiers.sh`, one case per invocation)

| falsifier | gates that fail | vs predicted |
|---|---|---|
| i. kernel ignores the per-cell array | UniformArrayBitwise, Heterogeneous, GlobalValue, SiUnits, PerConduitOverride, Fischer | as predicted (+2 kernel gates) |
| ii. `updateDispersion` never called | the four engine dispersion gates | as predicted |
| iii. `dispersionSolve` never called | the four engine dispersion gates | as predicted (shared with ii) |
| iv. skip the override inversion loop | PerConduitOverride | as predicted |
| v. drop the ucf² division | SiUnits | as predicted |
| vi. remove the bypass-warning branch | BypassConfigurationsWarnLoudly | as predicted |
| vii. remove the reset-on-error | ConfigErrors…&nbsp;— **only after §5.4** | **predicted, but unobservable as delivered** |
| viii. remove the FV_DISPERSION warning | FvDispersionKeyUnderArdIsAnnounced | new gate |

### 5.7 Measured separations (`e3_probe.log`)

Integrated |Δconc| at C5 over the run, as a fraction of the base signal:

| case | measured | gate floor | headroom |
|---|---|---|---|
| DISPERSION 100 (ft²/s) | 0.0447 | 0.01 | 4.5× |
| C3 override 200, at C5 | 0.0178 | 0.005 | 3.6× |
| C3 override 200, at C3 | 0.0372 | 0.01 | 3.7× |
| FISCHER (flat C3) | 9.27e-4 | 1e-6 | 927× (liveness check by design) |
| CMS DISPERSION 10 (m²/s) | 0.0300 | 0.01 | 3.0× |

Override containment: C1 and C2 bitwise identical to base across all 721
steps; C3, C4, C5 all differ. §3.2(c)'s worry about DYNWAVE micro reverse
flow at J2 did not materialize — the bitwise claim holds as written.

Falsifier v (drop the ucf² division) is the conversion evidence: the CMS
deck's separation is the only observer, and it fails. Note the SI gate as
written asserts activation, not magnitude — the 10.8× ratio §3.3 predicts is
recorded here rather than gated, because a magnitude assertion on a
hydraulically different deck would be pinning a number, not a law.

### 5.8 Suites, parity, sanitizers

- **11/11** E3 gates; full suite **134/135** (only the known pre-existing
  `FvEngine.RefiningTheMeshConvergesTowardTheDynwaveHydrograph`).
- **Bit-identity 14/14** — ten E0 hydraulics decks + four E2 quality decks,
  sha256 of `.out`, current build vs a `326b595c` worktree build. This is the
  deck-level form of the §2.1 exactness argument; `test_engine_ard_transport`
  (disp-inactive) also passes unchanged.
- **ASan + UBSan**: 0 findings across the dispersion and ARD transport
  suites.

### 5.9 Not done / left alone

- **§3.6 Taylor-moments cross-check**: deferred to the LARD G4 harness, as
  §3.6 allows. The kernel's `VarianceGrowthIsExactlyTwoDDtPerStep` covers the
  discrete-coefficient identity analytically today.
- **§2.5's `reset()` does not clear `ctx.reactions`**: left alone, per
  CLAUDE.md §3. It is a real stale-on-reopen hazard of the same class E3 just
  closed for `ard_config`, but it is R4's surface, not E3's, and no gate
  observes it (every gate builds a fresh engine). Recorded for IO5.
- `updateDispersion` writes `cell_disp_[b + i]` without the `begin < 0` guard
  every other `conduit_cell_begin` consumer uses. Verified safe: the builder
  assigns `begin` and `count` together, so `begin == -1` implies `count == 0`
  and the loop body never runs. Not adding a guard for an unreachable state.
- `DISPERSION 0` parses as VALUE mode with a zero coefficient, so
  `any_dispersion()` is true and the kernel runs a full identity solve every
  substep. Correct, and bitwise inert, but it is work for nothing; a
  short-circuit belongs with E5's surface pass.
