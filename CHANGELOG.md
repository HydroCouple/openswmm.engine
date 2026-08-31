# Changelog

All notable changes to the OpenSWMM Engine are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Version boundaries in this file: `6.0.0-alpha.2` covers work merged after the
`v6.0.0-alpha.1` tag up to and including 2026-07-12; `6.0.0-alpha.3` covers
everything from the `6.0.0-alpha.3` version bump (2026-07-12) onward. No
`v6.0.0-alpha.2` tag was ever cut — that version string lived only in
`CMakeLists.txt` (`OPENSWMM_PRERELEASE`), `vcpkg.json` and
`python/pyproject.toml` — so the `[6.0.0-alpha.2]` heading below is
retroactive.

> **Known gap.** Work merged between 2026-03-26 and 2026-06-09 — the
> manufactured-benchmark and analytical-test suites, the LID exfiltration fix,
> the RDII `IAModel` pinning, transect Error 227, `swmm_NODE_OUTFLOW`, rain-gage
> scale factors and the cross-section legacy-parity port — is not yet itemized
> here. See `git log v6.0.0-alpha.1..` for the interim record.

## [Unreleased]

### Added

- **Components write their own config files — API and GUI edits survive a
  save (IO3a).** `swmm_model_write` emitted the `[PROCESS_COMPONENTS]`
  `config=` path and copied the file the model was read from; nothing
  rewrote its content, so every edit made through a C API or the GUI — a
  `[HEAT_SOURCES]` temperature, a reaction expression — was silently lost
  on save while a hand-edit persisted. A new `ComponentConfigSave` hook
  (the inverse of `ComponentConfigApply`; empty return = decline → the
  carry-alongside copy still runs) lets components adopt saving one at a
  time with no intermediate state losing data. Heat and reactions adopt
  here; water age and ARD still decline (IO3c — their configs are
  unit-bearing: age stores seconds against a file in hours, so their
  serializers need a fixed-point round-trip gate heat never needed). `saveHeatConfig` writes
  only what the model actually set. IO3b (same release) extended the heat
  renderer to all five sections — `[RADIATIVE_FLUXES]` (all three SHORTWAVE
  spellings, with the TIMESERIES index mapped back to its series name),
  `[SOLAR_RADIATION]` and `[CLOUD_COVER]` — so heat edits survive a save on
  EVERY model, and the IO3a decline guard is deleted in favour of
  structural `sizeof` pins: growing any of the three config structs now
  breaks the build beside the renderer instead of silently reopening the
  loss. Found en route: the renderer's first spelling of the
  longwave-reflection key (`LW_REFLECTION`) is one the parser refuses
  (`ATM_LW_REFLECTION`) — caught by the new field-by-field round-trip gate
  before it shipped.

- **Python bindings for the five transport-process headers.** `solver.heat`,
  `solver.reactions`, `solver.water_age`, `solver.initial_quality` and
  `solver.process_components` bind `openswmm_heat.h`,
  `openswmm_reactions.h`, `openswmm_water_age.h`,
  `openswmm_initial_quality.h` and `openswmm_process_components.h` as
  keyed, iterable Python views rather than index-and-out-pointer calls:
  flux-module and `[RADIATIVE_FLUXES]`/`[SOLAR_RADIATION]`/`[CLOUD_COVER]`
  mappings plus the `[HEAT_SOURCES]` table with node overrides; species,
  coefficients, terms, per-scope expressions and the whole-file `.rxn`
  text surface; the `[WATER_AGE_SOURCES]` globals and overrides; the
  `[INITIAL_QUALITY]` rows; and the `[PROCESS_COMPONENTS]` registry.

  The bindings refuse exactly what the C API refuses, so the constraints
  that matter survive the language boundary rather than being re-derived
  on the Python side: heat values are refused rather than clamped and
  `COMPUTED` shortwave still demands an explicit latitude and longitude;
  reaction mutation stays eagerly validated and transactional, so a stored
  system is never uncompilable and `serialize -> apply_text -> serialize`
  is byte-identical; water-age hours stay **signed**, because a negative
  source age extracts age-volume by design; and `Reactions`'
  hydraulic-variable and function tables are exposed as engine-less
  `staticmethod`s so a completer enumerates the compiler's own vocabulary
  instead of carrying a copy of it.

  Nine enums land with them — `HeatFluxModule`, `HeatShortwaveMode`,
  `HeatRadiativeParam`, `HeatSolarParam`, `HeatCloudParam`,
  `HeatSourceKind`, `WaterAgeSource`, `ReactionScope` and
  `ReactionExprForm` — each mirroring its C counterpart's numbering, and
  each accepted anywhere the corresponding `int` or token string is.

- **Preissmann slot readers and 2-D name/bulk accessors.** `Link.slot_volume`
  reports the water standing above the pipe crown, and
  `LinkStatsView.peak_slot_share` / `LinkStatsView.slot_share` report the
  peak instantaneous and run-level slot fraction — the latter a ratio of
  time integrals, never an average of instantaneous ratios. All three are
  finite-volume-routing only and read `0.0` under dynamic wave.
  `Surface2D` gains `set_vertex_z_bulk` (one mesh rescan instead of one
  per vertex) and the three read-back companions the boundary-condition
  setters had been missing: `get_edge_bc_tseries_name`,
  `get_edge_bc_flow_tseries_name` and `get_edge_bc_rating_curve_name`,
  each returning `""` for a clear slot so a round trip through an editor
  no longer has to remember what it wrote.

- **C/Python/MCP parity matrix at zero gaps in all three columns.** Of
  1016 C symbols, 992 are at parity and the remaining 24 are
  documented-intentional exclusions — the first time the matrix has been
  clean across the C, Python and MCP columns simultaneously.

- **Heat transport H6a — incoming shortwave forcing.** `[RADIATIVE_FLUXES]
  SHORTWAVE` now takes three mutually exclusive spellings instead of one
  constant: a fixed W/m², `TIMESERIES <name>` for a measured record, or
  `COMPUTED` for a Spencer/NOAA solar position driving a Bird & Hulstrom
  (1981) clear-sky model. New `[SOLAR_RADIATION]` (site coordinates,
  timezone, elevation, Bird atmosphere) and `[CLOUD_COVER]` (fraction,
  constant or timeseries) sections in `model.heat`, plus the new public
  header `openswmm_heat.h` — the surface GUI task G4g was blocked on.
  The header also carries the `[HEAT_SOURCES]` inlet-temperature table
  (step 3): per-source GLOBAL read/write with a configured-vs-default
  distinction, NODE-override enumeration and CRUD, and an effective-value
  resolver that delegates to the engine's own precedence. The API refuses
  exactly what the parser refuses — including NaN/inf, a hole the
  pre-existing `[RADIATIVE_FLUXES]` fraction guard had and which is now
  closed. ⚠ API edits to a component config do not yet survive
  `swmm_model_write` (no per-component serialization until IO3) — pinned
  by a gate rather than left to be discovered.

  Cloud fraction modulates **both** directions: it attenuates shortwave
  (Kasten–Czeplak `1 − k C^n`) and raises atmospheric emissivity in the
  longwave (Bolz `1 + k_lw C²`). Clear sky is bit-identical to H3, not
  merely close — the cloud factor is a literal `1.0` that
  `atmosphericEmissivity` short-circuits, so the RHE-gated Brunt path is
  the same object code it was validated as.

  Three refusals worth knowing about, each chosen over a silent fallback:
  configuring two SHORTWAVE spellings is an error rather than a precedence
  ladder; `COMPUTED` without `LATITUDE`/`LONGITUDE` is an error rather than
  a fallback onto the `[TEMPERATURE]` SNOWMELT latitude (which defaults to
  0 and would model equatorial noon); and out-of-range values are refused
  rather than clamped, identically in the parser and the C API.

  **Solar position is Spencer/NOAA (~0.1°), not NREL SPA (±0.0003°).** The
  plan originally specified SPA; implementing it faithfully needs ~260 rows
  of periodic-term constants that would have been transcribed from memory
  alongside the test vector meant to check them. `solarPosition()` is the
  swap point for landing SPA later, against its published source. See
  `HEAT_TRANSPORT_PLAN.md` §2.5 and D-H6a-4.

### Performance

- **FV published node stages no longer stand above the conduits they connect.**
  The face-consistent stage reconstruction covered only degree-2 pass-through
  junctions -- the set the solver splices -- so on a branching network most
  nodes published the solver's own head. Measured on `Example1.inp` under
  `FLOW_ROUTING FV` at `FV_MIN_CELLS 1`, where a conduit is a single cell and
  the comparison is exact, those nodes sat up to **0.91 ft above their own
  adjacent cell**, falling ~first-order under refinement -- a datum term, not
  physics. It survives on nodes with no lateral inflow at all, which rules out
  "head needed to drive the inflow out".

  The reconstruction now covers storage-less clean junctions at any degree.
  Two guards make that safe: a face perched ABOVE the node's own water surface
  is a free overfall and does not vote (without this, a conduit entering 4 ft
  above the invert read its offset as node depth and drove one junction's
  reported average from 0.28 ft to 4.11 ft), and a pondable junction demoted to
  the bucket path above its rim keeps its volume-ledger head.

  Culvert-inlet and flap-gate faces are excluded: those node heads are genuine
  headwaters. Storage units, outfalls and structure-fed nodes are unchanged.

  After: max +0.0002 ft and -0.0001 ft on the two affected classes. **Routing is
  untouched** -- continuity identical and the Link Flow Summary byte-identical.
  Reporting only, on purpose: solver-internal heads double as ghost boundary
  states under LTS tier holds. Probe and full measurements in
  `tests/manual/fv_node_stage/`.

- **`FV_CFL_CENSUS_INTERVAL` now does what it says; it was inert for every
  value.** The explicit FV solver conflated two quantities in one member: the
  step the last accepted substep took, and the Courant bound the last census
  returned. The substep tail wrote the taken step — clamped to the time
  remaining in the routing step, so possibly a tiny fragment — into that
  member and then zeroed the census countdown to undo the damage, which forced
  a full face census on every substep no matter what the option was set to.

  The two are now separate. The bound survives a skipped census, which is the
  entire point of the option, and only a genuine invalidation resets the
  countdown: a retry (the post-step census having proved the bound
  inadmissible), a rebuild of the active face lists, a re-tier's accumulator
  settle, and both macro-cycle exits. A clamp to the routing-step boundary
  proves nothing about the Courant bound and is no longer written back.

  At the default of 1 this is bit-identical — verified, not asserted:
  `Example1.inp` under `FLOW_ROUTING FV` produces a byte-identical `.out` at
  `OMP_NUM_THREADS` 1 and 8 against the same build with the change reverted.

- **Dropped the write-only `f_scale_` face array from the explicit FV solver.**
  Written at four sites (`initialize`, `computeFaceFlux`, `limitPositivity`,
  `fireFaces`) and read nowhere in the repository.

- **Model load and initialization: up to 17× faster, and the wall-clock window
  before the first routing step cut on every model size.** A user reported 25
  minutes between clicking Run and the analysis starting on a large model; that
  window is `open()` + `initialize()` + `start()`, and none of it was
  instrumented. See `plans/MODEL_LOAD_OPTIMIZATION_RESULTS_2026-08-13.md` for
  the full measured breakdown, including the four plan predictions that
  measurement disproved.

  Measured in Release, medians of 5, one process per model:

  | Model | before | after |
  |---|---:|---:|
  | 100k-conduit FV model, `open+initialize` | 8047 ms | 466 ms (**17.3×**) |
  | 500 timeseries × 10k rows, `open` | 7213 ms | 2571 ms (**2.81×**) |
  | 10k rain gages, `open` | 335 ms | 49 ms (**6.90×**) |
  | 50k STREET conduits, `open` | 448 ms | 159 ms (**2.83×**) |
  | 100k nodes + inflows with `REPORT INPUT YES`, full | 2550 ms | 897 ms (**2.84×**) |
  | 500k nodes/links with geometry, `open` | 3553 ms | 2482 ms (**1.43×**) |

  Peak RSS also falls: −35% on the STREET model, −17% on the 500k model.

  The individual fixes: a case-insensitive hash index over the timeseries/curve
  store (it was a linear scan called once per data row while parsing
  `[TIMESERIES]`/`[CURVES]`); memoized STREET/CUSTOM transect tabulation (one
  ~1.3 KB table per street instead of one per link); a one-pass node-inflow
  prepass in the `REPORT INPUT YES` summary (was O(nodes × inflow rows));
  memoized finite-volume per-conduit geometry tabulation (~5,000 closure
  evaluations per conduit, for a result that depends only on the cross-section);
  reserved SoA capacity and token vectors during parsing; an allocation-free
  `;; UNITS:` prescan; a 1 MB buffer on the binary `.out` stream; and
  file-size-derived reserves for external FILE-backed timeseries in place of a
  flat 100,000-row allocation each.

  All of it is bit-identical: every change was gated on a parity check
  comparing the `.rpt` (timing lines masked), the `.out` byte for byte, and the
  written-back `.inp`, across six models covering STREET, IRREGULAR, FV,
  storage and quality.

### Added

- **Water age and ARD write their own config files — IO3 save is complete
  (IO3c).** The last two decliners of the `ComponentConfigSave` hook adopt
  it: every registered component now renders its own config on save.
  `swmm_water_age_save`'s writer became the shared
  `serializeWaterAgeConfig` (one spelling for `[WATER_AGE_SOURCES]`,
  upgraded from `%g` to shortest-exact — the config stores seconds against
  a file in hours, and a rounding formatter drifts the model every save;
  the new gate pins exact round-trips and a byte-stable fixed point by
  generation two). ARD renders `[TRANSPORT_OPTIONS]`,
  `[CONDUIT_DISPERSION]` and the raw boundary/source rows;
  `SCALAR_SCHEME`/`LIMITER` ride new provenance flags — they alias
  `[OPTIONS]` FV keys the writer only emits under FLOW_ROUTING FV, so on
  a DYNWAVE deck the component file is the only carrier of that state:
  the renderer re-emits the alias (at the current live value) exactly
  when the file originally spelled it, and never invents one. Found en
  route: the IO3a **render** path silently replaced a different
  pre-existing config at the save destination where the copy path
  announces it — the render path now carries the same
  announce-on-replace contract.

### Fixed

- **`[POLLUTANTS]` Kdecay was applied 86,400× too fast — and the common case
  destroyed the pollutant with a 100 % continuity error, booked nowhere
  (KD1).** The Kdecay column is 1/day; legacy divides by `SECperDAY` at parse
  (`landuse.c`), but this engine used the raw value against dt-in-seconds at
  every application site. Any deck with a plausible decay coefficient
  (0.05–1/day BOD or coliform values) silently reported ~zero for that
  pollutant: the linearized node factor `1 − k·dt` went negative, clamped to
  zero, and the annihilated mass never reached the ledger. The parser now
  stores 1/sec exactly as legacy does, and every file-unit boundary converts
  back — INP writer, GeoPackage both directions (pre-KD1 `.gpkg` files stored
  the deck value unconverted, the same unit, so both eras read correctly),
  and the C API, whose header documented 1/day all along. Three more defects
  fell out of making the new gates close: the legacy-path solver never booked
  decayed mass into `qual_routing_reacted` (nodes, all three link branches,
  and the reactions-active twin now book it); every node decayed where legacy
  decays only storage nodes or nodes actually holding volume, so pass-through
  junction flux picked up decay factors legacy never applies; and
  `mixAtNodes`' evaporation factor inferred evaporation from any volume
  decrease — true for every draining node — creating ~`c·v_out` of mass per
  step at a draining storage whenever concentrations weren't uniform (the
  `c_max` cap masked it on every k = 0 deck). It now uses the storage unit's
  actual evaporation volume, `fEvap = 1 + vEvap/v1`, per legacy
  `findStorageQual`. After all four: the k = 1/day differential probe reads
  Mass Reacted 0.158 lbs vs legacy's 0.159, and k = 200/day books 8.455 vs
  legacy's 8.523 with continuity at baseline. Known residual (both engines,
  P2.4): per-step transit mass takes the decay factor outside any
  volume-basis booking — legacy leaks 6.5–7 % on the same storage deck.
  Triage + evidence: `plans/transport/KDECAY_UNITS_TRIAGE_2026-08-31.md`.

- **The 2D results file now says what its coordinates mean.** `Mesh2_node_x/y`
  and `Mesh2_face_x/y` have always been written in the solver's SI metres and
  tagged `units = "m"`, but nothing in the file recorded the model's own CRS or
  the factor applied to get there. A consumer of a model in a foot-based
  projected CRS (EPSG:2249 and friends) reasonably read those metres as the
  CRS's own unit and placed the results ~0.3048x toward the CRS origin, while
  the `.2dm`-backed mesh — which never leaves model units — rendered correctly.
  The file gains a scalar `/crs` variable carrying `model_crs` (verbatim from
  `[OPTIONS] CRS`) and `metres_per_model_unit` (the factor
  `SurfaceRouter2D::initialize` actually applied), referenced by a
  `openswmm_crs = "crs"` attribute on `/Mesh2` and the coordinate variables.
  Deliberately not spelled as CF `spatial_ref`/`crs_wkt`: a reader honouring
  those would place metres in a foot CRS and reproduce the very offset the
  variable exists to describe. Existing files are unaffected and stay readable;
  they simply declare nothing. (Issue #155.)

- **A malformed option value handed to `swmm_options_set` returns
  `SWMM_ERR_BADPARAM` instead of killing the process.** Thirty of the
  dispatch's key branches parsed with raw `std::stod`/`std::stoi`, which
  throw on junk — and an exception crossing the C boundary is
  `std::terminate`. The guard alone was also not sufficient: two
  non-throwing families slipped through it — the lenient time parse
  fabricated `0.0` from junk (`"1e999999"` read back as 3600 s), and
  `std::stoi` accepted a numeric prefix (`"1.5"` read as 1). All parse
  sites in the dispatch now use strict wrappers that consume the entire
  token or reject, covered by an exhaustive malformed-value suite over
  every settable key. Reachable from any API client, including the MCP
  server passing through arbitrary text. (`d80bba34`.)

- **The `OPENSWMM_PERF` FV phase split double-counted, and its parts could
  exceed the whole.** `perf::GatedTimer` accumulated wall time, so a timed
  phase that called another timed phase booked the child twice — once in the
  child's accumulator and once inside its own. Three nestings exist:
  `settleAccumulators` and `restoreState` both call `refreshDepths`, and the
  boundary-flow callback sits inside the settle window. Two of the three are
  local-time-stepping only, which is why the symptom appeared only on LTS runs:
  `total` came out **larger than the routing `step` it is a breakdown of**, for
  an "unattributed" share of **−16.2 %** on Example1 and **−19.0 %** on a
  500-conduit graded chain. That is not a quantity that can be negative, and a
  breakdown whose parts exceed the whole cannot rank optimization targets —
  the only reason the timers exist.

  Timers now book **self** time: a thread-local tally collects the wall time of
  timers opening and closing inside the running one, and each timer subtracts it
  before accumulating. Unattributed time is now **+3.5 % to +6.2 %** across the
  same four runs. No call site changed, and with `OPENSWMM_PERF` unset the
  destructor still returns before touching anything.

  The correction changes what the table recommends. `settle` read 2.255 s on the
  graded chain — 18 % of the reported total — while its own work is 0.007 s;
  effectively all of it was the two `refreshDepths()` calls already counted under
  `refreshdepths`. `restore` was the same. The real target is `refreshDepths`, at
  33 % of total on that deck, and it was not legible before. Evidence and
  reproduction: `tests/manual/fv_phase_timers/RESULTS.md`.

### Changed

- **Hydraulics Reference Manual §8.6 and §8.7 rewritten to describe the solver
  that ships.** §8.6.1 documented junctions as `MIN_SURFAREA` linear reservoirs
  integrated in time — the BUCKET model, removed when junctions became
  interfaces. A plain junction has **no storage**: a clean degree-2 junction
  passes its cells' states straight through, and every other storage-less
  junction solves its head from an instantaneous flux balance, now written out
  as equation (8-31). §8.6.5's step 3 and the option table still documented
  `FV_NODE_CELL_COUPLING` and `FV_JUNCTION_MODEL`, both retired and
  accept-and-ignore since the interface treatment landed. The node time-step
  bound (8-32) is now stated to apply to bucket nodes only, which is what
  `nodeStableDt` implements.

  Two gaps are also closed. The chapter said junction momentum is not conserved
  without saying what that costs — roughly a millimetre of head per junction
  from splitting one Riemann problem into two, integrating to a 0.23 m backwater
  over a 199-junction subcritical chain, which is why the pass-through splice
  exists. And §8.7 now documents the published node stage (8-33), reconstructed
  from the incident wet cells rather than read off the solver's head; that rule
  changed in the previous release and was undocumented.

  A new subsection also answers a recurring question directly: the node is not
  given half of each connected link's volume, as dynamic wave does, because
  under finite volume the end cells already hold that water as explicit state.
  Booking it at the node too was measured twice and rejected twice, at ~0.3 % of
  routing continuity and −0.005 % per junction.

- **Relicensed from MIT to the Apache License, Version 2.0** (#123, #122). The
  `LICENSE` file now carries the full Apache 2.0 text, retaining the addendum
  that acknowledges the USEPA SWMM material residing in the public domain under
  17 USC § 105 — the Apache grant covers only the OpenSWMM authors' original
  contributions and places no restriction on that public domain material. A new
  `NOTICE` file records the required attribution and SWMM provenance. All
  first-party source headers now carry the Apache 2.0 boilerplate and an
  `SPDX-License-Identifier: Apache-2.0` tag in place of the previous MIT tags.
  `CLA.md` (v1.1), `CONTRIBUTING.md`, `CITATION.cff`, `README.md`, the Python
  bindings' `pyproject.toml` and the Sphinx license page were updated to match.
  Built-in plugin metadata (`IPluginComponentInfo::license_type`) now reports
  `"Apache-2.0"`.

### Added

- **Two-component pressure approach (TPA) — sub-atmospheric pressurized flow
  in both hydraulic solvers.** (#156) The closure of Vasconcelos, Wright & Roe
  (2006): a sealed full pipe may carry pressure below atmospheric instead of
  spuriously reverting to free-surface geometry, governed by a physical
  air-pathway regime flag with submergence-checked venting and a
  column-separation floor at −30 ft of head. In the finite-volume solver,
  `FV_PRESSURE_CLOSURE TPA` extends the existing slot line to both signs of
  ΔA (`FV_SLOT_CELERITY` doubles as the acoustic celerity) and composes with
  `FV_PRESSURIZED_IMPLICIT`; in the dynamic wave solver, `SURCHARGE_METHOD
  TPA` adds a constant-width slot `w = g·A_full/a²` above the crown (new
  `TPA_CELERITY` key, project units, default 100) with a per-conduit
  sub-atmospheric latch below it, updated once per routing step so the Picard
  iteration sees a fixed operator. Both default off and bit-inert when off.
  Measured on the paper's laboratory cases: the FV closure carries 0.0398 m of
  true vacuum below the crest invert of the negative-pressure siphon into the
  `.out` file — still deepening when the deck reaches the 40 s validity
  boundary it documents — where the slot closures floor at the invert; and
  DW TPA's rapid-fill bore arrival lands within 2 % of the
  FV timing where the Sjöberg slot is ~20 % early. Documented as Hydraulics
  Reference Manual §8.4.5 and §3.3.11. One pinned known issue: explicit FV
  TPA filling at a = 150 m/s diverges at the reflected surge (the paper's own
  high-celerity post-shock frontier; gate
  `FvTpa.KnownIssueHighCelerityFillingDiverges`).

- **Unsteady friction (`UNSTEADY_FRICTION VITKOVSKY`, `UF_K3`) in both
  hydraulic solvers.** (#156) The Pinto, Vasconcelos & Soares (2025) modified
  Vítkovský term with regime-dependent celerity, orthogonal to the
  pressurization closure: the local-acceleration half folds in
  semi-implicitly (no new prognostic state — hotstart- and rollback-safe),
  the convective half enters explicitly with a velocity dead-band and a
  half-momentum clamp. The DW gradient uses a cross-link stencil (the
  within-link difference is structurally zero on a full conduit), so DW
  damping requires discretized reaches — a single-link reach gets added
  inertia only. Default `NONE` is bit-identical to earlier releases.
  Documented as §3.3.12 and §8.5.4 of the Hydraulics Reference Manual.

- **`REPORT_SIGNED_HEADS` — true signed piezometric heads in the `.out`
  file.** (#156) The binary output's NODE_HEAD has always been rebuilt from a
  depth floored at zero, so sub-atmospheric heads were invisible. With
  `REPORT_SIGNED_HEADS YES` the HEAD field carries the true signed head for
  both solvers; DEPTH stays floored, and the default `NO` keeps legacy
  bit-parity (measured: a never-negative DW deck differs only in
  float32-rewrite ULPs, confined to HEAD).

- **Virtual-junction initial-state seeding under dynamic wave routing.**
  (#156) `[VIRTUAL_JUNCTIONS]` carries no initial-depth column, so VJs
  started dry regardless of their neighbors — a deck whose real nodes define
  an initial pool began with a hole at every splice. Each VJ head is now
  seeded by distance-weighted interpolation between the nearest non-virtual
  nodes along its spliced chain, requiring at least one wet endpoint (so a
  dry deck cannot manufacture water). DYNWAVE only, and that restriction is
  measured, not cautionary: seeding the node alone under FV contradicts the
  uniform-depth cell seed and was measured to drive a pressurized study deck
  from 0.000 % to −19 % continuity. Decks without VJs are bitwise untouched.

- **A Lagrangian transport engine (LARD) joins the quality solvers.**
  `QUALITY_SOLVER LAGRANGIAN` routes pollutants with a Lagrangian
  advection–reaction–dispersion scheme in place of the legacy complete-mix
  chain: plug-parcel advection on per-link segment stores, first-order
  `KDECAY`, quality substepping under a `QUALITY_STEP` key, and random-walk
  particle tracking for longitudinal dispersion (validated against the Elder
  profile). Water age rides the same engine as a reserved species: `WATER_AGE
  ON`, a `[WATER_AGE_SOURCES]` table for initial state and boundary ages,
  `__WATER_AGE__` rows in `[INFLOWS]`, hotstart round-tripping of the aged
  state (including dry elements), and age columns in the report and binary
  output. Negative source loads now extract mass deliberately at the node
  seam in all three quality engines — warned at parse, clamped to the mass
  actually held, booked to the ledger, and summarised in the report — instead
  of being silently dropped. The option and API surface lands with it:
  transport keys in `swmm_options_set`/`_get`, and a C age-source table API
  (`openswmm_water_age.h`). Configurations the LARD engine does not cover yet
  (heat, MSX-style reactions, `[TREATMENT]`) announce themselves with bypass
  warnings at open rather than failing silently. (`24602eb2`, `8c141a5e`,
  `647a3603`, `b9852cee`, `9f155227`, `d79c8bcf`, `d7b6c079`, `948b2840`,
  `4639be37`.)

- **FV solver statistics in the report file, and `OPENSWMM_PERF` phase timers
  for the FV step.** FV runs now emit an "FV Solver Statistics" block —
  explicit substeps, face-flux evaluations, mean/min/last substep, active-face
  occupancy and the LTS tier histogram. Setting `OPENSWMM_PERF=1` additionally
  prints a `[PERF-FV]` line whose bracketed phases sum to `total`, beside the
  whole-router `step`; the difference between them is unattributed time and is
  meant to be read as a finding rather than smoothed away.

- **`SWMM_FilePathRole` covers the remaining external-file slots.** Three new
  roles — `SWMM_FILE_MESH_2D`, `SWMM_FILE_OUTPUT_2D` and `SWMM_FILE_LID_REPORT` —
  let a host read the resolved and authored form of the 2D mesh reference, the 2D
  output file, and per-unit LID report files through the same
  `swmm_file_path_get` / `swmm_file_path_set` pair as every other slot. The 2D
  roles return `SWMM_ERR_BADPARAM` in builds without 2D support.

- **Model-load benchmark harness and parity gate.**
  `OPENSWMM_PERF=1` now emits a `[PERF-LOAD]` line attributing the whole
  open/initialize/start window across 20 phases (including a `read.scan` vs
  `read.dispatch` split of parsing).
  `tests/benchmarks/scripts/gen_load_bench.py` generates a deterministic
  scaling corpus up to 500k elements (not committed — 500 MB — but reproducible
  from the committed generator); `bench_model_load` times three cuts per model
  and reports peak RSS; `parity_probe` + `tests/benchmarks/scripts/load_parity.sh`
  compare `.rpt`/`.out`/write-back `.inp` between two builds. Two new ctest
  gates run under `ctest -L unit`: `load_parity_selfcheck` (engine output is
  deterministic) and `bench_corpus_generator_smoke`.

- **Per-triangle 2D initial conditions in the Python bindings.**
  `Surface2D.get_triangle_init_depth` / `set_triangle_init_depth` and
  `get_triangle_init_velocity` / `set_triangle_init_velocity`, wrapping
  `swmm_2d_triangle_get_init_depth`, `swmm_2d_set_triangle_init_depth`,
  `swmm_2d_triangle_get_init_velocity` and `swmm_2d_set_triangle_init_velocity`.
  These were the last four exported C functions with no Python wrapper; the
  binding surface is now complete (0 `py-gap` in
  `plans/parity/provenance_matrix.md`). Initial depth is in **mesh length
  units** — the vertex-Z convention, not the SI metres the runtime state
  accessors use.

- **Gymnasium coverage column in the parity tooling.**
  `plans/parity/tools/build_matrix_provenance.py` gained `--gym-root` and a
  third `Gym` column derived from the `openswmm.gymnasium` adapter surface
  (`_engine/solver_adapter.py`, plus any module that imports `openswmm`
  directly, so a bypass of the choke point shows up). Advisory and never
  gated — gymnasium is an RL surface, not an API mirror. 281 of 903 C symbols
  are currently reachable through it.

- **`FLOW_ROUTING FV` — an explicit conservative finite-volume solver.** A
  Godunov-type scheme on a cell mesh cut from the conduits, alongside (not
  replacing) dynamic wave analysis. Conservation form with hydrostatic
  (Audusse) reconstruction, HLL interface flux, semi-implicit Manning friction
  and explicit zero-D node continuity, substepping internally at the Courant
  limit so the routing step is a reporting cadence rather than a stability
  constraint. Documented as Chapter 8 of the Hydraulics Reference Manual.

  What it delivers on the EPA reference drainage model: **routing continuity
  error 0.000 %, at every mesh resolution**, against 0.026 % for the implicit
  solver on the same file. Mixed free-surface/pressurized flow needs no
  regime-switching logic — the Preissmann slot is folded into the cross-section
  closure with a tapered mouth, so a filling bore is captured rather than
  tracked and its speed is an output of the scheme. Virtual junctions become
  ordinary interior faces, so a conduit split by one reproduces the unsplit
  conduit cell for cell, including the reversed A→VJ←B orientation.

  Seventeen `FV_*` `[OPTIONS]` keys, all readable and writable through
  `swmm_options_get`/`set` (and therefore through the Python bindings and the
  MCP server), and all **inert rather than rejected** under the other routing
  models so switching `FLOW_ROUTING` never invalidates a file:
  `FV_CELL_LENGTH`, `FV_MIN_CELLS`, `FV_CFL`, `FV_RIEMANN`, `FV_ORDER`,
  `FV_LIMITER`, `FV_SCALAR_SCHEME`, `FV_TIME_INTEGRATION`, `FV_SLOT_CELERITY`,
  `FV_DISPERSION`, `FV_STRUCTURE_COUPLING`, `FV_COMPACTION`, `FV_BACKEND`,
  `FV_MIN_PARALLEL_CELLS`, `FV_LTS`, `FV_LTS_MAX_TIERS`,
  `FV_CFL_CENSUS_INTERVAL`.

  > **Refine further if peak flows matter.** The default is
  > `FV_MIN_CELLS 4`; at that mesh the solver attenuates this model's peaks by
  > 15.3 % on average, against 37.1 % at one cell per conduit and 7.6 % at
  > eight. The cause is geometric, not diffusive — a cell-centred scheme puts a
  > single cell's bed at the conduit's *mid-point* elevation, presenting an
  > artificial bed step of half the conduit's fall at every manhole — so
  > higher-order reconstruction does not rescue it. Dynamic wave analysis
  > remains the default routing model and the right choice for routine design
  > storms, continuous simulation and planning work.

  > **It is also slower.** On the same model the finite-volume solver runs
  > ~7× the dynamic wave solver's wall-clock at one cell per conduit, ~15× at
  > the default four, and ~34× at Δx = 20 ft. The peak-deviation figures above are a consistency
  > check against dynamic wave routing, not a measure of error — accuracy is
  > established against closed-form solutions, not against another numerical
  > method. The binding constraint is the node rather than the
  > pipe: a junction's `MIN_SURFAREA` storage floor is a few feet of effective
  > length against a conduit Δx of several hundred, so the manhole sets the
  > explicit step. Choose the solver for conservation, shock capture and
  > transcritical flow — not for speed.

- **`FV_ORDER 2` — second-order MUSCL reconstruction.** MUSCL on the free
  surface and velocity (not depth and discharge) with the bed taken from its
  exact per-cell gradient, plus a centred bed source, so the still-water
  property is preserved to machine precision at second order for all three
  limiters. Cells whose ends differ in elevation by an appreciable fraction of
  the water depth fall back to first order, so the option is safe to leave on.

- **Local time stepping for the finite-volume solver (`FV_LTS`, on by
  default).** Each control volume is assigned a power-of-two tier from its own
  Courant limit and advances at its own rate, so one 5 ft pipe or one
  surcharged manhole no longer sets the substep size for the whole network. On
  a reach with a 40x length ratio this cuts face evaluations by 2.5x at the
  same base step. Conservation across a tier interface is exact by
  construction — a face books its flux into both incident volumes'
  accumulators, so what leaves a fine cell is bit-for-bit what arrives in its
  coarse neighbour. Tiers are graded so no face spans more than one level, and
  volumes fire at the END of their windows so the flux they drain and the
  sources they integrate cover the same span. Where tiering finds nothing to
  separate the solver falls through to global stepping bit-for-bit, and it is
  disabled outright when species are being transported, because the
  flux-corrected transport limiter needs one synchronous sweep.
  `FV_LTS_MAX_TIERS` caps the spread.

- **Fixed: a model the finite-volume solver could not mesh ran with no routing
  at all.** `initFv` bails on a mesh-build error leaving no solver, and every
  subsequent step returned immediately — so the run completed, exited clean, and
  reported a network through which no water had ever moved. The diagnostics were
  collected and never read by anything. They now reach `ctx.errors`, and
  `initialize()` fails. A `DUMMY`-shape conduit is enough to trigger it, and
  those are common in real models.

- **Fixed: storage-node evaporation and exfiltration were charged to the mass
  balance but never removed from the water under `FLOW_ROUTING FV`.** The loss
  is reported as node outflow and booked as a routing loss, while the solver
  kept the volume — a continuity error scaling with the number of storage units
  that lose.

- **Fixed: conduit seepage was 43 200× too large under US units.** `[LOSSES]`
  seepage arrives in in/hr in *both* unit systems while the solver consumes
  ft/s, but the input-conversion pass short-circuits for US units on the
  grounds that the input is already internal — true for lengths, areas and
  flows, false for a rainfall-dimensioned rate. The loss then saturated at the
  availability cap: a model whose only seepage was one conduit at 0.20 in/hr
  showed −67 % routing continuity. Affects `KINWAVE`, `STEADY` and `FV`;
  dynamic wave routing computes its losses on a separate path and is
  unaffected.

- **Fixed: `FLOW_ROUTING FV` lost junction storage from the routing mass
  balance.** A plain junction reports zero contribution to routing storage by
  legacy convention, which dynamic wave routing can afford because it never has
  to hold water in a junction to stay stable. The finite-volume node *is* an
  explicit control volume, so the water standing in it is real — and excluding
  it produced a continuity error proportional to junction count: 0.00082
  acre-feet per junction, which rounds to 0.000 % on a twelve-node model and
  read 0.887 % on a five-hundred-node one. Junctions are now reported under FV
  with the same relation the solver integrates. Dynamic wave routing is
  untouched.

- **Semi-implicit node coupling (`FV_NODE_COUPLING`, default
  `SEMI_IMPLICIT`) — 2.9×.** A junction's `MIN_SURFAREA` storage floor is a few
  feet of effective length against a conduit Δx of several hundred, so under
  explicit coupling the manhole, not the pipe, set the stable substep for the
  whole model. Each coupling face's mass flux is now linearized in the node head
  through the characteristic relation |dQ/dH| = √(gAT). Conservation survives by
  construction rather than by care: the correction is applied to the face flux,
  which is the one quantity both the cell update and the node update read —
  damping the node *head* instead would imply a volume change the incident cells
  never saw. At equilibrium the correction is identically zero, so the two
  couplings agree on the steady state they reach.

- **The finite-volume depth inversion is 3.2× faster, at bit-identical
  results.** `depthOfArea` — inverting A(h) for the depth the solver reports and
  reconstructs from — was 87 % of solver time. The Illinois regula-falsi it used
  does not converge superlinearly on this closure: measured 16 closure
  evaluations per call on a circular pipe and 35 on a trapezoid. Newton with the
  top width as derivative is fast but wrong, because for tabulated shapes width
  and area are independent legacy tabulations rather than an exact derivative
  pair — round-trip error 4.7e-4 ft on a 3 ft pipe, enough to break the
  still-water property. Brent's method is superlinear on function values alone
  and lands at 5.9 evaluations with full accuracy. Every peak-flow figure in the
  benchmark is unchanged to three significant figures.

- **`FV_TIME_INTEGRATION RK2` now integrates.** The key parsed, validated and
  reported correctly but was never wired to the step — every run was forward
  Euler. It is now Heun/SSP-RK2 applied to the whole operator, friction and
  positivity limiting included in each stage, with the node update averaged
  through VOLUME rather than head and the flux ledgers averaged rather than
  summed. Mutually exclusive with local time stepping, which gives different
  volumes different steps for the two stages to average over.

- **Cell-resolved Eulerian scalar transport on the finite-volume mesh.** The
  species flux is the same mass flux the water used, upwinded on the contact
  speed, which makes solute mass conservation exact and keeps a uniform
  concentration field uniform under any flow including reversal and drying.
  First-order upwind, MUSCL and QUICKEST-ULTIMATE reconstructions, limited by
  flux-corrected transport so the discrete maximum principle holds without
  sacrificing conservation, and optional implicit longitudinal dispersion.

- **`RouteModel.FV` in the Python bindings**, and `FV` in the MCP server's
  routing-model reporting.

### Changed

- **One definition of every analytic cross-section formula.** The shape
  geometry existed twice: once per element in the portable kernels
  (`XSectKernels.hpp`, which the finite-volume solver and a future device
  backend compile) and once as SoA loops in `XSectBatch.cpp`, which is what the
  dynamic wave's `computeLinkGeometry` STEP B (widths) and STEP D (areas and
  hydraulic radii) run. Both now call the same `xsect::shape` leaves, so the
  two solvers' geometry cannot drift. Dynamic-wave output is byte-identical
  across 105 decks — the 20 EPA QA parity models plus every DYNWAVE deck in the
  unit corpus — in **both** the shipped fast-lookup configuration and the
  bit-exact one, and the batch path is asserted equal to the shared kernels
  element-wise at ULP zero over the full shape catalog × 401 depth stations.
  That sweep also turns the fused circular area/hyd-radius kernel's
  bit-identity claim, until now only a comment, into a gate.

### Fixed

- **Groundwater was silently lost on every save.** `[AQUIFERS]`, `[GROUNDWATER]`
  and `[GWF]` were parsed but had no writer, so a model that used groundwater
  came back from a round trip with none of it. All three now round-trip.
  `[GWF]` expressions are stored in `options.ext_options` under
  `"GWF:<subcatch>:<type>"` and were being re-emitted through the `[OPTIONS]`
  passthrough, which uppercased the key (breaking the lookup that reads them
  back) and kept only the first value token (truncating any real expression);
  those keys are now excluded from the passthrough and written as a real
  `[GWF]` section.

- **A `[GROUNDWATER]` receiving node declared later in the file never
  resolved.** `[GROUNDWATER]` normally precedes `[JUNCTIONS]`, and
  `handle_groundwater()` resolved the node eagerly with no deferred pass, so
  the index stayed `-1`. This was a live runtime bug as well as a write bug:
  groundwater fell back to the subcatchment's outlet node instead of the one
  the model specified. The name is now captured for `PostParseResolver`.

- **Links declared before the node sections loaded silently orphaned.** Legacy
  parsing is order-independent, but `[CONDUITS]`/`[PUMPS]`/`[ORIFICES]`/
  `[WEIRS]`/`[OUTLETS]` resolved their end nodes eagerly, kept no name, and
  were never retried — so an `.inp` whose link sections came first loaded with
  `node1`/`node2` at `-1` and **no error at all**, and then saved with `*` in
  the FromNode/ToNode columns. End-node names are now re-resolved after every
  section is parsed. An end node that still will not resolve is a fatal
  `ERROR 209`, matching legacy `link_readParams()`; such a model previously
  loaded quietly and ran wrong.

- **`[INLET_USAGE]` was parsed but never written.** Every inlet-to-link
  assignment was lost on save, leaving the surviving `[STREETS]` and
  `[INLETS]` definitions inert.

- **Five writers emitted `*` where the name was known, three of them fatally.**
  A subcatchment that drains to another subcatchment (or to itself) has no
  outlet *node*, and the writer only ever consulted the node index — so the
  Outlet column saved as `*` and the model would not reload
  (`ERROR 209: undefined object *`). The RainGage column had the same defect
  against a retained gage name, and a rain gage with neither a resolved series
  index nor a file path wrote **no row at all**, which made the subcatchments
  naming it fail to reload. Non-fatally, the Snowpack column dropped a retained
  pack name, and `[PUMPS]` ignored `pump_curve_name` and wrote `*` — silently
  downgrading a curved pump to IDEAL. Each of these now falls back to the
  retained name before giving up.

- **Four external-file slots were written to the `.inp` verbatim.** The writer
  rebases every external reference against the destination directory, but the
  external `.2dm` mesh, the 2D `OUTPUT_FILE` and per-unit LID report files never
  went through that path. `SolverOptions2D::mesh_file`, `::output_file` and
  `LidUsageStore::rpt_file` are now `FilePathPair`s registered with
  `resolve_external_file_slots`, and all three emit through `emit_path_token`
  like every other slot. `[PLUGINS]` paths remain absolute by design — they name
  installed shared libraries, not model data.

- **A `[2D_MESH_FILE]` reference dangled after Save As when the mesh had not
  loaded.** External mode normally writes the in-memory mesh to a `.2dm` beside
  the destination `.inp`, so the reference stays local and the mesh travels with
  the model. When there was no mesh in memory — a missing or unreadable `.2dm`,
  or a lenient (editor) open — nothing was written, yet the token was still
  copied through unchanged, so it re-resolved against the *new* directory and the
  saved model opened 1D-only with no diagnostic. That case now re-anchors against
  the resolved source path. The loaded case is unchanged, and a new test pins
  both halves, including that Save As never writes back over the source `.2dm`.

- **`[TEMPERATURE]` climate file was opened against the process working
  directory.** `SWMMEngine::initialize` passed `temp_file.original` — the token
  as authored — to `ClimateFileReader::open`, so a relative reference resolved
  against the CWD rather than the `.inp` directory. `open()` returns `false` on
  failure and the return value was discarded, so the run continued with no
  climate data: a clean-looking result with silently missing temperature and
  evaporation. It now opens the resolved `.absolute` path and pushes a warning
  when the file cannot be read or its format is not recognised.

- **`swmm_options_set_ext(engine, "WRITE_ABSOLUTE_PATHS", …)` did not set the
  option.** The key fell through to `ext_options`, where the writer never looks,
  so the save still emitted relative paths — while re-emitting
  `WRITE_ABSOLUTE_PATHS YES` into `[OPTIONS]`, meaning the deck reopened with the
  opt-out armed and behaved differently from the save that produced it. The key
  now writes `SimulationOptions::write_absolute_paths` directly.

- **`MIN_SURFAREA` was a project option the junction storage convention never
  read.** Legacy keeps no junction storage at all (`node_getVolume` returns
  `fullVolume*(d/fd)`, and `fullVolume` is 0 for a plain junction); this engine
  books it deliberately at `MIN_SURFAREA * fullDepth` — but took the 12.566 ft²
  **compile-time constant** rather than the option. The dynamic wave honoured
  the option in its surface-area floor, so the asymmetry hid: DW output moved
  with the setting while **FV output was byte-identical across 0.0001, 0.01 and
  12.566**, because the FV node area *is* that volume divided by full depth.
  A metric deck asking for `MIN_SURFAREA 12.566` (m²) got 12.566 ft² — the same
  10.8× unit slip the dynamic wave's own floor was fixed for.

  It matters most where nodes are an artifact of discretizing a channel rather
  than real manholes. On the SWASHES 1D analytic chains, which ask for 0.01,
  every node carried 1257× the intended storage. Fixing it improves FV's L1
  depth error against the analytic solution by **2.75× on Ritter** (0.144 →
  0.052), **2.81× on Stoker** (0.120 → 0.043) and **3.78× on Thacker planar**
  (1.467 → 0.388) — FV is now the most accurate solver in the suite on all
  three, ahead of both dynamic-wave columns and the 2D marcher. `lake-at-rest-
  emerged` moves the other way (0.0051 → 0.0119): less node storage means less
  damping at an emerged shoreline.

  `full_volume` is now set once from the effective area, so `node::getVolume`
  takes its `fullVolume > 0` branch and the mass balance, the dynamic wave and
  the FV mesh all read one number. **Default behaviour is unchanged** —
  `min_surf_area` defaults to 0, meaning "use the constant" — and 102 of 105
  regression decks are byte-identical. The three that move are exactly the
  three that set the option; all three have byte-identical `.out`, differing
  only in one node's reported continuity (0.22 % → 0.23 %).

- **Triangular conduit area disagreed with legacy in the last bit.** The batch
  path spelled it `s_bot*y*y`; legacy spells it `y*y*sBot`. IEEE multiplication
  is not associative, so those are different computations — for a plain
  3 ft × 4 ft triangular channel they disagree at 130 of 401 depth stations.
  It survived every parity run because **neither test corpus contains a single
  TRIANGULAR conduit**: no deck could exercise it, and no deck can show the
  fix. Adopting the shared formula puts DYNWAVE on legacy's spelling, and the
  new element-wise gate covers the shape by name.

### Notes

- Finite-volume routing is **not** under the legacy bit-parity contract — it is
  a different discretization, and is gated on analytic and engineering
  tolerances instead. No dynamic-wave, kinematic-wave or steady-state result
  moves.

- The dynamic wave's STEP B/D geometry and the finite-volume solver's still
  differ at the ULP level in the **shipped default** build, and sharing the
  shape formulas does not change that. `SWMM_XSECT_FAST_LOOKUP` (on by default,
  ~10 % faster routing) makes `xsect_batch` normalize by a precomputed
  reciprocal and interpolate with `* inv_delta`, while `XsectEval` always
  divides — the divergence lives in the normalize/lookup layer, below the shape
  formulas. Build with `-DOPENSWMM_FAST_XSECT_LOOKUP=OFF` for the bit-exact
  path the legacy parity contract governs.

## [6.0.0-alpha.3] — 2026-07-29

### Added

- **Decoupled 1D/2D timesteps with conservative per-step exchange booking.** `COUPLING_INTERVAL` is
  mapped to a physical time window and the old step-count gating is retired, so the 2D advance
  cadence no longer follows 1D variable-step shrinkage. Junction exchange is accumulated per 1D step
  against a provisional-drawdown budget, failed windows go onto a redelivery queue, and window
  exchange volumes are delivered back to the 1D spread over the following steps via
  `nodes.coupling_queue`. The held (default) coupling path carries the whole window exchange as a
  single mean-rate `coupling_flux` source — conservative, `y`-independent, and therefore transparent
  to the analytic Jacobian below. New env diagnostics: `OPENSWMM_2D_DEBUG_COUPLE` (per-window
  ledger) and `OPENSWMM_2D_DEBUG_SINK` (per-advance BDF linear-invariant defect).
- **Analytic sparse Jacobian-vector product.** New `SurfaceTangent` module supplies the closed-form
  tangent of the collapsed Manning diffusive-wave flux — `∂F/∂V_L`, `∂F/∂V_R` per edge via `regSqrt′`,
  `faceDepth′` and the `dη/dV` closure chain rule — plus the evaporation-sink diagonal, as a
  per-cell×3 sparse mat-vec that mirrors the RHS gather (race-free, antisymmetric). Registered as
  the CVODE `JacTimes`, so each Krylov iteration is an SpMV instead of a full finite-difference RHS
  recompute; on the `road_culvert` demo 78% of all RHS calls were FD J·v. New `[2D_OPTIONS]
  JACOBIAN` (`FD` | `ANALYTIC`, default `ANALYTIC`; env `OPENSWMM_2D_JACOBIAN` overrides), with
  automatic fall-back to FD wherever the analytic tangent does not cover a term. The diagonal
  boundary tangent `∂F_bc/∂V_i` for `NORMAL_FLOW` and `SPECIFIED_STAGE` edges mirrors
  `boundaryEdgeFlux` exactly — `SPECIFIED_STAGE` is the outfall-tailwater boundary every coupled
  model with an outfall carries, so it is what lands the analytic Jacobian on the primary targets.
  Only opt-in live-RHS orifice coupling and active-set masking still force FD. `test_2d_analytic_jv`
  proves analytic == central-difference J·v to 1e-6 (`FLAT`) / 5e-5 (`VFR`+`VFR_FACE`), with a
  dedicated `SpecifiedStage` case.
- **Graded per-cell absolute tolerance for multi-scale meshes.** `atol_i = abs_tolerance ·
  max(A_i, √(A_i·A_ref))` with `A_ref` the median cell area. On a mesh with wide cell-size variation
  this lifts the WRMS tolerance of cells far below the reference scale by `√(A_ref/A_i)`, so a
  handful of tiny cells at a coarse-cell interface no longer pin the BDF step — or the global error
  demand — for the whole domain. Conservation is untouched (BDF conserves mass at any tolerance);
  only local accuracy is redistributed. New `[2D_OPTIONS] ATOL_AREA_REF`: `AUTO` (median, default),
  `0` (off, legacy pure `A_i`), or a positive `A_ref` in m². Exactly a no-op on uniform meshes
  (`A_i == A_ref ⇒ √(A_i·A_ref) == A_i`), verified at identical BDF step counts.
- **Partial-progress windows, on by default.** Instead of rewinding a window whose CVode step failed,
  the solver accepts the state CVode actually reached (`tret = tn`, `yout = zn[0]`) as a short
  window: forcings are booked over the achieved span, un-absorbed held exchange is carried in-2D
  into the next injection, and the un-integrated span is held in a catch-up lag drained in
  window-sized chunks — whole-backlog catch-up blows the `MAX_CVODE_STEPS` budget — leashed at
  roughly two windows, because rainfall is sampled on the 1D clock but applied over the lagging 2D
  span and unbounded lag distorts the hyetograph. No `ReInit`, no BDF-history loss, no dropped rain.
  New "Partial Windows" report row. Set `OPENSWMM_2D_PARTIAL_WINDOW=0` to restore rewinding.
- **Clamp-consistent analytic tangent, on by default.** The tangent now mirrors the `V <= 0`
  reconstruction clamp (`dEta/dV = 0` for volume-debt cells; `V = 0` keeps the wet-branch `1/A`). It
  is paired with partial windows by construction — partial acceptance alone parks the corrector on
  the kink with an inconsistent Jacobian, a measured 4.8× step balloon. Set
  `OPENSWMM_2D_TANGENT_CLAMP=0` to restore the unclamped tangent.
- **Tangent-exact preconditioner, on by default.** `M = I - gamma*J` for BoomerAMG/Jacobi is
  assembled directly from the surface tangents (`SurfaceJacobian::assembleFromTangents`), carrying
  the `dF/dh_up` upwind-conveyance term the secant transmissivity omits — 10–100× dominant at
  wetting fronts — and preserving nonsymmetry. Set `OPENSWMM_2D_PRECOND_TANGENT=0` to restore the
  secant assembly.
- **Permanent "2D Solver Statistics" report block** — cumulative BDF steps, nonlinear and FD-J·v RHS
  evaluations, Newton and Krylov iterations, preconditioner setups, failures, and average/last
  internal step.
- **Single-cell analytic live coupling (opt-in).** Each live in-ODE coupling point becomes a
  single-cell (centroid) point at the lowest-bed incident cell, so `Q_k` depends only on the driving
  cell volume and its tangent is a clean diagonal. `dQ_k/dV_c` is a local central difference of
  `computeNodeCouplingQ` about the driving cell (2 Q-evaluations per point, assembled once per
  linear-solve setup) folded onto `diag[c]` and the augmented integral-`Q·dt` accumulator rows;
  `applyTangentJv` stays a pure SpMV. `analyticJvEligible` now passes on the live path when every
  point is single-cell. The RHS is unchanged and the held (default) path is byte-identical. Exchange
  totals shift held→live (continuous vs mean-rate coupling), so live stays opt-in behind
  `OPENSWMM_2D_LIVE_COUPLING`.
- **Wall-time attribution under `OPENSWMM_PERF`.** Header-only chrono accumulators
  (`core/PerfTimers.hpp`) wrap the per-window 2D advance (`fireAdvanceWindow` + `solver_->advance`)
  and the 1D router step; `SWMMEngine::end()` prints the split. Zero effect when the variable is
  unset. Used to establish that the 2D CVODE solve is 82–92% of runtime.
- **Wet-masked vertex render field.** `cellFreeSurfaceElevation()` performs a planar-bed
  stage-storage inversion `eta(V)` per cell, so a partially wet step-spanning cell pools water over
  its wetted fraction instead of the flat closure's `z_c + h` overstating `eta`; it reduces exactly
  to `z_c + h` when fully wet. `reconstructVertexRenderDepths()` then produces wet-only,
  depth-weighted **signed** vertex depths (`eta_v - z_v`) with a no-new-maxima clamp — negative over
  the dry side of a partially wet cell (the sub-cell shoreline), 0 with no wet incident cell.
  Computed per advance window and at initialize. Exported as the `/Mesh2_node_depth` HDF5 dataset,
  `SimulationSnapshot::surface_vert_depth`, `swmm_2d_vertex_get_render_depths_bulk` and
  `Surface2D.get_vertex_render_depths`; `/Mesh2_node_head` and the heads APIs remain for
  back-compatibility, but they report bed elevation over dry cells and are not suitable for
  water-surface rendering.
- **VFR cell closure (opt-in).** New `[2D_OPTIONS]` keys `CELL_CLOSURE` (`FLAT` | `VFR`),
  `FACE_RECONSTRUCTION` (`MEAN` | `VFR_FACE`) and `VFR_MIN_WET_FRAC` (default `0.01`, range
  `(0, 0.5]`). The Begnudelli & Sanders (2006/2007) volume/free-surface closure restores the
  C-property at shorelines and removes the "water climbs uphill" artifact of the flat closure (which
  overstates a partially wet cell's free surface by up to two-thirds of its relief). Implemented on
  **all** backends — serial CVODE, serial ARKODE, and the Kokkos OpenMP/GPU path (device VFR kernels
  + preconditioner chain-rule), and covered by the analytic tangent. **The default stays
  `FLAT`/`MEAN`** (measured 1.64–3.3× slower than `FLAT` on the inundation benchmarks with the
  analytic Jacobian active, at near-equal BDF step counts but roughly twice the Krylov iterations —
  shoreline linear-solve conditioning, not a defect): VFR resolves the shoreline wetting/drying the
  flat closure freezes out, and is best reserved for shallow-water / gentle-slope cases (pair with
  `PRECONDITIONER=JACOBI` and a looser `REL_TOLERANCE` on small meshes). The GUI exposes all three
  keys in Simulation Options → 2D.
- **A 2D triangle may now carry several coupling rows, one per 1D node.** Previously the last parsed
  line in `[2D_TRIANGLE_NODE_MAP]` silently won, so two 1D nodes landing in the same 2D cell — a
  weir's or orifice's two endpoints, typically — left only one of them exchanging.
  `MeshData::tri_couplings` (`TriCouplingRow`) is now the source of truth; the per-triangle arrays
  are kept as a last-row-wins mirror for the existing getter API and the GeoPackage writer, and the
  parser maintains that mirror in step, because consumers that run *before* resolve — the GeoPackage
  writer above all — read those arrays, so deferring the mirror to `SurfaceRouter2D::initialize`
  dropped cell couplings on an opened-but-not-initialized model. `SurfaceRouter2D` synthesises rows
  from the legacy arrays when none exist (the GeoPackage path), resolves row names to indices,
  mirrors the last row back, and scales row areas by the mesh unit factor; `buildCouplingPoints`
  emits one `CouplingPoint` per row; `InpWriter` emits one row per coupling, preserving the legacy
  per-triangle fallback for meshes that never synthesised rows. New C API:
  `swmm_2d_add_triangle_coupling`, `swmm_2d_clear_triangle_couplings`,
  `swmm_2d_triangle_coupling_rows` and `swmm_2d_get_triangle_coupling_row`.
- **Degree-day snow store in the exponential initial-abstraction model.** `[RDII_DECAY]` gains an
  optional per-response `SNOW snow_T snow_ddf` clause, ported from the reference `IAModel`
  (sparsehydro): precipitation at `T <= snow_T` accumulates as SWE with no liquid input; above
  `snow_T` it melts at `snow_ddf*(T - snow_T)` per day, capped by the available SWE, and the melt is
  added to rainfall ahead of the IA depletion step — so rain-on-snow adds to the driving depth and
  melt-only steps produce snowmelt-driven RDII. `InflowData`/RDII carry `snow_on`/`snow_T`/`snow_ddf`
  plus per-response SWE state; eight-token rows stay snow-off, so the section is fully backward
  compatible. Wired through `InpWriter`, the GeoPackage schema (new snow columns, tolerant reads of
  older files), `swmm_rdii_decay_add`/`_set`/`_get` (a negative `snow_ddf` is rejected), and the
  Python bindings (keyword-optional snow arguments, extended `RDIIDecayEntry`). `validateExpDecay`
  warns when snow is enabled without a temperature source. Snow physics is pinned to
  reference-derived goldens.
- **Permissive ("lenient") open.** With lenient open enabled, post-parse validation errors are
  recorded and the engine still reaches `OPENED`, so an editor can load as much of a broken model as
  parsed instead of getting nothing. The recorded diagnostics are queryable through
  `swmm_get_error_count`/`swmm_get_error_at` and `swmm_get_warning_count`/`swmm_get_warning_at`, and
  from Python via `Solver.set_lenient_open` with `open_errors` / `open_warnings`. **Running still
  requires a strict open.** The fixtures drain a subcatchment to an undefined outlet node, which is
  ERROR 209 raised during post-parse cross-reference resolution; an undefined node named in
  `[CONDUITS]` cannot exercise this path, because the reader accepts it silently and records no
  error at all.
- **Nine new delete + `analyze_impact` API pairs** covering every remaining data-object type:
  `swmm_pollutant_delete`, `swmm_pattern_delete`, `swmm_aquifer_delete`, `swmm_snowpack_delete`,
  `swmm_lid_delete`, `swmm_street_delete`, `swmm_inlet_delete`, `swmm_landuse_delete`, and
  `swmm_hydrograph_delete` (name-keyed). Each cascades or nullifies every cross-reference and reports
  the impact set; `analyze_impact` previews the same set without mutating. `SWMM_RefType` gained 15
  additive values (`SWMM_REF_EXT_INFLOW` … `SWMM_REF_CONTROL_RULE`).
- **`swmm_control_find_references`** — read-only scan reporting which control rules reference an
  object by name (word-boundary match on NODE/LINK/CONDUIT/PUMP/ORIFICE/WEIR/OUTLET clauses,
  case-insensitive). Node/link `analyze_impact` and delete reports now include affected rules as
  `SWMM_REF_CONTROL_RULE` entries; **rule text is never edited by a delete**.
- **`swmm_control_remove_rule`** — remove a single rule by index (previously only
  `swmm_control_clear_rules` existed).
- **Python bindings for the object-deletion and control-reference surface.**
  `ModelEditor.analyze_*_impact` / `delete_*` for all nine entity types, with `_REF_TYPE_NAMES`
  extended to all 22 `SWMM_RefType` values; `Controls.remove_rule` / `find_references`, and
  `Controls.__delitem__` now removing in one C call rather than clear-and-re-add.
- **Standalone cross-section geometry API** (`openswmm_xsect.h`). Exposes the engine's own geometry
  kernels — area, top width, hydraulic radius, section factor, critical depth, and their inverses —
  as a reference implementation usable with no model open. Sections are built from shape +
  Geom1–Geom4, from transect / shape-curve / street data, or from a link of an open model
  (`swmm_link_create_xsect`), in which case the handle deep-copies the geometry the engine actually
  built and outlives the engine. Every scalar query has an `_array` counterpart. There is no geometry
  maths in the new code: it delegates to `xsect::setParams` and the `xsect::` kernels, so results
  match a simulation exactly.
- **`XSectionGeometry`** in `openswmm.engine` — the Python surface for the above, with
  scalar-or-NumPy dispatch on every query, `from_transect` / `from_curve` / `from_street` /
  `from_link` constructors, and a required keyword-only `units` argument (no default, so a unit
  system is never silently assumed). Also `shape_name()`, and a new `xsect_geometry` guide page.
- **`Links.get_xsect_info()`** — implements the method `_geometry` had documented since it was
  written but which never existed, returning the already-exported-but-never-constructed
  `CrossSection`. Also `link.xsect.info()` and `link.xsect.geometry()`, plus `CrossSection.from_raw`.
- **Optional per-subcatchment rainfall and snow scale factors** — trailing tokens 9 and 10 of
  `[SUBCATCHMENTS]`, both defaulting to `1.0` (a true no-op; default-valued models round-trip
  byte-identically). They compose multiplicatively with the gage factors:
  `rainfall = gage_rain × gage.scaleFactor × rainScaleFactor`,
  `snowfall = gage_rain × gage.scaleFactor × SCF × snowScaleFactor`. Applied to the gage-derived
  component only — API/forcing overrides are absolute and deliberately unscaled. Token 8 accepts `*`
  as a "no snow pack" placeholder so tokens 9/10 are reachable without a pack. Exposed across the
  full stack: C API (`swmm_subcatch_{get,set}_{rain,snow}_scale_factor`, settable mid-run), legacy
  property enum (`swmm_SUBCATCH_{RAIN,SNOW}_SCALE_FACTOR`), Python bindings
  (`Subcatchment.{rain,snow}_scale_factor` on both trees), GeoPackage subcatchments table
  (`rain_scale_factor`/`snow_scale_factor` REAL columns, reader guards a column-existence check so
  pre-existing `.gpkg` files still open), and the openswmm.mcp
  `editing_{get,set}_subcatch_{rain,snow}_scale_factor` tools (plus
  `editing_{get,set}_gage_snow_factor`, closing the last gage-SCF gap). The GUI surfaces all four
  factors in the Property Browser and the Attribute Table.

### Changed

- **`FV_MIN_CELLS` now defaults to 4, and is a real floor.** It previously
  defaulted to 1 and was applied only when `FV_CELL_LENGTH` was also set, so on
  its own it did nothing — a parsed knob that needed a second knob to have any
  effect. It now applies with or without a length target.

  One cell per conduit was the default and should not have been. A conduit
  meshed as a single cell has no interior gradient of its own and presents an
  artificial bed step of half its fall at every manhole, so it under-conveys.
  Measured on Example1, mean absolute peak-flow deviation from dynamic wave
  against cells per conduit — 1: **37.1 %**, 2: 25.7 %, 4: **15.3 %**,
  8: 7.6 %; worst link −75.8 % → −43.2 % → −22.6 %; wall-clock 1.0× → 2.2× →
  5.4×. Four is the knee, not the answer: it more than halves the one-cell
  error for about twice the cost. Raise it, or set `FV_CELL_LENGTH`, where peak
  flows or in-conduit profiles matter.

- **The validated 2D solver regime is now the default**, being the configuration measured fastest
  *and* hydrologically complete on the 13k-cell, 48-hour Bellinge multiscale benchmark:
  - `MIN_TIMESTEP` default `0.001` → `0` (no CVODE step floor). Any hard floor makes wetting-front
    corrector retries unrecoverable — a measured 12k–128k hard failures, each producing a frozen
    window with its rainfall silently dropped. With no floor the same probe runs with zero hard
    failures and every window integrated.
  - Linear-solver setup lag defaults to 50 (was the SUNDIALS stock policy); env
    `OPENSWMM_2D_LSETUP_FREQ` overrides, `<= 0` restores stock.
  - Partial-progress windows, the clamp-consistent tangent and the tangent-exact preconditioner are
    on, each with the env kill-switch documented above.
- **The C-API-to-Python gap is closed.** `python/tests/test_api_coverage.py` now reports 886 C
  symbols with **0 unjustified unbound** (was 6 unjustified plus 18 allowlisted-but-in-scope),
  leaving only the four intentionally-unbound error accessors on the allowlist.
- **Control-rule parse errors now name the line and the cause.** Every rejection site in
  `ControlEngine::parseRuleText` records a 1-based line number (counting blank lines, so it indexes
  the caller's text directly) plus a specific reason — `"line 4: no link named 'OR_NOPE' exists in
  the model"` rather than the previous fixed `"Control-rule parser rejected the rule text"` with
  `line_out` hardcoded to `-1`. Exposed via `ControlEngine::lastParseError()`, and surfaced through
  `swmm_control_validate_rule` (`errbuf` + `line_out`), `swmm_control_add_rule`
  (`swmm_get_last_error_msg`), and the `[CONTROLS]` block error raised during `initialize()`.
- `link::applyTabulatedXSectParams` (new, in `Link.cpp`) consolidates the IRREGULAR / CUSTOM /
  STREET full-flow property derivation that previously lived only inside `PostParseResolver`, so the
  standalone cross-section constructors and the resolver share one legacy-parity implementation.
  Behaviour is unchanged — `ctest` is green including all transect/street parity tests.
- `swmm_pattern_remove` shares the `ObjectDeleter` code path instead of duplicating it.

### Fixed

- **`FLOW_ROUTING FV` process coverage — a family of silent divergences.** The
  finite-volume solver reduced the engine coupling to a four-field forcing
  struct, so anything not expressible in those four arrays was unreachable from
  the marching loop, and nothing checked for it. No FV test contained a pump,
  orifice, weir, outlet, control rule, storage node or ponding, so deleting the
  structure-flow field entirely would have passed all 57 of them. Each item
  below produced a plausible-looking report of something that did not happen:

  - Out-of-bounds write into dynamic-wave arrays that `Router::init` never
    allocates under FV, whenever a model contained any non-conduit link.
  - Mesh-build errors were discarded: a model the solver could not mesh ran to
    completion with **zero routing** and exited clean.
  - Storage-node losses were charged to the mass balance but never removed from
    the water; conduit seepage under US units was **43 200× too large**
    (`UCF(RAINFALL)`, skipped by the US early-out) and saturated at the
    availability cap.
  - `ALLOW_PONDING` and `SURCHARGE_DEPTH` were never read; a ponding node
    reported no flooding at all; ponded storage never reached the mass balance
    — the last of these a **DYNWAVE fix as much as an FV one**, worth 82.5 % of
    routing continuity error on a single-junction pond.
  - Flap gates on conduits and outfalls were ignored: a dry junction under an
    8 ft outfall stage filled at 16.4 cfs with both gates set.
  - Multi-barrel conduits reported `barrels ×` the flow of the single barrel
    actually marched, **creating 3.4 % of the routed volume out of nothing**. A
    cell is now the aggregate section of all barrels.
  - Culvert inlet control was applied by overwriting `links.flow` after the node
    ledger had been booked, so the cap existed only in the report — under
    DYNWAVE it still does not hold the water back. FV now applies it as a
    prescribed-discharge boundary at the culvert's upstream face.
  - Force mains used Manning's equivalent *n* at all depths instead of switching
    to Hazen-Williams / Darcy-Weisbach once pressurized.
  - `FV_STRUCTURE_COUPLING` was parsed, written, C-API exposed and read
    nowhere. Structure discharge was published as the last substep's sample
    rather than the mean actually applied.
  - `SAVE OUTFLOWS` wrote the interface file for the kinematic-wave node set.
  - The report said "STEADY", classified every conduit 100 % dry, left the
    Conduit Surcharge Summary permanently empty, and printed the explicit
    substep count under "Average Iterations per Step".

- **DYNWAVE conduit seepage was identically zero for every cross-section
  shape.** `buildXSP` never populated `yw_max`, so the seepage clamp compared
  the flow depth against 0 and drove the wetted width to 0. `[LOSSES]` seepage
  applied only under KINWAVE/STEADY/FV, which build their parameters elsewhere.

- **`[XSECTIONS]` and `[LOSSES]` rows were dropped for links declared later in
  the file.** In the conventional layout `[XSECTIONS]` precedes
  `[ORIFICES]`/`[WEIRS]`, so geometry for a regulator was routinely discarded
  and the link ran at zero area and passed no flow under any head. Unresolved
  rows are now replayed after the last section; one that still does not resolve
  raises ERROR 209, as legacy does.

- **Node convergence was tested on the mixed iterate, not the fixed-point residual (#97).**
  `updateNodeDepthsTeam()` tested `|nodes.depth - y_last| <= head_tol`, where `nodes.depth` is the
  possibly Anderson-mixed value. That measures how far the *accepted* iterate moved, not the
  residual of the hydraulic operator `G`. When the two-point coefficient clamps to 1 the mix
  returns `g_prev`; if the previous iteration did not itself mix then `y_last == g_prev`, so the
  accepted movement is exactly zero no matter how far `G(y_last)` actually maps — the node is
  declared converged with a raw residual of any magnitude. This is not a reporting quirk:
  `xnode_.converged` feeds `findBypassedLinks()` (which freezes both endpoint links) and the
  loop-exit test `t_converged = (unconv_shared == 0)`, so a network that all false-converges exits
  the Picard loop with an unconverged hydraulic state and frozen flows. Convergence now requires
  **both** the raw Picard residual `|g_k - y_last|` and the accepted movement
  `|nodes.depth - y_last|` to be within `head_tol`. No extra operator evaluation; with Anderson
  acceleration off — it remains opt-in and default off — `nodes.depth == g_k`, so the test collapses
  bit-exactly to the previous behaviour.
- **The Anderson mixing coefficient was applied to the wrong operand (#98).** The two-point mixer
  computed `alpha = r_k/(r_k - r_km1)`, the secant weight on the *previous* mapped value (Aitken
  form `y = g_k - alpha*(g_k - g_prev)`), then blended it onto the *current* one:
  `y = (1 - alpha)*g_prev + alpha*g_k`. On a sign-changing residual pair (`0.10 → -0.05`) the
  correct current-value weight is 2/3, which cancels the linear-model residual; the code used 1/3
  and left 0.05. Worse, on same-sign shrinking residuals (`0.10 → 0.05`) the unclamped coefficient
  is negative, so the `[0,1]` clamp drove `alpha` to 0 and the mixer returned the *older* mapped
  value — regressing the iteration exactly when it was converging monotonically. The blend operands
  are swapped and the coefficient kept, now documented as the previous-value weight. Residual
  definitions, the `[0,1]` clamp, the `20*head_tol` residual gate and the `dr^2` guard are unchanged.
- **Anderson-accepted depths bypassed the canonical node state commit (#100).** `setNodeDepth()`
  commits a complete node state for the raw Picard result — depth, head, volume, overflow, `dYdT` —
  but the accepted-Anderson branch in `updateNodeDepthsTeam()` overwrote only depth and head. When
  the accepted mix was the final Picard iteration the node's volume, overflow and timestep-control
  derivative still described the unmixed candidate, feeding inconsistent state into flooding
  totals, mass balance, next-step storage losses and the next adaptive routing step. The commit
  block at the tail of `setNodeDepth()` is refactored into `DWSolver::commitNodeDepthState()` and
  called from both paths: it reapplies the physical lower bound and the flooding/ponding caps,
  resets overflow before the flooding branch so a re-commit cannot inherit the raw candidate's
  overflow, recomputes volume from the accepted depth, and derives `dYdT` and the depth/head pair
  together. With Anderson off the single call performs the identical arithmetic in the identical
  order, so the default path is bit-exact.
- **Outfalls were counted in the per-node nonconvergence statistics (#101)** — an inherited legacy
  defect, present in both engines. Outfall nodes deliberately keep `converged == FALSE` inside the
  Picard iteration so outfall-connected links are never bypassed, and the step-convergence test
  already skips them — but after a failed routing step the per-node statistics counted every false
  flag, so "Most Frequent Nonconverging Nodes" could rank a boundary outfall first, at exactly the
  overall failed-step percentage, masking the junctions or storage nodes actually responsible.
  Outfalls are now filtered where the post-Picard diagnostic counter is incremented:
  `node_tile_[ui].is_outfall` in `DWSolver::execute` (refactored) and `Node[i].type == OUTFALL` in
  `updateConvergenceStats` (legacy). Reporting-only: converged flags, `unconv_shared`, the
  routing-step convergence decision, `findBypassedLinks` and `NonConvergeCount` are unchanged, so
  hydraulics and the overall failed-step percentage are unaffected.
- **`USE HOTSTART` rejected every legacy `.hsf` written by a model with subcatchments (#93).** The
  refactored engine dispatches `USE HOTSTART` to `HotStartManager::apply_legacy_routing()`, which
  bailed out on any header reporting `nSub != 0` with "Hotstart with subcatchments is not yet
  supported" — surfaced to Python as `HotStartError`. Legacy `.hsf` version 3/4 files *always*
  write a subcatchment count and a full `readRunoff()` section ahead of the routing records, so
  effectively no real model could start from a hot start even though the legacy solver reads the
  same file fine. The reader now advances the stream past the runoff section instead. The runoff
  record layout is not self-describing — each subcatchment's length depends on whether it has
  groundwater and/or a snowpack — so the skip length is reconstructed from the current model
  structure (`gw_aquifer[i] >= 0` mirrors legacy `Subcatch[i].groundwater != NULL`,
  `snowpack[i] >= 0` mirrors `Subcatch[i].snowpack != NULL`), which is the same "model must match
  the file" assumption legacy `readRunoff()` itself relies on. The `fileVersion == 2` inline
  two-float-per-subcatchment groundwater block is handled, and the subcatchment count is validated
  against the model rather than misread; a file claiming more subcatchments than the model is
  rejected rather than misread.
  **Scope:** routing (node/link) state only. Subcatchment runoff / infiltration / groundwater /
  snowpack / buildup state in the file is skipped, not applied, so a continuation run is
  hydraulically hot-started while its hydrology restarts cold. `SWMMEngine` now passes a warn
  callback into `apply_legacy_routing` so that divergence from legacy is surfaced as a model
  warning instead of being silently lossy; applying runoff state is a follow-up.
- **Repeated monthly unit-hydrograph parameters were applied silently (#43).** An `ALL` entry in
  `[HYDROGRAPHS]` overrides month-specific values entered on earlier lines — e.g. JAN–SEP entries
  followed by `ALL` leaves only later OCT–DEC entries intact — with no diagnostic. That, and any
  repeated assignment of the same month/response, now raises the new **WARNING 13** in the status
  report: the legacy engine tracks assigned months via `TUnitHyd.paramsSet` and issues it from
  `rdii_readUnitHydParams()`/`readOldUHFormat()`; the refactored engine pushes the matching
  `WARN_UH_PARAMS_REPEATED` (13) to `ctx.warnings` from `RDIISolver::init()`. The override behaviour
  is documented in User Manual Appendix D, `[HYDROGRAPHS]` Remarks.
- **`InpWriter` corrupted `[OUTLETS]`, `[XSECTIONS]`, `[CURVES]`, `[PUMPS]` and `[WEIRS]` on save.**
  The round trip was not idempotent: some sections crashed the legacy parser, others silently
  changed results in the refactored engine itself.
  - `[OUTLETS]` emitted a bare `FUNCTIONAL 0 0` instead of the real rating type
    (`TABULAR`/`HEAD`/…), curve name and gate flag. The bare form segfaulted the legacy parser
    (`strcomp(NULL, "HEAD")`) and dropped the rating curve.
  - `[XSECTIONS]` wrote `0.0000` in place of a `CUSTOM` shape-curve name (Error 209) and emitted
    OUTLET links as `CIRCULAR 0` (Error 211); OUTLET links are now skipped.
  - `[CURVES]` repeated the type keyword on every row, which the legacy parser read as an X-value
    (Error 211); it is now emitted on the first row only.
  - `[PUMPS]` hardcoded `ON 0 0`, dropping the wet-well startup/shutoff control depths. Zero
    thresholds make every pump run unconditionally: on the Bellinge model that doubled flooding and
    drove routing continuity from 0.35% to −4.7%. Real Status/Startup/Shutoff are now written.
  - `[WEIRS]` hardcoded `TRANSVERSE`/`NO`/`0`/`0`, so weir type, gate, end contractions, end
    coefficient and the can-surcharge flag were lost — and dropping can-surcharge defaulted it back
    to `YES` on re-read. All five are now emitted.
  - Pump startup/shutoff units did not round-trip: the display→feet conversion applied at load had
    no mirror in `convert_internal_to_display`, so the writer dumped internal feet and inflated the
    depths by 3.28084× per save/open cycle on SI projects. The feet→display back-conversion was
    added.
  - Conduit length and roughness, cross-section geometry and the DWF average value are written at
    full precision (`%.15g`) instead of being truncated to 4–6 decimals.

  Verified against the Bellinge model: the round trip now reproduces the original in both engines
  (legacy routing continuity 0.345% vs 0.350%, flooding 0.097 vs 0.097), with no regression on the
  standard round-trip test models.
- **The refactored engine accepted input that legacy EPA SWMM rejects, and its warnings never
  reached the `.rpt`.** Three root causes: the parser/resolver detected problems and then
  reset-to-`-1`/continued silently; `emit_warning()` was callback-only and never reached
  `ctx.warnings` or the report; and there was no equivalent of legacy `project_validate`.
  (`BellingeSWMM_v021_nopervious` — a subcatchment with `*` gage/outlet — is the reported case:
  legacy raises ERROR 209, the refactored engine ran it.)
  - New `push_report_warning()` routes warnings to **both** the `.rpt` and the API; unknown-section
    and unknown-`[OPTIONS]`-keyword warnings now go through the report accumulator using the legacy
    wording.
  - An undefined subcatchment outlet or rain gage, and an unresolved storage/pump/outlet curve name,
    are now fatal `ERR_NAME` (209). `SubcatchData::gage_name` was added so an out-of-order
    `[RAINGAGES]` section re-resolves instead of false-positiving.
  - New `validate_project()` reproduces the legacy step-clamp warnings 01/06/07, and WARNING 05
    (minimum slope) and 08 (elevation drop exceeds length) are emitted at the existing clamp sites.
  - `write_open_failure_report()` — a failed open now writes the `.rpt` with its ERROR/WARNING lines
    as legacy does (report `prepare()` runs only in `start()`).
  - 2D: numeric vertex/triangle tags in `[2D_*_NODE_MAP]` fall back to a tag lookup instead of being
    mis-read as an out-of-range index; `SurfaceRouter2D::initialize` is wrapped in try/catch so a
    bad-mesh throw becomes a graceful `.rpt` error rather than `std::terminate` across the `noexcept`
    boundary; and six stderr-only 2D warnings are routed into `ctx.warnings`.

  Bellinge now emits ERROR 209 to the `.rpt` like legacy, and the 14 standard EPA QA models open
  with no spurious errors.
- **`openswmm::discover_all_filters` / `discover_plugins_by_id` were unresolved on Windows.** The
  engine target sets `WINDOWS_EXPORT_ALL_SYMBOLS OFF` and relies on explicit export macros, but the
  declarations in `PluginDiscovery.hpp` carried none — which linked fine on ELF/Mach-O (the target's
  `CXX_VISIBILITY_PRESET` is `default`) while failing with LNK2001 on Windows. Both are now
  annotated `SWMM_ENGINE_API`, and the header documents the `std::vector`-by-value caveat: safe for
  consumers built with the same toolchain and C++ runtime as the engine, not across MSVC runtimes.
- **DWF and groundwater pollutant mass was added without its carrier volume, so it was discarded.**
  `QualitySolver::addDwfLoads()` and `addGwLoads()` accumulated pollutant mass into
  `nodes.qual_mass_in` but never added the corresponding inflow volume to `nodes.qual_vol_in`.
  `mixAtNodes()` short-circuits on `if (v_in <= 0.0) { nodes.conc[idx] = c_old; continue; }`, so a
  node fed only by dry-weather flow or groundwater stayed at zero concentration forever, passed
  zero mass downstream — leaving the whole network at 0 — and silently lost that mass from the
  quality balance. All six sibling loaders (wet weather, LID drain, RDII, interface file, link,
  inlets) accumulate `qual_vol_in`, and legacy `qualrout.c` mixes on `Node[j].inflow`, which
  includes DWF and GW. **Expected baseline shift:** models with DWF or groundwater inflow *and*
  quality routing enabled now report nonzero Dry Weather Inflow and Groundwater Inflow quality rows
  where they previously reported zero.
- **`swmm_control_add_rule` silently did nothing after `swmm_engine_initialize`.** Rules are
  compiled once, inside `initialize()` → `initHydraulics()`; text added afterwards was appended to
  `ctx.control_rules.rule_text` and never parsed. The call returned `SWMM_OK` and
  `swmm_control_count` incremented, so the rule looked accepted while having no effect for the
  entire run. It now compiles into the live `ControlEngine` and takes effect on the next step. Text
  is parsed into a throwaway engine first, so a rejection cannot leave the live rule set
  half-mutated, and rejected text is not stored. Behaviour before `initialize()` is unchanged
  (stored verbatim, compiled later) because a model under construction may legitimately reference
  objects that do not exist yet.
- **`ControlEngine::clearRules()`** added and called before the `[CONTROLS]` parse loop.
  `parseRuleText` appends, so re-running `initialize()` over the same rule store previously stacked
  a second copy of every rule.
- **`swmm_engine_initialize` could not be called twice before `start()`.** A second call recompiles
  stored control rules, which is the contract the control-rule work above depends on, but the state
  gate accepted only `OPENED` and the second call failed with `SWMM_ERR_LIFECYCLE`. `INITIALIZED` is
  now accepted as well: every `init_*` step rebuilds from `ctx_` rather than appending, and
  `ControlEngine::clearRules` already guards the rule-stacking hazard. Re-initializing after
  `start()`/`end()` still requires a fresh `open()`.
- **The failed-window freeze created water on every failed window.** New `resyncFromVolumes(t0)` on
  `ISurfaceSolver` (CVODE override; the default falls back to `reinitialize`): the freeze previously
  re-seeded `y` from the **clamped** head reconstruction, zeroing negative-volume debt — the
  dominant residual leak in the failed-window regime. On the overdraw reproducer the tight-tolerance
  2D block goes from −1.820% to 0.000%.
- **Quiescent-window skips and failed-window freezes desynchronised the CVODE clock.** Both advanced
  simulation time without advancing CVODE's internal clock, so the next live window integrated the
  newly held forcing over the whole gap — 0.4 m³/s of rain × 1080 s of lag produced a +432 m³
  phantom-volume spike. Added a clock-resync guard in `CvodeSurfaceSolver::advance`. As a follow-on,
  the failed-window `resyncFromVolumes` now re-times at the window **end**, eliminating the duplicate
  clock-desync `CVodeReInit` — and its second AMG invalidation — per frozen window;
  trajectory-equivalent.
- **The rendered and profiled water surface climbed adverse slopes and bed steps with no driving
  head.** The solver vertex-head field is a pseudo-Laplacian over *all* incident cells with dry-cell
  head set to bed elevation, and it was exported directly for rendering, so dry-cell bed elevations
  blended into shoreline vertices. Rendering now consumes the wet-masked signed render field
  described above; solver `vert_head`, gradients, limiters, fluxes and active-set semantics are
  untouched.
- **"2D Solver Statistics" reported 0 internal BDF steps and 0 RHS evaluations.** `CVodeReInit`
  (clock resync, reseed, reinitialize) resets the `CVodeGetNum*` counters, and `run_stats()` read
  them only after the last window — often a quiescent or frozen one. The live counters are now
  snapshotted into `acc_*` before each of the three `ReInit` sites and `run_stats()` returns
  `acc_* + live`; an 8-hour storm run goes from 0 to 2,077 BDF steps, matching `DEBUG_SINK`.
- **GeoPackage observed-series writes swallowed SQLite errors.**
  `swmm_gpkg_create_observed_series` ignored the `sqlite3_step` result, so a UNIQUE-name violation
  against a stale `.gpkg` silently returned `last_insert_rowid = 0` as the series id and observed
  values accumulated under `series_id 0` across runs. Step results are now checked in
  `create_observed_series` and `write_observed_value` (the bulk writer already did).
- **`SWMM_XSectShape` selected the wrong cross-section for every code from 8 up.**
  `swmm_link_set_xsect`/`get_xsect` pass the shape straight through with a `static_cast` to the
  engine's storage enum (`openswmm::XsectShape`), but the published `SWMM_XSECT_*` constants
  followed a different ordering — so `SWMM_XSECT_IRREGULAR` (19) stored a vertical ellipse,
  `SWMM_XSECT_EGGSHAPED` (14) stored a baskethandle, and so on. Only codes 0–7 and
  `SWMM_XSECT_STREET` were correct. The constants are now the storage codes, and 26
  `static_assert`s in `openswmm_links_impl.cpp` pin the two together so the cast is provably sound
  and any future drift breaks the build instead of the model. **Breaking for C code that hard-coded
  the old integers**; code that passed the constants symbolically is corrected by recompiling. Five
  shapes that had no constant at all (`BASKETHANDLE`, `SEMICIRCULAR`, `CUSTOM`, `FORCE_MAIN`,
  `DUMMY`) were added. `swmm_xsect_shape_name()` resolves a code at runtime.
- **Python `XSectShape.IRREGULAR` / `CUSTOM` / `FORCE_MAIN` were mismapped.** Their values (16/17/18)
  were read by the engine as `RECT_TRIANG` / `RECT_ROUND` / `HORIZ_ELLIPSE`, so assigning them
  silently produced the wrong cross-section. The enum now mirrors the engine's codes exactly and
  gained the seven shapes it was missing (`RECT_TRIANG`, `RECT_ROUND`, `HORIZ_ELLIPSE`,
  `VERT_ELLIPSE`, `ARCH`, `STREET_XSECT`, `DUMMY`) — all 26 are now nameable.
- **`_geometry._GEOM_LABELS_EXTRA` labelled the wrong shape codes.** The ellipse/arch labels sat on
  19/20/21 rather than 18/19/20. The table is folded into `_GEOM_LABELS`, now keyed by enum member
  and covering every shape.
- **Node delete left dangling references** in ext-inflow / DWF / RDII rows (rows now cascade-deleted,
  survivors renumbered), the positional treatment expression matrix (the deleted node's stripe is now
  erased — previously every node after it silently read its neighbor's treatment), and subcatchment
  `gw_node` (now nullified, previously only renumbered).
- **Subcatchment delete** now cascades LID-usage rows, clears LID `drain_to` and snowpack
  `removal_subcatch` name references.
- **Rain-gage delete** now clears unit-hydrograph gage assignments.
- **Table/timeseries delete** now clears the gage `ts_name` mirror and ext-inflow `ts_name`
  references, and nullifies + renumbers the subcatchment adjustment-pattern indices
  (`n_perv`/`d_store`/`infil`), which index tables and were previously silently misaligned by any
  table delete.
- **Street delete** resets STREET cross-sections on referencing conduits to CIRCULAR (mirrors
  transect delete) instead of leaving a dangling name.
- `swmm_hydrograph_remove_group` now delegates to the same deleter as the new APIs.
- **Refactored engine dropped the gage snow catch factor (SCF) at runtime.** The C++ engine parsed,
  stored, wrote and API-exposed the `[RAINGAGES]` SCF but never multiplied by it: `separatePrecip()`
  was dead code with zero call sites, and the live rain/snow split in `SWMMEngine.cpp` used the raw
  gage intensity. Legacy `gage.c` applies SCF. Snowfall volume now matches legacy. Replaced
  `separatePrecip()` with `gage::splitPrecip()`, the single source of truth for the split.
  **Behaviour change for any model with `SCF ≠ 1.0`** — every model in `tests/` and `examples/` uses
  `SCF = 1.0`, so no shipped baseline moves, but user snow models will.
- **Refactored engine applied no rain/snow split for snow-pack-less subcatchments.** The
  non-snow-pack runoff path read raw gage rainfall with no temperature test, so below freezing it
  received `rain × 1.0` where legacy gives `rain × SCF`. `RunoffSolver::execute()` now routes
  through `splitPrecip()` too. **Behaviour change for `SCF ≠ 1.0` snow-season models.**
- **`InpWriter` silently dropped the `[SUBCATCHMENTS]` snow-pack assignment.** Only 8 columns were
  emitted, so token 8 (Snowpack) was lost on every INP round-trip. The writer now emits it (and a
  `*` placeholder when a trailing scale factor must reach past an unassigned pack).
- **`swmm_gage_set_snow_factor` was not settable mid-run.** It carried a `CHECK_GEOMETRY` guard the
  sibling `swmm_gage_set_scale_factor` deliberately omits, so a calibration/RTC loop could not
  adjust the SCF after the model was opened. Removed the guard — SCF is a scalar precipitation
  multiplier, not geometry, and now matches the rainfall scale factor and the new subcatchment
  scale factors.
- **The missing `StorageShape` stub in `_enums.pyi`.** `_nodes.pyi` imports it, and without it
  autodoc fails to import every engine module — which is the entire documented API surface.
- **`SWMMSubcatchmentProperties.RAIN_SCALE_FACTOR` / `.SNOW_SCALE_FACTOR` were absent from
  `_solver.pyi`.** The legacy `_subcatchments.py` accessors reference them and they exist in the
  `_solver.pyx` runtime enum, but mypy reads the stub, so both typing CI jobs failed with
  `attr-defined` errors.

### Performance

- **Vertex-head reconstruction hoisted out of the RHS.** The all-vertex pseudo-Laplacian pass
  (`reconstructVertexHeads`) ran on every CVODE `rhs_fn`/`Jv` evaluation but is not read by the
  diffusive-wave edge flux, which uses centroid heads only. It now runs once per accepted window;
  the few coupling-point consumers inside the RHS evaluate their single vertex on demand via
  `vertexHeadAt()`, and the field is seeded at initialize so pre-first-window consumers do not read
  zeros. On vertex-heavy meshes this sheds the whole per-vertex pass from the ~4×10⁵ RHS calls a run
  makes; conservation is unchanged.
- **Analytic J·v on the primary targets:** `weir_culvert` 59 s → 47.7 s with Krylov iterations
  179,170 → 59,242 (the exact Jacobian beats the FD approximation); `road_culvert` 36 s → 24.1 s.
  Physics matches FD — road drain 42001.3 (FD) vs 42001.8 (analytic), 0.001%.
- **Analytic live coupling:** `weir` 46.85 s → 45.46 s (Krylov 59,242 → 32,842); `road` 23.95 s →
  15.34 s, −36% (Krylov −41%, nonlinear convergence failures 1,229 → 4, as the orifice diagonal
  makes the stiff exchange visible to Newton).
- **Graded tolerance on MS-A 10⁴:1:** 1,065 → 930 BDF steps (−13%), 12.63 s → 11.45 s, continuity
  −0.046% → −0.030%.
- **Tangent-exact preconditioner:** 7.7× on the 8-hour solo storm probe, 606 s → 79 s, Krylov −92%.
- **New defaults, 8-hour solo storm probe at `THREADS 4`:** 213 s with 12,391 frozen windows and
  their rainfall dropped → 80 s with zero failures, zero frozen windows, average internal step
  2.8 s, 2D continuity −0.04%. 48-hour solo: 2,048 s → 358 s. Coupled 48-hour: flow continuity
  −9.5% → +5.5% with a genuinely live surface — the old "clean" regime's 2D was 99.95% frozen.

## [6.0.0-alpha.2] — 2026-07-12

The headline of this release is legacy bit-parity for the 1D engine. The refactored engine's
divergence from legacy EPA SWMM 5.3 was almost entirely ULP-level operand-order and constant
substitution, which long surcharged runs amplify to macroscopic flow differences, so closing it
required matching legacy arithmetic op-for-op rather than merely reproducing its formulas. On the
Bellinge benchmark all 111,828 routing steps are now bit-identical — timestep sequence, iteration
counts and hex state fingerprints — and every `.out` variable (node depth/head/volume/lateral
inflow/total inflow/overflow; link flow/depth/velocity/volume/capacity; all subcatchment variables)
has max |diff| = 0.0 at float32 across all 2,640 report periods. **Outside the elementwise gate:**
the 15-float per-period system summary block (area-weighted means) is not yet byte-identical.

### Added

- **`SAVE HOTSTART` is implemented** — a legacy-format `.hsf` writer plus the end-of-run trigger, so
  a run can now produce the hot-start file that `USE HOTSTART` consumes.
- **First-class `CYLINDRICAL`, `CONICAL`, `PARABOLOID` and `PYRAMIDAL` storage shapes**, stored and
  validated as geometric shapes rather than being forced through a tabular depth-area curve.
- **Runtime climate forcing.** Air temperature and wind speed can be prescribed while a simulation
  runs, in both engines: legacy `swmm_API_TEMPERATURE` (`<= -999` clears) and `swmm_API_WINDSPEED`
  (negative clears) system properties with read-only `swmm_TEMPERATURE` / `swmm_WINDSPEED`
  companions; refactored `swmm_forcing_climate_temperature()` / `swmm_forcing_climate_wind()`
  channels (OVERRIDE/ADD, RESET/PERSIST) with `swmm_climate_get_temperature()` /
  `swmm_climate_get_wind_speed()`. Temperature is applied *before* the derived climate quantities
  (saturation vapor pressure, psychrometric constant, Hargreaves moving average) so snowmelt and
  temperature-evap consumers stay consistent. User units throughout (°F/°C, mph/km/hr). Python:
  `LegacySystem.set/get/clear_api_temperature` and `…_api_wind_speed`, `Forcing.climate_temperature`
  / `.climate_wind`, the matching getters, and `ForcingTarget.CLIMATE` for `Forcing.clear`.
- **Runtime evaporation forcing.** A global evaporation prescription replaces the post-adjustment
  `Evap.rate` for all consumers including conduits and storage (per-subcatchment PET still wins):
  legacy `swmm_API_EVAP` system property, refactored `swmm_forcing_climate_evap()` and the read-only
  `swmm_climate_get_evap_rate()` / `Forcing.climate_evap_rate()` for caller-side adjustment
  composition. The `DRY_ONLY` flag is togglable at runtime as well (legacy `swmm_EVAP_DRY_ONLY`,
  refactored `swmm_climate_set/get_dry_only()`). Python: `LegacySystem.set/get/clear_api_evap_rate`
  and `set/get_evap_dry_only`, `Forcing.climate_evap` / `.climate_dry_only`.
- **Per-subcatchment PET prescription.** New legacy `swmm_SUBCATCH_API_PET` subcatchment property
  prescribes a potential-evapotranspiration rate (in/day or mm/day) per subcatchment at runtime; the
  prescribed rate replaces the climate-derived `Evap.rate` for surface, LID and groundwater
  upper-zone evaporation, bypassing `DRY_ONLY` and the monthly adjustments, and a negative value
  clears it. New read-only `swmm_EVAPRATE` returns the current climate-derived rate. Python:
  `LegacySubcatchment.set_api_pet` / `get_api_pet` / `clear_api_pet`, `LegacySystem.get_evap_rate`.
- **Per-subcatchment snowfall forcing.** Refactored `swmm_forcing_subcatch_snowfall()` (in/hr US,
  mm/hr SI as SWE; OVERRIDE/ADD, RESET/PERSIST) with `Forcing.subcatchment_snowfall()` and
  `ForcingType.SUBCATCH_SNOWFALL`; it resolves on the temperature-split gage snowfall before
  accumulation, plowing and melt. Legacy already had `swmm_SUBCATCH_API_SNOWFALL`.
- **Runtime water-quality sources.** Legacy gains a `swmm_POLLUTANT` dispatch with
  `swmm_POLLUT_RAIN/GW/RDII/DWF_CONCEN` (500 block), all runtime-settable, plus
  `swmm_SUBCATCH_POLLUTANT_PONDED_CONCENTRATION` settable while running; the refactored engine gains
  a link-quality forcing channel `swmm_forcing_link_quality()` (REPLACE = concentration, ADD = mass
  rate, mass-balanced) with `Forcing.link_quality` and `ForcingType.LINK_QUALITY`. Python
  `SWMMPollutantProperties` enum. Each source feeds an existing inflow term already counted in the
  quality mass balance.
- **Runtime hydrologic state injection.** Groundwater moisture and lower-zone depth (legacy
  `swmm_SUBCATCH_GW_MOISTURE` / `_GW_LOWER_DEPTH` via `gwater_get/setState`; refactored
  `swmm_subcatch_set/get_gw_state()` with porosity/thickness clamping) and snowpack SWE / free water
  / ATI / cold content (legacy `swmm_SUBCATCH_SNOW_SWE/_FW/_ATI/_COLDC` per snow subarea via
  `sub_index`; refactored `swmm_subcatch_set/get_snow_state()`) are now readable and writable
  mid-run.
- **2D mesh evaporation** — a depth-limited evaporation sink inside the CVODE RHS, driven by
  `swmm_2d_force_evap()` / `swmm_2d_force_evap_uniform()` (m/s, OVERRIDE/ADD, RESET/PERSIST) and
  `Surface2D.force_evap`/`_uniform`; `swmm_2d_get_mass_balance()` gained an `evap_out` total. The 2D
  solver still has no infiltration sink.
- **A runtime-editable parameter surface, with an explicit contract per group.** Both engines now
  expose setters for the parameters that are sound to change mid-run, and reject the ones that are
  not rather than accepting an edit that silently does nothing:
  - **Time-pattern factors and street sweeping** — legacy `swmm_TIME_PATTERN` +
    `swmm_PatternProperty` (`FACTOR` with the factor index in `subIndex`, read-only `COUNT`/`TYPE`)
    and `swmm_LANDUSE` + `swmm_LanduseProperty` (`SWEEP_INTERVAL`/`SWEEP_REMOVAL`) through new
    `set/getPatternValue` / `set/getLanduseValue` in `swmm5.c`; both are per-step lookups, so both
    are settable pre-start and while running. Python `SWMMPatternProperties` /
    `SWMMLandUseProperties`.
  - **Buildup / washoff function coefficients** — `swmm_LanduseProperty` extended with
    `BUILDUP_FUNC`/`COEFF1..3`/`NORMALIZER` and `WASHOFF_FUNC`/`COEFF`/`EXPON`/`SWEEP_EFFIC`/
    `BMP_EFFIC` (pollutant index via `subIndex`); the accumulated buildup pool is preserved, so an
    edit only changes how buildup evolves going forward, and buildup edits recompute `maxDays` per
    `landuse_readBuildup`.
  - **Pollutant kinetics** — `kdecay` / co-pollutant / snow-only are read live each step; legacy
    parity via `swmm_PollutProperty` `KDECAY`/`CO_POLLUTANT`/`CO_FRACTION`/`SNOW_ONLY` (kdecay
    accepted in 1/day, stored as the legacy 1/sec), `SWMMPollutantProperties` 4→9. The initial
    network concentration (`INIT_CONCEN`) has no per-step consumer, so both engines reject it mid-run
    (refactored `LifecycleError`, legacy `ERR_API_IS_RUNNING`).
  - **External-inflow and DWF baselines and scale** — the inflow solver caches its definitions at
    start, so the new `swmm_ext_inflow_set_scale` / `_set_baseline` / `swmm_dwf_set_baseline` (and
    the add/remove paths) refresh that cache and a mid-run edit takes effect on the next step;
    bindings `Inflows.set_external_scale` / `_baseline` / `set_dwf_baseline`. Legacy parity for
    node-keyed `[INFLOWS]`/`[DWF]` baseline editing is deferred — the legacy per-node linked-list
    inflow model has no flat-index API — and runtime inflow control remains available through
    `swmm_NODE_LATFLOW`.
  - **Treatment expressions** — the step loop evaluates a compiled-expression cache built at start,
    so `swmm_treatment_set`/`_clear` now recompile the edited (node, pollutant) cell via
    `SWMMEngine::refreshTreatment`; an edit/replace/clear takes effect on the next step and a failed
    parse is rejected (`BadParamError`) with the previous expression restored. Legacy parity needs
    dedicated functions because an expression cannot ride `setNodeValue`: `swmm_setTreatment` /
    `swmm_clearTreatment` re-use the `[TREATMENT]` input parser and free any prior `MathExpr` so
    runtime replaces do not leak. Python `Solver.set_treatment` / `clear_treatment`.
  - **LID layer parameters** — the four refactored LID setters were silent no-op stubs and now write
    `ctx.lid_controls.*` for real, under a split contract: surface/soil/storage are
    **pre-start-only** (they seed per-unit LID state at start; `LifecycleError` mid-run, physical
    bounds enforced) while the **drain** coefficients are runtime-editable
    (`SWMMEngine::refreshLIDDrainParams` re-copies the per-unit drain columns the step loop reads —
    the cistern/rain-barrel RTC knob). Legacy parity for the drain group: `lid_setDrainParams` in
    `lid.c` (input-file units matching `readDrainData`), exported as `swmm_setLidDrain` with a
    `Solver.set_lid_drain` binding. Known follow-up: the refactored LID module consumes raw input
    values without the unit conversion legacy applies via UCF.
  - **Aquifer parameters** — new in both engines. The flux coefficients (conductivity, conductivity
    slope, tension slope, upper-evap fraction, lower-evap depth, lower-loss coefficient) are
    runtime-editable — refactored `SWMMEngine::refreshAquiferParams` re-derives the groundwater
    solver's per-subcatchment flux columns on each edit, legacy reads `Aquifer[]` live — while the
    structural / initial-condition parameters (porosity, wilting point, field capacity,
    bottom/water-table elevation, upper moisture) seed GW state and are pre-start-only
    (`LifecycleError` / `ERR_API_IS_RUNNING`). Refactored `swmm_aquifer_get_param`/`_set_param` +
    `SWMM_AquiferParam`, binding `Aquifers.get_param`/`set_param` + `AquiferParam`; legacy
    `swmm_AquiferProperty` (800 block) through the existing `swmm_AQUIFER` object case, binding
    `SWMMAquiferProperties`.
  - **Infiltration parameters are pre-start-only** in both engines: the refactored setters
    (`swmm_subcatch_set_infil_horton`/`_green_ampt`/`_curve_number`) are guarded to the editable
    states by `CHECK_GEOMETRY` and raise `LifecycleError` while running, because the
    per-subcatchment infiltration state is built once at `start()`.
  - **Monthly climate adjustment arrays get no new setter:** they are covered at runtime by the
    climate forcing channels above, and the per-subcatchment N-PERV/DSTORE/INFIL adjustment patterns
    are retunable mid-run through the pattern-factor setter.
- **GUI-editor round-trip APIs** — getters to match the existing setters, so a property or category
  editor can *load* an existing definition rather than only write one:
  - Pollutant: `swmm_pollutant_set_units` (inverse of the existing `_get_units`; pre-start-only).
  - Aquifer: `swmm_aquifer_get_evap_pattern` / `_set_evap_pattern` — the one string column
    (`[ETupat]`) the param-code API did not cover.
  - Snowpack (previously add/list-only): `swmm_snowpack_set/get_plowable`, `_impervious`,
    `_pervious` (the seven `[SNOWPACKS]` surface values), `_set/get_removal` (six values) and
    `_set/get_removal_subcatch`.
  - Inlet: `swmm_inlet_get_params` (inverse of `_set_params`) and `swmm_inlet_get_type`.
  - LID: `swmm_lid_get_surface` / `_soil` / `_storage` / `_drain` (inverse of the four layer
    setters), `swmm_lid_get_type`, and full set/get for the remaining two layers —
    `swmm_lid_set/get_pavement` (6 values: thick, void-ratio, frac-imperv, ksat, clog-factor,
    regen-days) and `swmm_lid_set/get_drainmat` (3 values: thick, void-frac, roughness) — so
    PERM_PAVEMENT and GREEN_ROOF controls round-trip every layer. All six LID layers are now covered
    end-to-end.
  - Python bindings for all of the above (`Pollutant.units` setter, `Aquifers.get/set_evap_pattern`,
    `Snowpacks.set/get_*`, `Inlets.get_params/get_type`, `LIDs.get_surface/soil/storage/drain/type` +
    `set/get_pavement` + `set/get_drainmat`) with `.pyi` stubs.
- **Portable Kokkos GPU surface solver + HIP/SYCL plugins (2D).** A performance-portable GPU path for
  the 2D surface router: CVODE control flow, tolerances and operator-splitting are unchanged — only
  the `N_Vector` and RHS move to Kokkos, numerically equivalent to the serial CPU reference. New
  `ISurfaceSolver` interface with `SurfaceSolverFactory` resolving the backend at runtime (dlopen a
  GPU plugin, else fall back to the serial `CvodeSurfaceSolver`; `OPENSWMM_2D_BACKEND` override, auto
  order cuda → hip → sycl → omp → serial). The GPU plugin is opt-in
  (`OPENSWMM_BUILD_GPU_PLUGIN`) behind a stable C ABI (`GpuPluginAbi.h`); the base build stays
  Kokkos-free. New vcpkg `gpu`/`gpu-cuda`/`gpu-hip`/`gpu-sycl` features. Strategy:
  `docs/2D_GPU_PORTABLE_CVODE_STRATEGY.md`.
- **Capped-pipe 1D/2D junction coupling model.** Replaces the two-regime free-inlet/surcharge blend
  with a bidirectional gradient orifice (`h_2d - h_1d`) gated by a C1 Hermite ramp at the pipe crown:
  below the crown the node pressurizes internally over its auto-sized ponded shaft with no exchange;
  the cover only connects the domains once water overtops it. `sur_depth` now solely sizes the 1D
  Preissmann-slot headroom above the crown. Doc: `docs/1D_2D_COUPLING_CONFIGURATION.md`.
- **Time-based 2D coupling window.** New `COUPLING_WINDOW` (s) in `[2D_OPTIONS]` (-1 AUTO / 0 every
  step / >0 s; env `OPENSWMM_2D_COUPLING_WINDOW`; INP + GeoPackage round-trip) decouples the 2D
  advance cadence from 1D variable-step shrinkage. Adds a quiescence short-circuit (windows with no
  water/rain/coupling sources and only WALL/NORMAL_FLOW boundaries skip the CVODE advance entirely)
  and a stencil-scoped CFL hint so a rain-wetted cell far from the network no longer pins the 1D
  routing step. Bellinge 3h smoke: 15.2 s → 9.2 s (omp).
- **Dry-cell active-set masking (wet-front tracking), opt-in.** Restricts every 2D RHS pipeline stage
  to `active = wet ∪ sourced ∪ halo` while the CVODE system stays full-size (frozen cells get
  `ydot = 0`, so the BDF history is never invalidated). Opt-in via `[2D_OPTIONS] ACTIVE_SET` (env
  `OPENSWMM_2D_ACTIVE_SET`); wall-guarded (mask errors are locally conservative, never a leak) with a
  breach-redo/halo-doubling safety net. `SurfaceStateData` gained an `active_set` pointer — GPU plugin
  ABI bumped to v2.
- **Python bindings for the user-flag schema C API** (`swmm_userflag_define` / `undefine` /
  `def_count` / `def_get` / `value_get` / `value_set` / `value_clear`) — the last unbound block of
  the 702-function engine surface. `ModelBuilder` gains `define_userflag`, `undefine_userflag`,
  `userflag_def_count`, `get_userflag_def` and `get/set/clear_userflag_value`; the `solver.userflags`
  view gains `define()`, `undefine()`, `definitions()` (returning `UserFlagDef` records),
  `get_value()` / `set_value()` / `clear_value()`, real `len()` / iteration over definitions, and
  STRING-flag support in the mapping interface. New `UserFlagType` enum (BOOLEAN / INTEGER / REAL /
  STRING).
- **Python bindings for 2D vertex coupling CD/AREA** — `Surface2D.get/set_vertex_coupling_cd` and
  `get/set_vertex_coupling_area` wrap `swmm_2d_get/set_vertex_coupling_cd` / `_area` (the
  `[2D_VERTEX_NODE_MAP]` CD and AREA columns) — plus a lazy `Solver.surface2d` property returning the
  cached `Surface2D` view, so 2D access no longer requires constructing `Surface2D(solver.handle)` by
  hand.
- **Python GeoPackage model export** — `Solver.write_with_plugin(path, output_plugin_id)` and the
  convenience `Solver.write_geopackage(path, crs=...)`, so a loaded model can be exported to a
  `.gpkg` from Python (the C API already had `swmm_model_write_with_plugin`; only `ModelBuilder`
  wrapped it before). `write_geopackage(crs="EPSG:2284")` applies the CRS via `solver.spatial.crs`
  first, so every feature layer is tagged with that SRS — without a CRS the geometries get an
  undefined SRS (`srs_id 0`) and GIS tools cannot place them. Added the `GEOPACKAGE_PLUGIN_ID`
  constant (the real id is `org.hydrocouple.openswmm.plugins.geopackage`; corrected the stale example
  in `openswmm_model.h` that omitted `.plugins.`).
- **DateTime conversion C API** — new `include/openswmm/engine/openswmm_datetime.h` exposes
  encode/decode/`add_seconds`/`time_diff` primitives matching the legacy `datetime.c` bit-for-bit,
  reached from Python through `openswmm.engine.datetime_api` plus the high-level
  `oadate_to_datetime` / `datetime_to_oadate` helpers. All "Julian date" wording is removed from the
  C API header documentation; the convention is documented as the OLE Automation / Delphi TDateTime
  epoch (1899-12-30) — **not** astronomical Julian.
- **New enums:** `OrificeType`, `WeirType`, `OutletRatingType` (in `openswmm_links.h`), and
  `ErrorCode.DEPENDENCY = 15` (was missing from the Python side).
- **Documentation for the v1 Python surface** — new `guide/datetime.rst` and `guide/plotting.rst`
  pages, `guide/concepts.rst`, `guide/error_handling.rst`, every domain guide and the migration page
  rewritten, and a v0 → v1 cheat sheet appended to `migration/swmm5_to_swmm6.rst`. The Sphinx CI
  gate (`sphinx-build -W --keep-going`) stays in place and every page renders warning-free against
  the new `.pyi` stubs.
- **A typing gate on the Python bindings** — `python/pyproject.toml` ships a `[tool.mypy]` block
  (default-mode check across the whole `openswmm.engine` package plus strict mode on the pure-Python
  `_enums`, `_exceptions`, `_dates`), `python/tests/typing/test_surface.py` exercises every public
  symbol with explicit annotations under strict mode, and a new `.github/workflows/typing.yml` runs
  both passes on every PR touching the bindings.
- **Legacy-parity trace tooling** — a per-model parity ladder plus a call-graph provenance matcher
  and gap-review report, for auditing refactored code against its legacy anchor.

### Changed

- **The Python binding surface is a property-style rewrite — breaking for Python callers; the C API
  is untouched.**
  - Lifecycle methods (`open`, `initialize`, `start`, `step`, `stride`, `end`, `report`, `close`)
    **raise on failure** instead of returning integer codes. `step()` and `stride()` return a
    `datetime.timedelta`, with `timedelta(0)` as the end-of-simulation sentinel. `Solver.state`
    returns the `EngineState` enum; `Solver.elapsed` and `Solver.routing_step` are `timedelta`; new
    `Solver.start_datetime`, `end_datetime`, `current_datetime`, `report_start_datetime` return
    `datetime.datetime`; new `Solver.steps()` iterator and `Solver.until(target)` (accepting
    `datetime` or `timedelta`). Every file argument accepts `pathlib.Path` / `os.PathLike`.
  - Views on the solver: `solver.options` is a `MutableMapping` over `[OPTIONS]` plus typed
    shortcuts (`start_datetime`, `routing_step`, …); `solver.userflags` a `MutableMapping` with
    auto-typed bool/int/float; `solver.events` a `MutableSequence[Event]` whose entries carry
    `datetime` `start`/`end`; `solver.save_schedule` a `MutableSequence[SaveScheduleEntry]` for the
    `[SAVE HOTSTART]` block.
  - Each `solver.<domain>` returns a collection **indexable by `int | str`**, iterable and
    `len`-able, whose items are typed wrapper objects with property-style access
    (`solver.nodes["J1"].depth = 1.2`, `solver.links["C1"].xsect = (XSectShape.CIRCULAR, 1.0, 0, 0,
    0)`, `solver.subcatchments["S1"].infiltration.set_horton(...)`, `solver.gages["RG1"].rainfall =
    25.4`, `solver.pollutants["TSS"].kdecay = 0.05`). Per-type sub-views raise `AttributeError` on
    the wrong node/link type — `node.outfall` only on OUTFALL, `node.storage` only on STORAGE,
    `link.pump` only on PUMP. Bulk numpy access is now a property pair
    (`solver.nodes.depths` / `solver.links.flows` / `solver.subcatchments.runoffs`).
  - `OutputReader` takes a path-agnostic constructor (`str` / `Path`) and exposes typed metadata —
    `start_datetime`, `report_step` (`timedelta`), `flow_units` (`FlowUnits`), `period_times`
    (`np.ndarray[datetime64[s]]`), `node_ids` / `link_ids` / `subcatchment_ids`. Variable-selector
    arguments require an enum (`OutNodeVar` etc.) while object selectors accept `int | str`;
    `node_attributes(key, period)` returns `Dict[OutNodeVar, float]` and `node_stats(key)` a typed
    view with `max_depth`, `max_overflow`, `vol_flooded`, `time_flooded`.
  - `solver.mass_balance.routing_diagnostics` returns the `RoutingDiagnostics` dataclass;
    `solver.statistics.<domain>_<stat>` are all bulk numpy properties; `HotStart.open(path)` is a
    classmethod and `HotStart.save_from(solver, path)` a static method, with `sim_datetime`
    (`datetime`), `warnings` (`list[str]`) and `apply(solver)` on the hot-start; `solver.tables`
    exposes `TimeSeries.points` as a structured numpy array `(time: datetime64[s], value: float64)`
    and `solver.patterns` is now a separate indexable collection.
  - New `EngineError` hierarchy in `openswmm.engine._exceptions`, where every subclass **also**
    inherits from a standard-library exception: `BadIndexError(IndexError)`,
    `BadParamError(ValueError)`, `LifecycleError(RuntimeError)`, `HotStartError(RuntimeError)`,
    `FileError(IOError)`, `ParseError(ValueError)`, `NumericalError(RuntimeError)`,
    `CRSError(ValueError)`, `DependencyError(RuntimeError)`, `BadHandleError(RuntimeError)`,
    `PluginError(RuntimeError)`, and
    `StaleObjectError(LifecycleError)` — raised when a wrapper's generation counter no longer matches
    the solver's after a rename or delete. Every `EngineError` carries `.code` (raw int),
    `.code_enum` (`ErrorCode` member) and `.message` (filled by the C API).
- **Node and link subtype data now live in normalized relational side-tables as the sole store.**
  The dense per-subtype tables (`StorageData`/`OutfallData`/`DividerData` in `NodeSubtypes.hpp`,
  `ConduitData`/pump/orifice/weir/outlet in `LinkSubtypes.hpp`, joined to the base object by
  `node_idx`/`link_idx`) hold all subtype configuration *and* mutable per-step state; the wide
  `storage_*`/`outfall_*`/`divider_*`/`exfil_*` arrays on `NodeData` and the conduit/pump/structure
  arrays on `LinkData` are deleted, along with the whole mirror machinery (build-from-wide,
  `verify_mirror`, `mark_dirty`, `ensure_fresh`). Parse, resolve, edit, compute, IO and the C API all
  read and write the side-tables directly, and the compiler enforces zero remaining wide-subtype
  references. `openswmm_nodes.h` and `openswmm_links.h` are byte-unchanged, and the cutover is gated
  on byte-identical `.out` files for the 18 `epaswmm5_qa` models in both CFS and CMS plus INP-save
  parity.
- **The on-disk GeoPackage node and link schemas are normalized to match.** Flat NULL-padded `nodes`
  and `links` tables become a slim base table (common columns + subtype discriminator + geometry)
  plus 1:1 child tables — `storages`/`outfalls`/`dividers` and
  `conduits`/`pumps`/`orifices`/`weirs`/`outlets` — keyed by `(simulation_id, node_id)` /
  `(simulation_id, link_id)` with a hard `FOREIGN KEY … ON DELETE/UPDATE CASCADE`. The child tables
  carry the full side-table field set (storage seep/evap/exfil, outfall `route_to`, divider
  cd/max_depth/curve/link), and the opaque `param1`/`param2` become named columns (orifice
  orientation `SIDE`/`BOTTOM`, `weir_type`, outlet `rating_type`); `xsect_*`/`has_flap_gate` stay on
  the link base table because conduit, orifice and weir all share them. **Breaking for existing
  `.gpkg` files:** a pre-relational file is rejected cleanly with an actionable error — no
  compatibility view, no crash and no silent misread. `PRAGMA foreign_key_check` is clean and the
  delete/rename cascades are verified.
- **`del solver.userflags[name]`** now removes the flag's schema definition and per-object values via
  `swmm_userflag_undefine` (previously raised `TypeError`), and assigning a `str` value auto-defines
  a STRING flag, mirroring the scalar setters.

### Fixed

- **Subcatchment runoff is bit-identical to legacy.** Three operand-order and constant defects: the
  Manning exponent `MEXP` used `5.0/3.0` where legacy `subcatch.c` carries the truncated literal
  `1.6666667` (a ~3.3e-8 exponent difference, ~2.3e-7 relative error in every `pow(xDepth, MEXP)`);
  the `VOLUME`/`CUMULATIVE` rain conversion formed the non-representable `1/12` constant first
  (`r/(interval/3600)` instead of legacy's `r/interval*3600`); and the Manning alpha reassociation
  `PHI*w*sqrt/(N*area)` was 1 ULP off legacy's `PHI*w/area*sqrt/N`. Every user-model subcatchment
  RUNOFF series is now byte-identical to legacy.
- **Dynamic-wave momentum matches legacy operand order.** `dq2`/`dq4`/`dq5`/`dqdh` divide by length
  directly rather than multiplying a precomputed reciprocal (`x/L != x*(1/L)`); `dqdh` groups its
  terms as legacy `dwflow.c:240` does, which matters because `dqdh` feeds the surcharge node-depth
  Jacobian; the normal-flow limit uses `pow(r1, 2./3.)` on the raw upstream hydraulic radius instead
  of `cbrt(r1*r1)` on a FUDGE-clamped value; and the Manning friction term drops a non-legacy
  `max(rWtd, FUDGE)` clamp. The dry-conduit kernel's `dqdh` was brought onto the same divide.
- **Metric (SI) input conversion divides by the unit factor.** Legacy computes
  `internal = display / UCF`; the refactored engine multiplied by a precomputed reciprocal. The two
  differ by up to 1 ULP, which is enough to break the exact flatness of an initial water surface —
  the momentum then emits a ~2e-15 flow that the under-relaxation sign-flip clamp amplifies to a
  spurious 0.001 cfs, seeding divergence. Applied to lengths, ponded area, functional-storage
  `a0`/`a1`, flow fields and seepage. One-time parse-path change; US models take an early return and
  are unaffected.
- **Conduit evaporation and seepage loss is recomputed every Picard iteration**, as legacy
  `dwflow_findConduitFlow` does, and DRY/UP_DRY/DN_DRY conduits are skipped rather than accruing a
  spurious loss. The refactored engine computed losses once per step for all conduits with no
  flow-class gate, which both leaked loss onto dry conduits and froze the rate for the whole step
  instead of tracking the iterate.
- **Structures, outfalls and `USE HOTSTART`.** Weirs never set the reported link depth; the legacy
  `weir_getInflow` depth is now set in every branch. Orifice surface-area scatter reproduces legacy
  `findNonConduitSurfArea` exactly, including on the dry and flap-gate early-exit paths that
  previously skipped it. Free-outfall boundary depth uses the connecting conduit's critical depth as
  legacy does, instead of `max(0, stage - invert)` — an outfall below its invert previously sat dry
  and under-conveyed its conduit.
- **Legacy-exact 1D reporting semantics**, plus an op-for-op `table_lookup`/`table_interpolate`
  cursor, storage-volume and pump-regime fixes, a parse-time-floored `TotalDuration` matching
  `swmm5.c:3198` (no `+0.5` rounding), and a closed orifice reporting frozen depth as legacy's
  `link_getInflow` early return does.
- **Transect elevation offset, `Xfactor` and UCF are applied**, making `IRREGULAR` cross-section
  tables bit-identical to legacy.
- **Storage depth Newton solve: index and unit bugs (#94).** Two defects in the branch used by
  `FUNCTIONAL` (nonlinear), `CONICAL` and `PYRAMIDAL` shapes. `TStorageVol` held the storage unit's
  `subIndex` but forwarded it to functions expecting a *node* index, so any storage node not sitting
  at the same `Node[]` position as its own `subIndex` — the normal case, once a junction, outfall or
  divider precedes it — read another node's `fullDepth`/`fullVolume` and `Storage[]` record. That
  affects US and SI projects alike. Separately, the Newton solve runs in display units but passed its
  trial depth straight into functions taking internal feet, then differenced the result against a
  display-unit target: silent under US units, wrong under SI.
- **`IGNORE_RAINFALL` / `IGNORE_SNOWMELT` / `IGNORE_GROUNDWATER` / `IGNORE_RDII` / `IGNORE_ROUTING` /
  `IGNORE_QUALITY` are honored at runtime**, matching legacy. They were parsed and stored but not
  consulted by the process dispatchers, so a model asking to skip a process still ran it.
- **Pollutant mass was lost when runoff fell below the cutoff (#90).** The mass-balance bookings are
  now made against the routed runoff rather than only the reported load, so a subcatchment whose
  reported runoff is zeroed by the cutoff no longer discards its washoff mass.
- **`TIMESERIES` / `TIDAL` outfalls read the wrong stage table (#92)** — the `[OUTFALLS]` handler
  read the stage-data name and discarded it, deferring resolution to a post-parse pass that was
  never written, so `OutfallData::param` kept its default of `0`. Curves and timeseries share one
  index space (`ctx.tables`) and `Outfall.cpp` guarded only with `>= 0`, so the outfall silently
  drew its stage from whichever table came first in the model — an unrelated shape curve, in the
  reported case, pinning the outfall at that curve's y-value with no error. `OutfallData` now carries
  `param_name` for deferred resolution (mirroring `DividerData::link_name`), `PostParseResolver`
  resolves it against `ctx.table_names` and type-checks the referent (a `TIMESERIES` outfall may not
  point at a curve), and an unresolvable name is a fatal `ERR_NAME` as in legacy
  `outfall_readParams()` rather than a silent misread. Unresolved references now use `-1`, not `0`.
- **`InpWriter` corrupted `[OUTFALLS]` on save** — the section was emitted as
  `Name Elev Type Gated [Stage]`, but the canonical (and parsed) order is
  `Name Elev Type StageData Gated RouteTo`. A `FIXED` outfall's stage was written *after* the gate
  flag and re-parsed as `0`; `TIDAL`/`TIMESERIES` names and `RouteTo` were never written at all. The
  writer now emits the canonical column order, resolving the table *name* for `TIDAL`/`TIMESERIES`,
  and the parser accepts the EPA-GUI `*` stage-data placeholder on `FREE`/`NORMAL` rows without
  shifting the gate column.
- **`InpWriter` dropped the `[POLLUTANTS]` `Cdwf` and `Cinit` columns** on save; both are now
  written.
- **The GeoPackage round trip was lossy in ten independent ways** (all uncovered while verifying
  the relational schema above, none of them subtype-specific): a gage SoA under-sizing crash
  (`grow_to` + store `ts_name`); object read ordering (SQLite returns UNIQUE-index order, not
  insertion order — every feature and data read now carries `ORDER BY fid`);
  `[INFLOWS]`/`[DWF]`/`[CONTROLS]`/`[TRANSECTS]` never persisted at all (tables + IO added);
  cross-sections storing derived rather than raw `[XSECTIONS]` geom1–4, which lost a TRAPEZOIDAL
  bottom width and side slopes; orifice/weir/outlet type dropped, so a `SIDE` orifice reloaded as
  `BOTTOM`; adverse-slope conduit `direction` dropped, which cannot be re-derived from the
  already-reversed positive-slope geometry; and the metric (CMS) unit round-trip, where the `.gpkg`
  is now a canonical **internal-unit** store (the reader sets `ctx.gpkg_units_internal` so
  `resolve_cross_references` skips the non-invertible display↔internal conversion, with only the raw
  link xsect geom1–4 converted in lock-step with the convert pass) — this is the root cause of the
  GUI's metric-save ×3.2808 inflation. Timeseries now store raw OADate and option dates/steps
  `%.17g`.
- **`[FILES]` `USE/SAVE RUNOFF` and `USE/SAVE RDII` now work in the refactored engine** — previously
  parsed and written back but never consumed. `SAVE RUNOFF` auto-opens the existing binary
  runoff-interface writer from the slot; `USE RUNOFF` replaces each runoff substep with the file's
  records (legacy `runoff_readFromFile`), driving the runoff clock from the recorded timesteps. New
  `RdiiInterfaceFile` implements the RDII slots: `SAVE RDII` exports computed flows in the legacy
  `SWMM5-RDII` binary format; `USE RDII` **bypasses the internal unit-hydrograph computation**
  (legacy `rdii_openRdii` semantics) and reads either the binary or the legacy text format with
  step-aligned (non-interpolated) lookup. Open/format failures fail `start()` with legacy errors
  323/325/343/345. `USE/SAVE RAINFALL` (collated binary rain file) remains unimplemented but now
  emits WARNING 103 instead of being silently ignored.
- **Routing interface files (`[FILES]` `USE INFLOWS` / `SAVE OUTFLOWS`) now work in the refactored
  engine** — the paths were parsed and stored but the `InterfaceManager` was never opened, so
  simulations silently ran without the upstream inflows (and wrote no outflows file).
  `swmm_engine_start()` now opens both files, reads/writes the legacy headers, and fails with legacy
  errors 351/353/355/357 on open/format problems. Outfall rows are written at reporting cadence
  (legacy `iface_saveOutletResults`), flows are converted to the declared units on write, interface
  pollutant loads flow into node quality mixing and the new "External Inflow" row of the
  quality-routing continuity report, and interface flow volume is booked as external inflow in the
  routing mass balance. Wrong-mode rows (`SAVE INFLOWS` / `USE OUTFLOWS`) are rejected at parse time
  (legacy `ERR_ITEMS`).
- **`[POLLUTANTS]` concentrations were silently zeroed:** `PostParseResolver` unconditionally re-ran
  `resize_pollutants()` (which zero-fills) *after* `handle_pollutants` had parsed the values, so
  every INP rain/GW/RDII/DWF/init concentration loaded as 0 — the DWF/GW/rain quality features did
  nothing for INP-driven models. The resize is now guarded like the adjacent node/link resizes
  (`if count != n`).
- **Refactored dry-weather-flow quality did not exist:** the `[POLLUTANTS]` `Cdwf` column was parsed
  and discarded. Added `PollutantData.c_dwf`, `QualitySolver::addDwfLoads()` (mirroring the RDII
  loads), a `qual_routing_dw_in` mass-balance bucket included in the continuity error, the report's
  Dry Weather Inflow row (previously hardcoded 0), and `swmm_pollutant_set/get_dwf_conc()`.
- **Groundwater inflow quality was never applied:** GW inflow pollutant mass (`q_gw × c_gw`) went
  nowhere. Added `QualitySolver::addGwLoads()` and a `qual_routing_gw_in` bucket; the report's
  Groundwater Inflow quality row (previously hardcoded 0) and the quality continuity total now
  include it.
- **Refactored snow never accumulated.** The `[SUBCATCHMENTS]` snow-pack column was never read (no
  deferred name resolution) and the snow solver's per-subarea `fArea` was never initialised, so
  `plowSnow`/melt treated every surface as zero-area; and `plowSnow()` was never called from the
  step pipeline at all, so packs could melt but never grow. Added `snowpack_name` deferred
  resolution and `fArea` init (legacy `snow_initSnowpack`); accumulation + plowing now run each
  runoff step before melt (matching legacy `runoff.c` order); and the snow solver takes
  per-subcatchment rain/snow inputs — previously a single area-weighted broadcast — with
  per-subcatchment rain-on-snow vs degree-day melt selection (matching legacy
  `snow_getSnowMelt`/`meltSnowpack`).
- **Legacy `apiSnowfall` continuity hole:** prescribed snowfall influenced melt computations but
  never accumulated in the pack (only gage snow did, via `snow_plowSnow`) while still counting as
  rainfall inflow in the runoff mass balance. `snow_plowSnow()` now includes `apiSnowfall`.
- **Refactored `swmm_subcatch_get_snow_depth()`** was a stub returning 0; it now returns the
  area-weighted pack SWE in user depth units via the new `SWMMEngine::subcatchSnowDepth()`.
- **Subcatchment rainfall forcing had no effect:** `applyForcings()` pre-wrote `subcatches.rainfall`,
  which the runoff solver then overwrote from the gage. `swmm_forcing_subcatch_rainfall()` now
  resolves inside the runoff solver's rainfall assembly, the same pattern as the PET forcing fix.
- **Subcatchment evaporation forcing had no effect:** `swmm_forcing_subcatch_evap()` overwrote
  `evap_loss` before the runoff solver recomputed it. It now prescribes a PET *rate* (user units:
  in/day US, mm/day SI — previously documented as ft/sec) consumed by the runoff, LID and
  groundwater solvers, so capping to available water and mass-balance accounting happen along the
  normal computation paths.
- **Climate temperature/wind forcing stuck after clear:** a one-shot or cleared prescription never
  reverted, because the forcing overwrote the same `ClimateState` field it read as the broadcast
  base. Added `temperature_src`/`wind_speed_src` source bases resolved fresh each step.
- **`Forcing.clear()` mapped the wrong channels:** the Python binding passed `ForcingTarget`
  object-kind codes where C `SWMM_ForcingType` channel codes were expected, so clearing a SUBCATCH
  actually cleared node quality. It now clears every channel belonging to the requested object.
- **`ForcingData::effective_rainfall`/`_snowfall` read out of bounds:** they lacked the size guard
  `effective_evap_rate` has, segfaulting direct-solver unit tests with unsized forcing arrays.
- **Link-quality forcing now sticks after node mixing**, and 2D forcing applied through the one-shot
  API is no longer overwritten by the next window.
- **Refactored DWF/external-inflow patterns ignored mid-run edits.** `InflowSolver::init` copies
  pattern factors into a per-step lookup cache, so `swmm_pattern_set_factors` (which mutates
  `ctx.patterns`) had no effect on DWF/external inflow mid-run (groundwater-evap patterns read the
  live context and were unaffected). Added `InflowSolver::refreshPatterns` +
  `SWMMEngine::inflowSolver()`; the setter now refreshes the cache so an edit takes effect on the
  next step.
- **2D mass-balance evaporation** moved from the 2D state mirror into `MassBalance2D::evap_out`,
  folded into `error()`, and surfaced in the report's 2D continuity block.
- **Legacy `get_value` misread valid negatives:** `swmm_getValueExpanded`'s return was validated by
  sign, so a sub-freezing air temperature or the −999 API-unset sentinel raised a spurious error. It
  now keys off the system ERROR_CODE and the API-error sentinel range.
- **Legacy subcatchment pollutant bindings** passed the pollutant index in the `sub_index` slot, but
  the C `getSubcatchValue`/`setSubcatchValue` pollutant cases read it from `pollutantIndex`.
  `LegacySubcatchment.get_pollutant_buildup`, `set_external_pollutant_buildup` and
  `set_ponded_concentration` now pass `pollutant_index=` and work at runtime (previously they raised
  an object-index API error).
- **Build and packaging:** `gpu-omp` wheels build again (Kokkos/OpenMP, macOS `delocate`); the macOS
  wheel deployment target is 11.0 rather than 15.0; the musllinux leg re-clones vcpkg per libc; the
  wheel-smoke job installs the local companion wheel instead of the PyPI `0.0.0` stub; and
  `PatchNumpyPxd.cmake` only rewrites the Cython pxd on numpy ≥ 2.0, so it no longer breaks the
  numpy 1.x build it was meant to support.

### Performance

- **1D dynamic-wave Picard loop.** Persistent-team OpenMP threading (the team spans the whole
  iteration loop; a CSR node-centric gather replaces the serial link-order scatter, provably
  bit-identical FP order to legacy at any thread count), batch-geometry kernels parallelized with an
  Apple-Silicon P-core clamp/QoS hint, and dead-work elimination (loss recompute / node-state init
  skipped when structurally a no-op). Bellinge: 204 s → 56.5 s (T=8, Apple Silicon); the routing
  loop goes 95.8 s → 80.1 s in a separate bit-parity-preserving optimization pass with
  phase-timing attribution and bypass-aware batch geometry. Every change is gated on a 20-model
  bit-parity scorecard with byte-identical `.out` files; the two 1D modes — bit-exact legacy parity
  versus fast — are documented.
- **Memory density from the relational side-tables:** ≈17% peak-RSS reduction on a 60k-junction
  model, and `LinkData` allocates ~29 fewer vectors per link.
- **2D held-path coupling:** zero-exchange coupling stencils are skipped in the active set (the
  exchange is a per-window constant already scattered into `coupling_flux`, so a stencil with zero
  flux this window contributes nothing) — the storm-peak active set drops from ~38% of the mesh to
  roughly wet+halo (~5%) on held-path coupled models.

## [6.0.0-alpha.1] — 2026-03-25

### Added

#### New Engine Architecture
- **Data-oriented engine** — Refactored core data structures to Structure of Arrays (SoA) layout for cache efficiency and SIMD-friendly computation.
- **Reentrant design** — All simulation state encapsulated in an opaque `SWMM_Engine` handle, eliminating global state and enabling multiple independent simulations per process.
- **Plugin-based I/O** — Output and report writing abstracted through a plugin interface with a dedicated I/O thread and double-buffered snapshots.
- **Engine lifecycle state machine** — Explicit states: CREATED → OPENED → INITIALIZED → STARTED → RUNNING → ENDED → CLOSED.

#### Comprehensive C API (19 headers)
- `openswmm_engine.h` — Engine lifecycle, error codes, state machine.
- `openswmm_model.h` — Model building, validation, serialization, options.
- `openswmm_nodes.h` — Junctions, outfalls, storage nodes, dividers.
- `openswmm_links.h` — Conduits, pumps, orifices, weirs, outlets with 20 cross-section shapes.
- `openswmm_subcatchments.h` — Subcatchments, infiltration (Horton/Green-Ampt/Curve Number), landuse coverage.
- `openswmm_gages.h` — Rain gages with timeseries and file data sources.
- `openswmm_pollutants.h` — Pollutant definitions and runtime quality injection.
- `openswmm_tables.h` — Time series, curves, patterns, and cursor-optimized lookups.
- `openswmm_inflows.h` — External inflows, dry weather flow, RDII.
- `openswmm_controls.h` — Control rule expressions and direct link setting/status actions.
- `openswmm_infrastructure.h` — Transects, streets, inlets, LID controls and LID usage.
- `openswmm_spatial.h` — CRS, coordinates, polylines, polygons for all object types.
- `openswmm_quality.h` — Landuse, buildup/washoff functions, treatment expressions.
- `openswmm_massbalance.h` — Continuity errors and cumulative flux totals.
- `openswmm_callbacks.h` — Progress, warning, step-begin/end, plugin state, and hot-start-missing callbacks.
- `openswmm_hotstart.h` — Hot start file save/load/modify/query with workflow examples.
- `openswmm_statistics.h` — Node, link, and subcatchment simulation statistics.
- `openswmm_engine_export.h` — Auto-generated shared library export macros.

#### Features
- **Hot start API** — Save, open, modify, query, and close hot start files through a transparent C ABI.
- **CRS support** — Coordinate reference system specification via OPTIONS section.
- **User flags** — Custom USER_FLAGS section for user-defined metadata on objects.
- **Plugin SDK** — Header-only development kit for building output/report plugins.
- **HEC-22 inlet analysis** — Street inlet capture with grate, curb, slotted, and custom inlet types (from SWMM 5.2).
- **Variable speed pumps** — Type5 pump curves with speed scaling.
- **New storage shapes** — Conical and pyramidal shapes with elliptical/rectangular bases.
- **Python bindings** — Cython-based bindings with solver context manager, iterative stepping, and output reading.

#### Testing & CI
- **Google Test migration** — All unit tests converted from Boost.Test to Google Test 1.15.2.
- **Comprehensive test suite** — 73+ legacy engine tests, 41 legacy output tests, and new engine unit tests.
- **Reorganized test structure** — `tests/unit/legacy/{engine,output}` and `tests/unit/{engine,output}`.
- **Multi-platform CI** — GitHub Actions for Windows x64, Linux x64, macOS x64, and macOS ARM64.
- **Performance benchmarks** — Google Benchmark integration for critical-path profiling.

#### Documentation
- **Doxygen API documentation** — All 19 public C API headers thoroughly documented with `@brief`, `@details`, `@param`, `@returns`, `@see`, and `@note` tags.
- **Technical reference manuals** — Hydrology, Hydraulics, and Water Quality reference manuals updated for OpenSWMM.
- **User manual** — Comprehensive user manual with modeling capabilities, typical applications, and input/output descriptions.
- **Author/license metadata** — All new engine source files annotated with `@author`, `@copyright`, and `@license` Doxygen tags.

### Changed

- **Project renamed** from `OpenSWMMCore` to `openswmm` with `openswmm.engine` as the primary library output name.
- **CMake minimum version** raised to 3.21 (from 3.15).
- **C++ standard** set to C++20 (from C++11/14).
- **C standard** set to C17.
- **CMake options** namespaced to `OPENSWMM_*` prefix (legacy `OPENSWMMCORE_*` aliases preserved).
- **Version scheme** updated to SemVer 2.0.0 with pre-release tags.
- **vcpkg** adopted as the dependency manager (replacing NuGet-based Boost distribution).
- **CI/CD pipelines** cleaned up: updated to `actions/checkout@v4`, `actions/setup-python@v5`, `actions/upload-artifact@v4`; removed stale branch triggers; fixed CMake flag from `-DBUILD_TESTS=ON` to `-DOPENSWMM_BUILD_TESTS=ON`.

### Removed

- **Boost.Test dependency** — Replaced entirely by Google Test.
- **NuGet package dependency** — Regression testing no longer requires external NuGet-hosted Boost packages.
- **Global state** — Eliminated from the new engine (legacy solver globals preserved in `src/legacy/`).

### Fixed

- **CI CMake flag** — Unit testing workflow was passing `-DBUILD_TESTS=ON` which did not match the actual `OPENSWMM_BUILD_TESTS` option, preventing tests from being built in CI.
- **Documentation workflow** — Removed stale `bug_fixes` branch trigger; updated to `actions/checkout@v4`.
- **Export header** — Fixed misplaced `@author`/`@copyright` block that was injected inside a `#define` preprocessor directive in `openswmm_engine_export.h`.

## Legacy EPA SWMM 5 engine history

The sections below cover the EPA-maintained `src/legacy` engine (`swmm5.c` /
`runoff.c` / `dynwave.c` / etc.), reconstructed from EPA's official update
notes (`epaswmm5_updates.txt`). Only **Engine Updates** are listed — the
original notes also describe Windows-GUI-only changes (EPA's separate
Delphi `epaswmm5.exe`), which are out of scope for this engine repository.
GUI-facing engine capabilities added here (e.g. Streets/Inlets, LID
practices) were later re-implemented for the OpenSWMM GUI/MVC layer in
`openswmm.gui`.

Builds `v5.0.22` through `v5.2.4` have matching `git` tags in this
repository. Builds `5.0.001`–`5.0.021` (SWMM 5's original 2004–2010
release run) predate this repo's tag history — no `v*` tag exists for
them here — and are listed below without version brackets for that
reason, oldest first.

## [5.2.4] — 2023-07-15

### Fixed

- Mismatch between reported pollutant Surface Runoff mass and conveyance
  system Wet Weather Inflow mass in a run's Status Report.
- Invalid-input-data test for an LID unit with an underdrain.
- Water-flux-rate calculations between layers in Bio-Retention, Permeable
  Pavement, and Infiltration Trench LID units.
- Hydraulic head seen by a storage-layer underdrain in a Permeable Pavement
  LID with a soil layer above it.
- Retrieval of the backing parameters for a Street cross-section.
- Generation of transect points for a Street cross-section with a
  depressed gutter, and the gutter-slope calculation for depressed-gutter
  Street links.
- Effective hydraulic head seen within a curb inlet with an inclined
  throat opening.

### Changed

- Conduit evaporation/seepage loss per time step is now limited to the
  conduit's current volume (was its flow rate) under dynamic wave routing,
  and is split evenly between both end nodes (was upstream node only).
- Default Inertial Damping and Variable Time Step option values now match
  the GUI's defaults.

## [5.2.3] — 2023-02-12

### Fixed

- Double counting of initial moisture volume in the drainage-mat layer of
  a green roof LID unit.

## [5.2.2] — 2022-12-01

### Added

- Dimension check for the Modified Basket Handle and Round-Rectangular
  cross sections (rounded-portion height cannot exceed total height).
- Additional performance statistics in the Street Flow Summary table.

### Changed

- Default number of dynamic-wave routing threads changed to 1, matching
  the User's Manual and the GUI.

### Fixed

- Long run times when the simulation duration exceeded the end of an
  externally applied time series.
- A bug (introduced in 5.2.0) causing the math-expression evaluator to
  compute `a*b^c` as `(a*b)^c` instead of `a*(b^c)`.
- Storage-unit evaporation/exfiltration loss reported as a percentage of
  total storage volume.
- Warning messages about raising a node's max depth and adjusting a
  conduit's elevation drop (removed in 5.2.1) restored.

## [5.2.1] — 2022-08-01

### Changed

- Use of the Normal Flow Limited feature for dynamic wave routing is now
  optional.
- For kinematic-wave storage routing, the reported depth after
  convergence is based on the last volume value rather than the next
  trial depth.
- Reduced excessive Status Report warnings: no message when a node's max
  depth is raised to the crown of the highest connecting conduit, or when
  a conduit's elevation drop/slope is adjusted to a minimum allowed value.

### Fixed

- A refactoring bug causing excessive execution times for projects with
  control rules.
- Egg-shaped cross-section geometry tables at the two lowest relative
  depth levels.
- Dry nodes no longer have their pollutant concentration forced to 0 when
  receiving non-zero pollutant inflow (a 5.2.0 regression); a non-storage
  node with no inflow now keeps its water-quality concentration unchanged
  instead of being zeroed.
- `F_OFF` definition in `output.c` for non-MS C/C++ compilers.

## [5.2.0] — 2021-11-01

Last EPA-maintained release before this project's refactor.

### Added

- **Street runoff capture by inlet drains** — new Street cross-section
  type (`[STREETS]`), Inlet object (`[INLETS]`), and conduit `[INLET_USAGE]`
  placement; HEC-22 (or custom capture-curve) inlet capture analysis
  interfaced with flow routing; new Street Summary table (peak flow depth
  and spread per Street conduit/Inlet).
- Type 5 variable-speed pump obeying the pump affinity laws (head/flow vs.
  speed).
- Pre-defined analytical Storage Curve shapes: cylinders, paraboloids,
  cones, pyramids.
- New control-rule condition-clause quantities, including past n-hour
  rainfall; condition clauses can now include named variables and math
  expressions.
- Listing of nodes with the highest flow-routing non-convergence frequency
  in the Status Report.
- Support for the latest NOAA Climate Data Online GHCN service (US or SI
  units).
- Additional validation check on the user-supplied Green-Ampt Initial
  Deficit value.
- New Rain Barrel LID parameter: covered or not.
- Command-line executable now supports binary output files larger than
  2 GB; number of open files increased to 8192.
- A number of new functions added to the SWMM 5 API.

### Changed

- Permeable Pavement LID effective permeability now accounts for the
  Impervious Surface Fraction parameter.
- Permeable Pavement LID depth values in the detailed report are now
  expressed in inches/mm (was feet).
- Math-expression parser now allows exponents to be expressions, not just
  constants.
- Time-step-average reporting option's average-flow computation changed.
- Shell sort replaces insertion sort for sorting event periods.

### Fixed

- Conversion of runon flow into an equivalent ponded depth for Curve
  Number infiltration.
- Total reporting time value used in several summary-table statistics.

## [5.1.15] — 2020-05-01

### Added

- A mix of infiltration methods can now be used within a single project.
- Status Report grouped frequency table of variable routing time steps
  used during a simulation.
- Fatal error now issued if a storage node's area curve produces a
  negative volume when extrapolated to the node's full depth.

### Fixed

- Average summary statistics for a reporting start date later than the
  simulation start date.
- Pollutant mass-balance error when very shallow storage units lost all
  inflow to flooding.

## [5.1.14] — 2020-03-01

### Fixed

- A refactoring bug producing incorrect rainfall when the same time
  series was used by an RDII-Unit-Hydrograph rain gage and another
  subcatchment gage.
- Skipping the first rain gage in a project when checking for duplicate
  station IDs with different data files.
- A crash running projects with LID units but no subcatchments.
- LID underdrain pollutant loads incorrectly added to mass-balance totals.
- The program hanging when an LID unit sent outflow back onto the
  pervious area of its own subcatchment.
- Failure to re-initialize layer volumes for each LID unit evaluated.
- Street sweeping being ignored when the sweeping period began with a
  higher day-of-year than its end.
- Incorrect adjustments for conduit evaporation/seepage losses under
  dynamic wave routing.
- Soil-moisture-deficit recovery being ignored for Green-Ampt
  exfiltration from storage units.
- Node/link ID names mistaken for option keywords in the `[REPORT]`
  section.
- A possible crash when reporting average (vs. point) values within each
  reporting time interval.

## [5.1.13] — 2018-05-10

### Added

- Monthly time patterns for a subcatchment's depression-storage depth,
  pervious Manning's n, and hydraulic conductivity.
- LID controls can now treat a designated portion of a subcatchment's
  pervious-area runoff (previously impervious-only).
- Permeable pavement LIDs subject to clogging can have permeability partly
  restored at periodic intervals.
- LID underdrain flow-control options: auto-open/auto-close depth
  thresholds and a head-based control curve for nominal drain flow.
- Pollutant removal percentages assignable to LID underdrain processes.
- Subcatchment Runoff Summary Report now includes pre-LID pervious and
  impervious total runoff volumes.
- Choice of dynamic-wave surcharge method: traditional EXTRAN Surcharge
  Algorithm, or a new SLOT (Preissmann Slot) option for closed conduits
  flowing >98.5% full.
- Storage-unit node can model a closed/pressurized vessel via a Surcharge
  Depth value.
- Weir discharge coefficient can vary with head via a Weir Curve.
- Periodic time step option for control-rule evaluation.
- Option to report node/link time-series results as reporting-step
  averages instead of interpolated point values.

### Changed

- A regulator link's upstream offset below its downstream node's invert
  is now auto-raised only under dynamic wave routing (with a warning);
  other routing methods only warn.

### Fixed

- Unused rain gages no longer examined when adjusting the wet-runoff time
  step.
- Permeable-pavement LID surface inflow rate capped at the pavement's
  permeability.
- Minimum Nodal Surface Area dynamic-wave option now applied only when a
  node's connecting-link surface area falls below it (was always-available
  surface area).
- Full closed rectangular cross-section top width now set to 0.
- Mitered Corrugated Metal Arch culvert "C" parameter value corrected.
- Flow-continuity-error reporting for systems with backflow through
  outfall nodes.

## [5.1.12] — 2017-03-14

### Changed

- `direct.h` now only `#include`d when compiled for Windows.
- Redacted the 5.1.011 update that internally aligned the wet time step
  with the reporting time step — it caused problems for certain time-step
  combinations.
- Subcatchment's bottom elevation (not its parent aquifer's) now used
  when saving a water-table value to the binary results file.
- Conduit seepage-rate conversion (per-area → per-length) now uses top
  width instead of wetted perimeter (only vertical seepage is assumed).
- Crest-length reductions for end contractions no longer used for
  trapezoidal weirs.
- `NO`/`YES` no longer accepted as `NORMAL_FLOW_LIMITED` attributes (only
  `SLOPE`/`FROUDE`/`BOTH`).
- User-supplied minimum-slope option now initialized to 0.0 (none).
- Routing Events and Skip Steady Flow options now work correctly
  together; steady-state periods with no flow routing no longer skew
  routing-time-step statistics.
- MS exception-handling statements now only enabled for the Microsoft C
  compiler.

### Fixed

- Failure to limit surface infiltration into a saturated rain-garden LID
  unit.
- Maximum-limit calculation on LID drain flows, for smoother results at
  low depths above the drain offset.
- A variable used for detailed LID reporting is now properly initialized.
- Occasional duplicate lines written to the detailed LID results file.
- Coefficient of the evaporation/seepage term in the dynamic-wave flow
  equation (corrected 1.5 → 2.5).
- Engels flow equation for side-flow weirs (incorrect since SWMM 3/4).
- Slope Correction Factor for culverts with mitered inlets.
- An entry in the gravel-roadway weir-coefficient table.
- Number of barrels now accounted for when compiling full-conduit-flow
  frequency statistics.
- Water level in storage nodes with no outflow links under kinematic
  wave/steady flow routing.
- Depth-at-max-width formula for the Modified Basket Handle cross
  section.

## [5.1.11] — 2016-08-22

### Added

- Detailed flow routing can be restricted to pre-defined event periods
  (`[EVENTS]` section: start/end date and time).
- New API functions `swmm_getError()` and `swmm_getWarnings()`.
- Recognizes the new NCDC Climate Data Online precipitation file format.
- Check that subcatchment imperviousness does not exceed 100%.
- Rule premises can include `SIMULATION DAYOFYEAR`.

### Changed

- Error codes returned by the API functions (`swmm_open`, `swmm_start`,
  `swmm_step`, etc.) corrected.
- Runoff time steps adjusted to stay aligned with the Report time step.
- LID native-soil infiltration now satisfied first when it occurs
  alongside underdrain flow.
- LID underdrain offset no longer limited to the top of the storage layer
  (allows upturned drains).
- Detailed LID report file now lists results by date/time and elapsed
  hours, and reports water level (not moisture content) for permeable
  pavement.
- A regulator link opening below its downstream node invert is now
  auto-raised to invert level (with a warning, was warning-only).
- Node surcharging now only reported for dynamic wave routing; storage
  nodes are never classified as surcharged.
- Status Report no longer lists modulated-control actions (continuous,
  produced an enormous number of entries).

### Fixed

- Monthly conductivity adjustments now also applied to the internal
  Green-Ampt "Lu" parameter.
- Time-step correction for outfall outflow returned to a subcatchment.
- A weir with an open rectangular shape and non-zero slope no longer
  raises a spurious input error (slope is ignored).
- Illegal array-index bug checking the pump-curve type for an Ideal Pump
  under dynamic wave routing.
- Redundant unit conversion of max. reported depth in the Node Depth
  Summary table.
- Storage unit surface-area-curve metric→internal conversion for bottom
  exfiltration.
- A bug resetting a link's `TIMEOPEN` control-rule variable when its
  setting changed between partly-open states.
- Roadway Weir road-width metric-unit conversion.
- Saved link settings read from a hot-start file, for models containing
  pollutants.
- A refactoring bug affecting water-quality mass-balance results for
  Steady Flow routing.
- Date/time fractional-part decoding could round to 24:00:00.

## [5.1.10] — 2015-08-05

### Added

- Modified Green-Ampt infiltration option (no upper-zone moisture-deficit
  redistribution during low-rainfall events; more infiltration for storms
  beginning with low intensity, e.g. SCS design storms).
- ROADWAY weir type (FHWA HDS-5 overtopping method), typically used in
  parallel with a culvert conduit.
- Rule premises can test whether a link has been open/closed for a
  specific duration.
- Unsaturated hydraulic conductivity ("K") usable in custom groundwater
  flow equations.
- Daily potential evapotranspiration (PET) added as a system output
  variable.

### Changed

- Hargreaves evaporation formula now uses a 7-day running average of
  daily temperatures (was single-day values).
- `qualrout.c` refactored to be more compact.
- Storage seepage/evaporation losses now based on end-, not start-,
  of-prior-time-step storage volume.

### Fixed

- A 5.1.008 regression that excluded LID infiltration from the
  groundwater routine.
- Failure to properly initialize the "initially wet" LID flag.
- Duplicate printing of the first line of an LID detailed report file.
- `makefile` for the GNU C/C++ compiler now correctly links the OpenMP
  libraries.

## [5.1.9] — 2015-04-30

### Added

- New warning for a control-rule premise comparing two different
  variable types.

### Fixed

- A refactoring bug preventing simulations longer than 68 years.
- Input-parsing error preventing recognition of a two-variable comparison
  in a control-rule premise.
- Runon to a subcatchment fully occupied by LIDs missing from its Summary
  Report (5.1.008 update 12 regression).
- LID units returning outflow to a subcatchment's pervious area even when
  LIDs occupied the entire subcatchment.
- Units label for Total Inflow Volume in the Node Inflow Summary table.

### Changed

- Dry conduit/storage-node definition for quality routing changed to
  ≤1 mm depth (avoids concentrations blowing up from evaporation losses).

## [5.1.8] — 2015-04-02

### Added

- Monthly adjustment patterns for hydraulic conductivity (rainfall
  infiltration and storage/conduit exfiltration).
- LID drains can send outflow to a different node/subcatchment than their
  parent subcatchment.
- Outfall nodes can send outflow onto a subcatchment (irrigation / complex
  LID treatment).
- New Rooftop Disconnection LID practice, with an optional downspout
  flow-capacity limit.
- Optional soil layer for Permeable Pavement LIDs (sand filter/bedding).
- New groundwater-equation variables: porosity, unsaturated hydraulic
  conductivity, infiltration rate, percolation rate; new Groundwater
  Summary table.
- Minimum Variable Time Step option for dynamic wave routing (down to
  0.001 s, was fixed at 0.5 s).
- Dynamic-wave routing parallelized across multiple processors (new
  `THREADS` option, default 1).
- Node Depth Summary column for max depth at the Reporting Time Step.
- Control-rule premises can compare a node/link variable's value at two
  different locations (e.g. `NODE 123 HEAD > NODE 456 HEAD`); node volume
  added as a condition variable.

### Changed

- LID runon from another source is now distributed across the
  subcatchment's non-LID area only (unless a single LID occupies the full
  area).
- Non-zero-runoff reporting threshold changed from 0.001 cfs to
  0.001 in/hr.
- Overall flow-routing mass-balance calculation now accounts for negative
  flow streams (e.g. total external inflow).
- Report labels renamed: "Surface Runoff" → "Total Runoff" (Runoff
  Continuity), "Internal Outflow" → "Flooding Losses" (Flow Routing
  Continuity).
- Pollutant washoff routines moved to a new module (`surfqual.c`),
  revised to account for LID runoff reduction.
- Steady Flow routing's initial flows are now ignored (removes their
  mass-balance contribution).
- Lateral inflows to conveyance nodes now evaluated at the start (was end)
  of the routing time step.
- Final runoff/routing time steps adjusted so total simulation duration
  is not exceeded.
- Storage node HRT added to Hot Start file state.

### Fixed

- Evaporation rates read from a time series only updated on new days,
  occasionally stopping a run prematurely.
- Hot Start runoff value assigned to the wrong internal property
  (`newRunoff` vs. `oldRunoff`).
- Indexing bug reading Hot Start files with snowmelt parameters.
- Non-conduit link setting from a Hot Start file not used to initialize
  the link.
- Snowmelt adjustment for snow-covered area derived from an areal
  depletion curve.
- Snowmelt double-counted in total subcatchment precipitation.
- Green Roof LID drainage-mat flow calculation applied void ratio to
  depth instead of area.
- LID wet/dry runoff time-step choice ignored LID unit state, causing
  excessive LID continuity errors.
- Refactoring bug leaving LID detailed-report time in minutes instead of
  hours; results now written at each runoff step where LID state changes.
- Groundwater evaporation loss not initialized to 0 for subcatchments
  with no pervious area.
- Excessive continuity errors for systems with high conduit seepage
  rates.
- Pollutant loss through conduit/storage-node seepage not included in
  mass balance.
- Conduit/storage-node concentrations not increased to account for
  evaporation volume loss.
- Capacity-limited-links check exiting prematurely on a non-conduit link.
- Bug identifying the percent of time a conduit has either end full.
- A refactoring bug that prevented surcharged weirs (5.1.007) from
  passing any flow.
- Bug evaluating recursive nodal water-quality treatment function calls.

## [5.1.7] — 2014-09-15

### Added

- Monthly adjustments for temperature, evaporation rate, and rainfall.
- Support for reading the new GHCN-Daily climate data files (NCDC Climate
  Data Online).
- Custom equation support extended to deep-groundwater-aquifer seepage
  flow (previously lateral flow only); `[GW_FLOW]` renamed to `[GWF]` with
  a format change to accommodate both.
- New Weir parameter: whether the weir can surcharge via an orifice
  equation.
- Storage-unit seepage can now use Green-Ampt infiltration (head-dependent
  seepage rate); constant-rate option remains via a zero initial moisture
  deficit.

### Changed

- Modified Horton method's dry-period infiltration-capacity-recovery
  formula revised.
- Green-Ampt infiltration functions refactored for clarity.
- Most LID simulation routines modified for more accurate results under
  flooded conditions; detailed LID results now always correspond to a
  full reporting time step.

### Fixed

- Green-Ampt initial cumulative infiltration into the upper soil zone was
  incorrectly set to the maximum value instead of zero.
- Infiltration out of the bottom of a Bio-Retention Cell or Permeable
  Pavement LID with a zero-depth storage layer.
- Groundwater flow-equation variable name for receiving channel bottom
  height corrected to match the GUI (`Hcb`).
- Crash when a climate file supplied evaporation rates with no
  subcatchments in the project.
- Flow/pollutant routing mass-balance accounting for negative external
  inflows.
- Area-available-for-seepage calculation for a storage node with a
  tabular storage curve.
- Depth-from-volume function for a storage curve where depth falls within
  a constant-area (vertical-wall) section.

## [5.1.6] — 2014-05-19

### Fixed

- Off-by-one error updating the next scheduled write time for detailed
  LID results.
- Soil-water-available-for-evaporation in LID soil layers wasn't limited
  by the wilting point.
- Misplaced parenthesis in the permeable-pavement infiltration-rate
  equation.
- Units-conversion error computing a pollutant's contribution from direct
  precipitation to subcatchment water quality.

### Changed

- Increased decimal places for hourly evaporation in the detailed LID
  report.

## [5.1.5] — 2014-04-23

### Fixed

- A problem reading hydraulic results from a hot-start file.

## [5.1.4] — 2014-04-14

### Added

- Support for the Ignore RDII analysis option.

## [5.1.3] — 2014-04-08

### Added

- New Upper Zone Evap. Pattern property on the Aquifer object (monthly
  adjustment of upper-zone evaporation fraction).

### Fixed

- Bug writing/reading RDII flows to the binary RDII file.

## [5.1.2] — 2014-03-31

### Fixed

- Bug preventing hotstart files with the latest format from being read.

### Changed

- Only non-ponded surface area is now saved for use in the dynamic-wave
  surcharge algorithm.

## [5.1.1] — 2014-03-24

### Added

- Support for the new NOAA-NCDC online precipitation file format.
- Modified Horton infiltration method (uses cumulative infiltration in
  excess of the minimum rate as its state variable).
- RDII interface files now saved in a binary format (ASCII still
  supported for externally-created files).
- Green Roof and Rain Garden LID categories (previously configured only
  via Bio-Retention Cell).
- Custom groundwater outflow equation per subcatchment.
- Evaporation of water from open channels.
- New conduit Seepage Rate property (uniform seepage along bottom/sloped
  sides).
- New Dynamic Wave options: maximum iterations and head tolerance per
  time step, plus reporting of the percentage of non-convergent time
  steps.
- User-settable flow tolerances for steady-state-skip determination.
- Control rules can use a conduit's OPEN/CLOSED status in premises and
  actions.
- New Node Inflows Summary column: mass-balance error in volume units.
- New Link Pollutant Load summary table.

### Changed

- Storage-unit infiltration renamed to seepage (single seepage-rate
  parameter; legacy Green-Ampt parameter sets still recognized).
- Meaning of the link "Capacity" view variable: fraction of full
  cross-section area filled (conduits) vs. control setting (other links).
- Froude Number link view variable replaced by flow volume; subcatchment
  "Losses" replaced by separate Evaporation/Infiltration variables; upper
  groundwater-zone Soil Moisture added.
- Rain Barrel Drain Delay of 0 now allows continuous draining while
  filling.
- Dropped the requirement that an impervious surface be dry before street
  sweeping.
- Remaining pollutant mass after a surface goes dry is now treated as
  unavailable for future washoff ("Remaining Buildup" in the mass-balance
  report).
- Wet-weather washoff inflow-load interpolation across a routing time
  step modified for better runoff/quality-routing agreement.
- RDII unit-hydrograph time-step selection modified for K < 1.0 (ratio of
  rising- to falling-limb duration).
- Upper groundwater zone reaching saturation now sets the lower saturated
  zone depth to the full aquifer depth.
- Conduits with small negative slopes are auto-corrected to the positive
  minimum slope (enables Steady Flow / Kinematic Wave routing).
- Avg. Froude Number / Avg. Flow Change columns replaced with
  normal-flow-limited and inlet-controlled time fractions in the Flow
  Classification Summary.
- Weirs no longer operate as an orifice when surcharged; excess flow
  floods the upstream node instead.
- Pump flow at a reporting time falling mid-transition now uses the
  nearest (start or end) value rather than interpolating.
- Binary results file no longer stores zero-valued pollutant results when
  Water Quality analysis is disabled.
- Hot Start files now contain the complete watershed + conveyance-system
  state.

### Fixed

- Fully-flowing open-channel flow can no longer exceed the full normal
  flow; Normal Flow Limit (slope + Froude) criteria unified.
- A check preventing outflow from a dry node.
- Control-rule elapsed-time/time-of-day equality tests made more
  accurate; such conditions now also accept decimal hours.
- Error 319 renumbered to 320 (new Error 319: unrecognized rainfall file
  format); external time-series format errors now use Error 363 (was
  173).

## [5.0.22] — 2011-04-21

### Added

- New validation errors: LID surface-layer vegetation volume fraction
  less than 1, total LID area exceeding subcatchment area, or total LID
  capture area exceeding subcatchment impervious area.
- New error 318 for a user rainfall file with dates out of sequence.
- New error 110 if a subcatchment's ground elevation is below its
  groundwater aquifer's initial water-table elevation.
- Checks added to the groundwater mass-balance solution (lower-zone depth
  vs. total depth; upper-zone moisture vs. porosity).
- Pump Summary Report expanded: number of startups, minimum flow, time
  off at both ends of the pump curve.

### Changed

- LID Storage-layer Conductivity now means the native soil's saturated
  hydraulic conductivity below the layer (was the layer's own
  conductivity).
- Storage layers are now optional (zero height) for Bio-Retention Cells
  and Permeable Pavement LIDs.
- A zero-top-width LID overland-flow surface now spills excess water
  above the surface storage depth instantaneously.
- Water initially stored in LID units is now reported in the Runoff
  Continuity table.

### Fixed

- Rain Barrel LID Drain Delay time conversion (hours → seconds).
- Vegetative Swale infiltration calculation, so a fully pervious swale
  with vertical sides matches an equivalent pervious subcatchment.
- Missing values for accumulation periods within an NWS rain file.
- Evaporation during wet periods incorrectly including rainfall/runon as
  available moisture (should be current ponded depth only).
- Curve Number infiltration now uses only direct precipitation (was
  including runon/internally-routed flow).
- Tailwater term in the groundwater flow equation now zero when no
  tailwater depth exists.
- Divide-by-zero for an empty Filled Circular pipe, and for an empty
  trapezoidal channel with zero bottom width.
- Critical/normal depth adjustment for a conduit no longer allowed to set
  depth to exactly zero.
- Orifice/weir flow depth not reported as 0 when its setting was changed
  to 0 (reporting only, no effect on routing).
- Node Surcharge Summary not reporting a ponded node as surcharged
  (reporting only, no effect on routing).

---

The releases below (SWMM 5.0.001–5.0.021, 2004–2010) predate this
repository's tag history; there is no corresponding `v*` git tag, so they
are headed by build number rather than `[x.y.z]`.

## Build 5.0.021 — 2010-09-30

### Changed

- Rainfall + runon used to compute infiltration no longer pre-adjusted by
  subtracting evaporation loss.
- Green-Ampt infiltration rate no longer allowed below the smaller of
  saturated hydraulic conductivity and available surface moisture
  (moisture below a small tolerance is now treated as 0).
- Pollutant Loading summary tables now list all pollutants in a single
  table (was 5 pollutants per table).

### Fixed

- A code-refactoring error in 5.0.019 that prevented recovery of
  infiltration capacity during dry periods.
- Pervious-area adjustment (5.0.019) for evaporation/infiltration to a
  subcatchment's groundwater zone.
- Accounting of evaporation loss from just a subcatchment's pervious
  area.
- Evaporation/infiltration losses from Storage nodes under Kinematic Wave
  and Steady Flow routing.

## Build 5.0.020 — 2010-08-23

### Fixed

- A refactoring bug preventing SWMM from reading rainfall data from
  external rainfall files.

## Build 5.0.019 — 2010-07-30

### Added

- Explicit modeling of five Low Impact Development (LID) practices at the
  subcatchment level.
- Pollutant buildup over a landuse can now be specified by a time series
  instead of just a buildup function.
- Option to evaporate standing water only during periods with no
  precipitation.
- Controls based on flow rates now account for flow direction.

### Changed

- Storage-node evaporation/infiltration losses now computed directly
  within the flow-routing routines for better mass conservation.
- Normal-flow check now uses only the upstream Froude number (was both
  up- and downstream).
- Maximum trials for dynamic-wave flow/head equations increased 4 → 8.
- Ponding calculation revised again for continuity: a surcharged/ponded
  node's depth change per time step is now bounded near full depth,
  governed by ponded area (dynamic wave); for Kinematic Wave/Steady Flow,
  ponded area is now just a pond/no-pond indicator and flooded depth is
  set to the node's maximum depth. Node Flooding Summary now reports
  ponded depth (dynamic wave) or ponded volume (other routing), not
  acre-inches.
- Groundwater mass-balance equations reverted to their 5.0.013 form.
- Villemonte correction for downstream submergence extended to partly
  filled orifices (previously weirs only).
- A non-conduit link connected to a storage node no longer contributes to
  the node's surface area.
- Auto max-depth adjustment to match a connected link's crown no longer
  applies to bottom orifices.
- Internal routing of runoff between impervious/pervious sub-areas is
  ignored when a subcatchment has only one type of sub-area.
- The Ignore Snowmelt switch is now automatically set true when no snow
  pack objects are defined.

### Fixed

- A missing term in the submerged-inlet-control check for Culvert
  conduits.
- Min/max daily temperatures from a climate file are now swapped if
  min > max; Hargreaves-derived evaporation rates can no longer be
  negative; several bugs reading Canadian DLY02/04 climate files.
- Zero rainfall values in a rain file/time series are now skipped
  (treated as a dry period) instead of desynchronizing the record.
- A bug desynchronizing evaporation time-series data from the simulation
  clock.
- Water-quality mass balance now correctly accounts for initial mass
  introduced via a hot-start file.
- For runoff-only models, the wet runoff time step is now capped at the
  reporting time step when the latter is smaller.

### Removed

- Fatal error is now raised for a negative conduit entrance/exit/average
  loss coefficient (previously silently accepted).

## Build 5.0.018 — 2009-11-18

### Added

- Storage Volume Summary table now reports total infiltration +
  evaporation loss as a percentage of total inflow, per storage unit.
- Warning message when a Rain Gage's recording interval is less than the
  smallest interval in its rainfall time series.
- Hot Start files now include each subcatchment's final groundwater-zone
  state.

### Changed

- Link Summary table now lists the actual conduit slope rather than the
  slope adjusted by conduit lengthening.
- Status Report now displays only the summary tables for which results
  were obtained.
- Engine version number corrected to 50018 (had been overlooked since
  5.0.010).

### Fixed

- Double counting of final stored volume when finding nodes with the
  highest mass-balance errors.

## Build 5.0.017 — 2009-10-07

### Added

- A default dry-weather-flow concentration property on the Pollutant
  object (overridable per node).

### Changed

- Ponding routine for dynamic wave routing further modified to handle a
  node transitioning between surcharged and ponded conditions within one
  time step (fixing large 5.0.016 ponding continuity errors).
- Error 112 (conduit elevation drop exceeds length) downgraded from fatal
  error to warning; slope computed the pre-5.0.014 way (elevation
  drop/length) in this case.
- Inflow interface files no longer need to contain every pollutant
  defined in the current project.
- RDII unit-hydrograph time step now uses the smaller of the wet runoff
  time step and the shortest hydrograph's time-to-peak (was the rain
  gage's recording interval), permitting hydrographs that peak faster
  than the gage interval.
- Curve Number infiltration now stops once the maximum capacity is fully
  used.
- CSTR mixing equation for water-quality routing replaced with a more
  robust finite-difference approximation (avoids numerical problems at
  high decay rates); first-order decay is now applied under Steady Flow
  routing via a dedicated routine.

### Fixed

- Water-quality mass-balance errors in systems with node treatment,
  by correctly accounting for both inflow mass and mass in storage.

### Removed

- The small ponded-depth tolerance before runoff initiation was removed
  for a smoother runoff response.

## Build 5.0.016 — 2009-06-22

### Added

- Option to compute daily evaporation from climate-file daily
  temperatures using Hargreaves' method.
- Recognition of comma-delimited NCDC rainfall files (with/without
  station name) and space-delimited NCDC files with empty condition-code
  fields.
- Error check for an RDII unit hydrograph whose time base is less than
  its rain gage's recording interval.

### Changed

- Nodes that can pond are no longer always treated as non-surcharging
  storage nodes — only once ponding actually occurs.
- Extrapolated storage-curve surface area above the table's highest depth
  is now only used if the curve slopes outward; otherwise the last
  tabulated area is used.

### Fixed

- A small full/not-full storage-node tolerance that could keep a full
  unit "full" despite small net outflow, removed.
- Spurious negative-elevation-offset warnings for `*`-offset or
  near-invert offset values.
- A 5.0.015 regression producing incorrect RDII inflows when the gage
  recording interval was less than the wet time step.

## Build 5.0.015 — 2009-04-10

### Added

- Optional Green-Ampt infiltration parameters on Storage nodes (infiltration
  basin support), now explicitly accounting for ponded-water-depth effect
  on infiltration rate.
- Separate Initial Abstraction parameters (max depth, initial depth,
  recovery rate) for each of the three RDII unit hydrographs (short/
  medium/long term) in a group.
- Meander Modifier transect parameter (ratio of meandering main-channel
  length to overbank length).
- Recognition of space-delimited NWS TD 3240/3260 files with a station
  name field.

### Changed

- Normal-flow limitation based on Froude number now requires the
  criterion hold for both upstream and downstream depths (was either).
- Computed top surface width for dynamic wave routing is no longer
  floored at the width-at-4%-depth value; the actual width is used no
  matter how small.

### Fixed

- A 5.0.014 regression that inadvertently removed the 2 GB binary
  output-file size limit for GUI runs.
- Backflow into an outfall node is now correctly counted in the node's
  Total Inflow result.
- Reporting error for overflow rate into ponded volume at a flooding
  node under dynamic wave routing.

### Removed

- Rainfall time-series/rain-gage recording-interval mismatches are now a
  fatal error instead of a silently auto-adjusted gage interval.

## Build 5.0.014 — 2009-01-21

Large feature release (culverts, custom cross-sections, minimum slope,
baseline inflow patterns).

### Added

- Culvert Inlet Control flow computation under dynamic wave routing for
  designated Culvert conduits.
- Minimum Slope option — a computed conduit slope is never allowed below
  this value.
- Optional Baseline Time Pattern for external inflows at nodes (monthly/
  weekly periodic adjustment).
- Outlet rating curve can be based on either freeboard depth (as before)
  or the upstream/downstream head difference.
- "SIMULATION MONTH"/"SIMULATION DAY" added as control-rule time
  conditions; conduit OPEN/CLOSED status usable in premises/actions.
- Time Series data can now be imported from an external file.
- Option to ignore any combination of Rainfall/Runoff, Snowmelt,
  Groundwater, Flow Routing, and Water Quality process models.
- A user-defined groundwater outflow equation per subcatchment.
- Modified Baskethandle cross section extended to any circular-top radius
  ≥ half the section width.

### Changed

- Rain gage recording interval auto-adjusted to the smallest interval in
  its time-series data (with a warning); fatal error if gages sharing a
  time series don't share the same Rainfall Format.
- Curve Number infiltration regeneration rate now simply the reciprocal
  of the user-supplied drying time (no longer needs saturated
  conductivity); optional monthly adjustment pattern for the recovery
  rate.
- Under-relaxation of pump flows between DW-routing iterations dropped
  (could violate the pump curve); upstream-weighted area now used in the
  dQ/dH term for conduits; Froude numbers for the normal-flow check now
  use hydraulic depth.
- Ponded volume under dynamic wave routing now computed from computed
  nodal depth (reverting to pre-5.0.010 behavior) for consistency with
  storage-node treatment; orifice head now measured from opening midpoint
  (not bottom), and orifices no longer contribute end-node surface area.
- Orifice partial-open setting now interpreted as fraction of opening
  height (was fraction of area); equivalent discharge coefficient
  recomputed on every setting change.
- Washoff of user-specified initial buildup with no buildup function now
  works correctly; runoff/runon/rainfall concentration mixing revised for
  more consistent results, especially with BMP removal.
- Storage-unit quality routing switched to the analytical CSTR solution;
  HRT update formula revised; Steady Flow quality routing now treats
  conduit concentration as equal to the upstream node's.
- Reverse (backflow) inflow at an Outfall is now treated as an external
  inflow for water-quality purposes (models saltwater/contaminant
  intrusion).
- Snow removal now begins once the removal-depth threshold is reached,
  correctly converted to internal feet.
- "Total Flooding"/ponded-volume Node Depth Summary column relabeled "Max
  Vol. Ponded"; MGD/CMS flow values now report to 3 decimal places.

### Fixed

- Green-Ampt infiltration rate at the point of surface saturation
  mid-time-step.
- A crash with the No Routing option combined with Save Outflows
  Interface File.
- Under Steady Flow/Kinematic Wave, a Dummy conduit connecting to a
  higher-elevation node no longer requires an inlet offset.
- Possible closing of tide gates on outfalls directly connected to
  orifice/weir/outlet links.
- A bug preventing RDII from being computed for hydrographs sharing a
  rain gage with another hydrograph; a groundwater bug allowing
  infiltration to continue once the water table fully saturated; a
  metric-units conversion error for computed groundwater flow.
- The flow contribution of the triangular ends of a trapezoidal weir.
- A roundoff error under kinematic wave/steady flow that occasionally
  mis-reported nodes as ponded.

## Build 5.0.013 — 2008-03-11

### Changed

- PID controller definition and implementation revised.
- Dynamic-wave routing: new method weights upstream conduit geometry more
  as the Froude number approaches 1; Normal Flow Limit (slope + Froude)
  now applies both criteria together; flow in a fully-flowing open
  channel capped at full normal flow; a dry node can no longer have
  outflow; ponding computation reverted to the 5.0.009 approach (depth
  from volume); max-depth-change time-step criterion restored.

### Fixed

- Acceptable site-latitude value check.
- A code-refactoring error in the dynamic-wave momentum equation's
  inertial term.
- A node's crown elevation now considers connecting non-conduit links.
- Possible incorrect initial orifice setting.
- Error checks added for invalid numbers in a hot-start file.

## Build 5.0.012 — 2008-02-04

### Added

- PID-type modulated control rule.
- User-assigned maximum conduit flow limit now applies to all routing
  options (was Dynamic Wave only).
- Possibility of ponding at a Type I pump's inlet (wet well) node.

### Changed

- Conduit/orifice/weir/outlet offsets can now be an absolute elevation or
  a relative depth above the node invert (`LINK_OFFSETS` option).
- "Flooding" now recorded whenever water level exceeds a node's top,
  whether or not ponding occurs (previously only when there was no
  ponding).
- Green-Ampt upper-soil-zone drying-time calculation moved from time 0 to
  the first rainfall period (removes a start-date-shift artifact).
- Steady-state-flow detection criteria realigned with SWMM 4.
- Minimum flow-area/hydraulic-radius floor (0.0001) for dynamic wave
  routing removed (redundant with the depth floor); flow-direction test
  for UPSTREAM/DOWNSTREAM CRITICAL conditions removed (could stall
  solutions); max-depth-change time-step criterion dropped again.
- Head-loss calculation from flap gates extended to orifices.
- "Snow Only" pollutant-buildup option, previously unimplemented, now
  works.

### Fixed

- SI unit-conversion bugs for pump on/off depth settings and pump-curve
  slope values; Hazen-Williams head-loss formula for force mains.
- A 5.0.010 regression preventing RDII computation for hydrographs
  sharing a rain gage.
- Pollutant loading from RDII now based on RDII quality (was rainfall
  quality).
- System outflow/flooding values saved to the binary results file now
  match the values used for the flow-continuity-error calculation.
- Command-line version's default `END_TIME` corrected from 24 days to 0.

## Build 5.0.011 — 2007-07-16

### Fixed

- Weir/Outlet settings not being updated after a control-rule change.
- Weir control setting not accounted for in the equivalent orifice
  coefficient for surcharged flow, in V-notch weir flow, or in reported
  weir flow depth.
- A 5.0.010 change to ponded depth/volume computation under dynamic wave
  routing.
- Runon/rainfall/ponded-water quality-mixing equations, to prevent
  numerical instability at very low volumes.
- NCDC rainfall-file missing values ('M' flag) now counted in the
  reported missing-record total.

## Build 5.0.010 — 2007-06-19

Major release: engine recompiled with all `float`s as `double`s (except
binary-interface-file fields) under VC++ 2005.

### Added

- NO ROUTING analysis option (runoff-only runs).
- Ideal Pump type (pumps at inlet inflow rate, no pump curve).
- Custom Shape conduit cross section (via a new Shape Curve) and Circular
  Force Main shape (Hazen-Williams or Darcy-Weisbach for pressurized
  flow).
- Pump startup/shutoff inlet-node depths as direct pump properties
  (previously control-rule only).
- Timed orifice gate open/close rate (SWMM 4 `ORATE` parity).
- Initial-abstraction loss on RDII unit hydrographs.
- Combined slope + Froude-number criterion for supercritical/normal flow.
- Flow Instability Index per non-pump link, with the five highest listed
  in the Status Report.
- Node volumes now initialized from hot-start depth to reflect implied
  initial ponding.

### Changed

- Orifice head now measured to the opening's midpoint (not bottom);
  orifices no longer contribute end-node surface area; partial-open
  setting reinterpreted as fraction of opening height with the discharge
  coefficient recomputed on each change.
- Ponded depth under dynamic wave routing always set equal to computed
  ponded depth (was the smaller of ponded/dynamic depth).
- Width-vs-depth tables for circular and irregular cross sections
  expanded to 51 entries.
- Treatment-function math-expression evaluation made more efficient.
- Node Depth Summary's ponded-volume column relabeled "Max Vol. Ponded".

### Fixed

- Area corrections to dynamic-wave inlet/outlet loss terms (introduced in
  5.0.008) removed — reverted a regression.
- Kinematic-wave inflow-area normalization when flow is capped at maximum
  normal flow.
- Dynamic-wave variable-time-step node-fullness check (avoided
  excessively small steps).
- Divider-node check now examines both diversion-link end nodes.
- Outlet-link conditions now recognized in control rules; error raised
  for multiple rule clauses on one line.
- Ignore Rainfall option now zeroes rain-gage rainfall (prevented a
  spurious reported value).
- New Error 108 when a subcatchment outlet ID collides with both a node
  and a subcatchment name.
- Groundwater bug allowing infiltration to continue once the water table
  fully saturated, plus a metric-units conversion error on computed flow.
- Flow contribution of a trapezoidal weir's triangular ends.
- A roundoff error occasionally mis-reporting ponded nodes under
  kinematic wave/steady flow.

## Build 5.0.009 — 2006-09-19

### Changed

- Minimum runoff able to generate pollutant washoff changed from
  0.001 in/hr to 0.001 cfs.
- A new RDII event now begins once continuous dry weather exceeds the
  longest unit hydrograph's base time (was a fixed 12 hours).

### Fixed

- User-prepared climate files no longer confused with the Canadian
  format.
- Dynamic-wave routing through long force mains connected to Type 3/4
  pumps.

## Build 5.0.008 — 2006-07-05

### Added

- Constant value + scaling factor for Direct External inflows.
- Total pollutant washoff-load listing per subcatchment; new Node
  Inflows/Flooding and Outfall flows/pollutant-loads summary tables.
- Checks for non-negative conduit offsets and orifice/weir/outlet
  heights.

### Changed

- Pipe invert elevations at outfalls now measured relative to the
  outfall stage elevation (was the outfall's own invert).
- Entrance/exit minor-loss terms for dynamic wave routing adjusted by the
  mid-point-to-entrance/exit area ratio.
- Equivalent length cap for orifices/weirs changed from a 200 ft minimum
  to a 200 ft maximum.
- Subcatchment pollutant washoff reprogrammed for more rigorous mass
  balance when runoff is routed across subcatchments or with direct
  rainfall deposition.
- Revoked Engine Update #12 from 5.0.006.

### Fixed

- Horton infiltration drying-time → regeneration-curve-constant
  conversion.
- Flow-depth-from-head error in the dynamic-wave Froude-number
  normal-flow check.
- Rainfall-unit conversion when reading from an external file.
- Display of washoff mass-balance results for Counts/Liter pollutants.
- Reporting of total system maximum runoff rate in the Subcatchment
  Runoff Summary table.

## Build 5.0.007 — 2006-03-10

### Added

- Ignore Rainfall analysis option (external inflows/DWF only, no
  rainfall-driven runoff).
- Peak runoff flow added to the Subcatchment Summary table; non-conduit
  links now included in the Link Flow Summary table.

### Changed

- Hydraulic-radius calculations for Rectangular-Closed,
  Rectangular-Triangular, and Rectangular-Round shapes now account for
  wetted-perimeter increase under full flow.
- Full-Flow vs. Maximum-Flow distinction refined in several closed-conduit
  code paths; irregular cross sections where max-normal-flow depth is
  less than full depth now handled correctly.

### Fixed

- Final ponded-water volume from node flooding now included in the
  reported flow-continuity error.

## Build 5.0.006a — 2005-10-19

### Fixed

- Snowmelt-during-rainfall formula returned ft/sec instead of in/hr.
- Routing-interface-file generation for systems with nodes but no links.

## Build 5.0.006 — 2005-09-05

### Added

- Storage Unit maximum-volume/outflow-rate summary table.
- Optional SWMM 4 `BC` parameter (minimum groundwater table elevation for
  flow) on the groundwater flow equation.
- Control-rule Action clause can set pump/orifice/weir/outlet control via
  a curve (vs. node depth) or a time series ("Modulated Controls").
- Geometry tables for standard-size elliptical pipes.

### Changed

- Storage curves (area vs. depth) now linearly extrapolated beyond the
  table limit (SWMM 4 behavior), not held constant.
- Evaporation no longer computed for a dry storage unit; storage-unit
  water-quality concentrations now adjusted for evaporation loss each
  step.
- A climate file now positions to the simulation start (not file start)
  unless the user specifies a starting date; reaching end-of-file during
  a run is now a fatal error (was silently held at last value).
- Pollutant treatment functions using storage-node concentration now use
  inflow concentration (matching non-storage nodes); global first-order
  decay no longer applied to a storage unit that has its own treatment
  function.
- Total moisture available for infiltration each runoff step now has
  evaporation subtracted first.
- Node/Conduit flow statistics in the Status Report now collected only
  over the reporting period (not the full simulation period).

### Fixed

- Interior nodes mistaken for outfall nodes (depending on connecting-link
  orientation) during water-quality analysis.
- Water-quality routing through dummy conduits.
- Standard-size elliptical-pipe code number mistaken for an actual
  dimension.
- Upper-soil-zone moisture depletion during dry periods under Green-Ampt
  infiltration.
- Initial/final groundwater storage volumes in the Groundwater Continuity
  table (reporting only; did not affect computed flows or water-table
  levels).
- Climate files can now supply evaporation during runoff-free runs (was
  ignored with no subcatchments present).

## Build 5.0.005b — 2005-06-15

### Fixed

- End-node offsets for partly-filled circular cross sections weren't
  increased to account for fill depth.
- Weir flow wasn't necessarily zero when the high-head side's water level
  was zero.

### Changed

- Bottom Orifice "crest height" now interpreted as a horizontal plane
  above the upstream node's invert (supports storage-unit riser
  outlets).

## Build 5.0.005a — 2005-05-25

### Fixed

- An erroneous error message for a node with multiple outflow links
  including an Outlet link.

## Build 5.0.005 — 2005-05-20

### Added

- Maximum-allowable-flow property on the Conduit object (default 0 = no
  limit).
- New dynamic-wave routing option selecting the normal-flow-limit
  criterion (SWMM 4 `KSUPER` parity); new option to skip routing during
  steady-flow periods (reduces continuous-simulation run time).
- New, more robust water-quality routing algorithm for dynamic wave
  routing.

### Changed

- Conversion factor for external pollutant mass inflows must now convert
  to mass-concentration-per-second (flow units no longer part of the
  conversion).
- Minimum elevation change for a flat conduit changed to 0.001 ft (SWMM 4
  parity).
- Irregular cross-section max depth now based on the highest station
  elevation (was first/last station), with vertical walls added at the
  ends if needed; nominal width now the top width at full depth (was max
  width over all depths).
- Head over a non-surcharged, submerged weir now based on height above
  the weir crest (was head difference across the weir); side-contraction
  weir-length-reduction equation fixed (SWMM 4 bug).
- Depths at outfalls under Steady/Kinematic Wave routing now reported as
  the connecting conduit's depth.

### Fixed

- Ponded-depth computation at flooded nodes under dynamic wave routing.
- Wrong lookup function for Time-Series outfall water elevations.
- Interpolation of values from a routing interface file.
- Rainfall-file reader confusing the standard space-delimited format with
  other formats; a reporting error for rainfall series with no ending
  zero value.
- A missing snowmelt-coefficient computation for pervious areas.
- Max-to-design flow ratio per conduit, now accounting for barrel count.

### Removed

- The Compatibility Mode dynamic-wave option was removed in favor of a
  single method designed for SWMM 4 compatibility with more stable
  results.

## Build 5.0.004 — 2004-11-24

### Added

- Pollutant concentration-unit codes added to the binary output file.

### Changed

- Curve Number infiltration's regeneration-rate-from-drying-time
  calculation corrected to use a constant (not continuously declining)
  infiltration capacity per rain event.
- Surcharged/high-Froude-number conduits are now included when computing
  a dynamic-wave variable time step (previously excluded).

### Fixed

- NCDC-formatted external rain-file identification/reading.
- Reported-velocity sign for links with adverse slope.
- Reading results from previously saved Runoff Interface files.
- Dynamic-wave routine for SWMM3/SWMM4 compatibility modes (better match
  to Extran results).
- Zero-sloped-conduit check widened to elevation differences below
  0.01 ft.
- Ponded-depth computation at flooded nodes under dynamic wave routing.

## Build 5.0.003 — 2004-11-10

### Added

- Error 405 for a binary results file that would exceed the 2.1 GB system
  limit.
- Support for Canadian DLY02/DLY04 temperature files.

### Fixed

- Full-depth width-table entries for closed rounded cross sections
  (numerical stability under dynamic wave routing).
- A units problem for RDII inflows under metric flow units.
- Reading the `TEMPDIR` option when it contained spaces.
- Rule-based control of weir crest height (control setting previously
  adjusted flow instead of the crest-to-crown distance).

## Build 5.0.002 — 2004-11-01

### Changed

- Modifications to the Picard method used for dynamic-wave flow routing.

## Build 5.0.001 — 2004-10-29

First official release of SWMM 5.
