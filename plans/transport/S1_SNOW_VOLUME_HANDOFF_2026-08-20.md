# S1 — the snow mixing volume — Validation & Commit Handoff (2026-08-20)

**For:** the checking agent.
**Base:** `0e8e57df`.
**Finding:** `SNOW_MIXING_VOLUME_FINDING_2026-08-20.md` — **read it first.**
**Standing findings:** lessons 1–103.

**A live defect in shipped code, across A3 and H5a.** Found while checking
prerequisites for the API bundle the plan has next.

---

## 1. The defect

On a subcatchment with a snowpack, the runoff solver uses
`snow_net_imperv` / `snow_net_perv` — `imelt + rain·(1 − asc)`, **per
subarea** (`Runoff.cpp:548-552`, built at `SWMMEngine.cpp:1608-1616`).

A3 and H5a instead read `ctx.subcatches.rainfall`, which is set once at
`Runoff.cpp:294-295` to **`rain + SNOWFALL`** and never updated. Wrong twice:

1. **Snowfall counted as arriving liquid** — water being stored in the pack
   is mixed in as though it had landed.
2. **Snowmelt not counted at all** — the water that genuinely arrives is
   invisible, so the surface ages as if nothing arrived. **That is A3's own
   net-gain failure (lessons 64, 68) through a different field.**

They do not cancel: they are displaced by the pack's whole residence time,
which is the quantity a snow-aware age model exists to measure.

## 2. Scope — and one thing this is NOT

**The LID seam is not part of this defect.** At `SWMMEngine.cpp:1735-1757`
the same `rain` value feeds *both* `g.inflow` (hydrology) and
`setLidInflowAge`/`setLidInflowTemperature` (transport), so they **agree**.
Whether that shared value is right under a pack is a legacy-fidelity question
about LID hydrology — legacy `lid.c` also reads subcatchment rainfall — and
**fixing transport alone there would make them disagree**, which is the
opposite of the defect. Recorded as an owed question, not touched here.

The defect is exactly where transport and hydrology **disagree**: the two
watershed modules.

## 3. What S1 does and does not settle

**S1 fixes the VOLUME only.** Melt arrives at the configured RAINFALL value.
That is *a defensible number over the right volume* instead of a defensible
number over the wrong one — a strict improvement, independently landable.

**S2 settles the VALUE** (user decision, 2026-08-20: *both tracks get the
full answer*): the pack carries an age that melt inherits, and melt arrives
at **0 °C**, which is what melting means. That needs new per-subcatchment
pack state with hotstart and reporting consequences, so it is a separate
changeset — the D-H5d/D-H5e precedent, so a falsifier sweep can tell a defect
fix from new physics.

`HeatWatershed.cpp` carries an `@note` at the mixing site saying exactly this,
so the gap is named in the code and not only here.

## 4. Changeset (uncommitted)

```
new:  src/engine/transport/components/WatershedCommon.{hpp,cpp}
      (arrivingPrecipRate — the one place that knows which field is right)
mod:  .../WaterAgeModule/WaterAgeWatershed.cpp   (per-subarea rate; the
      rain/run-on mix moves INSIDE the subarea loop)
mod:  .../HeatModule/HeatWatershed.cpp           (same)
new:  tests/unit/engine/test_transport_snow.cpp  (6 gates)
mod:  tests/unit/engine/CMakeLists.txt           (+1 target — SHARED FILE)
```

All TUs pass `g++ -std=c++20 -Wall -Wextra -fsyntax-only`. Nothing built or
run.

## 5. ⛔ THE HARD STOP NEEDS A CHANGED PROCEDURE THIS ROUND

`.git/index.lock` is **still present** (since 04:54, no live git process),
exactly as the last round reported. **The index is stale, so the usual check
gives the wrong answer:**

```
git diff       --numstat -- tests/unit/engine/CMakeLists.txt   →  2  0   ← STALE
git diff HEAD  --numstat -- tests/unit/engine/CMakeLists.txt   →  1  0   ← TRUTH
```

The `2` is the previous round's `transport_dt_reference` line, already in
`HEAD`, reappearing because the index predates that commit. **Verified:
`git show HEAD:…CMakeLists.txt | grep -c transport_dt_reference` = 1.**

**So: compare against `HEAD`, not the index, and expect `1  0`.** A `2  0`
against the index is the stale-index signature, not a foreign edit — but
**confirm it is that and not both** before committing. The lock was not
touched, per the standing constraint.

Once the lock clears, the previous round's recovery still applies and should
run before anything else commits from that index.

## 6. Validation protocol

1. **Isolated worktree at `0e8e57df`.** Lesson 71. Suite was **155/155** and
   nothing failed at base — carry no exemption forward (lesson 89).
2. **§5's hard stop**, with the `HEAD` comparison.
3. Build, zero new warnings. Then the snow suite, then the full suite.

   **Anticipated failure modes — and this round's list is worth more than
   usual, because the deck is the first of its kind in the program.**

   (a) **The pack may not form or may not melt.** `sd0`, `cmin/cmax` and
   `tbase` are deck units, and issue #131 is the standing precedent for
   parameters reaching the solver unconverted. Gate 1's SETUP
   (`MeltingWithNoRain`) asserts a pack exists, rainfall is exactly zero, the
   net rates are non-negative, and at least one is positive. **If it fires,
   fix the deck — raise the air temperature, the melt coefficients or `sd0`.
   Do not relax it.** Lesson 96: assert every term of the predicate.

   (b) **Gate 4 may find the two subarea rates identical.** The deck gives
   PERVIOUS slower coefficients (0.01/0.03 against 0.02/0.06) so they should
   differ, and the gate asserts that before comparing. If they coincide,
   widen the spread rather than dropping the assertion — §2.2's per-subarea
   claim is unobserved otherwise.

   (c) **An existing A3 or H5a gate moves.** It should not: gate 5 asserts a
   pack-less subcatchment reads the gage rate unchanged, and no existing deck
   has a pack. **If one moves, that is the most important result of the
   round** — it would mean the loop restructure changed the pack-less path.

   (d) **`REMOVAL` row arity.** I wrote six numeric fields with no subcatch
   name. If the parser wants seven or refuses the row, drop it — it is not
   load-bearing for any gate.

4. **Falsifier sweep:**

   | falsifier | expected failing gates |
   |---|---|
   | i. `arrivingPrecipRate` returns the gage rate always (the pre-S1 behaviour) | **2, 3** — the defect gates, plus 1's helper checks. If 2 and 3 do not fail, the mixing volume is unobserved and this changeset proves nothing |
   | ii. use `snow_net_imperv` for all three subareas | **4** — §2.2's claim |
   | iii. test `> 0.0` instead of `>= 0.0` for the sentinel | **probably nothing** — flagged. It only differs on a step where a pack exists but melts exactly nothing, which this deck may never produce. **Owed if it escapes**; the distinction is load-bearing (`SubcatchData.hpp:642` initialises to −1.0 and `Runoff.cpp:551` tests `>= 0`) |
   | iv. drop the `ignore_snow_melt` check | **6** |
   | v. drop the `snowpack[ui] < 0` check | 5 — and it should crash or read garbage on a pack-less deck, which is louder |

5. **Prior suites:** full C++ suite, 14/14 decks, ASan/UBSan. **A3's and
   H5a's suites must be bit-unchanged** — that is (c), and it is the check
   that matters most here.

6. **Record:** falsifiers i and iii; whether the deck reached a melting pack
   as written; and **the measured magnitude** — with no rain and a melting
   pack, what the subarea age and temperature actually are, correct form
   versus falsifier i. The finding document reasoned the defect from the
   code and explicitly did **not** measure it (lesson 91: a claim in a
   document is not an arithmetic check). **This round is where it gets
   measured.**

## 7. Known gaps

- **Falsifier iii predicted to escape.** Needs a step with a pack and
  exactly zero melt.
- **S2 is not started**: melt still arrives at the configured RAINFALL age
  and temperature. Named in the code at the `HeatWatershed` mixing site.
- **The LID-under-a-pack question** (§2) is open and is about hydrology
  fidelity, not transport.
- No gate here exercises `ADC` (areal depletion) explicitly, so whether the
  `asc` blend is continuous across pack formation and disappearance is
  untested — a discontinuity there would show up as a jump in the mixing
  volume at exactly the moment a pack appears or vanishes.

## 8. Prepared commit message

```
fix(transport): the mixing volume under a snowpack is not the rainfall (S1)

A3 and H5a read ctx.subcatches.rainfall as "how much water landed on this
subarea". Under a snowpack the runoff solver does not use that field: it
uses snow_net_imperv / snow_net_perv, per subarea, built as
imelt + rain*(1 - asc).

So the transport modules counted SNOWFALL as arriving liquid -- water being
stored in the pack, mixed in as though it had landed -- and did not count
SNOWMELT at all, leaving the water that genuinely arrived invisible to the
mixing volume. That second half is A3's own net-gain failure through a
different field. They do not cancel: they are displaced by the pack's whole
residence time.

arrivingPrecipRate is now the one place that knows which field is right, and
the rain/run-on mix moves inside the subarea loop because the precipitation
rate differs between subareas under a pack while run-on does not.

The LID seam is deliberately untouched: there the same value feeds both the
hydrology and the transport, so they agree, and fixing one side would make
them disagree.

S1 fixes the VOLUME only. Melt still arrives at the configured RAINFALL age
and temperature; S2 settles the value.
```

---

## 9. Validation results (2026-08-20) — COMMITTED `274b6506` + `d7ee70be`

**156/156 ctest**, **14/14 decks byte-identical** to the `815f0e8e` reference,
**73 tests clean under ASan/UBSan**, zero new warnings.
**Falsifier sweep: 5 of 6 observed** — including iii and iv, both of which the
handoff predicted would escape.
Full numbers: `tests/output/s1_validation_2026-08-20/measurements.md`.

**Two commits, not one.** Anticipated failure mode (a) fired on all four
defect gates, and the deck was not the cause.

### 9.1 `SnowSolver::setMeltCoeffs` had no caller — degree-day melt was dead

`grep` over `src/engine/` returns nothing; the only callers are in
`test_snow.cpp`, which sets the coefficients itself before stepping the
solver. So `dhm` stayed at its `assign(0.0)` value and
`imelt = dhm · (temp − tbase)` was **identically zero on every deck**,
whatever the air temperature. Only rain-on-snow melt could release water from
a pack. Legacy calls it once a day from `setTemp` (`climate.c:1176-1180`).

Measured on the gate deck, 50 °F against a 32 °F `tbase`, before and after:

```
before:  dhm=0/0/0            season=0          imelt=0  sni=0     runoff=0
after:   dhm=4.716e-07/...    season=-0.981306  sni=9.43e-06       runoff=0.99 cfs
```

Landed as `274b6506`, ahead of S1, with an engine-level gate in
`test_snow.cpp`; **155/155 with that commit alone**, verified separately. S1's
gates 1–4 and 6 all fail without it, which is why the two are ordered.

*This is the reason the S1 defect was never observed: the transport modules
read the wrong field, but there was never any melt to expose it.*

### 9.2 The deck needed a RIPE pack, and that is not decoration

Melt does not leave a pack until its free water exceeds `fwfrac · wsnow`
(`Snow.cpp` step 6). At 0.10 of a 0.5 ft pack that store is 0.05 ft and the
degree-day rate fills it in **~98 minutes** — longer than the 60-minute deck.
`fw0 = 0.0` and `0.3` both release nothing; `fw0 = fwfrac · sd0` releases from
the first step. The writer now computes it.

### 9.3 The magnitude (§6.6) — measured, not reasoned

Hydrology identical either way (2937.651904 ft³ of runoff):

| observable | correct (S1) | pre-S1 |
|---|---|---|
| subarea age IMPERV0 / IMPERV1 / PERV | 624.67 / 787.37 / 0 s | 3600 / 3600 / 3600 s |
| runoff age | 753.75 s | 3600 s |
| subarea temperature | 0 °C | 25 °C |

**3600 s is the elapsed run time exactly** — what a surface reads when nothing
mixes into it, i.e. A3's net-gain failure through a different field. The
temperature error is the whole span between `INITIAL_STATE` and the arriving
meltwater. Gate 3 was checked against three controls (melt at 25 into 25 → 25;
10 into 25 → 10; no pack, no rain → 25 held) so it is not passing on an
unseeded field.

### 9.4 Three gates did not test what they claimed

- **Gate 5 was vacuous.** A pack-less *project* has `IGNORE_SNOWMELT` forced
  on by `PostParseResolver.cpp:2199` (legacy `project.c:221`), so the deck
  returned at the FIRST guard and never reached the per-subcatchment one it
  is named for. Rewritten with two subcatchments on a pack-bearing project.
- **Gate 6's falsifier escaped**, for the same reason: on an
  `IGNORE_SNOWMELT` deck the snow block is skipped and `snow_net_*` never
  leave the sentinel. Added a leg that raises the flag at runtime
  (`openswmm_model_impl.cpp:1189`) after the pack has published — the stale
  arrays `Runoff.cpp:551` calls harmless. Falsifier iv now fails.
- **Falsifier iii is closed by a new gate 7.** §7 called it a known gap. It is
  constructible: below `tbase` nothing melts, and under full areal cover no
  rain reaches the ground, so `imelt + rain·(1 − asc)` is **exactly 0.0**
  while the gage rate is 1.157e-05 ft/s. `>= 0.0` returns the genuine zero;
  `> 0.0` hands the surface rain that went into the snowpack.

### 9.5 Falsifier v is unobservable by construction

`snow_net_*` are written only for subcatchments with a pack
(`SWMMEngine.cpp:1595`) and initialise to −1.0, so the sentinel test already
answers "no pack" correctly and deleting the `snowpack[ui] < 0` guard cannot
change any answer, on any deck. Kept — it mirrors the solver's own two-part
guard — with the redundancy now stated in the code rather than implied.

### 9.6 Notes

- **§5's hard stop reverted to the normal procedure.** `.git/index.lock` had
  cleared by the time this round started, so the stale index was refreshed
  first (`git reset -q HEAD -- <paths>`), and `git diff --numstat` and
  `git diff HEAD --numstat` then agreed at `1  0`.
- **HEAD moved mid-round** — a foreign `962fd48c` (2D bulk vertex-Z setter)
  landed between the build and the commit. No overlap with these files, but
  the worktree was rebased onto it and the full suite, the 14 decks and the
  falsifier sweep were all re-run there rather than reported from the older
  base.
- `tests/unit/legacy/engine/data/hotstart/*.rpt` are TRACKED files that every
  `ctest` run rewrites with fresh timestamps. Not part of either commit, but
  any `git add -A` would sweep them in.
- **§6(d), the `REMOVAL` row arity: six numeric fields parse.**

### 9.7 Still owed

- **S2** — melt still arrives at the configured RAINFALL age and temperature.
  Named in the code at the `HeatWatershed` mixing site.
- The **LID seam** (§2) is untouched and still open.
- **No `ADC` gate**: `asc` is 1.0 on every deck here, so whether the blend is
  continuous across pack formation and disappearance is still untested — and
  gate 7 depends on `asc == 1`, so a partial-cover deck would need a different
  construction.
- `Snow.cpp:349` uses `sin(2π(day−81)/365)` where legacy `climate.c:1176` uses
  `sin(0.0172615·(day−81))` — 2π/364.0, not 2π/365.0. A small seasonal parity
  difference, now live for the first time. Not changed here: `test_snow.cpp`
  pins the modern formula.
