# Benchmark: rubinato2017

Rubinato et al. (2017), *J. Hydrol.* 552, 421–432, doi:10.1016/j.jhydrol.2017.06.024.

## Rig

A 0.24 m circular manhole with no lid links a 75 mm pipe to an 8 m × 4 m flume
(slope 0.001, Manning n 0.009). The flume bed is the manhole crest, 0.478 m above
the pipe invert. Surface depth `h_s` is read 0.46 m from the manhole centre, pipe
head `h_p` (from the pipe invert) 0.35 m away.

## Steady scenarios (Table 2) and the equation fitted to each (Table 3)

| Scenario | Exchange | Condition | Equation | C (lower–upper) |
|---|---|---|---|---|
| S1 | surface → sewer | sewer not surcharged | free weir `Q = ⅔ C π D √(2g) h_s^{3/2}` | 0.540 (0.493–0.587) |
| S2 | surface → sewer | `Z_crest < h_p < h_s + Z_crest` | submerged weir `Q = C π D √(2g) h_s √(h_s + Z_crest − h_p)` | 0.056 (0.055–0.057) |
| S3 | sewer → surface | wet floodplain | orifice `Q = C A_M √(2g) √(h_p − h_s − Z_crest)` | 0.167 (0.160–0.174) |
| S4 | sewer → surface | dry floodplain (`h_s = 0`) | orifice, same form | 0.160 (0.159–0.161) |

The coefficients are slopes of linear fits of `Q_e` on the equation terms; the fits
have intercepts (−2.5 and −3.8 l/s for S3 and S4), so the zero-intercept equations
with these coefficients reproduce none of the 36 measured rows within `ξ_f + 2 %`.

## How the engine uses it

`tests/unit/engine/test_2d_exchange_rubinato_steady.cpp` evaluates
`openswmm::twoD::exchange::exchange` in each regime with unit coefficients (and the
φ regularisation head scaled to the rig), regresses the measured `Q_e` on the
resulting terms and checks the slope against Table 3 (±0.01) and R² ≥ 0.96 — a check
that the law's regime structure reproduces the paper's equation terms. Absolute
`Q` comparisons are written for review only.

`tests/unit/engine/test_2d_exchange_rubinato_model.cpp` rebuilds the flume in the full
engine (manhole `[2D_NODE_COUPLING] MANHOLE D=0.24 COEFFS=RUBINATO2017`, weir/orifice
law, structure-area wet-mean head, chamber pond) and runs every Table 2 row to steady
state. The paper does not tabulate inflows, so each row's boundary conditions are
reconstructed from the row itself: surface inflow `Q1 = Fr_s √(g h_s) h_s W`, sewer
inflow from `Q3 = abs(Q_e / (surcharge_pct/100))` (S2–S4; S1 has no sewer inflow), and the
measured `h_p` imposed at the manhole by a fixed outfall stage behind a short wide
pipe. The manhole sits at the flume centre and the mesh is a uniform 0.2 m grid: a
junction exchanges through one cell, and cells far smaller than the 0.045 m² opening
concentrate the whole exchange in a sliver. Results are exported in `model_vs_table2.csv`; see the dated review rather
than treating the earlier reconstructed-flow ranges as fixed acceptance values. The test gates only the exchange
direction and order of magnitude, plus a regression that the paper's own 0.0125 m
mesh is not choked by the head-equalisation cap.

## Finalisation review (21 October 2026)

The model test now rejects a late step failure or premature EOF, exports the
180-240 s and 240-300 s exchange rates, the nearest-centroid surface probe
and manhole head, continuity and mesh size. `PhysicalShaftSensitivity` runs
all 36 rows with actual cylindrical storage below the rim and MIN_SURFAREA
set to pi D squared / 4, retaining every other assumption. It exports
`shaft_vs_table2.csv`. This is a modelling sensitivity, not coefficient fitting.

The benchmark corpus now has metadata/provenance and `suites/coupling/`. Its
synthetic event uses HH:MM:SS timestamps (the previous MM:SS placed the pulse
outside the run), actual shaft storage and an explicitly assumed 0.45 m
downstream stage. The 27 synthetic tests exercise drainage, surcharge and
drainback. They are not replicas of the paper's measured transient events.

The source tables are unchanged. Using pipe Reynolds number as inlet flow
was a reconstruction error; the surcharge-percentage definition directly
constrains Q3. Absolute fit remains report-only, but the suite separately
gates completion, direction, two-window drift and ordinary 1D continuity.

Review report and proposed publication text: `docs/validation/rubinato2017/`.
The original junction representation is retained as evidence of its large
continuity errors. Neither that representation nor the physical-shaft
sensitivity establishes spatial convergence or independent measured
unsteady validation.
