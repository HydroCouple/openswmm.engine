# The runoff continuity ledger does not know about snow

**Found:** 2026-08-21, on the first run of the snow parity deck
(`tests/parity/snow/`).
**⚠ REVISED 2026-08-21, and the revision retracts more than it keeps.** The
original §2 claimed a ~3.4-inch unexplained hole in the snow water balance.
**There is no hole.** §1 stands and is certain; §2 is withdrawn in full and
replaced by §2R. §5 is new and is the part worth reading.
**Register:** §5 of `SNOW_DIVERGENCE_REGISTER.md`.
**✅ CLOSED 2026-08-22 by F8 (`0ad28685`).** The three rows are in, in
legacy's order and under legacy's guard, and `runoff_snowremov` has a
writer for the first time. The parity deck reads **+0.407 %** where it
read −8.193 %; the 0.055 in that remains is small enough that the
report's two-decimal per-subcatchment rounding is a live explanation.
§2R's hand reconciliation predicted +1.419 % and the difference is the
check, not a discrepancy — it had `Snow Removed` (0.122 in) as an
unquantified hypothesis and read the pack as SWE only (0.323 in against
the ledger's 0.340 in with free water). See
`F8_SNOW_LEDGER_HANDOFF_2026-08-22.md` §9.1.

---

## 1. What is certain — unchanged

**The engine's runoff continuity ledger has no snow terms.**

Legacy `src/legacy/engine/report.c` prints three rows whenever the project has
snowpacks (`Nobjects[SNOWMELT] > 0`):

```c
if ( Nobjects[SNOWMELT] > 0 )                      // line 521
    fprintf(..., "\n  Initial Snow Cover .......%14.3f%14.3f", ...);
...
if ( Nobjects[SNOWMELT] > 0 )                      // line 561
{
    fprintf(..., "\n  Snow Removed .............%14.3f%14.3f", ...);
    fprintf(..., "\n  Final Snow Cover .........%14.3f%14.3f", ...);
}
```

`grep -rn "Snow Cover" src/engine/` returns **nothing**, and the engine's
`MassBalance` struct carries `runoff_snowremov` with no cover terms at all
(`SimulationContext.hpp:1005`).

**Consequence.** On any deck with a `[SNOWPACKS]` section the starting pack is
unaccounted input and the surviving pack is unaccounted output, so the printed
percentage is not a meaningful number. **This is a reporting defect, and it is
the whole of the defect.**

## 2R. RETRACTED — the "3.4-inch hole" was an artefact of my own measurement

The original §2 reported a **+39.543 %** continuity error, and claimed that
supplying the three missing rows left it near **27 %** with ~3.4 inches
unexplained on every packed subcatchment and zero on the control. Both figures
came from a run I could not reproduce, and the residual came from **inferring**
the surviving pack from an API read rather than reading it out of the `.out`.

Measured properly, on a build with recorded provenance:

| | original claim | measured |
|---|---|---|
| runoff continuity error | **+39.543 %** | **−8.193 %** |
| after supplying the three missing rows | ~27 % | **+1.419 %** |
| unexplained residual | ~3.4 in | **none** |

What is left names itself. `SUB_DEEP`'s **0.753 in** is the only subcatchment
carrying it, and `SUB_DEEP` is **the only subcatchment with `Fout = 0.20`** —
so the row that accounts for it is **`Snow Removed`**, one of the three §1 says
is missing. The original finding had ranked the plow/removal path its
**weakest** candidate.

**The refutation was inside my own table and I did not read it.** `SUB_THIN`
carries a pack of **0.0005 in** and was shown losing as much as `SUB_DEEP`'s
**4.0 in** — a pack four thousand times deeper. Two subcatchments with wildly
different snow cannot lose the same amount *of snow*. That single row said the
per-subcatchment numbers were wrong, and it said so before any of the three
candidate causes were worth ranking.

**(120)** *a table that contains its own refutation still reads as evidence.
Before ranking causes for a residual, check whether the residual varies with
the thing it is supposed to be made of — if it does not, the measurement is
what is broken, not the model.*

## 3. What the corrected picture leaves owed

One thing, and it is §1's:

**Add `Initial Snow Cover`, `Snow Removed` and `Final Snow Cover` to the
engine's runoff ledger and report writer**, matching legacy's rows and its
`Nobjects[SNOWMELT] > 0` guard. Measured effect on the parity deck: **−8.193 %
→ +1.419 %.** No instrumentation round is needed; the earlier §6 called for a
per-step pack-balance instrument on the strength of the residual that does not
exist, and that call is withdrawn with it.

## 4. Separable, minor

`analysis_get_mass_balance` returns `runoff_continuity_error: 0.3954` where the
report printed `39.543` — the API returns the fraction, the report the percent,
and nothing in either name says which. A caller comparing the API value against
a 1 % threshold reads a 39.5 % error as passing. **Still true, and now doubly
worth fixing**, because §5 is about trusting API reads over the run's own
output.

## 5. ⬜ NEW AND OPEN — the API session path and the CLI disagree on this deck

**This is the real residue of the retraction, and it is bigger than what it
replaced.**

The unreproducible run was not "an engine that no longer exists in this tree",
as the baseline's provenance note supposes. It was **this tree's engine driven
through the MCP session API** — `lifecycle_open_model` then
`lifecycle_run_simulation` — rather than through the CLI. Three CLI builds
agree with each other and disagree with it, including one at `2992f7c5`, so
**the commit range is controlled for and the execution path is the variable
left standing.**

The two runs of the same deck:

| | API session | CLI |
|---|---|---|
| infiltration | 3.674 in | **6.436 in** |
| surface runoff | 3.573 in | **6.539 in** |
| delivered to the ground | 7.25 in | **12.98 in** |
| surviving pack | ~2.57 in | near zero |

Precipitation is 12.000 in in both. **Under the API the packs barely melted.**

**Hypothesis, stated as one:** the daily `setMeltCoeffs` hook does not fire on
the API stepping path, so `dhm` never takes its seasonal value and degree-day
melt is suppressed. That is **F1's exact signature one layer up** — F1 was
`setMeltCoeffs` having no caller at all, and this would be the same function
having no caller *on one of two paths*. It is a hypothesis; nothing here
measures `dhm`.

**The check is two runs and one read.** At a single commit, run the parity deck
via the CLI and via `lifecycle_open_model` + `lifecycle_run_simulation`, and
read `dhm` and `season` at the same simulated time. If they differ, every
snow simulation driven through the MCP tools, the Python bindings or the gym
has been running without degree-day melt — and none of those paths has a deck
in any corpus, which is how it would have stayed invisible.

**Until that is settled, no engine result should be quoted from an API-driven
run** — which is the rule the retraction above exists to establish.
