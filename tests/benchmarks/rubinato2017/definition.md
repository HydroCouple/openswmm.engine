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
