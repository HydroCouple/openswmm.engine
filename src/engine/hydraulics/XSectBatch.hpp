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
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
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
    STREET_XSECT       = 25
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

    // Pre-allocated working buffers (avoids per-call allocation in hot loop)
    mutable std::vector<double> buf_d;   ///< Gather buffer for depths
    mutable std::vector<double> buf_r;   ///< Scatter buffer for results

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
