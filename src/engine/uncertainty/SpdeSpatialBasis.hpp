/**
 * @file SpdeSpatialBasis.hpp
 * @brief Reduced spatial basis for correlated soft-rain coherence (CL-2b).
 *
 * @details Replaces the materialized `M×n` correlated coefficient field of
 *          `CorrelatedFieldGenerator::generateCoefficientField` with a small
 *          deterministic spatial basis:
 *
 *              W_i(t) = Σ_m a_im · φ_m(t),   m = 0..K_s−1,  K_s ≪ n
 *
 *          The basis reproduces the covariance the CL-1 generator actually
 *          realizes — a Whittle–Matérn field with smoothness ν = 2 and
 *          κ = 1/corr_len (the self-convolution of the exponential smoothing
 *          kernel), ρ(d) = ½(κd)²K₂(κd) — NOT exp(−d/ℓ); see
 *          docs/uncertainty/SPDE_SPATIAL_BASIS.md §2 for the derivation and
 *          measured evidence.
 *
 *          Modes are analytic Neumann-Laplacian eigenfunctions (tensor
 *          cosines) on the padded bounding rectangle of the target points,
 *          weighted by the Matérn spectral density evaluated at the exact
 *          eigenvalues — no graph construction, no Lanczos, no eigenvalue
 *          scaling calibration.
 *
 *          Mode 0 is always the constant mode and its per-member coefficient
 *          stream is the ROM's existing comonotone coefficient c_i, so
 *          corr_len → ∞ reproduces `COHERENCE FULL` exactly and the
 *          deviation-form column-mean invariant holds.
 *
 * @ingroup engine_uncertainty
 */

#ifndef OPENSWMM_ENGINE_UNCERTAINTY_SPDE_SPATIAL_BASIS_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_SPDE_SPATIAL_BASIS_HPP

#include "UncertaintyTypes.hpp"

#include <cstdint>
#include <vector>

namespace openswmm::uncertainty {

/// Tunables for SpdeSpatialBasis::build(). Defaults per design note §4.
struct SpdeSpatialBasisConfig {
    /// Fraction of the (candidate-set) field variance the retained modes must
    /// capture. Retained weights are rescaled so per-cell variance is
    /// preserved on average regardless of truncation.
    double target_variance_fraction = 0.95;

    /// Hard cap on the number of retained modes K_s.
    int max_modes = 64;

    /// Bounding-box padding per side, in units of min(corr_len, raw extent).
    /// 3.0 keeps the Neumann boundary-image correlation ≤ ρ(6ℓ) ≈ 0.03.
    double padding_factor = 3.0;
};

/**
 * @brief Analytic SPDE (Whittle–Matérn ν=2) spatial basis over scattered
 *        points, with comonotone-anchored per-member coefficient sampling.
 *
 * Usage:
 *   basis.build(x, y, n, corr_len);
 *   basis.sampleCoefficients(rom.softCoeff(), family, seed, a);
 *   basis.materializeField(a, M, field.values);          // optional
 */
class SpdeSpatialBasis {
public:
    /**
     * @brief Build the basis for `n_points` target locations.
     *
     * @param x,y       Point coordinates (m), length n_points. 2D triangle
     *                  centroids or 1D node coordinates — any scattered set.
     * @param n_points  Number of points (> 0).
     * @param corr_len  Correlation length ℓ (m), > 0. (corr_len == 0 means
     *                  comonotone and is the caller's scalar path, not a basis.)
     * @param cfg       Tunables; see SpdeSpatialBasisConfig.
     * @throws std::invalid_argument on non-positive corr_len or n_points.
     */
    void build(const double* x, const double* y, int n_points,
               double corr_len, const SpdeSpatialBasisConfig& cfg = {});

    bool is_built() const noexcept { return n_modes_ > 0; }
    int  n_modes() const noexcept { return n_modes_; }    ///< K_s
    int  n_points() const noexcept { return n_points_; }

    /// Fraction of candidate-set variance the retained modes represent
    /// (before the compensating rescale). 1.0 when K_s covers everything.
    double capturedVarianceFraction() const noexcept { return captured_; }

    /// Mode values φ_m[t], K_s × n_points row-major. Row 0 is all 1.0.
    const std::vector<double>& modeValues() const noexcept { return phi_; }

    /// Spectral weights w_m (length K_s); Σ w_m² = 1 exactly by construction.
    const std::vector<double>& modeWeights() const noexcept { return w_; }

    /// Cosine index pairs (p, q) of each retained mode; mode 0 is (0, 0).
    const std::vector<int>& modeP() const noexcept { return mode_p_; }
    const std::vector<int>& modeQ() const noexcept { return mode_q_; }

    /**
     * @brief Draw per-member modal coefficients a_im = w_m·ξ_im (M × K_s
     *        row-major).
     *
     * Mode 0 uses the caller's comonotone coefficients: ξ_i0 = mode0_coeff[i]
     * (the ROM's softCoeff() — probit(u_i) for NORMAL/LOGNORMAL, 2u_i−1 for
     * UNIFORM). Modes m ≥ 1 use independent shuffled-strata LHS draws with
     * seed `seed + 4 + m` and the family transform matching mode0_coeff's
     * variance (probit(u) or 2u−1).
     *
     * @param mode0_coeff Per-member comonotone coefficients c_i (size M ≥ 2).
     * @param family      Distribution family (selects the m ≥ 1 transform).
     * @param seed        Base seed for the per-mode LHS shuffles.
     * @param a_out       Resized to M × K_s.
     * @throws std::logic_error if not built; std::invalid_argument if M < 2.
     */
    void sampleCoefficients(const std::vector<double>& mode0_coeff,
                            DistType family, uint64_t seed,
                            std::vector<double>& a_out) const;

    /**
     * @brief Materialize the coefficient field W[i·n + t] = g(t)·Σ_m a_im·φ_m[t].
     *
     * O(M·K_s·n). Each cell is rescaled by a per-point factor g(t) so the
     * per-cell member variance equals the comonotone coefficient variance
     * Var_i(c_i) exactly (matching CL-1's rank map), correcting the interior
     * variance suppression of the domain-average weight normalization; g leaves
     * the covariance shape and the comonotone limit unchanged (design note §4).
     *
     * `field_out` is resized to n_members × n_points row-major — assignable to
     * SoftSpatialField::values / SpatialUncertaintyField::values.
     * @throws std::logic_error if not built; std::invalid_argument on size
     *         mismatch (a.size() != n_members·K_s).
     */
    void materializeField(const std::vector<double>& a, int n_members,
                          std::vector<double>& field_out) const;

    /**
     * @brief Materialize the per-point-normalized mode fields
     *        ψ_m(t) = g(t)·φ_m(t) (K_s × n_points row-major) — the "CL-2c seam".
     *
     * The reduced-projection path (CL-2c) computes the per-member per-mode
     * forcing as `R_{ij} = Σ_m a_im · Σ_t P_j[t]·spread[t]·ψ_m(t)`. For this to
     * reproduce the materialized field's variance-correct band, the per-point
     * normalization `g(t)` (design note §4) must be folded into the mode fields
     * *before* projection — a raw `φ_m` projection is only correct up to the
     * boundary-driven variance non-stationarity `g` removes.
     *
     * `g(t) = √(Var_i(c_i) / rawvar(t))` with `rawvar(t)` the empirical per-cell
     * member variance of `D_i(t) = Σ_m a_im·φ_m(t)` — computed identically to
     * materializeField(), so a reduced projection over `ψ_m` matches a direct
     * projection over the materialized field to floating-point round-off.
     *
     * @param a          Per-member modal coefficients (n_members × K_s), from
     *                   sampleCoefficients().
     * @param n_members  Number of members M (≥ 2; for M < 2, g ≡ 1 and ψ = φ).
     * @param psi_out    Resized to K_s × n_points row-major.
     * @throws std::logic_error if not built; std::invalid_argument on size
     *         mismatch (a.size() != n_members·K_s).
     */
    void normalizedModes(const std::vector<double>& a, int n_members,
                         std::vector<double>& psi_out) const;

    /**
     * @brief The covariance model the basis targets: Matérn ν=2 correlation
     *        ρ(d) = ½(κd)²K₂(κd), κ = 1/corr_len. ρ(0) = 1.
     */
    static double modelCorrelation(double d, double corr_len) noexcept;

    /// Reset to the unbuilt state.
    void clear();

private:
    int n_modes_  = 0;
    int n_points_ = 0;
    double captured_ = 0.0;
    std::vector<double> phi_;     ///< K_s × n_points row-major.
    std::vector<double> w_;      ///< Length K_s.
    std::vector<int> mode_p_;    ///< Length K_s.
    std::vector<int> mode_q_;    ///< Length K_s.
};

} // namespace openswmm::uncertainty

#endif // OPENSWMM_ENGINE_UNCERTAINTY_SPDE_SPATIAL_BASIS_HPP
