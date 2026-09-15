# Saving silently destroyed embedded reaction data — Handoff (2026-08-26)

**For:** the checking agent.
**Base:** HEAD at time of writing (post the reconciliation/G0 round).
**Standing findings:** lessons 1–160.
**Found by:** the 2026-08-26 check of the reconciliation round, which
correctly refused my claim that "the engine warns".

**This is a user-facing data-loss fix.** Open a deck with embedded
`[REACTION_*]`, edit anything, save — before this changeset the reaction
system was gone and **nothing said so, from the GUI included.**

---

## 1. The defect, and why it survived

The notice at `InpWriter.cpp:2580-2586` is real, correct, well-worded code. It
is gated on an **optional `warnings` sink**, and **all three production
callers passed `nullptr`**:

```
openswmm_model_impl.cpp:265   swmm_model_write              → nullptr
openswmm_model_impl.cpp:276   swmm_model_write_with_plugin  → nullptr   ← the GUI's path
DefaultInputPlugin.cpp:203    DefaultInputPlugin::write     → nullptr
```

**The writer could warn; the engine never asked it to.**

**Why no gate caught it:** the existing round-trip coverage calls
`inp_writer::writeInpFile` **directly and hands it a sink**. It therefore
exercises a code path production never takes and certifies a behaviour users
never get. Lesson 91's family, one level out — and the reason §3's gate is
built the way it is.

## 2. The fix — warn, not refuse, and the reasoning

`ctx.warnings` is the vector `swmm_get_warning_count`/`swmm_get_warning_at`
already read (`openswmm_engine_impl.cpp:242-252`) and the GUI already
consumes. Both C API write paths now collect the writer's sink and forward
into it.

**The save still SUCCEEDS.** The defect was the *silence*, not the policy —
warn-and-proceed is the writer's own stated intent, and refusing would be a
behaviour change beyond the defect (CLAUDE.md §2). **Whether a save that loses
model data should refuse outright is a real question and is deliberately left
to IO3**, where per-component `saveData()` removes the loss and makes the
question moot. Flag it if you disagree — it is the one policy call here.

**⚠ The plugin path is a partial fix and I want that visible.**
`IInputPlugin::write` receives a **`const` context by design**, so it cannot
reach `ctx.warnings`. It now collects the sink and appends to `last_error_`
**only when the write actually fails**. On success the warnings are collected
and **deliberately discarded**:

> **A defect I introduced and caught before shipping.** My first draft wrote
> the warnings into `last_error_` on a *successful* write. That is worse than
> the silence it replaces — a caller checking `last_error_message()` after a
> successful save would read a warning as a failure. Reverted; the residual
> gap is recorded instead of papered over. Closing it properly needs a
> non-error diagnostic channel on `IInputPlugin`, which is an interface change
> and not this round's.

```
mod: src/engine/core/openswmm_model_impl.cpp      (+ forwardWriteWarnings, 2 call sites)
mod: src/engine/plugins/DefaultInputPlugin.cpp    (sink on the failure path only)
mod: tests/unit/engine/test_process_components.cpp (+1 gate)
```

## 3. The gate — it drives the production path and nothing else

`SavingWarnsThroughTheApiWhenEmbeddedSectionsAreLost`:

- builds a deck with an **embedded** `[REACTION_OPTIONS]` (no external config
  — the configuration that gets dropped);
- **SETUP leg** asserts `ctx.embedded_component_sections` is non-empty, so a
  deck that fails to carry one fails loudly instead of proving nothing;
- calls **`swmm_model_write_with_plugin(engine, path, "")`** — literally the
  GUI's save — and reads **`swmm_get_warning_at`**, literally what the GUI
  reads. **It touches `inp_writer::writeInpFile` nowhere**;
- asserts a warning appeared, that it says **"lost from this save"**, and that
  it **names `REACTION_OPTIONS`** — a warning that does not name the section
  cannot tell a user what to rescue;
- asserts the saved deck really **lacks** the section, because the warning's
  claim must stay true or the warning becomes a lie.

Fixture names checked against the `b85b802d` guard's rule: `_pc_embed*` are
unique across all test sources.

## 4. Validation protocol

1. **The gate must FAIL at base** — revert the two source hunks, keep the
   gate. Expect *"saving destroyed the embedded reaction sections and emitted
   NO warning through the C API"*. **Quote it.** If it passes at base,
   something else already forwards warnings and my §1 is wrong.
2. `ctest -j8` ×3. Expect the standing figure (**177** registered tests as
   measured 2026-08-26) with no new failure. `test_engine_report_timing` is a
   known parallel-run flake — passes standalone.
3. **Corpus: 19/19 `.out` identical, matched configs, guard silent.** This
   round changes only what is *reported*, never what is *computed*. **A moved
   deck is a finding.**
4. **⚠ Check `.rpt` files too.** I do not believe warnings reach the report,
   but I have not confirmed it. If the new warning lands in every `.rpt` on
   every deck, that is a blast radius I did not predict — report it.
5. **Falsifier sweep:**

   | falsifier | expected |
   |---|---|
   | i. restore `nullptr` at `swmm_model_write_with_plugin` only | gate **fails** — that is the GUI path |
   | ii. restore `nullptr` at `swmm_model_write` only | gate **passes** — it drives the other entry point. **Confirms one fix does not cover the other**, and that a second gate is owed for `swmm_model_write` |
   | iii. drop the "names the section" assertion | passes today; guards against a future generic warning that says data was lost without saying which |
   | iv. drop the SETUP leg and remove `[REACTION_OPTIONS]` from the deck | gate passes **vacuously** — confirms the SETUP leg is load-bearing |
   | v. save a deck with **no** embedded sections | **no** warning. A warning that fires when nothing was lost is the "fires every time" failure of lesson 148 |
   | vi. restore `last_error_` on the plugin's success path (my reverted draft) | nothing in this suite fails — **the defect I caught has no gate.** Recorded as owed |

6. **Record:** the base failure message, falsifier ii and vi, and step 4's
   `.rpt` answer.

## 5. Known gaps

- **`DefaultInputPlugin::write` still cannot surface this on success** (§2).
  Real, recorded, needs an interface change.
- **Falsifier vi has no gate** — nothing stops a future edit from putting a
  non-error back in the error channel.
- **Only `swmm_model_write_with_plugin` is gated**; `swmm_model_write` is
  fixed but ungated (falsifier ii).
- **The GUI is not touched.** The warning now reaches the C API; whether the
  GUI *displays* what it reads is a separate question in the other repo, and
  the program review already found the GUI has no handling for this message.
  **The engine's silence is fixed; the GUI's may not be.**
- **This is a mitigation, not the cure.** IO3's per-component `saveData()` is
  what stops the loss. Until then the user is told, not protected.

## 6. Prepared commit message

```
fix(io): saving a model silently destroyed embedded reaction sections

InpWriter has warned since IO3 that embedded [REACTION_*] sections are lost
on save -- but the notice is gated on an optional warnings sink and all three
production callers passed nullptr. The writer could warn; the engine never
asked it to. Opening a deck with embedded reaction sections, editing anything
and saving destroyed the reaction system with no message anywhere, the GUI
included.

No gate caught it because the round-trip coverage calls writeInpFile directly
and hands it a sink -- exercising a path production never takes.

Both C API write paths now forward the writer's warnings into ctx.warnings,
the vector swmm_get_warning_count/at already expose and the GUI already reads.
The save still succeeds: the defect was the silence, not the policy, and
whether a save that loses model data should refuse belongs with IO3, where
per-component saveData() makes the question moot.

IInputPlugin::write takes a const context and cannot reach ctx.warnings, so
it forwards only on the failure path; putting a non-error into an error
channel on success would be worse than the silence.

The gate drives swmm_model_write_with_plugin and reads swmm_get_warning_at --
the GUI's own calls -- and touches the writer's API nowhere.
```

---

# CHECK RECORD — 2026-08-27

**Verdict: the fix is correct and lands. One defect in the gate itself, two
owed gates added, one falsifier expectation of yours is wrong.**

## The defect: the gate could not run

`_pc_embed.inp` carried `[REACTION_OPTIONS]` alone. A reactions config is
rejected without at least one species, so the deck failed to open with
`SWMM_ERR_PARSE`:

> `Reactions config declares no [REACTION_SPECIES] — at least one species is required.`

The gate died at its `open_deck` assertion, **before reaching a single one of
the assertions it exists for** — it never observed the warning, the section
name, or the loss. It cannot have been run before it was handed over. Fixed by
adding `[REACTION_SPECIES] BULK A MG` (species `A` deliberately avoids a
collision with the deck's `TSS` pollutant, which is its own open failure).

## §4.1 — base failure, quoted

With the gate fixed and both source hunks reverted, `warns_after` == 
`warns_before` == 1:

> `saving destroyed the embedded reaction sections and emitted NO warning
> through the C API. The notice exists in InpWriter but is gated on an
> optional sink; if this fails, a production caller is passing nullptr again.`

Exactly as predicted. §1 stands.

## §4.2 — suite

`ctest -j8` ×3 → **177/177 green, all three runs.** Registered count unchanged
(the new gates live in an existing binary). `test_engine_report_timing` did not
flake this time.

## §4.3 / §4.4 — corpus, and the .rpt answer you wanted

**The A/B is dylib-level, not CLI-level, and this matters.** The CLI resolves
the engine through `@rpath/libopenswmm.engine.6.dylib`, so copying two CLI
binaries aside would have compared a program to itself — both would load
whichever dylib happened to be in the build tree. Base and patched *dylibs*
were built, `md5`-confirmed to differ, and swapped in place between passes.

- **19/19 `.out` byte-identical.** No deck moved.
- **19/19 `.rpt` content-identical.** All 19 differ on the run clock only
  (`Analysis begun/ended on:`), and two additionally on `Total elapsed time`
  (3 s vs 2 s). No content difference anywhere.
- **Step 4's open question, answered: the warning reaches NO `.rpt`.**
  `grep 'lost from this save'` across every base and patched report → nothing.
  The blast radius you did not predict is nil, for the structural reason that
  the CLI never calls a write path (`grep swmm_model_write src/cli/` → empty).

Artifacts: `tests/output/embedsec_corpus_2026-08-27/{base,patched}/`.

## §4.5 — falsifier sweep

| # | expected | observed |
|---|---|---|
| i | gate fails | **FAILS** ✓ |
| ii | gate passes — one fix does not cover the other | **PASSES** ✓ — and the owed second gate is now written, see below |
| iii | passes today | trivially true (removing a passing assertion); not built |
| iv | gate "passes vacuously" | **WRONG — the gate FAILS** (`warns_after > warns_before, actual: 0 vs 0`). See below |
| v | no warning when nothing is lost | **CONFIRMED, 0 vs 0** ✓ — the lesson-148 check is clean |
| vi | nothing fails — the caught defect has no gate | **CONFIRMED** ✓ — gate now written |

**Your falsifier iv is incorrect.** Dropping the SETUP leg *and* the embedded
section does not make the gate pass vacuously — it makes it **fail** at
`ASSERT_GT`, because with nothing lost no warning fires. So the SETUP leg is
**not** what stands between this gate and vacuity; `ASSERT_GT` already is. The
leg is still worth keeping — it turns a baffling "0 vs 0" into a sentence
naming the cause — but it is a diagnostic, not the load-bearing guard, and the
sweep should say so.

## Two gates added, closing two of your §5 gaps

- **`SwmmModelWriteAlsoWarnsWhenEmbeddedSectionsAreLost`** — closes "only
  `swmm_model_write_with_plugin` is gated". Falsified by reverting that sink
  alone: fails, while gate 1 stays green.
- **`PluginWriteLeavesTheErrorChannelCleanOnSuccess`** — closes "falsifier vi
  has no gate". Falsified by re-applying your reverted draft (warnings into
  `last_error_` on success): fails.

Both bite; both verified independent of gate 1.

## Gaps that remain, unchanged

`DefaultInputPlugin::write` still cannot surface this on success (needs a
non-error channel on `IInputPlugin`). The GUI is untouched — the engine's
silence is fixed, and whether the GUI *displays* what it now reads is a
separate question in the other repo. This is a mitigation; IO3's
`saveData()` is the cure.

## The policy call

Warn-and-proceed is right, and I would not change it. Refusing the save would
strand a user with an unsaveable model and no way out, which is worse than a
save that tells them what it dropped and names it. Flagging it as owed to IO3
is the correct disposition.
