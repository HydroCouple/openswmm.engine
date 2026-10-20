# Coupled cell-area/discharge residual — bounded P1 increment

`ImplicitReachSolver` is a standalone native backward-Euler experiment on the
existing subgrid. No engine input, factory, plugin, binding or GUI selects it.
The pressure-storage candidate and the ordinary production pressure path retain
their previous behaviour. This implements the reviewed workplan's primary
fine-grid residual approach instead of replacing cell momentum with face-flow
averages after a pressure solve.

The solver uses the actual old conserved area and discharge, existing SLOT
closure and HLL mass/momentum flux at the new iterate, plus the Manning source
law evaluated at the new state. Each interface has one flux record. Both
integrated equations must converge before the cell state and face ledger are
returned. There is no area or momentum repair after convergence. Failed solves
leave the caller's endpoint unchanged; this is not an engine-wide checkpoint.

The first support contract is deliberately narrow: a flat, prismatic, single
closed-section reach, every cell and prescribed boundary above the slot taper,
first-order spatial reconstruction, Manning friction, walls or prescribed-stage
boundaries with extrapolated interior velocity. Mesh lengths may vary. There
are no unknown node heads, transitions, dry cells, TPA, force mains, structures,
losses, lateral forcing or transport. Reversed network frames and nonprismatic bed/geometry corrections are
handled by the separate network prototype described below. Coordinate reflection
is tested independently.

The Jacobian is assembled in O(N) from local finite differences, with analytic
storage/history derivatives, physical row/column scaling and re-evaluation of
the real closure/flux at every residual trial. The native O(N) block Thomas
kernel pivots within each 2x2 block and checks the true linear residual. Unsafe
elimination now falls back to native restarted GMRES with fixed right
preconditioning. A conduit-line factorization is the default preconditioner;
block Jacobi remains a comparator. Unsafe line pivots are shifted only in the
preconditioner, with the original matrix retained for every true residual.
General interface tree elimination, block ILU and FGMRES remain open; conduit
condensation is described below, and a native
scalar ILU(0) graph comparator is supplied by the separate network prototype. This
finite-difference reference prototype is not the final derivative implementation.

Use the strict-FP build in the [P0 README](../fv_implicit_phase0/README.md),
with unit tests and local Google Test enabled as in the
[adapter README](../fv_implicit_phase05/README.md):

```sh
python3 tests/verification/fv_implicit_phase1/run.py \
  --build-dir build/fv-implicit-phase0 \
  --output tests/output/fv_implicit_phase1/coupled_review
```

The output directory must be new. `--smoke` runs a smaller hydraulic matrix.
The full capture now runs 101 native Google Test cases (13 coupled-reach,
10 GMRES and the preceding 78 selected regressions), plus 61 hydraulic runs.
For the linear-solver increment use:

```sh
python3 tests/verification/fv_implicit_phase1/run.py \
  --build-dir build/fv-implicit-phase0 --gmres \
  --output tests/output/fv_implicit_phase1/gmres_review

python3 tests/verification/fv_implicit_phase1/review_gmres.py \
  tests/output/fv_implicit_phase1/gmres_review
```

This profile compares 52 direct/GMRES runs, including restarts 20/40/80, and
retains three separate Jacobi diagnostics, including explicit failed solves.
Failed diagnostics are never counted as completed hydraulic qualification.
Final state comparisons use 1e-8 ft head, 1e-10 ft² area and 1e-8 cfs discharge
absolute tolerances; original nonlinear/physical budgets remain unchanged.
The Newton wrapper tightens GMRES's absolute floor to one tenth of the smallest
normalized physical row budget, preventing a zero correction from being
accepted while the nonlinear endpoint is still outside its budget.

The capture requests one worker,
clears development overrides, saves build flags, hashes/copies actual sources
and artifacts, and retains all logs, states, physical mesh fingerprints,
boundary receipts and residual metrics. Later checkout changes are recorded
separately; unchanged artifacts and retained source bytes identify the result.
The source inventory is checked before and after build/copy. It includes
unrelated engine code because the shared native library consumes it.
The GMRES audit also checks 16 direct-mode state/mesh controls against the
retained preceding P1 capture; use `--reference PATH` to locate that evidence.

Hydraulic units are ft, ft², cfs and seconds. The acoustic reference uses
`c² = g*A_baseline/T_slot` from the actual full HLL operator, including stored
slot area. The earlier pressure-only qualification used capped conveyance area
for its speed reference; old scores must not be silently substituted for this
new reference. Both modes in this capture use the same new reference and mesh.
Wave RMS/peak and small-timestep momentum changes are independently recomputed
from saved cell CSVs. Wave refinement and timestep sweeps are accuracy evidence,
not matched-accuracy performance comparisons.

Completing these runs does not promote either candidate to production. The
original hydraulic accuracy, feature, rollback, native-kernel and GUI gates
still apply. A stable very large timestep can strongly damp a wave. Source and
evidence interpretation are recorded in the local
[P1 residual report](../../../plans/workplans/IMPLICIT_FV_1D_P1_COUPLED_REACH_2026-10-19.md).


## Balanced network increment

`ImplicitNetworkSolver` separately extends the original fine-grid equations to
bed/section corrections, reversed cell and face frames, aggregate-barrel
geometry and zero-storage algebraic junction heads. Every original cell remains
an area/discharge unknown. Ordinary degree-2 nodes use algebraic head equations
in this fully pressurized scope, matching the existing runtime's revocation of
the free-surface pass-through bypass. Typed virtual nodes stay spliced interior
faces. Cell and node lateral rates have explicit separate ownership; virtual
node lateral rates must already be allocated to cells. A frozen-cell bracketed
head initialization does not change initial A/Q. Optional retained depth must
reproduce conserved area exactly, preserving head precision in stiff slots.

The spatial residual uses the existing shared face geometry, hydrostatic
bed/section corrections and HLL kernel. Signed friction uses each cell's actual
conduit and per-barrel hydraulic radius. There is no duplicate bed source,
artificial node area, head pin or residual carry. All trial states, including
reconstructed faces, remain above the SLOT taper. Physical storage/ponding,
transitions, dry cells, structures, local losses, TPA, transport, event forcing
and engine-wide transactions remain unsupported. Production selection and GUI
options remain gated by the parent plan.

The Jacobian uses analytic storage/history and local face/source differences,
with cell (h,Q) rows followed by unknown heads. A native scalar ILU(0) graph
comparator supplies a fixed linear right inverse to native GMRES. Diagonal
shifts change only that inverse; acceptance always checks the original matrix
and physical equations. Conduit/interface condensation and block-ILU
qualification remain later work. Sparse pattern/factors are rebuilt per Newton
correction; no performance claim is made. The linear memory cap covers GMRES
plus ILU value/index buffers, excluding the caller's Jacobian and Newton state.

```sh
python3 tests/verification/fv_implicit_phase1/network_run.py \
  --build-dir build/fv-implicit-phase0 \
  --output tests/output/fv_implicit_phase1/network_review

python3 tests/verification/fv_implicit_phase1/network_review.py \
  tests/output/fv_implicit_phase1/network_review
```

This capture runs 115 native test cases (14 network plus the prior 101), 70
network cases (24 rest, 42 moving/source and four restart comparisons), and
16 prior reach controls. Each accepted step retains every cell and node state,
all face receipts, original mesh/closure fingerprints and residual metrics.
The independent audit reconstructs the analytic above-crown SLOT closure and
HLL face physics from the saved states, checks each original cell volume and
momentum equation, checks algebraic junction balance and source ownership, and
compares the prior reach states/meshes bit-for-bit. The matrix covers 4/8/16
cells per conduit at slot settings 150/660 ft/s and GMRES restarts 20/40/80.
These short conservation/consistency runs do not resolve transient-wave or
steady head-loss accuracy. Full details and support limits are in the local
[network increment report](../../../plans/workplans/IMPLICIT_FV_1D_P1B_BALANCED_NETWORK_2026-10-20.md).


## Exact conduit-interior condensation

The next internal method retains conduit-end cell coordinates and algebraic
heads, eliminates eligible block-tridiagonal conduit interiors, forms the
sparse interface Schur matrix, and reconstructs **every original correction**.
It uses the assembled scaled Jacobian; no hydraulic geometry or state is
coarsened. Small interfaces use an independent native globally pivoted dense
kernel, capped at 128 scalar unknowns by default. Larger/forced interfaces use
native ILU(0)/GMRES. The original full-matrix residual is checked after
reconstruction, and all original physical nonlinear budgets still apply.
Reduced tolerances are capped using the original tolerance so amplification of
the reduced RHS cannot weaken the acceptance target.

Unsafe interior pivots, unsupported wider stencils, fill/memory limits and
interface/reconstruction failures can fall back to the unchanged full graph.
Exact factors are never shifted. The standalone default remains full GMRES.
The block-leaf follow-on below adds opt-in interface peeling. General interior
pivot/retention, condition estimation and reuse remain later work. No engine or
GUI routing option selects these methods.

```sh
python3 tests/verification/fv_implicit_phase1/condensation_run.py \
  --build-dir build/fv-implicit-phase0 \
  --output tests/output/fv_implicit_phase1/condensation_review

python3 tests/verification/fv_implicit_phase1/condensation_review.py \
  tests/output/fv_implicit_phase1/condensation_review
```

This profile selects 126 native tests (11 new plus the previous 115) and 226
network runs: the original 70-case matrix in three linear modes, four forced
resource-fallback cases, and 12 comparisons at 64/256 cells per conduit. All 70
corresponding full-mode states, node states and meshes must remain identical to
the preceding capture. Every correction's original residual, tolerance, pivot
quality, method/fallback, dimensions, fill and dynamic-buffer bound is retained
in `linear.csv`. The driver also records step-solve wall time and process peak
RSS where available. These short observations are not a matched-accuracy
performance claim. Full scope and limitations are in the local
[condensation report](../../../plans/workplans/IMPLICIT_FV_1D_P1C_CONDUIT_CONDENSATION_2026-10-20.md).

## Native block-leaf interface increment

`CondensationControls::tree_interface` opts into exact scalar/2×2 endpoint-leaf
elimination after conduit condensation. The driver modes `tree` and
`tree_reduced` retain unsafe leaf equations and loop cores; only a nonempty
core uses the capped native dense or ILU(0)/GMRES solver. Both the original
unpeeled Schur and full cell/head equations must pass fresh residual checks
after reconstructing every correction. Existing modes/defaults remain.

```sh
python3 tests/verification/fv_implicit_phase1/tree_run.py \
  --build-dir build/fv-implicit-phase0 \
  --output tests/output/fv_implicit_phase1/tree_review

python3 tests/verification/fv_implicit_phase1/tree_run.py \
  --review tests/output/fv_implicit_phase1/tree_review
```

This focused implementation profile runs 34 native tests (nine new leaf tests,
11 condensation and 14 network), plus six physical fixtures in five linear
modes: 30 short runs and 24 comparisons. Eighteen existing-mode state/node/mesh
controls must remain identical to P1c. Source/build/artifact snapshots, original
physical equations and face physics, and every linear correction's core/leaf
dimensions and residual diagnostics are checked and retained. It is not a
rerun of the full previous 126-test/226-run campaign. Broader hydraulic and
explicit-FV/dynamic-wave performance campaigns are deferred at the user's
request. See the local [P1d report](../../../plans/workplans/IMPLICIT_FV_1D_P1D_INTERFACE_TREE_2026-10-20.md).

## Native mixed-block ILU(0)/GMRES increment

`Controls::preconditioner=Preconditioner::BlockIlu0` selects fixed native block
ILU for the original, reduced/interface and retained-core GMRES paths, including
original-matrix fallback. Direct static condensation calls use the corresponding
`CondensationControls` field. Each original cell remains a 2-variable block and
appended algebraic heads remain scalar. Missing preconditioner diagonals and
guarded factor shifts never change the original matrix/RHS. Defaults remain
scalar ILU(0). Driver modes accept `_block`, including `full_block`,
`reduced_block`, `tree_reduced_block` and forced-resource `fallback_block`.

```sh
python3 tests/verification/fv_implicit_phase1/block_ilu_run.py \
  --build-dir build/fv-implicit-phase0 \
  --output tests/output/fv_implicit_phase1/block_ilu_review

python3 tests/verification/fv_implicit_phase1/block_ilu_run.py \
  --review tests/output/fv_implicit_phase1/block_ilu_review
```

The focused profile runs 46 native tests (12 new plus the preceding 34),
38 short network cases, 32 method comparisons and 18 unchanged prior scalar
controls. It retains source/build/artifact snapshots, original physical
equations/face checks and per-correction block counts, shifts, buffer bounds
and original/reduced work/residuals. Algebraic tests include noncommuting block
updates, zero pivots, incompatible systems, limits/failure publication and a
384-scalar single-cycle core above the dense cap. Broad hydraulic/performance
campaigns remain deferred; no measured speedup is implied. See the local
[P1e report](../../../plans/workplans/IMPLICIT_FV_1D_P1E_NATIVE_BLOCK_ILU_2026-10-20.md).

## Reusable linear workspace increment

`Controls::reuse_linear_workspace` opts into instance-owned scalar/block ILU
symbolic layouts, numerical scratch and native GMRES buffers across Newton
corrections and standalone timesteps. Static solvers accept an optional
`LinearWorkspace*`. Exact CSR/backend/pair identity is checked; numerical
factors are rebuilt every call and GMRES starts from zero. Budget/key changes
and failures release caches. Structured budgets include cached buffers during
assembly, and a failed reduced attempt releases them before full fallback.
The default remains fresh solves. Each concurrent solve needs its own instance.

Driver modes append `_reuse`, such as `full_reuse` or
`tree_reduced_block_reuse`.

```sh
python3 tests/verification/fv_implicit_phase1/workspace_run.py \
  --build-dir build/fv-implicit-phase0 \
  --output tests/output/fv_implicit_phase1/workspace_review

python3 tests/verification/fv_implicit_phase1/workspace_run.py \
  --review tests/output/fv_implicit_phase1/workspace_review
```

This focused profile runs 66 native tests, 76 short network runs, 38 prior
fresh controls, 38 exact cached/fresh state/face/trace/mesh comparisons and 70
across-method endpoint comparisons. Sources/artifacts, original physical
equations and cache/Krylov growth/retained-memory records are preserved and
reviewed. It does not cache Jacobian/conduit/tree assembly or lag numerical
factors, and it makes no runtime claim. Broad hydraulic/performance campaigns
remain deferred. See the local
[P1f report](../../../plans/workplans/IMPLICIT_FV_1D_P1F_LINEAR_WORKSPACE_2026-10-20.md).

## Physical storage volume increment

The standalone network solver accepts bounded physical storage curves from the
existing mesh tables. Supply `old_node_volume` and its matching accepted storage
head; ordinary and virtual nodes have zero physical volume. Storage heads skip
algebraic initialization. The full volume difference enters the residual, with
its segment slope in the Jacobian. Storage and ordinary junction diagnostics
retain their separate volume and flow units. Heads above the rim/table top,
ponding/overflow, prescribed storage heads and unsupported physics are rejected.
The sampled mesh closure is not an assertion of analytic `node::getVolume`
equivalence between table knots; production publication remains gated.

Driver cases `storage_tee`, `storage_loop`, `storage_reversed` and
`storage_closed` retain every physical cell and additionally export
`storage.csv`, `sources.csv` and the storage geometry in the mesh snapshots.
The existing driver modes select native full/condensed/tree and scalar/block
solves, including optional `_reuse`.

```sh
python3 tests/verification/fv_implicit_phase1/storage_run.py \
  --build-dir build/fv-implicit-phase0 \
  --output tests/output/fv_implicit_phase1/storage_review

python3 tests/verification/fv_implicit_phase1/storage_run.py \
  --review tests/output/fv_implicit_phase1/storage_review
```

This focused profile runs 77 native cases, 48 short storage runs and 18 unchanged
prior no-storage controls. It compares 12 cached/fresh artifact sets exactly and
40 across-method cell endpoints and all accepted node heads/physical volumes.
The independent audit recomputes storage interpolation, every local balance,
shared face physics and total cell-plus-storage inventory. Source/configuration
and native artifact snapshots are retained. Broad hydraulic/performance testing
and production/GUI selection remain deferred. See the local
[P2a report](../../../plans/workplans/IMPLICIT_FV_1D_P2A_PHYSICAL_STORAGE_2026-10-20.md).

## Bounded wet free-surface and SLOT-transition increment

Set the standalone problem's `regime` to `BoundedWetSlot` to admit wet states
below, through and above the crown in closed sections. `minimum_wet_depth`
(default 0.01 ft) bounds every centre and reconstructed side by rejection.
Dry activation/depletion, ponding/flooding and production integration remain
outside this scope. The default remains `PressurizedOnly`.

Wet mode differentiates the actual closure and selected HLL/mixed-wave branches
with native fixed-size local directional arithmetic. The true residual calls
the existing shared kernels. Physical storage and ordinary zero-storage nodes
retain their respective equations and units; every physical cell is retained.

```sh
python3 tests/verification/fv_implicit_phase1/wet_run.py \
  --build-dir build/fv-implicit-phase0 \
  --output tests/output/fv_implicit_phase1/wet_review

python3 tests/verification/fv_implicit_phase1/wet_run.py \
  --review tests/output/fv_implicit_phase1/wet_review
```

The focused profile runs 87 native checks and 99 short runs (86 wet and 13
unchanged prior controls), 18 exact cached/fresh comparisons and 72 method
comparisons. It retains semantic closure/mesh data, sources, cell/node/storage
states, face receipts and native linear diagnostics. The independent wet audit
reconstructs closure polynomials, hydraulic radii, HLL/mixed-wave physics and
every accepted physical balance. Mixed-interface cases use four 1e-5-second
steps; the other wet cases use four 0.25-second steps. They are implementation
checks, not large-timestep/front/performance qualification.

An initial mixed-crown loop at dt=0.25 s still fails the line search and is
retained as a native safe-rejection test and promotion hold. No failed step is
published. Broad hydraulic/performance testing remains deferred. See the local
[P2b report](../../../plans/workplans/IMPLICIT_FV_1D_P2B_BOUNDED_WET_SLOT_2026-10-20.md).
