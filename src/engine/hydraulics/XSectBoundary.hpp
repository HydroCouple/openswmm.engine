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
 * @file XSectBoundary.hpp
 * @brief Exact circular-arc/line boundary primitives for cross-section geometry.
 *
 * @details A cross-section boundary is a closed, CCW-oriented chain of BElem
 *          (straight line or circular arc) elements. Every quantity needed to
 *          compile a POLYGON section — area, top width, wetted perimeter, and
 *          the hydrostatic first moment I1 — is computed EXACTLY from this
 *          chain via Green's theorem (evalExact), with no depth-quadrature
 *          anywhere. This is build-time-only machinery: it runs once, when a
 *          POLYGON section is compiled (see the Chebyshev compiler), not in
 *          the hot simulation loop, so nothing here is device-tagged.
 *
 * @note The Green's theorem formulas below are GIVEN by the design spec,
 *       already verified against 200 001-point numerical quadrature to
 *       1e-10, and are transcribed here rather than re-derived.
 * @ingroup engine_hydraulics
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_XSECT_BOUNDARY_HPP
#define OPENSWMM_XSECT_BOUNDARY_HPP

#include <vector>

namespace openswmm::xsboundary {

/**
 * @brief One boundary element: a straight line segment or a circular arc.
 * @details A line has `radius == 0`. An arc runs from `(x0,y0)` at angle `a0`
 *          to `(x1,y1)` at angle `a1` about `(cx,cy)`:
 *          `x(t) = cx + r*cos(a0 + t*(a1-a0))`,
 *          `y(t) = cy + r*sin(a0 + t*(a1-a0))`, `t` in `[0,1]`. The full
 *          chain is closed and oriented CCW overall so Green's theorem gives
 *          a positive enclosed area.
 * @note `a1 > a0` is the common case — a convex, outward-bulging arc walked
 *       in the CCW boundary direction (the standard positive-DXF-bulge
 *       shape). `a1 < a0` is valid and REQUIRED for a locally concave arc (a
 *       notch) walked in the same overall CCW boundary direction: the
 *       *signed* sweep `a1-a0` must match the boundary's walking direction
 *       for the Green's-theorem sum over all elements to give the correct
 *       total area/moment — swapping which endpoint is `a0` vs `a1` negates
 *       that element's contribution, exactly as swapping a segment's
 *       endpoints negates `greenArea(segment)`. Every function below is
 *       written to handle either sign; only `arcLength` takes `|a1-a0|`,
 *       since a physical length cannot be negative.
 */
struct BElem {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    double cx = 0.0, cy = 0.0;
    double radius = 0.0;       ///< 0 = straight segment
    double a0 = 0.0, a1 = 0.0; ///< arc angle at (x0,y0) and (x1,y1), radians
};

/// Exact section properties at one depth, from evalExact().
struct ExactProps {
    double area  = 0.0; ///< wetted area (ft²)
    double perim = 0.0; ///< wetted perimeter — WALL elements only, excludes
                         ///< the free-surface cut (ft)
    double width = 0.0; ///< top width — sum of the wet spans at depth y (ft)
    double ybar  = 0.0; ///< centroid height of the wetted area above invert (ft)
    double i1    = 0.0; ///< hydrostatic first moment A(y)*(y - ybar(y)) (ft³)
    int    ncomp = 0;   ///< number of disjoint wetted components at this depth
};

/// Error codes returned by the boundary-construction functions below. 5 and 6
/// are reserved for the Chebyshev compiler (piece budget exhausted / A(y) not
/// monotone) and are never returned here; 8 is raised by the [CURVES] row
/// parser, not by fromArcSpec itself, which takes already-parsed arrays.
enum class BoundaryError : int {
    OK                  = 0,
    TOO_FEW_POINTS      = 1, ///< n < 3
    SELF_INTERSECTING   = 2, ///< non-adjacent chords cross
    ZERO_AREA           = 3, ///< |signed area| below tolerance
    NON_FINITE          = 4, ///< a NaN/Inf coordinate or bulge
    BULGE_TOO_LARGE     = 7, ///< |bulge| > 1 (arc sweep would exceed pi)
    MALFORMED_CURVE_ROW = 8, ///< raised by the [CURVES] text parser, not here
};

// ===========================================================================
// Green's theorem primitives — exact, no quadrature
// ===========================================================================

/// Signed contribution to the enclosed area, 0.5*(x dy - y dx) along @p e.
double greenArea(const BElem& e) noexcept;

/// Signed contribution to the first moment about y=0, integral of y dA, along
/// @p e (via -0.5*integral of y^2 dx, Green's theorem with P = -y^2/2).
double greenMomentY(const BElem& e) noexcept;

/// Physical arc length of @p e (always >= 0).
double arcLength(const BElem& e) noexcept;

// ===========================================================================
// Height queries
// ===========================================================================

/**
 * @brief x-coordinates where @p e crosses height @p y.
 * @details A segment crosses at most once; an arc crosses at most twice
 *          (within one sweep of magnitude <= 2*pi). A horizontal segment
 *          (y0 == y1) never "crosses" — it either coincides with @p y or
 *          misses it entirely, and is reported as 0 crossings either way.
 * @param e the boundary element (segment or arc)
 * @param y the query height
 * @param[out] xout up to 2 x-coordinates, ascending.
 * @returns the crossing count, 0, 1, or 2.
 * @note At a depth exactly equal to a vertex height shared by two elements,
 *       both elements may independently report that shared crossing — the
 *       critical-height analysis is what identifies those depths so the
 *       Chebyshev compiler never has to evaluate exactly on one; evalExact()
 *       is only exact for a generic (non-vertex) depth.
 */
int crossingsAt(const BElem& e, double y, double xout[2]) noexcept;

/**
 * @brief Clip @p e to the portion(s) at or below height @p y.
 * @details For a segment this is at most one sub-segment. For an arc, the
 *          region sin(theta) <= (y-cy)/r restricted to one sweep of
 *          magnitude <= 2*pi is at most two disjoint sub-intervals (e.g. an
 *          arc whose sweep passes over its own crown has one "below" piece on
 *          each side of the peak) — hence `out[2]`. Each returned piece keeps
 *          the SAME signed walking direction as @p e (so its own contribution
 *          to greenArea/greenMomentY carries the correct sign).
 * @param e the boundary element (segment or arc)
 * @param y the query height
 * @param[out] out up to 2 surviving sub-elements
 * @returns the number of surviving pieces, 0, 1, or 2.
 */
int clipBelow(const BElem& e, double y, BElem out[2]) noexcept;

// ===========================================================================
// Whole-boundary evaluation
// ===========================================================================

/**
 * @brief Exact section properties at depth @p y — no depth-quadrature.
 * @details Clips every element to height <= y and sums Green contributions
 *          for area and first moment; sums arc length of the surviving WALL
 *          pieces only — a wall element lying exactly ON the y == level cut
 *          is the free surface, not wetted perimeter, so it is excluded.
 *          Top width is the sum of alternating spans of the sorted crossing
 *          abscissae.
 * @note Area and moment additionally need each wet span CLOSED at the free
 *       surface: summing Green's-theorem contributions over the open wall
 *       pieces alone is NOT the wetted area (a rectangle 10 ft wide clipped
 *       at h=3 gives 15, not 30, from wall pieces alone — verified by hand).
 *       A synthetic horizontal cap (right crossing -> left crossing, so the
 *       whole contour stays CCW) closes each span for the Green's sum; its
 *       own length is never added to `perim`.
 * @note I1 requires NO integration over depth: I1(y) = A(y)*(y - ybar(y)).
 *       This supersedes Simpson quadrature over depth for I1.
 */
ExactProps evalExact(const BElem* elems, int n, double y);

// ===========================================================================
// Critical heights (Morse / Newton-Puiseux analysis)
// ===========================================================================

/**
 * @brief One height at which A(y), B(y) or P(y) loses smoothness, tagged with
 *        the coordinate-map exponent on each side.
 * @details A piece `[y_i, y_{i+1}]` of the compiled section takes `exp_lo` from
 *          `crit[i].exp_above` and `exp_hi` from `crit[i+1].exp_below`.
 *
 * **What `exp_above`/`exp_below` actually encode — read this before using
 * them as if they were leading exponents.** The value is a *map selector*: it
 * answers "is A analytic on this side, or does it carry a half-integer branch
 * point?", NOT "what is the leading power of A here". Only two values are
 * emitted:
 *
 *   - `1.0` — A is ANALYTIC on that side; the Chebyshev fit needs no
 *     coordinate change (identity map).
 *   - `1.5` — A ~ |Δy|^{3/2}; a square-root branch point, so the piece needs
 *     the stretch that makes A analytic in the mapped variable.
 *
 * The two-value encoding is deliberately coarser than a true Puiseux exponent,
 * and the spec's 1.0/1.5 dichotomy is genuinely incomplete — reported here as
 * the design document asks rather than silently forced:
 *
 *   - A **V-notch invert** (two straight segments meeting at a point) has
 *     A = z·y², a true leading exponent of **2.0**, not 1.0.
 *   - A **pointed / gothic crown** likewise has A_full − A ~ δ².
 *
 * Both are nevertheless tagged `1.0`, and that is the CORRECT action, because
 * every integer exponent is analytic and therefore wants the identity map. Only
 * the half-integer case needs a different coordinate. Tagging by "analytic vs.
 * √-branch" is what the map table is really keyed on; recording 2.0 would add
 * information no consumer can use and would fall outside the table's rows.
 *
 * @warning The design document's blanket claim that "a closed conduit's crown
 *          has exp_below = 1.5; its invert has exp_above = 1.5" holds only for
 *          a SMOOTH (horizontally tangent) crown/invert. A pointed crown
 *          (GOTHIC) and a V-notch or flat invert are analytic and get 1.0.
 *          This function computes the tag rather than assuming it.
 */
struct CriticalHeight {
    double y         = 0.0;  ///< height above the invert (ft)
    double exp_above = 1.0;  ///< map selector for A just ABOVE this height
    double exp_below = 1.0;  ///< map selector for A just BELOW this height
};

/**
 * @brief Locate every height at which A(y), B(y) or P(y) loses smoothness.
 *
 * @details This is the Morse / Newton-Puiseux step: it is what lets the
 *          spectral fit downstream converge geometrically instead of
 *          algebraically. Splitting the depth range at these heights leaves A
 *          analytic on the interior of every piece; the exponent tags then say
 *          which pieces additionally need a coordinate change at their ends.
 *
 *          Two sources are enumerated, and together they are exhaustive:
 *
 *          1. **Every element endpoint height** (`y0`, `y1` of each element).
 *             This covers corners, tangential joints, and the ends of
 *             horizontal segments — every place where the boundary's own
 *             parameterisation changes.
 *          2. **Every arc horizontal tangency** — angles congruent to ±π/2
 *             within the arc's sweep, i.e. the arc's own lowest (`cy − r`) and
 *             highest (`cy + r`) points.
 *
 *          `0` and `y_full` are always included. Heights are returned sorted
 *          ascending and deduplicated to `1e-12 · y_full`; where duplicates
 *          merge, the exponent tags are unioned by MAX, so a `1.5` claim from
 *          any contributing feature survives.
 *
 *          **The tagging rule reduces to one sentence:** a `1.5` tag arises
 *          only from a smooth horizontal tangency, and it lands on the side of
 *          that height where the tangent arc actually lies — above its lowest
 *          point, below its highest point. Everything else is `1.0`.
 *
 *          That single rule is what makes this robust. A tangency is where two
 *          boundary crossings are born or annihilate (a fold), and a fold is
 *          the only way a crossing abscissa can pick up a √|Δy| term, which is
 *          in turn the only way A acquires a half-integer power. A corner —
 *          however sharp, and whichever direction it points — never produces
 *          one: each incident element contributes an abscissa that is analytic
 *          on its own side of the joint, so A is analytic on each side too.
 *          The function therefore never has to classify "corner vs. smooth
 *          joint", which would be the fragile part.
 *
 * @param elems boundary chain, closed and CCW, with `min(y) == 0` — i.e. as
 *              produced by fromPolyline()/fromArcSpec(). Heights are reported
 *              in that shifted frame.
 * @param n     element count.
 * @param[out] out sorted, deduplicated critical heights. Cleared on entry;
 *              left empty if @p n <= 0 or the chain has zero height.
 *
 * @note **Cases the design document leaves open, and what is done about each.**
 *
 *       - *Two arcs meeting tangentially (non-horizontal tangent) — corner or
 *         smooth point?* Operationally neither. It IS a critical height and
 *         must be split at: the curvature jumps, so A is C¹ but not C², and a
 *         Chebyshev fit spanning it converges only algebraically. But the tag
 *         is `1.0` on both sides, because A is analytic on each side
 *         separately with nonzero slope. `1.0` means "analytic, identity map",
 *         NOT "corner" — the split and the tag are independent decisions.
 *
 *       - *An exactly horizontal segment.* Critical, `1.0`/`1.0`. The design
 *         document's "infinite dA/dy over a finite span" is imprecise:
 *         dA/dy = B does not blow up, it JUMPS by the segment's width, so A
 *         stays continuous with a slope kink and is analytic on each side.
 *         Identity map both sides is right, and reproducing that jump exactly
 *         is what makes a bench's top-width discontinuity come out correct.
 *
 *       - *An arc extremum within `1e-9 · y_full` of an element endpoint.* The
 *         tangency height is SNAPPED onto the endpoint and the two exponent
 *         claims are unioned (so `1.5` survives). This deliberately overrides
 *         the general `1e-12` dedupe for this one pairing. Rationale: a piece
 *         narrower than `1e-9 · y_full` carries no resolvable information, its
 *         Chebyshev fit is numerically meaningless, and it burns one of the 24
 *         piece slots — whereas the merged height with the stronger tag
 *         describes the local behaviour correctly.
 *
 *       - *A cusp where two arcs meet with opposite curvature.* If the shared
 *         tangent is HORIZONTAL, each arc contributes a half-power on its own
 *         side and the height is tagged `1.5`/`1.5` — this is exactly the
 *         two-sided map case, and it falls out of the tagging rule with no
 *         special handling (the arc below claims `exp_below`, the arc above
 *         claims `exp_above`). If the shared tangent is NOT horizontal, the
 *         enclosed spike has width O(δ²), which is analytic, so `1.0`/`1.0`.
 *
 *       - *Three or more components merging at the same height.* One critical
 *         height is emitted; the claims union by MAX. Each merge contributes
 *         the same leading power, so the tag is unaffected by how many coincide
 *         — only the component count changes, and that is evalExact()'s
 *         business, not the exponent's. No special case is needed.
 *
 * @note Merge and split heights need no separate search pass. A merge requires
 *       two crossings to collide, which requires either a smooth horizontal
 *       tangency (source 2) or a vertex that is a local extremum of height
 *       (source 1). Both are already enumerated, so scanning for component
 *       merges independently would only rediscover heights already found.
 *
 * @see evalExact
 */
void findCriticalHeights(const BElem* elems, int n,
                         std::vector<CriticalHeight>& out);

// ===========================================================================
// Construction
// ===========================================================================

/**
 * @brief Build a closed all-straight boundary from a simple polygon.
 * @details Convenience wrapper for fromArcSpec() with a null bulge array.
 * @returns 0 (OK) or one of BoundaryError's TOO_FEW_POINTS / SELF_INTERSECTING
 *          / ZERO_AREA / NON_FINITE.
 */
int fromPolyline(const double* x, const double* y, int n,
                 std::vector<BElem>& out);

/**
 * @brief Build a closed boundary chain from points with optional DXF-style
 *        arc bulges.
 * @details Point i connects to point (i+1) mod n; `bulge[i]` (or 0.0 if
 *          @p bulge is null) is the DXF-convention bulge `b = tan(theta/4)`
 *          of that segment: `theta = 4*atan(b)` is the included angle (CCW
 *          positive), `radius = chord / (2*|sin(theta/2)|)`, and the arc's
 *          centre lies on the chord's perpendicular bisector at distance
 *          `radius*cos(theta/2)`, on the LEFT of point_i -> point_{i+1} when
 *          `b > 0` (and on the right when `b < 0`). `b == 0` is a straight
 *          segment. `theta` is added, unreduced, to point i's own angle to
 *          get point (i+1)'s angle — see the BElem note on why `a1 < a0` for
 *          a negative-bulge (concave) arc is correct, not a bug.
 *
 *          Validates n >= 3, every coordinate and bulge finite, every
 *          |bulge| <= 1, and that the chord chain (arcs approximated by
 *          their chords, as the spec permits for this O(n^2) check) is
 *          simple. Computes the total signed area; rejects a near-zero
 *          result, and reverses the whole chain (point order AND bulge
 *          signs, so the same physical arcs are kept) if the input traced
 *          clockwise, so the output is always CCW. Finally shifts every
 *          y-bearing field (y0, y1, and cy for arcs) so the boundary's own
 *          minimum y — including any arc's interior low point, not just its
 *          endpoints — lands at exactly 0.
 * @returns 0 (OK) or one of BoundaryError's TOO_FEW_POINTS /
 *          SELF_INTERSECTING / ZERO_AREA / NON_FINITE / BULGE_TOO_LARGE.
 *          MALFORMED_CURVE_ROW (8) is never returned here — it belongs to
 *          the [CURVES] text-row parser that produces these arrays.
 */
int fromArcSpec(const double* x, const double* y, const double* bulge, int n,
                std::vector<BElem>& out);

} // namespace openswmm::xsboundary

#endif // OPENSWMM_XSECT_BOUNDARY_HPP
