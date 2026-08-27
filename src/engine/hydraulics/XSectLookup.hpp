// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

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
 * @license  Apache-2.0
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

// ============================================================================
// Bucket LUT for locate() — plan XSECT_LOOKUP_ACCEL §4 item A1.
//
// `locate()` is a bisection over a monotone table: ~log2(n) data-dependent
// branches (~6 on the 51-row transect/built-in tables). The LUT replaces the
// first few of them with one multiply + truncate: the table's VALUE range is
// split into kBuckets equal spans, and for each span we precompute an index
// bracket that provably contains locate()'s answer. A short bisection inside
// that bracket finishes the job.
//
// **Index identity (why this stays bit-exact).** `locate()`'s answer is the
// unique index `max{ j <= jLast : table[j] <= y }`. This routine returns the
// same index for every input — it only narrows the search window — so the
// interpolation arithmetic downstream is byte-for-byte unchanged. The proof
// rests on one property: `bucket_of()` is monotone non-decreasing in y (an
// affine map with a positive scale, then a truncating cast, then a clamp), and
// the table is checked monotone non-decreasing at build time. Then
//
//   lo[b] = max{ j : bucket_of(table[j]) < b }        (0 if the set is empty)
//
// satisfies table[lo[b]] <= y for every y in bucket b, and — because
// `bucket_of(table[j])` is non-decreasing in j, so {j : bucket_of <= b} is a
// prefix — `lo[b+1] + 1` is exactly `min{ j : bucket_of(table[j]) > b }`, whose
// table value is strictly greater than y. That is precisely the bisection
// invariant, on a window of one or two table cells instead of the whole table.
//
// A table that fails the monotonicity check (or is degenerate) leaves `scale`
// at 0, which callers read as "no LUT" and fall back to plain bisection — so a
// pathological table can never change a result, only its speed.
// ============================================================================

/// Precomputed value→index-bracket map for one geometry table.
///
/// POD: trivially copyable into a device kernel alongside the table it indexes
/// (the Kokkos FV path copies XsectTables by value).
struct LocateLut {
    static constexpr int kBuckets = 64;

    double        t0     = 0.0;   ///< table[0] — the low end of the value range
    double        scale  = 0.0;   ///< kBuckets / (table[jLast] - t0); 0 == disabled
    int           j_last = 0;     ///< the `jLast` this map was built for
    unsigned char lo[kBuckets + 1] = {};  ///< lo[b] = max{ j : bucket(table[j]) < b }
};

/// Bucket index for a value — the ONE expression used by both build and query.
inline int lut_bucket(const LocateLut& L, double y) noexcept {
    int b = static_cast<int>((y - L.t0) * L.scale);
    if (b < 0) b = 0;
    else if (b >= LocateLut::kBuckets) b = LocateLut::kBuckets - 1;
    return b;
}

/// Plain bisection — the shipped `locate()` body, kept here so the LUT builder
/// and the fallback path share one definition with XsectEval::locate.
inline int locate_bisect(double y, const double* table, int jLast) noexcept {
    int j1 = 0;
    int j2 = jLast;
    if (y <= table[0])     return 0;
    if (y >= table[jLast]) return jLast;
    while (j2 - j1 > 1) {
        int j = (j1 + j2) >> 1;
        if (y >= table[j]) j1 = j;
        else               j2 = j;
    }
    return j1;
}

/// Build the bucket map for `table` over indices [0, jLast].
///
/// Leaves the map disabled (scale == 0) for a degenerate or non-monotone table,
/// or one too long to index with a byte — the caller then bisects as before.
inline void build_locate_lut(LocateLut& L, const double* table, int jLast) noexcept {
    L = LocateLut{};
    if (!table || jLast < 2 || jLast > 254) return;
    for (int j = 1; j <= jLast; ++j)
        if (!(table[j] >= table[j - 1])) return;   // non-monotone (or NaN) — no LUT

    const double span = table[jLast] - table[0];
    if (!(span > 0.0)) return;                     // flat table — nothing to bracket

    L.t0     = table[0];
    L.scale  = static_cast<double>(LocateLut::kBuckets) / span;
    L.j_last = jLast;
    for (int b = 0; b <= LocateLut::kBuckets; ++b) {
        int best = 0;
        for (int j = 0; j <= jLast; ++j) {
            if (lut_bucket(L, table[j]) >= b) break;   // bucket(table[.]) is sorted
            best = j;
        }
        L.lo[b] = static_cast<unsigned char>(best);
    }
}

/// LUT-accelerated `locate()` — returns the identical index for every input.
inline int locate_lut(double y, const double* table, int jLast,
                      const LocateLut& L) noexcept {
    // `!(y > table[0])` (not `y <= table[0]`) also catches NaN, which plain
    // bisection resolves to 0 because every `y >= table[j]` test is false.
    if (!(y > table[0])) return 0;
    if (y >= table[jLast]) return jLast;

    const int b  = lut_bucket(L, y);
    int       j1 = L.lo[b];
    int       j2 = L.lo[b + 1] + 1;
    if (j2 > jLast) j2 = jLast;
    while (j2 - j1 > 1) {
        int jm = (j1 + j2) >> 1;
        if (y >= table[jm]) j1 = jm;
        else                j2 = jm;
    }
    return j1;
}

/// Dispatch: use the map when one was built for this exact table extent.
inline int locate_maybe_lut(double y, const double* table, int jLast,
                            const LocateLut* L) noexcept {
    if (L && L->scale > 0.0 && L->j_last == jLast)
        return locate_lut(y, table, jLast, *L);
    return locate_bisect(y, table, jLast);
}

/// Build the map for the extent `invLookup(y, table, n_items)` actually
/// searches — including the section-factor tables' two-row top truncation, so
/// the S_* maps bracket the same window `locate()` is called on. (The
/// truncated top rows are resolved by invLookup's own branch, outside
/// `locate`, and are unaffected.)
inline void build_invlookup_lut(LocateLut& L, const double* table, int n_items) noexcept {
    if (!table || n_items < 4) { L = LocateLut{}; return; }
    int n = n_items;
    if (table[n - 3] > table[n - 1]) n = n - 2;
    build_locate_lut(L, table, n - 1);
}

} // namespace openswmm::xsect

#endif // OPENSWMM_XSECT_LOOKUP_HPP
