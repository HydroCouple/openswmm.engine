/**
 * @file      SectionHandlers2D.cpp
 * @brief     Implementation of 2D input section parsers.
 *
 * @see SectionHandlers2D.hpp
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "SectionHandlers2D.hpp"
#include "core/FileIO.hpp" // issue #7: UTF-8 paths on Windows

#include "../data/BoundaryData.hpp"
#include "../data/Report2DVars.hpp"
#include "../../input/InputParseUtils.hpp"
#include "../../input/InputReader.hpp"
#include "../../input/Tokenizer.hpp"
#include "../../core/ErrorCodes.hpp"
#include "../../core/NodeCoupling2D.hpp"
#include "../../core/SimulationContext.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

namespace openswmm::twoD {

namespace {

// Case-insensitive string comparison. Takes views so callers on the
// allocation-free scan paths (prescan2DUnitsHeader) need no temporaries;
// std::string arguments still convert implicitly.
/**
 * @brief Compare byte string views with unsigned-char case folding.
 * @param[in] a First view, valid throughout call.
 * @param[in] b Second view, valid throughout call.
 * @return True only for identical lengths and folded bytes; no Unicode
 * normalization/allocation/mutation.
 */
bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(a[i])) !=
            std::toupper(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

// Try to parse an integer; return -1 on failure
/**
 * @brief Parse whole strtol base-10 token and narrow it to int.
 * @param[in] s Null-terminated numeric token storage.
 * @param[out] ok True iff at least one character parsed and no trailing bytes.
 * @return Narrowed strtol result even on failure; leading whitespace/sign accepted. No
 * errno/long/int range checks, so callers require int-representable input. Never returns a
 * guaranteed -1 sentinel.
 * @note No model mutation/allocation; empty/surplus/noninteger syntax reports ok=false.
 */
int tryParseInt(const std::string& s, bool& ok) {
    char* end = nullptr;
    long val = std::strtol(s.c_str(), &end, 10);
    ok = (end != s.c_str() && *end == '\0');
    return static_cast<int>(val);
}

/**
 * @brief Parse one whole strtod token without finite/errno/range policy.
 * @param[in] s Numeric token storage, leading whitespace/sign accepted.
 * @param[out] ok True iff at least one character parsed and all bytes consumed.
 * @return strtod result even on failure; NaN/Inf and underflow/overflow are not rejected here.
 * @note Caller applies physical/unit/sign/finite checks; no model mutation or allocation.
 */
double tryParseDouble(const std::string& s, bool& ok) {
    char* end = nullptr;
    double val = std::strtod(s.c_str(), &end);
    ok = (end != s.c_str() && *end == '\0');
    return val;
}

// Find a vertex by tag name; returns index or -1
/**
 * @brief Find first exact case-sensitive vertex tag in row order.
 * @param[in] mesh Stable vertex tag vector covering n_vertices.
 * @param[in] tag Exact desired tag, including empty/numeric strings.
 * @return Zero-based first match or -1; no index/cache/physical mutation or allocation.
 */
int findVertexByTag(const MeshData& mesh, const std::string& tag) {
    for (int i = 0; i < mesh.n_vertices(); ++i) {
        if (mesh.vtag[i] == tag)
            return i;
    }
    return -1;
}

// Find a triangle by tag name; returns index or -1
/**
 * @brief Find first exact case-sensitive unified cell tag.
 * @param[in] mesh Stable cell tag vector covering n_triangles (including quads).
 * @param[in] tag Exact desired tag, including empty/numeric strings.
 * @return Zero-based first match or -1; no mutation/allocation.
 */
int findTriangleByTag(const MeshData& mesh, const std::string& tag) {
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        if (mesh.tri_tag[i] == tag)
            return i;
    }
    return -1;
}

/// Hard-error suffix for [2D_OPTIONS] keys retired with the CVODE/ARKODE
/// stack (D2, 2026-07-29).
const std::string RETIRED_SUFFIX =
    " was retired with the CVODE/ARKODE 2D solvers: the explicit "
    "local-inertial marcher is the only 2D integrator. Remove the line; "
    "marcher settings are MOMENTUM_EQUATION, THETA, CFL_NUMBER, LTS_TIERS, "
    "H_MOVE, FROUDE_MAX, RECONSTRUCTION_ORDER, MAX_TIMESTEP, COUPLING_AREA.";

/**
 * @brief Retired [2D_OPTIONS] material: warn-and-ignore when a warnings sink is available (the
 * file-load path — legacy models must still open), hard error otherwise (the programmatic set
 * path).
 *
 * @details Apply legacy-file warning or programmatic rejection policy for a retired option.
 * @param[in] what Human-readable retired key/value diagnostic context.
 * @param[in,out] warnings Optional borrowed output sink; nonnull appends formatted WARNING104.
 * @return Empty after warning when sink exists, otherwise owned hard-error text.
 * @note Does not change solver options or physics. Allocation can throw; caller serializes sink.
 */
std::string retiredOption(const std::string& what, std::vector<std::string>* warnings) {
    if (warnings) {
        warnings->push_back(openswmm::format_warning(openswmm::WARN_2D_OPTION_RETIRED, what));
        return {};
    }
    return what + RETIRED_SUFFIX;
}

} // anonymous namespace

/**
 * @details Recognizes authoring
 * process/backend/momentum/closure/coupling/report/heat/infiltration policy; options carry their
 * documented SI solver units (s, m, m2/s, dimensionless coefficients).
 * REPORT_2D_VARIABLES/SPECIES consume all value tokens; most other keys use first value and
 * ignore surplus. Enum values/ranges are checked per key, but numeric parsing is not uniformly
 * finite: DISPERSION and report/infil duration explicitly enforce finiteness, while several
 * legacy scalars can retain NaN/Inf or unbounded values for later validation. Some branches
 * assign before returning a diagnostic, so use a copy for transactional edits. Retired
 * keys/INTEGRATOR values append warning and return success only with a nonnull warning sink;
 * otherwise diagnostic failure. No physical owner rebuild or clock advance; allocations can
 * throw after partial options/warnings changes.
 */
std::string parse2DOptionsLine(const std::vector<std::string>& tokens, SolverOptions2D& opts,
                               std::vector<std::string>* warnings) {
    if (tokens.size() < 2)
        return "Expected PARAMETER VALUE";

    const auto& key = tokens[0];
    const auto& val = tokens[1];
    bool ok = false;

    if (iequals(key, "MAX_TIMESTEP")) {
        opts.max_timestep = tryParseDouble(val, ok);
        if (!ok)
            return "Invalid MAX_TIMESTEP value";
    } else if (iequals(key, "DRY_DEPTH")) {
        opts.dry_depth = tryParseDouble(val, ok);
        if (!ok)
            return "Invalid DRY_DEPTH value";
    } else if (iequals(key, "COUPLING_CD")) {
        opts.coupling_cd = tryParseDouble(val, ok);
        if (!ok)
            return "Invalid COUPLING_CD value";
    } else if (iequals(key, "DISPERSION")) {
        // S2: isotropic species dispersion, m2/s. Refused negative, not
        // clamped (the 1D [TRANSPORT_OPTIONS] DISPERSION convention).
        opts.dispersion = tryParseDouble(val, ok);
        if (!ok || !std::isfinite(opts.dispersion) || opts.dispersion < 0.0)
            return "Invalid DISPERSION value (m2/s, must be finite and >= 0)";
    } else if (iequals(key, "COUPLING_SYNC")) {
        opts.coupling_sync = tryParseDouble(val, ok);
        if (!ok || opts.coupling_sync < 0.0)
            return "Invalid COUPLING_SYNC value (seconds, >= 0)";
    } else if (iequals(key, "LIMITER_EPSILON")) {
        opts.limiter_epsilon = tryParseDouble(val, ok);
        if (!ok)
            return "Invalid LIMITER_EPSILON value";
    } else if (iequals(key, "FLUX_DH_EPS")) {
        opts.flux_dh_eps = tryParseDouble(val, ok);
        if (!ok)
            return "Invalid FLUX_DH_EPS value";
    } else if (iequals(key, "CELL_CLOSURE")) {
        if (iequals(val, "FLAT"))
            opts.cell_closure = CellClosure2D::FLAT;
        else if (iequals(val, "VFR"))
            opts.cell_closure = CellClosure2D::VFR;
        else
            return "Unknown CELL_CLOSURE: " + val;
    } else if (iequals(key, "FACE_RECONSTRUCTION")) {
        if (iequals(val, "MEAN"))
            opts.face_reconstruction = FaceDepth2D::MEAN;
        else if (iequals(val, "VFR_FACE"))
            opts.face_reconstruction = FaceDepth2D::VFR_FACE;
        else
            return "Unknown FACE_RECONSTRUCTION: " + val;
    } else if (iequals(key, "VFR_MIN_WET_FRAC")) {
        const double frac = tryParseDouble(val, ok);
        if (!ok || frac <= 0.0 || frac > 0.5)
            return "Invalid VFR_MIN_WET_FRAC value (expected (0, 0.5])";
        opts.vfr_min_wet_frac = frac;
    } else if (iequals(key, "RAINFALL_MODE")) {
        if (iequals(val, "NATURAL_NEIGHBOUR") || iequals(val, "NATURAL_NEIGHBOR"))
            opts.rainfall_mode = RainfallMode::NATURAL_NEIGHBOUR;
        else if (iequals(val, "NEAREST_NEIGHBOUR") || iequals(val, "NEAREST_NEIGHBOR"))
            opts.rainfall_mode = RainfallMode::NEAREST_NEIGHBOUR;
        else if (iequals(val, "SYSTEM"))
            opts.rainfall_mode = RainfallMode::SYSTEM;
        else if (iequals(val, "NONE"))
            opts.rainfall_mode = RainfallMode::NONE;
        else
            return "Unknown RAINFALL_MODE: " + val;
    } else if (iequals(key, "REPORT_2D")) {
        if (iequals(val, "YES") || val == "1")
            opts.report_2d = true;
        else if (iequals(val, "NO") || val == "0")
            opts.report_2d = false;
        else
            return "Invalid REPORT_2D value (YES/NO)";
    } else if (iequals(key, "OUTPUT_FILE")) {
        // Stored as the raw token; resolved against the .inp directory in
        // SWMMEngine::open when the Default2DOutputPlugin is instantiated.
        opts.output_file = val;
    } else if (iequals(key, "INTEGRATOR")) {
        // The explicit marcher is the only integrator (D2 retirement,
        // 2026-07-29). EXPLICIT is accepted (and the default); the retired
        // CVODE/ARKODE selections warn on file load (the model opens and
        // runs the marcher — not silent, WARNING 104 names the substitution)
        // and hard-error on the programmatic set path.
        if (!iequals(val, "EXPLICIT"))
            return retiredOption("INTEGRATOR " + val, warnings);
    } else if (iequals(key, "MOMENTUM_EQUATION")) {
        if (iequals(val, "LOCAL_INERTIAL") || iequals(val, "LI"))
            opts.momentum = Momentum2D::LOCAL_INERTIAL;
        else if (iequals(val, "FULL_SWE") || iequals(val, "SWE") || iequals(val, "FULL"))
            opts.momentum = Momentum2D::FULL_SWE;
        else if (iequals(val, "DIFFUSIVE_WAVE") || iequals(val, "DW") || iequals(val, "DIFFUSIVE"))
            opts.momentum = Momentum2D::DIFFUSIVE_WAVE;
        else
            return "Unknown MOMENTUM_EQUATION: " + val +
                   " (expected LOCAL_INERTIAL|FULL_SWE|DIFFUSIVE_WAVE)";
    } else if (iequals(key, "CORIOLIS")) {
        if (iequals(val, "OFF")) opts.coriolis = Coriolis2D::OFF;
        else if (iequals(val, "LATITUDE")) opts.coriolis = Coriolis2D::LATITUDE;
        else if (iequals(val, "CONSTANT_F")) opts.coriolis = Coriolis2D::CONSTANT_F;
        else return "Invalid CORIOLIS (OFF|LATITUDE|CONSTANT_F)";
    } else if (iequals(key, "CORIOLIS_LATITUDE") || iequals(key, "CORIOLIS_F") ||
               iequals(key, "SMAGORINSKY_CS")) {
        if (iequals(val, "UNSET") && !iequals(key, "SMAGORINSKY_CS")) {
            if (iequals(key, "CORIOLIS_LATITUDE")) opts.coriolis_latitude_set = false;
            else opts.coriolis_f_set = false;
            return {};
        }
        const double x = tryParseDouble(val, ok);
        if (!ok || !std::isfinite(x)) return "Invalid finite value for " + key;
        if (iequals(key, "CORIOLIS_LATITUDE")) {
            if (x < -90 || x > 90) return "CORIOLIS_LATITUDE must be in [-90,90] degrees north";
            opts.coriolis_latitude = x;
            opts.coriolis_latitude_set = true;
        } else if (iequals(key, "CORIOLIS_F")) {
            opts.coriolis_f = x;
            opts.coriolis_f_set = true;
        } else {
            if (x < 0) return "SMAGORINSKY_CS must be nonnegative";
            opts.smagorinsky_cs = x;
        }
    } else if (iequals(key, "TURBULENCE_MODEL")) {
        if (iequals(val, "NONE")) opts.turbulence = Turbulence2D::NONE;
        else if (iequals(val, "SMAGORINSKY")) opts.turbulence = Turbulence2D::SMAGORINSKY;
        else return "Invalid TURBULENCE_MODEL (NONE|SMAGORINSKY)";
    } else if (iequals(key, "FRONT_REBUILD")) {
        if (iequals(val, "AUTO"))
            opts.front_rebuild = -1;
        else if (iequals(val, "YES") || iequals(val, "ON"))
            opts.front_rebuild = 1;
        else if (iequals(val, "NO") || iequals(val, "OFF"))
            opts.front_rebuild = 0;
        else
            return "Unknown FRONT_REBUILD: " + val + " (expected AUTO|YES|NO)";
    } else if (iequals(key, "RECONSTRUCTION_ORDER")) {
        const int k = tryParseInt(val, ok);
        if (!ok || (k != 1 && k != 2))
            return "Invalid RECONSTRUCTION_ORDER value (expected 1|2)";
        opts.reconstruction_order = k;
    } else if (iequals(key, "THETA")) {
        const double th = tryParseDouble(val, ok);
        if (!ok || !std::isfinite(th) || th <= 0.0 || th > 1.0)
            return "Invalid THETA value (expected (0, 1])";
        opts.theta = th;
    } else if (iequals(key, "THETA_REFERENCE_STEP")) {
        const double step = tryParseDouble(val, ok);
        if (!ok || !std::isfinite(step) || step < 0.0)
            return "Invalid THETA_REFERENCE_STEP value (expected finite seconds >= 0)";
        opts.theta_reference_step = step;
    } else if (iequals(key, "CFL_NUMBER")) {
        const double c = tryParseDouble(val, ok);
        if (!ok || c <= 0.0 || c > 1.0)
            return "Invalid CFL_NUMBER value (expected (0, 1])";
        opts.cfl_number = c;
    } else if (iequals(key, "H_MOVE")) {
        const double h = tryParseDouble(val, ok);
        if (!ok || h < 0.0)
            return "Invalid H_MOVE value (expected metres >= 0)";
        opts.h_move = h;
    } else if (iequals(key, "LTS_TIERS")) {
        const int k = tryParseInt(val, ok);
        if (!ok || k < 1 || k > 8)
            return "Invalid LTS_TIERS value (expected 1..8)";
        opts.lts_tiers = k;
    } else if (iequals(key, "FROUDE_MAX")) {
        const double f = tryParseDouble(val, ok);
        if (!ok || f <= 0.0)
            return "Invalid FROUDE_MAX value (expected > 0)";
        opts.froude_max = f;
    } else if (iequals(key, "ADVECTION")) {
        // DEPRECATED (2026-09-06): the LI + staggered-advection experiment is
        // superseded by MOMENTUM_EQUATION FULL_SWE. Still honoured so an old
        // deck keeps its physics, but a file load names the replacement.
        if (warnings && (iequals(val, "YES") || iequals(val, "ON") || iequals(val, "TRUE")))
            warnings->push_back("[2D_OPTIONS] ADVECTION YES is deprecated — use "
                                "MOMENTUM_EQUATION FULL_SWE for a conservative convective "
                                "term with shock capturing.");
        if (iequals(val, "YES") || iequals(val, "ON") || iequals(val, "TRUE"))
            opts.advection = true;
        else if (iequals(val, "NO") || iequals(val, "OFF") || iequals(val, "FALSE"))
            opts.advection = false;
        else
            return "Unknown ADVECTION: " + val + " (expected YES|NO)";
    } else if (iequals(key, "COUPLING_IN_FLOODING")) {
        if (iequals(val, "YES") || iequals(val, "TRUE"))
            opts.coupling_in_flooding = true;
        else if (iequals(val, "NO") || iequals(val, "FALSE"))
            opts.coupling_in_flooding = false;
        else
            return "Unknown COUPLING_IN_FLOODING: " + val + " (expected YES|NO)";
    } else if (iequals(key, "COUPLING_LAW")) {
        if (iequals(val, "ORIFICE"))
            opts.coupling_law = CouplingLaw2D::ORIFICE;
        else if (iequals(val, "WEIR_ORIFICE"))
            opts.coupling_law = CouplingLaw2D::WEIR_ORIFICE;
        else
            return "Unknown COUPLING_LAW: " + val + " (expected ORIFICE|WEIR_ORIFICE)";
    } else if (iequals(key, "COUPLING_HEAD")) {
        if (iequals(val, "CELL"))
            opts.coupling_head = CouplingHead2D::CELL;
        else if (iequals(val, "VERTEX"))
            opts.coupling_head = CouplingHead2D::VERTEX;
        else if (iequals(val, "STENCIL_WET_MEAN"))
            opts.coupling_head = CouplingHead2D::STENCIL_WET_MEAN;
        else
            return "Unknown COUPLING_HEAD: " + val + " (expected CELL|VERTEX|STENCIL_WET_MEAN)";
    } else if (iequals(key, "COUPLING_POND_MODE")) {
        if (iequals(val, "FOOTPRINT"))
            opts.coupling_pond_mode = CouplingPondMode2D::FOOTPRINT;
        else if (iequals(val, "CHAMBER"))
            opts.coupling_pond_mode = CouplingPondMode2D::CHAMBER;
        else if (iequals(val, "TRACKING"))
            opts.coupling_pond_mode = CouplingPondMode2D::TRACKING;
        else
            return "Unknown COUPLING_POND_MODE: " + val + " (expected FOOTPRINT|CHAMBER|TRACKING)";
    } else if (iequals(key, "COUPLING_POND_RELAX")) {
        const double v = tryParseDouble(val, ok);
        if (!ok || v < 0.0)
            return "Invalid COUPLING_POND_RELAX value (expected seconds >= 0)";
        opts.coupling_pond_relax = v;
    } else if (iequals(key, "COUPLING_RIM_BAND")) {
        if (iequals(val, "DEFAULT")) {
            opts.coupling_rim_band = -1.0;
            return {};
        }
        const double v = tryParseDouble(val, ok);
        if (!ok || v < 0.0)
            return "Invalid COUPLING_RIM_BAND value (expected metres >= 0)";
        opts.coupling_rim_band = v;
    } else if (iequals(key, "COUPLING_EQUALIZE")) {
        if (iequals(val, "YES"))
            opts.coupling_equalize = 1;
        else if (iequals(val, "NO"))
            opts.coupling_equalize = 0;
        else if (iequals(val, "DEFAULT"))
            opts.coupling_equalize = -1;
        else
            return "Unknown COUPLING_EQUALIZE: " + val + " (expected YES|NO|DEFAULT)";
    } else if (iequals(key, "COUPLING_COEFFS")) {
        double preset = 0.0;
        const std::string err = nodecoupling2d::parseValue(NodeCoupling2DKey::COEFFS, val, preset);
        if (!err.empty())
            return "Unknown " + err;
        opts.coupling_coeffs = static_cast<int>(preset);
    } else if (iequals(key, "COUPLING_AREA")) {
        if (iequals(val, "AUTO"))
            opts.coupling_area_auto = true;
        else if (iequals(val, "DEFAULT"))
            opts.coupling_area_auto = false;
        else
            return "Unknown COUPLING_AREA: " + val + " (expected AUTO|DEFAULT)";
    } else if (iequals(key, "BACKEND")) {
        // Same token set as [OPTIONS] FV_BACKEND; unknown tokens are rejected
        // so a typo surfaces as a failed set instead of a silent AUTO.
        if (iequals(val, "AUTO"))
            opts.backend = Backend2D::AUTO;
        else if (iequals(val, "CPU"))
            opts.backend = Backend2D::CPU;
        else if (iequals(val, "OMP"))
            opts.backend = Backend2D::OMP;
        else if (iequals(val, "CUDA"))
            opts.backend = Backend2D::CUDA;
        else if (iequals(val, "HIP"))
            opts.backend = Backend2D::HIP;
        else if (iequals(val, "SYCL"))
            opts.backend = Backend2D::SYCL;
        else
            return "Unknown BACKEND: " + val + " (expected AUTO|CPU|OMP|CUDA|HIP|SYCL)";
    } else if (iequals(key, "OUTPUT_PRECISION")) {
        if (iequals(val, "FLOAT32") || iequals(val, "F32") || iequals(val, "SINGLE"))
            opts.output_precision = OutputPrecision2D::FLOAT32;
        else if (iequals(val, "FLOAT64") || iequals(val, "F64") || iequals(val, "DOUBLE"))
            opts.output_precision = OutputPrecision2D::FLOAT64;
        else
            return "Unknown OUTPUT_PRECISION: " + val + " (expected FLOAT32|FLOAT64)";
    } else if (iequals(key, "OUTPUT_COMPRESSION")) {
        const int lvl = tryParseInt(val, ok);
        if (!ok || lvl < 0 || lvl > 9)
            return "Invalid OUTPUT_COMPRESSION value (expected 0..9)";
        opts.output_compression = lvl;
    } else if (iequals(key, "REPORT_2D_VARIABLES")) {
        // The value may span several tokens on a file line
        // ("REPORT_2D_VARIABLES DEPTH VELOCITY") or arrive as one
        // space/comma-separated string from the C API; join then split.
        std::string joined;
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            if (i > 1)
                joined += ' ';
            joined += tokens[i];
        }
        unsigned mask = 0;
        const std::string err = parseReport2DVars(joined, mask);
        if (!err.empty())
            return err;
        opts.report_2d_vars = mask;
    } else if (iequals(key, "REPORT_2D_SPECIES")) {
        std::string joined;
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            if (i > 1)
                joined += ' ';
            joined += tokens[i];
        }
        opts.report_2d_species = report2d::parseSpecies(joined); // ALL → empty
    } else if (iequals(key, "REPORT_2D_STEP")) {
        // HH:MM:SS, HH:MM or plain seconds — the [OPTIONS] *_STEP spelling.
        const double sec = openswmm::input::parse_time_seconds(val);
        if (!(sec >= 0.0) || !std::isfinite(sec))
            return "Invalid REPORT_2D_STEP value (expected HH:MM:SS or seconds >= 0)";
        opts.report_2d_step = sec;
    } else if (iequals(key, "INFILTRATION")) {
        // E2 process enable. AUTO restores the pre-E2 rule (on iff rows).
        if (iequals(val, "YES") || iequals(val, "ON"))
            opts.infiltration = 1;
        else if (iequals(val, "NO") || iequals(val, "OFF"))
            opts.infiltration = 0;
        else if (iequals(val, "AUTO"))
            opts.infiltration = -1;
        else
            return "Unknown INFILTRATION: " + val + " (expected YES|NO|AUTO)";
    } else if (iequals(key, "INFIL_STEP")) {
        // Alias of [2D_INFILTRATION_OPTIONS] INFIL_STEP; same duration grammar
        // as WET_STEP / DRY_STEP. 0 = unset.
        const double secs = openswmm::input::parse_time_seconds(val);
        if (!(secs >= 0.0) || !std::isfinite(secs))
            return "Invalid INFIL_STEP value (expected hh:mm:ss or seconds >= 0)";
        opts.infil_step = secs;
    } else if (iequals(key, "INFIL_DEFAULT_METHOD")) {
        static const char* kMethods[] = {
            "NONE",           "HORTON",       "MOD_HORTON", "GREEN_AMPT",
            "MOD_GREEN_AMPT", "CURVE_NUMBER", "CONSTANT",
        };
        bool known = false;
        for (const char* m : kMethods) {
            if (iequals(val, m)) {
                opts.infil_default_method = m;
                known = true;
                break;
            }
        }
        // Legacy [INFILTRATION] spellings of the same methods.
        if (!known && iequals(val, "MODIFIED_HORTON")) {
            opts.infil_default_method = "MOD_HORTON";
            known = true;
        }
        if (!known && iequals(val, "MODIFIED_GREEN_AMPT")) {
            opts.infil_default_method = "MOD_GREEN_AMPT";
            known = true;
        }
        if (!known && iequals(val, "CURVE_NUM")) {
            opts.infil_default_method = "CURVE_NUMBER";
            known = true;
        }
        if (!known)
            return "Unknown INFIL_DEFAULT_METHOD: " + val +
                   " (expected NONE|HORTON|MOD_HORTON|GREEN_AMPT|MOD_GREEN_AMPT|"
                   "CURVE_NUMBER|CONSTANT)";
    } else if (iequals(key, "INFIL_DESTINATION")) {
        if (iequals(val, "LOST"))
            opts.infil_destination = "LOST";
        else if (iequals(val, "SUBCATCH_AQUIFER"))
            opts.infil_destination = "SUBCATCH_AQUIFER";
        else if (iequals(val, "AQUIFER_2D"))
            opts.infil_destination = "AQUIFER_2D";
        else
            return "Unknown INFIL_DESTINATION: " + val +
                   " (expected LOST|SUBCATCH_AQUIFER|AQUIFER_2D)";
    } else if (iequals(key, "HEAT_MODEL")) {
        if (iequals(val, "COUPLED"))
            opts.coupled_heat = true;
        else if (iequals(val, "TRANSPORT_ONLY"))
            opts.coupled_heat = false;
        else
            return "HEAT_MODEL expects TRANSPORT_ONLY or COUPLED";
        opts.heat_model_authored = true;
    } else if (iequals(key, "HEAT_SURFACE_EXCHANGE") || iequals(key, "HEAT_RADIATIVE_EXCHANGE")) {
        int mode = -1;
        if (iequals(val, "YES"))
            mode = 1;
        else if (iequals(val, "NO"))
            mode = 0;
        else if (!iequals(val, "AUTO"))
            return key + " expects AUTO, YES or NO";
        (iequals(key, "HEAT_SURFACE_EXCHANGE") ? opts.heat_surface : opts.heat_radiative) = mode;
        (iequals(key, "HEAT_SURFACE_EXCHANGE") ? opts.heat_surface_authored
                                               : opts.heat_radiative_authored) = true;
    } else if (iequals(key, "BED_HEAT_MODEL")) {
        if (iequals(val, "NONE"))
            opts.bed_heat_model = 0;
        else if (iequals(val, "LOCAL_STORAGE"))
            opts.bed_heat_model = 1;
        else if (iequals(val, "GROUND_COLUMN"))
            opts.bed_heat_model = 2;
        else
            return "BED_HEAT_MODEL expects NONE, LOCAL_STORAGE or GROUND_COLUMN";
        opts.bed_heat_authored = true;
    } else if (iequals(key, "EVAPORATION")) {
        if (iequals(val, "YES") || iequals(val, "ON") || iequals(val, "FORCING"))
            opts.evaporation = 1;
        else if (iequals(val, "NO") || iequals(val, "OFF"))
            opts.evaporation = 0;
        else if (iequals(val, "CLIMATE"))
            opts.evaporation = 2;
        else
            return "Unknown EVAPORATION: " + val + " (expected NO|YES|CLIMATE)";
    } else if (iequals(key, "TRANSPORT_POLLUTANTS") || iequals(key, "TRANSPORT_MSX") ||
               iequals(key, "TRANSPORT_AGE") || iequals(key, "TRANSPORT_TEMPERATURE")) {
        bool on = false;
        if (iequals(val, "YES") || iequals(val, "ON"))
            on = true;
        else if (iequals(val, "NO") || iequals(val, "OFF"))
            on = false;
        else
            return "Unknown " + key + ": " + val + " (expected YES|NO)";
        if (iequals(key, "TRANSPORT_POLLUTANTS"))
            opts.transport_pollutants = on;
        else if (iequals(key, "TRANSPORT_MSX"))
            opts.transport_msx = on;
        else if (iequals(key, "TRANSPORT_AGE"))
            opts.transport_age = on;
        else
            opts.transport_temperature = on;
    } else if (iequals(key, "GROUNDWATER")) {
        // Process enable for the integrated 2D subsurface (G1 kernel).
        // AUTO restores the pre-key rule (on iff [2D_AQUIFER*] rows).
        if (iequals(val, "ON") || iequals(val, "YES"))
            opts.groundwater = 1;
        else if (iequals(val, "OFF") || iequals(val, "NO"))
            opts.groundwater = 0;
        else if (iequals(val, "AUTO"))
            opts.groundwater = -1;
        else
            return "Unknown GROUNDWATER: " + val + " (expected YES|NO|AUTO)";
    } else if (iequals(key, "GW_ET")) {
        static const char* kEt[] = {"AUTO", "NONE", "CAPILLARY_RISE", "BOUNDARY_ET", "BOTH"};
        bool known = false;
        for (const char* e : kEt)
            if (iequals(val, e)) {
                opts.gw_et = e;
                known = true;
                break;
            }
        if (!known)
            return "Unknown GW_ET: " + val + " (expected NONE|CAPILLARY_RISE|BOUNDARY_ET|BOTH)";
    } else if (is2DRetiredOptionKey(key)) {
        // These keys configured the deleted CVODE/ARKODE stack. On file load
        // they are ignored with a WARNING 104 (legacy models must still
        // open); on the programmatic set path they stay hard errors.
        return retiredOption(key, warnings);
    } else {
        return "Unknown 2D_OPTIONS parameter: " + key;
    }

    return {};
}

/**
 * @param[in] key Whole option key, byte case-insensitive.
 * @return True only for recognized retired CVODE/ARKODE keys; no warning or parse side effect.
 * @note Allocation-free lookup, no runtime validation or clock change.
 */
bool is2DRetiredOptionKey(const std::string& key) {
    static const char* kRetired[] = {
        "MIN_TIMESTEP",    "REL_TOLERANCE",     "ABS_TOLERANCE",   "MAX_CVODE_STEPS",
        "MAX_KRYLOV_DIM",  "LINEAR_SOLVER",     "PRECONDITIONER",  "JACOBIAN",
        "ATOL_AREA_REF",   "COUPLING_INTERVAL", "COUPLING_WINDOW", "ACTIVE_SET",
        "ACTIVE_SET_HALO", "MOMENTUM",
    };
    for (const char* k : kRetired) {
        if (iequals(key, k))
            return true;
    }
    return false;
}

/**
 * @param[in] key Whole current option key, byte case-insensitive.
 * @return True for recognized current 2D option key, excluding retired-key list.
 * @note Does not validate an option value or its runtime availability; no allocation/mutation.
 */
bool is2DOptionKey(const std::string& key) {
    for (const char* k : current2DOptionKeys()) {
        if (iequals(key, k))
            return true;
    }
    return false;
}

/**
 * @return Every current 2D option key in documentation order; process-owned, never empty.
 */
std::span<const char* const> current2DOptionKeys() {
    static const char* const kKeys[] = {
        "MAX_TIMESTEP",
        "DRY_DEPTH",
        "COUPLING_CD",
        "COUPLING_SYNC",
        "DISPERSION",
        "LIMITER_EPSILON",
        "FLUX_DH_EPS",
        "RAINFALL_MODE",
        "REPORT_2D",
        "CELL_CLOSURE",
        "FACE_RECONSTRUCTION",
        "VFR_MIN_WET_FRAC",
        "OUTPUT_FILE",
        "INTEGRATOR",
        "MOMENTUM_EQUATION",
        "CORIOLIS", "CORIOLIS_LATITUDE", "CORIOLIS_F",
        "TURBULENCE_MODEL", "SMAGORINSKY_CS",
        "RECONSTRUCTION_ORDER",
        "FRONT_REBUILD",
        "THETA",
        "THETA_REFERENCE_STEP",
        "CFL_NUMBER",
        "H_MOVE",
        "LTS_TIERS",
        "FROUDE_MAX",
        "ADVECTION",
        "COUPLING_AREA",
        "COUPLING_IN_FLOODING",
        "COUPLING_LAW",
        "COUPLING_HEAD",
        "COUPLING_COEFFS",
        "COUPLING_POND_MODE",
        "COUPLING_POND_RELAX",
        "COUPLING_RIM_BAND",
        "COUPLING_EQUALIZE",
        "BACKEND",
        "OUTPUT_PRECISION",
        "OUTPUT_COMPRESSION",
        "REPORT_2D_VARIABLES",
        "REPORT_2D_SPECIES",
        "REPORT_2D_STEP",
        "INFILTRATION",
        "INFIL_STEP",
        "INFIL_DEFAULT_METHOD",
        "INFIL_DESTINATION",
        "EVAPORATION",
        "TRANSPORT_POLLUTANTS",
        "TRANSPORT_MSX",
        "TRANSPORT_AGE",
        "TRANSPORT_TEMPERATURE",
        "GROUNDWATER",
        "GW_ET",
        "HEAT_MODEL",
        "HEAT_SURFACE_EXCHANGE",
        "HEAT_RADIATIVE_EXCHANGE",
        "BED_HEAT_MODEL",
    };
    return kKeys;
}

/**
 * @brief Borrow canonical report-group keyword tokens in defined bit order.
 * @return Process-owned const vector from report2d::tokens; caller must not mutate. No model
 * effect; first construction may allocate, later reads require no owner synchronization.
 */
const std::vector<std::string>& report2DVarTokens() {
    return report2d::tokens();
}

/**
 * @brief Parse report-group keyword list/preset through the shared mask grammar.
 * @param[in] text Whole whitespace/comma-separated group list or DEFAULT|MINIMAL|ALL preset.
 * @param[out] mask Parsed bitmask on success; delegated parseMask failure contract applies.
 * @return Empty on success, otherwise unknown-token diagnostic; no model/options mutation.
 * @note Delegates canonical parser, may allocate; caller explicitly adopts successful mask.
 */
std::string parseReport2DVars(const std::string& text, unsigned& mask) {
    return report2d::parseMask(text, mask);
}

/**
 * @brief Format a report bitmask using canonical presets or ordered keyword list.
 * @param[in] mask Requested report-group bits.
 * @return Owned canonical preset/list string from formatMask. No model mutation, may allocate.
 */
std::string formatReport2DVars(unsigned mask) {
    return report2d::formatMask(mask);
}

/**
 * @details Numeric scalars use %.12g, enum/boolean/preset names canonical uppercase, durations
 * shared report step formatting; stored AUTO policy is not resolved to runtime enables. No model
 * or runtime owner mutation/validation; may allocate. This is a display/serialization token, not
 * a source receipt.
 *
 * @brief Format a stored 2D option as a canonical authoring/display token.
 * @param[in] opts Stable retained solver options, not effective runtime owner state.
 * @param[in] key Whole byte-case-insensitive option key.
 * @return Owned value token, or empty for unknown key/unset OUTPUT_FILE. Numeric values use12
 * significant digits; policy AUTO and configured unit conventions remain stored. No resolution,
 * validation or model mutation; allocation may throw.
 */
std::string format2DOptionValue(const SolverOptions2D& opts, const std::string& key) {
    auto fmt_g = [](double v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.12g", v);
        return std::string(buf);
    };

    if (iequals(key, "MAX_TIMESTEP"))
        return fmt_g(opts.max_timestep);
    if (iequals(key, "DRY_DEPTH"))
        return fmt_g(opts.dry_depth);
    if (iequals(key, "LIMITER_EPSILON"))
        return fmt_g(opts.limiter_epsilon);
    if (iequals(key, "FLUX_DH_EPS"))
        return fmt_g(opts.flux_dh_eps);
    if (iequals(key, "COUPLING_CD"))
        return fmt_g(opts.coupling_cd);
    if (iequals(key, "DISPERSION"))
        return fmt_g(opts.dispersion);
    if (iequals(key, "COUPLING_SYNC"))
        return fmt_g(opts.coupling_sync);
    if (iequals(key, "REPORT_2D"))
        return opts.report_2d ? "YES" : "NO";
    if (iequals(key, "OUTPUT_FILE"))
        return opts.output_file;
    if (iequals(key, "CELL_CLOSURE"))
        return (opts.cell_closure == CellClosure2D::VFR) ? "VFR" : "FLAT";
    if (iequals(key, "FACE_RECONSTRUCTION"))
        return (opts.face_reconstruction == FaceDepth2D::VFR_FACE) ? "VFR_FACE" : "MEAN";
    if (iequals(key, "VFR_MIN_WET_FRAC"))
        return fmt_g(opts.vfr_min_wet_frac);
    if (iequals(key, "RAINFALL_MODE")) {
        switch (opts.rainfall_mode) {
        case RainfallMode::NATURAL_NEIGHBOUR:
            return "NATURAL_NEIGHBOUR";
        case RainfallMode::NEAREST_NEIGHBOUR:
            return "NEAREST_NEIGHBOUR";
        case RainfallMode::SYSTEM:
            return "SYSTEM";
        case RainfallMode::NONE:
            return "NONE";
        }
        return "NATURAL_NEIGHBOUR";
    }
    if (iequals(key, "INTEGRATOR"))
        return "EXPLICIT";
    if (iequals(key, "MOMENTUM_EQUATION")) {
        switch (opts.momentum) {
        case Momentum2D::FULL_SWE:
            return "FULL_SWE";
        case Momentum2D::DIFFUSIVE_WAVE:
            return "DIFFUSIVE_WAVE";
        case Momentum2D::LOCAL_INERTIAL:
            break;
        }
        return "LOCAL_INERTIAL";
    }
    if (iequals(key, "CORIOLIS"))
        return opts.coriolis == Coriolis2D::LATITUDE ? "LATITUDE" :
               opts.coriolis == Coriolis2D::CONSTANT_F ? "CONSTANT_F" : "OFF";
    auto exact = [](double x) { char value[32]; std::snprintf(value,sizeof value,"%.17g",x);return std::string(value); };
    if (iequals(key, "CORIOLIS_LATITUDE"))
        return opts.coriolis_latitude_set ? exact(opts.coriolis_latitude) : "UNSET";
    if (iequals(key, "CORIOLIS_F"))
        return opts.coriolis_f_set ? exact(opts.coriolis_f) : "UNSET";
    if (iequals(key, "SMAGORINSKY_CS")) return exact(opts.smagorinsky_cs);
    if (iequals(key, "TURBULENCE_MODEL"))
        return opts.turbulence == Turbulence2D::SMAGORINSKY ? "SMAGORINSKY" : "NONE";
    if (iequals(key, "RECONSTRUCTION_ORDER"))
        return std::to_string(opts.reconstruction_order);
    if (iequals(key, "FRONT_REBUILD"))
        return opts.front_rebuild < 0 ? "AUTO" : (opts.front_rebuild ? "YES" : "NO");
    if (iequals(key, "THETA"))
        return fmt_g(opts.theta);
    if (iequals(key, "THETA_REFERENCE_STEP"))
        return fmt_g(opts.theta_reference_step);
    if (iequals(key, "CFL_NUMBER"))
        return fmt_g(opts.cfl_number);
    if (iequals(key, "H_MOVE"))
        return fmt_g(opts.h_move);
    if (iequals(key, "LTS_TIERS"))
        return std::to_string(opts.lts_tiers);
    if (iequals(key, "FROUDE_MAX"))
        return fmt_g(opts.froude_max);
    if (iequals(key, "ADVECTION"))
        return opts.advection ? "YES" : "NO";
    if (iequals(key, "COUPLING_AREA"))
        return opts.coupling_area_auto ? "AUTO" : "DEFAULT";
    if (iequals(key, "COUPLING_IN_FLOODING"))
        return opts.coupling_in_flooding ? "YES" : "NO";
    if (iequals(key, "COUPLING_LAW"))
        return opts.coupling_law == CouplingLaw2D::WEIR_ORIFICE ? "WEIR_ORIFICE" : "ORIFICE";
    if (iequals(key, "COUPLING_HEAD"))
        return opts.coupling_head == CouplingHead2D::VERTEX             ? "VERTEX"
               : opts.coupling_head == CouplingHead2D::STENCIL_WET_MEAN ? "STENCIL_WET_MEAN"
                                                                        : "CELL";
    if (iequals(key, "COUPLING_COEFFS"))
        return nodecoupling2d::formatValue(NodeCoupling2DKey::COEFFS, opts.coupling_coeffs);
    if (iequals(key, "COUPLING_POND_MODE"))
        return opts.coupling_pond_mode == CouplingPondMode2D::CHAMBER    ? "CHAMBER"
               : opts.coupling_pond_mode == CouplingPondMode2D::TRACKING ? "TRACKING"
                                                                         : "FOOTPRINT";
    if (iequals(key, "COUPLING_POND_RELAX"))
        return fmt_g(opts.coupling_pond_relax);
    if (iequals(key, "COUPLING_RIM_BAND"))
        return opts.coupling_rim_band < 0.0 ? "DEFAULT" : fmt_g(opts.coupling_rim_band);
    if (iequals(key, "COUPLING_EQUALIZE"))
        return opts.coupling_equalize < 0 ? "DEFAULT" : (opts.coupling_equalize ? "YES" : "NO");
    if (iequals(key, "BACKEND")) {
        switch (opts.backend) {
        case Backend2D::CPU:
            return "CPU";
        case Backend2D::OMP:
            return "OMP";
        case Backend2D::CUDA:
            return "CUDA";
        case Backend2D::HIP:
            return "HIP";
        case Backend2D::SYCL:
            return "SYCL";
        case Backend2D::AUTO:
            break;
        }
        return "AUTO";
    }
    if (iequals(key, "OUTPUT_PRECISION"))
        return opts.output_precision == OutputPrecision2D::FLOAT64 ? "FLOAT64" : "FLOAT32";
    if (iequals(key, "OUTPUT_COMPRESSION"))
        return std::to_string(opts.output_compression);
    if (iequals(key, "REPORT_2D_VARIABLES"))
        return formatReport2DVars(opts.report_2d_vars);
    if (iequals(key, "REPORT_2D_SPECIES"))
        return report2d::formatSpecies(opts.report_2d_species);
    if (iequals(key, "REPORT_2D_STEP"))
        return report2d::formatStep(opts.report_2d_step);
    // E2 process enables. INFILTRATION AUTO is reported as AUTO (a host
    // derives the effective state from swmm_infil2d_defaults_count and the
    // per-cell rows); INFIL_STEP / INFIL_DEFAULT_METHOD report the stored
    // key only — format2DOptionValueEx resolves the unset cases against the
    // Infil2D rows when a caller has them.
    if (iequals(key, "INFILTRATION"))
        return opts.infiltration < 0 ? "AUTO" : (opts.infiltration ? "YES" : "NO");
    if (iequals(key, "INFIL_STEP"))
        return report2d::formatStep(opts.infil_step);
    if (iequals(key, "INFIL_DEFAULT_METHOD"))
        return opts.infil_default_method.empty() ? "NONE" : opts.infil_default_method;
    if (iequals(key, "INFIL_DESTINATION"))
        return opts.infil_destination.empty() ? "LOST" : opts.infil_destination;
    if (iequals(key, "HEAT_MODEL"))
        return opts.coupled_heat ? "COUPLED" : "TRANSPORT_ONLY";
    if (iequals(key, "BED_HEAT_MODEL"))
        return opts.bed_heat_model == 0   ? "NONE"
               : opts.bed_heat_model == 1 ? "LOCAL_STORAGE"
                                          : "GROUND_COLUMN";
    if (iequals(key, "HEAT_SURFACE_EXCHANGE"))
        return opts.heat_surface < 0 ? "AUTO" : opts.heat_surface ? "YES" : "NO";
    if (iequals(key, "HEAT_RADIATIVE_EXCHANGE"))
        return opts.heat_radiative < 0 ? "AUTO" : opts.heat_radiative ? "YES" : "NO";
    if (iequals(key, "EVAPORATION"))
        return opts.evaporation == 0 ? "NO" : (opts.evaporation == 2 ? "CLIMATE" : "YES");
    if (iequals(key, "TRANSPORT_POLLUTANTS"))
        return opts.transport_pollutants ? "YES" : "NO";
    if (iequals(key, "TRANSPORT_MSX"))
        return opts.transport_msx ? "YES" : "NO";
    if (iequals(key, "TRANSPORT_AGE"))
        return opts.transport_age ? "YES" : "NO";
    if (iequals(key, "TRANSPORT_TEMPERATURE"))
        return opts.transport_temperature ? "YES" : "NO";
    // Like INFILTRATION, GROUNDWATER reports what was STORED; the Ex form
    // resolves AUTO against the [2D_AQUIFER*] rows, and reads GW_ET from the
    // [2D_AQUIFER_OPTIONS] struct that owns it.
    if (iequals(key, "GROUNDWATER"))
        return opts.groundwater < 0 ? "AUTO" : (opts.groundwater ? "YES" : "NO");
    if (iequals(key, "GW_ET"))
        return opts.gw_et.empty() ? "NONE" : opts.gw_et;
    return {};
}

/**
 * @brief Format stored 2D policy with optional infiltration/aquifer authoring fallback.
 * @param[in] opts Stable stored options.
 * @param[in] infil Optional borrowed infiltration authoring owner, read only during call.
 * @param[in] key Whole case-insensitive option key.
 * @return Canonical owned value token; unknown key empty. Unset INFIL_STEP uses owner cadence,
 * unset default method uses first '*' row or NONE. No physical/clock mutation; may allocate.
 */
std::string format2DOptionValueEx(const SolverOptions2D& opts, const Infil2D* infil,
                                  const std::string& key) {
    return format2DOptionValueEx(opts, infil, nullptr, key);
}

/**
 * @brief Format stored 2D policy with optional infiltration/aquifer authoring fallback.
 * @param[in] opts Stable stored options.
 * @param[in] infil Optional borrowed infiltration authoring owner, read only during call.
 * @param[in] key Whole case-insensitive option key.
 * @return Canonical owned value token; unknown key empty. Unset INFIL_STEP uses owner cadence,
 * unset default method uses first '*' row or NONE. No physical/clock mutation; may allocate.
 * @param[in] aquifer Optional borrowed aquifer authoring owner; unset GW_ET reads its policy or
 * AUTO.
 * @note GROUNDWATER AUTO remains stored AUTO; no runtime enable resolution is performed.
 */
std::string format2DOptionValueEx(const SolverOptions2D& opts, const Infil2D* infil,
                                  const SubsurfaceConfig* aquifer, const std::string& key) {
    if (infil) {
        if (iequals(key, "INFIL_STEP") && opts.infil_step <= 0.0)
            return report2d::formatStep(infil->options().infil_step);
        if (iequals(key, "INFIL_DEFAULT_METHOD") && opts.infil_default_method.empty()) {
            for (const auto& d : infil->defaults())
                if (d.tag == "*")
                    return infil2DMethodToken(d.row);
            return "NONE";
        }
    }
    if (aquifer) {
        // GW_ET lives in [2D_AQUIFER_OPTIONS]; the [2D_OPTIONS] spelling is an
        // alias that open() folds away, so the staging field is normally empty
        // and the authoritative value is the only one to report.
        if (iequals(key, "GW_ET") && opts.gw_et.empty())
            return aquifer->options.gw_et.empty() ? "AUTO" : aquifer->options.gw_et;
        // GROUNDWATER is deliberately NOT resolved here: like INFILTRATION it
        // reports AUTO | YES | NO exactly AS STORED, so a host round-trips
        // what the deck said rather than writing back a resolved YES/NO over
        // a deck that never spelled the key. Hosts derive the effective state
        // from the row count.
    }
    return format2DOptionValue(opts, key);
}

/**
 * @details Appends a zero-based vertex in input row order; X/Y/Z retain authored project length
 * or sidecar-declared SI units, converted only at initialization. Optional tag copied exactly,
 * surplus ignored. Syntax errors return before resizing, but no finite/geometry/duplicate-tag
 * validation occurs. No topology/physical owner rebuild or clock advance occurs. Requires
 * exclusive authoring access; allocation may throw after resizing/appending begins. Numeric
 * helpers accept full strtod/strtol syntax without general finite/range/errno checks; deferred
 * model validation remains necessary.
 */
std::string parse2DVertexLine(const std::vector<std::string>& tokens, MeshData& mesh) {
    if (tokens.size() < 3)
        return "Expected X Y Z [TAG]";

    bool ok = false;
    double x = tryParseDouble(tokens[0], ok);
    if (!ok)
        return "Invalid X coordinate";

    double y = tryParseDouble(tokens[1], ok);
    if (!ok)
        return "Invalid Y coordinate";

    double z = tryParseDouble(tokens[2], ok);
    if (!ok)
        return "Invalid Z coordinate";

    std::string tag;
    if (tokens.size() >= 4)
        tag = tokens[3];

    int idx = mesh.n_vertices();
    mesh.resize_vertices(idx + 1);
    mesh.vx[idx] = x;
    mesh.vy[idx] = y;
    mesh.vz[idx] = z;
    mesh.vtag[idx] = tag;

    return {};
}

/**
 * @details Vertex indices are zero-based integer tokens, with mesh range/degeneracy/orientation
 * checked later. INIT_DEPTH optional numeric nonnegative authored mean depth; a nonnumeric fifth
 * token is legacy TAG. Sixth is TAG only with numeric depth. Mannings n is retained without
 * positivity validation. Refuses triangles after any quad to preserve unified cell numbering;
 * surplus ignored. No topology/physical owner rebuild or clock advance occurs. Requires
 * exclusive authoring access; allocation may throw after resizing/appending begins. Numeric
 * helpers accept full strtod/strtol syntax without general finite/range/errno checks; deferred
 * model validation remains necessary.
 */
std::string parse2DTriangleLine(const std::vector<std::string>& tokens, MeshData& mesh) {
    if (tokens.size() < 4)
        return "Expected V1 V2 V3 MANNINGS_N [INIT_DEPTH] [TAG]";

    bool ok = false;
    int v0 = tryParseInt(tokens[0], ok);
    if (!ok)
        return "Invalid V1 index";

    int v1 = tryParseInt(tokens[1], ok);
    if (!ok)
        return "Invalid V2 index";

    int v2 = tryParseInt(tokens[2], ok);
    if (!ok)
        return "Invalid V3 index";

    double n = tryParseDouble(tokens[3], ok);
    if (!ok)
        return "Invalid MANNINGS_N value";

    // Optional column 5: INIT_DEPTH when numeric (m, >= 0, default 0 = dry),
    // otherwise it is the TAG (backward compatible with the historical
    // `V1 V2 V3 MANNINGS_N TAG` form). Column 6 is TAG when INIT_DEPTH is
    // present. Files written by the engine/GUI always emit INIT_DEPTH when a
    // tag exists, so round-tripped files are unambiguous.
    double init_depth = 0.0;
    std::string tag;
    if (tokens.size() >= 5) {
        bool num = false;
        double d = tryParseDouble(tokens[4], num);
        if (num) {
            if (d < 0.0)
                return "Invalid INIT_DEPTH (must be >= 0)";
            init_depth = d;
            if (tokens.size() >= 6)
                tag = tokens[5];
        } else {
            tag = tokens[4];
        }
    }

    int idx = mesh.n_triangles();
    // Cell index contract: triangles first, then quads (2D_TRI_QUAD_MESH_PLAN
    // §2.4). A triangle row arriving after a quad row would renumber every
    // cell-addressed section behind it.
    if (idx > 0 && mesh.cell_vertex_count(idx - 1) == 4)
        return "[2D_TRIANGLES] rows must precede all [2D_QUADS] rows "
               "(cells are numbered triangles first, then quads)";
    mesh.resize_triangles(idx + 1);
    mesh.set_triangle(idx, v0, v1, v2);
    mesh.mannings_n[idx] = n;
    mesh.tri_init_depth[idx] = init_depth;
    mesh.tri_tag[idx] = tag;

    return {};
}

// [2D_QUADS] — convex quadrilateral cells, appended AFTER every triangle:
//   V1 V2 V3 V4 MANNINGS_N [INIT_DEPTH] [TAG]
// The four vertices are listed in cyclic order (either orientation; the
// builder orients edge normals outward from the centroid). Cell index of the
// j-th quad row is n_triangles + j — the unified index every cell-addressed
// section (`TRI` columns) uses. Local edge k = (V[(k+1)%4], V[(k+2)%4]).
/**
 * @param[in] tokens V1 V2 V3 V4 MANNINGS_N [INIT_DEPTH] [TAG], zero-based int vertex indices in
 * cyclic order. Numeric optional mean depth must be nonnegative; nonnumeric optional token is
 * TAG.
 * @param[in,out] mesh Appends one four-vertex cell after existing unified cells, retaining
 * authored length/depth units and n coefficient. Earlier rows retained on syntax error.
 * @return Empty on parsed row, otherwise arity/index/numeric/depth diagnostic.
 * @note No finite/index-upper-bound/convexity/degeneracy validation; caller keeps all triangles
 * before quads. Surplus ignored. No topology/physical owner rebuild or clock advance occurs.
 * Requires exclusive authoring access; allocation may throw after resizing/appending begins.
 * Numeric helpers accept full strtod/strtol syntax without general finite/range/errno checks;
 * deferred model validation remains necessary.
 */
std::string parse2DQuadLine(const std::vector<std::string>& tokens, MeshData& mesh) {
    if (tokens.size() < 5)
        return "Expected V1 V2 V3 V4 MANNINGS_N [INIT_DEPTH] [TAG]";

    bool ok = false;
    int v[4];
    for (int k = 0; k < 4; ++k) {
        v[k] = tryParseInt(tokens[static_cast<std::size_t>(k)], ok);
        if (!ok)
            return "Invalid V" + std::to_string(k + 1) + " index";
    }

    double n = tryParseDouble(tokens[4], ok);
    if (!ok)
        return "Invalid MANNINGS_N value";

    double init_depth = 0.0;
    std::string tag;
    if (tokens.size() >= 6) {
        bool num = false;
        double d = tryParseDouble(tokens[5], num);
        if (num) {
            if (d < 0.0)
                return "Invalid INIT_DEPTH (must be >= 0)";
            init_depth = d;
            if (tokens.size() >= 7)
                tag = tokens[6];
        } else {
            tag = tokens[5];
        }
    }

    int idx = mesh.n_triangles();
    mesh.resize_triangles(idx + 1);
    mesh.set_quad(idx, v[0], v[1], v[2], v[3]);
    mesh.mannings_n[idx] = n;
    mesh.tri_init_depth[idx] = init_depth;
    mesh.tri_tag[idx] = tag;

    return {};
}

// [2D_INITIAL_VELOCITY] — optional per-triangle initial velocity (m/s):
//   TRI  U  V
// Default is (0, 0); rows may cover any subset of triangles. The explicit
// marcher projects (h·u, h·v) onto its face normals at initialize to seed the
// prognostic face discharges (a depth-only IC cannot represent solutions with
// v(t=0) ≠ 0, e.g. the SWASHES Thacker planar oscillation).
/**
 * @details TRI is zero-based unified cell index already present; U/V are m/s without
 * project-length conversion. Writes both components only after both numeric syntax checks; no
 * finite check, surplus ignored. Requires tri_init_u/v extents matching mesh. No
 * allocation/physics/clock update; serial edit.
 */
std::string parse2DInitialVelocityLine(const std::vector<std::string>& tokens, MeshData& mesh) {
    if (tokens.size() < 3)
        return "Expected TRI U V";

    bool ok = false;
    int tri = tryParseInt(tokens[0], ok);
    if (!ok || tri < 0 || tri >= mesh.n_triangles())
        return "Invalid triangle index: " + tokens[0];

    double u = tryParseDouble(tokens[1], ok);
    if (!ok)
        return "Invalid U value";
    double v = tryParseDouble(tokens[2], ok);
    if (!ok)
        return "Invalid V value";

    mesh.tri_init_u[tri] = u;
    mesh.tri_init_v[tri] = v;
    return {};
}

/**
 * @details Resolves zero-based index first, then exact tag fallback (including out-of-range
 * numeric tags), first matching tag wins. Stores deferred native node name; optional CD
 * dimensionless/AREA project-length2 are applied only when numeric, malformed optional tokens
 * are silently ignored. Valid optional values are not range/finite checked. No node-name lookup
 * yet; surplus ignored. Name allocation can throw; exclusive edit access, later
 * resolution/scaling required.
 */
std::string parse2DVertexNodeMapLine(const std::vector<std::string>& tokens, MeshData& mesh) {
    if (tokens.size() < 2)
        return "Expected VERTEX_INDEX_OR_TAG SWMM_NODE_NAME [CD] [AREA]";

    // Token is VERTEX_INDEX_OR_TAG. Try a numeric index first, but if the number
    // is out of range fall back to a tag lookup — meshes can name vertices with
    // numeric tags (e.g. "5001"), which must not be mis-read as an index and
    // rejected as out-of-range.
    bool ok = false;
    int vidx = tryParseInt(tokens[0], ok);
    if (ok && (vidx < 0 || vidx >= mesh.n_vertices()))
        ok = false; // numeric but not a valid index — treat as a tag
    if (!ok) {
        vidx = findVertexByTag(mesh, tokens[0]);
        if (vidx < 0)
            return "Unknown vertex index or tag: " + tokens[0];
    }

    if (vidx < 0 || vidx >= mesh.n_vertices())
        return "Vertex index out of range: " + tokens[0];

    mesh.vert_coupled_node_name[vidx] = tokens[1];

    // Optional discharge coefficient
    if (tokens.size() >= 3) {
        double cd = tryParseDouble(tokens[2], ok);
        if (ok) {
            mesh.vert_coupling_cd[vidx] = cd;
            mesh.vert_coupling_cd_set[vidx] = 1;
        }
    }

    // Optional exchange area
    if (tokens.size() >= 4) {
        double area = tryParseDouble(tokens[3], ok);
        if (ok) {
            mesh.vert_coupling_area[vidx] = area;
            mesh.vert_coupling_area_set[vidx] = 1;
        }
    }

    return {};
}

/**
 * @details Resolves zero-based unified cell index or exact first matching tag fallback,
 * including out-of-range numeric tags. Every row appends a distinct coupling (multiple nodes per
 * cell) and updates legacy per-cell name/CD/area mirror, last row wins there. Optional malformed
 * CD/area is ignored; no finite/sign/node-name check, retained AREA in project-length2. Array
 * mirrors change before vector append, so allocation failure can leave partial mutation. No
 * runtime owner rebuild or physics/calendar advance; exclusive authoring access and prepared
 * per-cell arrays required.
 */
std::string parse2DTriangleNodeMapLine(const std::vector<std::string>& tokens, MeshData& mesh) {
    if (tokens.size() < 2)
        return "Expected TRIANGLE_INDEX_OR_TAG SWMM_NODE_NAME [CD] [AREA]";

    // Token is TRIANGLE_INDEX_OR_TAG. Try a numeric index first, but fall back to
    // a tag lookup when the number is out of range (numeric triangle tags must
    // not be mis-read as indices and rejected).
    bool ok = false;
    int tidx = tryParseInt(tokens[0], ok);
    if (ok && (tidx < 0 || tidx >= mesh.n_triangles()))
        ok = false; // numeric but not a valid index — treat as a tag
    if (!ok) {
        tidx = findTriangleByTag(mesh, tokens[0]);
        if (tidx < 0)
            return "Unknown triangle index or tag: " + tokens[0];
    }

    if (tidx < 0 || tidx >= mesh.n_triangles())
        return "Triangle index out of range: " + tokens[0];

    // Repeated-row form: every line APPENDS a coupling row, so several nodes
    // may couple to the same triangle. (Previously per-triangle arrays were
    // overwritten — last line won.)
    MeshData::TriCouplingRow row;
    row.tri = tidx;
    row.node_name = tokens[1];

    if (tokens.size() >= 3) {
        double cd = tryParseDouble(tokens[2], ok);
        if (ok) {
            row.cd = cd;
            row.cd_set = true;
        }
    }

    if (tokens.size() >= 4) {
        double area = tryParseDouble(tokens[3], ok);
        if (ok) {
            row.area = area;
            row.area_set = true;
        }
    }

    // Keep the legacy per-triangle mirror in step (last row wins), same as
    // swmm_2d_add_triangle_coupling. Consumers that run BEFORE resolve —
    // the GeoPackage writer above all — read these arrays, so leaving them
    // to SurfaceRouter2D::initialize would drop couplings on an
    // opened-but-not-initialized model.
    mesh.tri_coupled_node_name[tidx] = row.node_name;
    mesh.tri_coupling_cd[tidx] = row.cd;
    mesh.tri_coupling_area[tidx] = row.area;

    mesh.tri_couplings.push_back(std::move(row));

    return {};
}

// ============================================================================
// Helper: build a section-level lambda that tokenizes each line and calls
// a line-oriented parser, reporting errors via the SimulationContext.
// ============================================================================

namespace {

using LineParser = std::function<std::string(const std::vector<std::string>&)>;

/**
 * @brief Create a raw-section callback retaining an owned nonempty row parser.
 * @param[in] line_parser Row parser called per nonempty tokenized line, captures its own stable
 * targets.
 * @return Callable mutating passed context error code5/message on first returned diagnostic,
 * preserving prior accepted rows; token/parser/allocation exceptions propagate. No
 * reset/rollback/physics or clock advance; captured authoring targets must outlive handler,
 * callbacks execute serially.
 */
input::SectionHandler makeSectionHandler(LineParser line_parser) {
    return [lp = std::move(line_parser)](openswmm::SimulationContext& ctx,
                                         const std::vector<std::string>& lines) {
        for (const auto& raw : lines) {
            auto tokens = openswmm::input::Tokenizer::tokenize(raw);
            if (tokens.empty())
                continue;
            std::string err = lp(tokens);
            if (!err.empty()) {
                // 5 = public SWMM_ERR_PARSE. Must be non-zero so InputReader
                // (which gates success on ctx.error_code == 0) treats the
                // section as failed; previously this was 1 (SWMM_ERR_NOMEM),
                // so every 2D section parse error surfaced as "Out of memory".
                ctx.error_code = 5;
                ctx.error_message = "[2D] " + err + " — line: " + raw;
                return;
            }
        }
    };
}

} // anonymous namespace

// ============================================================================
// V-E3 — [2D_BOUNDARY_CONDITIONS] line parser
// ============================================================================

/**
 * @param[in] tokens Empty or zero-based TRI EDGE TYPE [PARAM_1 [reserved [GROUP]]]. EDGE checked
 * against max four slots, actual cell shape/upper bound deferred. TYPE
 * wall/normal/stage/flow/series/ rating; '*' or missing parameter leaves default. PARAM_1 is
 * dimensionless slope, authored head, display flow per metre, or deferred table name by type.
 * Group copied from sixth token unless '*'.
 * @param[in,out] pending_rows Existing rows retained; appends one complete candidate on success.
 * @return Empty on empty/parsed row, otherwise arity/index/type/numeric diagnostic.
 * @note No table/mesh/physical finite validation; reserved fifth/surplus tokens ignored. No
 * scaling, activation or clock advance; allocation may throw, exclusive retained authoring
 * access.
 */
std::string
parse2DBoundaryConditionsLine(const std::vector<std::string>& tokens,
                              std::vector<SurfaceRouter2D::PendingBoundaryRow>& pending_rows) {
    if (tokens.empty())
        return {};
    if (tokens.size() < 3) {
        return "[2D_BOUNDARY_CONDITIONS] needs TRI EDGE TYPE [PARAM_1 [PARAM_2 [GROUP]]]";
    }

    bool ok = false;
    SurfaceRouter2D::PendingBoundaryRow row;
    row.tri = tryParseInt(tokens[0], ok);
    if (!ok || row.tri < 0)
        return "[2D_BOUNDARY_CONDITIONS] invalid TRI index";
    row.edge = tryParseInt(tokens[1], ok);
    if (!ok || row.edge < 0 || row.edge >= kMaxCellVerts)
        return "[2D_BOUNDARY_CONDITIONS] invalid EDGE (must be 0..2 for a "
               "triangle, 0..3 for a quad)";

    const std::string& type_tok = tokens[2];
    if (iequals(type_tok, "WALL") || iequals(type_tok, "WALL_DRAG"))
        row.bc_type = static_cast<int>(BoundaryType::WALL);
    else if (iequals(type_tok, "NORMAL_FLOW"))
        row.bc_type = static_cast<int>(BoundaryType::NORMAL_FLOW);
    else if (iequals(type_tok, "SPECIFIED_STAGE"))
        row.bc_type = static_cast<int>(BoundaryType::SPECIFIED_STAGE);
    else if (iequals(type_tok, "TS_STAGE"))
        row.bc_type = static_cast<int>(BoundaryType::SPECIFIED_STAGE);
    else if (iequals(type_tok, "SPECIFIED_FLOW"))
        row.bc_type = static_cast<int>(BoundaryType::SPECIFIED_FLOW);
    else if (iequals(type_tok, "TS_FLOW"))
        row.bc_type = static_cast<int>(BoundaryType::SPECIFIED_FLOW);
    else if (iequals(type_tok, "RATING_CURVE"))
        row.bc_type = static_cast<int>(BoundaryType::RATING_CURVE);
    else
        return "[2D_BOUNDARY_CONDITIONS] unknown TYPE: " + type_tok;

    // PARAM_1: typed by TYPE. For *_TS variants and RATING_CURVE the
    // parameter is a name. For NORMAL_FLOW it's the slope. For
    // SPECIFIED_STAGE it's the head. For SPECIFIED_FLOW it's the
    // per-metre discharge. "*" means "no value supplied".
    std::string p1 = (tokens.size() > 3) ? tokens[3] : "*";
    if (p1 == "*")
        p1.clear();

    // WALL keeps its legacy ignored parameter. WALL_DRAG explicitly opts in.
    if (iequals(type_tok, "WALL_DRAG")) {
        row.param1 = tryParseDouble(p1, ok);
        if (!ok || !std::isfinite(row.param1) || row.param1 < 0)
            return "WALL_DRAG requires a finite nonnegative dimensionless Cw";
    }
    // For Wall: PARAM_1 is irrelevant.
    if (!p1.empty() && !iequals(type_tok, "WALL") && !iequals(type_tok, "WALL_DRAG")) {
        if (iequals(type_tok, "NORMAL_FLOW") || iequals(type_tok, "SPECIFIED_STAGE") ||
            iequals(type_tok, "SPECIFIED_FLOW")) {
            row.param1 = tryParseDouble(p1, ok);
            if (!ok)
                return "[2D_BOUNDARY_CONDITIONS] non-numeric PARAM_1: " + p1;
        } else {
            // TS or curve name.
            row.name = p1;
        }
    }

    // PARAM_2 reserved; ignored today.
    // GROUP (optional).
    if (tokens.size() > 5 && tokens[5] != "*") {
        row.group = tokens[5];
    }

    pending_rows.push_back(std::move(row));
    return {};
}

// ============================================================================
// §11A — [2D_EDGE_CONVEYANCE] line parser
// ============================================================================

/**
 * @param[in] tokens Empty or FROM_VERTEX TO_VERTEX CONVEYANCE; distinct nonnegative zero-based
 * int vertex indices, dimensionless multiplier [0,1]. Surplus ignored.
 * @param[in,out] pending_rows Appends one parsed undirected vertex-pair candidate on success.
 * @return Empty on empty/success; arity/index/numeric/out-of-range multiplier diagnostic
 * otherwise.
 * @note Range is rejected, not clamped. strtod NaN passes ordinary comparisons, so this is not a
 * complete finite-value validator. Vertex existence/edge topology deferred; no scaling/physical
 * advance. Allocation may throw; exclusive authoring access.
 */
std::string
parse2DEdgeConveyanceLine(const std::vector<std::string>& tokens,
                          std::vector<SurfaceRouter2D::PendingEdgeConveyanceRow>& pending_rows) {
    if (tokens.empty())
        return {};
    if (tokens.size() < 3) {
        return "[2D_EDGE_CONVEYANCE] needs FROM_VERTEX TO_VERTEX CONVEYANCE";
    }

    SurfaceRouter2D::PendingEdgeConveyanceRow row;
    bool ok = false;

    row.v_from = tryParseInt(tokens[0], ok);
    if (!ok || row.v_from < 0)
        return "[2D_EDGE_CONVEYANCE] invalid FROM_VERTEX: " + tokens[0];

    row.v_to = tryParseInt(tokens[1], ok);
    if (!ok || row.v_to < 0)
        return "[2D_EDGE_CONVEYANCE] invalid TO_VERTEX: " + tokens[1];

    if (row.v_from == row.v_to)
        return "[2D_EDGE_CONVEYANCE] FROM_VERTEX and TO_VERTEX must differ";

    row.conveyance = tryParseDouble(tokens[2], ok);
    if (!ok)
        return "[2D_EDGE_CONVEYANCE] invalid CONVEYANCE: " + tokens[2];

    // Q1 — strict [0, 1] clamp at parse time.
    if (row.conveyance < 0.0 || row.conveyance > 1.0) {
        return "[2D_EDGE_CONVEYANCE] CONVEYANCE must be in [0, 1] (got " + tokens[2] + ")";
    }

    pending_rows.push_back(std::move(row));
    return {};
}

// ============================================================================
// §5.5 track I — [2D_INFILTRATION_OPTIONS] / [2D_INFILTRATION_DEFAULTS] /
// [2D_INFILTRATION] line parsers
// ============================================================================

namespace {

// Shared tail of a [2D_INFILTRATION_DEFAULTS] / [2D_INFILTRATION] row:
//
//     METHOD  [P1 .. P5]  [DEST]
//
// starting at tokens[first]. Parameter columns are POSITIONAL and carry
// PROJECT UNITS — they are stored verbatim; Infil2D::resolve() does the
// conversion. "-" means "unset" and leaves the slot at its default 0.
// Trailing columns a method does not use may be omitted (the writer trims
// them with infil2DParamCount), so a row's length varies by method. DEST is
// recognised as the final token when it is neither numeric nor "-"; absent,
// the Infil2DRow default (LOST) stands.
/**
 * @brief Parse positional infiltration method/parameters and optional final destination into a
 * candidate.
 * @param[in] tokens Full authored row token vector.
 * @param[in] first Method token index; must be within tokens for success.
 * @param[in] section Nonnull diagnostic section label.
 * @param[in,out] row Candidate method/has_method, up to five raw project-unit parameters and
 * optional destination flag. '-' leaves existing slot unchanged; untouched slots are not reset.
 * @return Empty on success, otherwise missing/method/destination/excess/numeric diagnostic.
 * @note Detects final nonnumeric non-'-' token as destination. Mutates candidate progressively
 * and does not enforce per-method arity/finite/sign/range/unit/ownership validation. Caller
 * stages row; no physical/rate/clock advance, allocation can throw.
 */
std::string parseInfil2DRowTail(const std::vector<std::string>& tokens, std::size_t first,
                                const char* section, Infil2DRow& row) {
    if (tokens.size() <= first)
        return std::string(section) + " missing METHOD";

    if (!parseInfil2DMethod(tokens[first], row.method, row.has_method))
        return std::string(section) + " unknown METHOD: " + tokens[first];

    std::size_t end = tokens.size();
    if (end > first + 1) {
        const std::string& last = tokens[end - 1];
        bool numeric = false;
        (void)tryParseDouble(last, numeric);
        if (!numeric && last != "-") {
            if (!parseInfil2DDest(last, row.dest))
                return std::string(section) + " unknown DEST: " + last;
            row.dest_explicit = true; // E2: keeps its own DEST over INFIL_DESTINATION
            --end;
        }
    }

    for (std::size_t k = first + 1; k < end; ++k) {
        const std::size_t idx = k - (first + 1);
        if (idx >= static_cast<std::size_t>(kInfil2DMaxParams))
            return std::string(section) + " too many parameter columns (max " +
                   std::to_string(kInfil2DMaxParams) + ")";
        if (tokens[k] == "-")
            continue; // unset
        bool ok = false;
        row.p[idx] = tryParseDouble(tokens[k], ok);
        if (!ok)
            return std::string(section) + " invalid parameter: " + tokens[k];
    }

    return {};
}

// makeSectionHandler's sibling for the [2D_INFILTRATION*] family. Those
// sections write into the Infil2D owned by SurfaceRouter2D, which — unlike the
// mesh, options and pending-row buffers — is reached through
// ctx.twod_io.infil rather than a captured reference. Null (engine built
// without 2D, or a detached context) means the section is skipped, matching
// how every other twod_io consumer runtime-guards.
using InfilLineParser = std::function<std::string(const std::vector<std::string>&, Infil2D&)>;

/**
 * @brief Create an infiltration-section callback using the context's current borrowed
 * infiltration owner.
 * @param[in] line_parser Owned nonempty row parser receiving tokens and writable Infil2D.
 * @return Callable skips a null ctx.twod_io.infil, otherwise tokenizes lines and stops on first
 * returned diagnostic with code5/raw-line message, retaining prior accepted rows. Owner must
 * remain stable/alive during serial callback. No rollback/physical advance; exceptions
 * propagate.
 */
input::SectionHandler makeInfilSectionHandler(InfilLineParser line_parser) {
    return [lp = std::move(line_parser)](openswmm::SimulationContext& ctx,
                                         const std::vector<std::string>& lines) {
        Infil2D* infil = ctx.twod_io.infil;
        if (!infil)
            return;
        for (const auto& raw : lines) {
            auto tokens = openswmm::input::Tokenizer::tokenize(raw);
            if (tokens.empty())
                continue;
            std::string err = lp(tokens, *infil);
            if (!err.empty()) {
                ctx.error_code = 5; // SWMM_ERR_PARSE (see makeSectionHandler)
                ctx.error_message = "[2D] " + err + " — line: " + raw;
                return;
            }
        }
    };
}

} // anonymous namespace

/**
 * @param[in] tokens Empty or INFIL_STEP duration (hh:mm:ss, hh:mm or seconds); surplus ignored.
 * @param[in,out] infil Replaces authored cadence seconds on accepted duration, otherwise
 * unchanged.
 * @return Empty on empty/accepted row, otherwise key/arity/negative parsed duration diagnostic.
 * @note Zero defers to wet-step resolution. This branch checks <0 without an independent finite
 * check. Does not resolve methods/advance infiltration capacity or clocks; serial authoring
 * only.
 */
std::string parse2DInfiltrationOptionsLine(const std::vector<std::string>& tokens, Infil2D& infil) {
    if (tokens.empty())
        return {};
    if (tokens.size() < 2)
        return "[2D_INFILTRATION_OPTIONS] needs PARAMETER VALUE";

    if (!iequals(tokens[0], "INFIL_STEP"))
        return "Unknown 2D_INFILTRATION_OPTIONS parameter: " + tokens[0];

    // Same duration grammar as WET_STEP / DRY_STEP in [OPTIONS].
    const double secs = openswmm::input::parse_time_seconds(tokens[1]);
    if (secs < 0.0)
        return "[2D_INFILTRATION_OPTIONS] invalid INFIL_STEP (expected "
               "hh:mm:ss >= 0): " +
               tokens[1];

    infil.options().infil_step = secs;
    return {};
}

/**
 * @param[in] tokens Empty or TAG|* METHOD [P1..P5] [DEST], method positional parameters in
 * project units; '-' retains zero default, recognized trailing destination optional.
 * @param[in,out] infil Appends a complete default/tag row on success; previous rows retained.
 * @return Empty on empty/success, otherwise grammar/method/destination/parameter diagnostic.
 * @note Defers tag/exact parameter/physical/ownership validation and unit conversion to
 * resolve(). No runtime rate/cumulative/clock advance; allocation may throw, exclusive authoring
 * access.
 */
std::string parse2DInfiltrationDefaultsLine(const std::vector<std::string>& tokens,
                                            Infil2D& infil) {
    if (tokens.empty())
        return {};
    if (tokens.size() < 2)
        return "[2D_INFILTRATION_DEFAULTS] needs TAG METHOD [P1..P5] [DEST]";

    Infil2DDefault entry;
    entry.tag = tokens[0];

    const std::string err = parseInfil2DRowTail(tokens, 1, "[2D_INFILTRATION_DEFAULTS]", entry.row);
    if (!err.empty())
        return err;

    infil.defaults().push_back(std::move(entry));
    return {};
}

/**
 * @param[in] tokens Empty or one-based CELL METHOD [P1..P5] [DEST], positional project-unit
 * parameters and optional final destination; '-' retains zero default.
 * @param[in,out] infil Appends complete per-cell override with stored zero-based cell on
 * success.
 * @return Empty on empty/success, otherwise cell lower-bound/grammar/method/parameter
 * diagnostic.
 * @note Mesh upper bound, physical parameters/ownership and unit conversion deferred to
 * resolve(). Does not alter prepared rate/cumulative/clock state; allocation can throw,
 * exclusive authoring only.
 */
std::string parse2DInfiltrationLine(const std::vector<std::string>& tokens, Infil2D& infil) {
    if (tokens.empty())
        return {};
    if (tokens.size() < 2)
        return "[2D_INFILTRATION] needs CELL METHOD [P1..P5] [DEST]";

    // CELL is 1-BASED in the file, 0-based in Infil2DOverride::tri. Only the
    // lower bound is checked here — the mesh may not be loaded yet, so the
    // upper bound is Infil2D::resolve()'s job.
    bool ok = false;
    const int cell = tryParseInt(tokens[0], ok);
    if (!ok || cell < 1)
        return "[2D_INFILTRATION] invalid CELL index (1-based): " + tokens[0];

    Infil2DOverride entry;
    entry.tri = cell - 1;

    const std::string err = parseInfil2DRowTail(tokens, 1, "[2D_INFILTRATION]", entry.row);
    if (!err.empty())
        return err;

    infil.overrides().push_back(std::move(entry));
    return {};
}

// ============================================================================
// [2D_INITIAL_QUALITY] — overland transport S1/S2
// ============================================================================

/**
 * @brief Parse one scoped initial surface native concentration authoring row.
 * @param[in] tokens Empty; '* SPECIES CONC'; 'TAG name SPECIES CONC'; or 'CELL n SPECIES CONC'.
 * Exact arity required, CELL positive one-based stored zero-based. CONC finite nonnegative
 * except case-insensitive __TEMPERATURE__ may be negative. Age retained in authored hours,
 * temperature degC.
 * @param[in,out] rows Existing rows retained; appends one complete candidate on success.
 * @return Empty on empty/success, otherwise scope/count/index/numeric/sign diagnostic.
 * @note Species/tag/mesh existence and canonical case/layout, age conversion and mass seeding
 * are separate initialization work. No water/native/clock change; allocation may throw, serial
 * authoring.
 */
std::string
parse2DInitialQualityLine(const std::vector<std::string>& tokens,
                          std::vector<SurfaceRouter2D::PendingInitialQualityRow>& rows) {
    if (tokens.empty())
        return {};
    // CELL <n> <species> <conc>  |  TAG <name> <species> <conc>  |  * <species> <conc>
    SurfaceRouter2D::PendingInitialQualityRow r;
    std::size_t at = 0;
    if (tokens[0] == "*") {
        if (tokens.size() != 3)
            return "[2D_INITIAL_QUALITY] '*' row needs SPECIES CONC";
        r.all = true;
        at = 1;
    } else if (iequals(tokens[0], "CELL")) {
        if (tokens.size() != 4)
            return "[2D_INITIAL_QUALITY] CELL row needs CELL <n> SPECIES CONC";
        bool ok = false;
        const int cell = tryParseInt(tokens[1], ok);
        if (!ok || cell < 1)
            return "[2D_INITIAL_QUALITY] invalid CELL index (1-based): " + tokens[1];
        r.tri = cell - 1; // upper bound is the router's, once the mesh exists
        at = 2;
    } else if (iequals(tokens[0], "TAG")) {
        if (tokens.size() != 4)
            return "[2D_INITIAL_QUALITY] TAG row needs TAG <name> SPECIES CONC";
        r.tag = tokens[1];
        at = 2;
    } else {
        return "[2D_INITIAL_QUALITY] row must start with CELL, TAG or '*': " + tokens[0];
    }
    r.species = tokens[at];
    bool okc = false;
    r.conc = tryParseDouble(tokens[at + 1], okc);
    // Celsius is signed; pollutant, reaction-species and age values remain
    // nonnegative. Never clamp an invalid authored value into another state.
    if (!okc || !std::isfinite(r.conc) || (r.conc < 0.0 && !iequals(r.species, "__TEMPERATURE__")))
        return "[2D_INITIAL_QUALITY] CONC must be finite and non-negative except for __TEMPERATURE__, got '" +
               tokens[at + 1] + "'";
    rows.push_back(std::move(r));
    return {};
}

/**
 * @brief Parse a finite local-bed native concentration seed without resolving a physical bed.
 * @param[in] tokens Empty or exact global/tag/one-based-CELL SPECIES CONC row, same scope
 * grammar as surface initial quality. CONC finite nonnegative, including any temperature-named
 * token here.
 * @param[in,out] rows Existing retained rows; appends one complete candidate on success.
 * @return Empty on empty/success, otherwise scope/arity/index/value diagnostic.
 * @note Bed owner/species support (including later temperature refusal) and mesh/tag/cell lookup
 * are deferred. Native concentration/age authored hours retained without conversion or physical
 * seeding. Allocation may throw, exclusive authoring access; no water/energy/clock advance.
 */
std::string
parse2DInitialBedQualityLine(const std::vector<std::string>& tokens,
                             std::vector<SurfaceRouter2D::PendingInitialBedQualityRow>& rows) {
    if (tokens.empty())
        return {};
    SurfaceRouter2D::PendingInitialBedQualityRow r;
    std::size_t at = 0;
    if (tokens[0] == "*") {
        if (tokens.size() != 3)
            return "[2D_INITIAL_BED_QUALITY] '*' row needs SPECIES CONC";
        r.all = true;
        at = 1;
    } else if (iequals(tokens[0], "CELL")) {
        if (tokens.size() != 4)
            return "[2D_INITIAL_BED_QUALITY] CELL row needs CELL <n> SPECIES CONC";
        bool ok = false;
        const int cell = tryParseInt(tokens[1], ok);
        if (!ok || cell < 1)
            return "[2D_INITIAL_BED_QUALITY] invalid CELL index (1-based): " + tokens[1];
        r.tri = cell - 1;
        at = 2;
    } else if (iequals(tokens[0], "TAG")) {
        if (tokens.size() != 4)
            return "[2D_INITIAL_BED_QUALITY] TAG row needs TAG <name> SPECIES CONC";
        r.tag = tokens[1];
        at = 2;
    } else {
        return "[2D_INITIAL_BED_QUALITY] row must start with CELL, TAG or '*': " + tokens[0];
    }
    r.species = tokens[at];
    bool okc = false;
    r.conc = tryParseDouble(tokens[at + 1], okc);
    if (!okc || !std::isfinite(r.conc) || r.conc < 0.0)
        return "[2D_INITIAL_BED_QUALITY] CONC must be a finite non-negative number, got '" +
               tokens[at + 1] + "'";
    rows.push_back(std::move(r));
    return {};
}

// ============================================================================
// [2D_BOUNDARY_QUALITY] — overland transport S2
// ============================================================================

/**
 * @brief Parse one zero-based surface boundary native concentration row.
 * @param[in] tokens Empty or exact TRI EDGE SPECIES CONC. TRI nonnegative zero-based unified
 * cell, EDGE in max four slots; actual cell bound/shape and boundary type checked later. CONC
 * finite nonnegative except case-insensitive __TEMPERATURE__ signed degC; age retained authored
 * hours.
 * @param[in,out] rows Appends one complete candidate on success, retaining earlier rows.
 * @return Empty on empty/success, otherwise count/index/numeric/sign diagnostic.
 * @note No table/native-row resolution, scaling, boundary flux, mass or clock change. Allocation
 * can throw; exclusive authoring access, initialization validates actual model identity.
 */
std::string
parse2DBoundaryQualityLine(const std::vector<std::string>& tokens,
                           std::vector<SurfaceRouter2D::PendingBoundaryQualityRow>& rows) {
    if (tokens.empty())
        return {};
    if (tokens.size() != 4)
        return "[2D_BOUNDARY_QUALITY] needs TRI EDGE SPECIES CONC";
    bool ok = false;
    SurfaceRouter2D::PendingBoundaryQualityRow r;
    r.tri = tryParseInt(tokens[0], ok);
    if (!ok || r.tri < 0)
        return "[2D_BOUNDARY_QUALITY] invalid TRI index: " + tokens[0];
    r.edge = tryParseInt(tokens[1], ok);
    if (!ok || r.edge < 0 || r.edge >= kMaxCellVerts)
        return "[2D_BOUNDARY_QUALITY] invalid EDGE (must be 0..2 for a "
               "triangle, 0..3 for a quad): " +
               tokens[1];
    r.species = tokens[2];
    bool okc = false;
    r.conc = tryParseDouble(tokens[3], okc);
    if (!okc || !std::isfinite(r.conc) || (r.conc < 0.0 && !iequals(r.species, "__TEMPERATURE__")))
        return "[2D_BOUNDARY_QUALITY] CONC must be finite and non-negative except for __TEMPERATURE__, got '" +
               tokens[3] + "'";
    rows.push_back(std::move(r));
    return {};
}

// ============================================================================
// register2DSections
// ============================================================================

/**
 * @brief Register mesh/options/native-quality/boundary/infiltration authoring section handlers.
 * @param[in,out] mesh Stable borrowed mesh target, must outlive callbacks.
 * @param[in,out] options Stable borrowed authoring options, must outlive callbacks.
 * @param[in,out] pending_bc_rows Borrowed retained hydraulic boundary rows, appended by
 * handlers.
 * @param[in,out] pending_ec_rows Borrowed retained undirected edge conveyance rows.
 * @param[in,out] pending_iq_rows Borrowed retained surface initial native quality rows.
 * @param[in,out] pending_ibq_rows Borrowed retained local-bed initial native quality rows.
 * @param[in,out] pending_bq_rows Borrowed retained boundary native quality rows.
 * @param[in,out] registry Receives custom handlers; conflict/replacement policy delegated to
 * registry.
 * @note Captured references remain externally owned. Infiltration target is taken from callback
 * context twod_io.infil (null skips). Mesh-file handler takes first FILE row only and preserves
 * raw unquoted path spacing. Row handlers tokenize/skip blanks, stop on first diagnostic with
 * context code5/raw line, retaining earlier rows; options receive warning sink for retired-key
 * compatibility. No automatic reset/rollback/model activation/clock advance. Allocation can
 * leave registration partial; callbacks/parsers may throw. Requires serial registry/model
 * lifecycle.
 */
void register2DSections(MeshData& mesh, SolverOptions2D& options,
                        std::vector<SurfaceRouter2D::PendingBoundaryRow>& pending_bc_rows,
                        std::vector<SurfaceRouter2D::PendingEdgeConveyanceRow>& pending_ec_rows,
                        std::vector<SurfaceRouter2D::PendingInitialQualityRow>& pending_iq_rows,
                        std::vector<SurfaceRouter2D::PendingInitialBedQualityRow>& pending_ibq_rows,
                        std::vector<SurfaceRouter2D::PendingBoundaryQualityRow>& pending_bq_rows,
                        input::SectionRegistry& registry) {
    // S1/S2: initial surface species concentration. Main .inp only in this
    // round — the .2dm sidecar's mini-registry does not carry it (recorded).
    registry.register_custom(
        "2D_INITIAL_QUALITY",
        makeSectionHandler([&pending_iq_rows](const std::vector<std::string>& tokens) {
            return parse2DInitialQualityLine(tokens, pending_iq_rows);
        }));
    registry.register_custom(
        "2D_INITIAL_BED_QUALITY",
        makeSectionHandler([&pending_ibq_rows](const std::vector<std::string>& tokens) {
            return parse2DInitialBedQualityLine(tokens, pending_ibq_rows);
        }));
    // S2: inflow concentration on a non-WALL boundary edge. Main .inp only.
    registry.register_custom(
        "2D_BOUNDARY_QUALITY",
        makeSectionHandler([&pending_bq_rows](const std::vector<std::string>& tokens) {
            return parse2DBoundaryQualityLine(tokens, pending_bq_rows);
        }));

    // Full-form handler (not makeSectionHandler): parse2DOptionsLine needs
    // ctx.warnings so retired CVODE-era keys warn-and-ignore on file load.
    registry.register_custom("2D_OPTIONS", [&options](openswmm::SimulationContext& ctx,
                                                      const std::vector<std::string>& lines) {
        for (const auto& raw : lines) {
            auto tokens = openswmm::input::Tokenizer::tokenize(raw);
            if (tokens.empty())
                continue;
            std::string err = parse2DOptionsLine(tokens, options, &ctx.warnings);
            if (!err.empty()) {
                ctx.error_code = 5; // SWMM_ERR_PARSE (see makeSectionHandler)
                ctx.error_message = "[2D] " + err + " — line: " + raw;
                return;
            }
        }
    });

    registry.register_custom("2D_VERTICES",
                             makeSectionHandler([&mesh](const std::vector<std::string>& tokens) {
                                 return parse2DVertexLine(tokens, mesh);
                             }));

    registry.register_custom("2D_TRIANGLES",
                             makeSectionHandler([&mesh](const std::vector<std::string>& tokens) {
                                 return parse2DTriangleLine(tokens, mesh);
                             }));

    registry.register_custom("2D_QUADS",
                             makeSectionHandler([&mesh](const std::vector<std::string>& tokens) {
                                 return parse2DQuadLine(tokens, mesh);
                             }));

    registry.register_custom("2D_INITIAL_VELOCITY",
                             makeSectionHandler([&mesh](const std::vector<std::string>& tokens) {
                                 return parse2DInitialVelocityLine(tokens, mesh);
                             }));

    registry.register_custom("2D_VERTEX_NODE_MAP",
                             makeSectionHandler([&mesh](const std::vector<std::string>& tokens) {
                                 return parse2DVertexNodeMapLine(tokens, mesh);
                             }));

    registry.register_custom("2D_TRIANGLE_NODE_MAP",
                             makeSectionHandler([&mesh](const std::vector<std::string>& tokens) {
                                 return parse2DTriangleNodeMapLine(tokens, mesh);
                             }));

    // V-E3 — [2D_BOUNDARY_CONDITIONS] accumulates pending rows; drained
    // during SurfaceRouter2D::initialize() after BoundaryData::resize.
    registry.register_custom(
        "2D_BOUNDARY_CONDITIONS",
        makeSectionHandler([&pending_bc_rows](const std::vector<std::string>& tokens) {
            return parse2DBoundaryConditionsLine(tokens, pending_bc_rows);
        }));

    // §11A — [2D_EDGE_CONVEYANCE] accumulates pending rows; drained
    // during SurfaceRouter2D::initialize() after buildMeshTopology so
    // the vertex-pair → (tri, edge_local) lookup table is available.
    registry.register_custom(
        "2D_EDGE_CONVEYANCE",
        makeSectionHandler([&pending_ec_rows](const std::vector<std::string>& tokens) {
            return parse2DEdgeConveyanceLine(tokens, pending_ec_rows);
        }));

    // §5.5 track I — per-cell infiltration. Rows land in the Infil2D reached
    // through ctx.twod_io.infil and are resolved against the mesh (tag/cell
    // precedence) in SurfaceRouter2D::initialize().
    registry.register_custom("2D_INFILTRATION_OPTIONS",
                             makeInfilSectionHandler(parse2DInfiltrationOptionsLine));

    registry.register_custom("2D_INFILTRATION_DEFAULTS",
                             makeInfilSectionHandler(parse2DInfiltrationDefaultsLine));

    registry.register_custom("2D_INFILTRATION", makeInfilSectionHandler(parse2DInfiltrationLine));

    // [2D_MESH_FILE] — capture only the first FILE token; mesh is loaded
    // after the main .inp is fully parsed (see SWMMEngine::open).
    registry.register_custom("2D_MESH_FILE", [&options](openswmm::SimulationContext& /*ctx*/,
                                                        const std::vector<std::string>& lines) {
        for (const auto& raw : lines) {
            auto tokens = openswmm::input::Tokenizer::tokenize(raw);
            if (tokens.size() >= 2 && iequals(tokens[0], "FILE")) {
                // Standard FILE rows consume the raw tail so unquoted
                // filenames retain repeated spaces. Keep CSV compatibility.
                const auto line = openswmm::input::Tokenizer::trim(
                    openswmm::input::Tokenizer::strip_comment(raw));
                std::string path = tokens[1];
                for (std::size_t k = 2; k < tokens.size(); ++k) {
                    path += ' ';
                    path += tokens[k];
                }
                if (line.size() > 4 && std::isspace(static_cast<unsigned char>(line[4]))) {
                    const auto tail = openswmm::input::Tokenizer::trim(line.substr(4));
                    if (!tail.empty() && tail.front() != '"' && tail.front() != ',')
                        path = std::string(tail);
                }
                options.mesh_file = path;
                return; // only first FILE line
            }
        }
    });
}

// ============================================================================
// load2DMeshExternalFile
// ============================================================================

/**
 * @brief Read a mesh sidecar through a local registry into caller-supplied authoring targets.
 * @param[in,out] mesh Borrowed mesh append target; caller clears/reconciles inline geometry as
 * needed before this call. This loader does not itself clear mesh or pending boundary stores.
 * @param[in,out] opts Authoring option target and mesh-units marker, may change during partial
 * read.
 * @param[in,out] pending_bc_rows Retained hydraulic boundary rows; sidecar lines append.
 * @param[in,out] pending_ec_rows Retained edge conveyance rows; sidecar lines append.
 * @param[in,out] infil Optional target; after successful read, nonempty sidecar
 * defaults/overrides replace corresponding target vectors, positive sidecar cadence replaces
 * options. Empty/zero keeps inline values, including a present section that produces no rows.
 * @param[in] mesh_file UTF-8 absolute or relative sidecar path.
 * @param[in] inp_base_dir UTF-8 base directory for relative mesh_file, empty leaves it relative.
 * @param[in,out] warnings Optional borrowed retired-option warning sink; null makes those
 * options hard errors. Other row diagnostics return in result.
 * @param[in,out] pending_iq_rows Optional borrowed surface seed target; null omits its handler.
 * @param[in,out] pending_ibq_rows Optional borrowed local-bed seed target; null omits its
 * handler.
 * @param[in,out] pending_bq_rows Optional borrowed boundary-quality target; null omits its
 * handler.
 * @return Empty on successful InputReader read/adoption; otherwise path-qualified parse/read
 * diagnostic.
 * @note Registry excludes 2D_MESH_FILE to prevent recursion. Pre-scans SI marker, then uses
 * detached context for reader errors. Mesh/options/pending rows are direct targets, so errors
 * may leave partial append/mutation; only infiltration parses privately before adoption. No unit
 * conversion/topology build/physical advance. Filesystem/allocation/parser exceptions may
 * propagate. Exclusive authoring access; every referenced target must remain stable/alive
 * through synchronous read.
 */
std::string
load2DMeshExternalFile(MeshData& mesh, SolverOptions2D& opts,
                       std::vector<SurfaceRouter2D::PendingBoundaryRow>& pending_bc_rows,
                       std::vector<SurfaceRouter2D::PendingEdgeConveyanceRow>& pending_ec_rows,
                       Infil2D* infil, const std::string& mesh_file,
                       const std::string& inp_base_dir, std::vector<std::string>* warnings,
                       std::vector<SurfaceRouter2D::PendingInitialQualityRow>* pending_iq_rows,
                       std::vector<SurfaceRouter2D::PendingInitialBedQualityRow>* pending_ibq_rows,
                       std::vector<SurfaceRouter2D::PendingBoundaryQualityRow>* pending_bq_rows) {
    namespace fs = std::filesystem;

    // Resolve path
    // utf8_path, not fs::path(std::string): both of these come from the model
    // as UTF-8 and the latter decodes in the ANSI code page (issue #7).
    fs::path p = openswmm::io::utf8_path(mesh_file);
    if (p.is_relative() && !inp_base_dir.empty())
        p = openswmm::io::utf8_path(inp_base_dir) / p;

    // Build a minimal registry (no 2D_MESH_FILE — prevents recursion)
    openswmm::input::SectionRegistry mini;
    mini.register_custom(
        "2D_OPTIONS", makeSectionHandler([&opts, warnings](const std::vector<std::string>& tokens) {
            return parse2DOptionsLine(tokens, opts, warnings);
        }));
    mini.register_custom("2D_VERTICES",
                         makeSectionHandler([&mesh](const std::vector<std::string>& tokens) {
                             return parse2DVertexLine(tokens, mesh);
                         }));
    mini.register_custom("2D_TRIANGLES",
                         makeSectionHandler([&mesh](const std::vector<std::string>& tokens) {
                             return parse2DTriangleLine(tokens, mesh);
                         }));
    mini.register_custom("2D_QUADS",
                         makeSectionHandler([&mesh](const std::vector<std::string>& tokens) {
                             return parse2DQuadLine(tokens, mesh);
                         }));
    mini.register_custom("2D_INITIAL_VELOCITY",
                         makeSectionHandler([&mesh](const std::vector<std::string>& tokens) {
                             return parse2DInitialVelocityLine(tokens, mesh);
                         }));
    mini.register_custom("2D_VERTEX_NODE_MAP",
                         makeSectionHandler([&mesh](const std::vector<std::string>& tokens) {
                             return parse2DVertexNodeMapLine(tokens, mesh);
                         }));
    mini.register_custom("2D_TRIANGLE_NODE_MAP",
                         makeSectionHandler([&mesh](const std::vector<std::string>& tokens) {
                             return parse2DTriangleNodeMapLine(tokens, mesh);
                         }));

    // V-E3 — external .2dm may carry its own [2D_BOUNDARY_CONDITIONS].
    mini.register_custom(
        "2D_BOUNDARY_CONDITIONS",
        makeSectionHandler([&pending_bc_rows](const std::vector<std::string>& tokens) {
            return parse2DBoundaryConditionsLine(tokens, pending_bc_rows);
        }));

    // S2/S3 — external .2dm may carry the surface quality sections too (they
    // travel with the mesh they address). Registered only when the caller
    // passes the pending stores, so older callers are unchanged.
    if (pending_iq_rows)
        mini.register_custom(
            "2D_INITIAL_QUALITY",
            makeSectionHandler([pending_iq_rows](const std::vector<std::string>& tokens) {
                return parse2DInitialQualityLine(tokens, *pending_iq_rows);
            }));
    if (pending_ibq_rows)
        mini.register_custom(
            "2D_INITIAL_BED_QUALITY",
            makeSectionHandler([pending_ibq_rows](const std::vector<std::string>& tokens) {
                return parse2DInitialBedQualityLine(tokens, *pending_ibq_rows);
            }));
    if (pending_bq_rows)
        mini.register_custom(
            "2D_BOUNDARY_QUALITY",
            makeSectionHandler([pending_bq_rows](const std::vector<std::string>& tokens) {
                return parse2DBoundaryQualityLine(tokens, *pending_bq_rows);
            }));

    // §11A — external .2dm may carry its own [2D_EDGE_CONVEYANCE].
    mini.register_custom(
        "2D_EDGE_CONVEYANCE",
        makeSectionHandler([&pending_ec_rows](const std::vector<std::string>& tokens) {
            return parse2DEdgeConveyanceLine(tokens, pending_ec_rows);
        }));

    // §5.5.5 — [2D_INFILTRATION*] are per-cell mesh attributes, so they follow
    // the mesh into the .2dm. The registry the main .inp uses reaches its
    // target through ctx.twod_io.infil, which the detached context below does
    // not carry, so the sidecar rows are parsed into a scratch object bound
    // here by reference and transplanted after the read. Per SECTION
    // precedence: a section the sidecar carries REPLACES the inline one
    // (external overrides inline, the [2D_MESH_FILE] rule) rather than
    // appending to it, so no row can be counted twice; sections the sidecar
    // omits keep whatever the .inp supplied.
    Infil2D sidecar_infil;
    mini.register_custom(
        "2D_INFILTRATION_OPTIONS",
        makeSectionHandler([&sidecar_infil](const std::vector<std::string>& tokens) {
            return parse2DInfiltrationOptionsLine(tokens, sidecar_infil);
        }));
    mini.register_custom(
        "2D_INFILTRATION_DEFAULTS",
        makeSectionHandler([&sidecar_infil](const std::vector<std::string>& tokens) {
            return parse2DInfiltrationDefaultsLine(tokens, sidecar_infil);
        }));
    mini.register_custom(
        "2D_INFILTRATION",
        makeSectionHandler([&sidecar_infil](const std::vector<std::string>& tokens) {
            return parse2DInfiltrationLine(tokens, sidecar_infil);
        }));

    // The external .2dm may carry its own `;; UNITS:` header. Scan first
    // so SurfaceRouter2D::initialize sees the right flag before it runs.
    // path_utf8, not p.string(): both callees take a UTF-8 std::string and
    // re-apply utf8_path themselves, and string() would re-encode to ANSI.
    const std::string p_utf8 = openswmm::io::path_utf8(p);
    prescan2DUnitsHeader(p_utf8, opts);

    openswmm::input::InputReader reader(mini);
    openswmm::SimulationContext dummy;
    if (!reader.read(p_utf8, dummy)) {
        return "2D_MESH_FILE: error reading '" + p_utf8 + "': " + dummy.error_message;
    }

    if (infil != nullptr) {
        if (!sidecar_infil.defaults().empty())
            infil->defaults() = std::move(sidecar_infil.defaults());
        if (!sidecar_infil.overrides().empty())
            infil->overrides() = std::move(sidecar_infil.overrides());
        if (sidecar_infil.options().infil_step > 0.0)
            infil->options() = sidecar_infil.options();
    }
    return {};
}

// ============================================================================
// prescan2DUnitsHeader
// ============================================================================

/**
 * @brief Scan an entire UTF-8 input/mesh file for a recognized SI mesh-units comment marker.
 * @param[in] inp_path UTF-8 file path, relative paths use process working directory.
 * @param[in,out] opts Sets mesh_units_si=true on case-insensitive ';; UNITS: SI (m)' or metric
 * synonym; no recognized marker, unknown units or ft leaves current flag unchanged. Never resets
 * false.
 * @note Missing/unreadable file silently returns so caller can report normal read errors. Scans
 * to EOF because writer can emit marker inside sections, trims whitespace by borrowed views;
 * does not parse topology or convert coordinates. File/string allocation can throw; exclusive
 * options edit. Multiple markers only enable SI, so later unknown/nonmetric text does not undo
 * an earlier match.
 */
void prescan2DUnitsHeader(const std::string& inp_path, SolverOptions2D& opts) {
    std::ifstream in(openswmm::io::utf8_path(inp_path));
    if (!in)
        return; // file missing — caller will surface the error

    // Views, not strings. This pass reads the ENTIRE .inp — see the note below
    // on why it cannot stop early — so on a large model it visits millions of
    // lines. The previous by-value `trim(std::string)` allocated twice per
    // line (the parameter copy and the substr result) for a scan that almost
    // always finds nothing.
    const auto trim = [](std::string_view s) noexcept {
        const auto issp = [](unsigned char c) { return std::isspace(c) != 0; };
        while (!s.empty() && issp(static_cast<unsigned char>(s.back())))
            s.remove_suffix(1);
        while (!s.empty() && issp(static_cast<unsigned char>(s.front())))
            s.remove_prefix(1);
        return s;
    };

    // NB: this deliberately scans to EOF rather than stopping at the first
    // "[SECTION]" header. The header is not always in the pre-section prefix —
    // InpWriter emits `;; UNITS: SI (m)` underneath [2D_VERTICES] (InpWriter.cpp
    // writeMesh2D), so every .inp the engine itself writes carries it mid-file.
    // Stopping early would silently drop SI mesh scaling on round-trip.
    // Recognized SI matches enable the flag; other markers leave it unchanged.
    std::string line;
    while (std::getline(in, line)) {
        const std::string_view t = trim(line);
        if (t.size() < 2 || t[0] != ';' || t[1] != ';')
            continue;
        std::string_view rest = trim(t.substr(2));
        // Match "UNITS:" prefix case-insensitively.
        constexpr std::string_view kKey = "UNITS:";
        if (rest.size() < kKey.size())
            continue;
        bool match = true;
        for (std::size_t i = 0; i < kKey.size(); ++i) {
            if (std::toupper(static_cast<unsigned char>(rest[i])) != kKey[i]) {
                match = false;
                break;
            }
        }
        if (!match)
            continue;
        const std::string_view value = trim(rest.substr(kKey.size()));
        // Recognised metric markers.  Anything else (including absent /
        // unknown / explicit "ft") leaves the flag at its current value.
        const bool si = iequals(value, "SI (m)") || iequals(value, "m") ||
                        iequals(value, "metre") || iequals(value, "metres") ||
                        iequals(value, "meter") || iequals(value, "meters");
        if (si)
            opts.mesh_units_si = true;
        // Continue scanning for recognized SI markers after section headers.
    }
}

} // namespace openswmm::twoD
