# S2b — verification results (2026-08-21)

**Verifier:** the checking agent. **Verified at:** base `2992f7c5`, isolated
worktree, Release, arm64-osx.
**Status of the code:** committed as `2a58d82c` by a concurrent session *during*
this verification. `Snow.cpp` and `WatershedCommon.cpp` as committed are
**byte-identical** to what was verified, so these results transfer.

## Protocol results (§6)

| step | result |
|---|---|
| 1. isolated worktree at `2992f7c5` | done — then **destroyed mid-sweep** by a concurrent session (see §4) |
| 2. ⛔ no `CMakeLists.txt` entry | **confirmed** — target already registered, no change needed |
| 3. ⚠ lesson 109 SoA count | **7 / 7** for both fields, and the six sites are the *same six* as `snow_melt_perv` (pairwise +2 offset: assign, `grow_to`, `reserve_to`, `erase_at`, `shrink_to_fit`, `reset_state`) |
| 4. build, zero new warnings | 311 units, **0 errors, 0 new warnings** |
| 4. gates | snow binary **18 → 23**, 23/23 pass; full suite **158/158** |
| 5. parity deck | **S2b bit-identical to a clean base build** — §5(e) PASSES. But see finding C |
| 6. 14 reference decks, ASan/UBSan | **NOT RUN** — worktree destroyed |
| 7. falsifier sweep | i ✓, ii **ESCAPED**, iii **ESCAPED**, iv ✓, v/vi/vii **NOT RUN** |

The "158 at base → 163" in §6(4) was a mis-statement: 158 is the *ctest target*
count and does not change, because the 5 new gates live inside an
already-registered target. The gtest count is 18 → 23.

## §4 — the things the author could not check

1. **`openswmm::SimulationContext ctx;` IS default-constructible.** All three
   gates compile, link and run. **The fallback is not needed.**
2. `WaterAgeSource` resolves in `SWMMEngine.cpp` — no extra include needed.
3. No shadowing warning from the `state()` overload pair.
4. SoA count 7 — see above.
5. `<cmath>` fine.

## Findings

### A. The `SWMMEngine` melt-age publish block has ZERO unit coverage ⛔

Falsifiers ii and iii both escaped, and **for one structural reason, not two:**
no gate in the suite runs `SWMMEngine::stepRunoff`. The five new gates split as

| gate | drives |
|---|---|
| `MeltwaterCarriesThePacksResidenceTime` | `arrivingPrecipAge` (SoA set by hand) |
| `SnowfallLowersThePackAgeStrictlyBetweenTheTwo` | `SnowSolver` |
| `ArrivingAgeBlendsStrictlyBetweenItsTwoSources` | `arrivingPrecipAge` (SoA set by hand) |
| `PlowedSnowCarriesTheDonorsAgeAcrossSubcatchments` | `SnowSolver` |
| `APackThatMeltsOutDoesNotCarryItsAgeIntoNewSnow` | `SnowSolver` |

So the publish block — **§3.4's melt-VOLUME weighting** and the **`out_age` read
at the publish site** — is executed by nothing. Consequences:

- **§3.2 is half true.** `APackThatMeltsOut...` really does protect the
  `out_age`/`age` split, and it is a well-built gate: it asserts
  `out_age == 50000` and `age == 0` after melt-out. But it protects it **inside
  the solver**. Falsifier ii edits the **publish site**, which that gate never
  reaches — so §3.2's "the one most likely to catch a rewrite that simplifies
  the two into one" does not hold where the handoff aimed it.
- **§7 iii predicted its own escape for the wrong reason.** It says a catching
  deck "needs two surfaces with different ages *and* different rates at once".
  That would not help: the code is never executed. Closing it needs an
  **engine-level** test, not a richer unit deck.
- §3.4's design decision (weight by melt volume, not area) is therefore
  **entirely unobserved**. Swapping it to area weighting changes nothing any
  gate can see.

**Owed:** one engine-level gate that runs `stepRunoff` on a two-surface pack
with contrasting ages and rates, asserting the published
`snow_melt_age_imperv`.

### B. The handoff's gate numbers are off by one

By file order the new gates are 19–23: 19 `MeltwaterCarriesThePacksResidenceTime`,
20 `SnowfallLowers...`, 21 `ArrivingAgeBlends...`, 22 `PlowedSnow...`,
23 `APackThatMeltsOut...`. The prose numbers them 18–22, so §3.2's "gate 22" is
23, §5(b)'s "gate 20" is 21, §5(c)'s and §7 iv's "gate 21" is 22. Falsifier
results below are given by **name** for that reason.

### C. The snow parity deck's committed baseline is not reproducible ⛔

`tests/parity/snow/README.md` states the baseline is "Reference binary output at
`2992f7c5`". It is not.

| run | sha256 |
|---|---|
| clean base `2992f7c5`, Release | `d9eb7f94…` |
| **S2b at `2992f7c5`, Release** | `d9eb7f94…` — **identical, §5(e) passes** |
| committed `baseline/snow_parity.out` | `ed4d0b63…` |
| main tree, `build-arm64-osx/bin/**Debug**/openswmm` | `ed4d0b63…` — **exact match** |

49,660 of 190,470 bytes differ, from offset 32058 to EOF. The baseline was
produced from the **main tree** — a *Debug* build at HEAD `2a5b964f` carrying
uncommitted `Infiltration.cpp` / `Runoff.cpp` / `Snow.cpp` edits — not from a
clean `2992f7c5`. This is lesson 71 (a count from the main tree is not
attributable) inside the very deck built to enforce attribution.

**This nearly produced a false verdict.** §5(e) calls the parity deck "the single
most valuable check in this round"; comparing S2b against the committed baseline
says DECK MOVED, i.e. exactly the "the plow publication changed the hydrology"
conclusion §3.1 forbids. Only the clean-base control shows the deck did not move.

**Owed:** regenerate `baseline/snow_parity.{out,rpt,SHA256SUMS}` from a clean
worktree at a named commit, in a named build type, and record both in the README.

### D. §5(a) confirmed exactly

`MeltwaterMixesIntoTheSubareaAge` is the **only** existing gate whose body
changed; 5 added, 0 removed. Its change is the declared deliberate one. No other
existing gate moved, which is what §5(a) asked to be checked.

## Falsifier sweep — measured

| # | falsifier | predicted | measured |
|---|---|---|---|
| i | drop the snowfall `mixAge` in `plowSnow` | 19, 22 | **caught** — `SnowfallLowersThePackAgeStrictlyBetweenTheTwo` only (one gate, not two) |
| ii | publish `age` instead of `out_age` in the melt blend | caught by 22 | **ESCAPED** — finding A |
| iii | blend melt age by area instead of melt volume | probably escapes | **ESCAPED** — finding A, not the predicted reason |
| iv | reconstruct the plow transfer instead of publishing it | 21 | **caught** — `PlowedSnowCarriesTheDonorsAgeAcrossSubcatchments`. Gate is **not** decorative |
| v | independent melt fraction in `arrivingPrecipAge` | probably escapes | **NOT RUN** |
| vi | drop the `track_age` guard | nothing, correctly | **NOT RUN** |
| vii | i–iv together | 18–22 | **NOT RUN** |

Falsifier iv is the good news the handoff flagged as its worst case: it is
caught, so the published plow transfer is observed.

## Not done

§6(6) reference decks and ASan/UBSan, and falsifiers v–vii. The isolated
worktree (`openswmm.engine.s2bwt`) and the base worktree (`openswmm.engine.basechk`)
were both **pruned by a concurrent session** while the sweep was running; the
main tree carries unrelated `Infiltration`/`Runoff` edits and an actively moving
HEAD, so it is not a valid substitute. These need a fresh worktree at `2a58d82c`.

## §1 — the continuity finding

Not settled here, and this round's numbers do not depend on it: every result
above is a bracket, a bit-identity, or a falsifier outcome, and none pins a
water-balance value.
