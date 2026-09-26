# GeoPackage Species Variables — Validation & Commit Handoff (2026-08-17)

**For:** the checking agent.
**Base:** `d7ce8efb` (or current HEAD; the change touches two GeoPackage
files and one test file, none of them shared with the concurrent session).
**Not a plan phase** — A2b carry (b), the last unclosed reporting gap in the
shipped quality/age work.
**Standing findings:** lessons 1–48.

---

## 1. The defect

`GeoPackageOutputPlugin::update()` already reads `subcatch_quality`,
`node_quality` and `link_quality` and looks each species up per object type.
Every one of those lookups returned **−1**, because `lookup_variable` reads a
cache built from the `variables` table and **nothing ever inserted a species
row**. `populate_default_variables()` (`GeoPackageSchema.cpp:1330`) is a
static list of ~45 hydraulic variables; species names are model-dependent and
cannot live there.

Consequence: **the GeoPackage output carries no quality data at all** —
pollutants included, not only water age. A user opening a `.gpkg` sees a
complete hydraulic record, no quality, and no error anywhere. This is the
third instance of the lesson-26 shape (a reporting carrier that is dead while
the engine state behind it is correct), after `957a1d62`'s all-zero `.out`
columns.

My A2b handoff §5 predicted the opposite — that age would appear here "for
free". The validator corrected that; this closes it.

## 2. Changeset (uncommitted)

```
mod:  src/engine/input/geopackage/GeoPackageOutputPlugin.hpp
      (register_species_variables declaration + why)
mod:  src/engine/input/geopackage/GeoPackageOutputPlugin.cpp
      (species_units_label helper; register_species_variables();
       one call site in prepare(), after populate_default_variables and
       BEFORE the variable-ID cache is built)
mod:  tests/unit/engine/test_geopackage.cpp   (+4 gates, 2 helpers)
```

Both TUs pass `g++ -std=c++20 -fsyntax-only`. No `.inp` surface, no format
change, no C/Python API change.

## 3. Design decisions to review

### 3.1 A name collision FAILS the open — the call most worth challenging

`variables` is `UNIQUE(name, object_type)`. A pollutant named `depth` would
therefore collide with the built-in NODE/LINK variable. An `INSERT OR IGNORE`
would leave the hydraulic row in place and `lookup_variable("depth","NODE")`
would resolve to **its** id — writing concentrations into the depth series.

I judged **silently corrupting an existing result strictly worse than
refusing to open**, and the condition is entirely under the user's control
(rename the pollutant), so `prepare()` returns −1 with a message naming the
species. Gate 4 asserts both the failure and that the hydraulic row survived.

**If you would rather degrade than refuse**, the change is one `continue`
instead of `return -1` — but then that species is silently absent, which is
the defect this changeset exists to remove, so I would want the skip to be
loud. The plugin has **no warning channel**: `prepare()` takes
`const SimulationContext&`, so `ctx.warnings` is unreachable, and there is no
logger in this translation unit. That constraint is why the choice is binary.
Worth recording either way.

### 3.2 All three object types, including SUBCATCH

`update()` has a species loop for SUBCATCH, NODE and LINK, so all three are
registered. Subcatchment age reports 0 until plan phase A3 — that is a VALUE
question, not a registration one, and the `.out` already carries the same
zero column, so registering keeps the two outputs consistent. Flag if you'd
rather suppress the SUBCATCH age row until A3 fills it.

### 3.3 Units are stated, not encoded

The `.out` cannot say HOURS (three-value concentration enum, no slot — which
is why the NAME is the discriminator there and why `06580dd6`/`d7ce8efb`
exist). The GeoPackage `variables.units` column is free text, so the age row
simply says `hours` and pollutants say `mg/L` / `ug/L` / `#/L`. Category is
`QUALITY`, which is also what distinguishes a re-run into the same file from
a collision.

### 3.4 Assumption I verified, worth re-checking

`ctx.pollutants.units` is indexed for `s < ctx.n_pollutants()`. Those are
sized together in `SimulationContext::resize()`
(`pollutants.resize_pollutants(pollutant_names.size())`, line 1534), so the
index is in range. I did not add a defensive bound (§2); if you disagree it
is one clamp.

## 4. Validation protocol

1. Build, zero new warnings. `ctest -R test_engine_geopackage`.
2. *Anticipated failure modes, likelihood order:*
   (a) **`build_test_context()` may not satisfy `prepare()`** — it is used
   for writer tests, and `prepare()` also calls `write_model()`. If prepare
   returns −1 for an unrelated reason, print `last_error_message()` first;
   that is a fixture problem, not a binding one.
   (b) **The collision gate's setup is deliberately unfaithful in one
   respect**: it sets `reported_species_names = {"depth"}` while
   `pollutant_names` still holds `TSS` from the fixture. The registration
   path reads `reported_species_names`, so the real code path is exercised,
   but the two lists would mirror each other in a real run. Tighten if you
   want it exact.
   (c) **A failed prepare leaves a partially written `.gpkg`** (model + a
   `status = 'running'` simulations row). Pre-existing for any prepare
   failure, but §3.1 adds a new way to reach it. Record whether that matters.
3. **Falsifier sweep:**

   | falsifier | expected failing gates |
   |---|---|
   | i. remove the `register_species_variables()` call from `prepare()` | **`SpeciesAreRegisteredAsVariables`** (0 QUALITY rows) and **`SpeciesRowsReachResultTimeseries`** (no rows) — ii is the defect gate and must fail loudly |
   | ii. move the call AFTER the variable-ID cache is built | `SpeciesRowsReachResultTimeseries` ONLY — registration succeeds, the table is right, and every lookup still returns −1. **This is the subtle one**: the table would look correct to anyone inspecting the file, and only the timeseries gate can see it |
   | iii. register NODE only (drop LINK/SUBCATCH) | `SpeciesAreRegisteredAsVariables` (count 6 → 2, and the per-type legs name which type) |
   | iv. write the age row's units as `mg/L` | `SpeciesAreRegisteredAsVariables` units leg |
   | v. drop the `ignore_quality` guard | `IgnoreQualityRegistersNoSpeciesVariables` |
   | vi. replace the collision check with `INSERT OR IGNORE` | `SpeciesCollidingWithHydraulicVariableFailsPrepare` — and note WHICH leg fails: if only the return-code leg fails, the hydraulic row survived anyway and the corruption risk was theoretical; if the category leg ALSO fails, the corruption is real and the fail-loud choice is vindicated. **Please record this distinction** — it is the evidence for or against §3.1 |
4. **Prior suites:** no engine or writer code changed, so the C++ suite
   should be unchanged apart from +4 gates, and 14/14 deck `.out` bit-identity
   must hold (the `.out` writer is untouched). Decks without `[POLLUTANTS]`
   register nothing (`nr <= 0` returns early), so GeoPackage output for
   hydraulics-only models is byte-unchanged.
5. **Record:** falsifier vi's leg breakdown, and whether any checked-in
   `.gpkg` fixture now gains species rows it did not have (that would be a
   visible, correct change, but it should be seen rather than discovered).

## 5. Commit message

```
fix(output): register species as GeoPackage variables

GeoPackageOutputPlugin::update() has always read node/link/subcatch quality
and looked each species up per object type, and every one of those lookups
returned -1: lookup_variable reads a cache built from the variables table,
and nothing ever inserted a species row. populate_default_variables carries a
fixed list of hydraulic variables, and species names are model-dependent so
they cannot live there.

The result was that a .gpkg carried NO quality data at all - pollutants
included, not just water age - with a complete hydraulic record beside it and
no error anywhere. Same shape as 957a1d62's all-zero .out columns: a dead
reporting carrier over correct engine state.

prepare() now registers the run's reported species (pollutants, then
__WATER_AGE__) for NODE, LINK and SUBCATCH before the variable-ID cache is
built, with category QUALITY and real units - the GeoPackage units column is
free text, so the age row says hours, which the binary .out cannot express.
IGNORE_QUALITY registers nothing, mirroring DefaultOutputPlugin.

A species whose name collides with a built-in variable FAILS the open rather
than being ignored: variables is UNIQUE(name, object_type), so an ignored
insert would leave lookup_variable resolving to the hydraulic row and write
concentrations into, say, the node depth series. Corrupting an existing
result silently is worse than refusing, and renaming the pollutant is in the
user's hands.

Gates: tests/unit/engine/test_geopackage.cpp +4 - registration per object
type with the age row's units read back, species values recovered from
result_timeseries through the name join a consumer would use, the
IGNORE_QUALITY bypass, and the collision trap (which also asserts the
built-in variable survived).
```

## 6. Validation results

**Verdict: accepted. All six falsifiers caught, and §3.1's fail-loud choice
is vindicated by direct measurement — the corruption is real, not
theoretical.** Commit `f37f7dde`. Artifacts:
`tests/output/gpkg_species_validation_2026-08-17/`.

| check | result |
|---|---|
| build | clean, **zero** warnings in the three files |
| `test_engine_geopackage` | **59/59** (55 prior + 4 new) |
| full `ctest` | **142/143** — the one failure is the known FV refinement gate, whose fix is uncommitted in the shared tree and so absent from this worktree |
| 14-deck `.out` bit-identity | **14/14** — the `.out` writer is untouched |
| ASan + UBSan over the suite | **0 findings**, 59/59 |
| falsifiers | **6 of 6 caught** |

### 6.1 The ordering assumption the gates cannot check — verified separately

The gates build a `SimulationContext` by hand and assign
`reported_species_names` directly, so **no gate could see it if the real
engine populated that vector after `prepare()` ran** — the species list would
be empty, `nr <= 0` would return early, and nothing would register while
every gate stayed green. Checked in the engine rather than assumed:
`reported_species_names` is built in `SWMMEngine::open()` (line 327), plugin
`prepare()` is called from `start()` ("Phase 4", line 1051). Order is
open → initialize → start, so the list is populated well before registration.
Sound, but it is the kind of thing that should be stated rather than inferred
from a passing fixture.

### 6.2 Falsifier sweep

| falsifier | outcome |
|---|---|
| i. never register | **caught** — 0 QUALITY rows; both the registration and timeseries gates fail |
| ii. register AFTER the ID cache | **caught by the timeseries gate ONLY**, exactly as predicted — 101 variables in the table and every species row still dropped |
| iii. NODE only | **caught** — count 6 → 2, and the per-type legs name LINK |
| iv. age units as `mg/L` | **caught** — the units leg |
| v. drop the IGNORE_QUALITY guard | **caught** — 6 rows where 0 were expected |
| vi. tolerate the collision | **caught** — see below |

ii deserves its billing: the `variables` table looks entirely correct to
anyone inspecting the file, and only a gate that reads back through
`result_timeseries` can see that every lookup still returned −1. That is the
same shape as `957a1d62` and this changeset's own defect — a carrier that
looks right and carries nothing.

### 6.3 §4.5's question: the collision corruption is REAL

You asked which legs fail under falsifier vi, because "if only the
return-code leg fails, the hydraulic row survived and the corruption risk was
theoretical."

**Only the return-code and error-message legs failed.** The category leg
passed: the built-in `depth` row survived as `STATE`, untouched. By the
criterion as written, that reads as *theoretical*.

**It is not.** The gate cannot see the corruption because `prepare()` fails,
so `update()` never runs — the collision's consequence is unreachable from
it. I measured it directly instead: with the tolerate build, prepared a
context whose only species is `depth`, pushed a snapshot carrying the
unmistakable value **99999.0**, and read back through the consumer join:

```
PROBE prepare rc=0
PROBE depth-series row: category=STATE value=99999.0
```

A species concentration in the row whose variable category is `STATE` — i.e.
in the node **depth** series a consumer reads. Refusing to open is the right
call, and the surviving hydraulic row is not evidence against it; it is what
makes the corruption silent.

I recorded that number in a comment on the collision gate. It cannot become
an assertion there (the failure it depends on prevents `update()`), and
without it the next person weighing "why not just skip the species?" sees a
falsifier whose category leg passed and reasonably concludes the risk was
overstated.

### 6.4 Everything else

- **§4.5, checked-in fixtures:** all 26 tracked `.gpkg` files are unchanged
  by the suite run, and none carries a QUALITY row — they are schema/reader
  fixtures, not products of the output plugin. Nothing gains species rows.
- **§3.4:** confirmed, `ctx.pollutants.units` is sized with
  `pollutant_names` in `resize()`; the index is in range and no clamp is
  needed.
- **§4.2(a) and (c) did not arise:** `build_test_context()` satisfies
  `prepare()`, and the partial-`.gpkg`-on-failure path is pre-existing for
  any prepare failure. Worth noting that §3.1 does add a new way to reach it
  — a collision leaves a file with the model written and a `status =
  'running'` simulations row — but that is the existing behaviour of every
  other prepare failure, not something this changeset should fix alone.
- **`sqlite3_step(ins)`'s return is unchecked.** The probe SELECT has just
  established no row exists, so a failure is not reachable by construction,
  and the surrounding code in `prepare()` ignores step results the same way.
  Consistent with house style; recorded rather than changed.
