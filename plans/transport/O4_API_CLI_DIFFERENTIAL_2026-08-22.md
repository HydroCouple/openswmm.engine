# O4 — the API session path and the CLI disagree — protocol (2026-08-22)

**For:** a session that can build and run.
**Status:** **§3 is DONE (2026-08-22, `3bdc30a2`) and the answer is
OUTCOME A.** Five variants of the C API — a reopen before stepping,
`start(0)`, `report()` before `end()`, and a working directory away from
the deck — all produce the same hydrology, and the control variant
reproduces a real CLI run byte for byte. **The C API is not the
variable; the MCP server is.** §5's standing rule stays in force until a
session with the openswmm MCP tools re-runs the deck through the server
and compares against `o4_cli.out`. Details:
`O4_DIFFERENTIAL_DRIVER_HANDOFF_2026-08-22.md` §10.

**Why it matters more than its size suggests:** if it holds, results taken
through the MCP tools, the Python bindings or the gym are not the results the
CLI produces — and **none of those paths has a deck in any corpus**, which is
how it would have stayed invisible.

---

## 1. What is measured

The snow parity deck, same file, run two ways:

| | API session | CLI |
|---|---|---|
| infiltration | 3.674 in | **6.436 in** |
| surface runoff | 3.573 in | **6.539 in** |
| delivered to the ground | 7.25 in | **12.98 in** |
| surviving pack | ~2.57 in | near zero |
| runoff continuity | +39.543 % | −8.193 % |

Precipitation is 12.000 in in both. **Under the API the packs barely melted.**

Three CLI builds agree with each other and disagree with the API run —
including one at the API run's own commit (`2992f7c5`) — so the commit range
is controlled for and **the execution path is the variable left standing**.

## 2. What has already been ruled out

**The `setMeltCoeffs` hypothesis — mine — is eliminated, and recording that is
the point of this section.** I proposed the daily hook does not fire on the API
stepping path: F1's signature one layer up.

Reading the code: `setMeltCoeffs` is called from `SWMMEngine::step`
(`SWMMEngine.cpp:1397`), guarded only by `doy != last_melt_doy_` with
`last_melt_doy_ = -1` at construction. **There is no second stepping loop.**
`src/cli/main.cpp` and `swmm_engine_run_with_callback`
(`openswmm_engine_impl.cpp:140`) both drive the identical C API —
`open → initialize → start → step* → end → report` — which is what any session
driver uses, and `start`'s only argument sets `save_results_`.

**There is no site at which one path takes the hook and the other skips it.**
Do not re-derive this.

## 3. The protocol — a PLAIN differential, not a targeted hunt

Because the cause is unknown, the first run must not presuppose one.

1. **One commit, one binary, one build directory.** Record all three.
2. **Run A — CLI:** `openswmm snow_parity.inp A.rpt A.out`
3. **Run B — API:** the same binary's library, driven
   `swmm_engine_open → initialize → start(1) → step* → end → report`, writing
   `B.rpt` / `B.out`. **Write this as a small C or Python driver in the tree,
   not through the MCP server** — the MCP server is a third variable and this
   step is about the API, not about the server.
4. `cmp A.out B.out` and `diff A.rpt B.rpt`.

**If they are identical**, the variable is the MCP server itself — its own
stepping, its working directory (`/Users/.../Projects/default`, which is not
the deck's directory), or its process lifetime across `close_model`/reopen.
Re-run with the MCP server as the only difference.

**If they differ**, bisect the divergence in time rather than in code: report
at the finest step both can emit and find the first period where the two
`.out` files part. That names the phase before anyone reads a line of source.

## 3a. What §3 returned

| variant | hydrology | artefacts |
|---|---|---|
| `cli` (control) | 0.407 %, snow 1.500 / 0.340 | **matches a real CLI run byte for byte** |
| `reopened` | identical | identical |
| `nosave` | identical | `.out` is a 390-byte stub, `.rpt` identical |
| `reportfirst` | identical | `.out` truncated 24 bytes; `.rpt` loses the end-of-run diagnostics |
| `elsewhere` | identical | identical |

8,640 steps in every case. O4's signature is 7.25 in against 12.98 in
delivered to the ground; **nothing here moves by a thousandth.**

`report()` before `end()` is legal — §7(b) guessed it might not be — and
lossy: a results file that ends mid-structure, and a report that says
`All links are stable.` about a run whose real report names `Link C1 (0)`.

**"Silently" was wrong and the correction to it was wrong too.** The driver
discarded the return codes; there is one, from **`end()`** (code 6,
`swmm_engine_end: engine must be running or ended`), because `step()` has
already set `ENDED`, `report()` therefore succeeds and sets `REPORTED`, and
`end()` accepts neither. Falsifier ii-b — letting `end()` accept `REPORTED` —
closes the `.out` to byte-identical with `cli` and leaves the `.rpt`
omissions in place, so the two artefacts have two different causes. Settled
at `O4_DRIVER_RETURNCODE_FIX_HANDOFF_2026-08-22.md` §5. Separable from O4.

## 4. Only then instrument

If §3 localises it to hydrology, read `dhm`, `season` and `last_melt_doy_` at
the same simulated time on both paths. **Not before** — the hypothesis that
motivated those three variables is the one §2 eliminated.

## 5. Standing rule until it is settled

**No engine result may be quoted from an API-driven run.** The retracted §2 of
`SNOW_CONTINUITY_FINDING_2026-08-21.md` is what that rule is made of: a
39.543 % continuity error, a 3.4-inch phantom hole, three ranked candidate
causes and a lesson — all of it built on one API-driven run that no CLI build
reproduces.
