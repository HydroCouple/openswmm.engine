# Snapshot Quality Population — Validation & Commit Handoff (2026-08-17)

**For:** the checking agent (sandbox: `g++ -fsyntax-only` only).
**Base:** `f704b83d` (post-A2a).
**Not a plan phase** — a defect fix found while scoping A2b, which is
blocked on it. Roadmap ⚠ row.
**Standing findings:** lessons 1–38; this defect IS lesson 26 a third time
(the age column would have faithfully reported a broken carrier) and
lesson 33's cousin: every transport gate in the suite reads `ctx` arrays
DIRECTLY, so none of them could observe a reporting-handoff failure.

---

## 1. The defect

`SimulationSnapshot::{node,link,subcatch}_quality` (declared in
`include/openswmm/plugin_sdk/SimulationSnapshot.hpp`) are READ by
`DefaultOutputPlugin` (binary `.out`, lines 143/164/185) and
`GeoPackageOutputPlugin` (156/211/261), and were **written by nothing**.
Exhaustive grep: 9 readers, 0 writers (the only other matches are the
unrelated `forcing.*_quality_*` arrays).

Both readers guard with `qi < size()` and fall back to `0.0`, so **every
pollutant column in every `.out` file was written as zero** while
`writeHeader` advertised `n_polluts_` columns, their IDs, and their unit
codes. Consequences: `.out` quality series, `analysis_output_*` quality
reads, GeoPackage quality output, and any GUI plot of concentration.

The engine state was correct throughout — `nodes.conc` / `links.conc`
carry the routed values — so this is purely the snapshot handoff.

## 2. The fix (changeset, uncommitted)

```
mod:  src/engine/core/SWMMEngine.cpp
      (snapshot builder: populate the three quality vectors just before
       the name-table pointers are attached)
new:  tests/unit/engine/test_output_quality.cpp   (2 gates)
mod:  tests/unit/engine/CMakeLists.txt
```

Interpolation matches the neighbouring snapshot fields AND legacy, cited
in the comment at the site:

| quantity | legacy site | formula |
|---|---|---|
| node | `node.c:502` | `f1*oldQual[p] + wt*newQual[p]` |
| link | `link.c:724` | `f1*oldQual[p] + f*newQual[p]` |
| subcatch | `subcatch.c:929-930` | `runoff == 0 ? 0 : f1*old + wt*new` |

Concentrations are already in user units (no UCF). Gated on
`!ignore_quality && n_pollutants() > 0`, which matches the writer's own
`n_polluts_ = ignore_quality ? 0 : n_pollutants()` — an ungated fill
would push values into columns the header says do not exist.

## 3. Validation protocol

1. Reconfigure (one new test TU), build, zero new warnings.
2. `ctest -R test_engine_output_quality` — 2 gates. Both read back through
   the PUBLIC output reader (`swmm_output_get_{node,link}_attribute`),
   deliberately: a gate reading `ctx` arrays cannot see this defect.
   *Anticipated failure modes:*
   (a) **Column offsets** — the gates index node `v[6]` and link `v[5]`
   from `writeHeader`'s layout (node: 6 fixed + pollutants; link: 5 +
   pollutants). If the reader returns a different ordering, fix the index,
   not the expectation.
   (b) **The level-pool deck must actually hold Cinit** — zero flow, wet
   junctions. If the settled value is not 42, print the `ctx` array first:
   a mismatch there is a DIFFERENT (transport) issue and this fix is still
   correct.
   (c) `swmm_output_get_pollut_count` (not `..._pollutant_count`) is the
   spelling used.
3. **Falsifier sweep:**

   | falsifier | expected failing gates |
   |---|---|
   | i. remove the node_quality fill | gate 1 node leg (reads 0) |
   | ii. remove the link_quality fill | gate 1 link leg |
   | iii. drop the `!ignore_quality` guard | gate 2 (or a size/format mismatch — record which) |
   | iv. drop the subcatch runoff gate (fill unconditionally) | **likely EMPTY on this deck (no subcatchments)** — flagged in advance per lesson 11: if it comes back green, either add a washoff deck reading `swmm_output_get_subcatch_attribute` or record the subcatch leg as review-only |
   | v. use `f_rt` alone (no interpolation) | likely green on a steady deck — record; the interpolation is legacy-parity, and pinning it needs a TRANSIENT deck (a rising washoff front). Worth adding if cheap. |
4. **Prior suites all green.** Expect **`.out` bit-identity to BREAK for
   every deck with pollutants** — that is the point of the fix (zeros →
   real concentrations). Verify instead that: (a) decks WITHOUT pollutants
   stay bit-identical; (b) for one pollutant deck, the changed bytes are
   confined to pollutant columns and the new values match `ctx.nodes.conc`
   at the same period; (c) the `.rpt` continuity tables are unchanged
   (they read the mass-balance ledger, not the snapshot).
5. **Legacy parity opportunity:** if a legacy SWMM `.out` for the same
   deck is available, this is the first chance to compare quality series
   directly. Record tolerances if run.
6. Append results to §5; commit with §4.

## 4. Commit message

```
fix(output): populate the snapshot's pollutant concentrations

SimulationSnapshot::{node,link,subcatch}_quality are what
DefaultOutputPlugin and GeoPackageOutputPlugin write into their pollutant
columns, and nothing ever filled them: both readers guard with
`qi < size()` and fall back to 0.0, so every pollutant column in every
.out file was written as ZERO while the header advertised the column
count, IDs and unit codes. The engine state was correct throughout
(nodes.conc / links.conc carry the routed values) - only the snapshot
handoff dropped them, which is why the transport suites, all of which
read those arrays directly, never observed it.

The builder now fills the three vectors with the same report-instant
interpolation the neighbouring fields and legacy use (node.c:502,
link.c:724, subcatch.c:929 including its runoff gate), in user
concentration units, gated on !IGNORE_QUALITY && n_pollutants() > 0 to
match the writer's own column count. Found while scoping A2b (water-age
reporting), which would otherwise have added an age column to a pipeline
that reported nothing.

Expected: .out files for decks WITH pollutants now differ from before
(zeros -> concentrations); decks without pollutants are unchanged.

Gates: tests/unit/engine/test_output_quality.cpp (2, read back through
the PUBLIC output reader - a ctx-array gate cannot see this defect).
```

## 5. Validation results

**Verdict: fix is correct and lands. Committed.** Checking agent had a full
toolchain (not the `-fsyntax-only` sandbox the header assumes), so the whole
protocol ran for real. Preset `Darwin-tests-release`, base `f704b83d`.

### 1. Build
Reconfigured (one new TU), 347/347 targets, **0 errors**. No new warnings in
either touched file: the four `SWMMEngine.cpp` warnings are pre-existing
unused-variable hits at lines 88/89/2058/2804, nowhere near the fill (~4170).

### 2. Gates — 2/2 PASS
`test_engine_output_quality`: NodeAndLinkConcentrationsReachTheOutFile,
IgnoreQualityWritesNoPollutantColumns. None of the three anticipated failure
modes occurred — the `v[6]`/`v[5]` column offsets were right, the level-pool
deck does settle at 42, and `swmm_output_get_pollut_count` is the spelling.

### 3. Falsifier sweep — 2 bite, 3 green (all three explained)

| falsifier | result | finding |
|---|---|---|
| i. remove node_quality fill | **FAILS** ✓ | gate 1 bites |
| ii. remove link_quality fill | **FAILS** ✓ | gate 1 bites |
| iii. drop `!ignore_quality` guard | green | **not a weak gate — unobservable BY CONSTRUCTION.** `DefaultOutputPlugin.cpp:256` sets `n_polluts_ = ignore_quality ? 0 : n_pollutants()`, so the writer's pollutant loop runs zero times whatever the snapshot holds. No size/format mismatch either. The guard is real defence-in-depth (it avoids computing vectors nobody reads) but CANNOT be pinned through the public reader. Gate 2 still correctly passes: the header genuinely says 0 pollutants. |
| iv. drop subcatch runoff gate | green | as predicted — deck has no subcatchments. **Subcatch leg is review-only**; pinning it needs a washoff deck reading `swmm_output_get_subcatch_attribute`. |
| v. `f_rt` alone (no interpolation) | green | predicted "steady deck", but the REASON matters and is not the predicted one — see below. |

### 3a. The interpolation is NOT inert — corrected finding
The prediction for (v) assumed the weights collapse. Instrumenting `f_rt`
directly on the gate deck (`ROUTING_STEP 5`, `REPORT_STEP 60`) shows they do
not:

```
18 reports  f_rt = 0.900000  span = 5000 ms
 2 reports  f_rt = 1.000000  span = 4500 ms   (duration-clamped tail)
```

So `f1_rt = 0.1` on almost every report — the interpolation is live even on a
fixed step that divides the report step evenly, because the routing clock sits
500 ms off the report grid. `compute_next` confirms it: no report-boundary
alignment, legacy free-steps and interpolates (`output.c:650/682`). **NB the
doc comment on `TimestepController::compute_next` is STALE** — it still
advertises "time to the next report instant (clamp scheduled for removal)";
the clamp is already gone. Worth a separate one-line docs fix.

Falsifier (v) is therefore green for a VALUE reason, not a weight reason: on a
level-pool deck `conc_old == conc == 42`, so interpolating between two equal
values returns 42 for any weights. Pinning the interpolation needs a TRANSIENT
deck (rising washoff front) where `conc_old != conc`; until then the
interpolation is **review-only, not gated**.

### 4. Prior suites + `.out` bit-identity
`ctest -j 6`: **142/143 pass**. The single failure is
`FvEngine.RefiningTheMeshConvergesTowardTheDynwaveHydrograph`, the known
pre-existing FV refinement-convergence failure. Proven not collateral: that
deck declares no `[POLLUTANTS]`, and the fill is gated on
`n_pollutants() > 0`, so the changed code never executes on it.

Matched decks (identical but for a `[POLLUTANTS]` block), each run against a
before-image build (fill disabled) and the fixed build:

| check | result |
|---|---|
| (a) deck WITHOUT pollutants | **bit-identical** ✓ |
| (b) deck WITH pollutants | differs ✓ (the point of the fix) |
| (b) differences confined to pollutant columns | **proven semantically**: read both files back through the public reader and compared every cell — **0 differing non-pollutant cells**, 70 differing pollutant cells, before = 0.0, after = 42.0 |
| (c) `.rpt` continuity tables | **identical** ✓ |

A byte-offset decode of the changed runs was attempted first and abandoned —
it mis-aligned and reported a meaningless `0.0 -> 0.0`. The attribute-level
comparison above is the trustworthy form of that check.

### 5. Legacy parity
Not run — no legacy `.out` for these decks was to hand. Still open.

### Residual risks (carried forward)
- subcatch washoff leg: **review-only**, no gate (falsifier iv).
- report-instant interpolation: **review-only**, no gate (falsifier v);
  needs a transient washoff deck.
- `!ignore_quality` guard: unpinnable through the public reader (falsifier
  iii) — correct but redundant with the writer's own column count.
- `TimestepController::compute_next` doc comment is stale (claims a
  report-boundary clamp that no longer exists).

