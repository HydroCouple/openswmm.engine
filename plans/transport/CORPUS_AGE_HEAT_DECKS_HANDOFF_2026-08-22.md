# Water-age and heat decks for the corpus — Handoff (2026-08-22)

**For:** the checking agent.
**Base:** `d633c53e`.
**Standing findings:** lessons 1–137.
**Closes:** `tests/parity/README.md` §4's "0 water-age decks, 0 heat decks".

**No engine code moves.** Three decks, one generator, and the corpus goes
15 → 18.

---

## 1. Why this and not H6

After roughly fifteen rounds of building water age and heat, **the corpus
could observe neither.** The snow deck is the precedent and the argument: it
closed the same hole for `[SNOWPACKS]` and **found a real ledger defect on its
first run**, in a module four rounds had already been over.

H6 is the roadmap's next heat step. Taking it first would add heat physics to
a corpus that cannot see heat at all — which is the configuration that
produced the snow track. Build the observation, then the layer.

## 2. The three decks

One generator, one shared network, so a difference between decks is the
**capability** and not the model.

| deck | reaches |
|---|---|
| `age_legacy.inp` | `WATER_AGE ON`, **np = 0**, default solver — the LEGACY CSTR mirror (A1b) |
| `age_ard.inp` | same body + `QUALITY_SOLVER EULERIAN_ARD` — the ARD mesh row (A1a). They differ by **one line** |
| `heat_parity.inp` | pollutant + age + heat — the `np + age + heat` stride, D-UT10's seam. Carries `heat_parity.heat` |

**The age pair is the point, not two copies of one deck.** Both move → shared
age machinery (loader set, subarea state, report column). One moves → that
engine's binding. A single deck cannot make that distinction and the program
has two independent age implementations.

**`np = 0` is deliberate and is the historically broken case.** E5a found all
six `QualitySolver` loaders guarding `if (np <= 0) return`, blocking external
inflow **volume** as well as mass — a reserved-species-only deck got zero
boundary injection, and every gate deck then had pollutants on, so the
motivating configuration was not in the matrix. Fixed; **unobserved since.**

**Network:** `S1 →run-on→ S2 → J1`, plus `S3 → J1` direct. Run-on is where
both age defects lived — A3 carried the receiver's age instead of the
donor's, A4 counted one of three contributors. `S1`/`S2` differ in `%Imperv`,
width and slope so donor and receiver ages cannot coincide by accident. `S3`
is the control.

**Met:** 7 days, hourly, flat within each day. 45/70/55/80/40/60/50 °F and
0.10/0/0.04/0/0/0.15/0 in/hr — dry spells so age grows, rain so old and new
water mix, and a temperature range so the surface balance is not sitting at
one fixed point all run. `[HEAT_FLUXES]` enables **two** families, because
with one enabled D-H5e's merge has nothing to sum and is unobservable.

## 3. Changeset

```
new: tests/parity/transport/gen_transport_parity.py
new: tests/parity/transport/{age_legacy,age_ard,heat_parity}.inp
new: tests/parity/transport/heat_parity.heat
new: tests/parity/transport/README.md
mod: tests/parity/MANIFEST         (+3 entries with reasons)
mod: tests/parity/README.md        (§2 companion-file exception; §4 recount)
mod: plans/transport/IMPLEMENTATION_ROADMAP.md
```

**The self-contained rule gained one exception and I would rather you
challenge it than inherit it.** Heat has no inline form — `[HEAT_SOURCES]`
and `[HEAT_FLUXES]` are read only from a component file. `heat_parity.heat`
is tracked beside its deck, and component configs resolve **relative to the
`.inp`** (`SWMMEngine.cpp` ~line 286: `base_dir = parent_path(inp_path)`),
not to the cwd — so the runner's per-deck working directory cannot break it.
**I read that; I did not run it.** §4 step 3 is where it gets tested.

## 4. Validation protocol

1. **Generator is byte-reproducible:** `python3 gen_transport_parity.py
   --check` → "all files match the generator". Then run it for real, re-check,
   and confirm `git diff` is empty. I ran `--check`; run it on your machine
   too, since dict ordering and locale are the classic ways this fails
   somewhere else.
2. **Corpus self-run:** `run_corpus.sh <cli> <cli> …` → **18/18 identical,
   exit 0**. This is what proves the three decks actually *run*; through stub
   binaries I only got 18/18 on the plumbing.
3. **⛔ The companion-file claim.** `heat_parity.inp` must produce a real heat
   run **from the runner's per-deck cwd**, which is not the deck's directory.
   If `heat_parity.heat` fails to resolve, the likely symptom is not a crash
   but a **parse error naming the config**, or worse a run that proceeds with
   no heat at all. **Check the `.rpt` for a `__TEMPERATURE__` column before
   believing a clean exit** — lesson 104's shape.
4. **Before/after, two build directories** (`README.md` §6). Base `d633c53e`,
   patched = this changeset. No engine source moves, so **18/18 identical**.
5. **Falsifier sweep:**

   | falsifier | expected |
   |---|---|
   | i. hand-edit a deck, then `--check` | **DIFFERS**, with a unified diff naming the file. A generator whose `--check` cannot fail is not a check |
   | ii. delete `heat_parity.heat` | `heat_parity` fails loudly. **If it runs clean and only loses its temperature column, that is a finding** — a missing component config should not be survivable on a strict open |
   | iii. run `heat_parity.inp` from its own directory by hand | same `.out` as the runner produced. **Separates "resolves relative to the .inp" from "happened to work because of the cwd"** |
   | iv. flip `age_ard.inp` back to the default solver | it should then match `age_legacy.inp` byte for byte. **If it does not, the two decks differ by more than the solver and the pair cannot localise anything** |
   | v. turn `WATER_AGE` off in `age_legacy.inp` | the `.out` loses a column. Confirms the deck's age column is real and not a name in a header |

6. **Record:** the three new decks' wall times (README §5's per-deck table
   needs three rows), whether the `.out` files carry the columns their decks
   claim, and falsifiers iii and iv in full.

## 5. What I could not check, and the honest expectations

**I ran nothing but the generator and the stub harness.** In particular:

- **I do not know that these decks run.** They are assembled from the snow
  parity deck's proven skeleton plus sections copied from
  `test_heat_watershed.cpp`'s deck writer, but no `openswmm` has parsed them.
  A section-order or column-count mistake would show as a parse error on the
  first real run, and that is the most likely way this round fails.
- **I do not know that they produce interesting numbers.** A deck can parse,
  run, and exercise nothing — lesson 104. §4 step 3's column check is the
  minimum; better is to look at whether the age column actually *varies*
  across `S1`/`S2`/`S3` and across the dry spells. **A deck where every age
  reads the same number is a deck that will never move.**
- **`age_ard.inp` may be the one that breaks.** `np = 0` under
  `EULERIAN_ARD` is precisely the configuration E5a found broken, and the ARD
  engine builds a mesh for a species set that here contains only the reserved
  age row. If anything in this changeset exposes a defect, my guess is here —
  and that would be the round's result, not a reason to drop the deck.

## 6. Known gaps, all recorded rather than fixed

- **No LID.** A4 and H5b put both reserved species through the LID layer
  stack and neither is in a corpus deck. **Issue #131**: a conventional
  `[LID_CONTROLS]` block reaches the solver unconverted, so a deck written
  today bakes in pre-#131 behaviour and moves when #131 lands, reading as a
  regression. **Owed until #131**, and it is the largest remaining hole.
- **No heat under `EULERIAN_ARD`** (H4's mesh row + per-cell fluxes).
- **`DRY_ELEMENT_TEMPERATURE`** exercises `HOLD` only; `AIR` and `DEFAULT`
  are untouched.
- **No snow + age together**, though S2b built it.
- **Still 0 SI decks and 0 STEADY decks.**
- **The MANIFEST reason column is unenforced prose** — nothing checks that a
  deck reaches the path its line claims, and these three lines are claims.

## 7. Prepared commit message

```
test(parity): water-age and heat decks -- the corpus could see neither

After roughly fifteen rounds of building water age and heat, the bit-identity
corpus had 0 water-age decks and 0 heat decks, so "15/15 byte-identical" was
structurally incapable of observing either. The snow deck closed the same hole
for [SNOWPACKS] and found a real ledger defect on its first run.

Three decks from one generator on one shared network, so a difference between
them is the capability and not the model. age_legacy and age_ard differ by a
single line -- both moving means shared age machinery, one moving means that
engine's binding, and a single deck could not tell those apart. np = 0 on both
is deliberate: a reserved-species-only deck is the configuration E5a found
broken in all six loaders, fixed then and unobserved since.

heat_parity is the only deck reaching the np+age+heat reported stride, which
is where D-UT10's parallel-accumulator decision is load-bearing. It enables
two flux families because with one there is nothing for D-H5e's merge to sum.

The network cascades S1 -> S2 -> J1 with S3 direct, because run-on is where
both water-age defects lived and S3 is the control that localises them.

No LID: issue #131 means a conventional [LID_CONTROLS] block reaches the
solver unconverted, so a deck written now would bake in pre-#131 behaviour
and move when the fix lands.
```

---

## 8. Validation results (2026-08-22) — COMMITTED `1da1d7ca`

**The three decks run, and they found TWO defects on their first run.** §1
argued from the snow-deck precedent; the precedent held twice over.

**18/18 identical** on the corpus self-run, generator `--check` clean and
byte-reproducible, all five falsifiers behaving, **0.20 s for the three decks
together**. Numbers: `tests/output/corpus_ageheat_2026-08-22/`.

### 8.1 ⛔ FINDING 1 — the runoff ledger double-counts cascaded run-on

The same hydrology, two engines, everything else equal:

| | Total Precip | Infiltration | **Surface Runoff** | **Continuity Error** |
|---|---|---|---|---|
| **openswmm 6.0** | 6.960 | 4.620 | **3.976 in** | **−23.667 %** |
| **legacy 5.x** | 6.960 | 4.620 | **2.348 in** | **−0.271 %** |

Precipitation and infiltration agree **to the digit**. Only `Surface Runoff`
differs, by 1.628 in — which is `S1`'s runoff, counted once as a system output
and again when `S2` sheds it.

**Legacy's mechanism is explicit** (`subcatch.c:761-765`):

```c
// --- include this subcatchment's contribution to overall flow balance
//     only if its outlet is a drainage system node
if ( Subcatch[j].outNode == -1 && Subcatch[j].outSubcatch != j ) vOutflow = 0.0;
massbal_updateRunoffTotals(RUNOFF_RUNOFF, vOutflow);
```

A subcatchment draining to another subcatchment contributes **zero** — its
water is an internal transfer, counted once when the receiver discharges to a
node. **`SWMMEngine.cpp:2357` adds every subcatchment's runoff
unconditionally**; there is no such guard.

**It was structurally invisible, and the reason is exactly §1's argument:**
checked every deck in the previous corpus — **not one has a subcatchment
whose outlet is another subcatchment.** `S1 → S2` is the first, and it is the
first run that could see this.

Not fixed here: the handoff says no engine code moves, and a ledger change
needs its own round and its own falsifiers. Note it will move any future deck
with a cascade — which today is only these three.

### 8.2 ⛔ FINDING 2 — the subcatchment `__TEMPERATURE__` column is never
###      written

`heat_parity.out` carries all three species. Measured ranges over the run:

| | S1 / S2 / S3 | J1 / OUT | C1 |
|---|---|---|---|
| `__WATER_AGE__` | 0 … 48.94 h | 0 … 49.47 | 0.06 … 49.45 |
| `__TEMPERATURE__` | **0 … 0** | −4.147 … 17.66 °C | −4.147 … 17.66 |
| `TSS` | 0 … 0 | 0 … 0 | 0 … 0 |

**Heat is running** — nodes and links carry a live temperature, so the
companion file resolved and both flux families are on. But **every
subcatchment reads exactly 0.0 for the whole run**, where water age on the
same decks reads 0…48.94 h.

Located: `SWMMEngine.cpp:4645` fills the *age* row of
`snapshot.subcatch_quality` from `subcatch_runoff_age`. **There is no
equivalent for the temperature row.** It stays at the `assign(…, 0.0)`
initialisation, and `DefaultOutputPlugin.cpp:140` faithfully writes the zero.
`ctx_.heat_state.subcatch_runoff_temp` exists and is asserted by the heat
gates — the value is available and simply not published.

**Third instance of the family**, after F8's `runoff_snowremov` and the
snapshot quality vectors: a column that is declared, appears in the `.out`
header, is read by consumers, and is never written. Invisible because the
heat gates read `ctx.heat_state` **in memory** and no corpus deck had heat.

**§4.3 called this exactly** — "or worse a run that proceeds with no heat at
all… check the `.rpt` for a `__TEMPERATURE__` column before believing a clean
exit". The `.rpt` has no reserved-species rows at all, so the check had to
move to the `.out`; and the column being *present* would still have passed.
**Only reading its values caught it.**

**(131)** *a column that exists is not a column that is written. The header is
authored once at open; the values are authored every step, and only one of
those two is evidence.*

### 8.3 The five falsifiers

| | expected | measured |
|---|---|---|
| i. hand-edit a deck, `--check` | DIFFERS with a diff | **`DIFFERS: age_legacy.inp`** and a unified diff naming the changed line |
| ii. delete `heat_parity.heat` | fails loudly | **exit 5**, `Process component config file not found or unreadable: …/heat_parity.heat`, in both stdout and the `.rpt`. Not survivable |
| iii. run `heat_parity.inp` from its own directory | same `.out` | **byte-identical** to the runner's. Resolution is relative to the `.inp`, measured rather than read |
| iv. `age_ard` back to the default solver | matches `age_legacy` byte for byte | **byte-identical.** The pair differs by exactly the solver line |
| v. `WATER_AGE OFF` in `age_legacy` | loses a column | **zero** pollutant columns, against `['__WATER_AGE__']` as shipped |

### 8.4 §5's honest expectations, answered

- **They run.** No parse error; the most likely failure mode did not occur.
- **`age_ard` did NOT break.** §5 guessed `np = 0` under `EULERIAN_ARD` was
  the likeliest defect. It runs, and it differs from `age_legacy` — so the
  ARD binding is live and E5a's configuration is now under observation.
- **The age column varies and the control separates**: S1/S2 reach 48.94 h,
  S3 reaches 49.19 h. Not a deck where every age reads the same number.
- **`TSS` is inert** — 0 everywhere, on a deck with no buildup or washoff
  configured. `np = 1` is real, so the `np+age+heat` **stride** is reached,
  which is what D-UT10 needs; but the pollutant carries no mass and the deck
  should not be read as exercising quality transport. Recorded, not fixed.

### 8.5 The rest

- **§4.1**: `--check` clean before and after a real regeneration; `git diff`
  empty. One cosmetic thing: the generator prints `len(text)` as "bytes",
  and each `.inp` carries a single `§` (U+00A7) in a header comment, so the
  reported size is one byte low on all three. Harmless — the decks parse and
  the check compares text — but they are not pure ASCII, which SWMM decks
  historically are.
- **§4.4, before/after in two build directories**: this changeset has no
  engine source, so both sides are the same engine by construction. Note the
  previous round's finding applies — a "before/after" run in one build
  directory compares one engine against itself, and `run_corpus.sh` now says
  so.
- **HEAD moved mid-round** (`24d51e6e`, the xsect perf commit). Re-ran the
  headline on a build of it: **`.out` byte-identical**, −23.667 % unchanged.
  So finding 1 is not an artefact of that work.
- **§4.6 timings**: `age_legacy` 0.06 s, `age_ard` 0.08 s, `heat_parity`
  0.06 s. README §5's table has the three rows.

### 8.6 What this leaves owed

- **Finding 1** — the run-on ledger guard. Its own round.
- **Finding 2** — the subcatchment temperature column. Its own round, and it
  is the smaller of the two.
- §6's gaps are unchanged: no LID (owed until #131), no heat under
  `EULERIAN_ARD`, `DRY_ELEMENT_TEMPERATURE` only `HOLD`, no snow + age
  together, 0 SI, 0 STEADY, and the MANIFEST reason column is still
  unenforced prose.
