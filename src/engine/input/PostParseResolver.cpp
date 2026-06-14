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

    // Project rain depth units: SI (mm) for CMS/LPS/MLD, US (in) otherwise.
    const bool project_si = static_cast<int>(ctx.options.flow_units) >= 3;

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
            if (!fp) continue; // legacy reports ERROR 361; mirror the TS loader
        }

        // File depths → project rain units.  File is MM when rain_units==1.
        const bool file_mm = ctx.gages.rain_units[ug] == 1;
        const double units_factor = file_mm ? (project_si ? 1.0 : 1.0 / 25.4)
                                            : (project_si ? 25.4 : 1.0);

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
        double val;
        while (std::fgets(line, sizeof(line), fp)) {
            if (line[0] == ';' || line[0] == '\n' || line[0] == '\r') continue;
            if (std::sscanf(line, "%63s %d %d %d %d %d %lf",
                            tok, &yr, &mo, &dy, &hr, &mn, &val) != 7) continue;
            if (!sta.empty() && sta != tok) continue;

            const double dt = datetime::encodeDate(yr, mo, dy)
                            + datetime::encodeTime(hr, mn, 0);

            // Whole-file statistics for the report summary.
            if (first_date == 0.0 || dt < first_date) first_date = dt;
            if (dt > last_date) last_date = dt;
            if (val > 0.0) ++periods_precip;

            // Retain only the records needed to route the simulation window.
            if (dt < win_lo || dt > win_hi) continue;
            series.x.push_back(dt);
            series.y.push_back(val * units_factor);
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

    // Signal XSectParams-cache holders (e.g. SWMMEngine's reporting cache)
    // that this link's cross-section-derived fields are changing.
    ++ctx.xsect_generation;

    double n_val    = ctx.links.roughness[uj];
    double slope    = std::fabs(ctx.links.slope[uj]);
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
    bool is_dw = (ctx.options.routing_model == RoutingModel::DYNWAVE);
    if (is_dw && is_force_main) {
        auto fm = static_cast<forcemain::FrictionModel>(ctx.options.force_main_eqn);
        double r_bot  = ctx.links.xsect_r_bot[uj];
        double y_full = ctx.links.xsect_y_full[uj];
        n_val = forcemain::getEquivN(fm, r_bot, y_full, slope, n_val);
        if (n_val <= 0.0) n_val = ctx.links.roughness[uj];
    }

    // Roughness factor for DW friction slope: GRAVITY * (n/PHI)^2
    ctx.links.rough_factor[uj] = GRAVITY * (n_val / PHI) * (n_val / PHI);

    // Conveyance factor: beta = PHI * sqrt(|slope|) / n
    double beta = PHI * std::sqrt(slope) / n_val;
    ctx.links.beta[uj] = beta;

    // Full-flow rate: q_full = sFull * beta
    ctx.links.q_full[uj] = s_full * beta;

    // Max flow at max section factor
    ctx.links.q_max[uj] = s_max * beta;

    // Conduit volume = A_full * modLength (or length if no lengthening)
    double mod_len = ctx.links.mod_length[uj];
    if (mod_len <= 0.0) mod_len = ctx.links.length[uj];
    ctx.links.mod_length[uj] = mod_len;
    ctx.links.volume[uj] = a_full * mod_len;

    // For DW force mains, store roughness factor in xsect_s_bot (Gap #22)
    // Matches legacy link.c lines 1127-1130:
    //   Link[j].xsect.sBot = forcemain_getRoughFactor(j, lengthFactor)
    // The lengthFactor = mod_length / length (1.0 if not lengthened).
    if (is_dw && is_force_main) {
        double length_factor = (ctx.links.length[uj] > 0.0)
            ? mod_len / ctx.links.length[uj] : 1.0;
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
    const double inv_len = ucf::Ucf_inv[ucf::LENGTH][usz];
    if (inv_len == 1.0) return;  // US units: input already in internal units.

    const double inv_area = inv_len * inv_len;
    const double inv_flow = ucf::Qcf_inv[static_cast<std::size_t>(ctx.options.flow_units)];
    const double inv_rain = ucf::Ucf_inv[ucf::RAINFALL][usz];

    // --- Nodes: elevations/depths → ft, ponded area → ft², stage/cutoff ---
    for (int i = 0; i < n_nodes; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        ctx.nodes.invert_elev[ui] *= inv_len;
        ctx.nodes.full_depth[ui]  *= inv_len;
        ctx.nodes.init_depth[ui]  *= inv_len;
        ctx.nodes.sur_depth[ui]   *= inv_len;
        ctx.nodes.ponded_area[ui] *= inv_area;
        if (ctx.nodes.type[ui] == NodeType::OUTFALL &&
            ctx.nodes.outfall_type[ui] == OutfallType::FIXED)
            ctx.nodes.outfall_param[ui] *= inv_len;
        if (ctx.nodes.type[ui] == NodeType::DIVIDER)
            ctx.nodes.divider_cutoff[ui] *= inv_flow;
        // Functional storage A(d) = a0 + a1·d^a2 (a2 dimensionless).  a0 is an
        // area (inv_area); a1 must keep A in ft² when d is in ft, i.e. scale by
        // inv_len^(2 - a2).  Tabulated storage (storage_curve >= 0) self-converts
        // at lookup, so leave it.
        if (ctx.nodes.type[ui] == NodeType::STORAGE &&
            ctx.nodes.storage_curve[ui] < 0) {
            ctx.nodes.storage_c[ui] *= inv_area;                                  // a0
            ctx.nodes.storage_a[ui] *= std::pow(inv_len, 2.0 - ctx.nodes.storage_b[ui]); // a1
        }
    }

    // --- Links: cross-section geometry, length, offsets, flow limits ---
    for (int j = 0; j < n_links; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        const XsectShape shp = ctx.links.xsect_shape[uj];
        // geom1 (full depth/diameter) — always a length, every shape.
        ctx.links.xsect_y_full[uj] *= inv_len;
        // geom2 (width) — a length for every shape EXCEPT FORCE_MAIN, whose
        // geom2 is the Hazen-Williams C-factor / Darcy-Weisbach roughness
        // height (saved to r_bot in the xsect loop). For shapes that derive
        // w_max from y_full the input is overwritten, so converting is harmless.
        if (shp != XsectShape::FORCE_MAIN)
            ctx.links.xsect_w_max[uj] *= inv_len;
        // geom3 (y_bot/r_bot) — a length only for these three shapes; for
        // RECT_OPEN it is a sides flag and for TRAPEZOIDAL/TRIANGULAR/POWER a
        // side slope/exponent (dimensionless) — leave those.
        if (shp == XsectShape::RECT_TRIANG ||
            shp == XsectShape::RECT_ROUND ||
            shp == XsectShape::MODBASKETHANDLE)
            ctx.links.xsect_y_bot[uj] *= inv_len;

        if (ctx.links.type[uj] == LinkType::CONDUIT) {
            if (ctx.links.length[uj] > 0.0) ctx.links.length[uj] *= inv_len;
            ctx.links.q0[uj]        *= inv_flow;
            ctx.links.q_limit[uj]   *= inv_flow;
            ctx.links.seep_rate[uj] *= inv_rain;   // legacy /UCF(RAINFALL)
        }
        // Offsets / crest heights are length-dimension in both DEPTH and ELEV
        // modes; the later ELEV→DEPTH subtraction stays consistent because the
        // node inverts above are converted too.
        ctx.links.offset1[uj]      *= inv_len;
        ctx.links.offset2[uj]      *= inv_len;
        ctx.links.crest_height[uj] *= inv_len;
    }

    // --- Subcatchments: width → ft.  Area/infiltration/depression storage are
    //     converted at init/at-use and intentionally excluded here. ---
    for (int s = 0; s < n_subcatch; ++s)
        ctx.subcatches.width[static_cast<std::size_t>(s)] *= inv_len;
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
        if (ctx.nodes.type[ui] == NodeType::OUTFALL &&
            ctx.nodes.outfall_type[ui] == OutfallType::FIXED)
            ctx.nodes.outfall_param[ui] *= len;
        if (ctx.nodes.type[ui] == NodeType::DIVIDER)
            ctx.nodes.divider_cutoff[ui] *= flow;
        // Functional storage A(d) = a0 + a1·d^a2: a0 (storage_c) is an area;
        // a1 (storage_a) scales by len^(2 - a2) so A stays ft²-consistent.
        // a2 (storage_b) is dimensionless and unchanged in both directions.
        if (ctx.nodes.type[ui] == NodeType::STORAGE &&
            ctx.nodes.storage_curve[ui] < 0) {
            ctx.nodes.storage_c[ui] *= area;
            ctx.nodes.storage_a[ui] *= std::pow(len, 2.0 - ctx.nodes.storage_b[ui]);
        }
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
            if (ctx.links.length[uj] > 0.0) ctx.links.length[uj] *= len;
            ctx.links.q0[uj]        *= flow;
            ctx.links.q_limit[uj]   *= flow;
            ctx.links.seep_rate[uj] *= rain;
        }
        ctx.links.offset1[uj]      *= len;
        ctx.links.offset2[uj]      *= len;
        ctx.links.crest_height[uj] *= len;
    }

    // --- Subcatchments ---
    for (int s = 0; s < n_subcatch; ++s)
        ctx.subcatches.width[static_cast<std::size_t>(s)] *= len;
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
        ctx.subcatches.resize_quality(n_polluts);
        ctx.nodes.resize_loads(n_polluts);
        ctx.links.resize_loads(n_polluts);
    }

    // -------------------------------------------------------------------------
    // Display → internal (feet) unit conversion — must run before any derived
    // geometry/head/conveyance computation below.
    // -------------------------------------------------------------------------
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
            ctx.gages.ts_index[ug] = ctx.table_names.find(ctx.gages.ts_name[ug]);
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
    // If SUBCATCHMENTS parsed before RAINGAGES, gage indices may be -1.
    // The handler stored the name in the gage field via gage_names.find().
    // No name stored — just re-validate indices are in range.
    for (int s = 0; s < n_subcatch; ++s) {
        auto us = static_cast<std::size_t>(s);
        int gi = ctx.subcatches.gage[us];
        if (gi < 0 || gi >= n_gages) {
            ctx.subcatches.gage[us] = -1;
        }
    }

    // -------------------------------------------------------------------------
    // Storage curve name resolution
    // -------------------------------------------------------------------------
    for (int i = 0; i < n_nodes; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (ctx.nodes.type[ui] == NodeType::STORAGE &&
            ctx.nodes.storage_curve[ui] < 0 &&
            !ctx.nodes.storage_curve_name[ui].empty()) {
            ctx.nodes.storage_curve[ui] = ctx.table_names.find(ctx.nodes.storage_curve_name[ui]);
        }
    }

    // -------------------------------------------------------------------------
    // Pump curve name resolution
    // -------------------------------------------------------------------------
    for (int j = 0; j < n_links; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx.links.type[uj] != LinkType::PUMP) continue;
        if (ctx.links.pump_curve[uj] >= 0) continue; // already resolved
        if (ctx.links.pump_curve_name[uj].empty()) continue;
        ctx.links.pump_curve[uj] = ctx.table_names.find(ctx.links.pump_curve_name[uj]);
    }

    // Outlet (TABULAR) rating curve name resolution — curve name stored in
    // pump_curve_name; resolved index stored in pump_curve (mutual exclusion).
    for (int j = 0; j < n_links; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx.links.type[uj] != LinkType::OUTLET) continue;
        int outlet_type = static_cast<int>(ctx.links.param1[uj]);
        if (outlet_type < 2) continue; // FUNCTIONAL — no curve needed
        if (ctx.links.pump_curve[uj] >= 0) continue; // already resolved
        if (ctx.links.pump_curve_name[uj].empty()) continue;
        ctx.links.pump_curve[uj] = ctx.table_names.find(ctx.links.pump_curve_name[uj]);
    }

    // Convert pump startup/shutoff depths from display units → internal (ft)
    {
        int us = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
        double ucf_len = ucf::Ucf[ucf::LENGTH][us];
        for (int j = 0; j < n_links; ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx.links.type[uj] != LinkType::PUMP) continue;
            if (ctx.links.pump_startup[uj] > 0.0)
                ctx.links.pump_startup[uj] /= ucf_len;
            if (ctx.links.pump_shutoff[uj] > 0.0)
                ctx.links.pump_shutoff[uj] /= ucf_len;
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
            int ts_idx = ctx.table_names.find(ctx.ext_inflows.ts_name[ui]);
            // Store resolved index - the inflow solver uses ts_name for lookup,
            // but we can cache the index for performance
            (void)ts_idx; // ts_name is used directly by InflowSolver
        }
        // Re-resolve node index if it was -1 during parsing
        if (ctx.ext_inflows.node_idx[ui] < 0) {
            // Can't resolve without stored node name — skip
        }
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
        auto ui = static_cast<std::size_t>(i);
        if (ctx.nodes.type[ui] != NodeType::DIVIDER) continue;

        // Re-resolve diversion link name → index
        if (ctx.nodes.divider_link[ui] < 0 &&
            !ctx.nodes.divider_link_name[ui].empty()) {
            ctx.nodes.divider_link[ui] = ctx.link_names.find(ctx.nodes.divider_link_name[ui]);
        }

        // Re-resolve diversion curve name → index (TABULAR dividers)
        if (ctx.nodes.divider_curve[ui] < 0 &&
            !ctx.nodes.divider_curve_name[ui].empty()) {
            ctx.nodes.divider_curve[ui] = ctx.table_names.find(ctx.nodes.divider_curve_name[ui]);
        }
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
        int ts_idx = ctx.table_names.find(ctx.options.evap_ts_name);
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
                // WARNING 10: if orifice invert (absolute) < downstream node invert
                if (n1 >= 0 && n1 < n_nodes && n2 >= 0 && n2 < n_nodes) {
                    double inv1 = ctx.nodes.invert_elev[static_cast<std::size_t>(n1)];
                    double inv2 = ctx.nodes.invert_elev[static_cast<std::size_t>(n2)];
                    double ori_abs = ctx.links.offset1[uj] + inv1; // after conversion
                    if (ori_abs < inv2) {
                        ctx.links.offset1[uj] = std::max(0.0, inv2 - inv1);
                        ctx.warnings.push_back(format_warning(WARN_REGULATOR_CREST_LOW, ctx.link_names.name_of(j)));
                    }
                }
            } else if (lt == LinkType::WEIR || lt == LinkType::OUTLET) {
                // [WEIRS]/[OUTLETS]: crest_height = absolute crest elevation → convert to depth above n1
                // offset1/offset2 are not explicit input fields for weirs/outlets, keep at 0
                if (n1 >= 0 && n1 < n_nodes) {
                    double rawCrest = ctx.links.crest_height[uj] - ctx.nodes.invert_elev[static_cast<std::size_t>(n1)];
                    ctx.links.crest_height[uj] = std::max(0.0, rawCrest);
                }
                // WARNING 10: if crest (absolute) < downstream node invert
                if (n1 >= 0 && n1 < n_nodes && n2 >= 0 && n2 < n_nodes) {
                    double crest_abs = ctx.links.crest_height[uj] + ctx.nodes.invert_elev[static_cast<std::size_t>(n1)];
                    double inv2     = ctx.nodes.invert_elev[static_cast<std::size_t>(n2)];
                    if (crest_abs < inv2) {
                        ctx.links.crest_height[uj] = std::max(0.0, inv2 - ctx.nodes.invert_elev[static_cast<std::size_t>(n1)]);
                        ctx.warnings.push_back(format_warning(WARN_REGULATOR_CREST_LOW, ctx.link_names.name_of(j)));
                    }
                }
            }
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
            td.x_left_bank  = ctx.transects.x_left_bank[ut];
            td.x_right_bank = ctx.transects.x_right_bank[ut];
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
                    if (ctx.transects.names[static_cast<std::size_t>(t)] == tname) {
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
            int ci = ctx.table_names.find(cname);
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
                if (ctx.streets.names[static_cast<std::size_t>(s)] == sname) { si = s; break; }
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
            // Properties already set from CUSTOM shape curve above
            a_full = ctx.links.xsect_a_full[uj];
            r_full = ctx.links.xsect_r_full[uj];
            w_max  = ctx.links.xsect_w_max[uj];
            s_full = a_full * std::pow(std::max(r_full, 1e-10), 2.0/3.0);
            s_max  = s_full;
            yw_max = y_full;
            break;
        }

        case XsectShape::IRREGULAR:
        case XsectShape::STREET_XSECT: {
            // Properties already set from transect / street tables above.
            int ci = ctx.links.xsect_curve[uj];
            if (ci >= 0 && static_cast<std::size_t>(ci) < ctx.transect_tables.size()) {
                const auto& td = ctx.transect_tables[static_cast<std::size_t>(ci)];
                a_full = td.a_full;
                r_full = td.r_full;
                w_max  = td.w_max;
                y_full = td.y_full;
                s_full = a_full * std::pow(r_full, 2.0/3.0);
                s_max  = s_full;
                yw_max = y_full;
            } else {
                // Fallback if transect/street not resolved
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

        double length = ctx.links.length[uj];
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
            slope = delta / length;
        } else {
            // slope = elev drop / horizontal distance
            slope = delta / std::sqrt(length * length - delta * delta);
        }

        // Apply minimum slope
        if (ctx.options.min_slope > 0.0 && slope < ctx.options.min_slope) {
            slope = ctx.options.min_slope;
        }

        // Negative slope for adverse gradient
        if (elev1 < elev2) slope = -slope;

        ctx.links.slope[uj] = slope;

        // Reverse conduit orientation for adverse slopes in DW routing
        // (matching legacy conduit_reverse in link.c)
        if (ctx.options.routing_model == RoutingModel::DYNWAVE &&
            slope < 0.0 &&
            ctx.links.xsect_shape[uj] != XsectShape::DUMMY) {

            std::swap(ctx.links.node1[uj], ctx.links.node2[uj]);
            std::swap(ctx.links.offset1[uj], ctx.links.offset2[uj]);
            std::swap(ctx.links.loss_inlet[uj], ctx.links.loss_outlet[uj]);
            ctx.links.slope[uj] = -slope;
            ctx.links.direction[uj] *= -1;
            ctx.links.q0[uj] = -ctx.links.q0[uj];
        }
    }

    // -------------------------------------------------------------------------
    // Conduit flow properties (matches legacy conduit_validate in link.c)
    // -------------------------------------------------------------------------
    for (int j = 0; j < n_links; ++j) {
        recompute_conduit_flow_properties(ctx, j);
    }

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

        // Upstream node: crown = offset1 + y_full (JUNCTION only, matches legacy Warning 02)
        if (n1 >= 0 && n1 < n_nodes &&
            ctx.nodes.type[static_cast<std::size_t>(n1)] == NodeType::JUNCTION) {
            double crown = ctx.links.offset1[uj] + y_full;
            if (crown > ctx.nodes.full_depth[n1]) {
                ctx.nodes.full_depth[n1] = crown;
                if (!warned_depth[static_cast<std::size_t>(n1)]) {
                    ctx.warnings.push_back(format_warning(WARN_MAX_DEPTH_INCREASED, ctx.node_names.name_of(n1)));
                    warned_depth[static_cast<std::size_t>(n1)] = true;
                }
            }
        }
        // Downstream node: crown = offset2 + y_full (JUNCTION only, matches legacy Warning 02)
        if (n2 >= 0 && n2 < n_nodes &&
            ctx.nodes.type[static_cast<std::size_t>(n2)] == NodeType::JUNCTION) {
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
    // Release excess vector capacity accumulated during parsing
    // -------------------------------------------------------------------------
    ctx.shrink_all_to_fit();
}

} /* namespace openswmm::input */
