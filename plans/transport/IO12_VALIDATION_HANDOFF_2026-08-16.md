# IO1–IO2 Implementation — Validation & Commit Handoff (2026-08-16)

**For:** the checking agent (same mandate: sandbox ran `g++ -fsyntax-only`
only; every touched TU passes; nothing linked or executed).
**Base:** `3f56e47a` (post-E2).
**Plan:** `plans/transport/TRANSPORT_IO_PLUGIN_CONFIG_PLAN.md` §7 phases
IO1–IO2 (D-UT8); roadmap Phase 1 order (IO before R1 so the `[REACTION_*]`
parsers are born targeting `model.rxn`).

---

## 1. Changeset (uncommitted)

```
new:  src/engine/input/handlers/ProcessComponentsHandler.{hpp,cpp}
new:  src/engine/plugins/ProcessComponentRegistry.{hpp,cpp}
new:  tests/unit/engine/test_process_components.cpp
mod:  src/engine/core/SimulationContext.hpp   (ProcessComponentSpec + specs vector)
mod:  src/engine/plugins/DefaultInputPlugin.cpp (section registration)
mod:  src/engine/core/SWMMEngine.cpp          (open(): resolution step after the
                                               2D-mesh block; <filesystem> include
                                               moved OUT of the OPENSWMM_HAS_2D
                                               guard — now unconditional)
mod:  src/engine/core/InpWriter.cpp           ([PROCESS_COMPONENTS] round-trip)
mod:  tests/unit/engine/CMakeLists.txt        (test_engine_process_components)
```

## 2. Design notes (review orientation)

- **IO1:** `[PROCESS_COMPONENTS]` rows parse [PLUGINS]-style (first token =
  id; `key="value"` args; `config=` extracted). Resolution runs in
  `SWMMEngine::open()` right after the external-2D-mesh block, mirroring
  its strict/lenient fatality pattern. The registry pre-seeds the six
  planned built-in ids so premature registration yields *"recognized but
  not yet implemented — arrives with plan phase R1/…"* rather than
  "unknown id"; genuinely unknown ids list the known set; library paths
  (contains `/` or `.so/.dylib/.dll`) get the HC2 diagnostic. **These
  diagnostics are the defined IO1 behavior, not inertness** — there is by
  design no silently-accepted-but-ignored registration.
- **IO2:** `read_component_config()` parses the component's `.inp`-dialect
  file: relative path against the .inp directory ([2D_MESH_FILE] §3),
  `;`-comment stripping, upper-cased section tags in file order, fatal on
  missing file, content-before-first-section, or a nested
  `[PROCESS_COMPONENTS]` (no recursion). Delivery via
  `ComponentConfigApply` hooks on a process-global registry
  (`register_component` overwrites — startup/test registration only, not
  thread-safe by contract).
- **Deliberately NOT here** (per plan/CLAUDE.md, don't "fix"): embedded-
  section fallback + style warning (lands with R1, the first component
  that has sections to embed); toggle↔registration consistency rules
  (land with each component's coarse toggle); IO3 per-component
  `saveData()` (InpWriter here round-trips only the *registration line* —
  config files are each component's own to write); GeoPackage embed (IO4);
  C/Python/MCP surfaces (IO5).
- First production consumer is **R1** (reactions). Until then the only
  apply-hooks are test-registered — which is what the IO2 gates use.

## 3. Validation protocol

1. Build; zero new warnings from touched files. Watch the `<filesystem>`
   include move: non-2D builds (`OPENSWMM_HAS_2D` off) now also include it
   — confirm such a configuration still builds if you routinely produce one.
2. `ctest -R test_engine_process_components` — four gates:
   UnknownIdFailsStrictOpen (lists known ids), PlannedIdReportsPendingPhase
   (R1 in message), RegisteredComponentReceivesConfig (sections in order +
   relative path + args + spec round-trip), MissingConfigAndNestingAreFatal.
   *Anticipated failure modes:* (a) `ctx.errors`-nonempty vs open() return
   contract — I assumed pushing to `ctx.errors` before the resolution step
   returns `SWMM_ERR_PARSE` on strict open; verify the two parse-error
   tests actually fail the open rather than merely recording; (b) the
   tests run with `WORKING_DIRECTORY tests/unit/engine/data` — they write
   `_pc_*` artifacts there (house convention), confirm cleanup leaves no
   strays; (c) `write_open_failure_report()` call in the new block — check
   it behaves when rpt path is a test throwaway.
3. Full suite: the section registry / options / plugin-lifecycle /
   InpWriter tests must be untouched; a deck WITHOUT the section must be
   bit-identical end-to-end (the new open() block is gated on
   `!process_component_specs.empty()`).
4. Round-trip check: open a deck with a registration line + args, write
   with InpWriter, reopen — spec identical (id, config path, args order).
5. Lenient-open check (GUI editor path): a bad registration under lenient
   open records the error and continues (mirrors the mesh-file behavior) —
   exercise via the same mechanism the mesh-file lenient tests use, if any
   exist; otherwise record as untested.
6. Append results to §5; commit with §4.

## 4. Commit message

```
feat(io): [PROCESS_COMPONENTS] registration + component config files (IO1-IO2)

Adds the D-UT8 registration section (PLUGINS-style rows: component id +
key="value" args, config="…" extracted), spec plumbing on SimulationContext,
InpWriter round-trip of registration lines, and the process-component
registry with config-file delivery: .inp-dialect section parsing, relative
paths against the parent .inp per the [2D_MESH_FILE] rules, fatal on
missing/nested/preamble content, strict/lenient open handling mirroring the
external 2D mesh. Six planned built-in ids pre-seeded so premature
registration diagnoses "arrives with plan phase X" instead of "unknown id";
library-path ids reserved for HC2. First production consumer: R1
(reactions plan) — until then apply-hooks are test-registered (4 gates).

Plan: plans/transport/TRANSPORT_IO_PLUGIN_CONFIG_PLAN.md §7 IO1-IO2.
Validation record: plans/transport/IO12_VALIDATION_HANDOFF_2026-08-16.md
```

## 5. Validation results

Validated and committed 2026-08-16 on base `3f56e47a` (HEAD matched the stated
base exactly; the working tree carried precisely the ten files of §1 and
nothing else). Artifacts: `tests/output/io12_validation_2026-08-16/`
(CLAUDE.md §4.1 — reviewable, not committed).

### 5.1 Build (protocol 1)

`GLOB_RECURSE` in `src/engine/CMakeLists.txt:30` has no `CONFIGURE_DEPENDS`,
so a **reconfigure is required** before the two new `.cpp` are compiled at
all — worth knowing for anyone applying this changeset by patch. After
`cmake -S . -B build/darwin`, the build is clean (rc=0, 247 targets,
`build.log`).

**Warnings from touched files: none new.** Four appear in `SWMMEngine.cpp`
(`SWMM_ERR_MEMORY`, `SWMM_ERR_FILE_NOT_FOUND`, `RAIN_TO_FTSEC`, `OMEGA_NC` —
all `-Wunused-*`); each is a pre-existing declaration, verified present and
identical at HEAD (lines 83/84/1974/2720, the +24-line offset the changeset
introduces above them). Zero warnings from
`ProcessComponentsHandler.{hpp,cpp}`, `ProcessComponentRegistry.{hpp,cpp}` or
the new test.

One *additional instance* of the pre-existing
`TableData.hpp:708 -Wmissing-field-initializers` (122 instances repo-wide)
is now emitted for the test TU, because the §5.4 round-trip strengthening
adds `#include "core/InpWriter.hpp"` there. Same warning, no new class —
`test_gage_file_roundtrip.cpp` already emits it for the same reason.

**`<filesystem>` include move, non-2D build:** I do not routinely produce
one, so rather than record it untested I syntax-checked the TU directly —
`SWMMEngine.cpp` recompiled with the project's exact flags minus
`-DOPENSWMM_HAS_2D=1`: **rc=0, 0 errors**. The move can only *add* an include
to non-2D builds, never remove one, and `ProcessComponentRegistry.cpp`
includes `<filesystem>` unconditionally regardless.

### 5.2 Gates (protocol 2) — 4/4 pass

`ctest -R test_engine_process_components` → Passed, 0.01 s
(`gates.log`): `UnknownIdFailsStrictOpen`, `PlannedIdReportsPendingPhase`,
`RegisteredComponentReceivesConfig`, `MissingConfigAndNestingAreFatal`.

Anticipated failure modes, all clear:

- **(a) `ctx.errors` vs the open() contract** — the assumption holds. Strict
  open returns `SWMM_ERR_PARSE` (rc=5) for both parse-error gates. Verified
  independently of gtest via `lenient_probe` (§5.5), and attributable to the
  new block rather than the deck: the third gate opens the *same* deck with a
  valid registration and asserts `SWMM_OK`.
- **(b) `_pc_*` artifacts** — the tests remove their `.inp`/`.cfg`; the four
  failure-report `.rpt` files remain in `tests/unit/engine/data/`, which is
  the house convention and covered by `.gitignore:82`
  (`tests/unit/engine/data/**/_*`). No repo strays.
- **(c) `write_open_failure_report()` on a throwaway rpt path** — behaves;
  the four reports are written correctly and are the artifacts in (b).

### 5.3 No-regression (protocol 3)

Full suite **129/130**, twice (before and after the §5.4 test change):
`ctest_full.log`, `ctest_final.log`. The single failure is
`FvEngine.RefiningTheMeshConvergesTowardTheDynwaveHydrograph`, the known
pre-existing one, reproducing **bit-identical** values
(`e_mid 0.055224237275644343` vs `e_coarse 0.052534507871460516`) to the
baseline recorded before E0. Section-registry, options, plugin-lifecycle and
InpWriter tests all pass untouched.

**Decks without the section are byte-identical.** Ten E0 decks (FV / DW / VJ
/ slot coverage, established deterministic in E0 §4) run through the CLI
built at HEAD and at this changeset — shelving the changeset via
`git stash` + physically moving the five new files aside, reconfiguring so
the glob genuinely dropped them (`grep -c ProcessComponent build.ninja` → 0):

| comparison | result |
|---|---|
| `.out` binaries, sha256 | **10/10 identical** |
| `.rpt`, timestamp/version lines filtered | **10/10 identical** |

`run_decks.sh`, `io12_base/`, `io12_new/`. This is the expected outcome — the
open() block is gated on `!process_component_specs.empty()` and the InpWriter
block on the same — but it is now measured rather than assumed.

### 5.4 Round-trip (protocol 4) — gate strengthened

The delivered `RegisteredComponentReceivesConfig` did **not** perform two of
the four checks §3.2 credits it with, so I gave it the teeth it claimed:

1. **Relative-path resolution was never exercised.** The deck was written to
   the process CWD, so `base_dir` was always `""` and resolution-against-the-
   `.inp`-directory and resolution-against-the-CWD were indistinguishable.
   Proof it was toothless: with `base_dir` deliberately forced empty in
   `SWMMEngine::open()`, the gate as delivered still **passed**. The deck and
   its config now live in a `_pc_sub/` subdirectory; under the same probe the
   gate now **fails** at the open (`test_process_components.cpp:168`,
   config unresolvable). Probe reverted; diff re-verified back to
   `25 insertions(+), 1 deletion(-)`.
2. **The "round-trip" block never called InpWriter** — it re-read `ctx` from
   the same open. It now writes the model with
   `openswmm::inp_writer::writeInpFile()`, destroys the engine, reopens the
   *written* deck, and asserts id, `config_path`, and the `version="9.9"`
   arg survive, plus `cap.calls == 1` so the reopened deck genuinely
   re-delivers the config. Passes.

Both changes are confined to the test file.

### 5.5 Lenient open (protocol 5)

Exercised through the C API (`swmm_engine_set_lenient_open`) rather than
recorded as untested — `lenient_probe.c` / `.log`, on a deck with an unknown
component id:

| open | rc | errors | model after open |
|---|---|---|---|
| strict | **5** (`SWMM_ERR_PARSE`) | 1 | — |
| lenient | **0** | 1 (same message) | 2 nodes, 1 link, intact |

Exactly the external-2D-mesh behavior the block is modeled on: fatal when
strict, recorded and survivable for the GUI editor path.

### 5.6 Open, NOT fixed (reported per CLAUDE.md §3, not "improved")

1. **`InpWriter` emits `config_path` verbatim, bypassing the Slice IO-4
   rebase.** — **FIXED, see §5.8.** Every other external-path slot goes through
   `emit_path_token(slot, dst_dir, force_abs_paths, warnings)`, which turns
   an absolute stored path into one relative to the destination directory;
   the new block `fprintf`s the token as-is
   (`InpWriter.cpp`, `[PROCESS_COMPONENTS]`). So an absolute `config=` set by
   the GUI stays absolute on save. This is a code reading, not an empirical
   result — no implemented component exists yet to save. Deliberately left:
   IO3 owns the writer phase, and rebasing the reference without also
   carrying the config file (the `[2D_MESH_FILE]` branch writes its sidecar
   alongside) would point a relative path at a file that isn't there. The two
   belong in one change.
2. **Duplicate ids are not detected** — the plan's IO1 verify list names
   "dup ids". Two rows for the same id resolve independently: measured via
   `dupid_probe`, two identical diagnostics for one duplicated id. For an
   *implemented* component that means `apply()` runs twice with two different
   configs and undefined precedence. Left for R1, the first real consumer,
   which is where the merge-vs-reject semantics can actually be decided.
3. **Toggle↔registration consistency** (plan IO1) — deferred by §2 of this
   handoff, by design, to each component's coarse toggle. Recorded here only
   so the plan's IO1 checklist is not read as complete.

### 5.7 Notes

- The pre-seeded planned-id diagnostic works as designed and reads well:
  `'org.hydrocouple.openswmm.reactions' (Multispecies reaction system
  (EPANET-MSX conventions)) is recognized but not yet implemented — arrives
  with plan phase R1 (reactions plan).`
- `known_ids()` output is `std::map`-ordered, so the unknown-id diagnostic
  lists ids alphabetically and deterministically — worth keeping if a test
  ever asserts on the whole string.
- The registry is process-global and `register_component` overwrites, so
  gtest's shared process is safe across repeated runs; the header states the
  registration-time thread-safety contract.

Committed as **`64c831d6`** with the §4 message. Not pushed.

### 5.8 Follow-up: Slice IO-4 rebase fixed (§5.6 item 1)

Fixed on request, after `64c831d6`. Reproduced first as a failing gate
(CLAUDE.md §4), so this is no longer the code reading §5.6 recorded:

```
absolute config path written verbatim, bypassing the Slice IO-4 rebase
every other external-file slot honors:
org.test.iogate3 config="…/tests/unit/engine/data/_pc_abs/_pc_abs.cfg"
```

**Fix:** the `config=` token now goes through
`emit_path_token(pc.config_path, dst_dir, force_abs_paths, warnings)` — the
same Slice IO-4 helper as every other external-file slot. `FilePathPair`'s
implicit string constructor assigns to `.original`, which lands exactly on
the helper's documented "resolver pass never ran" branch: an absolute token
is rebased against the destination directory, a relative one passes through
untouched, `WRITE_ABSOLUTE_PATHS` still opts out, and a cross-volume or
over-depth destination falls back to absolute *with* a portability warning
instead of silently. Two lines in `InpWriter.cpp`, inside the block that only
runs when the section is non-empty.

**New gate** `AbsoluteConfigPathIsRebasedOnWrite` — asserts the emitted row
carries no absolute directory, carries `config="_pc_abs.cfg"`, and that the
rebased deck **still opens**, so the check cannot be satisfied by emitting a
merely-relative-looking token that resolves nowhere. 5/5 gates,
suite **129/130** (`ctest_rebase_fix.log`, same known FV failure).

**The reason §5.6 gave for deferring was wrong for this case.** I argued a
rebase would point at a file that isn't there — but `makeRelative` of an
absolute config against the destination yields a path that resolves to the
*same* file (`/A/model.cfg` saved into `/B` becomes `../A/model.cfg`). The
worry only ever applied to a **relative** token saved into a different
directory, which this change does not touch: it still passes through, exactly
as the `[2D_MESH_FILE]` reference does.

**Still open, and now precisely stated:** a *relative* `config=` saved to a
different directory dangles, because the writer has no anchor to rebase it
against. The house fix is the three-step pattern the changeset skipped —
store `config_path` as a `FilePathPair`, have
`resolve_file_paths(ctx, anchor_dir)` populate `.absolute` from the source
`.inp` directory, then emit (step 3 is now done). That changes a struct field
consumed by the handler, the registry and any future GUI/API caller, and it
pairs naturally with IO3's per-component `saveData()`, which is what actually
carries the config file to the new directory. Left for IO3 rather than
half-done here.

Committed as **`14755a32`**.
