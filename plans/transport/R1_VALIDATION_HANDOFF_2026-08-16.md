# T0a + R1 Implementation — Validation & Commit Handoff (2026-08-16)

**For:** the checking agent (sandbox: `g++ -fsyntax-only` only, all TUs pass;
nothing linked/executed).
**Base:** `64c831d6` (post-IO1/IO2).
**Plans:** `MULTISPECIES_REACTIONS_MSX_PLAN.md` §5 R1 (+ the two carried IO
obligations resolved here); master plan §4.1 T0a.
**Patch-application note (standing finding):** `src/engine/CMakeLists.txt`
globs without `CONFIGURE_DEPENDS` — **reconfigure before building** or the
new `.cpp` files silently don't compile.

---

## 1. Changeset (uncommitted)

```
new:  src/engine/data/SpeciesRegistry.hpp                (T0a)
new:  src/engine/data/ReactionData.hpp                   (R1, hot/cold split D-L3)
new:  src/engine/transport/components/ReactionModule/ReactionsComponent.{hpp,cpp}
new:  tests/unit/engine/test_reactions_config.cpp        (6 gates, falsifiers stated)
mod:  src/engine/core/SimulationContext.hpp   (species_registry, reactions,
                                               embedded_component_sections)
mod:  src/engine/plugins/DefaultInputPlugin.cpp (embedded [REACTION_*] capture)
mod:  src/engine/plugins/ProcessComponentRegistry.cpp (duplicate-id refusal)
mod:  src/engine/core/SWMMEngine.cpp          (registry build from pollutants;
                                               registerReactionsComponent();
                                               embedded fallback after resolve)
mod:  tests/unit/engine/CMakeLists.txt        (test_engine_reactions_config)
```

## 2. What this adds

- **T0a species registry** (`ctx.species_registry`): pollutants occupy the
  first slots index-aligned with the legacy pollutant index; MSX species
  append with MSX_BULK/MSX_WALL kinds; name uniqueness enforced globally.
  Honest scope: `transported_count()` still equals the pollutant count —
  engines re-point at R6/E4 (documented in the header).
- **R1 reactions component** (`org.hydrocouple.openswmm.reactions`,
  registered idempotently in `open()`): parses `model.rxn`
  ([REACTION_OPTIONS/SPECIES/COEFFICIENTS/TERMS/PIPES/TANKS/QUALITY-GLOBAL])
  into `ctx.reactions` with full structural/reference validation (declared
  species, unique names across species/coefs/terms, no pollutant collision,
  valid forms/options). Expression BODIES are stored as source — compilation
  is R2's Tier-1 VM. Later-phase sections ([REACTION_SOURCES/PARAMETERS/
  PATTERNS/REPORT/SUBCATCHMENTS]) and NODE/LINK quality scopes produce
  "arrives with plan phase …" errors, never silent acceptance.
- **Duplicate `[PROCESS_COMPONENTS]` id ⇒ validation error** (carried IO
  obligation, resolved per the recorded proposal).
- **Embedded fallback** (carried IO obligation): [REACTION_*] sections in
  the legacy .inp are captured by DefaultInputPlugin and applied after
  component resolution with the D-UT8 style warning; when an external
  reactions component is registered, the external file wins wholesale and
  the embedded sections are reported IGNORED.

## 3. Known limitations (verify the behavior, don't fix)

- Embedded [REACTION_*] sections do **not** survive an InpWriter round-trip
  (the writer has no reactions serialization until IO3's per-component
  `saveData()`; the GUI's externalize-migration is the intended path).
  Verify a warning-worthy scenario: embedded-only deck → save via
  InpWriter → sections gone silently. If you judge silent loss
  unacceptable even short-term, the minimal patch is an InpWriter warning
  when `ctx.reactions.configured && !embedded_component_sections.empty()`
  — your call, record either way.
- Wall species carry no area normalization yet (Av arrives with R2/R3
  hydraulic variables).
- `.msx` translator script deferred to R3 validation (needs runnable
  kinetics to be worth translating against).
- GeoPackage round-trip of reactions config = IO4.

## 4. Validation protocol

1. **Reconfigure**, build, zero new warnings from touched files.
2. `ctest -R test_engine_reactions_config` — six gates; each states its
   falsifier in the file header. Independently probe at least:
   (a) comment out `transport::registerReactionsComponent()` in
   `SWMMEngine.cpp` → FullConfigParsesAndPopulates must fail (planned-id
   error path takes over); (b) revert the duplicate-id block in
   `ProcessComponentRegistry.cpp` → DuplicateProcessComponentIdFails must
   fail. Restore both.
3. `ctest -R test_engine_process_components` — the IO gates must stay
   green (PlannedIdReportsPendingPhase now exercises a DIFFERENT planned id
   path: reactions is implemented, so that gate's deck uses the reactions
   id and expects "not yet implemented"… **check this**: the gate as
   written registers `org.hydrocouple.openswmm.reactions` expecting the
   planned-id error, but R1 now registers the real component in open() —
   the gate will now take the missing-config-file path instead.
   **Anticipated failure: PlannedIdReportsPendingPhase needs its id
   switched to a still-planned one (e.g. org.hydrocouple.openswmm.heat)**
   — that is an intended test update, make it and note it.
4. Full suite; decks without [REACTION_*]/[PROCESS_COMPONENTS] bit-identical
   (registry build is additive; probe: sha256 a couple of quality decks
   pre/post like the IO validation did).
5. Smoke: the E1/E2 ARD gates unchanged (species registry does not perturb
   n_pollutants sizing).
6. Append results to §6; commit with §5.

## 5. Commit message

```
feat(reactions): species registry (T0a) + reactions component parsing (R1)

Adds the master-plan §4.1 species registry (pollutants index-aligned first,
MSX bulk/wall appended, global name uniqueness) and the
org.hydrocouple.openswmm.reactions process component: model.rxn parsing of
REACTION_OPTIONS/SPECIES/COEFFICIENTS/TERMS/PIPES/TANKS/QUALITY-GLOBAL with
full structural+reference validation; expression sources stored for R2's
Tier-1 VM; later-phase sections error with their plan phase. Resolves both
carried IO obligations: duplicate [PROCESS_COMPONENTS] ids are refused, and
embedded [REACTION_*] sections apply with the D-UT8 style warning (external
file wins wholesale). Gates: tests/unit/engine/test_reactions_config.cpp
(6, falsifiers stated).

Plans: MULTISPECIES_REACTIONS_MSX_PLAN.md §5 R1; master plan §4.1 T0a.
Validation record: plans/transport/R1_VALIDATION_HANDOFF_2026-08-16.md
```

## 6. Validation results

Validated and committed 2026-08-16. Base was stated as `64c831d6`; HEAD was
`14755a32` (the IO12 follow-up fixing the Slice IO-4 rebase, IO12 §5.8). Those
touch `InpWriter.cpp` and `test_process_components.cpp`, neither of which this
changeset modifies, so the changeset applied to HEAD unchanged — the working
tree carried exactly the ten files of §1. Artifacts:
`tests/output/r1_validation_2026-08-16/` (not committed).

Two commits:

- **`756afa6e`** — T0a + R1, §5 message plus a paragraph on the gate
  strengthening below.
- **`9d0dbbff`** — the §3 judgment call on silent save loss (§6.6).

### 6.1 Build (protocol 1)

Reconfigured first per the standing finding. Build clean (rc=0, 250 targets).
**No new warnings from touched files:** the four `-Wunused-*` in
`SWMMEngine.cpp` are the same pre-existing declarations tracked since IO12
(now lines 85/86/2023/2769), and the new `ReactionsComponent` TU surfaces one
more instance of the repo-wide `TableData.hpp:708
-Wmissing-field-initializers` (122 instances). No new warning class.

### 6.2 Gates (protocol 2) — 6/6, but two had no teeth as written

`test_engine_reactions_config` passes 6/6 on the first run. The independent
probes §4.2 asked for are the interesting part — **both found a gate that
passes for the wrong reason**:

| probe | required outcome | as delivered |
|---|---|---|
| (a) comment out `registerReactionsComponent()` | `FullConfigParsesAndPopulates` fails | **fails** ✓ (plus 3 others) |
| (b) revert the duplicate-id block | `DuplicateProcessComponentIdFails` fails | **still passes** ✗ |

- **`DuplicateProcessComponentIdFailsOpen` had no teeth.** With the registry
  check removed the open still fails — but from `applyReactionSections`'s
  independent `"Reactions system configured twice — duplicate registration or
  embedded sections…"` guard, whose text *contains the substring* the gate
  matched. Verified by running the deck through the CLI under probe (b):
  `Error opening input file: Reactions system configured twice — …`. Two
  defenses, one phrase, and the gate could not tell them apart. Now asserts
  the registry's own wording, `"each component id may appear once"`; re-probed
  → **fails** as required.
- **`LaterPhaseSectionFailsWithPhaseName` had a false-pass mode**, found via
  probe (a): it matched only `"arrives with plan phase"`, which the
  *planned-id* diagnostic also contains — so with the component never
  registered at all, the gate still passed. Now asserts
  `"[REACTION_SOURCES] is recognized but not yet supported — arrives with plan
  phase"`; re-probed under (a) → **fails** as required.

Both probes reverted; `git diff --stat` re-verified back to the delivered
shape before committing.

### 6.3 IO gates (protocol 3) — the anticipated update, confirmed

`PlannedIdReportsPendingPhase` failed exactly as §4.3 predicted, and for the
predicted reason: R1 registers the real reactions component, so that id takes
the implemented path and hits missing-config instead. Switched to
`org.hydrocouple.openswmm.heat` / phase `H1`, with a comment to move it again
when H1 lands. All 5 IO gates green.

### 6.4 No-regression (protocol 4)

Full suite **130/131** (`ctest_full.log`) and again after §6.6
(`ctest_savewarn.log`). The single failure is the known pre-existing
`FvEngine.RefiningTheMeshConvergesTowardTheDynwaveHydrograph`.

**Bit-identical decks — and this one needed measuring, not asserting.** R1
removed the `if (!process_component_specs.empty())` gate, so
`registerReactionsComponent()`, the species-registry build and
`applyEmbeddedReactionSections()` now run on **every** open; the structural
argument IO12 §5.3 relied on no longer holds. Fourteen decks (the ten E0
hydraulics decks + four E2 quality decks, since the registry is built from the
pollutant list) run through the CLI at HEAD and at this changeset, shelving it
and reconfiguring so the glob dropped it (`grep -c ReactionsComponent
build.ninja` → 0):

| comparison | result |
|---|---|
| `.out` binaries, sha256 | **14/14 identical** |
| `.rpt`, timestamp/version lines filtered | **14/14 identical** |

### 6.5 Smoke (protocol 5)

`test_engine_ard_transport` passes unchanged — the species registry is
additive and does not perturb `n_pollutants` sizing.

### 6.6 The §3 judgment call: silent save loss — **warn** (`9d0dbbff`)

Measured before deciding (`embedded_save_probe.cpp`, before/after logs):

```
open rc=0
reactions.configured=1  n_species=1  embedded_sections=1
writeInpFile rc=0  writer warnings=0        ← before
[REACTION_*] sections in the saved .inp: 0  (11 sections, none of them reactions)
```

Open an embedded-reactions deck, save it, and a live, applied
`[REACTION_SPECIES]` block is gone — rc=0, not one warning. That is the GUI's
normal save path destroying user-authored model data, so I took the patch §3
offered. The sections still cannot be written (no per-component `saveData()`
until IO3); what changed is that the writer now names what it dropped, into
the `warnings` sink callers already receive.

One deviation from §3's suggested condition: it proposed
`configured && !embedded_component_sections.empty()`. I fire on
`!embedded_component_sections.empty()` alone — in the external-wins case
`configured` is true but the embedded sections were *ignored*, and they are
dropped from the save just the same, so the narrower condition would stay
silent in a case where text the user wrote still disappears. Gate:
`EmbeddedSectionsLostOnSaveAreReported` (7 gates now).

### 6.7 Verified, not fixed

- **The embedded style warning reaches the .rpt** — checked directly rather
  than assumed, since a warning that only reaches `ctx.warnings` would be
  invisible in the report. It appears in the warning block after the title.
  **But** the component pushes to `ctx.warnings` directly instead of through
  `SWMMEngine::push_report_warning(msg, code)`, so (a) `emit_warning()` never
  fires and an API/GUI consumer subscribed to warnings never sees it, and
  (b) it lacks the house `"WARNING: "` prefix. Not fixed: the apply-hook
  signature has an `errors` sink but no warnings sink, and that asymmetry is
  a component-SDK decision, not a local patch. Minimal fix if wanted: have
  `open()` re-emit warnings appended during the component phase.
- **Species added to the registry before validation completes.** `parseSpecies`
  registers each species as it parses, so a config that later fails leaves MSX
  entries in `ctx.species_registry`. Harmless on strict open (fatal anyway)
  and cleared on the next open, but a lenient/editor open holds a registry
  that reflects a rejected file.
- §3's known limitations (wall-species Av, `.msx` translator, GeoPackage IO4)
  behave as described; `transported_count() == pollutant_count()` as the
  header states.
