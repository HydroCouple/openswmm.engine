# Step 3 — the heat C API is mostly BUILT; this closes the one gap — Handoff (2026-08-30)

**For:** the implementing/checking agent.
**Base:** `deb42172` (H7 complete; corpus 21; ctest 180/181).
**Standing findings:** lessons 1–191.
**Blocked behind:** `H6A_VALIDATION_HANDOFF_2026-08-30.md` — see §3. **Do not
start until H6a validates.**

---

## 1. The sequence was wrong: step 3 is not a from-scratch round

`FINALIZATION_SEQUENCE_2026-08-29.md` step 3 says *"`openswmm_heat.h` does not
exist"* and asks for the C API to be designed and written.

**It exists.** H6a created `include/openswmm/engine/openswmm_heat.h` and
`src/engine/core/openswmm_heat_impl.cpp` — **17 exported functions**, currently
**untracked and unvalidated**. Writing step 3 as specified would have produced
a second, conflicting heat API.

That is the fifth time this program has nearly redone landed work, and the
only reason it did not happen here is that the user said "another agent has
done some work on the heat." **The warning did the job the sequence should
have** — the sequence is a document, and documents go stale (lesson 167).

## 2. What H6a covers, and the one thing it does not

The authoritative section list is `HeatComponent.cpp:522-527`:
`[HEAT_SOURCES]`, `[HEAT_FLUXES]`, `[RADIATIVE_FLUXES]`, `[SOLAR_RADIATION]`,
`[CLOUD_COVER]`.

| section | API today |
|---|---|
| `[HEAT_FLUXES]` | ✅ `swmm_heat_get/set_module` |
| `[RADIATIVE_FLUXES]` | ✅ `swmm_heat_get/set_radiative`, shortwave mode + timeseries |
| `[SOLAR_RADIATION]` | ✅ `swmm_heat_get/set_solar`, `_get_solar_sited` |
| `[CLOUD_COVER]` | ✅ `swmm_heat_get/set_cloud`, `_set_cloud_timeseries`, `_clear_cloud` |
| **`[HEAT_SOURCES]`** | ❌ **nothing** |

**`[HEAT_SOURCES]` is the gap, and it is the one G4g most needs** — it is the
per-source inlet temperature table (`HeatComponent.hpp:29-33`: GLOBAL rows for
all seven sources + NODE overrides for DWF/EXTERNAL_INFLOW, **VALUE °C only**;
TIMESERIES temperatures and SUBCATCH/EDGE_BC scopes refuse with deferral
errors). An editor cannot round-trip a table the API cannot read.

### ⚠ `[HEAT_METEOROLOGY]` does not exist. Correct the GUI plan.

`TRANSPORT_QUALITY_GUI_PLAN_2026-08-12.md` §3.5 specifies G4g as *"forcing
table round-trips `[HEAT_METEOROLOGY]`/`[HEAT_SOURCES]`"*. **There is no
`[HEAT_METEOROLOGY]` section** — not in the recognized list, not in the
parser, nowhere in the tree.

RH and wind live in the **existing `[TEMPERATURE]` climate section**
(`HydrologyHandler.cpp:232` `WINDSPEED`, humidity at `:245`) and already have
UI. So G4g is **`[HEAT_SOURCES]` plus the Climatology sub-sections the plan
also names** — the plan is half right, and the half that is wrong would have
sent someone building a parser for a section that never existed.

## 3. ⚠ Sequencing: H6a is unvalidated, and that is load-bearing

H6a's own handoff says: *"nothing has been linked, and no gate has ever
executed"* — no `cmake` in its environment. So today's heat API is
**17 functions and an implementation that has never run.**

**Step 3 must land after H6a validates**, for a reason beyond tidiness: if
this round adds `[HEAT_SOURCES]` functions alongside 17 unvalidated ones and
something fails, attribution is impossible. **H6a's validation is what makes
this round's failures mine.**

If H6a's check turns up API-shape changes, **this round rebases onto them
rather than the reverse** — H6a designed the surface, and a late addition
should not redefine conventions the earlier work set.

## 4. Conventions to MATCH, not re-invent

H6a made three choices worth adopting verbatim; a `[HEAT_SOURCES]` API that
departs from them makes the header incoherent to read:

1. **Typed enums for parameter codes, `int` in the signature** —
   `SWMM_HeatSolarParam`, `SWMM_HeatCloudParam` exist, but functions take
   `int param`. Deliberate: the enum documents, the `int` keeps the ABI stable.
2. **Values are REFUSED, not clamped** — *"the parser's rule, so the API and
   the deck agree."* A refused write does not take effect. **This is the
   single most important convention to preserve**: it is what stops the API
   and the deck disagreeing about what a model contains.
3. **Mode-dependent refusal** — writing `SHORTWAVE` while the mode is not
   `CONSTANT` is refused, because *"storing one there would look configured
   while changing nothing."* The `[HEAT_SOURCES]` analogue: writing a NODE
   override for a source whose scope does not admit one, or a TIMESERIES
   temperature that H1 defers, must **refuse with the deferral error the
   parser already emits** — not silently accept.

## 5. What this round delivers

```
mod: include/openswmm/engine/openswmm_heat.h        (+[HEAT_SOURCES] surface)
mod: src/engine/core/openswmm_heat_impl.cpp
mod: tests/unit/engine/test_heat_api.cpp            (or H6a's suite)
```

Discovery + CRUD over the source table, mirroring `openswmm_reactions.h`'s
count/get/add shape (the established pattern for a table the GUI edits):
per-source GLOBAL temperature read/write, NODE override enumeration and
read/write, and whatever the editor needs to *distinguish* an unset source
from one set to a default.

**⚠ Apply P1.3's strict parse wrappers to every numeric entry point.** That
audit found `std::stod` partial-parses accepting `"1.5abc"` and an owner index
of `"0.5"` writing the wrong slot while returning `SWMM_OK`. **The MCP server
passes arbitrary LLM-authored text into these dispatches** — a lenient parse
here is a live corruption path.

## 6. Gates and falsifiers

**Gates.** (a) Round-trip: set a GLOBAL source temperature and a NODE
override, `swmm_model_write`, reopen, read the same values. (b) Exhaustive
malformed-value coverage in `test_options_malformed_values.cpp`'s pattern.
(c) Every getter on a model with **no heat configured** returns zero counts and
`SWMM_OK` — never a crash. That is the MCP's first call on most models.

**Falsifiers**

| falsifier | expected |
|---|---|
| i. restore a raw `std::sto*` at any new site | the malformed-value gate fails on the partial-parse rows |
| ii. accept a TIMESERIES source temperature instead of refusing | a gate must fail — **if none does, the deferral contract is unobserved and H1's refusal is decorative** |
| iii. clamp instead of refuse an out-of-range value | the round-trip gate fails: the deck and the API now disagree, which is convention 2's whole point |
| iv. a model with no `[HEAT_SOURCES]` | counts 0, `SWMM_OK`, no crash |
| v. write a NODE override for a source scope that does not admit one | refused with the parser's own error text — **not** a different message, or the API and deck diverge in wording while agreeing in behaviour |

**Corpus 21/21** throughout — this is an API-surface round and no corpus deck
calls the C API, so **any movement means the change reached into the engine.**

## 7. After this round

G4g (step 4) unblocks, with its spec corrected per §2: `[HEAT_SOURCES]` table
plus the existing Climatology sub-sections, **not** `[HEAT_METEOROLOGY]`.

**Also owed, from H7b and outside this round:** `[POLLUTANTS]` Kdecay is
applied as **1/second here vs legacy's 1/day** — a factor of 86 400 on every
decaying deck, the same shape as the underdrain and `landuse.c`. It is a
parity defect against the reference and deserves triage before more work is
calibrated on top of it. And an `[INFLOWS]` row naming a pollutant declared
later in the file **silently loads zero**.

---

# IMPLEMENTATION RECORD — 2026-08-30

**Implemented. Syntax-checked, not run.** `g++ -fsyntax-only -std=c++20`
against the real include tree: **0 errors** in `openswmm_heat_impl.cpp`.
Nothing built, linked or executed.

```
mod: include/openswmm/engine/openswmm_heat.h        (+11 functions, +1 enum)
mod: src/engine/core/openswmm_heat_impl.cpp         (+the [HEAT_SOURCES] block)
new: tests/unit/engine/test_heat_sources_api.cpp    (6 gates)
mod: tests/unit/engine/CMakeLists.txt               (+1 registration)
```

**Still blocked behind H6a's validation** (§3). Both files are H6a's; this
round appends to them. If H6a's check changes the surface, **this rebases onto
it.**

## The surface

GLOBAL table: `source_count`, `get/set_source_temp`,
`get_source_configured`, `clear_source_temp`. NODE overrides:
`node_override_count`, `get_node_override`, `set_node_override`,
`remove_node_override`. Plus `get_effective_source_temp`, which **delegates to
`HeatConfigData::source_temp`** rather than re-deriving override-beats-global —
two copies of a precedence rule drift, and the API's copy would drift silently.

`get_source_configured` is the one an editor cannot work without: a source with
no row reads 20 °C, and so does a source explicitly set to 20 °C. Without the
flag the editor writes rows the user never asked for.

## ⚠ THE FINDING: these edits do not survive a save

`swmm_model_write` emits `[PROCESS_COMPONENTS]` with the config= **path**
(`InpWriter.cpp:2536`) and **never rewrites the config file's content** —
there is no per-component `saveData()`; grep finds it only in comments naming
it as IO3's future work. So the saved `.inp` points at the **original**
`model.heat`, and every API edit is gone on reopen.

**This is the embedded-section data-loss family one layer out, and unlike that
case it is NOT warned.** The writer's warning fires only for *embedded*
sections, and its advice — *"Move them to an external component config file
… to keep them"* — **is false for anything edited through the API or the
GUI.** Hand-edit the file and it persists; edit it through the software and it
does not.

`NodeOverrideEditsDoNotSurviveASave` pins the current behaviour so the loss is
observed rather than discovered by a user. **When IO3 lands, that gate must
FAIL** — its message says so, and says to replace it with a real round-trip
rather than relax it.

**Consequence for step 4: G4g is blocked on component-config serialization,
not on this API.** An editor shipped on top of this today would let a user
edit a table that vanishes on save, silently. That is a bigger finding than the
missing API was, and it should be settled before G4g starts.

## Two deviations, both deliberate

1. **Repeated `set_node_override` UPDATES; the parser REFUSES a duplicate
   row.** One deck cannot mean two temperatures for a pair; an editor
   rewriting a value it just wrote must succeed. Same invariant — one row per
   (source, node) — reached the way each caller means it. **Challenge this if
   you disagree**; it is the only place the API and the deck deliberately
   differ, and the difference is in *how the same end state is reached*, not
   in what states are legal.
2. **The temperature range is a literal COPY** of the parser's file-local
   `kMinTemp`/`kMaxTemp`. Hoisting them to a shared header is a wider change
   than this round earns — so `RefusesWhatTheDeckRefusesAndDoesNotMutate`
   drives the same out-of-range value through **both** doors and requires both
   to refuse. That gate is what stops the copy drifting.

## Validation protocol

1. `ctest` the new suite. **All six gates should pass at base** — this is new
   API, so there is no "fails at base" for the surface itself. The gate that
   carries a claim about the *existing* engine is
   `NodeOverrideEditsDoNotSurviveASave`, and it asserts today's behaviour.
2. Full `ctest -j8` ×3 against the standing figure. Nothing existing should
   move: no production code path calls these functions.
3. **Corpus 21/21** against `tests/output/rebaseline_8f9f164d/corpus/`. An
   API-surface round that moves a deck has reached into the engine.
4. **Falsifier sweep:**

   | falsifier | expected |
   |---|---|
   | i. clamp instead of refuse in `set_source_temp` | the range gate fails on the "did not mutate" leg — **the deck/API agreement is what breaks first**, which is the point |
   | ii. widen the API range to ±200 while the parser stays [-50, 100] | the range gate fails at its FIRST door (the deck refuses 150, the API accepts) — confirms it tests both, not one |
   | iii. drop the `node_scope_ok` guard | the scope gate fails on five sources — confirms H1's deferral is enforced, not decorative |
   | iv. make `clear_source_temp` also drop NODE rows | the clear leg fails on `count == 1` — pins that clearing a GLOBAL does not delete model the caller did not name |
   | v. re-derive precedence in `get_effective_source_temp` instead of calling `source_temp` | passes today — **and that is the risk**: record it as an unobserved coupling, since the gate cannot distinguish delegation from a correct copy |
   | vi. let a refused write mark the source configured | the range gate's `cfgd == 0` leg fails |

5. **Record:** falsifier ii and v, and whether §"THE FINDING" changes anyone's
   view of G4g's readiness.

## Known gaps

- **No `swmm_model_write` round-trip gate**, because none can pass — see the
  finding. Its absence is deliberate and is itself pinned by a gate.
- **TIMESERIES source temperatures are still deferred** at the parser
  (H1 scope). The API has no way to express one, which is consistent — but
  nothing gates that the API *refuses* to invent one, because there is no
  entry point that could.
- **The range constants are duplicated** (deviation 2), pinned by a gate
  rather than by construction.

---

# CHECK RECORD — 2026-08-31 (H6a validation + step 3, one round)

**Verdict: VALIDATED and COMMITTED together with H6a as engine `803d5cbc` (on `9cda9cd3`).** H6a's validation
(its handoff §6, filled this round) is what §3 required before this could
land; both changesets were exercised as one tree because step 3 appends to
H6a's files.

## Method

Isolated detached worktree at `deb42172`, configured like `build/darwin`
(vcpkg_installed reused). Base built and measured FIRST; the heat changeset
(H6a + step 3 + this round's fixes) applied on top; re-validated a second
time after the peer's DW-TPA landing moved HEAD to `72474eb8`. Artifacts:
`tests/output/step3_heat_api/` (`heat_tracked.patch`, corpus A/B,
PROVENANCE).

## What this round changed beyond the two implementations

1. **The H6a fixture bug (§6.1 of the H6a handoff):** every
   `[PROCESS_COMPONENTS]` row in `test_heat_solar_radiation.cpp` lacked
   `config="…"` — 7/18 gates failed on first-ever execution and the
   refusal gates were passing vacuously. All 18 sites fixed → 18/18.
2. **The `frac()` NaN hole** in the pre-existing `[RADIATIVE_FLUXES]`
   ladder — a live deck/API disagreement (deck stored `ALBEDO GLOBAL nan`,
   API refuses NaN), i.e. a violation of exactly the contract §6's gates
   defend. Parse now goes through `parse_finite`; gate 13 gained an
   ALBEDO nan/inf leg. (`parse_celsius` never had the hole — the handoff's
   §5.6 was half wrong; comment corrected in place.)
3. **Bird coefficients verified** against pvlib's NREL-faithful `bird()`
   (H6a §2.2): −0.3034 ozone exponent, Ba = 0.85, air-mass usage — all
   match; gate 5 relabelled a verified reference pin.
4. **Gate 13b added** — the H6a handoff §5 adversarial decks (FRACTION 75,
   LATITUDE 100, TIMESERIES+cloud warn, unused-coordinates warn, cloud
   series 1.7 clamped at runtime).
5. `openswmm_heat.h`'s stale "does NOT cover [HEAT_SOURCES]" preamble
   replaced; CHANGELOG heat entry extended with the step 3 surface.

## Evidence

| check | result |
|---|---|
| base anchor (deb42172, isolated tree) | 180/181 — only `fv_tpa_closure` (the standing figure) |
| patched ctest ×3 (deb42172 + heat) | **182/183 ×3** — only `fv_tpa_closure`; both new suites green (18/18, 6/6) |
| corpus A/B (base wrapper vs patched CLI) | **21/21 byte-identical**, incl. `heat_parity` (H3 baseline) and `heat_lard` |
| H3 gates (`heat_radiative_exchange`) | green, untouched call signatures |
| revalidation on `72474eb8` | see the figure recorded below the falsifier table |

**Falsifier sweep** (edit → rebuild → run → restore; restored impl
`cmp`-equal to the shared tree):

| falsifier | result |
|---|---|
| i. clamp instead of refuse in `set_source_temp` | BITES — range gate fails on refusal + did-not-mutate legs |
| ii. widen API range to ±200, parser stays [−50,100] | BITES at the API door (`:229/:231`) — the gate tests both doors |
| iii. drop `node_scope_ok` | BITES — 6 failures (5 sources accepted + count) |
| iv. `clear_source_temp` drops NODE rows | BITES on the `count == 1` leg |
| v. re-derive precedence in `get_effective_source_temp` | **PASSES** — recorded: the gate cannot distinguish delegation from a correct copy (unobserved coupling, as the implementation record predicted) |
| vi. refused write marks configured | BITES on the `cfgd == 0` leg |

§6's falsifier ii (TIMESERIES source temperature through the API) remains
structurally unfalsifiable: no entry point can express one, which is the
consistent shape — recorded, not gated.

## Standing recommendations, unchanged by this round

- **G4g stays blocked on component-config serialization (IO3), not on this
  API** — the §"THE FINDING" data-loss is real, pinned by
  `NodeOverrideEditsDoNotSurviveASave`, and that gate MUST fail when IO3
  lands.
- Owed elsewhere: Kdecay 1/s-vs-1/day parity; `[INFLOWS]`-before-
  `[POLLUTANTS]` zero-load; `[RADIATIVE_FLUXES]` trailing-junk convention
  (H6a §5.7).
