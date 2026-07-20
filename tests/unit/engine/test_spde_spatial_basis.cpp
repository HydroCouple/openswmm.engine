/**
 * @file test_spde_spatial_basis.cpp
 * @brief CL-2b tests for the SPDE (Whittle–Matérn ν=2) reduced spatial basis.
 *
 * Design reference: docs/uncertainty/SPDE_SPATIAL_BASIS.md. The two headline
 * invariants from CORR_LEN_PR_CHECKLIST.md CL-2b:
 *   - the generated field reproduces the target covariance model to tolerance
 *     (the model is Matérn ν=2 with κ = 1/ℓ — what the CL-1 generator
 *     actually realizes — NOT exp(−d/ℓ); see design note §2), and
 *   - corr_len → ∞ collapses to K_s = 1 / the constant mode and reproduces
 *     the comonotone coefficients exactly.
 *
 * @ingroup unit_tests
 */

#include <gtest/gtest.h>

#include "uncertainty/SpdeSpatialBasis.hpp"
#include "uncertainty/LhsShuffle.hpp"

#ifdef OPENSWMM_HAS_2D
#include "2d/uncertainty/CorrelatedFieldGenerator.hpp"
#include "2d/uncertainty/SpatialUncertaintyField.hpp"
#endif

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace openswmm::uncertainty;

namespace {

// Regular G×G grid of points with spacing `dx` (metres).
void makeGrid(int G, double dx, std::vector<double>& x, std::vector<double>& y) {
    x.resize(static_cast<std::size_t>(G) * G);
    y.resize(static_cast<std::size_t>(G) * G);
    for (int i = 0; i < G; ++i)
        for (int j = 0; j < G; ++j) {
            x[static_cast<std::size_t>(i) * G + j] = j * dx;
            y[static_cast<std::size_t>(i) * G + j] = i * dx;
        }
}

// Comonotone member coefficients: family transform of shuffled LHS strata
// (mirrors the ROM's softCoeff()).
std::vector<double> familyCoeff(int M, DistType family, uint64_t seed) {
    const std::vector<double> u = shuffledStrata(M, seed);
    std::vector<double> c(static_cast<std::size_t>(M));
    for (int i = 0; i < M; ++i)
        c[static_cast<std::size_t>(i)] = (family == DistType::UNIFORM)
            ? (2.0 * u[static_cast<std::size_t>(i)] - 1.0)
            : probit(u[static_cast<std::size_t>(i)]);
    return c;
}

// Binned empirical member-correlation of an M×n field vs pair distance,
// from `npairs` uniformly sampled point pairs. Bin b covers
// [b, b+1)·dmax/nbins; bins with < 50 pairs are skipped (cnt == 0).
struct CorrBins {
    std::vector<double> d, rho;  // mean distance / mean correlation per bin
};
CorrBins binnedCorrelation(const std::vector<double>& W, int M, int n,
                           const std::vector<double>& x,
                           const std::vector<double>& y,
                           double dmax, int nbins, int npairs) {
    std::vector<double> mu(static_cast<std::size_t>(n), 0.0);
    std::vector<double> sd(static_cast<std::size_t>(n), 0.0);
    for (int t = 0; t < n; ++t) {
        double m = 0.0;
        for (int i = 0; i < M; ++i)
            m += W[static_cast<std::size_t>(i) * n + t];
        m /= M;
        double v = 0.0;
        for (int i = 0; i < M; ++i) {
            const double dev = W[static_cast<std::size_t>(i) * n + t] - m;
            v += dev * dev;
        }
        mu[static_cast<std::size_t>(t)] = m;
        sd[static_cast<std::size_t>(t)] = std::sqrt(v / (M - 1));
    }

    std::vector<double> sum(static_cast<std::size_t>(nbins), 0.0);
    std::vector<double> cnt(static_cast<std::size_t>(nbins), 0.0);
    std::vector<double> dsum(static_cast<std::size_t>(nbins), 0.0);
    uint64_t rng = 20260720u;
    for (int p = 0; p < npairs; ++p) {
        const int t = static_cast<int>(splitmix64(rng) % static_cast<uint64_t>(n));
        const int s = static_cast<int>(splitmix64(rng) % static_cast<uint64_t>(n));
        const double ddx = x[static_cast<std::size_t>(t)] - x[static_cast<std::size_t>(s)];
        const double ddy = y[static_cast<std::size_t>(t)] - y[static_cast<std::size_t>(s)];
        const double d = std::sqrt(ddx * ddx + ddy * ddy);
        if (d >= dmax) continue;
        if (sd[static_cast<std::size_t>(t)] < 1e-12 ||
            sd[static_cast<std::size_t>(s)] < 1e-12) continue;
        double c = 0.0;
        for (int i = 0; i < M; ++i)
            c += (W[static_cast<std::size_t>(i) * n + t] - mu[static_cast<std::size_t>(t)]) *
                 (W[static_cast<std::size_t>(i) * n + s] - mu[static_cast<std::size_t>(s)]);
        c /= (M - 1) * sd[static_cast<std::size_t>(t)] * sd[static_cast<std::size_t>(s)];
        const int b = static_cast<int>(d / dmax * nbins);
        sum[static_cast<std::size_t>(b)]  += c;
        cnt[static_cast<std::size_t>(b)]  += 1.0;
        dsum[static_cast<std::size_t>(b)] += d;
    }

    CorrBins out;
    for (int b = 0; b < nbins; ++b) {
        if (cnt[static_cast<std::size_t>(b)] < 50.0) continue;
        out.d.push_back(dsum[static_cast<std::size_t>(b)] / cnt[static_cast<std::size_t>(b)]);
        out.rho.push_back(sum[static_cast<std::size_t>(b)] / cnt[static_cast<std::size_t>(b)]);
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Comonotone limit
// ---------------------------------------------------------------------------

TEST(SpdeSpatialBasis, ComonotoneLimitExact) {
    std::vector<double> x, y;
    makeGrid(20, 100.0 / 19.0, x, y);  // 100 m domain
    const int n = static_cast<int>(x.size());

    SpdeSpatialBasis basis;
    basis.build(x.data(), y.data(), n, 1.0e6);  // ℓ ≫ domain

    ASSERT_EQ(basis.n_modes(), 1);
    EXPECT_EQ(basis.modeP()[0], 0);
    EXPECT_EQ(basis.modeQ()[0], 0);
    EXPECT_DOUBLE_EQ(basis.modeWeights()[0], 1.0);
    for (int t = 0; t < n; ++t)
        ASSERT_DOUBLE_EQ(basis.modeValues()[static_cast<std::size_t>(t)], 1.0);

    const int M = 30;
    const std::vector<double> c = familyCoeff(M, DistType::NORMAL, 77);
    std::vector<double> a, W;
    basis.sampleCoefficients(c, DistType::NORMAL, 42, a);
    basis.materializeField(a, M, W);

    // W_i(t) == c_i bit-exactly: w_0 = 1.0 and φ_0 ≡ 1.0.
    for (int i = 0; i < M; ++i)
        for (int t = 0; t < n; ++t)
            ASSERT_DOUBLE_EQ(W[static_cast<std::size_t>(i) * n + t],
                             c[static_cast<std::size_t>(i)]);
}

TEST(SpdeSpatialBasis, DegenerateGeometryIsComonotone) {
    // All points coincident: no spatial structure, DC-only basis.
    std::vector<double> x(5, 3.0), y(5, 4.0);
    SpdeSpatialBasis basis;
    basis.build(x.data(), y.data(), 5, 50.0);
    EXPECT_EQ(basis.n_modes(), 1);
    EXPECT_DOUBLE_EQ(basis.capturedVarianceFraction(), 1.0);
    EXPECT_DOUBLE_EQ(basis.modeWeights()[0], 1.0);
}

// ---------------------------------------------------------------------------
// Basis structure
// ---------------------------------------------------------------------------

TEST(SpdeSpatialBasis, Mode0IsConstantAndWeightsNormalized) {
    std::vector<double> x, y;
    makeGrid(20, 400.0 / 19.0, x, y);  // 400 m domain
    const int n = static_cast<int>(x.size());

    SpdeSpatialBasis basis;
    basis.build(x.data(), y.data(), n, 100.0);

    ASSERT_GT(basis.n_modes(), 1);       // finite ℓ ⇒ genuinely spatial basis
    EXPECT_EQ(basis.modeP()[0], 0);      // slot 0 pinned to the constant mode
    EXPECT_EQ(basis.modeQ()[0], 0);
    for (int t = 0; t < n; ++t)
        ASSERT_DOUBLE_EQ(basis.modeValues()[static_cast<std::size_t>(t)], 1.0);

    double sum_w2 = 0.0;
    for (double w : basis.modeWeights()) {
        EXPECT_GT(w, 0.0);
        sum_w2 += w * w;
    }
    EXPECT_NEAR(sum_w2, 1.0, 1e-12);

    EXPECT_GT(basis.capturedVarianceFraction(), 0.0);
    EXPECT_LE(basis.capturedVarianceFraction(), 1.0);
}

TEST(SpdeSpatialBasis, DeterministicAcrossCalls) {
    std::vector<double> x, y;
    makeGrid(15, 20.0, x, y);
    const int n = static_cast<int>(x.size());

    SpdeSpatialBasis b1, b2;
    b1.build(x.data(), y.data(), n, 80.0);
    b2.build(x.data(), y.data(), n, 80.0);
    ASSERT_EQ(b1.n_modes(), b2.n_modes());
    EXPECT_EQ(b1.modeValues(), b2.modeValues());
    EXPECT_EQ(b1.modeWeights(), b2.modeWeights());

    const std::vector<double> c = familyCoeff(24, DistType::LOGNORMAL, 5);
    std::vector<double> a1, a2;
    b1.sampleCoefficients(c, DistType::LOGNORMAL, 99, a1);
    b2.sampleCoefficients(c, DistType::LOGNORMAL, 99, a2);
    EXPECT_EQ(a1, a2);
}

// ---------------------------------------------------------------------------
// Deviation-form invariant: per-cell column mean equals mean_i(c_i)
// ---------------------------------------------------------------------------

TEST(SpdeSpatialBasis, ColumnMeanPreservesCbar) {
    std::vector<double> x, y;
    makeGrid(16, 25.0, x, y);  // 375 m domain
    const int n = static_cast<int>(x.size());

    SpdeSpatialBasis basis;
    basis.build(x.data(), y.data(), n, 120.0);
    ASSERT_GT(basis.n_modes(), 1);

    const int M = 40;
    const std::vector<double> c = familyCoeff(M, DistType::NORMAL, 3);
    double cbar = 0.0;
    for (double ci : c) cbar += ci;
    cbar /= M;

    std::vector<double> a, W;
    basis.sampleCoefficients(c, DistType::NORMAL, 11, a);
    basis.materializeField(a, M, W);

    // Threshold matches the 2D ROM's debug assert on supplied soft fields.
    for (int t = 0; t < n; ++t) {
        double colmean = 0.0;
        for (int i = 0; i < M; ++i)
            colmean += W[static_cast<std::size_t>(i) * n + t];
        colmean /= M;
        ASSERT_NEAR(colmean, cbar, 1e-9);
    }
}

// ---------------------------------------------------------------------------
// Family-aware variance matching
// ---------------------------------------------------------------------------

namespace {

// Domain-averaged per-cell member variance of a materialized field.
double meanCellVariance(const std::vector<double>& W, int M, int n) {
    double acc = 0.0;
    for (int t = 0; t < n; ++t) {
        double m = 0.0;
        for (int i = 0; i < M; ++i) m += W[static_cast<std::size_t>(i) * n + t];
        m /= M;
        double v = 0.0;
        for (int i = 0; i < M; ++i) {
            const double d = W[static_cast<std::size_t>(i) * n + t] - m;
            v += d * d;
        }
        acc += v / (M - 1);
    }
    return acc / n;
}

double sampleVariance(const std::vector<double>& c) {
    double m = 0.0;
    for (double v : c) m += v;
    m /= static_cast<double>(c.size());
    double s = 0.0;
    for (double v : c) s += (v - m) * (v - m);
    return s / (static_cast<double>(c.size()) - 1.0);
}

} // namespace

TEST(SpdeSpatialBasis, FamilyVarianceMatched) {
    std::vector<double> x, y;
    makeGrid(24, 16.0, x, y);  // 368 m domain
    const int n = static_cast<int>(x.size());
    const int M = 200;

    SpdeSpatialBasis basis;
    basis.build(x.data(), y.data(), n, 110.0);
    ASSERT_GT(basis.n_modes(), 1);

    for (DistType family : {DistType::NORMAL, DistType::UNIFORM}) {
        const std::vector<double> c = familyCoeff(M, family, 21);
        std::vector<double> a, W;
        basis.sampleCoefficients(c, family, 63, a);
        basis.materializeField(a, M, W);

        // Per-cell variance must equal the comonotone coefficient variance
        // (≈ 1 for NORMAL, ≈ 1/3 for UNIFORM) — the local band width is the
        // same as the FULL-coherence band. materializeField's per-point
        // normalization (design note §4) makes this exact up to the empirical
        // variance estimator; a small tolerance guards round-off only.
        const double ratio = meanCellVariance(W, M, n) / sampleVariance(c);
        EXPECT_GT(ratio, 0.97) << "family " << static_cast<int>(family);
        EXPECT_LT(ratio, 1.03) << "family " << static_cast<int>(family);
    }
}

// ---------------------------------------------------------------------------
// Covariance model reproduction (the headline CL-2b test)
// ---------------------------------------------------------------------------

TEST(SpdeSpatialBasis, EmpiricalCovarianceMatchesMatern) {
    // Domain/ℓ ratio ≈ 9.4. At smaller ratios the finite Neumann box makes the
    // realized field decay faster than the infinite-domain Matérn ν=2 ideal
    // (a boundary effect the CL-1 smoother shares); the SPDE field converges
    // to the model as the domain grows relative to ℓ (measured RMS: ratio 6.3
    // → 0.066, 9.4 → 0.042, 12.5 → 0.037).
    const int    G   = 48;
    const double dx  = 12.0;   // 564 m domain
    const double ell = 60.0;
    const int    M   = 200;

    std::vector<double> x, y;
    makeGrid(G, dx, x, y);
    const int n = G * G;

    SpdeSpatialBasis basis;
    basis.build(x.data(), y.data(), n, ell);
    ASSERT_GE(basis.n_modes(), 10);
    ASSERT_GE(basis.capturedVarianceFraction(), 0.90);

    const std::vector<double> c = familyCoeff(M, DistType::NORMAL, 8);
    std::vector<double> a, W;
    basis.sampleCoefficients(c, DistType::NORMAL, 17, a);
    basis.materializeField(a, M, W);

    const CorrBins bins =
        binnedCorrelation(W, M, n, x, y, 3.0 * ell, 24, 300000);
    ASSERT_GE(bins.d.size(), 10u);

    double rms = 0.0, maxerr = 0.0;
    for (std::size_t b = 0; b < bins.d.size(); ++b) {
        const double model = SpdeSpatialBasis::modelCorrelation(bins.d[b], ell);
        const double err   = bins.rho[b] - model;
        rms += err * err;
        maxerr = std::max(maxerr, std::abs(err));
    }
    rms = std::sqrt(rms / static_cast<double>(bins.d.size()));

    // Bounds set at the measured actuals + margin; failures here mean the
    // spectral weights or the mode normalization changed behaviour — escalate,
    // do not widen (checklist escalation rule).
    EXPECT_LT(rms, 0.06)    << "binned-correlation RMS deviation from Matérn ν=2";
    EXPECT_LT(maxerr, 0.12) << "worst-bin deviation from Matérn ν=2";

    // And the model must NOT be confused with the exponential kernel itself:
    // exp(−d/ℓ) misfits the realized field badly (design note §2).
    double rms_exp = 0.0;
    for (std::size_t b = 0; b < bins.d.size(); ++b) {
        const double err = bins.rho[b] - std::exp(-bins.d[b] / ell);
        rms_exp += err * err;
    }
    rms_exp = std::sqrt(rms_exp / static_cast<double>(bins.d.size()));
    EXPECT_GT(rms_exp, 3.0 * rms);
}

#ifdef OPENSWMM_HAS_2D
// The CL-1 generator's field must match the SAME model — this locks the CL-2b
// design claim (kernel smoothing ⇒ Matérn ν=2 covariance) and guards CL-1/CL-2
// consistency against future changes to either path.
TEST(SpdeSpatialBasis, CL1SoftFieldMatchesSameModel) {
    const int    G   = 24;
    const double dx  = 13.0;   // 299 m domain
    const double ell = 78.0;
    const int    M   = 100;

    std::vector<double> x, y;
    makeGrid(G, dx, x, y);
    const int n = G * G;

    const std::vector<double> c = familyCoeff(M, DistType::NORMAL, 8);
    openswmm::twoD::SpatialUncertaintyField fld;
    openswmm::twoD::CorrelatedFieldGenerator::generateCoefficientField(
        x.data(), y.data(), n, c, ell, 4242u, fld);

    const CorrBins bins =
        binnedCorrelation(fld.values, M, n, x, y, 3.0 * ell, 20, 200000);
    ASSERT_GE(bins.d.size(), 8u);

    double rms = 0.0;
    for (std::size_t b = 0; b < bins.d.size(); ++b) {
        const double err =
            bins.rho[b] - SpdeSpatialBasis::modelCorrelation(bins.d[b], ell);
        rms += err * err;
    }
    rms = std::sqrt(rms / static_cast<double>(bins.d.size()));

    // Scratch-bench measured 0.027 on a 48×48/M=200 variant; smaller M here
    // adds sampling noise. Escalate rather than widen on failure.
    EXPECT_LT(rms, 0.08);
}
#endif // OPENSWMM_HAS_2D

// ---------------------------------------------------------------------------
// Cost: this is the CL-2a "64-second field generation" replacement
// ---------------------------------------------------------------------------

TEST(SpdeSpatialBasis, BuildAndMaterializeFastOnLargePointSet) {
    // CL-2a profile geometry: 1 km domain, ℓ = 200 m, M = 50, ~10k points.
    const int    G   = 100;
    const double dx  = 10.0;
    const double ell = 200.0;
    const int    M   = 50;

    std::vector<double> x, y;
    makeGrid(G, dx, x, y);
    const int n = G * G;

    const auto t0 = std::chrono::steady_clock::now();
    SpdeSpatialBasis basis;
    basis.build(x.data(), y.data(), n, ell);
    const std::vector<double> c = familyCoeff(M, DistType::NORMAL, 8);
    std::vector<double> a, W;
    basis.sampleCoefficients(c, DistType::NORMAL, 17, a);
    basis.materializeField(a, M, W);
    const auto t1 = std::chrono::steady_clock::now();

    const double ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("[CL-2b] build+sample+materialize: %.1f ms, K_s = %d, "
                "captured = %.3f  (CL-1 generation at this scale: 64,159 ms)\n",
                ms, basis.n_modes(), basis.capturedVarianceFraction());

    EXPECT_EQ(static_cast<int>(W.size()), M * n);
    EXPECT_LT(basis.n_modes(), M);   // reduced: K_s < M on this geometry
    // Generous wall-time bound (measured ~tens of ms; CI machines vary).
    EXPECT_LT(ms, 5000.0);
}

// ---------------------------------------------------------------------------
// Argument validation
// ---------------------------------------------------------------------------

TEST(SpdeSpatialBasis, ThrowsOnInvalidArgs) {
    std::vector<double> x, y;
    makeGrid(4, 10.0, x, y);
    SpdeSpatialBasis basis;

    EXPECT_THROW(basis.build(x.data(), y.data(), 16, 0.0), std::invalid_argument);
    EXPECT_THROW(basis.build(x.data(), y.data(), 16, -5.0), std::invalid_argument);
    EXPECT_THROW(basis.build(x.data(), y.data(), 0, 10.0), std::invalid_argument);
    EXPECT_THROW(basis.build(nullptr, y.data(), 16, 10.0), std::invalid_argument);

    std::vector<double> a, W;
    const std::vector<double> c2 = familyCoeff(8, DistType::NORMAL, 1);
    EXPECT_THROW(basis.sampleCoefficients(c2, DistType::NORMAL, 1, a),
                 std::logic_error);
    EXPECT_THROW(basis.materializeField(a, 8, W), std::logic_error);

    basis.build(x.data(), y.data(), 16, 10.0);
    const std::vector<double> c1 = {0.5};
    EXPECT_THROW(basis.sampleCoefficients(c1, DistType::NORMAL, 1, a),
                 std::invalid_argument);
    basis.sampleCoefficients(c2, DistType::NORMAL, 1, a);
    EXPECT_THROW(basis.materializeField(a, 7, W), std::invalid_argument);
}

// ===========================================================================
// CL-2c — reduced-projection equivalence (normalizedModes / the g(t) seam)
// ===========================================================================

// Synthetic reduced modal basis P (k × n row-major) — deterministic cosine
// columns; any fixed P works, the reduced/materialized identity is basis-free.
static std::vector<double> makeProjP(int k, int n) {
    constexpr double kPi = 3.14159265358979323846;
    std::vector<double> P(static_cast<std::size_t>(k) * n);
    for (int j = 0; j < k; ++j)
        for (int t = 0; t < n; ++t)
            P[static_cast<std::size_t>(j) * n + t] =
                std::cos((j + 1) * kPi * (t + 0.5) / n);
    return P;
}

// The reduced projection R_{ij} = Σ_m a_im·(Σ_t P_j[t]·spread[t]·ψ_m[t]) must
// equal the direct projection over the materialized field
// R_{ij} = Σ_t P_j[t]·spread[t]·W_i[t] — the two differ only in summation
// order because ψ_m already folds in the per-point normalization g(t) that
// materializeField applies. This is the "CL-2c seam" (design note §4).
TEST(SpdeSpatialBasisReduced, ReducedProjectionMatchesMaterializedField) {
    std::vector<double> x, y;
    makeGrid(12, 15.0, x, y);          // 144 points, 165 m domain
    const int n = static_cast<int>(x.size());
    const int M = 40;
    const double ell = 45.0;           // moderate ℓ → K_s > 1 and < M

    SpdeSpatialBasis basis;
    basis.build(x.data(), y.data(), n, ell);
    ASSERT_GT(basis.n_modes(), 1);     // genuinely reduced (not the DC-only case)
    ASSERT_LT(basis.n_modes(), M);

    std::vector<double> a;
    basis.sampleCoefficients(familyCoeff(M, DistType::NORMAL, 7),
                             DistType::NORMAL, 7, a);
    const int Ks = basis.n_modes();

    // Materialized field W (M × n) and normalized modes ψ (K_s × n).
    std::vector<double> W, psi;
    basis.materializeField(a, M, W);
    basis.normalizedModes(a, M, psi);

    // A non-trivial spread plane and a synthetic k-mode projection basis.
    const int k = 6;
    const std::vector<double> P = makeProjP(k, n);
    std::vector<double> spread(static_cast<std::size_t>(n));
    for (int t = 0; t < n; ++t) spread[static_cast<std::size_t>(t)] = 0.5 + 0.5 * (t % 7);

    // Direct (materialized): R_mat[i][j] = Σ_t P_j[t]·spread[t]·W_i[t].
    std::vector<double> R_mat(static_cast<std::size_t>(M) * k, 0.0);
    for (int i = 0; i < M; ++i)
        for (int j = 0; j < k; ++j) {
            double s = 0.0;
            for (int t = 0; t < n; ++t)
                s += P[static_cast<std::size_t>(j) * n + t] *
                     spread[static_cast<std::size_t>(t)] *
                     W[static_cast<std::size_t>(i) * n + t];
            R_mat[static_cast<std::size_t>(i) * k + j] = s;
        }

    // Reduced: R_m[j] = Σ_t P_j[t]·spread[t]·ψ_m[t]; R_red[i][j] = Σ_m a_im·R_m[j].
    std::vector<double> Rm(static_cast<std::size_t>(Ks) * k, 0.0);
    for (int m = 0; m < Ks; ++m)
        for (int j = 0; j < k; ++j) {
            double s = 0.0;
            for (int t = 0; t < n; ++t)
                s += P[static_cast<std::size_t>(j) * n + t] *
                     spread[static_cast<std::size_t>(t)] *
                     psi[static_cast<std::size_t>(m) * n + t];
            Rm[static_cast<std::size_t>(m) * k + j] = s;
        }
    std::vector<double> R_red(static_cast<std::size_t>(M) * k, 0.0);
    for (int i = 0; i < M; ++i)
        for (int j = 0; j < k; ++j) {
            double s = 0.0;
            for (int m = 0; m < Ks; ++m)
                s += a[static_cast<std::size_t>(i) * Ks + m] *
                     Rm[static_cast<std::size_t>(m) * k + j];
            R_red[static_cast<std::size_t>(i) * k + j] = s;
        }

    double worst = 0.0, scale = 0.0;
    for (std::size_t idx = 0; idx < R_mat.size(); ++idx) {
        worst = std::max(worst, std::abs(R_mat[idx] - R_red[idx]));
        scale = std::max(scale, std::abs(R_mat[idx]));
    }
    // Pure summation-order difference → machine precision (relative).
    EXPECT_LT(worst, 1.0e-9 * (scale + 1.0))
        << "reduced projection must equal materialized projection (seam fold); "
        << "worst=" << worst << " scale=" << scale;
}

// Comonotone limit: ℓ ≫ domain ⇒ K_s = 1, g ≡ 1, ψ_0 ≡ 1, so the reduced
// projection reproduces the scalar c_i·(Pᵀspread)_j exactly.
TEST(SpdeSpatialBasisReduced, ComonotoneLimitExactReduced) {
    std::vector<double> x, y;
    makeGrid(8, 10.0, x, y);
    const int n = static_cast<int>(x.size());
    const int M = 24;
    const double ell = 1.0e6;          // ℓ ≫ domain ⇒ constant mode only

    SpdeSpatialBasis basis;
    basis.build(x.data(), y.data(), n, ell);
    ASSERT_EQ(basis.n_modes(), 1);

    const std::vector<double> c = familyCoeff(M, DistType::NORMAL, 3);
    std::vector<double> a, psi;
    basis.sampleCoefficients(c, DistType::NORMAL, 3, a);
    basis.normalizedModes(a, M, psi);

    // K_s = 1 ⇒ ψ_0(t) ≡ 1 for all t (g ≡ 1, φ_0 ≡ 1).
    for (int t = 0; t < n; ++t)
        EXPECT_NEAR(psi[static_cast<std::size_t>(t)], 1.0, 1.0e-12);

    // a_i0 = w_0·c_i with w_0 = 1 (single mode).
    for (int i = 0; i < M; ++i)
        EXPECT_NEAR(a[static_cast<std::size_t>(i)], c[static_cast<std::size_t>(i)],
                    1.0e-12);
}

