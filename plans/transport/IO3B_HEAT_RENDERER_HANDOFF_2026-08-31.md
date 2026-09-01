# IO3b — the heat renderer covers H6a's sections — Handoff (2026-08-31)

**For:** the implementing/checking agent. **This is the next round.**
**Base:** `4738bca9` (IO3a; 184/184; corpus 21/21).
**Standing findings:** lessons 1–201.

**Goal:** `saveHeatConfig` renders all five heat sections, so it stops
declining and heat edits survive a save on **every** model — not only those
without radiative configuration. That is what narrows G4g's blocker to zero.

---

## 1. What IO3a left, and why the decline exists

IO3a's heat renderer covers `[HEAT_SOURCES]` and `[HEAT_FLUXES]`. The check
round added `hasUnrenderableSections()`, which makes the save **decline**
whenever `[RADIATIVE_FLUXES]`, `[SOLAR_RADIATION]` or `[CLOUD_COVER]` carry
non-default state — so the carry-alongside copy preserves them.

**That was the right call and it closed a real hole** (without it, every H3+
deck's radiative config would have vanished on its first save). But it is a
*holding* position: on exactly those models, API and GUI edits still fall back
to the step-3 loss.

## 2. The work

Render the three remaining sections. **The only non-obvious part is the
TIMESERIES spellings**, and it is smaller than it looks:

`RadiativeConfig::sw_ts_index` and `CloudConfig::ts_index` are **indices into
`ctx.tables`**; the file needs a NAME. The back-mapping already exists —
`ctx.tables[i].id` is the string `TableData::by_name` is built from
(`TableData.hpp:725`). **No new machinery**; just do not re-derive it by
searching `by_name` for a matching index, which is the slower and more
fragile spelling of the same thing.

Emit only non-default values, exactly as `[HEAT_SOURCES]` and
`[HEAT_FLUXES]` already do — a serializer that writes every default makes
every saved model look configured (lesson 196).

**Then DELETE `hasUnrenderableSections` and the decline it drives.** Do not
leave it as a safety net: see §3.

## 3. ⚠ The most important thing in this round

`hasUnrenderableSections` compares **field by field against a
default-constructed config**, because only `CloudConfig` carries a
`configured` flag. That is a hand-maintained mirror of three structs.

**Add a field to `RadiativeConfig` and forget to add it there, and the decline
silently stops firing for that field — reopening the data loss with no
symptom.** The function is correct today and is one careless struct edit from
being wrong, in the direction that loses user data.

So the round has two acceptable endings, and **"leave the guard as it is" is
not one of them**:

- **Preferred: render everything, delete the guard.** A decline that cannot be
  reached is dead code pretending to be a safety net.
- **If any section still cannot be rendered:** keep the guard and make it
  **structural** — a `static_assert(sizeof(RadiativeConfig) == N)` beside the
  comparison, so adding a field breaks the build rather than the save. The
  `source_name` array in the same file already uses this pattern against
  `HeatSource::COUNT_`; copy it.

**Whichever ending, say which and why.** A silent third option — extend the
renderer partially and leave the hand-mirror covering the rest — is the one
that looks like progress and quietly keeps the trap.

## 4. Also in scope, if cheap

**Water age and ARD save hooks.** Both still decline and still lose API edits
(IO3a §6). Water age's config is small; ARD's `[TRANSPORT_*]` sections are
larger. **Take them only if they do not crowd out §3** — the fragility matters
more than the coverage, because coverage that is one struct-edit from failing
silently is not coverage.

## 5. Protocol

1. **A new gate must FAIL at base**: a model with radiative/solar/cloud
   configuration, edited through `swmm_heat_set_source_temp`, saved, reopened
   — the edit is lost today because the save declines. **Quote the loss.**
2. **The existing round-trip and idempotence gates must still pass**, and
   `SaveIsIdempotent` must still be the CAPTURING version — it was vacuous
   once already (it compared a file with itself); **re-read it before trusting
   it.**
3. `ctest -j8` ×3 against **184**. Corpus **21/21** `.out` and `.rpt` —
   `heat_parity` and `heat_lard` both carry heat config and now get a
   *written* file rather than a copied one. **A moved deck means the renderer
   is lossy**, which is the single most important thing this round can be
   wrong about.
4. **Falsifier sweep:**

   | falsifier | expected |
   |---|---|
   | i. drop one rendered field (e.g. `albedo`) | the round-trip gate fails on that field — **each section needs at least one field actually asserted, or "renders it" is untested** |
   | ii. emit all defaults instead of only non-defaults | the invented-row leg fails, as in IO3a |
   | iii. render the TIMESERIES spelling with the INDEX instead of the name | reopen fails to resolve the series — pins that the back-mapping happened |
   | iv. add a field to `RadiativeConfig` and do not touch the renderer | **if the guard was kept: the static_assert must break the build.** If it was deleted because everything renders: the round-trip gate must fail on the new field. **Either way something must complain** — this falsifier is the whole point of §3 |
   | v. a model with no heat config at all | still declines, copy fallback, no crash |

5. **Record:** §3's ending and its justification, falsifier iv, and the corpus
   answer.

## 6. After this round

**G4g is unblocked.** The editor round-trips `[HEAT_SOURCES]` plus the
existing Climatology sub-sections — and note the GUI plan's
`[HEAT_METEOROLOGY]` is a section that does not exist (step-3 handoff §2);
build against the corrected spec, not the plan's.

**Still owed and still un-triaged, from H7b:** `[POLLUTANTS]` Kdecay applied
as **1/second here vs legacy's 1/day** — 86 400× on every decaying deck, the
same shape as the underdrain and `landuse.c`, both of which turned out to be
real. **This has now been carried across four handoffs without being looked
at.** It is a parity defect against the reference and it is bigger than
anything left in IO3.

---

# IMPLEMENTATION + CHECK RECORD — 2026-08-31

**Verdict: IMPLEMENTED and COMMITTED as engine `23c1ddfb`** (on `1794ceb5`;
3 files; tree 1921). Evidence: `tests/output/io3b_heat_renderer/`.

## §3's ending: render everything, DELETE the guard — with structural pins

The preferred ending, taken in full. The guard's field-by-field comparisons
became the renderer's emission conditions (the guard morphed into the
renderer), and `static_assert(sizeof(...))` pins on all three config
structs sit beside the renderer (72/88/40 — compiler-measured, not
hand-computed; the first hand computation was wrong twice, which is the
point of asking the compiler). **Falsifier iv exercised**: adding a double
to `RadiativeConfig` fails the build at the pin with its instructional
message.

## Found en route — the field-by-field gate earned its keep on first run

1. **The renderer's first spelling `LW_REFLECTION` is a key the parser
   REFUSES** — the deck key is `ATM_LW_REFLECTION`. Renderer↔parser
   mismatch caught by the new gate before it shipped; both sides fixed.
2. **`[TIMESERIES]` must precede `[PROCESS_COMPONENTS]`** or the heat
   config's TIMESERIES spellings resolve against a table list not yet
   carrying the series (H7b's section-order family; fixture fixed, family
   noted a second time — worth a parser-order look someday).
3. **Invented radiative defaults are API-INVISIBLE** (no per-field
   configured state), so falsifier ii could not bite through the API; the
   gate gained a FILE-level leg (a default field's key must not appear in
   the written config), and falsifier ii bites on it.

## §4: water age and ARD deferred to IO3c, deliberately

Their configs are unit-bearing — `WaterAgeConfigData` stores SECONDS
against a file in HOURS, so a serializer must survive the ÷3600/×3600
round trip; whether fmt-shortest output re-parses to a fixed point after
one save (no ULP oscillation across save cycles) needs its own gate
design. Heat never converts (°C both sides). Rushing that here would have
crowded §3. Also noted: water age has NO per-source configured flag —
zero-vs-unset is indistinguishable, so its serializer will canonicalise
explicit-zero rows away (physically identical; record when built).

## Protocol results

| step | result |
|---|---|
| gates at base (`4738bca9`) | both new gates fail: the edit lost to the decline-copy (**"Which is: 14.5"** where 31.0 asserted) |
| patched | 9/9 gates; `SaveIsIdempotent` confirmed still the CAPTURING version, fixture widened to cover the new sections |
| ctest ×3 | **184/184 ×3** |
| corpus | **21/21 byte-identical** (`.out` + `.rpt`), incl. heat_parity + heat_lard |
| revalidation | 184/184 on `1794ceb5` (the FV substep-floor landing; disjoint files) |
| falsifier i (drop albedo emission) | bites — field gate reads 0 |
| falsifier ii (emit defaults) | bites — FILE-level leg ("invented configuration") |
| falsifier iii (index instead of name) | bites — reopen refuses (rc 5) |
| falsifier iv (grow RadiativeConfig) | **build breaks at the size pin** with its message |
| falsifier v (no heat config) | covered by construction: empty render declines, IO3a falsifier i proved declining destroys nothing |

**G4g is now UNBLOCKED in full** (build against the corrected spec: no
`[HEAT_METEOROLOGY]`). ~~The Kdecay 1/s-vs-1/day triage remains the oldest
un-looked-at debt — five handoffs now.~~ **TRIAGED same day and CONFIRMED
worse than flagged** (annihilation with 100 % continuity error, unbooked):
`KDECAY_UNITS_TRIAGE_2026-08-31.md` carries the evidence and the KD1 fix
handoff.
