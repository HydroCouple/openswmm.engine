# A2a Implementation — Validation & Commit Handoff (2026-08-17)

**For:** the checking agent (sandbox: `g++ -fsyntax-only` only).
**Base:** `d2f003e6` (post-A1b).
**Plan:** `WATER_AGE_TRACKING_PLAN.md` §2 (hotstart) / §7 A2 — **split:**
A2a = age hotstart persistence + restart continuity (this shot); A2b =
.rpt/.out age reporting + the age-volume ledger row.
**Standing findings:** lessons 1–35 apply.

---

## 1. Changeset (uncommitted)

```
mod:  src/engine/core/HotStartManager.{hpp,cpp}
      (NATIVE format V3: node/link records gain `double age = -1.0`
       (-1 ⇒ not tracked / pre-V3 file); write/read version-gated exactly
       like the V2 subcatch fields; reader accepts 1..3; save() promotes
       to V3 when WATER_AGE is ON (V3 implies the V2 subcatch fields,
       defaults when no accessors) and captures water_age_state; apply()
       restores ages into water_age_state (guarded one-time resize) and
       sets hotstart_loaded + legacy_seeded so BOTH engines seed from the
       loaded state instead of INITIAL_STATE)
mod:  src/engine/data/WaterAgeData.hpp   (hotstart_loaded flag; resize
       resets both flags)
mod:  src/engine/transport/components/EulerianArdComponent/ArdEngine.cpp
      (init snapshots loaded ages BEFORE its resize wipes them, then
       seeds cell rows from the LINK ages and stores from the NODE ages;
       INITIAL_STATE remains the fallback; the flag is consumed)
mod:  tests/unit/engine/test_water_age.cpp  (+2 gates, both engines:
       HotstartRoundTripsAgeAcrossBothEngines — BITWISE double round trip
       + one-step restart continuity with INITIAL_STATE 100 h as the
       seeding DISCRIMINATOR (loaded O(10²–10³) s vs 360000 s);
       PreV3HotstartFallsBackToInitialState — the −1 sentinel path)
```

All TUs pass `g++ -std=c++20 -fsyntax-only`. No new TUs — reconfigure only
if globbing is stale.

## 2. Design decisions to review

1. **Age-ONLY persistence.** The audit found POLLUTANT hotstart restore is
   a PRE-EXISTING gap in both formats: the native records carry no
   quality at all, and the legacy .hsf reader deliberately READS AND
   DISCARDS the qual[] floats ("does not affect hydraulic routing").
   Persisting age alongside a discarded pollutant state is coherent
   because age is NEW (no legacy-parity constraint pins its restart
   behavior); fixing pollutant restore is a parity DECISION (legacy
   discards on purpose) and is recorded as a carry, not smuggled in here.
2. **V3 always when WATER_AGE ON** (age −1 when the state is unsized);
   pre-V3 files fall through to INITIAL_STATE seeding via the sentinel.
3. **The LEGACY mirror needs no code change**: apply() sets
   legacy_seeded, which suppresses the first-step INITIAL_STATE fill and
   leaves the loaded arrays as the state. The ARD engine snapshots before
   its resize (the same wipe hazard lesson 14 warns about, handled).
4. **The gates' discriminator is structural**: INITIAL_STATE 100 h in the
   restart deck makes wrong-source seeding a 360000-vs-hundreds
   separation — no band tuning can blur it.

## 3. Validation protocol

1. Reconfigure if needed, build, zero new warnings.
2. `ctest -R test_engine_water_age` — 12 gates now.
   *Anticipated failure modes:*
   (a) **swmm_hotstart_save after end()** — the gate saves post-end;
   verify the C API permits that lifecycle state (if it requires an open
   run, move the save before end and record).
   (b) **apply() ordering vs initialize()** — the gate applies AFTER open
   and BEFORE initialize; if initQuality() re-seeds nodes.conc and
   anything downstream re-derives age state, the continuity leg catches
   it (age would jump to 360000).
   (c) **CRC/format**: any V3 read failure is a format-layout defect —
   compare write/read field order first.
3. **Falsifier sweep:**

   | falsifier | expected failing gates |
   |---|---|
   | i. skip the age capture in save() | round trip (loaded = −1 ⇒ INITIAL_STATE jump in continuity leg) |
   | ii. skip the age restore in apply() | round trip (bitwise legs) |
   | iii. drop the ARD init snapshot (seed from INITIAL_STATE always) | ARD continuity leg (360000 jump); LEGACY leg unaffected — note the split |
   | iv. drop legacy_seeded in apply() | LEGACY continuity leg (INITIAL_STATE refill) |
   | v. write age unconditionally as V2 layout (version not bumped) | round trip fails to read or misparses — CRC/format leg |
   | vi. break the −1 sentinel (treat −1 as a real age) | PreV3 fallback gate |
4. **Prior suites all green** — WATER_AGE-off saves still write V1/V2
   (byte-identical files: verify a saved hotstart from a benchmark deck
   is BIT-IDENTICAL to base — the version promotion is gated on
   water_age). 14/14 deck sha256 unchanged. Sanitizers.
5. **Pre-existing gap recorded, NOT fixed:** pollutant hotstart restore
   (native: absent; legacy: read-and-discarded). Needs a parity decision
   — carry to the roadmap.
6. Append results to §5; commit with §4.

## 4. Commit message

```
feat(transport): water-age hotstart persistence (A2a, native V3)

The native hotstart format gains version 3: every node/link record
carries the water age (seconds; -1 = not tracked / pre-V3 file),
version-gated exactly like the V2 subcatchment fields. save() promotes
to V3 when WATER_AGE is ON and captures water_age_state; apply()
restores it and flags both engines to seed from the LOADED state instead
of INITIAL_STATE (the ARD engine snapshots before its init resize; the
LEGACY mirror's first-step fill is suppressed via legacy_seeded).
Pre-V3 files fall through to INITIAL_STATE via the sentinel. Audit
finding recorded as a carry: POLLUTANT hotstart restore is a
pre-existing gap in both formats (native carries none; the legacy .hsf
reader deliberately discards qual[]) and needs a parity decision. Gates:
test_water_age.cpp +2 (bitwise double round trip + one-step restart
continuity under BOTH engines with INITIAL_STATE 100 h as a structural
seeding discriminator; pre-V3 sentinel fallback).

Plan: WATER_AGE_TRACKING_PLAN.md section 2/7 A2 (A2a half).
Validation record: plans/transport/A2A_VALIDATION_HANDOFF_2026-08-17.md
```

## 5. Validation results

*(appended by the checking agent)*

### 5.0 Outcome

**Committed.** The persistence design is sound and every falsifier is
caught — but **both delivered hotstart gates were vacuous on arrival**
(§5.2): they exited on a lifecycle error before reaching a single age
assertion, so nothing about age hotstart had actually been exercised.

Final: **16/16** gates, suite **139/140** (only the known pre-existing
`FvEngine.RefiningTheMeshConvergesTowardTheDynwaveHydrograph`), 14/14 deck
`.out` bit-identical, native hotstart byte-identical for WATER_AGE-off,
falsifier sweep 7/7. One pre-existing UBSan finding, blamed and recorded
(§5.6).

### 5.1 The working tree held TWO changesets

`git status` at `d2f003e6` showed 17 modified files. Only five are A2a's:
`HotStartManager.{hpp,cpp}`, `WaterAgeData.hpp`, `ArdEngine.cpp`,
`test_water_age.cpp`. The rest — `MultiColumnSeriesFile.{hpp,cpp}`,
`GageData.hpp`, `PostParseResolver.cpp` (328 lines), `TableData.cpp`,
`openswmm_gages*`, the Cython bindings, two manual chapters, two new test
targets in `tests/unit/engine/CMakeLists.txt` and ten new fixture files —
are an unrelated multi-column series-file feature
(`plans/MULTICOLUMN_SERIES_SINGLE_READ_2026-08-17.md`).

Committing the tree under §4's message would have bundled a whole separate
feature. **Only the five A2a files are staged**; the other changeset is
left untouched for its own handoff.

All validation ran in an **isolated worktree** (`/tmp/a2a_iso` =
`d2f003e6` + the five files, nothing else), so no suite result, bit-identity
comparison or sanitizer finding can be confounded by the foreign work. That
worktree is also exactly the committed state, which makes it the strongest
available check that the commit builds and passes on its own.

### 5.2 Both hotstart gates were vacuous: apply() needs INITIALIZED

```
[  FAILED  ] WaterAgeTest.HotstartRoundTripsAgeAcrossBothEngines
  swmm_hotstart_apply(e2, hs)  Which is: 6      (SWMM_ERR_LIFECYCLE)
[  FAILED  ] WaterAgeTest.PreV3HotstartFallsBackToInitialState
  swmm_hotstart_apply(e, hs)   Which is: 6
```

`swmm_hotstart_apply` requires `EngineState::INITIALIZED`
(`openswmm_hotstart_impl.cpp`), and both gates called it from `OPENED` —
open → apply → initialize. They failed on the API call, before the bitwise
round trip, before the continuity leg, before the sentinel check. **Nothing
about age persistence was tested by the gates as delivered.**

This is §3's anticipated failure (b), realized. Reordered to
open → **initialize** → apply → start → step, which is also the correct
order for the feature: `initialize()` does not reset `water_age_state`
(`ctx_.reset()` runs in `open()`, not `initialize()`), so the apply's writes
survive, and both engines consume the flags afterwards — `ArdEngine::init`
at the first `stepRouting`, the LEGACY mirror at its first step.

With the order fixed, all four legs pass and the design works as described.

### 5.3 Restart continuity measured; the delivered band was 800x too wide

The continuity leg banded the post-restart age at `(0.25*saved, 200000)` —
it separates "loaded" from "reset to zero" and from "re-seeded at
INITIAL_STATE 360000" and nothing else. It would pass a restart that
recovered a quarter of the state. Measured:

| engine | saved C5 | restored | one step after | delta |
|---|---|---|---|---|
| ARD | 876.447 | 876.447 (bitwise) | 837.909 | **−4.4%** |
| LEGACY | 964.078 | 964.078 (bitwise) | 948.457 | **−1.6%** |

Tightened to ±10%, which keeps 2.3x headroom over the worst measured
deviation.

**ARD's larger drop is structural, not a defect.** The hotstart record
carries ONE age per link; the ARD mesh carries one per CELL. A save
collapses the within-link age profile to a single number and the load
re-uniformizes it, so an ARD restart is continuous but not bit-continuous.
Making it exact would need a per-cell hotstart block — a format change well
beyond A2a. Recorded so nobody later reads the 4.4% as drift.

### 5.4 The pre-V3 fallback needed a second leg

`PreV3HotstartFallsBackToInitialState` covered ARD only. LEGACY reaches
INITIAL_STATE by a **different route**: `apply()` leaves `legacy_seeded`
false when no age was loaded, so `routeLegacyAge` fills on its first step,
where the ARD engine seeds in `init()`. One leg cannot cover both code
paths; the LEGACY leg was added.

### 5.5 Falsifier sweep — 7/7, after correcting two of my own anchors

| # | falsification | gate |
|---|---|---|
| i | skip the age capture in `save()` | round trip |
| ii | skip the age restore in `apply()` | round trip |
| iii | drop the ARD init snapshot | round trip |
| iv | drop `legacy_seeded` in `apply()` **(both sites)** | round trip |
| v | write age without bumping the version | round trip |
| vi | break the −1 sentinel | pre-V3 fallback |
| vii | drop `hotstart_loaded` in `apply()` **(both sites)** | round trip |

**iv and vii came back green on the first attempt, and that was my error,
not the code's.** `apply()` sets each flag in the node loop AND again in the
link loop, so an anchor that removes one site leaves the other doing the
work. Both fail once both sites go. Worth knowing for the next sweep on
this file: a flag assigned twice needs a two-site falsifier.

Six of the seven land on the same gate. That gate loops over
`{ARD, LEGACY}` and names the engine in every message, so a failure still
localizes to an engine even though the gate name does not distinguish them.

### 5.6 A pre-existing UBSan finding, surfaced not introduced

```
HotStartManager.cpp:246:33: runtime error: load of misaligned address
  0x61500000de73 for type 'const uint32_t', which requires 4 byte alignment
  #0 HotStartManager::read_file  #1 ::open  #2 swmm_hotstart_open
```

`const uint32_t stored_crc = *reinterpret_cast<const uint32_t*>(raw.data() +
file_size - 4);`. A `vector<uint8_t>`'s data is well aligned, but
`+ file_size - 4` is only 4-byte aligned when the file size happens to be —
the 229-byte V2 file is not. `git blame` puts the line at **`4e29c8869`
(2026-03-26)**, and it appears zero times in the A2a diff. A2a is simply the
first thing to read a native hotstart under UBSan.

Benign on arm64 in practice, but it is UB, and the one-line fix is a
`std::memcpy` into a local. **Not fixed here** — same treatment §2.1 gives
the pollutant-restore gap: recorded as a carry rather than smuggled into a
feature commit. Flagged loudly because a future ASan run on this suite will
show it and could be misread as A2a's.

### 5.7 Also checked, and clean

- **V3 without V2 content.** `save()` populates the V2 subcatchment fields
  only when `use_v2` (`state_accessors.can_read()`), while `write_file`
  emits them whenever `version >= 2` — and V3 is now set independently of
  accessors. A V3 file written with no accessors would carry default
  (zero) subcatchment state, and applying it on a context that *can* write
  would restore zeros over real infiltration/GW state. Unreachable:
  `SWMMEngine::open` wires all four accessors in one unconditional block,
  so `can_read() == can_write()` always, and a detached context that cannot
  read also cannot write (`apply_v2` false). No action.

### 5.8 Prior suites, byte identity, sanitizers

- `ctest` in the isolated tree: **139/140**; the failure is the known
  pre-existing `FvEngine` refinement case.
- **14/14 deck `.out` bit-identical** vs `d2f003e6`.
- **Native hotstart byte-identity for a WATER_AGE-off deck.** §4 asks for
  this, and the obvious route does not test it: `[FILES] SAVE HOTSTART`
  writes the **legacy** `.hsf` (`SWMM5-HOTSTART4`), which A2a does not
  touch. The native writer is reachable only through `swmm_hotstart_save`,
  so a probe drives that path. Result: 229 bytes both builds, and against a
  baseline run taken in a *different second* the only differing offsets are
  21 (the `std::time` stamp) and 226–229 (the trailing CRC) — both vary
  run-to-run within a single build. The version field at offset 0x10 reads
  **2**, so the promotion is correctly gated on `water_age`.
- ASan+UBSan over the 16-gate suite: **0 findings attributable to A2a**;
  the single UBSan line is §5.6's pre-existing misaligned CRC load.

### 5.9 Open items

- **§5.6's misaligned CRC read** — pre-existing UB, one-line `memcpy` fix.
- **Pollutant hotstart restore** — the carry §2.1 identified: native
  records carry no quality; the legacy `.hsf` reader deliberately reads and
  discards `qual[]`. Needs a parity decision.
- **ARD restart is not bit-continuous** (§5.3) — one age per link versus
  one per cell. Inherent to the record layout.
- Carried and untouched: `transported_count()` has no callers; the registry
  records `__WATER_AGE__` units as `"hours"` while state publishes SECONDS;
  `reset()` does not clear `ctx.reactions`; ARD's dt→0 outfall-age residual
  (A1b §5.3).
- Artifacts: `tests/output/a2a_validation_2026-08-17/` — `a2a_probe.cpp`,
  `a2a_hsbytes.cpp` and their logs, `falsifiers.sh` + per-case logs and
  `falsifiers_summary.log`, `run_decks.sh`, `ctest_full.log`,
  `asan_water_age.log`.
