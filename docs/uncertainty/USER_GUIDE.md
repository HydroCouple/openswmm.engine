# Uncertainty Quantification — User Guide (Scalar 2D ROM)

> **Scope**: Phase 1 scalar 2D ROM features only.  
> Spatial Manning uncertainty (Phase 2) and 1D spectral uncertainty (Phase 3) are not yet implemented.

## Overview

OpenSWMM's uncertainty module propagates parameter uncertainty through the 2D surface routing
solver using a Linear Galerkin Reduced-Order Model (ROM). Instead of running hundreds of full
Monte Carlo simulations, the ROM approximates the ensemble spread at O(M·k) per step — roughly
1000× faster than full MC for typical mesh sizes.

**What you get**: per-cell depth quantiles (q05, q50, q95) written to the HDF5 output file
alongside the deterministic solution. These quantiles represent the spread due to uncertain
Manning's roughness and rainfall intensity.

## Quick Start

Add two sections to your `.inp` file:

```ini
[2D_OPTIONS]
2D_OUTPUT_FILE    my_results.h5
; ... other options ...

[2D_ROM]
ENABLE        YES
MEMBERS       50
MODES         10

[UNCERTAINTY]
MANNINGS_PERT     0.20
RAINFALL_PERT     0.20
```

Run the simulation normally. The output HDF5 file will contain three additional time-varying
datasets: `Mesh2_face_depth_q05`, `Mesh2_face_depth_q50`, `Mesh2_face_depth_q95`.

## Section Reference

### `[2D_ROM]`

Controls the ROM solver. All keywords are optional; defaults are shown.

| Keyword   | Default | Description |
|-----------|---------|-------------|
| `ENABLE`  | NO      | Set to YES to activate the ROM sidecar. |
| `MEMBERS`           | 50  | Ensemble size M. More members → smoother quantiles. |
| `MODES`             | 10  | Number of Laplacian eigenmodes k. More modes → finer spatial resolution of spread. |
| `K_EFF`             | AUTO | Effective diffusive conductance (m^(4/3)/s). AUTO estimates from mean wet depth, Manning's n, and bed slope. |
| `MANNINGS_CORR_LEN` | 0   | Exponential correlation length (m) for spatially-varying Manning's n. 0 = scalar mode (all cells in member i share the same multiplier). |
| `RAINFALL_CORR_LEN` | 0   | Exponential correlation length (m) for spatially-varying rainfall multiplier. 0 = scalar mode. |

### `[UNCERTAINTY]`

Defines parameter perturbation ranges. Applies to both 1D and 2D where supported.

| Keyword          | Default | Description |
|------------------|---------|-------------|
| `MANNINGS_PERT`  | 0.20    | Fractional perturbation on Manning's n (±20% → LHS range [0.8n, 1.2n]). |
| `RAINFALL_PERT`  | 0.20    | Fractional perturbation on rainfall intensity. |
| `SOIL_PERT`      | 0.20    | Fractional perturbation on soil hydraulic conductivity (Ks for Green-Ampt; f0/fmin for Horton). Drives the runoff ensemble. |

When `[UNCERTAINTY]` is present, its `MANNINGS_PERT` and `RAINFALL_PERT` values override any
corresponding values set in `[2D_ROM]`.

## Runoff Ensemble (Phase 3)

When `SOIL_PERT > 0`, the engine internally creates M independent copies of each subcatchment's
infiltration state, each scaled by a Latin-hypercube draw from `[1 - SOIL_PERT, 1 + SOIL_PERT]`:

- **Green-Ampt**: `Ks` of member i = `Ks_base × soil_mult[i]`
- **Horton**: `f0` and `fmin` of member i = `f0_base × soil_mult[i]`
- **Curve Number**: effective retention `S` of member i = `S_base / soil_mult[i]`

Member ordering is shared with the 2D surface-routing ensemble — member i uses the same
soil draw as its corresponding 2D Manning and rainfall draws, enabling correlated
runoff-routing uncertainty propagation.

## WQ Uncertainty Bounds (Phase 3)

For first-order constituent decay with uncertain rate constant k:

```
c_i(t+dt) = c(t) × exp(-k × km_i × dt),   km_i ∈ [1 - SOIL_PERT, 1 + SOIL_PERT]
```

q05/q50/q95 bounds are computed analytically — no second ODE solve is needed. Higher k_mult
gives more decay, so `q05` of concentration corresponds to the highest k_mult member.

## AUTO K_eff

When `K_EFF AUTO` (the default), the effective diffusive conductance is estimated at the moment
the ROM is first seeded (after the mesh is wet):

```
K_eff = h_mean^(5/3) / (2 * n_mean * sqrt(S_mean))
```

where:
- `h_mean` — mean depth over wet cells (cells with depth > 1e-6 m)
- `n_mean` — mean Manning's roughness over wet cells
- `S_mean` — mean bed slope magnitude, floored at 1e-6 to handle flat domains

For flat domains (slope → 0), the floor prevents division by zero and K_eff reflects only the
depth/roughness dependence. If the AUTO estimate is inappropriate for your domain, set `K_EFF`
explicitly in `[2D_ROM]`.

## HDF5 Output Layout

The quantile datasets follow the same CF-1.11/UGRID-1.0 layout as the deterministic fields:

```
/Mesh2_face_depth_q05    [nTime, nFace]  5th-percentile ensemble depth (m)
/Mesh2_face_depth_q50    [nTime, nFace]  median ensemble depth (m)
/Mesh2_face_depth_q95    [nTime, nFace]  95th-percentile ensemble depth (m)
```

When the ROM is not active (or before seeding), these datasets are written with zeros.

## Visualization

The quantile datasets are readable by any CF/UGRID-aware tool:

- **ParaView**: open the `.h5` file, select `Mesh2_face_depth_q95` → `Mesh2_face_depth_q05`
  to visualize the uncertainty band width.
- **QGIS**: use the MDAL plugin with UGRID mesh support.
- **Python/xarray**: `ds = xr.open_dataset("results.h5", engine="netcdf4")`.

Example: spread width at each cell and time step:

```python
import xarray as xr
ds = xr.open_dataset("my_results.h5", engine="netcdf4")
spread = ds["Mesh2_face_depth_q95"] - ds["Mesh2_face_depth_q05"]
```

## Spatial uncertainty (Phase 2)

When `MANNINGS_CORR_LEN > 0` or `RAINFALL_CORR_LEN > 0`, the corresponding parameter gets a
**spatially-varying** multiplier field W[i][t] across cells, rather than a single scalar per
member. Each member's spatial field is generated by smoothing i.i.d. N(0,1) samples with an
exponential kernel of width `corr_len`, then adding to the member's global LHS level:

```
W_n[i][t] = global_level[i] + pert * Z_smooth[i][t]
```

As `corr_len` increases, the spatial variation decreases (members become nearly uniform = scalar
mode). When `corr_len = 0`, the spatial generator is bypassed and the original scalar path
(faster) is used.

**Example with spatial Manning's n:**
```ini
[2D_ROM]
ENABLE              YES
MEMBERS             50
MODES               10
MANNINGS_PERT       0.20
MANNINGS_CORR_LEN   20.0   ; correlation length 20 m
```

## Limitations

- **Linear ROM**: the Galerkin ROM linearises the diffusion operator about the current
  deterministic state. It captures spread growth accurately for small-to-moderate perturbations
  (≤ 30%). For larger perturbations, full MC remains more accurate.
- **No 1D uncertainty coupling**: the ROM ensemble does not yet propagate uncertainty through
  the 1D–2D coupling exchange. Coupling uses the deterministic state.
- **Uniform ICs**: a spatially uniform initial depth projects entirely onto the null Laplacian
  eigenvector, producing zero ROM spread at t=0. Spread builds as the solver advances and
  non-uniform depth patterns develop. This is expected behavior, not a bug.
- **Spatial correlation seed**: the spatial field uses a fixed internal seed (reproducible but
  not tied to the `[UNCERTAINTY]` ensemble seed). Full seed control is Phase 3.

## Performance

Typical overhead of enabling the ROM (M=50, k=10):

| Mesh size | CVODE cost | ROM overhead |
|-----------|-----------|--------------|
| 50×50 (~5k cells) | 177 ms/step | +2 ms/step (~1%) |
| 100×100 (~20k cells) | 610 ms/step | +6 ms/step (~1%) |

ROM cost is O(M·k·n_steps) — purely arithmetic, no ODE solves.
