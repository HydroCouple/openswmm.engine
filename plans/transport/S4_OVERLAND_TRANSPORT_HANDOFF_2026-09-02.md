# S4 — age, temperature and MSX rows on the surface; the tuple's age/temperature halves — Handoff (2026-09-02)

**For:** the checking agent.
**Plan:** `OVERLAND_TRANSPORT_HEAT_MSX_PLAN_2026-09-01.md` §4 + phase S4, D-2DT4
(age/enthalpy halves), D-2DT5 (`CELL2D` enumerator only — S5 gives it storage).
Ride-along: the S3 record's InpWriter/sidecar round-trip gate.
**Implemented syntax-only.** `g++ -fsyntax-only -std=c++20 -fopenmp
-DOPENSWMM_HAS_2D` on every touched TU: **0 errors**. Nothing built or run.
**Base:** `bbd13775`. Working tree carries unrelated modifications again
(`PROCESS_COMPONENT_*` plan, regenerated fixtures). **Stage only the files
below, by patch.**

```
new: tests/unit/engine/test_2d_transport_s4.cpp               (8 gates: 2 direct-solver, 6 deck)
new: plans/transport/S4_OVERLAND_TRANSPORT_HANDOFF_2026-09-02.md
mod: src/engine/2d/data/SurfaceTransportState.hpp   (+n_pollut, n_msx, age_row, temp_row, row_names, signedRow, rowIndex)
mod: src/engine/2d/data/SurfaceStateData.hpp        (+node_row_conc — published node row values, ns-strided)
mod: src/engine/2d/solver/ExplicitInertialSolver.hpp/.cpp
       (sinkTemperatureWithEvap ×3 sites; signed-row sink; nonzero (not >0) guards on rain/BC/coupling-src;
        spill reads node_row_conc; dispersion share uses |mass|)
mod: src/engine/2d/SurfaceRouter2D.hpp/.cpp
       (row layout at initialize; INITIAL_STATE temperature seed; rain temp at RAINFALL source;
        rowIndex resolvers for [2D_INITIAL_QUALITY]/[2D_BOUNDARY_QUALITY]; publishNodeRows;
        applyAgingAndReactions (aging + reactArdStage per wet cell); queue split by row kind)
mod: src/engine/data/NodeData.hpp                   (+coupling_age_vol_queue/_inflow, coupling_temp_vol_queue/_inflow,
                                                      coupling_tuple_age/_temp flags; resize/reorder/erase/clear)
mod: src/engine/core/SWMMEngine.cpp                 (drain_queue lambda; age/temp queue drains; snapshot row names)
mod: src/engine/quality/QualityRouting.cpp          (addCouplingLoads delivers tuple age/temp halves; stand-in only when the row is absent)
mod: src/engine/data/HeatOverrideData.hpp           (+HeatElemKind::CELL2D, HeatElement::cell2d)
mod: include/openswmm/plugin_sdk/SimulationSnapshot.hpp (+surface_species_names)
mod: src/engine/2d/output/Default2DOutputPlugin.cpp (species_names from the surface's row names; layout attr)
mod: tests/unit/engine/CMakeLists.txt               (test_engine_2d_transport_s4)
```

---

## 1. ⚠ Nets first

- 1D corpus green: `addCouplingLoads` still gated on `coupling_inflow > 0`; the
  new NodeData vectors are sized (cheap) but only written by the 2D router.
- 2D census: 33 byte-identical; `twod_coupled_quality.inp` **should NOT move**
  this round (no `WATER_AGE`/`HEAT_TRANSPORT`, `Kdecay 0`, no reactions ⇒
  `ns == np`, the reaction stage early-returns on `!any_decay && n_msx == 0`,
  `publishNodeRows` writes the same pollutant values the S3 pairing read from
  `nodes.conc`). If it moves, the row-layout refactor changed a pollutant path
  — diff `publishNodeRows` against the old `ctx.nodes.conc[ni*ns+sp]` read.

Hydraulics-adjacent edits to eye: the three `evapSink(...)` expressions were
hoisted into a named `evap` (same operands, same order, `src` identical) so
the temperature sink could read it; no other hydraulic line moved.

## 2. What S4 does

**Row layout (verbatim from `ArdEngine::initialize`):** `[0,np)` pollutants,
`[np, np+nm)` MSX in `ReactionData` order (only when the reactions component is
active AND declares no WALL species — same fallback + warning as ARD),
`__WATER_AGE__`, `__TEMPERATURE__` **last**. `IGNORE_QUALITY` zeroes `np` and
`nm` only; age and temperature are their own options, as in 1D. `row_names`
is the single source for HDF5 `species_names`, `[2D_INITIAL_QUALITY]` and
`[2D_BOUNDARY_QUALITY]` name resolution (reserved names accepted when the
option is on).

**What a row MEANS is applied in three places, and nowhere else:**

| Row | Meaning | Where |
|---|---|---|
| age | `d(age)/dt = 1`: `cell_mass[age] += dt·V` per batch (age-volume, like the ARD stores) | `applyAgingAndReactions`, Lie-split after the advance |
| temperature | SIGNED; evaporation removes `evap·T` (water leaves at its own temperature — a solute concentrates, a temperature must not) | `sinkTemperatureWithEvap`, all three source sites in the marcher |
| pollutants (kdecay) + MSX | `reactArdStage` on a per-wet-cell concentration view; `cell_a = V·flow_2d_to_1d, cell_dx = 1` so `qual_routing_reacted` books in 1D mass units; `n_nodes = 0`; cell temperature row feeds `RxHydVar::TEMP` | `applyAgingAndReactions` |

Everything else (advection, dispersion, sinks at cell conc, sources) is
generic over rows — which is exactly why the row/marcher split was made in S1.

**Finding worth stating:** pre-S4, pollutants on the surface had **no kdecay
at all** (S1–S3 never called a reaction stage). Gate 6 pins the closed form.

### 2.1 The signed row — every guard that had to move

`sinkMassAtCellConc` took `min(dm, m)`, which on two negatives empties the
row; the signed row takes `dm` exactly. `addRainMass`, the `fireCells` rain
block, `addCouplingSourceMass`, the BC inflow block and the spill block all
tested `> 0` on a concentration/mass — now `!= 0`. The dispersion limiter's
β-share used the giver's mass as a magnitude — now `|·|`. The queue floor
(`row < 0 → 0`) skips the signed row. Gate 2 runs a −5/+5 °C pond through
evaporation and dispersion to catch any guard I missed: if a cold half
vanishes, one did.

### 2.2 Coupling tuple halves (D-2DT4 completed for age/temperature)

The router publishes every node's ROW values once per batch
(`node_row_conc_[node*ns+s]`: `nodes.conc`, `reactions.msx_node_conc`,
`water_age_state.node_age`, `heat_state.node_temp`) and points
`state_.node_row_conc` at it. The marcher's spill and the outfall injector
read that array — **not `nodes.conc`**, whose stride is `np` and would put
Cu's value into the temperature row the moment `ns > np` (gate 5 falsifies
exactly this). The S3 gross/net pairing reads the same array.

Queue routing by row kind: pollutant rows → `coupling_qual_queue` (np-strided,
S3); age row → `coupling_age_vol_queue[node]`; temperature row →
`coupling_temp_vol_queue[node]`; **MSX rows → nowhere** (see §5). All drained
by the one `drain_queue` rule in `assembleLateralInflows`. `addCouplingLoads`
adds the age/temp inflow to `node_age_vol_in`/`node_temp_vol_in` when the
surface carries the row (`coupling_tuple_age/_temp`), else the S3
EXTERNAL_INFLOW stand-in survives.

## 3. Validation protocol

1. Nets (§1). 2. Build; S1 6/6, S2 9/9, S3 4/4 unchanged. 3. `test_engine_2d_transport_s4`:

| # | Gate | Falsifies |
|---|---|---|
| 1 | Evaporating still pond: solute `c0·h0/(h0−ET)` exactly, temperature `T0` to 1e-9 | evaporation heating the cell (row untouched while V falls) |
| 2 | −5/+5 °C pond, dispersion + evaporation: Σ T·V invariant, bounds hold, cold half survives, no binds | any positivity guard on the signed row |
| 3 | Deck, still pan, `WATER_AGE`+`HEAT_TRANSPORT`: rows `[Cu, __WATER_AGE__, __TEMPERATURE__]`, age = 100 + 1200 s exactly, T = 12.5 | layout, aging, reserved-name seeding |
| 4 | Pan at 15 °C drains into empty J1 under pure heat transport: `node_temp[J1] == 15` to 1e-9, Cu still 4.0 | stand-in (reads 20), stride, queue rule |
| 5 | J1 fed at 4 mg/L spills onto a pan pre-seeded at the node's published T: both rows uniform to 1e-9 | spill reading `nodes.conc` mis-strided |
| 6 | `Kdecay 0.5/day`, still pan: `c = 4·e^{−kt}` to 1e-9, `qual_routing_reacted == removed·f` (1e-6), T untouched | no reaction stage / wrong units / decay leaking into T |
| 7 | Drain hydraulics bit-identical with and without age+heat rows | rows touching hydraulics |
| 8 | (S3 debt) write → reopen: `cell_mass`, `bc_conc`, `DISPERSION` identical; section names and `__TEMPERATURE__` present in the written text | InpWriter/sidecar round trip |

## 4. What I am least sure about

1. **Gate 4's premise** that a node fed only by the drain mixes to exactly the
   tuple's ratio under the LEGACY heat mirror: `node_temp = node_temp_vol_in /
   qual_vol_in` when `V_old = 0`. If HeatLegacy seeds the node at
   INITIAL_STATE and mixes with a nonzero initial volume, the reading is
   `(20·V0 + 15·V_in)/(V0+V_in)` — the gate then names the seam
   (`legacy_seeded`), not the tuple; tighten by draining longer, not by
   loosening.
2. **Gate 5's probe run** discovers the node temperature the 1D side publishes
   (EXTERNAL_INFLOW default 20 °C) rather than hard-coding it. If `[INFLOWS]`
   with no `__TEMPERATURE__` row leaves the node at some other value the gate
   still holds — it seeds the pan at whatever the probe read.
3. **Gate 6's ledger**: `reactArdStage` books removed mass as `cell_a·cell_dx·Δc`;
   I pass `cell_a = V_m3·f`, `cell_dx = 1`. If the 1D report scales
   `qual_routing_reacted` elsewhere, the 1e-6 band tells you the factor.
4. **Gate 3/6 stillness**: pan at z = −10 mapped to O1 (invert −0.5) so nothing
   crosses. If the outfall path withdraws or discharges anyway, `volume`
   changes and the "exactly" claims fail for the fixture's reason — pick an
   uncoupled map, not a looser tolerance.
5. **MSX rows are carried but untested this round.** No MSX deck in the S4
   gates (a `.rxn` fixture is a larger lift; the plan's acceptance is
   zero-flow batch parity with the 1D engines). Owed as gate 9 — see §5.

## 5. Owed, not in this round

- **MSX**: gate 9 (zero-flow batch parity vs the 1D reaction stage); MSX
  drain delivery to the node (the 1D node has **no MSX inflow accumulator for
  any loader** — a 1D seam: `MsxLegacyTransport` mixes link inflows only);
  WALL species on the surface (area/volume with a bound, plan §4).
- 2D→outfall withdrawal species delivery (S3 debt, still `lost_coupling` only).
- Temperature-row hotstart across a 2D surface.
- `[2D_BOUNDARY_QUALITY]` age/temperature defaults are 0 unless named
  (boundary inflow arrives "new" and at 0 °C); consider the EXTERNAL_INFLOW
  source table as the default.
- S5: per-cell heat fluxes (`relaxT`, `HeatElement::cell2d(i)`), `CELL2D`
  override scope + parser. S6: GPU.
