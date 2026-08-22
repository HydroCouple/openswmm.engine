# The bit-identity corpus

**In the tree since:** 2026-08-22. **Before that it was not in the tree at
all** — see §1, which is the reason this directory exists.

A set of decks run through two builds of `openswmm` and compared byte for
byte. It answers one question: **did a changeset move a result it was not
supposed to move?**

```
tests/parity/run_corpus.sh <base-cli> <patched-cli> <outdir>
```

Exit 0 only if every deck is identical and every run produced an `.out`.

---

## 1. What was wrong with it before

Every round of this program has reported "14/14 reference decks
byte-identical". Until 2026-08-22:

- **Not one of the 14 decks was tracked by git.** `git ls-files tests/output`
  returned zero. The corpus lived in `tests/output/e0_validation_2026-08-16/`
  and `tests/output/e2_validation_2026-08-16/` — two rounds' scratch
  directories from August 16 — and would not have survived a clean clone.
- **Neither was the runner.** `run_decks.sh` existed as **ten-plus
  independent copies**, one per round directory, each with the repository
  root hardcoded as an absolute path, each with its own hand-edited comment
  block about what that round expected.
- **`tests/parity/` itself was untracked** — the snow deck, its generator and
  its provenance-carrying baseline included.

So the corpus was real, and it did real work, and it was one `rm -rf
tests/output` from gone. Nothing here is a new idea: it is the same corpus,
put where it can be relied on and given one runner instead of ten.

## 2. Adding a deck

Add a line to `MANIFEST`: the path from the repository root, a tab, and
**why the deck is here** — which code path it reaches that no other deck
reaches. The reason column is not decoration. Ten of the fifteen decks are FV
routing variants (§4), and that happened because decks were added by whoever
needed one, without anybody looking at the shape of the set.

Basenames must be unique. The runner refuses a duplicate: every deck writes
its `.out` under its own basename, so two decks sharing one would overwrite
each other and compare clean. That is the corpus-level form of the
fixture-name collision fixed in `b85b802d`.

Decks must be **self-contained** — no external rainfall or climate files. All
fifteen currently are. A deck with an external dependency would compare fine
on the machine that has the file and fail everywhere else.

## 3. Why there are no stored baselines

A stored baseline has to be regenerated whenever a change is intentional, and
**the regeneration is exactly the moment a wrong number gets blessed.** Two
binaries and a `cmp` need no blessing: the comparison is against the base
commit, whatever the base commit says.

The cost is real and worth naming: **this cannot detect drift across rounds.**
If a result moved three rounds ago and nobody attributed it, a before/after
run today is clean. It answers "did my changeset move this", not "is this
still right".

The snow deck is the exception and keeps a stored baseline with recorded
provenance under `snow/baseline/` — commit, build directory, and the sha256
of both the binary and the dylib. That deck is *expected* to move, so its
movement needs attributing against a fixed reference. **That baseline is a
separate mechanism and the runner does not touch it.**

> **The provenance rule, learned the hard way twice.** A baseline is only
> meaningful against the build it will be compared to. `snow/baseline/`
> carries a `RETIRED` entry — `ed4d0b63…`, runoff continuity +39.543 % —
> which no CLI build reproduces. On 2026-08-22 that hash was reproduced
> exactly by running the deck through the MCP server, whose library was built
> 2026-08-03 and predates the snow melt fix. **The retired baseline was a
> stale binary's output, recorded as a reference.** See
> `plans/transport/O4_RESOLVED_STALE_MCP_LIBRARY_2026-08-22.md`.

## 4. ⚠ What "15/15 unchanged" does not prove

The composition, stated plainly because it keeps being rediscovered as a
caveat in individual handoffs:

| routing | decks |
|---|---|
| FV | **10** |
| DYNWAVE | 4 (2 of them `EULERIAN_ARD`) |
| KINWAVE | 1 (the snow deck) |
| STEADY | **0** |

And by capability:

| exercised by the corpus | decks |
|---|---|
| pollutant transport | 4 |
| snowpacks | **1** (added 2026-08-22) |
| water age (`__WATER_AGE__`) | **0** |
| heat (`__TEMPERATURE__`) | **0** |
| LID | via the SDM decks only |
| SI units | **0** |

**Two thirds of the corpus is one router.** A clean run proves the pollutant
and FV paths are undisturbed. Until the snow deck landed it proved nothing
about any of the seven snow defects fixed behind it; it still proves nothing
about water age, heat, or any land-area unit defect, because no deck reaches
them. Those gaps are tracked in `plans/transport/SNOW_DIVERGENCE_REGISTER.md`
(O6, the SI deck) and in `PROGRESS.md`.

## 5. Cost — MEASURED 2026-08-22, and it is not where anyone thought

**One side of the corpus is ~49 s; a full before/after run is ~100 s.**
Per deck, Release, warm:

| deck | s | | deck | s |
|---|---|---|---|---|
| **sdm_fv_o1** | **20.11** | | agree_vj | 0.38 |
| **sdm_fv_o2_superbee** | **17.04** | | slot_capped | 0.31 |
| **sdm_fv_o2** | **9.95** | | **snow_parity** | **0.30** |
| sdm_struct_dw_ard | 0.64 | | steep_pass | 0.25 |
| orif_legacy | 0.28 | | vj_fv_steep_datum | 0.22 |
| vj_fv_invert_collision | 0.21 | | agree_pass | 0.16 |
| force_ard | 0.15 | | force_legacy | 0.14 |
| slot_uncapped | 0.13 | | | |

**Three Site-Drainage FV decks are 47.1 s of the 49.3 s — 96 %.** Everything
else, all twelve of them, is two seconds together.

**The snow deck is 0.30 s.** Earlier drafts of this file and of `MANIFEST`
called it "8,641 routing steps, ~15–24 s… the slowest deck here by an order
of magnitude". It is among the *fastest*, and the figure was ~50× out. It was
inherited from `snow/README.md`, which took it from the era whose numbers O4
traced to a 17-day-stale library — the same provenance as the retired
baseline. **The step count is right; the wall time was never measured on a
CLI build.**

If this corpus ever needs to be cheaper, the three `sdm_fv_*` decks are the
only place to look.

The corpus is deliberately **not** wired into ctest: it needs two binaries,
which ctest has no way to supply.

## 6. ⛔ Two binaries means two BUILD DIRECTORIES

`openswmm` is a thin driver that links `@rpath/libopenswmm.engine.<v>.dylib`.
**All of the engine is in the library, so two builds of different source
produce byte-identical CLI executables** — measured: a 143-line engine
changeset left `bdd99f49…` on both sides.

So copying one CLI aside, rebuilding in the *same* build directory, and
pointing the runner at both compares **one engine against itself**, and it
looks exactly like a real before/after. Build the two sides in separate build
directories (or separate worktrees) so each CLI's rpath finds its own
library.

`run_corpus.sh` resolves and hashes each side's engine library, records both
in `PROVENANCE.txt`, and says `this run cannot fail` when they are the same
file. That check exists because this round walked into the trap.

### 6.1 Build TYPE, on the other hand, does not matter here

Measured: Release (`-O3 -flto`) against Debug (`-O0 -g`), same source, from
two build directories with genuinely different engine libraries —
**15/15 identical**. The usage text still asks for matching build types, as
prudence rather than as a measured requirement; nothing here says it holds
under a different compiler, `-ffast-math`, or a deck closer to a numerical
boundary.
