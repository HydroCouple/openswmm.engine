# xsect-circular-ellipse-reference

## Purpose
Verify that the CIRCULAR cross-section lookup-table area function matches the
exact analytical formula (circular = ellipse with semi-axes a=b=R) across the
full depth range.

## Physical setup
- **Shape**: CIRCULAR pipe, D = 3.0 ft (R = 1.5 ft)
- **Full area**: A_full = π R² = π·2.25 ≈ 7.06858 ft²

## Analytical formula
For an ellipse with equal semi-axes (a = b = R) this reduces to the standard
circular segment formula:

    A(y) = R² · [(y/R − 1)·√(1 − (y/R − 1)²) + arcsin(y/R − 1) + π/2]

which is equivalent to:

    A(y) = R² · [arccos(1 − y/R) − (1 − y/R)·√(1 − (1 − y/R)²)]

## Columns
| Column | Units | Description |
|--------|-------|-------------|
| y_norm | — | Normalised depth y/D (0 to 1) |
| y_ft | ft | Absolute depth |
| A_analytical_ft2 | ft² | Exact analytical area |
| A_over_Afull | — | A / A_full |

## Expected accuracy
The SWMM `A_Circ` table has 51 points (spacing Δy_norm = 0.02) with quadratic
interpolation. Test tolerance: 0.5% of A_full except near y_norm = 0 where the
table uses linear leading to ~1% error in the first cell; the test uses 0.5%
of A_full as an absolute tolerance for all rows.

## Verification target

`src/engine/hydraulics/XSect.cpp` — `xsect::getArea` (CIRCULAR table lookup with quadratic interpolation).

## Consuming test

`tests/unit/engine/test_xsection.cpp` — `TEST(XSectionCircular, AreaMatchesEllipseReferenceBenchmark)`

## References

- Chow, V.T. (1959). *Open Channel Hydraulics*. McGraw-Hill. Appendix A (geometric properties of circular and other channel cross-sections).
- Rossman, L.A. and Huber, W.C. (2016). *Storm Water Management Model Reference Manual Volume II — Hydraulics*. EPA/600/R-17/111. Appendix A (A_Circ cross-section geometry table).
