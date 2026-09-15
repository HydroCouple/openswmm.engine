# Fixture-name collisions — removed at the root, plus a guard — Handoff (2026-08-22)

**For:** the checking agent.
**Base:** `97bfa512`.
**Standing findings:** lessons 1–127.

**This closes a defect of mine that your round fixed by serialising, and it
found a second instance nobody had hit.**

---

## 1. What your fix established, and why I went further

`test_engine_heat_watershed` and `test_engine_heat_lid` both wrote
`_h5b.inp` / `_h5b.heat` into the shared `data/` working directory. 8 failures
in 8 concurrent runs, every one `SURFACE_EXCHANGE did not parse` — **a content
error, not a file conflict.** That disguise is the whole problem: it is why
S2b's round wrote the sibling off as a flake.

**Both `_h5b` names are mine**, written a round apart into two files.

Your `RESOURCE_LOCK` is correct and it works. I have replaced it, because the
two cases in this file are different in kind:

- **`site_drainage_model.out`** — nine tests share a *real committed fixture*.
  You cannot rename your way out of that. **The lock stays.**
- **`_h5b.*`** — two tests happened to pick the same *scratch* name. Renaming
  removes the hazard instead of scheduling around it: no parallelism cost,
  and it protects against **the next test to pick that name**, not only the
  pair someone noticed.

## 2. The sweep found a second, unlocked instance

Lesson 109's discipline — find every site, not the one that surfaced. Across
**860 distinct fixture literals** in 25 test files, exactly two were shared:

| literal | files | status |
|---|---|---|
| `_h5b.{inp,heat,out,rpt}` | `test_heat_lid`, `test_heat_watershed` | yours, locked → **renamed** `_h5lid.*` |
| **`_out.inp`** | `test_object_deletion_ext`, `test_quality_roundtrip` | **live and unlocked** → renamed `_objdel_out.inp` |

The second has been racing this whole time. It may never have surfaced, or it
may be behind an unexplained failure someone already dismissed — **there is no
way to tell retrospectively**, which is the argument for the guard below
rather than for another lock.

## 3. The guard — configure-time, so nothing has to run

A `FATAL_ERROR` if any two `test_*.cpp` share a `"_name.ext"` literal, with
`CONFIGURE_DEPENDS` so **adding** a file re-runs it — a new file being exactly
when a name gets reused.

**⚠ I could not run CMake. The guard's syntax is unverified and it is the
first thing to check** — same shape as the driver round's §7(a), which passed,
but that is not evidence for this one. If it misfires, the failure mode is a
configure that will not complete, which is loud and immediate.

To confirm it actually bites rather than merely configuring cleanly:

```
# temporarily reintroduce a collision, e.g. in test_heat_lid.cpp
sed -i 's/_h5lid\.inp/_h5b.inp/' tests/unit/engine/test_heat_lid.cpp
cmake -B build          # must FAIL with both filenames named
git checkout -- tests/unit/engine/test_heat_lid.cpp
```

**A guard that has not been seen to fail is not a guard** — that is lesson 91,
and this round exists partly because of it.

## 4. Changeset (uncommitted)

```
mod:  tests/unit/engine/test_heat_lid.cpp            (_h5b.* → _h5lid.*)
mod:  tests/unit/engine/test_object_deletion_ext.cpp (_out.inp → _objdel_out.inp)
mod:  tests/unit/engine/CMakeLists.txt               (h5b lock → note;
      + the configure-time collision check)
mod:  plans/transport/IMPLEMENTATION_ROADMAP.md      (lessons 126, 127;
      the round's results; the owed C API contract row)
```

The two `.cpp` files pass `g++ -std=c++20 -fsyntax-only`. **CMake not run.**

## 5. Validation protocol

1. **Configure first.** If it fails, that is §3 and it is mine.
2. **Make the guard fail on purpose** (§3's recipe). Report the message.
3. `ctest -j8` **three times**. The `-j8` repeat is the point: the defect this
   closes was invisible at `-j1` and both suites passed alone.
4. `ctest -j1` once, to confirm the renames did not change any behaviour.
5. **Falsifier sweep:**

   | falsifier | expected |
   |---|---|
   | i. reintroduce `_h5b.inp` in the LID suite | **configure fails** naming both files |
   | ii. reintroduce `_out.inp` | same |
   | iii. remove `CONFIGURE_DEPENDS` | nothing on a clean tree — **flagged.** It only shows when a file is *added* without a manual reconfigure, which no falsifier here reproduces. Owed, and cheap to accept |
   | iv. restore the `h5b_heat_deck` lock as well | nothing — the names no longer collide, so the lock is inert. **Confirms the rename supersedes it** rather than merely duplicating it |

6. **Record:** the guard's failure message; whether `-j8` is now clean three
   times running; and **whether `_objdel_out.inp`'s rename moved anything** —
   it should not, but that pair has never been run under a lock, so its
   behaviour under contention is unmeasured.

## 6. Known gaps

- **The guard matches a `"_name.ext"` literal.** A test that builds a fixture
  name by concatenation, or uses one without a leading underscore, is
  invisible to it. The convention in this suite is the leading underscore, so
  this covers what exists — **it is a convention check, not a filesystem
  check.**
- **The real fix is a per-test working directory.** Every test here shares
  `data/`, so this whole class of defect exists by construction. Renaming and
  guarding is the cheap containment; giving each test its own cwd would make
  it impossible. **Not attempted** — it touches every `add_test` and the
  fixtures some tests legitimately read from `data/`.
- The `site_drainage_model.out` lock is untouched and still needed.

## 7. Prepared commit message

```
test: remove two fixture-name collisions at the root, and guard the class

test_heat_watershed and test_heat_lid both wrote _h5b.inp / _h5b.heat into
the shared data/ working directory: 8 failures in 8 concurrent runs, every
one "SURFACE_EXCHANGE did not parse" -- a content error rather than a file
conflict, which is why the sibling instance was written off as a flake.

A RESOURCE_LOCK fixed it by serialising the pair. Unique names are better for
an accidental collision: no parallelism cost, and they protect against the
next test to pick the name rather than only the pair someone noticed. The
site_drainage_model.out lock stays -- nine tests genuinely share a committed
fixture there and renaming cannot help.

A sweep over 860 fixture literals in 25 files found a second, unlocked
instance: _out.inp, shared by test_object_deletion_ext and
test_quality_roundtrip, racing since it was written.

The configure-time check is what stops a third. CONFIGURE_DEPENDS so adding
a test file re-runs it, that being exactly when a name gets reused.
```

---

## 8. Validation results (2026-08-22) — COMMITTED `b85b802d`

**§3's warning did not materialise: the guard configures clean first try, and
it bites when made to.** ctest **159/160 at `-j8` three times running and once
at `-j1`**, the same single failure each time — Track I's 0.31 % 2D
infiltration re-derivation. Numbers:
`tests/output/fixture_collision_2026-08-22/`.

### 8.1 The guard fails, and it names both files

Falsifier i, `_h5b.inp` reintroduced in the LID suite — **configure EXIT=1**:

```
  Two or more unit tests share a scratch-fixture name, and they all run with
  data/ as their working directory:

    "_h5b.inp" written by both test_heat_lid.cpp and test_heat_watershed.cpp

  Rename one of them.  A RESOURCE_LOCK also works but costs parallelism and
  only protects the pair you noticed.
```

Falsifier ii, `_out.inp` reintroduced — same, naming
`test_object_deletion_ext.cpp and test_quality_roundtrip.cpp`. Both restored
sha256-verified.

### 8.2 ⚠ Falsifier iii is NOT unreproducible, and `CONFIGURE_DEPENDS` is
###     load-bearing

§5's table flagged iii as something "no falsifier here reproduces". It
reproduces with one probe file:

| | dropped `CONFIGURE_DEPENDS` | kept it |
|---|---|---|
| add `test_zz_collision_probe.cpp` reusing `"_h5b.inp"`, then **build** | **build EXIT=0** — the guard never runs and the collision ships | **build EXIT=1** at the re-configure, naming `test_heat_watershed.cpp and test_zz_collision_probe.cpp` |

That is the whole case for the keyword, measured rather than asserted. Probe
removed; the tree reconfigures clean.

**(128)** *"no falsifier reproduces this" is a statement about the falsifiers
that were written, not about the property. A guard whose trigger condition is
"a file is added" is tested by adding a file.*

### 8.3 §5.6's third question: the rename moved nothing, and my first
###     measurement of it was wrong

Stressing the renamed pair by launching **8 copies of each binary** gave 8
failures in 16 runs — and it is an artefact of the harness, not a finding.
The control says so: **8 copies of `test_engine_object_deletion_ext` alone,
with no other suite running, fail 4 of 8.** The suite races *itself*, on a
`swmm_street_count` assertion, which has nothing to do with
`test_quality_roundtrip`.

Run the way ctest actually runs — **one copy of each, 12 paired rounds — 0
failures in 24.** The `_out.inp` rename is prophylactic: nothing was observed
failing before it and nothing after.

**(129)** *N copies of one test binary is not a model of `ctest -jN`. ctest
runs each test once; a self-collision reproduces under the stress harness and
never in the suite, and reads as the cross-suite race you were looking for.*

**This does not weaken the `_h5b` finding**, and the reason is worth stating:
that one was reported by a **real `ctest -j8` run** before any stress harness
existed, and its assertion — `SURFACE_EXCHANGE did not parse` — can only come
from the LID suite's config file, since a second copy of the watershed suite
writes byte-identical content. The stress harness corroborated it; it was not
the evidence.

**A minor thing it did turn up:** `test_engine_object_deletion_ext` is not
safe against a second copy of itself. Harmless under ctest, which runs it
once. Recorded, not fixed.

### 8.4 The sweep, independently

§2 reports 860 literals in 25 files. Counted independently over the directory:
**865 distinct scratch-fixture literals across 147 `test_*.cpp` files** — the
literal count is close, the file count is not. It does not change the
conclusion, because both the independent sweep and the guard glob **all 147**
and find **no remaining shared literal**. Coverage is complete either way.

### 8.5 §5.5's falsifier iv, answered by construction

The `h5b_heat_deck` lock is gone from the generated `CTestTestfile.cmake`
(`site_drainage_model_out` survives on its nine tests), and the heat pair is
clean across three `-j8` suite runs without it. Restoring the lock could only
be inert, since the names no longer collide — the rename supersedes it rather
than duplicating it.

### 8.6 §6's gaps, unchanged and worth keeping visible

- **It is a convention check, not a filesystem check.** A concatenated name,
  or one without a leading underscore, is invisible to it. What exists in
  this suite follows the convention; nothing enforces that it keeps to it.
- **The real fix is a per-test working directory**, which would make the
  whole class impossible. Not attempted, and the reason stands.
- The `site_drainage_model.out` lock is untouched and still needed.
