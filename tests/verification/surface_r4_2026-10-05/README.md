# R4 authoring qualification — 2026-10-05

The user approved R4-1–R4-4. This evidence qualifies the shared ownership resolver, optional persistence and editing API. Runtime source coupling is still gated; no recharge, weather suppression, accepted-volume allocation or source-clock changes are certified here.

Five suites pass on an isolated source tree containing the task changes added to engine baseline `515be645257b4f0b6ccec9ea68e34c73a18b1f7e`:

| Suite | Cases |
|---|---:|
| Surface ownership | 9 |
| Surface ET budget | 10 |
| Surface aquifer receiving | 16 |
| 2D aquifer | 23 |
| GeoPackage mesh | 10 |
| Total | 68 |

[CTest summary](isolated-tests.log), [ownership case results](isolated-owner-results.json) and [case counts](case-counts.json) record the checks. Dedicated cases cover LID-first allocation and sealed bottoms, half-inside/outside closure, US/SI units, concave/disconnected intersections, large coordinates, missing/mismatched/self-crossing geometry, peer conflicts, cancellation, stale tokens, atomic replacement, absence, lumped precedence, editing-time mesh queries, INP/GeoPackage round trips and the runtime guard.

The task tree was built independently of unrelated working-tree changes. The GUI evidence contains the [source audit](../../../../openswmm.gui/tests/verification/surface_r4_2026-10-05/task-source-audit.json); its `tree` hashes identify the isolated task sources and its `files` hashes identify the inspected working files. Overlapping working files can include unrelated edits, so those two sets intentionally differ. Source isolation excludes the pre-existing uncommitted legacy subcatchment-share resolver.

The verified isolated library and public headers were installed into the existing local `install/lid-nodes` SDK for GUI verification. No new corpus parity or runtime recharge qualification is claimed. Remaining gates and equations are in [the authoring note](../../../plans/SURFACE_PROCESS_R4_AUTHORING_2026-10-05.md).
