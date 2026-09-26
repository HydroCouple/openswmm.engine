# Self-routed subcatchments recirculate their own runoff — Handoff (2026-08-22)

**For:** the checking agent.
**Base:** `29cbc361`.
**Standing findings:** lessons 1–144.
**This is Finding 3**, the last and largest of the three the age/heat decks
turned up. **It changes routed water**, unlike the two before it.

---

## 1. The defect

Your falsifier-i fixture, against legacy 5.x:

| deck | ours | legacy |
|---|---|---|
| direct | 0.417 | 0.417 |
| cascade (2-deep) | 0.218 | 0.218 |
| **selfroute** | **2.328** | **0.417** |
| cascade (3-deep) | 0.318 | 0.318 |
| all-direct | 0.625 | 0.625 |

Four of five agree to the digit. `selfroute` is **5.6×** out, with **−265 %
continuity**. `421e95c2`'s ledger guard was correct and could not help — **the
water genuinely recirculates**, and the ledger was faithfully reporting a
hydrology that was already wrong.

`assembleRunon` guarded only `out_sc >= 0 && out_sc < n_subcatches()`. Legacy
also requires `k != subcatchIndex` (`subcatch.c:546-548`), so a subcatchment
naming itself as its outlet fed its own runoff back into its own run-on every
step.

**Legacy carries `!= subcatchIndex` in three places and we had one.** That is
lesson 142, and this changeset closes the other two.

## 2. The fix — two sites

| site | what it stops |
|---|---|
| `assembleRunon` (`:6503`) | `&& out_sc != i` — the recirculation itself |
| washoff ledger (`:2876`) | `outlet_node >= 0 \|\| outlet_subcatch == i` before `qual_runoff_load +=` — legacy's `surfqual.c:363` |

**The run-on guard covers three seams at once.** `addRunonAge` and
`addRunonTemperature` live inside the same branch, so a self-routed
subcatchment was also feeding its own **age** and its own **heat** back to
itself. Nobody looked, because no deck had a self-route.

**The washoff site splits one statement into two, deliberately.**
`qual_runoff_load` (the ledger) becomes conditional; `subcatches.total_load`
(the per-subcatchment total) stays unconditional — which is what legacy does,
`surfqual.c:356` sitting *above* its own guard. **The per-subcatchment total
is what this subcatchment washed off; the ledger term is what the system
received.** Conflating them is the same error as Finding 1, one level down.

**⚠ Blast radius is wider than the last two rounds.** The run-on change moves
routed water, so `.out` files move on any deck with a self-route — and **no
corpus deck has one**, so the honest expectation is **18/18 identical**. If a
corpus deck moves, something else is wrong and that is the finding.

## 3. Changeset

```
mod: src/engine/core/SWMMEngine.cpp         (two guards + why)
mod: tests/unit/engine/test_massbalance.cpp (+1 gate)
mod: plans/transport/{IMPLEMENTATION_ROADMAP,PROGRESS}.md
```

**The gate asserts EQUALITY, not inequality**, and that is stronger than the
cascade gate beside it. A self-route is a **no-op** in legacy's model: the
subcatchment discharges to the system exactly as if the outlet named a node.
So `selfroute` must equal `direct` to within rounding — `EXPECT_NEAR(self,
direct, direct * 1e-6)` — rather than merely being "less than the broken
value". Two fixtures differing only in `SA`'s outlet (`JN` vs `SA`), with the
precipitation-unchanged assertion the cascade gate established.

## 4. Validation protocol

1. **The gate must FAIL at base** (revert both hunks). Expect a ratio well
   above 1 in the message. **Quote it.**
2. `ctest -j8` ×3. The gate joins the existing `test_engine_massbalance`
   binary, so **the total stays 160** — expect **159/160**.
3. **Corpus before/after, two build directories. Expect 18/18 identical**,
   because no corpus deck self-routes. **A moving deck is a finding, not a
   pass.**
4. **Re-run your five-deck fixture set against legacy.** `selfroute` should
   join the other four at **0.417**. The other four must not move. **Quote all
   five, both engines** — that table is the result.
5. **Falsifier sweep:**

   | falsifier | expected |
   |---|---|
   | i. drop `out_sc != i` only | the gate fails; the washoff guard alone cannot save it, which is the point of §1 |
   | ii. drop the washoff guard only | the runoff gate **passes** — it is volumetric. **Needs a quality fixture to catch, and I have not written one.** If you can, put a pollutant on the selfroute deck and compare `qual_runoff_load` against legacy's `RUNOFF_LOAD` |
   | iii. make `total_load` conditional too | per-subcatchment washoff totals drop on a self-routed deck while the ledger is unchanged. **Confirms the two-statement split is load-bearing**, and that I put each on the right side |
   | iv. a self-route that is ALSO the only subcatchment | degenerate; should behave as direct. Untested |
   | v. age and heat on a self-routed deck | run-on age/temperature should stop self-feeding. **No gate covers this** — the seams ride the same guard, so they are fixed by construction and unobserved by assertion |

6. **Record:** the base failure ratio, the five-deck table against legacy, and
   falsifiers ii and iii — those are the two I could not close.

## 5. Known gaps

- **The washoff guard has no gate** (falsifier ii). The new gate is
  volumetric; a quality fixture is needed and I did not write one. **This is
  the weakest part of the changeset.**
- **Age and heat on a self-route are fixed but unasserted** (falsifier v).
  They ride the same branch, so they cannot be wrong if the volume is right —
  but "cannot be wrong by construction" is exactly the reasoning lesson 104
  keeps punishing.
- **`RUNOFF_LOAD` is the only quality ledger term checked**, as `RUNOFF_RUNOFF`
  was last round. Evaporation and infiltration under a self-route are
  unexamined in both.
- **No corpus deck self-routes**, so nothing in the standing sweep would
  catch a regression here. Adding one is cheap and I have deliberately not
  done it in the same round that fixes the defect — a deck added alongside its
  own fix cannot demonstrate that it would have caught it.
- **Is a self-route even legal input?** Legacy silently treats it as a no-op.
  We now match that. **Neither engine warns**, and a user who writes
  `SA → SA` by typo gets no signal. Worth a warning; not scoped here.

## 6. Prepared commit message

```
fix(hydrology): a self-routed subcatchment recirculated its own runoff

assembleRunon guarded only that the outlet subcatchment index was in range.
Legacy also requires k != subcatchIndex (subcatch.c:546-548), so a
subcatchment naming itself as its outlet fed its own runoff back into its own
run-on every step.

Measured against legacy 5.x: the selfroute fixture booked 2.328 in against
legacy's 0.417 -- 5.6x, with -265 % continuity -- while direct, 2-deep,
3-deep and all-direct cascades all agreed to the digit. The ledger guard from
421e95c2 was correct and could not help; the water genuinely recirculated and
the ledger faithfully reported a hydrology that was already wrong.

The run-on guard covers three seams at once: addRunonAge and
addRunonTemperature live in the same branch, so a self-route was also feeding
its own age and its own heat back to itself.

The washoff site is legacy's third instance of the same condition
(surfqual.c:363). qual_runoff_load becomes conditional; subcatches.total_load
stays unconditional, as legacy does -- the per-subcatchment total is what the
subcatchment washed off, the ledger term is what the system received.

Legacy carries != subcatchIndex in three places and we had one. Matching a
line is not matching an invariant.

The gate asserts self == direct rather than self < broken: a self-route is a
no-op in legacy's model, so equality is the real statement.
```

---

# 7. Validation results (2026-08-22) — PASSED, three new findings

**Validated on:** HEAD `880e239c` (the base named in §0, `29cbc361`, had moved
two commits by the time this ran). **Committed as** the hash recorded in §8.
Artefacts: `tests/output/selfroute_2026-08-22/`.

**The changeset was correct as delivered and needed no repair.** One gate was
added beyond it (§7.3), one comment carried a wrong unit, and three findings
outside its scope came out of the measurement (§7.6).

## 7.1 The gate at base — §4.1

Both hunks reverted:

```
test_massbalance.cpp:557: Failure
  self evaluates to 101420.31079854172,
  direct evaluates to 18157.173828410578
a self-routed subcatchment is not behaving like a directly connected one:
direct=18157.173828410578 self=101420.31079854172.
Ratio 5.5856881559314635 — greater than 1 means its runoff is recirculating.
```

**Ratio 5.5857**, matching §1's "5.6×" to three digits.

## 7.2 ctest — §4.2

**159/160, three consecutive runs.** Total stayed 160 as predicted: the new
gate joined `test_engine_massbalance`. The single failure is
`test_engine_2d_infil_integration` (`RainOnGridBudgetClosesWithInfiltration`,
`InfiltrationActuallyRemovesWaterFromTheSurface`) — another session's
**untracked** in-flight file, a 2D mesh-cell budget with **zero
`[SUBCATCHMENTS]` in the whole test**, which nothing in this changeset can
reach. It fails identically before and after.

## 7.3 Corpus — §4.3: 18/18 identical, on the second attempt

**The first corpus run reported 4 moved decks and it was an artefact of the
harness.** `force_legacy`, `force_ard`, `orif_legacy` and `sdm_struct_dw_ard`
all differed, which looked exactly like the washoff guard finding a quality
deck. It was not: the two build directories I picked had **different CMake
options** —

```
build/darwin-parity:        OPENSWMM_FAST_MANNING_POW=OFF  OPENSWMM_FAST_XSECT_LOOKUP=OFF
build/darwin-tests-release: OPENSWMM_FAST_MANNING_POW=ON   OPENSWMM_FAST_XSECT_LOOKUP=ON
```

so the run measured `24d51e6e`'s xsect accelerator, not this changeset.
Re-run with `build/darwin` (options identical to `darwin-tests-release`):
**18/18 identical, 0 moved, 0 missing** — the honest expectation from §2, and
confirmation that no corpus deck self-routes.

**`run_corpus.sh` cannot catch this.** Its usage text says "Both binaries must
be built the SAME WAY -- same preset, same build type", and it checks
*nothing*: it hashes both engine libraries and warns when they are the **same**
(the vacuous-run case), but never compares the two build directories' option
sets. The vacuity guard is pointed the wrong way round — the dangerous
direction is not "these are secretly the same build", it is "these are
secretly *different* builds". **Lesson 145.**

Provenance is recorded, and the patched engine hash
`318724a214742c10553e05a2d6395ba8b841b6a72c6cf9c6205666a0b3b6d9c1` was
re-checked after every later rebuild and never changed — the 18/18 stands for
the committed source without a re-run.

## 7.4 The five-deck table against legacy — §4.4

`Runoff Quantity Continuity → Surface Runoff`. Left in **acre-feet** so the
numbers line up with §1's table; the inches column is the same measurement and
is given for the two that matter.

| deck | legacy 5.x | ours @ base | ours @ patched |
|---|---|---|---|
| direct | 0.417 | 0.417 | **0.417** |
| cascade (2-deep) | 0.218 | 0.218 | **0.218** |
| **selfroute** | **0.417** | **2.328** | **0.417** |
| three_deep (3-deep) | 0.318 | 0.318 | **0.318** |
| three_flat (all-direct) | 0.625 | 0.625 | **0.625** |

In inches, `selfroute` goes **2.794 → 0.500** against legacy's 0.500, and its
continuity error **−265.245 % → −1.160 %** against legacy's −1.160 %. The
whole `Runoff Quantity Continuity` block diffs **clean against legacy on all
five decks** after the fix, not just the one row.

**§1's table understated the fix.** The `Flow Routing Continuity` block also
converges: legacy sends **0.123** acre-feet into the conveyance system on the
self-routed deck and so do we, before and after — a self-routed subcatchment's
water reaches no node in either engine, while its runoff ledger still books it
as a system output. That asymmetry is legacy's, we now reproduce it, and it is
the reason the fix is a ledger-visible change with no routing-visible one.

## 7.5 Falsifier sweep — §4.5

Each variant is derived from the pristine file, never stacked
(`tests/output/selfroute_2026-08-22/variants.py`); pristine restoration is
sha256-verified. All three gates, all four variants:

| variant | RunOn (existing) | SelfRoute | **Washoff (new, §7.3a)** |
|---|---|---|---|
| pristine | pass | pass | pass |
| **base** (both reverted) | pass | **FAIL** 5.586× | **FAIL** both legs |
| **i** run-on guard only | pass | **FAIL** 5.586× | **FAIL** self 1.873 vs 1.369 |
| **ii** washoff guard only | pass | pass | **FAIL** cascade 1.522 > direct 1.369 |
| **iii** `total_load` conditional too | pass | pass | **FAIL** SA total 0 vs 0.746 |

- **i** — as §5 predicted: the washoff guard alone cannot save it. `selfroute`
  returns to 2.794 in / −265.245 %, ratio 5.5857. It *also* fails the new
  quality gate, because recirculated water washes off more.
- **ii — §5's open item, now closed.** Both volumetric gates pass while
  `qual_runoff_load` on the cascaded deck reads **1.522 lbs against the
  directly-connected deck's 1.369**. More load reaching the system when SA
  drains through SB than when it drains straight to a node is impossible; that
  is the signature, and it is now a gate. **The comparison against legacy that
  §4.5 asked for is not available**: on this fixture EPA 5.x reports Surface
  Buildup **0.885** / Surface Runoff **0.000** against our **2.500** / **1.369**
  — the buildup and washoff functions themselves diverge, so a cross-engine
  number would measure that gap rather than this guard. Finding 7 below.
- **iii — the split is load-bearing in both directions.** With `total_load`
  made conditional, SA's own washoff total drops to **0** while the ledger term
  is unchanged; with the ledger term made unconditional (ii), the ledger moves
  and the totals do not. Neither statement can be moved to the other side.
  §5's wording ("on a self-routed deck") is off by one deck: on a *self*-route
  `outlet_subcatch == i` is true, so the guard is inert there — the observable
  is the **cascade** deck.
- **iv — the degenerate case is worse, not milder.** A self-route that is the
  only subcatchment: base **5.292 in / −529.722 %**, patched **0.704 in /
  −1.552 %**, legacy **0.704 in / −1.552 %**. **7.5×** rather than 5.6×, because
  no second subcatchment dilutes it. Nothing routed to the node in either
  engine, as expected.
- **v — age and heat are not merely "fixed by construction"; they are
  measured.** On a self-routed deck with `WATER_AGE ON` and `HEAT_TRANSPORT ON`,
  SA's `__WATER_AGE__` column moves **2.207471 h → 4.199100 h**, and
  **4.199100 is exactly what the directly-connected deck reports** — the same
  equality the volumetric gate asserts, holding in the age channel. The
  no-self-route control deck is **byte-identical** base-vs-patched. The
  temperature column reads a flat 20.0 on both, so **the heat seam is still
  unobserved** — the fixture cannot move it, which is owed work, not a result.

## 7.6 Findings outside the changeset

**Finding 4 — the node injection double-counts run-on. Pre-existing;
unchanged by this changeset; the largest of the three.**
`SWMMEngine.cpp:2183-2184` builds the node's lateral inflow as
`q_runoff + q_runon`. Legacy's `subcatch_getWtdOutflow` (`subcatch.c:855-859`)
returns **runoff alone** — run-on is already inside the receiving
subcatchment's `newRunoff`, so adding it again delivers the donor's water to
the node twice. Measured, `Flow Routing Continuity → Wet Weather Inflow`:

| deck | legacy | ours (base and patched alike) |
|---|---|---|
| direct | 0.417 | 0.417 |
| **cascade** | **0.218** | **0.511** |
| selfroute | 0.123 | 0.123 |
| **three_deep** | **0.318** | **0.536** |
| three_flat | 0.625 | 0.625 |

The excess on `cascade` is **0.293** acre-feet, and SA's own runoff is
**0.294** (direct 0.417 − SB-alone 0.123). The same mass twice, to three
digits. Our conveyance system receives 2.3× what our own runoff ledger says
left the surface, and **neither continuity check notices**, because they are
separate balances — Finding 1's shape exactly, one layer over. Note that
`runon_inflow` also carries LID-drain and outfall run-on, which are **not**
inside `runoff[]`, so this is not a one-line deletion.

**Finding 5 — the Subcatchment Washoff Summary is wrong by 453592×.**
`DefaultReportPlugin.cpp:1502,1535` divides `subcatches.total_load` by
453592 ("total_load is in mg"), while `:760` prints `qual_runoff_load` raw —
and both accumulate the *same* `mass` variable at `SWMMEngine.cpp:2888,2893`.
Measured on a deck scaled ×1e8 so the column is readable, the ratio between
the two report rows is **453592.0 exactly**. The practical effect is that the
Washoff Summary prints **0.000 for every subcatchment on every ordinary
deck**, which is why falsifier iii needed a scaled fixture to be observable at
all.

**Finding 6 — buildup/washoff diverge from legacy on the simplest possible
deck.** POW buildup + EXP washoff, one land use at 100 % coverage: legacy
0.885 lbs built up and 0.000 washed off; we build up 2.500 and wash off 1.369.
Both engines close their own quality continuity at 0.000 %. Neither number has
been shown right; they are simply not the same model. This is what blocks
§4.5.ii's cross-engine comparison.

Fixtures for all three are under `tests/output/selfroute_2026-08-22/decks/`.

## 7.7 Deviations from the delivered changeset

1. **One gate added** — `WashoffLoadIsBookedOnlyWhenItReachesTheSystem`, in the
   same file and suite, closing §5's stated weakest point. Three assertions,
   one per falsifier: `self == direct` (i), `cascade < direct` (ii),
   `SA's total_load unchanged and non-zero` (iii). It reads the ledger through
   `as_cpp_engine(e).context()`, which `test_massbalance.cpp` already uses, so
   no report parsing.
2. **One comment corrected** — the `assembleRunon` comment said "2.328 in";
   2.328 is acre-feet, and in inches the same measurement is 2.794. Changed to
   "2.328 acre-feet of surface runoff".
3. **`plans/` not committed**, per the standing rule against committing
   workplans.
4. **The committed `SWMMEngine.cpp` blob was built from HEAD**, not from the
   worktree: another session's uncommitted `has_subcatchments` /
   `sys_temperature` parity work, the no-temperature-source default, and two
   `refreshRenderFieldsIfStale()` calls all sit in the same file. The blob is
   HEAD plus these two hunks and nothing else, and it was **built and run on
   its own (13/13)** before committing.

## 7.8 What is still owed

- **Finding 4** (node injection double-counts run-on) is unfixed and is the
  largest thing on this list. Fixtures: `decks/cascade.inp`,
  `decks/three_deep.inp`.
- **Finding 5** (Washoff Summary ÷453592) and **Finding 6** (buildup/washoff
  parity) are unfixed.
- **The heat seam on a self-route is still unobserved** — the age half is now
  measured, the temperature column is flat 20.0 on the fixture.
- **No corpus deck self-routes**, deliberately, per §5. Adding one is now the
  cheap follow-up it was always going to be, and it can honestly be added in a
  round that is not this one.
- **Neither engine warns on `SA → SA`.** Still worth a warning.
- **`run_corpus.sh` does not check that its two build directories share an
  option set** (§7.3). One `diff` of the two `CMakeCache.txt` option blocks
  would have turned four phantom moved decks into an immediate error.

# 8. Commit

`69467241` — `fix(hydrology): a self-routed subcatchment recirculated its own
runoff`, on parent `880e239c`. Two files:
`src/engine/core/SWMMEngine.cpp` (+36 −4) and
`tests/unit/engine/test_massbalance.cpp` (+159).
