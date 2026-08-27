# The snow parity deck

**Added:** 2026-08-21, at `2992f7c5` (after S4).
**Why:** `SNOW_DIVERGENCE_REGISTER.md` §4. Seven defects (F1–F7) were fixed in
the snow module across four rounds against a 14-deck bit-identity corpus in
which **not one deck has a `[SNOWPACKS]` section**. "14/14 unchanged" was
structurally incapable of observing any of them. This deck closes that hole.

**It found something on its first run.** See §4 and
`plans/transport/SNOW_CONTINUITY_FINDING_2026-08-21.md`.

---

## 1. Files

| file | what it is |
|---|---|
| `gen_snow_parity.py` | The generator. **The deck is generated, not hand-written** — every met value has a stated reason in the source, and regeneration is byte-reproducible (no RNG, no clock, no dict ordering) |
| `snow_parity.inp` | The deck. Do not hand-edit; edit the generator and regenerate |
| `baseline/snow_parity.rpt` | Reference report at `2992f7c5` |
| `baseline/snow_parity.out` | Reference binary output at `2992f7c5` |
| `baseline/SHA256SUMS` | Checksums of both |

Regenerate with `python3 gen_snow_parity.py > snow_parity.inp`.

## 2. What the deck exercises, and why each part is there

Four subcatchments of 5 acres each on one gage, one junction, one conduit.
**Three carry packs; the fourth carries none and is the control** — it is the
only reason §4's finding can be localised to the snow path rather than to the
runoff ledger in general.

| subcatchment | pack | the defect regime it reaches |
|---|---|---|
| `SUB_DEEP` | `SP_DEEP`, `SD0 = 4.0 in`, **`FW0 = 0` (unripe)**, `SNN0 = 0.25` + a `REMOVAL` row | **F2, F4** — the water-balance regime. A pack with capacity to spare is the *only* configuration in which "SWE −= melt" and "SWE −= excess" differ; a pack that starts at capacity drains every drop instantly and the two are arithmetically the same number. **That is what made three of S3's four gates pass with their own defect fully restored.** The plowable fraction and `REMOVAL` row also make `plowSnow` live — the path S2b's published transfer runs through |
| `SUB_ADC` | `SP_ADC`, `SD0 = 2.0 in` against **`SD100 = 8.0 in`** | **F6, F7** — the pack starts *below* its SD100, which is exactly the condition F7 hides behind: while `si` was pinned to the initial depth, `wsnow >= si` fired on step 1 and *that* branch sets `awe = 1.0` itself, so a pack starting below SD100 never takes it. Also **F3**, since partial cover is the only way to get rain-through and melt at once |
| `SUB_THIN` | `SP_THIN`, `SD0 = 0.0005 in` | **F5** — below the 0.001 in instant-melt threshold, whose water used to be written into `imelt` and then assigned over |
| `SUB_BARE` | none (`*`) | The **per-subcatchment** no-pack guard. A pack-less *project* is not the same test: `PostParseResolver.cpp:2199` forces `IGNORE_SNOWMELT` on there (legacy `project.c:221`), so a bare project exercises a different guard. **It is also the control for §4** |

The 30-day meteorology runs five named phases — accumulate, first thaw,
refreeze with new snow, rain-on-snow, full melt — each documented in the
generator with what it drives. 30 days also makes the **seasonal melt
constant** observable: `dhm` is rescaled daily by `sin(0.0172615·(day − 81))`,
the constant D1 wrongly "corrected" and S4 restored.

## 3. Protocol — using it as a bit-identity deck

> **The baseline was regenerated on 2026-08-22 at `0ad28685` (F8)** and
> carries its provenance in `baseline/SHA256SUMS`: commit, build directory,
> and the sha256 of both the `openswmm` binary and the engine dylib. The
> `.out` is unchanged from the 2026-08-21 regeneration — F8 is a ledger and
> report change with no hydrology in it — and only the `.rpt` moved.
>
> **The rule, and it is the one the first baseline broke:** a bit-identity
> baseline is only meaningful against the build it will be compared to, so
> generate it with the same binary the sweep uses, at a named commit, in the
> same build directory, and record all three.
>
> The retired baseline (`ed4d0b63…`, runoff continuity **+39.543 %**) is
> reproduced by NO CLI build — it came from an API-driven run
> (`plans/transport/O4_API_CLI_DIFFERENTIAL_2026-08-22.md`). Its sums are kept
> in `SHA256SUMS` because its numbers are still quoted elsewhere.

```
openswmm snow_parity.inp <out>/snow_parity.rpt <out>/snow_parity.out
cmp <out>/snow_parity.out baseline/snow_parity.out
```

**The `.out` is the artefact to compare, not the `.rpt`** — the report carries
a run timestamp. Cost: **8,641 routing steps, ~15–24 s wall.** That is the
reason for `KINWAVE` and a 300 s routing step; the dynamic-wave form of this
deck costs 43,000 steps and ~90 s, which is too much for a deck that runs in
every round's sweep. Routing is not what this deck observes.

**Expect this deck to MOVE on any snow-module change, and that is the point.**
A snow round that leaves it byte-identical has either changed nothing
reachable or is not exercising what it claims. Report the movement with both
values and attribute it, exactly as `test_snow.cpp` gate movements are
attributed.

## 4. What it found — CLOSED 2026-08-22 by F8

**The deck's finding stands, the first measurement of it did not, and
the finding is now fixed** (`0ad28685`).

**What was certain, and is now fixed:** the engine's runoff continuity ledger
had **no snow terms**. Legacy `report.c:521/561` prints `Initial Snow Cover`, `Snow Removed`
and `Final Snow Cover` when the project has snowpacks; this engine printed
none of them, so on any deck with a pack the starting pack was unaccounted
input and the surviving pack unaccounted output. F8 adds all three, in
legacy's order and under legacy's guard.

**Measured, on a build with recorded provenance:**

| | first reported | measured | after F8 |
|---|---|---|---|
| runoff continuity error | +39.543 % | **−8.193 %** | **+0.407 %** |
| Initial Snow Cover | — | — | 1.500 in |
| Snow Removed | — | — | **0.122 in** |
| Final Snow Cover | — | — | 0.340 in |
| unexplained residual | ~3.4 in | none | **0.055 in** |

The hand reconciliation predicted +1.419 %. The ledger reads **+0.407 %**, and
the gap between the two is the check rather than a discrepancy: the prediction
had `Snow Removed` as an unquantified hypothesis and read the surviving pack
from `newSnowDepth`, which is SWE only. The ledger supplies removal at
**0.122 in** — on `SUB_DEEP`, the only subcatchment with `Fout = 0.20`,
exactly where the reconciliation put it — and the pack **with its free water**
at 0.340 in against 0.323 in SWE. 0.1916 − 0.122 − 0.017 = 0.055 in. Two
independent routes, agreeing to the third decimal.

**The retracted table contained its own refutation.** It showed `SUB_THIN`,
pack 0.0005 in, losing as much as `SUB_DEEP`, pack 4.0 in. Two packs four
thousand times apart cannot lose the same amount of snow. **(119)** *before
ranking causes for a residual, check whether the residual varies with the
thing it is supposed to be made of — if it does not, the measurement is what
is broken.*

Full write-up, including the API-vs-CLI divergence the retraction exposed:
`plans/transport/SNOW_CONTINUITY_FINDING_2026-08-21.md`.

## 5. Owed

- **The remaining 0.407 % — 0.055 in on 13.500 in.** No longer a hole with
  ranked candidates. Small enough that the report's two-decimal
  per-subcatchment rounding is a live explanation; worth one full-precision
  measurement before anyone treats it as a defect.
- **`O4`** — until the API/CLI divergence is settled, no number in this file
  may come from an API-driven run. That rule is what the retracted table is
  made of.
- Add the deck to the corpus runner. The current `run_decks.sh` globs
  `e0_validation_2026-08-16/decks/*.inp` plus four named decks; this one needs
  a line of its own, and the count becomes 15.
- A pollutant on a snow deck (`snow_only` behaviour) is untested by anything.
  Out of scope here, recorded so it is not mistaken for covered.
