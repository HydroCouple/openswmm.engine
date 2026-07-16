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

} // namespace openswmm::uncertainty