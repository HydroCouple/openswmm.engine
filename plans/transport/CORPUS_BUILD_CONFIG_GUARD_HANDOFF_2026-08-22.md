# The corpus runner cannot tell a config difference from a code change — Handoff (2026-08-22)

**For:** the checking agent.
**Base:** `69467241`.
**Standing findings:** lessons 1–147.

**Small changeset, and it lands before Finding 4 on purpose.** Finding 4 is
an engine defect whose verification runs through this instrument. Fixing the
instrument first means that round's corpus result can be trusted; doing them
together means it cannot.

---

## 1. The defect, which is mine

Your round reported **four moved decks** — `force_legacy`, `force_ard`,
`orif_legacy`, `sdm_struct_dw_ard`. **Precisely the quality decks, during a
washoff-guard round.** That is exactly what the defect under test would have
looked like.

It was not the changeset. The two build directories carried different CMake
options (`OPENSWMM_FAST_MANNING_POW`, `OPENSWMM_FAST_XSECT_LOOKUP` OFF in one
and ON in the other), so the run measured `24d51e6e`'s xsect accelerator. A
matched pair gave 18/18.

**`run_corpus.sh` could not catch it, and the reason is a design error in my
guard.** It asks *"are these secretly the same build?"* — a run that cannot
fail. **The dangerous direction is the mirror: two builds that differ by more
than the changeset**, and that produces the tool's loudest possible output.

## 2. The fix

The runner now resolves a `CMakeCache.txt` beside each binary (searching the
binary's directory and up to three parents, which covers `bin/Release/`,
`src/cli/` and staged layouts), extracts every `OPENSWMM_*`,
`CMAKE_BUILD_TYPE` and `CMAKE_CXX_FLAGS*` entry, and compares them.

Three outcomes, all printed **before** the deck table and all recorded in
`PROVENANCE.txt`:

| | |
|---|---|
| configs differ | `⛔ THE TWO BUILDS ARE CONFIGURED DIFFERENTLY`, then the diff |
| no cache found | a NOTE that configurations were **not** compared |
| configs match | one line in `PROVENANCE.txt` |

**It warns; it does not refuse.** Comparing two deliberately different
configurations is a legitimate thing to want to do — the Release-vs-Debug
falsifier from the corpus round was exactly that. The failure this prevents
is not *doing* it, it is doing it *without noticing*.

## 3. What I verified

Ran end-to-end against stub binaries with hand-written caches
(`tests/output/corpus_cfgguard_selftest_2026-08-22/`):

| case | result |
|---|---|
| `FAST_MANNING_POW` OFF vs ON — **your exact case** | fires, prints the diff, before the table |
| identical caches | silent; `PROVENANCE.txt` records the match |
| no `CMakeCache.txt` | the not-compared NOTE |

`bash -n` clean.

**Not verified: the cache-search paths against a real build tree.** The stubs
had a cache one directory up. **Whether the search finds it from
`bin/Release/openswmm` and from `src/cli/openswmm` is §4.1**, and if it does
not, the guard degrades to the "not compared" NOTE — which is safe but
useless, and is the most likely way this changeset underdelivers.

## 4. Validation protocol

1. **⛔ Run it against two REAL build directories and confirm the cache is
   found from both.** If either prints the "not compared" NOTE, the search
   paths are wrong for this project's layout and that is the round's finding.
   Try it from both a `bin/<config>/openswmm` and whatever your parity build
   produces.
2. **Reproduce your own false positive.** Point it at `darwin-parity` and
   `darwin-tests-release` — the two directories that produced "4 decks moved".
   **It must now print the option diff first.** That is the whole round;
   quote the output.
3. **Matched pair** (`build/darwin` against itself, or two matched dirs):
   silent, 18/18.
4. **Falsifier sweep:**

   | falsifier | expected |
   |---|---|
   | i. delete one `CMakeCache.txt` | the "not compared" NOTE, and the run still completes |
   | ii. change only `CMAKE_CXX_FLAGS` | fires — flags change results as surely as options do |
   | iii. change a cache entry the filter ignores (e.g. `CMAKE_INSTALL_PREFIX`) | **silent.** Confirms the filter is a filter and not "diff the whole cache", which would fire on every pair of build directories and be ignored within a week |
   | iv. make the guard `exit 2` instead of warning | Release-vs-Debug becomes impossible to run. **Confirms warn-not-refuse is the right call** — or shows me it is not |

5. **Record:** §4.1's answer for both layouts, and §4.2's output.

## 5. Known gaps

- **The filter is a guess about which options change results.** `OPENSWMM_*`,
  build type and CXX flags cover what bit this round. A toolchain file, a
  different compiler, or `-march=native` inherited from the environment would
  slip past — **the cache records the flags but not the compiler identity**,
  and I have not added `CMAKE_CXX_COMPILER`.
- **It compares configurations, not builds.** A cache that has drifted from
  what was actually compiled (edited and not reconfigured) would compare
  clean. The engine-library hash already in `PROVENANCE.txt` is the partial
  answer.
- **Nothing gates the guard itself.** Like the rest of `run_corpus.sh` it has
  no test; §3's stub cases are a manual harness, not a suite entry.

## 6. ⛔ Finding 4 — scoped here, NOT implemented

**The node injection double-counts run-on**, and it is bigger than the three
findings before it.

| deck | legacy | ours (base **and** patched) |
|---|---|---|
| cascade | 0.218 | **0.511** |
| three_deep | 0.318 | **0.536** |

`SWMMEngine.cpp:2183` feeds the node `q_runoff + q_runon`. Legacy's
`subcatch_getWtdOutflow` returns **runoff alone** — run-on is already inside
the receiver's `newRunoff`. The excess on cascade is **0.293 acre-feet
against the donor's own runoff of 0.294**.

**Our conveyance receives 2.3× what our own runoff ledger says left the
surface, and neither continuity check notices.** That is lesson **147**: two
self-consistent balances can both be wrong, and each certifies the other. It
is why every ledger defect in this program was found by comparing against
*legacy* and never by a balance failing.

**Why it is not a one-line deletion:** `runon_inflow` also carries LID-drain
run-on and outfall run-on, which may or may not already be inside
`newRunoff`. **That question has to be answered per contributor before
anything is removed**, and answering it is most of the round.

Two more, both blocking a clean cross-engine quality comparison and both
predating everything above:

- **Finding 5 — the Subcatchment Washoff Summary prints 0.000 on every
  ordinary deck.** It divides by **453592** while the ledger row prints the
  same mass variable raw; the ratio was measured at **exactly 453592.0**.
- **Finding 6 — buildup/washoff diverge from legacy on the simplest deck**:
  legacy 0.885 / 0.000 against our 2.500 / 1.369. **Unscoped**, and it means
  any cross-engine quality number today measures this gap rather than the
  thing under test.

## 7. Prepared commit message

```
test(parity): the corpus runner could not tell a config difference from code

A corpus run reported four moved decks -- force_legacy, force_ard,
orif_legacy, sdm_struct_dw_ard -- during a washoff-guard round. Precisely the
quality decks, which is exactly what the defect under test would have looked
like. It was the harness: the two build directories had different CMake
options (OPENSWMM_FAST_MANNING_POW, OPENSWMM_FAST_XSECT_LOOKUP), so the run
measured an xsect accelerator. A matched pair gave 18/18.

The existing guard asks whether the two sides are secretly the SAME build --
a run that cannot fail. The dangerous direction is the mirror, and it
produces this tool's loudest output.

The runner now resolves a CMakeCache.txt beside each binary, compares every
OPENSWMM_*, CMAKE_BUILD_TYPE and CMAKE_CXX_FLAGS entry, and prints the diff
before the deck table rather than after. It warns rather than refusing:
comparing two deliberately different configurations is legitimate, and the
failure being prevented is doing it without noticing.
```

---

# 8. Validation results (2026-08-22) — PASSED after one repair

**Validated on:** HEAD `69467241`, the base named in §0.
Artefacts: `tests/output/corpus_cfgguard_2026-08-22/`.

**§4.3 failed as delivered.** The most likely underdelivery you named —
cache-search paths — did not materialise; the guard found every cache from
every real layout. What failed was the opposite end: the guard fired on the
matched pair, over a **version string**.

## 8.1 §4.1 — the cache is found from every real layout

The function body was **extracted from `run_corpus.sh`** rather than retyped
(`probe.sh` sources `cache_opts_extracted.sh`), so the probe cannot drift from
the thing under test.

| binary | depth | result |
|---|---|---|
| `build/darwin-tests-release/bin/Release/openswmm` | 2 up | **28 option lines** |
| `build/darwin-tests-release/src/cli/openswmm` | 2 up | **28** |
| `build/darwin/src/cli/openswmm` | 2 up | **28** |
| `build/darwin-parity/src/cli/openswmm` | 2 up | **28** |
| `build/darwin-parity/src/legacy/cli/openswmm-legacy` | **3 up** | **28** |

No "not compared" NOTE anywhere. The three-parent search is exactly deep
enough — the legacy CLI at `src/legacy/cli/` needs all three, so the search
has **no margin left**. A layout one level deeper degrades silently to the
NOTE.

## 8.2 §4.2 — the false positive, reproduced and caught

`darwin-parity` against `darwin-tests-release`, the two directories that
produced "4 decks moved". The same four decks move, to the identical byte
counts (528 / 525 / 528 / 22251), and now:

```
⛔ THE TWO BUILDS ARE CONFIGURED DIFFERENTLY. Any moved deck below
   may be the configuration, not the changeset. Differences:
     11,12c11,12
     < OPENSWMM_FAST_MANNING_POW:BOOL=OFF
     < OPENSWMM_FAST_XSECT_LOOKUP:BOOL=OFF
     ---
     > OPENSWMM_FAST_MANNING_POW:BOOL=ON
     > OPENSWMM_FAST_XSECT_LOOKUP:BOOL=ON
```

before the table. **Exactly the two options that caused it, and nothing else**
— see §8.3 for why that took a repair.

## 8.3 ⛔ §4.3 failed as delivered — the guard cried wolf on the trustworthy pair

`build/darwin` against `build/darwin-tests-release` — **the matched pair that
produced last round's 18/18** — raised the banner:

```
⛔ THE TWO BUILDS ARE CONFIGURED DIFFERENTLY.
     < OPENSWMM_BUILD_TESTS:BOOL=ON          > OPENSWMM_BUILD_TESTS:BOOL=OFF
     < OPENSWMM_LEGACY_PRERELEASE:STRING=beta.3   > ...beta.1
     < OPENSWMM_PRERELEASE:STRING=alpha.3         > ...alpha.2
```

Three entries, **none of which can change a number**: which extra targets get
built, and two version strings. §4.3 required silence and got the loudest
output the tool produces.

**This is falsifier iii's failure mode arriving on the first real run, from
the direction §5 did not expect.** §5 worried the filter was too *narrow* — a
toolchain or compiler slipping past. The measured problem is that it is too
*wide*: `OPENSWMM_PRERELEASE` drifts between any two build directories
configured at different times, so **the guard would have fired on essentially
every honest before/after on this machine** — and a banner that fires every
time is a banner nobody reads, which is the failure it exists to prevent one
level up. **Lesson 148.**

**Repair:** a named exclusion list, `CFG_IGNORE` — the six
`OPENSWMM_BUILD_{TESTS,UNIT_TESTS,REGRESSION_TESTS,BENCHMARKS,PYTHON,O4_DIFFERENTIAL}`
entries, both `*PRERELEASE` strings, and the two install-side entries.

**It is a named list and not a `BUILD_*` pattern on purpose.**
`OPENSWMM_BUILD_2D` and `OPENSWMM_BUILD_GPU_PLUGIN` match `BUILD_` and change
what the engine computes; a pattern would have swallowed them and made the
guard worse than useless. Falsifier iii.c below is the assertion that it did
not.

After the repair: **silent, 18/18, `PROVENANCE.txt` records the match.**

## 8.4 Falsifier sweep — §4.4

Each case mutates a **copy** of a real `CMakeCache.txt` sitting beside a stub
that `exec`s the real CLI, so the eighteen decks genuinely run while the
caches are free to be wrong. Restoration is sha256-verified
(`stubs/SHA256`), as is `run_corpus.sh` after falsifier iv.

| falsifier | expected | measured |
|---|---|---|
| **i.** delete one cache | NOTE, run completes | NOTE printed, **18/18, exit 0** ✓ |
| **ii.** `CMAKE_CXX_FLAGS` only | fires | fires on that line alone ✓ |
| **iii.a** `CMAKE_INSTALL_PREFIX` | silent | silent — outside the include filter ✓ |
| **iii.b** `OPENSWMM_PRERELEASE` | *(new)* silent | silent — the §8.3 exclusion ✓ |
| **iii.c** `OPENSWMM_BUILD_2D` flipped | *(new)* **must fire** | **fires** ✓ |
| **iv.** `exit 2` instead of warning | refusal | **exit 2, zero decks run** ✓ |

**iii.c is the one that matters** — it is the assertion that §8.3's exclusion
is a named list rather than a pattern, and it is the gate on the repair.

**iv confirms warn-not-refuse.** With `exit 2` the Release-vs-Debug run
refuses at the banner and **not one deck runs**; warn-only completes and
reports. Note the 18/18 in that case is not evidence about Release-vs-Debug
bit-identity: both stubs `exec` the same Release binary and only the caches
differ, which is all falsifier iv is asking about.

## 8.5 Deviations

1. **`CFG_IGNORE` added** (§8.3) — required to meet §4.3, which the changeset
   as delivered did not.
2. **Two falsifiers added** — iii.b and iii.c, gating the exclusion in both
   directions.
3. **`CMAKE_CXX_COMPILER` still not added**, as §5 records. It is a one-token
   change to the include pattern and it closes a real hazard, but no
   acceptance criterion asked for it and it is left where you left it.

## 8.6 Still owed

- **`CMAKE_CXX_COMPILER` is not compared** — §5's own gap, unchanged.
- **The search has no depth margin**: `src/legacy/cli/` already needs all
  three parents (§8.1).
- **A cache that has drifted from what was compiled compares clean** — §5's
  second gap, unchanged; the engine hash in `PROVENANCE.txt` remains the
  partial answer.
- **Nothing gates the guard itself.** `tests/output/corpus_cfgguard_2026-08-22/sweep.sh`
  is a manual harness, not a suite entry — same standing as your §3 stubs.
- **Finding 4 is now unblocked.** The instrument is trustworthy; that round
  can rely on its corpus result.

# 9. Commit

`84984990` — `test(parity): the corpus runner could not tell a config
difference from code`, on parent `69467241`. One file,
`tests/parity/run_corpus.sh` (+65).
