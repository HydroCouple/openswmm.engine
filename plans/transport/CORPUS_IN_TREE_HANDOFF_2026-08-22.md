# The bit-identity corpus was never in the tree — Handoff (2026-08-22)

**For:** the checking agent.
**Base:** `b85b802d`.
**Standing findings:** lessons 1–132.
**Closes:** `SNOW_DIVERGENCE_REGISTER.md` §4 and `tests/parity/snow/README.md`
§5's "add the deck to the corpus runner… the count becomes 15."

**This round moves no engine code.** It is infrastructure, and the finding it
rests on is larger than the item that prompted it.

---

## 1. The finding

Every round of this program has reported **"14/14 reference decks
byte-identical."** Checking what those words refer to:

- **`git ls-files tests/output` returns zero.** Not one of the 14 decks is
  tracked. They live in `tests/output/e0_validation_2026-08-16/decks/` and
  `tests/output/e2_validation_2026-08-16/` — two rounds' scratch directories
  from **August 16** — and `tests/output` is not even gitignored, so they are
  simply untracked files that a clean clone would not have.
- **The runner is not tracked either.** `run_decks.sh` exists as **ten-plus
  independent copies**, one per round output directory, each hardcoding the
  repository root as an absolute path and each carrying its own hand-edited
  comment block.
- **`tests/parity/` is untracked too** — the snow parity deck, its generator,
  and its provenance-carrying baseline included.

So the corpus did real work for 50-odd commits while being one
`rm -rf tests/output` from gone, and the snow deck has sat unwired since
`2992f7c5` because "the runner" was not a thing anyone could edit once.

**A second thing the audit turned up, and it should be in the record even
though it changes nothing today:** the corpus is **10 FV decks, 4 DYNWAVE,
0 KINWAVE, 0 STEADY**. Two thirds of it is one router. That is not a design;
it is what happens when decks are added by whoever needs one and nobody looks
at the shape of the set. `tests/parity/README.md` §4 states it as a table so
that the next person to write "15/15 unchanged" can see what they are
claiming.

## 2. What this changeset does

| | |
|---|---|
| `tests/parity/corpus/decks/*.inp` | the 14 decks, **copied byte-identical** from the two round directories (verified by `cmp`, all 14) |
| `tests/parity/MANIFEST` | 15 decks, one per line, path + **why this deck is here** |
| `tests/parity/run_corpus.sh` | one committed runner |
| `tests/parity/README.md` | what the corpus is, how to add a deck, and §4's composition table |

**The snow deck is entry 15**, referenced in place at
`tests/parity/snow/snow_parity.inp` rather than copied — its generator,
README and baseline are a unit and moving it would break three cross
references for no gain.

**Nothing is deleted.** The originals under `tests/output/` stay where they
are; they are the provenance. Per CLAUDE.md §3 I have not touched them and
have not removed the ten `run_decks.sh` copies — **that is a cleanup for
whoever owns those round directories, and it is mentioned, not done.**

### Design, per the decisions taken this round

- **Before/after, two binaries, no stored baselines.** A stored baseline must
  be regenerated on every intentional change, and the regeneration is exactly
  the moment a wrong number gets blessed — `snow/baseline/`'s own `RETIRED`
  entry is the proof, and O4 showed that hash came from a 17-day-old library.
  Two binaries and a `cmp` need no blessing.
  **The cost, named in the README: this cannot detect drift across rounds.**
  It answers "did my changeset move this", not "is this still right".
- **Not wired into ctest.** It needs two binaries, which ctest cannot supply,
  and the snow deck alone is ~15–24 s.
- The snow deck's stored baseline is untouched and stays a separate
  mechanism, because that deck is *expected* to move and needs attribution
  against a fixed reference.

## 3. What I verified here, and what I could not

**I ran the harness end to end against stub binaries** — a shell script needs
no engine, and a runner nobody has seen fail is lesson 91's shape. Artefacts:
`tests/output/corpus_harness_selftest_2026-08-22/`.

| probe | expected | measured |
|---|---|---|
| A. same stub both sides | 15/15 identical, exit 0 | **as expected**, plus the "cannot fail" note |
| B. one deck's `.out` gains a line | that deck DIFFERS, exit 1 | **as expected** |
| B2. same, after the §3.1 fix | reports the size change | `DIFFERS (size 65 -> 71)` |
| G. same-length byte change | reports a byte count | `DIFFERS (4 of 65 bytes)`, 15/15 moved |
| C. a deck's run fails | NO `.out`, named under `NONZERO_EXIT`, exit 1 | **as expected** |
| D. missing binary | exit 2 before running anything | **as expected** |
| E. duplicate basename in MANIFEST | refuses, names the deck, exit 2 | **as expected**; manifest restored, sha verified |
| F. MANIFEST names a missing deck | refuses, names the path, exit 2 | **as expected** |

### 3.1 A defect in my own instrument, found by probe B

The first version printed **`DIFFERS (0 bytes)`** for the moved deck.
`cmp -l` lists differing byte *positions* and stops at the shorter file, so a
pure length change counts zero. **A truncated `.out` — the exact artefact
`reportfirst` produced two rounds ago — would have reported as differing in
nothing.** Fixed: sizes are compared first and reported separately.

**⛔ What I could not verify: any of this against the real `openswmm`.** The
stubs prove the plumbing, not that the 15 decks run or that they are
byte-stable under a real build. That is §4, and it is the round.

## 4. Validation protocol

1. **Build one binary. Run the corpus against itself:**
   `run_corpus.sh <cli> <cli> tests/output/corpus_<date>/self`
   → must be **15/15 identical, exit 0**, and print the "cannot fail" note.
   This is the harness check; it proves every deck *runs*, which the stubs
   could not.
2. **Then the real thing: build base (`b85b802d`) and patched, run both.**
   This changeset touches no engine source, so the honest expectation is
   **15/15 identical**. If anything moves, that is a finding about the tree,
   not about this changeset — attribute it.
3. **Confirm the copies are the corpus.** For each of the 14, `cmp` against
   its original under `tests/output/…`. I checked this and got 14/14; check
   it again, because if a copy is wrong every future round inherits it.
4. **Time it.** Record wall time for the 14 and for `snow_parity` separately.
   The README claims ~15–24 s for the snow deck from its own README; **that
   number is inherited, not measured by me.** If the corpus is minutes rather
   than seconds, say so — it changes how often it is reasonable to run.
5. **Falsifier sweep:**

   | falsifier | expected |
   |---|---|
   | i. delete a deck file named in MANIFEST | exit 2 naming the path, **before** any deck runs |
   | ii. add a second manifest line with an existing basename | exit 2 naming the basename |
   | iii. point `base` at Release and `patched` at Debug | **flagged, and I do not know.** Same source, different build type — if the `.out` files are identical the corpus is insensitive to build type and the usage warning is theatre; if they differ, the warning is load-bearing. **Either answer is worth having and neither is in the record** |
   | iv. run with the snow deck's line commented out | 14/14 — confirms the snow deck is the only thing separating this from the old script |
   | v. corrupt one byte of a deck under `corpus/decks/` | both sides read the corrupt deck, so it should still be **identical** — the corpus compares runs, not inputs. **If this fails, something is reading the untracked originals** |

6. **Record:** the self-run count, both wall times, and falsifier iii's
   answer.

## 5. Known gaps

- **Drift across rounds is not detectable** — §2's named cost. If a result
  moved three rounds ago unattributed, a before/after run today is clean.
- **The MANIFEST's reason column is unenforced prose.** Nothing checks that a
  deck reaches the path its line claims. A deck whose reason has gone stale
  looks exactly like one whose reason is current.
- **The composition gaps are recorded, not fixed**: 0 water-age decks, 0 heat
  decks, 0 SI decks, 0 STEADY. Adding them is not this round's changeset, and
  each needs its own justification — a deck that exercises nothing new makes
  every run slower and proves nothing.
- **The ten `run_decks.sh` copies still exist** and still work. Nothing stops
  the next round from copying an eleventh. Retiring them means editing other
  rounds' directories, which CLAUDE.md §3 says is not mine to do.
- **`tests/output/` remains untracked and un-ignored.** That is the condition
  that produced this; I have not changed it, because deciding whether round
  artefacts belong in git is a separate call with its own tradeoffs.

## 6. Prepared commit message

```
test(parity): put the bit-identity corpus in the tree, and add the snow deck

Every round of the transport program has reported "14/14 reference decks
byte-identical". git ls-files tests/output returns zero: not one of those
decks was tracked. They lived in two rounds' scratch directories from
August 16, and the runner existed as ten-plus copies of run_decks.sh, one per
round output directory, each hardcoding the repository root.

The decks move to tests/parity/corpus/decks/ byte-identical, with a MANIFEST
that records for each one which code path it reaches that no other deck
reaches, and a single run_corpus.sh.

snow_parity becomes entry 15, referenced in place -- the register called it a
hard prerequisite once S2b landed, and seven snow defects were fixed against
a corpus in which no deck had a [SNOWPACKS] section.

Before/after against two binaries rather than stored baselines: a stored
baseline must be regenerated whenever a change is intentional, and that is
the moment a wrong number gets blessed. snow/baseline/'s own RETIRED entry is
the proof -- O4 showed that hash came from a library 17 days stale. The cost
is that drift across rounds stays invisible, and the README says so.

The runner is not wired into ctest: it needs two binaries, which ctest cannot
supply.
```

---

## 7. Validation results (2026-08-22) — COMMITTED `d633c53e`

**§4.1 passes and §4.2 does not work as written — that is the round's
finding, and it is about the harness rather than the decks.**

**15/15 identical** on the self-run, **14/14 copies verified byte-identical**
to their originals, falsifiers **i, ii, iv, v** as specified, and **iii
answered**. Numbers: `tests/output/corpus_2026-08-22/`.

### 7.1 ⛔ §4.2 compares one engine against itself, and the harness said
###      "cannot fail" for a reason that did not hold

`openswmm` is a **thin driver** that links `@rpath/libopenswmm.engine.6.dylib`.
All of the engine is in the library. Measured: the tree's uncommitted
`DynamicWave.cpp` / `dwflow.c` work — **143 changed lines** — was reverted to
`b85b802d`, rebuilt, the CLI copied aside, the work restored and rebuilt, and
**both CLIs hash `bdd99f49…`. Byte-identical.** Both then resolved the *same*
dylib at load time, so the "before/after" run compared the same engine twice.

The first version of `run_corpus.sh` hashed only the CLI. It printed
`NOTE: base and patched are the same binary -- this run cannot fail`, which
was accidentally true and diagnostically wrong: on this project **an
engine-only changeset always produces identical CLIs**, so that check would
stay silent in exactly the case it exists for.

**Fixed in the changeset**: the runner resolves each side's engine library
(beside the exe, `../lib`, and `../engine` for the CMake build tree), hashes
both into `PROVENANCE.txt`, and bases the vacuity note on the library rather
than the driver — naming the trap explicitly when both sides resolve the same
file. Verified three ways: same build dir → *cannot fail* plus the
two-build-directories warning; a copied CLI with no library beside it →
`<unresolved>` and a note saying the CLI hash cannot answer the question; two
real build directories → no note, and two genuinely different engine hashes
in the provenance.

**§6 of `tests/parity/README.md` now says two binaries means two build
DIRECTORIES.** As written, §4.2 would have been run the way I ran it.

**(130)** *when the unit under test is a shared library, the executable's
hash is not its identity. A before/after that copies the driver and rebuilds
in place measures nothing, and looks exactly like a clean result.*

### 7.2 ⚠ The cost figure was ~50× out, in the opposite direction

§4.4 flagged the ~15–24 s snow figure as inherited. It is wrong. Measured,
Release, per deck:

| deck | s | deck | s |
|---|---|---|---|
| **sdm_fv_o1** | **20.11** | sdm_struct_dw_ard | 0.64 |
| **sdm_fv_o2_superbee** | **17.04** | agree_vj | 0.38 |
| **sdm_fv_o2** | **9.95** | slot_capped | 0.31 |
| orif_legacy | 0.28 | **snow_parity** | **0.30** |

**Three Site-Drainage FV decks are 47.1 s of 49.3 s — 96 %.** The snow deck
is **0.30 s**, among the fastest here, where `MANIFEST` and `snow/README.md`
called it "the slowest deck here by an order of magnitude". A full
before/after is **~100 s**, not "seconds". Both files corrected, with the
per-deck table in README §5.

The step count (8,641) was right; the wall time was never measured on a CLI
build, and it traces to the same era O4 pinned to a 17-day-stale library.

### 7.3 Falsifier iii, which §4.5 said it did not know: build type does not
###      matter here

Release (`-O3 -flto`) against Debug (`-O0 -g`), same source, **two build
directories with genuinely different engine libraries** (`0248b4e1…` vs
`224d8c82…`, both recorded in provenance) — **15/15 identical**.

So the usage warning is not load-bearing for this corpus on this toolchain.
It stays as prudence, labelled as such: nothing measured here says it holds
under a different compiler, `-ffast-math`, or a deck nearer a numerical
boundary. **The first attempt at iii was worthless** — it used a Debug binary
from June 10, which would have measured ten weeks of source change; the
Debug tree was rebuilt from current source before the answer counted.

### 7.4 The rest of §4, in order

- **§4.1 self-run: 15/15 identical, exit 0**, with the "cannot fail" note.
  Every deck runs, which the stubs could not show.
- **§4.3: 14/14 copies byte-identical** to their originals under
  `tests/output/e0_…/decks` and `tests/output/e2_…`.
- **Falsifier i** — deck deleted: `manifest names a missing deck: …`, **exit
  2 before any deck runs**.
- **Falsifier ii** — duplicate basename: `manifest has decks sharing a
  basename -- rename one: agree_vj`, **exit 2**.
- **Falsifier iv** — snow line commented out: **14/14**. The snow deck is the
  only difference from the old script.
- **Falsifier v** — a deck under `corpus/decks/` perturbed (conduit roughness
  0.013 → 0.055): both sides read it and report **identical**, and the run's
  `.out` **differs from the clean run**, so the corrupt deck really was the
  one being read. Nothing reads the untracked originals.
  **My first attempt at v was vacuous** — the `sed` pattern did not match, so
  the deck was never modified and "identical" meant nothing. The second leg
  (compare against the clean run) is what caught that, and it is why the
  falsifier now has one.

### 7.5 The clean-clone check, which is the point of the round

Every one of the 15 MANIFEST paths resolves at `HEAD`, and
`git archive HEAD tests/parity | tar -x` into an empty directory runs
**15/15 identical**. That is the check the corpus never had: entry 15 is the
snow deck, and a manifest whose fifteenth entry is untracked exits 2 on a
clean clone. Its deck, generator and README are tracked with it for that
reason.

**`snow/baseline/` is deliberately NOT tracked** — a generated artefact with
regeneration instructions, and a stored baseline is what §2 argues against.
The consequence is stated rather than hidden: a clean clone can run the
corpus but cannot reproduce the snow deck's *attribution* baseline without
regenerating it.

### 7.6 §5's gaps, and one addition

Unchanged: drift across rounds stays invisible; the MANIFEST's reason column
is unenforced prose; 0 water-age, 0 heat, 0 SI, 0 STEADY decks; the ten
`run_decks.sh` copies still exist; `tests/output/` is still untracked and
un-ignored.

Added: **the corpus cannot affect the build.** `grep` over every
`CMakeLists.txt` finds no reference to `tests/parity`, so this changeset
cannot move ctest, and no suite run was needed to establish that.
