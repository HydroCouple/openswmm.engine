/**
 * @file XSectBatch.cpp
 * @brief Data-oriented batch cross-section geometry — shape-grouped SoA.
 *
 * @details Shape-specific kernels are written as tight loops over contiguous
 *          arrays with no branching — the compiler can auto-vectorise them
 *          (and we can add explicit SIMD intrinsics later for hot paths).
 *
 *          The XSectGroups::computeXxx() methods iterate over shape groups,
 *          call the appropriate kernel, then scatter results back to the
 *          global link arrays via the link_idx mapping.
 *
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "XSectBatch.hpp"
#include "xsect_tables.hpp"
#include "XSectLookup.hpp"
#include "../core/SimulationContext.hpp"
#include "../math/SIMD.hpp"

// Belt-and-suspenders against FMA contraction fusing a `mul`+`add` into a single
// rounding (which would silently diverge NEON from x86). The CMake presets
// already pass -ffp-contract=off / /fp:precise globally; this pragma guards the
// bit-exact geometry kernels even if a future preset drops that flag. See
// docs/plans/xsect_bitexact_vectorization.md §3 and XSectLookup.hpp.
#if defined(__clang__) || defined(__GNUC__)
#  pragma STDC FP_CONTRACT OFF
#endif

#include <cmath>
#include <algorithm>
#include <numeric>

#if defined(SWMM_USE_OPENMP)
#include <omp.h>
#endif

namespace openswmm {

// Parallelising the outer shape-group loop in each compute* method pays off
// now that SWMMEngine::start() pins KMP_BLOCKTIME≈infinite so fork cost is
// ~500 ns (not ~20 µs). Groups write to disjoint conduit indices via
// scatter_results, so they are race-free. `schedule(dynamic, 1)` keeps the
// one big shape group (usually CIRCULAR, ~70 % of conduits) from blocking
// workers. We gate the parallel region on having multiple non-empty
// groups — single-group networks (rare) skip the fork entirely.


// ============================================================================
// ShapeGroup
// ============================================================================

void ShapeGroup::resize(int n) {
    count = n;
    auto un = static_cast<std::size_t>(n);
    link_idx.resize(un);
    y_full.resize(un);
    inv_y_full.resize(un);
    a_full.resize(un);
    r_full.resize(un);
    s_full.resize(un);
    w_max.resize(un);
    y_bot.resize(un);
    a_bot.resize(un);
    s_bot.resize(un);
    r_bot.resize(un);
    // Pre-allocate working buffers for hot loop
    buf_d.resize(un);
    buf_r.resize(un);
    buf_r2.resize(un);
}

// ============================================================================
// XSectGroups::build (from XSectParams array)
// ============================================================================

void XSectGroups::build(const XSectParams* params, int n_links) {
    groups_.clear();
    // Invalidate the bypass-mask packed mirrors; they are lazily re-sized on
    // the next setBypassMask() call against the new group layout.
    packed_groups_.clear();
    packed_count_.clear();
    mask_active_ = false;

    // Count links per shape (skip DUMMY=0: non-conduit links that need no geometry)
    constexpr int MAX_SHAPES = 26;
    int shape_count[MAX_SHAPES] = {};
    for (int i = 0; i < n_links; ++i) {
        int t = params[i].type;
        if (t > 0 && t < MAX_SHAPES) shape_count[t]++;
    }

    // Create groups for non-empty shapes (DUMMY excluded)
    int cursor[MAX_SHAPES] = {};
    int group_map[MAX_SHAPES];
    for (int s = 0; s < MAX_SHAPES; ++s) group_map[s] = -1;

    for (int s = 1; s < MAX_SHAPES; ++s) {  // start at 1, skip DUMMY
        if (shape_count[s] == 0) continue;
        group_map[s] = static_cast<int>(groups_.size());
        groups_.emplace_back();
        auto& g = groups_.back();
        g.shape = static_cast<XSectShape>(s);
        g.resize(shape_count[s]);
    }

    // Fill groups (skip DUMMY links)
    for (int i = 0; i < n_links; ++i) {
        int t = params[i].type;
        if (t <= 0 || t >= MAX_SHAPES) continue;
        int gi = group_map[t];
        if (gi < 0) continue;
        auto& g = groups_[static_cast<std::size_t>(gi)];
        int c = cursor[t]++;
        auto uc = static_cast<std::size_t>(c);
        g.link_idx[uc] = i;
        g.y_full[uc] = params[i].y_full;
        g.inv_y_full[uc] = (params[i].y_full > 0.0) ? 1.0 / params[i].y_full : 0.0;
        g.a_full[uc] = params[i].a_full;
        g.r_full[uc] = params[i].r_full;
        g.s_full[uc] = params[i].s_full;
        g.w_max[uc]  = params[i].w_max;
        g.y_bot[uc]  = params[i].y_bot;
        g.a_bot[uc]  = params[i].a_bot;
        g.s_bot[uc]  = params[i].s_bot;
        g.r_bot[uc]  = params[i].r_bot;
    }
}

void XSectGroups::attachTransectTables(const SimulationContext& ctx) {
    // The packed mirrors must include the per-link table pointers; force a
    // lazy re-init so they pick up the arrays attached below.
    packed_groups_.clear();
    packed_count_.clear();
    mask_active_ = false;
    for (auto& g : groups_) {
        if ((g.shape != XSectShape::IRREGULAR && g.shape != XSectShape::CUSTOM &&
             g.shape != XSectShape::STREET_XSECT) || g.count == 0) continue;

        auto uc = static_cast<std::size_t>(g.count);
        g.area_tables.resize(uc, nullptr);
        g.hrad_tables.resize(uc, nullptr);
        g.width_tables.resize(uc, nullptr);
        g.transect_tbl_size = transect::N_TRANSECT_TBL;

        for (int k = 0; k < g.count; ++k) {
            auto uk = static_cast<std::size_t>(k);
            int link_j = g.link_idx[uk];
            auto uj = static_cast<std::size_t>(link_j);

            int ci = ctx.links.xsect_curve[uj];
            if (ci >= 0 && static_cast<std::size_t>(ci) < ctx.transect_tables.size()) {
                const auto& td = ctx.transect_tables[static_cast<std::size_t>(ci)];
                g.area_tables[uk]  = td.area_tbl;
                g.hrad_tables[uk]  = td.hrad_tbl;
                g.width_tables[uk] = td.width_tbl;
                // Update full-depth properties from transect
                g.y_full[uk] = td.y_full;
                g.inv_y_full[uk] = (td.y_full > 0.0) ? 1.0 / td.y_full : 0.0;
                g.a_full[uk] = td.a_full;
                g.r_full[uk] = td.r_full;
                g.w_max[uk]  = td.w_max;
            }
        }
    }
}

const ShapeGroup* XSectGroups::findGroup(XSectShape shape) const {
    for (const auto& g : groups_) {
        if (g.shape == shape) return &g;
    }
    return nullptr;
}

// ============================================================================
// Batch kernels — area
// ============================================================================

namespace xsect_batch {

// NOTE on the `nrm` parameter (the per-link normalization array): in the
// default (bit-exact) build it is y_full and norm_lookup divides (y/yFull);
// under -DSWMM_XSECT_FAST_LOOKUP it is inv_y_full and norm_lookup does the
// reciprocal-multiply (plan §6). The dispatchers feed the matching array via
// norm_param(g); norm_lookup/norm_x (XSectLookup.hpp) pick the arithmetic. This
// keeps ONE kernel body for both modes.
using xsect::norm_lookup;
using xsect::norm_x;

void area_circular(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT nrm,
    const double* OPENSWMM_RESTRICT a_full,
    double*       OPENSWMM_RESTRICT area,
    int count
) {
    // Legacy xsect_getAofY: a_full * lookup(y/yFull). y<=0 -> area 0 is handled
    // by norm_x (x==0) + A_Circ[0]==0. (Kept serial: an OMP fork here regressed
    // ~38% on Rich_BC_CSO — kernel work per call is smaller than fork/join cost.)
    const double* table = xsect_tables::A_Circ;
    constexpr int n_items = xsect_tables::N_A_Circ;

    for (int k = 0; k < count; ++k)
        area[k] = a_full[k] * norm_lookup(depth[k], nrm[k], table, n_items);
}

void area_rect(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT w_max,
    double*       OPENSWMM_RESTRICT area,
    int count
) {
    // Trivially vectorisable: area = depth * w_max — use explicit SIMD multiply.
    openswmm::simd::multiply(depth, w_max, area, static_cast<std::size_t>(count));
}

void area_trapezoidal(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT y_bot,
    const double* OPENSWMM_RESTRICT s_bot,
    double*       OPENSWMM_RESTRICT area,
    int count
) {
    // area = (y_bot + s_bot * depth) * depth
    OPENSWMM_IVDEP
    for (int k = 0; k < count; ++k) {
        double d = depth[k];
        area[k] = (y_bot[k] + s_bot[k] * d) * d;
    }
}

void area_triangular(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT s_bot,
    double*       OPENSWMM_RESTRICT area,
    int count
) {
    // area = s_bot * depth^2
    OPENSWMM_IVDEP
    for (int k = 0; k < count; ++k) {
        double d = depth[k];
        area[k] = s_bot[k] * d * d;
    }
}

void area_parabolic(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT r_bot,
    double*       OPENSWMM_RESTRICT area,
    int count
) {
    // area = (4/3) * r_bot * depth^(3/2) — auto-vectorizable with ivdep
    constexpr double four_thirds = 4.0 / 3.0;
    OPENSWMM_IVDEP
    for (int k = 0; k < count; ++k) {
        double d = depth[k];
        area[k] = four_thirds * r_bot[k] * d * std::sqrt(d);
    }
}

void area_powerfunc(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT s_bot,
    const double* OPENSWMM_RESTRICT r_bot,
    double*       OPENSWMM_RESTRICT area,
    int count
) {
    // area = r_bot * depth^(s_bot+1)
    for (int k = 0; k < count; ++k) {
        area[k] = r_bot[k] * std::pow(depth[k], s_bot[k] + 1.0);
    }
}

void area_tabulated(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT nrm,
    const double* OPENSWMM_RESTRICT a_full,
    const double* table,
    int            table_size,
    double*       OPENSWMM_RESTRICT area,
    int count
) {
    // Legacy getAofY (tabulated shapes): a_full * lookup(y/yFull). y<=0 -> 0
    // via norm_x (x==0, A_tbl[0]==0). No min(.,1) clamp: yn>1 takes the lookup
    // i>=n-1 early-out (== table[n-1]), exactly as legacy.
    for (int k = 0; k < count; ++k)
        area[k] = a_full[k] * norm_lookup(depth[k], nrm[k], table, table_size);
}

void area_inv_tabulated(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT nrm,
    const double* OPENSWMM_RESTRICT a_full,
    const double* table,
    int            table_size,
    double*       OPENSWMM_RESTRICT area,
    int count
) {
    // Shapes whose area table is Y vs A (inverted) use invLookup. Legacy getAofY:
    // a_full * invLookup(y/yFull); norm_x gives x==0 at y<=0 (invLookup(0)==0).
    for (int k = 0; k < count; ++k)
        area[k] = a_full[k] * xsect::invLookup(norm_x(depth[k], nrm[k]), table, table_size);
}

/// Per-link tabulated lookup (for IRREGULAR shapes where each link has its own table).
void perlink_tabulated(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT nrm,
    const double* OPENSWMM_RESTRICT scale,      // a_full, r_full, or w_max
    const double* const* tables,                 // per-link table pointers
    int            table_size,
    double*       OPENSWMM_RESTRICT result,
    int count
) {
    // Per-link transect tables (IRREGULAR/CUSTOM/STREET). Matches legacy
    // getAofY/getRofY/getWofY normalization (norm_x = y/yFull, mode-aware).
    for (int k = 0; k < count; ++k) {
        if (!tables[k]) { result[k] = 0.0; continue; }
        result[k] = scale[k] * norm_lookup(depth[k], nrm[k], tables[k], table_size);
    }
}

// ============================================================================
// Batch kernels — hydraulic radius
// ============================================================================

void hydrad_circular(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT nrm,
    const double* OPENSWMM_RESTRICT r_full,
    double*       OPENSWMM_RESTRICT hydrad,
    int count
) {
    // Legacy xsect_getRofY: r_full * lookup(y/yFull), NO y<=0 guard — at y==0
    // norm_x gives x==0 and lookup(0)==R_Circ[0] (0.01, not 0), matching legacy.
    const double* table = xsect_tables::R_Circ;
    constexpr int n_items = xsect_tables::N_R_Circ;

    for (int k = 0; k < count; ++k)
        hydrad[k] = r_full[k] * norm_lookup(depth[k], nrm[k], table, n_items);
}

// Fused area + hydraulic radius for the dominant CIRCULAR/FORCE_MAIN shape.
// A_Circ and R_Circ share the same 51-entry grid, so the depth-normalisation,
// segment index, and interpolation weights are computed ONCE and reused for
// both lookups — the index work is the bulk of the per-link cost. Constants are
// hard-wired (no per-element divide, no function-call indirection).
void area_hydrad_circular(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT nrm,
    const double* OPENSWMM_RESTRICT a_full,
    const double* OPENSWMM_RESTRICT r_full,
    double*       OPENSWMM_RESTRICT area,
    double*       OPENSWMM_RESTRICT hydrad,
    int count
) {
#ifdef SWMM_XSECT_FAST_LOOKUP
    // §6 fast mode: reciprocal-multiply normalization (nrm == inv_y_full) +
    // `* inv_delta` index/interp. NOT bit-exact; tolerance-tested (§6).
    const double* A = xsect_tables::A_Circ;
    const double* R = xsect_tables::R_Circ;
    constexpr int    n         = xsect_tables::N_A_Circ;   // == N_R_Circ
    constexpr double inv_delta = static_cast<double>(n - 1);
    constexpr double delta     = 1.0 / inv_delta;
    OPENSWMM_IVDEP
    for (int k = 0; k < count; ++k) {
        double yn = norm_x(depth[k], nrm[k]);              // depth*inv, clamped [0,1]
        int i = static_cast<int>(yn * inv_delta);
        if (i >= n - 1) {
            area[k]   = a_full[k] * A[n - 1];
            hydrad[k] = r_full[k] * R[n - 1];
            continue;
        }
        double x0 = i * delta;
        double f  = (yn - x0) * inv_delta;
        double a  = A[i] + f * (A[i + 1] - A[i]);
        double r  = R[i] + f * (R[i + 1] - R[i]);
        if (i < 2) {
            double x1 = (static_cast<double>(i) + 1.0) * delta;
            double q  = (yn - x0) * (yn - x1) * (inv_delta * inv_delta);
            double a2 = a + q * (A[i] / 2.0 - A[i + 1] + A[i + 2] / 2.0);
            double r2 = r + q * (R[i] / 2.0 - R[i + 1] + R[i + 2] / 2.0);
            if (a2 > 0.0) a = a2;
            if (r2 > 0.0) r = r2;
        }
        area[k]   = a_full[k] * std::max(a, 0.0);
        hydrad[k] = r_full[k] * std::max(r, 0.0);
    }
    return;
#else
    // A_Circ and R_Circ share the same 51-entry grid, so the depth
    // normalization and segment index are computed ONCE and reused for both
    // lookups. Every arithmetic op is the DIVIDE form of legacy lookup()
    // (see xsect::lookup_exact) so this is bit-identical to calling
    // lookup_exact(yn, A_Circ) and lookup_exact(yn, R_Circ) separately:
    //   - index via `yn / delta` (not `yn * inv_delta`),
    //   - linear term `... / delta`, quadratic term `... / (delta*delta)`,
    //   - no fused multiply-add (kept as separate mul/sub/add/div).
    // area at yn == 0 is 0 (A_Circ[0] == 0), matching legacy getAofY's y<=0
    // guard; hydrad at yn == 0 is r_full * R_Circ[0], matching getRofY.
    // NOTE (§5.2 measured & rejected on this arch): a fully branchless variant
    // (compute the quadratic refinement for every lane + masked select) was
    // implemented and proven bit-identical, but microbenchmarked 2.4× SLOWER on
    // arm64/NEON — NEON has no hardware double-gather so the loop does not
    // vectorize, and evaluating the quadratic (2 extra divides) for every element
    // instead of only the rare i<2 lanes dominates. Since "performance is the
    // ultimate goal" (§0) and both forms are bit-exact, we keep the branched form
    // (the `if (i<2)` branch is rare and well-predicted). Revisit branchless only
    // with a measured win behind a hardware-gather (AVX2/AVX-512) path.
    const double* A = xsect_tables::A_Circ;
    const double* R = xsect_tables::R_Circ;
    constexpr int    n     = xsect_tables::N_A_Circ;   // == N_R_Circ
    constexpr double delta = 1.0 / static_cast<double>(n - 1);
    constexpr double dd    = delta * delta;

    OPENSWMM_IVDEP
    for (int k = 0; k < count; ++k) {
        double yn = norm_x(depth[k], nrm[k]);        // (nrm>0 && d>0) ? d/nrm : 0
        int i = static_cast<int>(yn / delta);
        if (i >= n - 1) {
            area[k]   = a_full[k] * A[n - 1];
            hydrad[k] = r_full[k] * R[n - 1];
            continue;
        }
        double x0 = i * delta;
        double a  = A[i] + (yn - x0) * (A[i + 1] - A[i]) / delta;
        double r  = R[i] + (yn - x0) * (R[i + 1] - R[i]) / delta;
        if (i < 2) {                                 // quadratic refinement (legacy lookup)
            double x1 = (static_cast<double>(i) + 1.0) * delta;
            double a2 = a + (yn - x0) * (yn - x1) / dd *
                            (A[i] / 2.0 - A[i + 1] + A[i + 2] / 2.0);
            double r2 = r + (yn - x0) * (yn - x1) / dd *
                            (R[i] / 2.0 - R[i + 1] + R[i + 2] / 2.0);
            if (a2 > 0.0) a = a2;
            if (r2 > 0.0) r = r2;
        }
        if (a < 0.0) a = 0.0;
        if (r < 0.0) r = 0.0;
        area[k]   = a_full[k] * a;
        hydrad[k] = r_full[k] * r;
    }
#endif  // SWMM_XSECT_FAST_LOOKUP
}

void hydrad_trapezoidal(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT y_bot,
    const double* OPENSWMM_RESTRICT s_bot,
    const double* OPENSWMM_RESTRICT r_bot,
    double*       OPENSWMM_RESTRICT hydrad,
    int count
) {
    // R = A / P = (y_bot + s_bot*d)*d / (y_bot + d*r_bot)
    OPENSWMM_IVDEP
    for (int k = 0; k < count; ++k) {
        double d = depth[k];
        if (d <= 0.0) { hydrad[k] = 0.0; continue; }
        double a = (y_bot[k] + s_bot[k] * d) * d;
        double p = y_bot[k] + d * r_bot[k];
        hydrad[k] = a / p;
    }
}

void hydrad_triangular(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT s_bot,
    const double* OPENSWMM_RESTRICT r_bot,
    double*       OPENSWMM_RESTRICT hydrad,
    int count
) {
    // R = (s_bot*y) / (2*r_bot)
    OPENSWMM_IVDEP
    for (int k = 0; k < count; ++k) {
        hydrad[k] = (s_bot[k] * depth[k]) / (2.0 * r_bot[k]);
    }
}

void hydrad_rect(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT w_max,
    double*       OPENSWMM_RESTRICT hydrad,
    int count
) {
    // R = (w*d) / (w + 2*d)
    OPENSWMM_IVDEP
    for (int k = 0; k < count; ++k) {
        double d = depth[k];
        double w = w_max[k];
        hydrad[k] = (w * d) / (w + 2.0 * d);
    }
}

// RECT_CLOSED hydraulic radius — matches legacy rect_closed_getRofA, including
// the near-full top-surface correction (P grows by the crown width as the
// section fills past RECT_ALFMAX). Branch is data-parallel-friendly.
void hydrad_rect_closed(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT w_max,
    const double* OPENSWMM_RESTRICT a_full,
    double*       OPENSWMM_RESTRICT hydrad,
    int count
) {
    constexpr double ALFMAX = 0.97;
    OPENSWMM_IVDEP
    for (int k = 0; k < count; ++k) {
        double d = depth[k];
        if (d <= 0.0) { hydrad[k] = 0.0; continue; }
        double w = w_max[k];
        double a = w * d;
        double p = w + 2.0 * a / w;
        double alpha = a / a_full[k];
        if (alpha > ALFMAX) p += (alpha - ALFMAX) / (1.0 - ALFMAX) * w;
        hydrad[k] = a / p;
    }
}

// RECT_OPEN hydraulic radius — matches legacy rect_open getRofA, honouring the
// s_bot "sides removed" term (0, 1, or 2 banks excluded from the perimeter).
void hydrad_rect_open(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT w_max,
    const double* OPENSWMM_RESTRICT s_bot,
    double*       OPENSWMM_RESTRICT hydrad,
    int count
) {
    OPENSWMM_IVDEP
    for (int k = 0; k < count; ++k) {
        double d = depth[k];
        if (d <= 0.0) { hydrad[k] = 0.0; continue; }
        double w = w_max[k];
        double a = w * d;
        hydrad[k] = a / (w + (2.0 - s_bot[k]) * a / w);
    }
}

// RECT_CLOSED top width — w everywhere except the closed crown (y == y_full),
// where legacy getWofY returns 0.
void width_rect_closed(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT nrm,
    const double* OPENSWMM_RESTRICT w_max,
    double*       OPENSWMM_RESTRICT width,
    int count
) {
    // Legacy getWofY RECT_CLOSED: 0 exactly at the crown (yNorm == 1.0), else
    // wMax. In the bit-exact default norm_x DIVIDES (y == yFull -> yNorm == 1.0
    // exactly); the fast reciprocal form can round just off 1.0 (accepted in §6).
    OPENSWMM_IVDEP
    for (int k = 0; k < count; ++k) {
        double y_norm = norm_x(depth[k], nrm[k]);
        width[k] = (y_norm == 1.0) ? 0.0 : w_max[k];
    }
}

void hydrad_tabulated(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT nrm,
    const double* OPENSWMM_RESTRICT r_full,
    const double* table,
    int            table_size,
    double*       OPENSWMM_RESTRICT hydrad,
    int count
) {
    // Legacy getRofY (tabulated shapes): r_full * lookup(y/yFull), NO y<=0 guard
    // — at y==0 norm_x gives x==0 and lookup(0)==R_tbl[0] (0.01, not 0).
    for (int k = 0; k < count; ++k)
        hydrad[k] = r_full[k] * norm_lookup(depth[k], nrm[k], table, table_size);
}

// ============================================================================
// Batch kernels — top width
// ============================================================================

void width_circular(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT nrm,
    const double* OPENSWMM_RESTRICT w_max,
    double*       OPENSWMM_RESTRICT width,
    int count
) {
    // Legacy xsect_getWofY: w_max * lookup(y/yFull), no y<=0 guard
    // (at y==0 norm_x gives x==0 and lookup(0)==W_Circ[0]==0 -> width 0).
    const double* table = xsect_tables::W_Circ;
    constexpr int n_items = xsect_tables::N_W_Circ;

    for (int k = 0; k < count; ++k)
        width[k] = w_max[k] * norm_lookup(depth[k], nrm[k], table, n_items);
}

void width_trapezoidal(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT y_bot,
    const double* OPENSWMM_RESTRICT s_bot,
    double*       OPENSWMM_RESTRICT width,
    int count
) {
    // W = y_bot + 2*s_bot*depth
    OPENSWMM_IVDEP
    for (int k = 0; k < count; ++k) {
        width[k] = y_bot[k] + 2.0 * s_bot[k] * depth[k];
    }
}

void width_triangular(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT s_bot,
    double*       OPENSWMM_RESTRICT width,
    int count
) {
    // W = 2*s_bot*depth
    OPENSWMM_IVDEP
    for (int k = 0; k < count; ++k) {
        width[k] = 2.0 * s_bot[k] * depth[k];
    }
}

void width_rect(
    const double* OPENSWMM_RESTRICT w_max,
    double*       OPENSWMM_RESTRICT width,
    int count
) {
    for (int k = 0; k < count; ++k) {
        width[k] = w_max[k];
    }
}

void width_tabulated(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT nrm,
    const double* OPENSWMM_RESTRICT w_max,
    const double* table,
    int            table_size,
    double*       OPENSWMM_RESTRICT width,
    int count
) {
    // Legacy getWofY: w_max * lookup(y/yFull), no crown special-case (the table's
    // last entry carries the crown width) and no y<=0 guard (lookup(0)==W_tbl[0]).
    for (int k = 0; k < count; ++k)
        width[k] = w_max[k] * norm_lookup(depth[k], nrm[k], table, table_size);
}

} // namespace xsect_batch

// ============================================================================
// Helper: gather depths for a group, compute via kernel, scatter results
// ============================================================================

namespace {

/// The per-link normalization array the kernels consume: y_full for the default
/// bit-exact divide form, or the precomputed inv_y_full under the §6 fast mode.
/// Selected at compile time so norm_x/norm_lookup (XSectLookup.hpp) and the fed
/// array always agree.
static inline const double* norm_param(const ShapeGroup& g) {
#ifdef SWMM_XSECT_FAST_LOOKUP
    return g.inv_y_full.data();
#else
    return g.y_full.data();
#endif
}

/// Gather depths from global array into contiguous group-local buffer.
void gather_depths(const ShapeGroup& g, const double* global_depths,
                   double* local_depths) {
    for (int k = 0; k < g.count; ++k) {
        local_depths[k] = global_depths[g.link_idx[static_cast<std::size_t>(k)]];
    }
}

/// Scatter results from group-local buffer back to global array.
void scatter_results(const ShapeGroup& g, const double* local_results,
                     double* global_results) {
    for (int k = 0; k < g.count; ++k) {
        global_results[g.link_idx[static_cast<std::size_t>(k)]] = local_results[k];
    }
}

/// Reconstruct the COMPLETE per-element XSectParams for group element k, so the
/// per-element xsect:: fallbacks behave identically to the per-element engine.
/// (s_max/yw_max are not needed by getAofY/getRofY/getWofY and are not stored.)
static inline XSectParams paramsAt(const ShapeGroup& g, int k) {
    auto uk = static_cast<std::size_t>(k);
    XSectParams xs;
    xs.type   = static_cast<int>(g.shape);
    xs.y_full = g.y_full[uk];
    xs.a_full = g.a_full[uk];
    xs.r_full = g.r_full[uk];
    xs.s_full = g.s_full[uk];
    xs.w_max  = g.w_max[uk];
    xs.y_bot  = g.y_bot[uk];
    xs.a_bot  = g.a_bot[uk];
    xs.s_bot  = g.s_bot[uk];
    xs.r_bot  = g.r_bot[uk];
    return xs;
}

/// Get the area lookup table and size for a tabulated shape.
struct TableRef { const double* data; int size; };

TableRef area_table_for(XSectShape shape) {
    using namespace xsect_tables;
    switch (shape) {
        case XSectShape::EGGSHAPED:      return {A_Egg, N_A_Egg};
        case XSectShape::HORSESHOE:      return {A_Horseshoe, N_A_Horseshoe};
        case XSectShape::BASKETHANDLE:   return {A_Baskethandle, N_A_Baskethandle};
        case XSectShape::HORIZ_ELLIPSE:  return {A_HorizEllipse, N_A_HorizEllipse};
        case XSectShape::VERT_ELLIPSE:   return {A_VertEllipse, N_A_VertEllipse};
        case XSectShape::ARCH:           return {A_Arch, N_A_Arch};
        default: return {nullptr, 0};
    }
}

TableRef area_inv_table_for(XSectShape shape) {
    using namespace xsect_tables;
    switch (shape) {
        case XSectShape::GOTHIC:         return {Y_Gothic, N_Y_Gothic};
        case XSectShape::CATENARY:       return {Y_Catenary, N_Y_Catenary};
        case XSectShape::SEMIELLIPTICAL: return {Y_SemiEllip, N_Y_SemiEllip};
        case XSectShape::SEMICIRCULAR:   return {Y_SemiCirc, N_Y_SemiCirc};
        default: return {nullptr, 0};
    }
}

TableRef hydrad_table_for(XSectShape shape) {
    using namespace xsect_tables;
    switch (shape) {
        case XSectShape::EGGSHAPED:      return {R_Egg, N_R_Egg};
        case XSectShape::HORSESHOE:      return {R_Horseshoe, N_R_Horseshoe};
        case XSectShape::BASKETHANDLE:   return {R_Baskethandle, N_R_Baskethandle};
        case XSectShape::HORIZ_ELLIPSE:  return {R_HorizEllipse, N_R_HorizEllipse};
        case XSectShape::VERT_ELLIPSE:   return {R_VertEllipse, N_R_VertEllipse};
        case XSectShape::ARCH:           return {R_Arch, N_R_Arch};
        default: return {nullptr, 0};
    }
}

TableRef width_table_for(XSectShape shape) {
    using namespace xsect_tables;
    switch (shape) {
        case XSectShape::EGGSHAPED:      return {W_Egg, N_W_Egg};
        case XSectShape::HORSESHOE:      return {W_Horseshoe, N_W_Horseshoe};
        case XSectShape::GOTHIC:         return {W_Gothic, N_W_Gothic};
        case XSectShape::CATENARY:       return {W_Catenary, N_W_Catenary};
        case XSectShape::SEMIELLIPTICAL: return {W_SemiEllip, N_W_SemiEllip};
        case XSectShape::BASKETHANDLE:   return {W_BasketHandle, N_W_BasketHandle};
        case XSectShape::SEMICIRCULAR:   return {W_SemiCirc, N_W_SemiCirc};
        case XSectShape::HORIZ_ELLIPSE:  return {W_HorizEllipse, N_W_HorizEllipse};
        case XSectShape::VERT_ELLIPSE:   return {W_VertEllipse, N_W_VertEllipse};
        case XSectShape::ARCH:           return {W_Arch, N_W_Arch};
        default: return {nullptr, 0};
    }
}

// ============================================================================
// Kernel helpers — dispatch shape switch on an already-gathered local_d
// buffer.  Used by the fused triple methods to avoid repeating the switch
// inside each pass.
// ============================================================================

static void apply_area_kernel(const ShapeGroup& g,
                               const double* local_d, double* local_a) {
    switch (g.shape) {
        case XSectShape::CIRCULAR:
        case XSectShape::FORCE_MAIN:
            xsect_batch::area_circular(local_d, norm_param(g),
                                       g.a_full.data(), local_a, g.count);
            break;
        case XSectShape::RECT_CLOSED:
        case XSectShape::RECT_OPEN:
            xsect_batch::area_rect(local_d, g.w_max.data(), local_a, g.count);
            break;
        case XSectShape::TRAPEZOIDAL:
            xsect_batch::area_trapezoidal(local_d, g.y_bot.data(),
                                          g.s_bot.data(), local_a, g.count);
            break;
        case XSectShape::TRIANGULAR:
            xsect_batch::area_triangular(local_d, g.s_bot.data(),
                                         local_a, g.count);
            break;
        case XSectShape::PARABOLIC:
            xsect_batch::area_parabolic(local_d, g.r_bot.data(),
                                        local_a, g.count);
            break;
        case XSectShape::POWERFUNC:
            xsect_batch::area_powerfunc(local_d, g.s_bot.data(),
                                        g.r_bot.data(), local_a, g.count);
            break;
        case XSectShape::IRREGULAR:
        case XSectShape::CUSTOM:
        case XSectShape::STREET_XSECT:
            if (!g.area_tables.empty())
                xsect_batch::perlink_tabulated(local_d, norm_param(g),
                                               g.a_full.data(), g.area_tables.data(),
                                               g.transect_tbl_size, local_a, g.count);
            break;
        default: {
            auto tbl = area_table_for(g.shape);
            if (tbl.data) {
                xsect_batch::area_tabulated(local_d, norm_param(g),
                                            g.a_full.data(), tbl.data, tbl.size,
                                            local_a, g.count);
            } else {
                auto inv = area_inv_table_for(g.shape);
                if (inv.data) {
                    xsect_batch::area_inv_tabulated(local_d, norm_param(g),
                                                    g.a_full.data(), inv.data, inv.size,
                                                    local_a, g.count);
                } else {
                    for (int k = 0; k < g.count; ++k) {
                        auto uk = static_cast<std::size_t>(k);
                        local_a[uk] = xsect::getAofY(paramsAt(g, k), local_d[uk]);
                    }
                }
            }
            break;
        }
    }
}

static void apply_hydrad_kernel(const ShapeGroup& g,
                                 const double* local_d, double* local_h) {
    switch (g.shape) {
        case XSectShape::CIRCULAR:
        case XSectShape::FORCE_MAIN:
            xsect_batch::hydrad_circular(local_d, norm_param(g),
                                         g.r_full.data(), local_h, g.count);
            break;
        case XSectShape::RECT_CLOSED:
            xsect_batch::hydrad_rect_closed(local_d, g.w_max.data(),
                                            g.a_full.data(), local_h, g.count);
            break;
        case XSectShape::RECT_OPEN:
            xsect_batch::hydrad_rect_open(local_d, g.w_max.data(),
                                          g.s_bot.data(), local_h, g.count);
            break;
        case XSectShape::TRAPEZOIDAL:
            xsect_batch::hydrad_trapezoidal(local_d, g.y_bot.data(),
                                            g.s_bot.data(), g.r_bot.data(),
                                            local_h, g.count);
            break;
        case XSectShape::TRIANGULAR:
            xsect_batch::hydrad_triangular(local_d, g.s_bot.data(),
                                           g.r_bot.data(), local_h, g.count);
            break;
        case XSectShape::IRREGULAR:
        case XSectShape::CUSTOM:
        case XSectShape::STREET_XSECT:
            if (!g.hrad_tables.empty())
                xsect_batch::perlink_tabulated(local_d, norm_param(g),
                                               g.r_full.data(), g.hrad_tables.data(),
                                               g.transect_tbl_size, local_h, g.count);
            break;
        default: {
            auto tbl = hydrad_table_for(g.shape);
            if (tbl.data) {
                xsect_batch::hydrad_tabulated(local_d, norm_param(g),
                                              g.r_full.data(), tbl.data, tbl.size,
                                              local_h, g.count);
            } else {
                for (int k = 0; k < g.count; ++k) {
                    auto uk = static_cast<std::size_t>(k);
                    local_h[uk] = xsect::getRofY(paramsAt(g, k), local_d[uk]);
                }
            }
            break;
        }
    }
}

static void apply_width_kernel(const ShapeGroup& g,
                                const double* local_d, double* local_w) {
    switch (g.shape) {
        case XSectShape::CIRCULAR:
        case XSectShape::FORCE_MAIN:
            xsect_batch::width_circular(local_d, norm_param(g),
                                        g.w_max.data(), local_w, g.count);
            break;
        case XSectShape::RECT_OPEN:
            xsect_batch::width_rect(g.w_max.data(), local_w, g.count);
            break;
        case XSectShape::RECT_CLOSED:
            xsect_batch::width_rect_closed(local_d, norm_param(g),
                                           g.w_max.data(), local_w, g.count);
            break;
        case XSectShape::TRAPEZOIDAL:
            xsect_batch::width_trapezoidal(local_d, g.y_bot.data(),
                                           g.s_bot.data(), local_w, g.count);
            break;
        case XSectShape::TRIANGULAR:
            xsect_batch::width_triangular(local_d, g.s_bot.data(),
                                          local_w, g.count);
            break;
        case XSectShape::IRREGULAR:
        case XSectShape::CUSTOM:
        case XSectShape::STREET_XSECT:
            if (!g.width_tables.empty())
                xsect_batch::perlink_tabulated(local_d, norm_param(g),
                                               g.w_max.data(), g.width_tables.data(),
                                               g.transect_tbl_size, local_w, g.count);
            break;
        default: {
            auto tbl = width_table_for(g.shape);
            if (tbl.data) {
                xsect_batch::width_tabulated(local_d, norm_param(g),
                                             g.w_max.data(), tbl.data, tbl.size,
                                             local_w, g.count);
            } else {
                for (int k = 0; k < g.count; ++k) {
                    auto uk = static_cast<std::size_t>(k);
                    local_w[uk] = xsect::getWofY(paramsAt(g, k), local_d[uk]);
                }
            }
            break;
        }
    }
}

// Combined area + hydraulic radius over one group (single depth gather). Uses
// the fused circular kernel for the dominant CIRCULAR/FORCE_MAIN shape (shares
// the table index between A and R); falls back to the two separate dispatchers
// for every other shape.
static void apply_area_hydrad_kernel(const ShapeGroup& g, const double* local_d,
                                     double* local_a, double* local_h) {
    if (g.shape == XSectShape::CIRCULAR || g.shape == XSectShape::FORCE_MAIN) {
        xsect_batch::area_hydrad_circular(local_d, norm_param(g),
                                          g.a_full.data(), g.r_full.data(),
                                          local_a, local_h, g.count);
    } else {
        apply_area_kernel(g, local_d, local_a);
        apply_hydrad_kernel(g, local_d, local_h);
    }
}

} // anonymous namespace

// ============================================================================
// XSectGroups::computeAreas
// ============================================================================

void XSectGroups::computeAreas(const double* depths, double* areas, int /*n_links*/) const {
    for (const auto& g : groups_) {
        if (g.count == 0) continue;
        double* local_d = g.buf_d.data();
        double* local_a = g.buf_r.data();
        gather_depths(g, depths, local_d);
        apply_area_kernel(g, local_d, local_a);
        scatter_results(g, local_a, areas);
    }
}

// ============================================================================
// XSectGroups::computeHydRad
// ============================================================================

void XSectGroups::computeHydRad(const double* depths, double* hydrad, int /*n_links*/) const {
    for (const auto& g : groups_) {
        if (g.count == 0) continue;
        double* local_d = g.buf_d.data();
        double* local_r = g.buf_r.data();
        gather_depths(g, depths, local_d);
        apply_hydrad_kernel(g, local_d, local_r);
        scatter_results(g, local_r, hydrad);
    }
}

// ============================================================================
// XSectGroups::computeAreaAndHydRad — fused (single gather per group)
// ============================================================================

void XSectGroups::computeAreaAndHydRad(const double* depths, double* areas,
                                        double* hydrad, int /*n_links*/) const {
    for (const auto& g : groups_) {
        if (g.count == 0) continue;

        double* local_d = g.buf_d.data();
        double* local_a = g.buf_r.data();    // area results
        double* local_h = g.buf_r2.data();   // hydrad results
        gather_depths(g, depths, local_d);   // single gather

        apply_area_hydrad_kernel(g, local_d, local_a, local_h);

        // Two scatters (same group, different output arrays)
        scatter_results(g, local_a, areas);
        scatter_results(g, local_h, hydrad);
    }
}

// ============================================================================
// XSectGroups::computeWidths
// ============================================================================

void XSectGroups::computeWidths(const double* depths, double* widths, int /*n_links*/) const {
    for (const auto& g : groups_) {
        if (g.count == 0) continue;

        double* local_d = g.buf_d.data();
        double* local_w = g.buf_r.data();
        gather_depths(g, depths, local_d);
        apply_width_kernel(g, local_d, local_w);

        scatter_results(g, local_w, widths);
    }
}

// ============================================================================
// XSectGroups::computeAreaHydRadTriple
// Fused: d1→(a1,hrad1), d2→a2, dm→(am,hrad_mid) in one pass over groups.
// Replaces the three separate calls in STEP D of computeLinkGeometry.
// ============================================================================

void XSectGroups::computeAreaHydRadTriple(
    const double* d1, const double* d2, const double* dm,
    double* a1, double* a2, double* am,
    double* hrad1, double* hrad_mid, int /*n_links*/) const
{
    for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        const ShapeGroup* gp = maskedGroup(gi);
        if (!gp || gp->count == 0) continue;
        const auto& g = *gp;

        double* ld = g.buf_d.data();
        double* la = g.buf_r.data();
        double* lh = g.buf_r2.data();

        // d1 → (a1, hrad1)
        gather_depths(g, d1, ld);
        apply_area_hydrad_kernel(g, ld, la, lh);
        scatter_results(g, la, a1);
        scatter_results(g, lh, hrad1);

        // d2 → a2
        gather_depths(g, d2, ld);
        apply_area_kernel(g, ld, la);   scatter_results(g, la, a2);

        // dm → (am, hrad_mid)
        gather_depths(g, dm, ld);
        apply_area_hydrad_kernel(g, ld, la, lh);
        scatter_results(g, la, am);
        scatter_results(g, lh, hrad_mid);
    }
}

// ============================================================================
// XSectGroups::computeWidthsTriple
// Fused: d1→w1, d2→w2, dm→wm in one pass over groups.
// Replaces the three separate computeWidths calls in STEP B of
// computeLinkGeometry.
// ============================================================================

void XSectGroups::setBypassMask(const std::uint8_t* bypassed_by_link) const {
    mask_active_ = (bypassed_by_link != nullptr);
    if (!mask_active_) return;

    // Lazy (re)build of the packed mirrors. The group layout is static after
    // build()/attachTransectTables() (both clear the mirrors), so sizing them
    // once per layout is enough; per-call work below only repacks contents.
    if (packed_groups_.size() != groups_.size()) {
        packed_groups_.clear();
        packed_groups_.resize(groups_.size());
        packed_count_.assign(groups_.size(), kMaskFullGroup);
        for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
            const auto& g = groups_[gi];
            auto& p = packed_groups_[gi];
            p.shape = g.shape;
            p.resize(g.count);
            if (!g.area_tables.empty()) {
                auto uc = static_cast<std::size_t>(g.count);
                p.area_tables.resize(uc, nullptr);
                p.hrad_tables.resize(uc, nullptr);
                p.width_tables.resize(uc, nullptr);
                p.transect_tbl_size = g.transect_tbl_size;
            }
        }
    }

    for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        const auto& g = groups_[gi];
        auto& p = packed_groups_[gi];
        if (g.count == 0) { packed_count_[gi] = kMaskFullGroup; continue; }

        // Cheap pre-count so the common all-active case (early Picard
        // iterations, unconverged regions) pays one byte-scan and no copies.
        int nb = 0;
        for (int k = 0; k < g.count; ++k)
            nb += bypassed_by_link[g.link_idx[static_cast<std::size_t>(k)]] ? 1 : 0;
        if (nb == 0)       { packed_count_[gi] = kMaskFullGroup; continue; }
        if (nb == g.count) { packed_count_[gi] = 0;              continue; }

        const bool has_tbl = !g.area_tables.empty();
        int n = 0;
        for (int k = 0; k < g.count; ++k) {
            auto uk = static_cast<std::size_t>(k);
            const int li = g.link_idx[uk];
            if (bypassed_by_link[li]) continue;
            auto un = static_cast<std::size_t>(n);
            p.link_idx[un]   = li;
            p.y_full[un]     = g.y_full[uk];
            p.inv_y_full[un] = g.inv_y_full[uk];
            p.a_full[un]     = g.a_full[uk];
            p.r_full[un]     = g.r_full[uk];
            p.s_full[un]     = g.s_full[uk];
            p.w_max[un]      = g.w_max[uk];
            p.y_bot[un]      = g.y_bot[uk];
            p.a_bot[un]      = g.a_bot[uk];
            p.s_bot[un]      = g.s_bot[uk];
            p.r_bot[un]      = g.r_bot[uk];
            if (has_tbl) {
                p.area_tables[un]  = g.area_tables[uk];
                p.hrad_tables[un]  = g.hrad_tables[uk];
                p.width_tables[un] = g.width_tables[uk];
            }
            ++n;
        }
        p.count = n;  // the kernels and gather/scatter read the view's count
        packed_count_[gi] = n;
    }
}

void XSectGroups::computeWidthsTriple(
    const double* d1, const double* d2, const double* dm,
    double* w1, double* w2, double* wm, int /*n_links*/) const
{
    for (std::size_t gi = 0; gi < groups_.size(); ++gi) {
        const ShapeGroup* gp = maskedGroup(gi);
        if (!gp || gp->count == 0) continue;
        const auto& g = *gp;

        double* ld = g.buf_d.data();
        double* lw = g.buf_r.data();

        gather_depths(g, d1, ld);
        apply_width_kernel(g, ld, lw); scatter_results(g, lw, w1);

        gather_depths(g, d2, ld);
        apply_width_kernel(g, ld, lw); scatter_results(g, lw, w2);

        gather_depths(g, dm, ld);
        apply_width_kernel(g, ld, lw); scatter_results(g, lw, wm);
    }
}

// ============================================================================
// Stubs for computeSectionFactors and computeDepthsFromArea
// (will be fleshed out when routing needs them)
// ============================================================================

void XSectGroups::computeSectionFactors(const double* areas, double* sfact, int n_links) const {
    // Fallback: per-element via XSection.hpp
    for (const auto& g : groups_) {
        for (int k = 0; k < g.count; ++k) {
            auto uk = static_cast<std::size_t>(k);
            int li = g.link_idx[uk];
            sfact[li] = xsect::getSofA(paramsAt(g, k), areas[li]);
        }
    }
    (void)n_links;
}

void XSectGroups::computeDepthsFromArea(const double* areas, double* depths, int n_links) const {
    for (const auto& g : groups_) {
        for (int k = 0; k < g.count; ++k) {
            auto uk = static_cast<std::size_t>(k);
            int li = g.link_idx[uk];
            depths[li] = xsect::getYofA(paramsAt(g, k), areas[li]);
        }
    }
    (void)n_links;
}

} // namespace openswmm
