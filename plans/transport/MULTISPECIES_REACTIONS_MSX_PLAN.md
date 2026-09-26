# Multispecies Reactions Plan (EPANET-MSX Conventions, Shared Module)

**Status:** Approved direction, 2026-08-12
**Parent:** `plans/transport/UNIFIED_TRANSPORT_MASTER_PLAN.md` (Phase T1; decision D-UT3)
**Supersedes in part:** ownership of the reaction system by LARD
(`plans/LAGRANGIAN_QUALITY_STRATEGY.md` §6, §9.2) — the design there is kept
almost verbatim but **factored out** into a solver-agnostic module consumed by
Eulerian ARD, LARD, legacy qualroute, FV in-solver transport, and 2D.
**API design basis:** `plans/LAGRANGIAN_QUALITY_API_STRATEGY.md` (two-tier
authoring), renamed from `lard`-specific to neutral surfaces.

---

## 1. Goal

One reaction subsystem, MSX-equivalent in expressive power, that every
transport engine calls instead of implementing chemistry:

- The **FV plan already mandates this**: "the FV path should call the same
  reaction bytecode VM rather than reimplementing chemistry"
  (`EXPLICIT_FV_KOKKOS_1D_SOLVER_PLAN.md` §3.2).
- The legacy `QualitySolver` gets it as a replacement for `applyDecay()`
  when MSX species are declared (kdecay pollutants become implicit
  `RATE = -k*C` expressions, preserving bit-parity when no MSX input exists
  — gate G-UT1).

## 2. MSX conventions adopted

Follows EPANET-MSX input/semantics conventions, namespaced per the LARD plan
to avoid collisions with SWMM sections. **Placement (D-UT8):** these sections
live in the reactions component's external config file (`model.rxn`),
registered via `[PROCESS_COMPONENTS] ... config="model.rxn"` — not in the
legacy `.inp` (embedded fallback with style warning only). See
`TRANSPORT_IO_PLUGIN_CONFIG_PLAN.md` §3.

```
[REACTION_OPTIONS]      AREA_UNITS FT2|M2|CM2 ; RATE_UNITS SEC|MIN|HR|DAY ;
                        SOLVER EUL|RK5|ROS2|BDF2 ; COUPLING NONE|FULL ;
                        TIMESTEP s ; ATOL / RTOL (global + per-species)
                        ; BDF2 is an in-house extension beyond MSX's EUL/RK5/ROS2
                        ; (recorded deviation) for stiff kinetics — D-R7
[REACTION_SPECIES]      BULK|WALL  name  units  [atol rtol]
[REACTION_COEFFICIENTS] PARAMETER|CONSTANT  name  value
[REACTION_TERMS]        name  expression          ; named intermediate terms
[REACTION_PIPES]        RATE|EQUIL|FORMULA  species  expression   ; conduits
[REACTION_TANKS]        RATE|EQUIL|FORMULA  species  expression   ; storage nodes
[REACTION_SOURCES]      CONC|MASS|FLOWPACED|SETPOINT  node species value [pattern]
[REACTION_QUALITY]      GLOBAL|NODE|LINK  species  value          ; initial conditions
[REACTION_PARAMETERS]   PIPE|TANK  element  param  value          ; per-element overrides
[REACTION_PATTERNS]     name  multipliers...
[REACTION_REPORT]       reporting toggles
```

Semantics per MSX: `RATE` species integrate dφ/dt = expr; `EQUIL` species
satisfy 0 = expr (algebraic, solved per step); `FORMULA` species are
explicit functions of other species. WALL species attach to wetted
perimeter with `AREA_UNITS` scaling (`Av` available as a hydraulic
variable). Pipe vs tank expression sets mirror MSX's `[PIPES]`/`[TANKS]`.

**SWMM-specific extensions** (recorded deviations from MSX):

- `[REACTION_SUBCATCHMENTS]` — optional rate expressions applied to ponded/
  runoff water on subcatchments (enables surface reactions and supports the
  age/heat modules; default: none, buildup/washoff untouched).
- Hydraulic variables available in expressions: `D` (depth), `Q`, `U`
  (velocity), `Re`, `Us` (shear velocity), `Ff` (Darcy–Weisbach), `Av`
  (surface area per volume), `HRT`, `DT` — union of MSX's set and the LARD
  plan §6 list; identical across engines.
- Reserved species `__WATER_AGE__` and `__TEMPERATURE__` (see age and heat
  plans) are pre-registered and may appear in user expressions (e.g.,
  Arrhenius `RATE` terms referencing temperature: `k20 * POW(theta, T-20)`).

## 3. Architecture

### 3.1 Module layout

```
src/engine/transport/components/ReactionModule/
  ReactionSystem.{hpp,cpp}       // registry: species, params, constants, terms, exprs
  ReactionCompiler.{hpp,cpp}     // DSL → RPN bytecode (extends quality/Treatment VM)
  ReactionVM.hpp                 // stack/register evaluator, SIMD batch entry point
  Integrators/{Euler,RK5,Rosenbrock2,Bdf2}.hpp
                                 // Bdf2: hand-rolled fixed-order BDF2, Newton +
                                 // per-cell dense LU (shared with ROS2), A/L-stable
                                 // — stiff coverage without SUNDIALS (D-R7)
  AlgebraicSolver.{hpp,cpp}      // damped Newton — IEquilibriumSolver (only v1
                                 // backend; KINSOL demoted per D-R4/D-R7)
src/engine/data/ReactionData.hpp // SoA: per-element params, species state views
src/engine/input/handlers/ReactionHandler.{hpp,cpp}
include/openswmm/engine/openswmm_reactions.h
python/openswmm/engine/_reactions.{pyx,pxd,pyi}
```

Names/structure intentionally match `LAGRANGIAN_QUALITY_STRATEGY.md` §5 —
just lifted out of `quality/lard/` into the shared tree; the LARD plan text
is amended to reference this module (see cross-reference edits).

### 3.2 Expression VM decisions

- **D-R1:** Extend the existing shunting-yard/RPN infrastructure of
  `src/engine/quality/Treatment.cpp` (per LARD plan §6.2): new token types
  `SPECIES[i]`, `PARAM[i]`, `CONST[i]`, `TERM[i]`, hydraulic variables, and
  `IF/STEP` — one compiler, two front doors (treatment expressions keep
  their existing variable set; reaction expressions get the extended set).
- **D-R2:** Adopt the `PROCESS_MODULARIZATION_PLAN.md` §10 optimization
  ladder for the evaluator: Tier 1 pre-resolved indices + stack-free
  `evaluate_fast()` at v1; Tier 2 register-file bytecode and Tier 3 SIMD
  batch evaluation as follow-ups driven by profiling. This also resolves
  the two-VM question (`quality/Treatment` vs `math/MathExpr`): the
  **Treatment-derived VM is the reaction/treatment engine**; `math/MathExpr`
  remains for `[GWF]`-style expressions until §10 consolidation happens.
- **D-R3 (amended 2026-08-16, data-oriented review):** Integrators are
  allocation-free, operate on SoA species blocks per element batch, and are
  callable from any engine's inner loop (LARD segments, ARD cells,
  qualroute node/link CSTRs, 2D cells). Binding layout obligations from
  the LARD plan §16 review (D-L1/D-L3): transported concentrations are
  **species-major** (`s * n + i`, the E0 kernel convention) with
  per-element gather/scatter into stack-resident species blocks at
  integrator entry/exit; compiled expressions live in **one contiguous RPN
  token pool** with CSR offsets (no per-expression heap vectors), evaluated
  by the Tier-1 pre-resolved-index VM — Tier 1 is required at v1, and
  `ReactionData` separates hot numeric arrays from cold
  name/unit/source-string data so no `std::string` enters the hot loop.
- **D-R4 (amended 2026-08-12):** Equilibrium species solved by damped Newton
  per element — hand-rolled, no SUNDIALS. The LARD plan's optional KINSOL
  backend is **demoted** from "v1 build flag" to "only if a documented Newton
  failure case is demonstrated" (see D-R7).
- **D-R5:** `COUPLING FULL` = reaction ODEs for all RATE species integrated
  as one system per element; `NONE` = species-sequential (MSX semantics).
- **D-R6:** No Python in the integrator loop. Python configurability is
  compile-time (DSL/sympy → bytecode) plus the per-quality-step node-level
  source-term callback of `LAGRANGIAN_QUALITY_API_STRATEGY.md` Tier 2 —
  now defined at the shared level so it works for every engine.
- **D-R8 — Unary minus binds BELOW `^` (Python/Fortran/MATLAB convention:
  `-2^2 = -4`), decided 2026-08-16; implemented with R3.** R2 shipped the
  Excel convention (`-2^2 = +4`) unpinned; the R2 validator discovered the
  ambiguity and proved the legacy `mathexpr.c` (shared with EPANET-MSX)
  cannot arbitrate — it returns 0 for both spellings (its own header
  documents a 2022 `^` bug-fix history). The deciding argument is
  **internal consistency with the R5 sympy authoring path**: sympy parses
  `-k**2` as `-(k**2)`, so a DSL with the Excel convention would silently
  disagree with expressions authored through the planned Python surface —
  the exact class of silent kinetic error this suite exists to prevent.
  Scientific-computing user expectation (Python/Fortran/MATLAB/R) points
  the same way. Implementation: NEG precedence drops below `^` (goldens
  updated: `-2^2 = -4`, `(-2)^2 = +4`, `-2^3 = -8` unchanged, `2^-2`
  unchanged); the R2 validator's documentation block in the goldens is
  retained with the resolution appended. Documented prominently in the DSL
  reference and the GUI expression-editor tooltip (R5/GUI contract).
- **D-R9 — Evaluator owns the empty-span guard (decided 2026-08-16,
  implemented with R3):** `evalReactionExpression` returns 0.0 for
  `span.len <= 0` rather than reading an uninitialized stack slot — the
  documented "no expression" encoding must be safe regardless of caller
  discipline (one predictable branch in a noexcept hot function). R3's
  loops may ALSO skip empty spans for speed; the guard is the contract,
  the skip is an optimization. Minor R5 polish item recorded: function
  arity errors currently surface as "malformed expression" col 1 — name
  the function and arity for the GUI validation UX.
- **D-R7 — No CVODE/SUNDIALS dependency (user decision 2026-08-12).**
  CSHComponent's CVODE (ADAMS/BDF) solvers are **not** carried over
  (consistent with precedence D-UT7, which already dropped its per-element
  ODE-solver menu). Rationale: the reaction workload is many tiny independent
  systems (n_species per cell, one transport substep each), where CVODE's
  per-call overhead dominates and hand-rolled fixed-size EUL/RK5/ROS2 batched
  over SoA blocks is faster and SIMD/GPU-friendly; EPANET-MSX itself ships
  hand-rolled RK5/ROS2 and covers the standard stiff cases (validated at gate
  R3); fast chemistry belongs in EQUIL species via Newton, not in the stiff
  integrator; and SUNDIALS was already removed with the 2D CVODE stack
  (2026-07-29) — no reintroduction. Strang splitting caps reaction intervals
  at one transport substep, bounding stiffness exposure. **Escape hatch:**
  integrators sit behind the existing integrator interface; if an R3/G-UT3
  validation case demonstrably defeats ROS2 (documented step-size collapse),
  a BDF backend may be added — dependency-free remains the default build.
  **Amendment (2026-08-12, user feedback on stiff problems):** an in-house
  fixed-order **BDF2** integrator is promoted into the v1 menu, not left to
  the escape hatch: Newton + the same per-cell dense LU as ROS2, ~250 lines,
  A/L-stable. Context: in CSHComponent, CVODE/BDF integrated the full
  method-of-lines transport system, whose dominant stiffness (dispersion Δx²
  eigenvalues, coupled sources) is handled structurally here (implicit D-FV1
  dispersion, CFL-subcycled advection); the reaction integrator only sees
  per-cell kinetics over one Strang substep. BDF2 covers the practically
  stiff range of that residual; variable-order BDF (CVODE's real edge) is
  reserved for cases that should instead be reformulated as EQUIL species.
  R3 gains a stiffness-ladder gate: an nh2cl-class case run at increasing
  rate-constant spread, BDF2 vs ROS2 step counts and accuracy recorded —
  this is the empirical check on whether the no-CVODE position holds.

### 3.3 Engine bindings

| Engine | Reaction site | Notes |
|---|---|---|
| qualroute (`QualitySolver`) | node + link CSTR volumes | replaces `applyDecay` when MSX active |
| Eulerian ARD (FV-kernel core, all routing models) | per cell, Strang split after FCT advection + dispersion | this suite, Phase T2; closes the FV "AD not ARD" gap (engine plan rev. 2) |
| LARD | per segment (LTD) | per existing LARD plan |
| 2D surface | per wet cell (scope `SURFACE2D`) | Phase T6 |
| GW (planned two-zone) | per triangle, **both zones** (unsat column + saturated CV; scope `SUBSURFACE`), with retardation R_f and λ₁ per GWComponent eq. 25 | Phase T7; 2D plan §5 |

## 4. Python API (configurable reactions — headline requirement)

Neutral namespace (renames the LARD-specific surfaces of
`LAGRANGIAN_QUALITY_API_STRATEGY.md`; that doc is amended):

```python
rx = sim.reactions
rx.options(solver="ROS2", rate_units="HR", coupling="NONE", timestep=60)

rx.add_species("HOCL", kind="BULK", units="MG")
rx.add_species("NH2CL", kind="BULK", units="MG")
rx.add_parameter("k1", 0.36)
rx.add_constant("kb", 0.12)
rx.add_term("AMM", "0.05 * NH2CL")

rx.set_pipe_rate("HOCL", "-k1 * HOCL * AMM - kb * HOCL")   # Tier 1 DSL string
rx.set_tank_rate("HOCL", "-kb * HOCL")
rx.set_equilibrium("H", "H2CO3 - K1*HCO3")                  # EQUIL
rx.set_formula("TOTCL", "HOCL + NH2CL")                     # FORMULA

# sympy authoring (optional dependency) and decorator tracing, per the
# API strategy doc:
rx.set_pipe_rate("HOCL", sympy_expr)
@rx.rate_expression("NH2CL")
def nh2cl(NH2CL, HOCL, AMM, k1): return -k1 * HOCL * AMM

rx.set_parameter("k1", link="C7", value=0.5)                # per-element override
rx.set_source("N3", "HOCL", kind="SETPOINT", value=1.2, pattern="chl")
rx.set_initial("HOCL", scope="GLOBAL", value=0.8)
rx.validate()            # compile + dimensional sanity, returns diagnostics
```

Mid-simulation editing follows the API strategy doc: atomic expression swap,
generation counters, `StaleObjectError`, recompile log to `.rpt`. MCP
exposes DSL-path tools only (`reactions_*`), no source-term callbacks
(safety boundary retained). C API in `openswmm_reactions.h` mirrors 1:1.

**GUI contract** (consumed by
`openswmm.gui/workplans/TRANSPORT_QUALITY_GUI_PLAN_2026-08-12.md` — the GUI
Reaction System editor clones the validated `TreatmentExpressionEdit` stack
and does no client-side parsing):

- `swmm_reaction_validate_expression(engine, scope, expr, errbuf, n, &col)` —
  compile-only validation with error column, mirroring
  `swmm_treatment_validate_expression`.
- Discovery getters (counts + names + kinds + units) for species, parameters,
  constants, terms, plus the hydraulic-variable and function name lists —
  the authoritative source for GUI completer/highlighter vocabulary
  (drift-guard).
- Species-kind query per the master plan §4.1 registry.
- Compiled-expression text getters for round-trip display (per the API
  strategy doc's GUI data contract, now engine-neutral).

## 5. Implementation phases

```
R1  ReactionSystem registry + [REACTION_*] handlers (in `model.rxn` via the
    IO1/IO2 machinery, `64c831d6`) + species registry integration (master
    plan §4.1). Carried obligation from IO1 validation: R1 decides and
    implements **duplicate-registration semantics** for
    [PROCESS_COMPONENTS] ids (currently two rows for one id run apply()
    twice with undefined precedence — proposal: duplicate id = validation
    error). R1 also implements the embedded-section fallback + style
    warning for its own sections (IO plan §3.2 rule).
    → verify: parse/round-trip INP + GeoPackage; MSX example inputs
      translate via the Python .msx converter script (tests/data);
      duplicate-id gate; embedded-fallback gate.
R2  Compiler + VM extension (D-R1/D-R2 Tier 1) + unit tests per token.
    Carried obligation from R1 validation: make registry population
    TRANSACTIONAL — parseSpecies currently registers MSX species as it
    parses, so a config that later fails leaves entries in the species
    registry (visible state from a rejected file under lenient/editor
    open). Stage locally; commit to the registry only on overall success.
    → verify: expression golden tests vs MSX reference evaluations;
      rejected-config-leaves-no-registry-entries gate (falsifier: lenient
      open of a failing config, assert registry count == pollutant count).
R3  Integrators (EUL/RK5/ROS2/BDF2) + Newton equilibrium + FORMULA
    resolution order (topological sort, cycle detection like treatment
    co-pollutant recursion).
    → verify: MSX reference cases as5, nh2cl (stiff → ROS2/BDF2), dbp, lead
      batch-reactor parity within published tolerances; stiffness ladder —
      nh2cl-class kinetics at increasing rate-constant spread, BDF2 vs ROS2
      step counts + accuracy recorded (empirical check on D-R7's no-CVODE
      position; failure triggers the D-R7 escape hatch).
R4  qualroute binding (CSTR sites) with kdecay-as-RATE fallback.
    → verify: G-UT1 bit-parity when no [REACTION_*] present; nh2cl on a
      network vs EPANET-MSX exported results.
R5  Python/C/MCP API + sympy/decorator authoring + mid-sim editing.
    → verify: parity registries; StaleObjectError tests; MCP DSL-only gate.
R6  Engine binding for the Eulerian ARD cell path (with T2; one binding
    covers all routing models per engine plan rev. 2); subcatchment
    expression hook.
    → verify: identical batch-reactor results across engines at the
      zero-flow limit (G-UT4 corollary).
```

## 6. Open items

- Wall-species surface area source in non-circular conduits (use xsect
  wetted perimeter from `xsect_*`; confirm against MSX `Av` definition).
- Dimensional analysis strictness on `validate()` (warn vs error).
- Whether `[REACTION_SUBCATCHMENTS]` ships in R6 or with Phase T3 age work.
