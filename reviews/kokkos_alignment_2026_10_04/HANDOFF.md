# Kokkos alignment handoff — 2026-10-04

## Status and scope

The existing triangle-only, water-only local-inertial Kokkos marcher now
implements the CPU corrections listed below. A separate experimental
`KokkosSweKernels.hpp` supplies tested reconstruction, interior-face,
gather, wall/momentum and ledger kernels for a future FULL_SWE driver.
**It is not a complete or selectable Kokkos FULL_SWE backend.** Factory
eligibility is unchanged: full SWE, diffusive wave, mixed/quadrilateral
meshes and transport still use the CPU marcher. The direct Kokkos
initializer now rejects unsupported momentum, transport and cell shapes
rather than silently dropping their physics.

No CUDA/HIP/SYCL compiler or GPU execution was available. OpenMP host
compilation, execution, bounds checking and sanitizers do not establish
device compilation, execution-space ordering on a GPU, performance,
register usage or numerical tolerance portability. No GPU speedup is
claimed. The changes and validation evidence are prepared for review by the next agent.

## Changed implementation

`src/engine/2d/gpu/ExplicitKokkosSurfaceSolver.cpp` and `.hpp`:

- Frozen-tier CFL refresh divides the current cell bound by its tier
  multiplier before reducing the base step.
- Faces fire at interval entry and cells consume transfers at interval
  completion, using `(substep + 1) % (1 << tier)` for cells.
- Final partial windows march all compact segments with global positivity
  budgets, retaining the tier assignments and CPU rebuild cadence.
  Previously the Kokkos tail collapsed tiers and forced an extra rebuild;
  the new cross-backend test detected divergent fields in the next window.
- Pending face transfers settle before final lazy sources and publication.
  Quiescent rain/coupling strides force reconsideration of the active set.
- `KokkosSourceKernels.hpp` applies one finite available-water budget after
  face volume lands. Rain/coupling inflow and existing storage fund
  proportionally shared infiltration, evaporation and coupling withdrawal.
  Gross transfers are booked even when net volume change is zero. Applied
  infiltration/coupling and evaporation are recorded; complete exhaustion
  produces exact zero volume. This is water-only; groundwater return and
  transported species are not implemented by this helper.
- Applied evaporation accumulates per device cell, drains once per publish
  in fixed host cell order and is reset. This adds one double per cell on
  device and one host scratch double per cell, plus a copy and reset per
  publication. It deliberately avoids introducing a floating-point device
  reduction; its cost needs GPU profiling.
- Halo membership is gathered per cell from immutable neighbour seed
  flags. The former concurrent `active(neighbour)=1` writes were data races
  even when all writers stored the same value.
- Direct initialization rejects non-LI momentum, transport or nontriangles.
  No plugin ABI version, factory selection, public engine option or CPU
  solver implementation changed.

`src/engine/2d/gpu/KokkosSweKernels.hpp`:

| Entry point | Contract |
|---|---|
| `gradients` | Exact CPU stencil rules: eta/u/v/h Green–Gauss in connected wet interiors; weighted eta/depth shoreline fit; determinant/trace conditioning; cell-centred shoreline velocity; connected-neighbour BJ limiting and all-face depth positivity scaling |
| `faces` | First-order or reconstructed-bed SWE flux through shared `SweKernels.hpp`; conveyance, exporter positivity budget, equal/opposite water booking and side-specific bed-pressure corrections, including blocked sills |
| `gather` | Cell-owned CSR gather and clear of water/momentum transfers; updates volume and returns integrated momentum increments |
| `momentum` | After closure/source update, mirror-WALL Riemann pressure, new-depth momentum/friction update and dry-momentum reset |
| `averageLedger` | Initial ledger plus half of the accumulated two-stage increment; this is not a complete RK2 state update |

The interfaces capture Kokkos views by value; no host mesh pointers are
captured in device lambdas. They use the selected `ExecSpace`/`MemSpace`.
Geometry uses compact interior faces and four padded slots per cell,
with triangle/quad vertex counts. Gradient arrays each contain four
cell-count blocks in order eta, u, v, depth. This differs from the CPU's
split-vector naming but preserves per-variable coalesced storage and
arithmetic. Mesh mirrors and buffer allocation/lifecycle are currently
provided by the test adapter, not wired into the production marcher.

Callers must supply valid extents, unique active lists, valid CSR
orientation, positive cell areas, geometry computed with the same
midpoint arithmetic as the CPU, and tier differences valid for refire
shifts. Views must remain alive until queued work completes. Stages use
the default execution-space ordering; any future use of multiple stream
instances needs explicit dependency/fence design. The shared SWE header
must receive its device annotation before its first inclusion in a device
translation unit; the new header establishes that annotation locally.

## Validation performed

Kokkos 4.7.4, Apple Clang, Apple ARM64, Release, strict floating-point
settings (`-fno-fast-math -ffp-contract=off`), Kokkos/OpenMP. The CPU
reference uses one thread; Kokkos executes at one and four threads in
separate CTest processes. The comparison target links the production
solver/mesh components directly, independent of the dirty full-engine
build. The existing four-line CPU rain-reactivation working change remains
in the reference and was not edited by this work. Source hashes and
command/output records accompany this file.

Per test process:

- 20 complete marcher configurations: FLAT on a flat mesh and VFR on a
  sloping mesh; one/four tiers; dam-break wetting, competing sinks,
  initially dry rain, balanced rain/infiltration, and flow with
  infiltration. Each spans three successive publication windows.
- Full per-cell volume, infiltration and coupling comparisons, evaporation
  totals, plus independent closed-water, rain-volume and competing-source
  conservation checks. Marcher tolerance is `2e-9 * (1 + abs(reference))`;
  independent conservation checks use `2e-11 * (1 + abs(reference))`.
- 24 SWE configurations: distorted triangles/quads/mixed meshes; smooth
  wet, dry shoreline, thin-film/large-step positivity, and stepped-head
  with partial-conveyance cases; orders one and two. Compare every
  gradient component and face water/momentum transfer with the CPU;
  then compare gathered volume and complete source-free Euler cell
  momentum including wall pressure and Manning friction. Kernel tolerance
  is `2e-11 * (1 + abs(reference))`.
- An explicit frozen-coarse-tier CFL regression after increasing its
  depth; three unsupported-initialization checks; a signed/nonzero-initial
  RK ledger averaging check.

Both CTest processes pass. The same processes pass with AddressSanitizer,
UndefinedBehaviorSanitizer and `KOKKOS_ENABLE_DEBUG_BOUNDS_CHECK` enabled.
Leak detection is disabled for the sanitizer invocation; the prebuilt
Kokkos/OpenMP libraries themselves are not sanitizer-instrumented.
The standalone OpenMP plugin is also compiled and linked as a smoke check;
this does not certify dynamic loading through the full engine.

The old standalone `test_kokkos_rhs_parity` references a removed
`KokkosSurfaceKernels.hpp`. Its CMake target is now conditional on that
header existing; it is not counted as a passing current test. The new
`test_kokkos_explicit_alignment` exercises the current marcher instead.

## Reproduction on this host

From the engine repository root (substitute the installed Kokkos prefix):

```sh
cmake -S tests/gpu -B build/kokkos-cpu-alignment \
  -DKokkos_ROOT="$PWD/build/darwin-parity/vcpkg_installed/arm64-osx" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/kokkos-cpu-alignment -j 3
ctest --test-dir build/kokkos-cpu-alignment --output-on-failure
```

For the instrumented host build, use a separate build directory and add
`-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -DKOKKOS_ENABLE_DEBUG_BOUNDS_CHECK"`
and `-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined`; run CTest with
`ASAN_OPTIONS=detect_leaks=0`. These are host flags, not CUDA/HIP/SYCL flags.

## Next agent: completion order and acceptance gates

1. **Compile on each intended device toolchain first.** Use a matching
   Kokkos CUDA/HIP/SYCL installation and architecture. Audit shared math
   helpers, annotation/include order, lambda captures, warnings and
   generated device code. Run the standalone alignment target on the
   actual execution space, then device memory/race checkers. Do not relax
   tolerances simply to obtain a pass; explain arithmetic differences.
2. **Wire a water-only SWE stage driver behind an explicit experimental
   gate.** Add persistent mirrored geometry, state, gradients and transfer
   buffers. For each Euler stage: gradients → interior transfers → cell
   gather → shared source budget → FLAT/VFR closure → wall/friction
   momentum → non-wall boundaries/live exchange. Support tri/quad closure
   correctly, including quad VFR. Sources and closure are not supplied by
   the experimental SWE kernel header. Preserve the CPU's ordering.
3. **Complete SSP-RK2 and time/active-set integration.** Snapshot volume,
   both momenta and every ledger before both stages; average active-cell
   conserved state, refresh closure, reset dry momentum, and average
   infiltration, coupling, boundary exchange, node drawn/spill budgets and
   evaporation consistently. `averageLedger` alone is not this driver.
   Keep second order global-step and reject groundwater as the CPU does.
   Port active-front rebuild/breach behavior before enabling the SWE path.
   Explicit `FRONT_REBUILD YES` parity in the existing LI plugin is also
   outside this pass; the default LI one-ring path is what was tested.
4. **Validate full physics through the plugin and engine API.** Add
   specified-stage/flow/rating and normal boundaries; live shared-node
   coupling, hot starts/reinitialization, failed-window volume resync,
   output/report ledgers and lifecycle tests. Groundwater, diffusive wave
   and surface species remain CPU-only until separately implemented and
   qualified. Do not lift factory restrictions from kernel tests alone.
5. **Run analytical/refinement and coupled matrices on hardware.** Reuse
   the phase-10/12 SWASHES-family fixtures: wet/dry lakes, Ritter/Stoker,
   planar/radial Thacker, mixed/distorted meshes, three/ten periods and
   source competition. Compare histories, phase/velocity, positivity,
   water/species accounting and outputs; include FLAT/VFR and wet Bellinge.
   The current tests are component parity, not full SWE time integration
   or an analytical GPU qualification.
6. **Profile only after correctness.** Measure transfers, launch overhead,
   register spills, branch divergence and active-list compaction. Reuse
   buffers, avoid copying the CPU's worker-private species ledgers directly
   to every GPU thread, and benchmark layouts before choosing species
   gather/fusion thresholds. Compare GPU/CPU at equal accuracy and include
   initialization/output in end-to-end timing. No new performance result
   is supplied by this pass.

Keep the supported backend restrictions until the corresponding gates
pass. Update engine/hydraulic manuals when a complete new backend becomes
available; their current CPU-only descriptions remain correct.
