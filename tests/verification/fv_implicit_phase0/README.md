# Native FV baseline capture — phase P0

This implements the component-baseline increments of the implicit 1D FV workplan.
It measures existing native `ExplicitFvSolver` behavior with the pressure-only
implicit correction OFF and ON, on the same physical mesh and celerity. It
does not implement the new fully implicit integrator.

The driver reuses the existing analytic FV test mesh builders and the real
engine library. It adds no third-party solver or benchmarking dependency.
The Python script uses only the standard library to build, hash, run and
compare the native cases.

## Build and run

From the engine repository root, configure a separate build. The baseline
target can also be enabled in an existing native build, including a 2D build.

```sh
cmake -S . -B build/fv-implicit-phase0 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_C_FLAGS="-fno-fast-math -ffp-contract=off -fno-math-errno" \
  -DCMAKE_CXX_FLAGS="-fno-fast-math -ffp-contract=off -fno-math-errno" \
  -DOPENSWMM_BUILD_FV_BASELINE=ON \
  -DOPENSWMM_BUILD_GPU_PLUGIN=OFF -DOPENSWMM_BUILD_2D=OFF \
  -DOPENSWMM_WITH_HDF5=OFF -DOPENSWMM_WITH_GEOPACKAGE=OFF \
  -DOPENSWMM_BUILD_TESTS=OFF -DOPENSWMM_BUILD_UNIT_TESTS=OFF \
  -DOPENSWMM_BUILD_REGRESSION_TESTS=OFF -DOPENSWMM_INSTALL=OFF

python3 tests/verification/fv_implicit_phase0/run.py \
  --build-dir build/fv-implicit-phase0 \
  --output tests/output/fv_implicit_phase0/global_euler
```

Use a new output directory on every invocation; existing evidence is never
overwritten. `--cells`, `--celerities`, `--cases`, `--duration`,
`--routing-step`, `--repeat`, `--jobs` and `--timeout` narrow/expand the matrix.
At least two repetitions are required. Inputs and artifacts use internal
feet, square feet, cubic feet, cfs and seconds. On MSVC, use the project's
appropriate compiler FP policy rather than the Clang/GCC flags above.

## Fixture matrix and measurements

The default matrix has ten scenarios, 4/8/16 cells per conduit,
100/300 ft/s requested slot celerities, pressure correction OFF/ON, and two
fresh-process repetitions: 240 runs. Single-reach cases keep a fixed 400-ft
length; each graph case keeps its specified conduit lengths under refinement.
First-order global Euler stepping,
CPU execution and one requested OpenMP worker are fixed. LTS, compaction,
quality and TPA are disabled to isolate this first comparator. These
settings describe a component baseline, not the best production FV mode.

- `open_slosh`: closed rectangular channel, initially perturbed free surface.
  Pressure correction must be inert, including identical sampled cell states.
- `pressure_rest`: closed circular pipe at uniform surcharge. Check rest-state
  depth/discharge and closed-volume conservation.
- `pressure_slosh`: closed circular pipe with a small pressure-head perturbation.
  Check finite admissible states and closed-volume conservation.
- `filling`: initially partly full circular pipe with prescribed stages at
  both ends. Record boundary volume receipts and balance defects; there is no
  independent front-arrival acceptance reference in this initial fixture.
- `pressure_flow`: fully pressurized pipe with prescribed stages of 6 and
  5.5 ft. This measures a pressure-dominated reach without an explicit wall
  face keeping the acoustic timestep bound active. Record boundary receipts;
  qualify independent head-loss/transient accuracy separately.
- `tee_pressure`: one 200-ft inlet and two identical 200-ft outlet conduits,
  joined at an unknown degree-three junction. The source stage is 6 ft and
  both sink stages are 5.5 ft. Check branch symmetry and folded-junction degree.
- `loop_pressure`: a 100-ft inlet, two parallel branches each comprising two
  150-ft conduits, and a 100-ft outlet. The internal fork/merge heads remain
  unknown; the prescribed source/sink do not break the cycle. Check symmetric
  branches and the non-path pressure-solver classification.
- `mixed_reversed`: a 100-ft inlet, 200-ft outlet and reversed 400-ft outlet
  meet at a degree-three junction. Check that the reversed conduit carries
  negative flow in its local frame; lengths remain fixed as cell count changes.
- `tee_rest` / `loop_rest`: the same graph geometries at uniform 5.5-ft head
  and zero discharge. Check cell and junction equilibrium with correction OFF/ON.

Each native run writes:

- `mesh_before.csv` / `mesh_after.csv`: canonical topology, cell lengths/beds,
  face orientations, node/conduit mappings, friction and section-closure data
  for this analytic, single-section, no-structure fixture family. There are no
  raw struct dumps, pointers, padding or host-order binary fingerprints.
- `states.csv`: every cell's area, discharge and depth at initial/routing times.
- `nodes.csv`: every node's head, published volume, signed continuity carry,
  prescribed-head status and most recent pressure-fold classification.
- `routing_steps.csv`: accepted solver-returned time, native internal-step
  count/last step, timed solver advance, cell volume, prescribed-boundary
  exchange, balance defect and sampled pressure/transition occupancy. Node
  storage, negative carry, cumulative exported flooding and folded degree are
  included in summary schema version 2.
- `summary.json`: physical checks, step counts, native dt-argmin counters,
  sampled slot share and elapsed solver time.

The runner retains `build.log`, compile commands/cache, source hashes before
and after build/runs, binary/local engine-library hashes, platform/environment
information, per-run logs, all summaries and mode comparisons. It rejects
source drift, mutable binaries/libraries, changed meshes, repeat state
mismatches, free-surface option non-inertness, cell/node/boundary conservation
failures, graph symmetry/direction errors, missing folded degree-three witnesses,
unexpected graph flooding, and rest-state failures.
Failed runs retain their output and a failed manifest; they are not successful
baselines. Build configuration and actual compile invocations remain reviewable.
`source_snapshot/` preserves each inventoried source/configuration file and
`artifact_snapshot/` preserves the driver and local engine library, with copied
bytes verified against their hashes. These are evidence copies, not a portable
runtime bundle; runtime dependencies and executable permissions are not packaged.

## Interpretation and remaining P0 work

For these fixtures, external volume is the sum of receipts at prescribed-head
nodes only. Internal junction receipts are not treated as external flows.
Inventory is cell water plus nonprescribed published node volume plus negative
continuity carry; positive carry is already published and counted once.
Exported flooding is added to cumulative outputs. Carry is reported as a
numerical ledger item, not physical storage or a substitute for convergence.
No lateral/structure/loss source is present in this matrix. General production
inventory and independent transient accuracy remain broader qualification work.

The two native read-only diagnostic accessors add no state or virtual methods
and change no integration equations. A degree-three folded-junction mask in an
all-pressurized fixture witnesses the non-path branch of the current pressure
solver by source inspection. It does not provide Krylov iteration or true
linear residual histories. The mask is historical when no pressure pass runs;
these graph witnesses are used only with pressure correction ON and persistently
pressurized regimes.

The graph-rest checks exposed cancellation when the existing pressure solver
assembled absolute heads. Its native scalar, chain and PCG paths now solve for
head relative to a common datum and restore absolute heads afterward. The matrix
and mathematical storage/flux equations are unchanged; floating-point results
with pressure correction ON can change. Dedicated regressions check equilibrium
and elevation-datum shifts. This precision repair is separate from the planned
nonlinear pressure-storage and failure-status work.

The native dt-argmin counters are census observations, not residence times.
Pressure occupancy, transition faces, regime changes and peak slot share are
sampled at routing endpoints; they can miss within-step events and do not
distinguish standing from advancing fronts reliably. The timed region covers
the native `advance()` call; writing/scanning diagnostics is outside it.
These short instrumented runs on an uncontrolled host support correctness and
work-count comparisons, not accepted end-to-end performance speedup claims.

Complete P0 by recovering representative engine-model manifests and units,
adding real-model storage/structure coverage, qualified LTS/RK2/closure
comparators, internal front/junction attribution, general boundary/carry inventory
audit, and matched-accuracy performance/reference gates. P0.5 then derives and
qualifies the native nonlinear pressure-storage/status change. Neither phase
is declared complete solely because this component matrix passes.
