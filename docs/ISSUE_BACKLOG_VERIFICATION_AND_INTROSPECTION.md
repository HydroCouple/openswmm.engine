# Solver Verification and Introspection Backlog

## Purpose

This document turns the current verification and observability ideas into issue-ready work items for the `swmm6_rel` branch. It is intentionally organized as a backlog that can be split into repository issues, grouped into milestones, or used as the basis for an epic-level roadmap.

The focus areas are:

1. Legacy solver verification against analytical and canonical benchmarks.
2. A layered solver verification suite.
3. Better in-memory introspection for calibration, debugging, and scientific validation.
4. Structured time-series export as a plugin-oriented extension area.
5. A canonical benchmark library of small deterministic cases.
6. Physics review of behavior that changed relative to `main`.
7. Advanced modeling and research extensions that should be staged behind verification.

## How To Use This Document

Each section below is written so it can be copied into a GitHub issue with minimal editing. For each proposed issue, consider adding labels such as:

- `verification`
- `legacy-engine`
- `tests`
- `benchmarks`
- `introspection`
- `api`
- `plugin`
- `export`
- `documentation`
- `good-first-epic`

## Branch Context

This backlog is written against the `swmm6_rel` repository layout, where the preserved EPA solver lives under `src/legacy/engine/` and newer test and plugin work is separated elsewhere.

Relevant solver files:

- `src/legacy/engine/infil.c`
- `src/legacy/engine/xsect.c`
- `src/legacy/engine/forcmain.c`
- `src/legacy/engine/node.c`
- `src/legacy/engine/exfil.c`
- `src/legacy/engine/culvert.c`
- `src/legacy/engine/massbal.c`

Relevant verification areas:

- `tests/unit/legacy/`
- `tests/regression/`
- `tests/benchmarks/`

## Epic 0: Physics Review Since `main`

### Goal

Review and verify the small set of legacy-physics behaviors that appear to have changed materially relative to `main`, so the branch can move forward with a clear understanding of what is genuinely new physics versus what is only infrastructure, packaging, or documentation work.

### Why This Matters

The `swmm6_rel` branch includes a broad repository refactor, but the highest-priority solver review should focus on the few places where physical behavior likely changed rather than re-reviewing the entire preserved engine indiscriminately.

### Current High-Priority Review Targets

1. Modified Horton cumulative infiltration limiting in `src/legacy/engine/infil.c`.
2. Elliptical pipe geometry corrections in `src/legacy/engine/xsect.c`.
3. Outfall depth handling with non-zero link offsets in `src/legacy/engine/node.c`.

---

## Issue: Review and Verify Physics Changed Since `main`

### Summary

Create a focused review issue that identifies, documents, and verifies the solver behaviors that changed materially relative to `main`.

### Target Files

- `src/legacy/engine/infil.c`
- `src/legacy/engine/xsect.c`
- `src/legacy/engine/node.c`
- `tests/unit/legacy/`
- `tests/regression/`

### Scope

1. Confirm the intended physical meaning of the Modified Horton `Fmax` update.
2. Confirm the intended elliptical geometry corrections for custom-sized elliptical pipes.
3. Confirm the outfall-depth fix for non-zero link offsets under free and normal outfall conditions.
4. Document what changed, why it changed, and what tests prove the new behavior.

### Acceptance Criteria

1. A short review note documents each physics-relevant change relative to `main`.
2. Each changed behavior has at least one targeted verification test.
3. The resulting tests are linked from the appropriate issue or design note.

---

## Epic 1: Analytical Verification of Core Legacy Formulations

### Goal

Build a scientifically defensible verification layer around the preserved SWMM solver core by testing isolated formulations against analytical results, published equations, or trusted benchmark tables.

### Why This Matters

The legacy engine contains real formulations, not only file handling and orchestration logic. Several modules can be verified directly against closed-form relationships or accepted reference values. This is the fastest path to increasing trust in the solver while the broader refactor continues.

---

## Issue: Verify Horton and Green-Ampt State Evolution

### Summary

Add analytical and semi-analytical tests for infiltration state evolution in `src/legacy/engine/infil.c`, focusing first on Horton and Green-Ampt behavior.

### Problem Statement

The infiltration module contains time-dependent state variables and multiple model branches. At present, there is not enough automated evidence that these state transitions remain correct under refactoring, packaging, or platform-specific build changes.

### Target Files

- `src/legacy/engine/infil.c`
- `src/legacy/engine/infil.h`
- `tests/unit/legacy/`
- `tests/regression/`

### Scope

1. Horton infiltration recovery and decay.
2. Modified Horton behavior under wetting and drying cycles.
3. Green-Ampt unsaturated and saturated transitions.
4. Green-Ampt cumulative infiltration evolution under fixed rainfall intensity.
5. State serialization and restore consistency if hotstart-related state is involved.
6. Explicit review of the `Build 5.3.0` Modified Horton cumulative `Fmax` limiting behavior.

### Candidate Test Cases

1. Constant rainfall with known Horton decay curve.
2. Drying period followed by re-wetting for Horton recovery.
3. Green-Ampt infiltration with fixed suction head, conductivity, and initial moisture deficit.
4. Green-Ampt transition from unsaturated to saturated upper zone.
5. Edge cases with zero rainfall, zero runon, and small ponded depth.
6. Modified Horton case where cumulative infiltration approaches and reaches `Fmax`.
7. Modified Horton dry-period recovery case showing that the capped cumulative term recovers consistently.

### Acceptance Criteria

1. Unit tests compare computed infiltration rates and state variables against hand-derived or literature-based reference values.
2. Tests cover both rate outputs and internal state evolution over time.
3. Numerical tolerances are justified and documented.
4. Tests run in CI.
5. The 5.3.0 `Fmax` behavior is specifically covered so regressions are detectable.

### Suggested Deliverables

1. A test fixture for infiltration state stepping.
2. A small reference notebook or markdown note deriving expected results.
3. At least one regression fixture that confirms no drift relative to established reference values.

---

## Issue: Verify Cross-Section Geometry Functions

### Summary

Add direct verification tests for area, wetted perimeter, hydraulic radius, section factor, and critical depth behavior in `src/legacy/engine/xsect.c`.

### Problem Statement

The geometry module is foundational for routing, normal flow, critical flow, and force main calculations. It is one of the most testable parts of the codebase because many relationships are either closed-form or can be checked against tabulated references.

### Target Files

- `src/legacy/engine/xsect.c`
- `src/legacy/engine/transect.c`
- `tests/unit/legacy/`

### Scope

1. Area as a function of depth.
2. Top width and wetted perimeter checks.
3. Hydraulic radius and section factor checks.
4. Inverse mappings such as area-to-depth and section-factor-to-area.
5. Critical depth calculations for representative shapes.
6. Explicit review of the `Build 5.3.0` custom-sized elliptical pipe correction.

### Candidate Test Cases

1. Circular full and partially full conduit cases.
2. Rectangular open and closed section cases.
3. Triangular and trapezoidal section sanity checks.
4. Force-main circular geometry consistency.
5. Monotonicity and invertibility checks over valid ranges.
6. Horizontal ellipse cases with known full-area and hydraulic-radius expectations.
7. Vertical ellipse cases with known full-area and hydraulic-radius expectations.
8. Custom-sized ellipse round-trip tests for depth, area, and section factor.

### Acceptance Criteria

1. Analytical shapes are verified against known formulas.
2. Numerical inverses satisfy round-trip tolerances.
3. Critical-depth routines are validated against independent calculations for representative sections.
4. Shape-specific edge cases are covered near dry, near-full, and max-section-factor conditions.
5. Elliptical geometry corrections are explicitly protected by regression tests.

---

## Issue: Verify Force Main Friction Slope Calculations

### Summary

Add verification tests for Hazen-Williams and Darcy-Weisbach friction slope calculations in `src/legacy/engine/forcmain.c`.

### Problem Statement

Force main behavior is physically important and mathematically compact enough to validate directly. This makes it a good target for analytical unit tests and benchmark tables.

### Target Files

- `src/legacy/engine/forcmain.c`
- `src/legacy/engine/link.c`
- `tests/unit/legacy/`

### Scope

1. Hazen-Williams friction slope checks.
2. Darcy-Weisbach friction slope checks.
3. Reynolds number regime transitions.
4. Friction factor continuity across laminar, transitional, and turbulent ranges.

### Candidate Test Cases

1. Published textbook examples for Hazen-Williams pipes.
2. Darcy-Weisbach checks with fixed roughness, hydraulic radius, and velocity.
3. Laminar flow limit checks.
4. Transitional regime continuity checks.
5. Fully rough turbulent regime checks.

### Acceptance Criteria

1. Computed slopes match independently derived reference values within documented tolerances.
2. Transitional regime behavior does not show unexpected discontinuities.
3. Unit tests identify the expected flow regime for reference cases.

---

## Issue: Verify Analytical Storage Shape Relationships

### Summary

Add verification tests for storage node volume-depth-area relationships and exfiltration-related shape handling in `src/legacy/engine/node.c` and `src/legacy/engine/exfil.c`.

### Problem Statement

Analytical storage shapes are highly suitable for deterministic testing. Errors here propagate into storage routing, exfiltration, continuity accounting, and stage boundary handling.

### Target Files

- `src/legacy/engine/node.c`
- `src/legacy/engine/exfil.c`
- `tests/unit/legacy/`

### Scope

1. Cylindrical, conical, paraboloid, and pyramidal storage volume-depth relationships.
2. Surface area and volume consistency.
3. Inverse depth-from-volume routines.
4. Exfiltration bottom and bank area handling for analytical shapes.
5. Separation of storage-geometry verification from outfall boundary-condition verification.

### Candidate Test Cases

1. Exact known volumes for simple depths.
2. Round-trip volume-to-depth-to-volume checks.
3. Exfiltration area partitioning checks for bottom versus banks.
4. Near-empty and near-full edge cases.

### Acceptance Criteria

1. Analytical shapes reproduce exact or near-exact geometric expectations.
2. Inverse geometry routines remain numerically stable.
3. Exfiltration area logic agrees with geometry assumptions for each shape.

---

## Issue: Add FHWA Culvert Inlet-Control Benchmark Cases

### Summary

Add known FHWA inlet-control benchmark cases for `src/legacy/engine/culvert.c`.

### Problem Statement

The culvert implementation uses parameterized inlet-control equations derived from FHWA guidance. This is a natural place to anchor the code to published reference cases.

### Target Files

- `src/legacy/engine/culvert.c`
- `tests/unit/legacy/`
- `tests/regression/`

### Scope

1. Unsubmerged inlet-control cases.
2. Submerged inlet-control cases.
3. Transition-zone cases.
4. Sensitivity to culvert code, slope correction, and full depth.

### Candidate Test Cases

1. Circular concrete culvert benchmark values.
2. Corrugated metal culvert benchmark values.
3. Box culvert benchmark values.
4. Cases spanning unsubmerged, submerged, and transition flow.

### Acceptance Criteria

1. Selected benchmark cases match FHWA-derived reference flows within documented tolerance.
2. Tests cover at least three culvert families and multiple flow regimes.
3. The tests clearly distinguish geometry setup from expected result derivation.

---

## Issue: Verify Outfall Boundary Conditions With Link Offsets

### Summary

Add targeted tests for outfall depth behavior with non-zero link offsets, especially for `FREE_OUTFALL` and `NORMAL_OUTFALL` cases in `src/legacy/engine/node.c`.

### Problem Statement

`Build 5.3.0` includes a fix for a hydraulically important boundary-condition bug where non-zero link offsets could force an incorrect zero depth at outfalls. This should be isolated and verified directly rather than left implicit inside larger routing scenarios.

### Target Files

- `src/legacy/engine/node.c`
- `src/legacy/engine/link.c`
- `src/legacy/engine/flowrout.c`
- `tests/unit/legacy/`
- `tests/regression/`

### Scope

1. Free outfall with non-zero upstream or downstream link offset.
2. Normal outfall with non-zero offset.
3. Zero-offset control case.
4. Sensitivity to conduit orientation and depth regime.

### Candidate Test Cases

1. Single conduit to outfall with offset and free outfall boundary.
2. Single conduit to outfall with offset and normal outfall boundary.
3. Paired zero-offset versus non-zero-offset comparison.
4. Regression fixture verifying depth is non-zero where hydraulically expected.

### Acceptance Criteria

1. Tests isolate the outfall boundary-condition logic rather than only observing full-model behavior.
2. Non-zero link offsets no longer collapse outfall depth incorrectly.
3. The expected behavior is documented in test comments or a short note.

---

## Issue: Add Closed-Basin And No-Loss Continuity Checks

### Summary

Add continuity-focused tests for `src/legacy/engine/massbal.c` using closed-basin or no-loss scenarios.

### Problem Statement

Mass balance drift is one of the most important ways a hydraulic engine loses trust. The current system computes detailed continuity bookkeeping, but deterministic tests should explicitly verify closed-system cases.

### Target Files

- `src/legacy/engine/massbal.c`
- `src/legacy/engine/routing.c`
- `src/legacy/engine/runoff.c`
- `tests/regression/`

### Scope

1. No-loss runoff continuity.
2. No-loss flow routing continuity.
3. Closed-basin storage continuity.
4. Isolated pollutant continuity where practical.

### Candidate Test Cases

1. Closed storage system with no evaporation, seepage, or overflow.
2. Single conduit transfer with no losses.
3. Pure routing test with known inflow/outflow accumulation.
4. Small runoff-only case with exact bookkeeping expectations.

### Acceptance Criteria

1. Continuity error stays within a very small specified tolerance for deterministic no-loss cases.
2. Tests fail loudly when accounting terms change unexpectedly.
3. Reported mass-balance components are inspectable by the test harness.

---

## Epic 2: Proper Solver Verification Suite

### Goal

Build a layered verification suite that separates formula-level checks, process-level benchmark cases, and whole-model regression scenarios.

### Why This Matters

This is the highest-value next step for the repository. It provides a durable framework for future solver work, API evolution, plugin work, packaging, and cross-platform support.

### Layer 1: Analytical Unit Tests For Isolated Formulas

These tests should cover compact, deterministic, directly verifiable computations.

Examples:

1. Infiltration state updates.
2. Cross-section geometry functions.
3. Force main friction slope and friction factor routines.
4. Storage geometry inverses.
5. Culvert inlet-control equations.
6. Outfall boundary-condition logic with offsets.

### Layer 2: Canonical Benchmark Cases For Individual Process Models

These tests should cover small process-level simulations where the expected behavior is known even if not fully closed form.

Examples:

1. Single conduit routing.
2. Reservoir draining.
3. Horton recovery under repeated events.
4. Green-Ampt wetting front progression.
5. Storage exfiltration under fixed stage.
6. Outfall stage/depth response with offset conduits.

### Layer 3: Golden-File Regression Tests For Whole-Model Scenarios

These tests should compare larger scenario outputs against blessed reference data.

Examples:

1. Example models already used by the project.
2. A stress-case network.
3. Cross-version legacy/new-engine comparison cases.
4. Binary output and selected in-memory summaries at reporting intervals.

### Suggested Issue: Build The Verification Pyramid

#### Summary

Create a formal three-layer verification strategy and implement the first representative case in each layer.

#### Acceptance Criteria

1. A documented testing taxonomy exists in `docs/`.
2. CI distinguishes formula tests, process benchmarks, and regression scenarios.
3. At least one representative case exists in each verification layer.
4. Tolerances and reference-data provenance are documented.

## Epic 3: Better In-Memory Introspection

### Goal

Expose internal solver state in a structured, stable, and testable form so developers can inspect what the engine is doing without relying only on report files or ad hoc debugging.

### Why This Matters

This will make calibration, debugging, scientific validation, and GUI/tooling integration much easier. It also creates a path toward better APIs and better test diagnostics.

### Proposed Introspection Targets

1. Link hydraulic terms.
2. Node convergence status.
3. Iteration counts.
4. Mass-balance components.
5. Courant-limited links and nodes.
6. Boundary-condition diagnostics where feasible.

---

## Issue: Expose Link Hydraulic Terms

### Summary

Add getters or structured state access for intermediate link hydraulic terms used during routing.

### Candidate Data

1. Current and prior flow.
2. Area and hydraulic radius.
3. Froude number.
4. `dqdh` and related sensitivity terms.
5. Loss rates and normal-flow limitation flags.

### Acceptance Criteria

1. Link hydraulic diagnostics are accessible without parsing report text.
2. Access is available through a stable API or structured debug interface.
3. Values can be asserted in tests for selected benchmark cases.

---

## Issue: Expose Node Convergence And Iteration Diagnostics

### Summary

Add visibility into dynamic-wave convergence state and routing iteration behavior.

### Candidate Data

1. Per-step iteration count.
2. Per-node convergence status.
3. Non-converging nodes and links.
4. Time-step reductions caused by convergence or Courant limits.
5. Boundary-condition decision diagnostics where practical.

### Acceptance Criteria

1. Routing diagnostics are queryable during or after simulation.
2. Tests can assert expected iteration behavior for small cases.
3. CI artifacts can optionally capture diagnostic summaries for failed runs.

---

## Issue: Expose Mass-Balance Components Programmatically

### Summary

Add structured getters for the mass-balance components currently accumulated internally.

### Candidate Data

1. Runoff totals.
2. Groundwater totals.
3. Flow routing totals.
4. Water-quality routing totals.
5. Per-step continuity terms where feasible.

### Acceptance Criteria

1. Tests can inspect balance components directly.
2. Golden-file regression can compare summary continuity tables without text scraping.
3. Downstream tooling can query the same values through API calls.

---

## Issue: Expose Courant-Limited Links And Nodes

### Summary

Add access to the links and nodes most frequently limiting the dynamic-wave time step.

### Why It Matters

This data is valuable for model diagnosis, performance analysis, and numerical stability investigations.

### Acceptance Criteria

1. The current time-step limiter can be identified programmatically.
2. Historical counters are accessible after a run.
3. Tests can assert expected behavior in selected dynamic-wave scenarios.

---

## Issue: Design A Structured Runtime State Graph

### Summary

Evaluate an in-memory graph-oriented type system for runtime model state and diagnostics.

### Motivation

There is a reasonable idea here: represent nodes, links, subcatchments, and cross-component relationships as a structured graph of typed entities, with time-varying state attached. This would improve observability, analysis tooling, and possibly GUI/debug integrations.

### Important Constraint

This should start as an internal abstraction or optional adapter, not a hard dependency on an external graph database in the core solver path.

### Recommended Direction

1. Define a lightweight in-memory graph/state model first.
2. Keep it detached from core numerical loops unless profiling proves acceptable.
3. Provide optional adapters for graph database export or external analysis.
4. Treat graph Laplacians and connectivity operators as derived analytics built from the runtime graph, not as mandatory core solver primitives.

### Candidate Scope

1. Typed entities: node, link, subcatchment, gage, storage, pollutant.
2. Typed relationships: upstream/downstream, drains-to, routed-to, attached-to.
3. State snapshots at a reporting period or debug checkpoint.
4. Query helpers for path tracing, bottleneck detection, and mass-balance tracing.
5. Derived structural, flow-weighted, and time-varying Laplacian operators for connectivity analysis.
6. Optional tagging of temporarily disconnected or weakly connected components during routing events.

### Acceptance Criteria

1. A design note defines the graph object model and lifecycle.
2. A prototype can generate a runtime graph snapshot for a small model.
3. The prototype avoids introducing runtime overhead into the hot path unless explicitly enabled.

## Epic 4: Structured Time-Series Export Plugins

### Goal

Support export of model results to structured time-series formats such as CSV, JSON, Parquet, NetCDF, or other analysis-friendly representations.

### Why This Matters

The repository already has strong binary I/O and evolving plugin concepts. Structured export would make the engine more useful for scientific workflows, Python analysis, data engineering, and downstream tools.

### Candidate Formats

1. CSV for broad compatibility.
2. JSON for lightweight API and web tooling.
3. Parquet for analytics pipelines.
4. NetCDF for scientific and geospatial time-series workflows.

---

## Issue: Create A Time-Series Export Plugin Interface

### Summary

Define a plugin or adapter interface for exporting time-series results from in-memory state or output streams.

### Scope

1. Schema for subcatchment, node, link, and system time series.
2. Metadata for units, timestamps, element identifiers, and variable names.
3. Batch and streaming export modes.
4. Compatibility with both legacy output and new engine abstractions.

### Acceptance Criteria

1. A documented export interface exists.
2. One reference implementation is provided.
3. Metadata is explicit and machine-readable.

---

## Issue: Implement First Structured Export Targets

### Summary

Implement initial exporters for CSV and JSON first, followed by Parquet and NetCDF if the abstraction holds.

### Recommended Sequencing

1. CSV exporter.
2. JSON exporter.
3. Parquet exporter.
4. NetCDF exporter.

### Acceptance Criteria

1. CSV and JSON export are covered by tests.
2. Exported output includes units and timestamps.
3. Documentation includes example output and loading examples.

## Epic 5: Metadata And Scenario Extensions

### Goal

Support richer metadata attachment and scenario perturbation workflows without overloading the core deterministic solver with research-specific logic.

### Why This Matters

There is real value in carrying more spatial, temporal, and spatiotemporal context alongside SWMM objects, and in running non-stationary or stochastic perturbation experiments. The key is to do this through typed extensions and analysis layers rather than by overloading fragile legacy text fields.

---

## Issue: Design Typed Metadata Attachments For Objects

### Summary

Design a typed metadata extension layer that allows objects to reference or carry richer spatial, temporal, or spatiotemporal data without forcing all such content into legacy text metadata fields.

### Scope

1. Metadata references for external datasets.
2. Small inline metadata payloads where appropriate.
3. Clear size and serialization constraints.
4. Compatibility with future Python and export tooling.

### Candidate Use Cases

1. Spatial geometry supplements.
2. Temporal forcing annotations.
3. Spatiotemporal calibration priors.
4. Provenance and uncertainty metadata.

### Acceptance Criteria

1. A design note defines the metadata model and lifecycle.
2. The design distinguishes lightweight inline metadata from external references.
3. The legacy text fields are not overloaded with large opaque payloads by default.

---

## Issue: Add Non-Stationary Scenario Perturbation Framework

### Summary

Design a scenario-layer mechanism for applying stochastic or non-stationary perturbations, such as random walks or structured drift, to forcing data or selected model parameters.

### Scope

1. Random-walk perturbations for time series.
2. Non-stationary drift models.
3. Reproducible seed handling.
4. Separation between deterministic solver core and stochastic wrappers.

### Acceptance Criteria

1. The deterministic core solver remains unchanged when perturbation mode is disabled.
2. Perturbation workflows are reproducible.
3. A small example demonstrates non-stationary forcing perturbation.

## Epic 6: Advanced Modeling And Research Extensions

### Goal

Capture promising research directions in a way that is actionable but clearly staged behind verification, diagnostics, and baseline API improvements.

### Why This Matters

Ideas such as antecedent moisture extensions, graph-Laplacian connectivity operators, and uncertainty quantification can add significant value, but they should be developed on top of a trusted solver and a robust introspection layer.

---

## Issue: Evaluate Antecedent Moisture Model Extensions

### Summary

Assess and prototype an antecedent moisture formulation that can augment existing infiltration and soil-moisture handling without destabilizing legacy behavior.

### Candidate Directions

1. Event-conditioned initial moisture states.
2. Antecedent wetness indices tied to prior rainfall.
3. Coupling to infiltration and groundwater state variables.

### Acceptance Criteria

1. A design note explains how the antecedent moisture state differs from current infiltration and groundwater states.
2. A prototype can be run on a small benchmark case.
3. The prototype includes a comparison against current baseline behavior.

---

## Issue: Evaluate Graph-Laplacian Connectivity Analytics

### Summary

Prototype graph-based operators, including structural and time-varying Laplacians, derived from the SWMM network and time-varying hydraulic state.

### Candidate Directions

1. Static topological Laplacian from network structure.
2. Flow-weighted Laplacian from active hydraulic state.
3. Time-varying Laplacian to capture changing connectivity and disconnectedness.
4. Diagnostics for weakly connected or disconnected subnetworks.

### Acceptance Criteria

1. The operator is derived from an explicit runtime graph/state representation.
2. At least one small case demonstrates connectivity changes over time.
3. The implementation begins as an analysis layer, not a mandatory solver dependency.

---

## Issue: Evaluate BME-Style Uncertainty And Operator-Based Inference

### Summary

Assess whether Bayesian maximum entropy style uncertainty formulations or related operator-based approaches are viable as external analysis layers built on SWMM state and connectivity outputs.

### Scope

1. Clarify the mathematical target and data requirements.
2. Identify which solver states and graph operators would be required.
3. Build a minimal proof-of-concept outside the core hot path.

### Acceptance Criteria

1. A research note explains feasibility, limitations, and likely implementation architecture.
2. The first prototype is external to the core deterministic engine.
3. Required diagnostics and exports are fed back into the introspection roadmap where appropriate.

## Epic 7: Canonical Benchmark Library

### Goal

Create a repository of small deterministic benchmark cases that exercise single processes cleanly and repeatedly.

### Why This Matters

This would help more than continuing to add ad hoc comparisons against older versions. Small deterministic benchmarks clarify solver intent and are easier to maintain than large opaque regression files.

### Candidate Benchmark Set

1. Single conduit.
2. Reservoir draining.
3. Force main steady flow.
4. Horton infiltration recovery.
5. Green-Ampt wetting front progression.
6. Storage exfiltration.
7. Outfall depth with non-zero link offset.
8. Elliptical conduit geometry and routing sanity case.

---

## Issue: Build The Initial Canonical Benchmark Library

### Summary

Create a first set of deterministic benchmark models and expected outputs covering the most analytically defensible process modules.

### Scope

1. One model per process.
2. Clear metadata for assumptions, expected behavior, and reference source.
3. Stable input files and expected output summaries.
4. Reusable test harness integration.

### Acceptance Criteria

1. At least six canonical benchmark cases are added.
2. Each case includes a short problem statement and expected result description.
3. CI runs the benchmark set and stores comparison artifacts on failure.

## Recommended Milestone Order

### Milestone 1: Foundations

1. Build the three-layer verification taxonomy.
2. Review changed physics relative to `main`.
3. Add infiltration and geometry analytical tests.
4. Add mass-balance programmatic getters.

### Milestone 2: Process Benchmarks

1. Add outfall boundary-condition tests.
2. Add force main, storage, and culvert benchmark cases.
3. Add initial canonical benchmark library.
4. Add convergence and Courant introspection.

### Milestone 3: Tooling And Export

1. Add structured runtime diagnostics.
2. Prototype the in-memory graph/state model.
3. Add CSV and JSON exporters.
4. Define typed metadata attachments.

### Milestone 4: Broader Ecosystem

1. Expand regression coverage.
2. Add Parquet and NetCDF exporters if justified.
3. Add graph adapters for advanced external analysis if the internal model proves useful.
4. Prototype non-stationary perturbation workflows.
5. Evaluate antecedent moisture, graph-Laplacian analytics, and operator-based uncertainty extensions.

## Closing Note

The most valuable principle here is to avoid mixing too many concerns into a single issue. The verification work should start with compact, high-confidence analytical targets. Introspection should expose what the solver already knows internally. Graph-oriented runtime models, richer metadata, stochastic perturbation layers, and advanced uncertainty analytics are promising, but they should follow a stable verification foundation instead of preceding it.