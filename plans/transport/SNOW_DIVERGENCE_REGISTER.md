# Snow module — divergence register

**Policy (user, 2026-08-20):** *correct where legacy is wrong, record every
case.* Legacy `src/legacy/engine/snow.c` is a **reference implementation, not
a specification**. Where it is demonstrably wrong the engine departs; where
the engine departs for any reason it is written down here, so anyone
comparing a deck against EPA SWMM knows what to expect before they run it.

**Maintained at:** every snow-module change. A divergence that is not in this
table is a defect, not a decision.

**Tracked in git (user decision, 2026-08-21)** by an explicit `.gitignore`
negation, as the one exception to the standing rule that keeps `/plans/` out of
the tree. The reason is that this is not a workplan: it is the document a user
needs in order to read a deck comparison against EPA SWMM, and the only place
that explains why `c316c83e` moves their runoff volumes.

---

## 1. Deliberate departures — the engine is right

*(none — see §1a. Every divergence checked so far has been the engine's.)*

## 1a. ⚠ RETRACTED — D1 was wrong, and the engine is the one that diverged

**Original entry:** "`Snow.cpp:349` uses `2π/365` where legacy uses
`0.0172615 = 2π/364`. 364 has no astronomical meaning; the seasonal cycle is
a year." **That reasoning does not survive checking, and it was mine.**

Legacy's constant gives a period of **exactly 364.000 days**, and with its
`day − 81` phase offset:

| | zero crossing | peak | quarter period |
|---|---|---|---|
| legacy `0.0172615` | day 81 (≈ vernal equinox) | **day 172.000** | **91 days exactly** |
| engine `2π/365` | day 81 | day 172.25 | 91.25 days |

**Day 172 is the summer solstice** (31+28+31+30+31+21 = June 21). Legacy did
not fail to divide by 365 — **364 = 4 × 91 was chosen so the equinox-to-
solstice quarter is a whole number of days and the melt peak lands exactly on
the solstice.** The engine's "correction" shifts the seasonal melt peak off
the solstice by a quarter day and buys nothing.

**This is an owed correction in the ENGINE, not an upstream defect.** It was
recorded the wrong way round because the constant was read as an arithmetic
slip without checking what it was calibrated to — lesson 69's shape applied
to a number instead of a field.

**Reverted to legacy's constant in S4**, with gate 17 pinning the three
properties that make it the right one: the peak falls exactly on day 172,
the zero crossing exactly on day 81, and the quarter period is a whole 91
days.

## 2. Defects fixed to match legacy

| # | Site | What was wrong | Fixed in |
|---|---|---|---|
| F1 | `SWMMEngine.cpp` melt coefficients | **`SnowSolver::setMeltCoeffs` had no caller anywhere.** `dhm` stayed at its `assign(0.0)`, so `imelt = dhm·(temp − tbase)` was identically zero and **degree-day snowmelt had never fired** — only rain-on-snow above 0.02 in/hr. Legacy calls it daily from `setTemp` (`climate.c:1176-1180`). | `274b6506` |
| F2 | `Snow.cpp` step 7 | **SWE was reduced by the DRAINED EXCESS, not by the melt** — step 6 overwrote `imelt` with the excess before step 7 read it. Snow that melted but stayed within the free-water capacity was counted twice, as snow *and* as free water, so a pack melting slower than its capacity never depleted. Legacy `routeSnowmelt` reduces `wsnow` by the melt first. | **S3 (this round)** |
| F3 | `Snow.cpp` step 6 | **Rain falling on the snow-covered fraction was discarded.** Legacy adds `rainfall·tStep·asc` to the free-water store. The engine added only the melt — and that rain is excluded from what reaches the ground (`snow_net` carries `rain·(1 − asc)`), so it left the water balance entirely. | **S3** |
| F4 | `Snow.cpp` step 6 | **Free-water capacity taken from PRE-melt SWE.** Legacy applies `fwfrac` to the post-melt value. Shifts the drainage threshold every step and compounds with F2. | **S3** |
| F5 | `Snow.cpp:205` step 0 | **The instant-melt branch discarded its water.** The sub-0.001-inch branch wrote `imelt += (ws + fw)/dt`, then steps 4 and 5 **assigned** `imelt` unconditionally and zeroed it because `wsnow` was now 0. Held aside and added back after routing. | **S3** |
| F6 | `SWMMEngine.cpp` snowpack init | **The deck's `SD100` field is never read**; `si` is pinned to the initial pack depth instead. Legacy reads it at `snow.c:352`. With the default ADC curve of all ones (`Snow.hpp:92`) the effect is that **every snow deck sits at `asc = 1`**, so `rain·(1 − asc)` is identically zero and no rain reaches the ground under a pack unless the deck writes an explicit `ADC` row. Found via lesson 110; reclassified from O1 once legacy was actually read. **`getArealDepletion` already guards `si <= 0` in both implementations, so a deck with `SD100 = 0` is unaffected** — the fix is inert where the field is unset and correct where it is set. | **S4 (this round)** |
| F7 | `Snow.cpp` snowpack init | **`awe` — the new-snow ADC index — initialised to 0 where legacy uses 1.0** (`snow.c:199`). With `awe = 0`, `awesi >= awe` holds on the first depleting step and `getArealDepletion` returns full cover forever, so **reading `SD100` changes nothing**. Invisible before F6 for the reason F6 itself describes: while `si` was pinned to the initial pack depth, `wsnow >= si` fired on step 1 and *that* branch sets `awe = 1.0` itself — **a pack starting below its SD100 never takes it.** Fixing F6 is what exposed it. Found by gate 16, which asserted the observable (`asc < 1`) rather than the mechanism and so failed on the half of the changeset that was still wrong. | **S4** |
| F8 | `SimulationContext.hpp` / `DefaultReportPlugin.cpp` | **The runoff continuity ledger had no snow terms**, where legacy prints `Initial Snow Cover`, `Snow Removed` and `Final Snow Cover` under `Nobjects[SNOWMELT] > 0` (`report.c:521/561`). On any deck with a pack the starting pack was unaccounted input and the surviving pack unaccounted output. **`runoff_snowremov` also had NO WRITER ANYWHERE** — declared, exposed through `SWMM_RUNOFF_SNOWREMOV`, returned by the API and read by callers, while `SnowSoA::removed` accumulated the real figure with no consumer. Same shape as F1 and as the snapshot quality vectors: the third instance, so it is a pattern in this codebase. Measured on the parity deck: **−8.193 % → +0.407 %** (the +1.419 % prediction omitted `Snow Removed`, 0.122 in, and read the surviving pack as SWE only). | `0ad28685` |
| F9 | `Snow.cpp` `plowSnow` | **The ploughed volume used a hardcoded `* 43560.0` commented "acres → ft2".** `subcatches.area` is in PROJECT land-area units — acres only in US, hectares in SI — so the plough volume was **2.471× too small on an SI deck**, the identical defect `SWMMEngine.cpp`'s rainfall-volume site carries a comment about having fixed once. Fixed in the F8 round rather than its own, because `runoff_snowremov` had no writer until then: wiring the value up is what makes the error visible, and shipping a newly-visible wrong number is worse than shipping none. **Unobservable on every DECK this program has** — no corpus deck is SI *and* snowy — but `plowSnow` takes a `SimulationContext`, so the unit system is a field and no deck is needed: `SnowRemoved.PlowingConvertsHectaresNotAcresUnderSI` builds the same pack under CFS and CMS and asserts the 2.471 ratio. It fails on the restored defect, and so does `SnowRemoved.PlowingAccumulatesRemoved`, which had hardcoded the same 43560 literal. **F9 IS ONE INSTANCE OF A WIDER DEFECT — see `LAND_AREA_UNIT_FINDING_2026-08-22.md` (F10).** The sweep lesson 118 obliges finds **twelve more live sites** converting `subcatches.area` with an acres-only factor: two are mass-balance ledger terms (`gw_init_storage` / `gw_final_storage`), several feed pollutant loads, and two fail *silently* rather than numerically — a LID coverage test that stops matching under SI, and a snapshot average that clamps subcatchments out. **The mechanism is the named constant `ucf::ACRES_TO_FT2`**, which reads correct at every call site while being wrong for half the unit systems; the recommended fix is to delete it and let the compiler find the uses, not to patch twelve call sites and leave the next one to be written. **The unit-system-twin gate this round invented is the pattern that closes them.** | `0ad28685` |

**F2–F5 were all unreachable until F1 was fixed** — with `dhm` at zero there
was no degree-day melt for the routing to mis-account. They became live one
commit before they were found. **F7 was unreachable until F6 was fixed**, for
the same structural reason in a different place. *Each fix in this track is
what made the next one visible; assume that continues rather than stops.*

### 2.1 Guards recorded as provably redundant

Recording these matters because a redundant guard reads as load-bearing to the
next person, and removing one "safely" is how a real guard gets removed later.

| guard | site | why it cannot be falsified |
|---|---|---|
| `si_val <= 0.0` | `getArealDepletion` | The condition is `if (si_val <= 0.0 \|\| wsnow >= si_val)`. When `si <= 0`, `wsnow >= si` is already true for every non-negative `wsnow`, and `wsnow` is clamped non-negative — so the second condition answers first and the ADC branch is **unreachable** with `si = 0`. There is no divide-by-zero to expose. S4's falsifier iv dropped the guard and **nothing failed**; §6.4 had expected most of the suite to crash. Same shape as S1's falsifier v. |
| `SD100 → si` on `PLOWABLE` | `SWMMEngine.cpp` snowpack init | `getArealDepletion` returns 1.0 for `SNOW_PLOWABLE` before reading `si`, so the value is **unread** there. S4's falsifier iii wrote it anyway and escaped, as predicted. Harmless today — and worth saying precisely because **an unobservable write is how a later change acquires a wrong premise.** |

## 2.2 Deliberate approximations — named, not hidden

An approximation that is written down is a design choice; the same
approximation unnamed is lesson 64. These are choices.

| # | Where | The approximation, and what it costs |
|---|---|---|
| A1 | `SnowSoA::age` (S2b) | **One water age per snow surface, complete-mixed over `wsnow + fw` together.** Two stores would be more faithful — snow and free water have genuinely different residence times — but the melt path moves water between them inside a single step, and one age is the minimum that carries residence time at all. **What it costs:** meltwater leaving a pack reads slightly younger than it should on a pack with a large free-water store, because the store's fresher water is averaged into the snow's. Bounded by `fwfrac`, so ≤10 % of the pool on a typical deck |
| A2 | `SnowSoA::age` init (S2b) | **The pack's initial water starts at age 0, not at the `INITIAL_STATE` source age.** ✅ **DECIDED (user, 2026-08-22): BOTH YES** — initial pack water takes `INITIAL_STATE`, and the pack age persists through a hotstart. ⬜ **Not yet implemented**; the F8 round is the ledger, not this. Implementing it also makes S2b's falsifier ii observable, which is the second reason for it. A2a's precedent points both ways: it declined to persist subarea ages because a restored age over an unrestored volume is a fiction, but pack SWE *is* restored (`SWMMEngine.cpp:5646`), so the objection does not transfer. **Tied to the hotstart question** — settle both together or neither |

## 3. Open — not yet decided

| # | Site | Issue |
|---|---|---|
| ~~O1~~ | — | **RECLASSIFIED as a defect (F6), not an open question.** Legacy *does* read the field: `snow.c:352`, `Snowmelt[j].si[k] = x[6] / UCF(RAINDEPTH)` for the non-plowable surfaces, with the same slot carrying `snn` on the PLOWABLE row. See §2. |
| ~~O2~~ | — | **CLOSED — matches legacy verbatim.** `snow.c` `getArealDepletion`: *"plowable sub-area not subject to areal depletion"*, `if (i == SNOW_PLOWABLE) return 1.0;`. Intentional, and it is what makes gate 11's two melt rates differ. |
| O3 | LID under a pack | `SWMMEngine.cpp:1735` feeds the LID `subcatches.rainfall` for both hydrology and transport, so the two agree; whether that shared value should be the snow-modified rate is a **fidelity question**, deliberately not answered by S1. |
| ~~O4~~ | API session path vs CLI | **✅ RESOLVED 2026-08-22 — NOT A DIVERGENCE. The MCP server was loading an engine library built 2026-08-03, seventeen days and ~40 commits stale, predating F1.** The re-run reproduces O4 to the digit (infiltration 3.674 in, runoff 3.573 in, continuity +39.543 %). The `.out` parts from `o4_cli.out` at **period 120, 2026-01-06 01:00** — the first hour of the deck's first thaw — with header, IDs and properties byte-identical. Plotting melt against the forcing named it in one pass: **the API run melts snow ONLY in the rain-on-snow window (01-15→01-18, 38 °F, 0.05 in/hr) and never by degree-day — zero melt across eight consecutive days at 47 °F.** That is F1's signature exactly: `setMeltCoeffs` uncalled, `dhm == 0`. **Nothing in the engine is wrong; the three snow commits O4 appeared to indict are correct.** Full account, including the two build-hygiene defects found on the way and lesson 128: `O4_RESOLVED_STALE_MCP_LIBRARY_2026-08-22.md`. **⚠ One link is unverified** — the server's config was not read to confirm which library it loads; the inference is behavioural, and only the venv copy predates F1. **The standing rule stays in force until the library is rebuilt and the re-run comes back byte-identical.** Prior state, kept because §2's elimination and the driver round both remain sound about what they actually tested: **⬜ NARROWED at `3bdc30a2` — the C API is EXONERATED.** `o4_differential` ran the parity deck through the C API five ways in one process from one binary — the CLI sequence, a reopen before stepping, `start(0)`, `report()` before `end()`, and a working directory away from the deck. **All five: 8,640 steps, continuity 0.407 %, snow rows 1.500 / 0.340 — every hydrology number identical**, and the `cli` control is byte-identical to a real `openswmm` run, so the comparison counts. O4's signature is 7.25 in against 12.98 in; nothing in the C API moves by a thousandth. **The MCP server is the variable left** — its own stepping, its working directory, or its process lifetime across `close_model`/reopen. That re-run needs a session with the openswmm MCP tools. **⚠ A sub-finding from that round was wrong, and so was its first correction — both are settled here by measurement.** The round recorded `report()` before `end()` as "legal and silently lossy, no error code"; the correction said `report()` is refused and the artefacts are "a report that never ran". Neither holds. `step()` sets `ENDED` at the end of the run (`SWMMEngine.cpp:1177`), so `report()`'s `state != ENDED` guard (4924) **never fires and `report()` succeeds**, setting `REPORTED` (4970) — the `.rpt` is a complete 204-line report with the full continuity block. **`end()` is the call the engine refuses** (4813 accepts only `RUNNING`/`ENDED`), returning `SWMM_ERR_WRONG_STATE` (6). The two artefacts then have two causes, separated by falsifier ii-b (let `end()` accept `REPORTED`): the `.out` is 24 bytes short because `end()` never wrote its closing block — ii-b makes it byte-identical to `cli` — while the `.rpt`'s missing `Link C1 (0)` and flat timestep ladder survive ii-b, because `report()` had already written them before `end()` computed them. Removing `report()`'s guard outright (falsifier ii) changes nothing at all. **The standing rule stays in force until the server is tested: no engine result should be quoted from an API-driven run.** |
| O5 | `WATER_AGE_SNOW` | **The behavioural half is CLOSED by S2b** — the pack holds age, unconditionally, which is the plan's proposed default. Scoped in `WATER_AGE_SNOW_SCOPING_2026-08-22.md`. Two things survive: **(a)** whether the `OFF` switch should exist at all — *recommendation: retire it from the plan*, nothing has asked for it and pass-through is not a modelling choice but the behaviour the model had while the pack's residence time was unmeasured; **(b) ⬜ the deferral-discipline gap, which is real and small** — a deck writing `WATER_AGE_SNOW OFF` today is **silently ignored**, and silence is the one outcome that is wrong whichever way (a) goes. |
| O6 | No SI deck anywhere | **⬜ OWED, and F10 is what justifies it.** The 14 reference decks are CFS and so is the snow parity deck, so **every land-area unit defect in the engine is invisible to every deck-level check this program runs** — twelve sites' worth, including two mass-balance terms. F9 showed the cheaper route where it applies: a **unit-system twin gate**, same model under CFS and CMS, asserting the 2.471 ratio rather than a value, which needs no deck at all wherever the code under test takes a `SimulationContext`. Use that first; reserve a real SI deck for what only a deck can reach. |

## 4. Where the parity corpus stands

**CONFIRMED (S3 and S4, independently): not one of the 14 reference decks
contains a `[SNOWPACKS]` section.** `grep -il` over all 14 returns 0 in both
rounds. So the 14/14 byte-identity reported for `c316c83e` and `2992f7c5` is a
real result rather than a coincidence — but it is real in a narrow sense that
has to be stated plainly:

**The bit-identity corpus is structurally incapable of observing any of
F1–F7.** That is *why* they survived four phases of this program. "14/14
unchanged" after a snow round proves the pollutant path is undisturbed and
proves nothing whatsoever about the snow path. The same caveat already applies
to water age and heat: no deck in the corpus turns either on.

**✅ BUILT — `tests/parity/snow/` (2026-08-21).** Four subcatchments, three
with packs and **one without as the control**; `SD100` against a graded `ADC`
curve, an unripe deep pack, a sub-threshold thin pack, a live `REMOVAL` row,
and 30 days of meteorology in five named phases. Generated from
`gen_snow_parity.py` so the met values are auditable and regeneration is
byte-reproducible. Baseline at `2992f7c5` in `tests/parity/snow/baseline/`.

**It found a defect on its first run — see §5.** Which is the argument for it,
made better than the argument could make it: seven defects were fixed in this
module against a corpus that could not observe any of them, and the first deck
built to look found an eighth in under an hour.

**⬜ Still owed:** the corpus runner needs a line for it (`run_decks.sh` globs
`e0_validation_2026-08-16/decks/*.inp` plus four named decks; the count becomes
15), and nothing anywhere tests a `snow_only` pollutant on a snow deck.

## 5. ~~⚠ OPEN~~ — FIXED as F8/F9; see §2. The runoff ledger and snow

**Confirmed, quantified, and SMALLER than first reported.** Full write-up:
`SNOW_CONTINUITY_FINDING_2026-08-21.md`, whose §2 is retracted.

**The defect.** The engine's runoff continuity ledger has **no snow terms**.
Legacy `report.c:521/561` prints `Initial Snow Cover`, `Snow Removed` and
`Final Snow Cover` whenever `Nobjects[SNOWMELT] > 0`; `grep -rn "Snow Cover"
src/engine/` returns nothing. On any deck with a pack the starting pack is
unaccounted input and the surviving pack unaccounted output.

**Measured on the parity deck**, on a build with recorded provenance:
**−8.193 %**, and **+1.419 %** once the three rows are supplied. The residual
is `SUB_DEEP`'s 0.753 in — the only subcatchment with `Fout = 0.20`, so
`Snow Removed` is exactly the row that carries it.

**This is a REPORTING defect and that is the whole of it.** An earlier version
of this entry claimed a ~3.4-inch hole in the water balance on top of it, from
a +39.543 % reading. **Both numbers came from a run that no CLI build
reproduces** (see O4), and the residual came from inferring the surviving pack
from an API read rather than reading it from the `.out`. Withdrawn in full.

**(120)** *a table that contains its own refutation still reads as evidence.*
The retracted table showed `SUB_THIN`, pack 0.0005 in, losing as much as
`SUB_DEEP`, pack 4.0 in. Two packs four thousand times apart cannot lose the
same amount of snow — that row said the measurement was broken, before any of
the three candidate causes was worth ranking.

**Done** in the F8 round (`0ad28685`): the three rows, the missing `snowremov` writer, and F9's unit conversion. `F8_SNOW_LEDGER_HANDOFF_2026-08-22.md` carries the validation protocol.

**Separable and minor:** `analysis_get_mass_balance` returns
`runoff_continuity_error: 0.3954` where the report prints `39.543` — fraction
against percent, with nothing in either name saying which.

