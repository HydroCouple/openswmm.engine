# H6b — bed conduction, deep-ground conduction, hyporheic exchange — Handoff (2026-08-31)

**For:** the implementing/checking agent.
**Base:** `23c1ddfb` (IO3b; 184/184; corpus 21/21).
**Standing findings:** lessons 1–201.
**Reference:** `HydroCouple/HTSComponent` — `src/element.cpp:121-200`,
`src/elementoutput.cpp:93-130`, `src/htsmodel.cpp:48-51`.
**Implemented syntax-only.** `g++ -fsyntax-only -std=c++20` over the real
include tree: **0 errors** in all six changed sources. Nothing built or run.
The pure functions WERE exercised by a throwaway driver — see §5, which is
where that driver falsified one of my own claims.

```
new: src/engine/data/BedZoneData.hpp                                (SedimentConfig, BedZoneState)
new: src/engine/transport/components/HeatFluxModules/BedExchange.hpp/.cpp
new: tests/unit/engine/test_bed_exchange.cpp                        (9 gates)
mod: src/engine/data/HeatData.hpp                    (+sediment_exchange, +sediment)
mod: src/engine/core/SimulationContext.hpp           (+bed_state, +clear, +include)
mod: src/engine/transport/components/HeatFluxModules/HeatFluxes.cpp (the coupled link step)
mod: src/engine/transport/components/HeatModule/HeatComponent.cpp   ([SEDIMENT_EXCHANGE] parse + render + size pin)
mod: src/engine/quality/QualityRouting.cpp           (the solute binding)
mod: src/engine/core/SWMMEngine.cpp                  (two bypass warnings)
```

CMake is `GLOB_RECURSE` (`src/engine/CMakeLists.txt:30`), so the new sources
need no build-file edit. **Confirm that** — it is the cheapest thing on this
list to be wrong about and the most annoying to discover late.

---

## 1. What was missing, and where it came from

The user's report: *"Conduction between pipe and between pipe and deep ground
is missing. Also the Hyporheic exchange is missing."* All three were absent.
`[HEAT_FLUXES] SEDIMENT_EXCHANGE` parsed only to **refuse itself**
(`HeatComponent.cpp:533`, *"arrives with plan phase H6"*), and hyporheic
exchange was in no plan at all.

The reference is four lines. Reproduced because everything below is a
transcription of them:

```cpp
// HTSComponent/src/element.cpp:132-141
mainChannelConductionHeat = alpha*W*L*(T_ch - T)*rho_s*c_s / depth;
groundConductionHeat      = alpha*W*L*(T_gr - T)*rho_s*c_s / groundConductionDepth;
mainChannelAdvectionHeat  = rho_w*c_p*Q_hts*(T_ch - T);
DTDt = (cond_mc + cond_gr + adv + ext + rad*L*W) / (rho_s*c_s*V);
```

`elementoutput.cpp:104,117` returns **`-adv` and `-cond_mc`** to the channel.
That reciprocity is the load-bearing property: the ground term is the only
true source or sink, and everything else moves energy between two bodies this
engine owns.

## 2. ⚠ The plan's cost estimate was wrong, and the wrong estimate was load-bearing

`HEAT_TRANSPORT_PLAN.md:576` and `HeatFluxes.hpp:54` both assert:

> **H6b's `SEDIMENT_EXCHANGE` is one added term in `netFluxOut` and nothing
> else.** There is no longer a shape in which a flux family could acquire a
> binding of its own.

**It cannot be**, for two independent reasons:

1. `netFluxOut` returns **W/m² at the free surface**. Bed exchange acts on the
   wetted perimeter — a different area, and for a **closed** conduit the
   free-surface area is 0 while the bed area is not. Summing them multiplies
   the bed flux by the wrong area and switches it off entirely in a full pipe,
   which is exactly when a buried pipe conducts most.
2. The bed is a **second state variable**. `relaxT` relaxes one body toward a
   *fixed* equilibrium; two bodies exchanging with each other have no fixed
   equilibrium, because each one's target moves as the other responds.
   Treating the bed as a constant-temperature reservoir inside `netFluxOut`
   makes a thin bed behave like an infinite heat sink.

So H6b adds a **coupled stepper**, and D-H5e's rule is why it is coupled
rather than sequential: relaxations do not commute, so the surface families
and the bed are solved in ONE step. **A first draft of `BedExchange.cpp` got
this wrong** — it had an `applyBedHeatExchange` called beside
`applyHeatFluxes`, which is D-H5e's defect reintroduced one phase after
D-H5e removed it. There is now deliberately **no function by that name**, and
the header says why, so the next person cannot reach for it.

**Please update the two stale assertions** in the plan and in
`HeatFluxes.hpp`. Leaving them makes the next flux family's author believe a
cost that is wrong in the expensive direction.

## 3. Found en route — ARD does not relax

`relaxT` has exactly **two** call sites: `HeatFluxes.cpp:98` and
`HeatWatershed.cpp:269`. **`ArdEngine.cpp` has zero** — its cell and
node-store heat steps are plain explicit Euler
(`ArdEngine.cpp:1281`, `1303`: `-flux_out(t_w) * surf_m2 * dt / hc`).

`HEAT_TRANSPORT_PLAN.md` §6.3 states *"Three of four bindings were already
correct — ArdEngine.cpp cells and HeatWatershed.cpp subareas each summed both
modules before relaxing."* The summing is right; **the relaxing is not**. So
the same deck under `QUALITY_SOLVER EULERIAN_ARD` integrates the same physics
with the integrator D-H5d replaced, and can overshoot at a large routing step
in exactly the way D-H5d was created to prevent.

**This is pre-existing and NOT fixed here** — it is a behaviour change on
every ARD heat deck and belongs in its own round with its own dt-sweep
evidence. It is the first thing to triage after this round. It also gates the
ARD half of §4.

## 4. What is bound, and what declines

| | heat | solutes |
|---|---|---|
| `LEGACY` | ✅ inside `applyHeatFluxes`'s link loop | ✅ last stage of `QualitySolver::execute` |
| `EULERIAN_ARD` | ❌ warns by name | ❌ warns by name |
| `LAGRANGIAN` | ❌ warns by name | ❌ warns by name |

The bed binds to the **LEGACY link store only**. The age and heat mirrors are
engine-independent because they write their own state; the bed exchanges **in
place** with whatever array holds channel concentrations, and that is
`ctx.links.conc` (`[link*np + p]`) under LEGACY, cells under ARD, parcels
under LARD.

**Mapping a per-link bed onto either is a modelling decision, not a wiring
one** — which cell does the bed under a 400 ft conduit exchange with? — and I
declined to invent it. `BedZoneData.hpp` records the resolution loss on the
array that would have to be widened, which is where the next person will be
standing.

Both refusals warn at open (`SWMMEngine.cpp`), per the E1-era rule that a
silent no-result configuration is never allowed. **Verify the ARD claim is
true**: confirm `QualitySolver::execute` (and therefore `routeLegacyHeat` →
`applyHeatFluxes`) genuinely does not run under `EULERIAN_ARD`. If it does,
the warning is a lie and the bed is silently half-applied — which is worse
than either honest outcome.

## 5. The physics, and the claim the driver falsified

`relaxPair` solves

```
C_w dT_w/dt = -A_s J(T_w) + G_wb (T_b - T_w)
C_b dT_b/dt =  G_wb (T_w - T_b) + G_bg (T_gr - T_b)
```

by the exact matrix exponential, `u(dt) = dt * phi1(M dt) * c`. The
discriminant is `(a11-a22)^2 + 4 a12 a21` with both off-diagonals `>= 0`, so
the eigenvalues are real and non-positive: **the pair cannot oscillate and
cannot overshoot at any dt.**

`exchangePair` solves the solute pair in (total, difference) coordinates —
total mass invariant, difference decaying exponentially — so conservation is
structural rather than a bookkeeping step.

**A throwaway driver over the two pure functions confirmed:** the reduction
to `relaxT` (1 ULP), conservation (1.3e-16 relative), the no-overshoot bound
and the exact landing on the capacity-weighted mean, the ground equilibrium
(12.000000000 from both sides), one-step-equals-360-steps to 12 digits, and
continuity across the degenerate-branch threshold.

**It also falsified one of my claims.** I had written that solute mass
conservation was *bit-exact*, and the gate asserted `EXPECT_DOUBLE_EQ`. At
`dt = 1e6` it is not: `vol_w*(vol_b*share)` and `vol_b*(vol_w*share)` are the
same factors in a different association and round differently once `share`
saturates. **I corrected the claim, not the tolerance-of-convenience** — the
header, the implementation comment and the gate all now state relative
round-off, and the gate's bound (1e-14 relative) is still orders of magnitude
away from what a genuinely unequal exchange would produce.

Two gate names also said "Exactly" and no longer do.

## 6. Three deliberate divergences from the reference

Recorded in `BedZoneData.hpp` with reasoning; summarised so a reviewer can
disagree with them in one place.

1. **Contact area is the WETTED PERIMETER (`A/R`), not the top width.** The
   reference is an open-channel model where the two coincide. For a full
   circular pipe the top width is **zero**, so the reference's spelling would
   switch the module off precisely when a surcharged pipe conducts most. This
   is the "clear conceptual issue" exception to the parity directive.
2. **Hyporheic exchange is a VELOCITY (m/s), not a discharge.** The reference
   takes `mainChannelAdvectionCoeff` per element from a coupled subsurface
   model (`elementinput.cpp:368`); SWMM has no such supplier, and one absolute
   m³/s applied to every conduit in a network of mixed lengths is not a
   physical statement. `Q_hts = v_hyp * A_bed` scales the way the conduction
   term already does.
3. **Sediment defaults are the STREAMBED pair, 1670 / 1807**
   (`htsmodel.cpp:50-51`) — deliberately NOT `ConductionConfig`'s 1970 / 2758,
   which came from `GWComponent` and describes a bioretention soil column.
   `HeatData.hpp:128` already recorded that these are different materials.

**A fourth thing I did not carry:** the reference defaults
`groundConductionDepth` to **0.01 m** (`element.cpp:38`), which makes the
ground term dominate everything else. That is a placeholder awaiting a coupled
`GWComponent`, not a recommendation; the default here is 2 m. If you disagree,
this is the number to argue about — it changes every result.

**And one the reference has that this does not:** CSH carries a fluid-friction
heat term, `J_f = 9805*Q*S/w` (`CSHComponent/src/element.cpp`). openswmm has no
analogue. Small, real, and out of scope here.

## 7. `[SEDIMENT_EXCHANGE]` — the deck surface

```
[HEAT_FLUXES]
SEDIMENT_EXCHANGE      ON

[SEDIMENT_EXCHANGE]
THERMAL_DIFFUSIVITY    GLOBAL 1.0e-6      ; alpha_sed, m2/s
SOLUTE_DIFFUSIVITY     GLOBAL 1.0e-9      ; D_sed, m2/s   (0 = advection only)
BED_THICKNESS          GLOBAL 0.20        ; Y_hts, m
GROUND_DEPTH           GLOBAL 2.0         ; Y_gr,  m
GROUND_TEMPERATURE     GLOBAL 11.5        ; or: GLOBAL TIMESERIES <name>
HYPORHEIC_VELOCITY     GLOBAL 1.0e-5      ; v_hyp, m/s     (0 = conduction only)
SEDIMENT_DENSITY       GLOBAL 1670        ; kg/m3
SEDIMENT_SPECIFIC_HEAT GLOBAL 1807        ; J/kg/K
INITIAL_TEMPERATURE    GLOBAL 13.0        ; defaults to GROUND_TEMPERATURE
```

Every key is GLOBAL scope for the reason in divergence 2: a per-element
spelling would be a table nothing could fill.

Lengths, diffusivities and material properties are **refused at zero or
below**, not clamped — each is a divisor or a capacity in `BedExchange.cpp`,
and a silently repaired zero produces an infinite conductance or a massless
bed that tracks the water exactly, both of which look like a working model.
`SOLUTE_DIFFUSIVITY` and `HYPORHEIC_VELOCITY` accept zero (each selects a
meaningful configuration) and refuse negatives.

**`GROUND_TEMPERATURE` warns when unstated** rather than refusing — the
`SolarConfig::has_timezone` precedent, not `has_latitude`. It has a real
default, but for a buried conduit it is often the largest term in the balance.

## 8. The save path was closed IN THIS ROUND, deliberately

`saveHeatConfig` renders `[SEDIMENT_EXCHANGE]` and the `SEDIMENT_EXCHANGE ON`
toggle, and `static_assert(sizeof(SedimentConfig) == 88)` sits with the other
three pins (compiler-measured, not hand-computed — IO3b's record says the
first hand computation was wrong twice).

This is lesson 201 applied prospectively. IO3a shipped a renderer that did not
know about H6a's sections and **every radiative deck would have lost its
configuration on first save**; adding a config struct and its serializer in
the same commit is the only shape in which that cannot recur. Both temperature
fields render on their `has_*` flag rather than a value comparison, because
12 °C is an ordinary deliberate choice and the flag is the only record of it.

## 9. Validation protocol

1. **A new deck-level gate must FAIL at base.** A closed circular conduit,
   `HEAT_TRANSPORT YES`, `SEDIMENT_EXCHANGE ON`, `GROUND_TEMPERATURE 5`,
   water entering at 20 °C. At base the deck is **refused at parse**
   (`HeatComponent.cpp` rejected the key), so quote the refusal; with the
   patch the outlet must cool toward the ground temperature. **Refusal is a
   legitimate base failure here — but say so explicitly**, because P1.4's
   round claimed a silent-zeroing defect that turned out to be a parse
   refusal, and I do not want that read twice (lesson 174).
2. **`test_bed_exchange.cpp`: 9 gates, all must pass.** Then re-read gate 6 —
   it was wrong once (§5) and is the one most likely to be wrong again.
3. `ctest -j8` x3 against **184 + 9**. **The corpus is the real check**:
   21/21 `.out` AND `.rpt` byte-identical. No corpus deck sets
   `SEDIMENT_EXCHANGE`, so **any movement means the OFF path changed**, which
   would be a defect in the `HeatFluxes.cpp` link-loop restructure (§4's
   `isOpen` test moved from gating the whole iteration to gating the area
   only) rather than in the physics.
4. **Falsifier sweep:**

   | falsifier | expected |
   |---|---|
   | i. drop the `bed_on` guard so the coupled path runs unconditionally | the corpus moves — pins that the module is genuinely OFF by default and that byte-identity depends on it |
   | ii. replace `relaxPair`'s matrix exponential with forward Euler | gates 3 and 9 fail; **gates 1, 2, 6, 8 still PASS** — a wrong integrator still conserves and still reduces, which is exactly why gates 3 and 9 exist and why the invariant gates alone are not enough |
   | iii. make `a21` use a different conductance from `a12` | gate 2 fails at every dt. **If it does not, reciprocity is not actually being tested** |
   | iv. use top width instead of wetted perimeter in `bedCouplingForLink` | the closed-conduit deck of step 1 stops cooling entirely — pins divergence 1. **Predict before running:** I expect the open-channel deck to barely move and the closed one to go flat |
   | v. remove the `[SEDIMENT_EXCHANGE]` block from `saveHeatConfig` | a save/reopen round-trip loses the bed configuration — pins §8 |
   | vi. add a field to `SedimentConfig` without touching the renderer | the size pin breaks the build with its message |
   | vii. set `SOLUTE_DIFFUSIVITY 0` and `HYPORHEIC_VELOCITY 0` | the solute exchange is a no-op and no NaN reaches a report |

5. **Record:** step 1's base refusal text, falsifiers ii and iv (iv's
   prediction is the one I am least sure of), and the corpus answer.

## 10. Still owed, in priority order

1. **§3 — ARD does not relax.** A live divergence on every ARD heat deck,
   found while reading for this round, fixed in none of it.
2. **§4 — the ARD and LARD bed bindings**, which need §3 settled first.
3. **`[POLLUTANTS]` Kdecay: 1/second here vs legacy's 1/day** — 86,400x on
   every decaying deck. `KDECAY_UNITS_TRIAGE_2026-08-31.md` now exists; this
   has been carried across **six** handoffs. It is a parity defect against the
   reference and it is bigger than anything in H6b.
4. The manual's Chapter 9 documents heat as it was **before** this round. It
   needs §9.3.11 for the bed zone, and Chapter 7 needs the transient-storage
   note. **Do not write it until this round is validated** — documenting
   unverified physics is how a manual comes to describe a model nobody ran.

---

## CHECK RECORD (checking agent, 2026-08-31 → 2026-09-01)

**VERDICT: VALIDATED AND COMMITTED — engine `89310068`** (14 files,
+1393/−29, tree 1946, unpushed). Full evidence:
`tests/output/h6b_bed_exchange/` (PROVENANCE.txt, falsifiers.log,
ctest_log.txt, corpus/ + corpus_fd9f6b94/ + falsifier_i_corpus/).

**The base moved twice.** The changeset (written at 23c1ddfb) was first
A/B'd at be9faeff; six more peer commits landed during the falsifier
sweep, so the entire non-falsifier battery was re-derived on the actual
landing base fd9f6b94 (pristine base2 build, identical refusal; 9/9;
7/7+7/7; ctest 185/185 ×3 clean; corpus 21/21; deck reproduces
19.942831 exactly). Peer/H6b SWMMEngine hunks verified non-overlapping
(stepRunoff:1931 vs open():352).

**Four files the §0 list was missing, added by the checker:**
1. `tests/unit/engine/CMakeLists.txt` — the test was NOT registered; the
   glob at :806 is only the fixture-collision guard.
   `add_gtest_unit(test_engine_bed_exchange test_bed_exchange.cpp)`.
2. `HeatFluxes.hpp` — §2's stale one-term assertion rewritten (the
   handoff asked for the edit but did not list the file).
   `HEAT_TRANSPORT_PLAN.md:576` corrected too (gitignored).
3. + 4. `test_heat_surface_exchange.cpp` / `test_heat_radiative_exchange.cpp`
   — both PINNED the H6 deferral ("must refuse") and failed ctest pass
   1. Flipped to the new contract per the surface gate's own written
   rule (retiring a deferral flips its gate in the same changeset).

**§4 ARD claim: HOLDS, with one nuance to know.** `quality_.execute`
DOES run under EULERIAN_ARD when the transport mesh fails to initialize
(SWMMEngine.cpp:3775) — but that path already warns "falling back to
LEGACY quality routing" by name, so the bed running there is the warned
fallback, not a silent half-application.

**Falsifier sweep (hardened; per-falsifier restore cmp-verified, final
state byte-identical to the shared tree on all 14 files):**
- i BITES: corpus 20/21, mover = heat_parity exactly (heat_lard is on
  H7's own path).
- ii BITES HARDER THAN PREDICTED: gates 1, 3, 4, 9 fail (the handoff
  predicted 1 would pass — forward Euler does not reduce to relaxT's
  exponential, and EXPECT_DOUBLE_EQ has no room). Its core claim stands:
  the invariant gates 2/6/8 cannot see a wrong integrator.
- iii BITES: gate 2 fails — reciprocity is genuinely tested.
- iv BITES in shape: open-flow deck barely moves (19.9428→19.9488);
  surcharged cooling collapses 8× (6.66→0.85 mK). NOT exactly zero: the
  residual is the pre-surcharge FILLING transient (nonzero top width
  while the pipe fills) — a miss that itself confirms the mechanism.
- v BITES viciously: the toggle line survives but the parameter section
  is dropped — a reopen would run the bed ON with silent defaults
  (ground 12 °C instead of the deck's 5). Lesson 201's same-commit rule,
  demonstrated.
- vi BITES: static_assert "96 == 88" with the pin's own message.
- vii CLEAN: zeros deck runs, no NaN, solute exchange a no-op.

**Also verified live (not just by falsifier):** [SEDIMENT_EXCHANGE]
survives swmm_model_write with gen2 == gen3 .heat BYTE-IDENTICAL and the
reopened deck bit-reproduces the outlet temperature. Found en route: a
PRE-EXISTING InpWriter idempotence drift (SWEEP_END 12/31→1/1, "Units
None"→"NONE" on gen2→gen3), present on a no-bed control — recorded as
debt, untouched.

**§10 status:** item 3 (Kdecay) is ALREADY FIXED — KD1 `3aa37c00`
landed the same day this handoff was written, before this check ran.
Items 1 (ARD does not relax) and 2 (ARD/LARD bed bindings) remain the
top debts; item 4 (manual Ch. 9 §9.3.11 + Ch. 7 transient-storage note)
is now unblocked by this validation.
