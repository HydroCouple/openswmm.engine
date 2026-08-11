/**
 * @file PostParseResolver.cpp
 * @brief Post-parse cross-reference resolution.
 * @see PostParseResolver.hpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "PostParseResolver.hpp"
#include "../core/Constants.hpp"
#include "../core/ErrorCodes.hpp"
#include "../core/PathResolver.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/DateTime.hpp"
#include "../core/UnitConversion.hpp"
#include "../hydraulics/xsect_tables.hpp"
#include "../hydraulics/XSectBatch.hpp"
#include "../hydraulics/Link.hpp"
#include "../hydraulics/Street.hpp"
#include "../hydraulics/ForceMain.hpp"
#include "../edit/VirtualJunctionOps.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace openswmm::input {

using openswmm::format_error;
using openswmm::format_warning;
using openswmm::ERR_TRANSECT_MANNING;
using openswmm::WarnCode;
using openswmm::WARN_NEGATIVE_OFFSET;
using openswmm::WARN_MIN_ELEV_DROP;
using openswmm::WARN_REGULATOR_CREST_LOW;
using openswmm::WARN_MAX_DEPTH_INCREASED;

// -------------------------------------------------------------------------
// Load external FILE timeseries from disk
// -------------------------------------------------------------------------
// Parses standard SWMM timeseries .dat files with format:
//   ;;comments
//   MM/DD/YYYY  H:MM  value
//   MM/DD/YYYY  H:MM  value
//   ...
// Fields may be tab or space delimited.
// -------------------------------------------------------------------------
// -------------------------------------------------------------------------
// Slice IO-3: resolve every external-file slot's `original` token against
// the .inp directory, populating `.absolute` for downstream callers
// (engine `fopen` paths, the GUI's relative-path display, future
// `IoPortabilityNormalizer` pre-save pass).
//
// `original` is left untouched — InpWriter consumes it verbatim today;
// Slice IO-4 will rebase it against the destination directory at write
// time.
//
// Convention: for tokens that carry a non-path decorator (the
// `path:column` form used by file-backed timeseries), the decorator is
// preserved in `.absolute` too — `.absolute` is "the same token, made
// absolute", not "the fopen-ready file path". Consumers that need to
// open the file (e.g. `load_external_timeseries_files` below) strip the
// decorator the same way they did before this slice.
// -------------------------------------------------------------------------
void resolve_external_file_slots(SimulationContext& ctx,
                                  const std::string& anchor_dir) {
    auto resolve = [&](FilePathPair& slot) {
        if (slot.original.empty()) {
            slot.absolute.clear();
            return;
        }
        slot.absolute = openswmm::io::resolveRelative(slot.original, anchor_dir);
    };

    auto& f = ctx.files;
    resolve(f.rainfall_path);
    resolve(f.runoff_path);
    resolve(f.rdii_path);
    resolve(f.inflows_path);
    resolve(f.outflows_path);
    resolve(f.hotstart_use_path);
    for (auto& save : f.hotstart_saves) resolve(save.path);

    for (auto& gfp : ctx.gages.file_path) resolve(gfp);

    resolve(ctx.options.temp_file);

    for (auto& tbl : ctx.tables.tables) {
        if (tbl.type != TableType::TIMESERIES) continue;
        resolve(tbl.file_path);
    }
}

static void load_external_timeseries_files(SimulationContext& ctx, const std::string& inp_dir) {
    for (std::size_t t = 0; t < ctx.tables.tables.size(); ++t) {
        auto& tbl = ctx.tables.tables[t];
        if (tbl.type != TableType::TIMESERIES) continue;

        // TablesHandler stores the FILE token (path, optionally with a
        // `:column` suffix) verbatim in Table::file_path.original. An
        // empty value means the series is inline, not file-backed.
        if (tbl.file_path.empty()) continue;

        // Slice IO-3: resolve_external_file_slots() has already filled
        // .absolute from .original against the .inp directory. Prefer
        // the cached resolution; fall back to inline resolution for
        // callers that built the model programmatically and never ran
        // the resolver pass.
        std::string file_path = !tbl.file_path.absolute.empty()
                                  ? tbl.file_path.absolute
                                  : tbl.file_path.str();

        // Strip optional :column suffix (e.g. "path.dat:ColName")
        auto colon_pos = file_path.rfind(':');
        // Only strip if it's not a drive letter (e.g. "C:\path")
        if (colon_pos != std::string::npos && colon_pos > 1) {
            file_path = file_path.substr(0, colon_pos);
        }

        // Resolve relative paths against INP file directory (legacy
        // fallback path — kept so a fresh programmatic Table still loads
        // even without the resolver pass).
        if (!file_path.empty() && file_path[0] != '/' && file_path[0] != '\\') {
            if (!inp_dir.empty())
                file_path = inp_dir + "/" + file_path;
        }

        // Open the file
        FILE* fp = std::fopen(file_path.c_str(), "r");
        if (!fp) {
            // Try the verbatim token as a final fallback (covers absolute
            // paths and same-cwd cases when inp_dir was empty).
            fp = std::fopen(tbl.file_path.c_str(), "r");
            if (!fp) continue; // Skip silently — legacy also reports ERROR 361
        }

        // Reserve estimated capacity (large files can be millions of lines)
        tbl.x.reserve(100000);
        tbl.y.reserve(100000);

        char line[256];
        while (std::fgets(line, sizeof(line), fp)) {
            // Skip comments and empty lines
            if (line[0] == ';' || line[0] == '\n' || line[0] == '\r') continue;

            // Parse: date  time  value
            // Format: MM/DD/YYYY  H:MM  value  (tab or space delimited)
            int month = 0, day = 0, year = 0;
            int hour = 0, minute = 0;
            double value = 0.0;

            // Try tab-delimited first, then space-delimited
            char date_str[32] = {}, time_str[32] = {};
            int fields = std::sscanf(line, "%31s %31s %lf", date_str, time_str, &value);
            if (fields < 3) continue;

            // Parse date: MM/DD/YYYY
            if (std::sscanf(date_str, "%d/%d/%d", &month, &day, &year) != 3) continue;

            // Parse time: H:MM or HH:MM or H:MM:SS
            int second = 0;
            if (std::sscanf(time_str, "%d:%d:%d", &hour, &minute, &second) < 2) continue;

            double dt = datetime::encodeDate(year, month, day)
                      + datetime::encodeTime(hour, minute, second);

            tbl.x.push_back(dt);
            tbl.y.push_back(value);
        }
        std::fclose(fp);

        // Shrink to fit
        tbl.x.shrink_to_fit();
        tbl.y.shrink_to_fit();

        // file_path is intentionally retained so InpWriter can preserve the
        // FILE-form reference on round-trip; the in-memory x/y rows are an
        // execution cache, not the canonical representation.
    }
}

// -------------------------------------------------------------------------
// Load external FILE-source rain-gage data from disk
// -------------------------------------------------------------------------
// Standard SWMM rain files (STAN_PRCP) hold one record per line:
//   StationID  Year  Month  Day  Hour  Minute  Value
// A single file may contain many stations; each gage selects its own rows by
// station ID.  The whole file is scanned to gather the "Rainfall File Summary"
// statistics (first/last date, periods-with-precip), but only records inside
// the simulation window are retained for routing — kept in the gage's own
// `rain_series` Table so the runtime reuses the same step-function lookup as
// an inline [TIMESERIES] gage without polluting ctx.tables.
// -------------------------------------------------------------------------
static void load_external_rain_files(SimulationContext& ctx) {
    const int n_gages = ctx.gages.count();
    if (n_gages == 0) return;

    // Retain a generous window around the run so the gage has data at and just
    // before the start, and through the end, of the simulation.
    const double start = ctx.options.start_date;
    const double end   = (ctx.options.end_date > start)
                           ? ctx.options.end_date : (start + 366.0);
    const double win_lo = start - 1.0;
    const double win_hi = end + 1.0;

    for (int g = 0; g < n_gages; ++g) {
        const auto ug = static_cast<std::size_t>(g);
        if (ctx.gages.source[ug] != RainSource::FILE_RAIN) continue;
        if (ctx.gages.file_format[ug] != RainFileFormat::STAN_PRCP) continue;

        std::string path = !ctx.gages.file_path[ug].absolute.empty()
                             ? ctx.gages.file_path[ug].absolute
                             : ctx.gages.file_path[ug].str();
        FILE* fp = std::fopen(path.c_str(), "r");
        if (!fp) {
            fp = std::fopen(ctx.gages.file_path[ug].c_str(), "r");
            if (!fp) {
                // A gage that declares FILE but whose file cannot be opened is
                // FATAL, exactly as in legacy (ERROR 317). Skipping it silently
                // leaves the series empty, which reads as 0.0 at every lookup —
                // indistinguishable from "it wasn't raining". The run then
                // completes, continuity closes, and the .rpt even prints a
                // Rainfall File Summary, so a model whose rain file was moved,
                // renamed, or left behind by a save-to-another-folder produces a
                // clean-looking result with no precipitation anywhere. Collect
                // and continue so every unopenable gage is named, then let the
                // open-time gate in SWMMEngine fail the load.
                ctx.errors.push_back(
                    format_error(openswmm::ERR_RAIN_FILE_OPEN, path));
                continue;
            }
        }

        // PARITY: legacy pipes external rain files through a binary "rain
        // interface file" that stores depths in INCHES as 4-byte FLOATS:
        //   - parseStdLine (rain.c) reads the value with sscanf "%f" (float);
        //   - readStdLine applies the rain-type transform in float
        //     (INTENSITY: x = x*Interval/3600.0f; CUMULATIVE: incremental
        //     delta via a float accumulator; VOLUME: none);
        //   - then x *= (float)UnitsFactor with UnitsFactor = 1.0/MMperINCH
        //     iff the file units are MM (rain.c:570), and the float is
        //     written to the interface file.
        // The gage read-back (gage.c convertRainfall) later multiplies by
        // Gage.unitsFactor = MMperINCH for SI projects (gage.c:300) — see
        // Gage.cpp. Keep the series in float-quantized INCHES here so the
        // composite conversion is bit-identical to legacy.
        const bool file_mm = ctx.gages.rain_units[ug] == 1;
        const float units_factor_f =
            file_mm ? static_cast<float>(1.0 / 25.40) : 1.0f;
        const int   rain_type_w  = ctx.gages.rain_type[ug];
        const float interval_f   = static_cast<float>(ctx.gages.interval_sec[ug]);
        float rain_accum_f = 0.0f;  // legacy rain.c file-scope RainAccum

        const std::string& sta = ctx.gages.station_id[ug];

        Table series;
        series.type = TableType::TIMESERIES;
        series.id   = ctx.gage_names.name_of(g);
        series.x.reserve(8192);
        series.y.reserve(8192);

        double first_date = 0.0, last_date = 0.0;
        long   periods_precip = 0;

        char line[256];
        char tok[64];
        int  yr, mo, dy, hr, mn;
        float val;
        while (std::fgets(line, sizeof(line), fp)) {
            if (line[0] == ';' || line[0] == '\n' || line[0] == '\r') continue;
            if (std::sscanf(line, "%63s %d %d %d %d %d %f",
                            tok, &yr, &mo, &dy, &hr, &mn, &val) != 7) continue;
            if (!sta.empty() && sta != tok) continue;

            const double dt = datetime::encodeDate(yr, mo, dy)
                            + datetime::encodeTime(hr, mn, 0);

            // Whole-file statistics for the report summary.
            if (first_date == 0.0 || dt < first_date) first_date = dt;
            if (dt > last_date) last_date = dt;
            if (val > 0.0f) ++periods_precip;

            // Retain only the records needed to route the simulation window.
            if (dt < win_lo || dt > win_hi) continue;

            // PARITY: legacy readStdLine (rain.c) write-side transform, all
            // in single precision.
            float x = val;
            if (rain_type_w == 0) {          // RAINFALL_INTENSITY → depth
                x = x * interval_f / 3600.0f;
            } else if (rain_type_w == 2) {   // CUMULATIVE_RAINFALL → delta
                if (x >= rain_accum_f) {
                    x = x - rain_accum_f;
                    rain_accum_f += x;
                } else rain_accum_f = x;
            }
            x *= units_factor_f;             // file units → inches (float)

            series.x.push_back(dt);
            series.y.push_back(static_cast<double>(x));
        }
        std::fclose(fp);

        // Records may be out of order across stations in a shared file; the
        // step-function lookup assumes ascending time.
        if (!std::is_sorted(series.x.begin(), series.x.end())) {
            std::vector<std::size_t> order(series.x.size());
            for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::sort(order.begin(), order.end(),
                      [&](std::size_t a, std::size_t b){ return series.x[a] < series.x[b]; });
            std::vector<double> sx(series.x.size()), sy(series.y.size());
            for (std::size_t i = 0; i < order.size(); ++i) {
                sx[i] = series.x[order[i]];
                sy[i] = series.y[order[i]];
            }
            series.x.swap(sx);
            series.y.swap(sy);
        }
        series.x.shrink_to_fit();
        series.y.shrink_to_fit();

        ctx.gages.file_first_date[ug]     = first_date;
        ctx.gages.file_last_date[ug]      = last_date;
        ctx.gages.file_periods_precip[ug] = periods_precip;
        ctx.gages.rain_series[ug]         = std::move(series);
    }
}

void recompute_conduit_flow_properties(SimulationContext& ctx, int j) {
    using constants::GRAVITY;
    using constants::PHI;

    auto uj = static_cast<std::size_t>(j);
    if (ctx.links.type[uj] != LinkType::CONDUIT) return;
    auto& CD = ctx.link_subtypes.conduits;
    const auto ucr = static_cast<std::size_t>(ctx.link_subtypes.conduit_row(j));

    // Signal XSectParams-cache holders (e.g. SWMMEngine's reporting cache)
    // that this link's cross-section-derived fields are changing.
    ++ctx.xsect_generation;

    double n_val    = CD.roughness[ucr];
    double slope    = std::fabs(CD.slope[ucr]);
    double a_full   = ctx.links.xsect_a_full[uj];
    double s_full   = ctx.links.xsect_s_full[uj];
    double s_max    = ctx.links.xsect_s_max[uj];

    // IRREGULAR (transect) conduits take their Manning's n from the transect's
    // MAIN-CHANNEL roughness, NOT the [CONDUITS] value — legacy link.c:1024
    //   Conduit[k].roughness = Transect[xsect.transect].roughness;
    // (Transect.roughness is the un-Lfactor-adjusted main-channel n.) Using the
    // [CONDUITS] n here made the dynamic-wave friction/conveyance wrong by the
    // ratio n_conduit/n_transect — e.g. user5's LIB transects (conduit n=0.014
    // vs transect n=0.04) conveyed ~2.9× too much flow.
    if (ctx.links.xsect_shape[uj] == XsectShape::IRREGULAR) {
        int ti = ctx.links.xsect_curve[uj];
        if (ti >= 0 && static_cast<std::size_t>(ti) < ctx.transects.n_channel.size()) {
            double nch = ctx.transects.n_channel[static_cast<std::size_t>(ti)];
            if (nch > 0.0) n_val = nch;
        }
    }

    if (n_val <= 0.0 || a_full <= 0.0) return;

    // For DW force mains, substitute equivalent Manning's n (Gap #22)
    // Matches legacy conduit_validate in link.c lines 1094-1096:
    //   if (RouteModel == DW && xsect.type == FORCE_MAIN)
    //       roughness = forcemain_getEquivN(j, k);
    bool is_force_main = (ctx.links.xsect_shape[uj] == XsectShape::FORCE_MAIN);
    // ==DYNWAVE audit (plan §4.1): FV solves the same momentum equation, so
    // the force-main equivalent-n substitution applies to it identically.
    bool is_dw = (ctx.options.routing_model == RoutingModel::DYNWAVE ||
                  ctx.options.routing_model == RoutingModel::FV);
    if (is_dw && is_force_main) {
        auto fm = static_cast<forcemain::FrictionModel>(ctx.options.force_main_eqn);
        double r_bot  = ctx.links.xsect_r_bot[uj];
        double y_full = ctx.links.xsect_y_full[uj];
        n_val = forcemain::getEquivN(fm, r_bot, y_full, slope, n_val);
        if (n_val <= 0.0) n_val = CD.roughness[ucr];
    }

    // Roughness factor for DW friction slope: GRAVITY * (n/PHI)^2.
    // PARITY link.c:1133: GRAVITY * SQR(roughness/PHI) — the square is grouped
    // FIRST, then multiplied by GRAVITY. Left-to-right G*(x)*(x) rounds
    // differently by 1 ULP and seeds dq1 divergence in the momentum kernel.
    CD.rough_factor[ucr] = GRAVITY * ((n_val / PHI) * (n_val / PHI));

    // Conveyance factor: beta = PHI * sqrt(|slope|) / n
    double beta = PHI * std::sqrt(slope) / n_val;
    CD.beta[ucr] = beta;

    // Full-flow rate: q_full = sFull * beta
    CD.q_full[ucr] = s_full * beta;

    // Max flow at max section factor
    CD.q_max[ucr] = s_max * beta;

    // Conduit volume = A_full * modLength (or length if no lengthening)
    double mod_len = CD.mod_length[ucr];
    if (mod_len <= 0.0) mod_len = CD.length[ucr];
    CD.mod_length[ucr] = mod_len;
    ctx.links.volume[uj] = a_full * mod_len;

    // For DW force mains, store roughness factor in xsect_s_bot (Gap #22)
    // Matches legacy link.c lines 1127-1130:
    //   Link[j].xsect.sBot = forcemain_getRoughFactor(j, lengthFactor)
    // The lengthFactor = mod_length / length (1.0 if not lengthened).
    if (is_dw && is_force_main) {
        double length_factor = (CD.length[ucr] > 0.0)
            ? mod_len / CD.length[ucr] : 1.0;
        auto fm = static_cast<forcemain::FrictionModel>(ctx.options.force_main_eqn);
        ctx.links.xsect_s_bot[uj] =
            forcemain::getRoughFactor(fm, ctx.links.xsect_r_bot[uj], length_factor);
    }
}

// ---------------------------------------------------------------------------
// Centralised display → internal (feet/cfs/ft³) unit conversion.
// ---------------------------------------------------------------------------
// The engine computes internally in US/imperial units (GRAVITY = 32.2 ft/s²,
// PHI = 1.486), exactly like legacy EPA-SWMM, which converts every input at
// parse via `value / UCF(quantity)`.  The refactored parsers stored many
// hydraulic-geometry fields RAW (metres / m² / m³·s⁻¹ for SI models); this pass
// converts them once, here, using the precomputed reciprocal tables (multiply,
// never divide).  US models have inv_len == 1.0 → early return (no-op), which is
// why the existing US regression tests never exercised this path.
//
// Runs at the TOP of resolve_cross_references so every downstream derived value
// (node head init, cross-section a_full/s_full, conduit slope/volume/conveyance,
// outfall normal depth) sees feet.
//
// Fields already converted at init/at-use (subcatch area, depression storage,
// infiltration, rainfall, DWF, storage/pump curve lookups) are intentionally
// EXCLUDED — converting them here would double-convert and break the (correct)
// runoff hydrology.  See docs/UNIT_CONVERSION_AUDIT.md.
static void convert_inputs_to_internal(SimulationContext& ctx,
                                       int n_nodes, int n_links, int n_subcatch) {
    const int us = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
    const auto usz = static_cast<std::size_t>(us);

    // Rainfall-dimensioned inputs convert in BOTH unit systems, so they must be
    // handled before the US early-out below.
    //
    // That early-out is keyed on the LENGTH factor being 1, which is the right
    // test for lengths, areas and flows: US input arrives in feet and cfs, which
    // are already the internal units. It is the WRONG test for [LOSSES] seepage,
    // which arrives in in/hr in both systems while the solver consumes ft/s —
    // UCF(RAINFALL) is 43200 for US, not 1. Skipping it left the rate 43200×
    // too large, and the loss then saturated at the availability cap: measured
    // −67 % routing continuity on a model whose only seepage was one conduit at
    // 0.20 in/hr. Legacy converts it unconditionally (link.c conduit_validate,
    // `seepRate /= UCF(RAINFALL)`), which is what this restores.
    {
        const double rain0 = ucf::Ucf[ucf::RAINFALL][usz];
        if (rain0 != 1.0) {
            auto& CD = ctx.link_subtypes.conduits;
            for (int j = 0; j < n_links; ++j) {
                if (ctx.links.type[static_cast<std::size_t>(j)] != LinkType::CONDUIT)
                    continue;
                const int cr = ctx.link_subtypes.conduit_row(j);
                if (cr >= 0) CD.seep_rate[static_cast<std::size_t>(cr)] /= rain0;
            }
        }
    }

    const double inv_len = ucf::Ucf_inv[ucf::LENGTH][usz];
    if (inv_len == 1.0) return;  // US units: input already in internal units.

    // PARITY: legacy converts metric input to internal feet by DIVIDING by the
    // forward factor (internal = display / UCF, e.g. m / 0.3048), NOT multiplying
    // by the precomputed reciprocal (display * (1/0.3048)). The two differ by up
    // to 1 ULP (e.g. 223.48/0.3048 = 733.2020997375328 vs *recip 733.2020997375326),
    // which breaks the exact flatness of an initial water surface (h1==h2) and
    // seeds a spurious sign-flip/clamp flow in the dynamic-wave solve. Divide by
    // the forward length factor here (one-time at parse — no hot-loop cost) to
    // match legacy bit-for-bit. See UnitConversion.hpp ("internal = display / Ucf").
    // Same divide convention for area (legacy node.c:154 `/ (UCF_L*UCF_L)`), flow
    // (`/ Qcf`), and rainfall (`/ UCF(RAINFALL)`).
    const double len  = ucf::Ucf[ucf::LENGTH][usz];
    const double len2 = len * len;
    const double qcf  = ucf::Qcf[static_cast<std::size_t>(ctx.options.flow_units)];
    const double rain = ucf::Ucf[ucf::RAINFALL][usz];

    // --- Nodes: elevations/depths → ft, ponded area → ft², stage/cutoff ---
    for (int i = 0; i < n_nodes; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        ctx.nodes.invert_elev[ui] /= len;
        ctx.nodes.full_depth[ui]  /= len;
        ctx.nodes.init_depth[ui]  /= len;
        ctx.nodes.sur_depth[ui]   /= len;
        ctx.nodes.ponded_area[ui] /= len2;
        if (ctx.nodes.type[ui] == NodeType::OUTFALL) {
            const int r = ctx.node_subtypes.outfall_row(i);
            if (r >= 0 && ctx.node_subtypes.outfalls.bc_type[static_cast<std::size_t>(r)]
                              == OutfallType::FIXED)
                ctx.node_subtypes.outfalls.param[static_cast<std::size_t>(r)] /= len;
        }
        if (ctx.nodes.type[ui] == NodeType::DIVIDER) {
            const int r = ctx.node_subtypes.divider_row(i);
            if (r >= 0)
                ctx.node_subtypes.dividers.cutoff[static_cast<std::size_t>(r)] /= qcf;
        }
        // Functional storage A(d) = a0 + a1·d^a2 coefficients are NOT
        // pre-converted. PARITY: legacy keeps them in USER units and converts
        // per call inside storage_getVolume/getSurfArea/getDepth (node.c:895,
        // 944, 778) using the TRUNCATED SI VOLUME factor 0.02832 ≠ 0.3048³
        // (swmm5.c:157), which is FP-distinguishable from evaluating
        // pre-converted internal coefficients. Node.cpp mirrors that per-call
        // regime, so the parsed user-unit values must stay as-is. Tabulated
        // storage (curve >= 0) likewise self-converts at lookup.
    }

    // --- Links: cross-section geometry, length, offsets, flow limits ---
    for (int j = 0; j < n_links; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        const XsectShape shp = ctx.links.xsect_shape[uj];
        // geom1 (full depth/diameter) — always a length, every shape.
        ctx.links.xsect_y_full[uj] /= len;
        // geom2 (width) — a length for every shape EXCEPT FORCE_MAIN, whose
        // geom2 is the Hazen-Williams C-factor / Darcy-Weisbach roughness
        // height (saved to r_bot in the xsect loop). For shapes that derive
        // w_max from y_full the input is overwritten, so converting is harmless.
        if (shp != XsectShape::FORCE_MAIN)
            ctx.links.xsect_w_max[uj] /= len;
        // geom3 (y_bot/r_bot) — a length only for these three shapes; for
        // RECT_OPEN it is a sides flag and for TRAPEZOIDAL/TRIANGULAR/POWER a
        // side slope/exponent (dimensionless) — leave those.
        if (shp == XsectShape::RECT_TRIANG ||
            shp == XsectShape::RECT_ROUND ||
            shp == XsectShape::MODBASKETHANDLE)
            ctx.links.xsect_y_bot[uj] /= len;

        if (ctx.links.type[uj] == LinkType::CONDUIT) {
            const int cr = ctx.link_subtypes.conduit_row(j);
            const auto ucr = static_cast<std::size_t>(cr);
            if (cr >= 0 && ctx.link_subtypes.conduits.length[ucr] > 0.0)
                ctx.link_subtypes.conduits.length[ucr] /= len;
            ctx.links.q0[uj]        /= qcf;
            ctx.links.q_limit[uj]   /= qcf;
        }
        // Offsets are length-dimension; crest lives on the weir/outlet row.
        ctx.links.offset1[uj]      /= len;
        ctx.links.offset2[uj]      /= len;
        const int wr = ctx.link_subtypes.weir_row(j);
        if (wr >= 0) ctx.link_subtypes.weirs.crest_height[static_cast<std::size_t>(wr)] /= len;
        else {
            const int olr = ctx.link_subtypes.outlet_row(j);
            if (olr >= 0) ctx.link_subtypes.outlets.crest_height[static_cast<std::size_t>(olr)] /= len;
        }
    }

    // --- Subcatchments: width → ft.  Area/infiltration/depression storage are
    //     converted at init/at-use and intentionally excluded here. ---
    for (int s = 0; s < n_subcatch; ++s)
        ctx.subcatches.width[static_cast<std::size_t>(s)] /= len;
}

// ---------------------------------------------------------------------------
// Internal (feet/cfs/ft³) → display unit conversion — exact inverse of
// convert_inputs_to_internal above.
// ---------------------------------------------------------------------------
// The .inp writer must emit values in the project's display units, but the
// engine stores them internally in feet/cfs after parse. Without this, every
// save dumps internal feet and the next open re-applies the m→ft factor, so
// repeated save/open cycles multiply length-dimensioned fields by 3.28084 each
// time (the "exploding model" bug). Mirrors convert_inputs_to_internal
// FIELD-FOR-FIELD with the forward UCF in place of its reciprocal — keep the
// two in lock-step. US models have len == 1.0 → early return (no-op).
void convert_internal_to_display(SimulationContext& ctx) {
    const int us = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
    const auto usz = static_cast<std::size_t>(us);

    // Mirror of the hoist in convert_inputs_to_internal: seepage is in/hr in
    // both unit systems, so it converts even when lengths do not.
    {
        const double rain0 = ucf::Ucf[ucf::RAINFALL][usz];
        if (rain0 != 1.0) {
            auto& CD = ctx.link_subtypes.conduits;
            for (int j = 0; j < ctx.n_links(); ++j) {
                if (ctx.links.type[static_cast<std::size_t>(j)] != LinkType::CONDUIT)
                    continue;
                const int cr = ctx.link_subtypes.conduit_row(j);
                if (cr >= 0) CD.seep_rate[static_cast<std::size_t>(cr)] *= rain0;
            }
        }
    }

    const double len = ucf::Ucf[ucf::LENGTH][usz];
    if (len == 1.0) return;  // US units: internal already equals display.

    const double area = len * len;
    const double flow = ucf::Qcf[static_cast<std::size_t>(ctx.options.flow_units)];
    const double rain = ucf::Ucf[ucf::RAINFALL][usz];

    const int n_nodes    = ctx.n_nodes();
    const int n_links    = ctx.n_links();
    const int n_subcatch = ctx.subcatch_names.size();

    // --- Nodes ---
    for (int i = 0; i < n_nodes; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        ctx.nodes.invert_elev[ui] *= len;
        ctx.nodes.full_depth[ui]  *= len;
        ctx.nodes.init_depth[ui]  *= len;
        ctx.nodes.sur_depth[ui]   *= len;
        ctx.nodes.ponded_area[ui] *= area;
        if (ctx.nodes.type[ui] == NodeType::OUTFALL) {
            const int r = ctx.node_subtypes.outfall_row(i);
            if (r >= 0 && ctx.node_subtypes.outfalls.bc_type[static_cast<std::size_t>(r)]
                              == OutfallType::FIXED)
                ctx.node_subtypes.outfalls.param[static_cast<std::size_t>(r)] *= len;
        }
        if (ctx.nodes.type[ui] == NodeType::DIVIDER) {
            const int r = ctx.node_subtypes.divider_row(i);
            if (r >= 0)
                ctx.node_subtypes.dividers.cutoff[static_cast<std::size_t>(r)] *= flow;
        }
        // Functional storage A(d) = a0 + a1·d^a2 coefficients stay in USER
        // units end-to-end (see convert_inputs_to_internal — legacy converts
        // per call in node.c), so there is nothing to back-convert; the .inp
        // writer emits the stored values directly.
    }

    // --- Links ---
    for (int j = 0; j < n_links; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        const XsectShape shp = ctx.links.xsect_shape[uj];
        ctx.links.xsect_y_full[uj] *= len;
        if (shp != XsectShape::FORCE_MAIN)
            ctx.links.xsect_w_max[uj] *= len;
        if (shp == XsectShape::RECT_TRIANG ||
            shp == XsectShape::RECT_ROUND ||
            shp == XsectShape::MODBASKETHANDLE)
            ctx.links.xsect_y_bot[uj] *= len;

        if (ctx.links.type[uj] == LinkType::CONDUIT) {
            const int cr = ctx.link_subtypes.conduit_row(j);
            const auto ucr = static_cast<std::size_t>(cr);
            if (cr >= 0 && ctx.link_subtypes.conduits.length[ucr] > 0.0)
                ctx.link_subtypes.conduits.length[ucr] *= len;
            ctx.links.q0[uj]        *= flow;
            ctx.links.q_limit[uj]   *= flow;
        }
        ctx.links.offset1[uj]      *= len;
        ctx.links.offset2[uj]      *= len;
        const int wr = ctx.link_subtypes.weir_row(j);
        if (wr >= 0) ctx.link_subtypes.weirs.crest_height[static_cast<std::size_t>(wr)] *= len;
        else {
            const int olr = ctx.link_subtypes.outlet_row(j);
            if (olr >= 0) ctx.link_subtypes.outlets.crest_height[static_cast<std::size_t>(olr)] *= len;
        }
        // Pump startup/shutoff depths — mirror the display→internal conversion
        // applied at load (the "/= ucf_len" block in resolve_cross_references).
        // Without this the writer dumps internal feet, inflating them ×3.28084
        // per save/open cycle on SI projects.
        const int pr = ctx.link_subtypes.pump_row(j);
        if (pr >= 0) {
            const auto upr = static_cast<std::size_t>(pr);
            if (ctx.link_subtypes.pumps.startup[upr] > 0.0)
                ctx.link_subtypes.pumps.startup[upr] *= len;
            if (ctx.link_subtypes.pumps.shutoff[upr] > 0.0)
                ctx.link_subtypes.pumps.shutoff[upr] *= len;
        }
    }

    // --- Subcatchments ---
    for (int s = 0; s < n_subcatch; ++s)
        ctx.subcatches.width[static_cast<std::size_t>(s)] *= len;
}

// ============================================================================
// validate_virtual_junctions()
// ============================================================================
// Refactored engine only — see plans/VIRTUAL_JUNCTION_IMPLEMENTATION_PLAN.md §6.
// Runs after conduit slope computation and adverse-slope reversal so the
// attachment orientation observed here is final. The per-node rules live in
// edit::vj_rule_violation (shared with the C API set-virtual/split/fuse
// operations); each violated rule produces its own error code so callers can
// render actionable messages.
static void validate_virtual_junctions(SimulationContext& ctx) {
    const int n_nodes = ctx.nodes.count();

    bool any = false;
    for (int i = 0; i < n_nodes; ++i) {
        if (ctx.nodes.is_virtual[static_cast<std::size_t>(i)]) { any = true; break; }
    }
    if (!any) return;

    // Rule 8 (model-level): only meaningful under dynamic-wave routing.
    // ==DYNWAVE audit: virtual junctions are meaningful under DW and are
    // EXACT under FV (they become plain interior faces), so FV joins DW here.
    if (ctx.options.routing_model != RoutingModel::DYNWAVE &&
        ctx.options.routing_model != RoutingModel::FV) {
        for (int i = 0; i < n_nodes; ++i) {
            if (ctx.nodes.is_virtual[static_cast<std::size_t>(i)]) {
                ctx.errors.push_back(format_error(ERR_VJ_ROUTING_MODEL,
                                                  ctx.node_names.name_of(i)));
                break;
            }
        }
    }

    for (int i = 0; i < n_nodes; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        if (!ctx.nodes.is_virtual[ui]) continue;
        const int code = edit::vj_rule_violation(ctx, i);
        if (code != 0) {
            ctx.errors.push_back(format_error(code, ctx.node_names.name_of(i)));
            continue;
        }
        // Derived geometry: full depth = shared pipe crown, no surcharge
        // depth, no ponding. The JUNCTION-only full-depth raise below is
        // skipped for virtual nodes (exact by construction).
        edit::vj_apply_derived_geometry(ctx, i);
    }
}

void resolve_cross_references(SimulationContext& ctx) {
    // -------------------------------------------------------------------------
    // Final counts → allocate SoA arrays to exact size
    // -------------------------------------------------------------------------
    const int n_nodes    = ctx.node_names.size();
    const int n_links    = ctx.link_names.size();
    const int n_subcatch = ctx.subcatch_names.size();
    const int n_gages    = ctx.gage_names.size();
    const int n_polluts  = ctx.pollutant_names.size();

    // Ensure all SoA arrays match final counts (handles forward-declared objects)
    if (ctx.nodes.count() != n_nodes)         ctx.nodes.resize(n_nodes);
    if (ctx.links.count() != n_links)         ctx.links.resize(n_links);
    if (ctx.subcatches.count() != n_subcatch) ctx.subcatches.resize(n_subcatch);
    if (ctx.gages.count() != n_gages)         ctx.gages.resize(n_gages);

    // Allocate quality matrices now that all counts are final
    if (n_polluts > 0) {
        // Only (re)allocate the pollutant definition arrays if the count
        // actually changed — resize_pollutants() zero-fills, so calling it
        // unconditionally here would wipe the concentrations (Crain/Cgw/
        // Crdii/Cdwf/Cinit/Kdecay/units) already parsed by handle_pollutants.
        // Mirrors the guarded node/link/subcatch resizes above.
        if (ctx.pollutants.n_pollutants() != n_polluts)
            ctx.pollutants.resize_pollutants(n_polluts);
        ctx.nodes.resize_quality(n_polluts);
        ctx.links.resize_quality(n_polluts);
        // Guarded (iteration 4): resize_quality() zero-fills, and the
        // subcatchment conc array already carries the [LOADINGS] initial
        // buildup parsed by handle_loadings — an unconditional call here
        // wiped it. Same rationale as the pollutant-definition guard above.
        if (ctx.subcatches.conc_n_pollutants != n_polluts ||
            static_cast<int>(ctx.subcatches.conc.size()) !=
                n_subcatch * n_polluts)
            ctx.subcatches.resize_quality(n_polluts);
        ctx.nodes.resize_loads(n_polluts);
        ctx.links.resize_loads(n_polluts);
    }

    // -------------------------------------------------------------------------
    // Display → internal (feet) unit conversion — must run before any derived
    // geometry/head/conveyance computation below.
    // -------------------------------------------------------------------------
    // A GeoPackage stores hydraulic fields already in internal units (its
    // canonical form); converting again would be wrong (and the non-invertible
    // ×0.3048 would also break bit-exact round-trip). The .inp path leaves the
    // flag false and converts here as before.
    if (!ctx.gpkg_units_internal)
        convert_inputs_to_internal(ctx, n_nodes, n_links, n_subcatch);

    // Spatial coordinate arrays
    const auto un = static_cast<std::size_t>(n_nodes);
    if (ctx.spatial.node_x.size() < un) ctx.spatial.node_x.resize(un, 0.0);
    if (ctx.spatial.node_y.size() < un) ctx.spatial.node_y.resize(un, 0.0);

    const auto ul = static_cast<std::size_t>(n_links);
    if (ctx.spatial.link_x.size() < ul) ctx.spatial.link_x.resize(ul, 0.0);
    if (ctx.spatial.link_y.size() < ul) ctx.spatial.link_y.resize(ul, 0.0);

    const auto us = static_cast<std::size_t>(n_subcatch);
    if (ctx.spatial.subcatch_x.size() < us) ctx.spatial.subcatch_x.resize(us, 0.0);
    if (ctx.spatial.subcatch_y.size() < us) ctx.spatial.subcatch_y.resize(us, 0.0);

    const auto ug = static_cast<std::size_t>(n_gages);
    if (ctx.spatial.gage_x.size() < ug) ctx.spatial.gage_x.resize(ug, 0.0);
    if (ctx.spatial.gage_y.size() < ug) ctx.spatial.gage_y.resize(ug, 0.0);

    if (ctx.spatial.link_vertices_x.size() < ul) ctx.spatial.link_vertices_x.resize(ul);
    if (ctx.spatial.link_vertices_y.size() < ul) ctx.spatial.link_vertices_y.resize(ul);
    if (ctx.spatial.subcatch_polygon_x.size() < us) ctx.spatial.subcatch_polygon_x.resize(us);
    if (ctx.spatial.subcatch_polygon_y.size() < us) ctx.spatial.subcatch_polygon_y.resize(us);


    // -------------------------------------------------------------------------
    // Slice IO-3: Resolve every external-file slot to an absolute path.
    // Runs BEFORE load_external_timeseries_files so the loader can read
    // each file's resolved path directly from `tbl.file_path.absolute`.
    // -------------------------------------------------------------------------
    const std::string inp_dir = openswmm::io::parentDir(ctx.inp_file_path);
    resolve_external_file_slots(ctx, inp_dir);

    // -------------------------------------------------------------------------
    // Load external FILE-referenced timeseries from disk
    // -------------------------------------------------------------------------
    // Timeseries with FILE references (e.g., rainfall .dat files) need to be
    // loaded into memory before any date offset or gage resolution.
    load_external_timeseries_files(ctx, inp_dir);

    // -------------------------------------------------------------------------
    // Load external FILE-source rain-gage data (standard SWMM rain files).
    // Must run after resolve_external_file_slots (for absolute paths) and after
    // options parsing (needs the simulation window to bound retained records).
    // -------------------------------------------------------------------------
    load_external_rain_files(ctx);

    // -------------------------------------------------------------------------
    // Timeseries date offset resolution
    // -------------------------------------------------------------------------
    // Timeseries without explicit dates have x-values starting near 0 (fractional
    // days from midnight). These are relative to the simulation start date.
    // Offset them by start_date so absolute OADate lookups work.
    for (std::size_t t = 0; t < ctx.tables.tables.size(); ++t) {
        auto& tbl = ctx.tables.tables[t];
        if (tbl.type != TableType::TIMESERIES) continue;
        if (tbl.x.empty()) continue;

        // If first x-value is small (< 366, i.e. less than one year in days),
        // it's a relative timeseries and needs the start_date offset.
        // Absolute dates (with MM/DD/YYYY) would produce values > 30000.
        if (tbl.x[0] < 366.0) {
            double offset = ctx.options.start_date;
            for (auto& xv : tbl.x) {
                xv += offset;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Gage timeseries re-resolution
    // -------------------------------------------------------------------------
    // If RAINGAGES section appeared before TIMESERIES, ts_index will be -1.
    // Re-resolve using the stored ts_name.
    for (int g = 0; g < n_gages; ++g) {
        auto ug = static_cast<std::size_t>(g);
        if (ctx.gages.ts_index[ug] < 0 &&
            ctx.gages.source[ug] == RainSource::TIMESERIES &&
            !ctx.gages.ts_name[ug].empty()) {
            ctx.gages.ts_index[ug] = ctx.find_timeseries(ctx.gages.ts_name[ug]);
        }
    }

    // -------------------------------------------------------------------------
    // Gap #53: Co-gage detection
    // -------------------------------------------------------------------------
    // When two or more gages share the same TIMESERIES source and ts_index, the
    // secondary gages should copy rainfall from the primary (lowest-index gage)
    // rather than querying the timeseries independently.  Matches legacy coGage.
    for (int gj = 0; gj < n_gages; ++gj) {
        auto ugj = static_cast<std::size_t>(gj);
        ctx.gages.co_gage_index[ugj] = -1;
        if (ctx.gages.source[ugj] != RainSource::TIMESERIES) continue;
        int ts_j = ctx.gages.ts_index[ugj];
        if (ts_j < 0) continue;
        for (int gi = 0; gi < gj; ++gi) {
            auto ugi = static_cast<std::size_t>(gi);
            if (ctx.gages.source[ugi] == RainSource::TIMESERIES &&
                ctx.gages.ts_index[ugi] == ts_j) {
                ctx.gages.co_gage_index[ugj] = gi;
                break;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Subcatchment outlet re-resolution
    // -------------------------------------------------------------------------
    // If SUBCATCHMENTS parsed before JUNCTIONS/OUTFALLS, outlet_node will be -1.
    // Re-resolve using the stored outlet_name string.
    for (int s = 0; s < n_subcatch; ++s) {
        auto us = static_cast<std::size_t>(s);
        if (us >= ctx.subcatches.outlet_name.size()) continue;
        const auto& name = ctx.subcatches.outlet_name[us];
        if (name.empty()) continue;

        // Already resolved during parsing — validate it
        if (ctx.subcatches.outlet_node[us] >= 0 &&
            ctx.subcatches.outlet_node[us] < n_nodes) {
            continue;
        }
        if (ctx.subcatches.outlet_subcatch[us] >= 0 &&
            ctx.subcatches.outlet_subcatch[us] < n_subcatch) {
            continue;
        }

        // Try node first, then subcatchment
        int node_idx = ctx.node_names.find(name);
        if (node_idx >= 0) {
            ctx.subcatches.outlet_node[us] = node_idx;
            ctx.subcatches.outlet_subcatch[us] = -1;
        } else {
            int sub_idx = ctx.subcatch_names.find(name);
            if (sub_idx >= 0) {
                ctx.subcatches.outlet_subcatch[us] = sub_idx;
                ctx.subcatches.outlet_node[us] = -1;
            } else {
                // Outlet references neither a node nor a subcatchment — legacy
                // raises a fatal ERR_NAME (209 undefined object). Surface it so
                // the open fails instead of silently dropping the outlet.
                ctx.errors.push_back(format_error(ERR_NAME, name));
            }
        }
    }

    // -------------------------------------------------------------------------
    // Subcatchment snow pack resolution
    // -------------------------------------------------------------------------
    // SUBCATCHMENTS is normally parsed before SNOWPACKS, so the snowpack index
    // could not be resolved at parse time. Resolve it here from the stored
    // name; without this the pack is never linked and never accumulates.
    for (int s = 0; s < n_subcatch; ++s) {
        auto us = static_cast<std::size_t>(s);
        if (us >= ctx.subcatches.snowpack_name.size()) continue;
        const auto& name = ctx.subcatches.snowpack_name[us];
        if (name.empty()) continue;
        if (ctx.subcatches.snowpack[us] >= 0) continue;  // already resolved
        ctx.subcatches.snowpack[us] = ctx.snowpack_names.find(name);
    }

    // -------------------------------------------------------------------------
    // Subcatchment gage re-resolution
    // -------------------------------------------------------------------------
    // If SUBCATCHMENTS parsed before RAINGAGES, gage indices may be -1. The
    // handler stored the gage NAME in gage_name for exactly this case: re-resolve
    // from the name, and raise a fatal ERR_NAME (209) if it names no defined gage
    // (legacy project.c behavior). A blank name means no gage assigned (allowed).
    for (int s = 0; s < n_subcatch; ++s) {
        auto us = static_cast<std::size_t>(s);
        int gi = ctx.subcatches.gage[us];
        if (gi >= 0 && gi < n_gages) continue;  // already resolved and in range

        const auto& name = (us < ctx.subcatches.gage_name.size())
                               ? ctx.subcatches.gage_name[us]
                               : std::string{};
        if (name.empty()) {
            ctx.subcatches.gage[us] = -1;
            continue;
        }
        int idx = ctx.gage_names.find(name);
        if (idx >= 0 && idx < n_gages) {
            ctx.subcatches.gage[us] = idx;
        } else {
            ctx.subcatches.gage[us] = -1;
            ctx.errors.push_back(format_error(ERR_NAME, name));
        }
    }

    // -------------------------------------------------------------------------
    // Storage curve name resolution
    // -------------------------------------------------------------------------
    for (int i = 0; i < n_nodes; ++i) {
        if (ctx.nodes.type[static_cast<std::size_t>(i)] != NodeType::STORAGE) continue;
        const int r = ctx.node_subtypes.storage_row(i);
        if (r < 0) continue;
        auto& S = ctx.node_subtypes.storages;
        const auto ur = static_cast<std::size_t>(r);
        if (S.curve[ur] < 0 && !S.curve_name[ur].empty()) {
            S.curve[ur] = ctx.find_curve(S.curve_name[ur]);
            if (S.curve[ur] < 0)
                ctx.errors.push_back(format_error(ERR_NAME, S.curve_name[ur]));
        }
    }

    // -------------------------------------------------------------------------
    // Outfall stage-data name resolution (TIDAL curve / TIMESERIES stage series)
    // -------------------------------------------------------------------------
    // [OUTFALLS] may precede [CURVES]/[TIMESERIES] in the .inp, so the handler
    // stores the raw name and defers resolution to here. Curves and timeseries
    // share one index space (ctx.tables), so an unresolved param of 0 would
    // silently alias whatever table happens to be first — hence -1 as the
    // sentinel and a fatal ERR_NAME on failure, matching legacy
    // outfall_readParams() (node.c:1345-1354).
    for (int i = 0; i < n_nodes; ++i) {
        if (ctx.nodes.type[static_cast<std::size_t>(i)] != NodeType::OUTFALL) continue;
        const int r = ctx.node_subtypes.outfall_row(i);
        if (r < 0) continue;
        auto& O = ctx.node_subtypes.outfalls;
        const auto ur = static_cast<std::size_t>(r);
        const bool is_tidal = (O.bc_type[ur] == OutfallType::TIDAL);
        if (!is_tidal && O.bc_type[ur] != OutfallType::TIMESERIES) continue;
        if (O.param[ur] >= 0.0) continue;              // already resolved (e.g. via API)

        const std::string& nm = O.param_name[ur];
        const int t = nm.empty() ? -1
                                 : (is_tidal ? ctx.find_curve(nm)
                                             : ctx.find_timeseries(nm));
        if (t < 0) {
            ctx.errors.push_back(format_error(ERR_NAME, nm));
            continue;
        }
        // Enforce the referent kind legacy got for free from separate
        // Curve[]/Tseries[] arrays: TIDAL wants a curve, TIMESERIES a series.
        const bool is_ts = (ctx.tables.tables[static_cast<std::size_t>(t)].type
                                == TableType::TIMESERIES);
        if (is_tidal == is_ts) {
            ctx.errors.push_back(format_error(ERR_NAME, nm));
            continue;
        }
        O.param[ur] = static_cast<double>(t);
    }

    // -------------------------------------------------------------------------
    // Pump curve name resolution
    // -------------------------------------------------------------------------
    for (int j = 0; j < n_links; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx.links.type[uj] != LinkType::PUMP) continue;
        const int pr = ctx.link_subtypes.pump_row(j);
        if (pr < 0) continue;
        const auto upr = static_cast<std::size_t>(pr);
        if (ctx.link_subtypes.pumps.curve[upr] >= 0) continue; // already resolved
        // Empty or "*" means no curve — an IDEAL pump (legacy link.c:1437).
        // "*" can still arrive here from the GeoPackage reader / API, which
        // store the raw name.
        if (ctx.links.pump_curve_name[uj].empty() ||
            ctx.links.pump_curve_name[uj] == "*") continue;
        ctx.link_subtypes.pumps.curve[upr] = ctx.find_curve(ctx.links.pump_curve_name[uj]);
        if (ctx.link_subtypes.pumps.curve[upr] < 0)
            ctx.errors.push_back(format_error(ERR_NAME, ctx.links.pump_curve_name[uj]));
    }

    // Outlet (TABULAR) rating curve name resolution — curve name stored in
    // pump_curve_name; resolved index stored in OutletData.curve.
    for (int j = 0; j < n_links; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx.links.type[uj] != LinkType::OUTLET) continue;
        const int olr = ctx.link_subtypes.outlet_row(j);
        if (olr < 0) continue;
        const auto uolr = static_cast<std::size_t>(olr);
        int outlet_type = static_cast<int>(ctx.link_subtypes.outlets.outlet_type[uolr]);
        if (outlet_type < 2) continue; // FUNCTIONAL — no curve needed
        if (ctx.link_subtypes.outlets.curve[uolr] >= 0) continue; // already resolved
        if (ctx.links.pump_curve_name[uj].empty()) continue;
        ctx.link_subtypes.outlets.curve[uolr] = ctx.find_curve(ctx.links.pump_curve_name[uj]);
        if (ctx.link_subtypes.outlets.curve[uolr] < 0)
            ctx.errors.push_back(format_error(ERR_NAME, ctx.links.pump_curve_name[uj]));
    }

    // Convert pump startup/shutoff depths from display units → internal (ft)
    {
        int us = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
        double ucf_len = ucf::Ucf[ucf::LENGTH][us];
        for (int j = 0; j < n_links; ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx.links.type[uj] != LinkType::PUMP) continue;
            const int pr = ctx.link_subtypes.pump_row(j);
            if (pr < 0) continue;
            const auto upr = static_cast<std::size_t>(pr);
            if (ctx.link_subtypes.pumps.startup[upr] > 0.0)
                ctx.link_subtypes.pumps.startup[upr] /= ucf_len;
            if (ctx.link_subtypes.pumps.shutoff[upr] > 0.0)
                ctx.link_subtypes.pumps.shutoff[upr] /= ucf_len;
        }
    }

    // NOTE: A centralised display→internal (feet) unit conversion for node and
    // cross-section geometry was prototyped here but reverted — see the audit
    // in docs/UNIT_CONVERSION_AUDIT.md.  Converting geometry in isolation made
    // SI routing continuity *worse* (dry −13.9% → −577%) because the hydraulic
    // core (volume / continuity accounting) carries compensating unit bugs that
    // were tuned to the un-converted (metres) geometry.  The full fix must
    // convert every field in the audit AND repair the exposed core accounting
    // together, validated by a metric (SI) regression model.

    // -------------------------------------------------------------------------
    // Node init_depth → head initialisation
    // -------------------------------------------------------------------------
    for (int i = 0; i < n_nodes; ++i) {
        ctx.nodes.head[i]  = ctx.nodes.invert_elev[i] + ctx.nodes.init_depth[i];
        ctx.nodes.depth[i] = ctx.nodes.init_depth[i];
    }

    // -------------------------------------------------------------------------
    // External inflow timeseries resolution
    // -------------------------------------------------------------------------
    // Resolve timeseries name → table index for all external inflows
    for (int i = 0; i < ctx.ext_inflows.count(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (!ctx.ext_inflows.ts_name[ui].empty()) {
            int ts_idx = ctx.find_timeseries(ctx.ext_inflows.ts_name[ui]);
            // Store resolved index - the inflow solver uses ts_name for lookup,
            // but we can cache the index for performance
            (void)ts_idx; // ts_name is used directly by InflowSolver
        }
    }

    // -------------------------------------------------------------------------
    // [INFLOWS] / [DWF] / [RDII] node re-resolution
    // -------------------------------------------------------------------------
    // These sections may precede the node sections in the .inp — handlers
    // store the raw node name and defer resolution here (legacy two-pass
    // parsing is order-independent). A name that still fails to resolve is
    // a fatal input error, matching legacy error_setInpError(ERR_NAME, ...)
    // in inflow_readExtInflow/inflow_readDwfInflow/rdii_readRdiiInflow.
    // Iterate backwards so erase() keeps remaining indices valid.
    {
        auto resolve_node_rows =
            [&ctx](auto& store, std::vector<int>& idx,
                   const std::vector<std::string>& names) {
            for (int i = store.count() - 1; i >= 0; --i) {
                auto ui = static_cast<std::size_t>(i);
                if (idx[ui] >= 0) continue;
                const std::string& name = names[ui];
                if (!name.empty()) {
                    idx[ui] = ctx.node_names.find(name);
                    if (idx[ui] >= 0) continue;
                    ctx.errors.push_back(format_error(ERR_NAME, name));
                }
                store.erase(i);   // never expose an unresolved row downstream
            }
        };
        resolve_node_rows(ctx.ext_inflows, ctx.ext_inflows.node_idx,
                          ctx.ext_inflows.node_name);
        resolve_node_rows(ctx.dwf_inflows, ctx.dwf_inflows.node_idx,
                          ctx.dwf_inflows.node_name);
        resolve_node_rows(ctx.rdii_assigns, ctx.rdii_assigns.node_idx,
                          ctx.rdii_assigns.node_name);
    }

    // -------------------------------------------------------------------------
    // Quality data matrix sizing
    // -------------------------------------------------------------------------
    // Resize buildup/washoff matrices now that landuse and pollutant counts are final
    const int n_landuses = ctx.landuse_names.size();
    if (n_landuses > 0 && n_polluts > 0) {
        if (ctx.buildup.n_landuses == 0) {
            ctx.buildup.resize(n_landuses, n_polluts);
        }
        if (ctx.washoff.n_landuses == 0) {
            ctx.washoff.resize(n_landuses, n_polluts);
        }
    }
    if (n_polluts > 0 && n_nodes > 0) {
        if (ctx.treatment.n_nodes == 0) {
            ctx.treatment.resize(n_nodes, n_polluts);
        }
    }

    // -------------------------------------------------------------------------
    // Subcatchment coverage matrix sizing
    // -------------------------------------------------------------------------
    if (n_landuses > 0 && n_subcatch > 0) {
        auto total = static_cast<std::size_t>(n_subcatch * n_landuses);
        if (ctx.subcatches.coverage.size() < total) {
            ctx.subcatches.coverage.resize(total, 0.0);
        }
        ctx.subcatches.coverage_n_landuses = n_landuses;
    }

    // -------------------------------------------------------------------------
    // Divider link and curve re-resolution
    // -------------------------------------------------------------------------
    for (int i = 0; i < n_nodes; ++i) {
        if (ctx.nodes.type[static_cast<std::size_t>(i)] != NodeType::DIVIDER) continue;
        const int r = ctx.node_subtypes.divider_row(i);
        if (r < 0) continue;
        auto& D = ctx.node_subtypes.dividers;
        const auto ur = static_cast<std::size_t>(r);

        // Re-resolve diversion link name → index
        if (D.link[ur] < 0 && !D.link_name[ur].empty())
            D.link[ur] = ctx.link_names.find(D.link_name[ur]);

        // Re-resolve diversion curve name → index (TABULAR dividers)
        if (D.curve[ur] < 0 && !D.curve_name[ur].empty())
            D.curve[ur] = ctx.find_curve(D.curve_name[ur]);
    }

    // -------------------------------------------------------------------------
    // Node degree (connectivity) computation
    // -------------------------------------------------------------------------
    // Count the number of links connected to each node for downstream routing
    for (int j = 0; j < n_links; ++j) {
        auto uj = static_cast<std::size_t>(j);
        int n1 = ctx.links.node1[uj];
        int n2 = ctx.links.node2[uj];
        if (n1 >= 0 && n1 < n_nodes)
            ctx.nodes.degree[static_cast<std::size_t>(n1)]++;
        if (n2 >= 0 && n2 < n_nodes)
            ctx.nodes.degree[static_cast<std::size_t>(n2)]++;
    }
    // NOTE: the degree SIGN (negated for upstream-terminal nodes, used by the
    // EXTRAN surcharge corr=0.6 factor) is applied once at the END of node
    // initialisation in SWMMEngine (after the second, conduit-only degree pass),
    // matching legacy flowrout.c::validateGeneralLayout.

    // -------------------------------------------------------------------------
    // Evaporation timeseries resolution
    // -------------------------------------------------------------------------
    if (ctx.options.evap_type == 2 && !ctx.options.evap_ts_name.empty()) {
        int ts_idx = ctx.find_timeseries(ctx.options.evap_ts_name);
        (void)ts_idx; // stored by name, resolved at runtime
    }

    // -------------------------------------------------------------------------
    // Link cross-section derived properties
    // -------------------------------------------------------------------------
    // -------------------------------------------------------------------------
    // Non-conduit link offset conversion (ELEVATION → DEPTH)
    // -------------------------------------------------------------------------
    // The conduit pass below skips non-conduit links (length==0). We need a
    // separate pass for pumps, orifices, weirs, outlets.
    if (ctx.options.link_offsets == 1) { // ELEV_OFFSET
        for (int j = 0; j < n_links; ++j) {
            auto uj = static_cast<std::size_t>(j);
            auto lt = ctx.links.type[uj];
            if (lt == LinkType::CONDUIT || lt == LinkType::PUMP) continue;
            int n1 = ctx.links.node1[uj];
            int n2 = ctx.links.node2[uj];

            if (lt == LinkType::ORIFICE) {
                // [ORIFICES] offset1 = absolute invert elevation → convert to depth above n1
                if (n1 >= 0 && n1 < n_nodes) {
                    double raw1 = ctx.links.offset1[uj] - ctx.nodes.invert_elev[static_cast<std::size_t>(n1)];
                    if (raw1 < 0.0)
                        ctx.warnings.push_back(format_warning(WARN_NEGATIVE_OFFSET, ctx.link_names.name_of(j)));
                    ctx.links.offset1[uj] = std::max(0.0, raw1);
                }
            } else if (lt == LinkType::WEIR || lt == LinkType::OUTLET) {
                // [WEIRS]/[OUTLETS]: crest_height = absolute crest elevation →
                // convert to depth above n1, operating on the side-table row.
                const int wr  = ctx.link_subtypes.weir_row(j);
                const int olr = (wr < 0) ? ctx.link_subtypes.outlet_row(j) : -1;
                double* crest = (wr >= 0)
                    ? &ctx.link_subtypes.weirs.crest_height[static_cast<std::size_t>(wr)]
                    : (olr >= 0 ? &ctx.link_subtypes.outlets.crest_height[static_cast<std::size_t>(olr)]
                                : nullptr);
                if (crest && n1 >= 0 && n1 < n_nodes) {
                    double rawCrest = *crest - ctx.nodes.invert_elev[static_cast<std::size_t>(n1)];
                    *crest = std::max(0.0, rawCrest);
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // Regulator crest below downstream invert (legacy WARNING 10)
    // -------------------------------------------------------------------------
    // PARITY link.c link_validate:424-439: for ORIFICE/WEIR/OUTLET links whose
    // crest (invert(n1) + offset) lies below the downstream node invert, DW
    // routing RAISES the crest with offset = invert(n2) - invert(n1). Legacy
    // applies this for BOTH offset conventions (it runs after the ELEV→DEPTH
    // conversion in link_validate), so it must NOT be gated on link_offsets.
    // The raised offset's bits differ from invert+parsed-offset (e.g. Bellinge
    // G60F61Yo1 hcrest by 1 ULP), which near-crest head cancellation amplifies.
    for (int j = 0; j < n_links; ++j) {
        auto uj = static_cast<std::size_t>(j);
        auto lt = ctx.links.type[uj];
        if (lt != LinkType::ORIFICE && lt != LinkType::WEIR &&
            lt != LinkType::OUTLET) continue;
        int n1 = ctx.links.node1[uj];
        int n2 = ctx.links.node2[uj];
        if (n1 < 0 || n1 >= n_nodes || n2 < 0 || n2 >= n_nodes) continue;

        double* off = nullptr;
        if (lt == LinkType::ORIFICE) {
            off = &ctx.links.offset1[uj];
        } else {
            const int wr  = ctx.link_subtypes.weir_row(j);
            const int olr = (wr < 0) ? ctx.link_subtypes.outlet_row(j) : -1;
            off = (wr >= 0)
                ? &ctx.link_subtypes.weirs.crest_height[static_cast<std::size_t>(wr)]
                : (olr >= 0 ? &ctx.link_subtypes.outlets.crest_height[static_cast<std::size_t>(olr)]
                            : nullptr);
        }
        if (!off) continue;

        double inv1 = ctx.nodes.invert_elev[static_cast<std::size_t>(n1)];
        double inv2 = ctx.nodes.invert_elev[static_cast<std::size_t>(n2)];
        if (inv1 + *off < inv2) {
            if (ctx.options.routing_model == RoutingModel::DYNWAVE ||
                ctx.options.routing_model == RoutingModel::FV) {
                *off = inv2 - inv1;   // legacy link.c:434-435 (bit-exact form)
            }
            ctx.warnings.push_back(format_warning(WARN_REGULATOR_CREST_LOW, ctx.link_names.name_of(j)));
        }
    }

    // Build transect geometry tables for IRREGULAR cross-sections.
    // Each TransectStore entry → TransectData with precomputed area/width/hrad tables.
    {
        int nt = ctx.transects.count();
        ctx.transect_tables.resize(static_cast<std::size_t>(nt));
        for (int t = 0; t < nt; ++t) {
            auto ut = static_cast<std::size_t>(t);
            auto& td = ctx.transect_tables[ut];
            td.name      = ctx.transects.names[ut];
            td.n_left    = ctx.transects.n_left[ut];
            td.n_right   = ctx.transects.n_right[ut];
            td.n_channel = ctx.transects.n_channel[ut];
            if (td.n_channel <= 0.0) {
                ctx.errors.push_back(format_error(ERR_TRANSECT_MANNING, td.name));
                continue;
            }
            td.stations  = ctx.transects.stations[ut];
            td.elevations = ctx.transects.elevations[ut];
            td.length_factor = ctx.transects.length_factor[ut];
            // PARITY: legacy transect setParams/addStation (transect.c:360-410)
            // transform the raw [TRANSECTS] GR data BEFORE building the tables:
            //   Station = x * Xfactor / UCF(LENGTH)          (mult, then divide)
            //   Elev    = (y + Yfactor) / UCF(LENGTH),  Yfactor = x9 / UCF
            //   Xbank   = (xbank / UCF) * Xfactor            (divide, then mult)
            // The elevation OFFSET (x9, e.g. 799/798 in extran8a) is the load-
            // bearing part: legacy builds every slice area/width/hrad at the
            // real bed elevation (~800 ft), so `y - yhi` rounds at that
            // magnitude. Building on the raw ~0-ft elevations rounds
            // differently (~1e-13 per entry) and breaks bit-parity even though
            // the geometry is offset-invariant in exact arithmetic. Applied to
            // the td COPY only, so [TRANSECTS] round-trips the raw GR values.
            const int t_us = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
            const double t_ucf = ucf::Ucf[ucf::LENGTH][static_cast<std::size_t>(t_us)];
            double xFactor = ctx.transects.x_factor[ut];      // parse-time 0→1 default
            if (xFactor == 0.0) xFactor = 1.0;
            const double yFactor = ctx.transects.y_factor[ut] / t_ucf;   // x9 / UCF
            for (auto& s : td.stations)   s = s * xFactor / t_ucf;
            for (auto& e : td.elevations) e = (e + yFactor) / t_ucf;
            td.x_left_bank  = (ctx.transects.x_left_bank[ut]  / t_ucf) * xFactor;
            td.x_right_bank = (ctx.transects.x_right_bank[ut] / t_ucf) * xFactor;
            transect::buildTables(td);
        }
        // Resolve IRREGULAR link transect names → indices, then set properties
        for (int j = 0; j < n_links; ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx.links.xsect_shape[uj] != XsectShape::IRREGULAR) continue;
            // Resolve transect name (stored in pump_curve_name as temp field)
            const auto& tname = ctx.links.pump_curve_name[uj];
            if (!tname.empty()) {
                for (int t = 0; t < nt; ++t) {
                    if (ieq(ctx.transects.names[static_cast<std::size_t>(t)],
                            tname)) {
                        ctx.links.xsect_curve[uj] = t;
                        break;
                    }
                }
            }
            int ci = ctx.links.xsect_curve[uj];
            if (ci >= 0 && ci < nt) {
                const auto& td = ctx.transect_tables[static_cast<std::size_t>(ci)];
                ctx.links.xsect_y_full[uj] = td.y_full;
                ctx.links.xsect_a_full[uj] = td.a_full;
                ctx.links.xsect_r_full[uj] = td.r_full;
                ctx.links.xsect_w_max[uj]  = td.w_max;
                // The full section-factor params (sFull, physical sMax, aBot,
                // ywMax) — legacy getTransectParams (xsect.c:1339-1358) — are
                // set in the shape-property resolution block below (the
                // XsectShape::IRREGULAR case), which runs after transect-name
                // resolution and owns s_full/s_max/a_bot/yw_max.
            }
        }
    }

    // Resolve CUSTOM shape curves — these use [CURVES] Shape type entries
    // that define normalized (depth/yFull, width/wMax) relationships.
    {
        int n_tables = static_cast<int>(ctx.tables.tables.size());
        for (int j = 0; j < n_links; ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx.links.xsect_shape[uj] != XsectShape::CUSTOM) continue;

            const auto& cname = ctx.links.pump_curve_name[uj];
            if (cname.empty()) continue;

            // Find curve by name in tables
            int ci = ctx.find_curve(cname);
            if (ci >= 0 && ci < n_tables) {
                ctx.links.xsect_curve[uj] = ci;

                // Compute full-depth properties from shape curve
                // The curve gives normalized (y/yFull) vs (w/wMax).
                // At y/yFull = 1.0, w/wMax should be 0 (top of shape).
                // Integrate area using trapezoidal rule.
                const auto& tbl = ctx.tables.tables[static_cast<std::size_t>(ci)];
                double y_full = ctx.links.xsect_y_full[uj];
                if (y_full <= 0.0 || tbl.x.size() < 2) continue;

                // Find max width from curve (typically at y_norm ~0.5)
                double w_max_norm = 0.0;
                for (std::size_t i = 0; i < tbl.y.size(); ++i) {
                    if (tbl.y[i] > w_max_norm) w_max_norm = tbl.y[i];
                }
                // wMax is stored in tbl.y as normalized values, but the actual
                // wMax comes from integrating the shape. For CUSTOM, legacy
                // computes wMax = max width from the curve.
                // The curve x = depth/yFull, y = width/wMax (normalized)
                // We need to find wMax. For now, use y_full as width scale
                // (CUSTOM shapes typically define width relative to y_full).
                double w_max = w_max_norm * y_full; // approximate

                // Physical width = curve_y * y_full (curve_y IS width/yFull, not width/wMax)
                // buildCustomTables will compute correct a_full/r_full/w_max
                ctx.links.xsect_w_max[uj] = w_max; // Initial estimate, refined below

                // Build CUSTOM table as a TransectData for XSectBatch lookup
                // Store offset = nt + index in supplementary vector
                transect::TransectData ctd;
                ctd.name = cname;
                ctd.y_full = y_full;
                ctd.a_full = 0.0; // Computed by buildCustomTables
                ctd.r_full = 0.0;
                ctd.w_max  = w_max;
                transect::buildCustomTables(ctd, y_full,
                    tbl.x.data(), tbl.y.data(), static_cast<int>(tbl.x.size()));
                // Update link properties from built table
                ctx.links.xsect_a_full[uj] = ctd.a_full;
                ctx.links.xsect_r_full[uj] = ctd.r_full;
                ctx.links.xsect_w_max[uj]  = ctd.w_max;
                // Store table; use negative xsect_curve for CUSTOM
                // (offset into transect_tables by nt + custom_index)
                int custom_idx = static_cast<int>(ctx.transect_tables.size());
                ctx.transect_tables.push_back(std::move(ctd));
                ctx.links.xsect_curve[uj] = custom_idx;
            }
        }
    }

    // Resolve STREET cross-sections — build a transect from each referenced
    // [STREETS] entry (gutter + road crown + backing) and attach it like an
    // IRREGULAR/CUSTOM table. Slopes are %→fraction and lengths display→ft,
    // matching legacy street_readParams.
    {
        const int us = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
        const double inv_len = ucf::Ucf_inv[ucf::LENGTH][static_cast<std::size_t>(us)];
        for (int j = 0; j < n_links; ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx.links.xsect_shape[uj] != XsectShape::STREET_XSECT) continue;
            const auto& sname = ctx.links.pump_curve_name[uj];   // street name (temp field)
            if (sname.empty()) continue;

            int si = -1;
            for (int s = 0; s < ctx.streets.count(); ++s) {
                if (ieq(ctx.streets.names[static_cast<std::size_t>(s)], sname)) {
                    si = s;
                    break;
                }
            }
            if (si < 0) continue;
            auto su = static_cast<std::size_t>(si);

            street::StreetParams sp;
            sp.width             = ctx.streets.t_crown[su]       * inv_len;
            sp.curb_height       = ctx.streets.h_curb[su]        * inv_len;
            sp.slope             = ctx.streets.sx[su]            / 100.0;   // % → fraction
            sp.roughness         = ctx.streets.n_road[su];
            sp.gutter_depression = ctx.streets.gutter_depres[su] * inv_len;
            sp.gutter_width      = ctx.streets.gutter_width[su]  * inv_len;
            sp.sides             = ctx.streets.sides[su];
            sp.back_width        = ctx.streets.back_width[su]    * inv_len;
            sp.back_slope        = ctx.streets.back_slope[su]    / 100.0;   // % → fraction
            sp.back_roughness    = ctx.streets.back_n[su];

            transect::TransectData td;
            td.name = sname;
            street::buildTransect(sp, td);

            int idx_tbl = static_cast<int>(ctx.transect_tables.size());
            ctx.transect_tables.push_back(std::move(td));
            const auto& built = ctx.transect_tables[static_cast<std::size_t>(idx_tbl)];
            ctx.links.xsect_curve[uj]  = idx_tbl;
            ctx.links.xsect_y_full[uj] = built.y_full;
            ctx.links.xsect_a_full[uj] = built.a_full;
            ctx.links.xsect_r_full[uj] = built.r_full;
            ctx.links.xsect_w_max[uj]  = built.w_max;
        }
    }

    // Use global constants
    using constants::PI;
    using constants::GRAVITY;
    using constants::PHI;
    using constants::MIN_DELTA_Z;

    // Compute full-flow properties from cross-section geometry
    // (matches legacy xsect_setParams in xsect.c)
    // NOTE: Must run for ALL link types that have cross-sections (CONDUIT,
    // ORIFICE, WEIR). Orifice/weir flow equations use xsect_a_full, y_full,
    // and w_max from the [XSECTIONS] section — skipping them leaves a_full=0
    // which causes zero flow for all orifices.
    for (int j = 0; j < n_links; ++j) {
        auto uj = static_cast<std::size_t>(j);
        auto lt = ctx.links.type[uj];
        if (lt != LinkType::CONDUIT && lt != LinkType::ORIFICE &&
            lt != LinkType::WEIR) continue;

        double y_full = ctx.links.xsect_y_full[uj];
        double w_max  = ctx.links.xsect_w_max[uj];
        XsectShape shape = ctx.links.xsect_shape[uj];

        double a_full = 0.0, r_full = 0.0, s_full = 0.0, s_max = 0.0;
        double yw_max = 0.0;

        switch (shape) {
        case XsectShape::CUSTOM: {
            // Properties already set from CUSTOM shape curve above.
            a_full = ctx.links.xsect_a_full[uj];
            r_full = ctx.links.xsect_r_full[uj];
            w_max  = ctx.links.xsect_w_max[uj];
            s_full = a_full * std::pow(r_full, 2.0/3.0);   // xsect.c:684 (no clamp)
            s_max  = s_full;   // fallback if the shape curve was not resolved
            yw_max = y_full;
            int ci = ctx.links.xsect_curve[uj];
            if (ci >= 0 && static_cast<std::size_t>(ci) < ctx.transect_tables.size()) {
                const auto& td = ctx.transect_tables[static_cast<std::size_t>(ci)];
                XSectParams xs;
                xs.type = link::translateShape(shape);
                link::applyTabulatedXSectParams(xs, td, y_full, ci);
                a_full = xs.a_full; r_full = xs.r_full; w_max = xs.w_max;
                s_full = xs.s_full; s_max = xs.s_max; yw_max = xs.yw_max;
                ctx.links.xsect_a_bot[uj] = xs.a_bot;
            }
            break;
        }

        case XsectShape::IRREGULAR: {
            int ci = ctx.links.xsect_curve[uj];
            if (ci >= 0 && static_cast<std::size_t>(ci) < ctx.transect_tables.size()) {
                const auto& td = ctx.transect_tables[static_cast<std::size_t>(ci)];
                XSectParams xs;
                xs.type = link::translateShape(shape);
                link::applyTabulatedXSectParams(xs, td, y_full, ci);
                y_full = xs.y_full; a_full = xs.a_full; r_full = xs.r_full;
                w_max  = xs.w_max;  s_full = xs.s_full; s_max  = xs.s_max;
                yw_max = xs.yw_max;
                ctx.links.xsect_a_bot[uj] = xs.a_bot;
            } else {
                // Fallback if transect not resolved
                a_full = w_max * y_full;
                double p_def = 2.0 * y_full + w_max;
                r_full = (p_def > 0.0) ? a_full / p_def : 0.0;
                s_full = a_full * std::pow(r_full, 2.0/3.0);
                s_max  = s_full;
                yw_max = y_full;
            }
            break;
        }

        case XsectShape::STREET_XSECT: {
            // Properties already set from street tables above. NB: unlike the
            // IRREGULAR/CUSTOM branches this deliberately leaves xsect_a_bot
            // untouched.
            int ci = ctx.links.xsect_curve[uj];
            if (ci >= 0 && static_cast<std::size_t>(ci) < ctx.transect_tables.size()) {
                const auto& td = ctx.transect_tables[static_cast<std::size_t>(ci)];
                XSectParams xs;
                xs.type = link::translateShape(shape);
                link::applyTabulatedXSectParams(xs, td, y_full, ci);
                y_full = xs.y_full; a_full = xs.a_full; r_full = xs.r_full;
                w_max  = xs.w_max;  s_full = xs.s_full; s_max  = xs.s_max;
                yw_max = xs.yw_max;
            } else {
                // Fallback if street not resolved
                a_full = w_max * y_full;
                double p_def = 2.0 * y_full + w_max;
                r_full = (p_def > 0.0) ? a_full / p_def : 0.0;
                s_full = a_full * std::pow(r_full, 2.0/3.0);
                s_max  = s_full;
                yw_max = y_full;
            }
            break;
        }

        default: {
            // SINGLE SOURCE OF TRUTH: delegate every self-contained shape to the
            // legacy-faithful xsect::setParams. Feed the RAW [XSECTIONS] Geom1–4
            // (display units, never overwritten) plus the length ucf, exactly as
            // legacy xsect_setParams expects — setParams divides only the
            // length-valued params by ucf and leaves slopes / size-codes /
            // C-factors raw. Reading raw geom (not the derived working fields)
            // makes setup idempotent and identical across the INP and
            // programmatic-builder paths. (LinkData::XsectShape is renumbered vs
            // the legacy/batch XSectShape, so translate first.)
            const int us = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
            const double ucf_len = ucf::Ucf[ucf::LENGTH][static_cast<std::size_t>(us)];
            XSectParams xs;
            double p[4] = { ctx.links.xsect_geom1[uj], ctx.links.xsect_geom2[uj],
                            ctx.links.xsect_geom3[uj], ctx.links.xsect_geom4[uj] };
            int rc = xsect::setParams(xs, link::translateShape(shape), p, ucf_len);
            if (rc == 0 && xs.a_full > 0.0) {
                y_full = xs.y_full; w_max = xs.w_max; yw_max = xs.yw_max;
                a_full = xs.a_full; r_full = xs.r_full;
                s_full = xs.s_full; s_max  = xs.s_max;
                ctx.links.xsect_y_bot[uj] = xs.y_bot;
                ctx.links.xsect_a_bot[uj] = xs.a_bot;
                ctx.links.xsect_s_bot[uj] = xs.s_bot;
                ctx.links.xsect_r_bot[uj] = xs.r_bot;
            } else {
                // Invalid geometry — preserve the previous generic fallback.
                a_full = w_max * y_full;
                double p_def = 2.0 * y_full + w_max;
                r_full = (p_def > 0.0) ? a_full / p_def : 0.0;
                s_full = a_full * std::pow(r_full, 2.0/3.0);
                s_max  = s_full;
                yw_max = y_full;
            }
            break;
        }
        }

        ctx.links.xsect_y_full[uj] = y_full;
        ctx.links.xsect_a_full[uj] = a_full;
        ctx.links.xsect_r_full[uj] = r_full;
        ctx.links.xsect_s_full[uj] = s_full;
        ctx.links.xsect_s_max[uj]  = s_max;
        ctx.links.xsect_w_max[uj]  = w_max;
        ctx.links.xsect_yw_max[uj] = yw_max;
    }

    // -------------------------------------------------------------------------
    // Conduit slope computation (matches legacy conduit_getSlope in link.c)
    // -------------------------------------------------------------------------
    for (int j = 0; j < n_links; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx.links.type[uj] != LinkType::CONDUIT) continue;

        int n1 = ctx.links.node1[uj];
        int n2 = ctx.links.node2[uj];
        if (n1 < 0 || n2 < 0) continue;

        const int cr = ctx.link_subtypes.conduit_row(j);  // ≥0 (CONDUIT)
        const auto ucr = static_cast<std::size_t>(cr);
        double length = (cr >= 0) ? ctx.link_subtypes.conduits.length[ucr] : 0.0;
        if (length <= 0.0) continue;

        // Convert elevation offsets if ELEV_OFFSET mode
        if (ctx.options.link_offsets == 1) { // ELEV_OFFSET
            double raw1 = ctx.links.offset1[uj] - ctx.nodes.invert_elev[n1];
            double raw2 = ctx.links.offset2[uj] - ctx.nodes.invert_elev[n2];
            if (raw1 < 0.0)
                ctx.warnings.push_back(format_warning(WARN_NEGATIVE_OFFSET, ctx.link_names.name_of(j)));
            if (raw2 < 0.0)
                ctx.warnings.push_back(format_warning(WARN_NEGATIVE_OFFSET, ctx.link_names.name_of(j)));
            ctx.links.offset1[uj] = std::max(0.0, raw1);
            ctx.links.offset2[uj] = std::max(0.0, raw2);
        }

        double elev1 = ctx.links.offset1[uj] + ctx.nodes.invert_elev[n1];
        double elev2 = ctx.links.offset2[uj] + ctx.nodes.invert_elev[n2];
        double delta = std::fabs(elev1 - elev2);

        // Enforce minimum elevation change (matches legacy WARNING 04)
        if (delta < MIN_DELTA_Z) {
            delta = MIN_DELTA_Z;
            ctx.warnings.push_back(format_warning(WARN_MIN_ELEV_DROP, ctx.link_names.name_of(j)));
        }

        double slope;
        if (delta >= length) {
            // Elevation drop meets or exceeds the conduit length (legacy WARNING 08)
            slope = delta / length;
            ctx.warnings.push_back(
                format_warning(WARN_ELEV_DROP_EXCEEDS, ctx.link_names.name_of(j)));
        } else {
            // slope = elev drop / horizontal distance
            slope = delta / std::sqrt(length * length - delta * delta);
        }

        // Apply minimum slope (legacy WARNING 05)
        if (ctx.options.min_slope > 0.0 && slope < ctx.options.min_slope) {
            slope = ctx.options.min_slope;
            ctx.warnings.push_back(
                format_warning(WARN_MIN_SLOPE, ctx.link_names.name_of(j)));
        }

        // Negative slope for adverse gradient
        if (elev1 < elev2) slope = -slope;

        if (cr >= 0) ctx.link_subtypes.conduits.slope[ucr] = slope;

        // Reverse conduit orientation for adverse slopes in DW routing
        // (matching legacy conduit_reverse in link.c). A GeoPackage already
        // stores the model POST-reverse — its adverse conduits were flipped to a
        // positive slope at the original parse, so this never fires for a gpkg
        // load; the reversed `direction` is restored from the persisted column
        // in read_links instead (it is not derivable from the now-positive slope).
        // ==DYNWAVE audit: FV handles adverse slopes natively and does NOT
        // need this reversal — but it is kept so the same .inp preprocesses
        // identically under both models and the virtual-junction pair tables
        // stay orientation-consistent between them.
        if ((ctx.options.routing_model == RoutingModel::DYNWAVE ||
             ctx.options.routing_model == RoutingModel::FV) &&
            slope < 0.0 &&
            ctx.links.xsect_shape[uj] != XsectShape::DUMMY) {

            std::swap(ctx.links.node1[uj], ctx.links.node2[uj]);
            std::swap(ctx.links.offset1[uj], ctx.links.offset2[uj]);
            ctx.links.direction[uj] *= -1;
            ctx.links.q0[uj] = -ctx.links.q0[uj];
            if (cr >= 0) {
                std::swap(ctx.link_subtypes.conduits.loss_inlet[ucr],
                          ctx.link_subtypes.conduits.loss_outlet[ucr]);
                ctx.link_subtypes.conduits.slope[ucr] = -slope;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Conduit flow properties (matches legacy conduit_validate in link.c)
    // -------------------------------------------------------------------------
    for (int j = 0; j < n_links; ++j) {
        recompute_conduit_flow_properties(ctx, j);
    }

    // -------------------------------------------------------------------------
    // Virtual junction validation + derived geometry (after slope computation
    // and adverse-slope reversal so orientation is final)
    // -------------------------------------------------------------------------
    validate_virtual_junctions(ctx);

    // -------------------------------------------------------------------------
    // Node fullDepth adjustment from connected link crowns
    // (matches legacy link_validate → node fullDepth adjustment)
    // -------------------------------------------------------------------------
    std::vector<bool> warned_depth(static_cast<std::size_t>(n_nodes), false);
    for (int j = 0; j < n_links; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx.links.type[uj] != LinkType::CONDUIT) continue;

        double y_full = ctx.links.xsect_y_full[uj];
        int n1 = ctx.links.node1[uj];
        int n2 = ctx.links.node2[uj];

        // Upstream node: crown = offset1 + y_full (JUNCTION only, matches legacy Warning 02;
        // virtual junctions skipped — their full depth is exact by construction)
        if (n1 >= 0 && n1 < n_nodes &&
            ctx.nodes.type[static_cast<std::size_t>(n1)] == NodeType::JUNCTION &&
            !ctx.nodes.is_virtual[static_cast<std::size_t>(n1)]) {
            double crown = ctx.links.offset1[uj] + y_full;
            if (crown > ctx.nodes.full_depth[n1]) {
                ctx.nodes.full_depth[n1] = crown;
                if (!warned_depth[static_cast<std::size_t>(n1)]) {
                    ctx.warnings.push_back(format_warning(WARN_MAX_DEPTH_INCREASED, ctx.node_names.name_of(n1)));
                    warned_depth[static_cast<std::size_t>(n1)] = true;
                }
            }
        }
        // Downstream node: crown = offset2 + y_full (JUNCTION only, matches legacy Warning 02;
        // virtual junctions skipped — their full depth is exact by construction)
        if (n2 >= 0 && n2 < n_nodes &&
            ctx.nodes.type[static_cast<std::size_t>(n2)] == NodeType::JUNCTION &&
            !ctx.nodes.is_virtual[static_cast<std::size_t>(n2)]) {
            double crown = ctx.links.offset2[uj] + y_full;
            if (crown > ctx.nodes.full_depth[n2]) {
                ctx.nodes.full_depth[n2] = crown;
                if (!warned_depth[static_cast<std::size_t>(n2)]) {
                    ctx.warnings.push_back(format_warning(WARN_MAX_DEPTH_INCREASED, ctx.node_names.name_of(n2)));
                    warned_depth[static_cast<std::size_t>(n2)] = true;
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // Report flag propagation (matches legacy project_validate project.c:260-266)
    // -------------------------------------------------------------------------
    // Propagate rpt_subcatchments / rpt_nodes / rpt_links from SimulationOptions
    // to per-object rpt_flag vectors.  ALL → every object; SOME → named objects;
    // NONE → no objects.
    {
        auto& opts = ctx.options;

        // Subcatchments
        if (opts.rpt_subcatchments == 1) { // ALL
            std::fill(ctx.subcatches.rpt_flag.begin(),
                      ctx.subcatches.rpt_flag.end(), 1);
        } else if (opts.rpt_subcatchments == 2) { // SOME
            for (const auto& name : opts.rpt_subcatch_names) {
                int idx = ctx.subcatch_names.find(name);
                if (idx >= 0)
                    ctx.subcatches.rpt_flag[static_cast<std::size_t>(idx)] = 1;
            }
        }
        // NONE (0): leave all zeros

        // Nodes
        if (opts.rpt_nodes == 1) { // ALL
            std::fill(ctx.nodes.rpt_flag.begin(),
                      ctx.nodes.rpt_flag.end(), 1);
        } else if (opts.rpt_nodes == 2) { // SOME
            for (const auto& name : opts.rpt_node_names) {
                int idx = ctx.node_names.find(name);
                if (idx >= 0)
                    ctx.nodes.rpt_flag[static_cast<std::size_t>(idx)] = 1;
            }
        }

        // Links
        if (opts.rpt_links == 1) { // ALL
            std::fill(ctx.links.rpt_flag.begin(),
                      ctx.links.rpt_flag.end(), 1);
        } else if (opts.rpt_links == 2) { // SOME
            for (const auto& name : opts.rpt_link_names) {
                int idx = ctx.link_names.find(name);
                if (idx >= 0)
                    ctx.links.rpt_flag[static_cast<std::size_t>(idx)] = 1;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Auto-set "ignore process" flags for absent object classes, matching the
    // legacy project_validate() (project.c:221-222). With no aquifers there is
    // no groundwater to compute; with no snowpacks there is no snowmelt. This
    // couples the process flags to model content so the runtime guards and the
    // report process-model echo behave exactly like the legacy engine.
    // (FLOW_ROUTING NONE => ignore_routing is handled at parse time in
    // OptionsHandler; IGNORE_RAINFALL => no RDII is realized at runtime.)
    // -------------------------------------------------------------------------
    if (ctx.aquifers.count() == 0)  ctx.options.ignore_groundwater = true; // project.c:222
    if (ctx.snowpacks.count() == 0) ctx.options.ignore_snow_melt   = true; // project.c:221

    // -------------------------------------------------------------------------
    // Release excess vector capacity accumulated during parsing
    // -------------------------------------------------------------------------
    ctx.shrink_all_to_fit();

    // Relational refactor (Phase 4): the side-table rows were populated directly
    // by the parse/resolution writers above; this only re-derives the base→row
    // reverse map and sizes it to the final node count (a consistency pass after
    // any in-parse re-types). No build-from-wide.
    ctx.node_subtypes.rebuild_index(ctx.nodes.count());
}

} /* namespace openswmm::input */
