# Amendment 1 — Water Age as a First-Class Species in the UI (D-Y4)

**Date:** 2026-08-23 · **Amends:**
`plans/transport/LARD_AGE_EXPEDITE_SUBPLAN_2026-08-23.md` and
`openswmm.gui/workplans/TRANSPORT_QUALITY_GUI_PLAN_2026-08-12.md` ·
**Status of the engine track:** complete through X5/Y0; GUI track Y1, Y2a,
Y3 landed.

---

## 1. The request (user, 2026-08-23)

> Make water age accessible as a new pollutant when turned on. It should
> allow prescribing inflows from the UI. The UI should also allow plotting
> of the pollutant timeseries.

Three asks: **(a)** age visible/usable wherever pollutants are, **(b)** age
inflows prescribable from the UI, **(c)** species timeseries plotting.

## 2. What this reverses — stated plainly

The GUI plan's user-approved decision table (2026-08-12) says:

> *"Heat/age — Reserved species configured via dedicated pages, not fake
> pollutants."*

and §3.3 says `PollutantEditorDialog` and pollutant pickers **exclude**
`__WATER_AGE__` / `__TEMPERATURE__`.

**D-Y4 reverses that for water age.** Temperature keeps the old treatment
until its own round asks otherwise. Recorded as an amendment rather than a
quiet edit because CLAUDE.md §5.0 makes vetted plans authoritative — a
change of direction has to be visible, dated, and reasoned.

## 3. Three measured constraints (checked in the code, not assumed)

1. **Age is deliberately NOT a `[POLLUTANTS]` row, and that is
   load-bearing.** `n_pollutants()` (np) gates the entire legacy quality
   path, and A2b's design separates the *transport* stride (np) from the
   *reported* stride (nr = np + age) precisely so the two cannot be
   conflated — the conflation family that produced the E4/R6 and A1a
   defects. Adding age to np would change np on every age deck, move the
   `.out` pollutant count, and invalidate corpus baselines and readers.
2. **`[INFLOWS]` constituents resolve against pollutant names only** —
   `Inflow.cpp:152`: `pollut_idx = ctx.pollutant_names.find(cons)`, and
   `:303` skips rows with `p < 0`. So `J0 __WATER_AGE__ …` today resolves
   to −1 and is **silently dropped**. Prescribing age inflows needs engine
   work; it is not a UI-only change.
3. **The GUI cannot plot ANY pollutant timeseries today.** `src/plot/`
   contains zero `QUAL` / `POLLUT_BASE` references; `PlotAttribute` is a
   fixed 37-value enum with no species entries and
   `swmmoutrunlayer_codes.cpp` maps only hydraulic variables. **So ask (c)
   is not age-specific** — it is the D-G1 descriptor work, and it will
   serve TSS exactly as much as age.

## 4. D-Y4 — the decision

**Age becomes a first-class species everywhere the UI shows species, and
gains a real inflow pathway — WITHOUT becoming a `[POLLUTANTS]` row.**

Concretely:

| Layer | What changes | What does NOT change |
|---|---|---|
| Engine | `[INFLOWS]`-style rows may name a reserved species; the value routes to the age-volume accumulator (`addAgeVolume`'s seam), with TIMESERIES support — closing A1a's deferred `WATER_AGE_SOURCES … TIMESERIES` item | `np` / `n_pollutants()`, the `[POLLUTANTS]` section, the np-vs-nr stride separation, corpus parity |
| GUI | age appears in species pickers, the inflow editor's constituent list, and (with Y2b) the plot pickers | temperature's treatment (unchanged until its own round) |

**Why not a real pollutant row (the rejected alternative):** it buys the
same user-visible outcome at the cost of np churn on every age deck —
moving the `.out` pollutant count, invalidating the 19-deck corpus
baselines, and collapsing the np/nr separation three landed rounds depend
on. The UI-level treatment gets the user everything they asked for and
leaves the parity story intact. **If review wants the literal
`[POLLUTANTS]` row anyway, that is a much larger round and should be
scoped as an engine parity project, not a GUI convenience.**

**Semantics that make this coherent:** for a pollutant, `CONCEN` means
"inflow water carries concentration C" and the engine forms `q·C` as mass.
For age, "inflow water carries age A" forms `q·A` as age-volume — which is
*exactly* what `addAgeVolume(ctx, i, q, EXTERNAL_INFLOW)` already computes.
So an age inflow row is not a new physics model; it is a time-varying
source value for a seam that already exists.

## 5. New rounds

| # | Round | Repo | Deps | Verify |
|---|---|---|---|---|
| **Z1** | Reserved species as inflow constituents: parser accepts `__WATER_AGE__` in `[INFLOWS]`, VALUE and TIMESERIES; routes to the age accumulator; InpWriter round-trip; C API to author it; **the silent-drop path replaced by either a resolution or a warning** | engine | X4 ✅ | a deck with a time-varying age inflow shifts the outfall age on the measured schedule; a misspelled species WARNS instead of vanishing; corpus 19/19 |
| **Y2b-1** | `ResultDescriptor` plumbed through `IRunLayer` (+ `attributesForKind` → descriptor list), existing enums retained as fixed descriptors | gui | — | descriptor list for a run with 2 pollutants + age = hydraulic set + 3; legacy `.out` = hydraulic set only |
| **Y2b-2** | Plot pickers + comparison-plot "Add Series" consume descriptors; `swmmoutrunlayer_codes` resolves species to `POLLUT_BASE + idx` **by name** (Y2a's rule) | gui | Y2b-1 | plotting age at a node reproduces the `.out` column; `.oswp` keeps the species NAME and survives a reorder |
| **Y2b-3** | Tabular + statistics surfaces; D-G1's **warn-on-miss** when a saved series names an absent species | gui | Y2b-2 | a project saved against a 3-species run, reopened against a 1-species run, warns and degrades rather than mis-plotting |
| **Y4** | Age as a species in the **inflow editor** (constituent list) and pollutant-adjacent pickers; §3.3's exclusion lifted for age only | gui | Z1, Y2b-1 | an age inflow authored in the UI round-trips through save/reopen and shows in the plot picker |
| **Y3b** | (unchanged, still owed) reachability wiring for the Y3 editor | gui | Y3 ✅ | menu action opens the editor |

**Ordering note:** Z1 and Y2b-1 are independent and can run in parallel.
Y4 needs both. Y2b-2/3 are the user's ask (c) and are worth landing even if
Y4 slips — they light up TSS plotting at the same time.

## 6. Consequences to watch

- **A1a's TIMESERIES deferral becomes a deliverable.** Its precise
  deferral error (`"TIMESERIES ages arrive with a later phase"`) is a
  landed gate in `test_water_age.cpp` — **Z1 must FLIP that gate**, not
  delete it (the H1/X2/X4 inversion precedent).
- **The Y3 editor and `[INFLOWS]` age rows will overlap.** Two ways to set
  an external-inflow age (constant per node in the source table, or a row
  in the inflow editor). Z1 must define precedence and warn on conflict —
  otherwise a user sets one and the other silently wins. **Recommended: the
  inflow row wins where present, because it is the more specific
  statement.** Decide it in Z1's handoff, do not discover it later.
- **`speciesUnitLabel` already handles age as hours** (Y2a) — the inflow
  editor must use hours too, not mg/L, or the value the user types means
  something else than they think.
- **Temperature is untouched**, so the UI will treat two reserved species
  inconsistently until heat's round. That is deliberate and recorded, not
  an oversight.

## 7. Plan-document patches applied with this amendment

- The subplan's scope table gains a **D-Y4** row pointing here, and §3's
  round list references Z1/Y4/Y2b-*.
- The GUI plan's decision table gains a dated amendment note beside the
  "not fake pollutants" line, and §3.3 is annotated that the exclusion now
  applies to temperature only.
