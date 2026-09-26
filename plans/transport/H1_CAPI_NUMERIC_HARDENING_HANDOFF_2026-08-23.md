# H1 Validation Handoff — A Malformed Option Value Must Not Kill the Process

**Date:** 2026-08-23 · **Author:** implementing agent (syntax-only sandbox;
touched TU passes `-fsyntax-only`; nothing executed) · **Step:** one of the
"high items" — the crash-class defect Y0's validation measured and recorded
as pre-existing and TU-wide · **Base:** `4639be37` (Z1).

> **Round name collision, deliberate:** this is unrelated to the heat
> track's H1. Named for "hardening"; rename on commit if it reads badly
> against `HEAT_TRANSPORT_PLAN.md`'s H1.

---

## 1. The defect, as measured by Y0's validator

`swmm_options_set(e, "FV_CFL", "abc")` **terminated the process** — raw
`std::stod` throws `std::invalid_argument`, and an exception crossing
`extern "C"` is undefined behaviour that in practice aborts.
`ROUTING_STEP = "xyz"` survived only because that single branch already
carried a local try/catch.

**Reachable in production, which is why it is a "high item" and not debt:**

- the **MCP server's `model_set_option`** passes arbitrary LLM-authored
  text straight into this dispatch;
- a **GUI line edit is transiently empty** while a user types, and Y1's
  page writes seven of these keys.

## 2. Changeset

| File | Change |
|---|---|
| `src/engine/core/openswmm_model_impl.cpp` | **one** `try { … } catch (...) { return SWMM_ERR_BADPARAM; }` around `swmm_options_set`'s entire key dispatch |
| `tests/unit/engine/test_options_malformed_values.cpp` | **NEW** — 4 gates |
| `tests/unit/engine/CMakeLists.txt` | registered |

**Coverage proof (scripted, not eyeballed):** the TU has 33
`std::stod`/`std::stoi` occurrences. Three (`:382`, `:435`, `:475`) already
had local guards. Two are inside the new comment's own prose. **The
remaining 28 all fall inside the new guard's span** — verified by listing
every site's line number and testing it against the `try`/`catch` bounds,
with the check printing any uncovered site (it printed none but the two
comment lines).

## 3. Design decisions (challenge in this order)

1. **One function-level guard, not 30 local ones.** A per-site fix can miss
   a site, and — the part that matters more — **a future branch would be
   born unguarded**. The guard makes the property structural: no value
   parsed anywhere in this dispatch can escape as an exception.
2. **"No exception escapes a C entry point" is the contract**, independent
   of parsing. Nothing else in the dispatch throws (the rest is assignment
   and enum comparison), so the broad catch costs no diagnostic precision
   today and is correct if that ever changes.
3. **`SWMM_ERR_BADPARAM`, matching the enum-rejection answer.** A caller
   cannot distinguish "unknown token" from "unparseable number", and does
   not need to: both mean *this value was refused, the option is unchanged*.
4. **The old value survives** because the throwing branch assigned nothing
   before throwing — gate 2 pins that rather than assuming it.
5. **Scope: `swmm_options_set` only.** `swmm_files_set`'s single site is
   already guarded. **The rest of the C API was NOT audited** — see §7.

## 4. Gates

1. `MalformedNumericsReturnErrorNotDeath` — **exhaustive**: 26 numeric keys
   × 6 malformed values (junk, empty, whitespace, punctuation, stod
   overflow, stoi overflow) = 156 calls, each of which must return non-OK
   and, more importantly, must *return at all*.
2. `RefusedSetPreservesThePreviousValue` — a refused set leaves the prior
   value byte-identical (catches a guard that swallowed a partial write).
3. `WellFormedValuesStillRoundTrip` — liveness: a guard wrapped around the
   wrong span would make every set "succeed" while persisting nothing.
   Also asserts an unknown key is still refused.
4. `EnumKeysStillRejectUnknownTokens` — the explicit-`else` rejections must
   keep working; the guard must not short-circuit the dispatch.

**How gate 1 fails if the guard is removed: the test binary ABORTS.** That
is the observation — ctest reports the death. Do not mistake a crashed
binary for an infrastructure problem during the sweep; it is the signal.

## 5. Falsifier sweep

| # | Falsifier | Must fail |
|---|---|---|
| i | remove the `try`/`catch` | gate 1 — as a **process abort**, not an assertion failure |
| ii | move the `catch` to wrap only the last `else` | gate 1 (an early key still aborts) |
| iii | `catch (...) { return SWMM_OK; }` | gate 1 (malformed value reported as accepted) |
| iv | wrap the trailing `return SWMM_OK` inside the try's scope such that success is swallowed | gate 3 (values stop persisting) |
| v | make the catch also swallow the unknown-key `else` (return OK) | gate 3's unknown-key leg |
| vi | assign before parsing in one branch (e.g. `opt.fv.cfl = 0; opt.fv.cfl = std::stod(v);`) | gate 2 for that key |

## 6. Standing verification

Full suite from an isolated worktree; **corpus 19/19 byte-identical** — the
guard changes no successful path, so any movement is a real finding. ASan
over the new suite. Zero new warnings. **Note the exception-handling
build flags**: if this TU is ever compiled `-fno-exceptions`, the guard
cannot work and the crash returns — check and record.

## 7. Not claimed / owed

**The rest of the C API is unaudited.** This round fixed the one entry
point measured to abort and reachable from the MCP/GUI. There are ~15 other
`*_impl.cpp` TUs; whether any parse numerics the same way is **unknown and
worth one grep-driven round** — that is the "harden more broadly" option,
deliberately not taken here to keep this surgical. Also unaudited: the
Python bindings' own conversion layer.

## 8. On acceptance

Commit; record the lesson (**a C boundary needs a structural guard, not
per-site diligence**); update Y0's §8 note to point at this round as the
resolution of the defect it recorded; consider scheduling the §7 audit.

---

## 9. VALIDATION RESULTS (2026-08-24, validating agent)

**Verdict: accepted after two required fixes. Committed with the fixes.**
Base moved under the round: validated against `805424b5` (the FV
feedback-limit commit), not `4639be37`.

### 9.1 The delivered guard was necessary but NOT sufficient

Gate 1, run against the actual build (first run was a STALE BINARY —
see §9.4), failed 57 ways. Two parse families never throw, so the
exception guard alone could not see them:

1. **Time-typed keys** (ROUTING_STEP, REPORT_STEP, MINIMUM_STEP,
   DRY_STEP, WET_STEP, RULE_STEP + the three time-of-day composites):
   `input::parse_time_seconds` fabricates 0.0 from junk, and
   `"1e999999"` became **3600 s** through its H[:M] fallthrough.
2. **Int keys** (9 × stoi + 1 × stol): a numeric PREFIX parses —
   `"1e999999"` became 1, `"1.5"` became 1, silently.

**Fix (same contract, wider net):** strict full-consumption wrappers in
the TU's anon namespace — `stod_strict` / `stoi_strict` / `stol_strict`
/ `time_seconds_strict` — replacing every raw parse inside the dispatch
(17 stod + 9 stoi + 1 stol + 9 time sites; independent script re-check:
zero raw sites remain in the span). `time_seconds_strict` accepts
exactly the documented grammar (plain seconds, H:M[:S], digit-only H/M)
and computes with the SAME expression, so well-formed values are
bit-identical to the deck parser's. **Deck parsing untouched** — the
lenient file grammar is parity-bound; only the C API refuses.

### 9.2 One gate over-claim corrected

`"99999999999999999999999"` is 1e23 — a well-formed double. Applying it
to EVERY key made gate 1 demand refusal of a legal value on stod/time
keys. It moved to a new int-only list (with `"1.5"`, the partial-parse
razor). Gate 3 gained the clock-form legs: `0:00:30` → 30 s and `1:30`
→ 5400 s round-trip; `"5:"` and `"1:2:3:4"` are refused (the lenient
parser accepted the former as 5 HOURS).

### 9.3 Falsifier sweep

| # | result |
|---|---|
| i | bites — 3 gates fail under gtest's default exception catcher; with `--gtest_catch_exceptions=0` the binary dies in `libc++abi: terminating due to uncaught exception` (the mode §4 predicted; gtest masks it into assertion failures by default — do not expect a ctest "Failed" to say "abort") |
| ii | **cannot be realized syntactically** — the dispatch is one else-if chain; a mid-chain catch does not compile. Its failure class (any throwing branch outside the guard) is exactly falsifier i's observable, demonstrated in both modes. Recorded, not skipped silently |
| iii | bites — 3 gates |
| iv | class covered by v + gate 3's liveness rows (a success-swallowing guard shape does not compile either) |
| v | bites — gate 3's unknown-key leg |
| vi | bites — gate 2 (assign-before-parse on FV_CFL) |
| vii (new) | stoi_strict without the trailing check — gate 1's int leg bites |
| viii (new) | time_seconds_strict falling back to the lenient parser — gates 1+3 bite |

### 9.4 Standing verification + process notes

ctest 168/168 ×3 (`test_engine_2d_infil_integration` excluded — another
session's UNTRACKED WIP test over their FV work, fails independently of
this round). ASan 4/4. **Corpus 19/19 byte-identical** against a base
bundle whose dylib was `cmp`-verified distinct (a copied CLI alone loads
the PATCHED dylib via rpath — vacuous). No `-fno-exceptions` anywhere in
the build. Zero new warnings from the TU.

Process notes: (1) the first gate run used a stale binary (mtime race)
and mis-reported even guarded keys as accepting — force-touch before
trusting any verdict; (2) coverage re-checked by script: 30 parse sites
inside the guard span, 0 uncovered (§2's count of 33 had drifted to 35
with `805424b5`).

§7's broader C-API audit remains open and is now BETTER motivated: the
non-throwing families found here (lenient time parse, stoi prefix
truncation) will not announce themselves with a crash anywhere else
either.
