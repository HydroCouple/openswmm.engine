/**
 * @file test_2d_rom_marcher_coverage_corr.cpp
 * @brief W4 — 2D validation breadth: correlated-field marcher Monte Carlo and
 *        a second surface, measured with member coverage on every rung.
 *
 * @details Every 2D band claim before W4 rested on ONE cell: the W3 steady
 *          runoff plane with a comonotone (spatially uniform) Manning
 *          multiplier (test_2d_rom_marcher_coverage.cpp, member coverage 0.82,
 *          width-med 1.32). SP3 wired `COHERENCE CORR_LEN` on the 2D ROM with
 *          no correlated MC behind it at all. W4 is a MEASUREMENT, not a fix:
 *          it builds the missing MC cells and records member coverage; it
 *          asserts nothing new about band magnitude (C1/C2 verdicts are
 *          printed; any cell below the floor gets a named follow-up in
 *          VALIDATION.md).
 *
 *          CELLS (each MC member is a real marcher run; member i of the ROM
 *          sees the SAME field as MC member i — like-for-like):
 *
 *            plane  / correlated RAIN,   corr_len 20 m and 100 m  (adv rung)
 *            plane  / correlated MANNING, corr_len 20 m and 100 m
 *            channel/ comonotone Manning, all four rungs
 *            channel/ correlated RAIN,   corr_len 20 m          (adv rung)
 *
 *          Correlated fields come from the engine's own `SpdeSpatialBasis`
 *          (Matérn ν=2, CL-2b) seeded with the ROM's own per-member
 *          coefficients, so the ROM's CORR_LEN path (reduced ψ_m/a_im when
 *          K_s < M, materialized field otherwise — the same branch rule
 *          SurfaceRouter2D::buildGridSoftField applies) is driven by the
 *          identical realization the marcher is run with:
 *
 *            rain_i(t)    = R · (1 + CV · W_i(t)),      NORMAL family
 *            n_i(t)       = n̄ · (1 + p  · V_i(t)),      UNIFORM family
 *
 *          with W, V zero-mean per cell (column mean = mean_i c_i) and
 *          per-point variance Var(c), exactly the CL-2b contract.
 *
 *          STRUCTURAL FACT THIS HARNESS EXPOSED (W4) AND H15 FIXED: before
 *          H15 `SpectralROM::advance()` took the reduced-operator path only
 *          when `spatial_mannings` was NOT set, so a spatially correlated
 *          Manning field ran on the DIAGONAL λ·K_eff path regardless of rung.
 *          H15 added per-member reduced operators (one assembly per member
 *          with cond_mult = 1/W_i, `setReducedOperatorPerMember`). The
 *          correlated-Manning cells now measure that path ("adv/per-member")
 *          and keep the pre-H15 diagonal result as the reference row. The
 *          correlated-RAIN cells always reached the production operator.
 *
 *          H15 MEASURED 2026-10-07 (M = 25):
 *            plane-mann-corr20   adv/per-member 0.801 / 0.92  (pre-H15 diag 0.649 / 0.68)
 *            plane-mann-corr100  adv/per-member 0.844 / 1.32  (pre-H15 diag 0.835 / 1.24)
 *          The 20 m cell clears the floor by 0.001 -- recorded as calibrated,
 *          deliberately NOT asserted (a margin that thin is a coin flip, and
 *          asserting it would amount to tuning). Still ~8% narrow there.
 *
 *          SECOND SURFACE: a channel with a defined thalweg,
 *          z = S·x + T·|y − y_c|, so flow converges laterally onto the centre
 *          line and then runs streamwise. Along-flow and across-flow are no
 *          longer aligned with the mesh everywhere, and the wet area is a band
 *          rather than the whole plane — the case the anisotropic operator was
 *          built for and never measured on.
 *
 *          MEASURED 2026-10-07 (M = 25, member coverage / median width ratio):
 *            plane-rain-corr20    adv   0.897 / 1.31  calibrated   (materialized, K_s=64)
 *            plane-rain-corr100   adv   0.886 / 1.42  calibrated   (reduced, K_s=23)
 *            plane-mann-corr20    diag  0.649 / 0.68  ranking only (grounded basis; Neumann 0.39/0.29)
 *            plane-mann-corr100   diag  0.835 / 1.24  calibrated   (grounded basis; Neumann 0.48/0.39)
 *            channel-mann-comono  adv   0.946 / 2.93  conservative (legacy 3.04, iso 2.20, aniso 2.28)
 *            channel-rain-corr20  adv   0.910 / 1.60  conservative
 *          Only the two plane-rain cells assert the C1 floor (the claims they
 *          validate are new); every other cell prints its verdict. Follow-ups
 *          are named in VALIDATION.md ("W4").
 *
 *          Environment knobs (defaults are the committed measurement):
 *            W4_MEMBERS   ensemble / MC size (default 25)
 *            W4_CELLS     comma list of cell names to run (default: all)
 *            W4_CV        rain CV (default 0.20)
 *            W4_THALWEG   lateral slope T of the channel (default 0.02)
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
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
#include "2d/uncertainty/SpatialUncertaintyField.hpp"
#include "2d/uncertainty/SpectralROM.hpp"
#include "uncertainty/SpdeSpatialBasis.hpp"
#include "uncertainty/UncertaintyTypes.hpp"
#include "mc_quantiles.hpp"

using namespace openswmm::twoD;
using openswmm::uncertainty::DistType;
using openswmm::uncertainty::SpdeSpatialBasis;

namespace {

// ─── Configuration (W3's plane, unchanged) ──────────────────────────────────
constexpr double kPert     = 0.20;    // ±20% Manning prior
const int        kModes    = [](){ const char* v = std::getenv("W4_MODES"); return v ? std::atoi(v) : 40; }();   // H13-2D capture sweep
constexpr double kBaseN    = 0.03;
constexpr double kS        = 0.002;   // streamwise bed slope; outlet at x = 0
constexpr int    kNx       = 40, kNy = 40;
constexpr double kDx       = 5.0;     // m  (domain 200 m × 200 m)
constexpr double kRain     = 2.0e-4;  // m/s
constexpr double kRep      = 60.0;
// Window: kReports report intervals after spin-up, scored from kScoreR0.
// W4_REPORTS / W4_SCORE_FROM override them (H13-2D: the 2D cells are scored
// mid-transient; a longer window tests whether widths keep growing).
inline int envIntEarly(const char* k, int d) { const char* v = std::getenv(k); return v ? std::atoi(v) : d; }
const int    kReports  = envIntEarly("W4_REPORTS", 30);
const int    kScoreR0  = envIntEarly("W4_SCORE_FROM", 20);
constexpr double kAlphaPar    = 0.62;
constexpr double kAlphaPerp   = 2.00;
constexpr double kCFactor     = 5.0 / 3.0;
constexpr double kGroundScale = 0.25;

int    envInt(const char* k, int d)    { const char* v = std::getenv(k); return v ? std::atoi(v) : d; }
double envDbl(const char* k, double d) { const char* v = std::getenv(k); return v ? std::atof(v) : d; }

int    gM       = envInt("W4_MEMBERS", 25);
double gCV      = envDbl("W4_CV", 0.20);
double gThalweg = envDbl("W4_THALWEG", 0.02);

bool cellEnabled(const char* name) {
    const char* v = std::getenv("W4_CELLS");
    if (!v || !*v) return true;
    return std::string(",").append(v).append(",").find(std::string(",") + name + ",")
           != std::string::npos;
}

// Surface description: plane (thalweg 0) or V-channel.
struct Surface {
    const char* name;
    double thalweg;      // lateral slope T; z = S·x + T·|y − y_c|
    double spin_s;       // spin-up before the scoring window
};

std::vector<double> comonotoneMultipliers(int M) {
    std::vector<double> m(static_cast<std::size_t>(M));
    for (int i = 0; i < M; ++i)
        m[static_cast<std::size_t>(i)] = (1.0 - kPert) + (i + 0.5) / M * 2.0 * kPert;
    return m;
}

MeshData makeMesh(const Surface& sf) {
    MeshData mesh;
    const int nvx = kNx + 1, nvy = kNy + 1;
    const double yc = 0.5 * kNy * kDx;
    mesh.resize_vertices(nvx * nvy);
    for (int j = 0; j < nvy; ++j)
        for (int i = 0; i < nvx; ++i) {
            const int v = j * nvx + i;
            const double x = i * kDx, y = j * kDx;
            mesh.vx[static_cast<std::size_t>(v)] = x;
            mesh.vy[static_cast<std::size_t>(v)] = y;
            mesh.vz[static_cast<std::size_t>(v)] = kS * x + sf.thalweg * std::abs(y - yc);
        }
    mesh.resize_triangles(2 * kNx * kNy);
    int t = 0;
    for (int j = 0; j < kNy; ++j)
        for (int i = 0; i < kNx; ++i) {
            const int v00 = j * nvx + i,       v10 = j * nvx + i + 1;
            const int v01 = (j + 1) * nvx + i, v11 = (j + 1) * nvx + i + 1;
            mesh.set_triangle(t, v00, v10, v11); ++t;
            mesh.set_triangle(t, v00, v11, v01); ++t;
        }
    for (int i = 0; i < mesh.n_triangles(); ++i)
        mesh.mannings_n[static_cast<std::size_t>(i)] = kBaseN;
    buildMeshTopology(mesh);
    return mesh;
}

// Plane normal-depth profile as the spin-up seed (the channel redistributes
// it during its longer spin).
void seedSteady(const MeshData& mesh, SurfaceStateData& s) {
    s.resize(mesh.n_triangles(), mesh.n_vertices());
    const double L = kNx * kDx;
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double q = kRain * std::max(L - mesh.tri_cx[ui], 0.0);
        const double h = (q > 0.0) ? std::pow(kBaseN * q / std::sqrt(kS), 3.0 / 5.0) : 1e-3;
        s.head[ui]   = mesh.tri_cz[ui] + h;
        s.depth[ui]  = h;
        s.volume[ui] = h * mesh.tri_area[ui];
    }
}

void addOutlet(const MeshData& mesh, BoundaryData& b) {
    b.resize(mesh.n_edge_slots());
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        const auto ui = static_cast<std::size_t>(i);
        for (int e = 0; e < mesh.cell_vertex_count(static_cast<int>(ui)); ++e) {
            const int idx = openswmm::twoD::MeshData::slot(static_cast<int>(i), e);
            if (mesh.cell_neighbour(static_cast<int>(ui), e) >= 0) continue;
            if (mesh.edge_mx[static_cast<std::size_t>(idx)] < 1e-9) {
                b.edge_bc_type[static_cast<std::size_t>(idx)] =
                    static_cast<int8_t>(BoundaryType::NORMAL_FLOW);
                b.edge_bed_slope[static_cast<std::size_t>(idx)] = kS;
            }
        }
    }
}

std::vector<double> groundWeights(const MeshData& mesh) {
    std::vector<double> g(static_cast<std::size_t>(mesh.n_triangles()), 0.0);
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        const auto ui = static_cast<std::size_t>(i);
        for (int e = 0; e < mesh.cell_vertex_count(static_cast<int>(ui)); ++e) {
            const auto idx = static_cast<std::size_t>(openswmm::twoD::MeshData::slot(static_cast<int>(i), e));
            if (mesh.cell_neighbour(static_cast<int>(ui), e) >= 0) continue;
            if (mesh.edge_mx[idx] >= 1e-9) continue;
            const double dx = mesh.edge_mx[idx] - mesh.tri_cx[ui];
            const double dy = mesh.edge_my[idx] - mesh.tri_cy[ui];
            const double d  = 2.0 * std::sqrt(dx * dx + dy * dy);
            if (d < 1e-12) continue;
            g[ui] += kGroundScale * mesh.edge_length[idx] / d;
        }
    }
    return g;
}

/// One marcher run. `n_field` / `rain_field` are per-cell (null = nominal).
std::vector<std::vector<double>> driveMarcher(const Surface& sf,
                                              const double* n_field,
                                              const double* rain_field) {
    auto mesh = makeMesh(sf);
    const int nt = mesh.n_triangles();
    if (n_field)
        for (int i = 0; i < nt; ++i)
            mesh.mannings_n[static_cast<std::size_t>(i)] = n_field[i];
    SolverOptions2D opts;
    opts.cell_closure = CellClosure2D::FLAT;
    SurfaceStateData state;
    seedSteady(mesh, state);
    BoundaryData b;
    addOutlet(mesh, b);
    state.boundary = &b;
    for (int i = 0; i < nt; ++i)
        state.rainfall[static_cast<std::size_t>(i)] = rain_field ? rain_field[i] : kRain;

    ExplicitInertialSolver solver;
    solver.initialize(mesh, state, opts);
    solver.advance(0.0, sf.spin_s);
    std::vector<std::vector<double>> out;
    out.reserve(static_cast<std::size_t>(kReports));
    for (int r = 0; r < kReports; ++r) {
        solver.advance(sf.spin_s + r * kRep, sf.spin_s + (r + 1) * kRep);
        out.emplace_back(state.depth.begin(), state.depth.begin() + nt);
    }
    solver.finalize();
    return out;
}

double classicDiffusivity(const MeshData& mesh, const std::vector<double>& h) {
    double sh = 0.0; int nw = 0;
    for (int i = 0; i < mesh.n_triangles(); ++i)
        if (h[static_cast<std::size_t>(i)] > 1e-3) { sh += h[static_cast<std::size_t>(i)]; ++nw; }
    if (nw == 0) return 0.0;
    return std::pow(sh / nw, 5.0 / 3.0) / (2.0 * kBaseN * std::sqrt(kS));
}

void manningVelocity(const MeshData& mesh, const std::vector<double>& h,
                     std::vector<double>& u, std::vector<double>& v) {
    const int n = mesh.n_triangles();
    u.assign(static_cast<std::size_t>(n), 0.0);
    v.assign(static_cast<std::size_t>(n), 0.0);
    for (int i = 0; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double eta_i = mesh.tri_cz[ui] + h[ui];
                double gx = 0.0, gy = 0.0, w = 0.0;
        for (int e = 0; e < mesh.cell_vertex_count(static_cast<int>(ui)); ++e) {
            const int j = mesh.cell_neighbour(static_cast<int>(ui), e);
            if (j < 0) continue;
            const auto uj = static_cast<std::size_t>(j);
            const double dx = mesh.tri_cx[uj] - mesh.tri_cx[ui];
            const double dy = mesh.tri_cy[uj] - mesh.tri_cy[ui];
            const double d2 = dx * dx + dy * dy;
            if (d2 < 1e-20) continue;
            const double de  = (mesh.tri_cz[uj] + h[uj]) - eta_i;
            const double len = mesh.edge_length[static_cast<std::size_t>(openswmm::twoD::MeshData::slot(static_cast<int>(i), e))];
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

/// What drives the ROM's spread in a cell.
struct RomInputs {
    // H13 (2D): elasticity on the Manning source and a multiplier on the
    // diffusivity scale handed to the operator (sweep knobs).
    double elasticity = [](){ const char* v = std::getenv("W4_ELASTICITY"); return v ? std::atof(v) : 1.0; }();
    double d_mult = 1.0;
    // H16: reconstruct depth deviations as a local fraction of h_det instead
    // of an absolute delta (see SpectralROM::relative_deviation). The
    // production default since 2026-10-11; W4_RELATIVE=0 reproduces the
    // absolute form. Never applied on the ungrounded (legacy/Neumann) rung.
    bool relative = [](){ const char* v = std::getenv("W4_RELATIVE"); return !v || std::atoi(v) != 0; }();
    // Comonotone Manning multipliers (length M) or empty for "ones".
    std::vector<double> mann_mult;
    // Correlated Manning multiplier field (M × n). Without `per_member` the
    // ROM takes the spatial (diagonal) path — the pre-H15 behaviour, kept as
    // the record; with it, one reduced operator is assembled per member
    // (cond_mult = 1/W_i) and installed via setReducedOperatorPerMember (H15).
    const SpatialUncertaintyField* mann_field = nullptr;
    bool per_member = false;
    // Correlated rain: loc/spread planes + reduced basis OR materialized field.
    const double* soft_loc = nullptr;
    const double* soft_spread = nullptr;
    const double* psi = nullptr;      // K_s × n (reduced path)
    const double* a_coef = nullptr;   // M × K_s
    int ks = 0;
    const SpatialUncertaintyField* soft_field = nullptr;   // materialized path
};

struct RomBands {
    std::vector<std::vector<double>> q05, q95;
    int ks_used = 0;
    bool reduced_path = false;
    bool ok = false;
};

/// Build the correlated rain realization shared by MC and ROM. Returns the
/// materialized W (M × n), and fills psi/a for the reduced path when K_s < M.
struct CorrRain {
    SpdeSpatialBasis basis;
    std::vector<double> a, psi, W;        // W: M × n materialized
    std::vector<double> loc, spread;      // per-cell planes
    SpatialUncertaintyField field;        // materialized (K_s ≥ M)
    bool reduced = false;
};

RomBands runRomRung(Rung rung, const MeshData& mesh0, const MeshEigenBasis& basis,
                    const std::vector<double>& ground_w, const RomInputs& in,
                    const std::vector<std::vector<double>>& hdet,
                    const std::vector<double>& h0, CorrRain* corr /* may be null */) {
    const int M = gM;
    SpectralROM rom;
    rom.basis         = &basis;
    rom.n_ensemble    = M;
    rom.mannings_pert = in.mann_mult.empty() ? 0.0 : kPert;
    rom.rainfall_pert = 0.0;
    std::vector<double> ones(static_cast<std::size_t>(M), 1.0);
    rom.setExternalSamples(in.mann_mult.empty() ? ones : in.mann_mult, ones);
    rom.initialize();
    rom.seed(h0.data());
    rom.manning_elasticity = in.elasticity;
    rom.relative_deviation = in.relative && rung != Rung::LEGACY;
    if (in.mann_field) rom.spatial_mannings = *in.mann_field;

    RomBands out;
    if (corr) {
        // Same recipe as SurfaceRouter2D::buildGridSoftField: the ROM's own
        // family coefficients seed mode 0 of the SPDE basis; K_s < M → reduced.
        rom.setSoftForcing(corr->loc.data(), corr->spread.data(), DistType::NORMAL);
        const int Ks = corr->basis.n_modes();
        out.ks_used = Ks;
        if (Ks < M) {
            rom.setSoftForcingReduced(corr->loc.data(), corr->spread.data(), DistType::NORMAL,
                                      corr->psi.data(), corr->a.data(), Ks);
            out.reduced_path = true;
        } else {
            rom.setSoftForcing(corr->loc.data(), corr->spread.data(), DistType::NORMAL,
                               &corr->field);
        }
    }

    const double K_legacy = 2.0 * classicDiffusivity(mesh0, h0);
    DeviationOperator2D op;
    std::vector<double> vel_u, vel_v;
    out.q05.resize(static_cast<std::size_t>(kReports));
    out.q95.resize(static_cast<std::size_t>(kReports));

    for (int r = 0; r < kReports; ++r) {
        const std::vector<double>& h_prev = (r == 0) ? h0 : hdet[static_cast<std::size_t>(r - 1)];
        if (rung != Rung::LEGACY) {
            switch (rung) {
                case Rung::ISO:       op.alpha_par = op.alpha_perp = 1.0; op.c_factor = 0.0; break;
                case Rung::ANISO:     op.alpha_par = kAlphaPar; op.alpha_perp = kAlphaPerp; op.c_factor = 0.0; break;
                case Rung::ANISO_ADV: op.alpha_par = kAlphaPar; op.alpha_perp = kAlphaPerp; op.c_factor = kCFactor; break;
                default: break;
            }
            manningVelocity(mesh0, h_prev, vel_u, vel_v);
            const double D = classicDiffusivity(mesh0, h_prev) * in.d_mult;
            // Relative mode needs the similarity-transformed operator (kept
            // for the per-member assemblies below, which reuse `op`).
            // Two-channel relative mode: plain operator for the additive
            // channel first, then the depth-weighted one for Manning.
            op.similarity_h = nullptr;
            if (rom.relative_deviation) {
                if (!op.assemble(mesh0, basis, D, h_prev.data(), vel_u.data(), vel_v.data(),
                                 ground_w.data()))
                    return out;
                rom.setAdditiveOperator(op.M);
            }
            op.similarity_h = rom.relative_deviation ? h_prev.data() : nullptr;
            if (!op.assemble(mesh0, basis, D, h_prev.data(), vel_u.data(), vel_v.data(),
                             ground_w.data()))
                return out;
            rom.setReducedOperator(op.M);
            rom.setRelativeGram(op.similarity_h ? op.Ginv : std::vector<double>{},
                                op.similarity_h ? op.GramWeight : std::vector<double>{});
            if (in.mann_field && in.per_member) {
                const int nt = mesh0.n_triangles();
                const auto kk = static_cast<std::size_t>(op.k) * static_cast<std::size_t>(op.k);
                std::vector<double> M_all(static_cast<std::size_t>(M) * kk), cm(static_cast<std::size_t>(nt));
                for (int i = 0; i < M; ++i) {
                    for (int t = 0; t < nt; ++t) cm[static_cast<std::size_t>(t)] = 1.0 / in.mann_field->at(i, t);
                    if (!op.assemble(mesh0, basis, D, h_prev.data(), vel_u.data(), vel_v.data(),
                                     ground_w.data(), cm.data()))
                        return out;
                    std::copy(op.M.begin(), op.M.end(),
                              M_all.begin() + static_cast<std::ptrdiff_t>(i) * static_cast<std::ptrdiff_t>(kk));
                }
                rom.setReducedOperatorPerMember(M_all);
            }
        }
        rom.advance(kRep, K_legacy, nullptr, nullptr, hdet[static_cast<std::size_t>(r)].data());
        rom.computeQuantiles(hdet[static_cast<std::size_t>(r)].data());
        out.q05[static_cast<std::size_t>(r)] = rom.q05;
        out.q95[static_cast<std::size_t>(r)] = rom.q95;
    }
    out.ok = true;
    return out;
}

// ─── Scoring (W3's, verbatim semantics) ─────────────────────────────────────

struct DepthBin {
    double lo = 0.0, hi = 0.0;
    int n = 0;
    double cov_sum = 0.0;
    std::vector<double> ratios;
    // W4b: the MC's and the ROM's own depth-to-roughness elasticity per bin,
    // (q95−q05 of depth)/(median depth) divided by (q95−q05 of mm)/1.
    std::vector<double> e_mc, e_rom;
};

struct Score {
    int samples = 0;
    double median_cont = 0.0, ratio_med = 0.0, w_frac = 0.0, member_cov = 0.0, ratio_med_mid = 0.0;
    double wet_frac = 0.0;   // fraction of (cell,time) samples with MC median ≥ 1e-4 m
    // W4b: the same statistics binned by the MC median depth (m), so a
    // converging surface's thalweg and its thin-film flanks are not pooled.
    std::vector<DepthBin> bins;
};

Score scoreBands(const RomBands& bands, const MeshData& mesh0,
                 const std::vector<std::vector<std::vector<double>>>& mc) {
    const int M  = gM;
    const int nt = mesh0.n_triangles();
    const int lo = static_cast<int>(std::round(0.05 * (M - 1)));
    const int md = static_cast<int>(std::round(0.50 * (M - 1)));
    const int hi = static_cast<int>(std::round(0.95 * (M - 1)));
    int n_tot = 0, n_cov = 0, n_w = 0, n_w_ok = 0, n_all = 0;
    double member_cov_sum = 0.0;
    std::vector<double> ratios, ratios_mid, hc(static_cast<std::size_t>(M));
    std::vector<DepthBin> bins = {{1e-4, 1e-3}, {1e-3, 3e-3}, {3e-3, 1e-2}, {1e-2, 2e-2}, {2e-2, 5e-2},
                                  {5e-2, 1e-1}, {1e-1, 2e-1}, {2e-1, 1e9}};
    // Relative Manning spread of the comonotone design at the same-index
    // quantile rule: mm[hi] − mm[lo] (mm is 0.8 + 0.4·(i+0.5)/M, ascending).
    const double mm_lo = (1.0 - kPert) + (lo + 0.5) / M * 2.0 * kPert;
    const double mm_hi = (1.0 - kPert) + (hi + 0.5) / M * 2.0 * kPert;
    const double dmm_rel = mm_hi - mm_lo;
    for (int r = kScoreR0; r < kReports; ++r) {
        const auto ur = static_cast<std::size_t>(r);
        for (int c = 0; c < nt; ++c) {
            const auto uc = static_cast<std::size_t>(c);
            ++n_all;
            for (int i = 0; i < M; ++i)
                hc[static_cast<std::size_t>(i)] = mc[static_cast<std::size_t>(i)][ur][uc];
            std::sort(hc.begin(), hc.end());
            const double mc_med = hc[static_cast<std::size_t>(md)];
            const double mc_w   = hc[static_cast<std::size_t>(hi)] - hc[static_cast<std::size_t>(lo)];
            if (mc_med < 1e-4) continue;
            ++n_tot;
            const double q05 = bands.q05[ur][uc], q95 = bands.q95[ur][uc];
            if (q05 <= mc_med && mc_med <= q95) ++n_cov;
            const double icov = mcq::intervalCoverage(hc, q05, q95);
            member_cov_sum += icov;
            const double w_mid = mcq::quantileMidpoint(hc, 0.95) - mcq::quantileMidpoint(hc, 0.05);
            if (w_mid > 1e-6) ratios_mid.push_back((q95 - q05) / w_mid);
            if (mc_w > 1e-6) {
                const double ratio = (q95 - q05) / mc_w;
                ++n_w; ratios.push_back(ratio);
                if (ratio >= 0.3 && ratio <= 3.0) ++n_w_ok;
                for (auto& b : bins)
                    if (mc_med >= b.lo && mc_med < b.hi) {
                        ++b.n; b.cov_sum += icov; b.ratios.push_back(ratio);
                        b.e_mc.push_back((mc_w / mc_med) / dmm_rel);
                        b.e_rom.push_back(((q95 - q05) / mc_med) / dmm_rel);
                        break;
                    }
            }
        }
    }
    Score s;
    s.samples = n_tot;
    s.wet_frac = n_all ? static_cast<double>(n_tot) / n_all : 0.0;
    s.median_cont = n_tot ? static_cast<double>(n_cov) / n_tot : 0.0;
    s.member_cov  = n_tot ? member_cov_sum / n_tot : 0.0;
    s.w_frac = n_w ? static_cast<double>(n_w_ok) / n_w : 0.0;
    if (!ratios.empty())     { std::sort(ratios.begin(), ratios.end());         s.ratio_med     = ratios[ratios.size() / 2]; }
    if (!ratios_mid.empty()) { std::sort(ratios_mid.begin(), ratios_mid.end()); s.ratio_med_mid = ratios_mid[ratios_mid.size() / 2]; }
    for (auto& b : bins) {
        std::sort(b.ratios.begin(), b.ratios.end());
        std::sort(b.e_mc.begin(), b.e_mc.end());
        std::sort(b.e_rom.begin(), b.e_rom.end());
    }
    s.bins = std::move(bins);
    return s;
}

void printDepthBins(const char* cell, const char* rung, const Score& s) {
    if (!std::getenv("W4_BINS")) return;
    for (const auto& b : s.bins) {
        if (b.n == 0) continue;
        std::printf("[W4 depth-bin] %-22s %s MC depth [%.0e,%.0e) m: n=%6d  cov=%.3f  width-med=%.3f  "
                    "elasticity MC=%.3f ROM=%.3f\n",
                    cell, rung, b.lo, b.hi, b.n, b.cov_sum / b.n,
                    b.ratios[b.ratios.size() / 2],
                    b.e_mc[b.e_mc.size() / 2], b.e_rom[b.e_rom.size() / 2]);
    }
}

void printScore(const char* cell, const char* rung, const Score& s, const RomBands& b);
void printScoreImpl(const char* cell, const char* rung, const Score& s, const RomBands& b) {
    std::printf("[W4 2D-ROM-vs-marcher] %-28s %s median-containment=%.3f member-coverage=%.3f "
                "width-med=%.3f (midpoint %.3f) in[0.3,3]=%.3f wet=%.2f n=%d%s\n",
                cell, rung, s.median_cont, s.member_cov, s.ratio_med, s.ratio_med_mid,
                s.w_frac, s.wet_frac, s.samples,
                b.ks_used ? (b.reduced_path ? "  [CORR_LEN reduced]" : "  [CORR_LEN materialized]") : "");
}
void printScore(const char* cell, const char* rung, const Score& s, const RomBands& b) {
    printScoreImpl(cell, rung, s, b);
    printDepthBins(cell, rung, s);
}

// ─── Correlated field construction ──────────────────────────────────────────

/// Correlated rain realization at corr_len; W materialized for the MC, and
/// psi/a for the ROM when K_s < M. The ROM's own c_i (probit of its shuffled
/// strata) seed mode 0, so member i is the same draw on both sides.
void buildCorrRain(const MeshData& mesh, double corr_len, CorrRain& cr,
                   double& mult_min, double& mult_max) {
    const int M = gM, nt = mesh.n_triangles();
    cr.loc.assign(static_cast<std::size_t>(nt), kRain);
    cr.spread.assign(static_cast<std::size_t>(nt), gCV * kRain);
    cr.basis.build(mesh.tri_cx.data(), mesh.tri_cy.data(), nt, corr_len);
    // Coefficients: identical to what the ROM computes in setSoftForcing for
    // NORMAL -- probit(u_i) over shuffledStrata(M, sample_seed+4). Rather than
    // re-derive them here, read them off a ROM instance after setSoftForcing.
    std::vector<double> coeff;
    {
        SpectralROM r2;
        MeshEigenBasis b2;
        if (!b2.build(mesh, 2)) throw std::runtime_error("probe basis build failed");
        r2.basis = &b2; r2.n_ensemble = M; r2.mannings_pert = 0.0; r2.rainfall_pert = 0.0;
        r2.initialize();
        r2.setSoftForcing(cr.loc.data(), cr.spread.data(), DistType::NORMAL);
        coeff = r2.softCoeff();
    }
    cr.basis.sampleCoefficients(coeff, DistType::NORMAL, UINT64_C(0x2d50f7c0de5eed02), cr.a);
    cr.basis.materializeField(cr.a, M, cr.W);
    const int Ks = cr.basis.n_modes();
    if (Ks < M) {
        cr.basis.normalizedModes(cr.a, M, cr.psi);
        cr.reduced = true;
    } else {
        cr.field.values = cr.W;
        cr.field.n_members = M;
        cr.field.n_cells = nt;
        cr.reduced = false;
    }
    mult_min = 1e300; mult_max = -1e300;
    for (double w : cr.W) { mult_min = std::min(mult_min, 1.0 + gCV * w); mult_max = std::max(mult_max, 1.0 + gCV * w); }
}

/// Correlated Manning multiplier field 1 + p·V_i(t), UNIFORM family, mode 0 =
/// the comonotone strata (so corr_len → ∞ recovers W3's multipliers exactly).
void buildCorrManning(const MeshData& mesh, double corr_len, SpatialUncertaintyField& f,
                      double& mult_min, double& mult_max, int& ks) {
    const int M = gM, nt = mesh.n_triangles();
    std::vector<double> c(static_cast<std::size_t>(M));
    for (int i = 0; i < M; ++i) c[static_cast<std::size_t>(i)] = 2.0 * (i + 0.5) / M - 1.0;
    SpdeSpatialBasis basis;
    basis.build(mesh.tri_cx.data(), mesh.tri_cy.data(), nt, corr_len);
    std::vector<double> a, V;
    basis.sampleCoefficients(c, DistType::UNIFORM, UINT64_C(0x5eedbead5eedbead), a);
    basis.materializeField(a, M, V);
    ks = basis.n_modes();
    f.n_members = M; f.n_cells = nt;
    f.values.resize(V.size());
    mult_min = 1e300; mult_max = -1e300;
    for (std::size_t k = 0; k < V.size(); ++k) {
        f.values[k] = 1.0 + kPert * V[k];
        mult_min = std::min(mult_min, f.values[k]);
        mult_max = std::max(mult_max, f.values[k]);
    }
}

using MC = std::vector<std::vector<std::vector<double>>>;

MC runMC(const Surface& sf, const std::vector<std::vector<double>>& n_fields,
         const std::vector<std::vector<double>>& rain_fields) {
    MC mc(static_cast<std::size_t>(gM));
    for (int i = 0; i < gM; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        mc[ui] = driveMarcher(sf,
                              n_fields.empty() ? nullptr : n_fields[ui].data(),
                              rain_fields.empty() ? nullptr : rain_fields[ui].data());
    }
    return mc;
}

struct Fixture {
    Surface sf;
    MeshData mesh;
    std::vector<double> gw, h0;
    MeshEigenBasis grounded, neumann;
    std::vector<std::vector<double>> hdet;
    double steadiness = 0.0;   // max |h(end) − h(start)| of the nominal window
};

Fixture makeFixture(const Surface& sf) {
    Fixture f;
    f.sf = sf;
    f.mesh = makeMesh(sf);
    f.gw = groundWeights(f.mesh);
    if (!f.grounded.build(f.mesh, kModes, f.gw.data())) throw std::runtime_error("grounded basis");
    if (!f.neumann.build(f.mesh, kModes)) throw std::runtime_error("neumann basis");
    SurfaceStateData s0;
    seedSteady(f.mesh, s0);
    f.h0.assign(s0.depth.begin(), s0.depth.begin() + f.mesh.n_triangles());
    f.hdet = driveMarcher(sf, nullptr, nullptr);
    for (std::size_t t = 0; t < f.hdet.front().size(); ++t)
        f.steadiness = std::max(f.steadiness, std::abs(f.hdet.back()[t] - f.hdet.front()[t]));
    return f;
}

} // namespace

// ════════════════════════════════════════════════════════════════════════════

TEST(Rom2dMarcherCoverageCorr, CorrelatedFieldsAndSecondSurface) {
    const Surface plane  {"plane",   0.0,       3000.0};
    const Surface channel{"channel", gThalweg,  6000.0};
    const double corr_lens[2] = {20.0, 100.0};

    Fixture fp = makeFixture(plane);
    std::printf("[W4 fixture] plane   : nominal window drift max|dh|=%.2e m\n", fp.steadiness);

    // ── Plane, correlated RAIN at two correlation lengths (adv rung) ────────
    for (double L : corr_lens) {
        char name[64];
        std::snprintf(name, sizeof name, "plane-rain-corr%d", static_cast<int>(L));
        if (!cellEnabled(name)) continue;
        CorrRain cr;
        double mn, mx;
        buildCorrRain(fp.mesh, L, cr, mn, mx);
        std::printf("[W4 field] %s: K_s=%d captured=%.3f rain multiplier range [%.3f, %.3f]\n",
                    name, cr.basis.n_modes(), cr.basis.capturedVarianceFraction(), mn, mx);
        ASSERT_GT(mn, 0.0) << "correlated rain multiplier went negative; lower W4_CV";
        const int nt = fp.mesh.n_triangles();
        std::vector<std::vector<double>> rain(static_cast<std::size_t>(gM),
                                              std::vector<double>(static_cast<std::size_t>(nt)));
        for (int i = 0; i < gM; ++i)
            for (int t = 0; t < nt; ++t)
                rain[static_cast<std::size_t>(i)][static_cast<std::size_t>(t)] =
                    kRain * (1.0 + gCV * cr.W[static_cast<std::size_t>(i) * nt + t]);
        const MC mc = runMC(plane, {}, rain);
        RomInputs in;   // no Manning spread
        RomBands b = runRomRung(Rung::ANISO_ADV, fp.mesh, fp.grounded, fp.gw, in, fp.hdet, fp.h0, &cr);
        ASSERT_TRUE(b.ok);
        const Score s = scoreBands(b, fp.mesh, mc);
        printScore(name, rungName(Rung::ANISO_ADV), s, b);
        // Measured 2026-10-07 (M=25): corr20 0.897/1.313 (materialized branch),
        // corr100 0.886/1.419 (reduced branch) -- both CALIBRATED. These are the
        // first MC numbers behind the 2D CORR_LEN path, so per C1 practice the
        // floor is asserted here (margins 0.10 / 0.09); the width ceiling is a
        // label, not a gate.
        EXPECT_TRUE(mcq::reportCalibration(name, s.member_cov, s.ratio_med, "W4 follow-up"))
            << name << ": correlated-rain cell is a validated claim; member coverage must stay >= 0.80";
        EXPECT_GT(s.samples, 0);
    }

    // ── Plane, correlated MANNING at two correlation lengths ────────────────
    //    The ROM is handed the adv rung but, with spatial_mannings set, advances
    //    on the diagonal path (header). Record both bases.
    for (double L : corr_lens) {
        char name[64];
        std::snprintf(name, sizeof name, "plane-mann-corr%d", static_cast<int>(L));
        if (!cellEnabled(name)) continue;
        SpatialUncertaintyField f;
        double mn, mx; int ks;
        buildCorrManning(fp.mesh, L, f, mn, mx, ks);
        std::printf("[W4 field] %s: K_s=%d Manning multiplier range [%.3f, %.3f]\n", name, ks, mn, mx);
        ASSERT_GT(mn, 0.0);
        const int nt = fp.mesh.n_triangles();
        std::vector<std::vector<double>> nf(static_cast<std::size_t>(gM),
                                            std::vector<double>(static_cast<std::size_t>(nt)));
        for (int i = 0; i < gM; ++i)
            for (int t = 0; t < nt; ++t)
                nf[static_cast<std::size_t>(i)][static_cast<std::size_t>(t)] = kBaseN * f.at(i, t);
        const MC mc = runMC(plane, nf, {});
        RomInputs in;
        in.mann_field = &f;
        // H15 production path: per-member reduced operators on the adv rung.
        in.per_member = true;
        RomBands bp = runRomRung(Rung::ANISO_ADV, fp.mesh, fp.grounded, fp.gw, in, fp.hdet, fp.h0, nullptr);
        ASSERT_TRUE(bp.ok);
        const Score sp = scoreBands(bp, fp.mesh, mc);
        printScore(name, "adv/per-member", sp, bp);
        mcq::reportCalibration((std::string(name) + " (adv, per-member M_i)").c_str(),
                               sp.member_cov, sp.ratio_med, "H15 follow-up");
        // Pre-H15 record: the same field on the diagonal fallback.
        in.per_member = false;
        RomBands bg = runRomRung(Rung::ANISO_ADV, fp.mesh, fp.grounded, fp.gw, in, fp.hdet, fp.h0, nullptr);
        ASSERT_TRUE(bg.ok);
        const Score sg = scoreBands(bg, fp.mesh, mc);
        printScore(name, "diag/grounded ", sg, bg);
        mcq::reportCalibration((std::string(name) + " (diag, grounded basis -- pre-H15)").c_str(),
                               sg.member_cov, sg.ratio_med, "reference, ungated");
        RomBands bn = runRomRung(Rung::LEGACY, fp.mesh, fp.neumann, fp.gw, in, fp.hdet, fp.h0, nullptr);
        ASSERT_TRUE(bn.ok);
        const Score sn = scoreBands(bn, fp.mesh, mc);
        printScore(name, "diag/neumann ", sn, bn);
        mcq::reportCalibration((std::string(name) + " (diag, Neumann basis)").c_str(),
                               sn.member_cov, sn.ratio_med, "reference, ungated");
        EXPECT_GT(sg.samples, 0);
    }

    // ── Channel (thalweg), comonotone Manning, all four rungs ───────────────
    // ── Plane, comonotone Manning, all four rungs (W3's cell, re-measurable
    //    here with the window knobs) ────────────────────────────────────────
    if (cellEnabled("plane-mann-comono")) {
        const auto mult = comonotoneMultipliers(gM);
        std::vector<std::vector<double>> nf(static_cast<std::size_t>(gM),
            std::vector<double>(static_cast<std::size_t>(fp.mesh.n_triangles())));
        for (int i = 0; i < gM; ++i)
            std::fill(nf[static_cast<std::size_t>(i)].begin(), nf[static_cast<std::size_t>(i)].end(),
                      kBaseN * mult[static_cast<std::size_t>(i)]);
        const MC mc = runMC(plane, nf, {});
        // W4_SWEEP="e:d,e:d,..." runs the adv rung for each (elasticity,
        // diffusivity multiplier) pair against the same MC (H13-2D sweep);
        // unset = the four rungs at (1, 1).
        if (const char* sw = std::getenv("W4_SWEEP")) {
            std::string spec(sw);
            std::size_t pos = 0;
            while (pos < spec.size()) {
                std::size_t nxt = spec.find(',', pos);
                if (nxt == std::string::npos) nxt = spec.size();
                const std::string tok = spec.substr(pos, nxt - pos);
                const std::size_t colon = tok.find(':');
                RomInputs in;
                in.mann_mult = mult;
                in.elasticity = std::atof(tok.substr(0, colon).c_str());
                in.d_mult = (colon == std::string::npos) ? 1.0 : std::atof(tok.substr(colon + 1).c_str());
                RomBands b = runRomRung(Rung::ANISO_ADV, fp.mesh, fp.grounded, fp.gw, in, fp.hdet, fp.h0, nullptr);
                ASSERT_TRUE(b.ok);
                const Score s = scoreBands(b, fp.mesh, mc);
                char name[80];
                std::snprintf(name, sizeof name, "plane e=%.2f D×%.1f", in.elasticity, in.d_mult);
                printScore(name, rungName(Rung::ANISO_ADV), s, b);
                pos = nxt + 1;
            }
        } else {
            RomInputs in;
            in.mann_mult = mult;
            const Rung rungs[] = {Rung::LEGACY, Rung::ISO, Rung::ANISO, Rung::ANISO_ADV};
            for (Rung rg : rungs) {
                const MeshEigenBasis& basis = (rg == Rung::LEGACY) ? fp.neumann : fp.grounded;
                RomBands b = runRomRung(rg, fp.mesh, basis, fp.gw, in, fp.hdet, fp.h0, nullptr);
                ASSERT_TRUE(b.ok) << rungName(rg);
                const Score s = scoreBands(b, fp.mesh, mc);
                printScore("plane-mann-comono", rungName(rg), s, b);
                mcq::reportCalibration((std::string("plane-mann-comono ") + rungName(rg)).c_str(),
                                       s.member_cov, s.ratio_med, "reference (W3 gate is the asserted copy)");
                EXPECT_GT(s.samples, 0);
            }
        }
    }

    if (cellEnabled("channel-mann-comono") || cellEnabled("channel-rain-corr20") || cellEnabled("channel-mann-dw")) {
        Fixture fc = makeFixture(channel);
        std::printf("[W4 fixture] channel : thalweg T=%.3f, nominal window drift max|dh|=%.2e m, "
                    "max depth %.3f m\n", gThalweg, fc.steadiness,
                    *std::max_element(fc.hdet.back().begin(), fc.hdet.back().end()));

        if (cellEnabled("channel-mann-comono")) {
            const auto mult = comonotoneMultipliers(gM);
            std::vector<std::vector<double>> nf(static_cast<std::size_t>(gM),
                std::vector<double>(static_cast<std::size_t>(fc.mesh.n_triangles())));
            for (int i = 0; i < gM; ++i)
                std::fill(nf[static_cast<std::size_t>(i)].begin(), nf[static_cast<std::size_t>(i)].end(),
                          kBaseN * mult[static_cast<std::size_t>(i)]);
            const MC mc = runMC(channel, nf, {});
            RomInputs in;
            in.mann_mult = mult;
            const Rung rungs[] = {Rung::LEGACY, Rung::ISO, Rung::ANISO, Rung::ANISO_ADV};
            for (Rung rg : rungs) {
                const MeshEigenBasis& basis = (rg == Rung::LEGACY) ? fc.neumann : fc.grounded;
                RomBands b = runRomRung(rg, fc.mesh, basis, fc.gw, in, fc.hdet, fc.h0, nullptr);
                ASSERT_TRUE(b.ok) << rungName(rg);
                const Score s = scoreBands(b, fc.mesh, mc);
                printScore("channel-mann-comono", rungName(rg), s, b);
                mcq::reportCalibration((std::string("channel-mann-comono ") + rungName(rg)).c_str(),
                                       s.member_cov, s.ratio_med,
                                       rg == Rung::ANISO_ADV ? "W4 follow-up" : "reference rung, ungated");
                EXPECT_GT(s.samples, 0);
            }
        }

        // H16 probe: does depth-weighting the BASIS (not just the operator,
        // which already gets h_cell at assembly) reduce thin-cell leakage?
        // buildDepthWeighted was deliberately kept off the production rung
        // (router comment: "would silently leave the calibrated
        // configuration") -- before anyone had diagnosed WHY depth contrast
        // mattered. Cheap to test since the machinery already exists.
        if (cellEnabled("channel-mann-dw")) {
            const auto mult = comonotoneMultipliers(gM);
            std::vector<std::vector<double>> nf(static_cast<std::size_t>(gM),
                std::vector<double>(static_cast<std::size_t>(fc.mesh.n_triangles())));
            for (int i = 0; i < gM; ++i)
                std::fill(nf[static_cast<std::size_t>(i)].begin(), nf[static_cast<std::size_t>(i)].end(),
                          kBaseN * mult[static_cast<std::size_t>(i)]);
            const MC mc = runMC(channel, nf, {});
            MeshEigenBasis dw;
            double h_sum = 0.0;
            const int nt = fc.mesh.n_triangles();
            for (int t = 0; t < nt; ++t) h_sum += std::max(fc.hdet.front()[static_cast<std::size_t>(t)], 0.0);
            const double h_mean = h_sum / nt;
            std::vector<double> d_cell(static_cast<std::size_t>(nt), 1.0);
            if (h_mean > 1e-9) {
                const double inv53 = 1.0 / std::pow(h_mean, 5.0 / 3.0);
                for (int t = 0; t < nt; ++t)
                    d_cell[static_cast<std::size_t>(t)] =
                        std::pow(std::max(fc.hdet.front()[static_cast<std::size_t>(t)], 0.0), 5.0 / 3.0) * inv53;
            }
            const bool dw_ok = dw.buildDepthWeighted(fc.mesh, kModes, d_cell.data(), fc.gw.data());
            std::printf("[H16 probe] channel depth-weighted basis build: %s (depth_weighted=%d)\n",
                        dw_ok ? "ok" : "FAILED (fell back to geometric)", dw.depth_weighted ? 1 : 0);
            RomInputs in;
            in.mann_mult = mult;
            RomBands b = runRomRung(Rung::ANISO_ADV, fc.mesh, dw_ok ? dw : fc.grounded, fc.gw, in, fc.hdet, fc.h0, nullptr);
            ASSERT_TRUE(b.ok);
            const Score s = scoreBands(b, fc.mesh, mc);
            printScore("channel-mann-dw", "adv/dw-basis", s, b);
            mcq::reportCalibration("channel-mann-dw (depth-weighted basis)", s.member_cov, s.ratio_med, "H16 probe");
            EXPECT_GT(s.samples, 0);
        }

        if (cellEnabled("channel-rain-corr20")) {
            CorrRain cr;
            double mn, mx;
            buildCorrRain(fc.mesh, 20.0, cr, mn, mx);
            std::printf("[W4 field] channel-rain-corr20: K_s=%d captured=%.3f range [%.3f, %.3f]\n",
                        cr.basis.n_modes(), cr.basis.capturedVarianceFraction(), mn, mx);
            ASSERT_GT(mn, 0.0);
            const int nt = fc.mesh.n_triangles();
            std::vector<std::vector<double>> rain(static_cast<std::size_t>(gM),
                                                  std::vector<double>(static_cast<std::size_t>(nt)));
            for (int i = 0; i < gM; ++i)
                for (int t = 0; t < nt; ++t)
                    rain[static_cast<std::size_t>(i)][static_cast<std::size_t>(t)] =
                        kRain * (1.0 + gCV * cr.W[static_cast<std::size_t>(i) * nt + t]);
            const MC mc = runMC(channel, {}, rain);
            RomInputs in;
            RomBands b = runRomRung(Rung::ANISO_ADV, fc.mesh, fc.grounded, fc.gw, in, fc.hdet, fc.h0, &cr);
            ASSERT_TRUE(b.ok);
            const Score s = scoreBands(b, fc.mesh, mc);
            printScore("channel-rain-corr20", rungName(Rung::ANISO_ADV), s, b);
            mcq::reportCalibration("channel-rain-corr20", s.member_cov, s.ratio_med, "W4 follow-up");
            EXPECT_GT(s.samples, 0);
        }
    }
}
