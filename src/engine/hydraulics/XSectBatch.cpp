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
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "XSectBatch.hpp"
#include "xsect_tables.hpp"
#include "../core/SimulationContext.hpp"
#include "../math/SIMD.hpp"

#include <cmath>
#include <algorithm>
#include <numeric>

namespace openswmm {

// ============================================================================
// ShapeGroup
// ============================================================================

void ShapeGroup::resize(int n) {
    count = n;
    auto un = static_cast<std::size_t>(n);
    link_idx.resize(un);
    y_full.resize(un);
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
}

// ============================================================================
// XSectGroups::build (from XSectParams array)
// ============================================================================

void XSectGroups::build(const XSectParams* params, int n_links) {
    groups_.clear();

    // Count links per shape
    constexpr int MAX_SHAPES = 26;
    int shape_count[MAX_SHAPES] = {};
    for (int i = 0; i < n_links; ++i) {
        int t = params[i].type;
        if (t >= 0 && t < MAX_SHAPES) shape_count[t]++;
    }

    // Create groups for non-empty shapes
    // Track write cursors per shape
    int cursor[MAX_SHAPES] = {};
    int group_map[MAX_SHAPES];  // shape → group index (-1 if empty)
    for (int s = 0; s < MAX_SHAPES; ++s) group_map[s] = -1;

    for (int s = 0; s < MAX_SHAPES; ++s) {
        if (shape_count[s] == 0) continue;
        group_map[s] = static_cast<int>(groups_.size());
        groups_.emplace_back();
        auto& g = groups_.back();
        g.shape = static_cast<XSectShape>(s);
        g.resize(shape_count[s]);
    }

    // Fill groups
    for (int i = 0; i < n_links; ++i) {
        int t = params[i].type;
        if (t < 0 || t >= MAX_SHAPES) continue;
        int gi = group_map[t];
        if (gi < 0) continue;
        auto& g = groups_[static_cast<std::size_t>(gi)];
        int c = cursor[t]++;
        auto uc = static_cast<std::size_t>(c);
        g.link_idx[uc] = i;
        g.y_full[uc] = params[i].y_full;
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
    for (auto& g : groups_) {
        if ((g.shape != XSectShape::IRREGULAR && g.shape != XSectShape::CUSTOM) ||
            g.count == 0) continue;

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

void area_circular(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT y_full,
    const double* OPENSWMM_RESTRICT a_full,
    double*       OPENSWMM_RESTRICT area,
    int count
) {
    // Vectorisable: all same table, pure arithmetic interpolation
    const double* table = xsect_tables::A_Circ;
    constexpr int n_items = xsect_tables::N_A_Circ;
    constexpr double delta = 1.0 / static_cast<double>(n_items - 1);
    constexpr double inv_delta = static_cast<double>(n_items - 1);

    for (int k = 0; k < count; ++k) {
        double y = depth[k];
        if (y <= 0.0) { area[k] = 0.0; continue; }

        double y_norm = y / y_full[k];
        y_norm = std::min(y_norm, 1.0);

        int i = static_cast<int>(y_norm * inv_delta);
        if (i >= n_items - 1) { area[k] = a_full[k]; continue; }

        double x0 = i * delta;
        double x1 = (static_cast<double>(i) + 1.0) * delta;

        // Linear interpolation (matching legacy lookup() exactly)
        double t_val = table[i] + (y_norm - x0) * (table[i + 1] - table[i]) * inv_delta;
        t_val = std::max(t_val, 0.0);

        area[k] = a_full[k] * t_val;
    }
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
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC ivdep
#endif
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
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC ivdep
#endif
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
    // area = (4/3) * r_bot * depth^(3/2)
    for (int k = 0; k < count; ++k) {
        double d = depth[k];
        area[k] = (4.0 / 3.0) * r_bot[k] * d * std::sqrt(d);
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
    const double* OPENSWMM_RESTRICT y_full,
    const double* OPENSWMM_RESTRICT a_full,
    const double* table,
    int            table_size,
    double*       OPENSWMM_RESTRICT area,
    int count
) {
    const double delta = 1.0 / static_cast<double>(table_size - 1);
    const double inv_delta = static_cast<double>(table_size - 1);

    for (int k = 0; k < count; ++k) {
        double y = depth[k];
        if (y <= 0.0) { area[k] = 0.0; continue; }

        double y_norm = y / y_full[k];
        y_norm = std::min(y_norm, 1.0);

        int i = static_cast<int>(y_norm * inv_delta);
        if (i >= table_size - 1) { area[k] = a_full[k]; continue; }

        double x0 = i * delta;
        double t_val = table[i] + (y_norm - x0) * (table[i + 1] - table[i]) * inv_delta;
        t_val = std::max(t_val, 0.0);

        area[k] = a_full[k] * t_val;
    }
}

void area_inv_tabulated(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT y_full,
    const double* OPENSWMM_RESTRICT a_full,
    const double* table,
    int            table_size,
    double*       OPENSWMM_RESTRICT area,
    int count
) {
    // For shapes where area table is Y vs A (inverted), use invLookup
    for (int k = 0; k < count; ++k) {
        double y = depth[k];
        if (y <= 0.0) { area[k] = 0.0; continue; }
        double y_norm = y / y_full[k];
        y_norm = std::min(y_norm, 1.0);
        area[k] = a_full[k] * xsect::invLookup(y_norm, table, table_size);
    }
}

/// Per-link tabulated lookup (for IRREGULAR shapes where each link has its own table).
void perlink_tabulated(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT y_full,
    const double* OPENSWMM_RESTRICT scale,      // a_full, r_full, or w_max
    const double* const* tables,                 // per-link table pointers
    int            table_size,
    double*       OPENSWMM_RESTRICT result,
    int count
) {
    const double inv_delta = static_cast<double>(table_size - 1);
    const double delta = 1.0 / inv_delta;

    for (int k = 0; k < count; ++k) {
        double y = depth[k];
        if (y <= 0.0) { result[k] = 0.0; continue; }
        if (!tables[k]) { result[k] = 0.0; continue; }

        double y_norm = y / y_full[k];
        if (y_norm >= 1.0) { result[k] = scale[k]; continue; }

        int i = static_cast<int>(y_norm * inv_delta);
        if (i >= table_size - 1) { result[k] = scale[k]; continue; }

        const double* tbl = tables[k];
        double t_val = tbl[i] + (y_norm - i * delta) * (tbl[i + 1] - tbl[i]) * inv_delta;
        t_val = std::max(t_val, 0.0);
        result[k] = scale[k] * t_val;
    }
}

// ============================================================================
// Batch kernels — hydraulic radius
// ============================================================================

void hydrad_circular(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT y_full,
    const double* OPENSWMM_RESTRICT r_full,
    double*       OPENSWMM_RESTRICT hydrad,
    int count
) {
    const double* table = xsect_tables::R_Circ;
    constexpr int n_items = xsect_tables::N_R_Circ;
    constexpr double inv_delta = static_cast<double>(n_items - 1);
    constexpr double delta = 1.0 / inv_delta;

    for (int k = 0; k < count; ++k) {
        double y = depth[k];
        if (y <= 0.0) { hydrad[k] = 0.0; continue; }

        double y_norm = y / y_full[k];
        y_norm = std::min(y_norm, 1.0);

        int i = static_cast<int>(y_norm * inv_delta);
        if (i >= n_items - 1) { hydrad[k] = r_full[k]; continue; }

        double x0 = i * delta;
        double t_val = table[i] + (y_norm - x0) * (table[i + 1] - table[i]) * inv_delta;
        t_val = std::max(t_val, 0.0);

        hydrad[k] = r_full[k] * t_val;
    }
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
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC ivdep
#endif
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
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC ivdep
#endif
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
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC ivdep
#endif
    for (int k = 0; k < count; ++k) {
        double d = depth[k];
        double w = w_max[k];
        hydrad[k] = (w * d) / (w + 2.0 * d);
    }
}

void hydrad_tabulated(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT y_full,
    const double* OPENSWMM_RESTRICT r_full,
    const double* table,
    int            table_size,
    double*       OPENSWMM_RESTRICT hydrad,
    int count
) {
    const double inv_delta = static_cast<double>(table_size - 1);
    const double delta = 1.0 / inv_delta;

    for (int k = 0; k < count; ++k) {
        double y = depth[k];
        if (y <= 0.0) { hydrad[k] = 0.0; continue; }

        double y_norm = y / y_full[k];
        y_norm = std::min(y_norm, 1.0);

        int i = static_cast<int>(y_norm * inv_delta);
        if (i >= table_size - 1) { hydrad[k] = r_full[k]; continue; }

        double x0 = i * delta;
        double t_val = table[i] + (y_norm - x0) * (table[i + 1] - table[i]) * inv_delta;
        t_val = std::max(t_val, 0.0);

        hydrad[k] = r_full[k] * t_val;
    }
}

// ============================================================================
// Batch kernels — top width
// ============================================================================

void width_circular(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT y_full,
    const double* OPENSWMM_RESTRICT w_max,
    double*       OPENSWMM_RESTRICT width,
    int count
) {
    const double* table = xsect_tables::W_Circ;
    constexpr int n_items = xsect_tables::N_W_Circ;
    constexpr double inv_delta = static_cast<double>(n_items - 1);
    constexpr double delta = 1.0 / inv_delta;

    for (int k = 0; k < count; ++k) {
        double y_norm = depth[k] / y_full[k];
        y_norm = std::min(y_norm, 1.0);

        int i = static_cast<int>(y_norm * inv_delta);
        if (i >= n_items - 1) { width[k] = 0.0; continue; }

        double x0 = i * delta;
        double t_val = table[i] + (y_norm - x0) * (table[i + 1] - table[i]) * inv_delta;
        t_val = std::max(t_val, 0.0);

        width[k] = w_max[k] * t_val;
    }
}

void width_trapezoidal(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT y_bot,
    const double* OPENSWMM_RESTRICT s_bot,
    double*       OPENSWMM_RESTRICT width,
    int count
) {
    // W = y_bot + 2*s_bot*depth
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC ivdep
#endif
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
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC ivdep
#endif
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
    const double* OPENSWMM_RESTRICT y_full,
    const double* OPENSWMM_RESTRICT w_max,
    const double* table,
    int            table_size,
    double*       OPENSWMM_RESTRICT width,
    int count
) {
    const double inv_delta = static_cast<double>(table_size - 1);
    const double delta = 1.0 / inv_delta;

    for (int k = 0; k < count; ++k) {
        double y_norm = depth[k] / y_full[k];
        y_norm = std::min(y_norm, 1.0);

        int i = static_cast<int>(y_norm * inv_delta);
        if (i >= table_size - 1) { width[k] = 0.0; continue; }

        double x0 = i * delta;
        double t_val = table[i] + (y_norm - x0) * (table[i + 1] - table[i]) * inv_delta;
        t_val = std::max(t_val, 0.0);

        width[k] = w_max[k] * t_val;
    }
}

} // namespace xsect_batch

// ============================================================================
// Helper: gather depths for a group, compute via kernel, scatter results
// ============================================================================

namespace {

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

} // anonymous namespace

// ============================================================================
// XSectGroups::computeAreas
// ============================================================================

void XSectGroups::computeAreas(const double* depths, double* areas, int /*n_links*/) const {
    for (const auto& g : groups_) {
        if (g.count == 0) continue;

        // Use pre-allocated buffers (no allocation in hot path)
        double* local_d = g.buf_d.data();
        double* local_a = g.buf_r.data();
        gather_depths(g, depths, local_d);

        switch (g.shape) {
            case XSectShape::CIRCULAR:
            case XSectShape::FORCE_MAIN:
                xsect_batch::area_circular(local_d, g.y_full.data(),
                                           g.a_full.data(), local_a, g.count);
                break;

            case XSectShape::RECT_CLOSED:
            case XSectShape::RECT_OPEN:
                xsect_batch::area_rect(local_d, g.w_max.data(),
                                       local_a, g.count);
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
                if (!g.area_tables.empty()) {
                    xsect_batch::perlink_tabulated(local_d, g.y_full.data(),
                                                    g.a_full.data(), g.area_tables.data(),
                                                    g.transect_tbl_size, local_a, g.count);
                }
                break;

            default: {
                // Check tabulated shapes
                auto tbl = area_table_for(g.shape);
                if (tbl.data) {
                    xsect_batch::area_tabulated(local_d, g.y_full.data(),
                                                g.a_full.data(), tbl.data, tbl.size,
                                                local_a, g.count);
                } else {
                    auto inv = area_inv_table_for(g.shape);
                    if (inv.data) {
                        xsect_batch::area_inv_tabulated(local_d, g.y_full.data(),
                                                         g.a_full.data(), inv.data, inv.size,
                                                         local_a, g.count);
                    } else {
                        // Fallback: per-element using XSection.hpp
                        for (int k = 0; k < g.count; ++k) {
                            XSectParams xs;
                            xs.type = static_cast<int>(g.shape);
                            xs.y_full = g.y_full[static_cast<std::size_t>(k)];
                            xs.a_full = g.a_full[static_cast<std::size_t>(k)];
                            xs.w_max  = g.w_max[static_cast<std::size_t>(k)];
                            xs.y_bot  = g.y_bot[static_cast<std::size_t>(k)];
                            xs.a_bot  = g.a_bot[static_cast<std::size_t>(k)];
                            xs.s_bot  = g.s_bot[static_cast<std::size_t>(k)];
                            xs.r_bot  = g.r_bot[static_cast<std::size_t>(k)];
                            local_a[static_cast<std::size_t>(k)] = xsect::getAofY(xs, local_d[static_cast<std::size_t>(k)]);
                        }
                    }
                }
                break;
            }
        }

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

        switch (g.shape) {
            case XSectShape::CIRCULAR:
            case XSectShape::FORCE_MAIN:
                xsect_batch::hydrad_circular(local_d, g.y_full.data(),
                                             g.r_full.data(), local_r, g.count);
                break;

            case XSectShape::RECT_CLOSED:
            case XSectShape::RECT_OPEN:
                xsect_batch::hydrad_rect(local_d, g.w_max.data(),
                                         local_r, g.count);
                break;

            case XSectShape::TRAPEZOIDAL:
                xsect_batch::hydrad_trapezoidal(local_d, g.y_bot.data(),
                                                g.s_bot.data(), g.r_bot.data(),
                                                local_r, g.count);
                break;

            case XSectShape::TRIANGULAR:
                xsect_batch::hydrad_triangular(local_d, g.s_bot.data(),
                                               g.r_bot.data(), local_r, g.count);
                break;

            case XSectShape::IRREGULAR:
            case XSectShape::CUSTOM:
                if (!g.hrad_tables.empty()) {
                    xsect_batch::perlink_tabulated(local_d, g.y_full.data(),
                                                    g.r_full.data(), g.hrad_tables.data(),
                                                    g.transect_tbl_size, local_r, g.count);
                }
                break;

            default: {
                auto tbl = hydrad_table_for(g.shape);
                if (tbl.data) {
                    xsect_batch::hydrad_tabulated(local_d, g.y_full.data(),
                                                  g.r_full.data(), tbl.data, tbl.size,
                                                  local_r, g.count);
                } else {
                    for (int k = 0; k < g.count; ++k) {
                        XSectParams xs;
                        xs.type = static_cast<int>(g.shape);
                        xs.y_full = g.y_full[static_cast<std::size_t>(k)];
                        xs.a_full = g.a_full[static_cast<std::size_t>(k)];
                        xs.r_full = g.r_full[static_cast<std::size_t>(k)];
                        xs.w_max  = g.w_max[static_cast<std::size_t>(k)];
                        xs.y_bot  = g.y_bot[static_cast<std::size_t>(k)];
                        xs.s_bot  = g.s_bot[static_cast<std::size_t>(k)];
                        xs.r_bot  = g.r_bot[static_cast<std::size_t>(k)];
                        local_r[static_cast<std::size_t>(k)] = xsect::getRofY(xs, local_d[static_cast<std::size_t>(k)]);
                    }
                }
                break;
            }
        }

        scatter_results(g, local_r, hydrad);
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

        switch (g.shape) {
            case XSectShape::CIRCULAR:
            case XSectShape::FORCE_MAIN:
                xsect_batch::width_circular(local_d, g.y_full.data(),
                                            g.w_max.data(), local_w, g.count);
                break;

            case XSectShape::RECT_CLOSED:
            case XSectShape::RECT_OPEN:
                xsect_batch::width_rect(g.w_max.data(), local_w, g.count);
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
                if (!g.width_tables.empty()) {
                    xsect_batch::perlink_tabulated(local_d, g.y_full.data(),
                                                    g.w_max.data(), g.width_tables.data(),
                                                    g.transect_tbl_size, local_w, g.count);
                }
                break;

            default: {
                auto tbl = width_table_for(g.shape);
                if (tbl.data) {
                    xsect_batch::width_tabulated(local_d, g.y_full.data(),
                                                 g.w_max.data(), tbl.data, tbl.size,
                                                 local_w, g.count);
                } else {
                    for (int k = 0; k < g.count; ++k) {
                        XSectParams xs;
                        xs.type = static_cast<int>(g.shape);
                        xs.y_full = g.y_full[static_cast<std::size_t>(k)];
                        xs.w_max  = g.w_max[static_cast<std::size_t>(k)];
                        xs.y_bot  = g.y_bot[static_cast<std::size_t>(k)];
                        xs.s_bot  = g.s_bot[static_cast<std::size_t>(k)];
                        xs.r_bot  = g.r_bot[static_cast<std::size_t>(k)];
                        local_w[static_cast<std::size_t>(k)] = xsect::getWofY(xs, local_d[static_cast<std::size_t>(k)]);
                    }
                }
                break;
            }
        }

        scatter_results(g, local_w, widths);
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
            XSectParams xs;
            xs.type = static_cast<int>(g.shape);
            xs.y_full = g.y_full[uk]; xs.a_full = g.a_full[uk];
            xs.r_full = g.r_full[uk]; xs.s_full = g.s_full[uk];
            xs.w_max  = g.w_max[uk];  xs.y_bot  = g.y_bot[uk];
            xs.a_bot  = g.a_bot[uk];  xs.s_bot  = g.s_bot[uk];
            xs.r_bot  = g.r_bot[uk];
            sfact[li] = xsect::getSofA(xs, areas[li]);
        }
    }
    (void)n_links;
}

void XSectGroups::computeDepthsFromArea(const double* areas, double* depths, int n_links) const {
    for (const auto& g : groups_) {
        for (int k = 0; k < g.count; ++k) {
            auto uk = static_cast<std::size_t>(k);
            int li = g.link_idx[uk];
            XSectParams xs;
            xs.type = static_cast<int>(g.shape);
            xs.y_full = g.y_full[uk]; xs.a_full = g.a_full[uk];
            xs.w_max  = g.w_max[uk];  xs.y_bot  = g.y_bot[uk];
            xs.a_bot  = g.a_bot[uk];  xs.s_bot  = g.s_bot[uk];
            xs.r_bot  = g.r_bot[uk];
            depths[li] = xsect::getYofA(xs, areas[li]);
        }
    }
    (void)n_links;
}

} // namespace openswmm
