# Benchmark: dw-ritter-drybed-strip

## Physics

Ritter (1892) exact solution for a 1D frictionless, horizontal, dry-bed dam
break on a rectangular open channel.  At t = 0 a dam at x = x_d separates a
still reservoir of depth h₀ (x < x_d) from a completely dry bed (x > x_d).
After the dam is removed the solution is a rarefaction fan; no shock forms
because the downstream depth is zero.

**Unit system**: US customary throughout.  The OpenSWMM 1D DW solver uses
`GRAVITY = 32.2 ft/s²` (hardcoded in `src/engine/core/Constants.hpp`); all
parameters and reference values are in feet and seconds.

## Coordinate convention

Local coordinate ξ = x_abs − x_d (positive downstream).  The reservoir
occupies ξ < 0; the dry bed occupies ξ > 0.  All analytical formulas are
evaluated at ξ, not at the absolute coordinate x_abs.

## Analytical solution

For t > 0 the three regions are:

| Region | Condition | h | u |
|---|---|---|---|
| Undisturbed reservoir | ξ ≤ −c₀ t | h₀ | 0 |
| Rarefaction fan | −c₀ t < ξ < 2 c₀ t | (2c₀ − ξ/t)² / (9g) | (2/3)(c₀ + ξ/t) |
| Dry bed | ξ ≥ 2 c₀ t | 0 | 0 |

where c₀ = √(g h₀).

**Dam-station steady value** (ξ = 0, all t > 0): h = 4h₀/9, u = (2/3)c₀.

## Setup parameters

| Parameter | Symbol | Value | Units |
|---|---|---|---|
| Initial reservoir depth | h₀ | 1.0 | ft |
| Gravitational acceleration | g | 32.2 | ft/s² |
| Wave celerity | c₀ = √(g h₀) | 5.6745 | ft/s |
| Strip length | L | 250 | ft |
| Dam location | x_d | 125 | ft |
| Channel width | b | 5 | ft |
| Conduit length (cell size) | Δx | 5 | ft |
| Number of conduits | N | 50 | — |
| Timestep | Δt | 0.5 | s |
| Courant number (CFL) | c₀ Δt / Δx | ≈ 0.57 | — |

## Validity window

The analytical solution is valid for t < t_max, where the binding constraint
is the downstream wet front reaching the boundary:

  t_max = (L − x_d) / (2 c₀) = 125 / (2 × 5.6745) ≈ 11.01 s

The comparison harness must reject any sample at t ≥ t_max.  Comparison
times used here: t ∈ {2, 4, 6, 8} s — all safely below t_max.

## Frictionless configuration

The 1D DW solver uses Manning's n via `rough_factor = GRAVITY * (n/PHI)²`.
Setting n = 0 gives `rough_factor = 0`, which zeroes the Manning friction
term without any division by zero.  This benchmark uses **exact** n = 0
(not a near-frictionless approximation).

## Boundary conditions

| Node | Type | Setting |
|---|---|---|
| Node 0 (upstream) | OUTFALL FIXED | stage = h₀ = 1.0 ft (infinite reservoir) |
| Node 50 (downstream) | OUTFALL FREE | depth = min(yNorm, yCrit); with β = 0 returns 0 (correct: outfall stays dry for t ≤ 8 s) |

The backward rarefaction characteristic reaches the upstream boundary at
t ≈ 22 s (= x_d / c₀), so the FIXED stage is never violated during the
comparison window.

## Metrics and tolerances

All non-mass tolerances are **initial targets** to be calibrated after the
first clean baseline run.  Mass error is a hard requirement.

| Metric | Definition | Initial target | Hard? |
|---|---|---|---|
| Relative mass error | \|V_in − V_out − ΔV\| / V_scale | < 0.5 % | Yes |
| Dam-station depth | \|h(ξ=0, t) − 4h₀/9\| / h₀ | < 10 % | No |
| L₁(h) wet region | mean \|h_solver − h_ref\| over nodes with h_ref > h_wet_tol | < 5 % of h₀ = 0.05 ft | No |
| Front position error | \|ξ_front_solver − 2 c₀ t\| | < 3 Δx = 15 ft | No |

h_wet_tol = 0.01 ft (excludes near-dry cells at the wetting front from the
L₁ and front-position metrics, where hydraulic radius collapses).

## Reference CSV layout

File: `reference.csv`
Columns: `t_s, xi_ft, h_ft, u_fps`
Rows: 51 nodes × 4 times = 204 rows.
Node i: x_i = 5 i ft, ξ_i = x_i − 125 ft.
