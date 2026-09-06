# O4 driver — the instrument discarded its verdicts — Handoff (2026-08-22)

**For:** the checking agent.
**Base:** `3bdc30a2`.
**Standing findings:** lessons 1–125.

**A defect in the instrument I shipped last round, and it produced a wrong
finding that is now in the record.** Small changeset; the correction matters
more than the code.

---

## 1. What went wrong

The round recorded, as a §7(b) sub-finding:

> `report()` before `end()` is legal and silently lossy. No crash, no error
> code.

**It is not legal, and there is an error code.** `SWMMEngine::report` guards
on `state != EngineState::ENDED` and returns `SWMM_ERR_WRONG_STATE`
(`SWMMEngine.cpp:4924-4928`).

My driver called it like this:

```cpp
if (v.report_before_end) {
    swmm_engine_report(e);      // ← return value discarded
    swmm_engine_end(e);         // ← and here
}
```

Both branches discarded both codes, and so did `close()`. The engine
**refused the call and said so**; the instrument threw the answer away and
the round read the silence as permission.

**That also re-explains the artefacts.** The 24-byte-short `.out` and the
`.rpt` missing `Link C1 (0)` and the real `300.000 → 0.500` timestep ladder
are the signature of a report that **never ran at all** — not of one that ran
early against unfinalised state. The engine behaved correctly throughout.

## 2. Why this is worth a round rather than a quiet edit

The finding is already written into the O4 handoff §10 and the protocol's new
§3a as an engine behaviour. Left alone it would have become a premise: *the C
API permits reporting before ending*. It does not, and a later round building
on that would have been building on nothing.

**Lesson 124:** *an instrument that discards a return value cannot tell "the
engine allowed this" from "the engine refused and nobody asked."* That is
lesson 91's family — a thing that does not check what it claims to measure —
applied to the instrument instead of to a gate. I wrote the gate-level version
of this lesson four rounds ago and then shipped the instrument-level one.

## 3. Changeset (uncommitted)

```
mod:  src/tools/o4_differential/main.cpp
      (check and print rc from end / report / close; the comment at the
       call site records why, so the next reader does not re-derive it)
mod:  plans/transport/IMPLEMENTATION_ROADMAP.md   (lessons 124, 125; the
      round's results)
mod:  plans/transport/SNOW_DIVERGENCE_REGISTER.md (O4 narrowed + the
      sub-finding corrected at source)
```

Syntax-clean under `-Wall -Wextra`. Not built, not run.

## 4. What to expect when it runs

`reportfirst` should now print:

```
  [reportfirst] report() returned <SWMM_ERR_WRONG_STATE>: swmm_engine_report: must call end() first
```

and its `.out`/`.rpt` should be **exactly as before** — the behaviour has not
changed, only the reporting of it. **If the artefacts move, something else is
going on and that is the finding.**

The other four variants should print nothing new and stay byte-identical to
`3bdc30a2`'s run.

## 5. Validation protocol

1. Build with `-DOPENSWMM_BUILD_O4_DIFFERENTIAL=ON`.
2. **The control still governs.** Re-run §3 of the driver handoff: `cli` must
   remain byte-identical to a real `openswmm` run.
3. Re-run all five variants and **diff every output against `3bdc30a2`'s**.
   Only stdout should differ.
4. **Falsifier sweep:**

   | falsifier | expected |
   |---|---|
   | i. discard `rc_report` again | `reportfirst` prints nothing — **the defect, restored.** There is no automated gate for this, which is §6 |
   | ii. invert the guard in `SWMMEngine::report` to allow non-`ENDED` | `reportfirst`'s `.rpt` should then gain the content the round attributed to `end()`. **This is the real test of my re-explanation in §1** — if the artefacts do *not* change, my account is wrong and the original one may have been closer |
   | iii. check `rc` but do not print it | nothing — flagged. A checked-and-swallowed code is the same defect wearing a seatbelt |

5. **Record:** the exact error string and code `reportfirst` prints, and the
   result of falsifier ii — that one decides whose explanation of the
   artefacts is right, and I would rather it were tested than believed.

## 6. Known gaps

- **No gate covers this.** The driver is a diagnostic tool with no test, so
  falsifier i has nothing to fail. Gating it would mean a test that runs the
  tool and greps stdout, which is worth doing **only if the tool outlives
  O4** — right now it is scaffolding for one investigation.
- **`nosave` and `elsewhere` may also have been swallowing codes** that were
  `SWMM_OK` anyway. Now visible; if either prints something, it was invisible
  before and is a new finding, not a regression.
- O4 itself is unchanged: the C API is exonerated, **the MCP server is the
  variable left**, and the standing rule holds — no engine result should be
  quoted from an API-driven run.

## 7. Prepared commit message

```
fix(tools): the O4 driver discarded the engine's return codes

The round that ran o4_differential recorded `report()` before `end()` as
"legal and silently lossy -- no crash, no error code". The engine rejects
it: SWMMEngine::report guards on state != ENDED and returns
SWMM_ERR_WRONG_STATE (SWMMEngine.cpp:4924-4928). The driver discarded that
code on both branches, and discarded close()'s as well.

So the truncated .out and the .rpt missing "Link C1 (0)" and the real
timestep ladder are a report that never ran, not one that ran early against
unfinalised state. The engine behaved correctly and the instrument could not
see it say so.

An instrument that ignores a return value cannot distinguish "the engine
allowed this" from "the engine refused and nobody asked". The corrected
sub-finding is recorded in the divergence register at source, because it had
already been written down as an engine behaviour and would have become a
premise.

O4 itself is unchanged: outcome A stands, the C API is exonerated, and the
MCP server is the variable left.
```

---

## 8. Validation results (2026-08-22) — COMMITTED `97bfa512`

**§1 is half right, and the wrong half is the half it was written to fix.**
There is a return code and the instrument discarded it — that stands, and it
is the round's point. But **the refused call is not `report()`**, and §4's
predicted line never appeared.

Predicted:

```
  [reportfirst] report() returned <SWMM_ERR_WRONG_STATE>: swmm_engine_report: must call end() first
```

Measured:

```
  [reportfirst] end() returned 6: swmm_engine_end: engine must be running or ended
```

### 8.1 The state machine, read after the run rather than before it

- `step()` sets `ENDED` when the simulation finishes
  (`SWMMEngine.cpp:1177`). By the time this driver reports, the engine is
  **already ended**.
- `report()`'s `state != ENDED` guard (4924) therefore **never fires**.
  `report()` **succeeds**, and sets `REPORTED` (4970).
- `end()` accepts only `RUNNING` or `ENDED` (4813). It sees `REPORTED`.
  **`end()` is the call the engine refuses**, with code 6.

**§1's "the truncated `.out` is a report that never ran" is refuted by the
artefact itself.** `o4_reportfirst.rpt` is a complete **204-line** report —
the same length as `o4_cli.rpt` — carrying the whole runoff continuity block,
`Initial Snow Cover 1.500`, `Final Snow Cover 0.340`, `Continuity Error
0.407 %`. A report that never ran cannot write that.

### 8.2 §5.4's falsifiers, and the one that had to be added

| falsifier | predicted | measured |
|---|---|---|
| i — discard the codes again | `reportfirst` goes silent | **silent.** The defect, restored |
| ii — remove `report()`'s `ENDED` guard | the `.rpt` gains what `end()` computes | **nothing changes at all** — same stdout, byte-identical artefacts. The guard never fires, so there is nothing to remove |
| iii — check but do not print | nothing | not run; it is i with extra steps, and i already covers it |
| **ii-b — let `end()` accept `REPORTED`** | *(added: ii aimed at the wrong guard)* | **the `.out` closes** — 190446 → 190470, byte-identical to `cli` — **and the `.rpt` does not change** |

**§5.5 asked which explanation of the artefacts is right. ii-b says: both, for
different files.**

- The **`.out`** truncation is `end()`'s absence. It is `end()` that writes
  the closing block, and ii-b restores it exactly.
- The **`.rpt`** omissions — `All links are stable.` instead of
  `Link C1 (0)`, and a flat `300.000 - 300.000 sec` ladder instead of the
  real `300.000 → 83.462 → 23.220 → 6.460 → 1.797 → 0.500` — are **a report
  that ran early**. ii-b leaves them missing, because by the time `end()`
  computes them the file is already written.

So the original round was right about the report and wrong about the error
code; the correction was right about the error code and wrong about the
report.

**(126)** *a correction is a claim and inherits the burden it was written to
discharge. This one named a guard from a code read without running the thing,
and the guard it named never fires.*

### 8.3 §5's protocol, in order

1. Built with `-DOPENSWMM_BUILD_O4_DIFFERENTIAL=ON`; clean, no warning from
   the changed file.
2. **The control still governs and still passes**: `o4_cli.out` is
   byte-identical to a real `openswmm` run on this build, `.rpt` identical
   with timestamps excluded.
3. **All five variants byte-identical to `3bdc30a2`'s artefacts**, `.out` and
   `.rpt` alike. §4's "if the artefacts move, something else is going on" —
   they did not. That also clears the in-flight `DynamicWave`/`dwflow` work
   in the tree of touching this KINWAVE deck.
4. Sweep above. Every restoration sha256-verified, pristine rebuild clean.
5. Exact string recorded in §8 and in the driver's own comment.

### 8.4 One fix beyond the changeset, flagged

**A full `ctest -j 8` failed `test_engine_heat_watershed`** — not this
changeset's, and not a flake to shrug at. `test_engine_heat_watershed` and
`test_engine_heat_lid` **both write `_h5b.inp` / `_h5b.heat`** into the
shared `data/` working directory and then open them, so one suite reads the
other's deck. Reproduced deliberately: **8 failures in 8 concurrent runs**,
every one the same SETUP assertion — `SURFACE_EXCHANGE did not parse` — which
is the gate catching the wrong *file* rather than a wrong number. Both pass
alone.

This is the same defect the file already documents twenty lines above for the
`site_drainage_model.out` fixture, and it has the same one-line remedy: a
shared `RESOURCE_LOCK`. Applied. The pair now passes 3 of 3 repeated
concurrent runs and the full suite is back to **159/160** — the remaining
failure is Track I's 0.31 % 2D infiltration re-derivation.

**It is worth saying why this was fixed rather than reported.** The S2b round
lost a suite count to this defect's sibling and wrote it off as a flake. A
shared-fixture race does not stay confined to its own suite: it corrupts
whatever count is being quoted that round.

### 8.5 §6's gaps, revisited

- **Still no gate**, and falsifier i still has nothing to fail. Unchanged.
- **`nosave` and `elsewhere` print nothing** now that the codes are visible,
  so their swallowed codes were `SWMM_OK` — as §6 supposed, now measured.
- **O4 itself is unchanged**: outcome A stands, the C API is exonerated, the
  MCP server is the variable left, and the standing rule holds.
- **A separable API question this leaves open:** `end()` after `report()`
  returns `SWMM_ERR_WRONG_STATE` and the `.out` is left unfinalised with
  nothing forcing the caller to notice. Either the state machine should let
  `end()` close a `REPORTED` run, or the truncation should be documented.
  Recorded, not fixed — it is a C API contract question, not this round's.
