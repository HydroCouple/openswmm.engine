# R4 source clocks and forcing qualification

The internal connected-source clock/forcing adapter and selected runoff batch pass **138 cases in seven isolated suites**, including **16 new cases**. [Test summary](isolated-tests.log), [case counts and hashes](case-counts.json), [new case results](isolated-results.json) and [all final per-case results](isolated-final-cases/) record the final passing run. Earlier local fixture runs are retained in the ignored `isolated-cases/` folder; the final results above are the qualified evidence.

The [source audit](source-audit.json) identifies the baseline commit and task-only source tree. Working and isolated CMake hashes differ because unrelated working registrations are excluded. The reused isolated build source matches the archived task code and build files. The engine and GUI shared-checkout edits and staging are preserved; this tranche does not replace the SDK or GUI application.

[The real-engine clock probe](clock.json) observes routing completed through **0.5 s**, legacy runoff evaluated through **300 s**, and the staged source group committed only through **0.5 s**. The probe uses the routing completion as a caller-supplied qualification horizon. There is no production scheduler caller yet, and a marker commit is not a receiving/source-state transaction.

[The runoff refinement probe](refinement.json) compares 10, 5 and 2.5 s intervals with a 0.25 s reference. The completed-source path uses storage-balance discharge, closes interval water balance at roundoff and converges under refinement. It covers one sloping non-LID kernel; it excludes mesh, routing, aquifer clocks, source quality and whole-model costs.

Coverage includes transitive/cyclic runoff and LID-drain groups, node precedence, independent clocks, cancellation/retry, no replay, immutable source forcing, subsecond future-rate exclusion, US/SI rain formats and cumulative resets, rain-file scaling, step PET, dry-only/prescribed PET, monthly patterns, named unsupported-history refusals, selected native soil behavior, untouched peer states, private bounded source trials, mixed-area/instant-drain conservation and one sealed storage-LID rain fixture.

The existing ownership initialization guard remains. LID capture/drain/return drivers, transfer histories across changing cadences, source quality, area/weather/ET partition, numerical refunds, restart clocks and runtime results remain pending. See [the implementation note](../../../plans/SURFACE_PROCESS_R4_SOURCE_CLOCKS_2026-10-05.md).
