# IO3a — components write their own config files — Handoff (2026-08-31)

**For:** the checking agent.
**Base:** `803d5cbc` (H6a + step 3; heat C API complete).
**Standing findings:** lessons 1–194.
**Implemented syntax-only.** `g++ -fsyntax-only -std=c++20` over the real
include tree: **0 errors** in all five changed sources. Nothing built or run.

```
mod: src/engine/plugins/ProcessComponentRegistry.hpp   (+ComponentConfigSave, +entry field)
mod: src/engine/plugins/ProcessComponentRegistry.cpp   (register_component gains save)
mod: src/engine/transport/components/HeatModule/HeatComponent.cpp      (+saveHeatConfig)
mod: src/engine/transport/components/ReactionModule/ReactionsComponent.cpp (wires the EXISTING serializer)
mod: src/engine/core/InpWriter.cpp                     (the missing call site)
mod: tests/unit/engine/test_heat_sources_api.cpp       (1 gate FLIPPED, +1 new)
```

---

## 1. What was actually wrong

Step 3's finding: `swmm_model_write` emitted the `[PROCESS_COMPONENTS]`
`config=` **path** and copied the file the model was read from, but **nothing
rewrote its content**. Every edit through a C API or the GUI — a
`[HEAT_SOURCES]` temperature, a reaction expression — was **silently lost on
save**, while a hand-edit of the same file persisted.

**The intent was already written down.** `InpWriter`'s own comment at the copy
site says *"the component config FILES are each component's own to write,
never ours."* That division was correct and nothing implemented the other half.

**And for reactions the capability already existed.**
`serializeReactionSystem()` (E-C3, `ReactionsWriter.cpp`) is a complete
canonical `.rxn` serializer — reachable only through the C API's get-text
call. **The writer, the intent, and the serializer were all present; the CALL
SITE was missing.** Reactions adopts saving in this round in one line.

## 2. The seam

```cpp
using ComponentConfigSave =
    std::function<std::string(const SimulationContext& ctx,
                              const ProcessComponentSpec& spec)>;
```

The inverse of `ComponentConfigApply`, with three properties that matter:

1. **`ctx` is const.** A save must not mutate the model it describes — that is
   what makes writing twice produce the same file, and `SaveIsIdempotent`
   gates it.
2. **Empty return means DECLINE**, and the writer falls back to the existing
   carry-alongside copy. This is the whole migration strategy: components
   adopt saving **one at a time**, and any component that has not
   (`water age`, `ARD`) keeps today's behaviour exactly. **No intermediate
   state loses data**, which a flag-day conversion could not promise.
3. **`save` is defaulted to `nullptr`** in `register_component`, so every
   existing registration compiles untouched.

**Two components adopt it here:** heat (`saveHeatConfig`, new) and reactions
(one line onto the existing serializer). Water age and ARD decline and keep
copying — deliberately, so this round's blast radius is two components wide,
not four.

## 3. What `saveHeatConfig` will and will not write

**Only what the model actually SET.** A source sitting at the 20 °C default
with `configured_source == false` produces no row. That is what
`configured_source` was for, and it is the half a naive serializer gets wrong:
writing all seven rows at their defaults makes **every** saved model look
configured, which is a different kind of data corruption — invented
configuration rather than lost.

`[HEAT_FLUXES]` emits only modules that are ON, for the same reason.

**It declines (returns empty) when nothing is configured** — so a heat
component whose file carries sections this function cannot yet render falls
back to the copy rather than truncating them away.

## 4. ⚠ The gate that was written to fail, and did

`NodeOverrideEditsDoNotSurviveASave` pinned the loss and its failure message
said: *"the edit SURVIVED — replace it with a real round-trip assertion rather
than relaxing it."* **That is what happened.** It is now
`SourceEditsSurviveASaveAndReopen`, which edits both row kinds (a GLOBAL the
deck carried, a NODE override it did not — different branches of the
serializer) and additionally asserts that an unconfigured source was **not**
invented by the save.

The old body is **not** kept commented out: its claim is the negation of the
new one, and two contradictory statements of one fact in a single file is how
the next reader believes the wrong one. The file's header comment, which
announced the deliberate absence of a round-trip gate, was corrected in the
same edit — I made it stale and it was mine to fix.

## 5. Validation protocol

1. **`SourceEditsSurviveASaveAndReopen` must FAIL at base** (revert the five
   source hunks, keep the gates). Expect DWF reading **14.5** where 31.0 is
   asserted — the original file copied back over the edit. **Quote it.**
2. `ctest -j8` ×3 against the standing figure. **Reaction round-trip suites
   are the ones to watch**: reactions now writes its config on every save
   where it previously copied. If a reactions gate moves, the serializer and
   the parser disagree somewhere, and **that is a finding about
   `serializeReactionSystem`, not about this wiring.**
3. **Corpus 21/21 `.out` AND `.rpt`.** Corpus decks that use a component
   config now get a *written* file rather than a copied one. **If the written
   file differs from the copied one in a way that changes a run, the
   serializer is lossy** — that is the single most important thing this round
   can be wrong about, and the corpus is what would show it.
4. **Falsifier sweep:**

   | falsifier | expected |
   |---|---|
   | i. make `saveHeatConfig` always return empty | the round-trip gate fails; **the copy fallback still runs**, so nothing is destroyed — confirms declining is safe, which the migration strategy depends on |
   | ii. drop the `configured_source` filter (write all seven rows) | `SourceEditsSurviveASaveAndReopen`'s last leg fails — the save invented a row |
   | iii. remove the reactions `save` hook | reaction suites return to copy behaviour and should still pass — **if any FAILS, it was depending on the write, and that dependency is new information** |
   | iv. write the config before the `.inp` instead of after | should be indistinguishable; if not, there is an ordering coupling nobody has named |
   | v. make `saveHeatConfig` non-idempotent (e.g. append a timestamp) | `SaveIsIdempotent` fails |

5. **Record:** step 1's base numbers, falsifier i and iii, and step 3's corpus
   answer.

## 6. Known gaps

- **Water age and ARD still decline.** IO3b. They keep today's copy behaviour,
  so nothing regresses — but their API edits are still lost, and **the step-3
  finding remains true for them.**
- **The embedded-section case is untouched.** `[REACTION_*]` embedded in the
  `.inp` still cannot be written back, and the warning still fires. IO3c could
  now route them through `save` into a config file, which is what the warning
  already advises. **That warning's advice becomes true for the first time
  once reactions' hook lands** — worth re-reading it then, because it was
  wrong for API editing until this round.
- **No gate asserts the writer's ORDER** (config before or after the `.inp`) —
  falsifier iv probes it but nothing pins it.
- **`saveHeatConfig` does not render `[RADIATIVE_FLUXES]`, `[SOLAR_RADIATION]`
  or `[CLOUD_COVER]`** — H6a's sections. It declines only when *nothing* is
  configured, so a model with radiative config but no sources/fluxes writes a
  file **missing those sections**. ⚠ **This is the one place this round can
  lose data**, and it is the first thing to check: either extend the renderer
  to cover H6a's sections, or make the decline condition "any section I cannot
  render is present". **I judged the second safer but did not implement it —
  flag if you disagree.**

## 7. Prepared commit message

```
feat(io): components write their own config files -- edits survive a save

swmm_model_write emitted the [PROCESS_COMPONENTS] config= path and copied the
file the model was read from, but nothing rewrote its content. Every edit made
through a C API or the GUI -- a [HEAT_SOURCES] temperature, a reaction
expression -- was silently lost on save, while a hand-edit of the same file
persisted. Unlike the embedded-section case it was not even warned.

The intent was already recorded: InpWriter's own comment says the component
config files are "each component's own to write, never ours". Nothing
implemented the other half. For reactions the serializer existed too --
serializeReactionSystem (E-C3) is complete and was reachable only through the
C API's get-text call. The writer, the intent and the serializer were all
present; the call site was missing.

ComponentConfigSave is the inverse of ComponentConfigApply: const ctx, returns
the file text, and an EMPTY return declines so the carry-alongside copy runs
instead. That is what lets components adopt saving one at a time with no
intermediate state losing data -- heat and reactions adopt here, water age and
ARD keep copying.

saveHeatConfig writes only what the model actually set: a source at its default
with configured_source == false produces no row, because a serializer that
writes all seven defaults makes every saved model look configured, which is
invented configuration rather than lost.

test_heat_sources_api's NodeOverrideEditsDoNotSurviveASave -- written to pin
the loss, with a message saying to replace it when serialization landed -- is
now SourceEditsSurviveASaveAndReopen, plus a SaveIsIdempotent gate.

Protocol: plans/transport/IO3A_COMPONENT_SAVE_HANDOFF_2026-08-31.md
```

---

# CHECK RECORD — 2026-08-31

**Verdict: VALIDATED and COMMITTED as engine `4738bca9`** (on `9645f7d0`;
7 files, tree 1921 unchanged, no attribution). Evidence:
`tests/output/io3a_component_save/` (io3a.patch, corpus A/B, PROVENANCE).

## Check-round changes beyond the handoff's implementation

1. **§6's data-loss risk was implemented away, per the handoff's own
   recommendation**: `saveHeatConfig` now declines whenever
   radiative/solar/cloud carry non-default state (`hasUnrenderableSections`,
   field-by-field against default-constructed configs — none of those
   structs has a per-section flag except cloud). New gate
   `UnrenderableSectionsDeclineRatherThanTruncate` pins BOTH halves: the
   radiative section survives a save (copy fallback), and the API edit is
   still lost for such models — the documented IO3b gap, asserted so IO3b
   flips a gate instead of a comment.
2. **`SaveIsIdempotent`'s first spelling was VACUOUS**: it slurped
   `_hs_idem.heat` twice AFTER both saves — comparing the file with itself.
   Repaired to capture between saves; falsifier v (timestamp) now bites,
   proving the gate observes.
3. CHANGELOG entry added ([Unreleased] › Added), hunk-split staged so the
   peer's concurrent Python-bindings entry stayed out of the commit.

## Protocol results

| step | result |
|---|---|
| 1. gates at base | `SourceEditsSurviveASaveAndReopen` FAILS: **"Which is: 14.5 / 31.0"** (`test_heat_sources_api.cpp:353`) — the original file copied back over the edit, exactly as predicted. Preservation gates pass at base by copy |
| 2. ctest ×3 | **184/184 ×3** (the fully green census holds); all six reaction suites green — the serializer and parser agree |
| 3. corpus | **21/21 byte-identical** (`.out` and `.rpt`), incl. heat_parity + heat_lard |
| falsifier i (always decline) | only the round-trip gate fails; 7/8 stay green — **declining is safe**, the migration strategy's load-bearing property |
| falsifier ii (drop configured_source filter) | bites: "the save invented a row the model never set" |
| falsifier iii (remove reactions hook) | all 6 reaction suites still pass — nothing depends on the write |
| falsifier iv (write order) | assessed by reading, not exercised: the hook writes a separate file from const ctx, no shared state with the `.inp` stream; no gate pins order — the handoff's own recorded gap stands |
| falsifier v (timestamp) | bites `SaveIsIdempotent` (only meaningful after fix 2) |

## Note for IO3b and the GUI plan

The save hook receives the writer's DISPLAY-UNITS context copy
(`convert_internal_to_display` — verified it never touches heat or reaction
config, so heat/reactions are unit-neutral). When water age / ARD adopt,
their serializers inherit that context; if any config value is unit-bearing,
display units are the right ones (they match what apply parses), but verify
per component. **G4g's blocker narrows from "no serialization" to IO3b**:
the heat renderer must cover `[RADIATIVE_FLUXES]`/`[SOLAR_RADIATION]`/
`[CLOUD_COVER]` before an editor exposing those sections can promise its
edits survive. `[HEAT_SOURCES]`-only editing round-trips today.
