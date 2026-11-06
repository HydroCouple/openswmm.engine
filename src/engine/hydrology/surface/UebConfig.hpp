// SPDX-License-Identifier: Apache-2.0

/**
 * @file      UebConfig.hpp
 * @brief     Utah Energy Balance snow model configuration and validation.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "UebSnow.hpp"
#include "../../core/FilePathPair.hpp"
#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace openswmm::surface {
enum class SnowKind { DegreeDay, Ueb };
struct SnowpackConfig {
    SnowKind model = SnowKind::Ueb;
    ueb::Site site;
    double initial_energy = 0;
};
struct RadiationSource {
    enum class Mode { Computed, Timeseries, File };
    Mode mode = Mode::Computed;
    std::string series;
    bool authored = false;
};
struct ClimateSite {
    double latitude = 0, longitude = 0, timezone = 0, elevation = 0;
};
struct SnowConfig {
    SnowKind model = SnowKind::DegreeDay;
    bool model_authored = false;
    bool division_temperature = false;
    double step = 3600;
    ueb::Parameters parameters;
    double bc_a = .8, bc_c = 2.4;
    // NaN/unspecified is represented explicitly: never invent a temperature range.
    std::array<std::optional<double>, 12> monthly_range;
    std::optional<ClimateSite> site;
    RadiationSource shortwave, longwave;
    FilePathPair met_file;
    std::map<int, SnowpackConfig> packs;

    /** @brief Resolve a per-pack model override or the global snow-model fallback.
     * @param pack Integer pack key; no pack-existence/range validation is performed.
     * @return Stored override model when present, otherwise global model. Does not
     * initialize runtime columns, test ignore-snowmelt or require an authored override. */
    SnowKind modelFor(int pack) const {
        const auto it = packs.find(pack);
        return it == packs.end() ? model : it->second.model;
    }
    /** @brief Test whether nondefault/authored temperature-section snow rows should exist.
     * @return True for model_authored, nondefault snow step, SITE, authored short/longwave
     * or MET_FILE. Does not inspect UEB parameter changes, monthly ranges or per-pack
     * options, so false does not mean the entire SnowConfig equals defaults. */
    bool hasTemperatureRows() const;
};

// Shared by the input reader and public option setters. A failed update leaves
// the configuration unchanged. Tokens exclude the section name.
/** @brief Parse one UEB_OPTIONS row into a candidate configuration then publish it.
 * @param[in,out] config Current coherent configuration; normal rejection retains it.
 * Caller serializes updates with runtime initialization/forcing and applies any
 * required runtime invalidation separately.
 * @param tokens Section-excluded token list: key/value exactly, except BC_MONTHLY
 * requires integer month1..12 and a finite temperature range in (0,100] C. Keywords
 * are case insensitive; whitespace/quotes have already been tokenized by caller.
 * SURFACE_TEMP accepts EQUILIBRIUM/MODIFIED_FORCE_RESTORE; PRECIP_SPLIT accepts
 * UEB/DIVT. Numeric parameter keys are Z0,ZM,KS,KSAT,LC,DE,RHO_S,RHO_G,CG,LAMBDA_S,
 * LAMBDA_G,OMEGA_LF,R,FSTAB,TR,TB,ABG,ALPHA_VO,ALPHA_IRO,EPSILON,DNEWS,R3,CV,CIR,
 * plus BC_A/BC_C. Values use the native Parameters units without conversion.
 * @param validate True validates the full candidate Parameters; false permits
 * transient physical parameter combinations during multi-row input assembly, which
 * must be qualified before use. Bristow-Campbell comparisons always enforce
 * BC_A in (0,1] and BC_C>0 for coherent finite input configuration.
 * @return Empty after replacement, otherwise syntax/key/range/parameter diagnostic.
 * No file/source binding, runtime state update or partial normal mutation. Copy/map/
 * string allocations can throw; this is not a runtime solver reconfiguration seam. */
std::string setUebOption(SnowConfig& config, const std::vector<std::string>& tokens,
                         bool validate = true);
/** @brief Serialize nondefault UEB_OPTIONS settings in deterministic schema order.
 * @param config Qualified source configuration; no value/enum validation here.
 * @return Independent section-body rows: changed temperature method, DIVT flag,
 * changed numeric Parameters in table order, changed BC_A/BC_C, then authored monthly
 * ranges in month order. Numeric text uses stream precision17/current locale. Exact
 * default comparisons control omission. Does not serialize temperature-section SITE/
 * model/step/radiation/MET_FILE or per-pack rows, nor preserve authored spelling/order. */
std::vector<std::string> uebOptionRows(const SnowConfig& config);
/** @brief Read a scalar UEB option's effective value, including its default.
 * @param config Qualified configuration; borrowed without mutation.
 * @param key Case-insensitive exact option keyword; no trimming/section parsing.
 * @return Independent canonical method/split token or numeric text with precision17;
 * nullopt for unknown keys, including BC_MONTHLY (requires a separate month selector).
 * Does not validate, report authoring provenance or query temperature/per-pack rows. */
std::optional<std::string> uebOptionValue(const SnowConfig& config, const std::string& key);
/** @brief Parse one snow-related TEMPERATURE row and publish a copied candidate.
 * @param[in,out] config Current configuration, retained on normal refusal. Runtime
 * weather/solver caches are not rebuilt by this setter.
 * @param tokens Section-excluded exact row: SITE(lat[-90,90],lon east[-180,180],UTC
 * hours[-14,14],elevation metres[-500,10000]); SNOWMELT_MODEL(DEGREE_DAY/UEB);
 * SNOW_STEP(seconds or hh:mm[:ss], total[1,21600]); MET_FILE(nonempty token path);
 * or SHORTWAVE/LONGWAVE(COMPUTED,FILE,TIMESERIES name). Keywords case insensitive.
 * Numeric SITE/step values must be finite; clock hours/minutes are nonnegative
 * integers, minutes/seconds below60, fractional seconds allowed. Plain seconds can
 * be fractional. SNOW_STEP updates both step and Parameters.max_step. Path/series
 * names are retained as tokens without file existence/name/coverage checks.
 * @return Empty on success, otherwise diagnostic. Records model_authored/radiation
 * authored flags and clears stale radiation series when selecting COMPUTED/FILE.
 * Does not run full Parameters validation or apply SITE to climate state here.
 * Copy/string/allocation exceptions propagate before candidate publication. */
std::string setSnowTemperature(SnowConfig& config, const std::vector<std::string>& tokens);
/** @brief Replace one pack's optional UEB site/model override from authored tokens.
 * @param[in,out] config Per-pack map updated only after normal parsing/range success.
 * @param pack Nonnegative integer key; existence in a model snowpack registry is
 * not checked here. Replaces any earlier override rather than merging fields.
 * @param tokens Fc forest fraction[0,1], F drift>=0, Qg ground heat W/m2 (signed),
 * aep positive extinction metres, optional finite U0 point energy kJ/m2, optional
 * MODEL DEGREE_DAY/UEB. Exactly these trailing fields; keywords case insensitive.
 * Default new override model is UEB even if global model is degree-day; omitted
 * site fields are not inherited from an earlier override. No native unit conversion.
 * @return Empty after map assignment, otherwise syntax/value/range diagnostic.
 * No runtime column initialization, site pressure update or source state mutation;
 * allocation exceptions propagate. Caller binds valid model pack identity later. */
std::string setSnowpackUeb(SnowConfig& config, int pack, const std::vector<std::string>& tokens);
} // namespace openswmm::surface
