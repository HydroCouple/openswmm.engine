# Cross-Section Computation Review: Legacy Parity + SOA Performance

**Date:** 2026-06-01
**Scope:** `src/engine/hydraulics/XSection.cpp`, `XSectBatch.{hpp,cpp}`,
`xsect_tables.hpp`, and the cross-section setup in
`src/engine/input/PostParseResolver.cpp`, compared shape-by-shape against the
legacy reference `src/legacy/engine/xsect.c`.

## Context

The new engine has two cross-section implementations: the legacy SWMM C code
(`xsect.c` + `xsect.dat`) and a refactored, data-oriented C++ implementation
(per-element `xsect::` + the SoA batch `XSectGroups`). The new header claimed it
was "numerically identical to legacy." This review built an **automated golden
harness** to test that claim rigorously, then fixed every divergence it
surfaced and closed the performance gaps the SOA design had left on the table.

The harness immediately disproved the "identical" claim: it found a **crash**
(infinite recursion) and a broad cluster of numerical divergences spanning most
non-trivial shapes. All are now fixed; the new code matches legacy to
machine precision.

## Methodology — the golden harness

`tests/unit/engine/test_xsect_parity.cpp` compiles the legacy `xsect.c`
(+ `findroot.c` + a tiny stub for the unused IRREGULAR/CUSTOM/STREET globals)
directly into a GoogleTest target and diffs it against the new code:

- **Getter parity** — seed *both* implementations from legacy `xsect_setParams`,
  then diff `getAofY/getWofY/getRofY/getYofA/getSofA/getRofA/getAofS/getdSdA/
  getYcrit/getAmax/isOpen` over a dense depth/area sweep (dense near 0 and near
  the crown). Seeding from legacy isolates the *getter* math from setup.
- **setParams parity** — diff the field output of `xsect::setParams` vs legacy
  for the analytic shapes it populates.
- **Batch (SoA) parity** — build `XSectGroups` from legacy-seeded params and diff
  its batch kernels (`computeAreas/HydRad/Widths`) against legacy. This covers
  the production hot-loop path.

Tolerance: abs `1e-8`, rel `1e-7` (the code is algorithmically identical, so it
actually matches to `1e-9`; the looser bound is cross-platform FP headroom).
24 self-contained shapes × the full sweep. IRREGULAR/CUSTOM/STREET are excluded
(their legacy setters need global transect/curve arrays) and are covered
end-to-end by `tests/regression`.

## Parity defects found and fixed (per-element — `XSection.cpp`)

The per-element file was a substantially incomplete port. It was rewritten as a
faithful 1:1 translation of `xsect.c`. Key defects:

| # | Defect | Severity | Fix |
|---|--------|----------|-----|
| 1 | **MOD_BASKET `getRofY` infinite recursion → SIGSEGV.** No `getRofY`/`getRofA` case existed; `getRofY` default called `getRofA`, whose default called `getRofY` → stack overflow. | **Crash** | Restored legacy `getRofA` dispatch with explicit cases; default uses the section-factor identity `R = (S/A)^{3/2}`. |
| 2 | **MOD_BASKET geometry inverted.** New helpers modelled a *circular bottom + rect top*; legacy MOD_BASKET is a *rectangular bottom + circular-arc top* (`rBot/yBot/aBot/sBot` describe the top). All MOD_BASKET getters were wrong. | High | Rewrote all `mod_basket_*` helpers to legacy. |
| 3 | **Small-area circular `getYcircular`/`getScircular`** used an empirical fit `0.7854·α^0.4`, diverging ~4× at α≈0.01. | High | Ported legacy exact form (`getThetaOfAlpha` Newton solve; α≤1e-5 series). Added `getAcircular`/`getThetaOfPsi`. |
| 4 | **PARABOLIC perimeter** used stepwise numerical integration; legacy uses the closed-form `½·rBot²·(x·t + ln(x+t))`. | High | Ported the analytical `parab_getPofY`. |
| 5 | **FILLED_CIRCULAR `getRofY`** used a hand-rolled `acos` perimeter; legacy derives R from the `R_Circ` table plus the filled-perimeter adjustment. | High | Ported legacy `filled_circ_getRofY`. |
| 6 | **`getdSdA` always used a generic central difference**; legacy uses analytic per-shape derivatives (`circ_getdSdA`, `tabular_getdSdA`, rect/trapez/triang formulas). | Med | Ported the full `getdSdA` dispatch. |
| 7 | **`getAofS`** used an ad-hoc Newton loop that mishandled the reversed `[aFull, aMax]` bracket (decreasing S near full) and the circular inverse. | Med | Ported `circ_getAofS` (with `getAcircular` + `invLookup`), the tabulated `invLookup` paths, and legacy `findroot_Newton`. |
| 8 | **Near-full "top-surface" corrections** missing in `rect_closed`/`rect_triang`/`rect_round` R and S (perimeter must grow by the crown width past `ALFMAX`). | Med | Ported the `ALFMAX` corrections + `*_getSofA` sMax interpolation. |
| 9 | **RECT_OPEN getters ignored `s_bot`** (sides-removed flag); legacy perimeter is `(2−sBot)·y + w`. | Med | Ported. |
| 10 | **`getYcrit`** always enumerated; legacy switches to **Ridder's method** when the area ratio is outside `[0.5, 2]` (e.g. trapezoids). | Med | Ported `getYcritRidder` + `findroot_Ridder`. |
| 11 | **`xsect::setParams`** RECT_OPEN dropped `s_bot`; FORCE_MAIN used `R^{2/3}` instead of the Hazen-Williams `R^{0.63}` and the wrong sMax. | Med | Fixed; analytic-shape `setParams` now matches legacy. |
| 12 | **RECT_CLOSED `setParams` r_full** used the open-channel perimeter `w·y/(2y+w)` (3 sides); a full closed box wets all 4 sides → `a/(2(y+w))`. | Med | Fixed (also corrected the stale expectation in `test_routing.cpp`). |

A deliberate choice: `XSection.cpp` now uses legacy's `PI = 3.141592654` (not
full-precision `M_PI`) for bit-faithful parity. One over-strict assertion in
`test_xsection.cpp` was relaxed from `1e-10` to `1e-8` accordingly.

## Parity defects found and fixed (batch SoA — `XSectBatch.cpp`)

The batch kernels had matched the *old* (buggy) per-element code, so once the
per-element path was corrected the batch path diverged. A new batch-vs-legacy
test surfaced these:

- **Linear-only table interpolation.** Every table kernel (`area/hydrad/width`
  circular + tabulated + per-link) used plain linear interpolation; legacy
  `lookup()` adds a **quadratic refinement for the first two segments**, so all
  table shapes diverged at small depths. Fixed with a shared, inlined
  legacy-faithful `batch_lookup()`.
- **Incomplete fallback params.** The per-element fallback reconstructed a
  partial `XSectParams` (missing `s_full`), breaking shapes whose R derives from
  S (GOTHIC/CATENARY/SEMIELLIPTICAL/SEMICIRCULAR). Fixed with a single
  `paramsAt(group, k)` helper used by all fallbacks.
- **RECT kernels wrong.** `hydrad_rect` ignored `s_bot` (RECT_OPEN) and the
  near-full correction (RECT_CLOSED); `width_rect` returned `w` at the crown for
  RECT_CLOSED (legacy: 0). Replaced with correct, still-vectorized
  `hydrad_rect_closed`, `hydrad_rect_open`, `width_rect_closed` kernels.
- **Duplicated dispatch.** The area/hydrad/width switch was copy-pasted across
  `computeAreas/computeHydRad/computeWidths/computeAreaAndHydRad`. Collapsed onto
  single `apply_area_kernel/apply_hydrad_kernel/apply_width_kernel` helpers used
  by every entry point (incl. the fused `*Triple` paths the solver uses), so the
  dispatch now has one source of truth.

## Parity defects found and fixed (setup — `PostParseResolver.cpp`)

`PostParseResolver` re-implements legacy `xsect_setParams` per shape (a parallel
implementation — see **Risks**). Two composite shapes were inconsistent with the
now-correct getters:

- **RECT_ROUND had no case** — it fell through to a generic rectangle
  (`a_full = w·y`), discarding the circular-invert geometry and producing a wrong
  `s_max`. Added a faithful case (sets `y_bot/a_bot/s_bot/r_bot`).
- **MOD_BASKET** assumed a *semicircle*, read the arc radius from the wrong field
  (`xsect_r_bot` instead of `xsect_y_bot`, where geom3 is parsed), and never set
  `y_bot/a_bot/s_bot` — so the corrected getters would read garbage. Rewritten to
  legacy (arc angle `θ = 2·asin(w/2r)`, `s_bot = θ`, etc.).

## Performance — SOA improvements (Phase 3)

- **RECT_CLOSED/RECT_OPEN stay vectorized.** The parity fix is delivered via new
  branch-light SIMD kernels (`hydrad_rect_closed` with the near-full top-surface
  correction, `hydrad_rect_open` honouring the `s_bot` sides term,
  `width_rect_closed`) rather than per-element fallback, so the most common open
  shapes keep the SoA fast path.
- **Circular fast path (dominant shape).** `A_Circ` and `R_Circ` share one
  51-entry grid, so `area_hydrad_circular` computes the normalized depth, segment
  index and interpolation weight **once** and reuses them for both lookups; the
  fused `computeAreaAndHydRad`/`computeAreaHydRadTriple` (the Picard hot-loop
  calls) use it for CIRCULAR/FORCE_MAIN. `batch_lookup` also replaced its
  per-element division (`x/delta`) with a multiply (`x·(n-1)`), helping every
  table shape.
- **Dispatch dedup.** Single `apply_*_kernel` helpers remove duplicated switch
  logic; the fused triples route through them so all paths share one kernel set.
- **OpenMP stays off** in the kernels. The existing measurement (≈38 %
  wall-time regression on Rich_BC_CSO; ~9 µs kernel work vs ~3 µs fork/join, ×
  millions of Picard iters) is sound — left disabled, reasoning retained in
  `area_circular`'s comment.

### Measured throughput (release `-O2`, arm64, fused hot-loop calls)

`computeAreaHydRadTriple` (the Picard STEP-D call), batch SoA vs the per-element
`xsect::` path it replaces, ns per link:

| group      | batch (ns/link) | per-element | batch speedup |
|------------|-----------------|-------------|---------------|
| circular   | ~9–10 (was ~13–15) | ~18–20   | **2.0×** (was 1.3×) |
| mixed 60/20/10/10 | ~7–12    | ~21         | ~1.8–2.9×     |
| rect-only  | ~5–7            | ~25–26      | **~4–5×**     |

The circular fusion + division→multiply cut the dominant-shape hot call ~⅓.
Micro-benchmark: `tests/benchmarks/bench_xsect.cpp`-style chrono harness (kept
out of tree; google-benchmark targets are gated off by default).

### Remaining performance opportunities (not done — low impact / not hot)
- `computeSectionFactors` / `computeDepthsFromArea` are unused stubs (no
  callers); promote to shape-grouped kernels only if a solver path starts
  calling them per timestep.
- `area_powerfunc` uses `std::pow` per element. The exponent is per-link
  arbitrary, so the fixed-exponent `fastmath` helpers don't apply; POWERFUNC is
  rare, so this is left as-is.
- Non-circular tabulated hydrad (GOTHIC/CATENARY/SEMIELLIPTICAL/SEMICIRCULAR have
  no R table) and the composite shapes (MOD_BASKET/RECT_TRIANG/RECT_ROUND/
  FILLED_CIRCULAR) fall to the per-element path in the batch hot loop — correct,
  but not vectorized. They are uncommon; vectorize only if profiling flags them.
- Per-link tabulated (IRREGULAR/STREET) still gathers a table pointer per
  element; pre-sorting by table pointer would help those-heavy networks.

## Verification

- `test_engine_xsect_parity` — **12/12 green** (per-element, setParams, and batch
  SoA), all 24 self-contained shapes × dense sweep, abs `1e-8` / rel `1e-7`.
- `test_engine_xsection`, `test_engine_routing`, and the new
  `test_engine_street_xsect` (STREET end-to-end under DYNWAVE) — green.
- Full unit suite (`ctest -L unit -j1`) — **68/68 green, 0 build warnings** — and
  `ctest -L regression` (legacy-vs-new `example1`) green.
- The setParams collapse made geometry setup idempotent, which **fixed** the
  formerly-failing `site_drainage_builder.WriteReopenRoundTrip` (baseline had two
  failures here; both now pass). Note: under `-j` parallelism a spurious
  `output_node_stats` failure can appear — a pre-existing test-isolation race over
  the shared `data/site_drainage_model.out`; passes serially / in isolation.

## Follow-up 1 — setParams collapsed to a single source of truth (DONE)

`xsect::setParams` was extended to a **complete** faithful port of legacy
`xsect_setParams` (all 24 self-contained shapes, every field incl.
`y_bot/a_bot/s_bot/r_bot`), and `PostParseResolver` now **delegates** to it
instead of its parallel per-shape switch. The setParams parity test was widened
to assert all shapes/fields match legacy.

Key design choice: the delegation feeds the **raw, never-overwritten
`xsect_geom1-4` + the length `ucf`** (exactly as legacy expects), not the
derived working fields. This makes setup **idempotent** and identical across the
three former construction paths (INP parse, write→reopen, programmatic builder).
Two consequences:

- It **fixed the long-standing `site_drainage_builder.WriteReopenRoundTrip`
  failure** — that bug was non-idempotent geometry setup; the whole unit suite
  is now **68/68 green**.
- It corrected production shapes the old `PostParseResolver` switch got wrong:
  **POWERFUNC and RECT_ROUND** fell through to a generic-rectangle default, and
  MOD_BASKET used a semicircle approximation with the wrong field — all now
  legacy-exact.

`swmm_link_set_xsect` (the API builder) still stores partial display geometry
for round-trip, but the authoritative full geometry is computed once by the
shared `setParams` during resolve, so all paths agree.

## Follow-up 2 — STREET_XSECT now handled (DONE)

Streets were parsed (`[STREETS]` → `StreetStore`) and a `street::buildTransect`
existed, but nothing wired STREET conduits to it. Added the full path:

- **Parse** — `LinksHandler` stores the street name for `STREET` xsections
  (mirrors IRREGULAR).
- **Resolve** — `PostParseResolver` builds a transect from each referenced
  street (`%`→fraction slopes, display→ft lengths, matching legacy
  `street_readParams`), appends it to `transect_tables`, and sets the link's
  full geometry — exactly like IRREGULAR/CUSTOM.
- **Batch** — `XSectGroups::attachTransectTables` and the area/hydrad/width
  kernel dispatch now treat `STREET_XSECT` like IRREGULAR (per-link tabulated).
- **Test** — `tests/unit/engine/test_street_xsect.cpp` runs a STREET conduit
  end-to-end under DYNWAVE and asserts valid geometry + non-zero, finite flow.

## Remaining follow-ups

- **IRREGULAR/CUSTOM/STREET** getter parity is covered end-to-end
  (`test_street_xsect`, regression) but not in the per-element unit harness,
  because their geometry comes from transect/street tables rather than
  `XSectParams`. Adding transect fixtures to the harness would close this gap.
- The `-j` parallel ctest run can show a spurious `output_node_stats` failure: it
  reads `site_drainage_model.out` from the shared `data/` dir while
  `site_drainage_builder` rewrites it. Pre-existing test-isolation issue (passes
  serially / in isolation); give the two tests private working dirs to fix.
