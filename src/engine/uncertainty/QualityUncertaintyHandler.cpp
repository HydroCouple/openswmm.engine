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

input::SectionHandler makeSectionHandler(LineParser line_parser) {
    return [lp = std::move(line_parser)](
        openswmm::SimulationContext& ctx,
        const std::vector<std::string>& lines)
    {
        for (const auto& raw : lines) {
            auto tokens = openswmm::input::Tokenizer::tokenize(raw);
            if (tokens.empty()) continue;

            std::string err = lp(tokens);
            if (!err.empty()) {
                ctx.error_code = 101; // SWMM_ERR_PARSE_LINE
                ctx.error_message = "Error parsing [UNCERTAINTY] line: " + err;
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
        if (tokens.size() >= 4) dist_str  = tokens[3];
        if (tokens.size() >= 5) entry_str = tokens[4];
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
    std::string param_upper = param_str;
    std::transform(param_upper.begin(), param_upper.end(), param_upper.begin(),
                   [](unsigned char c){ return static_cast<char>(std::toupper(c)); });

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
    spec.name        = param_upper;
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
        makeSectionHandler([&config](const std::vector<std::string>& tokens) {
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
        } else {
            return "Unknown option '" + tokens[i] + "' (supported: FORCE_LOCATION, NODES <file>)";
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
        makeSectionHandler([&config](const std::vector<std::string>& tokens) {
            return parseSoftRainfallGridLine(tokens, config);
        }));
}

// ============================================================================
// parseSoftRaingagesLine (SR-1a, design §3.1)
// ============================================================================

std::string parseSoftRaingagesLine(openswmm::SimulationContext& ctx,
                                   const std::vector<std::string>& tokens)
{
    if (tokens.size() < 4)
        return "Expected: Gage Family SpreadKind SpreadSource";

    const std::string& gage_name = tokens[0];
    const int gage_idx = ctx.gage_names.find(gage_name);
    if (gage_idx < 0)
        return "Unknown gage '" + gage_name + "'";

    auto ui = static_cast<std::size_t>(gage_idx);
    ctx.soft_rain.grow_to(ctx.gage_names.size());

    DistType family;
    if (iequals(tokens[1], "UNIFORM")) family = DistType::UNIFORM;
    else if (iequals(tokens[1], "NORMAL")) family = DistType::NORMAL;
    else if (iequals(tokens[1], "LOGNORMAL")) family = DistType::LOGNORMAL;
    else return "Unsupported Family '" + tokens[1] + "' (supported: UNIFORM, NORMAL, LOGNORMAL)";

    SoftSpreadKind sk;
    if (iequals(tokens[2], "SD")) sk = SoftSpreadKind::SD;
    else if (iequals(tokens[2], "CV")) sk = SoftSpreadKind::CV;
    else if (iequals(tokens[2], "HALFRANGE")) sk = SoftSpreadKind::HALFRANGE;
    else return "Unsupported SpreadKind '" + tokens[2] + "' (supported: SD, CV, HALFRANGE)";

    if (sk == SoftSpreadKind::HALFRANGE && family != DistType::UNIFORM)
        return "HALFRANGE is only valid with UNIFORM family";
    if ((sk == SoftSpreadKind::SD || sk == SoftSpreadKind::CV) && family == DistType::UNIFORM)
        return "SD/CV are not valid with UNIFORM family; use HALFRANGE";

    ctx.soft_rain.family[ui] = family;
    ctx.soft_rain.spread_kind[ui] = sk;
    ctx.soft_rain.spread_const[ui] = 0.0;
    ctx.soft_rain.spread_ts[ui] = -1;
    ctx.soft_rain.spread_ts_name[ui].clear();
    ctx.soft_rain.configured[ui] = true;

    if (iequals(tokens[3], "TIMESERIES")) {
        if (tokens.size() < 5)
            return "TIMESERIES spread source requires a timeseries name";
        ctx.soft_rain.spread_ts_name[ui] = tokens[4];
        ctx.soft_rain.spread_ts[ui] = ctx.table_names.find(tokens[4]);
    } else {
        bool ok = false;
        const double spread = tryParseDouble(tokens[3], ok);
        if (!ok)
            return "SpreadSource must be a non-negative number or TIMESERIES <name>";
        if (spread < 0.0)
            return "SpreadSource must be non-negative";
        ctx.soft_rain.spread_const[ui] = spread;
    }

    return {};
}

// ============================================================================
// registerSoftRaingagesSection (SR-1a)
// ============================================================================

void registerSoftRaingagesSection(input::SectionRegistry& registry)
{
    registry.register_custom("SOFT_RAINGAGES",
        [](openswmm::SimulationContext& ctx, const std::vector<std::string>& lines) {
            for (const auto& raw : lines) {
                auto tokens = openswmm::input::Tokenizer::tokenize(raw);
                if (tokens.empty()) continue;
                std::string err = parseSoftRaingagesLine(ctx, tokens);
                if (!err.empty()) {
                    ctx.error_code = 101;
                    ctx.error_message = "Error parsing [SOFT_RAINGAGES] line: " + err;
                    return;
                }
            }
        });
}

} // namespace openswmm::uncertainty