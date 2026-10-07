# Boston continuity investigation and conservative exchange repair

The saved Boston output has a 2D continuity error of −4.7595% (−62,178.02 m³). This is a historical observation, not a result from the repaired engine. See `findings.json` for the original measured terms and file path.

## Confirmed defect

Outfall exchange previously sampled the last 1D flow over an entire surface batch, capped the aggregate request, and distributed a held sink across cells. Individual donor cells could not honor that distribution, but the exchange ledger recorded the requested amount. A two-cell reproduction requested 1.1 m³, removed only 1.0 m³, and left 0.1 m³ while reporting the full withdrawal. A cap applied after 1D routing also could not undo water already accepted by the pipe network.

## Implemented repair

Before each 1D routing step, reserve available surface cell volumes, sharing overlapping outfall footprints. Divide each node budget among its incident links and bound surface-donor flows before they enter the 1D solution. Dynamic-wave and finite-volume routes enforce the bound; the finite-volume implicit pressure path reapplies it after pressure correction.

After each 1D step, transfer the accepted net node volume directly. Withdrawals follow the reserved donor volumes, positive discharge uses footprint cell areas, and transported mass follows the accepted water. Surface sinks and fluxes subsequently see the remaining water. Both system exchange ledgers use the accepted transfer. Incoming and outgoing 2D totals are booked separately per routing step, retaining reversals inside a surface batch. Current storage is updated with the transfer.

Outfall cells remain eligible for CPU activation without rebuilding the entire active mesh for every exchange. External-volume resynchronization invalidates stale CPU and Kokkos active sets.

This is first-order operator splitting. Equal link reservations can leave unused capacity until the next routing step. Area-weighted positive discharge changes the previous slope-weighted distribution. Conservation does not establish temporal accuracy: routing and coupling step refinement remains necessary for demanding coupled dynamics.

## Validation

All 77 tests passed across outfall coupling (5), CPU correctness (21), decoupled stepping (3), transport S3 (4), transport S4 (10), and finite-volume integration (34). The outfall cases cover overlapping donors, multiple links, both flow directions, transport, competing sinks, immediate/batched surface advancement, and finite-volume implicit pressure coupling. Both Kokkos CPU alignment test processes passed, including external-volume wetting and resynchronization.

The public-API reverse-flow reproduction now removes exactly the available 1.1 m³, leaves zero surface storage, and reports zero 2D continuity error for dynamic-wave and finite-volume routing, with immediate or six-second surface batches. Finite-volume overall 1D error is −1.83e−15 as a fraction. Tests also compare accepted 2D exchanges against the corresponding 1D boundary totals.

Dynamic-wave routing retains a separate overall 1D error of +6.7159% in this abrupt dry-pipe fixture, despite the matching exchange ledgers. This repair does not resolve or conceal that residual.

`fix_validation/` contains test/build logs, before/after fixture inputs and reports, numerical results, and tested source/library hashes. The baseline uses the older GUI-bundled library; the after results use the rebuilt engine library. The shared working tree contains unrelated pending work, so this comparison is not an isolated-commit performance or behavior attribution.

## Remaining validation

The full Boston model has not been rerun, and the GUI application bundle has not been rebuilt or installed. The complete Boston residual cannot yet be attributed to this defect. Actual GPU execution remains untested; Kokkos verification used its CPU backend. Next, deploy the rebuilt engine through the normal application build, rerun Boston with preserved input settings, and compare the separate surface, routing, and exchange ledgers. Investigate any remaining residual independently.
