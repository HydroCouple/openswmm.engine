# A2b Implementation — Validation & Commit Handoff (2026-08-17)

**For:** the checking agent.
**Base:** `957a1d62` (the snapshot-quality fix — A2b's prerequisite).
**Plan:** `WATER_AGE_TRACKING_PLAN.md` §1 (report as hours) / §7 A2.
**Scope note:** this is the REPORTING half only. The age-volume
mass-balance ledger row is deliberately NOT here — see §2.4.
**Standing findings:** lessons 1–39. This changeset is the lesson-14/15
stride family by construction (a new column in a strided block), so read
§2.1 first.

---

## 1. Changeset (uncommitted)

```
mod:  src/engine/core/SimulationContext.hpp
      (reported_species_names + n_reported_species() — the single
       naming/count truth for the REPORTED species block, built once at
       open so the snapshot's pollut_names pointer stays valid)
mod:  src/engine/core/SWMMEngine.cpp
      (build reported_species_names = pollutants then __WATER_AGE__;
       snap.pollut_count / pollut_names now the REPORTED list; the three
       quality vectors stride by nr and carry the age column in HOURS)
mod:  src/engine/plugins/DefaultOutputPlugin.cpp
      (n_polluts_ = n_reported_species(); IDs from the reported list;
       the age column's unit code — see §2.3)
mod:  tests/unit/engine/test_output_quality.cpp   (+1 gate; deck helper
       gains a [PROCESS_COMPONENTS] argument + write_file)
```

All TUs pass `g++ -std=c++20 -fsyntax-only`.

## 2. Design decisions to review

### 2.1 TWO strides, named apart on purpose

- **Transport stride `np`** — `nodes.conc`, `links.conc`, `qual_mass_in`,
  everything the engines touch. Unchanged.
- **Reported stride `nr` = np + (water_age ? 1 : 0)** — the snapshot's
  three quality vectors, `snap.pollut_count`, and the writer's
  `n_polluts_`.

Conflating them is exactly the E4/R6 and A1a defect shape, so the count
has its own named accessor rather than an inline `+1`. The gate's razor:
with age ON, the TSS column must still read 42 at its original index.

### 2.2 Age takes no old/new interpolation

Pollutant columns interpolate `f1*old + f*new` (legacy parity, and
validation proved the weights are live — lesson 39). Age has no
`node_age_old` to weight against: `water_age_state` holds the published
step value, so the age column reports it directly. Flag if you'd rather
an age-old snapshot be added for symmetry — it would be a real state
addition, not a free change.

### 2.3 The age column's unit code is MG_PER_L's (0), deliberately

`.out` stores a 3-value `MassUnits` enum per species column. Age is
HOURS, which has no slot. Widening the enum would break every existing
reader (legacy included), so the age column writes code 0 and **readers
must key on the NAME `__WATER_AGE__`**, which the header already carries.
Recorded as a format decision. If you'd rather add a 4th code (HOURS = 3)
and accept that older readers mislabel it, say so — it is a one-line
change plus a reader-compat note.

### 2.4 The age-volume ledger row is NOT in this changeset

The `.rpt` continuity table is pollutant-shaped (mass in/out/stored per
pollutant). Age is not a mass and does not conserve — it grows at 1 s/s
in every parcel — so an "age-volume balance" row is a different object
from the pollutant rows and needs its own definition before it is
implemented. Splitting it out keeps this shot to a surface that already
has a working pipeline. Recorded as A2c on the roadmap.

## 3. Validation protocol

1. Reconfigure if needed, build, zero new warnings.
2. `ctest -R test_engine_output_quality` — 3 gates (2 existing + the new
   `WaterAgeReportsAsATrailingColumnInHours`).
   *Anticipated failure modes, likelihood order:*
   (a) **The age column index** — the gate reads node `v[7]` (6 fixed +
   TSS + age). If the header's species order differs, fix the index.
   (b) **The 2 h band** — deck is a level pool with INITIAL_STATE 2 h and
   END_TIME 10 min, so the last report should be ≈ 2.167 h. If it reads
   ≈ 0.167, the INITIAL_STATE seeding did not reach the report (check the
   ARD init path); if ≈ 7810, a seconds/hours slip.
   (c) **`reported_species_names` build order** — it is built in `open()`
   BEFORE the components resolve. Pollutant names are known by then; the
   age entry depends only on `options.water_age`, also parsed by then.
   Verify no path clears it afterwards (`ctx.reset()` runs earlier in
   open, which is why the build sits where it does).
3. **Falsifier sweep:**

   | falsifier | expected failing gates |
   |---|---|
   | i. writer strides by `n_pollutants()` again | new gate (pollut_count == 1) |
   | ii. drop the age column fill in the snapshot (node loop) | new gate (age reads 0) |
   | iii. write the age in SECONDS (drop /3600) | new gate (upper bound) |
   | iv. write the age column FIRST instead of last | new gate (TSS leg reads the age, age leg reads 42) — this is the stride razor |
   | v. leave `snap.pollut_names` pointing at `pollutant_names.names()` | header ID for column 2 is empty/garbage — record what the reader returns |
   | vi. build `reported_species_names` without the age entry while `n_polluts_` still counts it | ID read out of range — record (this is the coupling that makes the single-truth vector worth having) |
4. **Prior suites:** expect the WATER_AGE-off world to be **unchanged** —
   `nr == np` then, so every stride and the header are identical. Verify
   14/14 deck `.out` bit-identity against `957a1d62` (NOT against the
   pre-fix base). The water-age suite (16 gates) must stay green; it reads
   `water_age_state` directly and is unaffected by reporting.
5. **GeoPackage consumer check:** `GeoPackageOutputPlugin` indexes
   `snapshot.node_quality` with `snapshot.pollut_names` — both now include
   age, so it should emit an age row per element for free. Worth a look:
   if it writes into a pollutant-typed table, the age row may need a type
   or a filter. Record either way.
6. Append results to §5; commit with §4.

## 4. Commit message

```
feat(transport): report water age as a trailing .out species column (A2b)

With [OPTIONS] WATER_AGE on, the binary .out gains a __WATER_AGE__
pseudo-pollutant column carrying the published node/link age in HOURS
(internal state is seconds). SimulationContext::reported_species_names is
the single naming/count truth: pollutants, then age. The snapshot's
pollut_count/pollut_names and the writer's n_polluts_ both take that
count, and the three quality vectors stride by it, so the TRANSPORT
stride (np, for nodes.conc et al.) and the REPORTED stride (nr) stay
named apart - conflating them is the stride-slip family that produced two
earlier defects.

Age takes no old/new interpolation (there is no age-old state to weight
against; pollutants keep legacy's f1*old + f*new). The age column writes
MassUnits code 0 because the .out unit field has no HOURS slot: readers
key on the NAME, which the header carries. Subcatchment age stays 0 until
plan phase A3 rather than reporting a value it does not track. With
WATER_AGE off, nr == np and every byte of the output is unchanged.

The age-volume ledger row is NOT included: the .rpt continuity table is
mass-shaped and age neither is a mass nor conserves, so that row needs
its own definition (roadmap A2c).

Gates: tests/unit/engine/test_output_quality.cpp +1 (analytic 2 h
INITIAL_STATE on a level pool - the units razor at 3600x, and the TSS
column must still read 42 at its original index).

Plan: WATER_AGE_TRACKING_PLAN.md section 1/7 A2 (reporting half).
Validation record: plans/transport/A2B_VALIDATION_HANDOFF_2026-08-17.md
```

## 5. Validation results

**Verdict: accepted, with one gate strengthened and three findings recorded.**
Commit `d4889329`. Artifacts: `tests/output/a2b_validation_2026-08-17/`.

### 5.1 How this was validated

The working tree again held **two independent changesets**: A2b's four files
and an in-progress multi-column rain-series feature (13 modified files +
`MultiColumnSeriesFile.{cpp,hpp}` and four new fixtures, another agent's
work). Everything below was therefore measured in a worktree detached at
`957a1d62` carrying **only** the four A2b files — so no suite result, byte
comparison or sanitizer finding can be confounded by the neighbour, and the
validated tree is byte-identical to what was committed (verified with `cmp`
on all four files).

| check | result |
|---|---|
| configure + build | clean; **zero** new warnings in the four files (the 4 hits in `SWMMEngine.cpp` are pre-existing unused constants at lines 88, 89, 2067, 2813) |
| `test_engine_output_quality` | 3/3 |
| full `ctest` | **140/141** — the one failure is `FvEngine.RefiningTheMeshConvergesTowardTheDynwaveHydrograph`, the known pre-existing baseline failure |
| water-age suite | 16/16 |
| 14-deck `.out` bit-identity vs `957a1d62` | **14/14 identical**, plus 2 extra WATER_AGE-off probe decks identical |
| ASan + UBSan | output-quality **0 findings**; water-age 16/16 with the one **pre-existing** `HotStartManager.cpp:246` misaligned CRC load (`4e29c8869`, carried from A2a — not A2b's) |
| falsifiers | 7 of 8 caught; see §5.3 |

### 5.2 The gate is not vacuous, and the number is exact

Unlike A2a's, these gates reach their assertions. The age column reads
**2.166667 h** against an analytic 2 h + 10 min — not merely inside the
±0.5 h band the gate allows, but exact to six decimals, so the 3600× units
razor is settled by the value rather than by the band.

**The delivered gate's deck cannot see per-element differentiation**, though:
on a level pool every element holds the same age, so an implementation that
broadcast element 0's age to all of them would pass. Measured directly on a
flow-through chain (`_a2b_flow.inp`, 5 cfs baseline, `INITIAL_STATE 0`):

| element | ARD | LEGACY |
|---|---|---|
| J0 | 0.003128 | 0.001796 |
| C1 | 0.410400 | 0.707884 |
| J1 | 0.713766 | 0.710954 |
| C2 | 1.108641 | 1.390729 |
| J2 | 1.402799 | 1.393704 |
| C3 | 1.635805 | 1.793262 |
| OUT | 1.809253 | 1.794488 |

Monotone increasing downstream, and the **node** ages agree between two
independent age engines (FV mesh vs CSTR mirror) to under 1% — the column is
a genuine per-element travel-time field, not a broadcast.

The **link** ages differ structurally and legitimately: ARD publishes a link
value strictly between its end nodes (a per-cell profile averaged), while
LEGACY's `updateLinkQuality` mirror assigns each link its DOWNSTREAM node's
age (A1b stage 4 reads `node_age` post-mixing with k = 0). That difference
was invisible before A2b; it is now in the `.out`, and it is worth stating in
the user manual rather than treating as a discrepancy.

Also confirmed against the shipped behaviour rather than assumed:

- **`WATER_AGE` + zero pollutants** now reports one column named
  `__WATER_AGE__` (`_a2b_ageonly`). Before A2b the block was gated on
  `n_pollutants() > 0` and such a deck reported nothing at all — this is new
  reach, not just a new column.
- **`IGNORE_QUALITY` + `WATER_AGE`** → `npollut = 0`, no columns
  (`_a2b_iq_age`), matching the writer's own bypass.
- **A pollutant value that changes when age is switched on** — node `OUT`
  reads TSS 42 with age off and **0** with age on — is **not** A2b. The same
  deck with `EULERIAN_ARD` and age OFF (`_a2b_ard_noage`) already reads 0
  there, and LEGACY + age ON reads 42 at all four nodes. It is the known ARD
  outfall behaviour, and the 14/14 byte-identity is what proves the point
  generally.

### 5.3 Falsifier sweep — one nominated falsifier was blind, one is inert

Patches applied by unique-anchor replacement (abort if the anchor is missing
or ambiguous) and restored from saved copies, never `git checkout --`: this
worktree is detached at the base, so a checkout would have silently deleted
the whole changeset and left every later falsifier measuring base. Verified
green after restore.

| falsifier | outcome |
|---|---|
| i. writer strides by `n_pollutants()` | **caught** — `pollut_count` 1 |
| ii. drop the node age fill | **caught** — age reads 0 |
| iii. age in SECONDS | **caught** — 7800 |
| **iv-a. reorder the NAMES only** | **ESCAPED as delivered** — see below |
| iv-b. move the age DATA column first | **caught** — both legs |
| v. `snap.pollut_names` left on the pollutant list | **inert for `.out` by construction** — see below |
| vi. drop the age name, writer still counts it | **caught** — header ID reads `''` |

**iv-a escaped, and it is the falsifier §3.3 calls "the stride razor."**
Reordering only the name list leaves the header calling column 0
`__WATER_AGE__` and column 1 `TSS` while the data keeps TSS at 0 and age at
1. Every value assertion still passed, because the gate reads by fixed index
and never looked at a name. That matters more here than in a normal column
swap: §2.3 makes the NAME the only way to tell HOURS from a concentration,
since the age column deliberately writes MG_PER_L's unit code — so the one
field consumers must trust was the one field nothing checked.

**Fixed, in the delivered gate** (`read_species_ids`, +30 lines): the header's
species IDs are now read from the bytes and asserted to be
`{TSS, __WATER_AGE__}` in order. Re-ran iv-a against it — now **caught**
("the age column must be named LAST"). The helper reads bytes because the
shipped readers give no other option; see §5.4.

**v is inert against the `.out` and cannot bite in shipped code.**
The handoff predicts "header ID for column 2 is empty/garbage", but
`writeHeader` takes `const SimulationContext&` and reads
`ctx.reported_species_names` directly — it never touches `snap.pollut_names`.
So the file is unaffected and the gate passes correctly. The pointer is
consumed only by `GeoPackageOutputPlugin`, where under the falsifier
`(*snapshot.pollut_names)[1]` with `pollut_count == 2` and a size-1 vector is
an out-of-bounds `std::vector` read — worse than garbage. Shipped code cannot
reach it: `pollut_count` and `pollut_names` now derive from the *same*
vector, which is the argument for the single-truth vector holding.

### 5.4 Findings recorded, not fixed (all outside A2b's four files)

1. **§2.3's mitigation has no reader.** "Readers must key on the NAME" is
   sound for the **legacy** reader — `SMO_getElementName(…, SMO_pollut, …)`
   exists and third-party tools use it. It is **not available in the modern
   stack**: `swmm_output_*` has no pollutant-ID entry point,
   `OutputReader` parses and stores subcatch/node/link IDs but **no**
   pollutant IDs, and the Python binding exposes only `pollutant_count`. A
   modern-API consumer sees `pollut_count == 2` and a column of hours
   labelled MG/L with no way to discover otherwise. The format decision is
   fine; it needs a `swmm_output_get_pollut_id` to be actionable. Recommend
   adding it before A2b ships to users — it is a small, additive change, but
   it is a new API surface and so not this changeset's to make.
2. **GeoPackage emits no species rows at all — pre-existing.** §3.5 expects
   the age row "for free". It will not appear, and neither do the pollutant
   rows today: `populate_default_variables` (`GeoPackageSchema.cpp:1330`)
   inserts a fixed variable list with no species entries, nothing else
   inserts one, and `update()` skips any row whose `lookup_variable(pname,
   …)` returns −1. A2b makes this no worse and is correctly shaped for the
   day it is fixed (both the count and the names come from the snapshot).
   Not reachable from `[PROCESS_COMPONENTS]` either — the plugin is only
   attached on a GeoPackage-sourced model — so this is by inspection.
3. **A dry element keeps aging.** A first run of the chain deck delivered no
   inflow (my `[INFLOWS]` row lacked a baseline); the links then reported
   exactly **6.000000 h** — the full simulation length — on water that never
   existed. That is state behaviour from A1a/A1b, not reporting, but it now
   reaches the `.out`, where "age 6 h" on a dry pipe is a misleading number.
   Worth a dry-element convention (report 0, or the fill value) before A3.

### 5.5 Answers to the two questions the handoff asks

- **§2.2, age-old interpolation:** agree with the choice — do not add it. But
  note the consequence: on a transient deck the pollutant columns are
  interpolated to the report instant while the age column is the routing
  step's published value, so the two are sampled up to one routing step
  apart (≈ 5 s here). Below any reporting resolution, but it should be stated
  where the column is documented.
- **§2.3, a 4th unit code:** agree — keep code 0. Widening the enum buys
  nothing until finding 1 above is closed, and costs every existing reader.
