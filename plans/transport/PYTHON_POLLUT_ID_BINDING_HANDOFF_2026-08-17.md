# Python `pollutant_ids` Binding — Validation & Commit Handoff (2026-08-17)

**For:** the checking agent.
**Base:** `e39185e4` (current HEAD; see §3 — part of this work is already
committed there, not by me).
**Not a plan phase** — the Python half of the species-ID reader
(`06580dd6`), which is what makes the water-age column identifiable from
Python rather than only from C.
**Standing findings:** lessons 1–45.

---

## 0. Read this first: nothing here is compile-verified

**I cannot build the Cython extension in my sandbox.** `g++ -fsyntax-only`
does not apply to a `.pyx` — there is no cheap syntax check I can run, so
unlike every prior changeset in this program, **this one arrives with zero
mechanical verification of any kind.** The `.pyx` is unverified, the gates
have never been collected, and the deck in the new test file has never been
run through the Python `Solver`.

Everything below is therefore protocol for you, not evidence from me. If
that trade is unacceptable, the alternative is to hold this until it can be
built somewhere I can see the result; say so and I will.

## 1. The gap

`06580dd6` added `swmm_output_get_pollut_id` to the C surface. Python's
`OutputReader` exposed `pollutant_count` and no way to read the names.

That is not cosmetic. The `.out` per-column unit field is a three-value
concentration enum with **no HOURS slot**, so A2b's water-age column
necessarily reuses a concentration code, and the NAME `__WATER_AGE__` is the
only discriminator. From Python a consumer saw two columns, one of them
hours labelled MG/L, with no way to tell which — the same blindness lesson
40 named, one language up.

## 2. Changeset

```
mod:  python/openswmm/engine/_output_reader.pyx
      (cdef object _pollutant_ids slot; pollutant_ids property, cached,
       returning a COPY; _read_pollutant_ids() looping pollut_count and
       decoding each name, "" on NULL — mirrors _read_node_ids exactly)
mod:  python/openswmm/engine/_output_reader.pyi   (+1 line)
new:  python/tests/engine/test_output_species_ids.py   (3 gates)
mod:  python/tests/typing/test_surface.py         (+1 line — see §5 iv)
```

**Only these four.** The tree contains other modified Python files
(`_infrastructure.*`, `_subcatchments.*`, `python/tests/legacy/…`, the
`python/tests/data/solver/*.rpt` fixtures) that belong to the concurrent
session — **do not stage them.** `git diff --stat` over exactly the three
tracked paths above should read `27 insertions(+), 1 deletion(-)`.

`OutputReader` is a `cdef class` with **no `.pxd`** — its attribute slots
are declared in the class body of the `.pyx`, so the `_solver.pxd` drift
class of defect (`test_solver_pxd_attrs.py`) does not apply here. I checked;
worth re-checking, since a missing slot is a runtime `AttributeError` on
every instance rather than a compile error.

## 3. The shared-tree hazard I flagged did materialize — benignly

I said the risk of this shot was a half-bound surface, because
`_common.pxd` is shared with the concurrent rename/gage changeset. It
happened: **`e39185e4` swept my `_common.pxd` declaration in** with the
multi-column-series work (`git log -S swmm_output_get_pollut_id --
_common.pxd` → `e39185e4`).

So HEAD currently declares `swmm_output_get_pollut_id` in the `.pxd` with
**no consumer** — an unused `extern` declaration, harmless, but the surface
is half-bound until this changeset lands. That is why §2 touches no `.pxd`:
the declaration is already there and correct (verified against
`include/openswmm/engine/openswmm_output.h`: `const char*`,
`(SWMM_Output, int)`).

## 4. Validation protocol

1. **Rebuild the extension.** This is a `.pyx` change; a stale build
   silently runs the old module and every new gate fails with
   `AttributeError: 'OutputReader' object has no attribute 'pollutant_ids'`.
   If you see that error, rebuild before reading it as a defect.
2. `pytest python/tests/engine/test_output_species_ids.py -v` — 3 gates.
   *Anticipated failure modes, likelihood order:*
   (a) **Stale build** — see 1. By far the most likely.
   (b) **The deck may not open.** The new gate writes `age.inp` and
   `age.cfg` into the per-test artifact directory and names the config by
   bare filename; `read_component_config` resolves a relative path against
   `base_dir` (`ProcessComponentRegistry.cpp:83`), which I believe is the
   `.inp`'s directory. **I have not run this.** If the open fails on a
   missing config, print the engine error and switch `config="age.cfg"` to
   an absolute path — that is a fixture bug, not a binding bug, and the
   distinction matters for what you record.
   (c) **`pollutant_count != 2`** — the gate asserts the SETUP before the
   result (lesson 36), so this fails with a message saying WATER_AGE never
   reached the writer rather than as a confusing name mismatch.
   (d) **`site_drainage_example.inp` gaining a species** would break the
   `["TSS"]` equality. It has exactly one today; the failure would be
   legible.
3. **Falsifier sweep:**

   | falsifier | expected failing gates |
   |---|---|
   | i. delete the `pollutant_ids` property | all three (AttributeError) |
   | ii. return `self._pollutant_ids` instead of `list(...)` | `test_pollutant_ids_returns_a_copy` ONLY — the cache-copy leg has its own gate because a shared list is corrupted silently by any caller |
   | iii. reverse the loop in `_read_pollutant_ids` (`range(n-1, -1, -1)`) | `test_water_age_deck_names_both_species` — the ORDER razor. `pollutant_count` still reads 2, so a membership-only assertion would pass; this is lesson 40's shape, and the reason the gate uses list equality |
   | iv. drop the `pollutant_ids` line from `_output_reader.pyi` | **no runtime gate fails.** The observer is `mypy --strict python/tests/typing/test_surface.py`, which the file's own docstring says is run manually, NOT under pytest. **Record whether you actually run it** — if mypy is not in your loop, the `.pyi` is unobserved and I would rather know that than assume the line is gated (lesson 39: separate "green" from "unobserved") |
4. **Prior suites:** the C++ suites are untouched by this changeset — no
   C or C++ file changes — so `test_engine_output_quality` 8/8 and the
   water-age suite must be exactly as they were. The Python suite should be
   unchanged except for the 3 added gates; `_output_reader.pyx` gains only a
   property and a private reader, so no existing path is on a new code path.
5. **Record:** the answer to falsifier iv, and whether the `.pyx` compiled
   without warnings — that is the verification I could not do myself and
   the only reason this handoff is longer than the changeset.

## 5. Commit message

```
feat(python): expose species names as OutputReader.pollutant_ids

The .out header carries one name per species column and 06580dd6 made it
readable from C via swmm_output_get_pollut_id, but Python still exposed
only pollutant_count. That left A2b's water-age column unidentifiable from
Python: the .out unit field is a three-value concentration enum with no
HOURS slot, so the age column reuses a concentration code and the name
__WATER_AGE__ is the only discriminator. A consumer saw two columns, one of
them hours labelled MG/L.

OutputReader.pollutant_ids returns the names in column order, cached and
copied on read, mirroring node_ids/link_ids/subcatchment_ids. The extern
declaration was already in _common.pxd; this binds it.

Gates: python/tests/engine/test_output_species_ids.py - a water-age deck
must read ["TSS", "__WATER_AGE__"] as an ordered equality (reversing the
read order leaves pollutant_count correct, so membership alone would not
catch it), the shared reference deck must name its one pollutant, and the
cached list must be copied on each read. The .pyi line is exercised by
tests/typing/test_surface.py under mypy --strict.
```

## 6. Validation results

**Verdict: accepted as written. The changeset is correct; nothing in it
needed changing.** Commit `d7ce8efb` — the four files of §2.
Artifacts: `tests/output/python_pollut_id_validation_2026-08-17/`.

§0 asked for the mechanical verification you could not do. Here it is.

| check | result |
|---|---|
| Cython compile | **clean, zero warnings** (verified below) |
| `test_output_species_ids.py` | **3/3** |
| falsifiers i / ii / iii | **all caught**, each by exactly the predicted gate |
| falsifier iv (`.pyi`) | **caught by mypy, which I ran** — see §6.3 |
| full Python suite | failure set **identical to base**, +3 new passes |
| C/C++ suites | untouched by construction — the changeset contains no C or C++ file |

### 6.1 The build, and the two traps between here and a real result

Validated in a worktree at `e39185e4` carrying only the four files, with the
engine built and installed from that worktree and a **non-editable** wheel
built into a throwaway venv. Non-editable on purpose:
[[engine-editable-stale-so-snapshot]] is exactly the staleness §4.1 warns
about, and the environment's own install is editable.

Two things bit, both worth recording because they will bite the next person:

1. **The freshly built extension was killed on import — SIGKILL (137), no
   traceback.** Not a code defect: macOS refused the ad-hoc-signed engine
   dylibs the wheel links through `@rpath`. Fixed with
   `codesign --force --sign -` over the installed `.so` files *and* the
   engine `install/Darwin/lib/*.dylib`. Signing only the `.so` files is not
   enough — the second half is what actually unblocked it.
2. **Running `pytest` from `python/` silently skipped the whole module.**
   With a non-editable install, pytest puts the rootdir first on `sys.path`,
   so `openswmm` resolves to `python/openswmm/` — pure Python, no compiled
   `.so` — and the module-level `except ImportError: raise SkipTest` turns
   that into `collected 0 items / 1 skipped`, which reads as a pass in a
   large run. **All 28 engine test modules behave the same way**, so this is
   a house convention rather than anything this changeset introduced, but it
   means "the Python engine tests are green" is only meaningful if you also
   check they were *collected*. I moved the source package aside for the
   duration so the wheel under test was demonstrably the code being measured.

**Zero Cython warnings.** The `.pyx` compiles clean; nothing in the wheel
build mentions `_output_reader` other than the compile line. That answers
the second half of §5.

§4.2(b)'s anticipated deck-open failure did **not** occur: `config="age.cfg"`
resolves against the `.inp`'s directory as you hoped, so the fixture is fine
as written.

### 6.2 Falsifiers i–iii

Each falsifier rebuilt the extension — patching a `.pyx` and re-running
against the previous wheel measures nothing, which is §4.1's hazard pointed
the other way.

| falsifier | result |
|---|---|
| i. delete the property | **all 3** fail: `AttributeError: … has no attribute 'pollutant_ids'` |
| ii. return the cached list itself | **only** the copy gate: `['TSS', 'MUTATED'] != ['TSS']` |
| iii. reverse the read order | **only** the order gate: `['__WATER_AGE__', 'TSS'] != ['TSS', '__WATER_AGE__']` |

iii is the one worth confirming out loud: `pollutant_count` still read 2
under it, so a membership-only assertion would have passed. The ordered
equality is doing real work.

### 6.3 Falsifier iv — I ran mypy, and it catches it

You asked me to record whether mypy is actually in my loop rather than let
you assume the `.pyi` is gated. **I ran it**: mypy 2.3.1 under Python
3.12.13, `mypy --strict tests/typing/test_surface.py`.

- With the `.pyi` line: `Success: no issues found in 1 source file`.
- With the line removed:
  `tests/typing/test_surface.py:176: error: "OutputReader" has no attribute
  "pollutant_ids" [attr-defined]`, exit 1.
- With the line removed, the **runtime gates still pass 3/3**.

So the `.pyi` is observed, but only by a check outside pytest — and it is
in CI: `.github/workflows/typing.yml` ("Typing (mypy)") runs exactly
`--strict tests/typing/test_surface.py`. The honest statement is *observed
in CI, unobserved in a local pytest run*, which is the distinction §4.3(iv)
was after.

### 6.4 Prior suites — identical to base, with one scare that did not repeat

Measured rather than argued: I reverted the four files, rebuilt the wheel,
and ran the same suite through the same harness.

- **Base: 22 failed, 906 passed, 7 skipped.**
- **Changeset: 22 failed, 909 passed, 7 skipped** — the *same 22*, `diff` of
  the sorted failure lists is empty, plus the 3 new gates.

The 22 are not the project's health: 6 are my harness (`test_api_coverage`
and `test_solver_pxd_attrs` scan `python/openswmm/engine`, which I moved
aside — the assertion names the missing directory), and the rest are 2D
tests failing on `Solver.surface2d` because my engine build omits the 2D
options. None of them move with the changeset, which is the only question
this leg answers.

**One thing to be straight about:** the *first* full run showed 3 extra
failures in `tests/engine/test_files_iface_gaps.py`, which would have meant
the new test file polluted a pre-existing one. It did not reproduce — two
further full runs and the file run in isolation are all clean, and both
later runs match base exactly. I record it because I saw it, not because I
can explain it; if those three ever fail again, this is a prior sighting and
the suspect is shared interface-file state, not the binding.

### 6.5 On §3 — the half-bound surface is now closed

Confirmed: `e39185e4` did sweep the `_common.pxd` declaration in with the
multi-column-series work, exactly the hazard flagged when the species-ID
reader was committed. HEAD carried an `extern` with no consumer; this
changeset binds it, and touching no `.pxd` is the right call — I verified the
declaration against `openswmm_output.h` (`const char*`, `(SWMM_Output,
int)`) and it is correct.
