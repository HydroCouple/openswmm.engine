# A4 (LID Layer Water Age) — Implementation Brief

**For:** the implementing agent, then the checking agent.
**Base:** `b5be8ec3` (post-A3).
**Plan:** `WATER_AGE_TRACKING_PLAN.md` §7 A4 — *"LID layer age (generic
per-layer species block — heat reuses it)"*.
**Standing findings:** lessons 1–68 in `IMPLEMENTATION_ROADMAP.md`. Read
59–68 before writing a gate; five of them are about gates that could not
fail.

**Why A4 matters beyond itself:** it is **H5's blocker**. H5 (watershed + LID
temperature) lists deps "H1, A4 block", and A4 builds the per-layer species
block that *heat reuses*. Build it once, for both.

---

## 1. Survey result — and it inverts A3's central mistake

A3's §4.1 deferred an exact mixing volume on the premise that the solver did
not publish per-compartment fluxes. **That premise was false, and it cost the
phase a 6.3× error.** So this survey led with the same question, and the
answer here is unambiguous.

**The LID solver publishes its inter-layer fluxes as stored per-unit fields.**
`LIDGroupSoA` (`src/engine/hydrology/LID.hpp:83`) carries:

| purpose | field | note |
|---|---|---|
| layer water state | `surf_depth`, `pave_depth`, `soil_moist`, `stor_depth` | per unit |
| **inter-layer flux rates** | `f_old_surf`, `f_old_soil`, `f_old_stor`, `f_old_pave` | **stored** — written at `LID.cpp:824` after `f_new` is computed; read back at `:792` |
| unit inflow | `inflow` (ft/s, set before `execute()`) | |
| outflows | `surface_runoff`, `drain_flow`, `evap_loss`, `infil_loss` | |
| volume balance | `wb_inflow`, `wb_evap`, `wb_infil`, `wb_surf_flow`, `wb_drain_flow`, `wb_init_vol`, `wb_final_vol` | cumulative, ft |

**So an exact volume-mixed per-layer age is possible. Do not ship a net-gain
estimate.** If the implementing agent finds itself writing
`max(0, v_new − v_old)`, stop — that is A3's defect, and the fields above are
why it is avoidable.

**Indexing:** LID units are grouped BY TYPE, not held in one flat list —
`LIDManager::group(int type_index)` (`LID.hpp:194-195`) returns an
`LIDGroupSoA` per type. Iteration is therefore two-level (type, then unit
within the group). Confirm the type count and the group→subcatchment mapping
before writing the loop; the survey did not pin those.

**Not yet established (finish the survey first):**
- Whether absent layers are zero-thickness or skipped (`*_thick == 0` is the
  likely tell — verify, do not assume).
- `lid_.storedVolume()` (`SWMMEngine.cpp:~3670`) — what it sums over.
- Whether any **per-layer** quality state exists, or whether LID quality is
  only the lumped `ctx.subcatches.lid_drain_qual_load`. A3's analogue
  (`ponded_qual`) was the structural model to copy; find A4's.

## 2. Decisions to put to the user BEFORE writing code

A3 set the precedent that these are user calls, not implementer defaults.

1. **Granularity.** One age per LAYER per unit (surface/pavement/soil/
   storage/drainmat), matching the four depth fields plus drainmat? A3 chose
   per-subarea over lumped for the same reason — the depths already exist
   separately. **But A4 must also serve heat (H5)**, so ask whether heat
   wants the same granularity, because building it twice is what the plan's
   own rows warn against.
2. **Drain outflow age.** LID drain flow reaches `lid_drain_runon_cfs` and a
   node. Does it leave at the storage layer's age, or a volume-weighted mean
   of the layers contributing that step?
3. **Hotstart.** A3 deferred. Check whether LID layer depths are in the
   hotstart (A3's were not — that was the reason to defer). Same reasoning
   should apply, but verify rather than inherit.

## 3. Implementation notes carried from A3's round

- **`WaterAgeState::resize` no longer has a defaulted parameter** — the `= 0`
  was removed precisely because it was a trap (lesson 66). Adding LID rows
  means extending the signature again. **Do not add a default.** Make a
  short-argument call a compile error, and prefer that over writing a gate
  to watch for a wipe.
- The RATE convention: donors accumulate `q · age`, the consumer divides by
  the flow rate, the accumulator is zeroed after consumption. Follow
  `subcatch_runon_age_vol_in` exactly.
- LID sits **inside** a subcatchment, so A3's `subcatch_runoff_age` and the
  LID drain age must compose coherently. State which one the outlet node
  sees, and gate it.

## 4. Gate requirements — written against the failures of the last four rounds

Every gate must satisfy these before it is allowed to assert anything.

1. **Reachability (lesson 59).** A gate for a branch needs a deck that
   *reaches* the branch. A4's branches include: a unit with a drain, a unit
   without, a layer present vs absent, drain flow routed to a node vs to a
   subcatchment. Name in the file comment what each deck must produce.
2. **Regime, not existence (lesson 65).** A3's setup assertion checked
   `runoff > 0` — true and useless, because the deck rained 5 of 60 minutes
   and every gate watched a draining surface. For A4: assert the LID unit is
   *receiving* water for the duration the claim needs, and check the
   magnitude (`wb_inflow`) not just its sign. **An `INTENSITY` gage reads one
   value per interval — a series needs one entry per interval.**
3. **Magnitude, not direction (lesson 61).** "The age changed" is satisfied
   by a mixing volume twice or half correct. At least one gate must compare
   against an analytic value — a single-layer unit under steady inflow is a
   complete-mix tank with age `V/Q`, which is the A3 analogue that exposed
   the 6.3× error. Choose numbers so no wrong arithmetic lands on the right
   answer (lesson 26).
4. **Prefer unrepresentable over observed (lesson 66).** If a hazard cannot
   be made to fail a gate, change the types so it cannot compile.
5. **Falsifier table with predictions**, and for each: if it fails nothing,
   say so plainly rather than recording the gate as covering it.

## 5. Verification protocol for the checking agent

1. **Greps first.**
   - `grep -rn "water_age_state.resize\|ws\.resize(" src/engine/` — every
     call site must pass the full argument list. A3's round found **six**
     sites where the handoff claimed four.
   - `grep -rn "A4" tests/ src/engine/` — A4 retires no deferral of its own,
     but H5's blocker text and any "arrives with A4" message must be swept
     (lesson 58 — this has caught a stale **name**, not just a message,
     three phases running).
2. **The corpus caveat (from A3's round).** The 14/14 bit-identity set
   contains **no deck with `WATER_AGE` on**, so "14/14 unchanged" cannot
   observe any age reporting change. **Adding one age-enabled deck to that
   corpus is owed work and A4 is a reasonable place to do it** — it would
   give the reserved columns their first regression net.
3. **Record:** whether the exact per-layer mixing was achievable from the
   published fluxes (§1's claim), the analytic comparison from gate
   requirement 3, and any falsifier that fails nothing.

## 6. Also owed, and unrelated to A4's features

Small, in the same code, cheap to fold in:

- `postOutputSnapshot`'s comment still promises the subcatchment age column
  "stays 0 here", directly above the code A3 made fill it.
- **`WATER_AGE_SNOW` (plan §8) is untouched AND undeferred** — melt water is
  treated as rain-aged and no error names the decision. That is a gap in the
  deferral discipline, not just in features.
- `DryElementHotstartCarriesTheAgedState` has been working-tree-only for six
  rounds; the water-age state/report separation remains unobserved.
- H2's owed top-width gate, and H4's owed G-UT3 (CSH parity, which needs the
  time-varying radiative forcing H3 deferred).
