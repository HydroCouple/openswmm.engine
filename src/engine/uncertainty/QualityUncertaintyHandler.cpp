/**
 * @file QualityUncertaintyHandler.cpp
 * @brief Implementation of input section parser for the QUALITY uncertainty layer.
 *
 * @ingroup engine_uncertainty
 */

#include "QualityUncertaintyHandler.hpp"
#include "../core/SimulationContext.hpp"
#include "../input/Tokenizer.hpp"
#include "UncertaintyTypes.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <utility>

namespace openswmm::uncertainty {

namespace {

// Case-insensitive string comparison
bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin(),
                      [](char ca, char cb) { return std::tolower(ca) == std::tolower(cb); });
}

// Try to parse a string as a double, returning success status
double tryParseDouble(const std::string& s, bool& ok) {
    ok = false;
    try {
        std::size_t pos = 0;
        double val = std::stod(s, &pos);
        if (pos == s.size()) {
            ok = true;
            return val;
        }
    } catch (const std::exception&) {
        // Fall through
    }
    return 0.0;
}

// Helper: build a section-level lambda that tokenizes each line and calls
// a line-oriented parser, reporting errors via the SimulationContext.
using LineParser = std::function<std::string(const std::vector<std::string>&)>;

input::SectionHandler makeSectionHandler(std::string section_name,
                                         LineParser line_parser) {
    return [sn = std::move(section_name), lp = std::move(line_parser)](
        openswmm::SimulationContext& ctx,
        const std::vector<std::string>& lines)
    {
        for (const auto& raw : lines) {
            auto tokens = openswmm::input::Tokenizer::tokenize(raw);
            if (tokens.empty()) continue;

            std::string err = lp(tokens);
            if (!err.empty()) {
                ctx.error_code = 101; // SWMM_ERR_PARSE_LINE
                ctx.error_message = "Error parsing [" + sn + "] line: " + err;
                return;
            }
        }
    };
}

} // anonymous namespace

// ============================================================================
// parseQualityUncertaintyLine
// ============================================================================

std::string parseQualityUncertaintyLine(
    const std::vector<std::string>& tokens,
    UncertaintyConfig& config)
{
    // Grammar (PR 9c — PARAMETER_REGISTRY.md §6):
    //   new    : LAYER NAME PERT [DIST] [ENTRY]     e.g. "1D INFLOW 0.3 LOGNORMAL FORCING_VECTOR"
    //   legacy : LAYER NAME [DIST] PERT             e.g. "2D RAINFALL UNIFORM 0.15"
    // Disambiguation: if tokens[2] parses as a number the new order is in
    // effect, otherwise the legacy DIST-first order.
    if (tokens.size() < 3)
        return "Expected LAYER NAME PERT [DIST] [ENTRY]";

    const auto& layer_str = tokens[0];
    const auto& param_str = tokens[1];

    bool ok = false;
    std::string dist_str  = "UNIFORM";
    std::string entry_str;                 // empty = derive from NAME
    double      pert      = 0.0;

    const double t2_num = tryParseDouble(tokens[2], ok);
    if (ok) {
        // New order: PERT [DIST] [ENTRY]
        pert = t2_num;
        // tokens[3] could be DIST or ENTRY (DIST omitted). If it's a known
        // ENTRY keyword, treat it as ENTRY and keep the default DIST.
        auto is_entry_kw = [](const std::string& s) {
            return iequals(s, "QUALITY_MULT");
        };
        if (tokens.size() >= 4) {
            if (is_entry_kw(tokens[3]))
                entry_str = tokens[3];
            else
                dist_str  = tokens[3];
        }
        if (tokens.size() >= 5 && entry_str.empty()) entry_str = tokens[4];
    } else {
        // Legacy order: DIST PERT
        if (tokens.size() < 4)
            return "Expected LAYER NAME PERT [DIST] [ENTRY] (or legacy LAYER NAME DIST PERT)";
        dist_str = tokens[2];
        pert = tryParseDouble(tokens[3], ok);
        if (!ok)
            return "PERTURBATION must be a non-negative number";
    }
    if (pert < 0.0)
        return "PERTURBATION must be a non-negative number";

    // --- Parse LAYER ---
    LayerTarget layer;
    if (iequals(layer_str, "QUALITY"))
        layer = LayerTarget::QUALITY;
    else
        return "This function only supports LAYER 'QUALITY'";

    // --- Parse DISTRIBUTION ---
    DistType dist;
    if (iequals(dist_str, "UNIFORM"))
        dist = DistType::UNIFORM;
    else
        return "QUALITY layer only supports UNIFORM distribution";

    // --- Resolve NAME and ENTRY ---
    // Preserve the pollutant NAME exactly as written on the [UNCERTAINTY] line.
    // Pollutant names in NameIndex are stored verbatim from [POLLUTANTS] and
    // looked up case-sensitively, so uppercasing here would break resolution
    // for any non-uppercase pollutant name.
    ParamEntry entry;
    if (!entry_str.empty()) {
        // Explicit ENTRY — accepts any pollutant NAME.
        if (iequals(entry_str, "QUALITY_MULT"))
            entry = ParamEntry::QUALITY_MULT;
        else
            return "QUALITY layer only supports ENTRY 'QUALITY_MULT'";
    } else {
        // Pollutant names are resolved to indices later during engine init.
        entry = ParamEntry::QUALITY_MULT;
    }

    // --- Build spec and record ---
    UncertaintySourceSpec spec;
    spec.name        = param_str;
    spec.layer       = layer;
    spec.dist        = dist;
    spec.perturbation = pert;
    spec.entry       = entry;
    config.sources.push_back(spec);

    return {};
}

// ============================================================================
// registerQualityUncertaintySection
// ============================================================================

void registerQualityUncertaintySection(UncertaintyConfig& config,
                                       input::SectionRegistry& registry)
{
    // [UNCERTAINTY] — scalar parameter uncertainty for QUALITY layer.
    registry.register_custom("UNCERTAINTY",
        makeSectionHandler("UNCERTAINTY", [&config](const std::vector<std::string>& tokens) {
            return parseQualityUncertaintyLine(tokens, config);
        }));
}

// ============================================================================
// parseSoftRainfallGridLine (SR-2b, design §3.2)
// ============================================================================

std::string parseSoftRainfallGridLine(
    const std::vector<std::string>& tokens,
    UncertaintyConfig& config)
{
    // Grammar: Target File Mapping [Options]
    //   Target:   2D | RUNOFF | INFLOWS
    //   File:     quoted path (may contain spaces if quoted — but the tokenizer
    //             strips quotes, so a simple token is expected)
    //   Mapping:  CENTROID | BILINEAR | AREA_MEAN
    //   Options:  FORCE_LOCATION | NODES <file>
    if (tokens.size() < 3)
        return "Expected: Target File Mapping [Options]";

    // --- Parse Target ---
    GridTarget target;
    if (iequals(tokens[0], "2D"))
        target = GridTarget::TWO_D;
    else if (iequals(tokens[0], "RUNOFF"))
        target = GridTarget::RUNOFF;
    else if (iequals(tokens[0], "INFLOWS"))
        target = GridTarget::INFLOWS;
    else
        return "Unknown Target '" + tokens[0] + "' (supported: 2D, RUNOFF, INFLOWS)";

    // --- Parse File path ---
    std::string file_path = tokens[1];
    // Strip surrounding quotes if present
    if (file_path.size() >= 2 && file_path.front() == '"' && file_path.back() == '"')
        file_path = file_path.substr(1, file_path.size() - 2);

    // Existence check (deferred resolution for relative paths happens at engine init)
    // — we only check at parse time if the path is absolute and doesn't exist
    // (relative paths may resolve against the .inp directory later)

    // --- Parse Mapping ---
    GridMapping mapping;
    if (iequals(tokens[2], "CENTROID"))
        mapping = GridMapping::CENTROID;
    else if (iequals(tokens[2], "BILINEAR"))
        mapping = GridMapping::BILINEAR;
    else if (iequals(tokens[2], "AREA_MEAN"))
        mapping = GridMapping::AREA_MEAN;
    else
        return "Unknown Mapping '" + tokens[2] + "' (supported: CENTROID, BILINEAR, AREA_MEAN)";

    // --- Parse Options (tokens 3+) ---
    bool force_location = false;
    std::string nodes_file;
    Coherence coherence = Coherence::FULL;
    double corr_len = 0.0;
    for (std::size_t i = 3; i < tokens.size(); ++i) {
        if (iequals(tokens[i], "FORCE_LOCATION")) {
            force_location = true;
        } else if (iequals(tokens[i], "NODES")) {
            if (i + 1 >= tokens.size())
                return "NODES option requires a file path";
            nodes_file = tokens[++i];
            // Strip quotes
            if (nodes_file.size() >= 2 && nodes_file.front() == '"' && nodes_file.back() == '"')
                nodes_file = nodes_file.substr(1, nodes_file.size() - 2);
        } else if (iequals(tokens[i], "COHERENCE")) {
            // COHERENCE FULL | COHERENCE CORR_LEN <meters>  (design §6, CL-1a)
            if (i + 1 >= tokens.size())
                return "COHERENCE requires a mode (FULL or CORR_LEN <meters>)";
            const std::string& mode = tokens[++i];
            if (iequals(mode, "FULL")) {
                coherence = Coherence::FULL;
                corr_len  = 0.0;
            } else if (iequals(mode, "CORR_LEN")) {
                if (i + 1 >= tokens.size())
                    return "CORR_LEN requires a positive numeric argument (meters)";
                bool ok2 = false;
                const double v = tryParseDouble(tokens[++i], ok2);
                if (!ok2)
                    return "CORR_LEN argument must be a numeric value (meters)";
                if (v <= 0.0)
                    return "CORR_LEN must be a positive number of meters";
                coherence = Coherence::CORR_LEN;
                corr_len  = v;
            } else {
                return "COHERENCE mode '" + mode + "' not yet supported (use FULL or CORR_LEN <meters>)";
            }
        } else {
            return "Unknown option '" + tokens[i] + "' (supported: FORCE_LOCATION, NODES <file>, COHERENCE FULL|CORR_LEN <meters>)";
        }
    }

    // --- Validate target-specific requirements ---
    if (target == GridTarget::INFLOWS && nodes_file.empty()) {
        // NODES file is recommended for INFLOWS but not strictly required
        // (empty = all nodes); just note it in the spec
    }

    // --- Build spec and record ---
    SoftGridSourceSpec spec;
    spec.file_path      = file_path;
    spec.target         = target;
    spec.mapping        = mapping;
    spec.force_location = force_location;
    spec.nodes_file     = nodes_file;
    spec.coherence      = coherence;
    spec.corr_len       = corr_len;
    config.grid_sources.push_back(spec);

    return {};
}

// ============================================================================
// registerSoftRainfallGridSection (SR-2b)
// ============================================================================

void registerSoftRainfallGridSection(UncertaintyConfig& config,
                                     input::SectionRegistry& registry)
{
    // [SOFT_RAINFALL_GRID] — gridded rainfall input (SR-2b, design §3.2).
    registry.register_custom("SOFT_RAINFALL_GRID",
        makeSectionHandler("SOFT_RAINFALL_GRID", [&config](const std::vector<std::string>& tokens) {
            return parseSoftRainfallGridLine(tokens, config);
        }));
}

// parseSoftRaingagesLine / registerSoftRaingagesSection (SR-1a, the
// [SOFT_RAINGAGES] gage-level soft-rainfall parser) intentionally not ported
// onto this base: they read SimulationContext::soft_rain and
// uncertainty::SoftSpreadKind/GageCoherence, none of which exist here — that
// engine-side state is SR-1b, a separate and still-unstarted feature, not
// part of SR-2c deterministic gridded rainfall. See PORT_V2_PREFLIGHT.md.

} // namespace openswmm::uncertainty