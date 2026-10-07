from pathlib import Path
import shutil
p=Path(__file__).resolve().parent;dest=p/'src_selected';shutil.copytree(p/'src_recon',dest,dirs_exist_ok=True)
f=dest/'src/engine/2d/solver/SweKernels.hpp';s=f.read_text();s=s.replace('OPENSWMM_KERNEL_FN bool faceFluxRecon(', 'OPENSWMM_KERNEL_FN bool faceFluxReconBed(',1)
a=s.index('/**\n * @brief Face flux from RECONSTRUCTED');b=s.index('OPENSWMM_KERNEL_FN bool faceFluxReconBed',a)
s=s[:a]+'''/**
 * @brief Second-order face flux with independently reconstructed depth/bed.
 *
 * zLf/zRf are face beds from the limited eta and depth reconstruction.
 * zL/zR remain the cell-equivalent beds. The pressure reference deliberately
 * uses eta_face - z_cell: relative to the usual hydrostatic face correction,
 * this includes the within-cell bed source. Keeping that reference makes a
 * constant free surface balance exactly, including a first-order neighbour.
 * On a flat bed it reduces to the original homogeneous MUSCL flux.
 */
'''+s[b:]
a=s.index('/// Barth');s=s[:a]+'''/// Piecewise-constant-bed compatibility entry point.
OPENSWMM_KERNEL_FN bool faceFluxRecon(double etaLf, double uxLf, double uyLf,
                                      double zL, double hL_cell,
                                      double etaRf, double uxRf, double uyRf,
                                      double zR, double hR_cell,
                                      double nx, double ny, double h_dry,
                                      FaceFlux& out,
                                      double& corrL_x, double& corrL_y,
                                      double& corrR_x, double& corrR_y) noexcept {
    return faceFluxReconBed(etaLf, uxLf, uyLf, zL, hL_cell,
                            etaRf, uxRf, uyRf, zR, hR_cell,
                            nx, ny, h_dry, out, corrL_x, corrL_y,
                            corrR_x, corrR_y, zL, zR);
}

'''+s[a:];f.write_text(s)
f=dest/'src/engine/2d/solver/ExplicitInertialSolver.cpp';s=f.read_text().replace('swe::faceFluxRecon(', 'swe::faceFluxReconBed(')
s=s.replace('''            // MUSCL: extrapolate (η, u, v) from each centroid to the face
            // midpoint along the precomputed Perot arms; the bed stays
            // piecewise constant per cell.''','''            // MUSCL: extrapolate eta, depth and velocity to the face.
            // Reconstructing depth with eta removes the artificial bed step
            // that otherwise remains even on a smooth, fully wet slope.''')
s=s.replace('// RECONSTRUCTION_ORDER 2: Green-Gauss gradients of (η, u, v) over the active','// RECONSTRUCTION_ORDER 2: Green-Gauss gradients of (eta, u, v, h) over the active')
f.write_text(s)
f=dest/'src/engine/2d/solver/ExplicitInertialSolver.hpp';s=f.read_text();s=s.replace('    std::vector<double> ghx_, ghy_;','    /// Limited depth gradients supply the second-order face bed (eta - h).\n    std::vector<double> ghx_, ghy_;');f.write_text(s)
print('Selected candidate prepared with compatible kernel wrapper')
