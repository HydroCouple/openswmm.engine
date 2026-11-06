# Reusable reaction networks

Run each `.inp` with the OpenSWMM 6 executable; its `.rxn` must remain beside
it. Native output contains named species columns. The first three decks hold
a level pool to isolate kinetics from advective mixing.

| Network | Independent reference after one hour |
| --- | --- |
| `parent_product` | A = exp(-0.3) = 0.7408182207; B = 1 - A = 0.2591817793; A + B = 1 |
| `arrhenius` | CL2 = 2 exp(-0.3 * 1.07^5) = 1.3130880606 (computed with TEMP fallback 25 °C) |
| `attached_wall` | Pipe W = 10 exp(-0.2) = 8.1873075308; Tank W stays 5; A stays 1 |

`parent_product` uses equal mass units and unit yield; its sum is a mass
invariant in this isolated pool. COUPLING FULL integrates both rates from
the same evolving A. The wall example checks an attached local density;
it does not assert a water-volume mass balance for W or a wall-to-bulk
geometry conversion. It runs under LEGACY; ARD falls back to LEGACY for
WALL systems and LAGRANGIAN refuses them.

For element-specific kinetics append `NODE Tank k 0.6` or `LINK Pipe k 0.15`
under `[REACTION_PARAMETERS]` in the parent/product configuration. The
corresponding element then uses that rate while other elements retain 0.3.

The analytic
multispecies suite in `openswmm.engine.benchmarks/suites/quality/reactions`
runs the parent/product model across network quality solvers and time units.

## Rain-fed wet subcatchment pool

`wet_pool.inp` and `wet_pool.rxn` convert A to B only in the wet subcatchment
pool. Rain carries A and a conservative tracer at 2 mg/L. The independent
invariant A+B=tracer must hold in wet subcatchment and network output, and B
must reach the receiving network. There is no claim of a closed-form
concentration curve for this rainfall/runoff model.

The scope mixes retained mass with rain/run-on, reacts for the runoff
interval, and allocates mass to infiltration, runoff and storage. Check
WET_STEP sensitivity for rapid kinetics. Evaporation retains solute in the
remaining mass. Pools at or below one litre do not integrate concentrations;
LID units use the separate layer scope demonstrated below.

## Additional qualified examples

- `wall_geometry`: prescribed M2 contact areas with equal-unit bulk/wall exchange; A*water_litres + W*reference_area is constant and bulk A follows an exponential. Uses LEGACY; ARD falls back to it.
- `dry_buildup`: land-use A→B conversion before accrual/washoff. At one hour, remaining A=1000 exp(-0.3) project mass units and A+B=1000. No rainfall or accrual masks the dry reactor.
- `lid_layers`: a fully covered bioretention system, deliberately saturated and overloaded to exercise surface and drain exports. A+B=tracer in retained inventories and exported concentrations; this is a conservation example, without an analytic history.
- `setpoint`: positive clean-water inflow with a 2 mg/L mixed-store target. All three network solvers report Tank A=2.

The engine tests separately qualify full native continuation across network solvers,
wet/dry inventories, bioretention/permeable-pavement layers and bucket/Richards
storage nodes. Partial capture, covered barrels, WALL porous chemistry and active
2D chemistry remain outside these profiles. Examples are numerical verification,
not calibrated chemical or treatment designs.
