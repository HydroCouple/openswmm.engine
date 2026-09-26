# S2 + S1 debts — source concentrations, dispersion, HDF5, initial quality — Handoff (2026-09-01)

**For:** the checking agent.
**Plan:** `OVERLAND_TRANSPORT_HEAT_MSX_PLAN_2026-09-01.md` §2 (S2) plus the
two debts `S1_OVERLAND_TRANSPORT_HANDOFF` §7 recorded (HDF5 species
variables, `[2D_INITIAL_QUALITY]`). S3–S6 are NOT here.
**Implemented syntax-only.** `g++ -fsyntax-only -std=c++20 -fopenmp
-DOPENSWMM_HAS_2D` on every touched translation unit (plus a stub `hdf5.h`
for the output plugin): **0 errors**. Nothing built or run.
**Base:** `4ad689b6` (S1 as validated). The working tree also carries
modifications that are NOT this round's (`openswmm_edit.h`, `NameIndex.hpp`,
`ObjectDeleter.*`, `openswmm_edit_impl.cpp`, regenerated `.gpkg`/`.out`/`.rpt`
fixtures under `tests/`). **Stage only the files below, by patch.**

```
new: tests/unit/engine/test_2d_transport_s2.cpp                (8 gates)
new: plans/transport/S2_OVERLAND_TRANSPORT_HANDOFF_2026-09-01.md
mod: src/engine/2d/data/SurfaceTransportState.hpp   (+rain_conc, bc_conc, bc_quality_rows,
                                                      dispersion_limiter_binds, gained_* ledgers;
                                                      totalIncludingLedgers subtracts gains)
mod: src/engine/2d/data/SolverOptions2D.hpp         (+dispersion, m²/s, default 0)
mod: src/engine/2d/data/PendingRows2D.hpp           (+PendingInitialQualityRow, PendingBoundaryQualityRow)
mod: src/engine/2d/solver/ExplicitInertialSolver.hpp (+addRainMass)
mod: src/engine/2d/solver/ExplicitInertialSolver.cpp (fireFaces dispersion block; fireCells rain;
                                                      BC inflow species; both lazy paths rain;
                                                      initialize resolves bc_quality_rows → bc_conc)
mod: src/engine/2d/SurfaceRouter2D.hpp/.cpp         (pending rows accessors; rain_conc from
                                                      c_rain; [2D_INITIAL_QUALITY] resolver;
                                                      [2D_BOUNDARY_QUALITY] species/tri resolve)
mod: src/engine/2d/input/SectionHandlers2D.hpp/.cpp (parse2DInitialQualityLine,
                                                      parse2DBoundaryQualityLine, registration,
                                                      [2D_OPTIONS] DISPERSION)
mod: src/engine/2d/output/Default2DOutputPlugin.hpp/.cpp (Mesh2_face_species_conc [nTime,nSpecies,nFace])
mod: include/openswmm/plugin_sdk/SimulationSnapshot.hpp (+surface_species_conc, surface_species_count)
mod: src/engine/core/SWMMEngine.cpp                 (register2DSections arg; fillSurfaceSnapshot species)
mod: tests/unit/engine/CMakeLists.txt               (test_engine_2d_transport_s2)
```

---

## 1. ⚠ 2D bitwise net first, as in S1

`run_corpus.sh base patched` must report the 2D section 32/32 identical. No
tracked 2D deck carries `[POLLUTANTS]`, so `sacc_L_` is empty and every S2
branch is dead on them: rain block (`!sacc_L_.empty()` via `addRainMass`'s
`tr.active()`), BC block (`!sacc_L_.empty()`), dispersion block
(`!sacc_L_.empty() && opts_->dispersion > 0.0`). A movement is a
hydrodynamics regression from this round and outranks every gate.

The one hydrodynamic-adjacent edit to eye: the BC block's `v_new < v_old`
outflow branch was restructured into an `if/else` inside `if
(!sacc_L_.empty())`. `state_->volume[i] = v_new` is untouched and outside
it. Diff it.

## 2. What S2 adds, and the one design point

| Term | Where it is booked | Ledger |
|---|---|---|
| Rain at `[POLLUTANTS]` rain conc | `fireCells` species loop (`rain_m3 * rain_conc[s]`), both lazy paths via `addRainMass` | `gained_rainfall[s]` |
| BC **inflow** at `[2D_BOUNDARY_QUALITY]` conc | BC block, `(v_new − v_old) * bc_conc[k*ns+s]` | `gained_boundary[s]` |
| BC outflow | unchanged — at cell conc (S1) | `lost_boundary` |
| Isotropic dispersion `D` | `fireFaces`, **after** the advective species booking, into `sacc_L_/sacc_R_` on the face's tier | none (conservative exchange) |

**D-2DT7 — dispersion rides the same face, cadence and accumulators as
advection.** The face already has `hf`, `ξ`, `inv_dx_normal`, `dt_f` and its
tier; the exchange is `D·hf·ξ·(c_a − c_b)·inv_dx·dt_f` booked into the same
`sacc_*` slot, so `settleAccumulators` and the LTS tier bookkeeping see one
species flux per face, not two. Any other placement (a separate diffusion
pass, a cell-centred Laplacian) would need its own tier-consistency proof;
this placement inherits S1's.

**The limiter.** Explicit diffusion is stable only for `D·dt/d² ≲ ½` and
`dt0` is set by gravity waves, not by `D`. Instead of coupling `dt0` to `D`
(a global cost for a local term), each face's exchange is capped at
`min(|equalisation|, β/refire · M_giver)` where equalisation
`= (c_a−c_b)·V_aV_b/(V_a+V_b)` is the transfer that makes the two
concentrations equal. Neither cell can cross the other or go negative, so the
discrete max principle holds for any `D`. A bind is **counted** in
`dispersion_limiter_binds`, because a binding face means the dispersion is
under-resolved at this dt — a modelling signal the user should see, not a
silent clamp. **Nothing reports the counter yet** (see §6).

**Still-pond scope.** Cells below `h_move` are on the lazy source-only path
and their faces never fire, so a thin film does not disperse. This is the
marcher's own activity rule applied to species; recorded, not changed.

### 2.1 `totalIncludingLedgers` now subtracts gains

It reads *"what was there at t=0"*: surface + lost − gained. Every S1 gate
still holds under that definition (no sources ⇒ gains are 0). The S2 gates
assert its invariance with sources on.

### 2.2 `[2D_BOUNDARY_QUALITY]` — `TRI EDGE SPECIES CONC`

Same 0-based `TRI` and `0..2 EDGE` spelling as `[2D_BOUNDARY_CONDITIONS]`.
The router resolves species by name and range-checks `TRI`; the **solver**
resolves the edge slot against its own non-WALL boundary list at
`initialize` and throws for a WALL or interior edge (the concentration could
never enter). Main `.inp` only — the `.2dm` sidecar mini-registry does not
carry it, same as `[2D_INITIAL_QUALITY]` (recorded).

### 2.3 `[2D_INITIAL_QUALITY]` — `CELL n | TAG name | *  SPECIES CONC`

Precedence `* < TAG < CELL` by kind, not file order. `CELL` is 1-based (the
user-facing mesh convention); unknown species/tag, off-mesh cell, or rows
with transport off are fatal. Mass = conc × the cell's initial volume, so a
row on a dry cell seeds nothing — deliberate (concentration of no water).

### 2.4 `[2D_OPTIONS] DISPERSION <m²/s>` — refuses `< 0`.

### 2.5 HDF5 `Mesh2_face_species_conc [nTime, nSpecies, nFace]`

Lazily created on the first snapshot with `surface_species_count > 0`
(`createUnlimitedDataset` + `extendAndWrite3D`), attrs `species_names` from
`snap.pollut_names`. The filler in `fillSurfaceSnapshot` divides mass by
volume behind `dry_depth × area`, so dry cells report 0 exactly as the S1
`concentration()` accessor does.

## 3. Validation protocol

1. `run_corpus.sh base patched` → 2D section 32/32 identical; 1D corpus green.
2. Build; run `test_engine_2d_transport_s1` (must still be 6/6 — it is the
   D=0, no-source control for S2) and `test_engine_2d_transport_s2`.
3. Gate-by-gate expectations:

| # | Gate | Falsifies |
|---|---|---|
| 1 | Rain at `c_rain == c0` keeps a still pond uniform to 1e-12; `gained_rainfall == R·A·T·c0` | rain mass booked against a different volume/time than the rain's own |
| 2 | Clean rain dilutes every cell by exactly `h0/(h0+RT)` | rain path touching mass when `rain_conc` empty |
| 3 | Step, D=0.05: mass 1e-10, max principle, monotone profile, `binds == 0`, smoothed | sign error in `dMd`, wrong accumulator side, limiter tripping when it should not |
| 4 | Pulse variance grows `2·D·T` within **[0.6, 1.4]×** | wrong conductance scale (`ξ`, `inv_dx_normal`, `hf`) |
| 5 | D=20 (unstable explicit): `binds > 0`, still conservative and bounded | limiter absent or non-conservative |
| 6 | D=0 leaves `cell_mass` **bit-identical** | dispersion block booking at D=0 |
| 7 | SPECIFIED_FLOW inflow (`edge_bc_flow = −1e-3`) at `c0` keeps pond uniform to 1e-12; species 1 (no row) gets 0 and dilutes; `gained_boundary[0] == V_in·c0` | slot/species stride error in `bc_conc`, booking to a volume other than `v_new−v_old` |
| 8 | `[2D_BOUNDARY_QUALITY]` on a WALL edge throws at `initialize` | silent no-op row |

4. Gate 4's band is the one figure I could not calibrate. **Tighten it to
   the observed value ± a modest margin; do not widen it.** If growth lands
   well below 0.6×, suspect `ξ` (face length ratio) or the centroid distance
   in `inv_dx_normal` being applied twice; well above 1.4×, suspect `hf`
   being a depth where a volume was meant.

## 4. What I am least sure about

1. **Gate 7's inflow magnitude.** `edge_bc_flow = −1e-3 m³/s/m` on a ~1 m
   edge over 600 s adds ~0.6 m³ to a 40 m³ pond — enough to measure, small
   enough not to raise a wave. If the BC cell's head rises and drives
   interior flow, uniformity still must hold (S1 property), so the gate is
   sound either way; only `ASSERT_GT(v_in, 0)` depends on the sign
   convention (`computeBoundaryEdgeFlux` returns `−edge_bc_flow·L`, inflow
   positive — read off the source, not run).
2. **`#pragma omp atomic` on `dispersion_limiter_binds`** inside
   `fireFaces`. If `fireFaces` is not an OpenMP region on some build the
   pragma is inert; if it is, `long` atomic increment is supported. If the
   compiler rejects it, replace with `std::atomic<long>` in the state or drop
   the pragma and accept a racy count (telemetry only).
3. **Rain in `fireCells` reads `state_->rainfall[i]`** in the same species
   loop that gathers `sacc_*`; the volume update that follows adds the same
   `rainfall*dt_c*area`. If the volume path applies a different rain
   quantity (e.g. a forced override array), gate 1 fails and points exactly
   at the mismatch — fix by reading the same quantity, not by loosening.
4. Gate 3's monotone-profile check assumes the strip's column index is
   `floor(tri_cx)` — true for `dx = 1`.

## 5. Falsifiers that bite the plan

- If gate 4 cannot be brought inside ±25 % on this regular mesh, the
  two-point face flux is not a consistent Laplacian on triangle centroids
  and S2b (gradient-based, LTS-consistent) is needed for dispersion too, not
  only for second-order advection.
- If gate 5 shows binds but gate 3 shows binds too at D=0.05, the β-share
  cap (not the equalisation cap) is the active one at ordinary D — the share
  divisor `/3` inherited from volume is too tight for species and should be
  reconsidered.

## 6. Owed, not in this round

- Report `dispersion_limiter_binds` (continuity table / 2D summary) — the
  counter exists, nothing prints it.
- `[2D_BOUNDARY_QUALITY]`, `[2D_INITIAL_QUALITY]` in the `.2dm` sidecar
  mini-registry and in the InpWriter round-trip.
- Curated tracked 2D deck **with** `[POLLUTANTS]` for the bitwise net
  (lesson 225) — without it S1/S2 stay invisible to the corpus.
- Time-varying boundary concentration (timeseries name in place of CONC).
- Coupling spill 1D→2D concentration and `exch_mass` delivery — S3.

## 7. Then S3–S6, unchanged from the plan.

---

# CHECK RECORD (2026-09-02, checking agent)

**VERDICT: VALIDATED AND COMMITTED** as `2d2730d9`, on landing base
`1af1db1a` (two peer commits arrived mid-round; the H6b rule). Evidence:
`tests/output/2d_transport_s2/PROVENANCE.txt`.

## One REAL physics fix — the first S-round to need one

**The max principle broke WITH the limiter binding** (gate 5, D = 20:
cmax = 15.312568024340905 on an initial max of 10). Pairwise
equalisation caps do not compose: three faces, each capped at FULL
pairwise equalisation from the same start-of-substep state, can leave
the receiver richer than every donor. Fixed with `eq /= 3` — each face
closes at most a third of its gap; harmonic volume ≤ receiver's own
bounds the 3-face sum by the largest donor, the same composition
argument your β/3 makes on the volume side. Falsifier E (drop the /3)
reproduces the failure to the bit.

## Your calibration instruction was load-bearing

Gate 4 measured **growth/expect = 1.006944** — §5's S2b-for-dispersion
concern is closed in the negative (the two-point face flux IS a
consistent Laplacian here). Band tightened to [0.9, 1.1] as instructed —
and under falsifier D (ξ dropped) the ratio reads **0.890**: your
original [0.6, 1.4] would have passed the defect. Calibrate-then-tighten
is not cosmetics.

## The gate you did not write

All eight gates drive the solver directly, so `[2D_INITIAL_QUALITY]` —
which this round LANDS — had no observer at all: parser, pending rows,
`* < TAG < CELL` precedence, seeding, all unwatched (lesson 223's shape,
in-round). The check added gate 9: full deck path, four tagged cells,
three scopes, two species (passed first try), plus the three refusal
legs. Base runs those decks at **rc 0, silently ignoring the section**
— which is also this round's fails-at-base statement for the deck
surface.

## Fixture + instrument fixes

- Gate 7 asserted `cmax < c0` for the diluting species — 0.6 m³ into a
  40 m³ pond does not traverse the strip; the far end holds exactly c0.
  Now `cmax ≤ c0` and `cmin < c0`.
- The census swept the check's own `_s2iq_*` scratch fixtures in as
  "differing" decks; discovery now excludes underscore-prefixed decks
  (lesson 225 follow-through).

## Falsifiers

A (sinks after volume write) → S1's ledger gate alone, exact. B (rain
at 2× dt) → gate 1 alone, exact. C (sign flip) → gates 3 AND 4. D → see
above. E → bit-identical reproduction. F (BC at −f·dt_c) → gate 7 at
worst_rel = 1.0 — honest note: the substitution inverted the sign, so
the sign fired, not requested-vs-applied; those are EQUAL unclamped,
and the k-stride is invisible at one BC slot (k·ns+s ≡ s·nslots+k when
nslots = 1). A two-slot deck would discriminate — residual gap,
recorded.

## Figures

S2 **9/9** (with gate 9); S1 control **6/6** unchanged; 1D corpus
**23/23**; 2D census clean-checkout 2/2, full **33/33** comparable vs
the shared-tree S2 binary; ctest **190/190 ×3** (189 at the landing
base + this suite; your "206" counted gates).

## §4's worries, answered

1. Gate 7's inflow: sound as designed; `v_in = 0.6 m³` measured.
2. `omp atomic` on the bind counter: compiled clean on clang.
3. Rain quantity: same `state_->rainfall[i]` both sides — gate 1 pins it
   (falsifier B).
4. `floor(tri_cx)` column indexing held on the dx = 1 strip.
