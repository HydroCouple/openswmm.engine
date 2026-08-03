/**
 * @file test_2d_rom_marcher_coverage.cpp
 * @brief W3 — 2D ROM uncertainty bands vs brute-force Monte Carlo under the
 *        explicit local-inertial marcher (ExplicitInertialSolver).
 *
 * @details The 2D analogue of test_rom_coverage.cpp (which validated the 1D
 *          ROM against MC). The marcher solves local-inertial dynamics; the
 *          linearized deviation operator about sustained Manning flow is a
 *          FLOW-ALIGNED ANISOTROPIC advection–diffusion
 *          (LOCAL_INERTIAL_DEVIATION_OPERATOR.md):
 *
 *              ∂δh/∂t = D∥·∂²∥δh + D⊥·∂²⊥δh − c_k·∂∥δh,
 *              D∥ = 0.62·D,  D⊥ = 2.0·D  (D = h̄^{5/3}/(2n̄√S)),  c_k = (5/3)·u.
 *
 *          FIXTURE — steady runoff plane, the operator's validity regime.
 *          Constant rainfall over a sloped plane draining through a
 *          NORMAL_FLOW outlet (the marcher's own Manning-steady gate fixture),
 *          spun to steady state before the scoring window. This is sustained
 *          friction-dominated flow (Λ = r_f/ck ≳ 3), the regime the linearized
 *          operator is derived for and the 2D counterpart of PR-10's
 *          "saturated regime" scoring. Each MC member runs the real marcher
 *          with its own scaled n and settles onto its own steady profile, so
 *          the window measures the sustained parametric spread. Transient
 *          drain-to-pond fixtures leave this envelope mid-run (u → 0 kills the
 *          advective sensitivity while the MC retains frozen positional
 *          spread); that regime is documented as a limitation in
 *          VALIDATION.md, not gated here — the same split PR-10 drew for
 *          SURCHARGED nodes and front-arrival timing.
 *
 *          MEMBERS ARE LIKE-FOR-LIKE: the same LHS Manning multiplier drives
 *          MC member i (a real marcher run) and ROM member i (via
 *          setExternalSamples).
 *
 *          RUNGS. Each operator rung is just what is assembled into the
 *          reduced M (one code path, dials):
 *            legacy : diagonal λ·K_eff on ungrounded graph-Laplacian
 *                     eigenvalues — the historical convention; recorded, not
 *                     gated.
 *            iso    : reduced M, isotropic physical FV diffusion.
 *            aniso  : + flow-aligned tensor (α∥ = 0.62, α⊥ = 2.0).
 *            adv    : + upwind advection at c_k = (5/3)·u  ← production rung.
 *
 *          GROUNDING (load-bearing — the 2D analogue of the 1D grounded
 *          Laplacian). Physical rungs build the eigenbasis AND the operator
 *          with outlet-adjacent cells grounded (a diagonal-only edge to a
 *          zero-deviation ghost across each open boundary face). A uniform
 *          Manning perturbation shifts the whole steady profile — a response
 *          carried mostly by the (quasi-)uniform mode, which a pure-Neumann
 *          basis excludes as the Laplacian null vector: ungrounded, the bands
 *          collapse to ≈0.39× the MC width no matter how the diffusivity is
 *          dialed, because the fixed point δa_ss = (mm−1)·Pᵀh_det cannot see
 *          the mean shift. Grounding restores it. The ground conductance is
 *          softened (×0.25) because a NORMAL_FLOW outlet is not an absorbing
 *          boundary — the member's own depth deviation persists at the outlet,
 *          so a full-strength zero ghost over-drains the near-outlet cells.
 *
 *          The flow field for aniso/adv comes from readable state only: a
 *          Green–Gauss surface-gradient estimate on the nominal depth field,
 *          u⃗ = −(h^{2/3}/(n√|∇η|))·∇η, which correctly vanishes where the
 *          surface is flat. The operator is reassembled once per report
 *          interval (basis-update cadence); the eigenbasis is built once.
 *
 *          GATE: the production rung (adv) must meet the floors —
 *          coverage ≥ 0.90, width-ratio median ∈ [0.5, 2], width ratio within
 *          [0.3, 3] for ≥ 80% of samples. Calibrated 2026-08-02 on this
 *          fixture: coverage 1.000, median 1.43, in-band 0.884 (k = 40,
 *          ground scale 0.25). The ~1.4× width bias is the conveyance-
 *          sensitivity overshoot also present in the 1D validation (PR-10
 *          median 1.25): the Manning-sensitivity fixed point responds with
 *          the full n-sensitivity of the conveyance while the steady depth
 *          responds as n^{3/5}. Never loosen these floors to make a rung
 *          pass — recalibrate the dials instead (they are the model, this is
 *          the meter).
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "2d/data/BoundaryData.hpp"
#include "2d/data/MeshData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/uncertainty/DeviationOperator2D.hpp"
#include "2d/uncertainty/MeshEigenBasis.hpp"
#include "2d/uncertainty/SpectralROM.hpp"

using namespace openswmm::twoD;

namespace {

// ─── Configuration (calibrated 2026-08-02; see the header note) ─────────────
constexpr int    kM        = 25;      // ensemble / MC members
constexpr double kPert     = 0.20;    // ±20% Manning prior (BOTH sides)
constexpr int    kModes    = 40;      // ROM modes (capture flattens by k≈40 here)
constexpr double kBaseN    = 0.03;
constexpr double kS        = 0.002;   // bed slope; z = S·x, outlet at x = 0
constexpr int    kNx       = 40, kNy = 40;
constexpr double kDx       = 5.0;     // m
constexpr double kRain     = 2.0e-4;  // m/s — the marcher gate's own rate
constexpr double kSpin     = 3000.0;  // s to steady state before the window
constexpr double kRep      = 60.0;    // s report step
constexpr int    kReports  = 30;      // 30-minute window
constexpr int    kScoreR0  = 20;      // score t = 20–30 min (fully saturated)

// Local-inertial linearization constants relative to D = h̄^{5/3}/(2n̄√S).
constexpr double kAlphaPar    = 0.62;
constexpr double kAlphaPerp   = 2.00;
constexpr double kCFactor     = 5.0 / 3.0;
// Softened outlet grounding (NORMAL_FLOW is not absorbing; see header).
constexpr double kGroundScale = 0.25;

std::vector<double> sharedMultipliers() {
    std::vector<double> m(kM);
    for (int i = 0; i < kM; ++i)
        m[static_cast<std::size_t>(i)] =
            (1.0 - kPert) + (i + 0.5) / kM * 2.0 * kPert;
    return m;
}

MeshData makeMesh(double n_mult) {
    MeshData mesh;
    const int nvx = kNx + 1, nvy = kNy + 1;
    mesh.resize_vertices(nvx * nvy);
    for (int j = 0; j < nvy; ++j)
        for (int i = 0; i < nvx; ++i) {
            const int v = j * nvx + i;
            mesh.vx[static_cast<std::size_t>(v)] = i * kDx;
            mesh.vy[static_cast<std::size_t>(v)] = j * kDx;
            mesh.vz[static_cast<std::size_t>(v)] = kS * (i * kDx);
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
        mesh.mannings_n[static_cast<std::size_t>(i)] = kBaseN * n_mult;
    buildMeshTopology(mesh);
    return mesh;
}

// Analytic nominal normal-depth profile — the spin-up seed.
void seedSteady(const MeshData& mesh, SurfaceStateData& s) {
    s.resize(mesh.n_triangles(), mesh.n_vertices());
    const double L = kNx * kDx;
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double q = kRain * std::max(L - mesh.tri_cx[ui], 0.0);
        const double h = (q > 0.0)
            ? std::pow(kBaseN * q / std::sqrt(kS), 3.0 / 5.0)
            : 1e-3;
        s.head[ui]   = mesh.tri_cz[ui] + h;
        s.depth[ui]  = h;
        s.volume[ui] = h * mesh.tri_area[ui];
    }
}

void addOutlet(const MeshData& mesh, BoundaryData& b) {
    b.resize(mesh.n_triangles() * 3);
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const int nbrs[3] = {mesh.tri_nbr0[ui], mesh.tri_nbr1[ui],
                             mesh.tri_nbr2[ui]};
        for (int e = 0; e < 3; ++e) {
            const int idx = i * 3 + e;
            if (nbrs[e] >= 0) continue;
            if (mesh.edge_mx[static_cast<std::size_t>(idx)] < 1e-9) {
                b.edge_bc_type[static_cast<std::size_t>(idx)] =
                    static_cast<int8_t>(BoundaryType::NORMAL_FLOW);
                b.edge_bed_slope[static_cast<std::size_t>(idx)] = kS;
            }
        }
    }
}

// Softened grounding conductances at the outlet faces:
// kGroundScale · len / (2·dist(centroid, face midpoint)).
std::vector<double> groundWeights(const MeshData& mesh) {
    std::vector<double> g(static_cast<std::size_t>(mesh.n_triangles()), 0.0);
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const int nbrs[3] = {mesh.tri_nbr0[ui], mesh.tri_nbr1[ui],
                             mesh.tri_nbr2[ui]};
        for (int e = 0; e < 3; ++e) {
            const auto idx = static_cast<std::size_t>(i * 3 + e);
            if (nbrs[e] >= 0) continue;
            if (mesh.edge_mx[idx] >= 1e-9) continue;   // outlet = x = 0 faces
            const double dx = mesh.edge_mx[idx] - mesh.tri_cx[ui];
            const double dy = mesh.edge_my[idx] - mesh.tri_cy[ui];
            const double d  = 2.0 * std::sqrt(dx * dx + dy * dy);
            if (d < 1e-12) continue;
            g[ui] += kGroundScale * mesh.edge_length[idx] / d;
        }
    }
    return g;
}

std::vector<std::vector<double>> driveMarcher(double n_mult) {
    auto mesh = makeMesh(n_mult);
    SolverOptions2D opts;
    opts.cell_closure = CellClosure2D::FLAT;
    SurfaceStateData state;
    seedSteady(mesh, state);
    BoundaryData b;
    addOutlet(mesh, b);
    state.boundary = &b;
    for (int i = 0; i < mesh.n_triangles(); ++i)
        state.rainfall[static_cast<std::size_t>(i)] = kRain;

    ExplicitInertialSolver solver;
    solver.initialize(mesh, state, opts);
    solver.advance(0.0, kSpin);
    std::vector<std::vector<double>> out;
    out.reserve(kReports);
    for (int r = 0; r < kReports; ++r) {
        solver.advance(kSpin + r * kRep, kSpin + (r + 1) * kRep);
        out.emplace_back(state.depth.begin(),
                         state.depth.begin() + mesh.n_triangles());
    }
    solver.finalize();
    return out;
}

// Classic diffusion-wave diffusivity D = h̄^{5/3}/(2n̄√S) over wet cells (m²/s).
double classicDiffusivity(const MeshData& mesh, const std::vector<double>& h) {
    double sh = 0.0;
    int nw = 0;
    for (int i = 0; i < mesh.n_triangles(); ++i)
        if (h[static_cast<std::size_t>(i)] > 1e-3) {
            sh += h[static_cast<std::size_t>(i)];
            ++nw;
        }
    if (nw == 0) return 0.0;
    return std::pow(sh / nw, 5.0 / 3.0) / (2.0 * kBaseN * std::sqrt(kS));
}

// Per-cell Manning velocity from readable state (Green–Gauss ∇η; see header).
void manningVelocity(const MeshData& mesh, const std::vector<double>& h,
                     std::vector<double>& u, std::vector<double>& v) {
    const int n = mesh.n_triangles();
    u.assign(static_cast<std::size_t>(n), 0.0);
    v.assign(static_cast<std::size_t>(n), 0.0);
    for (int i = 0; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double eta_i = mesh.tri_cz[ui] + h[ui];
        const int nbrs[3] = {mesh.tri_nbr0[ui], mesh.tri_nbr1[ui],
                             mesh.tri_nbr2[ui]};
        double gx = 0.0, gy = 0.0, w = 0.0;
        for (int e = 0; e < 3; ++e) {
            const int j = nbrs[e];
            if (j < 0) continue;
            const auto uj = static_cast<std::size_t>(j);
            const double dx = mesh.tri_cx[uj] - mesh.tri_cx[ui];
            const double dy = mesh.tri_cy[uj] - mesh.tri_cy[ui];
            const double d2 = dx * dx + dy * dy;
            if (d2 < 1e-20) continue;
            const double de =
                (mesh.tri_cz[uj] + h[uj]) - eta_i;
            const double len =
                mesh.edge_length[static_cast<std::size_t>(i * 3 + e)];
            gx += len * de * dx / d2;
            gy += len * de * dy / d2;
            w += len;
        }
        if (w < 1e-14) continue;
        gx /= w; gy /= w;
        const double g = std::sqrt(gx * gx + gy * gy);
        if (g < 1e-8 || h[ui] < 1e-3) continue;
        const double sp = std::pow(h[ui], 2.0 / 3.0) * std::sqrt(g) / kBaseN;
        u[ui] = -sp * gx / g;
        v[ui] = -sp * gy / g;
    }
}

// ─── Rungs ──────────────────────────────────────────────────────────────────

enum class Rung { LEGACY, ISO, ANISO, ANISO_ADV };

const char* rungName(Rung r) {
    switch (r) {
        case Rung::LEGACY:    return "legacy";
        case Rung::ISO:       return "iso   ";
        case Rung::ANISO:     return "aniso ";
        case Rung::ANISO_ADV: return "adv   ";
    }
    return "?";
}

struct RomBands {
    std::vector<std::vector<double>> q05, q95;
    bool ok = false;
};

RomBands runRomRung(Rung rung, const MeshData& mesh0,
                    const MeshEigenBasis& basis,
                    const std::vector<double>& ground_w,
                    const std::vector<double>& mult,
                    const std::vector<std::vector<double>>& hdet,
                    const std::vector<double>& h0) {
    SpectralROM rom;
    rom.basis         = &basis;
    rom.n_ensemble    = kM;
    rom.mannings_pert = kPert;
    rom.rainfall_pert = 0.0;
    std::vector<double> ones(kM, 1.0);
    rom.setExternalSamples(mult, ones);   // member i ↔ MC member i
    rom.initialize();
    rom.seed(h0.data());

    // Legacy convention: K_eff = h̄^{5/3}/(n̄√S) (no ½), frozen at window start.
    const double K_legacy = 2.0 * classicDiffusivity(mesh0, h0);

    DeviationOperator2D op;
    std::vector<double> vel_u, vel_v;

    RomBands out;
    out.q05.resize(kReports);
    out.q95.resize(kReports);

    for (int r = 0; r < kReports; ++r) {
        const std::vector<double>& h_prev = (r == 0) ? h0 : hdet[r - 1];

        if (rung != Rung::LEGACY) {
            switch (rung) {
                case Rung::ISO:
                    op.alpha_par = op.alpha_perp = 1.0;
                    op.c_factor  = 0.0;
                    break;
                case Rung::ANISO:
                    op.alpha_par  = kAlphaPar;
                    op.alpha_perp = kAlphaPerp;
                    op.c_factor   = 0.0;
                    break;
                case Rung::ANISO_ADV:
                    op.alpha_par  = kAlphaPar;
                    op.alpha_perp = kAlphaPerp;
                    op.c_factor   = kCFactor;
                    break;
                default: break;
            }
            manningVelocity(mesh0, h_prev, vel_u, vel_v);
            const double D = classicDiffusivity(mesh0, h_prev);
            if (!op.assemble(mesh0, basis, D, h_prev.data(),
                             vel_u.data(), vel_v.data(), ground_w.data()))
                return out;   // ok stays false
            rom.setReducedOperator(op.M);
        }

        rom.advance(kRep, K_legacy, /*rainfall=*/nullptr,
                    /*h_cell=*/nullptr, /*h_det=*/hdet[r].data());
        rom.computeQuantiles(hdet[r].data());
        out.q05[r] = rom.q05;
        out.q95[r] = rom.q95;
    }
    out.ok = true;
    return out;
}

// ─── Scoring ────────────────────────────────────────────────────────────────

struct RungScore {
    int    samples = 0;
    double coverage = 0.0, ratio_med = 0.0, w_frac = 0.0;
    bool passes() const {
        return coverage >= 0.90 && w_frac >= 0.80 &&
               ratio_med >= 0.5 && ratio_med <= 2.0;
    }
};

RungScore scoreBands(const RomBands& bands, const MeshData& mesh0,
                     const std::vector<std::vector<std::vector<double>>>& mc) {
    const int nt = mesh0.n_triangles();
    const int lo = static_cast<int>(std::round(0.05 * (kM - 1)));
    const int md = static_cast<int>(std::round(0.50 * (kM - 1)));
    const int hi = static_cast<int>(std::round(0.95 * (kM - 1)));

    int n_tot = 0, n_cov = 0, n_w = 0, n_w_ok = 0;
    std::vector<double> ratios;
    std::vector<double> hc(kM);

    for (int r = kScoreR0; r < kReports; ++r) {
        const auto ur = static_cast<std::size_t>(r);
        for (int c = 0; c < nt; ++c) {
            const auto uc = static_cast<std::size_t>(c);
            for (int i = 0; i < kM; ++i)
                hc[static_cast<std::size_t>(i)] =
                    mc[static_cast<std::size_t>(i)][ur][uc];
            std::sort(hc.begin(), hc.end());
            const double mc_med = hc[static_cast<std::size_t>(md)];
            const double mc_w   = hc[static_cast<std::size_t>(hi)]
                                - hc[static_cast<std::size_t>(lo)];
            if (mc_med < 1e-4) continue;

            ++n_tot;
            if (bands.q05[ur][uc] <= mc_med && mc_med <= bands.q95[ur][uc])
                ++n_cov;
            if (mc_w > 1e-6) {
                const double ratio =
                    (bands.q95[ur][uc] - bands.q05[ur][uc]) / mc_w;
                ++n_w;
                ratios.push_back(ratio);
                if (ratio >= 0.3 && ratio <= 3.0) ++n_w_ok;
            }
        }
    }

    RungScore s;
    s.samples  = n_tot;
    s.coverage = n_tot ? static_cast<double>(n_cov) / n_tot : 0.0;
    s.w_frac   = n_w ? static_cast<double>(n_w_ok) / n_w : 0.0;
    if (!ratios.empty()) {
        std::sort(ratios.begin(), ratios.end());
        s.ratio_med = ratios[ratios.size() / 2];
    }
    return s;
}

} // namespace

// ════════════════════════════════════════════════════════════════════════════

TEST(Rom2dMarcherCoverage, ProductionRungBandsBracketMarcherMonteCarlo) {
    const auto mult = sharedMultipliers();

    // ── Brute-force MC: kM perturbed-n marcher runs + the nominal anchor ────
    std::vector<std::vector<std::vector<double>>> mc(kM);
    for (int i = 0; i < kM; ++i)
        mc[static_cast<std::size_t>(i)] =
            driveMarcher(mult[static_cast<std::size_t>(i)]);
    const auto hdet = driveMarcher(1.0);

    // ── Shared fixture state for the ROM side ───────────────────────────────
    auto mesh0 = makeMesh(1.0);
    const auto gw = groundWeights(mesh0);

    // Grounded basis for the physical rungs; ungrounded for the legacy record.
    MeshEigenBasis basis_grounded, basis_neumann;
    ASSERT_TRUE(basis_grounded.build(mesh0, kModes, gw.data()));
    ASSERT_TRUE(basis_neumann.build(mesh0, kModes));

    SurfaceStateData s0;
    seedSteady(mesh0, s0);
    const std::vector<double> h0(s0.depth.begin(),
                                 s0.depth.begin() + mesh0.n_triangles());

    // ── Run every rung against the same MC truth ────────────────────────────
    const Rung rungs[] = {Rung::LEGACY, Rung::ISO, Rung::ANISO,
                          Rung::ANISO_ADV};
    RungScore scores[4];
    for (int q = 0; q < 4; ++q) {
        const MeshEigenBasis& basis =
            (rungs[q] == Rung::LEGACY) ? basis_neumann : basis_grounded;
        RomBands bands =
            runRomRung(rungs[q], mesh0, basis, gw, mult, hdet, h0);
        ASSERT_TRUE(bands.ok) << "rung " << rungName(rungs[q])
                              << ": operator assembly failed";
        scores[q] = scoreBands(bands, mesh0, mc);
        std::printf("[2D-ROM-vs-marcher] %s coverage=%.3f width-med=%.3f "
                    "in[0.3,3]=%.3f (n=%d)%s\n",
                    rungName(rungs[q]), scores[q].coverage,
                    scores[q].ratio_med, scores[q].w_frac, scores[q].samples,
                    scores[q].passes() ? "  [PASSES FLOORS]" : "");
    }

    ASSERT_GT(scores[0].samples, 0) << "MC produced no wet samples";

    // ── Gate the production rung (adv). Floors per the header; calibrated
    //    actuals 2026-08-02: coverage 1.000, median 1.43, in-band 0.884. ─────
    const RungScore& prod = scores[3];
    EXPECT_GE(prod.coverage, 0.90)
        << "production-rung bands must bracket the marcher MC median";
    EXPECT_GE(prod.w_frac, 0.80)
        << "width ratio within [0.3,3] at >=80% of samples";
    EXPECT_GE(prod.ratio_med, 0.5) << "median width ratio implausibly low";
    EXPECT_LE(prod.ratio_med, 2.0) << "median width ratio implausibly high";
}
