# Overland transport — species, heat and MSX on the 2D surface — Plan (2026-09-01)

**Phase 3 of the unified transport program, fleshed out.** Expands
`TWOD_TRANSPORT_PLAN.md` §3 and its S1–S5 phase outline into an
implementable design. Groundwater transport (that plan's §5 / S7) is
deliberately out of scope here and stays where it is.

**Scope:** species, water age, temperature and multi-species reactions on
the 2D overland surface, plus the 1D↔2D channel that carries them.

---

## 0. What exists, measured (2026-09-01)

`src/engine/2d/` is **hydrodynamics only**. The audit in
`TWOD_TRANSPORT_PLAN.md` §1 still holds, and re-reading the code confirms it
with four specifics that shape everything below.

**1. The integrated state is VOLUME, not depth.**
`SurfaceStateData::volume` is annotated *"the integrated state"*; `depth` and
`head` are reconstructed from it each step. **Species must therefore be
carried as MASS per cell**, not concentration — a concentration state would
need reconstruction against a volume that itself moves, and the two would
disagree at every wet/dry transition.

**2. The solver is tiered LTS.**
`tier_[i] = k` means cell `i` updates every `2^k` base substeps with
`Δt = 2^k·dt0`; **a face's tier is the minimum of its incident cells**, and
the header states that conservation across tier interfaces *"is exact by
construction"* for volume. Transport must inherit that property rather than
re-derive it. This is the single hardest thing in S1 and the existing plan
compresses it to the parenthetical "(LTS-consistent)".

**3. `edge_flux` is the conservative quantity.**
Per-edge normal flux, already accumulated by `fireFaces` into per-cell
accumulators that `fireCells` gathers. Species advection rides this, on the
same cadence, or it does not conserve.

**4. The 1D↔2D coupling carries volume only.**
`CouplingPoint` → `coupling_flux` → `coupling_queue` → `lat_flow`. There is
no species channel, so **any scalar is silently lost at every exchange** —
in both directions.

Also present and directly reusable: per-cell `rainfall`, `evap_rate`,
`infil_rate`, `net_source`, and a forcing family (`rainfall_forced` /
`_persist` / `_force_val`) whose OVERRIDE/ADD/RESET shape is the one PE4 just
adopted for per-element climate.

## 1. Decisions

**D-2DT1 — cell state is MASS per species, species-major.**
`std::vector<double> cell_mass` laid out `[s * n_cells + c]`, matching
`ArdEngine`'s `cell_phi` so the two engines' kernels read alike and a person
moving between them is not also switching layouts. Concentration is a
**derived, reported** quantity: `c = m / V` guarded by the same dry-volume
threshold the hydraulics uses.

Rationale beyond convenience: mass is what the flux form conserves, and the
wet/dry problem becomes trivial — a drying cell keeps its mass and its
concentration goes to the guard, rather than a concentration state having to
be extrapolated against a vanishing volume.

**D-2DT2 — advection fires with the faces, on the face's own tier.**
Species flux for edge `e` is `F_s(e) = edge_flux(e) · c_donor(e)`, computed
inside the SAME `fireFaces` pass that computes the volume flux, accumulated
into the same per-cell accumulators, and gathered by the same `fireCells`.

**This is the whole of D-2DT2 and it is not negotiable:** any arrangement
that computes species fluxes in a separate sweep must reproduce the tier
cadence independently, and the first time the two disagree — a cell re-tiered
mid-macro-cycle — mass is created or destroyed at a tier interface, silently,
in a way that a uniform-concentration test cannot see.

**The donor concentration must be read at the same substep the volume flux
was.** A donor read after the volume update is reading post-flux state and
the scheme is no longer conservative.

**D-2DT3 — the uniform-concentration property is the S1 acceptance test, not
a nice-to-have.** If every cell starts at concentration `C` and no source
adds species, every cell must remain at exactly `C` under *arbitrary* flow,
including across tier interfaces and through wet/dry transitions. This is the
one test that catches a tier-cadence mismatch, a FACE donor-timing error and
a volume/mass inconsistency at once. **It must hold to round-off, not to a
tolerance.** *(Corrected by the S1 check, per the handoff's own §6: a
donor-timing error in the SINKS is invisible to this property — a closed
dam-break has no sinks; falsifier i failed the ledger gate alone, exactly as
the handoff predicted. Sink timing has its own observer, the
infiltration/evaporation gate.)*

**D-2DT4 — the coupling carries a TUPLE, not a scalar.**
`coupling_flux` becomes accompanied by a species tuple in the same queue
entry. The direction matters:
- **2D → 1D:** the cell's concentration rides the volume into the node's
  `qual_mass_in` / `node_temp_vol_in` / `node_age_vol_in` accumulators — the
  same D-UT10 seam every 1D loader already uses.
- **1D → 2D:** the *node's* published concentration rides into the cell's
  mass. Under LEGACY that is `nodes.conc`; under ARD it is the node store;
  under LARD it is the mixed node value. **All three publish to
  `nodes.conc`**, so the coupling reads that and is engine-independent —
  which is the property that keeps this from becoming three implementations.

**D-2DT5 — a 2D cell is a new `HeatElemKind`, and PE's token was built for
this.** `HeatElement` already carries `{kind, index}` with `NODE/LINK/
SUBCATCH/LID`. Overland heat adds `CELL2D`, and `radiativeFor`/`sedimentFor`
gain one branch each. **Per-element shading on the 2D mesh is then free** —
which matters, because a floodplain under a canopy and one over asphalt is
exactly the case a 2D heat model exists to resolve.

Honest note: PE1 did **not** anticipate `CELL2D`; the enum lists LID, which
is a 1D concept. Adding an enumerator is one line, and the accessors' `index
< 0 ⇒ global` guard means an unresolved cell degrades safely.

**D-2DT6 — reactions reuse `reactSpeciesBlock` per cell, unchanged.**
The shared integrator already takes a species block, a pollutant context, an
HRT and a temperature. A 2D cell supplies all four. **No new integrator, no
new expression compiler, no new registry** — the 2D surface is a fourth
element geometry for machinery that already serves three.

**D-2DT7 — dispersion is isotropic in v1, and the plan says why.**
An anisotropic tensor on an unstructured TIN needs a defensible principal
direction per cell, and on overland flow that direction is the local velocity
— which is already the thing the scheme's numerical diffusion aligns with.
Isotropic first; revisit only with a field case that shows the difference.
(This carries forward `TWOD_TRANSPORT_PLAN.md` §8's open item unchanged,
with the reasoning made explicit.)

## 2. Formulation

Depth-averaged, per triangle `i`, in mass form:

\f[\frac{dm_{s,i}}{dt} = -\sum_{e \in \partial i} F_{s}(e)\,n_e
   + \nabla\!\cdot(V_i D \nabla c_{s,i})
   + V_i r_s(\mathbf{c}_i, T_i) + S_{s,i}\f]

with `m` cell species mass, `F_s(e) = Q(e)·c_donor` the advective flux on the
existing `edge_flux`, `D` the dispersion coefficient, `r_s` the reaction rate
from the shared integrator, and `S` the surface sources of §2.3.

### 2.1 Advection

First-order donor-cell upwind in S1 — monotone, LTS-consistent, and the only
scheme whose conservation across tier interfaces is as provable as the volume
scheme's. Second-order with a limiter (the `SCALAR_SCHEME`/`LIMITER` keys
Chapter 7 documents for 1D) arrives in S2, **behind the same key names**, so a
modeller carries one vocabulary across the two engines.

### 2.2 Wet/dry

A cell whose volume falls below the hydraulics' own dry threshold:
- **keeps its mass** (it did not go anywhere),
- **reports** its concentration through the guard rather than dividing,
- **takes no flux** — its faces carry no volume, so they carry no species.

Re-wetting mixes the arriving volume against the held mass, which is the same
CSTR statement the 1D node mixing makes.

### 2.3 Surface sources

| Source | Species rule |
|---|---|
| Rainfall | concentration from `[POLLUTANTS]` rain concentration; temperature from `HeatSource::RAINFALL`; age 0 |
| Evaporation | **removes volume, not mass** — the concentration rises. This is the up-concentration the 1D path gets wrong (KD1's open defect) and the 2D path should get right from the start |
| Infiltration | removes volume AND mass at the cell's concentration; the mass is handed to the GW plan when it exists, and **booked as a loss with a named ledger row** until then |
| Coupling | D-2DT4 |

**Evaporation is worth stating loudly:** on a shallow overland sheet under
sun, evapoconcentration is a first-order effect on concentration, and it is
the one source rule where "do what the 1D engine does" would carry a known
defect onto a new surface.

## 3. Heat on the surface

Per wet cell, reusing the flux modules unchanged:

```cpp
const HeatElement e{HeatElemKind::CELL2D, i};
const double j0 = heat::netFluxOut(ctx, e, t_cell);
const double j1 = heat::netFluxOut(ctx, e, t_cell + heat::kProbeC);
t_cell += heat::relaxT(j0, j1, heat::kProbeC, area_m2, vol_m3, dt, rho, cp);
```

Three properties come free and are worth naming because each was earned in a
previous phase:

- **`relaxT`, not forward Euler.** A shallow overland sheet has almost no
  thermal mass per unit exchanging area — the exact regime that produced
  H5a's `5 → 182 → −1.8e4 → NaN` divergence and forced D-H5d. A 2D surface is
  that regime everywhere, so an explicit step here would be worse than it was
  in the LID column.
- **One `netFluxOut`, all families summed, one relaxation** (D-H5e). Cells
  must not relax per module.
- **Per-cell shading** via D-2DT5, at no additional cost.

**The bed analogue is deliberately deferred.** `TWOD_TRANSPORT_PLAN.md` §8
lists "sediment-bed heat layer per cell — decide at S5 review". With H6b
landed, that decision now has a concrete shape: `BedZoneState` would gain a
`cell2d_temp` array and `bedCouplingFromContact` would take the cell's wetted
area. **It is one round and it should NOT be in S5** — overland sheets are
thin and transient, and their bed coupling matters most exactly where the
sheet is thinnest and the surface flux is already dominant. Do it after S5
measures whether it matters.

## 4. MSX on the surface

`reactArdStage`'s per-cell loop is the template. Per cell: gather the species
column, supply the pollutant context and the cell temperature (NaN when heat
is off, which the integrator already reads as "use the `[REACTION_OPTIONS]`
constant"), integrate, scatter back.

**Two things the 1D engines learned that apply unchanged:**
- **Pollutant `Kdecay` is NOT re-applied in the reaction stage** — the decay
  stage owns it. The L3 record names this trap; the 2D binding must not
  reintroduce it.
- **Scope is PIPE, not TANK.** An overland cell is a flowing element with a
  wall, not a mixed reservoir. `WALL` species use the cell's own
  surface-area-to-volume ratio, which on a triangle is `area/volume` and is
  therefore *very* large on a thin sheet — worth a sanity bound and a warning
  rather than an unbounded `AR`.

## 5. Phases, with acceptance criteria

```
S1  SurfaceTransportState (mass, species-major) + donor-cell advection
    INSIDE fireFaces/fireCells + wet/dry + HDF5 species vars.
    ACCEPT: (a) D-2DT3's uniform-concentration property to ROUND-OFF,
            on a mesh with >= 3 LTS tiers and a wet/dry front;
            (b) rotating-slope dam-break tracer mass conserved to 1e-12;
            (c) discrete max principle — no cell exceeds the initial
            max or falls below the initial min.
    FALSIFIER: force every cell to tier 0. (a) must still pass — if it
            only passes at uniform tier, the tier handling is untested.

S2  Dispersion (isotropic) + limited second-order + boundary species +
    rainfall/evap/infil rules of §2.3.
    ACCEPT: 2D point release vs the analytical Gaussian on a flat plane
            under uniform flow; Peclet sweep; evapoconcentration on a
            closed pond raises C and conserves mass EXACTLY.

S3  1D<->2D species/age/enthalpy tuple (D-2DT4), both directions.
    ACCEPT: 1D -> 2D -> 1D round-trip conservation on the weir/road
            fixtures; age continuity across the coupling (no reset, no
            jump); the SAME deck under all three 1D engines agrees,
            which is what proves the coupling reads `nodes.conc` rather
            than an engine-specific store.

S4  Water age + temperature rows + MSX per cell (§4).
    ACCEPT: batch-reactor parity vs the 1D engines at ZERO flow (same
            reactions, same dt -> same answer, since the integrator is
            literally the same code); age == travel time on a tilted
            plane at steady state.

S5  Per-cell surface heat fluxes (§3) + per-cell shading via D-2DT5.
    ACCEPT: pond diurnal cycle vs a CSH-style column; energy closure;
            a shaded half-mesh and an exposed half-mesh diverge, which
            is PE2's gate one geometry over.

S6  GPU transport kernels behind the extended plugin ABI.
    ACCEPT: CPU/GPU parity per the 2D_GPU verification pattern.
```

## 6. ⚠ The corpus problem, and it blocks S1

`UNIFIED_PLAN_STATUS` §8 records it and the closeout round's two new decks did
not fix it: **the bit-identity corpus contains 0 2D decks** (23 decks, zero
mesh). A separate script — `tests/scripts/trackI_bitwise_regression.sh`,
32 decks — covers 2D and is **not wired into `run_corpus.sh`**.

**Wire it before S1 opens.** Otherwise the first 2D transport round is also
the round that discovers its own regression net does not run, and every
"corpus green" claim in this phase means "the 1D corpus is green", which says
nothing about the surface solver.

This is the cheapest item in this plan and the one with the worst
consequences if skipped.

## 7. Open items, honestly

- **Anisotropic dispersion** (D-2DT7) — deferred with reasoning.
- **Cell-resolved bed heat** (§3) — deferred to *after* S5, with a shape.
- **Species boundary conditions on 2D edges.** `[2D_BOUNDARY_CONDITIONS]`
  carries TYPE + params today; a species column is additive but the *inflow*
  concentration at an open boundary needs a default, and "zero" is a real
  statement rather than an absence. Decide at S2, do not default silently.
- **The GW hand-off.** §2.3's infiltration row books mass as a named loss
  until the two-zone kernel exists. **That row must be a real ledger line,
  not an omission** — the ARD engine's `[TRANSPORT_SOURCES]` mass currently
  lands in the continuity *error* for want of a row (Chapter 7 §7.6), and
  repeating that on a new surface would be doing it knowingly.
- **Reporting.** Species on a TIN is an HDF5 variable per species; the
  `.out` format has no 2D block and should not grow one. The GUI's G6g row
  covers rendering and is gated on this phase.

  **Update (2026-09-01, HydroCouple 2.0):** there is a second, native
  answer. `hydrocouplespatiotemporal.h` defines
  `ITimeSeriesTINComponentDataItem` — a time series over a triangulated
  irregular network, which is exactly this mesh. The same per-cell arrays
  therefore serve both an HDF5 variable and a coupling exchange item, and a
  coupled atmospheric or shade model can drive per-cell forcing through
  D-2DT5's `CELL2D` token the way PE4 already drives 1D elements.
  **This changes no sequencing here** — S1–S5 build the transport; the TIN
  item is a wrapper over the result and belongs to the process-component
  plan's catalogue (§8 there), after the surface state exists.

---

## 8. Amendment 2026-09-19 — cell coverages, buildup, washoff and sweeping (phase S7)

**Owner direction (2026-09-19):** the 2D overland surface adopts the
subcatchment **coverage / land-use convention** so that buildup and washoff
relations can be configured per cell. Land uses are not redefined; a cell
only says which existing land uses cover it. This section is the
authoritative design; the program plan
(`openswmm.gui/workplans/GW_TRANSPORT_HYDROCOUPLE_COMPONENT_PROGRAM_PLAN_2026-09-05.md`)
carries the schedule row (S7) and decisions D-A22–D-A26.

### 8.1 What exists that this reuses (verified 2026-09-19)

| Piece | Where | Reused as |
|---|---|---|
| `[LANDUSES]` (sweep interval / removal / last swept), `[BUILDUP]` and `[WASHOFF]` per (land use × pollutant), `[COVERAGES]`, `[LOADINGS]`, `[OPTIONS] DRY_DAYS` | `QualityData.hpp:45-70`, `Landuse.hpp:80-96`, `QualityHandler.cpp`, `SWMMEngine.cpp:8123-8143` | the **only** definition of land uses, buildup and washoff relations — unchanged |
| Subcatchment surface-quality step: buildup accrual (POW / EXP / SAT / EXTERNAL), washoff EXP / RC / EMC, co-pollutants, BMP removal, sweeping, unit seam `mcf_p` | `SWMMEngine::stepSurfaceQuality` (`:2227`, `:2650-2760`), `LanduseSolver`, `applyCoPollutant` | the per-cell step is the same arithmetic over a cell-major SoA (§8.4) |
| Per-cell attribute ladder GLOBAL < TAG < CELL (D-PE3) and the `[2D_INFILTRATION*]` parsers / writer / GeoPackage round trip | `Infil2D`, `SectionHandlers2D.cpp` | template for `[2D_COVERAGES]` / `[2D_LOADINGS]` |
| Cell → containing subcatchment (`cell_subcatch_`, Track I-b) | `SurfaceRouter2D.hpp:310, :449-452` | the double-counting guard (§8.5) |
| Species mass rows on the marcher, `gained_*` / `lost_*` ledgers, the S1 conservation identity `totalIncludingLedgers` | `SurfaceTransportState.hpp:105-220`, `ExplicitInertialSolver.cpp:317-372` | washoff is one more **source** row; buildup is a separate store |
| Rain concentration source per cell | `SurfaceRouter2D.cpp:786-791` | precedent for “surface source at the runoff-step cadence” |

Nothing about buildup / washoff exists in `src/engine/2d/` today (zero hits).

### 8.2 Input convention (additive; `[2D_*]` family in the `.inp` for v1, re-homed to `model.i2d` with D-I5)

```
[2D_COVERAGES]
;; scope            LANDUSE   PERCENT   [LANDUSE PERCENT] ...   ; one row = the whole coverage set of that scope
*                   Residential 60   Commercial 25
TAG  street         Street     100
CELL 812            Park       100

[2D_LOADINGS]
;; scope            POLLUTANT  INITIAL_BUILDUP      ; mass per unit area (project units: lbs/ac or kg/ha), like [LOADINGS]
*                   TSS        0
CELL 812            TSS        12.5

[2D_CURB_LENGTH]                                     ; only if a covering land use has a PER_CURB normalizer
;; scope            LENGTH     ; project length units per cell
TAG  street         30
```

Rules:
1. **Scope keys** are exactly those of `[2D_INFILTRATION]` / `[2D_INITIAL_QUALITY]`: `*`, `TAG name`, `CELL n` (same index base as every other cell-addressed 2D section — the tri-quad rule, triangles first). Resolution GLOBAL < TAG < CELL; a row **replaces** the coverage set for its scope (no merging), exactly as a subcatchment's `[COVERAGES]` line does.
2. Percentages per scope sum to ≤ 100; the remainder carries no land use (no buildup, no washoff) — legacy semantics.
3. Land-use and species names resolve against `[LANDUSES]` / the species registry; unknown names are open-time errors. **Surface species = pollutants ∪ MSX species** (owner direction 2026-09-19, D-A24 reversed): `[BUILDUP]`, `[WASHOFF]`, `[LOADINGS]` and `[2D_LOADINGS]` accept an MSX species name exactly as `[INITIAL_QUALITY]` / `[INFLOWS]` do (`kKindMsxFirst − m` encoding). See §8.9 for the 1D-wide part this needs.
4. Initial buildup: `[2D_LOADINGS]` row if present, else the land use's buildup function evaluated at `[OPTIONS] DRY_DAYS` — the subcatchment rule (`SWMMEngine.cpp:8123-8143`).
5. `PER_CURB` land uses need `[2D_CURB_LENGTH]` for every scope they cover; a covered cell without a curb length is an open-time error (D-A25 — refuse rather than silently zero).
6. `[2D_OPTIONS] TRANSPORT_POLLUTANTS NO` or `IGNORE_QUALITY` makes the rows inert with the same one-line warning `[GW_*]` uses.
7. Writer emits the sections only when rows exist (bit-identical decks otherwise); GeoPackage tables `mesh_2d_coverages`, `mesh_2d_loadings`, `mesh_2d_curb_length`.

### 8.3 Formulation

Per cell `c`, land use `l`, pollutant `p`, with coverage fraction `f_{c,l}`, cell area `A_c` and normalizer `N_{c,l} = f_{c,l}·A_c` (per-area) or `f_{c,l}·L_curb,c` (per-curb):

- **Buildup** `B_{c,l,p}` (mass per normalizer unit) accrues with the land use's function exactly as on a subcatchment: POW `min(C1·t^C2, C0)`, EXP `C0(1−e^{−C1 t})`, SAT `C0 t/(C2+t)`, EXTERNAL from the time series — through the inverse-days form the subcatchment step uses so the two cannot drift. Accrual and sweeping run at the **runoff-step cadence** (`dt_runoff`), immediately after `stepSurfaceQuality` in A4c, not per marcher substep.
- **Runoff rate of a cell** (the `q` of the washoff laws) — **D-A22 as revised 2026-09-19 (implemented)**: `q_c = max(0, V_out,c − V_in,c) / (A_c·Δt_runoff)`, the cell's **net** outflow over the runoff step: what left across its faces, open boundary edges and drains to nodes, minus what arrived across faces, from node spills, outfall discharge, prescribed inflow and inflow boundaries — accumulated by the marcher in `SurfaceTransportState::cell_runoff_vol` at every site the species ledgers already book (so the water side and the runoff side cannot disagree). Rainfall and groundwater return are generation, not inflow, and are not subtracted. By continuity the positive part is `rain excess + storage release`: the runoff the cell **itself** produced — the cell analogue of the subcatchment step's `q = outflow / area`, zero on a still pond, zero on a cell that only passes upstream water through, and **independent of how finely a plane is meshed**. The gross outward face flux that §8.3 first proposed was implemented and rejected on the two-triangle pan gate: it counts the same water once per cell it crosses, so on a plane of `n` cells in series the EMC and RC loads scale with `n` and the EXP rate at the downslope cell is `n×` the areal rate — a mesh-dependent formulation. Rainfall excess alone (the other alternative) was rejected because it stops washing during the recession, which SWMM's `q` (outflow/area) does not.
- **Delivery and lag**: the washoff of runoff step `n` is computed from the runoff the marcher accumulated **during** step `n` and added to the cell rows at the end of that step; it leaves with the water of the following steps at the cell's concentration through the existing face / drain / boundary bookings. The delivery is therefore one runoff step behind the water that produced it (SWMM's own `addWetWeatherLoads` trapezoid is comparable), and the washoff of the last step of a hydrograph stays on the (drying) cell until the next event, as the ponded-water mass does on a subcatchment. The gates account for it: the EMC pan drains over several runoff steps and the identity is checked on `runoff + the marcher's pending accumulation`.
- **Washoff** per (l, p): EXP `W = C1·q_c^{C2}·B_{c,l,p}·N_{c,l}`, RC `W = C1·(q_c A_c)^{C2}`, EMC `W = C_emc·q_c·A_c`, with the subcatchment step's unit seam (`mcf_p`, `MIN_RUNOFF_RATE`), buildup limiting and BMP removal applied per land use, then summed over land uses (co-pollutant fractions are a 1D-only mechanism, as D-A28 says for MSX; not mirrored on cells). The mass `W·dt_runoff` is **added to the cell's species row** (`cell_mass[p][c]`) and booked to a new ledger `gained_washoff[p]`; every law reduces `B` (parity with the subcatchment step). The cell's mass then advects with the following steps' water, so the load reaches the 1D node through the existing drain / spill tuple — no new coupling row (see *Delivery and lag*).
- **Sweeping**: per land use interval / removal with `last_swept_{c,l}` state, same date arithmetic as subcatchments; removal booked to `lost_sweeping[p]`.
- **Ledger identity**: `total_mass + lost_* − gained_washoff … ` extends the S1 identity; buildup has its own row pair (`buildup_added`, `buildup_washed`, `buildup_swept`) in `MassBalance2D` and the `.rpt` Washoff Summary gains a “2D surface” block.
- Runs on the CPU marcher only (transport rows); the Kokkos plugin keeps refusing / warning per S4b.

### 8.4 Data layout, API, outputs, state

```
SurfaceQuality2D (src/engine/2d/quality/, owned by SurfaceRouter2D; active iff coverages resolved && pollutant/MSX rows exist)   [landed 2026-09-19]
  coverage   [c * n_lu + l]                 fraction 0..1 (resolved from the ladder)
  curb_len   [c]                            project length units (needed only by PER_CURB land uses)
  buildup    [(l * n_sp + s) * n_cells + c] user mass per normalizer unit, s over pollutants then MSX species — cell-major inner index
  last_swept [c * n_lu + l]                 days
  runoff_vol [c]                            m³, cumulative (read-back of what the laws saw)
  SurfaceTransportState::cell_runoff_vol [c]   signed net outflow since the last runoff step (marcher-accumulated)
ledgers (user mass, per species): init_buildup, buildup, washoff, sweeping, bmp; SurfaceTransportState::gained_washoff[row] (row units)
```

C API (`openswmm_sq2d.h`, landed): `swmm_2d_coverage_count / set / row_size / get / remove`, `swmm_2d_loading_count / set / get / remove`, `swmm_2d_curb_length_count / set / get / remove`, `swmm_2d_get_buildup_bulk(species, double* per_cell, n)`. Row edits share the file's parsers (one grammar) and validate names at once. Not landed: `swmm_2d_get_washoff_rate_bulk`, a ledger entry in `swmm_2d_get_mass_balance`, Python / MCP mirrors — follow-ups.

Outputs (landed): `REPORT_2D_VARIABLES` token `BUILDUP` (in DEFAULT) → `Mesh2_face_buildup [t, species, face]` (user mass per acre | hectare, `units`, `species_names` attributes; the pollutant and MSX rows); `.rpt` **2D Surface Washoff Summary** (per species: initial buildup, net buildup, washed off, swept, BMP removed, on mesh). Not landed: `Mesh2_face_washoff`, the transport-matrix note, hotstart carry of `buildup` + `last_swept` (next record bump, with T7.5's V6).

Exchange items (standalone `integrated2d`): `cell_buildup[p]` and `cell_washoff[p]` as polyhedral-surface items (D-A14), read-only.

### 8.5 Ownership — no double counting

A subcatchment whose footprint is meshed **and** rained on (`RAINFALL_MODE ≠ NONE`) already sends its own runoff and washoff to its outlet; cells under it that also carry coverages would load the same storm twice. Rule (D-A23):
- `[2D_COVERAGES]` is **inert with a warning** when `RAINFALL_MODE NONE` (no rain on the mesh ⇒ the subcatchments own the surface).
- Otherwise, at open, every covered cell whose containing subcatchment (`cell_subcatch_`) itself has `[COVERAGES]` is listed in **one** warning naming the subcatchments; the run proceeds (the modeller may intend a hybrid). A `[2D_OPTIONS] SURFACE_QUALITY_OWNER CELL | SUBCATCH` switch is **not** added in v1 — the warning plus the two enables (`TRANSPORT_POLLUTANTS`, `IGNORE_QUALITY`) are enough to express either choice.

### 8.6 GUI (`openswmm.gui`, MVC per CLAUDE.md §5.1)

- **Land uses**: the existing `LandUseRegistry` / `LandUseEditorDialog` — unchanged; nothing is duplicated.
- **Cell coverages model**: `Mesh2DCoverageModel` (QObject over the C API, per project window) feeding three views: (1) `MeshAttributeTableModel` (Kind::Cell) gains one percent column per land use generated from the registry plus `Curb length`; bulk edit via the existing multi-select apply; (2) `MeshCellPropertyAdapter` shows the same set for the selection; (3) the Mesh generation dialog's region-defaults table (GG0 §3.3 pattern) seeds a fresh mesh.
- **Assignment from GIS** (`MeshAttributeAssignDialog`): a new **Coverage** mode with two sub-modes — *Classified*: a land-use raster / vector class field → a land use at 100 % (or a class → coverage-set lookup table, the `ClassifiedInfil` pattern); *Zonal share*: a polygon layer with a land-use field → percent by area overlap per cell (the polygon-in-cell overlay the mesh generator already computes for regions). This is the primary way a real model gets its coverages.
- **Loadings**: `[2D_LOADINGS]` rows in the Initial Quality dialog's 2D tab beside `[2D_INITIAL_QUALITY]` (gating plan §6.2).
- **Results**: `SWMM2DResultsLayer` attribute *Buildup — <pollutant>* from `Mesh2_face_buildup`; washoff series in the comparison plot; ledger rows in the Simulation Status tree.
- Round-trip tests: no-edit `n == 0`; coverage row → engine → file → reopen; zonal-share assignment on a 2-polygon fixture gives the expected percents on tri and quad cells.

### 8.7 Gates (S7)

1. **Buildup parity**: one covered cell with no flow reproduces the subcatchment buildup curve for the same land use, `DRY_DAYS`, and sweeping schedule **bit-for-bit** (same arithmetic path).
2. **EMC exactness**: on a meshed plane draining to one node, cumulative washoff load = `C_emc × cumulative runoff volume`, and the runoff volume (Σ_c net outflow, plus the marcher's not-yet-stepped accumulation) = the volume that drained — both to 1e-10, on a two-triangle pan and on a one-quad pan (the net definition is what makes the two agree).
3. **Meshed vs lumped**: a rectangular meshed plane with uniform coverage vs an equivalent subcatchment (same area, slope, n, land use, storm): cumulative EXP washoff within a **recorded** band (the hydrographs differ by construction — the band is written down with its reason, not tuned).
4. **Ledger closure**: buildup rows + washoff + sweeping + BMP + the S1 identity close to 1e-10 on a mixed deck with co-pollutants.
5. **Ownership warning** fires on a deck whose covered cells lie in a covered subcatchment; is silent otherwise; `RAINFALL_MODE NONE` makes the rows inert with one warning.
6. **Tri / quad / mixed** (§B.4a of the program plan) for gates 1–4; all-triangle decks without rows bit-identical.
7. GUI round trips (§8.6).

Status 2026-09-19 (engine, sandbox): gates 1, 2, 4, 5, 6 pass in `tests/unit/engine/test_2d_surface_quality.cpp` (7 tests: buildup parity bit-for-bit incl. an MSX species, EMC exactness on tri and quad pans, ledger closure with sweeping and BMP, ownership warnings, round trip and grammar refusals, inert rows bit-identical, C-API grammar) plus the `.h5` / C-API end-to-end gate in `test_2d_output_options.cpp`. Gate 3 (meshed vs lumped band) and the mixed tri/quad deck of gate 6 are left for the validator's round (see the S7 handoff); gate 7 is the GUI's.

### 8.8 Decisions requested

| # | Decision | Recommendation |
|---|---|---|
| D-A22 | Cell runoff rate for the washoff laws | **Revised 2026-09-19 (implemented, S7): the cell's NET outflow per unit area** (outflows across faces / boundaries / drains minus inflows across faces / spills / discharge / inflow boundaries, clamped at 0) — rain excess plus storage release, mesh-independent. The gross outward face flux first recommended was implemented, measured on the two-triangle pan (2.11 m³ counted for 1.41 m³ drained) and withdrawn: it scales EMC / RC loads and the EXP rate with the number of cells in series. Owner sign-off requested on the revision. |
| D-A23 | Double counting: warn + rely on enables (recommended) vs a `SURFACE_QUALITY_OWNER` option | Warn; add the option only if a real deck needs a hybrid the enables cannot express. |
| D-A24 | Species scope | **Decided 2026-09-19: pollutants ∪ MSX species**, on subcatchments and cells alike (§8.9). |
| D-A25 | `PER_CURB` normalizer on cells: require `[2D_CURB_LENGTH]` (recommended) vs refuse PER_CURB land uses on the mesh | Require the section; streets are exactly where quad patches and curb loads meet. |
| D-A26 | Cadence: buildup / sweeping / washoff at the runoff step with marcher-accumulated `q_c` (recommended) vs per marcher substep | Runoff step — same calendar as subcatchments, negligible cost, mass added as a source is conservative either way. Implemented; the one-step delivery lag is recorded under §8.3 *Delivery and lag*. |

Sequencing: S7 needs S1–S2 (rows, sources, ledgers — landed) and the tri-quad
layout (landed); independent of S5 / S6. GUI zonal-share assignment can start
against the C API stubs.

### 8.9 MSX species build up and wash off too (owner direction 2026-09-19; phase BW-MSX, 1D + 2D)

Buildup / washoff are keyed by pollutant everywhere today (`SurfaceQualitySoA` is
`[subcatch × landuse × pollutant]`, `addWetWeatherLoads` writes
`nodes.qual_mass_in[p]`). MSX species already have every *other* authoring seam
(`[INITIAL_QUALITY]`, `[INFLOWS]` CONCEN / MASS, `[DWF]`, hotstart species block
— engine `2ea8ebe5`) and a delivery seam for external loads:
`ReactionData::msx_ext_mass_in[node × species]`, a mass rate in the
`qual_mass_in` shape consumed by the ARD engine (`ArdEngine.cpp:770, :836`),
the legacy MSX dispatch (`MsxLegacyTransport.cpp:111-117`) and LARD. The
extension is therefore a *widening of the surface store and its grammar*, not
new physics:

1. **Grammar.** `[BUILDUP]` / `[WASHOFF]` rows: `LANDUSE SPECIES TYPE …` where
   SPECIES resolves through the registry (pollutant or MSX species, kind badge
   in errors). `[LOADINGS]` and `[2D_LOADINGS]` likewise. `[POLLUTANTS]`
   co-pollutant fractions stay pollutant-only in v1 (D-A28); an MSX species
   washes off by its own relation.
2. **Store.** `SurfaceQualitySoA` and the S7 `SurfaceQuality2D` index by
   *surface species* `s ∈ [0, np + nm)` (pollutants first, then MSX species in
   `ReactionData` order — the same row order `TransportPolicy` uses), so
   `buildup_params / washoff_params` are `[landuse × surface species]`;
   `last_swept` unchanged (per land use).
3. **Step.** `stepSurfaceQuality` and the S7 cell step loop over surface
   species with one arithmetic; the only species-dependent input is the unit
   factor `mcf_s`: pollutants as today; MSX species from `species_units[m]`
   — `MG` → `UCF(MASS)`, `UG` → `/1000`, anything else (`MMOL`, counts,
   dimensionless) → `1.0` (D-A27), mirroring the pollutant switch.
4. **Delivery.** In `addWetWeatherLoads`, pollutant loads go to
   `qual_mass_in[p]` as today; MSX loads go to a new
   `ReactionData::msx_wet_weather_mass_in[node × species]` that the three
   consumers add to `msx_ext_mass_in` at the point they read it — separate
   accumulator so the ledger's *wet-weather* row stays distinguishable from
   *external inflow*. On the 2D surface (S7) the washoff mass enters the
   cell's MSX row directly, as for pollutants.
5. **Sweeping, BMP removal, EXTERNAL buildup series**: unchanged, per surface
   species.
6. **No kinetics in the dry store** in v1: a built-up MSX species is inert
   until it is washed into water, where it reacts under the normal scopes.
   `[REACTION_SURFACE_STORE]` (e.g. die-off of built-up indicator bacteria) is
   a named later slice (D-A29).
7. **Ledgers / reports.** `qual_surface_buildup`, washoff and sweeping rows
   sized `np + nm`; the `.rpt` Washoff Summary and the transport matrix list
   MSX species; hotstart: buildup block keyed by species *name* (the V4
   pattern), so decks that add or reorder MSX species still restore.
8. **GUI.** `LandUseEditorDialog` Buildup / Washoff tables track the **species
   registry** (pollutants + MSX with kind badges) instead of
   `PollutantRegistry` alone (`landuseeditordialog.cpp:64-68`); the Loadings
   editor and the S7 coverage / loadings surfaces likewise. Species units are
   shown from the registry so a buildup value reads “kg/ha of <species
   units>”.
9. **Gates.** (a) An MSX species given the same buildup / washoff parameters
   as a pollutant produces **identical** surface loads (bit-for-bit on the
   surface step) and the same cumulative node load within the delivery-seam
   tolerance under all three quality solvers; (b) ledger closure with mixed
   pollutant + MSX land uses to 1e-10; (c) decks without MSX rows bit-identical;
   (d) round trips incl. GeoPackage and hotstart by name; (e) GUI: an MSX
   species appears in the land-use editor tables and round-trips.

Sequencing: BW-MSX(1D) is independent of S7 and small; do it **before** S7 so
the cell store is born species-row keyed rather than widened later.

| # | Decision | Recommendation |
|---|---|---|
| D-A27 | Unit factor for MSX buildup mass | from `species_units`: MG / UG / other → `UCF(MASS)` / `÷1000` / `1.0`. |
| D-A28 | Co-pollutant fractions for MSX species | not in v1 (`[POLLUTANTS]` grammar); revisit if a deck needs it. |
| D-A29 | Reactions inside the built-up store | deferred; named slice `[REACTION_SURFACE_STORE]`. |

