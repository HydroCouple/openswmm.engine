# forcemain-friction-reference-curves

## Purpose
Verify the Hazen-Williams and Darcy-Weisbach friction slope computations against
exact analytical reference values across a velocity range typical of pressurised
force mains.

## Physical setup
- **Pipe**: R_h = 0.5 ft (full-pipe hydraulic radius); D_pipe = 4·R_h = 2.0 ft
- **HW**: C = 130 (smooth pipe; typical range 100–150)
- **DW**: roughness ε = 0.005 ft (concrete; typical range 0.001–0.01 ft)
- **Velocities**: 0.5, 1, 2, 4, 8, 12 ft/s

Note: for a full circular pipe, hydraulic radius R_h = D_pipe/4, so D_pipe = 4·R_h.
The HW formula uses R_h directly; the DW formula uses D_pipe throughout.

## Analytical formulas

**Hazen-Williams** (US customary):

    Sf = [v / (1.318 · C · R_h^0.63)]^(1/0.54)

**Darcy-Weisbach** (turbulent, Swamee-Jain):

    D = 4·R_h = 2.0 ft   (pipe diameter)
    Re = v · D / ν,   ν = 1.1×10⁻⁵ ft²/s
    f  = 0.25 / [log₁₀(ε/(3.7D) + 5.74/Re^0.9)]²
    Sf = f · v² / (2 · g · D),   g = 32.2 ft/s²

## Columns
| Column | Units | Description |
|--------|-------|-------------|
| model | — | "HW" or "DW" |
| v_fps | ft/s | Flow velocity |
| R_ft | ft | Hydraulic radius |
| param | — | C (HW) or roughness ε (DW) |
| Sf_ref | — | Friction slope (analytical) |

## Expected accuracy
The C++ formulas are direct transcriptions — no table interpolation.
Test tolerance: 1×10⁻¹⁰ (relative) for all rows.

## Verification target

`src/engine/hydraulics/ForceMains.cpp` — `getFricSlope_HW` and `getFricSlope_DW`.

## Consuming test

`tests/unit/engine/test_routing.cpp` — `TEST(ForceMain, FrictionReferenceCurvesBenchmark)`

## References

- Williams, G.S. and Hazen, A. (1905). *Hydraulic Tables*, 3rd ed. Wiley. (Hazen-Williams friction formula).
- Colebrook, C.F. (1939). Turbulent flow in pipes, with particular reference to the transition region between smooth and rough pipe laws. *Journal of the Institution of Civil Engineers*, 11, 133–156. (Colebrook-White equation basis for DW friction).
- Swamee, P.K. and Jain, A.K. (1976). Explicit equations for pipe-flow problems. *Journal of the Hydraulics Division, ASCE*, 102(5), 657–664. (explicit approximation of Colebrook-White used by SWMM).
