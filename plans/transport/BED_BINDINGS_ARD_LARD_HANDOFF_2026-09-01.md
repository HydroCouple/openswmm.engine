# Bed bindings under ARD and LARD — Handoff (2026-09-01)

**For:** the next implementing agent.
**Base:** `3e87868e` or later.
**Precondition, now met:** H6b §3 (ARD does not relax) — FIXED `6264eb8a`.
**Reference:** H6b (`89310068`), its handoff + CHECK RECORD, `BedZoneData.hpp`.

## What is owed

`[HEAT_FLUXES] SEDIMENT_EXCHANGE` binds to the LEGACY link store only;
ARD and LARD decline by name at open (SWMMEngine.cpp, the H6b warning).
This round binds both, retiring the warning.

## The design decision, already recorded where it belongs

`BedZoneData.hpp` (the @warning on `BedZoneState`) pre-commits the shape:
the bed is per LINK and does not subdivide, so under ARD it exchanges
with the link's **volume-weighted mean** temperature/concentration, and
under LARD with the link's segment-mean. The resolution loss (a bed under
a long conduit cannot resolve a front) is recorded there. Do NOT invent a
cell-resolved bed in this round — that is the follow-on the warning
documents.

## Sketch

- **ARD heat:** per conduit link, after the per-cell flux stage: compute
  the volume-weighted mean cell temperature; `bedCouplingForLink` with
  the link's total water volume; ONE `relaxPair` step (surface term zero
  — the cells already took their surface exchange; the pair here carries
  only g_wb/g_bg, which keeps D-H5e's one-step rule for the bodies that
  actually exchange); distribute `dt_w` UNIFORMLY as a delta to the
  link's wet cells (a uniform delta preserves the profile shape; a
  volume-weighted redistribution would be a second mixing operator).
- **ARD solutes:** `applyBedSoluteExchange` already takes a generic
  array; feed it the link-mean concentrations and redistribute the delta
  the same way. Recovering bed area from g_bg (its one derivation) as
  the LEGACY binding does.
- **LARD:** heat rides H7's temp row on segments; same link-mean +
  uniform-delta shape over the link's segments, at LARD's own step
  seam (NOT applyHeatFluxes — falsifier i in the H6b record proved
  heat_lard does not pass through that loop).
- Retire the SWMMEngine.cpp bypass warning for whichever engines bind;
  flip/extend the warning gates the way H6b's round flipped the
  deferral pins (lesson 213: grep the warning text for its gates FIRST).

## Validation

- Cross-engine: the H6b closed-conduit deck (tests/output/h6b_bed_exchange/
  h6b_bed_on.inp) under LEGACY vs ARD vs LARD — outlet cooling within a
  falsifier-calibrated band (lesson 210: run the falsifier BEFORE
  choosing the band; candidate falsifier = distribute dt_w to the WRONG
  link's cells).
- The OFF path must stay byte-exact: corpus 21/21 (still the OFF-path
  statement only — no corpus deck sets SEDIMENT_EXCHANGE).
- ⚠ Stay INSIDE hydraulic contract on gate decks: the 2026-09-01 batch
  found ARD advection runs unbounded under CFL starvation on draining
  decks at forced large steps — a bed gate must not stand on that
  regime or it measures the wrong defect (lesson 215).

## Also owed nearby (recorded 2026-09-01, separate rounds)

- ARD advection robustness on draining decks (6.2e+117 degC cell state;
  evidence in tests/output/ard_relax_batch/PROVENANCE.txt).
- Temperature vs the transport's max(0, mass) clamp (0 degC floor).
- A corpus deck that runs ARD heat (and one that sweeps) — until then
  corpus green cannot see either surface.
