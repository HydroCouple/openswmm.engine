# E5a Implementation — Validation & Commit Handoff (2026-08-17)

**For:** the checking agent (sandbox: `g++ -fsyntax-only` only; nothing
linked/executed).
**Base:** `4df5cc0f` (post-E4/R6).
**Plan:** `EULERIAN_ARD_TRANSPORT_PLAN.md` §3.3/§4/§6 E5 — the CONFIG/
TRANSPORT half. **E5 is split** (recorded scope decision): E5a =
boundaries + sources + full [TRANSPORT_OPTIONS] keys (the nh2cl unblock);
E5b = treatment interop + mass-balance ledger rows + detailed sidecar,
next shot with IO3. The split follows the R3 lesson — the two halves are
different risk classes, and bundling them one-shot invites a 0/N round.
**Standing findings:** all of lessons 1–19 apply; especially 9 (wet decks),
10 (bypass both directions), 14 (loop-narrowing sweeps invariants), 15
(symmetric observers for new row classes).

---

## 1. Changeset (uncommitted)

```
mod:  src/engine/data/ArdConfigData.hpp
      (ArdTransportRow raw rows — stored UNRESOLVED at apply because the
       reactions component may apply before OR after transport.ard;
       resolved hot arrays bc_*/src_*; kLitersPerFt3; any_transport_rows())
mod:  src/engine/transport/components/EulerianArdComponent/ArdConfig.{hpp,cpp}
      ([TRANSPORT_BOUNDARIES]/[TRANSPORT_SOURCES] parsing (syntax+numeric
       at apply; VALUE|TIMESERIES); SCALAR_SCHEME/LIMITER keys write the
       internal fv fields (model.ard wins over [OPTIONS] FV_* — applies
       after the options parse; plan §4 alias semantics); TARGET_DX still
       defers naming plan §8 + E5b; bypass warnings extended to
       boundary/source content; NEW resolveArdTransportRows(): post-apply
       name resolution — nodes, conduits, MSX species (pollutants refused
       naming the legacy loading pathways), timeseries via
       tables.find_by_kind; duplicates refused; source VALUE rates convert
       species-mass/s → internal conc·ft³/s via kLitersPerFt3)
mod:  src/engine/core/SWMMEngine.cpp
      (resolveArdTransportRows called AFTER resolve_process_components +
       the embedded reactions fallback, into the same fatal errs channel)
mod:  src/engine/transport/components/EulerianArdComponent/ArdEngine.{hpp,cpp}
      (init maps resolved rows onto the mesh: bc species row = np + msx,
       source conduit row + length; updateTransportRows() evaluates
       VALUE/timeseries once per routing step
       (table_tseries_lookup_cursor at ctx.current_date, negative → 0);
       substep 5b: BC mass += load_frac·qual_vol_in·c_now (internal store
       mass is conc·ft³ — no conversion); substep 5c: source Δconc per
       cell = r·dt_sub/(L·a), dx cancels; DRY cells skipped — undelivered
       share documented, ledger booking arrives with E5b)
new:  tests/unit/engine/test_ard_transport_bcs.cpp   (8 gates)
mod:  tests/unit/engine/CMakeLists.txt
```

All TUs pass `g++ -std=c++20 -fsyntax-only`.

## 2. Design decisions to review

1. **BC semantics:** the node's TOTAL external inflow volume (qual_vol_in —
   the same water the pollutant loaders integrate) carries the species at
   the row's concentration. Per-pathway BCs (washoff vs DWF vs direct) are
   not distinguished; flag if you want that recorded as a future split.
2. **Pollutants refused** in both sections: their loading surface is the
   legacy pathways, and allowing them here would create a double-count
   ambiguity E5a refuses to have. The error names [INFLOWS].
3. **Order-independence by construction:** rows are stored raw at apply and
   resolved after ALL components ran — gate 6 lists transport.ard FIRST so
   the species is referenced before its declaring component applies.
4. **Source units:** rows and timeseries carry species MASS per second
   (MG → mg/s etc.); internal MSX store mass is conc(mass/L)·ft³, so rates
   divide by kLitersPerFt3 = 28.316846592 exactly once (VALUE at
   resolution, TIMESERIES at evaluation). Gate 3's steady state
   Δc = r/(L/ft³·Q) is the analytic units gate — a dropped conversion
   misses by 28×.
5. **Dry-cell source share is NOT delivered** (no water to dissolve into);
   E5b's ledger will book the undelivered remainder. A once-per-run
   warning was considered and rejected as noise — flag if you disagree.
6. **SCALAR_SCHEME/LIMITER as aliases** of the internal FV fields, model.ard
   winning over [OPTIONS] (applies later). FV_DISPERSION stays warn-only
   (your E3 call); TARGET_DX stays open (plan §8).

## 3. Validation protocol

1. **Reconfigure** (one new test TU), build, zero new warnings.
2. `ctest -R test_engine_ard_transport_bcs` — eight gates.
   *Anticipated failure modes, likelihood order:*
   (a) **[TIMESERIES] time axis** — gate 2 assumes time-only rows ("0:19")
   land on the simulation date axis the same way [INFLOWS] series do
   (table_tseries_lookup_cursor at ctx.current_date). If gate 2 never
   rises while gate 1 passes, the ts x-axis convention is the suspect —
   check how TimeseriesHandler stores time-only rows before touching the
   engine.
   (b) **Gate 3's 15% band** — the steady flow at C5 may not be exactly
   5 cfs (DYNWAVE losses/backwater). Probe the actual flow; normalizing by
   measured Q is legitimate, widening the band is a decision to record.
   (c) **Gate 1/6 front timing** — same flush physics E4's dilution gate
   validated; if the rise misses 0.8·8, measure the head-store first.
   (d) **Gate 7's UPWIND-vs-MUSCL separation** — if below the 0.005 floor,
   check the front is still in transit at C5 during the window (a fully
   flushed chain shows no scheme difference); shortening END_TIME is the
   deck fix.
3. **Falsifier sweep** (verified restoration; record the table):

   | falsifier | expected failing gates |
   |---|---|
   | i. comment substep 5b (BC injection) | 1, 2, 6 (chain stays clean) |
   | ii. comment substep 5c (source injection) | 3 |
   | iii. break ts evaluation (always return bc_value_) | 2 (never falls); 1 unaffected |
   | iv. resolve rows at APPLY time instead of post | 6 (ard-first deck fails to open) |
   | v. drop the pollutant refusal | 4 |
   | vi. drop the kLitersPerFt3 division | 3 (misses by ~28×) |
   | vii. drop the scheme-key writes | 7 (trajectories identical) |
   | viii. revert the bypass-warning extension | 8 |
4. **Prior suites all green** — E3/E4 ARD suites (decks without
   boundary/source rows: the new substep loops are EMPTY, updateTransport-
   Rows is gated on non-empty rows; bit-identity should hold — verify with
   the sha256 discipline), R4/R1–R3 suites, FV suite. Sanitizers over the
   new suite.
5. **Bit-identity:** all benchmark decks (none has a transport.ard with
   boundary/source rows) vs base.
6. **nh2cl network parity vs EPANET-MSX** — the inlet BC now exists
   ([TRANSPORT_BOUNDARIES] at the head node, QUALITY_SOLVER EULERIAN_ARD).
   Remaining blocker from your E4 record was a runnable EPANET-MSX. If you
   can obtain/build one, run the comparison and record tolerances; if not,
   record precisely that the ENGINE side is no longer the blocker.
7. Append results to §5; commit with §4.

## 4. Commit message

```
feat(transport): [TRANSPORT_BOUNDARIES]/[TRANSPORT_SOURCES] + full
[TRANSPORT_OPTIONS] keys (E5a)

MSX inlet boundaries (VALUE|TIMESERIES per node: the node's external
inflow water carries the species at the given concentration) and
distributed conduit sources (species mass/s, converted once to internal
conc*ft3/s, spread over the conduit's wet cells) now configure through
model.ard and run in the ARD engine. Rows are stored raw at component
apply and resolved AFTER all process components ran, so transport.ard
and the reactions component may appear in either order. Pollutant rows
are refused with a precise error naming the legacy loading pathways
(no double-count ambiguity). SCALAR_SCHEME/LIMITER land as model.ard
aliases of the internal FV transport fields (model.ard wins over
[OPTIONS] FV_*); TARGET_DX remains an open-item deferral (plan section 8).
Bypass warnings extend to boundary/source content. E5 is SPLIT: E5b
(treatment interop, mass-balance ledger rows, detailed sidecar) + IO3
follow separately. Gates: tests/unit/engine/test_ard_transport_bcs.cpp
(8, incl. the analytic steady-state units gate dc = r/(L-per-ft3 * Q)
and the component-order-independence razor).

Plan: EULERIAN_ARD_TRANSPORT_PLAN.md section 3.3/4/6 E5 (E5a half).
Validation record: plans/transport/E5A_VALIDATION_HANDOFF_2026-08-17.md
```

## 5. Validation results

**Committed as `cbb9d321`.** All eight delivered gates passed on arrival — and
that again said less than it looked like. Validation found **one production
defect** (the headline feature dead on the exact model shape E5a exists to
enable), **one prior-suite regression the handoff did not flag**, and **two
gate-teeth holes**. Final: 10/10, full suite 136/137, bit-identity 14/14,
ASan/UBSan 0.

### 5.1 PRODUCTION DEFECT — boundaries deliver nothing on an MSX-only model

A `[TRANSPORT_BOUNDARIES]` row injects `qual_vol_in × concentration`.
`qual_vol_in` is accumulated by the QualitySolver external-load loaders —
and every one of them opens with `if (np <= 0) return;`. On a model with no
`[POLLUTANTS]` the volume is never assembled, so `vol_ext` is 0 every substep
and **the boundary delivers exactly nothing**.

Measured, same deck, same BC (`J0 X VALUE 8`), only the pollutant row
differing:

| deck | peak X at C5 |
|---|---|
| with one inert `[POLLUTANTS]` row | 7.999999 |
| MSX-only (no `[POLLUTANTS]`) | **0.000000** |

That is the nh2cl shape. An MSX network with an inlet boundary and no legacy
pollutant is the canonical EPANET-MSX model, and unblocking exactly that
comparison is E5a's stated purpose (§3.6) — so the feature was dead on its
own target configuration. All eight gate decks pass `pollutants = true`,
which is why nothing saw it.

Fixed by relaxing the six loader guards to a shared predicate,
`loadersNeeded(np, ctx) = np > 0 || ardBoundariesNeedExternalVolumes(ctx)`.
The mass loops are already no-ops at `np == 0` (`for p < np`), and
`qual_vol_in` is sized per NODE, not per node-pollutant, so running the
loaders there is safe — verified before changing anything.

`applyTreatment` shares the `np <= 0` shape and was deliberately NOT relaxed:
treatment is pollutant-only and treatment interop is E5b's scope.

This is E4/R6's lesson recurring one layer out. There it was a clamp swept
along with a stride narrowing; here it is a volume accumulation sharing a
guard with a mass accumulation. **When one guard protects two things, ask
which of them the guard's condition is actually about.**

### 5.2 PRIOR-SUITE REGRESSION — E3's deferral cases, unflagged

`test_engine_ard_dispersion` failed. Two cases in
`ConfigErrorsArePreciseAndNeverHalfApply` assert the E5 deferral errors for
`SCALAR_SCHEME` and `[TRANSPORT_BOUNDARIES]` — **which E5a implements**, so
those opens now succeed. Correct and intended, but §1 lists the E5a changes
without noting that a prior suite encodes the old behaviour, and §3.4 asks
for prior suites green. E4/R6 handled the equivalent situation explicitly by
flipping R4's gate 10; this one was missed.

Retargeted rather than deleted, because both cases carry the never-half-apply
assertions and need a good row parsed BEFORE the failing one:
`SCALAR_SCHEME BOGUS` (invalid value) and a malformed `[TRANSPORT_BOUNDARIES]`
row (wrong token count), each after a valid `DISPERSION 5`. Semantic coverage
of the new sections lives in the E5a suite.

### 5.3 Gate 7 could not support its own failure message

`SchemeKeysConfigureTheEngine` set `SCALAR_SCHEME UPWIND` **and**
`LIMITER SUPERBEE` on one deck and asserted a single separation. Either write
alone produces it, so neither key had an observer of its own — removing the
SCALAR_SCHEME write left the gate green (falsifier vii) because SUPERBEE was
still reaching the reconstruction, while the failure message says
"SCALAR_SCHEME UPWIND left the trajectory identical to MUSCL". Split into two
comparisons against a shared baseline, one key each; falsifiers vii and xi
now fail it independently.

### 5.4 The TIMESERIES source conversion had no observer

The species-mass/s → internal conc·ft³/s division happens in **two** places:
at resolution for VALUE rows, at evaluation for TIMESERIES rows. §2.4 calls
gate 3 the analytic units gate, but gate 3 only exercises VALUE — dropping
the timeseries division would have passed the entire suite. Added gate 10: a
constant TIMESERIES source must equal the identical VALUE source. Measured
agreement is exact (rel. diff 0.0), and falsifier x now fails it.

### 5.5 Falsifier sweep (`falsifiers.sh`, one case per invocation)

| falsifier | gates that fail | vs predicted |
|---|---|---|
| i. remove BC injection (5b) | 1, 2, 6, **9** | predicted 1,2,6 |
| ii. remove source injection (5c) | 3, **10** | predicted 3 |
| iii. break BC timeseries evaluation | 2 | as predicted |
| iv. resolve rows at apply time | 1, 2, 3, 4, 5, 6, 9, 10 | predicted 6 — see note |
| v. drop the pollutant refusal | 4 | as predicted |
| vi. drop kLitersPerFt3 (VALUE path) | 3, 10 | as predicted |
| vii. drop the SCALAR_SCHEME write | 7 — **only after §5.3** | predicted, unobservable as delivered |
| viii. revert the bypass-warning extension | 8 | as predicted |
| ix. re-narrow the loader guards | **9** | new — gate 9 is its only observer |
| x. drop kLitersPerFt3 (TIMESERIES path) | **10** | new — gate 10 is its only observer |
| xi. drop the LIMITER write | 7 | new — split out of vii |

Falsifier iv fails far more than the predicted gate 6 because removing the
resolution call leaves every row unresolved, not just the order-dependent
one. That is over-coverage, not a hole — gate 6 remains the only gate that
distinguishes resolution TIMING (component order) from resolution existing at
all, which is what it claims.

### 5.6 Suites, parity, sanitizers

- **10/10** E5a gates; **11/11** E3 suite after §5.2; full suite **136/137**
  (only the known pre-existing
  `FvEngine.RefiningTheMeshConvergesTowardTheDynwaveHydrograph`).
- **Bit-identity 14/14** vs a `4df5cc0f` worktree build. The loader-guard
  relaxation is inert on every benchmark deck: all have pollutants, so
  `np > 0` short-circuits the predicate before it looks at anything else.
- **ASan + UBSan**: 0 findings across the transport-BC, dispersion and ARD
  reaction suites (30 tests).

### 5.7 §3.6 nh2cl — the engine side is genuinely unblocked

Recorded as a demonstration, not a parity claim. The E4 record named two
blockers; E5a removes the first (no inlet BC). Running the nh2cl kinetics
with `[TRANSPORT_BOUNDARIES] J0 NH2CL VALUE 2.0 / J0 TOC VALUE 4.0` under
EULERIAN_ARD:

| link | NH2CL | TOC |
|---|---|---|
| C1 | 1.90227 | 3.98105 |
| C2 | 1.74762 | 3.94932 |
| C3 | 1.60580 | 3.91787 |
| C4 | 1.47442 | 3.88635 |
| C5 | 1.35840 | 3.85631 |

Monotone down the chain, bounded by the inlet, and TOC decays far more slowly
than NH2CL as `kd = 0.15` vs `k1 = 1.3` requires. The deck shape the
comparison needs now runs end to end.

**The second blocker stands**: there is no runnable EPANET-MSX on this
machine (only the 2.0 user-manual PDF), so there is still nothing to compare
against. Obtaining or building a reference is now the ONLY thing between here
and the parity verify — worth stating plainly, since E5a was justified partly
as the unblock and only half the blockage was E5a's to remove.

### 5.8 Design points referred back (§2)

- **§2.1 per-pathway BCs** — left as delivered (the node's TOTAL external
  inflow carries the species). Worth recording as a future split: a model
  with both DWF and storm inflow at one node cannot give them different
  influent concentrations today.
- **§2.5 dry-cell source share** — agreed, no warning. A source into a dry
  conduit having nowhere to go is a modelling condition, not an error, and
  the E5b ledger will make the undelivered mass visible as a number rather
  than as log noise.
- **§2.6 TARGET_DX still deferred** — its error is gated (E5a gate 7).

### 5.9 Left alone

- `ArdEngine::init` silently `continue`s past boundary/source rows whose
  resolved indices fall outside the mesh (`bc_node >= nn`, no mesh conduit
  row for a source link). Unreachable given resolution validates names
  against the same context, and no gate can observe it; noted rather than
  guarded (CLAUDE.md §2).
- `updateTransportRows` leaves the rate at 0 when a timeseries index is out
  of range — same class, same reasoning.
- `SimulationContext::reset()` still does not clear `ctx.reactions` (carried
  from E3 §5.9 / E4 §5.9, for IO5).
