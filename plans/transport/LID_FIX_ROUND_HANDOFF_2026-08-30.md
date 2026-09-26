# The LID round: three units gates + one live pairing defect — Handoff (2026-08-30)

**For:** the implementing/checking agent.
**Base:** `8f9f164d` (180 registered, 175 passing, 5 failing).
**Standing findings:** lessons 1–189.
**Reference:** `tests/output/rebaseline_8f9f164d/corpus/`.

**This is TWO rounds, not one, and they must not be merged.** Part A is gate
re-derivation with no production change. Part B is a live defect fix in
`SWMMEngine.cpp`. Landing them together means a moved gate cannot be
attributed to either.

---

## 0. What the check established, and what it corrected

**PR #103 is correct; the gates encode the regime it repaired.** Measured
drain ratio **12 470.8× = 43 200/√12** — the rate-units defect (in/hr used as
ft/s) *times* a second one (head in ft read as inches) interacting through
`pow(h, expon)`. Runoff continuity on the `heat_lid` deck goes
**96.5 % → −0.067 %**: the "more physical" tell, in numbers.

**My §5.4 falsifier was too narrow and failed as written.** Restoring one
line left all four gates red. The check's split is the real attribution:

| restore at `8f9f164d` | water_age_lid | heat_lid | transport_dt_ref | heat_watershed |
|---|---|---|---|---|
| drain line only | red | red | red | red |
| **all 19 of #103's unit conversions** | **green** | **green** | **green** | red |
| #103 units kept, availability clamp off | red | red | red | red |
| pre-merge own-subcatch drain semantics | — | — | — | **green** |

**So three gates encode the WHOLE pre-#103 units regime** — thickness, three
ksat conversions, void ratio, init_sat, drain delay, offsets — **and
`heat_watershed` is a different defect entirely.** That is Part A and Part B.

---

# PART A — re-derive the three units-regime gates

**MUST NOT restore any pre-#103 LID unit conversion**, singly or in the group
that turns the gates green. That group *is* the defect. If the sequence feels
blocked, the sequence yields.

Take them in §4's **horizon/premise-first** order — the claim before the
number:

1. **`StorageAgeIsTheSumOfTheLayerResidenceTimes`** (`held_s/chain_s = 0.00057`).
   The claim is structural and should survive. 0.00057 says the chain has not
   filled, not that the sum is wrong. **Extend the horizon and re-measure
   before touching any expectation.**
2. **`DrainLeavesAtStorageAgeAndReachesTheNode`** — same shape; run until the
   drain actually delivers, then assert.
3. **`LidColumnTemperatureConvergesUnderRefinement`** (ratio 1.67 vs band
   0.0011). The band was fitted in the old regime. **Re-measure the achievable
   floor and re-pin; do not widen the band to fit** (lesson 149). If the column
   is genuinely first-order now, that is a finding about the scheme, not a
   tolerance to relax — say so rather than absorbing it.

**Gate value test for each:** after re-derivation, restore the full pre-#103
units group and confirm the gate goes **red** again. A re-derived gate that
passes in both regimes has stopped observing the thing it was re-derived for.

---

# PART B — `heat_watershed`: a live defect, three questions deep

`SWMMEngine.cpp:2071-2073`, the own-subcatchment / no-target drain branch:

```cpp
} else {
    ctx_.subcatches.runoff[usc] += g.drain_flow[uu] * lid_area;  // CFS
}
```

**B1 — the pairing is broken, and this part is not in question.**
The run-on branch above (`:2025`) pairs its water with age *and* temperature
(`addRunonTemperatureAt`). **This `else` branch pairs neither.** Water enters
`subcatches.runoff` carrying no temperature and no age — H5b's pair invariant,
broken. `runon_inflow[0] == 0` is the symptom; the missing pairing is the
defect. **Fix it in whichever channel B2 settles on.**

**B2 — routing or reporting?** Legacy is unambiguous that this is *not*
run-on: `lid_addDrainRunon` (`lid.c:1554`) guards `if ( k >= 0 && k != j )`,
excluding the own-subcatchment case along with its pollutant load. And legacy
*does* add drain flow to runoff — at **`subcatch.c:897-900`**, under the
comment *"add any LID drain flow to **reported** runoff"*, with drain tracked
separately in `VlidDrain`.

**So the question is whether `subcatches.runoff[usc] +=` is our reporting line
or a routing addition.** If routing, we create water legacy does not.
**Measure it**: does that addition propagate into the subcatchment's outflow
to its outlet node, or only into the reported runoff series? The continuity
ledger answers it faster than reading will.

**B3 — water and quality take different paths here, and that needs checking.**
For this same self case, the quality block at `:2099` computes
`target_sc = (drain_subcatch >= 0) ? drain_subcatch : sc` — **without** the
`!= sc` guard the water path has — and books quality/age to *that
subcatchment's outlet node* (`lid_drain_qual_vol`). So:

- **water** → `subcatches.runoff[usc]`
- **quality/age** → the outlet node's drain accumulators

**If `runoff[usc]` also reaches that outlet node, the volume is counted once
and the quality twice** — or the two disagree about how much water carried the
load. Either way the seam is inconsistent, and it is invisible to any gate
that looks at only one side. **This is the check to run before choosing B2's
answer**, because the answer must make both sides agree.

**⚠ One discrepancy to resolve first.** The comment at `:2016-2019` says
*"no target / self → discharge to THIS subcatchment's outlet ... EPA sends it
straight to the outlet"*, but the code adds to `subcatches.runoff[usc]`.
Those are the same thing only if runoff routes to the outlet unmodified.
**The comment and the code may already disagree** — and per lesson 154 a stale
comment outranks a stale document, so settle which is right before building on
either.

## Part B protocol

1. Answer **B3** first (the two-path check) — it constrains B2.
2. Answer **B2** by measurement against the continuity ledger.
3. Fix **B1** in the channel B2 selects. The pairing fix is owed regardless of
   how B2 resolves.
4. **New gate**: own-subcatch drain water arrives carrying its temperature and
   its age. It must **fail at base** — `runon_inflow[0] == 0` today, so a gate
   asserting a *paired* arrival has something to bite on.
5. Falsifiers: (i) drop the temperature pairing → new gate fails, age leg still
   passes; (ii) drop the age pairing → mirror; **(iii) route self-drain as
   run-on** → expect the double-feed continuity leak `:2017` names, which is
   the check that legacy's `k != j` guard is doing real work.

## Both parts

- **Corpus 20/20** against the stored reference throughout. No corpus deck has
  LID underdrains, so **any movement means the change reached further than
  intended.**
- Record in `IMPLEMENTATION_ROADMAP.md`, not only here.
- **`fv_tpa_closure` is out of scope** — pre-dates both merges (`47c00ae3`),
  belongs to the peer.
- **H7b unblocks when Part A is green and B1 is fixed.** B2/B3 may resolve into
  their own follow-up if the seam turns out to be a larger mass-balance
  question — but the pairing must not wait for that.

---

# CHECK/IMPLEMENTATION RECORD — 2026-08-30

**Landed as THREE commits on `swmm6_rel`** (the handoff asked for two; the
first exists because Part A could not open its own decks):

| commit | part | content |
|---|---|---|
| `e07d66e5` | **0** | the Gap #82 validator checked the storage/pavement void RATIO against (0, 1] — the FRACTION's range — so 75 % voids (ratio 3) was ERROR 185 and Part A's re-expression could not open. Legacy accepts any non-negative ratio. Gate `StorageVoidRatioAboveOneIsLegal` fails at base |
| `082dd7c1` | **A** | deck re-expression in user units — the plan the test files' own @warning blocks recorded ("when the conversion lands, these decks must be re-expressed … and they will fail loudly until they are"). SAME physical column: drain 12.4708 in/hr·in^-0.5 = 1e-3 ft/s·ft^-0.5 × 43200/√12 exactly |
| `ee7494ea` | **B** | target-less/self underdrain resolves to the outlet (legacy `lid.c:1215`), water+quality one path, temperature+age paired at the node seam, RUNOFF_DRAINS ledger + LID Drainage row |

Every number from isolated worktrees; committed content `cmp`-equal to the
isolated tree for all 11 files; the shared tree's peer hunks (`SWMMEngine`
@~4570/@~4860, `DefaultReportPlugin` @62/@571) untouched — staged via
`git apply --cached`, never `git add`, on the two peer-dirty files.

## Part A — the horizon question §4 asked turned out to be a units question

The handoff's horizon-first reading assumed the decks stay fixed. The files
themselves had already decided otherwise (their @warning was written for
exactly this day), so the gates' claims AND numbers survive re-expression:
`StorageAgeIsTheSumOfTheLayerResidenceTimes` passes with no band change,
`heat_lid` 9/9 unchanged. Deviations, both measured and recorded in the
commit: `DrainLeaves` needs a slower drain (1.0) once nothing recirculates;
dt-reference bands re-pinned to corrected-engine floors (age 0.0080 /
measured 0.00644 at contraction 2.77; temperature 0.0095 / 0.00762 at 1.89 —
the ~1.9 contraction is flagged in the test as a composition finding, per §A3).
**Gate-value test:** restoring the whole pre-#103 unit group turns the gates
red (`A_units_group.log`). **Intermediate state pinned:** at `082dd7c1`
exactly one gate is red — `heat_watershed.EveryRunonContributor…`, same as at
base (measured in a worktree holding P0+PA only).

## Part B — B2/B3 answered, then two more defects under them

- **B2 (routing or reporting): routing, to the NODE.** Legacy resolves a
  target-less drain to `outNode` at init (`lid.c:1215`) and books
  RUNOFF_DRAINS only for node targets; `subcatch.c:897`'s addition is the
  REPORTED series. The comment at `:2016` ("EPA sends it straight to the
  outlet") was right; the code under it wasn't.
- **B3 (two paths): confirmed, and fixed by unification** — water and
  quality both take the node path; the external-inflow loader books only the
  non-drain share, so the drain volume is in the quality denominator once,
  at storage age/temperature.
- **Found under B1: drain-to-node water NEVER reached the network.**
  `clearInflowSources()` zeroes `ext_inflow` at the top of every routing
  step — after `stepRunoff` added the drain. Every drain-to-node case, not
  only the self case, silently lost its water. New `nodes.lid_drain_inflow`
  channel (per-runoff-step, like `lid_drain_qual_vol`) joins the lateral
  assembly and the external ledger term.
- **Found under that: the runoff ledger has no drain-outflow term** (legacy
  RUNOFF_DRAINS / the `report.c:551` LID Drainage row). Added both. Ledger
  parity nuance recorded in the plugin: legacy prints the row whenever LID
  area exists; we print it when the term is nonzero.
- The node-seam temperature stand-in (`HeatSource::RAINFALL`) is retired by
  `node_lid_drain_temp_vol_in` — `HeatSource`'s comment claimed H5b had
  already done this; the code had not (lesson 154's family).

## Falsifiers

| # | expected | observed |
|---|---|---|
| A. pre-#103 unit group restored | 3 suites red | red across water_age_lid / heat_lid(7b) / heat_watershed / dt_ref ✓ |
| B-i. temperature booking dropped | q·T leg only | "the drain's temperature-volume at J1 is not q * T_storage", age leg passes ✓ |
| B-ii. age booking dropped | q·age leg only | mirror ✓ |
| B-iii. self-drain as run-on | double-feed leak | runoff continuity −46.8 % vs −43.5 % on the recirculation deck ✓ |

**The recirculation deck's −43.5 % is PRE-EXISTING and not this round's:**
base vs patched on an identical ratio-1.0 variant measured **−44.007 % vs
−44.063 %** — the gap is the flooded volume (3.4 acre-ft) that leaves the
routing system and never returns to the runoff ledger's loop; the routing
ledger itself closes at −0.24 %. Recorded, not chased here.

## Suite / corpus

ctest **179/180 ×3** (isolated, full rebuild — an earlier ×3 read 15 failures
from STALE test binaries after a struct-layout change built with a 6-target
ninja: lesson `stale-test-binary-partial-target-build`, again). The one red
is `fv_tpa_closure`, the peer's, red since `47c00ae3`. Corpus **20/20 `.out`
byte-identical and 0/20 `.rpt` moved** vs `tests/output/rebaseline_8f9f164d/corpus/`
— re-run AFTER the report-writer change (no corpus deck has LID drains, so no
deck gains the new row).

## Consequences

- **H7b is UNBLOCKED**: Part A green, B1 paired, and the heat gates are
  attributed and green at `ee7494ea`.
- Owed onward: `TheUnderdrainContributesToRunonTemperature`'s drain leg is
  now vacuous for self-drains (gate 7b covers the node seam); legacy prints
  LID Drainage at 0.000 for drain-less LID decks, we omit it; the standing
  figure is now **179/180 at `ee7494ea`, one known (fv_tpa_closure)**.
