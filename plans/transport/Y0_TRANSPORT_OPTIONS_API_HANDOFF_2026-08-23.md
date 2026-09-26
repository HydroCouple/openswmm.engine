# Y0 Validation Handoff — The Transport Option Keys Reach the C API (unblocks Y1)

**Date:** 2026-08-23 · **Author:** implementing agent (syntax-only; nothing
executed) · **Step:** subplan **Y0 — a round that was not in the plan**,
opened by a blocker found while surveying for Y1 · **Base:** `d7b6c079`
(X5).

---

## ⛔ Why this round exists (a correction to the subplan, and it is mine)

The subplan §1 says *"Engine option keys for G1g exist (`QUALITY_SOLVER`,
`WATER_AGE`, `ARD_*` aliases, `QUALITY_STEP`) — G1g is unblocked today."*
**That claim was checked against the `[OPTIONS]` PARSER, not against the C
API's key dispatch, and the C API is what the GUI hydrates through.**

Measured: `swmm_options_set(e, "QUALITY_SOLVER", "LAGRANGIAN")` returns
**`SWMM_ERR_BADPARAM`** — the setter's final `else` rejects unknown keys
(no `ext_options` fallthrough on this entry point; that is the separate
`swmm_options_*_ext` surface). Same for all seven keys, in both directions.
The Y1 options page could not have read or written a single one of its
controls.

This is the lesson-26 shape one level up: a prerequisite verified in the
wrong layer. Recorded as such; the subplan §1 bullet is corrected in the
same round.

## 0. Hunk-presence check

| grep (repo root) | expected |
|---|---|
| `grep -c "QUALITY_SOLVER\|WATER_AGE\|HEAT_TRANSPORT\|QUALITY_STEP\|MAX_SEGMENTS_PER_LINK\|DISPERSION\|RWPT_SEED" src/engine/core/openswmm_model_impl.cpp` | **16** |
| `grep -c "^TEST(" tests/unit/engine/test_transport_options_api.cpp` | **4** |
| `grep -c "transport_options_api" tests/unit/engine/CMakeLists.txt` | **1** |

## 1. Changeset

| File | Change |
|---|---|
| `src/engine/core/openswmm_model_impl.cpp` | `swmm_options_get`: seven new key branches (enum → canonical token, bools → YES/NO, numerics → `std::to_string`). `swmm_options_set`: the seven setter branches — enums reject junk (the FV precedent), parser aliases `ARD`/`LARD` accepted, `MAX_SEGMENTS_PER_LINK` floors at 2 and `QUALITY_STEP` at 0 (mirroring the solver's own clamps), `RWPT_SEED` unclamped |
| `tests/unit/engine/test_transport_options_api.cpp` | **NEW** — 4 gates, no decks |
| `tests/unit/engine/CMakeLists.txt` | registered |

## 2. Design decisions (challenge in this order)

1. **Keys are live under ANY solver**, not rejected when inert — the exact
   contract the FV block above them documents ("a GUI can configure FV
   before selecting it"). Rejecting would make the dialog's page order
   load-bearing.
2. **Canonical tokens out, aliases in** — `get` returns `EULERIAN_ARD` /
   `LAGRANGIAN`; `set` also accepts `ARD` / `LARD` because the deck parser
   does. A GUI that learned its vocabulary from a deck must not meet a
   different one here.
3. **Enum keys reject unknown tokens**; bools are permissive
   (`YES/ON/TRUE/1` true, anything else false) — matching `[OPTIONS]`
   parsing exactly, so a round-trip through file and API agree.
4. **The clamps live in BOTH places** (API and solver init). Duplication is
   deliberate: the API clamp makes the value the GUI reads back honest,
   and the solver clamp defends decks that never touch the API.
5. **`WATER_AGE`/`HEAT_TRANSPORT` render as YES/NO**, not ON/OFF — the
   engine's own `engineBoolString` convention, which the GUI's
   `parseEngineBool` already consumes.

## 3. Anticipated failure modes, likelihood order

1. **`QUALITY_STEP` numeric rendering**: `std::to_string(0.0)` is
   `"0.000000"`, and the gate's `opt_equals` parses numerically — but the
   GUI's `optionValueEquals` must do the same or the dialog will rewrite
   the key on every OK. If the hydration-contract test (Y1) flags churn,
   that is the cause and it is a GUI-side fix.
2. **Alias asymmetry**: `get` never returns `ARD`/`LARD`. Intended (§2.2);
   if review wants echo-what-was-set, that changes the InpWriter too.
3. **Corpus**: the keys are additive to the API only; no deck path
   changes. Any corpus movement is a real finding.
4. **`RWPT_SEED` negative** is accepted deliberately (any int is a valid
   RNG key) — if you think it should floor at 0, say so and add a gate.

## 4. Gates

Y0-1 KeysReadTheirDefaults · Y0-2 KeysRoundTripThroughTheApi (both
directions — ON *and* OFF) · Y0-3 EnumKeysRejectJunkAndAcceptAliases (plus
the clamps) · Y0-4 **SettersReachTheEngineState** — the load-bearing one:
a key that round-trips but changes no engine state is exactly the defect
this round removes.

## 5. Falsifier sweep

| # | Falsifier | Must fail |
|---|---|---|
| i | make the setter store into `ext_options` instead of `opt.*` | Y0-4 (round-trips, engine unmoved — the precise defect being fixed) |
| ii | drop the enum rejection (`else` accepts silently) | Y0-3 |
| iii | drop the `ARD`/`LARD` aliases | Y0-3 |
| iv | remove the getter branches (leave the setter) | Y0-1, Y0-2 (`<ERR>`) |
| v | drop the `MAX_SEGMENTS_PER_LINK` floor | Y0-3 (clamp leg) |
| vi | render bools as ON/OFF | Y0-1, Y0-2 |
| vii | `QUALITY_SOLVER` getter returns the alias forms | Y0-2 (canonical expectations) |

## 6. Standing verification

Full suite isolated worktree; **all six LARD/quality suites untouched**.
Corpus **19/19** (API-only change). ASan/UBSan over the new suite. Zero new
warnings. `std::stod`/`std::stoi` throw on garbage numerics — the house
pattern in this file already does this for every numeric key, so it is
consistent, but **note whether a malformed numeric propagates an exception
through the C boundary**; if it does, that is a PRE-EXISTING defect of this
TU (every FV numeric key has it), worth recording separately, not fixing
here.

## 7. On acceptance

Commit; **correct the subplan §1 bullet** ("unblocked today" → "unblocked
by Y0, which added the C API dispatch the claim assumed"); record the
lesson (verify a prerequisite in the LAYER that will consume it); then Y1
proceeds. The GUI plan's §6 prereq list should gain a line: the options
page needs C-API keys, not just parser keys — the same trap waits for
`openswmm_heat.h` at G4g.

---

## 8. Validation results (2026-08-23, validating agent)

**Committed `948b2840`** on `d7b6c079`, branch `swmm6_rel`. Three files,
no shared-file complications. All three §0 greps passed; **all four gates
passed on the first run**; **7/7 falsifiers bite** on their predicted
gates (i — the round's reason for existing — fails Y0-4 with the
"surface bound to nothing" diagnosis, plus Y0-3's rejection legs since a
string table accepts junk).

### §6's exception question, answered by measurement

A malformed numeric DOES propagate `std::invalid_argument` through the C
boundary and TERMINATES the process. Probed both directions:
`FV_CFL = "abc"` aborts identically (pre-existing — every FV numeric key
in this dispatch is raw `std::stod`/`std::stoi`), while `ROUTING_STEP =
"xyz"` survives (rc 0 — it uses the try/catch pattern that also lives in
this same file at :382). So the defect is PRE-EXISTING and TU-wide, Y0's
keys are consistent with the house pattern, and it is recorded here —
not fixed — per §6's instruction. It deserves its own small round: a GUI
transiently holding an empty line edit must not be able to terminate the
host process, and the guard pattern already exists thirty lines away.

### Standing verification

ctest full ×3: standing `test_engine_2d_infil_integration` only. All six
LARD/quality suites plus the two API suites untouched-green. Corpus
**19/19** (base CLI built from HEAD's model_impl, swap-verified restore).
ASan/UBSan clean on the new suite. Zero new warnings.

## 9. Y1 is now actually unblocked

The subplan §1 bullet is corrected in this round (see the edit beside
this handoff). The lesson recorded: **verify a prerequisite in the LAYER
that will consume it** — the parser accepting a key says nothing about
the C API dispatching it. The same trap sits in front of G4g
(`openswmm_heat.h` + the heat option keys) — the GUI plan's prereq list
now says so.


> **2026-08-24:** the unguarded-stod termination this handoff recorded is FIXED by round H1 (`H1_CAPI_NUMERIC_HARDENING_HANDOFF_2026-08-23.md` §9) — exception guard + strict full-consumption parses across the whole swmm_options_set dispatch.
