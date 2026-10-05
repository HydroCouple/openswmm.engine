# R4 receiving transaction and source-boundary qualification — 2026-10-05

The user authorized proceeding from the R4 authoring tranche. This implements the receiving transaction and pre-withdrawal kernel boundary in step 2 of the [approved review](../../openswmm.gui/workplans/SURFACE_SUBSURFACE_R4_AREA_REVIEW_2026-10-05.md). It does not activate general recharge or complete the production source-clock adapter.

## Implemented and checked

`SubsurfaceSolver::sourceInfiltrationCapacity` evaluates the existing closure/soil law with the actual donor pond head without advancing its front, held rates or pending water. Interface capacity and physical headroom are separate. Mesh rate publication continues to cap that rate by full-cell headroom on its existing cadence.

`SurfaceExchange` accepts one contiguous completed interval and gathers mesh, non-LID and LID-bottom requests by stable donor/unit/cell identity. It bounds requests by source-head interface capacity and one donor water ceiling across cells, then apportions receiver headroom proportionally. Headroom includes pending positive surface, lateral, node and pipe arrivals through the existing receiving calculation. Requests cannot overbook contact area. Denied volume is not migrated to another cell. Long-double accumulation and deterministic rounding keep donor/receiver sums within their ceilings.

Planning changes no water. Settlement validates every actual volume and species tuple, refreshes receiving capacity/headroom, and books nothing on failure. The source driver must cancel/recompute a stale trial before installing source state; it must not debit first and lower a receipt afterward. Unused awards expire. Only accepted volumes advance the shared front. Receipts carry donor kind/ID/unit/cell, interval, water and actual donor-pool masses.

The runoff kernel has an optional whole-pervious-area boundary before infiltration subtraction and pond/runoff integration. A selected external boundary replaces the native kernel and does not advance native soil state; lumped aquifers retain precedence. The default remains the native path. The current boundary does not yet split an inside/outside pervious area. A separate actual-pervious evaporation volume avoids using legacy `Vpevap`, which includes impervious evaporation, in a future spatial ET budget.

Reviewed non-LID SI areas can be supplied explicitly at runoff initialization, with a consistent full-footprint runoff-rate denominator. This removes legacy rounded LANDAREA constants only for those sources. Both US and SI legacy constants have a measurable area discrepancy; ordinary sources keep their original conversion. The engine resolver, not an additional GUI area control, will supply these areas.

Fifteen cases pass: completed/contiguous intervals, replay/cancel, donor heads across 3 closures × 4 soil laws, shared headroom, dry donors, unequal water-limited requests, ordering invariance, one donor spanning cells, stale capacity/headroom, unused awards, contact/identity validation, paired pending mass, closed saturation, actual aquifer firings, two real bounded runoff sources, unchanged native trajectories, component evaporation, one real storage-LID bottom limit, US/SI reviewed areas and the clock/work-count probes. Six suites pass in the shared checkout. Isolated evidence is recorded in [the evidence folder](../tests/verification/surface_r4_exchange_2026-10-05/README.md).

## Measured clock integration gap

An actual engine run, observed through its SAVE RUNOFF record, evaluates runoff through 300 s while its first routing step has completed 0.5 s. The approved completed-interval contract cannot use that runoff state for current recharge. `SWMMEngine::stepRunoff` rolls all subcatchments/LIDs, advances gage/climate cursors, executes global source/quality/GW bookkeeping and interpolates outlet/drain rates from one shared pair of runoff clocks. The marcher separately completes local cell source intervals, including lazy inactive-cell sources and different LTS tiers.

The transaction's completed-time argument is a caller assertion; it is not proof of production forcing or scheduler alignment. The kernel fixtures supply completed intervals explicitly. No production caller installs the new boundary or exchange transaction yet. Production initialization still refuses ownership records with the existing actionable qualification error.

A two-flat-source, fixed-forcing, zero-intake probe compares one 300 s solve with 600 solves at 0.5 s. It checks storage agreement and records calls and wall time. It excludes hydraulics, quality, gage/climate sampling, mesh/GW work and reporting. The 600-fold call count is not a whole-model runtime estimate.

## Remaining implementation under the approved design

1. Form the connected hydrologic source graph, including upstream runoff and LID drains/returns; isolate its clocks and gage/climate/forcing state from unaffected sources. Preserve existing runoff interpolation and evaluation cadence for unaffected graphs. Do not globally shorten WET_STEP or book future losses.
2. Land completed source intervals at the mesh source stage and gather mesh/non-LID/LID claims before any corresponding debit. Qualify lazy sources/LTS receivers, outside/native/lumped areas, actual per-unit LID bottom caps (including no-storage and sealed types) and component atmospheric budgets. The storage-trench fixture alone does not qualify other LID types or capture/drain graphs.
3. Pair actual pollutant withdrawal at the donor pool with its receiver receipt; distinguish external losses from internal recharge in the source/combined ledgers. The transaction's supplied-mass check does not implement those source-quality kernels.
4. Preserve donor receipts until column consumption and return numerical rejection to its originating source. The current groundwater backstop still returns to the generic mesh accumulator; the receipt list here is not integrated with that path or hot starts. Add restart clocks, pending tuples/ET and no-replay markers before activation.
5. Qualify weather suppression, all ET stores, unsupported snow/heat/age/MSX/USE RUNOFF diagnostics, independent timing refinement, realistic performance and an unchanged-control census. Then remove the runtime guard and expose recorded results through the approved GUI.

R4 is not complete. The existing LID-node/Richards alternative, R5 bottom adapter and UEB sequence retain their previous scope and order. No new input option or schedule knob is introduced by this tranche.
