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
 * @file PostParseResolver.cpp
 * @brief Post-parse cross-reference resolution.
 * @see PostParseResolver.hpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "PostParseResolver.hpp"

#include <array>
#include "MultiColumnSeriesFile.hpp"
#include "../core/Constants.hpp"
#include "../core/ErrorCodes.hpp"
#include "../core/PathResolver.hpp"
#include "../core/PerfTimers.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/DateTime.hpp"
#include "../core/UnitConversion.hpp"
#include "../hydraulics/xsect_tables.hpp"
#include "../hydraulics/XSectBatch.hpp"
#include "../hydraulics/Link.hpp"
#include "../hydraulics/Street.hpp"
#include "../hydraulics/ForceMain.hpp"
#include "../edit/VirtualJunctionOps.hpp"

#ifdef OPENSWMM_HAS_2D
// SolverOptions2D is only forward-declared in SimulationContext.hpp; the full
// definition is needed to resolve its mesh_file / output_file slots.
#include "../2d/data/SolverOptions2D.hpp"
#endif
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <unordered_map>
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
//
// Multi-column files (CSV/TSV/TSF, optionally referenced as "path:column")
// are routed through the shared MultiColumnSeriesFile parse-once cache
// instead — see load_external_timeseries_files below.
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

    for (auto& rpt : ctx.lid_usage.rpt_file) resolve(rpt);

    // 2D slots live behind a non-owning pointer wired by SWMMEngine's
    // constructor, so it is non-null well before parsing — but a
    // SimulationContext built standalone (unit tests, programmatic models)
    // leaves it null. Without these two the external mesh reference could not
    // be re-anchored on Save-As: the writer only ever saw a bare relative
    // token with no way to tell what it was relative TO.
#ifdef OPENSWMM_HAS_2D
    if (ctx.twod_io.options) {
        resolve(ctx.twod_io.options->mesh_file);
        resolve(ctx.twod_io.options->output_file);
    }
#endif
}

static void load_external_timeseries_files(SimulationContext& ctx, const std::string& inp_dir,
                                           MultiColumnFileCache& file_cache) {
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
        std::string file_path;
        if (!tbl.file_path.absolute.empty()) {
            file_path = tbl.file_path.absolute;  // already anchored to the .inp dir
        } else {
            file_path = tbl.file_path.str();
            // Resolve relative paths against INP file directory (legacy
            // fallback path — kept so a fresh programmatic Table still loads
            // even without the resolver pass). Applies only to the verbatim
            // token: .absolute is already anchored, and prepending inp_dir a
            // second time broke cwd-relative opens.
            if (!file_path.empty() && file_path[0] != '/' && file_path[0] != '\\') {
                if (!inp_dir.empty())
                    file_path = inp_dir + "/" + file_path;
            }
        }

        // Split the optional :column suffix (e.g. "path.csv:ColName") with the
        // SHARED rule the gage reader uses (MultiColumnSeriesFile.hpp): last
        // colon, ignoring a drive letter and any colon that belongs to a
        // directory name. Both consumers must derive the same path or they key
        // the cache differently and the file is read twice.
        std::string col_name;
        {
            std::string path_only;
            split_series_file_token(file_path, path_only, col_name);
            file_path = path_only;
        }

        // Multi-column route: an explicit `:column` selector, or a file
        // whose first content line reads as a header (CSV/TSV/TSF), goes
        // through the shared parse-once cache so a file referenced by many
        // series — and by rain gages — is read from disk exactly once per
        // resolve pass. Plain `date time value` files keep the legacy
        // whitespace path below.
        if (!col_name.empty() || looks_like_multicolumn_series_file(file_path)) {
            std::vector<std::string> file_errors;
            SeriesFileStatus st = SeriesFileStatus::OK;
            const ParsedSeriesFile* pf =
                file_cache.get_or_parse(file_path, file_errors, &st);
            if (!pf && st == SeriesFileStatus::OPEN_FAILED) {
                // Same final fallback the legacy fopen chain had: the
                // verbatim token (minus any :column suffix) relative to the
                // current working directory.
                std::string verbatim, vcol;
                split_series_file_token(tbl.file_path.str(), verbatim, vcol);
                if (verbatim != file_path)
                    pf = file_cache.get_or_parse(verbatim, file_errors, &st);
            }
            if (!pf) {
                // Loud, not silent: an unreadable FILE series previously
                // loaded as empty and read 0.0 at every lookup.
                ctx.errors.push_back(format_error(
                    st == SeriesFileStatus::OPEN_FAILED
                        ? openswmm::ERR_TABLE_FILE_OPEN
                        : openswmm::ERR_TABLE_FILE_READ,
                    tbl.id));
                continue;
            }
            const int col = col_name.empty() ? pf->first_data_column()
                                             : pf->find_column(col_name);
            if (col < 0) {
                ctx.errors.push_back(format_error(
                    openswmm::ERR_TABLE_FILE_READ, tbl.id,
                    "column \"" + col_name + "\" not found in " + file_path));
                continue;
            }
            const auto& vals = pf->columns[static_cast<std::size_t>(col)];
            tbl.x.reserve(pf->dates.size());
            tbl.y.reserve(pf->dates.size());
            for (std::size_t i = 0; i < pf->dates.size(); ++i) {
                if (std::isnan(vals[i])) continue;  // missing/unreadable cell
                tbl.x.push_back(pf->dates[i]);
                tbl.y.push_back(vals[i]);
            }
            if (tbl.x.empty()) {
                ctx.errors.push_back(format_error(
                    openswmm::ERR_TABLE_FILE_READ, tbl.id, file_path));
                continue;
            }
            tbl.x.shrink_to_fit();
            tbl.y.shrink_to_fit();
            // file_path is intentionally retained (see the note at the end
            // of the legacy path below).
            continue;
        }

        // Open the file
        FILE* fp = std::fopen(file_path.c_str(), "r");
        if (!fp) {
            // Try the verbatim token as a final fallback (covers absolute
            // paths and same-cwd cases when inp_dir was empty).
            fp = std::fopen(tbl.file_path.c_str(), "r");
            if (!fp) {
                // Was a silent skip; legacy reports ERROR 361 and fails the
                // open, so match it — an unloadable series otherwise reads
                // as 0.0 everywhere with a clean-looking report.
                ctx.errors.push_back(
                    format_error(openswmm::ERR_TABLE_FILE_OPEN, tbl.id));
                continue;
            }
        }

        // Reserve from the file's actual size rather than a flat 100k rows.
        // The old constant committed 1.6 MB per FILE-backed series before
        // reading a byte — on a model with hundreds of small rain files that
        // is hundreds of megabytes of untouched pages, and on a genuinely
        // large file it was too small anyway. ~24 bytes per "date time value"
        // row is a deliberate under-estimate: geometric growth handles the
        // remainder, whereas over-reserving cannot be given back.
        {
            std::error_code ec;
            const auto bytes = std::filesystem::file_size(file_path, ec);
            std::size_t rows = ec ? std::size_t{1024}
                                  : static_cast<std::size_t>(bytes) / 24u + 16u;
            rows = std::min<std::size_t>(rows, 2000000u);
            tbl.x.reserve(rows);
            tbl.y.reserve(rows);
        }

        // Row grammar — mirror legacy table_parseFileLine() (table.c:838).
        // Tokens split on space/tab/CR/LF/comma (legacy TBLSEPSTR); a first
        // token starting with ';' is a comment. A row is either
        //   date time value   — date M/D/Y with '/' or '-' separators,
        //                       numeric or 3-letter month name (DateFormat
        //                       is pinned M_D_Y at open, legacy swmm5.c:647)
        //   time value        — date carried from the last dated row
        // and the time token is decimal HOURS when it is entirely numeric,
        // else H:MM[:SS] (legacy datetime_strToTime). Rows before the first
        // dated row are elapsed times anchored at the simulation START
        // DATETIME: legacy input.c:176 seeds every series' lastDate with
        // StartDate + StartTime, and options are already parsed when this
        // loader runs, so the anchor is applied directly here (the rows are
        // stored absolute; the resolver's inline relative-row offset pass
        // does not apply to them). The previous parser accepted ONLY
        // "M/D/Y H:MM value" rows, so every elapsed-time or decimal-hour
        // legacy file loaded zero rows and failed the open with ERROR 363.
        double last_date = ctx.options.start_date; // date carried across rows

        auto next_tok = [](char*& p) -> char* {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
                   *p == ',') ++p;
            if (*p == '\0') return nullptr;
            char* start = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' &&
                   *p != '\r' && *p != ',') ++p;
            if (*p) { *p = '\0'; ++p; }
            return start;
        };

        auto parse_file_date = [](const char* s, double& d_out) -> bool {
            if (!std::strchr(s, '/') && !std::strchr(s, '-')) return false;
            unsigned m = 0, d = 0, y = 0;
            char sep1 = 0, sep2 = 0;
            if (std::sscanf(s, "%u%c%u%c%u", &m, &sep1, &d, &sep2, &y) < 5) {
                char mon[4] = {};
                if (std::sscanf(s, "%3[A-Za-z]%c%u%c%u",
                                mon, &sep1, &d, &sep2, &y) < 5) return false;
                static const char* kMonths[12] = {
                    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
                m = 0;
                for (unsigned i = 0; i < 12; ++i) {
                    if (std::toupper(static_cast<unsigned char>(mon[0])) == kMonths[i][0] &&
                        std::toupper(static_cast<unsigned char>(mon[1])) == kMonths[i][1] &&
                        std::toupper(static_cast<unsigned char>(mon[2])) == kMonths[i][2]) {
                        m = i + 1;
                        break;
                    }
                }
                if (m == 0) return false;
            }
            const double enc = datetime::encodeDate(static_cast<int>(y),
                                                    static_cast<int>(m),
                                                    static_cast<int>(d));
            if (enc == -static_cast<double>(datetime::DateDelta)) return false;
            d_out = enc;
            return true;
        };

        auto parse_file_time = [](const char* s, double& t_out) -> bool {
            char* endp = nullptr;
            const double hrs = std::strtod(s, &endp);
            if (endp && *endp == '\0') {           // decimal hours
                t_out = hrs / 24.0;
                return true;
            }
            int hr = 0, min = 0, sec = 0;
            if (std::sscanf(s, "%d:%d:%d", &hr, &min, &sec) < 1) return false;
            if (hr < 0 || min < 0 || sec < 0) return false;
            t_out = datetime::encodeTime(hr, min, sec);
            return true;
        };

        char line[256];
        while (std::fgets(line, sizeof(line), fp)) {
            char* p = line;
            char* s1 = next_tok(p);
            if (!s1 || *s1 == ';') continue;       // blank line or comment
            char* s2 = next_tok(p);
            char* s3 = next_tok(p);                // extra tokens ignored

            const char* time_tok;
            const char* value_tok;
            double d = 0.0;
            if (s3) {                              // date  time  value
                if (!parse_file_date(s1, d)) continue;
                last_date = d;
                time_tok  = s2;
                value_tok = s3;
            } else if (s2) {                       // time  value
                d = last_date;
                time_tok  = s1;
                value_tok = s2;
            } else {
                continue;
            }

            double t = 0.0;
            if (!parse_file_time(time_tok, t)) continue;
            char* endp = nullptr;
            const double value = std::strtod(value_tok, &endp);
            if (endp == value_tok || *endp != '\0') continue;

            tbl.x.push_back(d + t);
            tbl.y.push_back(value);
        }
        std::fclose(fp);

        // Loud, not silent: a file that opened but yielded no parseable
        // rows (wrong delimiter, wrong format) previously produced an
        // empty series that read as 0.0 at every lookup.
        if (tbl.x.empty()) {
            ctx.errors.push_back(
                format_error(openswmm::ERR_TABLE_FILE_READ, tbl.id, file_path));
            continue;
        }

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
// -------------------------------------------------------------------------
// USER_CSV rain files (multi-column CSV / TSV / PCSWMM TSF)
// -------------------------------------------------------------------------
// `FILE "rain.csv:COLUMN"` — a header row, with the value taken from the
// column whose header matches COLUMN (empty column = first data column).
// Column 0 carries a full date-time. The delimiter and TSF header form are
// auto-detected by MultiColumnSeriesFile. Values are in the PROJECT's rain
// units and are stored verbatim: unlike the standard format there is no
// legacy interface file to be bit-compatible with, so the read side
// interprets them per the gage's declared Format exactly as it does for an
// inline [TIMESERIES] gage. See gage::gageUnitsFactor, which is scoped to
// STAN_PRCP for this reason.
// -------------------------------------------------------------------------

static void load_rain_file_user_csv(SimulationContext& ctx, int g,
                                    const std::string& path,
                                    double win_lo, double win_hi,
                                    MultiColumnFileCache& file_cache) {
    const auto ug = static_cast<std::size_t>(g);

    // Parse-once: the shared cache reads the file on first request; every
    // other gage (and FILE timeseries) on the same file copies out of the
    // same ParsedSeriesFile. CSV/TSV/TSF are auto-detected by content, so
    // USER_CSV now means "multi-column text file" generally.
    std::vector<std::string> file_errors;
    SeriesFileStatus st = SeriesFileStatus::OK;
    const ParsedSeriesFile* pf = file_cache.get_or_parse(path, file_errors, &st);
    if (!pf && st == SeriesFileStatus::OPEN_FAILED &&
        ctx.gages.file_path[ug].str() != path) {
        // Same fallback the direct fopen had: try the verbatim token.
        pf = file_cache.get_or_parse(ctx.gages.file_path[ug].str(), file_errors, &st);
    }
    if (!pf) {
        ctx.errors.push_back(format_error(
            st == SeriesFileStatus::OPEN_FAILED ? openswmm::ERR_RAIN_FILE_OPEN
                                                : openswmm::ERR_RAIN_FILE_FORMAT,
            path));
        return;
    }

    const std::string& want = ctx.gages.col_name[ug];
    // B2 fix: an empty column name selects the first data column, matching
    // the documented default (GageData.hpp col_name) instead of erroring.
    const int col = want.empty() ? pf->first_data_column()
                                 : pf->find_column(want);
    if (col < 0) {
        ctx.errors.push_back(format_error(openswmm::ERR_RAIN_FILE_FORMAT,
                                          path + " (column \"" + want + "\")"));
        return;
    }
    const auto uc = static_cast<std::size_t>(col);

    Table series;
    series.type = TableType::TIMESERIES;
    series.id   = ctx.gage_names.name_of(g);

    // Retain only the records needed to route the simulation window; the
    // cache rows are already sorted ascending, so the copy stays sorted.
    const auto& vals = pf->columns[uc];
    for (std::size_t i = 0; i < pf->dates.size(); ++i) {
        const double val = vals[i];
        if (std::isnan(val)) continue;  // missing/unreadable cell
        const double dt = pf->dates[i];
        if (dt < win_lo || dt > win_hi) continue;
        series.x.push_back(dt);
        series.y.push_back(val);
    }

    const long unparsed_rows = pf->unparsed_rows + pf->col_unparsed_cells[uc];
    if (unparsed_rows > 0) {
        // One warning per gage, not per row: a mis-specified file would
        // otherwise bury the report under thousands of identical lines.
        ctx.warnings.push_back(
            format_warning(openswmm::WARN_RAIN_CSV_ROWS_SKIPPED,
                           ctx.gage_names.name_of(g),
                           std::to_string(unparsed_rows) + " row(s), " + path));
    }

    series.x.shrink_to_fit();
    series.y.shrink_to_fit();

    // Whole-file, per-column statistics for the "Rainfall File Summary".
    ctx.gages.file_first_date[ug]     = pf->col_first_date[uc];
    ctx.gages.file_last_date[ug]      = pf->col_last_date[uc];
    ctx.gages.file_periods_precip[ug] = pf->col_periods_precip[uc];
    ctx.gages.rain_series[ug]         = std::move(series);
}

static void load_external_rain_files_impl(SimulationContext& ctx,
                                          MultiColumnFileCache& file_cache) {
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

        bool is_csv = ctx.gages.file_format[ug] == RainFileFormat::USER_CSV;
        if (!is_csv && ctx.gages.file_format[ug] != RainFileFormat::STAN_PRCP) continue;

        std::string path = !ctx.gages.file_path[ug].absolute.empty()
                             ? ctx.gages.file_path[ug].absolute
                             : ctx.gages.file_path[ug].str();

        // A USER_CSV gage whose column is empty (= "first data column") is
        // written as a bare `FILE "path"` token, because `FILE "path:"` is
        // malformed for EPA SWMM / PCSWMM. That token re-parses as STAN_PRCP,
        // so recover the format here, where the path is resolved and the file
        // can be inspected: an empty station id plus multi-column CONTENT can
        // only mean the compact form. The STAN_PRCP reader would otherwise
        // fail every sscanf and hand back a silently empty series — the exact
        // failure mode this change set exists to remove. Gated on an empty
        // station id and on content, so a whitespace station file (which the
        // sniff rejects) and any gage that names a station are untouched.
        if (!is_csv && ctx.gages.station_id[ug].empty() &&
            looks_like_multicolumn_series_file(path)) {
            ctx.gages.file_format[ug] = RainFileFormat::USER_CSV;
            is_csv = true;
        }

        if (is_csv) {
            load_rain_file_user_csv(ctx, g, path, win_lo, win_hi, file_cache);
            continue;
        }

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

void load_external_rain_files(SimulationContext& ctx) {
    // Standalone entry point (swmm_gage_reload_rain_files): a fresh cache
    // per call still guarantees one parse per unique file within the call.
    // The resolve pass instead shares one cache with the timeseries loader
    // (see resolve_cross_references).
    MultiColumnFileCache file_cache;
    load_external_rain_files_impl(ctx, file_cache);
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
    if (inv_len == 1.0) {
        // US units: lengths are already internal feet, but flow-dimension
        // inputs are in the deck's FLOW_UNITS — a GPM or MGD deck still
        // needs the Qcf division legacy applies to every parsed flow
        // (link.c:352-353 `x[4]/UCF(FLOW)`, node.c divider cutoff). Skipping
        // it left q0/q_limit/cutoff in user units: a 2.6 MGD MaxFlow acted
        // as a 2.6 cfs cap (small-orifice: pipe held to 1.68 instead of
        // 4.02 cfs and the whole storage drawdown diverged).
        const double qcf_us =
            ucf::Qcf[static_cast<std::size_t>(ctx.options.flow_units)];
        if (qcf_us != 1.0) {
            for (int i = 0; i < n_nodes; ++i) {
                if (ctx.nodes.type[static_cast<std::size_t>(i)] !=
                    NodeType::DIVIDER) continue;
                const int r = ctx.node_subtypes.divider_row(i);
                if (r >= 0)
                    ctx.node_subtypes.dividers.cutoff[
                        static_cast<std::size_t>(r)] /= qcf_us;
            }
            for (int j = 0; j < n_links; ++j) {
                const auto uj = static_cast<std::size_t>(j);
                if (ctx.links.type[uj] != LinkType::CONDUIT) continue;
                ctx.links.q0[uj]      /= qcf_us;
                ctx.links.q_limit[uj] /= qcf_us;
            }
        }
        return;
    }

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
        ctx.nodes.rim_depth[ui]   /= len;   // display-only, but still a length
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
    if (len == 1.0) {
        // US units: lengths are display-identical, but GPM/MGD flow fields
        // were divided by Qcf on the way in (see convert_inputs_to_internal)
        // and must be multiplied back for writer round-trips.
        const double qcf_us =
            ucf::Qcf[static_cast<std::size_t>(ctx.options.flow_units)];
        if (qcf_us != 1.0) {
            for (int i = 0; i < ctx.n_nodes(); ++i) {
                if (ctx.nodes.type[static_cast<std::size_t>(i)] !=
                    NodeType::DIVIDER) continue;
                const int r = ctx.node_subtypes.divider_row(i);
                if (r >= 0)
                    ctx.node_subtypes.dividers.cutoff[
                        static_cast<std::size_t>(r)] *= qcf_us;
            }
            for (int j = 0; j < ctx.n_links(); ++j) {
                const auto uj = static_cast<std::size_t>(j);
                if (ctx.links.type[uj] != LinkType::CONDUIT) continue;
                ctx.links.q0[uj]      *= qcf_us;
                ctx.links.q_limit[uj] *= qcf_us;
            }
        }
        return;
    }

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
        ctx.nodes.rim_depth[ui]   *= len;   // display-only, but still a length
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
// convert_internal_to_authored()
// ============================================================================
// resolve_cross_references applies two parse-time normalisations that mutate
// authored link data in place: (1) adverse-slope conduits are reversed under
// DYNWAVE/FV (node1/node2, offset1/offset2, q0 sign, inlet/outlet losses;
// direction = -1), and (2) in LINK_OFFSETS=ELEVATION mode every offset/crest
// is rewritten as a depth above its node invert. Neither was undone by the
// .inp writer, so Open → Save silently swapped adverse conduits and wrote
// depths under an ELEVATION header (destroying offsets on the next open).
// Legacy SWMM-GUI never hit this because it exports its own object model, not
// engine state. This is the exact inverse; call it on a COPY.
bool needs_authored_conversion(const SimulationContext& ctx) {
    if (ctx.options.link_offsets == 1) return true;
    for (int j = 0; j < ctx.n_links(); ++j)
        if (ctx.links.direction[static_cast<std::size_t>(j)] < 0) return true;
    return false;
}

// Un-reverse adverse-slope conduits. Offsets/losses travel with their node, so
// swapping both pairs keeps them aligned. Vertices were never touched by the
// reversal and stay as authored. Safe on a live editing context: the GUI runs
// simulations from a separately opened engine, never from the edit context.
int restore_authored_orientation(SimulationContext& ctx) {
    int n = 0;
    for (int j = 0; j < ctx.n_links(); ++j) {
        const auto uj = static_cast<std::size_t>(j);
        if (ctx.links.type[uj] != LinkType::CONDUIT || ctx.links.direction[uj] >= 0)
            continue;
        std::swap(ctx.links.node1[uj], ctx.links.node2[uj]);
        std::swap(ctx.links.offset1[uj], ctx.links.offset2[uj]);
        ctx.links.q0[uj] = -ctx.links.q0[uj];
        ctx.links.direction[uj] = 1;
        const int cr = ctx.link_subtypes.conduit_row(j);
        if (cr >= 0) {
            const auto ucr = static_cast<std::size_t>(cr);
            std::swap(ctx.link_subtypes.conduits.loss_inlet[ucr],
                      ctx.link_subtypes.conduits.loss_outlet[ucr]);
            ctx.link_subtypes.conduits.slope[ucr] = -ctx.link_subtypes.conduits.slope[ucr];
        }
        ++n;
    }
    return n;
}

void convert_internal_to_authored(SimulationContext& ctx) {
    const int n_links = ctx.n_links();
    const int n_nodes = ctx.n_nodes();

    // (1) Orientation.
    restore_authored_orientation(ctx);

    // (2) Depth → elevation (inverse of the two ELEV_OFFSET passes above).
    if (ctx.options.link_offsets != 1) return;
    for (int j = 0; j < n_links; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        const LinkType lt = ctx.links.type[uj];
        if (lt == LinkType::PUMP) continue;
        const int n1 = ctx.links.node1[uj];
        const int n2 = ctx.links.node2[uj];
        const bool ok1 = n1 >= 0 && n1 < n_nodes;
        const bool ok2 = n2 >= 0 && n2 < n_nodes;
        const double inv1 = ok1 ? ctx.nodes.invert_elev[static_cast<std::size_t>(n1)] : 0.0;
        const double inv2 = ok2 ? ctx.nodes.invert_elev[static_cast<std::size_t>(n2)] : 0.0;

        if (lt == LinkType::CONDUIT) {
            if (ok1) ctx.links.offset1[uj] += inv1;
            if (ok2) ctx.links.offset2[uj] += inv2;
        } else if (lt == LinkType::ORIFICE) {
            if (ok1) ctx.links.offset1[uj] += inv1;
        } else if (lt == LinkType::WEIR || lt == LinkType::OUTLET) {
            const int wr  = ctx.link_subtypes.weir_row(j);
            const int olr = (wr < 0) ? ctx.link_subtypes.outlet_row(j) : -1;
            double* crest = (wr >= 0)
                ? &ctx.link_subtypes.weirs.crest_height[static_cast<std::size_t>(wr)]
                : (olr >= 0 ? &ctx.link_subtypes.outlets.crest_height[static_cast<std::size_t>(olr)]
                            : nullptr);
            if (crest && ok1) *crest += inv1;
        }
    }
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
    const auto _pt_extfiles0 = perf::now();
    {
        // One parse-once cache shared by BOTH external-file loaders: a
        // multi-column file referenced by any number of timeseries and rain
        // gages is read from disk exactly once per resolve pass (plan
        // MULTICOLUMN_SERIES_SINGLE_READ_2026-08-17 §5). Freed at the end of
        // this scope — every consumer has copied its column into its own
        // x/y arrays by then.
        MultiColumnFileCache series_file_cache;
        load_external_timeseries_files(ctx, inp_dir, series_file_cache);

        // ---------------------------------------------------------------------
        // Load external FILE-source rain-gage data (standard SWMM rain files).
        // Must run after resolve_external_file_slots (for absolute paths) and
        // after options parsing (needs the simulation window to bound retained
        // records).
        // ---------------------------------------------------------------------
        load_external_rain_files_impl(ctx, series_file_cache);
    }
    perf::sec_res_extfiles += perf::since(_pt_extfiles0);

    // -------------------------------------------------------------------------
    // Timeseries date offset resolution
    // -------------------------------------------------------------------------
    // Rows authored without a date are elapsed times anchored at the
    // simulation start (legacy input.c:170 seeds every series' lastDate with
    // StartDate + StartTime before parsing). The parser stored those rows
    // relative to 0 and counted them in Table::n_relative; add start_date to
    // exactly those rows here so absolute OADate lookups work. Rows past
    // n_relative carry explicit dates and must NOT move — the old x[0] < 366
    // heuristic shifted a whole mixed series, pushing its dated rows ~107
    // years out. rel_anchor records the offset currently baked in, which
    // makes a re-resolve idempotent and re-anchors by the delta if
    // START_DATE was edited between resolves.
    for (std::size_t t = 0; t < ctx.tables.tables.size(); ++t) {
        auto& tbl = ctx.tables.tables[t];
        if (tbl.type != TableType::TIMESERIES) continue;
        if (tbl.n_relative <= 0) continue;

        const double start = ctx.options.start_date;
        const double delta = start - tbl.rel_anchor;
        if (delta == 0.0) continue;
        const std::size_t n = std::min(tbl.x.size(),
                                       static_cast<std::size_t>(tbl.n_relative));
        for (std::size_t k = 0; k < n; ++k) tbl.x[k] += delta;
        tbl.rel_anchor = start;
    }

    // -------------------------------------------------------------------------
    // Gage timeseries re-resolution
    // -------------------------------------------------------------------------
    // Start of the name-binding region: gage/co-gage, subcatchment outlet and
    // gage, storage and outfall and pump curves, external-inflow series. These
    // are the find_timeseries/find_curve callers.
    const auto _pt_tables0 = perf::now();
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
    // Single pass: remember the first gage seen on each timeseries and point
    // every later one at it. The inner scan this replaces was O(n_gages^2) and
    // broke at its first hit, i.e. the LOWEST-indexed earlier gage — which is
    // exactly the value first_on_ts holds, so the assignment is unchanged.
    {
        std::unordered_map<int, int> first_on_ts;   // ts index -> first gage
        first_on_ts.reserve(static_cast<std::size_t>(n_gages));
        for (int gj = 0; gj < n_gages; ++gj) {
            auto ugj = static_cast<std::size_t>(gj);
            ctx.gages.co_gage_index[ugj] = -1;
            if (ctx.gages.source[ugj] != RainSource::TIMESERIES) continue;
            const int ts_j = ctx.gages.ts_index[ugj];
            if (ts_j < 0) continue;
            const auto ins = first_on_ts.emplace(ts_j, gj);
            if (!ins.second) ctx.gages.co_gage_index[ugj] = ins.first->second;
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

        // Legacy subcatch_validate (subcatch.c:391-393): an outlet name that
        // matches BOTH a node and a subcatchment is ambiguous — ERROR 108.
        // Legacy stores the two resolutions independently and errors when
        // both landed; here the single-slot model must check explicitly.
        if (ctx.node_names.find(name) >= 0 &&
            ctx.subcatch_names.find(name) >= 0) {
            ctx.errors.push_back(format_error(
                ERR_SUBCATCH_OUTLET, ctx.subcatch_names.name_of(s)));
            continue;
        }

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
    // Subcatchment groundwater receiving-node re-resolution
    // -------------------------------------------------------------------------
    // [GROUNDWATER] normally precedes [JUNCTIONS], so handle_groundwater()
    // resolved the node against an empty name index and stored -1. The raw name
    // was captured in ctx.pending_gw_nodes for exactly this pass. A name that
    // still fails to resolve is left at -1: the runtime falls back to the
    // subcatchment outlet node (Groundwater.cpp), and the [GROUNDWATER] writer
    // skips the row rather than emitting '*'.
    for (const auto& [si, nm] : ctx.pending_gw_nodes) {
        if (si < 0 || si >= n_subcatch) continue;
        auto us = static_cast<std::size_t>(si);
        if (us < ctx.subcatches.gw_node.size() && ctx.subcatches.gw_node[us] < 0)
            ctx.subcatches.gw_node[us] = ctx.node_names.find(nm);
    }
    ctx.pending_gw_nodes.clear();

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

    // No external-inflow timeseries resolution here: ExtInflowData carries no
    // index field, only ts_name, and InflowSolver::init resolves the name
    // itself (hydrology/Inflow.cpp). This used to run find_timeseries() once
    // per external inflow and discard the result.
    perf::sec_res_tables += perf::since(_pt_tables0);

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
    // [INITIAL_QUALITY] element + constituent resolution
    // -------------------------------------------------------------------------
    // The section may precede the node/link/pollutant sections, so the handler
    // stored raw names; resolve and classify here. Every failure is loud
    // (fatal on strict open) and the failing row is erased so downstream
    // consumers never see an unresolved entry. Iterate backwards so erase()
    // keeps remaining indices valid.
    {
        auto& iq = ctx.initial_quality;
        for (int i = iq.count() - 1; i >= 0; --i) {
            auto ui = static_cast<std::size_t>(i);
            const bool  link = iq.is_link[ui] != 0;
            const auto& en   = iq.elem_name[ui];
            const auto& cons = iq.constituent[ui];

            // Element name → index.
            iq.elem_idx[ui] = link ? ctx.link_names.find(en)
                                   : ctx.node_names.find(en);
            if (iq.elem_idx[ui] < 0) {
                ctx.errors.push_back(
                    std::string("[INITIAL_QUALITY] unknown ") +
                    (link ? "link" : "node") + " '" + en + "'.");
                iq.erase(i);
                continue;
            }

            // Constituent → kind. Pollutant first, then the reserved species.
            if (cons == "__WATER_AGE__") {
                iq.kind[ui] = InitialQualityData::kKindWaterAge;
                if (!ctx.options.water_age)
                    ctx.warnings.push_back(
                        "[INITIAL_QUALITY] __WATER_AGE__ rows are present but "
                        "[OPTIONS] WATER_AGE is OFF — the rows are inert this "
                        "simulation.");
            } else if (cons == "__TEMPERATURE__") {
                iq.kind[ui] = InitialQualityData::kKindTemperature;
                if (!ctx.options.heat_transport)
                    ctx.warnings.push_back(
                        "[INITIAL_QUALITY] __TEMPERATURE__ rows are present "
                        "but [OPTIONS] HEAT_TRANSPORT is OFF — the rows are "
                        "inert this simulation.");
            } else {
                iq.kind[ui] = ctx.pollutant_names.find(cons);
                if (iq.kind[ui] < 0) {
                    ctx.errors.push_back(
                        "[INITIAL_QUALITY] unknown constituent '" + cons +
                        "' at " + std::string(link ? "link" : "node") + " '" +
                        en + "' — pollutant names and __WATER_AGE__/"
                        "__TEMPERATURE__ are accepted here; MSX species "
                        "initial values belong in the reactions config "
                        "[REACTION_QUALITY] NODE|LINK scopes.");
                    iq.erase(i);
                    continue;
                }
                // Age (signed per D-NS1) and temperature (degC) may be
                // negative; a pollutant concentration may not.
                if (iq.value[ui] < 0.0) {
                    ctx.errors.push_back(
                        "[INITIAL_QUALITY] negative value for pollutant '" +
                        cons + "' at " + std::string(link ? "link" : "node") +
                        " '" + en + "'.");
                    iq.erase(i);
                    continue;
                }
            }
        }

        // Duplicate (scope, element, constituent) keys are an error, not
        // last-wins — silent override order in a hand-edited deck is exactly
        // the ambiguity this section should refuse. Forward scan so the
        // SECOND occurrence is the one reported.
        for (int i = 0; i < iq.count(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            for (int j = 0; j < i; ++j) {
                auto uj = static_cast<std::size_t>(j);
                if (iq.is_link[ui] == iq.is_link[uj] &&
                    iq.elem_idx[ui] == iq.elem_idx[uj] &&
                    iq.kind[ui] == iq.kind[uj]) {
                    ctx.errors.push_back(
                        "[INITIAL_QUALITY] duplicate row for '" +
                        iq.constituent[ui] + "' at " +
                        std::string(iq.is_link[ui] ? "link" : "node") + " '" +
                        iq.elem_name[ui] + "'.");
                    iq.erase(i);
                    --i;
                    break;
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // Link end-node re-resolution
    // -------------------------------------------------------------------------
    // Legacy parsing is order-independent, so [CONDUITS]/[PUMPS]/[ORIFICES]/
    // [WEIRS]/[OUTLETS] may precede the node sections. The handlers resolve
    // eagerly (yielding -1) and record the raw names in ctx.pending_link_nodes;
    // re-resolve them here. A name that still fails to resolve is a fatal
    // ERR_NAME, matching legacy link_readParams(). Previously such a link was
    // loaded silently orphaned and then written back out with '*' end nodes.
    // Runs before any consumer of links.node1/node2 further down this function.
    {
        auto resolve_end = [&ctx, n_nodes](int& idx, const std::string& name) {
            if (idx >= 0 && idx < n_nodes) return;
            idx = ctx.node_names.find(name);
            if (idx < 0) ctx.errors.push_back(format_error(ERR_NAME, name));
        };
        for (const auto& [li, names] : ctx.pending_link_nodes) {
            if (li < 0 || li >= ctx.links.count()) continue;
            auto ul = static_cast<std::size_t>(li);
            resolve_end(ctx.links.node1[ul], names.first);
            resolve_end(ctx.links.node2[ul], names.second);
        }
        ctx.pending_link_nodes.clear();
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
    // Virtual-junction initial-state seeding (issue #156)
    // -------------------------------------------------------------------------
    // [VIRTUAL_JUNCTIONS] carries no init-depth column, so VJs started dry
    // regardless of their neighbors — a deck whose real nodes define an
    // initial pool began with a hole at every splice (found by the mixed-flow
    // study P0 verification: a station VJ drained the Aureli initial pool).
    // Seed each VJ head by distance-weighted linear interpolation between the
    // nearest NON-virtual nodes reached by walking its spliced conduit chain
    // in both directions. Runs before the solver initializes; decks without
    // VJs (or with all-dry neighbors) are bitwise untouched.
    //
    // DYNWAVE ONLY, and that restriction is measured, not cautionary. Under FV
    // the initial condition does not live in the node state: Router::initFv
    // lays each conduit's cells at a UNIFORM DEPTH taken from links.depth (the
    // average of the two end-node depths), and only overrides that where one
    // bank is dry across a bed step. Seeding the node alone therefore starts
    // the run with a VJ head that its own adjacent cells contradict, and the
    // junction coupling drives the difference: measured on the study's e3
    // deck (a pressurized 0.094 m pipe, FV_SLOT_CELERITY 300), the flow-
    // routing continuity error went 0.000% -> -19.133% and VJ99's head
    // excursion reached 4371 m. Making FV benefit needs the level-surface
    // both-wet cell projection that Router::initFv deliberately does not do
    // (see the comment block there, and the P0 verification's open issue 2);
    // that is its own change with its own gates, so FV is left exactly as it
    // was rather than half-seeded.
    if (ctx.options.routing_model == RoutingModel::DYNWAVE) {
        // Incident CONDUITS per node (VJ splices are conduit-only by
        // validation; a VJ with conduit-degree != 2 is left unseeded here and
        // rejected later by the mesh builder / DW vjunc validation).
        std::vector<std::array<int, 2>> inc(static_cast<std::size_t>(n_nodes),
                                            {-1, -1});
        std::vector<int> inc_n(static_cast<std::size_t>(n_nodes), 0);
        for (int j = 0; j < n_links; ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx.links.type[uj] != LinkType::CONDUIT) continue;
            for (int nd : {ctx.links.node1[uj], ctx.links.node2[uj]}) {
                if (nd < 0 || nd >= n_nodes) continue;
                auto und = static_cast<std::size_t>(nd);
                if (ctx.nodes.is_virtual[und] == 0) continue;
                if (inc_n[und] < 2) inc[und][inc_n[und]] = j;
                ++inc_n[und];
            }
        }
        auto walk = [&](int start_vj, int via_link, double& head_out,
                        double& dist_out, bool& wet_out) -> bool {
            int node = start_vj, link = via_link;
            double dist = 0.0;
            for (int guard = 0; guard <= n_links; ++guard) {
                auto ul = static_cast<std::size_t>(link);
                const int cr = ctx.link_subtypes.conduit_row(link);
                dist += (cr >= 0)
                    ? ctx.link_subtypes.conduits.length[static_cast<std::size_t>(cr)]
                    : 0.0;
                const int other = (ctx.links.node1[ul] == node)
                    ? ctx.links.node2[ul] : ctx.links.node1[ul];
                if (other < 0 || other >= n_nodes) return false;
                auto uo = static_cast<std::size_t>(other);
                if (ctx.nodes.is_virtual[uo] == 0) {
                    head_out = ctx.nodes.head[uo];
                    dist_out = dist;
                    wet_out  = ctx.nodes.depth[uo] > 0.0;
                    return true;
                }
                if (inc_n[uo] != 2) return false;
                link = (inc[uo][0] == link) ? inc[uo][1] : inc[uo][0];
                if (link < 0) return false;
                node = other;
            }
            return false;  // cycle of VJs (rejected elsewhere)
        };
        for (int i = 0; i < n_nodes; ++i) {
            auto ui = static_cast<std::size_t>(i);
            if (ctx.nodes.is_virtual[ui] == 0 || inc_n[ui] != 2) continue;
            double hA = 0.0, dA = 0.0, hB = 0.0, dB = 0.0;
            bool wetA = false, wetB = false;
            const bool okA = walk(i, inc[ui][0], hA, dA, wetA);
            const bool okB = walk(i, inc[ui][1], hB, dB, wetB);
            // A dry node's head is just its invert — interpolating between two
            // dry endpoints with differing inverts would MANUFACTURE water on a
            // dry deck (P1 verification finding: vj_fv_invert_collision seeded
            // 1 ft from nothing). Seeding requires at least one WET endpoint.
            if (!(okA && wetA) && !(okB && wetB)) continue;
            double head;
            if (okA && okB) {
                const double dt_sum = dA + dB;
                head = (dt_sum > 0.0) ? (hA * dB + hB * dA) / dt_sum
                                      : 0.5 * (hA + hB);
            } else if (okA) {
                head = hA;
            } else if (okB) {
                head = hB;
            } else {
                continue;
            }
            const double depth = head - ctx.nodes.invert_elev[ui];
            if (depth > 0.0) {
                // init_depth is the field that SURVIVES: SWMMEngine::initialize
                // calls NodeData::reset_state(), which re-derives depth,
                // old_depth and head from init_depth on every cold start. A
                // seeding that wrote only depth/head was visible to a caller
                // that just opened the model and gone by the first routing
                // step — measured on the study's e4 deck, where base and
                // seeded binaries produced byte-identical .out files.
                ctx.nodes.init_depth[ui] = depth;
                ctx.nodes.depth[ui]      = depth;
                ctx.nodes.head[ui]       = head;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Evaporation timeseries resolution
    // -------------------------------------------------------------------------
    // Nothing to do: SWMMEngine::initHydrology resolves evap_ts_name into
    // climate_state.evap_ts_index. The lookup that used to sit here discarded
    // its result.

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
    } else {
        // DEPTH mode: legacy link_validate (link.c:1061-1070) zeroes a
        // NEGATIVE authored offset on every link with WARNING 03. Keeping it
        // shifts the link's invert-relative geometry — slope, elevation
        // drop, which conduit draws WARNING 04 — and diverges the hydraulics
        // from the first routing step (800-node-sewer: legacy WARN03 on
        // conduit 136687's -0.01 inOffset vs v6 WARN04 elsewhere). The ELEV
        // branch above and the conduit conversion below already clamp their
        // converted depths the same way. Weir/outlet crests live in the
        // side tables (legacy keeps them in offset1 and zeroes them by the
        // same check).
        for (int j = 0; j < n_links; ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx.links.offset1[uj] < 0.0) {
                ctx.warnings.push_back(format_warning(
                    WARN_NEGATIVE_OFFSET, ctx.link_names.name_of(j)));
                ctx.links.offset1[uj] = 0.0;
            }
            if (ctx.links.offset2[uj] < 0.0) {
                ctx.warnings.push_back(format_warning(
                    WARN_NEGATIVE_OFFSET, ctx.link_names.name_of(j)));
                ctx.links.offset2[uj] = 0.0;
            }
            auto lt = ctx.links.type[uj];
            if (lt == LinkType::WEIR || lt == LinkType::OUTLET) {
                const int wr  = ctx.link_subtypes.weir_row(j);
                const int olr = (wr < 0) ? ctx.link_subtypes.outlet_row(j) : -1;
                double* crest = (wr >= 0)
                    ? &ctx.link_subtypes.weirs.crest_height[static_cast<std::size_t>(wr)]
                    : (olr >= 0 ? &ctx.link_subtypes.outlets.crest_height[static_cast<std::size_t>(olr)]
                                : nullptr);
                if (crest && *crest < 0.0) {
                    ctx.warnings.push_back(format_warning(
                        WARN_NEGATIVE_OFFSET, ctx.link_names.name_of(j)));
                    *crest = 0.0;
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
        perf::ScopedTimer _pt_transects(perf::sec_res_transects);
        int nt = ctx.transects.count();
        ctx.transect_tables.resize(static_cast<std::size_t>(nt));
        // Copy-transform-build extracted to transect::buildFromStore so
        // swmm_link_set_xsect (IRREGULAR) builds bit-identical tables; the
        // parity notes on the legacy transforms live with the helper.
        const int t_us = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
        const double t_ucf = ucf::Ucf[ucf::LENGTH][static_cast<std::size_t>(t_us)];
        for (int t = 0; t < nt; ++t) {
            auto ut = static_cast<std::size_t>(t);
            auto& td = ctx.transect_tables[ut];
            if (!transect::buildFromStore(ctx.transects, t, t_ucf, td)) {
                ctx.errors.push_back(format_error(ERR_TRANSECT_MANNING, td.name));
                continue;
            }
        }
        // Resolve IRREGULAR link transect names → indices, then set properties.
        // The name→index map replaces a linear ieq scan per IRREGULAR link.
        // emplace keeps the FIRST index for a duplicated name, which is what
        // the scan returned (it broke at its first hit).
        std::unordered_map<std::string, int, CiHash, CiEqual> transect_by_name;
        transect_by_name.reserve(static_cast<std::size_t>(nt));
        for (int t = 0; t < nt; ++t)
            transect_by_name.emplace(ctx.transects.names[static_cast<std::size_t>(t)], t);

        for (int j = 0; j < n_links; ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx.links.xsect_shape[uj] != XsectShape::IRREGULAR) continue;
            // Resolve transect name (stored in pump_curve_name as temp field)
            const auto& tname = ctx.links.pump_curve_name[uj];
            if (!tname.empty()) {
                const auto hit = transect_by_name.find(tname);
                if (hit != transect_by_name.end())
                    ctx.links.xsect_curve[uj] = hit->second;
                else if (ctx.links.xsect_curve[uj] < 0)
                    // Dangling reference: legacy raises fatal ERROR 209 here
                    // (transect_validate). Leaving it silent let the link
                    // degenerate to zero area with no diagnostic — the state a
                    // corrupted save produced. Lenient opens still load for
                    // repair; strict opens must fail loudly.
                    ctx.errors.push_back(format_error(ERR_NAME, tname));
            } else if (ctx.links.xsect_curve[uj] < 0) {
                // No name and no resolved index: an IRREGULAR link with no
                // transect at all is as dead as a dangling one.
                ctx.errors.push_back(
                    format_error(ERR_NAME, ctx.link_names.name_of(j)));
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

    // Per-link named cross-section resolution (CUSTOM shape curves, then
    // STREET) followed by the full-flow xsect parameter loop. This is the
    // region the per-link linear scans and per-link TransectData builds live
    // in — see plan items 1.3 and 1.4.
    const auto _pt_xsect0 = perf::now();

    // Resolve CUSTOM shape curves — these use [CURVES] Shape type entries
    // that define normalized (depth/yFull, width/wMax) relationships.
    {
        int n_tables = static_cast<int>(ctx.tables.tables.size());
        // A CUSTOM table is fully determined by (shape curve, y_full): every
        // link sharing both gets a byte-identical ~1.2 KB TransectData. Build
        // one per distinct pair instead of one per link. Exact double equality
        // is the right test here — identical inputs, identical tabulation.
        std::map<std::pair<int, double>, int> custom_memo;
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

                // Already built for this (curve, y_full)? Point at it and skip
                // the tabulation entirely.
                {
                    const auto memo = custom_memo.find({ci, y_full});
                    if (memo != custom_memo.end()) {
                        const auto& shared = ctx.transect_tables[
                            static_cast<std::size_t>(memo->second)];
                        ctx.links.xsect_a_full[uj] = shared.a_full;
                        ctx.links.xsect_r_full[uj] = shared.r_full;
                        ctx.links.xsect_w_max[uj]  = shared.w_max;
                        ctx.links.xsect_curve[uj]  = memo->second;
                        continue;
                    }
                }

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
                custom_memo.emplace(std::pair<int, double>{ci, y_full}, custom_idx);
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
        // StreetParams derives entirely from the street index (inv_len is
        // constant across links), so every link on a street produces the same
        // ~1.2 KB table. N links over S streets used to allocate and tabulate
        // N of them; now it is S. TransectData::name is write-only for these
        // entries — nothing reads it back — so sharing one is safe even when
        // two links spell the street name with different case.
        std::unordered_map<int, int> street_memo;
        // Same treatment as transects: one map instead of a linear ieq scan
        // per STREET link, first index wins on a duplicated name. Also hoists
        // the ctx.streets.count() that the old inner loop re-read every pass.
        std::unordered_map<std::string, int, CiHash, CiEqual> street_by_name;
        {
            const int n_streets = ctx.streets.count();
            street_by_name.reserve(static_cast<std::size_t>(n_streets));
            for (int s = 0; s < n_streets; ++s)
                street_by_name.emplace(ctx.streets.names[static_cast<std::size_t>(s)], s);
        }
        for (int j = 0; j < n_links; ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx.links.xsect_shape[uj] != XsectShape::STREET_XSECT) continue;
            const auto& sname = ctx.links.pump_curve_name[uj];   // street name (temp field)
            if (sname.empty()) continue;

            int si = -1;
            if (const auto hit = street_by_name.find(sname);
                hit != street_by_name.end())
                si = hit->second;
            if (si < 0) continue;
            auto su = static_cast<std::size_t>(si);

            if (const auto memo = street_memo.find(si);
                memo != street_memo.end()) {
                const auto& shared = ctx.transect_tables[
                    static_cast<std::size_t>(memo->second)];
                ctx.links.xsect_curve[uj]  = memo->second;
                ctx.links.xsect_y_full[uj] = shared.y_full;
                ctx.links.xsect_a_full[uj] = shared.a_full;
                ctx.links.xsect_r_full[uj] = shared.r_full;
                ctx.links.xsect_w_max[uj]  = shared.w_max;
                continue;
            }

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
            street_memo.emplace(si, idx_tbl);
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

    perf::sec_res_xsect += perf::since(_pt_xsect0);

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

        // Apply minimum slope (legacy WARNING 05). Legacy conduit_getSlope
        // (link.c:1291-1298) RETURNS the positive MinSlope for SF/KW routing
        // before the adverse-sign flip — a sub-MinSlope adverse conduit is
        // sanitized to a positive slope under those models.
        bool min_slope_kw_sf = false;
        if (ctx.options.min_slope > 0.0 && slope < ctx.options.min_slope) {
            slope = ctx.options.min_slope;
            ctx.warnings.push_back(
                format_warning(WARN_MIN_SLOPE, ctx.link_names.name_of(j)));
            min_slope_kw_sf =
                (ctx.options.routing_model == RoutingModel::STEADY ||
                 ctx.options.routing_model == RoutingModel::KINWAVE);
        }

        // Negative slope for adverse gradient
        if (elev1 < elev2 && !min_slope_kw_sf) slope = -slope;

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
    {
        perf::ScopedTimer _pt(perf::sec_res_shrink);
        ctx.shrink_all_to_fit();
    }

    // Relational refactor (Phase 4): the side-table rows were populated directly
    // by the parse/resolution writers above; this only re-derives the base→row
    // reverse map and sizes it to the final node count (a consistency pass after
    // any in-parse re-types). No build-from-wide.
    ctx.node_subtypes.rebuild_index(ctx.nodes.count());
}

} /* namespace openswmm::input */
