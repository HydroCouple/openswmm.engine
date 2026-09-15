# Finalizing the Unified Transport Program — Sequenced Handoffs (2026-08-29)

**For:** the implementing and checking agents, working one step at a time.
**Status source:** `UNIFIED_PLAN_STATUS_2026-08-29.md`.
**Standing findings:** lessons 1–170 in `IMPLEMENTATION_ROADMAP.md`.

**How to use this.** Steps are ordered by dependency, not by plan lineage. Each
is **one implement→validate loop**: implement, hand off, the checker compiles,
runs, sweeps the falsifiers, reports; then the round is marked complete or
iterated. **Do not batch steps.** Every multi-step batch in this program's
history hid a defect in the step that was not separately observed.

**Finish line.** After step 7 the 1D water-quality program is complete: every
capability reachable from a deck, from the GUI, and from the API, with an
observer for each. Steps 8–9 are the debts that outlive it.

---

## 0. Standing protocol — applies to EVERY step, not repeated below

1. **Shared tree.** Both repos have concurrent sessions. Stage narrowly by
   path or by patch; **never `git commit -a`**; never stage a shared file from
   a copy. If `.git/index.lock` exists, check for a live git process before
   touching it.
2. **The corpus is the inertness proof.** `tests/parity/run_corpus.sh`,
   **20 decks**, matched build configs. A moved deck on a change meant to be
   inert **is a finding, not noise**. Two builds means two build *directories*,
   and an A/B swaps the **dylib** — the CLI hash is not the engine's identity
   (lesson 135).
3. **`ctest -j8` ×3.** Standing figure **177** as measured 2026-08-26.
   `test_engine_report_timing` flakes under `-j8` and passes standalone.
4. **A round without an observer for its own claim is not finished — it is a
   claim.** Every step below names its gate.
5. **Run the vacuous case.** Falsifier predictions are claims like any other
   (lesson 163); a predicted-pass that fails is a finding, not an error.
6. **Record in `IMPLEMENTATION_ROADMAP.md`, not only in the handoff.** Five
   stale rows in the last stock-take existed because rounds reported into the
   document that requested them (lesson 167).
7. **Parity with legacy is the default.** Diverge only for a demonstrated
   formulation error, and record it in `SNOW_DIVERGENCE_REGISTER.md`.

---

## Step 1 — P1.4: negative `[TRANSPORT_SOURCES]` rows · 1 round

**⚠ BLOCKED ON A DECISION. Do not start until it is answered.**

D-NS1 (negative sources = extraction) is implemented at the **node** seam in
all three engines. ARD's **cell** sources were deliberately excluded (X6 §2.5)
because they have their own conservation story, so a negative cell source is
**unspecified behaviour today**.

> **The decision:** does a clamped cell extraction get **its own ledger row**,
> or does it **ride `qual_routing_ex_in`**?
>
> - *Own row* — visible, matches D-NS1's "count and summarise" contract, and
>   the clamp is auditable. **Cost: the `.rpt` moves on every ARD deck with
>   sources**, so the corpus expectation changes and every stored report is
>   stale.
> - *Ride the existing row* — no `.rpt` movement, corpus stays byte-clean on
>   `.rpt` as well as `.out`. **Cost: the clamp is invisible in the ledger**,
>   so a model silently extracting more than it holds looks identical to one
>   that isn't.
>
> **My recommendation: its own row.** D-NS1 chose visibility at the node seam,
> and a cell clamp that cannot be seen is the harder failure to diagnose later.
> But this is a reporting-contract call and the blast radius is yours to
> accept, so it is not mine to make silently.

**Implementation once decided.** Mirror D-NS1's contract at the cell seam:
parse-time warning on a negative row, clamp to the cell's held mass, book what
actually left, count and summarise.

**Changeset shape**
```
mod: ArdConfig.cpp            (parse warning on a negative source row)
mod: ArdEngine.cpp            (clamp to held mass at the source application)
mod: the ARD ledger           (per the decision above)
mod: tests/unit/engine/…      (+2 gates)
```

**Gates.** (a) A negative cell source removes mass and the ledger books
exactly what left. (b) A negative source larger than the cell's held mass is
clamped, and the deficit is counted — **not** silently satisfied by driving
the cell negative.

**Falsifier sweep**

| falsifier | expected |
|---|---|
| i. remove the clamp | gate (b) fails with a negative concentration |
| ii. clamp but do not book | gate (a) fails: mass leaves the system unrecorded — the ledger's whole purpose |
| iii. a POSITIVE source deck | unchanged, byte-identical. **A clamp that fires on ordinary decks is lesson 148's failure** |
| iv. a negative source of exactly the held mass | boundary: expect empty cell, no deficit counted, no warning |

**Done when:** both gates pass, iii is byte-identical, and the `.rpt` blast
radius matches what the decision predicted.

---

## Step 2 — H7: heat under LARD · 2 rounds

**Why here:** it is the only step that unblocks another track (step 4), so it
buys the most per round.

Today `"temperature state does not advance under the LARD engine yet"` fires at
open. **The work mirrors X4 exactly**: temperature as a second reserved species
row on the segments, sourced from `node_temp_vol_in` — the D-UT10 accumulator,
**already filled by all seven loaders**. This is a binding, not a new module.

**Read X4's handoff before writing anything.** The two rounds are the same
shape and its traps are yours: the reserved row's index arithmetic, the
dry-element state/report separation, and hotstart persistence.

**Changeset shape**
```
mod: SegmentStore              (second reserved row at np+1)
mod: LagrangianSolver          (advect it; do NOT let it participate in dispersion without a decision)
mod: the LARD bypass warning   (delete it — leaving a warning for a shipped capability is its own defect)
mod: tests/unit/engine/…       (+3 gates)
```

**⚠ Decide and record:** does temperature take part in **RWPT dispersion**
under LARD? Water age does. Heat physically does, but the dispersion
coefficient is not the same one. **If you defer it, the warning must say so
specifically** — "heat is transported but not dispersed" — rather than being
deleted wholesale.

**Gates.** (a) A LARD deck with `__TEMPERATURE__` advances temperature and
matches the LEGACY control within the band X4 established for age. (b) Dry
segments keep their state while the report masks it (the A2b separation).
(c) Hotstart round-trips the temperature row bit-identically.

**Falsifier sweep**

| falsifier | expected |
|---|---|
| i. leave the bypass warning in | a gate must fail — **if none does, no gate observes the user-visible surface** |
| ii. mask the STATE instead of the report | gate (b) fails. This is X4.vii's exact razor |
| iii. drop the row from the hotstart writer | gate (c) fails; a silent restart-temperature reset is the defect |
| iv. a deck with no temperature species | byte-identical `.out`; corpus 20/20 |

**Done when:** all three gates pass, the corpus is inert, and the bypass
warning is either gone or narrowed to the deferred part.

---

## Step 3 — `openswmm_heat.h`: the C API surface · 1 round

**Why here:** step 4 cannot start without it. This is **the Y0 trap repeating**
— the GUI plan's prereq 5 assumed a header that does not exist.

**Do not design this from the GUI's wish list.** Read `openswmm_reactions.h`
and `openswmm_waterage.h` and mirror their shape: count/get discovery, CRUD
with eager validation and rollback (D-RC5), and a `validate` entry point if
the editor will need live feedback.

**Changeset shape**
```
new: include/openswmm/engine/openswmm_heat.h
new: src/engine/core/openswmm_heat_impl.cpp
mod: the export list / CMake install rules
new: tests/unit/engine/test_heat_api.cpp
```

**⚠ Apply P1.3's finding directly:** every numeric parse in the C API must use
H1's **strict wrappers**. The audit found `std::stod` partial-parses silently
accepting `"1.5abc"` and an owner index of `"0.5"` writing the wrong slot and
returning `SWMM_OK`. **The MCP server passes arbitrary LLM-authored text into
these dispatches** — a lenient parse here is a live corruption path, not a
theoretical one.

**Gates.** Exhaustive-key malformed-value coverage in
`test_options_malformed_values.cpp`'s pattern, plus a discovery/CRUD
round-trip.

**Falsifier sweep**

| falsifier | expected |
|---|---|
| i. restore a raw `std::sto*` at any new site | the malformed-value gate fails on the partial-parse rows |
| ii. a CRUD call that leaves the config uncompilable | must be REFUSED and rolled back, not stored (D-RC5) |
| iii. call every getter on an engine with no heat configured | zero counts and `SWMM_OK`, never a crash — this is the MCP's first call on most models |

**Done when:** the header installs, the gates pass, and a `swmm_heat_*` call on
a heat-free model is well-defined.

---

## Step 4 — G4g: heat configuration editor (GUI) · 1–2 rounds

**Repo:** `openswmm.gui`. **Depends on:** steps 2 and 3.

The last unstarted editor. `[HEAT_METEOROLOGY]` / `[HEAT_SOURCES]` forcing
tables plus the Climatology RH/shortwave sub-sections.

**Precedent to follow:** `WaterAgeSourcesDialog` — dependency-light, driven
directly by tests, engine handle **is** the model (no shadow copy). That
deviation was upheld at Y3's validation; do not reintroduce a table model.

**⚠ Reachability is part of the round, not a follow-up.** Y3 shipped an editor
with no menu entry and it sat unreachable until `bc4e07c`. **Action +
menu + comprehensive-editor entry + options-page button land in the same
commit as the dialog**, and a gate asserts the action exists.

**Gates.** Forcing table round-trips both sections; element-range pickers
refuse `start > end`; the action is wired.

**Falsifier sweep**

| falsifier | expected |
|---|---|
| i. remove the action from the .ui | the reachability gate fails — **this is the Y3 defect, gated so it cannot recur** |
| ii. reverse a range picker's ordering check | the ordering gate fails |
| iii. open the dialog against a model with no heat | populates empty, saves nothing, no crash (step 3's falsifier iii from the GUI side) |

**Done when:** a heat configuration authored in the UI round-trips through
save/reopen **and** the editor is reachable from a menu without reading source.

---

## Step 5 — L3: MSX reactions on LARD segments · 2–3 rounds

**The last unstarted quality step.** `"the LARD reaction binding is not
implemented (deferred L3)"` fires today; only first-order KDECAY reacts under
LARD.

**This is a binding, not a new module.** The shared `ReactionSystem` (R1–R4)
exists. The work is D-L1's stated **gather/scatter**: collect each segment's
species column into a stack block, integrate, scatter back.

**Split it.** Three rounds, each observed:
- **5a** gather/scatter plumbing + the block layout, with a null integrator —
  proves the data movement alone is lossless.
- **5b** bind the real integrators; single-species first-order **must match the
  existing KDECAY path bit-for-bit**, which is the strongest available check.
- **5c** multi-species with a translated MSX example (`nh2cl`).

**5b's bit-match is the load-bearing gate of the whole step** — it pins the
new path against a known-good one on identical inputs.

**Falsifier sweep (5b)**

| falsifier | expected |
|---|---|
| i. perturb the scatter index by one | the bit-match fails. **If it passes, the gather/scatter is not observed and 5a's gate was vacuous** |
| ii. integrate with the wrong dt | bit-match fails on any decaying deck |
| iii. a deck with no reactions | byte-identical; corpus 20/20 |
| iv. leave the bypass warning in | as step 2 falsifier i |

**Done when:** 5c round-trips a translated MSX example under LARD, 5b's
bit-match holds, and the bypass warning is gone.

---

## Step 6 — P2.3: treatment interop under LARD · 1 round

`"[TREATMENT] expressions … no removal is applied"` fires today.

**Template: E5b, the ARD precedent.** Run the legacy evaluator on published
node concentrations, then absorb the treated values back into the node stores.

**⚠ Carry the known defect forward rather than inheriting it silently.**
Treatment `mass_lost` is **step-dependent in both engines** — a pre-existing
legacy defect. Do not fix it here; **do not let this round's gate encode the
wrong value as correct**. State the dependence in the gate's comment.

**Gates.** A treated node under LARD removes the same mass the ARD path
removes on the same deck, within the band E5b established.

**Falsifier sweep**

| falsifier | expected |
|---|---|
| i. absorb without publishing first | gate fails: treatment reads stale concentrations |
| ii. a deck with no `[TREATMENT]` | byte-identical |
| iii. run the evaluator twice per step | gate fails — pins that absorption is once per step |

---

## Step 7 — G5g: per-species units from the `.out` · 1 round

**Repo:** `openswmm.gui`. The last owed piece of the results surface: Y2b-3
landed warn-on-miss and the `.oswp` token gate, leaving per-species units.

Species carry units in the `.out`; the plot and tabular surfaces currently
label them generically. **Reserved species are the trap**: `__WATER_AGE__` is
seconds internally and **hours reported**, `__TEMPERATURE__` is °C. A units
lookup that reads the raw column will label age in seconds and be wrong by
3600 without failing anything.

**Gates.** A `.out` with three pollutants + age + temperature labels all five
correctly, age in **hours**.

**Falsifier sweep**

| falsifier | expected |
|---|---|
| i. read age's internal unit | the age row reads "s" — the gate fails, which is the point |
| ii. a legacy `.out` with no quality | no species submenu, no crash |

---

## Step 8 — Verification breadth: the decks that do not exist · 2–3 rounds

Independent of each other; take them in any order.

- **A 2D corpus deck.** 20 decks, **zero 2D** — so "corpus green" says nothing
  about the surface solver, and **D-N1 (Phase 4) is explicitly conditional on
  this landing first**. Also wire
  `tests/scripts/trackI_bitwise_regression.sh` (32 decks) into
  `run_corpus.sh` so its precondition actually bites.
- **A snow parity deck.** Seven defects (F1–F7) were fixed in a module no
  corpus deck exercises. One deck with a nonzero `SD100` and a real `ADC`
  curve turns "byte-identical because nothing ran" into a result.
- **An RWPT corpus deck.** Deferred until the Elder band was pinned; it now is
  (0.96–1.44 across five seeds), so a deck is buildable.
- **`tests/parity/snow/baseline/` is untracked** — the one mechanism that
  detects cross-round drift lives in a single working tree. Either regenerate
  from `gen_snow_parity.py` plus a recorded build, or drop the claim.

**Each deck must justify itself.** A deck reaching nothing new makes every run
slower and proves nothing.

---

## Step 9 — The debts that outlive the program

- **`plans/` and `workplans/` are gitignored in both repos.** The entire plan
  corpus of this program exists in **one working tree on one machine**.
  Reaffirm the ignore and accept that, or reverse it. **Decision owed twice.**
- **Rounds must report into the roadmap.** Five stale rows in the 2026-08-29
  pass and five on 2026-08-25, all overstating remaining work, all because a
  validated round wrote its result into the handoff that requested it. The
  rows were corrected; **the mechanism was not.**
- **Build hygiene.** Builds 40 commits apart both report `6.0.0-alpha.3`, and
  a generated `version.h` dated 2026-06-01 sits on the `build/` include path.
  **A version line is not evidence about what code ran.**
- **`end()` after `report()`** leaves the `.out` unfinalised and returns
  `SWMM_ERR_WRONG_STATE`, with nothing forcing the caller to notice.
- **IO3 per-component `saveData()`** — the actual cure for the embedded-section
  loss that steps in two repos have so far only *mitigated*.
- **Dry elements report a carried temperature indefinitely** — needs a
  per-column no-data sentinel in the `.out`; 0 °C is a real temperature.

---

## Explicitly NOT in the finalization

- **P1.5** negative DWF/GW/RDII — the plan itself suggests closing it as
  *won't do* rather than carrying it. **Close it, don't leave it listed.**
- **P2.4** storage mixing beyond CMSTR — only for models where storage
  residence structure matters.
- **P2.5** full A6 Python + MCP age surfaces — only if the MCP workflow
  matters. Carries lesson 46/47 traps (silent SkipTest, macOS codesign).
- **Phase 2 (HydroCouple)** — no ABI exists; the plugin system's four
  enumerators and single `dlsym` symbol are a *different* extension mechanism.
  Not a finalization item; a project.
- **Phase 3 (2D transport)** and **Phase 4 code** — separate programs, except
  for step 8's 2D deck which Phase 4 depends on.
