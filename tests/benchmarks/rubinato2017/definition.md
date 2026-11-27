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

## Long-window check (23 November 2026)

`PhysicalShaftSensitivity` runs each row for 900 s. `shaft_vs_table2.csv` keeps
the 180-240 s and 240-300 s rates (`q_previous_lps`, `q_num_lps`) and adds
`q_long_lps` (mean rate over 300-900 s), `q_minute_min_lps` and
`q_minute_max_lps` (extremes of the one-minute rates in that span),
`long_windows` (their count) and `steady` (YES when they stay within 2 % of the
mean). `continuity_pct` and `duration_s` now describe the 900 s run. On
23 November seven rows were not steady at the 0.5 s step (S2_2, S2_3, S2_5,
S4_1, S4_4, S4_6, S4_7). The S4 rows dropped and recovered for minutes at a time
because the spill limit read dynamic wave's booked node volume, which drifted
away from the pond the node's depth holds; the limit uses that pond since
24 November and all S4 rows are steady (`DryFloodplainSpillHoldsItsRate` guards
S4_6). S2_2, S2_3 and S2_5 stayed outside the 2 % measure until 27 November:
the deck's default head tolerance (0.005 ft, 1.5 mm) was wider than the
`WEIR_ORIFICE` free/submerged band (0.1 h_s, about 1 mm on this rig), so the
node head wandered across the band and S2_2 drained in bursts. The test decks
set `HEAD_TOLERANCE 0.00001`, as the archived comparison did, and all 36 rows
are steady; the other rows moved by at most 0.005 L/s and S2_2 settled at
-0.134 L/s, the same bias as its S2 neighbors. `PhysicalShaftSensitivity` fails
when any row is not steady. Since 27 November the decks also author
`H_EPS=0.0033333` (0.02/6 m), the regularization head of the law's root at
the rig scale that the law-only test has always used: the S2 rows rise by
1.22 to 1.30 (S2_2 to -0.164 L/s) and the S1 rows by about a tenth, S3 and S4
do not move. Also since 27 November the law drowns the weir once the sewer head
reaches the crest (the blend band sits below the crest, see the hydraulics
manual), so S2_1, whose sewer head is at the crest, drains at -0.154 L/s
(-36 %) instead of the free-weir 0.75 L/s (+212 %); it is the only row that
moves. The other tests run 300 s.

Review report and proposed publication text: `docs/validation/rubinato2017/`.
The original junction representation is retained as evidence of its large
continuity errors. Neither that representation nor the physical-shaft
sensitivity establishes spatial convergence or independent measured
unsteady validation.
