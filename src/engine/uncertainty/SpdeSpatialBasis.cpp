/**
 * @file SpdeSpatialBasis.cpp
 * @see SpdeSpatialBasis.hpp and docs/uncertainty/SPDE_SPATIAL_BASIS.md
 */

#include "SpdeSpatialBasis.hpp"
#include "LhsShuffle.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace openswmm::uncertainty {

namespace {

// Modified Bessel functions K0, K1 (Abramowitz & Stegun 9.8.5–9.8.8 polynomial
// approximations, abs error < 2e-7); K2 by the recurrence K2 = K0 + 2·K1/x.
// Only used by modelCorrelation() — accuracy requirements are those of test
// tolerances, not of the basis itself (the basis uses the spectral density,
// which is elementary).
double besselI0(double x) noexcept {
    const double t = x / 3.75, t2 = t * t;
    return 1.0 + t2 * (3.5156229 + t2 * (3.0899424 + t2 * (1.2067492 +
           t2 * (0.2659732 + t2 * (0.0360768 + t2 * 0.0045813)))));
}
double besselI1(double x) noexcept {
    const double t = x / 3.75, t2 = t * t;
    return x * (0.5 + t2 * (0.87890594 + t2 * (0.51498869 + t2 * (0.15084934 +
           t2 * (0.02658733 + t2 * (0.00301532 + t2 * 0.00032411))))));
}
double besselK0(double x) noexcept {
    if (x <= 2.0) {
        const double t2 = x * x / 4.0;
        return -std::log(x / 2.0) * besselI0(x) - 0.57721566 +
               t2 * (0.42278420 + t2 * (0.23069756 + t2 * (0.03488590 +
               t2 * (0.00262698 + t2 * (0.00010750 + t2 * 0.00000740)))));
    }
    const double t = 2.0 / x;
    return std::exp(-x) / std::sqrt(x) * (1.25331414 + t * (-0.07832358 +
           t * (0.02189568 + t * (-0.01062446 + t * (0.00587872 +
           t * (-0.00251540 + t * 0.00053208))))));
}
double besselK1(double x) noexcept {
    if (x <= 2.0) {
        const double t2 = x * x / 4.0;
        return std::log(x / 2.0) * besselI1(x) + (1.0 / x) * (1.0 +
               t2 * (0.15443144 + t2 * (-0.67278579 + t2 * (-0.18156897 +
               t2 * (-0.01919402 + t2 * (-0.00110404 + t2 * -0.00004686))))));
    }
    const double t = 2.0 / x;
    return std::exp(-x) / std::sqrt(x) * (1.25331414 + t * (0.23498619 +
           t * (-0.03655620 + t * (0.01504268 + t * (-0.00780353 +
           t * (0.00325614 + t * -0.00068245))))));
}

} // namespace

double SpdeSpatialBasis::modelCorrelation(double d, double corr_len) noexcept {
    if (d <= 0.0 || corr_len <= 0.0) return 1.0;
    const double x = d / corr_len;
    // ½ x² K₂(x); underflows harmlessly to 0 for large x.
    if (x > 60.0) return 0.0;
    const double K2 = besselK0(x) + 2.0 * besselK1(x) / x;
    return 0.5 * x * x * K2;
}

void SpdeSpatialBasis::clear() {
    n_modes_  = 0;
    n_points_ = 0;
    captured_ = 0.0;
    phi_.clear();
    w_.clear();
    mode_p_.clear();
    mode_q_.clear();
}

void SpdeSpatialBasis::build(const double* x, const double* y, int n_points,
                             double corr_len,
                             const SpdeSpatialBasisConfig& cfg) {
    if (n_points <= 0)
        throw std::invalid_argument("SpdeSpatialBasis::build: n_points must be > 0");
    if (corr_len <= 0.0)
        throw std::invalid_argument(
            "SpdeSpatialBasis::build: corr_len must be > 0 "
            "(corr_len == 0 is the caller's comonotone scalar path)");
    if (x == nullptr || y == nullptr)
        throw std::invalid_argument("SpdeSpatialBasis::build: null coordinates");

    clear();
    n_points_ = n_points;

    const int    max_modes = std::max(1, cfg.max_modes);
    const double f_target  = std::min(std::max(cfg.target_variance_fraction, 0.0), 1.0);

    // Bounding box of the target points.
    double xmin = x[0], xmax = x[0], ymin = y[0], ymax = y[0];
    for (int t = 1; t < n_points; ++t) {
        xmin = std::min(xmin, x[t]);  xmax = std::max(xmax, x[t]);
        ymin = std::min(ymin, y[t]);  ymax = std::max(ymax, y[t]);
    }
    const double ext = std::max(xmax - xmin, ymax - ymin);

    // Degenerate geometry (all points coincident): no spatial structure —
    // the DC-only basis is exact.
    if (ext <= 0.0) {
        n_modes_  = 1;
        captured_ = 1.0;
        phi_.assign(static_cast<std::size_t>(n_points), 1.0);
        w_.assign(1, 1.0);
        mode_p_.assign(1, 0);
        mode_q_.assign(1, 0);
        return;
    }

    // Padded embedding rectangle (design note §3). The min(ℓ, extent) cap
    // makes the ℓ → ∞ limit collapse onto the constant mode instead of
    // keeping a scale-invariant padded box.
    const double pad = std::max(cfg.padding_factor, 0.5) * std::min(corr_len, ext);
    const double x0 = xmin - pad, Lx = (xmax - xmin) + 2.0 * pad;
    const double y0 = ymin - pad, Ly = (ymax - ymin) + 2.0 * pad;

    const double kappa2 = 1.0 / (corr_len * corr_len);

    // Candidate cutoff: eigenvalues up to Λ_max cover ≥ 99.9 % of the
    // analytic Matérn ν=2 variance (tail fraction (κ²/(κ²+Λ))² ≤ 1e-3).
    const double lam_max = kappa2 * (std::sqrt(1.0e3) - 1.0);
    const double pi = 3.14159265358979323846;
    auto index_cap = [&](double L) {
        int p = static_cast<int>(std::ceil(L * std::sqrt(lam_max) / pi));
        return std::min(std::max(p, 1), 512);
    };
    const int p_max = index_cap(Lx);
    const int q_max = index_cap(Ly);

    // Relative variance of each candidate: v = e_p·e_q·(1 + λ/κ²)^{-3}.
    struct Cand { double v; int p; int q; };
    std::vector<Cand> cand;
    cand.reserve(static_cast<std::size_t>(p_max + 1) *
                 static_cast<std::size_t>(q_max + 1));
    double total_v = 0.0;
    for (int p = 0; p <= p_max; ++p) {
        const double lx = (p * pi / Lx) * (p * pi / Lx);
        const double ep = (p == 0) ? 1.0 : 2.0;
        for (int q = 0; q <= q_max; ++q) {
            const double lam = lx + (q * pi / Ly) * (q * pi / Ly);
            const double eq  = (q == 0) ? 1.0 : 2.0;
            const double r   = 1.0 + lam / kappa2;
            const double v   = ep * eq / (r * r * r);
            cand.push_back({v, p, q});
            total_v += v;
        }
    }

    // Slot 0 is always the constant mode (0,0) — comonotone anchoring
    // (design note §5). Rank the rest by variance, deterministic tie-break.
    std::sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) {
        const bool a_dc = (a.p == 0 && a.q == 0);
        const bool b_dc = (b.p == 0 && b.q == 0);
        if (a_dc != b_dc) return a_dc;
        if (a.v != b.v) return a.v > b.v;
        if (a.p != b.p) return a.p < b.p;
        return a.q < b.q;
    });

    // Retain until the variance target (relative to the candidate total) or
    // the mode cap is hit.
    double kept_v = 0.0;
    int K = 0;
    for (const Cand& c : cand) {
        kept_v += c.v;
        ++K;
        if (K >= max_modes || kept_v >= f_target * total_v) break;
    }

    n_modes_  = K;
    captured_ = kept_v / total_v;
    w_.resize(static_cast<std::size_t>(K));
    mode_p_.resize(static_cast<std::size_t>(K));
    mode_q_.resize(static_cast<std::size_t>(K));
    phi_.resize(static_cast<std::size_t>(K) * static_cast<std::size_t>(n_points));

    // Truncation-compensating normalization: Σ w² = 1 exactly, so the
    // domain-averaged per-cell variance equals the coefficient variance.
    for (int m = 0; m < K; ++m) {
        const Cand& c = cand[static_cast<std::size_t>(m)];
        w_[static_cast<std::size_t>(m)]      = std::sqrt(c.v / kept_v);
        mode_p_[static_cast<std::size_t>(m)] = c.p;
        mode_q_[static_cast<std::size_t>(m)] = c.q;

        const double ep = (c.p == 0) ? 1.0 : 2.0;
        const double eq = (c.q == 0) ? 1.0 : 2.0;
        const double amp = std::sqrt(ep * eq);
        const double fx = c.p * pi / Lx;
        const double fy = c.q * pi / Ly;
        double* row = phi_.data() + static_cast<std::size_t>(m) *
                                    static_cast<std::size_t>(n_points);
        for (int t = 0; t < n_points; ++t)
            row[t] = amp * std::cos(fx * (x[t] - x0)) * std::cos(fy * (y[t] - y0));
    }
}

void SpdeSpatialBasis::sampleCoefficients(const std::vector<double>& mode0_coeff,
                                          DistType family, uint64_t seed,
                                          std::vector<double>& a_out) const {
    if (!is_built())
        throw std::logic_error("SpdeSpatialBasis::sampleCoefficients: build() first");
    const int M = static_cast<int>(mode0_coeff.size());
    if (M < 2)
        throw std::invalid_argument(
            "SpdeSpatialBasis::sampleCoefficients: need >= 2 members");

    const int K = n_modes_;
    a_out.assign(static_cast<std::size_t>(M) * static_cast<std::size_t>(K), 0.0);

    // Mode 0: the caller's comonotone coefficients c_i (exact FULL limit).
    for (int i = 0; i < M; ++i)
        a_out[static_cast<std::size_t>(i) * K] =
            w_[0] * mode0_coeff[static_cast<std::size_t>(i)];

    // Modes m >= 1: independent shuffled-strata LHS, family-aware transform
    // matching the variance of the comonotone coefficients (design note §5).
    for (int m = 1; m < K; ++m) {
        const std::vector<double> u =
            shuffledStrata(M, seed + 4u + static_cast<uint64_t>(m));
        const double wm = w_[static_cast<std::size_t>(m)];
        for (int i = 0; i < M; ++i) {
            const double ui = u[static_cast<std::size_t>(i)];
            const double xi = (family == DistType::UNIFORM)
                              ? (2.0 * ui - 1.0)
                              : probit(ui);
            a_out[static_cast<std::size_t>(i) * K + m] = wm * xi;
        }
    }
}

void SpdeSpatialBasis::materializeField(const std::vector<double>& a,
                                        int n_members,
                                        std::vector<double>& field_out) const {
    if (!is_built())
        throw std::logic_error("SpdeSpatialBasis::materializeField: build() first");
    const int K = n_modes_;
    const int n = n_points_;
    if (n_members <= 0 ||
        a.size() != static_cast<std::size_t>(n_members) * static_cast<std::size_t>(K))
        throw std::invalid_argument(
            "SpdeSpatialBasis::materializeField: a must be n_members x n_modes");

    // Raw modal field D_i(t) = Σ_m a_im·φ_m(t).
    field_out.assign(static_cast<std::size_t>(n_members) *
                     static_cast<std::size_t>(n), 0.0);
    for (int i = 0; i < n_members; ++i) {
        double* Wi = field_out.data() + static_cast<std::size_t>(i) *
                                        static_cast<std::size_t>(n);
        const double* ai = a.data() + static_cast<std::size_t>(i) *
                                      static_cast<std::size_t>(K);
        for (int m = 0; m < K; ++m) {
            const double aim = ai[m];
            if (aim == 0.0) continue;
            const double* pm = phi_.data() + static_cast<std::size_t>(m) *
                                             static_cast<std::size_t>(n);
            for (int t = 0; t < n; ++t)
                Wi[t] += aim * pm[t];
        }
    }

    // Per-point variance normalization (design note §4). The domain-average
    // weight normalization (Σw² = 1) preserves variance averaged over the
    // *padded* box, but the target points occupy its interior where the low
    // cosines are suppressed, so the raw per-cell variance falls short of the
    // comonotone coefficient variance. Rescale each cell so the per-cell
    // member variance equals Var_i(c_i) exactly — matching CL-1's rank map
    // (constant per-cell band) and keeping correlation unchanged (a per-point
    // scalar cancels in the correlation ratio). The comonotone limit stays
    // exact: K_s = 1 ⇒ w_0 = 1 ⇒ rv(t) = Var(c) ⇒ g ≡ 1 ⇒ W_i(t) = c_i.
    if (n_members < 2) return;
    const double invM1 = 1.0 / static_cast<double>(n_members - 1);

    // Target = Var_i(c_i), recovered from mode-0 column a_i0 = w_0·c_i.
    const double w0 = w_[0];
    double m0 = 0.0;
    for (int i = 0; i < n_members; ++i)
        m0 += a[static_cast<std::size_t>(i) * K];
    m0 /= n_members;
    double var0 = 0.0;
    for (int i = 0; i < n_members; ++i) {
        const double d = a[static_cast<std::size_t>(i) * K] - m0;
        var0 += d * d;
    }
    const double target_var = (w0 > 0.0) ? (var0 * invM1) / (w0 * w0) : 0.0;
    if (target_var <= 0.0) return;  // degenerate: zero-spread coefficients.

    for (int t = 0; t < n; ++t) {
        double mean = 0.0;
        for (int i = 0; i < n_members; ++i)
            mean += field_out[static_cast<std::size_t>(i) * n + t];
        mean /= n_members;
        double rv = 0.0;
        for (int i = 0; i < n_members; ++i) {
            const double d = field_out[static_cast<std::size_t>(i) * n + t] - mean;
            rv += d * d;
        }
        rv *= invM1;
        const double g = (rv > 1.0e-300) ? std::sqrt(target_var / rv) : 1.0;
        if (g == 1.0) continue;
        for (int i = 0; i < n_members; ++i)
            field_out[static_cast<std::size_t>(i) * n + t] *= g;
    }
}

} // namespace openswmm::uncertainty
