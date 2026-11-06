# The water-age and heat parity decks

**Added:** 2026-08-22. **Why:** `tests/parity/README.md` §4 recorded **0
water-age decks and 0 heat decks** in the bit-identity corpus after roughly
fifteen rounds of building both. The snow deck closed the same hole for
`[SNOWPACKS]` and found a real ledger defect on its first run.

Regenerate with `python3 gen_transport_parity.py`. Verify with
`python3 gen_transport_parity.py --check`, which regenerates in memory and
diffs — **the decks are generated, not hand-written**, and a hand edit that
the generator does not reproduce is a defect the `--check` run names.

---

## 1. Files

| file | what it is |
|---|---|
| `gen_transport_parity.py` | the generator; every met value and network choice has a stated reason in the source |
| `age_legacy.inp` | `WATER_AGE ON`, **np = 0**, default solver — age through the LEGACY CSTR mirror (A1b) |
| `age_ard.inp` | the same body plus `QUALITY_SOLVER EULERIAN_ARD` — age as a row on the ARD mesh (A1a) |
| `heat_parity.inp` | one pollutant + age + heat — the `np + age + heat` reported stride |
| `heat_parity.heat` | the heat component's config, resolved **relative to the .inp** |

## 2. What each deck reaches that nothing else does

**`age_legacy` / `age_ard` are a pair, and the pair is the point.** They
differ by one line. If both move, the change is in shared age machinery — the
loader set, the subarea state, the report column. If one moves, it is that
engine's binding. A single age deck cannot make that distinction, and the
program has two independent age implementations to protect.

**`np = 0` is deliberate and is the historically broken case.** E5a found all
six `QualitySolver` loaders guarding `if (np <= 0) return`, which blocked
external-inflow **volume** as well as mass — so a deck carrying only a
reserved species got zero boundary injection, and every gate deck at the time
had pollutants enabled, so the motivating configuration was not in the matrix.
Fixed then; **unobserved by any corpus since.**

**`heat_parity` is the only deck that reaches a three-class stride.** D-UT10
decided the reserved capabilities ride as parallel per-capability
accumulators rather than a widened loader tuple, and that the seam is the
LOADER SET. `np + age + heat = 3` is where that decision is load-bearing, and
until now no deck reached it — every heat gate deck is a unit test writing its
own fixture.

Its `[HEAT_FLUXES]` enables **two** families. With one enabled, D-H5e's merge
— sum every family, then one semi-implicit relaxation — is unobservable,
because there is nothing to sum. Two is the minimum that can see it.

## 3. The network

```
S1 ──run-on──► S2 ──► J1 ──► OUT
S3 ────────────────► J1
```

Run-on is where both water-age defects lived. A3 shipped run-on carrying the
*receiver's* age instead of the donor's; A4 found run-on counting one of three
contributors, which produced water younger than anything entering the model.
`S1` and `S2` differ in `%Imperv`, width and slope so donor and receiver ages
cannot coincide by accident — **a donor/receiver swap has to show.** `S3` is
the control: same gage, no run-on, so a movement can be localised to the
run-on path rather than to subarea age in general.

## 4. The met series

Seven days, one row per hour, flat within each day so that movement
attributes to a day rather than to interpolation.

| day | air °F | rain in/hr | what it is for |
|---|---|---|---|
| 1 | 45 | 0.10 | wet-up — every surface goes dry → wet |
| 2 | 70 | 0.00 | dry and warm — age grows, elements dry |
| 3 | 55 | 0.04 | light rain onto aged water — the mixing contrast |
| 4 | 80 | 0.00 | the widest air/water temperature gap |
| 5 | 40 | 0.00 | the gap reverses sign |
| 6 | 60 | 0.15 | the largest event — run-on actually reaches S2 |
| 7 | 50 | 0.00 | tail — drying down |

Age needs dry spells to grow and rain to mix old water with new, or every
element reads the same number. Heat needs the temperature to move, or the
surface balance sits at one fixed point and the deck cannot observe an
integrator change.

## 5. ⚠ Not here, and why

- **LID.** A4 and H5b put age and temperature through the LID layer stack,
  and neither is in a corpus deck. **Issue #131**: a conventional
  `[LID_CONTROLS]` block reaches the solver unconverted — a soil layer given
  in inches arrives as 18 ft with a 0.5 ft/s conductivity. A deck written
  today would bake in pre-#131 behaviour and move when #131 lands, which
  reads as a regression rather than a fix. **Owed until #131.**
- **Heat under `EULERIAN_ARD`** — H4's mesh row with per-cell surface fluxes.
  The LEGACY path is what an ordinary deck gets, so it went first. One more
  entry here would cover it; nobody has asked yet.
- **The other two dry-element policies.** `DRY_ELEMENT_TEMPERATURE` takes
  `HOLD | AIR | DEFAULT` (D-H5c) and this deck exercises `HOLD` only.
- **SI units** — register O6, and it is true of every deck in the corpus.
- **Snow + age together.** S2b put age through the snowpack; that interaction
  lives in neither this deck nor the snow deck.

## 6. Provenance

Unlike `../snow/`, these decks carry **no stored baseline**. That is the
corpus's design (`../README.md` §3): comparison is against a build of the base
commit, not against a file somebody blessed. The snow deck's baseline exists
because that deck predates the corpus runner, and `../README.md` §3 carries a
warning that it is not tracked.
