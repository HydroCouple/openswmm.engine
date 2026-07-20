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
#include "2d/solver/SpectralPrecond2D.hpp"
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
    SpectralPrecond2D basis;
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
    SpectralPrecond2D basis;
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

} // anonymous namespace

