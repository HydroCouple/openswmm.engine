/**
 * @file test_soft_forcing_rom.cpp
 * @brief Focused SR-3a tests for the soft forcing API on both ROMs.
 *
 * @ingroup unit_tests
 */

#include <gtest/gtest.h>

#define private public
#include "uncertainty/GraphEigenBasis.hpp"
#include "uncertainty/NetworkLaplacian1D.hpp"
#include "uncertainty/SpectralROM1D.hpp"
#include "uncertainty/LhsShuffle.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/uncertainty/MeshEigenBasis.hpp"
#include "2d/uncertainty/SpectralROM.hpp"
#undef private

#include <algorithm>
#include <cmath>
#include <vector>

using namespace openswmm::uncertainty;
using namespace openswmm::twoD;

namespace {

static CsrGraph make_chain_laplacian(int n) {
    int nc = n - 1;
    std::vector<int> n1(static_cast<std::size_t>(nc));
    std::vector<int> n2(static_cast<std::size_t>(nc));
    for (int k = 0; k < nc; ++k) {
        n1[static_cast<std::size_t>(k)] = k;
        n2[static_cast<std::size_t>(k)] = k + 1;
    }
    std::vector<int> is_outfall(static_cast<std::size_t>(n), 0);
    std::vector<int> active_map, full_to_active;
    return NetworkLaplacian1D::buildUniform(n, nc, n1.data(), n2.data(),
                                            is_outfall.data(),
                                            active_map, full_to_active);
}

static MeshData makeStructuredMesh(int N = 3, double domain_m = 3.0) {
    MeshData mesh;
    int nv = (N + 1) * (N + 1);
    int nt = 2 * N * N;
    mesh.resize_vertices(nv);
    mesh.resize_triangles(nt);

    double dx = domain_m / N;
    double dy = domain_m / N;

    for (int i = 0; i <= N; ++i)
        for (int j = 0; j <= N; ++j) {
            int vi = i * (N + 1) + j;
            mesh.vx[static_cast<std::size_t>(vi)] = j * dx;
            mesh.vy[static_cast<std::size_t>(vi)] = i * dy;
            mesh.vz[static_cast<std::size_t>(vi)] = 0.0;
        }

    int t = 0;
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) {
            int v00 = i * (N + 1) + j,    v01 = i * (N + 1) + j + 1;
            int v10 = (i+1) * (N + 1) + j, v11 = (i+1) * (N + 1) + j + 1;
            mesh.tri_v0[t] = v00; mesh.tri_v1[t] = v01; mesh.tri_v2[t] = v11; ++t;
            mesh.tri_v0[t] = v00; mesh.tri_v1[t] = v11; mesh.tri_v2[t] = v10; ++t;
        }

    buildMeshTopology(mesh);
    return mesh;
}

TEST(SoftForcingROM1D, ProjectsLocAndSpreadAndActivatesModes) {
    CsrGraph L = make_chain_laplacian(8);
    GraphEigenBasis basis;
    ASSERT_TRUE(basis.build(L, 4));

    SpectralROM1D rom;
    rom.basis = &basis;
    rom.n_ensemble = 11;
    rom.mannings_pert = 0.0;
    rom.runoff_pert = 0.0;
    rom.initialize();

    std::vector<double> h_det(static_cast<std::size_t>(basis.n_nodes), 0.0);
    std::vector<double> loc(static_cast<std::size_t>(basis.n_nodes));
    std::vector<double> spread(static_cast<std::size_t>(basis.n_nodes));
    const double* mode0 = &basis.P[0];
    for (int i = 0; i < basis.n_nodes; ++i) {
        loc[static_cast<std::size_t>(i)] = mode0[i];
        spread[static_cast<std::size_t>(i)] = 0.5 * mode0[i];
    }

    rom.seed(h_det.data());
    rom.setSoftForcing(loc.data(), spread.data());
    rom.advance(1.0, 0.0, h_det.data(), nullptr, h_det.data());

    EXPECT_NEAR(rom.r_coarse[0], 1.0, 1.0e-12);
    EXPECT_NEAR(rom.soft_r_spread_[0], 0.5, 1.0e-12);
    EXPECT_GT(rom.n_modes_active, 0);
}

TEST(SoftForcingROM1D, ZeroSpreadEquivalentToNoSoftForcing) {
    CsrGraph L = make_chain_laplacian(8);
    GraphEigenBasis basis;
    ASSERT_TRUE(basis.build(L, 4));

    SpectralROM1D a;
    a.basis = &basis;
    a.n_ensemble = 11;
    a.mannings_pert = 0.0;
    a.runoff_pert = 0.0;
    a.initialize();

    // Build another identical ROM (SpectralROM1D is non-copyable).
    SpectralROM1D c;
    c.basis = &basis;
    c.n_ensemble = 11;
    c.mannings_pert = 0.0;
    c.runoff_pert = 0.0;
    c.initialize();

    std::vector<double> h_det(static_cast<std::size_t>(basis.n_nodes), 0.0);
    std::vector<double> loc(static_cast<std::size_t>(basis.n_nodes), 0.0);
    std::vector<double> spread(static_cast<std::size_t>(basis.n_nodes), 0.0);

    a.seed(h_det.data());
    a.advance(1.0, 0.0, h_det.data(), nullptr, h_det.data());
    a.computeQuantiles(h_det.data(), nullptr);

    c.seed(h_det.data());
    c.setSoftForcing(loc.data(), spread.data());
    c.advance(1.0, 0.0, h_det.data(), nullptr, h_det.data());
    c.computeQuantiles(h_det.data(), nullptr);

    EXPECT_EQ(a.q05, c.q05);
    EXPECT_EQ(a.q50, c.q50);
    EXPECT_EQ(a.q95, c.q95);
}

TEST(SoftForcingROM2D, ProjectsLocAndSpreadAndActivatesModes) {
    MeshData mesh = makeStructuredMesh();
    MeshEigenBasis basis;
    ASSERT_TRUE(basis.build(mesh, 6));

    SpectralROM rom;
    rom.basis = &basis;
    rom.n_ensemble = 11;
    rom.mannings_pert = 0.0;
    rom.rainfall_pert = 0.0;
    rom.initialize();

    std::vector<double> h_det(static_cast<std::size_t>(basis.n_triangles), 0.0);
    std::vector<double> loc(static_cast<std::size_t>(basis.n_triangles));
    std::vector<double> spread(static_cast<std::size_t>(basis.n_triangles));
    const double* mode0 = &basis.P[0];
    for (int i = 0; i < basis.n_triangles; ++i) {
        loc[static_cast<std::size_t>(i)] = mode0[i];
        spread[static_cast<std::size_t>(i)] = 0.25 * mode0[i];
    }

    rom.seed(h_det.data());
    rom.setSoftForcing(loc.data(), spread.data());
    rom.advance(1.0, 0.0, nullptr, nullptr, h_det.data());

    EXPECT_NEAR(rom.r_coarse[0], 1.0, 1.0e-12);
    EXPECT_NEAR(rom.soft_r_spread_[0], 0.25, 1.0e-12);
    EXPECT_GT(rom.n_modes_active, 0);
}

TEST(SoftForcingROM2D, ZeroSpreadEquivalentToNoSoftForcing) {
    MeshData mesh = makeStructuredMesh();
    MeshEigenBasis basis;
    ASSERT_TRUE(basis.build(mesh, 6));

    SpectralROM a;
    a.basis = &basis;
    a.n_ensemble = 11;
    a.mannings_pert = 0.0;
    a.rainfall_pert = 0.0;
    a.initialize();

    SpectralROM c;
    c.basis = &basis;
    c.n_ensemble = 11;
    c.mannings_pert = 0.0;
    c.rainfall_pert = 0.0;
    c.initialize();

    std::vector<double> h_det(static_cast<std::size_t>(basis.n_triangles), 0.0);
    std::vector<double> loc(static_cast<std::size_t>(basis.n_triangles), 0.0);
    std::vector<double> spread(static_cast<std::size_t>(basis.n_triangles), 0.0);

    a.seed(h_det.data());
    a.advance(1.0, 0.0, nullptr, nullptr, h_det.data());
    a.computeQuantiles(h_det.data());

    c.seed(h_det.data());
    c.setSoftForcing(loc.data(), spread.data());
    c.advance(1.0, 0.0, nullptr, nullptr, h_det.data());
    c.computeQuantiles(h_det.data());

    EXPECT_EQ(a.q05, c.q05);
    EXPECT_EQ(a.q50, c.q50);
    EXPECT_EQ(a.q95, c.q95);
}

// ---------------------------------------------------------------------------
// SR-1b family-aware member coefficient (2u-1 for UNIFORM, z=probit(u) else)
// ---------------------------------------------------------------------------

TEST(SoftForcingFamily, NormalCoefficientIsProbit) {
    CsrGraph L = make_chain_laplacian(8);
    GraphEigenBasis basis;
    ASSERT_TRUE(basis.build(L, 4));

    SpectralROM1D rom;
    rom.basis = &basis;
    rom.n_ensemble = 21;
    rom.mannings_pert = 0.0;
    rom.runoff_pert = 0.0;
    rom.initialize();

    std::vector<double> loc(static_cast<std::size_t>(basis.n_nodes), 0.0);
    std::vector<double> spread(static_cast<std::size_t>(basis.n_nodes), 0.0);
    rom.setSoftForcing(loc.data(), spread.data(), DistType::NORMAL);

    double max_abs = 0.0;
    for (int i = 0; i < rom.n_ensemble; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        EXPECT_DOUBLE_EQ(rom.soft_coeff_[ui], rom.soft_z_[ui]);
        max_abs = std::max(max_abs, std::abs(rom.soft_coeff_[ui]));
    }
    EXPECT_DOUBLE_EQ(rom.soft_max_abs_coeff_, max_abs);
}

TEST(SoftForcingFamily, LognormalUsesSameProbitCoefficient) {
    CsrGraph L = make_chain_laplacian(8);
    GraphEigenBasis basis;
    ASSERT_TRUE(basis.build(L, 4));

    SpectralROM1D rom;
    rom.basis = &basis;
    rom.n_ensemble = 21;
    rom.mannings_pert = 0.0;
    rom.runoff_pert = 0.0;
    rom.initialize();

    std::vector<double> loc(static_cast<std::size_t>(basis.n_nodes), 0.0);
    std::vector<double> spread(static_cast<std::size_t>(basis.n_nodes), 0.0);
    rom.setSoftForcing(loc.data(), spread.data(), DistType::LOGNORMAL);

    for (int i = 0; i < rom.n_ensemble; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        EXPECT_DOUBLE_EQ(rom.soft_coeff_[ui], rom.soft_z_[ui]);
    }
}

TEST(SoftForcingFamily, UniformCoefficientIsCenteredBand) {
    CsrGraph L = make_chain_laplacian(8);
    GraphEigenBasis basis;
    ASSERT_TRUE(basis.build(L, 4));

    SpectralROM1D rom;
    rom.basis = &basis;
    rom.n_ensemble = 21;
    rom.mannings_pert = 0.0;
    rom.runoff_pert = 0.0;
    rom.initialize();

    std::vector<double> loc(static_cast<std::size_t>(basis.n_nodes), 0.0);
    std::vector<double> spread(static_cast<std::size_t>(basis.n_nodes), 0.0);
    rom.setSoftForcing(loc.data(), spread.data(), DistType::UNIFORM);

    for (int i = 0; i < rom.n_ensemble; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double expected = 2.0 * rom.soft_u_[ui] - 1.0;
        EXPECT_DOUBLE_EQ(rom.soft_coeff_[ui], expected);
        EXPECT_GE(rom.soft_coeff_[ui], -1.0);
        EXPECT_LE(rom.soft_coeff_[ui], 1.0);
    }
    EXPECT_LE(rom.soft_max_abs_coeff_, 1.0);
}

TEST(SoftForcingFamily, ProbitAccuracyOverRange) {
    // Acklam's stated accuracy: |probit(Phi(z)) - z| < 1.2e-8 on z in [-6, 6].
    for (int k = -600; k <= 600; ++k) {
        const double z = 0.01 * k;
        const double phi = 0.5 * std::erfc(-z / std::sqrt(2.0));
        const double zback = probit(phi);
        EXPECT_LT(std::abs(zback - z), 1.2e-8) << "z=" << z;
    }
}

// ---------------------------------------------------------------------------
// SR-3c — lognormal delta-linearization validity (exact vs. delta member field)
//
// The ROM forces the lognormal family with the delta linearization
//   member_i = loc * (1 + z_i * sigma_log)
// while the exact median-preserving field is
//   member_i = loc * exp(z_i * sigma_log).
// The error grows with sigma_log (CV); the SR-3c guard warns once past CV 0.5.
// The one-shot 2D grid warning itself is covered by test_soft_rain_grid_2d
// (LognormalHighCvWarnsOnce); here we bound the numeric accuracy.
// ---------------------------------------------------------------------------

static double lognormalDeltaRelError(double sigma_log, double z) {
    const double f_delta = 1.0 + z * sigma_log;      // loc factored out (loc = 1)
    const double f_exact = std::exp(z * sigma_log);
    return std::abs(f_delta - f_exact) / f_exact;
}

TEST(SoftForcingLognormal, DeltaAccurateAtLowCv) {
    // Interquartile core (|z| <= 1, ~68% of members): CV = 0.2 stays within the
    // ~5% delta bound. exp() convexity makes the low tail worse than the high
    // tail, so the extreme 95th-percentile coefficient is bounded separately.
    const double z68 = 1.0;
    EXPECT_LT(lognormalDeltaRelError(0.2, z68), 0.05);
    EXPECT_LT(lognormalDeltaRelError(0.2, -z68), 0.05);
    // 95th-percentile coefficient the reported q05/q95 bands ride on: still
    // small (< 7.5%) at CV = 0.2.
    const double z95 = 1.6448536269514722;
    EXPECT_LT(lognormalDeltaRelError(0.2, z95), 0.075);
    EXPECT_LT(lognormalDeltaRelError(0.2, -z95), 0.075);
}

TEST(SoftForcingLognormal, DeltaDegradesAtHighCv) {
    // CV = 0.8 is well past the 0.5 guard: the linearization error at z95 is
    // large, documenting why the SR-3c warning fires.
    const double z95 = 1.6448536269514722;
    EXPECT_GT(lognormalDeltaRelError(0.8, z95), 0.15);
}

// ---------------------------------------------------------------------------
// CL-1b — correlated-coherence spatial soft-forcing field (COHERENCE CORR_LEN)
//
// A per-member, per-active-node coefficient field W_i[t] replaces the scalar
// c_i. A field with every row constant (W_i[t] = c_i ∀t) must reproduce the
// comonotone scalar path exactly; a zero-mean field must preserve q50 (the
// nominal member stays zero-deviation); a zero spread must produce no band.
// ---------------------------------------------------------------------------

// Build the constant-row field W_i[t] = soft_coeff_[i] for the comonotone
// equivalence check. Requires setSoftForcing(family) called first so the ROM's
// soft_coeff_ reflects the family selection.
static SoftSpatialField makeConstantRowField(const SpectralROM1D& rom,
                                              int n_nodes) {
    SoftSpatialField f;
    f.allocate(rom.n_ensemble, n_nodes);
    for (int i = 0; i < rom.n_ensemble; ++i)
        for (int t = 0; t < n_nodes; ++t)
            f.at(i, t) = rom.soft_coeff_[static_cast<std::size_t>(i)];
    return f;
}

TEST(SoftForcingCorrelated, ComonotoneFieldMatchesScalarPath) {
    CsrGraph L = make_chain_laplacian(8);
    GraphEigenBasis basis;
    ASSERT_TRUE(basis.build(L, 4));

    // Non-trivial deterministic head + spread that varies over space so the
    // per-member projection R_{ij} exercises multiple modes.
    std::vector<double> h_det(static_cast<std::size_t>(basis.n_nodes), 0.0);
    std::vector<double> loc(static_cast<std::size_t>(basis.n_nodes));
    std::vector<double> spread(static_cast<std::size_t>(basis.n_nodes));
    const double* mode0 = &basis.P[0];
    const double* mode1 = &basis.P[static_cast<std::size_t>(basis.n_nodes)];
    for (int i = 0; i < basis.n_nodes; ++i) {
        loc[static_cast<std::size_t>(i)]    = mode0[i];
        spread[static_cast<std::size_t>(i)] = 0.4 * mode0[i] + 0.25 * mode1[i];
    }

    // Scalar (comonotone) reference.
    SpectralROM1D a;
    a.basis = &basis;
    a.n_ensemble = 11;
    a.mannings_pert = 0.0;
    a.runoff_pert = 0.0;
    a.initialize();
    a.seed(h_det.data());
    a.setSoftForcing(loc.data(), spread.data(), DistType::NORMAL);
    a.advance(1.0, 0.5, h_det.data(), nullptr, h_det.data());
    a.computeQuantiles(h_det.data(), nullptr);

    // Spatial ROM with a constant-row field W_i[t] = c_i.
    SpectralROM1D c;
    c.basis = &basis;
    c.n_ensemble = 11;
    c.mannings_pert = 0.0;
    c.runoff_pert = 0.0;
    c.initialize();
    c.seed(h_det.data());
    // First set scalar to populate soft_coeff_, build the matching field, then
    // re-set with the spatial field.
    c.setSoftForcing(loc.data(), spread.data(), DistType::NORMAL);
    SoftSpatialField field = makeConstantRowField(c, basis.n_nodes);
    c.setSoftForcing(loc.data(), spread.data(), DistType::NORMAL, &field);
    c.advance(1.0, 0.5, h_det.data(), nullptr, h_det.data());
    c.computeQuantiles(h_det.data(), nullptr);

    ASSERT_EQ(a.n_modes_active, c.n_modes_active);
    for (std::size_t j = 0; j < a.a_ensemble.size(); ++j)
        EXPECT_NEAR(a.a_ensemble[j], c.a_ensemble[j], 1.0e-14)
            << "member/mode flat index " << j;
    for (int t = 0; t < basis.n_nodes; ++t) {
        const auto ut = static_cast<std::size_t>(t);
        EXPECT_NEAR(a.q05[ut], c.q05[ut], 1.0e-14);
        EXPECT_NEAR(a.q50[ut], c.q50[ut], 1.0e-14);
        EXPECT_NEAR(a.q95[ut], c.q95[ut], 1.0e-14);
    }
}

TEST(SoftForcingCorrelated, NominalMemberStillZero) {
    CsrGraph L = make_chain_laplacian(8);
    GraphEigenBasis basis;
    ASSERT_TRUE(basis.build(L, 4));

    SpectralROM1D rom;
    rom.basis = &basis;
    rom.n_ensemble = 11;
    rom.mannings_pert = 0.0;
    rom.runoff_pert = 0.0;
    rom.initialize();

    std::vector<double> h_det(static_cast<std::size_t>(basis.n_nodes), 0.0);
    std::vector<double> loc(static_cast<std::size_t>(basis.n_nodes));
    std::vector<double> spread(static_cast<std::size_t>(basis.n_nodes));
    const double* mode0 = &basis.P[0];
    const double* mode1 = &basis.P[static_cast<std::size_t>(basis.n_nodes)];
    for (int i = 0; i < basis.n_nodes; ++i) {
        loc[static_cast<std::size_t>(i)]    = mode0[i];
        spread[static_cast<std::size_t>(i)] = 0.5 * mode0[i];
    }

    rom.seed(h_det.data());
    rom.setSoftForcing(loc.data(), spread.data(), DistType::NORMAL);

    // Genuinely spatial, non-comonotone field with zero column mean: use a
    // non-constant spatial shape s[t] and set W_i[t] = c_i * s[t]. Because the
    // NORMAL coefficients are symmetric (mean_i(c_i) = c̄ = 0), the per-node
    // column mean is c̄ at every node (satisfies the setSoftForcing assert) and
    // the nominal member (c_i = 0) keeps an all-zero row → zero deviation.
    const int M = rom.n_ensemble;
    SoftSpatialField field;
    field.allocate(M, basis.n_nodes);
    for (int t = 0; t < basis.n_nodes; ++t) {
        const double s = 1.0 + 0.6 * (mode1[t] / (std::abs(mode0[t]) + 1.0e-9));
        for (int i = 0; i < M; ++i)
            field.at(i, t) = rom.soft_coeff_[static_cast<std::size_t>(i)] * s;
    }
    rom.setSoftForcing(loc.data(), spread.data(), DistType::NORMAL, &field);
    rom.advance(1.0, 0.5, h_det.data(), nullptr, h_det.data());
    rom.computeQuantiles(h_det.data(), nullptr);

    // Identify the nominal member (c_i closest to 0) and assert its deviation
    // is exactly zero across all modes.
    int nominal = 0;
    double min_abs = std::abs(rom.soft_coeff_[0]);
    for (int i = 1; i < M; ++i) {
        const double a = std::abs(rom.soft_coeff_[static_cast<std::size_t>(i)]);
        if (a < min_abs) { min_abs = a; nominal = i; }
    }
    const auto nk = static_cast<std::size_t>(rom.n_kept);
    for (std::size_t j = 0; j < nk; ++j)
        EXPECT_NEAR(rom.a_ensemble[static_cast<std::size_t>(nominal) * nk + j],
                    0.0, 1.0e-15) << "nominal member mode " << j;

    // q50 must still track the deterministic answer (h_det == 0 here).
    for (int t = 0; t < basis.n_nodes; ++t)
        EXPECT_NEAR(rom.q50[static_cast<std::size_t>(t)], 0.0, 1.0e-12)
            << "node " << t;
}

TEST(SoftForcingCorrelated, ZeroSpreadStillZeroBand) {
    CsrGraph L = make_chain_laplacian(8);
    GraphEigenBasis basis;
    ASSERT_TRUE(basis.build(L, 4));

    SpectralROM1D rom;
    rom.basis = &basis;
    rom.n_ensemble = 11;
    rom.mannings_pert = 0.0;
    rom.runoff_pert = 0.0;
    rom.initialize();

    std::vector<double> h_det(static_cast<std::size_t>(basis.n_nodes), 0.0);
    std::vector<double> loc(static_cast<std::size_t>(basis.n_nodes), 0.0);
    std::vector<double> spread(static_cast<std::size_t>(basis.n_nodes), 0.0);
    const double* mode0 = &basis.P[0];
    for (int i = 0; i < basis.n_nodes; ++i)
        loc[static_cast<std::size_t>(i)] = mode0[i];

    rom.seed(h_det.data());
    rom.setSoftForcing(loc.data(), spread.data(), DistType::NORMAL);

    // A fully populated spatial field but with zero spread ⇒ zero forcing.
    SoftSpatialField field;
    field.allocate(rom.n_ensemble, basis.n_nodes);
    for (int i = 0; i < rom.n_ensemble; ++i)
        for (int t = 0; t < basis.n_nodes; ++t)
            field.at(i, t) = rom.soft_coeff_[static_cast<std::size_t>(i)];
    rom.setSoftForcing(loc.data(), spread.data(), DistType::NORMAL, &field);
    rom.advance(1.0, 0.5, h_det.data(), nullptr, h_det.data());
    rom.computeQuantiles(h_det.data(), nullptr);

    for (int t = 0; t < basis.n_nodes; ++t) {
        const auto ut = static_cast<std::size_t>(t);
        EXPECT_DOUBLE_EQ(rom.q05[ut], rom.q95[ut]);
        EXPECT_DOUBLE_EQ(rom.q50[ut], rom.q95[ut]);
    }
}

// 2D ROM: a constant-row spatial field (W_i[t] = c_i ∀t) must reproduce the
// comonotone scalar soft path exactly, confirming the shared spatial machinery
// on the 2D SpectralROM.
TEST(SoftForcingCorrelated2D, ComonotoneFieldMatchesScalarPath) {
    MeshData mesh = makeStructuredMesh();
    MeshEigenBasis basis;
    ASSERT_TRUE(basis.build(mesh, 6));

    const int nt = basis.n_triangles;
    std::vector<double> h_det(static_cast<std::size_t>(nt), 0.0);
    std::vector<double> loc(static_cast<std::size_t>(nt));
    std::vector<double> spread(static_cast<std::size_t>(nt));
    const double* mode0 = &basis.P[0];
    const double* mode1 = &basis.P[static_cast<std::size_t>(nt)];
    for (int i = 0; i < nt; ++i) {
        loc[static_cast<std::size_t>(i)]    = mode0[i];
        spread[static_cast<std::size_t>(i)] = 0.3 * mode0[i] + 0.2 * mode1[i];
    }

    SpectralROM a;
    a.basis = &basis;
    a.n_ensemble = 11;
    a.mannings_pert = 0.0;
    a.rainfall_pert = 0.0;
    a.initialize();
    a.seed(h_det.data());
    a.setSoftForcing(loc.data(), spread.data(), DistType::NORMAL);
    a.advance(1.0, 0.0, nullptr, nullptr, h_det.data());
    a.computeQuantiles(h_det.data());

    SpectralROM c;
    c.basis = &basis;
    c.n_ensemble = 11;
    c.mannings_pert = 0.0;
    c.rainfall_pert = 0.0;
    c.initialize();
    c.seed(h_det.data());
    c.setSoftForcing(loc.data(), spread.data(), DistType::NORMAL);
    SpatialUncertaintyField field;
    field.allocate(c.n_ensemble, nt);
    for (int i = 0; i < c.n_ensemble; ++i)
        for (int t = 0; t < nt; ++t)
            field.at(i, t) = c.soft_coeff_[static_cast<std::size_t>(i)];
    c.setSoftForcing(loc.data(), spread.data(), DistType::NORMAL, &field);
    c.advance(1.0, 0.0, nullptr, nullptr, h_det.data());
    c.computeQuantiles(h_det.data());

    ASSERT_EQ(a.n_modes_active, c.n_modes_active);
    for (std::size_t j = 0; j < a.a_ensemble.size(); ++j)
        EXPECT_NEAR(a.a_ensemble[j], c.a_ensemble[j], 1.0e-14) << "flat index " << j;
    for (int t = 0; t < nt; ++t) {
        const auto ut = static_cast<std::size_t>(t);
        EXPECT_NEAR(a.q05[ut], c.q05[ut], 1.0e-14);
        EXPECT_NEAR(a.q50[ut], c.q50[ut], 1.0e-14);
        EXPECT_NEAR(a.q95[ut], c.q95[ut], 1.0e-14);
    }
}

} // anonymous namespace


// ===========================================================================
// PR H7 — true per-family coefficient planes (MIXED + multi-gage)
//
// Before H7 the ROM carried ONE (spread, family) channel, so a MIXED source
// had to borrow a single family's coefficient column for every cell/gage and
// range-match the others by pre-scaling their spread (the SR-4b ~3.0x hack) or
// fall back to the first family outright (SR-1b). H7 adds a second, nullable
// (spread_b, family_b) channel: two projections per advance, each carrying its
// OWN family's per-member coefficient,
//
//     g_ij  +=  c_i * (P^T spread)_j  +  c_i^B * (P^T spread_b)_j
//
// with both coefficient columns drawn from the SAME u_i stream
// (shuffledStrata(M, sample_seed+4)) so comonotone coherence -- one rank per
// member everywhere, the COHERENCE FULL contract -- is preserved across
// families by construction.
//
// NOTE ON WHAT IS AND IS NOT TESTABLE PER CELL. The spec's phrasing ("UNIFORM
// cells' member values uniform ... not z-shaped") cannot be asserted on a
// reconstructed per-CELL value: the ROM's eigenmodes are GLOBAL, so every mode
// sees both planes and every cell's reconstruction is a linear combination of
// both coefficients. That mixing is a property of the spectral ROM itself, not
// something per-family planes can or should remove. The claim that IS exactly
// true -- and is what H7 actually fixes -- is per SOURCE PLANE: each plane's
// modal forcing carries its own family's coefficient column. The marginal-shape
// tests below therefore use spread planes aligned with ORTHOGONAL eigenmodes,
// which isolates one plane per mode and makes the marginal claim exact rather
// than approximate, and the checkerboard fixture then verifies the full
// two-plane decomposition end to end against an independent analytic
// per-member reference.
// ===========================================================================

// Analytic single-step modal solution from a zero initial deviation:
//   d(da)/dt = -rate*da + g,  da(0) = 0  =>  da(dt) = (g/rate)*(1-exp(-rate*dt))
// (Euler below the rate floor). Used as an independent reference so the tests
// do not merely re-derive the ROM's own arithmetic.
static double analyticStepFromZero(double g, double rate, double dt) {
    if (rate > 1.0e-12)
        return (g / rate) * (1.0 - std::exp(-rate * dt));
    return g * dt;
}

// Spearman-style check: two coefficient columns are comonotone when sorting
// members by one also sorts them by the other (identical rank permutation).
static bool sameRankOrder(const std::vector<double>& a,
                          const std::vector<double>& b) {
    const std::size_t M = a.size();
    if (b.size() != M) return false;
    std::vector<std::size_t> ia(M), ib(M);
    for (std::size_t i = 0; i < M; ++i) { ia[i] = i; ib[i] = i; }
    std::sort(ia.begin(), ia.end(), [&](std::size_t p, std::size_t q) { return a[p] < a[q]; });
    std::sort(ib.begin(), ib.end(), [&](std::size_t p, std::size_t q) { return b[p] < b[q]; });
    return ia == ib;
}

// ---------------------------------------------------------------------------
// Coefficient-column contract
// ---------------------------------------------------------------------------

TEST(SoftForcingFamilyPlanes, BothChannelsShareOneRankPerMember) {
    CsrGraph L = make_chain_laplacian(8);
    GraphEigenBasis basis;
    ASSERT_TRUE(basis.build(L, 4));

    SpectralROM1D rom;
    rom.basis = &basis;
    rom.n_ensemble = 21;            // odd -> one member sits exactly at u = 0.5
    rom.mannings_pert = 0.0;
    rom.runoff_pert = 0.0;
    rom.initialize();

    const auto nn = static_cast<std::size_t>(basis.n_nodes);
    std::vector<double> h_det(nn, 0.0), spread_a(nn), spread_b(nn);
    const double* mode0 = &basis.P[0];
    const double* mode1 = &basis.P[nn];
    for (std::size_t t = 0; t < nn; ++t) {
        spread_a[t] = 0.5 * mode0[t];
        spread_b[t] = 0.5 * mode1[t];
    }

    rom.seed(h_det.data());
    rom.setSoftForcing(nullptr, spread_a.data(), DistType::NORMAL, nullptr,
                       spread_b.data(), DistType::UNIFORM);

    ASSERT_TRUE(rom.softPlanesError().empty()) << rom.softPlanesError();
    ASSERT_EQ(static_cast<int>(rom.softCoeffB().size()), rom.n_ensemble);

    // COHERENCE FULL contract: one rank per member across families.
    EXPECT_TRUE(sameRankOrder(rom.softCoeff(), rom.softCoeffB()));

    // Both columns are zero-mean, so neither channel shifts the median.
    double mean_a = 0.0, mean_b = 0.0;
    for (int i = 0; i < rom.n_ensemble; ++i) {
        mean_a += rom.softCoeff()[static_cast<std::size_t>(i)];
        mean_b += rom.softCoeffB()[static_cast<std::size_t>(i)];
    }
    EXPECT_NEAR(mean_a / rom.n_ensemble, 0.0, 1.0e-12);
    EXPECT_NEAR(mean_b / rom.n_ensemble, 0.0, 1.0e-12);

    // The nominal member is nominal in BOTH channels simultaneously -- the
    // property that makes the deviation-form invariant survive two families.
    int nominal = -1;
    for (int i = 0; i < rom.n_ensemble; ++i)
        if (std::abs(rom.softCoeffB()[static_cast<std::size_t>(i)]) < 1.0e-15)
            nominal = i;
    ASSERT_GE(nominal, 0) << "odd M must place one member exactly at u = 0.5";
    EXPECT_NEAR(rom.softCoeff()[static_cast<std::size_t>(nominal)], 0.0, 1.0e-12);

    rom.advance(1.0, 0.5, h_det.data(), nullptr, h_det.data());
    const auto nk = static_cast<std::size_t>(rom.n_kept);
    for (std::size_t j = 0; j < nk; ++j)
        EXPECT_NEAR(rom.a_ensemble[static_cast<std::size_t>(nominal) * nk + j],
                    0.0, 1.0e-15) << "nominal member mode " << j;
}

// ---------------------------------------------------------------------------
// The marginal-shape claim, made exact by aligning each plane with its own
// eigenmode: mode 0 is fed only by the NORMAL plane, mode 1 only by the
// UNIFORM plane, so each mode's member values must follow that family's
// quantiles -- and, decisively, NOT the other's. Under the pre-H7 hack both
// modes would have carried the NORMAL column.
// ---------------------------------------------------------------------------

TEST(SoftForcingFamilyPlanes, EachPlaneKeepsItsOwnMarginalShape) {
    CsrGraph L = make_chain_laplacian(10);
    GraphEigenBasis basis;
    ASSERT_TRUE(basis.build(L, 4));

    SpectralROM1D rom;
    rom.basis = &basis;
    rom.n_ensemble = 25;
    rom.mannings_pert = 0.0;
    rom.runoff_pert = 0.0;
    rom.mode_drop_threshold = 0.0;   // keep every mode active; isolate the test
    rom.initialize();

    const auto nn = static_cast<std::size_t>(basis.n_nodes);
    std::vector<double> h_det(nn, 0.0), spread_a(nn), spread_b(nn);
    const double* mode0 = &basis.P[0];
    const double* mode1 = &basis.P[nn];
    for (std::size_t t = 0; t < nn; ++t) {
        spread_a[t] = 0.7 * mode0[t];   // -> R_A = (0.7, 0, 0, ...)
        spread_b[t] = 0.7 * mode1[t];   // -> R_B = (0, 0.7, 0, ...)
    }

    rom.seed(h_det.data());
    rom.setSoftForcing(nullptr, spread_a.data(), DistType::NORMAL, nullptr,
                       spread_b.data(), DistType::UNIFORM);
    ASSERT_TRUE(rom.softPlanesError().empty());
    rom.advance(1.0, 0.5, h_det.data(), nullptr, h_det.data());

    const auto nk = static_cast<std::size_t>(rom.n_kept);
    const int   M = rom.n_ensemble;
    ASSERT_GE(rom.n_kept, 2);

    // Orthonormal basis => the two planes land on disjoint modes.
    EXPECT_NEAR(rom.soft_r_spread_[0],   0.7, 1.0e-12);
    EXPECT_NEAR(rom.soft_r_spread_[1],   0.0, 1.0e-12);
    EXPECT_NEAR(rom.soft_r_spread_b_[0], 0.0, 1.0e-12);
    EXPECT_NEAR(rom.soft_r_spread_b_[1], 0.7, 1.0e-12);

    // Mode 0 must be proportional to z_i (NORMAL) and mode 1 to (2u_i - 1)
    // (UNIFORM). Fit each mode's scale off a single member, then require every
    // other member to match that family's quantile to machine precision.
    auto shapeMisfit = [&](std::size_t mode, const std::vector<double>& coeff) {
        std::size_t ref = 0;
        for (int i = 0; i < M; ++i)
            if (std::abs(coeff[static_cast<std::size_t>(i)]) >
                std::abs(coeff[ref])) ref = static_cast<std::size_t>(i);
        const double scale = rom.a_ensemble[ref * nk + mode] / coeff[ref];
        double worst = 0.0;
        for (int i = 0; i < M; ++i) {
            const auto ui = static_cast<std::size_t>(i);
            const double predicted = scale * coeff[ui];
            worst = std::max(worst,
                std::abs(rom.a_ensemble[ui * nk + mode] - predicted));
        }
        return worst / std::abs(scale);   // relative to the fitted amplitude
    };

    // Each mode fits its OWN family essentially exactly ...
    EXPECT_LT(shapeMisfit(0, rom.softCoeff()),  1.0e-12);
    EXPECT_LT(shapeMisfit(1, rom.softCoeffB()), 1.0e-12);
    // ... and is a poor fit to the other family, which is precisely the defect
    // the pre-H7 single-column hack had: a UNIFORM source rendered z-shaped.
    // Measured cross-family misfit on this fixture: 0.460 (mode 1 against the
    // NORMAL column) and 0.215 (mode 0 against the UNIFORM column), i.e. 20-46x
    // the 1e-2 bar below -- a wide margin, not a tuned threshold.
    EXPECT_GT(shapeMisfit(1, rom.softCoeff()),  1.0e-2);
    EXPECT_GT(shapeMisfit(0, rom.softCoeffB()), 1.0e-2);
}

// ---------------------------------------------------------------------------
// Regression lock: a caller that supplies no second plane -- or supplies one
// that is identically zero -- must be bit-identical to the single-family path.
// This is the "single-family paths bit-identical before/after" gate; the 14
// pre-existing SR-3a/CL-1b tests in this file are its companion (they exercise
// the single-family path exclusively and are unchanged by H7).
// ---------------------------------------------------------------------------

TEST(SoftForcingFamilyPlanes, NullAndZeroSecondPlaneAreBitIdentical) {
    CsrGraph L = make_chain_laplacian(8);
    GraphEigenBasis basis;
    ASSERT_TRUE(basis.build(L, 4));

    const auto nn = static_cast<std::size_t>(basis.n_nodes);
    std::vector<double> h_det(nn, 0.0), loc(nn), spread(nn);
    std::vector<double> zero_plane(nn, 0.0);
    const double* mode0 = &basis.P[0];
    const double* mode1 = &basis.P[nn];
    for (std::size_t t = 0; t < nn; ++t) {
        loc[t]    = mode0[t];
        spread[t] = 0.4 * mode0[t] + 0.25 * mode1[t];
    }

    auto run = [&](const double* second) {
        SpectralROM1D rom;
        rom.basis = &basis;
        rom.n_ensemble = 11;
        rom.mannings_pert = 0.0;
        rom.runoff_pert = 0.0;
        rom.initialize();
        rom.seed(h_det.data());
        rom.setSoftForcing(loc.data(), spread.data(), DistType::NORMAL,
                           nullptr, second, DistType::UNIFORM);
        rom.advance(1.0, 0.5, h_det.data(), nullptr, h_det.data());
        rom.computeQuantiles(h_det.data(), nullptr);
        return rom;
    };

    SpectralROM1D none = run(nullptr);
    SpectralROM1D zero = run(zero_plane.data());

    ASSERT_EQ(none.n_modes_active, zero.n_modes_active);
    for (std::size_t i = 0; i < none.a_ensemble.size(); ++i)
        EXPECT_EQ(none.a_ensemble[i], zero.a_ensemble[i])   // EQ, not NEAR
            << "member/mode flat index " << i;
    for (std::size_t t = 0; t < nn; ++t) {
        EXPECT_EQ(none.q05[t], zero.q05[t]);
        EXPECT_EQ(none.q50[t], zero.q50[t]);
        EXPECT_EQ(none.q95[t], zero.q95[t]);
    }
}

// ---------------------------------------------------------------------------
// Superposition: the two-plane result equals the sum of the two single-plane
// results (each run with ITS OWN family), because the modal ODE is linear in
// the forcing and every run starts from an identical zero deviation. This is
// the structural statement that "two projections, one coefficient each" is
// what actually got implemented -- not a range-matched single column.
// ---------------------------------------------------------------------------

TEST(SoftForcingFamilyPlanes, TwoPlanesSuperposeTheirSingleFamilyRuns) {
    CsrGraph L = make_chain_laplacian(9);
    GraphEigenBasis basis;
    ASSERT_TRUE(basis.build(L, 4));

    const auto nn = static_cast<std::size_t>(basis.n_nodes);
    std::vector<double> h_det(nn, 0.0), spread_a(nn), spread_b(nn);
    const double* mode0 = &basis.P[0];
    const double* mode1 = &basis.P[nn];
    const double* mode2 = &basis.P[2 * nn];
    for (std::size_t t = 0; t < nn; ++t) {
        // Deliberately NOT mode-aligned: both planes excite several modes, so
        // the superposition claim is tested on genuinely overlapping forcing.
        spread_a[t] = 0.5 * mode0[t] + 0.2 * mode2[t];
        spread_b[t] = 0.3 * mode1[t] + 0.4 * mode2[t];
    }

    auto run = [&](const double* sa, DistType fa,
                   const double* sb, DistType fb) {
        SpectralROM1D rom;
        rom.basis = &basis;
        rom.n_ensemble = 15;
        rom.mannings_pert = 0.0;
        rom.runoff_pert = 0.0;
        rom.mode_drop_threshold = 0.0;   // all modes active in every run
        rom.initialize();
        rom.seed(h_det.data());
        rom.setSoftForcing(nullptr, sa, fa, nullptr, sb, fb);
        rom.advance(2.0, 0.5, h_det.data(), nullptr, h_det.data());
        return rom;
    };

    SpectralROM1D both = run(spread_a.data(), DistType::NORMAL,
                             spread_b.data(), DistType::UNIFORM);
    SpectralROM1D only_a = run(spread_a.data(), DistType::NORMAL, nullptr,
                               DistType::UNIFORM);
    SpectralROM1D only_b = run(spread_b.data(), DistType::UNIFORM, nullptr,
                               DistType::UNIFORM);

    for (std::size_t i = 0; i < both.a_ensemble.size(); ++i)
        EXPECT_NEAR(both.a_ensemble[i],
                    only_a.a_ensemble[i] + only_b.a_ensemble[i], 1.0e-14)
            << "member/mode flat index " << i;

    // And against a fully independent closed form, so the test is not just the
    // ROM agreeing with itself.
    const auto nk = static_cast<std::size_t>(both.n_kept);
    for (int i = 0; i < both.n_ensemble; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        for (std::size_t j = 0; j < nk; ++j) {
            const double g = both.softCoeff()[ui]  * both.soft_r_spread_[j]
                           + both.softCoeffB()[ui] * both.soft_r_spread_b_[j];
            const double rate = basis.eigenvalues[j] * 0.5;   // lambda * K1d, mm = 1
            EXPECT_NEAR(both.a_ensemble[ui * nk + j],
                        analyticStepFromZero(g, rate, 2.0), 1.0e-13)
                << "member " << i << " mode " << j;
        }
    }
}

// ---------------------------------------------------------------------------
// Multi-gage mixed families (SR-1b's fallback, removed): two disjoint node
// groups carrying different families both contribute a real band, with no
// first-family fallback and no warning.
// ---------------------------------------------------------------------------

TEST(SoftForcingFamilyPlanes, TwoGageMixedFamiliesBothContributeNoFallback) {
    CsrGraph L = make_chain_laplacian(10);
    GraphEigenBasis basis;
    ASSERT_TRUE(basis.build(L, 4));

    const auto nn = static_cast<std::size_t>(basis.n_nodes);
    std::vector<double> h_det(nn, 0.0);
    // Gage A serves the upstream half (NORMAL), gage B the downstream half
    // (UNIFORM) -- disjoint supports, exactly how a multi-gage source splits.
    // Unequal spreads on purpose: with EQUAL spreads the two complementary
    // halves project onto every (zero-mean) eigenmode with exactly opposite
    // signs, so the fallback reference below would collapse to a degenerate
    // zero band and the comparison would prove less than it appears.
    std::vector<double> spread_a(nn, 0.0), spread_b(nn, 0.0);
    for (std::size_t t = 0; t < nn; ++t) {
        if (t < nn / 2) spread_a[t] = 0.60;
        else            spread_b[t] = 0.35;
    }

    SpectralROM1D rom;
    rom.basis = &basis;
    rom.n_ensemble = 21;
    rom.mannings_pert = 0.0;
    rom.runoff_pert = 0.0;
    rom.initialize();
    rom.seed(h_det.data());
    rom.setSoftForcing(nullptr, spread_a.data(), DistType::NORMAL, nullptr,
                       spread_b.data(), DistType::UNIFORM);

    // No fallback: the second family is honoured, not collapsed onto the first.
    EXPECT_TRUE(rom.softPlanesError().empty()) << rom.softPlanesError();
    ASSERT_EQ(static_cast<int>(rom.softCoeffB().size()), rom.n_ensemble);

    rom.advance(1.0, 0.5, h_det.data(), nullptr, h_det.data());
    rom.computeQuantiles(h_det.data(), nullptr);

    // Both gages' spreads reach the modal forcing.
    double abs_a = 0.0, abs_b = 0.0;
    for (int j = 0; j < rom.n_kept; ++j) {
        abs_a = std::max(abs_a, std::abs(rom.soft_r_spread_[static_cast<std::size_t>(j)]));
        abs_b = std::max(abs_b, std::abs(rom.soft_r_spread_b_[static_cast<std::size_t>(j)]));
    }
    EXPECT_GT(abs_a, 1.0e-6);
    EXPECT_GT(abs_b, 1.0e-6);

    // A real band forms, and the median still tracks the deterministic run.
    double widest = 0.0;
    for (std::size_t t = 0; t < nn; ++t)
        widest = std::max(widest, rom.q95[t] - rom.q05[t]);
    EXPECT_GT(widest, 1.0e-6);
    for (std::size_t t = 0; t < nn; ++t)
        EXPECT_NEAR(rom.q50[t], 0.0, 1.0e-12) << "node " << t;

    // The decisive comparison: reproduce SR-1b's REMOVED first-family fallback
    // -- every gage forced onto gage A's family, i.e. one NORMAL plane over the
    // summed spread -- and require the per-family answer to differ materially
    // from it. This is what "no longer falls back to the first family" means
    // operationally.
    //
    // NOTE (real property, not a defect): the per-family band here is NARROWER
    // than the fallback's, not wider. Comonotone coherence gives every member
    // the same rank in both families, and two gages on complementary parts of
    // the network project onto the shared eigenmodes with opposing signs, so
    // their contributions partially cancel. Adding a second family plane is
    // therefore not monotone in band width -- asserting "wider" would be
    // asserting a coincidence of the fixture.
    std::vector<double> spread_sum(nn, 0.0);
    for (std::size_t t = 0; t < nn; ++t)
        spread_sum[t] = spread_a[t] + spread_b[t];

    SpectralROM1D fallback;
    fallback.basis = &basis;
    fallback.n_ensemble = 21;
    fallback.mannings_pert = 0.0;
    fallback.runoff_pert = 0.0;
    fallback.initialize();
    fallback.seed(h_det.data());
    fallback.setSoftForcing(nullptr, spread_sum.data(), DistType::NORMAL);
    fallback.advance(1.0, 0.5, h_det.data(), nullptr, h_det.data());
    fallback.computeQuantiles(h_det.data(), nullptr);

    double widest_fb = 0.0;
    for (std::size_t t = 0; t < nn; ++t)
        widest_fb = std::max(widest_fb, fallback.q95[t] - fallback.q05[t]);
    EXPECT_GT(widest_fb, 1.0e-6);

    // Materially different -- gage B's UNIFORM marginal is genuinely in play,
    // not absorbed into gage A's NORMAL column.
    EXPECT_GT(std::abs(widest - widest_fb) / widest_fb, 0.05)
        << "per-family band " << widest << " vs first-family fallback "
        << widest_fb;
}

// ---------------------------------------------------------------------------
// Scope guard: CORR_LEN x MIXED is refused with a clear message, and the ROM
// stays in a valid single-family CORR_LEN state rather than half-configured.
// ---------------------------------------------------------------------------

TEST(SoftForcingFamilyPlanes, CorrLenTimesMixedIsRefusedNotSilentlyMixed) {
    CsrGraph L = make_chain_laplacian(8);
    GraphEigenBasis basis;
    ASSERT_TRUE(basis.build(L, 4));

    const auto nn = static_cast<std::size_t>(basis.n_nodes);
    std::vector<double> h_det(nn, 0.0), spread_a(nn), spread_b(nn);
    const double* mode0 = &basis.P[0];
    const double* mode1 = &basis.P[nn];
    for (std::size_t t = 0; t < nn; ++t) {
        spread_a[t] = 0.5 * mode0[t];
        spread_b[t] = 0.5 * mode1[t];
    }

    SpectralROM1D rom;
    rom.basis = &basis;
    rom.n_ensemble = 11;
    rom.mannings_pert = 0.0;
    rom.runoff_pert = 0.0;
    rom.initialize();
    rom.seed(h_det.data());

    // Populate soft_coeff_ so the constant-row field can be built, then arm
    // CORR_LEN and a second family plane together.
    rom.setSoftForcing(nullptr, spread_a.data(), DistType::NORMAL);
    SoftSpatialField field = makeConstantRowField(rom, basis.n_nodes);
    rom.setSoftForcing(nullptr, spread_a.data(), DistType::NORMAL, &field,
                       spread_b.data(), DistType::UNIFORM);

    EXPECT_FALSE(rom.softPlanesError().empty());
    EXPECT_NE(rom.softPlanesError().find("CORR_LEN"), std::string::npos);
    EXPECT_TRUE(rom.softCoeffB().empty());

    // Behaviour is exactly the single-family CORR_LEN run, not a half-applied
    // second plane.
    rom.advance(1.0, 0.5, h_det.data(), nullptr, h_det.data());

    SpectralROM1D ref;
    ref.basis = &basis;
    ref.n_ensemble = 11;
    ref.mannings_pert = 0.0;
    ref.runoff_pert = 0.0;
    ref.initialize();
    ref.seed(h_det.data());
    ref.setSoftForcing(nullptr, spread_a.data(), DistType::NORMAL);
    SoftSpatialField ref_field = makeConstantRowField(ref, basis.n_nodes);
    ref.setSoftForcing(nullptr, spread_a.data(), DistType::NORMAL, &ref_field);
    ref.advance(1.0, 0.5, h_det.data(), nullptr, h_det.data());

    for (std::size_t i = 0; i < ref.a_ensemble.size(); ++i)
        EXPECT_EQ(rom.a_ensemble[i], ref.a_ensemble[i]) << "flat index " << i;

    // A later single-family call clears the error -- it is per-call state.
    rom.setSoftForcing(nullptr, spread_a.data(), DistType::NORMAL);
    EXPECT_TRUE(rom.softPlanesError().empty());
}

// ---------------------------------------------------------------------------
// 2D: the checkerboard MIXED grid the SR-4b hack was written for. Cells
// alternate NORMAL/UNIFORM family; each family's cells go into their own plane
// (zero elsewhere), and the result is checked against an independent analytic
// per-member reference built from the two coefficient columns.
// ---------------------------------------------------------------------------

TEST(SoftForcingFamilyPlanes2D, CheckerboardMixedGridUsesBothFamilies) {
    MeshData mesh = makeStructuredMesh(4, 4.0);
    MeshEigenBasis basis;
    ASSERT_TRUE(basis.build(mesh, 6));

    const int nt = basis.n_triangles;
    const auto unt = static_cast<std::size_t>(nt);
    std::vector<double> h_det(unt, 0.0);
    std::vector<double> spread_norm(unt, 0.0), spread_unif(unt, 0.0);

    // /family_code checkerboard: even cells NORMAL, odd cells UNIFORM. Each
    // plane holds only its own cells' spread; the two are disjoint and sum to
    // the full spread field, which is exactly the split H7 introduces.
    const double* mode0 = &basis.P[0];
    for (int t = 0; t < nt; ++t) {
        const auto ut = static_cast<std::size_t>(t);
        const double sp = 0.05 + 0.2 * std::abs(mode0[t]);
        if (t % 2 == 0) spread_norm[ut] = sp;
        else            spread_unif[ut] = sp;
    }

    SpectralROM rom;
    rom.basis = &basis;
    rom.n_ensemble = 21;
    rom.mannings_pert = 0.0;
    rom.rainfall_pert = 0.0;
    rom.mode_drop_threshold = 0.0;
    rom.initialize();
    rom.seed(h_det.data());
    rom.setSoftForcing(nullptr, spread_norm.data(), DistType::NORMAL, nullptr,
                       spread_unif.data(), DistType::UNIFORM);

    ASSERT_TRUE(rom.softPlanesError().empty()) << rom.softPlanesError();
    ASSERT_EQ(static_cast<int>(rom.softCoeffB().size()), rom.n_ensemble);
    EXPECT_TRUE(sameRankOrder(rom.softCoeff(), rom.softCoeffB()));

    const double dt = 1.0;
    rom.advance(dt, 0.0, nullptr, nullptr, h_det.data());
    rom.computeQuantiles(h_det.data());

    // Independent reference: project each plane by hand off the basis and
    // integrate the modal ODE in closed form, per member.
    const auto nk = static_cast<std::size_t>(rom.n_kept);
    std::vector<double> RA(nk, 0.0), RB(nk, 0.0);
    for (std::size_t j = 0; j < nk; ++j) {
        const double* Pj = &basis.P[j * unt];
        for (std::size_t t = 0; t < unt; ++t) {
            RA[j] += Pj[t] * spread_norm[t];
            RB[j] += Pj[t] * spread_unif[t];
        }
    }
    // Both families genuinely reach the modal forcing.
    double maxA = 0.0, maxB = 0.0;
    for (std::size_t j = 0; j < nk; ++j) {
        maxA = std::max(maxA, std::abs(RA[j]));
        maxB = std::max(maxB, std::abs(RB[j]));
    }
    EXPECT_GT(maxA, 1.0e-6);
    EXPECT_GT(maxB, 1.0e-6);

    for (int i = 0; i < rom.n_ensemble; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double cA = rom.softCoeff()[ui];
        const double cB = rom.softCoeffB()[ui];
        for (std::size_t j = 0; j < nk; ++j) {
            // K_eff = 0 here (no Manning channel), so rate = 0 -> Euler branch.
            const double expected = analyticStepFromZero(cA * RA[j] + cB * RB[j],
                                                         0.0, dt);
            EXPECT_NEAR(rom.a_ensemble[ui * nk + j], expected, 1.0e-13)
                << "member " << i << " mode " << j;
        }
    }

    // A real band, and the median still tracks the deterministic field.
    double widest = 0.0;
    for (std::size_t t = 0; t < unt; ++t)
        widest = std::max(widest, rom.q95[t] - rom.q05[t]);
    EXPECT_GT(widest, 1.0e-6);
    for (std::size_t t = 0; t < unt; ++t)
        EXPECT_NEAR(rom.q50[t], 0.0, 1.0e-12) << "cell " << t;
}

// 2D regression lock, mirroring the 1D one.
TEST(SoftForcingFamilyPlanes2D, NullAndZeroSecondPlaneAreBitIdentical) {
    MeshData mesh = makeStructuredMesh();
    MeshEigenBasis basis;
    ASSERT_TRUE(basis.build(mesh, 6));

    const int nt = basis.n_triangles;
    const auto unt = static_cast<std::size_t>(nt);
    std::vector<double> h_det(unt, 0.0), loc(unt), spread(unt);
    std::vector<double> zero_plane(unt, 0.0);
    const double* mode0 = &basis.P[0];
    const double* mode1 = &basis.P[unt];
    for (std::size_t t = 0; t < unt; ++t) {
        loc[t]    = mode0[t];
        spread[t] = 0.3 * mode0[t] + 0.2 * mode1[t];
    }

    auto run = [&](const double* second) {
        SpectralROM rom;
        rom.basis = &basis;
        rom.n_ensemble = 11;
        rom.mannings_pert = 0.0;
        rom.rainfall_pert = 0.0;
        rom.initialize();
        rom.seed(h_det.data());
        rom.setSoftForcing(loc.data(), spread.data(), DistType::NORMAL,
                           nullptr, second, DistType::UNIFORM);
        rom.advance(1.0, 0.0, nullptr, nullptr, h_det.data());
        rom.computeQuantiles(h_det.data());
        return rom;
    };

    SpectralROM none = run(nullptr);
    SpectralROM zero = run(zero_plane.data());

    ASSERT_EQ(none.n_modes_active, zero.n_modes_active);
    for (std::size_t i = 0; i < none.a_ensemble.size(); ++i)
        EXPECT_EQ(none.a_ensemble[i], zero.a_ensemble[i]) << "flat index " << i;
    for (std::size_t t = 0; t < unt; ++t) {
        EXPECT_EQ(none.q05[t], zero.q05[t]);
        EXPECT_EQ(none.q50[t], zero.q50[t]);
        EXPECT_EQ(none.q95[t], zero.q95[t]);
    }
}
