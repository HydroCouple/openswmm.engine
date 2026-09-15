# E5b + IO3 Implementation — Validation & Commit Handoff (2026-08-17)

**For:** the checking agent (sandbox: `g++ -fsyntax-only` only; nothing
linked/executed).
**Base:** `cbb9d321` (post-E5a).
**Plans:** `EULERIAN_ARD_TRANSPORT_PLAN.md` §6 E5b;
`TRANSPORT_IO_PLUGIN_CONFIG_PLAN.md` IO3/IO5.
**Standing findings:** lessons 1–22 all apply; this handoff applies 21
proactively (the E5a TARGET_DX deferral gate is FLIPPED in this changeset)
and 20 (the treatment and ledger decks are the configurations these
features exist for).

---

## 1. Changeset (uncommitted)

```
mod:  src/engine/data/ArdConfigData.hpp
      (target_dx + detailed_output_path; any_engine_content() — the bypass
       predicate now spans dispersion/rows/TARGET_DX/DETAILED_OUTPUT)
mod:  src/engine/transport/components/EulerianArdComponent/ArdConfig.cpp
      (TARGET_DX parses — plan §8 resolved; DETAILED_OUTPUT parses,
       relative paths resolve against the CONFIG file's directory; unknown-
       key list updated; bypass warnings use any_engine_content)
mod:  src/engine/transport/components/EulerianArdComponent/ArdEngine.{hpp,cpp}
      (mesh options now CONVERT display→ft before buildNetworkMesh — this
       also fixes an E1-era LATENT SI DEFECT: raw ctx.options.fv was
       passed, so an explicit FV_CELL_LENGTH in metres meshed as feet,
       invisible to every CFS gate (the lesson-20 shape, §2.5);
       TARGET_DX overrides cell_length under non-FV routing, warns under
       FV; absorbTreatedNodeConc() — treated nodes ONLY (a blanket absorb
       breaks the conc→mass round trip bitwise); per-cell CSV sidecar
       (open at init, header + rows every routing step, open-failure is a
       warning); reactArdStage call gains cell_dx)
mod:  src/engine/transport/components/ReactionModule/ReactionArdBinding.{hpp,cpp}
      (kdecay stage books removed mass — cells a·dx·Δconc + stores Δmass —
       into qual_routing_reacted; signature gains cell_dx)
mod:  src/engine/quality/QualityRouting.hpp   (applyTreatment → public for
       the ARD interop; it books its own reacted losses)
mod:  src/engine/core/SWMMEngine.cpp          (B5: after ard_.step, if
       treatment.hasAny(): quality_.applyTreatment on the PUBLISHED
       nodes.conc, then ard_.absorbTreatedNodeConc)
mod:  src/engine/core/SimulationContext.hpp   (ProcessComponentSpec.
       resolved_config_path — set at resolution)
mod:  src/engine/plugins/ProcessComponentRegistry.cpp (sets it; spec loop
       now non-const)
mod:  src/engine/core/InpWriter.cpp           (IO3 carry-alongside: a
       RELATIVE config= is copied from resolved_config_path next to the
       written .inp when destinations differ; failures WARN, never fail
       the save; absolute paths keep the IO-4 rebase)
mod:  tests/unit/engine/test_ard_transport_bcs.cpp (lesson 21 applied IN
       the retiring changeset: the TARGET_DX deferral leg flipped to
       expect a successful open)
new:  tests/unit/engine/test_ard_e5b.cpp      (6 gates)
mod:  tests/unit/engine/CMakeLists.txt
```

All TUs pass `g++ -std=c++20 -fsyntax-only`.

## 2. Design decisions to review

1. **Treatment ordering under ARD** (documented difference): legacy applies
   treatment BEFORE decay inside the CSTR update; ARD applies it at END of
   step on the published concentrations, then absorbs into stores. Same
   expressions, same process variables (Cin from qual_mass_in/vol_in, HRT,
   depth, area), same reacted booking. Parity at the CSTR limit is
   therefore approximate in ordering — flag if you want a stricter
   arrangement recorded as future work.
2. **Absorb treated nodes ONLY**: conc→mass→conc round trips are not exact
   in floating point; a blanket absorb would silently perturb every
   untreated node. Gate 1's upstream EXPECT_EQ legs are the razor.
3. **Ledger scope**: ARD books REACTED (kdecay + treatment's own booking).
   Outflow was already booked engine-level (outfall inflow × published
   conc — runs under both engines); flood/seep/evap have NO write sites
   under either engine (pre-existing; §2.5). MSX species get no report
   ledger rows (the report is pollutant-shaped) — their accounting surface
   is the sidecar. The E5a promise to book the UNDELIVERED dry-cell
   source share is DEFERRED (recorded on the roadmap): it needs a
   per-source loss accumulator and, being MSX-only mass, has no report
   row to land in today — flag if you want it in the sidecar instead.
4. **Sidecar**: CSV, no new dependencies (the HDF5 idea from the plan is
   dropped — dependency-minimal per the D-R7 philosophy). Written EVERY
   routing step; cadence control can follow if sizes demand. Relative
   paths resolve against the config file's directory.
5. **TARGET_DX** (plan §8 resolved): transport-mesh cell length under
   non-FV hydraulics; ignored with a warning under FLOW_ROUTING FV.
6. **IO5 closed by review**: component apply hooks warn through
   ctx.warnings (in use since R1/E3 — the bypass warnings live there);
   record as the sanctioned sink, no code needed.

### 2.5 Pre-existing gaps observed, NOT fixed (CLAUDE.md §3)

- `applyDecay` (LEGACY, no reactions component) and R4's
  `decayPollutantsExact` (LEGACY + reactions) do NOT book
  qual_routing_reacted — decay mass is unattributed in the LEGACY report.
  Only treatment booked reacted before this changeset. ARD now books it;
  the LEGACY gap is recorded for a later fix so the two engines' reports
  can be compared like-for-like.
- qual_routing_flood/seep have no write sites under either engine.
- The E1-era raw-mesh-options SI defect is FIXED here (it sat directly on
  the line TARGET_DX had to touch); noted so the bit-identity check knows
  why SI decks with an explicit FV_CELL_LENGTH may legitimately differ.

## 3. Validation protocol

1. **Reconfigure** (one new test TU), build, zero new warnings.
2. `ctest -R test_engine_ard_e5b` — six gates — plus the FLIPPED
   TARGET_DX leg in `test_engine_ard_transport_bcs`.
   *Anticipated failure modes, likelihood order:*
   (a) **[TREATMENT] row format** `J2 TSS R = 0.5` — verify the parser's
   expected spelling (spaces around `=`); fix the deck, not the gate.
   (b) **Gate 2's 2% closure band** — init/final are booked from published
   conc × SOLVER volumes while ARD's internal mass is cell a·dx; the two
   volume books can disagree a little on the level pool. Probe the actual
   terms before touching the band; a measured, documented widening is a
   decision to record.
   (c) **Gate 1's upstream bitwise legs** — same J2-backwater caveat as
   E3's override gate (which held across 721 steps on this deck shape).
   (d) **Gate 3's FV leg** — FLOW_ROUTING FV on this deck must run under
   EULERIAN_ARD (E1 validated the projection under FV); if the FV solver
   refuses the deck, adjust the deck, the warning assert stands.
   (e) **Gate 5** assumes writeInpFile round-trips [PROCESS_COMPONENTS]
   with the relative path intact (IO1 behavior) — if emit_path_token
   rewrites it, compare against what was actually written before blaming
   the copy.
3. **Falsifier sweep** (verified restoration; record the table):

   | falsifier | expected failing gates |
   |---|---|
   | i. comment absorbTreatedNodeConc call in B5 | 1 (downstream separation vanishes — treatment evaporates at next publish) |
   | ii. absorb ALL nodes instead of treated-only | 1 (upstream EXPECT_EQ legs — the round-trip razor) |
   | iii. comment the reacted booking in reactArdStage | 2 (closure fails by ~init−final) |
   | iv. drop the TARGET_DX cell_length override | 3 (identical trajectories; FV warning leg still green — note the split) |
   | v. comment writeDetailRows | 4 (header-only file) |
   | vi. comment the InpWriter copy block | 5 |
   | vii. revert any_engine_content | 6 |
   | viii. drop the ucf division on target_dx | CFS gates blind (ucf=1) — CMS probe: measured mesh effect shifts ~3.28×; record measured values (lesson 22 shape: the conversion's only observer is this probe) |
4. **Prior suites all green** — E5a suite with its flipped TARGET_DX leg;
   E3/E4 suites; R-suites; FV suite. Decks without a transport.ard and
   with kdecay = 0 must stay BIT-IDENTICAL (mesh conversion is ×1 under
   CFS; ledger writes only fire with kdecay ≠ 0 or treatment). Sanitizers
   over the new suite.
5. **Bit-identity:** benchmark decks vs base; any SI benchmark with an
   explicit FV_CELL_LENGTH under EULERIAN_ARD may differ BY DESIGN (the
   fixed E1 defect) — record it if one exists.
6. **Ledger closure on a real deck** (the E5 plan-verify): pick one
   benchmark-style deck, run under LEGACY and EULERIAN_ARD, and record
   both continuity errors — ARD's should now be attributable (init −
   final − reacted − outflow ≈ external inflows), LEGACY's decay remains
   unattributed (§2.5). This is the like-for-like comparison baseline.
7. Append results to §5; commit with §4.

## 4. Commit message

```
feat(transport,io): treatment interop, ARD quality ledger, detail sidecar,
TARGET_DX, save-as config carry-alongside (E5b + IO3)

Treatment now runs under the ARD engine: the legacy evaluator executes on
the published node concentrations after the ARD step and the engine
absorbs treated nodes (ONLY treated nodes - a blanket absorb breaks the
conc->mass round trip) back into its stores; treatment books its own
reacted losses. The ARD kdecay stage books removed mass (cells a*dx*dC +
stores dM) into qual_routing_reacted, closing the quality continuity gap
under EULERIAN_ARD (LEGACY's unbooked decay is recorded as a pre-existing
gap). TARGET_DX resolves the plan section-8 open item: transport-mesh
cell length under non-FV hydraulics, warned-and-ignored under FLOW_ROUTING
FV; the same line fixes an E1-era latent SI defect (mesh options were
passed to buildNetworkMesh unconverted). DETAILED_OUTPUT writes a
per-cell/per-store CSV sidecar every routing step (no new dependencies -
the HDF5 idea is dropped). IO3: a save-as copies RELATIVE component
config files alongside the written .inp (resolved_config_path captured at
open); failures warn, never fail the save. IO5 closes by review:
ctx.warnings is the sanctioned apply-hook sink. The E5a TARGET_DX
deferral gate is flipped in this changeset (lesson 21). Gates:
tests/unit/engine/test_ard_e5b.cpp (6, incl. the treated-only round-trip
razor and the ledger-closure gate).

Plans: EULERIAN_ARD_TRANSPORT_PLAN.md section 6 E5b;
TRANSPORT_IO_PLUGIN_CONFIG_PLAN.md IO3/IO5.
Validation record: plans/transport/E5B_IO3_VALIDATION_HANDOFF_2026-08-17.md
```

## 5. Validation results

**Committed as `721ae60c`.** 4/6 delivered gates passed; the two failures were
both **gate-deck faults** (features tested in configurations where they
cannot act), diagnosed without weakening any metric. Validation also found
**one data-loss path in save-as** and **bounded two claims that the commit
message overstated**. Final: 7/7, full suite 137/138, bit-identity 14/14,
ASan/UBSan 0.

### 5.1 Gate 1 — treatment had nothing to act on

`§3.2(a)` guessed the `[TREATMENT]` row spelling. The row parses fine
(`treatment::parse("R = 0.5")` → 0, and `has_treatment[J2]` is true — my
first probe read that flag BEFORE `initialize()`, where `initQuality()`
compiles it, which is my error, not the engine's).

The real cause: a **removal** expression acts on the node's INFLOW
concentration. `applyTreatment` sets `cin = qual_mass_in / qual_vol_in` and
computes `cOut = (1−R)·cin`, falling back to the node's own concentration
when `cin` is 0. J2 has no external inflow — its water arrives through the
conduit — so `cin = 0`, `cOut = c_node`, and treatment removes nothing.

Verified against LEGACY, which shares the function: adding the J2 treatment
left **External Outflow identical at 12.119 lb**. So the feature was being
tested where the reference engine also does nothing. Deck fixed — the
treated node now gets its own lateral inflow carrying pollutant (`J2 FLOW 2`
+ `J2 TSS CONCEN 20`), present in BOTH the base and treated decks so the
comparison still isolates treatment.

### 5.2 Gate 3 — TARGET_DX 250 is a no-op by construction

`ard_config.target_dx` parsed correctly as 250, and the run came out
BIT-IDENTICAL. `FV_MIN_CELLS` is a **floor of 4 cells per conduit**:
`ceil(500/250) = 2 → max(2,4) = 4`, which is exactly what the default
(`cell_length 0 → 1 → max(1,4) = 4`) already produces. The key reached the
mesh builder and the builder correctly ignored it.

Deck fixed to `TARGET_DX 50` (`500/50 = 10`, clearing the floor). The floor
itself is pre-existing and documented in `FvOptions.hpp`; TARGET_DX inherits
it consistently, so nothing to change — but a user asking for a coarser mesh
than the floor allows gets silence, which is worth knowing.

### 5.3 DATA LOSS — save-as silently destroyed an unrelated file

The IO3 copy uses `copy_options::overwrite_existing`. Measured: a save-as
into a folder already containing a **different** `model.rxn` replaced it and
reported `warnings = 0`. The original content was gone.

Overwriting is *required* for the feature to be correct — re-saving into a
folder holding the last save's copy must refresh it, or the deck ships with a
stale config — so the defense is not "refuse" but "say so". Added a content
comparison: replacing an existing file with different content now warns,
while the ordinary idempotent re-save stays silent (verified both).

Gate 5 could not see this: it writes into a freshly created empty directory.
Added gate 7.

### 5.4 The reacted ledger does NOT close, and treatment's booking is
### step-dependent — two claims bounded

**(a) Treatment's `mass_lost` is not a mass.** `applyTreatment` computes
`mass_lost = (…)/dt` and accumulates it into `qual_routing_reacted`, which
the report prints in the same units as outflow and init/final — i.e. a mass.
Measured on one deck under LEGACY, varying only `ROUTING_STEP`:

| ROUTING_STEP | Mass Reacted |
|---|---|
| 5 s | 0.413 |
| 10 s | 0.105 |
| 20 s | 0.027 |

A mass-balance term must be step-invariant. This is **pre-existing legacy-path
code** — E5b only makes the function public — and it affects LEGACY reports
identically, so it is recorded, not fixed (CLAUDE.md §3): correcting it moves
every existing model's reported Mass Reacted and needs its own parity round
against the QA suite. But it means "treatment books its own reacted losses"
is true only in the sense that it books *something*.

**(b) The ARD ledger narrows the gap; it does not close it.** §3.6's
comparison on a flowing kdecay deck:

| engine | in | out | reacted | init | final | unattributed |
|---|---|---|---|---|---|---|
| EULERIAN_ARD | 22.474 | 6.776 | 10.093 | 3.491 | 2.592 | **6.504** |
| LEGACY | 22.474 | 9.785 | 0.000 | 3.491 | 3.502 | **12.678** |

Real and large — the booking accounts for ~61% of ARD's own loss and halves
the unexplained mass — but 6.5 of 22.5 inflow remains unattributed. Gate 2
passes because its level-pool deck has essentially no through-flow, so
outflow ≈ 0 and only init/final/reacted are in play; that is a much gentler
configuration than a flowing network. **The commit message was reworded from
"closing the quality continuity gap" to what was measured.**

Leading hypothesis for the residual, recorded rather than chased: the E2-era
store resync scales `node_mass_` down when a node's volume drops ("the mass
follows the water down") and that mass leaves unbooked. Naming it here so the
next phase has a starting point.

### 5.5 Falsifier sweep (`falsifiers.sh`, one case per invocation)

| falsifier | gates that fail | vs predicted |
|---|---|---|
| i. remove the absorb call | 1 | as predicted |
| ii. absorb ALL nodes | 1 (round-trip razor) | as predicted |
| iii. remove the reacted booking | 2 | as predicted |
| iv. drop the TARGET_DX override | 3 | as predicted |
| v. remove writeDetailRows | 4 | as predicted |
| vi. remove the InpWriter copy | 5, 7 | as predicted (+7) |
| vii. revert any_engine_content | 6 | as predicted |
| viii. drop the ucf division on target_dx | **none** | predicted empty — see below |
| ix. drop the replaced-a-different-file warning | **7** | new — gate 7 is its only observer |

Falsifier viii is a **legitimately empty row**, predicted as such by the
handoff: every gate deck is CFS, where `ucf = 1`. The substitute observer it
asks for, measured (`si_probe.log`):

| deck | requested | converted | cells WITH | WITHOUT | ratio |
|---|---|---|---|---|---|
| CFS | 50 | 50.000 ft | 20 | 20 | 1.00× |
| CMS | 50 | 164.042 ft | 20 | 66 | **3.30×** |

That 3.30× is also the size of the E1-era latent defect this changeset fixes
(identical figures for `FV_CELL_LENGTH` on a CMS deck). No gate covers it;
the probe is the record.

### 5.6 Suites, parity, sanitizers

- **7/7** E5b gates; **10/10** E5a suite with its flipped TARGET_DX leg;
  **11/11** E3; full suite **137/138** (only the known pre-existing
  `FvEngine.RefiningTheMeshConvergesTowardTheDynwaveHydrograph`).
- **Bit-identity 14/14** vs a `cbb9d321` worktree build. §3.5's SI caveat does
  not arise: the three benchmark decks carrying an explicit `FV_CELL_LENGTH`
  (`cfl_0.1`, `cfl_0.25`, `cfl_clamp`) are all US-unit, so the mesh
  conversion is ×1 on every one of them.
- **ASan + UBSan**: 0 findings across the E5b, transport-BC and dispersion
  suites (28 tests).

### 5.7 Design points (§2)

- **§2.1 treatment ordering** — accepted as delivered and documented. Worth
  recording as future work only if a LEGACY-vs-ARD treatment parity gate is
  ever wanted; the ordering difference is real but second-order next to
  §5.4(a), which affects both engines.
- **§2.3 undelivered dry-cell source share** — still deferred, and §5.4(b)
  strengthens the case for putting it in the sidecar rather than the report:
  the report's ledger has a larger unexplained term than that share.
- **§2.6 IO5 closed by review** — agreed, `ctx.warnings` is the sink; this
  changeset's own new warnings use it.

### 5.8 Left alone

- `applyTreatment`'s step-dependent booking (§5.4(a)) — pre-existing, both
  engines.
- LEGACY `applyDecay` / R4 `decayPollutantsExact` still book nothing
  (§2.5) — confirmed by the 12.678 residual above.
- `qual_routing_flood` / `seep` have no write sites under either engine.
- `SimulationContext::reset()` still does not clear `ctx.reactions`
  (carried from E3/E4/E5a, for IO5).
