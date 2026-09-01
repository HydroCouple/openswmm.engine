# E0 Kernel Promotion — Validation & Commit Handoff (2026-08-16)

**For:** validation agent (build machine with the full toolchain; the
implementation sandbox could only syntax-check)
**Scope:** phase E0 of `plans/transport/EULERIAN_ARD_TRANSPORT_PLAN.md` §6
and `UNIFIED_TRANSPORT_MASTER_PLAN.md` (D-UT1 as amended).
**Author of changes:** planning session 2026-08-16 (Claude, per C. Buahin's
direction).

---

## 1. What is in the tree

### Commit `3ca9f4ed` (ALREADY COMMITTED — checkpoint, validate it too)

Pre-existing in-progress work found uncommitted in the same files E0
refactors; committed as-is so the two changesets stay bisectable:
face-consistent pass-through node stage unified with the VJ publish rule
(`ExplicitFvSolver.cpp` solveAlgebraicNode region), `Routing.cpp` /
`NetworkMeshBuilder.cpp` / `NetworkMeshData.hpp` / `ErrorCodes.*` support,
new `tests/unit/engine/test_fv_node_head_consistency.cpp`, updated
`site_drainage_model.rpt` + legacy hotstart `.rpt` goldens. **This commit
was NOT build-verified when committed.** If it fails, fix forward in a
follow-up commit scoped to it — do not entangle with E0.

### UNCOMMITTED WORKING-TREE CHANGES (the E0 changeset — yours to validate & commit)

```
new:  src/engine/transport/fvkernels/SpeciesTransportKernels.hpp
new:  src/engine/transport/fvkernels/SpeciesTransportKernels.cpp
mod:  src/engine/hydraulics/fv/ExplicitFvSolver.cpp
mod:  src/engine/hydraulics/fv/ExplicitFvSolver.hpp
mod:  src/engine/CMakeLists.txt          (adds transport/*.cpp + *.hpp globs)
```

Content: `reconstructScalars`, `limitSpeciesFluxes`, `dispersionSolve`, and
`limitSlope` moved VERBATIM from `ExplicitFvSolver.cpp` into
`openswmm::transport::fvkernels`, consuming solver state through the
non-owning `SpeciesKernelView` (built by the new private
`ExplicitFvSolver::speciesKernelView()`). `adjustedFlux` became a lambda
inside the moved `limitSpeciesFluxes` (member + declaration removed —
orphan-removal rule). The solver's `reconstructScalars`/`dispersionSolve`
remain as thin forwarders; hydro `reconstructState` keeps using
`limitSlope` via a using-declaration. **Do not tidy the local alias blocks
at the top of the moved kernel functions — they are what makes the bodies
textually identical to the pre-move code.**

Already machine-verified in the sandbox (record, don't repeat):
`g++ -std=c++20 -fsyntax-only` passes on both changed TUs; a normalized
diff of each moved body against `git show HEAD:...ExplicitFvSolver.cpp`
proved all three bodies verbatim modulo the intended
`opts_.* → v.*` / member-alias substitutions; grep confirms zero remaining
references to `adjustedFlux`/`limitSpeciesFluxes` outside the new unit.

Untracked files NOT part of this changeset (leave alone): `.claude/`,
`CLAUDE.md`, `SWMM_5.3-6.0_User_Testing_Questionnaire.docx`, `cliff.toml`,
`paper/`.

## 2. Validation protocol

1. **Configure + build** with the platform preset (`cmake --preset Darwin`
   or as this machine normally builds; both Release and a debug build if
   time permits). Zero new warnings attributable to the changed files.
2. **Full engine unit suite** (`ctest` in the build dir). Gates that
   specifically exercise the moved code — all must pass UNCHANGED (no
   tolerance edits, no golden updates):
   - `test_fv_solver_network` — `UniformConcentrationStaysUniformUnderArbitraryFlow`,
     `StepConcentrationObeysTheDiscreteMaximumPrinciple`,
     `SoluteMassIsConservedInAClosedReach`,
     `FrontSharpnessSeparatesRiemannFromReconstruction`,
     `ImplicitDispersionRemovesTheDeltaXSquaredStepRestriction`
   - `test_virtual_junction`, `test_fv_node_head_consistency` (also cover
     commit `3ca9f4ed`), plus the remainder of the suite.
3. **Bitwise regression** (the E0 gate — behavior identical, not just
   passing):
   ```
   git stash                                   # shelve E0
   build; run the FV benchmark decks (site_drainage_model + the FV parity
       models used in tests/unit/engine/data) with FLOW_ROUTING FV;
       archive .rpt/.out to /tmp/e0_base (a user-visible dir is fine too,
       per CLAUDE.md §4.1 — e.g. tests/out/e0_base)
   git stash pop                               # restore E0
   rebuild; rerun identical decks → e0_new
   cmp each pair — MUST be byte-identical (species are wired to zero in
       production, Routing.cpp:844, so any diff whatsoever is a defect in
       the refactor, not a tolerance question)
   ```
4. **Legacy + quality suites** untouched by design — run them anyway
   (cheap insurance that the CMake glob addition changed nothing else).

## 3. Commit protocol

If §2 passes in full, commit the working-tree changes (the five files in
§1 only — `git add src/engine/transport src/engine/hydraulics/fv/ExplicitFvSolver.cpp src/engine/hydraulics/fv/ExplicitFvSolver.hpp src/engine/CMakeLists.txt`) with:

```
refactor(transport): promote FV species kernels to shared transport/fvkernels (E0)

Move reconstructScalars, limitSpeciesFluxes (with adjustedFlux folded in
as a lambda), dispersionSolve, and limitSlope verbatim from
ExplicitFvSolver into openswmm::transport::fvkernels, consumed through a
non-owning SpeciesKernelView. First step of the unified Eulerian ARD
engine (plans/transport/EULERIAN_ARD_TRANSPORT_PLAN.md rev. 2, D-UT1
amended): the standalone engine and the FV solver will share this exact
implementation; the solver keeps thin forwarders and behavior is
bitwise-identical (verified: solver-level transport tests unchanged, FV
benchmark outputs byte-identical).

Validation record: plans/transport/E0_VALIDATION_HANDOFF_2026-08-16.md
```

Append your measured results (test counts, benchmark cmp results, build
warnings) to §4 of this document in the same commit.

If §2 FAILS: do not commit. Record the failure in §4, leave the tree as
is, and flag which of (a) commit `3ca9f4ed` regressions vs (b) the E0
changeset the failure attributes to — `git stash` isolates (b).

## 4. Validation results

**Validated 2026-08-16** on Darwin 25.5.0 / arm64, Apple clang, preset
`Darwin` (`build/darwin`, Release, `OPENSWMM_BUILD_UNIT_TESTS=ON`,
`OPENSWMM_BUILD_REGRESSION_TESTS=ON`). Artifacts, scripts and both output
sets are kept under `tests/output/e0_validation_2026-08-16/` (CLAUDE.md
§4.1). HEAD at validation time was `678a9e87`, i.e. `3ca9f4ed` plus one
unrelated gage commit.

**Tree note.** The working tree also contained changeset B (the E1 ARD
engine, `E1_VALIDATION_HANDOFF_2026-08-16.md`). It was shelved for the whole
of this exercise — `git stash push -- <E1 tracked files>` plus a physical
move of the untracked `src/engine/transport/components/` and
`tests/unit/engine/test_ard_transport.cpp` — and restored afterwards, so
every number below is E0 in isolation. E1 remains uncommitted and
unvalidated.

### 4.1 Verbatim-move re-verification (independent of the sandbox claim)

`tests/output/e0_validation_2026-08-16/extract.py` brace-matches each moved
body out of `git show HEAD:...ExplicitFvSolver.cpp` and out of the new unit,
strips the alias prologue, applies only the intended
`opts_.* → v.*` / `limitSpeciesFluxes(v, …)` substitutions, and diffs:

```
VERBATIM  reconstructScalars  (144 lines)
VERBATIM  limitSpeciesFluxes  (105 lines)
VERBATIM  dispersionSolve     ( 66 lines)
VERBATIM  limitSlope          ( 13 lines)
```

Every `SpeciesKernelView` field also resolves 1:1 to the identically named
`ExplicitFvSolver` member with a matching type (`ExplicitFvSolver.hpp:389`,
`397`, `400`, `404`, `409-411`, `414`, `434`, `474-475`). Orphan grep for
`adjustedFlux` / `limitSpeciesFluxes` outside the new unit: clean.

### 4.2 Build

Zero warnings **and zero notes attributable to the changed files** — the
only three in the whole build are pre-existing and in untouched code:

```
src/engine/hydraulics/../data/TableData.hpp:708  missing field 'cursor' initializer
src/engine/hydraulics/Routing.cpp:791            unused variable 'links'
vcpkg: feature hypre was passed, but that is not a feature supported ...
```

Full log: `tests/output/e0_validation_2026-08-16/build_e0.log`. The
`transport/*.cpp` + `*.hpp` glob addition picks up the new unit correctly;
no other target's source list changed.

### 4.3 Unit suite — 127/128, one PRE-EXISTING failure (not E0, not 3ca9f4ed)

`ctest -j6` (log: `ctest_e0_final.log`; 127 unit + 1 regression, 26.5 s).

The five E0 gates in `test_engine_fv_network`, run by name:

```
[ OK ] FvNetwork.UniformConcentrationStaysUniformUnderArbitraryFlow      (134 ms)
[ OK ] FvNetwork.StepConcentrationObeysTheDiscreteMaximumPrinciple        (83 ms)
[ OK ] FvNetwork.SoluteMassIsConservedInAClosedReach                      (99 ms)
[ OK ] FvNetwork.FrontSharpnessSeparatesRiemannFromReconstruction        (583 ms)
[ OK ] FvNetwork.ImplicitDispersionRemovesTheDeltaXSquaredStepRestriction (49 ms)
```

The `3ca9f4ed` gates also pass: `test_engine_virtual_junction` (0.18 s),
`test_engine_fv_node_head_consistency` (0.26 s). So does the legacy layer
(`test_legacy_solver_api/_errors/_hotstart/_shapes/_storage_shapes/
_expanded_api`, `test_legacy_output`, `test_engine_controls_legacy_parity`)
and the quality layer (`test_engine_quality_routing`,
`test_engine_quality_roundtrip`).

The single failure is **`test_engine_fv_integration` →
`FvEngine.RefiningTheMeshConvergesTowardTheDynwaveHydrograph`**:

```
test_fv_engine_integration.cpp:242: Expected: (e_mid) < (e_coarse),
  actual: 0.055224237275644343 vs 0.052534507871460516
```

Bisected by rebuilding and rerunning the same test three ways:

| tree | e_coarse | e_mid | result |
|---|---|---|---|
| E0 applied | 0.052534507871460516 | 0.055224237275644343 | FAIL |
| HEAD `678a9e87` (E0 shelved) | 0.052534507871460516 | 0.055224237275644343 | FAIL |
| `297796f2` = `3ca9f4ed^` (clean worktree) | 0.052534507871460516 | 0.055224237275644343 | FAIL |

Bit-identical at all three points, so the failure is attributable to
**neither E0 nor `3ca9f4ed`** — it predates both. Diagnosis for whoever
picks it up: the test only asserts the refinement *trend* while
`e_coarse > kNoiseFloor = 0.05`, and its own comment records COARSE sitting
at "~3 %". COARSE has since drifted to 5.25 %, which re-arms the trend
branch, and the COARSE→dx=50 leg is non-monotone (dx=20 does improve, to
3.92 %). Out of scope here; not entangled with E0.

That the three trees agree to the last bit is also the strongest available
evidence for E0 itself: this deck exercises `limitSlope` through the
hydrodynamic `reconstructState` at three mesh resolutions and reproduces
identical doubles across the move.

### 4.4 Bitwise regression — 20/20 byte-identical

Ten FV decks (`tests/output/e0_validation_2026-08-16/decks/`), run through
`bin/Release/openswmm` by `run_decks.sh` against a pre-E0 baseline built in
the same directory with E0 shelved:

- `sdm_fv_o1` — `site_drainage_model.inp` with `FLOW_ROUTING FV`
- `sdm_fv_o2` — the same plus `FV_ORDER 2` / `FV_LIMITER VANLEER`
- `sdm_fv_o2_superbee` — `FV_ORDER 2` / `SUPERBEE` / `FV_DISPERSION 1.0`
- `agree_pass`, `agree_vj`, `steep_pass`, `slot_capped`, `slot_uncapped`
  (the `fv_node_head_out` parity decks) and `vj_fv_invert_collision`,
  `vj_fv_steep_datum` (the FV virtual-junction decks)

The `FV_ORDER 2` variants are deliberate: with species wired to zero in
production (`Routing.cpp:860`, `fv_state_.resize(nc, nn, 0)`) the moved
species kernels are dead on a deck run, but `limitSlope` is **not** — it
drives the second-order hydrodynamic reconstruction, so those decks put the
moved code on the bitwise path.

Volatility was characterised first by running the baseline twice: `.out` is
byte-identical run-to-run (10/10), `.rpt` differs only in
`Analysis begun on` / `Analysis ended on` / `Total elapsed time`.

```
.out, strict cmp                       10/10 IDENTICAL
.rpt, three timestamp lines filtered   10/10 IDENTICAL
```

Both output sets retained as `e0_base/` and `e0_new/` for re-checking.

### 4.5 Verdict

§2 passes in full. E0 committed; the pre-existing
`RefiningTheMeshConvergesTowardTheDynwaveHydrograph` failure is left as
found and recorded above rather than papered over.
