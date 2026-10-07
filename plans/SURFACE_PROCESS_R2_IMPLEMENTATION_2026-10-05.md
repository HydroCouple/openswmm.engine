# R2: aquifer-owned surface infiltration — 2026-10-05

The user authorized implementation after the ownership review with “Proceed”. This round implements the first R2 tranche of the original surface–subsurface program. The optional semi-discrete Richards LID solver and the existing default LID formulation remain available. R2 does not change the aquifer into a full Richards solver.

## Ownership and migration

Effective aquifer coverage follows the existing solver: authored aquifer rows activate a whole-mesh state seeded with implicit aquifer defaults. Zero conductivity and missing explicit cell rows do not remove coverage. With infiltration enabled, covered cells are EXTERNAL entries in the shared infiltration bank. Wildcard and TAG surface defaults skip them and report matching applied/skipped counts; explicit surface cell methods fail initialization atomically. External-only meshes work without ordinary infiltration rows. Explicit infiltration OFF publishes no receiving capacity. Aquifer OFF leaves the ordinary bank active.

AQUIFER_2D remains readable authoring syntax, but initialization rejects it. New ordered authoring APIs preserve defaults, duplicate records, override order and destination presence. Their batch replacement validates all records before writing; it can restore obsolete destinations for migration Undo. Ordinary setters retain their existing validation. Bulk ownership queries provide owner, aquifer-row source and conflict before initialization.

## Finite receiving volume

For cell area A, thickness L = max(zs − hg, 0), define W = theta_s hg + S_col. Maximum physical storage is theta_s zs, so the physical deficit is

    H = max(A (theta_s L − S_col), 0).

S_col is hu for CLOSED_FORM, the actual integral of the sigma-layer contents for SIGMA, and the soil law's exact equilibriumStorage(L) for ENSLAVED. SIGMA intake also respects the existing top-layer deficit: A L/m (theta_s − theta_top). A zero-length column stores zero water; a numerical denominator floor does not create physical storage.

Subtract committed positive receipts from surface, lateral faces, links and node beds. Do not credit requested future withdrawals or net positive and negative pending channels. The remaining allowance is recomputed from physical state and these reservations; refreshing a held rate cannot reset spent storage. Surface acceptance reserves the actual volume before surface removal, books that volume once, and leaves denied water on the surface. Transport follows the accepted water. Sealed and uninitialized interfaces cannot accept delivery.

The published rate is the lesser of the candidate top-interface law and H_remaining/(A interval). It is held on the existing INFIL_STEP cadence and refreshed after the receiving cell fires. The interval, remaining volume, pending surface volume and refresh time are reported separately from accepted infiltration.

## Closure-specific prediction

All quantities at this boundary are SI. Suction is a nonnegative magnitude; ponded depth is nonnegative.

* SIGMA: use the existing saturated wet-side interface conductivity Ks over the top half-layer path, q = Ks [1 + (pond + psi(theta_top))/(L/(2m))]. This is a receiving prediction for the existing sigma column, not an additional Richards boundary solve. The actual top-layer storage cap prevents advance credit for future internal drainage.
* CLOSED_FORM: delta = theta_s − hu/L; derive suction from the existing retention law at bulk effective saturation. The instantaneous law is q = Ks [1 + (pond + psi) delta/F]. The held-interval prediction solves its integral for deltaF, using B = (pond + psi) delta and deltaF − B log(1 + deltaF/(F+B)) = Ks interval. This gives a finite first intake at F = 0 without an arbitrary initial front or a storage-sized startup pulse. A bracketed Newton solve with a stable small-argument logarithm remainder returns a conservative lower bracket if iteration is limited. F grows by accepted depth only. After a column firing, shrink F only when the actual deficit exceeds the deficit expected after accepted infiltration, using expected_deficit/actual_deficit. Recovery therefore follows restored pore space rather than a new arbitrary drying clock.
* ENSLAVED: q = Ks (1 + pond/L), limited by the exact nonlinear equilibrium storage deficit. At L = 0, every closure publishes a valid zero.

The deep-column test compares integrated intake to the independent analytic Green-Ampt relation (F−F0)−B log((F+B)/(F0+B)) = Ks t, with slowly varying bulk moisture. Other tests demonstrate declining SIGMA intake as the top wets and declining bulk allowance as the table rises. These establish controlled behavior, not universal field accuracy of the existing aquifer closures.

## Numerical gaps exposed by receiving limits

Physical column storage no longer includes the old artificial 1 mm minimum thickness. An absent unsaturated column has no internal recharge flux. Sigma ALE handover uses the same floored specific yield as the saturated update; its compression/stretch correction accounts for the final profile's mean content and the actual bottom-flux availability limit. The existing node fill cap now uses physical receiving headroom and pending receipts. This prevents the near-saturated bed from repeatedly accepting and returning water. A bed-only fixture isolates that intake limit from genuine neighboring-cell lateral saturation excess.

Rejected top delivery and physical Dunne excess have separate water ledgers and results; both retain the existing equal/opposite return and transported-mass path. Transport comparisons include pending downward mass as well as upward mass. Genuine saturation-excess tests use independent link/lateral forcing, since a finite receiving interface intentionally removes the old forced-infiltration/refund trigger.

## Restart and output

V12 hotstart appends held capacity, remaining allowance, refresh/interval, front and return diagnostics, pending water and species accumulators, and corresponding surface intake state. Matching dimensions and groundwater species order are checked before any hotstart mutation. A mismatched V12 interface is refused rather than partially applied. V1–V11 remain readable; older aquifer files warn that they lack held/pending interface continuation. The controlled restart test retains a pending tracer receipt and compares subsequent groundwater firings exactly. It does not claim a bit-identical whole-network simulation-clock restart across different model periods.

Seven additive cell-result selectors and a rejection-ledger selector feed the snapshot and HDF5 output. Existing snapshot field offsets remain unchanged; new vectors are appended. Capacity, remaining allowance and refresh time use NaN when unpublished/OFF; a physically zero capacity is finite zero. The reader preserves these meanings. Existing applied-depth results remain the accepted-water measure. Actual sigma profiles retain their existing output; bulk closures do not synthesize layer profiles.

## Verification and scope

Review artifacts are under `tests/verification/surface_r2_2026-10-05/`. `test_summary.json` records the shared-checkout checks; `isolated/test_summary.json` records HEAD plus only this task's patch, excluding unrelated local edits. The isolated source and before snapshots are local build aids, not source changes for this commit. `task.patch`, source hashes, API trajectory summaries and corpus provenance provide attribution. The unit fixtures cover 3 closures × 4 soil laws, equivalent US/SI meshes and trajectories, sealed/saturated cells, shared reservations, accepted drainage, external-only operation, ownership conflicts, ordered replacement and pending-tracer restart.

The GUI implementation and screenshot are recorded in the companion R2 review. This is the mesh-surface intake tranche. R4 still owns subcatchment-area/supply partitioning; R5 still owns further LID/node/pipe interface enrollment and contact area. R3 shared ET, double-stress correction, default changes and UEB remain subsequent reviewed rounds. This work adds no partial-aquifer coverage token or new soil calibration knob.

Final measured checks: the isolated engine passes 125 tests (16 receiving, 21 infiltration, 22 aquifer, 36 hotstart, 30 transport) and all groundwater verification gates. Its 25 explicit manifest outputs and 24 no-aquifer/OFF API trajectories are byte-identical to the separately built base engine. The shared engine checkout passes 128 tests; its three additional hotstart cases belong to other local work. These checks do not include unqualified UEB reference comparisons or the future ET allocation change.
