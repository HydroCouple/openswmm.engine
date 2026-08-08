# OpenSWMM Engine 6.0.0-alpha.3

**Pre-release.** APIs, file formats and defaults may still change before 6.0.0.

Covers everything merged since [`v6.0.0-alpha.1`](https://github.com/HydroCouple/openswmm.engine/releases/tag/v6.0.0-alpha.1) — 257 commits across the `6.0.0-alpha.2` and `6.0.0-alpha.3` development cycles. No `v6.0.0-alpha.2` tag was ever cut; that version string lived only in the build files, so alpha.2 work is released here for the first time. See [`CHANGELOG.md`](https://github.com/HydroCouple/openswmm.engine/blob/v6.0.0-alpha.3/CHANGELOG.md) for the itemized record.

---

## Highlights

### Legacy bit-parity is now the measured standard, not an aspiration

On the Bellinge benchmark the refactored engine reproduces the vendored legacy EPA SWMM 5.3 engine **exactly**: all 111,828 routing steps bit-identical (timestep sequence, iteration counts, hex state fingerprints), and every `.out` variable at max |diff| = 0.0 in float32 across all 2,640 report periods. This is an elementwise gate, not an aggregate mass-balance comparison. Two caveats: the 15-float per-period system summary block (area-weighted means) is not yet byte-identical, and full elementwise parity is established on this one benchmark — the broader user-model and EPA QA sweeps are checked per-variable but not all the way to zero.

Landing it required fixing real divergences, nearly all ULP-level operand-order or constant substitution that long surcharged runs amplify to macroscopic flow differences: subcatchment runoff (a truncated Manning exponent literal, a non-representable rain-conversion constant, a 1-ULP alpha reassociation), `dwflow` momentum operand order, metric input conversions dividing rather than multiplying by the unit factor, per-Picard-iteration conduit evaporation and seepage loss with the dry-conduit flow-class gate, dry-link `dqdh` length division, transect elevation-offset/`Xfactor`/UCF handling for `IRREGULAR` sections, cross-section geometry for every shape, structures, outfalls and `USE HOTSTART`, and legacy-exact 1D reporting semantics.

Alongside it, the engine stopped being quietly more permissive than legacy. Input that EPA SWMM rejects is now rejected — undefined subcatchment outlets and rain gages, unresolved storage/pump/outlet curve names all raise `ERROR 209` — warnings actually reach the `.rpt` instead of dying in a callback, a failed open writes its report as legacy does, and a new `validate_project()` reproduces the legacy step-clamp and slope warnings.

### Dynamic-wave convergence correctness

Four defects in the Picard loop, all community-reported:

- Node convergence was tested on the Anderson-**mixed** iterate rather than the fixed-point residual, so a network could false-converge with an unconverged hydraulic state and frozen flows (#97).
- The Anderson mixing coefficient was applied to the wrong operand, regressing the iteration precisely when it was converging monotonically (#98).
- Anderson-accepted depths bypassed the canonical node state commit, leaving volume, overflow and `dYdT` describing the unmixed candidate (#100).
- Outfalls were counted in the per-node nonconvergence statistics, so a boundary node could top the "Most Frequent Nonconverging Nodes" table and mask the junction actually responsible (#101).

With Anderson acceleration off — the default — all four fixes are bit-exact no-ops.

### 2D surface routing: reformulated, and 2.5–7.7× faster

The 2D solver was rebuilt around an **analytic sparse Jacobian**: a closed-form tangent for the collapsed Manning diffusive-wave flux, registered as the CVODE `JacTimes`, so each Krylov iteration is a sparse mat-vec instead of a full finite-difference RHS recompute. On top of that, 1D and 2D timesteps are **decoupled** with conservative per-step exchange booking, failed windows accept partial progress instead of rewinding, the preconditioner is assembled from the exact tangents, and per-cell tolerance is **graded by cell area** so a handful of tiny cells no longer pins the BDF step for the whole domain.

Measured on the validated defaults, which now ship as the defaults:

| Benchmark | Before | After |
|---|---|---|
| 8-hour solo storm, `THREADS 4` | 213 s, 12,391 frozen windows | **80 s, zero frozen windows** |
| 48-hour solo storm | 2,048 s | **358 s** |
| Tangent-exact preconditioner probe | 606 s | **79 s** |
| `road_culvert` (analytic J·v, then live coupling) | 36 s | **15.3 s** |

The `road_culvert` row chains two separately measured steps — 36 s → 24.1 s from the analytic Jacobian, then 23.95 s → 15.34 s from analytic live coupling — rather than being one end-to-end run. The preconditioner row is a single-feature probe, not a shipped default configuration.

The frozen-window count matters more than the wall time: each frozen window silently dropped its rainfall. The old "clean" coupled regime had a 2D domain that was 99.95% frozen. Removing the `MIN_TIMESTEP` floor — any hard floor makes wetting-front corrector retries unrecoverable — is what fixed it.

Also new: GPU acceleration via portable Kokkos backends (CUDA/HIP/SYCL/OpenMP) with hypre BoomerAMG, shipped as an `openswmm-gpu-omp` companion wheel; capped-pipe junction coupling that gates on the crown rather than spilling below `z_top`; and VFR (volume/free-surface) cell closure as an opt-in shoreline treatment.

### Runtime forcing and editing: the engine is now scriptable mid-run

A complete runtime-forcing surface landed across four phases — climate and snowfall forcing, external inflow and DWF baselines, time-pattern factors, buildup/washoff coefficients, pollutant kinetics, treatment expressions, LID layer parameters, aquifer parameters, street sweeping — plus object **deletion** with full referential integrity: nine new `delete` + `analyze_impact` API pairs that cascade or nullify every cross-reference and can preview the impact set without mutating.

Control rules became live: `swmm_control_add_rule` previously returned `SWMM_OK`, incremented the rule count, and had no effect for the entire run if called after `initialize()`. Rules now compile into the running `ControlEngine`, and parse errors name the line and the cause (`"line 4: no link named 'OR_NOPE' exists in the model"`) instead of a fixed message with `line_out = -1`.

### Python bindings: complete and typed

The C-API-to-Python gap is closed — **886 C symbols, zero unjustified unbound**, with only four intentionally-unbound error accessors allowlisted. The bindings are Pythonic rather than a 1:1 C transliteration, fully type-stubbed, and mypy-clean in CI. A permissive **lenient open** mode lets an editor load as much of a broken model as parsed, with the diagnostics queryable, instead of getting nothing.

### Other notable fixes

- **`USE HOTSTART` rejected every legacy `.hsf` written by a model with subcatchments** (#93) — which is effectively all of them, since version 3/4 files always write a runoff section. Routing state now applies; hydrology still restarts cold and warns.
- **`SAVE HOTSTART` is implemented** — the writer side previously did not exist.
- **`.inp` round-trip fidelity.** `[PUMPS]` hardcoded `ON 0 0`, dropping wet-well control depths; zero thresholds run every pump unconditionally, which on Bellinge doubled flooding and drove continuity from 0.35% to −4.7%. `[OUTLETS]`, `[XSECTIONS]`, `[CURVES]` and `[WEIRS]` had comparable defects, several of which segfaulted or errored the legacy parser on re-read.
- **DWF and groundwater pollutant mass was silently discarded** — the mass was added without its carrier volume, so a node fed only by dry-weather flow or groundwater stayed at zero concentration forever and left the whole network at zero. Models with DWF or GW inflow *and* quality routing will now report nonzero values where they reported zero.
- **`IGNORE_*` process flags are honored at runtime**, matching legacy.
- **Pollutant mass is conserved when runoff falls below the cutoff** (#90).
- **Repeated monthly RDII unit-hydrograph parameters** are no longer applied silently (#43, new WARNING 13), and the exponential initial-abstraction model gains an optional degree-day snow store.
- **First-class `CYLINDRICAL`/`CONICAL`/`PARABOLOID`/`PYRAMIDAL` storage shapes**, plus index and unit fixes in the storage depth Newton solve that were silent under US units and wrong under SI (#94).
- **Outfall `TIMESERIES`/`TIDAL` stage data is resolved by name** (#92) — v5.2 reported 1.00 where v6 reported 99.00.
- **Error codes are honest** — parse errors are no longer reported as "Out of memory".
- **`swmm_NODE_OUTFLOW`** added to `getNodeValue` and both binding trees, and **Error 227** is raised for zero channel Manning's *n*.

### Architecture and infrastructure

The node and link stores were normalized into relational side-tables, cutting over completely rather than dual-writing, with a byte-exact GeoPackage schema round-trip. GeoPackage gained an input-plugin abstraction. A manufactured-benchmark suite with an analytical test harness now backs the solvers against closed-form solutions. Wheels build across manylinux, musllinux, macOS (deployment target 11.0) and Windows, with GPU companion wheels and a verified install-and-run smoke test.

---

## Breaking changes

- **`SWMM_XSectShape` is renumbered** to the engine's internal storage codes. Any caller that hardcoded the pre-6.0 integer values will now select the wrong shape — every code from 8 up moved. Use the `SWMM_XSECT_*` constants.
- **`SWMM_RefType` gained 15 additive values.** Additive, but exhaustive switches over it will warn.
- **2D defaults changed**: `MIN_TIMESTEP` now defaults to `0` (no CVODE step floor), the linear-solver setup lag defaults to 50, and partial-progress windows, the clamp-consistent tangent and the tangent-exact preconditioner all default **on**. Set `OPENSWMM_2D_PARTIAL_WINDOW`, `OPENSWMM_2D_TANGENT_CLAMP` or `OPENSWMM_2D_PRECOND_TANGENT` to `0` to restore previous behaviour.
- **`JACOBIAN` defaults to `ANALYTIC`** in `[2D_OPTIONS]`; set `FD` to restore finite differences.
- **Quality baselines shift** for models with DWF or groundwater inflow and quality routing enabled — see the carrier-volume fix above. This is a correctness fix, but it will change reported numbers.
- **Input previously accepted is now rejected.** Models with undefined subcatchment outlets or rain gages, or unresolved curve names, now fail to open with `ERROR 209`, matching legacy. Use lenient open if you need to load them anyway.
- **Snowfall volume changes for any model with a gage `SCF ≠ 1.0`.** The engine never applied the gage snow catch factor, and applied no rain/snow split at all for subcatchments without a snow pack. Both now match legacy. Models using the default `SCF = 1.0` are byte-identical.
- **Metric (SI) models shift at the ULP level** from the input-conversion divide described above. This is a parity fix, but SI results will differ in the last bits from alpha.1.

---

## Acknowledgements

**Code contributions**

- **Corinne Wiesner** — the manufactured benchmark suite and analytical test harness, the `dw-ritter-drybed-strip` dry-bed benchmark, benchmark provenance reconciliation, the LID exfiltration and barrel-clogging fixes, the Anderson-acceleration skip guard for surcharged EXTRAN nodes and the DPS/SLOT regimes (issue #3), the storage-node conduit half-area fix (issue #2), and thread-safety verification with concurrent-engine tests.
- **GitHub Copilot coding agent** — `swmm_NODE_OUTFLOW` (PR #85) and transect Error 227 (PR #77).

**Issue reports**

- **[@wiesnerfriedman](https://github.com/wiesnerfriedman)** (Jacobs Engineering) — #97, #98, #100, #101 and #93. The entire dynamic-wave convergence section above exists because of these five reports, each of which localized the offending code path directly rather than just describing a symptom.
- **[@MitchHeineman](https://github.com/MitchHeineman)** — #43 (repeated monthly RDII unit-hydrograph parameters) and #46 (rain-gage scale factor with co-gage support).
- **[@karosc](https://github.com/karosc)** — #94 (storage depth Newton solve).
- **[@NandanaPerera](https://github.com/NandanaPerera)** — #90 (pollutant mass below the runoff cutoff).
- **[@adivoky](https://github.com/adivoky)** (Gemini Engineering & Sciences) — #92 (outfall stage data resolved by position, not name).

Thank you — several of these reports were more precise than most patches.

See [`AUTHORS.md`](https://github.com/HydroCouple/openswmm.engine/blob/v6.0.0-alpha.3/AUTHORS.md) for the full list.
