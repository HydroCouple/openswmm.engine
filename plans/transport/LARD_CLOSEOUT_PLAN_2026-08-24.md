# LARD Closeout Plan — What Is Actually Left (2026-08-24)

**Context.** The expedite subplan
(`LARD_AGE_EXPEDITE_SUBPLAN_2026-08-23.md`) and its
**Amendment 1 / D-Y4** (`AMENDMENT_1_WATER_AGE_AS_SPECIES_2026-08-23.md`)
are **both closed**. Verified in source, not from commit messages:

| Track | Landed |
|---|---|
| Engine | X1 `24602eb2` · X2 `8c141a5e` · X3a `647a3603` · X3b `b9852cee` · X4 `9f155227` · X6 `d79c8bcf` · X5 `d7b6c079` · Y0 `948b2840` · **Z1 `4639be37`** · **H1 `d80bba34`** |
| GUI | Y1 `ebf28ae` · Y2a `dcc20e6` · Y3 `f5e0d9b` · Y3b `bc4e07c` · **Y2b-1 `dae4bad`** · **Y2b-2 `7a5f732`** · **Y2b-3 `9e63357`** · **Y4 `94ff3b5`** |

This document covers only what remains. Items are ordered by **risk×reach**,
not by plan lineage. Sizes are in *rounds* (one implement→validate loop).

---

## P0 — Release hygiene (do first; cheap, and everything else compounds on it)

### P0.1 Push. **13 commits are unpushed** (≈5 engine, ≈8 GUI)

Every validating round this session ended "nothing pushed," which reads as
a deliberate convention rather than an oversight — **confirm that intent
before pushing**, then push both repos. Two of the older engine commits
move hydrology on every snow deck, so the longer this sits, the larger the
integration surface for whoever pulls next.

*Verify:* both repos report zero ahead of upstream; CI green on both.
*Size:* well under a round.

### P0.2 CHANGELOG — required by CLAUDE.md §5.2

> **DONE 2026-08-25** — engine `2ecdef8a` (LARD Added entry + H1 Fixed
> entry), gui `d9ad932` (quality/species/age Added entry). Both path-scoped
> single-file commits per the shared-tree protocol.

Both repos' `CHANGELOG.md` are untouched by this program. §5.2 says
"update change log with critical commits upon releases." Eighteen commits
across two repos qualify.

*Approach:* one entry per repo summarising the LARD capability (transport,
age, dispersion, negative sources, the option/API surface) and the GUI
capability (options page, species theming + plotting, age editors), each
naming the commits. Do **not** transcribe the round structure — a
changelog is for users of the software, not for this program's bookkeeping.
*Size:* well under a round.

### P0.3 Adopt the shared-tree protocol as standing practice

`SHARED_TREE_STATE_2026-08-23.md` §4 exists because four concurrent-session
incidents landed in this program (A2b fixtures · X1's two lost
`SWMMEngine.cpp` hunks · Y2a's borrowed CMakeLists · the stale index, whose
four phantom deletions included X5's public header). The index is repaired
and proven lossless; the protocol is written but not yet habitual.

*Verify:* the next round that touches a shared file claims it in §5 first.
*Size:* free; it is a habit, not a task.

---

## P1 — Correctness debts with a known defect story

These are not speculative cleanups. Each has a *measured* or *structurally
argued* failure mode already recorded.

### P1.1 The X2.viii routing-step instrument · **1 round**

**What's missing.** X2's falsifier viii — the node mix reading
`nodes.volume` instead of `old_volume` — has **no observer**. X3a proved
why: its instrument refines `QUALITY_STEP` under a *frozen* `ROUTING_STEP`,
and old-vs-new volume is dtq-independent by construction (measured spread
19.057 → 17.952, i.e. it moves the *limit*, not the convergence).

**Approach.** A second instrument on the orthogonal axis: run I1's washout
deck at `ROUTING_STEP` {40, 20, 10} with `dtq = rs`, assert contraction
`|A(40)−A(20)| > |A(20)−A(10)|`, report the ratio. **The heat-instrument
caveat returns here** — refining `rs` moves the flow solution too, so the
ratio is *reported, not pinned*, and the band comes from measuring the
correct form against falsifier X2.viii's form.

**Gates.** One contraction gate + the measured band. **Falsifier: apply
X2.viii and confirm it now bites** — that is the round's entire purpose;
if it does not, the instrument is on the wrong axis again and that must be
recorded rather than papered over.

> **DONE 2026-08-25, committed `6566f407`, with the instrument form
> corrected by measurement**
> (`LardDtReferenceTest.StorageFillLardVsLegacyGapBoundedOnRsLadder`).
> Three findings, each measured rather than assumed:
>
> 1. **The proposed contraction form fails on this axis even clean.** The
>    point observable expands (ratio 0.899 — O(rs) hydraulic phase shift
>    amplified by the front's slope) and the integral oscillates (gaps
>    3232/463/1261 on {80,40,20,10}); the dynamic-wave solution's own
>    rs-dependence is not smooth enough to contract through.
> 2. **The washout deck cannot carry the gate at all.** X2.viii's error
>    term is the per-step volume change at the mixing node, and junction
>    volumes are `MIN_SURFAREA` residue: the defect measured at ~1/20 of
>    the ladder's hydraulic noise. The deck that works starts DRY and
>    fills a STORAGE unit (volume that is state) from a steady CONCEN
>    source — defect shift −324/−314/−243/−158 across the ladder,
>    rs-dependent, exactly the signature the dtq axis could not see.
> 3. **The observable is the lard-vs-legacy gap D, and its razor is a
>    LEVEL, not a rate.** Pairing each rung against the LEGACY control on
>    identical hydraulics cancels the drift, but D contracts to a
>    STRUCTURAL limit, not zero (CSTR instant breakthrough vs late
>    parcels), so |D| grows clean as refinement strips noise
>    (−114→−316). The gate is a band on D(80) — clean −114.4, X2.viii
>    −329.8, pinned at the geometric mean 194. **The falsifier bites**
>    (confirmed against the final gate form, then reverted; tree
>    byte-clean). The finest rung is reported, not gated (±15% margins).

### P1.2 The dry-hotstart gate (X4.vii) · **1 round, small**

> **STALE CLAIM — verified already done 2026-08-25.** The gate exists at
> `tests/unit/engine/test_water_age.cpp:911` with exactly this plan's
> approach (bone-dry deck, `INITIAL_STATE 6 h`, saved link age > 21600 s,
> bit round-trip) and its comment records the mask-the-state falsifier
> story. It passes in the X6-era logs. Nothing to do.

**What's missing.** `DryElementHotstartCarriesTheAgedState` has been owed
since the A2b era and is *still* absent from `test_water_age.cpp`. It is
the only observer of the **state/report separation**: a dry element's age
must keep advancing in state while the report masks it.

**Approach.** Bone-dry deck (`InitDepth 0`, FREE outfall, no inflow) +
`INITIAL_STATE 6 h`; save a native hotstart, assert the SAVED link age
exceeds 21600 s, reload, assert bit-round-trip.

**Gate value test.** Its worth is one falsifier: **mask the STATE instead
of the report and the gate must fail.** If it still passes, the gate does
not observe what it claims and should not be committed.

### P1.3 Finish the C-API numeric audit (H1 §7) · **1 round**

**What's missing.** H1 hardened `swmm_options_set` — and its validation
found the exception guard alone was **necessary but not sufficient**: two
*non-throwing* families slipped through (time keys fabricating `0.0` from
junk, `"1e999999"` → 3600 s; and `std::stoi` partial-parses, `"1.5"` → 1).
Strict wrappers now cover 36 sites in that one TU. **The other ~15
`*_impl.cpp` TUs were never audited**, nor the Python binding's conversion
layer.

**Approach.** Grep every `*_impl.cpp` for `std::sto*` and for the
lenient `parse_time_seconds`; apply the same strict wrappers; extend
`test_options_malformed_values.cpp`'s exhaustive-key pattern to whichever
setters are found.

**Why it earns a round.** The MCP server passes arbitrary LLM-authored
text into these dispatches. The one measured instance aborted the process;
silent truncation is the quieter and arguably worse sibling.

**Also check:** whether any TU is built `-fno-exceptions` — the H1 guard
is inert there and the crash returns (recorded in H1 §6, unverified).

> **DONE 2026-08-25, committed `22e55228`.** The audit found the surface
> far smaller than feared:
>
> - **The other `*_impl.cpp` TUs carry ZERO parse sites.** Swept for
>   `std::sto*`, `strto*`, `atoi/atof`, and `sscanf` — every numeric
>   parse in the C API lives in `openswmm_model_impl.cpp`. Three raw
>   sites remained there, all inside local try/catch (so none could
>   abort — the residue was the SILENT families): `HOTSTART_SAVE_DATETIME`
>   stored 1.5 for `"1.5abc"` and 1.0 for `"01/01/2026"`; a file-slot
>   owner index of `"0.5"` resolved to slot 0 and **wrote the wrong
>   slot, returning SWMM_OK**. All three now use H1's strict wrappers.
> - **Falsifiers bite individually**: A (datetime `std::stod` restored)
>   fails exactly the two partial-parse rows; B (owner `std::stoi`
>   restored) fails all three owner rows AND the wrong-slot write
>   corrupts slot 0's path in the gate's liveness check. The LID_REPORT
>   owner shares the same edit but has no direct observer (needs a LID
>   deck the TU does not carry) — recorded in the gate's comment.
> - **The Python conversion layer has no C-level parse sites** — Cython
>   typed coercions raise `ValueError`/`TypeError` as ordinary Python
>   exceptions; no abort path.
> - **No TU builds `-fno-exceptions`** (swept every CMakeLists and
>   cmake module) — H1 §6's inert-guard concern is closed.

### P1.4 `[TRANSPORT_SOURCES]` negative rows · **1 round**

D-NS1 is implemented at the **node** seam in all three engines. ARD's
**cell** sources (`[TRANSPORT_SOURCES]`, E5a) were deliberately left out
(X6 §2.5) because they have their own conservation story. Today a negative
cell source is unspecified behaviour.

**Approach.** Same contract as D-NS1: parse warning, clamp to the cell's
held mass, book what actually left, count and summarise. **Decide first**
whether a cell clamp needs its own ledger row or rides
`qual_routing_ex_in` — the answer differs from the node case because cell
sources are distributed.

### P1.5 Negative DWF / GW / RDII concentrations · **1 round, optional**

Explicitly out of D-NS1's v1 scope (X6 §2.2): those loaders were not
touched, so a negative concentration there is unhandled. Worth doing only
if you actually want extraction on those pathways — otherwise **close it
as "won't do" rather than leaving it as an open debt**, so the list stops
carrying it.

---

## P2 — Capability gaps (deferred by your scope decisions, not defects)

Each is gated behind a live bypass warning, so users are told rather than
surprised. Verified present at `SWMMEngine.cpp:342/350/357`.

### P2.1 Heat under LARD (H7) · **2 rounds**

`"temperature state does not advance under the LARD engine yet"` fires
today. The work mirrors X4 exactly: temperature as a second reserved
species row on the segments, sourced from `node_temp_vol_in` (the D-UT10
accumulator, already filled by all seven loaders).

**Prerequisite for the GUI half:** `openswmm_heat.h` does not exist — the
same Y0 trap. GUI plan prereq 5's other half. Sequence
**engine H7 → `openswmm_heat.h` → G4g editor**, and verify the C API layer
before declaring the GUI unblocked.

### P2.2 L3 — MSX reactions on segments · **2–3 rounds**

`"the LARD reaction binding is not implemented (deferred L3)"` fires today;
only first-order KDECAY reacts under LARD. The shared `ReactionSystem`
(R1–R4) already exists, so this is a *binding*, not a new module: gather
each segment's species column into a stack block (D-L1's stated
gather/scatter), integrate, scatter back.

### P2.3 Treatment interop under LARD · **1 round**

`"[TREATMENT] expressions … no removal is applied"` fires today. The ARD
precedent (E5b) is the template: run the legacy evaluator on published
node concentrations, then absorb the treated values back into the node
stores.

### P2.4 Storage mixing models beyond CMSTR · **1–2 rounds**

TWO_COMPARTMENT / FIFO / LIFO (strategy §2.3), sharing the E2b token with
the ARD engine. Only worth it for models where storage residence structure
matters.

### P2.5 Full A6 — Python + MCP age surfaces · **1 round**

X5 delivered the C subset the GUI needed. Python bindings and MCP tools for
the age source table remain. Do this if the MCP-driven workflow matters;
the `.pyx`/`.pyi` half carries the lesson-46/47 traps (silent SkipTest,
macOS codesign).

---

## P3 — Verification breadth (do when the above is quiet)

- **Laminar RWPT deck.** X3b's laminar branch is pinned only at unit level;
  Re ≈ 1e6 on every current deck. A trickle-flow deck is fragile — this is
  why it was deferred, and that reasoning still holds.
- **An RWPT corpus deck.** Deferred until the Elder band was pinned. It now
  is (measured 0.96–1.44 across five seeds), so a deck is buildable.
- **The `swmmvis_core` extraction.** The structural fix for the GUI's
  widget-observer hole (`tests/gui/CMakeLists.txt:1996`) — it would close
  Y1's five blind falsifier rows *and* the FV page's identical gap.
  **Re-scoped and worth repeating: this is a refactor, not a small round.**
  The cheaper partial is the policy-extraction pattern Y2a and Y3 both
  used — put the fallible logic in a linkable TU.

---

## Recommended sequence

**P0 first** (push, CHANGELOG) — hours, and it stops the integration
surface growing. Then **P1.2 → P1.1 → P1.3**: the dry-hotstart gate is
small and closes the oldest owed observer; the rs-instrument closes the
last blind falsifier from the LARD rounds; the API audit closes a
crash/silent-corruption class that is reachable from the MCP today.

**P2 only on demand.** Every item is warned at open, so nothing fails
silently. Pick the one your next project needs — heat if you are modelling
temperature, L3 if you need real chemistry under LARD.

**Standing rule for all of the above:** the implement→validate loop with a
falsifier sweep is what caught every defect in this program, including four
in code that had already passed review. Keep it. A round without an
observer for its own claim is not finished — it is a claim.
