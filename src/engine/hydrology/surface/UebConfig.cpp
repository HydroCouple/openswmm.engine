// SPDX-License-Identifier: Apache-2.0
#include "UebConfig.hpp"
#include "../../input/InputParseUtils.hpp"
#include "../../input/Tokenizer.hpp"
#include <cmath>
#include <iomanip>
#include <sstream>

namespace openswmm::surface {
namespace {
struct Parameter { const char* key; double ueb::Parameters::*member; };
constexpr Parameter parameters[] = {
    {"Z0", &ueb::Parameters::roughness}, {"ZM", &ueb::Parameters::measurement_height},
    {"KS", &ueb::Parameters::equilibrium_conductance}, {"KSAT", &ueb::Parameters::drainage_conductivity},
    {"LC", &ueb::Parameters::liquid_retention}, {"DE", &ueb::Parameters::active_soil_depth},
    {"RHO_S", &ueb::Parameters::snow_density}, {"RHO_G", &ueb::Parameters::soil_density},
    {"CG", &ueb::Parameters::soil_heat_capacity}, {"LAMBDA_S", &ueb::Parameters::snow_conductivity},
    {"LAMBDA_G", &ueb::Parameters::soil_conductivity}, {"OMEGA_LF", &ueb::Parameters::low_frequency},
    {"R", &ueb::Parameters::amplitude}, {"FSTAB", &ueb::Parameters::stability},
    {"TR", &ueb::Parameters::rain_temperature}, {"TB", &ueb::Parameters::snow_temperature},
    {"ABG", &ueb::Parameters::ground_albedo}, {"ALPHA_VO", &ueb::Parameters::visible_albedo},
    {"ALPHA_IRO", &ueb::Parameters::infrared_albedo}, {"EPSILON", &ueb::Parameters::emissivity},
    {"DNEWS", &ueb::Parameters::new_snow_depth}, {"R3", &ueb::Parameters::age_dust}
    , {"CV", &ueb::Parameters::visible_age}, {"CIR", &ueb::Parameters::infrared_age}
};
std::string upper(const std::string& s) { return input::Tokenizer::to_upper(s); }
bool number(const std::string& s, double& v) {
    return !s.empty() && input::parse_double_strict(s, v) && std::isfinite(v);
}
std::string numericRow(const char* key, double value) {
    std::ostringstream out; out << key << ' ' << std::setprecision(17) << value;
    return out.str();
}
bool model(const std::string& s, SnowKind& out) {
    const auto k = upper(s);
    if (k == "UEB") out = SnowKind::Ueb;
    else if (k == "DEGREE_DAY") out = SnowKind::DegreeDay;
    else return false;
    return true;
}
}
bool SnowConfig::hasTemperatureRows() const {
    return model_authored || step != 3600 || site || shortwave.authored || longwave.authored || !met_file.empty();
}
std::string setUebOption(SnowConfig& cfg, const std::vector<std::string>& tok, bool validate) {
    if (tok.size() < 2) return "UEB option requires a key and value.";
    const auto key = upper(tok[0]);
    auto next = cfg;
    if (key == "BC_MONTHLY") {
        double month = 0, value = 0;
        if (tok.size() != 3 || !number(tok[1], month) || month < 1 || month > 12 || month != std::floor(month) ||
            !number(tok[2], value) || value <= 0 || value > 100) return "BC_MONTHLY requires month 1–12 and a positive temperature range (°C).";
        next.monthly_range[static_cast<int>(month) - 1] = value;
    } else {
        if (tok.size() != 2) return "Unexpected UEB option fields.";
        const auto value = upper(tok[1]);
        if (key == "SURFACE_TEMP") {
            if (value == "EQUILIBRIUM") next.parameters.temperature = ueb::TemperatureMethod::Equilibrium;
            else if (value == "MODIFIED_FORCE_RESTORE") next.parameters.temperature = ueb::TemperatureMethod::ModifiedForceRestore;
            else return "Unknown UEB surface temperature scheme.";
        } else if (key == "PRECIP_SPLIT") {
            if (value != "DIVT" && value != "UEB") return "PRECIP_SPLIT must be UEB or DIVT.";
            next.division_temperature = value == "DIVT";
        } else {
            double v = 0; if (!number(tok[1], v)) return "UEB option must be finite and numeric.";
            bool found = false;
            for (const auto& p : parameters) if (key == p.key) { next.parameters.*(p.member) = v; found = true; break; }
            if (key == "BC_A") { next.bc_a = v; found = true; }
            if (key == "BC_C") { next.bc_c = v; found = true; }
            if (!found) return "Unknown UEB option: " + tok[0];
        }
    }
    if (validate) if (auto error = next.parameters.validate(); !error.empty()) return error;
    if (next.bc_a <= 0 || next.bc_a > 1 || next.bc_c <= 0) return "Invalid Bristow–Campbell parameters.";
    cfg = std::move(next); return {};
}
std::vector<std::string> uebOptionRows(const SnowConfig& cfg) {
    const SnowConfig defaults;
    std::vector<std::string> rows;
    if (cfg.parameters.temperature != defaults.parameters.temperature) rows.emplace_back("SURFACE_TEMP EQUILIBRIUM");
    if (cfg.division_temperature) rows.emplace_back("PRECIP_SPLIT DIVT");
    for (const auto& p : parameters)
        if (cfg.parameters.*(p.member) != defaults.parameters.*(p.member))
            rows.push_back(numericRow(p.key, cfg.parameters.*(p.member)));
    if (cfg.bc_a != defaults.bc_a) rows.push_back(numericRow("BC_A", cfg.bc_a));
    if (cfg.bc_c != defaults.bc_c) rows.push_back(numericRow("BC_C", cfg.bc_c));
    for (int i = 0; i < 12; ++i) if (cfg.monthly_range[i])
        rows.push_back("BC_MONTHLY " + std::to_string(i + 1) + " " + numericRow("", *cfg.monthly_range[i]).substr(1));
    return rows;
}
std::optional<std::string> uebOptionValue(const SnowConfig& cfg, const std::string& name) {
    const auto key = upper(name);
    if (key == "SURFACE_TEMP") return cfg.parameters.temperature == ueb::TemperatureMethod::Equilibrium ? "EQUILIBRIUM" : "MODIFIED_FORCE_RESTORE";
    if (key == "PRECIP_SPLIT") return cfg.division_temperature ? "DIVT" : "UEB";
    for (const auto& p : parameters) if (key == p.key) return numericRow("", cfg.parameters.*(p.member)).substr(1);
    if (key == "BC_A") return numericRow("", cfg.bc_a).substr(1);
    if (key == "BC_C") return numericRow("", cfg.bc_c).substr(1);
    return std::nullopt;
}
std::string setSnowTemperature(SnowConfig& cfg, const std::vector<std::string>& tok) {
    if (tok.empty()) return "Missing snow/climate key.";
    const auto key = upper(tok[0]); auto next = cfg;
    if (key == "SITE") {
        ClimateSite site;
        if (tok.size() != 5 || !number(tok[1], site.latitude) || !number(tok[2], site.longitude) ||
            !number(tok[3], site.timezone) || !number(tok[4], site.elevation) ||
            std::abs(site.latitude) > 90 || std::abs(site.longitude) > 180 || std::abs(site.timezone) > 14 ||
            site.elevation < -500 || site.elevation > 10000) return "SITE requires latitude, longitude east, UTC offset and elevation in metres.";
        next.site = site;
    } else if (key == "SNOWMELT_MODEL") {
        if (tok.size() != 2 || !model(tok[1], next.model)) return "SNOWMELT_MODEL must be DEGREE_DAY or UEB.";
        next.model_authored = true;
    } else if (key == "SNOW_STEP") {
        if (tok.size() != 2) return "SNOW_STEP requires seconds or hh:mm[:ss].";
        // Strict clock parsing; the legacy permissive parser remains unchanged.
        auto s = tok[1]; double seconds = 0;
        if (s.find(':') == std::string::npos) { if (!number(s, seconds)) return "Invalid SNOW_STEP."; }
        else {
            std::istringstream stream(s); std::string field; std::vector<double> fields;
            while (std::getline(stream, field, ':')) { double v; if (!number(field, v)) return "Invalid SNOW_STEP."; fields.push_back(v); }
            if (s.back() == ':' || fields.size() < 2 || fields.size() > 3 || fields[0] < 0 || fields[0] != std::floor(fields[0]) ||
                fields[1] < 0 || fields[1] >= 60 || fields[1] != std::floor(fields[1]) ||
                (fields.size() == 3 && (fields[2] < 0 || fields[2] >= 60))) return "Invalid SNOW_STEP.";
            seconds = fields[0]*3600 + fields[1]*60 + (fields.size() == 3 ? fields[2] : 0);
        }
        if (seconds < 1 || seconds > 21600) return "SNOW_STEP must be between one second and six hours.";
        next.step = seconds; next.parameters.max_step = seconds;
    } else if (key == "MET_FILE") {
        if (tok.size() != 2 || tok[1].empty()) return "MET_FILE requires a path.";
        next.met_file = tok[1];
    } else if (key == "SHORTWAVE" || key == "LONGWAVE") {
        if (tok.size() < 2) return "Radiation source is missing.";
        auto& source = key == "SHORTWAVE" ? next.shortwave : next.longwave;
        const auto mode = upper(tok[1]);
        if (mode == "TIMESERIES" && tok.size() == 3 && !tok[2].empty()) {
            source.mode = RadiationSource::Mode::Timeseries; source.series = tok[2];
        } else if ((mode == "COMPUTED" || mode == "FILE") && tok.size() == 2) {
            source.mode = mode == "FILE" ? RadiationSource::Mode::File : RadiationSource::Mode::Computed;
            source.series.clear();
        } else return "Radiation source must be COMPUTED, FILE or TIMESERIES name.";
        source.authored = true;
    } else return "Unknown snow/climate option: " + tok[0];
    cfg = std::move(next); return {};
}
std::string setSnowpackUeb(SnowConfig& cfg, int pack, const std::vector<std::string>& tok) {
    // Tokens start with Fc F Qg aep [U0] [MODEL kind].
    if (pack < 0 || tok.size() < 4) return "UEB snowpack requires Fc F Qg aep [U0] [MODEL kind].";
    SnowpackConfig next;
    if (!number(tok[0], next.site.forest) || !number(tok[1], next.site.drift) || !number(tok[2], next.site.ground_heat) ||
        !number(tok[3], next.site.extinction_depth)) return "Non-numeric UEB snowpack site value.";
    std::size_t at = 4;
    if (at < tok.size() && upper(tok[at]) != "MODEL") {
        if (!number(tok[at++], next.initial_energy)) return "Invalid UEB initial energy.";
    }
    if (at < tok.size()) {
        if (at + 2 != tok.size() || upper(tok[at]) != "MODEL" || !model(tok[at + 1], next.model)) return "Invalid UEB snowpack model override.";
    }
    if (next.site.forest < 0 || next.site.forest > 1 || next.site.drift < 0 || next.site.extinction_depth <= 0)
        return "Invalid UEB snowpack site range.";
    cfg.packs[pack] = next; return {};
}
} // namespace openswmm::surface
