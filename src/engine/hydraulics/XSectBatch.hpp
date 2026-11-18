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
 * @file      XSectBatch.hpp
 * @brief     Cross-section geometry — unified batch + per-element API.
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
 *          groups.build(params, n_links);  // params holds resolved per-link geometry
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_XSECT_BATCH_HPP
#define OPENSWMM_XSECT_BATCH_HPP

#ifndef OPENSWMM_RESTRICT
#if defined(_MSC_VER)
#define OPENSWMM_RESTRICT __restrict
#else
#define OPENSWMM_RESTRICT __restrict__
#endif
#endif

#include <vector>
#include <cstddef>
#include <cstdint>
#include <cmath>

#include "XSectLookup.hpp"

namespace openswmm {

// Forward declaration
struct SimulationContext;

// ============================================================================
// Cross-section shape codes (matches legacy enums.h XsectType)
// ============================================================================

enum class XSectShape : int {
    DUMMY = 0,
    CIRCULAR = 1,
    FILLED_CIRCULAR = 2,
    RECT_CLOSED = 3,
    RECT_OPEN = 4,
    TRAPEZOIDAL = 5,
    TRIANGULAR = 6,
    PARABOLIC = 7,
    POWERFUNC = 8,
    RECT_TRIANG = 9,
    RECT_ROUND = 10,
    MOD_BASKET = 11,
    HORIZ_ELLIPSE = 12,
    VERT_ELLIPSE = 13,
    ARCH = 14,
    EGGSHAPED = 15,
    HORSESHOE = 16,
    GOTHIC = 17,
    CATENARY = 18,
    SEMIELLIPTICAL = 19,
    BASKETHANDLE = 20,
    SEMICIRCULAR = 21,
    IRREGULAR = 22,
    CUSTOM = 23,
    FORCE_MAIN = 24,
    STREET_XSECT = 25
};

// ============================================================================
// Cross-section parameter struct (mirrors legacy TXsect)
// ============================================================================

/**
 * @brief Native cross-section geometry and borrowed optional normalized transect tables.
 *
 * type uses kernel XSectShape ordinals, distinct from model-storage XsectShape. Lengths are ft,
 * areas ft2. Ordinary section factor A*R^(2/3) has units ft^(8/3); FORCE_MAIN setParams stores its
 * Hazen-Williams full factor A*R^0.63 instead. Bottom fields are shape-dependent and must be
 * initialized together. Table/LUT pointers are non-owning; their backing arrays must remain
 * live/unmoved during all queries. Copying this struct copies those borrowed pointers.
 */
struct XSectParams {
    int type = 0;
    int culvert_code = 0;
    int transect = -1;

    double y_full = 0.0; ///< Full depth (ft)
    double w_max = 0.0;  ///< Width at widest point (ft)
    double yw_max = 0.0; ///< Depth at widest point (ft)
    double a_full = 0.0; ///< Area when full (ft2)
    double r_full = 0.0; ///< Hydraulic radius when full (ft)
    double s_full = 0.0; ///< Section factor when full (normally ft^(8/3); FORCE_MAIN uses R^0.63)
    double s_max = 0.0;  ///< Section factor at max flow (same convention as s_full)

    double y_bot = 0.0; ///< Depth of bottom section / fill depth
    double a_bot = 0.0; ///< Area of bottom section
    double s_bot = 0.0; ///< Slope of bottom section / exponent
    double r_bot = 0.0; ///< Radius of bottom section / coefficient

    // Tabulated geometry for IRREGULAR / CUSTOM / STREET_XSECT shapes, whose
    // A/R/W vs depth come from a per-link transect table rather than a shared
    // static table.  These point into ctx.transect_tables (stable for the run)
    // and stay null for every self-contained shape.  Without them the
    // context-free per-element accessors return 0 for irregular sections (so
    // e.g. init-time getDepthFromFlow could not compute normal depth/storage).
    const double* area_tbl = nullptr;  ///< Normalized area      vs y/y_full
    const double* hrad_tbl = nullptr;  ///< Normalized hyd-radius vs y/y_full
    const double* width_tbl = nullptr; ///< Normalized width     vs y/y_full
    int transect_tbl_size = 0;         ///< Entry count of the tables above

    /// Bucket map over `area_tbl` for the getYofA inversion (plan A1). Points
    /// into the owning TransectData, so it has the same lifetime as area_tbl
    /// and is null whenever that is.
    const xsect::LocateLut* area_lut = nullptr;
};

// ============================================================================
// Per-element functions (xsect:: namespace)
// ============================================================================

namespace xsect {

/**
 * @brief Query native wetted area from depth through the shared host evaluator.
 *
 * Nonpositive depth/full depth returns0; tabulated shapes hold endpoints, analytic open profiles
 * can extend beyond full depth. No display-unit conversion, allocation on normal query path or
 * ownership transfer.
 *
 * @param[in] xs Borrowed initialized native geometry with kernel shape numbering and live
 *     applicable tables/LUTs; queried physical shape must be supported.
 * @param[in] y Finite nonnegative depth ft.
 * @return Computed ft2.
 */
double getAofY(const XSectParams& xs, double y);
/**
 * @brief Query native hydraulic radius from depth through the shared host evaluator.
 *
 * Requires valid positive full geometry; optional missing radius table returns0. Unsupported/DUMMY
 * positive-area radius queries are outside the valid domain. No display-unit conversion, allocation
 * on normal query path or ownership transfer.
 *
 * @param[in] xs Borrowed initialized native geometry with kernel shape numbering and live
 *     applicable tables/LUTs; queried physical shape must be supported.
 * @param[in] y Finite nonnegative depth ft.
 * @return Computed ft.
 */
double getRofY(const XSectParams& xs, double y);
/**
 * @brief Query native top width from depth through the shared host evaluator.
 *
 * Shape-specific dry/crown rules apply; closed rectangle returns0 at exact full depth. Missing
 * optional width table/unsupported code returns0. No display-unit conversion, allocation on normal
 * query path or ownership transfer.
 *
 * @param[in] xs Borrowed initialized native geometry with kernel shape numbering and live
 *     applicable tables/LUTs; queried physical shape must be supported.
 * @param[in] y Finite nonnegative depth ft.
 * @return Computed ft.
 */
double getWofY(const XSectParams& xs, double y);
/**
 * @brief Query native depth from area through the shared host evaluator.
 *
 * Nonpositive area returns0. Uses analytic inverses, direct Y tables or inversion of borrowed A
 * tables; no universal above-full clamp. No display-unit conversion, allocation on normal query
 * path or ownership transfer.
 *
 * @param[in] xs Borrowed initialized native geometry with kernel shape numbering and live
 *     applicable tables/LUTs; queried physical shape must be supported.
 * @param[in] a Finite nonnegative wetted area ft2.
 * @return Computed ft.
 */
double getYofA(const XSectParams& xs, double a);
/**
 * @brief Query native section factor from area through the shared host evaluator.
 *
 * Ordinary units are ft^(8/3), with the FORCE_MAIN full-factor convention described on XSectParams.
 * Uses specialized S tables/crown corrections or A*R^(2/3); valid non-DUMMY supported geometry is
 * required. No display-unit conversion, allocation on normal query path or ownership transfer.
 *
 * @param[in] xs Borrowed initialized native geometry with kernel shape numbering and live
 *     applicable tables/LUTs; queried physical shape must be supported.
 * @param[in] a Finite nonnegative wetted area ft2.
 * @return Computed native section-factor units.
 */
double getSofA(const XSectParams& xs, double a);
/**
 * @brief Query native hydraulic radius from area through the shared host evaluator.
 *
 * Nonpositive area returns0; unsupported/DUMMY positive area is invalid for generic recursive
 * radius dispatch. No display-unit conversion, allocation on normal query path or ownership
 * transfer.
 *
 * @param[in] xs Borrowed initialized native geometry with kernel shape numbering and live
 *     applicable tables/LUTs; queried physical shape must be supported.
 * @param[in] a Finite nonnegative wetted area ft2.
 * @return Computed ft.
 */
double getRofA(const XSectParams& xs, double a);
/**
 * @brief Query native section-factor derivative with respect to area through the shared host
 * evaluator.
 *
 * Uses analytic/table-segment derivative or normalized-area finite difference; no complete
 * area/finiteness validation. No display-unit conversion, allocation on normal query path or
 * ownership transfer.
 *
 * @param[in] xs Borrowed initialized native geometry with kernel shape numbering and live
 *     applicable tables/LUTs; queried physical shape must be supported.
 * @param[in] a Finite nonnegative wetted area ft2.
 * @return Computed section-factor units per ft2.
 */
double getdSdA(const XSectParams& xs, double a);
/**
 * @brief Query native area from section factor through the shared host evaluator.
 *
 * Nonpositive factor/DUMMY returns0; table inverse or Newton candidate is returned without a
 * convergence flag. Direct-table normalized input precedes local s_max cap, preserving current
 * legacy arithmetic. No display-unit conversion, allocation on normal query path or ownership
 * transfer.
 *
 * @param[in] xs Borrowed initialized native geometry with kernel shape numbering and live
 *     applicable tables/LUTs; queried physical shape must be supported.
 * @param[in] s_factor Finite nonnegative factor in xs.s_full conventions.
 * @return Computed ft2.
 */
double getAofS(const XSectParams& xs, double s_factor);
/**
 * @brief Read the built-in area-at-maximum-conveyance ratio.
 *
 * Returns a ratio rather than the physical custom/irregular xs.a_bot area.
 *
 * @param[in] xs Borrowed initialized native geometry with kernel shape numbering and live
 *     applicable tables/LUTs; queried physical shape must be supported.
 * @return Dimensionless built-in ratio for kernel codes0..25, otherwise1.
 */
double getAmax(const XSectParams& xs);
/**
 * @brief Query native critical depth from discharge through the shared host evaluator.
 *
 * Nonpositive discharge/DUMMY returns0. Uses analytic,25-interval enumeration or Ridder solve;
 * upper depth capped y_full and no separate root-failure status. No display-unit conversion,
 * allocation on normal query path or ownership transfer.
 *
 * @param[in] xs Borrowed initialized native geometry with kernel shape numbering and live
 *     applicable tables/LUTs; queried physical shape must be supported.
 * @param[in] q Finite nonnegative discharge cfs; caller prevents q*q overflow.
 * @return Computed ft.
 */
double getYcrit(const XSectParams& xs, double q);
/**
 * @brief Classify a kernel shape using the legacy open-section set.
 *
 * @param[in] type Kernel XSectShape ordinal, not model-storage enum.
 * @return True for
 *     DUMMY/RECT_OPEN/TRAPEZOIDAL/TRIANGULAR/PARABOLIC/POWERFUNC/IRREGULAR/STREET_XSECT; false
 *     otherwise.
 */
bool isOpen(int type);
/**
 * @brief Derive native full-flow geometry from authored shape dimensions.
 *
 * Trapezoid uses p1 bottom width,p2/p3 side slopes; triangle/parabola/power use height/width with
 * power exponent p2. Standard ellipse/arch code selects inch-based catalogue dimensions; otherwise
 * custom dimensions apply. Arc radius is raised to at least width/2. FORCE_MAIN retains p1 as
 * C/roughness without conversion and uses R^0.63 full factor. This is not complete validation: many
 * nonpositive/nonfinite dimensions can return0, and -1 can leave partial fields changed.
 *
 * @param[in,out] xs Borrowed destination; type is assigned first, applicable geometry fields change
 *     incrementally and unrelated fields/pointers are retained.
 * @param[in] type Kernel XSectShape ordinal. Table-backed IRREGULAR/CUSTOM/STREET, DUMMY and
 *     unknown codes change only type and return0.
 * @param[in] p Readable four-double shape parameter span. Slot0 is height/diameter or standard-size
 *     code; slot1 width, sediment depth or FORCE_MAIN roughness; slot2 exponent, arc radius,
 *     excluded-bank count or standard ellipse/arch code; slot3 second trapezoid side slope. Caller
 *     supplies shape-valid finite values.
 * @param[in] ucf Positive finite authored-length-units per native ft; length slots divide by it,
 *     dimensionless slopes/codes/exponents remain unscaled.
 * @return 0 after a handled/default branch, -1 for selected fill/rectangle/arc/standard-size
 *     constraints.
 */
int setParams(XSectParams& xs, int type, const double p[], double ucf);

// Lookup table helpers (exposed for batch kernels and testing)
/**
 * @brief Interpolate a borrowed equally spaced normalized geometry table.
 *
 * @param[in] x Normalized coordinate; nonpositive/NaN selects first, >1 selects final entry.
 * @param[in] table Readable n_items-entry table, n_items>=4 for low-end quadratic lookahead.
 * @param[in] n_items Logical count>=4.
 * @return Table-unit value using the exact legacy shared lookup, with lower-result floor0 for
 *     interior interpolation.
 */
double lookup(double x, const double* table, int n_items);
/**
 * @brief Invert a borrowed normalized geometry table including the legacy declining top.
 *
 * @param[in] y Finite dependent table-unit value.
 * @param[in] table Readable n_items entries, monotone except the supported two-row declining top.
 * @param[in] n_items Logical count>=4.
 * @param[in] lut Optional live map built for the same unchanged table/search extent.
 * @return Normalized coordinate clamped0..1; no pointer/count/finiteness validation.
 */
double invLookup(double y, const double* table, int n_items, const LocateLut* lut = nullptr);
/**
 * @brief Find a lower bracket in a nondecreasing geometry table.
 *
 * @param[in] y Query in table units, endpoint held; NaN resolves index0.
 * @param[in] table Readable nondecreasing indices0..n.
 * @param[in] n Nonnegative inclusive last index, rather than entry count.
 * @return Bracket index clamped0..n.
 */
int locate(double y, const double* table, int n);
/**
 * @brief Compute normalized circular depth from area ratio.
 *
 * @param[in] alpha Finite area/full-area ratio; <=0 returns0, >=1 returns1.
 * @return Depth/full-depth via small-area approximation or angle Newton solve; iteration exhaustion
 *     uses empirical angle guess.
 */
double getYcircular(double alpha);
/**
 * @brief Compute normalized circular section factor from area ratio.
 *
 * @param[in] alpha Finite area/full-area ratio; <=0 returns0, >=1 returns1.
 * @return S/Sfull via small-area approximation or angle formula; no separate convergence status.
 */
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
    int count = 0;

    // Mapping back to global link arrays
    std::vector<int> link_idx; ///< link_idx[i] = index in SimulationContext

    // Geometry parameters (contiguous, aligned for SIMD)
    std::vector<double> y_full; ///< Full depth (ft)
    std::vector<double> a_full; ///< Full area (ft2)
    std::vector<double> r_full; ///< Hyd. radius at full (ft)
    std::vector<double> s_full; ///< Section factor at full
    std::vector<double> w_max;  ///< Max width (ft)

    // Pre-computed reciprocal of y_full (avoids per-element division in kernels)
    std::vector<double> inv_y_full; ///< 1.0 / y_full (or 0 if y_full==0)

    // Multi-purpose parameters (meaning depends on shape)
    std::vector<double> y_bot;
    std::vector<double> a_bot;
    std::vector<double> s_bot;
    std::vector<double> r_bot;

    // Per-link transect table pointers (IRREGULAR shapes only)
    // Each pointer → a normalized table of N_TRANSECT_TBL entries.
    std::vector<const double*> area_tables;  ///< Per-link area table
    std::vector<const double*> hrad_tables;  ///< Per-link hyd-rad table
    std::vector<const double*> width_tables; ///< Per-link width table
    int transect_tbl_size = 0;               ///< Table size (same for all)

    // Pre-allocated working buffers (avoids per-call allocation in hot loop)
    mutable std::vector<double> buf_d;  ///< Gather buffer for depths
    mutable std::vector<double> buf_r;  ///< Scatter buffer for results
    mutable std::vector<double> buf_r2; ///< Second scatter buffer (for fused ops)

    /**
     * @brief Resize primary shape-group geometry and numerical work arrays.
     *
     * Sets count and resizes link indices, scalar geometry and three scratch vectors, preserving
     * existing prefixes. Does not resize/clear optional per-link table pointer vectors. Allocation may
     * leave partially resized arrays; borrowed element views may invalidate.
     *
     * @param[in] n Nonnegative element count; negative is not checked before size_t conversion.
     */
    void resize(int n);
};

// ============================================================================
// XSectGroups — the shape-grouped index over all links
// ============================================================================

/**
 * @brief Shape-grouped cross-section manager for batch computation.
 *
 * @details Call `build(params, n_links)` once after the model is loaded/built. This sorts
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
     * @brief Declare the context convenience overload for shape-group construction.
     *
     * No definition of this overload is supplied in the current engine sources; calling it requires a
     * separate implementation at link time. Use build(params,n_links), then attachTransectTables(ctx),
     * in the implemented routing path.
     *
     * @param[in] ctx Borrowed model context described by this interface declaration.
     */
    void build(const SimulationContext& ctx);

    /**
     * @brief Attach borrowed profile tables to irregular, custom and street shape groups.
     *
     * Call after build. Clears packed/mask state, fills valid-index area/radius/width pointers and
     * selected full dimensions, then stable-sorts each eligible group by transect index. Invalid
     * indices retain existing/default pointer slots rather than diagnosing. Does not update every
     * section-factor scalar; no ownership transfer. Allocation can leave partial
     * attachment/permutation.
     *
     * @param[in] ctx Borrowed coherent link curve indices and completed transect_tables; their vector
     *     elements must remain unmoved/live throughout query use.
     */
    void attachTransectTables(const SimulationContext& ctx);

    /**
     * @brief Replace shape groups from copied per-link native geometry.
     *
     * Clears prior groups/packed masks, orders groups by shape code and rows by link order, copies
     * scalar parameters and positive-depth reciprocals. Does not copy optional transect pointers from
     * params; attachTransectTables must supply them. No full geometry validation; allocation can leave
     * partial construction and invalidates borrowed group references.
     *
     * @param[in] params Readable n_links-entry parameter span; NULL permitted when n_links<=0. Kernel
     *     type codes1..25 group, DUMMY/unknown skip.
     * @param[in] n_links Nonnegative link count; negative currently clears groups and performs no input
     *     reads.
     */
    void build(const XSectParams* params, int n_links);

    // ========================================================================
    // Batch compute — results scattered to global link arrays
    // ========================================================================

    /**
     * @brief Compute grouped-link area ft2 in native units.
     *
     * Ignores the triple-only bypass mask. Mutable scratch makes simultaneous calls on one manager
     * unsafe; source/destination spans must not overlap. Runs gather/kernel/scatter over built groups
     * without per-call allocation; borrowed profile tables must remain live.
     *
     * @param[in] depths Readable global depth ft span covering every grouped link index.
     * @param[out] areas Writable global area ft2 span; ungrouped/DUMMY entries are not initialized.
     * @param[in] n_links Historical total count, currently ignored; does not enforce buffer bounds.
     */
    void computeAreas(const double* depths, double* areas, int n_links) const;

    /**
     * @brief Compute grouped-link radius ft in native units.
     *
     * Ignores the triple-only bypass mask. Mutable scratch makes simultaneous calls on one manager
     * unsafe; source/destination spans must not overlap. Runs gather/kernel/scatter over built groups
     * without per-call allocation; borrowed profile tables must remain live.
     *
     * @param[in] depths Readable global depth ft span covering every grouped link index.
     * @param[out] hydrad Writable global radius ft span; ungrouped/DUMMY entries are not initialized.
     * @param[in] n_links Historical total count, currently ignored; does not enforce buffer bounds.
     */
    void computeHydRad(const double* depths, double* hydrad, int n_links) const;

    /**
     * @brief Compute grouped-link width ft in native units.
     *
     * Ignores the triple-only bypass mask. Mutable scratch makes simultaneous calls on one manager
     * unsafe; source/destination spans must not overlap. Runs gather/kernel/scatter over built groups
     * without per-call allocation; borrowed profile tables must remain live.
     *
     * @param[in] depths Readable global depth ft span covering every grouped link index.
     * @param[out] widths Writable global width ft span; ungrouped/DUMMY entries are not initialized.
     * @param[in] n_links Historical total count, currently ignored; does not enforce buffer bounds.
     */
    void computeWidths(const double* depths, double* widths, int n_links) const;

    /**
     * @brief Compute grouped-link area ft2,radius ft in native units.
     *
     * Ignores the triple-only bypass mask. Mutable scratch makes simultaneous calls on one manager
     * unsafe; source/destination spans must not overlap. Runs gather/kernel/scatter over built groups
     * without per-call allocation; borrowed profile tables must remain live.
     *
     * @param[in] depths Readable global depth ft span covering every grouped link index.
     * @param[out] areas Writable global area ft2 span; ungrouped/DUMMY entries are not initialized.
     * @param[out] hydrad Writable global radius ft span; ungrouped/DUMMY entries are not initialized.
     * @param[in] n_links Historical total count, currently ignored; does not enforce buffer bounds.
     */
    void computeAreaAndHydRad(const double* depths, double* areas, double* hydrad,
                              int n_links) const;

    /**
     * @brief Compute area/radius for three depth fields using one grouped pass.
     *
     * Uses mutable preallocated gather/scatter scratch; source/destination arrays must not overlap, and
     * independent calls on one group manager must not run concurrently. Honors the current packed
     * bypass mask. Serial wrapper runs worker0 of1; does not create an OpenMP team.
     *
     * @param[in] d1 Readable global link-depth span in ft; every active grouped index must be in range.
     * @param[in] d2 Readable global link-depth span in ft; every active grouped index must be in range.
     * @param[in] dm Readable global link-depth span in ft; every active grouped index must be in range.
     * @param[out] a1 Writable global link span: ft2 area for d1; bypassed/ungrouped outputs are
     *     preserved.
     * @param[out] a2 Writable global link span: ft2 area for d2; bypassed/ungrouped outputs are
     *     preserved.
     * @param[out] am Writable global link span: ft2 area for dm; bypassed/ungrouped outputs are
     *     preserved.
     * @param[out] hrad1 Writable global link span: ft radius for d1; bypassed/ungrouped outputs are
     *     preserved.
     * @param[out] hrad_mid Writable global link span: ft radius for dm; bypassed/ungrouped outputs are
     *     preserved.
     * @param[in] n_links Historical count argument; currently ignored, so spans must cover all stored
     *     global group indices regardless of this value.
     */
    void computeAreaHydRadTriple(const double* d1, const double* d2, const double* dm, double* a1,
                                 double* a2, double* am, double* hrad1, double* hrad_mid,
                                 int n_links) const;

    /**
     * @brief Compute width for three depth fields using one grouped pass.
     *
     * Uses mutable preallocated gather/scatter scratch; source/destination arrays must not overlap, and
     * independent calls on one group manager must not run concurrently. Honors the current packed
     * bypass mask. Serial wrapper runs worker0 of1; does not create an OpenMP team.
     *
     * @param[in] d1 Readable global link-depth span in ft; every active grouped index must be in range.
     * @param[in] d2 Readable global link-depth span in ft; every active grouped index must be in range.
     * @param[in] dm Readable global link-depth span in ft; every active grouped index must be in range.
     * @param[out] w1 Writable global top-width span in ft for d1; bypassed/ungrouped outputs preserved.
     * @param[out] w2 Writable global top-width span in ft for d2; bypassed/ungrouped outputs preserved.
     * @param[out] wm Writable global top-width span in ft for dm; bypassed/ungrouped outputs preserved.
     * @param[in] n_links Historical count argument; currently ignored, so spans must cover all stored
     *     global group indices regardless of this value.
     */
    void computeWidthsTriple(const double* d1, const double* d2, const double* dm, double* w1,
                             double* w2, double* wm, int n_links) const;

    /**
     * @brief Compute area/radius for three depth fields on one caller-owned team slice.
     *
     * Uses mutable preallocated gather/scatter scratch; source/destination arrays must not overlap, and
     * independent calls on one group manager must not run concurrently. Honors the current packed
     * bypass mask. Static cache-line-aligned slices are disjoint; every team member must use the same
     * arrays/mask and wait before consumers read outputs.
     *
     * @param[in] d1 Readable global link-depth span in ft; every active grouped index must be in range.
     * @param[in] d2 Readable global link-depth span in ft; every active grouped index must be in range.
     * @param[in] dm Readable global link-depth span in ft; every active grouped index must be in range.
     * @param[out] a1 Writable global link span: ft2 area for d1; bypassed/ungrouped outputs are
     *     preserved.
     * @param[out] a2 Writable global link span: ft2 area for d2; bypassed/ungrouped outputs are
     *     preserved.
     * @param[out] am Writable global link span: ft2 area for dm; bypassed/ungrouped outputs are
     *     preserved.
     * @param[out] hrad1 Writable global link span: ft radius for d1; bypassed/ungrouped outputs are
     *     preserved.
     * @param[out] hrad_mid Writable global link span: ft radius for dm; bypassed/ungrouped outputs are
     *     preserved.
     * @param[in] n_links Historical count argument; currently ignored, so spans must cover all stored
     *     global group indices regardless of this value.
     * @param[in] tid This worker index,0<=tid<effective nthreads; caller validates.
     * @param[in] nthreads Team size; values<1 become1. Caller invokes every worker and owns completion
     *     barrier.
     */
    void computeAreaHydRadTripleTeam(const double* d1, const double* d2, const double* dm,
                                     double* a1, double* a2, double* am, double* hrad1,
                                     double* hrad_mid, int n_links, int tid, int nthreads) const;

    /**
     * @brief Compute width for three depth fields on one caller-owned team slice.
     *
     * Uses mutable preallocated gather/scatter scratch; source/destination arrays must not overlap, and
     * independent calls on one group manager must not run concurrently. Honors the current packed
     * bypass mask. Static cache-line-aligned slices are disjoint; every team member must use the same
     * arrays/mask and wait before consumers read outputs.
     *
     * @param[in] d1 Readable global link-depth span in ft; every active grouped index must be in range.
     * @param[in] d2 Readable global link-depth span in ft; every active grouped index must be in range.
     * @param[in] dm Readable global link-depth span in ft; every active grouped index must be in range.
     * @param[out] w1 Writable global top-width span in ft for d1; bypassed/ungrouped outputs preserved.
     * @param[out] w2 Writable global top-width span in ft for d2; bypassed/ungrouped outputs preserved.
     * @param[out] wm Writable global top-width span in ft for dm; bypassed/ungrouped outputs preserved.
     * @param[in] n_links Historical count argument; currently ignored, so spans must cover all stored
     *     global group indices regardless of this value.
     * @param[in] tid This worker index,0<=tid<effective nthreads; caller validates.
     * @param[in] nthreads Team size; values<1 become1. Caller invokes every worker and owns completion
     *     barrier.
     */
    void computeWidthsTripleTeam(const double* d1, const double* d2, const double* dm, double* w1,
                                 double* w2, double* wm, int n_links, int tid, int nthreads) const;

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
     * @brief Compute grouped-link native section factor in native units.
     *
     * Ignores the triple-only bypass mask. Mutable scratch makes simultaneous calls on one manager
     * unsafe; source/destination spans must not overlap. Uses scalar paramsAt fallback, which omits
     * optional profile pointers/s_max; this path does not reconstruct all table-backed/crown metadata.
     *
     * @param[in] areas Readable global area ft2 span covering every grouped link index.
     * @param[out] sfact Writable global native section factor span; ungrouped/DUMMY entries are not
     *     initialized.
     * @param[in] n_links Historical total count, currently ignored; does not enforce buffer bounds.
     */
    void computeSectionFactors(const double* areas, double* sfact, int n_links) const;

    /**
     * @brief Compute grouped-link depth ft in native units.
     *
     * Ignores the triple-only bypass mask. Mutable scratch makes simultaneous calls on one manager
     * unsafe; source/destination spans must not overlap. Uses scalar paramsAt fallback, which omits
     * optional profile pointers/s_max; this path does not reconstruct all table-backed/crown metadata.
     *
     * @param[in] areas Readable global area ft2 span covering every grouped link index.
     * @param[out] depths Writable global depth ft span; ungrouped/DUMMY entries are not initialized.
     * @param[in] n_links Historical total count, currently ignored; does not enforce buffer bounds.
     */
    void computeDepthsFromArea(const double* areas, double* depths, int n_links) const;

    // ========================================================================
    // Accessors
    // ========================================================================

    /// Number of non-empty shape groups.
    int numGroups() const { return static_cast<int>(groups_.size()); }

    /**
     * @brief Borrow one indexed shape group.
     *
     * @param[in] i Zero-based index<numGroups(); unchecked.
     * @return Owned group reference, invalidated by build or vector changes; table data remains
     *     externally borrowed.
     */
    const ShapeGroup& group(int i) const { return groups_[i]; }

    /**
     * @brief Borrow the first group with the requested kernel shape.
     *
     * @param[in] shape Kernel enum; absent/unknown/DUMMY normally has no group.
     * @return Borrowed group pointer or NULL; invalidated by build/destruction.
     */
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
    mutable std::vector<int> packed_count_;
    mutable bool mask_active_ = false;

    /**
     * @brief Select the original, packed or empty group under the current bypass mask.
     *
     * Returned pointer remains valid only until build/attach/mask mutation/destruction; no allocation
     * or packing occurs here.
     *
     * @param[in] gi Valid index into groups and corresponding packed metadata when mask_active;
     *     unchecked.
     * @return Borrowed group view or NULL when every link is bypassed.
     */
    const ShapeGroup* maskedGroup(std::size_t gi) const {
        const ShapeGroup* gp = &groups_[gi];
        if (mask_active_) {
            const int na = packed_count_[gi];
            if (na == 0)
                return nullptr;
            if (na != kMaskFullGroup)
                gp = &packed_groups_[gi];
        }
        return gp;
    }
};

// ============================================================================
// Shape-specific batch kernels (called by XSectGroups, also usable directly)
// ============================================================================

namespace xsect_batch {

/**
 * @brief Evaluate circular/force-main normalized area for a contiguous batch.
 *
 * Uses a_full*A_Circ(depth-normalized); nonpositive default-mode depth normalizes0 and area table
 * starts0. Caller validates spans/geometry/finiteness; no allocation, ownership transfer or
 * per-element error status. Restrict-qualified source/destination spans must not overlap; NULL
 * numeric spans are permitted only at count0.
 *
 * @param[in] depth Readable count-entry finite nonnegative water depths in ft.
 * @param[in] y_full Readable count-entry normalization operands: full depth ft in default build,
 *     reciprocal1/ft under SWMM_XSECT_FAST_LOOKUP. The historical y_full parameter name does not
 *     change with build mode.
 * @param[in] a_full Readable count-entry positive full areas ft2.
 * @param[out] area Writable count-entry result span, area ft2.
 * @param[in] count Nonnegative batch length; each non-NULL span must cover this many entries;0
 *     performs no element reads/writes.
 */
void area_circular(const double* OPENSWMM_RESTRICT depth, const double* OPENSWMM_RESTRICT y_full,
                   const double* OPENSWMM_RESTRICT a_full, double* OPENSWMM_RESTRICT area,
                   int count);

/**
 * @brief Multiply rectangular depths and widths using the shared SIMD routine.
 *
 * Computes depth*w_max without full-depth or sign clamps. Count must be nonnegative before size_t
 * conversion. Caller validates spans/geometry/finiteness; no allocation, ownership transfer or
 * per-element error status. Restrict-qualified source/destination spans must not overlap; NULL
 * numeric spans are permitted only at count0.
 *
 * @param[in] depth Readable count-entry finite nonnegative water depths in ft.
 * @param[in] w_max Readable count-entry positive widths ft.
 * @param[out] area Writable count-entry result span, area ft2.
 * @param[in] count Nonnegative batch length; each non-NULL span must cover this many entries;0
 *     performs no element reads/writes.
 */
void area_rect(const double* OPENSWMM_RESTRICT depth, const double* OPENSWMM_RESTRICT w_max,
               double* OPENSWMM_RESTRICT area, int count);

/**
 * @brief Evaluate trapezoid batch area through the shared analytic leaf.
 *
 * Computes (bottom width+average side slope*depth)*depth, preserving leaf operation order. Caller
 * validates spans/geometry/finiteness; no allocation, ownership transfer or per-element error
 * status. Restrict-qualified source/destination spans must not overlap; NULL numeric spans are
 * permitted only at count0.
 *
 * @param[in] depth Readable count-entry finite nonnegative water depths in ft.
 * @param[in] y_bot Readable count-entry nonnegative trapezoid bottom widths ft.
 * @param[in] s_bot Readable count-entry average horizontal/vertical side slopes.
 * @param[out] area Writable count-entry result span, area ft2.
 * @param[in] count Nonnegative batch length; each non-NULL span must cover this many entries;0
 *     performs no element reads/writes.
 */
void area_trapezoidal(const double* OPENSWMM_RESTRICT depth, const double* OPENSWMM_RESTRICT y_bot,
                      const double* OPENSWMM_RESTRICT s_bot, double* OPENSWMM_RESTRICT area,
                      int count);

/**
 * @brief Evaluate triangular batch area through the shared analytic leaf.
 *
 * Computes depth*depth*side slope without full-depth clamping. Caller validates
 * spans/geometry/finiteness; no allocation, ownership transfer or per-element error status.
 * Restrict-qualified source/destination spans must not overlap; NULL numeric spans are permitted
 * only at count0.
 *
 * @param[in] depth Readable count-entry finite nonnegative water depths in ft.
 * @param[in] s_bot Readable count-entry average horizontal/vertical side slopes.
 * @param[out] area Writable count-entry result span, area ft2.
 * @param[in] count Nonnegative batch length; each non-NULL span must cover this many entries;0
 *     performs no element reads/writes.
 */
void area_triangular(const double* OPENSWMM_RESTRICT depth, const double* OPENSWMM_RESTRICT s_bot,
                     double* OPENSWMM_RESTRICT area, int count);

/**
 * @brief Evaluate parabolic batch area through the shared analytic leaf.
 *
 * Computes(4/3)*r_bot*depth*sqrt(depth); nonnegative depth is required. Caller validates
 * spans/geometry/finiteness; no allocation, ownership transfer or per-element error status.
 * Restrict-qualified source/destination spans must not overlap; NULL numeric spans are permitted
 * only at count0.
 *
 * @param[in] depth Readable count-entry finite nonnegative water depths in ft.
 * @param[in] r_bot Readable count-entry positive inverse sqrt(parabola coefficient), sqrt(ft).
 * @param[out] area Writable count-entry result span, area ft2.
 * @param[in] count Nonnegative batch length; each non-NULL span must cover this many entries;0
 *     performs no element reads/writes.
 */
void area_parabolic(const double* OPENSWMM_RESTRICT depth, const double* OPENSWMM_RESTRICT r_bot,
                    double* OPENSWMM_RESTRICT area, int count);

/**
 * @brief Evaluate power-function batch area through the shared analytic leaf.
 *
 * Computes r_bot*depth^(s_bot+1); caller establishes valid positive coefficients/power domain.
 * Caller validates spans/geometry/finiteness; no allocation, ownership transfer or per-element
 * error status. Restrict-qualified source/destination spans must not overlap; NULL numeric spans
 * are permitted only at count0.
 *
 * @param[in] depth Readable count-entry finite nonnegative water depths in ft.
 * @param[in] s_bot Readable count-entry positive depth exponents (inverse authored exponent).
 * @param[in] r_bot Readable count-entry positive area coefficient ft^(1-s_bot).
 * @param[out] area Writable count-entry result span, area ft2.
 * @param[in] count Nonnegative batch length; each non-NULL span must cover this many entries;0
 *     performs no element reads/writes.
 */
void area_powerfunc(const double* OPENSWMM_RESTRICT depth, const double* OPENSWMM_RESTRICT s_bot,
                    const double* OPENSWMM_RESTRICT r_bot, double* OPENSWMM_RESTRICT area,
                    int count);

/**
 * @brief Scale one shared normalized area table for a batch.
 *
 * Computes a_full*norm_lookup for each element, preserving exact or opted-in fast arithmetic.
 * Caller validates spans/geometry/finiteness; no allocation, ownership transfer or per-element
 * error status. Restrict-qualified source/destination spans must not overlap; NULL numeric spans
 * are permitted only at count0.
 *
 * @param[in] depth Readable count-entry finite nonnegative water depths in ft.
 * @param[in] y_full Readable count-entry normalization operands: full depth ft in default build,
 *     reciprocal1/ft under SWMM_XSECT_FAST_LOOKUP. The historical y_full parameter name does not
 *     change with build mode.
 * @param[in] a_full Readable count-entry positive full areas ft2.
 * @param[in] table Non-NULL borrowed readable table_size-entry shared normalized table; proper
 *     monotonicity required for inverse lookup.
 * @param[in] table_size Shared logical table length>=4 for quadratic lookup; table grid is equally
 *     spaced over normalized depth/area0..1.
 * @param[out] area Writable count-entry result span, area ft2.
 * @param[in] count Nonnegative batch length; each non-NULL span must cover this many entries;0
 *     performs no element reads/writes.
 */
void area_tabulated(const double* OPENSWMM_RESTRICT depth, const double* OPENSWMM_RESTRICT y_full,
                    const double* OPENSWMM_RESTRICT a_full, const double* table, int table_size,
                    double* OPENSWMM_RESTRICT area, int count);

/**
 * @brief Scale separate normalized profile tables for each batch element.
 *
 * Missing per-element table pointer writes0; other entries use norm_lookup. Used for
 * area/radius/width with corresponding scale units. Caller validates spans/geometry/finiteness; no
 * allocation, ownership transfer or per-element error status. Restrict-qualified source/destination
 * spans must not overlap; NULL numeric spans are permitted only at count0.
 *
 * @param[in] depth Readable count-entry finite nonnegative water depths in ft.
 * @param[in] nrm Readable count-entry normalization operands: full depth ft in default build,
 *     reciprocal1/ft under SWMM_XSECT_FAST_LOOKUP. The historical y_full parameter name does not
 *     change with build mode.
 * @param[in] scale Readable count-entry dimensional multipliers; output inherits their units
 *     (typically ft2 area or ft radius/width).
 * @param[in] tables Readable count-pointer array; each non-NULL borrowed table has table_size
 *     entries on the same normalized grid, NULL selects zero output.
 * @param[in] table_size Shared logical table length>=4 for quadratic lookup; table grid is equally
 *     spaced over normalized depth/area0..1.
 * @param[out] result Writable count-entry result span, in corresponding scale-array units.
 * @param[in] count Nonnegative batch length; each non-NULL span must cover this many entries;0
 *     performs no element reads/writes.
 */
void perlink_tabulated(const double* OPENSWMM_RESTRICT depth, const double* OPENSWMM_RESTRICT nrm,
                       const double* OPENSWMM_RESTRICT scale, const double* const* tables,
                       int table_size, double* OPENSWMM_RESTRICT result, int count);

/**
 * @brief Fuse two per-element profile lookups on one normalized depth grid.
 *
 * Shares index/interpolation weights while applying independent quadratic refinements and lower
 * clamps. Missing either pointer evaluates the available channel separately and writes0 for the
 * missing one. Default arithmetic matches two exact passes; FAST_LOOKUP permits ULP differences.
 * Caller validates spans/geometry/finiteness; no allocation, ownership transfer or per-element
 * error status. Restrict-qualified source/destination spans must not overlap; NULL numeric spans
 * are permitted only at count0.
 *
 * @param[in] depth Readable count-entry finite nonnegative water depths in ft.
 * @param[in] nrm Readable count-entry normalization operands: full depth ft in default build,
 *     reciprocal1/ft under SWMM_XSECT_FAST_LOOKUP. The historical y_full parameter name does not
 *     change with build mode.
 * @param[in] scale_a Readable count-entry dimensional multipliers; output inherits their units
 *     (typically ft2 area or ft radius/width).
 * @param[in] scale_b Readable count-entry dimensional multipliers; output inherits their units
 *     (typically ft2 area or ft radius/width).
 * @param[in] tables_a Readable count-pointer array; each non-NULL borrowed table has table_size
 *     entries on the same normalized grid, NULL selects zero output.
 * @param[in] tables_b Readable count-pointer array; each non-NULL borrowed table has table_size
 *     entries on the same normalized grid, NULL selects zero output.
 * @param[in] table_size Shared logical table length>=4 for quadratic lookup; table grid is equally
 *     spaced over normalized depth/area0..1.
 * @param[out] out_a Writable count-entry result span, in corresponding scale-array units.
 * @param[out] out_b Writable count-entry result span, in corresponding scale-array units.
 * @param[in] count Nonnegative batch length; each non-NULL span must cover this many entries;0
 *     performs no element reads/writes.
 */
void perlink_tabulated_pair(const double* OPENSWMM_RESTRICT depth,
                            const double* OPENSWMM_RESTRICT nrm,
                            const double* OPENSWMM_RESTRICT scale_a,
                            const double* OPENSWMM_RESTRICT scale_b, const double* const* tables_a,
                            const double* const* tables_b, int table_size,
                            double* OPENSWMM_RESTRICT out_a, double* OPENSWMM_RESTRICT out_b,
                            int count);

/**
 * @brief Invert one shared normalized depth-versus-area table for batch areas.
 *
 * Computes a_full*invLookup(normalized depth), optionally using a matching immutable bucket map.
 * Caller validates spans/geometry/finiteness; no allocation, ownership transfer or per-element
 * error status. Restrict-qualified source/destination spans must not overlap; NULL numeric spans
 * are permitted only at count0.
 *
 * @param[in] depth Readable count-entry finite nonnegative water depths in ft.
 * @param[in] y_full Readable count-entry normalization operands: full depth ft in default build,
 *     reciprocal1/ft under SWMM_XSECT_FAST_LOOKUP. The historical y_full parameter name does not
 *     change with build mode.
 * @param[in] a_full Readable count-entry positive full areas ft2.
 * @param[in] table Non-NULL borrowed readable table_size-entry shared normalized table; proper
 *     monotonicity required for inverse lookup.
 * @param[in] table_size Shared logical table length>=4 for quadratic lookup; table grid is equally
 *     spaced over normalized depth/area0..1.
 * @param[in] lut Optional borrowed map built for this exact unchanged inverse table/search extent.
 * @param[out] area Writable count-entry result span, area ft2.
 * @param[in] count Nonnegative batch length; each non-NULL span must cover this many entries;0
 *     performs no element reads/writes.
 */
void area_inv_tabulated(const double* OPENSWMM_RESTRICT depth,
                        const double* OPENSWMM_RESTRICT y_full,
                        const double* OPENSWMM_RESTRICT a_full, const double* table, int table_size,
                        const xsect::LocateLut* lut, double* OPENSWMM_RESTRICT area, int count);

// --- Hydraulic radius batch kernels ---

/**
 * @brief Evaluate circular/force-main batch hydraulic radii from the built-in table.
 *
 * Computes r_full*R_Circ(normalized depth). At zero normalized depth uses nonzero R_Circ[0],
 * preserving legacy dry-radius convention. Caller validates spans/geometry/finiteness; no
 * allocation, ownership transfer or per-element error status. Restrict-qualified source/destination
 * spans must not overlap; NULL numeric spans are permitted only at count0.
 *
 * @param[in] depth Readable count-entry finite nonnegative water depths in ft.
 * @param[in] y_full Readable count-entry normalization operands: full depth ft in default build,
 *     reciprocal1/ft under SWMM_XSECT_FAST_LOOKUP. The historical y_full parameter name does not
 *     change with build mode.
 * @param[in] r_full Readable count-entry full hydraulic radii ft.
 * @param[out] hydrad Writable count-entry result span, hydraulic radius ft.
 * @param[in] count Nonnegative batch length; each non-NULL span must cover this many entries;0
 *     performs no element reads/writes.
 */
void hydrad_circular(const double* OPENSWMM_RESTRICT depth, const double* OPENSWMM_RESTRICT y_full,
                     const double* OPENSWMM_RESTRICT r_full, double* OPENSWMM_RESTRICT hydrad,
                     int count);

/**
 * @brief Fuse circular batch area and hydraulic-radius table interpolation.
 *
 * Uses one normalized coordinate/index for A_Circ and R_Circ, with independent positive quadratic
 * refinement/lower clamps. Zero depth produces zero area but legacy nonzero radius-table floor;
 * fast build uses reciprocal arithmetic. Caller validates spans/geometry/finiteness; no allocation,
 * ownership transfer or per-element error status. Restrict-qualified source/destination spans must
 * not overlap; NULL numeric spans are permitted only at count0.
 *
 * @param[in] depth Readable count-entry finite nonnegative water depths in ft.
 * @param[in] y_full Readable count-entry normalization operands: full depth ft in default build,
 *     reciprocal1/ft under SWMM_XSECT_FAST_LOOKUP. The historical y_full parameter name does not
 *     change with build mode.
 * @param[in] a_full Readable count-entry positive full areas ft2.
 * @param[in] r_full Readable count-entry full hydraulic radii ft.
 * @param[out] area Writable count-entry result span, area ft2.
 * @param[out] hydrad Writable count-entry result span, hydraulic radius ft.
 * @param[in] count Nonnegative batch length; each non-NULL span must cover this many entries;0
 *     performs no element reads/writes.
 */
void area_hydrad_circular(const double* OPENSWMM_RESTRICT depth,
                          const double* OPENSWMM_RESTRICT y_full,
                          const double* OPENSWMM_RESTRICT a_full,
                          const double* OPENSWMM_RESTRICT r_full, double* OPENSWMM_RESTRICT area,
                          double* OPENSWMM_RESTRICT hydrad, int count);

/**
 * @brief Evaluate trapezoid batch hydraulic radius through the shared leaf.
 *
 * Area divided by bottom width+depth*r_bot, with exact zero-depth return0. Caller validates
 * spans/geometry/finiteness; no allocation, ownership transfer or per-element error status.
 * Restrict-qualified source/destination spans must not overlap; NULL numeric spans are permitted
 * only at count0.
 *
 * @param[in] depth Readable count-entry finite nonnegative water depths in ft.
 * @param[in] y_bot Readable count-entry nonnegative trapezoid bottom widths ft.
 * @param[in] s_bot Readable count-entry average horizontal/vertical side slopes.
 * @param[in] r_bot Readable count-entry positive side-length/depth factors, dimensionless.
 * @param[out] hydrad Writable count-entry result span, hydraulic radius ft.
 * @param[in] count Nonnegative batch length; each non-NULL span must cover this many entries;0
 *     performs no element reads/writes.
 */
void hydrad_trapezoidal(const double* OPENSWMM_RESTRICT depth,
                        const double* OPENSWMM_RESTRICT y_bot,
                        const double* OPENSWMM_RESTRICT s_bot,
                        const double* OPENSWMM_RESTRICT r_bot, double* OPENSWMM_RESTRICT hydrad,
                        int count);

/**
 * @brief Evaluate triangular batch hydraulic radius through the shared leaf.
 *
 * Computes depth*s_bot/(2*r_bot); caller provides nonzero side-length factor. Caller validates
 * spans/geometry/finiteness; no allocation, ownership transfer or per-element error status.
 * Restrict-qualified source/destination spans must not overlap; NULL numeric spans are permitted
 * only at count0.
 *
 * @param[in] depth Readable count-entry finite nonnegative water depths in ft.
 * @param[in] s_bot Readable count-entry average horizontal/vertical side slopes.
 * @param[in] r_bot Readable count-entry positive side-length/depth factors, dimensionless.
 * @param[out] hydrad Writable count-entry result span, hydraulic radius ft.
 * @param[in] count Nonnegative batch length; each non-NULL span must cover this many entries;0
 *     performs no element reads/writes.
 */
void hydrad_triangular(const double* OPENSWMM_RESTRICT depth, const double* OPENSWMM_RESTRICT s_bot,
                       const double* OPENSWMM_RESTRICT r_bot, double* OPENSWMM_RESTRICT hydrad,
                       int count);

/**
 * @brief Evaluate the simple two-bank rectangular batch radius formula.
 *
 * Computes width*depth/(width+2*depth) directly. Production dispatch uses explicit closed/open
 * variants instead; this retained function does not add crown corrections. Caller validates
 * spans/geometry/finiteness; no allocation, ownership transfer or per-element error status.
 * Restrict-qualified source/destination spans must not overlap; NULL numeric spans are permitted
 * only at count0.
 *
 * @param[in] depth Readable count-entry finite nonnegative water depths in ft.
 * @param[in] w_max Readable count-entry positive widths ft.
 * @param[out] hydrad Writable count-entry result span, hydraulic radius ft.
 * @param[in] count Nonnegative batch length; each non-NULL span must cover this many entries;0
 *     performs no element reads/writes.
 */
void hydrad_rect(const double* OPENSWMM_RESTRICT depth, const double* OPENSWMM_RESTRICT w_max,
                 double* OPENSWMM_RESTRICT hydrad, int count);

/**
 * @brief Scale one shared normalized hydraulic-radius table for a batch.
 *
 * Computes r_full*norm_lookup; at zero depth retains the table's first value rather than forcing
 * zero. Caller validates spans/geometry/finiteness; no allocation, ownership transfer or
 * per-element error status. Restrict-qualified source/destination spans must not overlap; NULL
 * numeric spans are permitted only at count0.
 *
 * @param[in] depth Readable count-entry finite nonnegative water depths in ft.
 * @param[in] y_full Readable count-entry normalization operands: full depth ft in default build,
 *     reciprocal1/ft under SWMM_XSECT_FAST_LOOKUP. The historical y_full parameter name does not
 *     change with build mode.
 * @param[in] r_full Readable count-entry full hydraulic radii ft.
 * @param[in] table Non-NULL borrowed readable table_size-entry shared normalized table; proper
 *     monotonicity required for inverse lookup.
 * @param[in] table_size Shared logical table length>=4 for quadratic lookup; table grid is equally
 *     spaced over normalized depth/area0..1.
 * @param[out] hydrad Writable count-entry result span, hydraulic radius ft.
 * @param[in] count Nonnegative batch length; each non-NULL span must cover this many entries;0
 *     performs no element reads/writes.
 */
void hydrad_tabulated(const double* OPENSWMM_RESTRICT depth, const double* OPENSWMM_RESTRICT y_full,
                      const double* OPENSWMM_RESTRICT r_full, const double* table, int table_size,
                      double* OPENSWMM_RESTRICT hydrad, int count);

// --- Top width batch kernels ---

/**
 * @brief Evaluate circular batch top widths from the built-in width table.
 *
 * Computes w_max*W_Circ(normalized depth), including the table-defined crown/dry endpoint widths.
 * Caller validates spans/geometry/finiteness; no allocation, ownership transfer or per-element
 * error status. Restrict-qualified source/destination spans must not overlap; NULL numeric spans
 * are permitted only at count0.
 *
 * @param[in] depth Readable count-entry finite nonnegative water depths in ft.
 * @param[in] y_full Readable count-entry normalization operands: full depth ft in default build,
 *     reciprocal1/ft under SWMM_XSECT_FAST_LOOKUP. The historical y_full parameter name does not
 *     change with build mode.
 * @param[in] w_max Readable count-entry positive widths ft.
 * @param[out] width Writable count-entry result span, top width ft.
 * @param[in] count Nonnegative batch length; each non-NULL span must cover this many entries;0
 *     performs no element reads/writes.
 */
void width_circular(const double* OPENSWMM_RESTRICT depth, const double* OPENSWMM_RESTRICT y_full,
                    const double* OPENSWMM_RESTRICT w_max, double* OPENSWMM_RESTRICT width,
                    int count);

/**
 * @brief Evaluate trapezoid batch top widths through the shared leaf.
 *
 * Computes bottom width+2*average side slope*depth without full-depth clamp. Caller validates
 * spans/geometry/finiteness; no allocation, ownership transfer or per-element error status.
 * Restrict-qualified source/destination spans must not overlap; NULL numeric spans are permitted
 * only at count0.
 *
 * @param[in] depth Readable count-entry finite nonnegative water depths in ft.
 * @param[in] y_bot Readable count-entry nonnegative trapezoid bottom widths ft.
 * @param[in] s_bot Readable count-entry average horizontal/vertical side slopes.
 * @param[out] width Writable count-entry result span, top width ft.
 * @param[in] count Nonnegative batch length; each non-NULL span must cover this many entries;0
 *     performs no element reads/writes.
 */
void width_trapezoidal(const double* OPENSWMM_RESTRICT depth, const double* OPENSWMM_RESTRICT y_bot,
                       const double* OPENSWMM_RESTRICT s_bot, double* OPENSWMM_RESTRICT width,
                       int count);

/**
 * @brief Evaluate triangular batch top widths through the shared leaf.
 *
 * Computes2*side slope*depth without a full-depth clamp. Caller validates
 * spans/geometry/finiteness; no allocation, ownership transfer or per-element error status.
 * Restrict-qualified source/destination spans must not overlap; NULL numeric spans are permitted
 * only at count0.
 *
 * @param[in] depth Readable count-entry finite nonnegative water depths in ft.
 * @param[in] s_bot Readable count-entry average horizontal/vertical side slopes.
 * @param[out] width Writable count-entry result span, top width ft.
 * @param[in] count Nonnegative batch length; each non-NULL span must cover this many entries;0
 *     performs no element reads/writes.
 */
void width_triangular(const double* OPENSWMM_RESTRICT depth, const double* OPENSWMM_RESTRICT s_bot,
                      double* OPENSWMM_RESTRICT width, int count);

/**
 * @brief Copy constant rectangular widths to the batch output.
 *
 * No depth argument or closed-crown correction; used for open rectangles. Caller validates
 * spans/geometry/finiteness; no allocation, ownership transfer or per-element error status.
 * Restrict-qualified source/destination spans must not overlap; NULL numeric spans are permitted
 * only at count0.
 *
 * @param[in] w_max Readable count-entry positive widths ft.
 * @param[out] width Writable count-entry result span, top width ft.
 * @param[in] count Nonnegative batch length; each non-NULL span must cover this many entries;0
 *     performs no element reads/writes.
 */
void width_rect(const double* OPENSWMM_RESTRICT w_max, double* OPENSWMM_RESTRICT width, int count);

/**
 * @brief Scale one shared normalized width table for a batch.
 *
 * Computes w_max*norm_lookup with table-defined dry/crown behavior rather than an extra crown
 * special case. Caller validates spans/geometry/finiteness; no allocation, ownership transfer or
 * per-element error status. Restrict-qualified source/destination spans must not overlap; NULL
 * numeric spans are permitted only at count0.
 *
 * @param[in] depth Readable count-entry finite nonnegative water depths in ft.
 * @param[in] y_full Readable count-entry normalization operands: full depth ft in default build,
 *     reciprocal1/ft under SWMM_XSECT_FAST_LOOKUP. The historical y_full parameter name does not
 *     change with build mode.
 * @param[in] w_max Readable count-entry positive widths ft.
 * @param[in] table Non-NULL borrowed readable table_size-entry shared normalized table; proper
 *     monotonicity required for inverse lookup.
 * @param[in] table_size Shared logical table length>=4 for quadratic lookup; table grid is equally
 *     spaced over normalized depth/area0..1.
 * @param[out] width Writable count-entry result span, top width ft.
 * @param[in] count Nonnegative batch length; each non-NULL span must cover this many entries;0
 *     performs no element reads/writes.
 */
void width_tabulated(const double* OPENSWMM_RESTRICT depth, const double* OPENSWMM_RESTRICT y_full,
                     const double* OPENSWMM_RESTRICT w_max, const double* table, int table_size,
                     double* OPENSWMM_RESTRICT width, int count);

} // namespace xsect_batch

} // namespace openswmm

#endif // OPENSWMM_XSECT_BATCH_HPP
