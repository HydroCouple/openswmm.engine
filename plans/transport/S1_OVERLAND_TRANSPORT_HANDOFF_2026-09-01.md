# S1 — species mass on the 2D surface — Handoff (2026-09-01)

**For:** the checking agent.
**Plan:** `OVERLAND_TRANSPORT_HEAT_MSX_PLAN_2026-09-01.md` — D-2DT1, D-2DT2,
D-2DT3 and §2.2/§2.3 are what this implements. S2–S6 are NOT here.
**Implemented syntax-only.** `g++ -fsyntax-only -std=c++20 -fopenmp` (and
`-DOPENSWMM_HAS_2D` for the router): **0 errors**. Nothing built or run.
**This round edits the LTS marcher itself.** Read §1 before anything else.

```
new: src/engine/2d/data/SurfaceTransportState.hpp     (mass state + ledgers)
new: tests/unit/engine/test_2d_transport_s1.cpp        (6 gates)
mod: src/engine/2d/data/SurfaceStateData.hpp           (+transport member)
mod: src/engine/2d/solver/ExplicitInertialSolver.hpp   (+sacc_L_/sacc_R_, donorConc, sinkMassAtCellConc)
mod: src/engine/2d/solver/ExplicitInertialSolver.cpp   (fireFaces, fireCells, settleAccumulators, both lazy paths, BC outflow, exchange drain, advance reset, initialize)
mod: src/engine/2d/SurfaceRouter2D.cpp                 (sizes transport before solver init)
mod: tests/parity/run_corpus.sh                        (S1a: 2D bitwise script wired in)
mod: tests/unit/engine/CMakeLists.txt                  (test registered)
```

---

## 1. ⚠ Run the 2D regression net FIRST — it was not wired until this round

`UNIFIED_PLAN_STATUS` §8 recorded *"0 2D decks"* in the bit-identity corpus.
`tests/scripts/trackI_bitwise_regression.sh` (32 surface decks) existed and
was **never called by `run_corpus.sh`**. This round wires it in and gates the
corpus verdict on it.

**Why this is the first thing to do:** every change in this round is inside
`fireFaces`, `fireCells`, `settleAccumulators` and the two lazy-source paths
— the LTS marcher. Before S1a, "corpus green" would have meant "the 1D decks
are green" and said nothing about the code that moved. Now it says
something. **Run `run_corpus.sh base patched` and confirm the 2D section
executes and reports 32/32 identical** before reading further. If the 2D
script is not executable in your environment, the runner now says so and
exits 3 rather than passing silently.

**Every 2D deck in that script must be byte-identical.** No deck carries
`[POLLUTANTS]`, so `transport.n_species == 0`, `sacc_L_` is empty, and every
species branch is skipped on `!sacc_L_.empty()`. A movement here is a
hydrodynamics regression introduced by this round — most plausibly the
`settleAccumulators` edit (§3) — and it outranks every gate below.

## 2. What S1 does, and what it deliberately does not

| Does | Does not (and which phase does) |
|---|---|
| Carries per-cell species **mass**, species-major, for `[POLLUTANTS]` rows | Age / temperature / MSX rows — S4 |
| Donor-cell advection on the marcher's own face accumulators, on the face's tier | Second-order + limiter — S2 |
| Wet/dry: dry cells hold mass, take no flux, report 0 | — |
| Infiltration, BC outflow, coupling drain remove mass **at cell concentration** and **book it to a ledger** | Delivering the drain to the 1D node's `qual_mass_in` — S3 |
| Evaporation removes water and **no mass** (concentration rises) | — |
| Rain, BC inflow, coupling spill arrive at **zero concentration** | Their concentrations — S2 (rain, BC), S3 (coupling tuple) |
| `exch_mass[k*ns+s]` per coupling point per advance | Reading it into the node — S3 |
| Reported concentration `m/V` behind the dry threshold | HDF5 species variables, TIN exchange item — S1 owes the first; see §7 |

**Initial mass is zero.** There is no `[2D_INITIAL_QUALITY]` surface yet
(S2), so a deck with pollutants and a mesh starts clean and — with every
source at zero concentration — stays clean. **S1 is therefore invisible on
every existing deck by construction**, which is what lets the 32-deck net
above be the regression instrument. The gates drive the solver directly and
seed mass through the state.

## 3. The one design point that carries everything — D-2DT2

```cpp
// fireFaces, immediately after the volume booking:
const double dM = qn1 * ed.xi[e] * dt_f;      // FINAL qn1: post-Froude, post-share
facc_L_[e] -= dM;  facc_R_[e] += dM;
if (!sacc_L_.empty() && dM != 0.0) {
    const int donor = (dM > 0.0) ? a : b;
    for (s) { dMs = dM * donorConc(s, donor); sacc_L_[s*ne+e] -= dMs; sacc_R_[s*ne+e] += dMs; }
}
```

Species mass is booked **by the same writer, on the same face, in the same
substep, from the same final `qn1`** as the volume. It therefore inherits the
marcher's tier cadence rather than reproducing it — which is the entire
conservation argument, and why a separate species sweep was never an option.

`fireCells` gathers and clears the species side in the same CSR walk as the
volume side; `settleAccumulators` settles both; both lazy-source paths sink
species with the water they remove. **Read `settleAccumulators` carefully**:
it is the one place I added a species loop to a function whose header warns
that a stranded accumulator is realized as created volume. The species loop
sits **before** the volume early-out `if (pending == 0.0) continue;`, and
that ordering is load-bearing, not cosmetic: a cell whose incident faces
cancel in VOLUME (+dM in through one face, −dM out through another) has zero
pending volume but **nonzero pending species whenever the two faces' donor
concentrations differ**. An early-out on volume alone would strand that
species mass exactly the way the header describes for volume. Falsifier iv
in §4 is the probe, and §4 notes it may pass by luck.

### 3.1 The donor guard, and why it is `V > 0` and not the dry threshold

First draft guarded `donorConc` on `V > dry_depth · area`. **That was wrong at
the front.** A face fires when its *face* depth from the two heads exceeds
`dry_depth`, which can happen while the exporter's *volume* sits below
`dry_depth · area`. Under the depth guard that face exported water at zero
species, concentrating what stayed behind, and gate 1 would have failed
exactly where it looks hardest. The positivity share already caps every take
at `β·V`, so any `V > 0` may export and its concentration is `m/V`. The
**reported** concentration keeps the dry threshold — that is a display choice,
this is a flux. Caught by reasoning, not by a run; gate 1 is what would have
caught it by a run.

### 3.2 Sinks read concentration BEFORE the volume moves

`sinkMassAtCellConc` is called in `fireCells` **before** `state_->volume[i]`
is updated, in the BC block **before** `state_->volume[i] = v_new`, and in the
exchange block **before** `state_->volume[ci] -= Q·dt`. The mass removed is
`dv · c_old`, which is the mass the departing water was carrying. A call
placed after the volume write would remove `dv · c_new` and the two would
disagree by the step's own change. Worth a glance at all five call sites.

## 4. Validation protocol

1. **§1.** Corpus with the 2D section: 23/23 1D identical, **32/32 2D
   identical**. Anything else stops the round.
2. **Gates, in order:** run **gate 5 before gate 1**. Gate 5 is the
   single-tier control; if it fails, the defect is donor timing or
   mass/volume bookkeeping and gate 1's failure tells you nothing extra. If
   gate 5 passes and gate 1 fails, the defect is in the tier handling and
   nowhere else — that separation is the reason both exist.
3. `ctest -j8` ×3. Standing figure after PE is **200**; this adds 6 →
   **206**.
4. **Falsifier sweep:**

   | falsifier | expected |
   |---|---|
   | i. read `donorConc` AFTER `state_->volume` is updated in `fireCells` (move the sink calls below the volume write) | gate 4a's concentration assertion fails; gates 1/5 still pass (no sinks on a closed dam-break) — which is why gate 4 exists separately |
   | ii. book species in a separate pass after `fireFaces` returns, reading `q_[e]` | gate 1 fails at K=6, **gate 5 passes** — the tier-cadence defect D-2DT2 forbids, isolated by the control |
   | iii. restore the `dry_depth·area` guard in `donorConc` | gate 1 fails at the front; **predict first** whether gate 5 also fails (I expect yes, weaker) |
   | iv. drop the species loop from `settleAccumulators` | gate 1 fails only if a rebuild lands mid-cycle with species pending — **may pass by luck**; if it does, say so, because then §3's claim is untested by this suite |
   | v. index `sacc_*` as `[e*ns+s]` instead of `[s*ne+e]` (stride bug) | gate 3 fails, gate 1 passes (one species cannot see a stride) |
   | vi. make evaporation call `sinkMassAtCellConc` | gate 4b fails — evaporation must remove no mass |

5. **Record:** the corpus's 2D line verbatim, gates 5 and 1's outcomes in
   that order, falsifier ii and iv.

## 5. What I am least sure about

- **Gate 1's `1e-12` relative tolerance.** The property is exact in real
  arithmetic; in floating point `c0·V + Σ(c0·dM_k)` and `c0·(V + Σ dM_k)`
  differ by association round-off, ~1e-16 per substep. Over 300 s at a
  `dt0` of tens of ms that is thousands of substeps; a random walk lands
  near 1e-14, a worst case near 1e-12. **If the measured worst cell is in
  1e-12..1e-11, loosen to 1e-11 AND RECORD THE NUMBER** — do not loosen
  further, and do not loosen silently. Anything above 1e-10 is a defect,
  not round-off.
- **Gate 2's `downstream > 0.05·m0`.** An engineering guess that the front
  crosses `x = 4` in 240 s on this strip. If it does not, lengthen the run
  rather than lowering the bar.
- **Whether `n_pollutants()` is the right row count for S1.** The router
  sizes `transport` to `ctx.n_pollutants()` (0 under `IGNORE_QUALITY`). MSX
  species are excluded on purpose (S4); confirm nothing else in the 2D
  router assumed a species count of zero.
- **`settleAccumulators` ordering** (§3) — a reviewer's eye is worth more
  than mine here.

## 6. The falsifier that bites the *plan*

The plan's D-2DT3 says the uniform-concentration property *"catches a
tier-cadence mismatch, a donor-timing error and a volume/mass inconsistency
at once."* Falsifier i shows that is **not quite true**: a donor-timing error
in the SINKS is invisible to gate 1 because a closed dam-break has no sinks.
Gate 4 covers it. The plan text should say "catches a tier-cadence mismatch
and a face donor-timing error"; sink timing needs its own observer. I have
not edited the plan — the checker should, after confirming falsifier i
behaves as predicted.

## 7. Owed by S1, not by S2

- **HDF5 species variables.** S1 carries the state and reports nothing. The
  `Default2DOutputPlugin` needs one variable per species (`conc` behind the
  dry guard). Small; not done here because the output plugin's variable
  registration was not read this round, and guessing at it is how a
  variable comes to be written with the wrong shape.
- **`[2D_INITIAL_QUALITY]`** — technically S2's, but without it the only
  way to seed mass is the C++ state, so the deck-level gate cannot exist
  until it lands.
- **The plan edit in §6.**

## 8. Then S2–S5, unchanged from the plan

S2 dispersion + limited second-order + source concentrations; S3 the coupling
tuple (the `exch_mass` ledger is already the 2D→1D half of it); S4 age /
temperature / MSX rows with `HeatElemKind::CELL2D`; S5 per-cell surface heat.

---

# CHECK RECORD (2026-09-02, checking agent)

**VERDICT: VALIDATED AND COMMITTED.** Base `658d9a17`, lesson-222 rig
from the start (one dylib proven loaded via DYLD_PRINT_LIBRARIES).
Evidence: `tests/output/2d_transport_s1/PROVENANCE.txt`.

## The marcher code survived first contact unchanged

Both fixes landed in the round's own instruments, not in the physics:

1. **Gate 4a fixture arithmetic:** 1e-5 m/s × 600 s removes 1.2 % of a
   500 mm pond's mass; the gate demanded ≥ 5 %. The mechanism was exact
   (0.988·m0 to the digit). Rate → 1e-4.
2. **S1a's wiring was doubly broken (F2, REAL):**
   `trackI_bitwise_regression.sh` was **never committed** — a clean
   checkout reports UNCOVERED and exits 3 — and the runner resolved the
   script's path from `BASH_SOURCE` *after* changing cwd, so a relative
   invocation degraded to UNCOVERED even where the script existed. The
   script is committed with the round; resolution anchors on `$HERE`.
   **A clean checkout holds only 3 2D decks** — your "32" (now 35) lives
   in untracked scratch. Curating a tracked deck set is OWED.

## §1's requirement, met

Isolated A/B: 1D **23/23**, clean-checkout 2D census **2/2** (+1
by-design skip). Supplementary full census against the shared-tree
binary: **"G1: 34 identical, 0 differing, 1 skipped (of 35)"**
(verbatim; that binary also carries a peer's uncommitted edit-layer
code — not on the simulation path, and the byte-identity is itself the
evidence). Gate 5 then gate 1, both green first try; gate 1's 1e-12
held as written — §5's tolerance worry did not materialize.

## Falsifiers — the interesting ones first

- **ii took THREE formulations, and that is the record:** a separate
  species pass over the same firings reading the just-stored `q_[e]`
  books identical numbers — it *cannot* bite, because a "separate
  sweep" defect is a defect of FREQUENCY, not code location. Species
  booked only at global cadence fails gates 1 AND 5 (no separation —
  even K=1 runs are not all global steps). Species integrated with the
  wrong tier's dt (× 2^tier, identity at K=1) fails **gate 1 while
  gate 5 passes** — the exact separation the pair exists for. D-2DT2's
  discriminator is real, and that is its shape.
- **i predicted exactly** (gate 4 alone) — your §6 finding against the
  plan's D-2DT3 claim is CONFIRMED and the plan text is corrected.
- **iii: gate 1 ALONE.** Both of us predicted gate 5 would weakly fail;
  it does not — the starved-exporter state (face wet, volume below
  dry_depth·area) only arises under sub-tier firing. §3.1's defect is a
  tier phenomenon.
- **iv does NOT pass by luck** — gates 1 and 3 fail; rebuilds land
  mid-cycle with species pending on these decks, so §3's settle
  ordering IS tested by the suite.
- v → gate 3 alone; vi → gate 4 alone. Both exact.

## Figures

6/6 gates; ctest **188/188 ×3** (binaries: 187 + this suite — your
"200→206" counted gates); corpus as above.

## Review notes (§5's asks)

`settleAccumulators` ordering read and agreed (falsifier iv now proves
it observed). `n_pollutants()` is right for S1 — nothing else in the 2D
router consumes the state. One recorded nit: both lazy paths skip the
species sinks when `src == 0.0` exactly (a cell whose gross fluxes
cancel in fp would infiltrate unbooked mass) — measure-zero, and S2
restructures these blocks anyway.
