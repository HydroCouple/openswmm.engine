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
 * @file XSectBatch.hpp
 * @brief Cross-section geometry — unified batch + per-element API.
 *
 * @details This is the SINGLE cross-section header for the new engine.
 *          It provides:
 *
 *          1. **Shape enum + XSectParams struct** — data types for cross-sections
 *          2. **Per-element functions** (`xsect::` namespace) — used by C API,
 *             hot start, KW/DW solvers for single-conduit queries
 *          3. **Batch API** (`XSectGroups`, `xsect_batch::`) — shape-grouped SoA
 *             for the routing hot loop; groups links by shape, computes over
 *             contiguous arrays with no branching
 *
 *          **Routing hot loop (batch):**
 *          @code
 *          XSectGroups groups;
 *          groups.build(ctx);  // once, at init
 *          groups.computeAreas(link_depths, link_areas);
 *          groups.computeHydRad(link_depths, link_hydrad);
 *          groups.computeWidths(link_depths, link_widths);
 *          @endcode
 *
 * @see SIMD.hpp — SIMD abstraction layer
 * @see xsect_tables.hpp — lookup table data (internal)
 * @note Legacy reference: src/legacy/engine/xsect.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_XSECT_BATCH_HPP
#define OPENSWMM_XSECT_BATCH_HPP

#ifndef OPENSWMM_RESTRICT
#  if defined(_MSC_VER)
#    define OPENSWMM_RESTRICT __restrict
#  else
#    define OPENSWMM_RESTRICT __restrict__
#  endif
#endif

#include <vector>
#include <cstddef>
#include <cstdint>
#include <cmath>

namespace openswmm {

// Forward declaration
struct SimulationContext;

namespace chebsec { struct ChebSection; }

// ============================================================================
// Cross-section shape codes (matches legacy enums.h XsectType)
// ============================================================================

enum class XSectShape : int {
    DUMMY              =  0,
    CIRCULAR           =  1,
    FILLED_CIRCULAR    =  2,
    RECT_CLOSED        =  3,
    RECT_OPEN          =  4,
    TRAPEZOIDAL        =  5,
    TRIANGULAR         =  6,
    PARABOLIC          =  7,
    POWERFUNC          =  8,
    RECT_TRIANG        =  9,
    RECT_ROUND         = 10,
    MOD_BASKET         = 11,
    HORIZ_ELLIPSE      = 12,
    VERT_ELLIPSE       = 13,
    ARCH               = 14,
    EGGSHAPED          = 15,
    HORSESHOE          = 16,
    GOTHIC             = 17,
    CATENARY           = 18,
    SEMIELLIPTICAL     = 19,
    BASKETHANDLE       = 20,
    SEMICIRCULAR       = 21,
    IRREGULAR          = 22,
    CUSTOM             = 23,
    FORCE_MAIN         = 24,
    STREET_XSECT       = 25,
    POLYGON            = 26
};

// ============================================================================
// Cross-section parameter struct (mirrors legacy TXsect)
// ============================================================================

struct XSectParams {
    int    type         = 0;
    int    culvert_code = 0;
    int    transect     = -1;

    double y_full  = 0.0;      ///< Full depth (ft)
    double w_max   = 0.0;      ///< Width at widest point (ft)
    double yw_max  = 0.0;      ///< Depth at widest point (ft)
    double a_full  = 0.0;      ///< Area when full (ft2)
    double r_full  = 0.0;      ///< Hydraulic radius when full (ft)
    double s_full  = 0.0;      ///< Section factor when full (ft^4/3)
    double s_max   = 0.0;      ///< Section factor at max flow (ft^4/3)

    double y_bot   = 0.0;      ///< Depth of bottom section / fill depth
    double a_bot   = 0.0;      ///< Area of bottom section
    double s_bot   = 0.0;      ///< Slope of bottom section / exponent
    double r_bot   = 0.0;      ///< Radius of bottom section / coefficient

    // Tabulated geometry for IRREGULAR / CUSTOM / STREET_XSECT shapes, whose
    // A/R/W vs depth come from a per-link transect table rather than a shared
    // static table.  These point into ctx.transect_tables (stable for the run)
    // and stay null for every self-contained shape.  Without them the
    // context-free per-element accessors return 0 for irregular sections (so
    // e.g. init-time getDepthFromFlow could not compute normal depth/storage).
    const double* area_tbl  = nullptr;   ///< Normalized area      vs y/y_full
    const double* hrad_tbl  = nullptr;   ///< Normalized hyd-radius vs y/y_full
    const double* width_tbl = nullptr;   ///< Normalized width     vs y/y_full
    int    transect_tbl_size = 0;        ///< Entry count of the tables above

    // Piecewise-Chebyshev boundary (POLYGON shapes ALWAYS; other shapes only
    // under `[OPTIONS] XSECT_GEOMETRY EXACT`). When non-null, every accessor
    // in XsectEval takes this path instead of its per-shape table/formula
    // dispatch, regardless of @ref type — see the `xs.cheb` early-return at
    // the top of each dispatcher in XSectKernels.hpp. Points into
    // ctx.cheb_sections (a std::deque, so the address is stable for the run).
    const chebsec::ChebSection* cheb = nullptr;
};

// ============================================================================
// Per-element functions (xsect:: namespace)
// ============================================================================

namespace xsect {

double getAofY(const XSectParams& xs, double y);
double getRofY(const XSectParams& xs, double y);
double getWofY(const XSectParams& xs, double y);
double getYofA(const XSectParams& xs, double a);
double getSofA(const XSectParams& xs, double a);
double getRofA(const XSectParams& xs, double a);
double getdSdA(const XSectParams& xs, double a);
double getAofS(const XSectParams& xs, double s_factor);
double getAmax(const XSectParams& xs);
double getYcrit(const XSectParams& xs, double q);
bool   isOpen(int type);
/// Per-instance open/closed test. A POLYGON section (or any shape whose
/// XSectParams::cheb is set under XSECT_GEOMETRY EXACT) cannot be classified
/// from its shape code alone — open vs. closed is a property of the specific
/// compiled boundary, recorded in ChebSection::is_open. Falls back to
/// isOpen(int) for shapes with no compiled boundary.
bool   isOpen(const XSectParams& xs);
int    setParams(XSectParams& xs, int type, const double p[], double ucf);

// Lookup table helpers (exposed for batch kernels and testing)
double lookup(double x, const double* table, int n_items);
double invLookup(double y, const double* table, int n_items);
int    locate(double y, const double* table, int n);
double getYcircular(double alpha);
double getScircular(double alpha);

} // namespace xsect

// ============================================================================
// Shape group — contiguous SoA for all links sharing one shape type
// ============================================================================

/**
 * @brief SoA parameter block for all links of one cross-section shape.
 *
 * @details All arrays have size `count`. The `link_idx` array maps each
 *          position back to the original link index in SimulationContext so
 *          results can be scattered back to the global arrays.
 *
 *          Parameter arrays are populated once at initialisation from
 *          the per-link XSectParams and do not change during simulation.
 */
struct ShapeGroup {
    XSectShape shape = XSectShape::DUMMY;
    int        count = 0;

    // Mapping back to global link arrays
    std::vector<int> link_idx;      ///< link_idx[i] = index in SimulationContext

    // Geometry parameters (contiguous, aligned for SIMD)
    std::vector<double> y_full;     ///< Full depth (ft)
    std::vector<double> a_full;     ///< Full area (ft2)
    std::vector<double> r_full;     ///< Hyd. radius at full (ft)
    std::vector<double> s_full;     ///< Section factor at full
    std::vector<double> w_max;      ///< Max width (ft)

    // Pre-computed reciprocal of y_full (avoids per-element division in kernels)
    std::vector<double> inv_y_full;    ///< 1.0 / y_full (or 0 if y_full==0)

    // Multi-purpose parameters (meaning depends on shape)
    std::vector<double> y_bot;
    std::vector<double> a_bot;
    std::vector<double> s_bot;
    std::vector<double> r_bot;

    // Per-link transect table pointers (IRREGULAR shapes only)
    // Each pointer → a normalized table of N_TRANSECT_TBL entries.
    std::vector<const double*> area_tables;   ///< Per-link area table
    std::vector<const double*> hrad_tables;   ///< Per-link hyd-rad table
    std::vector<const double*> width_tables;  ///< Per-link width table
    int transect_tbl_size = 0;                ///< Table size (same for all)

    // Per-link compiled Chebyshev boundary (POLYGON group always; other
    // groups only under XSECT_GEOMETRY EXACT — see attachChebSections()).
    // Non-null entries here are what make the scalar per-element fallback
    // (paramsAt()) route through the exact/Chebyshev path.
    std::vector<const chebsec::ChebSection*> cheb;

    // Pre-allocated working buffers (avoids per-call allocation in hot loop)
    mutable std::vector<double> buf_d;   ///< Gather buffer for depths
    mutable std::vector<double> buf_r;   ///< Scatter buffer for results
    mutable std::vector<double> buf_r2;  ///< Second scatter buffer (for fused ops)

    /// Resize all arrays to n elements.
    void resize(int n);
};

// ============================================================================
// XSectGroups — the shape-grouped index over all links
// ============================================================================

/**
 * @brief Shape-grouped cross-section manager for batch computation.
 *
 * @details Call `build()` once after the model is loaded/built. This sorts
 *          links by shape type and builds contiguous SoA parameter blocks.
 *          Then use `computeAreas()`, `computeHydRad()`, `computeWidths()`
 *          in the routing hot loop — each iterates over shape groups and
 *          calls the shape-specific vectorised kernel.
 *
 *          Results are written directly into the caller's global arrays
 *          (indexed by link) using the scatter index `link_idx`.
 */
class XSectGroups {
public:
    /**
     * @brief Build shape groups from SimulationContext link data.
     *
     * @details Scans all links, groups them by xsect shape, and copies
     *          the geometry parameters into contiguous SoA arrays. Only
     *          shapes that have at least one link get a group.
     *
     * @param ctx  SimulationContext (must have links populated).
     */
    void build(const SimulationContext& ctx);

    /**
     * @brief Attach transect tables to the IRREGULAR shape group.
     *
     * @details Must be called after build() when IRREGULAR shapes exist.
     *          Populates per-link area/hrad/width table pointers from
     *          the precomputed transect tables in the context.
     *
     * @param ctx  SimulationContext with transect_tables populated.
     */
    void attachTransectTables(const SimulationContext& ctx);

    /**
     * @brief Attach compiled Chebyshev boundaries to POLYGON (and, under
     *        XSECT_GEOMETRY EXACT, any other) shape groups.
     *
     * @details Must be called after build() when POLYGON shapes exist, or
     *          when EXACT mode compiled built-in shapes too. Mirrors
     *          attachTransectTables() exactly, one array (cheb) instead of
     *          three, keyed by LinkData::xsect_cheb_idx instead of
     *          xsect_curve.
     *
     * @param ctx  SimulationContext with cheb_sections populated.
     *
     * @note **Sorting a group's elements by compiled section was built,
     *       measured and REJECTED (promptperf.md Phase D) — do not re-add it
     *       without new evidence.** The idea: a group stores links in
     *       ascending link-index order, so consecutive elements reference
     *       different compiled sections, and sorting by section would keep
     *       each section's data hot across the run of links using it. It
     *       works and it is numerically inert (Bellinge's 180 MB EXACT `.out`
     *       came back byte-identical, as the permutation argument requires),
     *       and it genuinely engages — 932 of the 951 CIRCULAR elements moved,
     *       collapsing into 46 contiguous runs. It is simply **not faster**:
     *       interleaved A/B on Bellinge measured 44.20 s unsorted vs 45.18 s
     *       sorted (3 pairs each), i.e. ~2 % the wrong way and well inside
     *       this network's run-to-run spread.
     *       The mechanism, which is the part worth keeping: sorting **trades
     *       output locality for input locality**. Unsorted, `link_idx` rises
     *       monotonically, so gather_depths/scatter_results stream
     *       sequentially through the global per-link arrays (eight of them on
     *       the triple path). Sorted, those accesses scatter. The input side
     *       had little left to buy, because the compiled sections are already
     *       deduplicated — 46 of them, only a few hot cache lines each (see
     *       ChebSection.hpp) — so the section working set was cache-resident
     *       before any reordering. promptperf.md's own synthetic table
     *       predicted a 2.5x win here, but it modelled section access ALONE,
     *       with no global scatter arrays present, and against the pre-Phase-B
     *       25 kB sections. Dedup (Phase C) removed the problem Phase D was
     *       designed to solve.
     */
    void attachChebSections(const SimulationContext& ctx);

    /**
     * @brief Build shape groups from an array of XSectParams.
     *
     * @details Alternative to build(ctx) — useful for testing.
     *
     * @param params  Array of per-link XSectParams.
     * @param n_links Number of links.
     */
    void build(const XSectParams* params, int n_links);

    // ========================================================================
    // Batch compute — results scattered to global link arrays
    // ========================================================================

    /**
     * @brief Compute area for every link, reading depth from `depths[link]`.
     *
     * @param depths  [in]  Global depth array (indexed by link).
     * @param areas   [out] Global area array (indexed by link).
     * @param n_links Total number of links.
     */
    void computeAreas(const double* depths, double* areas, int n_links) const;

    /**
     * @brief Compute hydraulic radius for every link.
     *
     * @param depths  [in]  Global depth array.
     * @param hydrad  [out] Global hydraulic radius array.
     * @param n_links Total number of links.
     */
    void computeHydRad(const double* depths, double* hydrad, int n_links) const;

    /**
     * @brief Compute top width for every link.
     *
     * @param depths  [in]  Global depth array.
     * @param widths  [out] Global top width array.
     * @param n_links Total number of links.
     */
    void computeWidths(const double* depths, double* widths, int n_links) const;

    /**
     * @brief Fused area + hydraulic radius computation (single gather/scatter).
     *
     * @param depths  [in]  Global depth array.
     * @param areas   [out] Global area array.
     * @param hydrad  [out] Global hydraulic radius array.
     * @param n_links Total number of links.
     */
    void computeAreaAndHydRad(const double* depths, double* areas,
                              double* hydrad, int n_links) const;

    /**
     * @brief Fused triple: d1→(a1,hrad1), d2→a2, dm→(am,hrad_mid) in one
     *        pass over shape groups.  Replaces three separate compute calls
     *        in STEP B / STEP D of computeLinkGeometry.
     */
    void computeAreaHydRadTriple(
        const double* d1, const double* d2, const double* dm,
        double* a1, double* a2, double* am,
        double* hrad1, double* hrad_mid, int n_links) const;

    /**
     * @brief Fused triple: d1→w1, d2→w2, dm→wm in one pass over shape groups.
     */
    void computeWidthsTriple(
        const double* d1, const double* d2, const double* dm,
        double* w1, double* w2, double* wm, int n_links) const;

    /**
     * @brief Team-callable triple kernels: thread `tid` of `nthreads`
     *        processes only its static slice of every shape group.
     *
     * @details Call from EVERY thread of an OpenMP team (typically the
     *          persistent DW Picard team) with that thread's id/team size;
     *          the caller owns the barrier that orders the outputs for
     *          consumers. Slices are disjoint (single-producer per element,
     *          including the shared group-local scratch buffers, which are
     *          sliced on cache-line boundaries), and per-element results are
     *          position-independent — so results are bit-identical to the
     *          serial triple at any thread count. Honors the bypass mask
     *          exactly like the serial forms. (tid=0, nthreads=1) IS the
     *          serial form.
     */
    void computeAreaHydRadTripleTeam(
        const double* d1, const double* d2, const double* dm,
        double* a1, double* a2, double* am,
        double* hrad1, double* hrad_mid, int n_links,
        int tid, int nthreads) const;

    void computeWidthsTripleTeam(
        const double* d1, const double* d2, const double* dm,
        double* w1, double* w2, double* wm, int n_links,
        int tid, int nthreads) const;

    /**
     * @brief Restrict the triple kernels to non-bypassed links for the
     *        current Picard iteration.
     *
     * @details Legacy findLinkFlows skips a "bypassed" link (both end nodes
     *          converged) entirely, leaving its geometry at the previously
     *          computed values.  The batch triple kernels instead recompute
     *          every link from its (unchanged) stored depths — producing
     *          bit-identical values, i.e. pure waste that grows with the
     *          bypass fraction (Bellinge averages ~9 Picard iterations per
     *          step, so most kernel invocations run with a mostly-converged
     *          network).  This packs each shape group down to its active
     *          links so the kernels and gather/scatter touch only those.
     *          Bypassed links keep their stored outputs, exactly as legacy.
     *
     *          Pass nullptr to clear (all links active, zero overhead).
     *          The mask only affects computeWidthsTriple and
     *          computeAreaHydRadTriple; all other compute* methods are
     *          init/reporting-path and stay unmasked.
     *
     * @param bypassed_by_link  Per-link flags indexed by GLOBAL link index
     *                          (1 = bypassed/skip), or nullptr for all-active.
     *
     * @note const: the mask is per-iteration scratch (mutable members), set
     *       through the same const XSectGroups* the solver computes with.
     */
    void setBypassMask(const std::uint8_t* bypassed_by_link) const;

    /**
     * @brief Compute section factor for every link (from area, not depth).
     *
     * @param areas   [in]  Global area array.
     * @param sfact   [out] Global section factor array.
     * @param n_links Total number of links.
     */
    void computeSectionFactors(const double* areas, double* sfact, int n_links) const;

    /**
     * @brief Compute depth from area for every link (inverse).
     *
     * @param areas   [in]  Global area array.
     * @param depths  [out] Global depth array.
     * @param n_links Total number of links.
     */
    void computeDepthsFromArea(const double* areas, double* depths, int n_links) const;

    // ========================================================================
    // Accessors
    // ========================================================================

    /// Number of non-empty shape groups.
    int numGroups() const { return static_cast<int>(groups_.size()); }

    /// Access a specific shape group.
    const ShapeGroup& group(int i) const { return groups_[i]; }

    /// Find the group for a given shape (returns nullptr if no links have that shape).
    const ShapeGroup* findGroup(XSectShape shape) const;

private:
    std::vector<ShapeGroup> groups_;

    // Per-iteration bypass-mask state (see setBypassMask).  packed_groups_[gi]
    // is a packed view of groups_[gi] holding only the active links; it reuses
    // the ShapeGroup layout so the gather/kernel/scatter path runs unchanged.
    // packed_count_[gi]:  kMaskFullGroup → no link in the group is bypassed
    // (use the original group directly, no packing was done); 0..count →
    // number of active links in the packed view.  All mutable because the
    // mask is per-Picard-iteration scratch while the compute API is const.
    static constexpr int kMaskFullGroup = -1;
    mutable std::vector<ShapeGroup> packed_groups_;
    mutable std::vector<int>        packed_count_;
    mutable bool                    mask_active_ = false;

    /// Group view honoring the bypass mask: nullptr → skip group entirely.
    const ShapeGroup* maskedGroup(std::size_t gi) const {
        const ShapeGroup* gp = &groups_[gi];
        if (mask_active_) {
            const int na = packed_count_[gi];
            if (na == 0) return nullptr;
            if (na != kMaskFullGroup) gp = &packed_groups_[gi];
        }
        return gp;
    }
};

// ============================================================================
// Shape-specific batch kernels (called by XSectGroups, also usable directly)
// ============================================================================

namespace xsect_batch {

/**
 * @brief Batch area for CIRCULAR/FORCE_MAIN — lookup table interpolation.
 *
 * @details For each element i: area[i] = a_full[i] * lookup(depth[i]/y_full[i]).
 *          The inner loop is written for auto-vectorisation (no branches, no
 *          function calls except for the quadratic refinement at small depths).
 */
void area_circular(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT y_full,
    const double* OPENSWMM_RESTRICT a_full,
    double*       OPENSWMM_RESTRICT area,
    int count
);

/// Batch area for RECT_CLOSED / RECT_OPEN: area = depth * w_max.
void area_rect(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT w_max,
    double*       OPENSWMM_RESTRICT area,
    int count
);

/// Batch area for TRAPEZOIDAL: area = (y_bot + s_bot * depth) * depth.
void area_trapezoidal(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT y_bot,
    const double* OPENSWMM_RESTRICT s_bot,
    double*       OPENSWMM_RESTRICT area,
    int count
);

/// Batch area for TRIANGULAR: area = s_bot * depth^2.
void area_triangular(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT s_bot,
    double*       OPENSWMM_RESTRICT area,
    int count
);

/// Batch area for PARABOLIC: area = (4/3) * r_bot * depth^(3/2).
void area_parabolic(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT r_bot,
    double*       OPENSWMM_RESTRICT area,
    int count
);

/// Batch area for POWERFUNC: area = r_bot * depth^(s_bot+1).
void area_powerfunc(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT s_bot,
    const double* OPENSWMM_RESTRICT r_bot,
    double*       OPENSWMM_RESTRICT area,
    int count
);

/// Batch area for any tabulated shape (egg, horseshoe, arch, ellipse, etc.).
void area_tabulated(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT y_full,
    const double* OPENSWMM_RESTRICT a_full,
    const double* table,
    int            table_size,
    double*       OPENSWMM_RESTRICT area,
    int count
);

/// Batch area for shapes using invLookup (gothic, catenary, semielliptical, semicircular).
void area_inv_tabulated(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT y_full,
    const double* OPENSWMM_RESTRICT a_full,
    const double* table,
    int            table_size,
    double*       OPENSWMM_RESTRICT area,
    int count
);

// --- Hydraulic radius batch kernels ---

void hydrad_circular(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT y_full,
    const double* OPENSWMM_RESTRICT r_full,
    double*       OPENSWMM_RESTRICT hydrad,
    int count
);

/// Fused area + hydraulic radius for CIRCULAR/FORCE_MAIN (shared table index).
void area_hydrad_circular(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT y_full,
    const double* OPENSWMM_RESTRICT a_full,
    const double* OPENSWMM_RESTRICT r_full,
    double*       OPENSWMM_RESTRICT area,
    double*       OPENSWMM_RESTRICT hydrad,
    int count
);

void hydrad_trapezoidal(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT y_bot,
    const double* OPENSWMM_RESTRICT s_bot,
    const double* OPENSWMM_RESTRICT r_bot,
    double*       OPENSWMM_RESTRICT hydrad,
    int count
);

void hydrad_triangular(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT s_bot,
    const double* OPENSWMM_RESTRICT r_bot,
    double*       OPENSWMM_RESTRICT hydrad,
    int count
);

void hydrad_rect(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT w_max,
    double*       OPENSWMM_RESTRICT hydrad,
    int count
);

void hydrad_tabulated(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT y_full,
    const double* OPENSWMM_RESTRICT r_full,
    const double* table,
    int            table_size,
    double*       OPENSWMM_RESTRICT hydrad,
    int count
);

// --- Top width batch kernels ---

void width_circular(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT y_full,
    const double* OPENSWMM_RESTRICT w_max,
    double*       OPENSWMM_RESTRICT width,
    int count
);

void width_trapezoidal(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT y_bot,
    const double* OPENSWMM_RESTRICT s_bot,
    double*       OPENSWMM_RESTRICT width,
    int count
);

void width_triangular(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT s_bot,
    double*       OPENSWMM_RESTRICT width,
    int count
);

void width_rect(
    const double* OPENSWMM_RESTRICT w_max,
    double*       OPENSWMM_RESTRICT width,
    int count
);

void width_tabulated(
    const double* OPENSWMM_RESTRICT depth,
    const double* OPENSWMM_RESTRICT y_full,
    const double* OPENSWMM_RESTRICT w_max,
    const double* table,
    int            table_size,
    double*       OPENSWMM_RESTRICT width,
    int count
);

} // namespace xsect_batch

} // namespace openswmm

#endif // OPENSWMM_XSECT_BATCH_HPP
