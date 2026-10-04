from pathlib import Path
p=Path(__file__).resolve().parent;src=p/'src_selected';f=src/'src/engine/2d/solver/ExplicitInertialSolver.cpp';s=f.read_text();a=s.index('// RECONSTRUCTION_ORDER 2: Green-Gauss');b=s.index('void ExplicitInertialSolver::computeLimitedGradientsSwe()',a);s=s[:a]+'''// RECONSTRUCTION_ORDER 2: limited Green-Gauss gradients in connected wet
// interiors; a conditioned wet-neighbour fit of eta and depth at shorelines.
// Shoreline velocity stays cell-centered. Thin cells and insufficient wet
// stencils stay first order. All face-midpoint depths remain nonnegative.
'''+s[b:];f.write_text(s)
f=src/'src/engine/2d/solver/SweKernels.hpp';s=f.read_text();a=s.index(' * zLf/zRf are face beds');b=s.index(' */',a);s=s[:a]+''' * zLf/zRf are face beds from the limited eta and depth reconstruction;
 * zL/zR are the cell-equivalent beds. Add the hydrostatic face correction
 * and the centered bed contribution -g*(h_cell+h_face)/2*(z_face-z_cell).
 * The latter integrates a linear depth along the cell-to-face bed segment.
 * For constant eta this reduces to g/2*(h_star^2-h_cell^2), preserving
 * lake-at-rest balance even next to a first-order cell. On a flat bed the
 * within-cell contribution vanishes. Face-depth positivity and cell-centered
 * shoreline velocity are enforced by computeLimitedGradientsSwe().
'''+s[b:];f.write_text(s)
f=src/'src/engine/2d/solver/ExplicitInertialSolver.hpp';s=f.read_text().replace('/// RECONSTRUCTION_ORDER 2 (FULL_SWE): limited Green-Gauss gradients of','/// RECONSTRUCTION_ORDER 2 (FULL_SWE): limited reconstruction gradients of');f.write_text(s)
f=src/'src/engine/2d/data/SolverOptions2D.hpp';s=f.read_text().replace('/// with the Barth–Jespersen-limited Green-Gauss gradient + SSP-RK2','/// with limited Green-Gauss / wet-shoreline gradients + SSP-RK2');f.write_text(s)
