# Authors

OpenSWMM is built on the EPA SWMM 5.x foundation and extended by the contributors listed below.

## Project Lead

- **Caleb Buahin** @cbuahin -- Project lead, architect, and primary developer of the OpenSWMM 6.x engine rewrite (data-oriented SoA architecture, plugin system, GeoPackage I/O, C/C++ API, dynamic wave solver alignment, Python bindings).

## Contributors

- **Corinne Wiesner** -- Verification and validation infrastructure: the manufactured
  benchmark suite (batches 1 and 2) and its analytical test harness, the
  `dw-ritter-drybed-strip` dry-bed benchmark and diagnostic regression test, and the
  benchmark provenance/tolerance reconciliation pass. Also contributed the LID
  exfiltration and barrel-clogging fixes, the Anderson-acceleration skip guard for
  surcharged EXTRAN nodes and the DPS/SLOT regimes (issue #3), the storage-node
  conduit half-area fix (issue #2), and thread-safety verification with
  concurrent-engine tests. Uncertainty-quantification sidecar work (spectral ROM,
  spatial fields, 1D network ROM, cross-layer coupling) is in progress on a feature
  branch and is not yet part of a release.

- **GitHub Copilot coding agent** -- `swmm_NODE_OUTFLOW` in the `getNodeValue` API and
  both binding trees (PR #85), and Error 227 for zero channel Manning's *n* in the
  transect reader, in both the legacy and refactored engines (PR #77).

## Issue Reporters

Community members whose reports drove defects fixed in the 6.0.0-alpha.2 and
6.0.0-alpha.3 cycles. Where an issue was filed in this repository on someone else's
behalf, the original reporter is credited.

- **@wiesnerfriedman** (Jacobs Engineering) -- The dynamic-wave convergence and Anderson
  acceleration campaign, reported as four separately localized defects: node convergence
  tested on the mixed iterate rather than the Picard residual (#97), the Anderson mixing
  coefficient applied to the wrong operand (#98), Anderson-accepted depths bypassing the
  canonical node state commit (#100), and outfalls counted in the per-node nonconvergence
  statistics (#101). Also reported that `USE HOTSTART` rejected every legacy `.hsf`
  written by a model with subcatchments (#93). Each report identified the offending code
  path directly.

- **@MitchHeineman** -- Repeated monthly RDII unit-hydrograph parameters applied silently,
  now WARNING 13 (#43, via `USEPA/Stormwater-Management-Model#18`), and the rain-gage
  rainfall scale factor with co-gage support (#46, via EPA discussion #147).

- **@karosc** -- Index and unit bugs in the storage depth Newton solve, affecting
  `FUNCTIONAL`, `CONICAL` and `PYRAMIDAL` storage shapes (#94).

- **@NandanaPerera** -- Pollutant mass not conserved when runoff falls below the cutoff
  (#90).

- **@adivoky** (Gemini Engineering & Sciences, Inc.) -- Outfall `TIMESERIES`/`TIDAL`
  stage-data references resolved by position instead of by name (#92).

Two further issues in these cycles (#74, `[CONTROLS]` `SIMULATION TIME` units; #95,
per-subcatchment precipitation scale factors) were raised internally.

## AI-Assisted Development

Portions of the OpenSWMM 6.x codebase were developed with the assistance of:

- **Claude** (Anthropic) -- Code generation, architecture design, GeoPackage strategy and implementation, test development.
