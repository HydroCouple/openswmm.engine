# O4 — the differential driver — Handoff (2026-08-22)

**For:** the checking agent.
**Base:** `0ad28685`.
**Protocol:** `O4_API_CLI_DIFFERENTIAL_2026-08-22.md` — **§2 and §3 are
binding on this round.** §2 lists what is already eliminated; §4 forbids
instrumenting before localisation.
**Standing findings:** lessons 1–123.

**This round delivers an INSTRUMENT, not a fix.** The cause of O4 is unknown
and my last hypothesis about it was eliminated by a code read. Guessing again
would be lesson 110 — predicting a symptom is not diagnosing it.

---

## 1. The finding that shaped this round

`lifecycle_open_model` and `lifecycle_run_simulation` are **MCP tool names,
not engine entry points.** There is no `lifecycle_run_simulation` anywhere in
`src/`.

So the 7.25 in measurement was taken **through the MCP server** — and §3
already anticipated this: *"the variable is the MCP server itself — its own
stepping, its working directory, its process lifetime."* The server is a
**third variable**, and until it is removed the differential compares two
things that differ in more than one way.

That is what this driver is for, and it is why §3 asks for a driver **in the
tree, not through the MCP server**.

## 2. What it does

`o4_differential <deck.inp> <out-prefix>` runs the same deck through the C
API **in one process, from one binary**, four ways — one variable each:

| variant | differs from the CLI by | why it is a suspect |
|---|---|---|
| `cli` | **nothing** — byte-for-byte `src/cli/main.cpp:76-131` | **the control** |
| `reopened` | one `open`/`close` before the real open | a session driver has already opened the deck by the time it steps |
| `nosave` | `start(0)` instead of `start(1)` | `start`'s only argument; a driver that does not want an `.out` passes 0 |
| `reportfirst` | `report()` before `end()` | a driver that reports early has not had the final continuity terms computed |
| `elsewhere` | working directory moved to the temp dir | §3 names the MCP server's cwd (`.../Projects/default`) explicitly |

The deck path is resolved to **absolute** first, so `elsewhere` tests the
working directory and not the engine's ability to resolve a relative path
from one. The cwd is restored on scope exit, so one variant cannot leak into
the next.

**The tool does not compare anything.** `cmp` and `diff` are the protocol's
step 4, and a comparison written into the instrument is one nobody can check
independently.

## 3. ⛔ The control comes first, and it can invalidate the round

`<prefix>_cli.*` **must match a real CLI run of the same deck.**

```
openswmm  <deck>.inp /tmp/real_cli.rpt /tmp/real_cli.out
o4_differential <deck>.inp /tmp/o4
cmp /tmp/real_cli.out /tmp/o4_cli.out
diff /tmp/real_cli.rpt /tmp/o4_cli.rpt      # header timestamps may differ
```

**If they differ, stop.** It means the driver does not reproduce the CLI, so
nothing it says about the other variants means anything. That is the round's
first result either way — report it.

## 4. Build

Off by default; it is a debugging instrument, not a shipped artefact:

```
cmake -B build -DOPENSWMM_BUILD_O4_DIFFERENTIAL=ON
cmake --build build --target o4_differential
```

Deck: `tests/output/s2b_validation_2026-08-21/parity/snow_parity.inp`
(fully inline — no external `[TIMESERIES]` or temperature file, which
**already weakens** the working-directory hypothesis for data loading; it can
still bite on output paths).

## 5. What each outcome means, decided in advance

**A. All five variants agree, and match the real CLI.**
→ The C API is not the variable. **The MCP server is**, and the search
narrows to its stepping, its cwd, or its process lifetime across
`close_model`/reopen. Re-run with the server as the only difference.
*This is the outcome I consider most likely, and it is a real result.*

**B. One variant diverges.**
→ Name it, and **do not fix anything yet**. Bisect in time as §3 asks:
report at the finest step both can emit and find the first period where the
two `.out` files part. That names the phase before anyone reads source.

**C. `cli` itself does not match the CLI.**
→ §3. Stop and report.

**Only after A, B or C is settled** does §4 permit reading `dhm`, `season`
and `last_melt_doy_` — the three variables the eliminated hypothesis
motivated.

## 6. Changeset (uncommitted)

```
new:  src/tools/o4_differential/main.cpp
new:  src/tools/o4_differential/CMakeLists.txt
mod:  src/CMakeLists.txt   (option + guarded add_subdirectory)
```

Syntax-clean under `-Wall -Wextra`. **Not built, not run** — the option
default is OFF, so an ordinary build is unaffected, and *that itself needs
confirming* since I could not run CMake.

## 7. Anticipated failure modes

My record on these is 5 of 33, so weight §5 above this list.

(a) **The build option may not wire up.** I edited `src/CMakeLists.txt` by
pattern match and could not run CMake. If `o4_differential` is not a target,
that is the first thing to fix and it is mine.

(b) **`report()` before `end()` may be illegal**, not merely different — it
could crash or assert rather than produce a divergent report. If it does,
that is a finding about the C API's contract and worth recording, but drop
the variant rather than working around it.

(c) **`swmm_engine_close` after `end` may already be implied.** The CLI calls
both; I mirrored it. If `close` is a no-op there, `reopened` is testing less
than it claims.

(d) **`elsewhere` may change where the `.rpt`/`.out` are written** rather
than what is in them, if the prefix resolves relative. I made the prefix
absolute for exactly this reason — **verify that it did**, because if the
files land in the temp dir the comparison silently reads stale files from a
previous variant.

## 8. Known gaps

- **The variant list is a hypothesis about what a session driver does
  differently.** It is not derived from the MCP server's source, which I did
  not read. If the server does something not on this list, all five variants
  can agree and the cause still be real — which is outcome A, and why A is
  written as a result rather than a dead end.
- **No gate.** This is a diagnostic tool, not a tested behaviour. Adding a
  gate would mean pinning a divergence that is not yet understood — lesson
  55's shape.
- The standing rule holds until O4 is settled: **no engine result should be
  quoted from an API-driven run.**

## 9. Prepared commit message

```
tools: an in-tree A/B driver for the O4 API/CLI divergence

O4 measured 7.25 in delivered to the ground through the API against 12.98 in
through the CLI, on the same deck and the same precipitation. But
lifecycle_open_model and lifecycle_run_simulation are MCP tool names, not
engine entry points -- there is no lifecycle_run_simulation in src/ -- so the
measurement came through the MCP server, which the protocol names as a third
variable.

o4_differential removes it: one deck, one process, one binary, through the C
API five ways, changing one thing at a time -- a reopen before stepping,
start(0), report before end, and a working directory away from the deck. The
`cli` variant is byte-for-byte src/cli/main.cpp and is the control; if it
does not reproduce a real CLI run, nothing else the tool says counts.

It compares nothing. cmp and diff are the protocol's step 4, and a
comparison built into the instrument is one nobody can check.

Off by default (OPENSWMM_BUILD_O4_DIFFERENTIAL).
```

---

## 10. Validation results (2026-08-22) — COMMITTED `3bdc30a2`, **OUTCOME A**

**§7(a) did not happen.** The option wired up first try; an ordinary build is
unaffected (156 targets, no `o4_differential`), and with
`-DOPENSWMM_BUILD_O4_DIFFERENTIAL=ON` the target builds and links clean.
ctest **159/160**, unchanged — the failure is Track I's 0.31 % 2D
infiltration re-derivation, which predates this changeset.
Numbers: `tests/output/o4_validation_2026-08-22/`.

### 10.1 ✅ The control passes

`o4_cli.out` is **byte-identical** to a real
`openswmm snow_parity.inp` run, and `o4_cli.rpt` is identical with the two
timestamp lines excluded. §3 is satisfied, so everything below counts.

### 10.2 **OUTCOME A. No variant reproduces O4.** The C API is not the variable.

| variant | `.out` vs `cli` | `.rpt` vs `cli` | continuity | Initial / Final Snow Cover |
|---|---|---|---|---|
| `cli` | — | — | **0.407 %** | 1.500 / 0.340 |
| `reopened` | same | same | 0.407 % | 1.500 / 0.340 |
| `nosave` | differs (390 B stub) | **same** | 0.407 % | 1.500 / 0.340 |
| `reportfirst` | differs (truncated) | differs | 0.407 % | 1.500 / 0.340 |
| `elsewhere` | same | same | 0.407 % | 1.500 / 0.340 |

**All five agree on every hydrology number.** 8,640 steps each, identical
continuity error, identical snow rows. O4's signature is 7.25 in against
12.98 in delivered to the ground — nothing here moves by so much as a
thousandth.

So the search narrows exactly as §5(A) said it would: **the MCP server is the
remaining variable** — its own stepping, its working directory, or its process
lifetime across `close_model`/reopen. **This session has no openswmm MCP tools
available**, so the re-run with the server as the only difference is the next
round's, not this one's.

### 10.3 Two variants differ, and neither is O4 — but one is a real API finding

**`reportfirst`: `report()` before `end()` is legal and lossy.**

> **⚠ CORRECTED 2026-08-22 (`O4_DRIVER_RETURNCODE_FIX_HANDOFF`, §5).** "No
> error code" was wrong: the driver discarded the return codes. There is one,
> and it comes from **`end()`**, not `report()` — `step()` has already set
> `ENDED`, so `report()` succeeds and sets `REPORTED`, which `end()` then
> refuses with code 6. The artefact list below is accurate; only its
> attribution moves — the `.out` truncation is `end()`'s absence, the `.rpt`
> omissions are a report that ran early.

§7(b) guessed it might be illegal; it is not — no crash. What it produces is:

- an `.out` that is **24 bytes short and ends at EOF mid-structure** —
  `cmp` reports `EOF on o4_reportfirst.out`, so the closing block `end()`
  writes is simply absent and a reader hits the end of the file where a
  record should be;
- a `.rpt` missing the end-of-run diagnostics that `end()` finalises: the
  flow-instability index reads `All links are stable.` where the real run
  names `Link C1 (0)`, and the routing time-step histogram prints
  `300.000 - 300.000 sec` in every bucket instead of the real
  `300.000 → 0.500` ladder.

**The continuity block is unaffected**, which is the part that matters for
O4 and the reason this is a footnote rather than the answer. But a caller
that reports early gets a stable-looking model that was not stable and a
results file that ends early, with nothing telling it so.

**`nosave`: `start(0)` costs the results file and nothing else.** The 390-byte
`.out` is a header with no periods in it; the `.rpt` is byte-identical. A
driver that passes 0 gets no results, not different physics.

### 10.4 §7(c) and §7(d), both answered

- **`close()` is not a no-op** (`SWMMEngine.cpp:4977`): it dumps perf, stops
  the IO thread, closes the interface and RDII files and sets
  `EngineState::CLOSED`. So `reopened` tests what it claims — and the result
  is worth stating positively: **an open/close/open on the same handle leaves
  no residue**, byte for byte.
- **`elsewhere` wrote where it should.** All five `.rpt`/`.out` pairs landed
  under the given prefix; the absolute-prefix precaution worked, and a second
  full run from a different binary location reproduced all five files exactly,
  so no comparison read a stale file.

### 10.5 One change beyond the changeset

`src/tools/o4_differential/CMakeLists.txt` gained the CLI's **POST_BUILD
staging** (and its Windows DLL copy, untested here). Without it the binary
stays in `src/tools/o4_differential/` while `openswmm` is in
`bin/<config>/`, and §3's and §4's own commands read as though the two sit
together — the next person to follow them would not find it. The install,
RPATH and bundling rules are deliberately **not** mirrored: this is a
debugging instrument that is off by default, not a shipped artefact.

### 10.6 What O4 still owes

- **Re-run through the MCP server**, which is now the only variable left. A
  session with the openswmm MCP tools can do it in one step: same deck, same
  binary, compare against `o4_cli.out`.
- If that reproduces the 7.25 in, §3's time bisection is the next move and
  **§4 still forbids reading `dhm` / `season` / `last_melt_doy_` first.**
- **The standing rule holds**: no engine result quoted from an API-driven
  run until this is settled.
- **`report()` before `end()`** (§10.3) is a separate, smaller item: either
  the API should refuse it or the truncation should be documented. Recorded,
  not fixed — it is not this round's changeset.
