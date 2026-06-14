# Benchmark Data Layout

## Purpose

On `swmm6_rel`, this directory is already the Google Benchmark performance subtree for `openswmm.engine`.

It can also hold reference datasets and supporting metadata used for solver verification, but those assets should live in subdirectories so they do not interfere with the top-level benchmark executables and CMake files.

The intended use case is to retain benchmark artifacts generated externally, including with internal Flash-X workflows, without importing Flash-X source code into this repository.

## Principles

1. Store generated reference data, not external solver source.
2. Keep provenance next to the dataset.
3. Prefer open, stable, text-based formats.
4. Separate benchmark data from test code.
5. Make each dataset reproducible from its metadata and generation notes.

## Recommended Layout

```text
tests/
  benchmarks/
    CMakeLists.txt
    bench_engine_vs_legacy.cpp
    bench_timeseries_lookup.cpp
    bench_hydraulics.cpp
    README.md
    provenance-template.md
    generated/
      <benchmark-name>/
        README.md
        provenance.yaml
        reference.csv
        reference.json
        notes.md
    manufactured/
      <benchmark-name>/
        README.md
        definition.md
        reference.csv
    shared/
      units.md
      conventions.md
```

## Directory Roles

### Top-level files

The top level of `tests/benchmarks/` is reserved for Google Benchmark performance targets already used by this branch.

Current examples:

- `bench_engine_vs_legacy.cpp`
- `bench_timeseries_lookup.cpp`
- `bench_hydraulics.cpp`
- `CMakeLists.txt`

Do not place verification datasets directly alongside those files. Put verification assets in the subdirectories below.

### `generated/`

For benchmark outputs created by an external tool or solver run.

Typical examples:

- time histories,
- reference hydrographs,
- depth profiles,
- infiltration trajectories,
- storage-loss tables,
- tabulated exact or semi-analytic data.

Each benchmark directory should contain:

- `README.md`: what the dataset represents and how tests consume it,
- `provenance.yaml`: machine-readable generation metadata,
- `reference.csv` or `reference.json`: the actual reference values,
- `notes.md`: optional generation caveats, tolerances, or assumptions.

### `manufactured/`

For hand-defined or analytically derived verification cases where the reference solution is defined by formulas or compact tables rather than an external production run.

Typical examples:

- smooth manufactured dynamic-wave solution,
- exact scalar ODE trajectories for integrator testing,
- closed-form infiltration cases,
- linear storage recession cases.

### `shared/`

For conventions reused by multiple datasets.

Suggested contents:

- unit conventions,
- time origin conventions,
- coordinate/sign conventions,
- variable naming conventions,
- acceptable interpolation rules between stored points and test query points.

## File Format Guidance

Prefer:

- `CSV` for dense numeric tables,
- `JSON` for structured reference datasets with metadata-rich records,
- `YAML` for provenance and configuration,
- `Markdown` for human-readable notes.

Avoid:

- opaque binary formats when a text format is practical,
- external-tool-native formats that require the external tool to parse,
- embedding provenance only in code comments.

## Naming Guidance

Benchmark names should describe both the physics and the scenario, for example:

- `odesolve-exponential-decay`
- `infil-greenampt-constant-rainfall`
- `exfil-cylindrical-storage-greenampt`
- `kinwave-step-inflow-rectangular-conduit`

## Provenance Minimum

Every externally generated benchmark should record:

- generator name,
- generator version or commit,
- source problem/setup name,
- input deck or parameter file identity,
- date generated,
- variables exported,
- units,
- spatial and temporal sampling,
- any post-processing steps,
- known limitations.

For Flash-X-generated data, the metadata should say that the dataset was generated with Flash-X and cite the Flash-X publication, while keeping Flash-X source code outside this repository unless there is an explicit licensing decision to vendor code.

## How Tests Should Consume Data

Tests should:

1. load a single benchmark dataset from `tests/benchmarks/generated/...` or `tests/benchmarks/manufactured/...`,
2. run the SWMM code path under test,
3. compare the computed result to the reference values,
4. report max error, RMS or $L_2$, and conservation or mass-balance error where relevant,
5. fail with benchmark-specific tolerances stored in code or metadata.

The benchmark dataset should remain immutable once baselined. If regenerated, update provenance and explain why the baseline changed.

## Branch-Specific Note

On `swmm6_rel`, correctness tests belong under `tests/unit/engine/` and `tests/regression/`. This directory remains performance-first; the verification datasets stored here are inputs to those correctness tests, not replacements for them.