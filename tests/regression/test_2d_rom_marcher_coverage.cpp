/**
 * @file test_2d_rom_marcher_coverage.cpp
 * @brief P3/W3 — 2D ROM uncertainty bands vs brute-force Monte Carlo under the
 *        EXPLICIT LOCAL-INERTIAL MARCHER (ExplicitInertialSolver).
 *
 * This is the 2D analogue of tests/regression/test_rom_coverage.cpp (PR-10,
 * which validated the 1D ROM against MC). It exists because CVODE/ARKODE were
 * retired on swmm6_rel (2026-07-31) and the 2D deviation operator now rides a
 * local-inertial marcher. Two prototypes
 * (docs/uncertainty/prototypes/local_inertial_decay*.py) showed the deviation
 * operator collapses to a FLOW-ALIGNED ANISOTROPIC advection–diffusion:
 *     ∂δh/∂t = D∥·∂²∥δh + D⊥·∂²⊥δh − c_k·∂∥δh,
 *     D∥ ≈ 0.31·K_eff,  D⊥ ≈ 1.0·K_eff  (D⊥/D∥ ≈ 3.2),  c_k = (5/3)u.
 * The old 2D ROM uses an ISOTROPIC geometric Laplacian. This test measures how
 * much that mismatch costs in coverage/width against the real marcher, and —
 * via a streamwise/transverse split — tells us which rung of the fidelity
 * ladder (isotropic-scalar → anisotropic-tensor → +advection) is required.
 *
 * ── BUILD PRECONDITIONS (this is a SKETCH keyed to the post-P3 tree) ─────────
 *   1. The port branch must be rebased onto the marcher swmm6_rel so that
 *      `2d/solver/ExplicitInertialSolver.hpp` exists alongside the sidecar.
 *   2. W1 done: the geometric eigenbasis is standalone. Here we call the
 *      existing `SpectralPrecond2D::build()`; W1 renames it `MeshEigenBasis`
 *      (drop-in — same P / eigenvalues fields).
 *   3. W2 done is NOT required: this test drives the marcher and SpectralROM
 *      DIRECTLY (operator-level validation), the same way test_2d_spectral_rom
 *      and test_2d_inertial_marcher already drive their objects. An engine
 *      end-to-end (.inp) variant is a follow-on once the 2D-mesh input path is
 *      exercised.
 * Not registered in CMake yet — add once (1) lands.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "2d/data/BoundaryData.hpp"
#include "2d/data/MeshData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"   // (1) post-rebase
#include "2d/solver/InertialKernels.hpp"
#include "2d/solver/SpectralPrecond2D.hpp"         // (2) W1 -> MeshEigenBasis
#include "2d/uncertainty/SpectralROM.hpp"

using namespace openswmm::twoD;

namespace {

// ─── Configuration ──────────────────────────────────────────────────────────
constexpr int    kM          = 25;     // ensemble / MC members
constexpr double kPert        = 0.20;   // ±20% Manning prior (BOTH MC and ROM)
constexpr int    kModes       = 24;     // ROM modes
constexpr double kBaseN       = 0.03;
constexpr double kSlopeX      = 0.002;  // bed slope in +x  → mean flow in +x
constexpr double kReportStep  = 30.0;   // s
constexpr int    kReports     = 20;     // 10-minute window
constexpr int    kNx          = 40;
constexpr int    kNy          = 40;
constexpr double kDx          = 5.0;    // m
// Off-centre bump location (shared by the IC and the anisotropy classifier).
constexpr double kBumpX0      = 0.30 * kNx * kDx;
constexpr double kBumpY0      = 0.35 * kNy * kDx;
constexpr double kBumpSig     = 12.0;

// LHS strata midpoints of the ±20% uniform prior — the SHARED design the MC
// runs sample one-per-member and the ROM receives via setExternalSamples(),
// so member i means the same Manning multiplier on both sides (like-for-like).
std::vector<double> sharedMultipliers() {
    std::vector<double> m(kM);
    for (int i = 0; i < kM; ++i)
        m[i] = (1.0 - kPert) + (i + 0.5) / kM * 2.0 * kPert;
    return m;
}

// nx×ny quad-split-triangle mesh; bed sloped in +x; Manning n uniform (scaled
// per member by the caller). Mirrors makeGridMesh in test_2d_inertial_marcher.
MeshData makeSlopedMesh(double n_mult) {
    MeshData mesh;
    const int nvx = kNx + 1, nvy = kNy + 1;
    mesh.resize_vertices(nvx * nvy);
    for (int j = 0; j < nvy; ++j)
        for (int i = 0; i < nvx; ++i) {
            const int v = j * nvx + i;
            mesh.vx[v] = i * kDx;
            mesh.vy[v] = j * kDx;
            // Bed descends toward +x so the mean flow is unambiguously +x
            // (fixes the streamwise axis for the anisotropy split below).
            mesh.vz[v] = kSlopeX * (kNx * kDx - i * kDx);
        }
    mesh.resize_triangles(2 * kNx * kNy);
    int t = 0;
    for (int j = 0; j < kNy; ++j)
        for (int i = 0; i < kNx; ++i) {
            const int v00 = j * nvx + i,       v10 = j * nvx + i + 1;
            const int v01 = (j + 1) * nvx + i, v11 = (j + 1) * nvx + i + 1;
            mesh.tri_v0[t] = v00; mesh.tri_v1[t] = v10; mesh.tri_v2[t] = v11; ++t;
            mesh.tri_v0[t] = v00; mesh.tri_v1[t] = v11; mesh.tri_v2[t] = v01; ++t;
        }
    for (int i = 0; i < mesh.n_triangles(); ++i)
        mesh.mannings_n[i] = kBaseN * n_mult;
    buildMeshTopology(mesh);
    return mesh;
}

// OFF-CENTRE wet Gaussian bump (centred bumps project to zero on the anti-
// symmetric low modes — the documented symmetry pitfall). Depth over a wet
// floor so the domain stays wet (marcher validity + ROM depth space).
SurfaceStateData makeBumpState(const MeshData& mesh, const SolverOptions2D& o) {
    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());
    const double floor_d = 0.10, amp = 0.10;
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        const double dx = mesh.tri_cx[i] - kBumpX0, dy = mesh.tri_cy[i] - kBumpY0;
        const double d = floor_d +
            amp * std::exp(-0.5 * (dx * dx + dy * dy) / (kBumpSig * kBumpSig));
        state.head[i]   = mesh.tri_cz[i] + d;
        state.depth[i]  = d;
        state.volume[i] = d * mesh.tri_area[i];
    }
    return state;
}

// Representative isotropic K_eff (mean over wet cells of the Manning
// diffusivity h^{5/3}/(n·√S)). This is the ONE scalar the isotropic operator
// gets — the prototype says the true D is anisotropic (0.31·K_eff streamwise,
// 1.0·K_eff transverse), so how well this single number serves is exactly what
// the coverage/width numbers below reveal.
double representativeKeff(const MeshData& mesh, const SurfaceStateData& s) {
    double sh = 0, sn = 0; int nw = 0;
    for (int i = 0; i < mesh.n_triangles(); ++i)
        if (s.depth[i] > 1e-3) { sh += s.depth[i]; sn += mesh.mannings_n[i]; ++nw; }
    if (!nw) return 1.0;
    const double h = sh / nw, n = sn / nw, S = std::max(kSlopeX, 1e-4);
    return std::pow(h, 5.0 / 3.0) / (n * std::sqrt(S));
}

// Drive the real marcher from the bump IC, sampling per-cell depth at each of
// kReports report boundaries. Returns depth[report][cell].
std::vector<std::vector<double>> driveMarcher(double n_mult) {
    auto mesh = makeSlopedMesh(n_mult);
    SolverOptions2D opts;                 // post-rebase default = INTEGRATOR EXPLICIT
    opts.cell_closure = CellClosure2D::FLAT;
    auto state = makeBumpState(mesh, opts);

    ExplicitInertialSolver solver;
    solver.initialize(mesh, state, opts);
    std::vector<std::vector<double>> out;
    out.reserve(kReports);
    for (int r = 0; r < kReports; ++r) {
        solver.advance(r * kReportStep, (r + 1) * kReportStep);
        out.emplace_back(state.depth.begin(),
                         state.depth.begin() + mesh.n_triangles());
    }
    solver.finalize();
    return out;
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
TEST(Rom2dMarcherCoverage, IsotropicBandsBracketMarcherMonteCarlo) {
    const auto mult = sharedMultipliers();

    // ── Brute-force MC: M perturbed-n marcher runs ──────────────────────────
    // mc[member][report][cell]
    std::vector<std::vector<std::vector<double>>> mc(kM);
    for (int i = 0; i < kM; ++i) mc[i] = driveMarcher(mult[i]);

    // Nominal (mult=1) run supplies h_det for the ROM anchor.
    const auto hdet = driveMarcher(1.0);

    // ── ROM run: same M multipliers via setExternalSamples ──────────────────
    auto mesh0 = makeSlopedMesh(1.0);
    SpectralPrecond2D basis;                       // W1: MeshEigenBasis
    ASSERT_TRUE(basis.build(mesh0, kModes)) << "eigenbasis build failed";
    const int nt = mesh0.n_triangles();

    SpectralROM rom;
    rom.basis         = &basis;
    rom.n_ensemble    = kM;
    rom.mannings_pert = kPert;
    rom.rainfall_pert = 0.0;
    std::vector<double> ones(kM, 1.0);
    rom.setExternalSamples(mult, ones);            // member i ↔ MC member i
    rom.initialize();

    SolverOptions2D o0; o0.cell_closure = CellClosure2D::FLAT;
    const double K_eff = representativeKeff(mesh0, makeBumpState(mesh0, o0));

    rom.seed(hdet[0].data());
    std::vector<std::vector<double>> q05(kReports), q50(kReports), q95(kReports);
    for (int r = 0; r < kReports; ++r) {
        rom.advance(kReportStep, K_eff, /*rainfall=*/nullptr,
                    /*h_cell=*/nullptr, /*h_det=*/hdet[r].data());
        rom.computeQuantiles(hdet[r].data());
        q05[r] = rom.q05; q50[r] = rom.q50; q95[r] = rom.q95;
    }

    // ── Compare in the saturated window (2nd half; deviation spread spins up
    //    from zero by construction — DEVIATION_FORM.md §4.3). ────────────────
    const int r0 = kReports / 2;
    const int lo = int(std::round(0.05 * (kM - 1)));  // nearest-rank q05
    const int md = int(std::round(0.50 * (kM - 1)));
    const int hi = int(std::round(0.95 * (kM - 1)));

    int n_tot = 0, n_cov = 0, n_w = 0, n_w_ok = 0;
    std::vector<double> ratios;
    // Anisotropy split: does band fidelity differ along- vs across-flow? Flow
    // is +x, so classify each cell by whether the nominal depth gradient is
    // predominantly streamwise (|∂h/∂x|>|∂h/∂y|) or transverse.
    struct Acc { int tot=0, cov=0, w=0; std::vector<double> ratio; } strm, tran;

    for (int r = r0; r < kReports; ++r) {
        for (int c = 0; c < nt; ++c) {
            std::vector<double> hc(kM);
            for (int i = 0; i < kM; ++i) hc[i] = mc[i][r][c];
            std::sort(hc.begin(), hc.end());
            const double mc_med = hc[md], mc_w = hc[hi] - hc[lo];
            if (mc_med < 1e-4) continue;             // dry / noise floor

            ++n_tot;
            const bool covered = q05[r][c] <= mc_med && mc_med <= q95[r][c];
            if (covered) ++n_cov;

            // Orientation proxy: for a single off-centre bump advecting/spreading
            // in +x, a cell's dominant depth-gradient direction is set by its
            // offset from the bump centre. |Δx|>|Δy| ⇒ the spread it sees is
            // predominantly STREAMWISE (D∥), else TRANSVERSE (D⊥). Geometric —
            // no mesh-connectivity accessor needed. (A face-gradient version is
            // strictly better once the MeshData neighbour API is confirmed.)
            const double adx = std::fabs(mesh0.tri_cx[c] - kBumpX0);
            const double ady = std::fabs(mesh0.tri_cy[c] - kBumpY0);
            const bool streamwise = adx >= ady;
            Acc& a = streamwise ? strm : tran;
            ++a.tot; if (covered) ++a.cov;

            if (mc_w > 1e-6) {
                const double ratio = (q95[r][c] - q05[r][c]) / mc_w;
                ++n_w; ratios.push_back(ratio);
                if (ratio >= 0.3 && ratio <= 3.0) ++n_w_ok;
                ++a.w; a.ratio.push_back(ratio);
            }
        }
    }
    ASSERT_GT(n_tot, 0);
    ASSERT_GT(n_w, 0) << "MC produced no resolvable spread — fixture too static";

    std::sort(ratios.begin(), ratios.end());
    const double coverage  = double(n_cov) / n_tot;
    const double w_frac    = double(n_w_ok) / n_w;
    const double ratio_med = ratios[ratios.size() / 2];
    auto medOf = [](std::vector<double> v) {
        std::sort(v.begin(), v.end()); return v.empty() ? 0.0 : v[v.size() / 2]; };

    std::printf("[2D-ROM-vs-marcher] samples=%d coverage=%.3f "
                "width-ratio med=%.3f in[0.3,3]=%.3f | streamwise med=%.3f "
                "transverse med=%.3f\n",
                n_tot, coverage, ratio_med, w_frac,
                medOf(strm.ratio), medOf(tran.ratio));

    // ── Assertions — INITIAL FLOORS (per HSYM checklist P4: "don't gate
    //    tighter than 2× initially; record actuals"). The isotropic operator
    //    is EXPECTED to be imperfect; this run's job is to quantify by how much
    //    and to expose the streamwise/transverse asymmetry. Tighten once the
    //    W1 fidelity rung is chosen. ──────────────────────────────────────────
    EXPECT_GE(coverage, 0.90) << "isotropic ROM bands must bracket the marcher "
                                 "MC median at >=90% of saturated samples";
    EXPECT_GE(w_frac, 0.80)   << "width ratio within [0.3x,3x] at >=80%";
    EXPECT_GE(ratio_med, 0.5) << "median width ratio implausibly low";
    EXPECT_LE(ratio_med, 2.0) << "median width ratio implausibly high";

    // Diagnostic (NOT yet a gate): a large streamwise-vs-transverse width-ratio
    // gap is the signal that the isotropic operator must become the flow-aligned
    // ANISOTROPIC Laplacian (P3_2D_REHOME_SPEC.md W3, rung 2). Record it.
    if (!strm.ratio.empty() && !tran.ratio.empty()) {
        const double gap = medOf(tran.ratio) / std::max(medOf(strm.ratio), 1e-9);
        std::printf("[2D-ROM-vs-marcher] anisotropy signal (transverse/streamwise "
                    "width-ratio) = %.2f  (>~1.5 → build the anisotropic operator)\n",
                    gap);
    }
}
