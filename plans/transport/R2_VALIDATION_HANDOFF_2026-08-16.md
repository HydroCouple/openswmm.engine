# R2 Implementation — Validation & Commit Handoff (2026-08-16)

**For:** the checking agent (sandbox: `g++ -fsyntax-only` only, all TUs pass;
nothing linked/executed).
**Base:** `9d0dbbff` (post-T0a/R1).
**Plan:** `MULTISPECIES_REACTIONS_MSX_PLAN.md` §5 R2 (D-R1/D-R2 Tier 1,
D-L3 flat pool) + the carried transactional-registry obligation.
**Standing findings that apply:** reconfigure before building (glob without
`CONFIGURE_DEPENDS` — two new `.cpp`); probe gates with wording UNIQUE to
the defense under test (defense-in-depth aliasing).

---

## 1. Changeset (uncommitted)

```
new:  src/engine/data/ReactionTokens.hpp        (POD RxToken/RxExprSpan/RxHydVar —
                                                 data layer, no deps)
new:  src/engine/transport/components/ReactionModule/ReactionExpression.{hpp,cpp}
new:  tests/unit/engine/test_reaction_expressions.cpp   (7 gates)
mod:  src/engine/data/ReactionData.hpp          (token_pool + term/pipe/tank spans,
                                                 compiled flag)
mod:  src/engine/transport/components/ReactionModule/ReactionsComponent.cpp
      (compile pass after parsing; TRANSACTIONAL registry commit — species
       staged during parse, committed only after full success; rejected
       config clears all reaction state)
mod:  tests/unit/engine/CMakeLists.txt          (test_engine_reaction_expressions)
```

## 2. What R2 adds

- **Compiler** (`compileReactionExpression`): shunting-yard from the MSX DSL
  to the FLAT shared token pool — numbers; identifiers resolved
  species → coefficient → term → hydraulic variable (D,Q,U,Re,Us,Ff,Av,
  HRT,DT) with PRE-RESOLVED indices; `+ - * / ^` (right-assoc `^`), unary
  minus, parens; functions EXP/LOG/LOG10/SQRT/ABS/SGN/STEP/SIN/COS/TAN
  (unary), MIN/MAX/POW (binary). Diagnostics carry a 1-based column
  (`undefined identifier 'x'`, `expression too deep` at depth > 32 —
  provably bounding the evaluator's fixed stack — `term 'T' references
  later term — term references are forward-only`). Terms are forward-only
  by rule, so evaluation is an in-order sweep with no cycle machinery.
- **Tier-1 evaluator** (`evalReactionExpression`): one pass over the span,
  fixed `double[32]` stack, zero allocation, zero name lookups, `noexcept`.
  Post-compile stack-discipline verification guarantees the evaluator
  cannot under/overflow.
- **Compile-at-open:** `applyReactionSections` now compiles every term and
  pipe/tank expression after parsing; compile failures are open() failures
  with section + species/term + column context.
- **Transactional registry (carried obligation):** species are STAGED
  during parse (collision-checked against the registry without inserting)
  and committed only after parse **and** compile succeed; any failure
  clears `ctx.reactions` entirely. A rejected file leaves no registry
  entries and no half-held reaction state under lenient open.
- Deliberately NOT here: integrators + EQUIL/FORMULA evaluation semantics
  (R3); rate-unit scaling (applied at integration time, R3); `Av`
  population by engines (R6); Tier-2/3 VM (profiling-driven follow-ups).

## 3. Validation protocol

1. **Reconfigure**, build, zero new warnings from touched files.
2. `ctest -R test_engine_reaction_expressions` — 7 gates (4 direct VM,
   3 engine-level). Falsifier probes to actually run:
   (a) in `ReactionsComponent.cpp`, re-add a direct
   `ctx.species_registry.add(...)` inside `parseSpecies` →
   `TransactionalRegistryOnRejectedConfig` MUST fail (registry count 1 → 3);
   restore. (b) flip `^` to left-associative in the compiler
   (`ra = false`) → `PrecedenceAndAssociativityGoldens` MUST fail on
   `2^3^2`; restore. Both probes target wording/values unique to this
   changeset — no aliasing with R1 diagnostics.
3. `ctest -R "test_engine_reactions_config|test_engine_process_components"`
   — prior gates stay green. NOTE: `FullConfigParsesAndPopulates` (R1)
   asserts `registry_base == 1` and registry contents — the transactional
   commit changes WHEN entries land but not the final state on success, so
   it must still pass unchanged. If it fails, the commit-stage ordering is
   wrong — do not adjust the test.
4. Full suite + the standard no-section bit-identity spot-check (sha256 a
   couple of decks — the compile path only runs when a reactions config is
   present).
5. Append results to §5; commit with §4.

## 4. Commit message

```
feat(reactions): expression compiler + Tier-1 flat-pool VM (R2)

Shunting-yard compiler from the MSX-convention DSL to one contiguous
RxToken pool (D-L3: per-expression spans, pre-resolved species/coef/term/
hydraulic-variable indices, column-bearing diagnostics, forward-only term
references, depth-bounded) and an allocation-free fixed-stack Tier-1
evaluator. Expressions compile at open with section+column context.
Resolves the carried R1 obligation: species registry population is now
transactional — staged during parse, committed only after parse+compile
succeed; a rejected config leaves no registry entries and no reaction
state. Gates: tests/unit/engine/test_reaction_expressions.cpp (7).

Plan: MULTISPECIES_REACTIONS_MSX_PLAN.md §5 R2 (D-R1/D-R2/D-L3).
Validation record: plans/transport/R2_VALIDATION_HANDOFF_2026-08-16.md
```

## 5. Validation results

Validated and committed 2026-08-16 as **`352638e6`** (§4 message plus a
paragraph on §5.5). Base `9d0dbbff` matched HEAD exactly; the working tree
carried precisely the seven files of §1. Artifacts:
`tests/output/r2_validation_2026-08-16/` (not committed).

### 5.1 Build (protocol 1)

Reconfigured first. Build clean (rc=0). **Zero warnings from any touched
file** — including, for once, none from the new TUs: `ReactionExpression.cpp`
does not pull in `TableData.hpp`, so it avoids the repo-wide
`-Wmissing-field-initializers` instance the R1 component TU surfaces.

### 5.2 Gates (protocol 2) — 7/7, both falsifier probes bite

| probe | required outcome | observed |
|---|---|---|
| (a) re-add direct `species_registry.add()` in `parseSpecies` | `TransactionalRegistryOnRejectedConfig` fails, count 1 → 3 | **fails, "Which is: 3"** ✓ |
| (b) `ra = false` (left-assoc `^`) | `PrecedenceAndAssociativityGoldens` fails on `2^3^2` | **fails, 64 vs 512** ✓ |

Both exactly as §2 predicted, including the predicted values. Unlike R1's
gates, these carry no aliasing risk: the transactional gate asserts a
*count*, and the goldens assert *exact doubles*. Probes reverted and
`git diff --stat` re-verified before committing.

### 5.3 Prior gates (protocol 3)

`test_engine_reactions_config` and `test_engine_process_components` both
green, unchanged. Specifically `FullConfigParsesAndPopulates` still passes
with `registry_base == 1` — the commit-stage reordering changes *when*
entries land, not the final state, as §3 required. No test adjustment
needed.

### 5.4 No-regression (protocol 4)

Full suite **131/132** (`ctest_full.log`); the single failure is the known
pre-existing `FvEngine.RefiningTheMeshConvergesTowardTheDynwaveHydrograph`.

Bit-identity spot-check, 14 decks (10 E0 hydraulics + 4 E2 quality),
`.out` sha256: **14/14 identical**. Baseline is the R1-validation `r1_new`
run, which was itself shown identical to a shelved HEAD build; the only
commit since is `9d0dbbff`, which touches an InpWriter path the CLI never
invokes. The compile pass only runs when a reactions config is present, so
this is the expected result — measured rather than assumed.

### 5.5 Finding: unary minus vs `^` was unpinned, and legacy cannot arbitrate

Not in §2's probe list; found by reading the operator table and confirmed by
running it. The compiler gives unary minus precedence 4, above `^` (3):

```
-2^2      R2 = 4      legacy mathexpr = 0    *** DIVERGES ***
-2^3      R2 = -8     legacy mathexpr = 0
2^-2      R2 = 0.25   legacy mathexpr = 0.25   agree
-2*3      R2 = -6     legacy mathexpr = -6     agree
```

So `-2^2` is `(-2)^2 = +4` — the Excel convention, opposite to
Python/Fortran/Matlab (`-(2^2) = -4`). **No gate pinned it**, and
`PrecedenceAndAssociativityGoldens` cannot catch it: its unary-minus case is
`- -5`, where precedence against `^` never arises.

I went to the engine's own parser for an authority and it has none to give.
`src/legacy/engine/mathexpr.c` — the same file EPANET-MSX uses, so the
literal "MSX convention" — returns **0 for both `-2^2` and `(-2)^2`**, while
handling `0-2^2` and `-(2^2)` correctly as −4 (calibration in
`legacy_mathexpr_probe.log`; note the file's own header credits a 2022 bug
fix for "Problems related to '^' operator"). My first pass predicted −4 from
*reading* `getOp`/`getSingleOp`; running it corrected that. There is
therefore no conventional answer to inherit, and R2's behavior is at least
well-defined.

Action taken: **pinned the current behavior with goldens**, with the above
recorded in the test so nobody "corrects" it by accident — a test-only
change, no behavior change. Flipping to the Python convention (make
`NEG.prec` 0.5 below `^`, or apply negation after the power) remains a
one-line decision if you prefer it; it is a real semantic difference for any
kinetic with an even power on a negated base.

Also note `-2^3` agrees under both conventions because the exponent is odd —
the reason the golden uses an even exponent.

### 5.6 Open, NOT fixed

**`evalReactionExpression` reads an uninitialized stack slot on an empty
span.** `RxExprSpan{}` with `len == 0` is the documented encoding for "no
expression" (and gate `GoodConfigCompilesToPool` asserts
`pipe_expr[1].len == 0` for a species without one), but the evaluator loops
zero times and then `return st[0]` — reading an indeterminate `double` in a
`noexcept` function. It returned 0.0 on every probe attempt, which is the
unhelpful kind of luck. R3 is the first caller and must check `len` before
evaluating; the one-line belt-and-braces fix is `if (span.len <= 0) return
0.0;` at the top. Left out because it is a contract decision (does the
evaluator own the guard, or the caller?) that R3's loop shape should settle.

Function arity is not checked at parse time — `MIN(1)`, `EXP(1,2)` and
`MIN(1,2,3)` are all caught, but by the post-compile stack-discipline pass,
so they report the generic "malformed expression" at column 1 rather than
naming the function and its arity. Acceptable, worth knowing when a user
reports a confusing diagnostic.
