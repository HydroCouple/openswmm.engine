# R4 Implementation — Validation & Commit Handoff (2026-08-16)

**For:** the checking agent (sandbox: `g++ -fsyntax-only` only; nothing
linked/executed).
**Base:** `eca08593` (post-R3 + follow-ups).
**Plan:** `MULTISPECIES_REACTIONS_MSX_PLAN.md` §5 R4 (LEGACY binding).
**Standing findings:** reconfigure (two new `.cpp`); unique-diagnostic
falsifiers; EXPECT-not-ASSERT in table loops; sanitizers where state
plumbing changed.

---

## 1. Changeset (uncommitted)

```
new:  src/engine/transport/components/ReactionModule/ReactionLegacyBinding.{hpp,cpp}
new:  tests/unit/engine/test_reaction_legacy_binding.cpp   (6 gates)
mod:  src/engine/data/ReactionTokens.hpp        (PUSH_POLLUT op)
mod:  src/engine/transport/components/ReactionModule/ReactionExpression.{hpp,cpp}
      (RxSymbols.pollutants / RxEvalEnv.pollutants; resolution order:
       species → coef → term → POLLUTANT → hydvar; note a pollutant named
       "D" shadows the depth variable — documented, user-defined beats
       builtin)
mod:  src/engine/transport/components/ReactionModule/ReactionIntegrator.{hpp,cpp}
      (pollutants pointer threaded through every env; DEFAULT ARG nullptr
       keeps every R3 gate source-identical)
mod:  src/engine/transport/components/ReactionModule/ReactionsComponent.cpp
      (pollutant symbols at compile; species-column rows naming a pollutant
       get the E4/R6 deferral error)
mod:  src/engine/data/ReactionData.hpp          (msx_node_conc/msx_link_conc
                                                 element state + warn flags)
mod:  src/engine/quality/QualityRouting.cpp     (execute() branch: reactions
      active ⇒ reactLegacyNodes at the applyDecay site, reactLegacyLinks
      AFTER updateLinkQuality; in-mix linear link decay zeroed when active)
mod:  tests/unit/engine/CMakeLists.txt
```

## 2. What R4 does (design decisions to review)

1. **kdecay upgrades to the exact exponential** (`c *= exp(−k·dt)`) at
   nodes and links when a reactions component is configured. Rationale:
   pollutant kdecay IS first-order decay, whose closed form is the
   exponential — running the RK5 integrator to approximate what we can
   write down would be waste. Bit-parity when NO component is configured:
   applyDecay and the in-mix linear decay run untouched (gate 1 + your
   sha256 discipline).
2. **Link ordering:** links react AFTER `updateLinkQuality` (whose internal
   linear decay is zeroed when active) because that pass assigns
   `links.conc` from `conc_old` + upstream mixing — reacting first would be
   overwritten. Net semantics under reactions: mix-then-decay instead of
   legacy's decay-then-mix within the link update. Documented behavioral
   difference gated behind the component, like the exponential itself.
3. **Pollutants are READ-ONLY in MSX expressions** (`PUSH_POLLUT`, element
   pollutant block passed per element). Pollutant KINETICS rows (RATE/
   EQUIL/FORMULA naming a pollutant) are a defined deferral error naming
   E4/R6. MSX species carry per-element state under LEGACY
   (`msx_node_conc`/`msx_link_conc`, seeded from GLOBAL initial values)
   but are NOT transported between elements — once-per-run warning when
   any RATE MSX species exists (R4b).
4. **Failure containment:** integration failure leaves the element block
   unchanged and warns once per run with element id + the integrator's
   remedy text. Never fatal mid-run.

## 3. Validation protocol

1. **Reconfigure**, build, zero new warnings from touched files.
2. `ctest -R test_engine_reaction_legacy_binding` — six gates.
   *Anticipated failure modes, in likelihood order:*
   (a) **Gate 2/1's premise that no-inflow mixing preserves node conc** —
   if `mixAtNodes`/`findNodeQual` re-derives concentration when the node
   volume decays toward zero over the 2-minute horizon, the analytic
   product breaks. Check `nodes.conc[0]`'s trajectory first (probe print)
   before debugging the binding; shortening the horizon or raising the
   initial depth is a legitimate deck fix, weakening the band is not.
   (b) **J0 == node index 0** assumption in the gates — verify parse
   order; switch to a name lookup if wrong.
   (c) Gate 3's 1e-9 band on FORMULA-tracks-pollutant: the FORMULA
   evaluates at the END of the node react step while pollutant decay ran
   FIRST in the same call — the ordering in `reactLegacyNodes`
   (decay, then MSX) makes X track the POST-decay TSS; if it fails,
   check whether treatment (which runs before) perturbs `nodes.conc`.
   (d) STEADY routing: the steady branch of updateLinkQuality used
   `exp(−k·dt)` already; with reactions active its k is zeroed and the
   binding re-applies the same exponential — net identical for links, but
   verify with a STEADY smoke that nothing double-applies.
3. Falsifier probes: (i) comment the `k = 0.0` zeroing in
   `updateLinkQuality` → gate 2 must fail LOW (double decay); (ii) revert
   the `PUSH_POLLUT` resolution block in `ReactionExpression.cpp` → gates
   3/5 configs must fail to open with "undefined identifier 'TSS'".
4. Prior suites all green — especially the FULL R3 integrator suite
   (default-arg threading must be source-invisible to it) and E1/E2 ARD
   gates. Sanitizer pass over the new binding test (fresh element-state
   plumbing).
5. Bit-identity: quality decks WITHOUT a reactions component vs base —
   the execute() branch and in-mix k fetch are the only touched
   production lines; both are gated on `legacyReactionsActive`.
6. **nh2cl network parity vs EPANET-MSX** (plan R4 verify): if you have
   EPANET-MSX runnable, a small network with the nh2cl kinetics under
   LEGACY vs MSX's own results (published tolerances) — RATE MSX species
   are element-local here, so restrict the comparison to a single-tank
   batch configuration, or record as deferred to R4b/E4 where transport
   makes the full network comparison meaningful. Your call; record either
   way.
7. Append results to §5; commit with §4.

## 4. Commit message

```
feat(reactions): LEGACY quality engine binding (R4)

With a reactions component configured: pollutant kdecay upgrades from the
legacy linearized (1-k*dt) to the exact exponential at nodes and links
(bit-parity preserved when unconfigured); MSX species react per element
(nodes tank-scope, links pipe-scope) via ReactionIntegrator with the
element's pollutant concentrations readable in expressions (PUSH_POLLUT;
pollutant KINETICS rows defer to E4/R6 with a precise error); per-element
MSX state seeded from GLOBAL initial values, not yet transported between
elements (R4b, warned once); links react after updateLinkQuality with its
internal linear decay zeroed (no double-apply); integration failures warn
once with element id + remedy and leave the element unchanged. Gates:
tests/unit/engine/test_reaction_legacy_binding.cpp (6, incl. the
exponential-vs-linear discrimination gate and both falsifier probes).

Plan: MULTISPECIES_REACTIONS_MSX_PLAN.md §5 R4.
Validation record: plans/transport/R4_VALIDATION_HANDOFF_2026-08-16.md
```

## 5. Validation results

**Committed as `326b595c` after four fixes.** All six delivered gates passed on
the first run — which turned out to say less than it looked like, because two
of the three defenses the handoff named had no gate that could see them.

### 5.1 What the delivered changeset got right

Reconfigure + build clean (no new warnings from touched files). Six gates
green out of the box. Falsifier (ii) behaved exactly as §3.3 predicted: the
config fails to open with `[REACTION_TANKS] 'X' (col 7): undefined identifier
'TSS'`, so PUSH_POLLUT is load-bearing. §3.2's anticipated failure modes (a)
through (d) did not occur — node concentration tracks pure decay to 0.0000%
at every kdecay tried, J0 is node index 0, the FORMULA ordering is as
described, and the STEADY branch does not double-apply.

### 5.2 Falsifier (i) does not falsify — the link side had no gate

§3.3 predicted that un-zeroing the in-mix linear decay in `updateLinkQuality`
would make gate 2 fail low. It does not: **gate 2 still passes**. Gate 2 reads
`nodes.conc[0]`, and J0 is UPSTREAM of C1, so link concentration never reaches
a node. Nothing in the delivered suite reads `links.conc` or
`msx_link_conc`, and every deck uses `[REACTION_TANKS]`.

So two headline claims in §4's commit message were untested: "links react
after updateLinkQuality with its internal linear decay zeroed (no
double-apply)", and pipe-scope MSX reactions altogether — half the binding,
including the whole `tank=false` integrator path.

Measured at kdecay 0.03 (`r4_probe3.log`), link concentration relative to the
same deck without a component:

| build | links.conc[0] | ratio to legacy |
|---|---|---|
| correct | 0.246442 | 1.215 |
| in-mix k not zeroed (double decay) | 0.039884 | 0.197 |
| reactLegacyLinks removed (no decay) | 5.347800 | 26.4 |

Added **gate 7** (brackets the ratio in [1.10, 1.50]) and **gate 8**
(`[REACTION_PIPES] FORMULA X 3*TSS` ⇒ `msx_link_conc[0] == 3*links.conc[0]`
to 1e-9, plus the tank-scope companion so a scope mix-up cannot pass).

### 5.3 Three configurations reacted nothing, silently

None of the six decks varies `QUALITY_SOLVER`, `IGNORE_QUALITY`, or the
presence of `[POLLUTANTS]`. All three skip the binding (`r4_probe.log`):

- **No `[POLLUTANTS]`** — an MSX-only model, the canonical EPANET-MSX shape.
  Two independent guards blocked it: the step loop's `n_pollutants() > 0` and
  `QualitySolver::execute`'s `n_pollutants_ <= 0` early return. Run
  completed, `msx_node_conc` never even sized, no warning. **Fixed**: both
  guards now also admit `legacyReactionsActive(ctx)`. Parity is preserved by
  construction — without a component the conditions are unchanged.
- **`QUALITY_SOLVER EULERIAN_ARD`** — `ard_.step()` replaces
  `quality_.execute()`, so the binding never runs. Not reacting is correct
  (the ARD binding is R6); the silence is not. The only warning emitted was
  E1's pre-existing note about pollutant kdecay, which says nothing about the
  configured component.
- **`IGNORE_QUALITY YES`** — same bypass, no warning.

**Fixed**: `warnIfLegacyBindingBypassed()`, called at open, names the
configuration and the remedy. Added **gate 9** (MSX-only deck reacts:
`X = exp(0.12)`) and **gate 10** (both bypasses announce themselves).

### 5.4 The harness crashed instead of reporting

Under falsifier (ii) the suite **segfaulted (exit 139)** rather than failing.
`run_node_conc` used `EXPECT_EQ` on the open and then returned
`nodes.conc[0]`; a failed open leaves that array empty, so the helper read
past the end. This is the R3 ASSERT-in-a-loop lesson from the other
direction: EXPECT where the code cannot continue. **Fixed**: `run_deck` uses
ASSERT (callers wrap in `ASSERT_NO_FATAL_FAILURE`), and the accessors return
−1 on an empty array rather than indexing it.

### 5.5 Gate 2's discrimination floor had 0.15% headroom

At the delivered kdecay 0.01, `exp(−k·N·dt)` and `(1−k·dt)^N` differ by only
3.15%, while the check asserted `c > linear * 1.03` — a floor of 3.00749
against a true value of 3.01194. The ±1% NEAR band already excluded the
linear product, so the check added fragility without teeth. **Fixed**: kdecay
0.03 separates the two products by 35%; the floor moved to 1.10 (18%
headroom).

### 5.6 Falsifier sweep (`falsifiers.sh`, one case per invocation)

| falsifier | gates that fail |
|---|---|
| i — un-zero the in-mix link decay | 7 (low side) |
| ii — remove PUSH_POLLUT resolution | 2, 3, 5, 7, 8, 10 — reported, no crash |
| iii — disable `reactLegacyLinks` | 7 (high side), 8 |
| iv — revert the MSX-only step-loop gate | 9 |
| v — drop the bypass warning | 10 |

Each defense is covered, and each new gate is tied to a specific one. The
first version of this script restored with `cp` inside a zsh function and
silently did not, producing two bogus rows (gate 7 "failing" under iv and v,
which touch only `SWMMEngine.cpp`). It now asserts both the patch and the
restore against the file. Note also that `git checkout --` is NOT usable as
a restore here: the whole changeset is uncommitted while the sweep runs.

### 5.7 Suites, parity, sanitizers

- **10/10** R4 gates; full suite **133/134** (the only failure is the known
  pre-existing `FvEngine.RefiningTheMeshConvergesTowardTheDynwaveHydrograph`).
- **Bit-identity 14/14**: the ten E0 hydraulics decks + four E2 quality decks,
  sha256 of `.out`, current build vs a `d0702612` worktree build.
- **ASan + UBSan**: 0 findings across the binding, integrator and config
  suites (26 tests).

### 5.8 Not done — §3.6 nh2cl parity

Deferred, and recorded as deferred rather than quietly skipped. Under LEGACY
a RATE MSX species is element-local (R4b), so the only comparison available
today is a single-tank batch, which exercises the integrator (already gated
in R3) rather than the binding. The network comparison becomes meaningful
once MSX species are transported — E4/R4b owns it.

### 5.9 Left alone deliberately

- `parseExpressions` reports "'X' is a pollutant" for any name found in the
  species registry. Only pollutants can reach it today (registry MSX entries
  are also `rx.find_species` hits, and reserved species are unimplemented),
  so the message cannot currently mislead. Worth tightening to a
  `kind == POLLUTANT` test when A1/H1 land.
- `PUSH_POLLUT idx` aliases the registry index to the pollutant index. That
  holds because `SWMMEngine` registers pollutants into the first slots, and
  it is documented there — but nothing enforces it.
- A second `swmm_engine_initialize()` on one open does not re-seed MSX state.
  Measured (`r4_probe2.log`): it does not re-seed pollutants either, and the
  second run is a no-op end to end. Pre-existing engine behavior, not R4's.
