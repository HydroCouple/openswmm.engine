# `subcatches.area` is converted to ft² by an acres-only factor at twelve sites

**Found:** 2026-08-22, by running the sweep lesson 118 obliges after F9 —
the sweep the F8 handoff should have listed and did not.
**Status:** located and counted by code read; **not measured**, because no deck
in any corpus is SI.
**Provisional name: F10.** It subsumes F9, which is one instance of it.

---

## 1. The defect

`SubcatchData::area` is in **project land-area units** — acres in US, hectares
in SI. The engine converts it to ft² correctly in **four** places, via
`1.0 / ucf::UCF(ucf::LANDAREA, options)`, and incorrectly in **twelve**, via a
hardcoded `43560.0` or the named constant `ucf::ACRES_TO_FT2`.

Every one of the twelve is **2.471× wrong on an SI deck** (43 560 ft²/acre
against 107 639 ft²/hectare).

| site | what it feeds | severity |
|---|---|---|
| `SWMMEngine.cpp:4829` | `gw_final_storage` | **Mass-balance ledger term.** Same class as F8 |
| `SWMMEngine.cpp:6811` | `gw_init_storage` | **Mass-balance ledger term** |
| `Groundwater.cpp:335` | GW volume | Feeds the GW balance |
| `Groundwater.cpp:391` | GW volume | Feeds the GW balance |
| `SWMMEngine.cpp:2696` | washoff `area_ft2` → EXPON unit runoff | **Pollutant loads** |
| `SWMMEngine.cpp:2744` | ponded-quality `area_ft2_pq` | **Pollutant mass** |
| `SWMMEngine.cpp:2887`, `2909`, `2926` | quality areas | **Pollutant mass** |
| `SWMMEngine.cpp:6421` | subcatchment area | to confirm |
| `SWMMEngine.cpp:2828` | `full_ft2`, compared to `lid_ft2` within 1.0 ft² | **Silent branch failure** — on SI the "LIDs cover the full subcatchment" test stops matching, and runon quality is skipped with no error |
| `SWMMEngine.cpp:4344` | snapshot system snow depth: `area − lid_ft2/43560` | **Silent element loss** — on SI the subtraction is 2.471× too large, `a` clamps to 0, and subcatchments drop out of the system average |

`Snow.cpp`'s `plowSnow` was the thirteenth and is fixed as F9 (`0ad28685`).

## 2. The mechanism, and why patching twelve call sites is the wrong fix

**`ucf::ACRES_TO_FT2` is the defect.** A named constant that reads correct at
every call site while being wrong for half the unit systems is worse than a
bare literal, because the name is what stops the reader checking. Nine of the
twelve use it; three use the literal it is named after.

`SWMMEngine.cpp:2333` already carries a comment recording that this exact
error was found and fixed once, at the rainfall-volume site — *"models scale by
107639 (ha→ft²), not 43560 (ac→ft²) … without this the precip/infil volumes
were 2.471× too small for SI"*. **The fix was applied to the site that was
noticed and the constant that caused it was left in place**, which is lesson
118 in its general form: *a unit that lives only in a name is not carried by
the compiler, so finding one instance obliges a sweep.*

**Recommended fix: delete `ucf::ACRES_TO_FT2`** and let the compiler find every
use. Replacing it with a correct helper — `landAreaToFt2(options)` — makes the
right thing the only available thing. Patching twelve call sites and keeping
the constant leaves the next one to be written.

## 3. What makes this unobservable, and what would fix that

**No deck in any corpus is SI.** The 14 reference decks are CFS; the snow
parity deck is CFS. Some 2D gate decks use CMS, which is why the 2D track has
not been bitten. So all twelve sites, and F9 before them, are invisible to
every check this program runs.

**⬜ The owed artefact is an SI deck**, and it is now justified by twelve sites
rather than by F9's one. The cheapest version is a unit-system twin: the same
model expressed in CMS/hectares, asserting the ratio rather than a value —
which is exactly the shape the F8 round used for F9's own gate, and it worked.

## 4. What this does not claim

- **No numbers.** Nothing here is measured; the severity column is inference
  from what each site feeds.
- **`6421` is listed as "to confirm"** rather than guessed at.
- **The GW pair (4829 / 6811) may partly cancel** in the *continuity error*,
  since both sides scale together — but `gw_infil` and the flux terms do not
  come through these sites, so the cancellation is unlikely to be exact and
  the reported storages are wrong regardless.
- **Whether legacy has the same defect is unchecked.** If it does, this is a
  parity question, not a divergence — and the register is where that has to be
  decided, not assumed.
