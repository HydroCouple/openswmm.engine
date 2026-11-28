# Splitting SWMMEngine.cpp

Keep the public C API and the SWMMEngine class interface stable while moving method definitions into smaller translation units under src/engine/core. SWMMEngineCheckpoint.cpp provides an existing example.

| Order | Move | Verify |
| --- | --- | --- |
| 1 | Move initGeometry, initHydraulics, initHydrology and initQuality into initialization files grouped by subsystem. Keep the current call order in init_modules. | Compare validation errors and initialized state on the same input decks. Run hydraulics, runoff, LID and water-quality unit tests. |
| 2 | Move assembleGWCoupling, assembleRunon and assembleLateralInflows with their file-local helpers. Keep their positions in the simulation step. | Compare lateral flows, constituent loads and continuity totals for coupled and uncoupled models. |
| 3 | Move report emission and output scheduling. Keep lifecycle callbacks and clock advancement in place during this extraction. | Compare report timestamps, binary records and checkpoint/resume outputs at the same reporting interval. |
| 4 | Extract runoff and routing step implementations once the source, initialization and output dependencies are explicit. Keep open, initialize, start, step, end and close as the lifecycle entry points. | Compare step times, callback order, return codes and final continuity. Run the unit and regression suites with 2D enabled and build with 2D disabled. |

Use one pull request per extraction. Before moving a group, identify its anonymous-namespace helpers, file-static state and conditional compilation guards. Move a helper with its sole caller. Where several files share it, keep one implementation with a private declaration.

Capture reference outputs on the parent revision using the same compiler, options and fixtures as the extraction. Check that CMake compiles each definition once on all supported platforms. Review any numerical or algorithmic changes separately from the moves.
