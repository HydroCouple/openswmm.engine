# Street-inlet capture parity set

Conduit-attribute inlet decks run through both the refactored engine and the
legacy SWMM 5.2 engine (`openswmm` vs `openswmm-legacy`, both from the same
build directory), comparing the inlet performance statistics, the Street Flow
Summary and the capture nodes' lateral inflow.

```
python3 tests/parity/inlets/run_inlet_parity.py                 # every deck here
python3 tests/parity/inlets/run_inlet_parity.py street_grate_inlet_7.inp
```

**Gate:** the capture efficiency at peak flow (the "Peak Flow Capture Pcnt"
column both engines print) must agree within `--tol-pct` (1.0 percentage
point). Where legacy prints no street row (a drop inlet on an open channel) the
capture node's peak lateral inflow is compared instead, within `--tol-flow`
(5 %, routing transient included). Everything else — peak flows, average
capture, bypass and backflow frequency, volumes — is printed for review.
Exit 0 only when no un-attributed deck fails. Reports land in `_out/new/` and
`_out/legacy/` (gitignored).

## Decks

The EPA regression models that introduced street inlets in SWMM 5.2
(`SWMMRegressionTestSuite/models/update_v52`, public domain; HEC-22 worked
examples where noted), three surcharged-sewer variants from the same author's
test set, and five variants made here (title block says how).

| Deck | What it reaches |
|---|---|
| `street_grate_inlet_7.inp` | on-grade curved-vane grate, depressed gutter (a = 0.167 ft, W = 2 ft); HEC-22 ex. 4-7 |
| `street_curb_inlet_9a.inp` | on-grade curb opening, CMS units, two streets with different gutter depressions, two capture nodes |
| `sweeper_combo_inlet_10.inp` | on-grade combination inlet (grate + curb sweeper), depressed gutter |
| `onsag_grate_inlet_11.inp` | on-sag grate at a topological sink, two inlets, 50 % clogged |
| `onsag_curb_inlet_12.inp` | on-sag curb opening at a sink |
| `drop_grate_inlet_17.inp`, `drop_grate_inlet_18.inp` | drop grate on a trapezoidal channel (HEC-22 ex. 4-17 / 4-18); legacy prints no street row for a non-street host |
| `Example7-Inlets.inp` | EPA Example 7 dual-drainage network: half- and full-street sections with backing, combo inlets on four streets, saturated streets |
| `steady_fullstreet_q37.inp`, `steady_halfstreet_q37.inp`, `steady_fullstreet_q10.inp` | Example 7's two street sections and inlet on the deck-7 layout with a *constant* inflow: kernel probes free of routing transients, saturated (37 cfs) and not (10 cfs) |
| `street_grate_inlet_7_backwater.inp`, `onsag_grate_inlet_11_backwater.inp` | the sewer outfall held at a fixed stage (as distributed; neither engine actually backflows) |
| `street_curb_inlet_9a-CFS_backwater.inp` | two capture nodes under a fixed downstream stage |
| `street_grate_inlet_7_backflow.inp` | 30 cfs added at the capture node so it floods: net capture goes negative and sewer overflow returns to the street (Back Flow Freq 100 % in both engines) |
| `street_grate_inlet_7_kinwave.inp` | deck 7 under KINWAVE (only `FLOW_ROUTING` changed) — listed as a known divergence, see below |

`Example7-Inlets.inp` names a backdrop image in `[BACKDROP]`; neither engine
reads it, so the deck is self-contained for a run.

## What the set shows (2026-09-06, engine build/darwin Release)

- **Capture kernel at parity.** Capture efficiency at peak is identical on
  every on-grade and on-sag deck (73.80 / 61.49 / 87.11 / 61.43 / 87.27 /
  100.00 %) and on the steady probes (38.66 / 30.8 / 57.02 %). Combination
  inlet with sweeper: peak captured flow identical to four figures.
- **Physical captured volume identical.** The sewer outfall totals agree to
  the reported precision (deck 7: 0.269 vs 0.269 Mgal; deck 11: 0.360 vs
  0.359; drop grates 0.508 vs 0.508 and 0.952 vs 0.951).
- **Peak flows differ for a routing reason, not an inlet one.** The refactored
  dynamic wave overshoots a step inflow at start-up (deck 7 with the inlet
  removed: 2.53 cfs peak on a flat 2.3 cfs inflow; legacy 2.300 exactly), so
  the *instant* of peak flow, and everything sampled at it, differs. On
  Example 7 the streets are saturated at peak and efficiency falls steeply
  with flow, which is why its peak-instant efficiencies disagree (17 vs 34 %)
  while its average efficiencies (68 vs 70 %) and volumes agree. Listed in
  `KNOWN` in the runner.
- **KINWAVE + STREET is a routing gap.** Under KINWAVE the refactored engine
  conveys no flow at all through STREET cross-sections — with or without an
  inlet (94 % continuity error; legacy routes it) — so the KINWAVE deck's
  zero capture is not the inlet's doing. Listed in `KNOWN`.
- **Node Inflow Summary statistics are not comparable across engines.**
  Legacy records the inlet transfer as a signed lateral (-1.70 cfs at the
  bypass node) and under-counts its volume at the capture node (0.229 vs a
  physical 0.269 Mgal); the refactored engine accumulates lateral statistics
  as absolute values, so the bypass node shows +1.78 / +0.270. Neither column
  is used as a gate.
- **Backflow.** With the capture node surcharged both engines report zero
  capture and 100 % backflow frequency at peak. The split of the excess
  between flooding at the capture node and return flow onto the street differs
  (peak backflow 16.0 vs 12.9 cfs), which follows from the two dynamic-wave
  solvers' surcharge and flooding treatment rather than from the inlet.

The refactored engine's own inlet-junction form (`[INLET_JUNCTIONS]`) has no
legacy counterpart and is covered by `tests/unit/engine/test_inlet_capture.cpp`
(inlet junction vs. the equivalent conduit inlet on the same street) and by
`examples/inlets/street_inlet_junction.inp`.
