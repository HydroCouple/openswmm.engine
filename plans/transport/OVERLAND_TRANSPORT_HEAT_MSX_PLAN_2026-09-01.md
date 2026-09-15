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
