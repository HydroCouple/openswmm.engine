/**
 * @file XSectLookup.hpp
 * @brief Bit-exact geometry-table interpolation — the single source of truth.
 *
 * @details `lookup_exact()` is an op-for-op transliteration of legacy
 *          src/legacy/engine/xsect.c:lookup(). It is shared by BOTH the
 *          per-element accessor (`xsect::lookup`) and the batch geometry kernels
 *          (`XSectBatch.cpp`) so the two paths can never drift. The circular /
 *          force-main hot path and every shared-table shape (egg, horseshoe,
 *          arch, ellipse, baskethandle, …) interpolate through this one routine.
 *
 *          **Bit-exactness contract (see docs/plans/xsect_bitexact_vectorization.md):**
 *            - Use IEEE division (`x / delta`), never a precomputed reciprocal
 *              (`x * inv_delta`): `a*(1/b) != a/b` in double.
 *            - Keep legacy's exact operation grouping.
 *            - Never emit a fused multiply-add (FMA fuses `mul`+`add` and
 *              silently diverges NEON from x86). The global `-ffp-contract=off`
 *              / `/fp:precise` presets stop the compiler from contracting; do
 *              not reintroduce `std::fma`/intrinsic FMA in callers.
 *          IEEE division is correctly rounded and identical on every platform,
 *          so this routine is bit-identical on Windows/MSVC, Linux, and
 *          macOS(arm64/x86).
 *
 *          **Preconditions (mirror legacy, whose callers guarantee them):**
 *          `x` must be finite and in [0, 1] on entry (values > 1 are tolerated
 *          via the `i >= n-1` early-out, matching legacy). `static_cast<int>` of
 *          a non-finite double is undefined behavior, so callers that can
 *          produce non-finite / negative `x` — e.g. a conduit normalized by a
 *          zero full-depth — MUST guard before calling (see `xsect::lookup` and
 *          the batch kernels).
 *
 * @note INTERNAL HEADER — not installed.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_XSECT_LOOKUP_HPP
#define OPENSWMM_XSECT_LOOKUP_HPP

namespace openswmm::xsect {

/**
 * @brief Bit-exact transliteration of legacy xsect.c:lookup().
 *
 * @param x  Normalized independent variable (depth/y_full or area/a_full),
 *           finite and in [0, 1] (see file-level preconditions).
 * @param t  Geometry table (n equally spaced entries on [0, 1]).
 * @param n  Logical entry count (== legacy nItems).
 * @return   Interpolated dependent value; ULP==0 vs legacy `lookup()`.
 */
inline double lookup_exact(double x, const double* t, int n) noexcept {
    const double delta = 1.0 / static_cast<double>(n - 1);   // same rounding as legacy
    int i = static_cast<int>(x / delta);                     // DIVIDE (legacy), not x*inv_delta
    if (i >= n - 1) return t[n - 1];

    double x0 = i * delta;
    double y  = t[i] + (x - x0) * (t[i + 1] - t[i]) / delta; // grouping matches legacy

    if (i < 2) {                                             // quadratic refinement for low x
        double x1 = (static_cast<double>(i) + 1.0) * delta;
        double y2 = y + (x - x0) * (x - x1) / (delta * delta) *
                        (t[i] / 2.0 - t[i + 1] + t[i + 2] / 2.0);
        if (y2 > 0.0) y = y2;
    }
    if (y < 0.0) y = 0.0;
    return y;
}

// ============================================================================
// Fast mode (opt-in, NOT bit-exact) — plan §6.
//
// Enabled per-build with -DSWMM_XSECT_FAST_LOOKUP. Substitutes the baseline
// reciprocal-multiply form: normalize by a precomputed 1/y_full and index /
// interpolate with `* inv_delta` (inv_delta = N-1 is an EXACT integer) instead
// of `/ delta`. This removes ~3 IEEE divisions per element (measured ~25% faster
// in the fused circular kernel on arm64) at the cost of ULP-level deviation from
// legacy (measured well inside the abs 1e-8 / rel 1e-7 tolerance gate — see
// tests/unit/engine/test_xsect_parity.cpp XSectFastMode). Default OFF: the
// bit-exact `lookup_exact` path above is the shipped default.
//
// `lookup_fast` is always compiled (so the tolerance test can assert its bound
// regardless of the active default), but is only *wired into the kernels* when
// SWMM_XSECT_FAST_LOOKUP is defined.
// ============================================================================

/** @brief Reciprocal-multiply lookup (fast, NOT bit-exact — see §6). */
inline double lookup_fast(double x, const double* t, int n) noexcept {
    const double inv_delta = static_cast<double>(n - 1);   // exact integer
    const double delta     = 1.0 / inv_delta;
    if (x < 0.0) x = 0.0; else if (x > 1.0) x = 1.0;       // fast mode pre-clamps
    int i = static_cast<int>(x * inv_delta);
    if (i >= n - 1) return t[n - 1];
    double x0 = i * delta;
    double y  = t[i] + (x - x0) * (t[i + 1] - t[i]) * inv_delta;
    if (i < 2) {
        double x1 = (static_cast<double>(i) + 1.0) * delta;
        double y2 = y + (x - x0) * (x - x1) * (inv_delta * inv_delta) *
                        (t[i] / 2.0 - t[i + 1] + t[i + 2] / 2.0);
        if (y2 > 0.0) y = y2;
    }
    if (y < 0.0) y = 0.0;
    return y;
}

// Mode-aware normalization. In the default (bit-exact) build `param` is y_full
// and x = y/yFull via IEEE divide; under SWMM_XSECT_FAST_LOOKUP `param` is the
// precomputed inv_y_full and x = depth*inv (clamped). The kernels feed the
// matching per-link array (see XSectBatch.cpp norm_param()).
inline double norm_x(double depth, double param) noexcept {
#ifdef SWMM_XSECT_FAST_LOOKUP
    double x = depth * param;                              // param == inv_y_full
    if (x < 0.0) x = 0.0; else if (x > 1.0) x = 1.0;
    return x;
#else
    return (param > 0.0 && depth > 0.0) ? (depth / param) : 0.0;  // param == y_full
#endif
}

/** @brief Mode-aware normalize + table lookup (bit-exact by default; §6 fast if opted in). */
inline double norm_lookup(double depth, double param, const double* t, int n) noexcept {
#ifdef SWMM_XSECT_FAST_LOOKUP
    return lookup_fast(norm_x(depth, param), t, n);
#else
    return lookup_exact(norm_x(depth, param), t, n);
#endif
}

} // namespace openswmm::xsect

#endif // OPENSWMM_XSECT_LOOKUP_HPP
