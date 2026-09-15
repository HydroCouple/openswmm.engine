# O4 — resolved. The MCP server was loading a 17-day-old engine library.

**Run by:** the implementing session (Claude), 2026-08-22, with the openswmm
MCP tools available for the first time.
**Base:** `b85b802d`.
**Closes:** `O4_API_CLI_DIFFERENTIAL_2026-08-22.md` §3's last remaining step.
**Artefacts:** `tests/output/o4_mcp_2026-08-22/` — `o4_mcp.out`, `o4_mcp.rpt`,
`analyze_o4_mcp.py`, `analysis.txt`.

---

## 1. O4 reproduces exactly

Same deck (`snow_parity.inp`), through `lifecycle_open_model` →
`lifecycle_run_simulation` → `lifecycle_close_model`:

| | recorded (O4 §1) | measured now | CLI |
|---|---|---|---|
| infiltration | 3.674 in | **3.674 in** | 6.436 in |
| surface runoff | 3.573 in | **3.573 in** | 6.539 in |
| delivered to the ground | 7.25 in | **7.247 in** | 12.98 in |
| runoff continuity | +39.543 % | **+39.543 %** | +0.407 % |

To the digit, months apart, on a different machine state. **The divergence is
real and deterministic** — it was never a mis-transcription.

## 2. The `.out` diff, and the bisection §3 asked for

`o4_mcp.out` and `o4_cli.out` are both 190,470 bytes; **49,660 bytes differ.**
Header, ID block and property block are **byte-identical** — same model, same
structure, same 720 reporting periods.

- **Periods 0–119 are byte-identical** (2026-01-01 01:00 → 2026-01-06 00:00).
- **First divergence: period 120, 2026-01-06 01:00**, in one variable —
  `SUB_DEEP` snow depth, 7.677166 (CLI) against 7.679003 (MCP).
- 600 of 720 periods differ thereafter.

2026-01-06 01:00 is the first hour of the deck's first **thaw**: air goes
18 °F → 41 °F at midnight on the 6th. The two runs part at the exact hour
degree-day melt should begin.

## 3. What the bisection then named — and it is not an execution path

Melt per 24 h, `SUB_DEEP`, against the deck's own forcing:

| date | air °F | rain in/hr | CLI melt | **MCP melt** | regime |
|---|---|---|---|---|---|
| 01-06 → 01-09 | 41 | 0.00 | 0.33, 0.36, 0.33, 0.17 | **0.00000** | degree-day only |
| 01-15 → 01-18 | 38 | 0.05 | 1.50, 1.31, 1.28, 1.15 | **0.80, 1.38, 1.38, 1.24** | rain-on-snow |
| 01-19 → 01-21 | 35 | 0.00 | 0.11, 0.11, 0.16 | **0.00000** | degree-day only |
| 01-22 → 01-29 | 47 | 0.00 | 0.54 … 0.18 | **0.00000** | degree-day only |

**The MCP run melts snow only when it rains on the pack above the dividing
temperature. Degree-day melt never fires — not at 41 °F, not at 47 °F, not
for eight consecutive days 15 °F above the base temperature.** In the
rain-on-snow window it melts as much as the CLI or slightly more.

That is not a stepping difference, a working directory, or a process
lifetime. **It is F1** — `SnowSolver::setMeltCoeffs` never called, so `dhm`
stays zero and only rain-on-snow above 0.02 in/hr can move the pack. F1 is
the defect this program fixed in **`274b6506`, 2026-08-20 06:39**.

**O4 is a stale binary.** The MCP server is running an engine library built
before the snow track existed.

## 4. Which library, and the one link I did not verify

Two engine dylibs on disk are candidates:

| path | built | has F1? |
|---|---|---|
| `install/Darwin/lib/libopenswmm.engine.6.0.0.dylib` | 2026-08-21 16:22 | **yes** (F1 landed 08-20 06:39) |
| `python/.venv/…/site-packages/openswmm/engine/libopenswmm.engine.6.0.0.dylib` | **2026-08-03 07:19** | **no** |

The measured behaviour is pre-F1, and **only the venv copy predates F1** — so
that is the one the server loaded. Consistent with it: the MCP `.rpt` carries
**no snow rows** (`Initial Snow Cover`, `Snow Removed`, `Final Snow Cover`
are absent), which places it before F8/F9 (`0ad28685`, 08-21 20:37) as well;
neither dylib contains those strings.

**⚠ The unverified link: I did not read the MCP server's configuration to
confirm which library it `dlopen`s.** The inference is behavioural. Whoever
owns the server can confirm it in one look, and **that check should happen
before this document is treated as settled** — lesson 126 is four hours old
and it is exactly about naming a mechanism from a read rather than a run.

## 5. Two build-hygiene defects found on the way, both live

**(a) The version string cannot distinguish builds 40 commits apart.** The
Aug 3 venv dylib and the Aug 21 install dylib **both report
`6.0.0-alpha.3`**. A user comparing report headers would see agreement.

**(b) `build/install-prefix/include/openswmm/version.h` is a generated header
dated 2026-06-01 saying `6.0.0-alpha.2`**, and it is on the include path of
the `build/` tree. That is why `o4_cli.rpt` — produced from current source on
Aug 22 — prints **alpha.2** while the source tree has said alpha.3 since
`d612283e`. I nearly concluded from that line alone that the MCP server had
the *newer* binary. It has the older one, by 17 days.

Together: **the report header's version line is not evidence about what code
ran.** Two builds of the same source print different versions, and two builds
40 commits apart print the same one.

## 6. What this retracts and what it leaves

**Retracted:** O4's framing that "the execution path is the variable left
standing" (protocol §1) and the driver round's outcome-A conclusion that "the
MCP server is the variable — its own stepping, its working directory, or its
process lifetime" (`O4_DIFFERENTIAL_DRIVER_HANDOFF` §10.2). The server's
*behaviour* was never the variable. **The bytes it loaded were.**

Outcome A itself stands and was worth having: the C API is exonerated, and
the five variants agreeing is what made "same code, different build" the only
remaining shape.

**The protocol's §2 stands too, and reads differently now.** It eliminated the
`setMeltCoeffs` hypothesis by reading the source — correctly, for the source.
The hypothesis was right about the *mechanism* and wrong only about *whose
copy*. **A source read cannot eliminate a hypothesis about a binary.**

**Still owed:**

- **Confirm the load path** (§4) and rebuild/reinstall the Python package and
  whatever the MCP server imports. Then re-run this deck; `o4_mcp.out` should
  become byte-identical to `o4_cli.out`.
- **The standing rule stays in force until that re-run passes**: no engine
  result may be quoted from an API-driven run. It is now known *why*, which
  makes it cheap to lift rather than permanent.
- **Nothing in the engine needs fixing for O4.** The three snow commits it
  appeared to indict are correct.
- **A staleness guard is worth considering** and is not scoped here: the
  server could compare its library's build identity against the tree it was
  invoked from and say so. Recorded, not proposed — it belongs to whoever
  owns the server, and §4's unverified link should close first.

## 7. The lesson

**(130) an execution path and a build are different variables, and a
differential that controls the first says nothing about the second.** O4 held
the deck, the commit and the entry point fixed and concluded the *path* must
be responsible — because the path was the only thing left in the frame. The
binary was never in the frame. Three CLI builds agreeing with each other and
disagreeing with the API run (protocol §1) was read as evidence that the
commit range was controlled for; it was equally evidence that **all three CLI
builds were current and the API's was not**, and nobody asked the API's
library how old it was.

The tell was available the whole time and free: **the API run melted snow
only when it rained.** That is a physical signature of a specific known
defect, and one plot of melt against the forcing named it in a minute. Four
rounds went into instrumenting the *path* before anyone characterised the
*symptom*. Lesson 110 says predicting a symptom is not diagnosing it; this is
the converse — **the symptom was diagnostic and went unread.**
