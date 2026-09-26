# G0 sign-off — recommendations for decision (2026-08-25)

**For:** C. Buahin. **This is a decision document, not a changeset.** G0's
plan was delivered 2026-08-15; sign-off on its §10 open decisions has been
owed **ten days** and gates all of Phase 4.

**Premises verified against the code, not the plan** — and one of them has
gone stale in a way that makes a decision materially cheaper than its own
risk register says. That is §1.

---

## 0. 🛑 RETRACTED 2026-08-26 — §1's re-rating was WRONG

**§1 below re-rated D-N1 from "risk" to "chore" on a list of four fixed
sites. There are eleven, and the two I missed are the dangerous ones.** Both
are on the GPU path, which I searched only for the clamp:

```
ExplicitKokkosSurfaceSolver.hpp:137   std::array<int, 9>  tier_off_{};
ExplicitKokkosSurfaceSolver.hpp:138   std::array<int, 9>  ftier_off_{};
ExplicitKokkosSurfaceSolver.hpp:149   std::array<long, 8> tier_occupancy_{};
```

`tier_off_`/`ftier_off_` hold **K+1 segment offsets**, so K = 8 exactly fills
9 elements. **Raising the ceiling to 9 tiers writes index 9 out of bounds** —
not a truncated telemetry counter, a buffer overrun in the GPU solver. And
`:149`'s occupancy is indexed **unguarded**, so it overflows where the serial
path merely truncates.

**§11's original rating was right and mine was wrong. D-N1 is a RISK.** The
approval stands — the ceiling is still arbitrary and still worth raising —
but it must be budgeted and gated as a shared-code change touching the GPU
solver, exactly as the plan said.

**§1's own hedge predicted this and I shipped the wrong rating anyway:**
*"treat this paragraph as a claim to be re-checked before anyone budgets on
it — a read of one site is not a reading of the chain."* Writing the caveat
is not the same as heeding it. **A hedge is not a substitute for the check it
describes** — and it is worse than no hedge, because it lets a claim ship
wearing the appearance of caution.

**Contributing cause, and it will bite others** — the shell's `grep`/`find`
here are wrappers that **silently skip gitignored paths**. My "eleven sites"
sweep was really "sites in files the wrapper chose to show me". Any count
taken that way is a lower bound of unknown depth.

§1 is kept below **unedited** as the retracted record. Do not cite it.

---

## 1. ⚠ [RETRACTED — see §0] D-N1 is much cheaper than the plan believes

**The plan's §11 lists "runtime tier count touches the surface solver's fixed
arrays" as a risk, and calls it "surgical but shared-code".** Measured today,
the expensive part does not exist:

| what | state |
|---|---|
| tier **cell lists** | `std::vector<std::vector<int>> cells_by_tier_` (`ExplicitInertialSolver.hpp:150`) — **already dynamic** |
| tier **edge lists** | `std::vector<std::vector<int>> edges_by_tier_` (`:151`) — **already dynamic** |
| tier **occupancy telemetry** | `std::array<long, 8> tier_occupancy_{}` (`:205`) — **the only fixed array** |
| CPU clamp | `std::clamp(opts.lts_tiers, 1, 8)` (`ExplicitInertialSolver.cpp:79`) |
| GPU clamp | `std::clamp(opts.lts_tiers, 1, 8)` (`ExplicitKokkosSurfaceSolver.cpp:230`) |
| parser | `SectionHandlers2D.cpp:193`, documented "1..8" at `SolverOptions2D.hpp:204` |

**So D-N1 is: one telemetry array → vector, two clamp bounds, one parser
bound, one doc comment.** The risky thing — the tier data structures — was
already written dynamically. Whoever wrote §11's risk entry was describing a
design that had changed by the time it was written, or was being cautious
without re-checking.

**Recommendation: APPROVE D-N1, and re-rate it from "risk" to "chore".** But
treat this paragraph as a claim to be re-checked before anyone budgets on it
— it is a code read by me, and this program's own lesson 152 is that a read
of one site is not a reading of the chain.

**⚠ And the protection §11 names does not exist in the form implied.** §11
says "gate 10's bitwise surface regression protects it". There is no bitwise
*surface* corpus deck — **`tests/parity/MANIFEST` has no 2D deck at all**
(19 decks, zero mesh). What exists is `test_2d_lts_equivalence.cpp`, with
`ConservationAtEveryTierCount`, `TierSolutionMatchesGlobalDt` and
`FinePatchInCoarseWatershedStable` (the last at `lts_tiers = 6`). Those are
good gates and they do cover the tier count — **but they are unit
conservation/equivalence gates, not bitwise regression**, and the distinction
is exactly the one this program has been bitten by repeatedly. **If D-N1
proceeds, either add a 2D deck to the corpus first or amend §11 to name the
gate that actually exists.**

---

## 2. The five new decisions

| # | Decision | Recommendation | Why |
|---|---|---|---|
| **D-N1** | runtime tier count (replace fixed 8-tier array) | **APPROVE** | §1 — near-trivial, and the 8-tier ceiling is arbitrary. Condition: settle the gate question in §1 first |
| **D-N2** | `C_GW` / `C_COL` defaults | **DEFER — do not sign off blind** | These are Courant-like safety factors for the lateral-Darcy and σ-column steps. **No value is defensible before the closures run**; the plan's own gates 4–5 are what produce the evidence. Ship G1 with a conservative pair, log the achieved Δt, and set the default from the measurement at step 18 alongside decisions 5/6/7/15, which are already deferred to that point |
| **D-N3** | capillary-diffusion term default **OFF** | **APPROVE** | Consistent with legacy behaviour (no such term), keeps G1's closures comparable to the reference, and it is an addition rather than a removal — the safe default for a term whose stability cost is unmeasured. Revisit if a benchmark demands it |
| **D-N4** | first-order upwind σ column in v1, MUSCL follow-up | **APPROVE** | Matches how the ARD track shipped (E1 tracer first, limiters later) and, more importantly, **first-order is the only variant whose conservation is easy to gate** — gate 5's oscillating-table test is far more valuable on a scheme you trust. Condition: record it as a *known accuracy limitation* in the migration guide, not as a silent choice |
| **D-N5** | GW GPU offload deferred | **APPROVE** | It is sequenced behind the surface-transport ABI extension (2D-S6) which is itself unstarted. Deferring a dependent of an unstarted step is not a decision so much as an acknowledgement |

---

## 3. The carried-over draft items

Most are already recommendations the plan made and nobody has contradicted.
My advice is to **sign off the block as recommended** rather than re-litigate,
with two exceptions called out.

| Draft # | Recommendation | Note |
|---|---|---|
| 1 (default soil char) | approve Russo | unchanged since draft |
| 2 (shipping list) | approve Russo+Gardner in G1/G2, BC+VG at step 13 | |
| 5, 6, 7, 15 (enslaving / αL auto-select / AUTO default) | approve AUTO, **thresholds confirmed at step 18 from measurement** | already correctly deferred — do not set numbers now |
| **8 (m defaulting → `M_LAYERS 8` + per-cell override)** | **approve, but see §4** | the re-posing is sound: σ removes the α-scaling rationale |
| 9 (Dunne default-on) | approve default-on with a compat flag | it *is* the mass balance; off-by-default would ship a knowingly incomplete water balance |
| 10 (Darcy default, `[GWF]` override wins) | approve | most-specific-statement-wins, consistent with how this program resolved the Z1 inflow/editor precedence question |
| **11, 12 (ET retirement, infil gate)** | approve **only with the migration guide as a hard gate**, not a follow-up | these change existing models' results. The plan already says "migration guide obligatory" — make it a merge blocker |
| 13 (centroid-in-polygon) | approve for v1 | |

---

## 4. Two things I would add to the sign-off

**(a) `M_LAYERS 8` collides with the tier ceiling in the reader's head.**
Draft 8 sets a default of **8** σ layers; D-N1 removes a hard ceiling of
**8** LTS tiers. They are unrelated quantities that will appear near each
other in every future discussion of this subsystem. **Recommend `M_LAYERS`
default 8 stands, but that the doc names it explicitly as unrelated to
`LTS_TIERS`** — this program has already lost time to two label collisions
(Phase-3 `S1–S7` vs snow, and `Y4` twice), and this one is cheaper to prevent
than to untangle.

**(b) Track I's two open items should be signed off in the same pass**, since
they are the only part of this plan family with shipped code:
- **`INFIL_STEP` default** — recommend `WET_STEP`, as drafted. A separate 2D
  default is a second cadence to explain and to get wrong, and no measurement
  yet argues for it.
- **The I8 double-counting overlap** — recommend **warning, not error**, as
  drafted. A meshed area overlapping an infiltrating subcatchment is a
  modelling choice that can be legitimate during migration; refusing it would
  block exactly the workflow the migration guide is meant to support. But the
  warning must name **both** the mesh region and the subcatchment, or it is
  the kind of message users learn to ignore.

---

## 5. What sign-off unblocks, and what it does not

**Unblocks:** G1 (plan steps 1–8) — closures A+B standalone, the σ-column ALE
gates 4–5, ET boundary, `PER_SUBCATCH` + node Darcy exchange. That in turn
un-defers **A3's and H5's groundwater sub-items**, which have been sitting on
"consume G1 if landed, else defer" since the water-age and heat rounds.

**Does not unblock, and should not be read as approving:** Phase 3 (`2D-S1…
2D-S7`) has no code and is not gated on this; **2D-S7 / T7 GW transport rides
G2**, which is far downstream; and none of this touches the Phase 2 question,
which the program review found is **refused rather than unstarted** and which
is a genuinely open strategic question about whether HydroCouple is still the
integration path.

**My honest read on sequencing:** signing G0 off is worth doing now because
it is free and it has been blocking cheaply. But I would **not** start G1
immediately. The engine currently has an unvalidated quality-ledger units
round in flight, `IO3`'s embedded-section round-trip is broken in a way users
can hit, and Phase 1's own API surface (E6, full A6, R5's Python/MCP halves)
is the largest user-visible gap. Groundwater is a large new subsystem; it
should start from a settled base.

---

## 6. ✅ ANSWERED — recorded 2026-08-25, user-approved

**Yes, C. Buahin signed off. All four questions were answered as
recommended**, in an interactive prompt during the session that produced this
document. The checking agent (2026-08-26) could not resolve this from the
tree and was **right to challenge it** — see the process defect below.

| question | answer given |
|---|---|
| D-N1 | **"Approve, add a 2D corpus deck first"** — the recommended option, including the condition |
| D-N2 | **"Defer to step 18, ship conservative"** — recommended |
| D-N3/N4/N5 + carried block | **"Approve all as recommended"**, including both attached conditions (migration guide as merge blocker; `M_LAYERS` documented as unrelated to `LTS_TIERS`) |
| Track I | **"INFIL_STEP = WET_STEP; overlap = warning"** — recommended |

### ⚠ The process defect the challenge exposed, and it is mine

**An interactive approval left no auditable trace.** I recorded the *outcome*
in the plan's §10 and the roadmap, but never closed the loop on **this
document — the one that asked the questions** — so it still ended with "What
I need from you". A reader comparing the two could not tell approval from
assumption, and every decision matched the recommendation verbatim, which is
exactly what an unapproved write-through would also look like.

**Worse, I used a convention the project does not use.** The established form
is Track I's `recorded 2026-08-20, user-approved`. I wrote
`(C. Buahin)` against a date instead — different enough to read as a
different kind of claim.

**Rule going forward: an approval is recorded where it was requested, in the
project's existing convention, before it is recorded anywhere else.** A
decision document that still asks its questions has not been closed, whatever
the downstream files say. **The challenge was correct and the sign-off should
not have been trusted from the tree alone.**

## 7. Superseded: what I originally needed from you

Four answers, and the first is the only one where I would push back on a
"yes":

1. **D-N1** — approve? (And do you want a 2D corpus deck first, per §1's gate
   finding?)
2. **D-N2** — accept my recommendation to **defer** to step 18 rather than
   set a default now?
3. **D-N3, D-N4, D-N5** and the §3 block — approve as recommended?
4. **§4's two Track-I items** — `INFIL_STEP = WET_STEP`, and overlap →
   warning?

Answer those and G0 closes. I will record the decisions in the plan's §10, in
the roadmap, and in `PROGRESS.md`, and the phase moves from 🔄 to ⬜ ready.
