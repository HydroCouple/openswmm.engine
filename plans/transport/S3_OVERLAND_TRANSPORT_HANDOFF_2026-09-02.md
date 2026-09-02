# S3 + S2 debts — the 1D↔2D coupling carries species — Handoff (2026-09-02)

**For:** the checking agent.
**Plan:** `OVERLAND_TRANSPORT_HEAT_MSX_PLAN_2026-09-01.md` D-2DT4 (species half;
age/enthalpy rows arrive with S4) plus the S2 record's four debts.
**Implemented syntax-only.** `g++ -fsyntax-only -std=c++20 -fopenmp
-DOPENSWMM_HAS_2D` on every touched TU: **0 errors**. Nothing built or run.
**Base:** `0f0b1df5`. The working tree again carries modifications that are
NOT this round's (regenerated fixtures, `PROCESS_COMPONENT_*` plan edit from
an earlier round). **Stage only the files below, by patch.**

```
new: tests/unit/engine/test_2d_transport_s3.cpp                (4 deck gates)
new: tests/parity/corpus/decks/twod_coupled_quality.inp        (census/corpus deck WITH pollutants)
new: plans/transport/S3_OVERLAND_TRANSPORT_HANDOFF_2026-09-02.md
mod: src/engine/data/NodeData.hpp                    (+coupling_qual_queue, coupling_qual_inflow; resize/erase/clear)
mod: src/engine/2d/data/SurfaceTransportState.hpp   (+gained_coupling, coupling_src; identity subtracts it)
mod: src/engine/2d/solver/ExplicitInertialSolver.hpp/.cpp (spill at nodes.conc; addCouplingSourceMass in fireCells + both lazy paths)
mod: src/engine/2d/coupling/NodeCoupling.hpp/.cpp   (scatterCouplingFlux(..., conc); injectAccumulatedExchange(..., node_conc))
mod: src/engine/2d/SurfaceRouter2D.cpp              (coupling_src sizing/clear; exch_mass → node mass queue;
                                                      outfall conc into inject; limiter-bind warning)
mod: src/engine/core/SWMMEngine.cpp                 (mass-queue drain beside the volume queue; sidecar rows; twod_io wiring)
mod: src/engine/quality/QualityRouting.hpp/.cpp     (addCouplingLoads — LAST loader)
mod: src/engine/core/SimulationContext.hpp          (twod_io.pending_iq/pending_bq + fwd decls)
mod: src/engine/core/InpWriter.cpp                  (DISPERSION key; [2D_INITIAL_QUALITY]/[2D_BOUNDARY_QUALITY] emit)
mod: src/engine/2d/input/SectionHandlers2D.hpp/.cpp (.2dm mini-registry carries both quality sections)
mod: tests/parity/MANIFEST                          (+twod_coupled_quality row, "EXPECT TO MOVE")
mod: tests/unit/engine/CMakeLists.txt               (test_engine_2d_transport_s3)
```

---

## 1. ⚠ Nets first — and this round the census MUST move on exactly one deck

`run_corpus.sh base patched`:
- 1D corpus: green. `addCouplingLoads` is added LAST and is a no-op when
  `coupling_inflow == 0` everywhere (every 1D deck), and `coupling_qual_*`
  are sized only with pollutants and touched only through the 2D router.
- 2D census: the 33 pre-existing decks byte-identical (no `[POLLUTANTS]` ⇒
  `sacc_L_` empty, `coupling_src` unsized, `exch_mass` empty, queue empty).
  **`twod_coupled_quality.inp` is NEW and is expected to DIFFER against base**
  (base has no S1–S3: J1's Cu column reads 0 there, and base ignores the
  two quality sections at rc 0 — the S2 check's finding). Attribute it, do
  not "fix" it. It is also on the MANIFEST, so the corpus reports it once
  more; one deck, two nets, one attribution.

The hydraulics-adjacent edits to diff by eye: (a) `scatterCouplingFlux` was
refactored around an `add(cell, q_share)` lambda — the three
`coupling_flux += q/area` sites must produce the same doubles (same
operands, same order: `(Q * w) / area` → `dens = q_share / area`, then
`+= dens`; `Q * (slope/wsum)` and `Q * wt` are passed as `q_share`
unchanged). (b) `injectAccumulatedExchange` gained a defaulted parameter
only. (c) `assembleLateralInflows` gained a block guarded on
`!coupling_qual_queue.empty()`.

## 2. What S3 does — the two directions are NOT symmetric, on purpose

| Direction | Where the species is booked | 1D-side counterpart |
|---|---|---|
| **2D → 1D junction drain** | marcher: `sinkMassAtCellConc` → `exch_mass[k*ns+s]` (S1, unchanged) → router: `coupling_qual_queue[node*np+p] += exch_mass × flow_2d_to_1d` | `assembleLateralInflows` drains the mass queue by the **same rule** as the volume queue (uniform over `coupling_delivery_remaining`, flush when ≤ dt) into `coupling_qual_inflow` (rate); `addCouplingLoads` adds the WATER to `qual_vol_in` and the MASS to `qual_mass_in`, books `qual_routing_ex_in` |
| **1D → 2D junction spill** | marcher live-exchange block: `cell_mass += take × nodes_1d->conc[ni*ns+s]`, `gained_coupling` | **none — deliberately.** See §2.1 |
| **1D → 2D outfall discharge** | `scatterCouplingFlux(..., conc)` fills `transport.coupling_src[s][cell]` (conc·m/s density) beside `coupling_flux`; `fireCells`/lazy paths add `coupling_src × area × dt` | none needed — the outfall already books its load as leaving the 1D system |
| 2D → outfall withdrawal | S1: `lost_coupling` at cell conc | not delivered to the outfall (recorded, §6) |

### 2.1 Why the spill queues no debit — and the finding under it

The CSTR mix is `c_new = (c_old·V_old + M_in)/(V_old + V_in)` with `V_in`
the **net** inflow volume. A spill is a negative lateral flow, so it reduces
`V_in`; the end-of-step node mass `c_new·V_new` plus link outflow `c_new·V_out`
sum to `c_old·V_old + M_in − c_new·S`: **the node already loses the spill's
mass at the mixed concentration**, implicitly. Queuing `−S·c` through the
D-NS1 extraction path would remove it twice. The 2D side books `S ×
nodes.conc` (the published value, one step older than `c_new`) — the same
one-step lag the frozen node head has. Exact when the node is at steady
concentration (gate 2), first-order-in-dt otherwise; that is the honest
statement and the reason gate 2 is built on a node fed at one concentration.

**Finding (pre-S3 defect, now fixed):** the 2D→1D drain's WATER reached the
node hydraulically (`lat_flow`) but never entered `qual_vol_in`, so drained
water diluted nothing and the node's mass was implicitly inflated by `c·V_drain`
per step. No legacy parity question — legacy has no 2D surface. Fixed in
`addCouplingLoads` by the `qual_vol_in += q·dt` line; age/temperature take the
`EXTERNAL_INFLOW` stand-ins until S4 carries them on the mesh (the same
convention every other loader uses for water without its own state).

### 2.2 Units

2D mass is conc·m³; 1D mass is conc·ft³. The queue multiplies by
`flow_2d_to_1d` — the SAME factor the volume takes — so mass/volume is the
cell concentration exactly. Under SI decks the factor is still 35.315 (1D is
internally ft³); `nodes.conc` is unit-free. `n_species == n_pollutants`
(S1–S3 carry pollutant rows only), so `nodes.conc`'s stride is the 2D stride;
both sides bounds-check `(ni+1)*ns <= size` rather than assume.

## 3. The S2 debts, closed

- **`dispersion_limiter_binds` reported**: `SurfaceRouter2D::finalize` pushes a
  WARNING with the count (same shape as the outfall-clamp warning).
- **`.2dm` sidecar**: `load2DMeshExternalFile` takes two optional pending
  stores; the mini-registry registers `2D_INITIAL_QUALITY`/`2D_BOUNDARY_QUALITY`
  only when passed (older callers unchanged). `SWMMEngine` passes them.
- **InpWriter round-trip**: `DISPERSION` emitted when > 0 (default omitted, per
  the option-default rule); both sections emitted verbatim from the retained
  pending rows (`CELL` 1-based, `TRI/EDGE` 0-based — exactly as parsed).
  `twod_io.pending_iq/pending_bq` wired. **Not gated this round**: add a
  round-trip leg (write → reopen → same pending rows) to the existing IO4
  round-trip suite; I did not want to touch its fixture set blind.
- **Tracked deck with pollutants**: `twod_coupled_quality.inp`, §1.

## 4. Validation protocol

1. Nets (§1). 2. Build; `test_engine_2d_transport_s1` 6/6, `_s2` 9/9 still.
3. `test_engine_2d_transport_s3`:

| # | Gate | Falsifies |
|---|---|---|
| 1 | Pan at c0=4 drains into an empty clean J1: `nodes.conc[J1] == c0` to 1e-9; `lost_coupling == c0·drained`; 2D identity; **`lost_coupling×f == qual_routing_ex_in + Σ coupling_qual_queue`** to 1e-9 | mass and volume queues drained by different rules; mass booked at another concentration; the `qual_vol_in` finding (J1 would read > c0 without it) |
| 2 | J1 fed 0.3 m³/s at c0 surcharges and spills onto a c0 pan: J1 reads c0 (premise), every wet cell stays c0 to 1e-9, `gained−lost == c0·(spilled−drained)` | spill booked at zero (S1 placeholder), wrong stride, wrong volume |
| 3 | Outfall O1 coupled: `outfall_in > 0.5`, `coupling_src` sized, `0 < gained ≤ c0·outfall_in`, `cmax ≤ c0`, 2D identity, O1 reads c0 at the end | source scattered above the outfall's conc; species arriving clean |
| 4 | Drain deck with and without `[POLLUTANTS]`: `state.volume` and `coupling_2d_to_1d_out` **bit-identical** | any S3 branch touching hydraulics |

Gate 3 is deliberately weaker (C1 starts clean so O1's concentration rises
over the run — a uniformity gate would fail for the right reason). If you
can seed C1, tighten it to uniformity.

## 5. What I am least sure about

1. **Gate 1's `qual_routing_ex_in` identity at run end.** The mass queue may
   hold a remainder at the final step; the gate adds `Σ coupling_qual_queue`
   for that. If `qual_routing_ex_in` is reset/consumed by the report stage
   before `context()` is read (I read it with the engine still open, before
   `swmm_engine_end`), the identity shifts by exactly the last delivery —
   move the read, don't loosen.
2. **Gate 2's fixture geometry.** J1 rim 1.0 m, pan bed 0.5 m + 0.2 m water
   (η = 0.7). C1 (0.3 m circular, 30 m, 0.5 m fall) carries ≈0.12 m³/s;
   0.3 m³/s in ⇒ surcharge to the rim, spill through the 0.7·1.0 m² orifice.
   If `spilled < 0.5 m³` in 20 min the assert names the fixture, not S3 —
   raise the feed or lower the pan. `ALLOW_PONDING NO` so the excess above
   the rim floods (1D ledger) rather than accumulating.
3. **Gate 3's outfall geometry.** Pan bed at −1.0 with 0.05 m water, O1 invert
   −0.5: the discharge should land as a free source. If
   `accumulateOutfallDischargeStep` withholds (tailwater/stage logic), the
   `outfall_in` assert says so.
4. **`init_mass` in gates 1/3 uses `mesh().tri_area × h0`** — correct only if
   the mesh is SI-native (no `;; UNITS` header ⇒ metres). If the S2 iq gate's
   mesh convention differs, read initial volumes instead.
5. **Outfall conc row when `nodes.conc` is unsized** (pure-age/heat decks):
   guarded by `(ni+1)*ns <= size`; `coupling_src` is sized only when
   `tr.active()` so `np == 0` allocates nothing.

## 6. Owed, not in this round

- InpWriter/sidecar round-trip GATE for the two sections and DISPERSION.
- 2D→outfall withdrawal species delivery (currently `lost_coupling` only).
- Age/temperature rows across the coupling (S4; `node_age_vol_in`,
  `node_temp_vol_in` from the tuple instead of the EXTERNAL_INFLOW stand-in).
- Forced coupling flux (`coupling_force_val`) carries no species — by design;
  document when the forcing API grows a quality argument.
- S4: age/temp/MSX rows + `HeatElemKind::CELL2D`; S5 per-cell heat; S6 GPU.

---

# CHECK RECORD (2026-09-02, checking agent)

**VERDICT: VALIDATED AND COMMITTED** as `178576ae` on `0f0b1df5` (the
handoff's base held). Evidence:
`tests/output/2d_transport_s3/PROVENANCE.txt`.

## Your §2.1 finding, confirmed and made quotable

With the `qual_vol_in` line removed (falsifier iv), gate 1's junction
reads **exactly 2×c0** — 8.0000 on a 4.0 drain: mass with no diluting
volume. The pre-S3 defect was real and this is its number.

## The check's one physics fix — the flip-flop gross/net mismatch

Your design queued the drain GROSS while the volume side NETS (signed
`exch_`, signed volume queue), and the rim exchange flip-flops — on
gate 1's pan the recirculation EXCEEDS the pond (98 m³ back over a
50 m³ seed). A mixed-sign window handed the node net water with gross
mass: gate 2's junction, fed only c0 water, read **4.3118**. Fixed at
WINDOW granularity: the marcher tracks the spill volume per point
(`exch_spill`), the router debits `min(spill, gross drain)` of it at
the node's published concentration from the queued mass. Two dead ends
are recorded so nobody re-walks them: an incremental per-substep debit
is order-dependent (residue 4.094/0.035 measured), and both of the
check's first identity formulations (gross, then lost−gained) were
wrong — the gross ledgers count the recirculation. §2.1's
implicit-CSTR argument SURVIVES, scoped to exactly the net-negative
remainder.

## The identity that closes (gate 1, rewritten)

queue-delivered mass == c0 × `coupling_2d_to_1d_out` × f — measured
**20898.1528 == 4 × 147.9424 × f to the digit**. The books split by
window sign: positive windows put volume in the out-ledger and mass in
the queue; negative windows subtract from the signed volume queue and
their mass leaves the node implicitly. Your §5.1 worry (end-of-run
queue remainder) is handled by the gate's `+ Σ queue` term as designed.

## Nets

1D 23/23 identical + `twod_coupled_quality.inp` DIFFERS (360 of 4788
bytes — the Cu column absent at base); the census reports the same
single mover; runner rc 1 forced exactly the attribution your §1
demanded. One deck, two nets, one mover.

## Falsifiers

i (instant-flush queue) bites BOTH coupled gates — the check predicted
it might converge invisibly by run end and was wrong. ii
(half-concentration mass) both. iii (clean spill) both — gate 1's own
flip-flop spill-backs dilute too. iv → 2×c0 exactly. v (pairing
removed) == the pre-fix state, reproduced. vi (nullptr outfall conc) →
gate 3 alone.

## Figures

S3 **4/4**, S1 **6/6**, S2 **9/9**; ctest **191/191 ×3** (binaries:
190 + this suite). §4's fixture worries: gate 2 spilled plenty
(spilled = 98 m³ ≫ 0.5); gate 3's outfall discharged (out_in > 0.5 ✓,
O1 read c0 at 1e-6); the SI-native mesh init_mass convention held.

## Owed (unchanged from your §6, plus none new)

InpWriter/sidecar round-trip gate; 2D→outfall withdrawal delivery; S4
rows across the coupling; forced-flux quality argument; S5; S6.
