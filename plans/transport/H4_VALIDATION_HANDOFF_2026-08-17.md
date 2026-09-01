# H4 Implementation — Validation & Commit Handoff (2026-08-17)

**For:** the checking agent.
**Base:** `7038bea9`.
**Plan:** `HEAT_TRANSPORT_PLAN.md` §6 H4 — Eulerian ARD binding, full CSH
eq. 4.1.
**Standing findings:** lessons 1–58.

---

## 1. What this delivers

`__TEMPERATURE__` becomes a row on the ARD transport mesh, after pollutants,
MSX and age. It therefore inherits **advection, FCT, node CSTR mixing,
structure passthrough and dispersion** from the shared kernels — all of which
loop `for s < ns` with no species index anywhere — plus a new **per-cell**
surface-flux stage.

Per-cell is the point of the phase. The LEGACY mirror applies H2/H3's fluxes
once per link on one lumped temperature; on the mesh a conduit is many cells
with their own temperatures, which is what makes an advected thermal wave
representable at all.

**H1's "ARD tracks no temperature, wait for H4" warning is retired and its
gate inverted in this changeset** (lesson 21).

## 2. Changeset (uncommitted)

```
mod:  src/engine/transport/components/EulerianArdComponent/ArdEngine.{hpp,cpp}
      (temp_row_; row allocation; cell + node-store seeding; loader
       consumption; applyHeatFluxes(); publish() branches; nm arithmetic;
       sidecar naming; structure passthrough)
mod:  src/engine/core/SWMMEngine.cpp        (H1 ARD warning retired)
mod:  src/engine/data/SpeciesRegistry.hpp   (RESERVED_TEMPERATURE added to
      the transported whitelist — see §4.4)
mod:  tests/unit/engine/test_heat_transport.cpp   (gate 6 INVERTED)
new:  tests/unit/engine/test_heat_ard_binding.cpp (4 gates)
mod:  tests/unit/engine/CMakeLists.txt      (+1 target — shared file)
```

**Also carried, from the previous round:** the `SEDIMENT_EXCHANGE` deferral
said "phase H4"; the plan says **H6** (H4 is this binding). Fixed in
`HeatComponent.cpp` plus two stale test comments. Small, but a deferral whose
whole job is to name the right phase was naming the wrong one.

All touched TUs pass `g++ -std=c++20 -fsyntax-only`.

## 3. The row that had to be added in six places, and what breaks if it isn't

`publish()` dispatches `if (s == age_row_) … else if (s < np) … else MSX`,
and derives `nm = ns − np − na`. A temperature row **without its own branch
falls into the MSX arm** and writes past `msx_*_conc`; **without the `− nt`**
the MSX count is one too high.

Neither shows up as a wrong temperature. They corrupt the **pollutant and
MSX** arrays. That is why **gate 1 asserts TSS on a heat deck**, differenced
against the same deck with heat off — lesson 14's shape (a row-count change
sweeping a neighbour) and lesson 51's (a category gaining a second member).

Sites duplicated from the age row: allocation (`ArdEngine.cpp` ~:95), cell
seed (~:232), node-store seed (~:250), loader consumption (~:655),
`publish()` link + node branches, structure passthrough, sidecar naming.

## 4. Design decisions to review

### 4.1 Per-cell free surface = `top width(depth) × cell_dx`

`cell_h` is **not maintained** by this engine (only the hydraulic FV solver
writes it), so depth is recovered as
`fv::kernels::depthOfArea(geom, cell_a)` and width as
`widthOfDepth(geom, h)`. Both are already barrel-scaled via
`FvGeometry::barrel_scale`, so **`barrels` is not applied again** — the
LEGACY path multiplies by `CD.barrels` because its `top_width × length` is
per-barrel; the mesh's is not.

### 4.2 Gated on `FvGeometry::is_open`, which also disposes of the slot

`widthOfDepth` includes the **Preissmann slot** above the crown — for a
surcharged pipe that is a numerical device, not a water surface. Gating on
`is_open` (the mesh twin of `xsect::isOpen`) means a closed conduit never
exchanges, so the slot width can never be mistaken for a free surface.
Gate 4 is the observer. **This is the cleanest resolution I found; flag if
you would rather clamp the width at the crown instead.**

### 4.3 The flux stage sits in the aging slot, once per ROUTING step

Not per substep. It is Lie-split exactly where A1a's aging is, at the same
cadence, after the advection–dispersion subcycle. Consistent with every
other source term in this engine (lesson 13's O(dt) split).

### 4.4 `SpeciesRegistry::transported_count()` — a latent lesson-51 site

Its kind whitelist omitted `RESERVED_TEMPERATURE` and its doc said the kind
"joins with phase H1" (it did not). **It has no callers in `src/`**, so the
staleness was latent rather than live — which is exactly why it was still
wrong when H4 arrived. Fixed, and annotated as the shape lesson 51 warns
about so the next reserved kind is not a third instance.

### 4.5 Temperature disperses at the SOLUTE dispersion coefficient

`dispersionSolve` is generic over rows and has no per-row coefficient. So
temperature gets whatever `D` the deck configures for species. Physically
thermal diffusivity differs, but at the scales here dispersion is dominated
by shear, not molecular diffusivity, so a shared `D` is defensible. **Stated
because it is a physics decision made by inheritance rather than
deliberately** — flag if it deserves its own coefficient.

## 5. Validation protocol

1. Reconfigure, build, zero new warnings.
   **Lesson 52's grep, and lesson 58's:** `grep -rn "age_row_" src/engine/`
   — every hit should now have a `temp_row_` sibling or be provably generic.
   And `grep -rn "H4" tests/` — the phase name is being retired, so any
   other suite asserting it must move (H1's ARD gate is inverted here; H2's
   `SEDIMENT_EXCHANGE` leg was migrated last round).
2. `ctest -R "test_engine_heat"` — 9 + 7 + 7 + 4 gates.
   *Anticipated failure modes, likelihood order:*
   (a) **Gate 2's cross-engine band (2 °C)** is a guess — I have not run
   either engine. If it fails, report BOTH values before widening: the
   setup legs assert each engine left its 5 °C seed, so a real failure
   distinguishes "one engine inert" from "schemes differ".
   (b) **`depthOfArea` may not invert cleanly** for a nearly-empty cell;
   the guard is `h > 0`, then `width > 0`. If gate 3 shows no cooling,
   instrument the first cell's `h` and `width` — that is where I would look
   first.
   (c) **Gate 1 uses `EXPECT_NEAR(…, 1.0e-12)`** on pollutants, which is
   effectively bitwise. It should hold: heat adds a row but changes no
   pollutant arithmetic. If it fails by a rounding-scale amount, that is
   still a finding — say so rather than loosening it.
3. **Falsifier sweep:**

   | falsifier | expected failing gates |
   |---|---|
   | i. drop the `s == temp_row_` branch from `publish()`'s LINK loop | **1** — via corrupted `msx_link_conc`/pollutants, not via temperature. If it fails nothing, the MSX-arm hazard is unobserved and §3 is an assertion |
   | ii. revert `nm` to `ns − np − na` | **1** |
   | iii. remove `applyHeatFluxes` from `step()` | 3 |
   | iv. drop the `is_open` gate in `applyHeatFluxes` | 4 |
   | v. seed cells from 0 instead of `temp_seed` | 2 (ARD starts at 0 °C, diverges from LEGACY) |
   | vi. skip the `heat_state.resize` at init | 1's setup leg (`heat_state` empty) |
   | vii. leave H1's ARD warning in place | the inverted gate 6 in `test_heat_transport.cpp` |
   | viii. apply the flux per SUBSTEP instead of per routing step | nothing — **flagged in advance as probably unobserved**; it would scale the flux by the substep count. Record whether anything catches it; if not, that is an owed gate and the cheap shape is a one-routing-step deck with a known substep count |
4. **Prior suites:** ARD decks WITHOUT `HEAT_TRANSPORT` take `nt = 0` and
   every row index is unchanged, so **the whole ARD suite must be unchanged**
   and 14/14 deck `.out` bit-identity must hold against `7038bea9`. The
   water-age suite must be unchanged — age keeps its row and its index.
5. **Record:** (a) falsifier i's result, which is the entire argument of §3;
   (b) falsifier viii, which I expect to be unobserved.

## 6. Known gaps

- **G-UT3 is NOT delivered.** Plan §6 H4 asks for a CSHComponent validation
  case (an advected diurnal temperature wave) within documented tolerance.
  Gate 2 is an ARD-vs-LEGACY cross-check, which is weaker: it shows the two
  engines agree, not that either matches an external reference. A diurnal
  wave also needs time-varying forcing, and H3 ships shortwave as a
  **constant** with timeseries refused — so G-UT3 needs that surface first.
  **Recorded as owed with its dependency named**, rather than claimed.
- **CSHComponent is present** at `HydroCouple/CSHComponent/` in the mounted
  checkout (the H3 round reported RHEComponent missing; it is there —
  `RHEComponent/src/element.cpp` — so a different clone was being read).
  Worth confirming before concluding a reference is unavailable.
- H2's owed top-width gate still applies, now to the mesh path too.

## 7. Commit message

```
feat(transport): bind temperature to the Eulerian ARD mesh (H4)

__TEMPERATURE__ becomes a mesh row after pollutants, MSX and age, so it
inherits advection, FCT, node mixing, structure passthrough and dispersion
from the shared kernels - every one of which loops over all species rows with
no pollutant index - and gains a per-cell surface-flux stage. Per-cell is the
phase: the LEGACY mirror applies H2/H3's fluxes once per link on one lumped
temperature, while the mesh evaluates each cell against its own state, which
is what makes an advected thermal wave representable.

Free surface per cell is top width(depth) x cell_dx, with depth recovered via
depthOfArea because this engine does not maintain cell_h. Both are already
barrel-scaled by FvGeometry, so barrels are not applied twice. Gated on
FvGeometry::is_open, which also disposes of the Preissmann slot: a closed
conduit never exchanges, so its slot width can never be read as water.

The row had to be added in six places, and the dangerous ones do not show up
as wrong temperatures. publish() dispatches through an MSX else-branch and
derives the MSX row count by subtraction, so a missing branch or an
uncorrected count corrupts the POLLUTANT arrays instead - which is why the
first gate asserts TSS on a heat deck, differenced against the same deck with
heat off.

H1's "ARD tracks no temperature, wait for H4" warning is retired and its gate
inverted here, asserting both that the message is gone and that the feature
works. SpeciesRegistry::transported_count()'s kind whitelist, which had no
callers and so stayed stale, gains RESERVED_TEMPERATURE.

Gates: tests/unit/engine/test_heat_ard_binding.cpp - pollutant integrity with
the row present, ARD-vs-LEGACY agreement with setup legs proving neither
engine is inert, per-cell fluxes acting, and closed conduits not exchanging.

Plan: HEAT_TRANSPORT_PLAN.md section 6 H4.
Validation record: plans/transport/H4_VALIDATION_HANDOFF_2026-08-17.md
```

## 8. Validation results

*(appended by the checking agent, 2026-08-18)*

Validated in an isolated worktree at `7038bea9` carrying only the manifest.
Artefacts: `tests/output/h4_validation_2026-08-18/`.

### 8.1 Outcome

| check | result |
|---|---|
| configure + build | clean; **zero** new warnings (the four `SWMMEngine.cpp` `-Wunused` hits are pre-existing at base) |
| `test_engine_heat_*` | **29/29** — H1 9, H2 7, H3 7, H4 **6** (4 as delivered; see §8.3) |
| full `ctest` | **146/147** — the one failure is `FvEngine.RefiningTheMeshConvergesTowardTheDynwaveHydrograph`, confirmed failing **at base** by stashing the changeset and rebuilding (the known bistable refinement gate) |
| deck `.out` bit-identity vs `7038bea9` | **14/14**, including `force_ard` and `sdm_struct_dw_ard` — §4's "the whole ARD suite must be unchanged" holds byte for byte |
| ASan + UBSan | **0 findings** over 6 suites / 44 tests, including `test_engine_reaction_ard_binding` and `test_engine_ard_transport`, which share `publish()` |
| falsifiers | 11 run (your 8 + 3 of mine). **As delivered: 5 of 11 observed.** After the gate work: **9 of 11**, and the 2 that still escape are not defects |

`grep -rn "age_row_" src/engine/` leaves exactly two sites without a
`temp_row_` sibling, both correctly so: the hotstart consumption at
`ArdEngine.cpp:193` (there is no `heat_state.hotstart_loaded` — heat hotstart
is still owed from H1) and the aging increment at `:1008`, whose temperature
counterpart is `applyHeatFluxes` on the next line. `grep -rn "H4" tests/`
found one more stale name than the manifest fixed — see §8.5.

### 8.2 The falsifier sweep, as delivered

| falsifier | you predicted | as delivered |
|---|---|---|
| i. drop the temp branch from `publish()`'s LINK loop | gate 1 | **SIGSEGV** — but not by the stated mechanism (§8.4) |
| ii. revert `nm` to `ns − np − na` | gate 1 | **nothing** |
| iii. remove `applyHeatFluxes` | gate 3 | gate 3 ✔ |
| iv. drop the `is_open` gate | gate 4 | gate 4 ✔ |
| v. seed cells from 0 | gate 2 | **nothing** |
| vi. skip `heat_state.resize` | gate 1's setup leg | **nothing** (§8.6) |
| vii. leave H1's warning in place | inverted gate 6 | gate 6 ✔ |
| viii. flux per SUBSTEP | probably nothing | nothing — and it is **not a defect** (§8.7) |
| ix. *(mine)* apply `barrel_scale` to the cell area a second time | — | **nothing** |
| x. *(mine)* flux inside the subcycle at the FULL `dt` | — | **nothing** |
| xi. *(mine)* double the cell surface area outright | — | **nothing** |

**§5(a): falsifier i is observed, and §3 is still an assertion.** The two are
not in tension. Dropping the branch sends the temperature row into the MSX arm
at `msx_link_conc[link * nm + (s − np)]`, which with `np=1, nm=2` is
`link*2 + 2` — the NEXT conduit's first species slot. The next iteration of
the outer conduit loop then writes that slot correctly, so **every misdirected
write except the last conduit's is overwritten before anything can read it**;
the final one lands one past the end. Probing an MSX + heat deck under the
falsifier: `msx_link_conc` comes back **bit-identical to the clean run**, and
the only visible state is `link_temp` frozen at the 5 °C seed. So the array
corruption §3 describes is self-healing, and what actually kills the process
is a one-past-the-end heap write repeated every publish. Worth having in the
record because "gate 1 catches it" and "it segfaults before gate 1 finishes"
are different guarantees.

**§5(b): falsifier viii is unobserved, and correctly so.** See §8.7 — as
specified it is a consistent refinement, not a defect. Its real shape is
falsifier x.

### 8.3 Gate changes

Four falsifiers that should have been caught were not, so:

**Gate 1 → `AddingTheTemperatureRowLeavesPollutantsAndMsxIntact`.** Two
properties of the deck were doing no work.

  * *It had no MSX rows.* On a pollutant-only deck every `s` is either a
    pollutant or a reserved row, so the MSX arm is unreachable and `nm` is
    never used — falsifier ii was inert **by deck**, not by luck. The deck now
    carries two BULK species. Under falsifier ii `msx_node_conc` comes back
    sized **12 where the reference is 8**; the gate now asserts the strides
    before the values, because an inflated `nm` does not move a number, it
    widens the row so every consumer computing its own index reads a
    neighbour.
  * *It ran for an hour.* The clean inflow flushes TSS to **1.07e-21** and the
    MSX rows to ~1e-22 — the delivered gate was comparing two kinds of nothing
    with a 1e-12 band. At **five minutes** the same deck holds TSS
    8.79/34.16/39.98 and MSX 1.674/0.628, and the thermal front is mid-chain.
    Setup legs now assert the reference fields are non-trivial before
    comparing them.

  It now compares `nodes.conc`, `links.conc`, `msx_node_conc` and
  `msx_link_conc`, sizes first.

**Gate 2 gains a seed leg.** The cross-engine comparison is made after both
engines have flushed to the 30 °C inflow, so it cannot see the initial state
at all — seeding the cells from zero produces the identical 30 °C field and
the identical agreement. A five-minute leg on the same deck asserts C1 has
warmed (the front entered) and C3 has not (it still holds its seed): the
clean run reads **5.567 °C** there, falsifier v reads **0.937**.

I did **not** move the main comparison to five minutes. Measured at 5 min the
two engines are 19.84 vs 8.99 °C at J1 — a CSTR chain and an advected mesh
disagree completely mid-transient. Your choice of the flushed endpoint for the
cross-engine check was right; it just needed a separate instrument for the
seed.

**New gate 5 — `PerCellFluxAreaMatchesTheLegacyPath`.** Nothing in the suite
could see the flux MAGNITUDE. Gate 3's assertion is "cooler than with the
module off, by more than 1e-9", which an area that is double, half or
barrel-blind satisfies equally well — falsifiers ix, x and xi all passed every
gate. That leaves §4.1, the design decision this phase turns on, untested.

The gate runs one deck through both engines for the full hour. By then the
transport difference has washed out and the only thing separating the two node
fields is the flux, so it is a direct comparison of `getWofY(depth) × length ×
barrels` against `Σ width(h_c) × dx_c`. **Two barrels deliberately**: at one
barrel an implementation that applies `barrels` twice is indistinguishable
from a correct one.

Measured: LEGACY 30 / 29.8874277753 / 29.7742781026 / 29.6588823752 against
ARD 30 / 29.8889972900 / 29.7787524310 / 29.6692543995 — a **0.0104 °C** worst
gap against **0.3411 °C** of cooling. Band 0.05. Falsifiers ix and xi move it
by the whole signal.

So **§4.1's barrel claim is confirmed empirically, not just by reading
`widthOfDepth`**: at barrels = 2 the cooling roughly doubles under *both*
engines (lower depth, larger W/a) and they still agree to 0.01 °C.

**New gate 6 — `SurfaceFluxIsAppliedOncePerRoutingStep`.** Instrumenting
`nsub` showed every gate deck meshes to 12 cells and runs at **nsub == 1**, so
"per substep" and "per routing step" are literally the same code path and
§4.3 had no observer of any kind. At `ROUTING_STEP 60` the same deck subcycles
**4** ways, so the gate runs it at 5 s and 60 s and requires the hour's
cooling to agree: **29.6692543995 vs 29.6625871996**, a 0.0067 °C gap against
a 0.05 band. Falsifier x moves it by ~1 °C.

After these, 9 of 11 falsifiers are observed:

| falsifier | now caught by |
|---|---|
| i | gate 1 (heap-corruption abort) + `test_heat_transport`'s ARD gate (SIGSEGV) |
| ii | gate 1 — MSX stride 8 → 12 |
| iii | gates 3, 5 **and** 6 |
| iv | gate 4 |
| v | gate 2's seed leg — C3 0.937 vs 5 |
| vi | nothing — §8.6 |
| vii | `test_heat_transport`'s inverted ARD gate |
| viii | nothing — §8.7 |
| ix | gate 5 |
| x | gate 6 |
| xi | gate 5 |

### 8.4 Anticipated failures 2(a)–(c)

None of the three happened. Gate 2's 2 °C band passed with room (both engines
flush to 30 °C, gap < 0.001). `depthOfArea` inverted cleanly at every cell —
gate 3 shows 0.032 °C of cooling through C1 in one residence time. Gate 1's
`1.0e-12` held exactly, as you expected, though for a weaker reason than
intended until the deck was fixed.

### 8.5 Two things the manifest missed

* `test_heat_radiative_exchange.cpp`'s gate is still **named**
  `TheH3DeferralIsRetiredAndH4IsNot` — the message strings were corrected to
  H6 but the test name was not, and it now asserts that H4 is *not* retired in
  the very changeset that retires it. Renamed to `…AndH6IsNot`. This is the
  same class of staleness the carried fix was for, and `grep -rn "H4" tests/`
  is what surfaced it — worth keeping that grep in the protocol.
* `tests/unit/engine/CMakeLists.txt` in the working tree also carries a
  foreign `test_engine_inp_writer_saveas_paths` line from another session.
  Only the H4 line was staged.

### 8.6 Falsifier vi: the resize is redundant, not unobserved

`ctx.heat_state.resize(...)` in `ArdEngine::init` cannot be observed because
`QualitySolver::assembleExternalLoads` sizes `heat_state` from the same
`INITIAL_STATE` value, and `stepRouting` calls it **after** `ard_.init` on
every step including the first. The line is harmless defensive duplication,
not dead code, and removing it would make `ArdEngine` depend on that ordering
— so it stays. Flagging it rather than deleting it.

### 8.7 Falsifier viii is not a defect; falsifier x is

`dt_sub = dt / nsub`, so applying a source term once per substep at `dt_sub`
is the *same total* as applying it once at `dt`, to within the nonlinearity of
flux-versus-temperature — a consistent refinement of the split, not an error.
Your predicted failure mode ("it would scale the flux by the substep count")
describes a different mutation: passing the full `dt` inside the loop. That is
falsifier x, it over-applies by `nsub`, and gate 6 now catches it.

### 8.8 Design decisions reviewed

* **§4.1 free surface / barrels** — confirmed by measurement (§8.3), and now
  under gate.
* **§4.2 `is_open` gating** — keep it, no clamp. A closed conduit is enclosed;
  there is no sky above it to radiate to and no air to evaporate into, so
  refusing to exchange is the physics, not just slot avoidance. It also
  matches the LEGACY path's `xsect::isOpen` exactly, which is what makes gate 5
  a fair comparison. Note the gate deck's pipe is part-full, not surcharged,
  so what gate 4 actually tests is the `is_open` gate; the slot is excluded by
  construction (`is_open` is a property of the geometry, never of the state).
* **§4.3 flux cadence** — sound, and now observable (gate 6).
* **§4.5 shared dispersion coefficient** — agreed as stated; shear dispersion
  dominates molecular at these scales. Not worth its own coefficient until a
  deck exists where it matters.
* **The LEGACY mirror does not double-apply.** Verified in `stepRouting`: with
  the ARD engine initialized only `assembleExternalLoads` + `ard_.step` run,
  and `QualitySolver::execute` — which is where `routeLegacyHeat` lives — is
  skipped entirely.

### 8.9 Still owed

* **G-UT3** (advected diurnal wave vs CSHComponent) — as you recorded, blocked
  on time-varying shortwave. `CSHComponent/` is present in the mounted
  HydroCouple checkout; the H3 round's "RHEComponent missing" stands as
  reported for the clone read then, and both should be re-checked together
  when G-UT3 is attempted.
* **Heat hotstart persistence** (H1), which is why `ArdEngine.cpp:193` has no
  temperature sibling.
* H2's absolute top-width gate, now applicable to the mesh path too. Gate 5
  constrains the mesh area only *relative* to the LEGACY expression, so a
  fault common to both would survive it.
* Gate 5 resolves an area error to about 15 % of the signal; sharper would
  need a deck whose cooling is a larger fraction of the temperature range.

### 8.10 Commit

`8b5b3ef5` — 10 files (the 8 in the manifest, plus the renamed radiative gate
and the CMakeLists line), staged through a temp index and verified
byte-identical to the validated worktree.
