# S4 — SD100 and the seasonal constant — Handoff (2026-08-20)

**For:** the checking agent.
**Base:** S3 (uncommitted at the time of writing — **S4 sits on top of S3;
they must land in order**).
**Register:** `SNOW_DIVERGENCE_REGISTER.md` — updated, and §1a is a
**retraction of my own earlier entry**.
**Standing findings:** lessons 1–111.

---

## 1. Why this round exists

The user asked for a defect to report **upstream to EPA SWMM**. I checked,
and **there is none.** Every divergence this program has found in the snow
module is the engine's, not legacy's — including the one I had recorded the
other way round.

That check produced two corrections to our own code.

## 2. D1 RETRACTED — legacy's seasonal constant is right and ours was not

I had recorded: *"`Snow.cpp:349` uses `2π/365` where legacy uses
`0.0172615 = 2π/364`. 364 has no astronomical meaning."*

**Measured:**

| | day 81 | day 172 | day 354 | quarter period |
|---|---|---|---|---|
| legacy `0.0172615` | `+0.000000000` | `+1.000000000` | `−1.000000000` | **91 days exactly** |
| engine `2π/365` | 0 | `+0.999990740` | — | 91.25 days |

**Day 172 is the summer solstice** (31+28+31+30+31+21 = June 21); day 81 is
the vernal equinox. EPA did not fail to divide by 365 — **364 = 4 × 91 was
chosen so the equinox-to-solstice quarter is a whole number of days and the
melt peak lands exactly on the solstice.** Our "correction" moved the
seasonal melt peak to day 172.25 and bought nothing.

Reverted. The constant is now named `kSeasonRad` with the calibration stated,
so the next reader does not repeat the inference.

**How the error happened, and it is worth naming:** I read a constant as an
arithmetic slip without checking what it was calibrated to. That is lesson
69's shape — a declaration is not a value — applied to a *number* rather than
a field. **A magic constant that looks wrong may be a calibration; check what
it makes true before correcting it.**

## 3. F6 — the deck's `SD100` was never read

`si` (the depth at which areal coverage reaches 100 %) was pinned to the
**initial pack depth**. That makes `wsnow >= si` true on the first step and
every step after, so `getArealDepletion` returned 1.0 unconditionally:
**every snow deck in this program sat at 100 % cover**, `rain·(1 − asc)` was
identically zero, and no rain ever reached the ground under a pack unless the
deck wrote an explicit `ADC` row.

Legacy reads it at `snow.c:352` — `si[k] = x[6] / UCF(RAINDEPTH)` for the two
depleting surfaces, with the same slot carrying `snn` on `PLOWABLE`. Ours now
splits it the same way.

**This was O1 in the register — an open question about legacy. It is not:
legacy reads the field. Reclassified to F6, a defect of ours.**

### 3.1 The change is inert where the field is unset

Both implementations guard `si <= 0` and return full cover
(`getArealDepletion`; legacy `snow.c` *"no depletion if depth zero or above
SI"*). A deck with `SD100 = 0` therefore behaves exactly as before — which
covers every existing gate deck. **Verified before writing the gate**, since
the alternative was a divide-by-zero in the ADC branch.

## 4. Also closed: O2 was legacy behaviour

*"The plowable surface is never areally depleted"* is legacy verbatim:
`if (i == SNOW_PLOWABLE) return 1.0;`. Intentional, and it is what makes
gate 11's two melt rates differ. Closed in the register rather than left as
an open question.

## 5. Changeset (uncommitted, on top of S3)

```
mod:  src/engine/hydrology/Snow.cpp        (kSeasonRad — D1 reverted)
mod:  src/engine/core/SWMMEngine.cpp       (SD100 read into si — F6)
mod:  tests/unit/engine/test_transport_snow.cpp  (+2 gates, 15 → 17;
      Opts::sd100 added to the deck writer)
mod:  plans/transport/SNOW_DIVERGENCE_REGISTER.md
```

Syntax-clean under `-Wall -Wextra`. Nothing built or run.

## 6. Validation protocol

1. **Isolated worktree.** S3 then S4, in order — S4's gate numbering assumes
   S3's gates 12–15 exist.
2. **⛔ HARD STOP — lesson 79.** No `CMakeLists.txt` entry expected.
3. Build, zero new warnings. Snow suite, then the full suite.

   **Anticipated failure modes.**

   (a) **`test_snow.cpp` gates that pin `season` or `dhm` will move**, and
   they *should* — the constant changed. Any gate written against `2π/365`
   is asserting the departure this round retracts. **Report each with both
   values.** Same judgement call as S3's (a), and the two rounds compound:
   run them in order and attribute each movement to the right commit.

   (b) **`asc` moves on any deck that sets `SD100`.** None of the existing
   gate decks do (`Opts::sd100` defaults to 0), so this should be confined to
   gate 16 — **if anything else moves, a deck is setting the field
   unintentionally**, which would mean the 7th column was being parsed into
   something before.

   (c) **Gate 16's `wsnow < si` SETUP may not hold** if the pack grows or the
   units surprise. `SD100 = 24` against `SD0 = 6` is a 4× margin, chosen so
   the assertion is not marginal.

4. **Falsifier sweep:**

   | falsifier | expected failing gates |
   |---|---|
   | i. `si = wsnow` again (restore F6) | **16** — both legs. If neither fails, SD100 is unobserved |
   | ii. `2π/365` again (restore D1) | **17** — the day-172 leg. The discriminating margin is **9.3e-6 against a 1e-6 band**, measured, so this is not a coincidence of tolerance |
   | iii. read `SD100` into `si` for PLOWABLE too | **probably nothing** — flagged. `getArealDepletion` returns 1.0 for PLOWABLE before touching `si`, so the value is unread there. Harmless, and **that is exactly why it needs saying**: an unobservable write is how a later change acquires a wrong premise |
   | iv. drop the `si <= 0` guard | should crash or produce `inf` in the ADC branch on any `SD100 = 0` deck — i.e. **most of the suite** |

5. **Prior suites:** full C++ suite, 14/14 decks, ASan/UBSan.

6. **Record:** falsifiers i and ii; every `test_snow.cpp` gate that moved,
   attributed to S3 or S4; and **whether any reference deck sets `SD100`** —
   §4 of the register still has "no reference deck is *known* to have snow"
   as an unconfirmed claim, and F6 makes it matter twice over.

## 7. Known gaps

- **Falsifier iii predicted to escape**, deliberately.
- The register's §4 claim is **still unconfirmed** and now blocks two rounds'
  bit-identity reporting.
- **O3 remains open**: whether a LID under a pack should receive the
  snow-modified rate. Unchanged by S4.

## 8. Prepared commit message

```
fix(hydrology): read the deck's SD100, and restore legacy's seasonal constant

Two corrections found while checking whether any snow-module divergence was
reportable upstream to EPA. None is: every one has been ours.

SD100 -- the depth at which areal snow coverage reaches 100% -- was never
read from the deck; si was pinned to the initial pack depth instead. That
makes wsnow >= si true forever, so getArealDepletion returned 1.0
unconditionally and every snow deck sat at 100% cover, with rain*(1-asc)
identically zero. Legacy reads it at snow.c:352. Decks that leave SD100 at 0
are unaffected: both implementations guard si <= 0 and return full cover.

The seasonal melt constant is restored to legacy's 0.0172615. It was
recorded as a legacy error on the reasoning that "364 has no astronomical
meaning". It has: the period is exactly 364 days, so with the day-81 phase
offset the melt peak falls exactly on day 172, the summer solstice, and the
equinox-to-solstice quarter is a whole 91 days. 2*pi/365 peaks at 172.25.

A magic constant that looks wrong may be a calibration; check what it makes
true before correcting it.
```

---

## 9. Validation results (2026-08-20) — COMMITTED `2992f7c5` (on `c316c83e`)

**158/158 ctest**, **14/14 decks byte-identical**, **71 tests ASan-clean**,
zero new warnings. **Falsifier sweep: 3 of 5 as listed, plus a new one for a
defect this round exposed.** S3 landed first as `c316c83e`, as §6.1 requires.
Numbers: `tests/output/s4_validation_2026-08-20/`.

### 9.1 Reading SD100 is necessary but was NOT sufficient — gate 16 caught it

With `si` correctly read, gate 16 still reported `asc = 1`. **`awe` — the
new-snow ADC index — initialises to 0 where legacy uses 1.0
(`snow.c:199`).** With `awe = 0`, `awesi >= awe` holds on the first depleting
step and `getArealDepletion` returns full cover forever, so SD100 changes
nothing.

It was invisible before this round for the reason F6 itself describes: while
`si` was pinned to the initial pack depth, `wsnow >= si` fired on step 1 and
that branch sets `awe = 1.0` *itself*. **A pack that starts below its SD100
never takes that branch** — so fixing F6 is exactly what exposes it. Fixed in
the same commit; call it **F7**.

Gate 16 is a good gate: it asserted the observable (`asc < 1`) and not the
mechanism, so it failed on the half of the problem the changeset missed.

### 9.2 The other half: depletion needs a curve AND a depth

The **default ADC curve is all ones** (`Snow.hpp:92`) — "no depletion at any
index". So SD100 alone still gives full cover. Gate 16 now sets both.

The same coupling moved two S2a gates, and §6.3(b) predicted exactly this
class of movement while attributing it to the wrong cause. Gates 9 and 11
reached partial cover through an explicit `ADC` row **while relying on `si`
being accidentally nonzero** — the very pinning F6 removes. They now set
`SD100` as well. Nothing else in the suite moved, so no deck was picking up
the 7th column unintentionally.

### 9.3 Falsifier sweep

| falsifier | outcome |
|---|---|
| 0. `awe` initialised to 0 again (F7) | gates 16 **and 11** |
| i. `si = wsnow` again (F6) | gate 16 |
| ii. `2π/365` again (D1) | gate 17 **and** `SnowSeason.SeasonFactorMatchesSinFormula` |
| iii. SD100 into `si` for PLOWABLE too | escapes, as predicted |
| iv. drop the `si <= 0` guard | **escapes** — §6.4 expected a crash |

**iv is unobservable by construction, not by omission.** The guard reads
`if (si_val <= 0.0 || wsnow >= si_val)`. When `si <= 0`, `wsnow >= si` is true
for every non-negative `wsnow`, and `wsnow` is clamped non-negative — so the
second condition already answers, and the ADC branch is unreachable with
`si = 0`. There is no divide-by-zero to expose. Same shape as S1's
falsifier v: a guard dominated by the next condition.

### 9.4 The one `test_snow.cpp` gate that moved (§6.3(a))

`SnowSeason.SeasonFactorMatchesSinFormula`, day 1: **−0.98202758** against the
gate's expected **−0.98130647**. The gate re-derived `2π/365` — it was
asserting the departure this round retracts, so the gate was fixed, not the
code. It now uses legacy's constant, with a pointer to gate 17, which asserts
what the constant is *calibrated to* rather than the formula itself. That is
the stronger of the two and it is why falsifier ii fails in both files.

`dhm` gates did not move: they are ratios, and `setMeltCoeffs` is called
directly with a day.

### 9.5 §6.6 — does any reference deck set SD100?

**No reference deck has a `[SNOWPACKS]` section at all** — `grep -il` over all
14 returns 0, confirmed this round. The register's §4 claim is settled, and
14/14 byte-identity for both S3 and S4 is a real result rather than a
coincidence.

### 9.6 Not committed

`SNOW_DIVERGENCE_REGISTER.md` is a plan document and the standing rule keeps
workplans out of the tree, so §5 lists it but it was left uncommitted. It
also now needs two edits this round produced: **F7** is a new entry, and
**falsifier iv's guard should be recorded as provably redundant** rather than
load-bearing.
