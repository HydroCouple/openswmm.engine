# Disconnected outfall coupling — 2026-10-05

User-approved behavior: warn and ignore 2D couplings to outfalls that have no incident hydraulic link, rather than aborting 2D initialization. Follow-through to the GUI Boston diagnosis `workplans/BOSTON_OUTFALL_INITIALIZATION_2026-10-05.md`.

## Implementation

- After resolving coupling points, derive incident-node membership from every hydraulic link in one linear pass.
- Remove only outfall coupling points whose node has no incident link, before tailwater, donor-reservation, output and solver setup. Warn once per affected node with its ID and explicitly state that its 2D mappings are ignored.
- Retain the 1D node, forcing definitions and authored mesh/maps. Ignore applies at runtime; saved model files are not rewritten.
- Keep the reservation helper's invariant check for incorrect direct internal callers. Normal engine initialization supplies only valid outfall points.

## Verification

- Add an end-to-end regression before modifying production code; reproduce initialization failure on vertex, cell, repeated mappings and all-disconnected coupling sets.
- Confirm a named warning exactly once per node, successful initialization and completed runs.
- Compare connected-outfall exchange and continuity exactly against the same model without disconnected mappings.
- Run the existing complete outfall coupling/conservation suite, including reverse flows and overlapping donor budgets.
- Build and verify the engine and refresh the GUI's bundled engine for testing if package compatibility is confirmed. Preserve other agents' changes and all original Boston files.

Status: implemented. Standard engine build succeeds; six outfall tests and four conservation tests pass. The new regression fails on the original engine and passes with the fix, including exact comparison of valid connected-outfall exchange/continuity and one warning for repeated maps. Named warnings are verified in the generated reports.

The installed `install/lid-nodes` engine and main GUI bundle have been refreshed with the tested engine. Their earlier binaries are backed up under this artifact folder; replacement is atomic so existing loaded instances retain their previous library mappings. Strict/deep GUI bundle signature verification passes. A small fixture using all 17 audited Boston outfall IDs initializes with exactly 17 named warnings, even with a repeated mapping. A fresh main GUI instance was launched and confirmed running (PID 98345). Full Boston simulation throughput and later independent model errors are not certified.

An initial baseline compile encountered an in-progress runoff signature edit; a later configuration referred to a source-clock test file before it was created. Focused validation used the existing generated build graph during that interval. After the concurrent files were available, the normal CMake build succeeded and both suites were rerun on that result; the final package uses that standard build.

Evidence is in `artifacts/disconnected_outfalls_2026-10-05/`: baseline failure, standard build, outfall/conservation test logs, named Boston fixture/result, original binary backups, package signature/update record, and the isolated implementation patch. Original Boston model files were not edited.
