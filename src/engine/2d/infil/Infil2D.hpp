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
 * @file Infil2D.hpp
 * @brief Per-cell infiltration for the 2D overland-flow mesh (track I).
 *
 * @details Implements `plans/TWO_ZONE_GROUNDWATER_EXPLICIT_LTS_PLAN_2026-08-15.md`
 *          §5.5 (steps I1–I8, decisions D-I1…D-I6). This is the
 *          `GROUNDWATER OFF` infiltration path: a per-cell loss model so
 *          surface-only runs do not overestimate pervious runoff.
 *
 *          Design invariants — do not violate these without amending §5.5:
 *
 *          - **D-I1.** Infiltration is a *held rate*, recomputed on the
 *            `INFIL_STEP` cadence and published into
 *            `SurfaceStateData::infil_rate` (m/s). It is NOT evaluated inside
 *            the marcher's `fireCells`. The marcher only consumes the held
 *            rate through `infilSink()`.
 *          - **D-I2.** Sink order within a substep: rainfall source →
 *            evaporation (unchanged `evapSink` semantics) → infiltration
 *            against the remaining depth.
 *          - **D-I3.** Parameters resolve
 *            `per-cell override > tag row > '*' row > none`, once, at
 *            configure time. The solver never consults tags. Provenance is
 *            retained so the writer re-emits a compact file.
 *          - **D-I4.** `LOST` is the only destination accepted in this
 *            release; the others parse and are rejected with a clear message.
 *          - **D-I6.** Six methods: HORTON, MOD_HORTON, GREEN_AMPT,
 *            MOD_GREEN_AMPT, CURVE_NUM, CONSTANT.
 *
 *          **Units.** Row parameters are in *project units* (the same numbers
 *          a user types into `[INFILTRATION]`) — user decision 2026-08-20.
 *          The `infil::*_init` functions consume those directly via
 *          `ucf::UCF(...)`. The `infil::*_getInfil` kernels work in ft and
 *          ft/s; the 2D solver is SI. All conversion happens in
 *          Infil2D::updateRates() at the call boundary, so the hydrology
 *          kernels keep bit-parity with legacy `infil.c`.
 *
 * @ingroup twoD
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_2D_INFIL2D_HPP
#define OPENSWMM_2D_INFIL2D_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "../../hydrology/Infiltration.hpp"

namespace openswmm { struct SimulationOptions; }

namespace openswmm::twoD {

struct MeshData;
struct SurfaceStateData;

// ============================================================================
// Value types
// ============================================================================

/// Destination of infiltrated water. D-I4 as amended by U3 and G1: LOST and
/// SUBCATCH_AQUIFER are routed, and AQUIFER_2D is accepted once a
/// `[2D_AQUIFER]` section resolves — without one it is still refused, with a
/// message naming the section to add.
///
/// @note With a `[2D_AQUIFER]` present the aquifer OWNS infiltration: every
///       infiltrating cell recharges it and LOST reads as AQUIFER_2D. That is
///       what a user means by putting an aquifer under the mesh, and it keeps
///       the water to one owner. SUBCATCH_AQUIFER is the one destination that
///       cannot coexist with it, for the same one-owner reason the integrated
///       component conflict already states — see `SurfaceRouter2D::initialize`.
enum class Infil2DDest : int {
    LOST             = 0,  ///< Leaves the domain; booked to MassBalance2D::infil_out
    SUBCATCH_AQUIFER = 1,  ///< Legacy subcatchment aquifer (U3 track I-b)
    AQUIFER_2D       = 2   ///< The two-zone 2D kernel (G1 step 11b)
};

/// Number of positional parameter columns carried per row. Matches the widest
/// legacy `[INFILTRATION]` method (Horton: f0 fmin decay dry_time Fmax).
inline constexpr int kInfil2DMaxParams = 5;

/**
 * @brief One infiltration specification: method + positional parameters + destination.
 *
 * @details Parameters are POSITIONAL and in PROJECT UNITS, matching legacy
 *          `[INFILTRATION]` exactly:
 *
 *          | method          | p0        | p1        | p2          | p3         | p4   |
 *          |-----------------|-----------|-----------|-------------|------------|------|
 *          | HORTON          | f0        | fmin      | decay (1/hr)| dry_time(d)| Fmax |
 *          | MOD_HORTON      | f0        | fmin      | decay (1/hr)| dry_time(d)| Fmax |
 *          | GREEN_AMPT      | S (suction)| Ks       | IMD         | —          | —    |
 *          | MOD_GREEN_AMPT  | S (suction)| Ks       | IMD         | —          | —    |
 *          | CURVE_NUM       | CN        | —         | dry_time(d) | —          | —    |
 *          | CONSTANT        | rate      | —         | —           | —          | —    |
 *
 *          f0/fmin/Ks/rate are in/hr (US) or mm/hr (SI); S and Fmax are in
 *          (US) or mm (SI). CURVE_NUM's p1 is unused and ignored, matching the
 *          legacy column layout where the middle value is a no-op.
 */
struct Infil2DRow {
    /// False = NONE: the cell has no infiltration model. When false, every
    /// other field is meaningless.
    bool        has_method = false;
    InfilModel  method     = InfilModel::HORTON;
    double      p[kInfil2DMaxParams] = {0.0, 0.0, 0.0, 0.0, 0.0};
    Infil2DDest dest       = Infil2DDest::LOST;
    /// E2: true when the row spelled its own DEST column. Rows that did not
    /// take the project default ([2D_OPTIONS] INFIL_DESTINATION) at
    /// initialize; the writer emits DEST only for explicit rows so a deck
    /// round-trips unchanged.
    bool        dest_explicit = false;
};

/// One `[2D_INFILTRATION_DEFAULTS]` row. `tag == "*"` is the mesh-wide
/// fallback and may appear anywhere in the section.
struct Infil2DDefault {
    std::string tag;
    Infil2DRow  row;
};

/// One `[2D_INFILTRATION]` per-cell override row.
struct Infil2DOverride {
    int        tri = -1;   ///< Triangle index (0-based internally; 1-based in file)
    Infil2DRow row;
};

/// `[2D_INFILTRATION_OPTIONS]`.
struct Infil2DOptions {
    /// Evaluation cadence (s). D-I1. <= 0 means "use the project WET_STEP",
    /// which SurfaceRouter2D resolves at initialize().
    double infil_step = 0.0;
};

/// Where a resolved cell's parameters came from. Retained so the writer emits
/// a compact file (D-I3) instead of N per-cell rows.
enum class Infil2DProvenance : std::uint8_t {
    NONE     = 0,  ///< No model resolved
    STAR     = 1,  ///< From the '*' default row
    TAG      = 2,  ///< From a tag default row
    OVERRIDE = 3   ///< From a per-cell [2D_INFILTRATION] row
};

// ============================================================================
// Infil2D — per-cell infiltration state and driver
// ============================================================================

/**
 * @brief Owns per-cell infiltration parameters, kernel state and held rates.
 *
 * @details Lifecycle:
 *          1. Parsers fill `defaults()`, `overrides()`, `options()`.
 *          2. `SurfaceRouter2D::initialize()` calls resolve() once the mesh
 *             (and therefore `MeshData::tri_tag`) is available.
 *          3. `SurfaceRouter2D` calls updateRates() every `INFIL_STEP`.
 *          4. The marcher consumes `SurfaceStateData::infil_rate` only.
 *
 *          Empty/unconfigured is the fast path: `active()` is false and no
 *          array is allocated, so a project with no `[2D_INFILTRATION*]`
 *          sections is bitwise identical to the pre-track-I engine (gate I7).
 */
class Infil2D {
public:
    // --- configuration (populated by the section handlers) -----------------
    std::vector<Infil2DDefault>&        defaults()        noexcept { return defaults_; }
    const std::vector<Infil2DDefault>&  defaults()  const noexcept { return defaults_; }
    std::vector<Infil2DOverride>&       overrides()       noexcept { return overrides_; }
    const std::vector<Infil2DOverride>& overrides() const noexcept { return overrides_; }
    Infil2DOptions&                     options()         noexcept { return options_; }
    const Infil2DOptions&               options()   const noexcept { return options_; }

    /**
     * @brief Resolve tag/override precedence into flat per-cell state.
     *
     * @details D-I3 precedence: override > tag > '*' > none. Initializes the
     *          hydrology kernel state for every cell that resolved to a model,
     *          converting project-unit parameters through `infil::*_init`.
     *
     * @param mesh  Mesh (uses `tri_tag` and `n_triangles()`).
     * @param opts  Simulation options (unit system for the *_init conversions).
     * @param err   [out] Human-readable message when the return is false.
     * @returns False on a validation failure (unsupported destination,
     *          out-of-range triangle index, unknown method, bad parameters).
     */
    bool resolve(const MeshData& mesh, const SimulationOptions& opts,
                 std::string& err);

    /**
     * @brief G1 — tell validation that a `[2D_AQUIFER]` resolved, so the
     *        `AQUIFER_2D` destination has somewhere to send its water.
     *
     * @details Must be called before `resolve()`. Without it `AQUIFER_2D` is
     *          refused, which is the right answer when there is no aquifer:
     *          silently treating it as LOST would drain a model the user
     *          believed was recharging.
     */
    void setAquifer2DAvailable(bool on) noexcept { aquifer_2d_available_ = on; }
    bool aquifer2DAvailable() const noexcept { return aquifer_2d_available_; }

    /**
     * @brief Recompute and publish per-cell infiltration rates (D-I1).
     *
     * @details Called on the `INFIL_STEP` cadence. Advances kernel state for
     *          EVERY model-carrying cell, including cells the marcher has
     *          deactivated — otherwise a dry cell presents full initial
     *          capacity when rain finally arrives (§5.5.2).
     *
     *          Writes `state.infil_rate[i]` in m/s, >= 0. Conversion:
     *          precip/depth SI→ft on the way in, rate ft/s→SI on the way out.
     *
     * @param mesh   Mesh (unused today; kept for symmetry and future area use).
     * @param state  Surface state — reads `rainfall`/`depth`, writes `infil_rate`.
     * @param dt     Elapsed seconds since the previous call (the INFIL_STEP).
     */
    void updateRates(const MeshData& mesh, SurfaceStateData& state, double dt);

    /// True when at least one cell resolved to a model.
    bool active() const noexcept { return active_; }

    /// E2: [2D_OPTIONS] INFILTRATION NO — keep the rows (they still save)
    /// but run without infiltration. Called after resolve().
    void deactivate() noexcept { active_ = false; }

    /// Resolved method per triangle; `has_method == false` entries are NONE.
    const std::vector<Infil2DRow>& resolvedRows() const noexcept { return resolved_; }

    /// Provenance per triangle (D-I3, for the writer).
    const std::vector<Infil2DProvenance>& provenance() const noexcept { return prov_; }

    /// Cumulative infiltration CAPACITY per triangle (m): the integral of the
    /// published rate, without the solver's depth ramp. On a drying cell this
    /// exceeds the water actually removed, so it is NOT ledger-consistent with
    /// `MassBalance2D::infil_out`. `SurfaceRouter2D::infilCumulative()` is the
    /// ledger-consistent series and is what the sidecar's `infil_cum` and the
    /// C API's `*_get_cum_bulk` report; this one is retained as the kernel-side
    /// diagnostic.
    const std::vector<double>& cumulative() const noexcept { return cum_depth_; }

    /// Resolved cadence in seconds (options_.infil_step, or the project
    /// WET_STEP when that was <= 0). Set by resolve().
    double stepSeconds() const noexcept { return step_seconds_; }

    /// Clear everything back to the unconfigured fast path.
    void reset();

private:
    std::vector<Infil2DDefault>  defaults_;
    std::vector<Infil2DOverride> overrides_;
    Infil2DOptions               options_;

    bool   active_        = false;
    double step_seconds_  = 0.0;
    /// G1: a `[2D_AQUIFER]` resolved, so AQUIFER_2D has a receiver. Set by
    /// SurfaceRouter2D before resolve(), and cleared by reset() with the rest
    /// of the authored state.
    bool   aquifer_2d_available_ = false;

    // Per-triangle resolved state (empty when !active_).
    std::vector<Infil2DRow>        resolved_;
    std::vector<Infil2DProvenance> prov_;
    std::vector<double>            cum_depth_;   ///< m

    // Kernel state, one entry per triangle (allocated only for the methods in
    // use; indexed by triangle so lookups stay branch-free).
    std::vector<infil::HortonState>    horton_;
    std::vector<infil::GreenAmptState> grnampt_;
    std::vector<infil::CurveNumState>  curvenum_;
};

// ============================================================================
// Free helpers
// ============================================================================

/// Parse a method token (`HORTON`, `MODIFIED_HORTON`, `GREEN_AMPT`,
/// `MODIFIED_GREEN_AMPT`, `CURVE_NUMBER`, `CONSTANT`, `NONE`), case-insensitive.
/// @returns False if the token is unrecognised. `NONE` returns true with
///          @p has_method set false.
bool parseInfil2DMethod(const std::string& token, InfilModel& method,
                        bool& has_method);

/// Inverse of parseInfil2DMethod — the canonical file token.
const char* infil2DMethodToken(const Infil2DRow& row);

/// Parse a destination token. @returns false if unrecognised.
bool parseInfil2DDest(const std::string& token, Infil2DDest& dest);

/// Inverse of parseInfil2DDest.
const char* infil2DDestToken(Infil2DDest dest);

/// Number of meaningful positional parameters for @p method (used by the
/// writer to trim trailing placeholders and by validation).
int infil2DParamCount(InfilModel method);

} // namespace openswmm::twoD

#endif // OPENSWMM_2D_INFIL2D_HPP
