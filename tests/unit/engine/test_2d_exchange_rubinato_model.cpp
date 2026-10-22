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
 * @file      test_2d_exchange_rubinato_model.cpp
 * @brief     Rubinato et al. (2017) Table 2 in the full engine: the flume rebuilt
 *        from rig_geometry.yaml, each steady row's boundary conditions
 *        reconstructed, run to steady state, exchange compared with Q_e
 *        (1D2D fine-scale coupling plan §8.2, report-only per §0R R2).
 *
 * @details The paper does not tabulate the inflows; they are recovered from
 *          Table 2 itself. Surface inflow Q1 = Fr_s·√(g h_s)·h_s·W (the
 *          Froude number and depth at the surface transducer, uniform across
 *          the 4 m width; S4 has none). Sewer inflow Q3 = |Q_e / (surcharge_pct/100)| for S2–S4
 *          (Table 2 defines surcharge_pct = 100 (Q3-Q4)/Q3). The pipe
 *          Reynolds number must not be mistaken for the inlet flow. S1 has
 *          no sewer inflow. The manhole head is held at the measured h_p by a FIXED
 *          outfall stage 1 m downstream (S1: free outfall). The manhole sits
 *          at the flume centre (x 4 m, y 2 m) — an assumption. The mesh is
 *          a uniform 0.2 m grid, NOT the paper's 0.0125 m refinement: a
 *          junction exchanges through ONE cell, so cells far smaller than the
 *          0.045 m² opening concentrate the whole exchange in a sliver the
 *          explicit solver (and the equalisation cap) must throttle —
 *          spreading a structure over its footprint cells is plan §10 work.
 *          The pond is
 *          the 0.24 m shaft (COUPLING_POND_MODE CHAMBER): under the legacy
 *          FOOTPRINT pond a fine mesh leaves the node a ~1e-4 m² pond, and
 *          a spill can release only what that pond holds.
 *
 *          Absolute agreement is REPORTED, not gated: Table 3's coefficients
 *          are regression slopes with intercepts (§0R R2), and these boundary
 *          conditions are reconstructions. The test asserts that every run
 *          completes and the exchange has the measured direction and order
 *          of magnitude (|Q_num| > Q_e/4 — a choke detector); routing
 *          continuity is reported (the 3-node network's DW node bookkeeping,
 *          not the coupling, dominates it). Results land in
 *          ./rubinato2017_model_out/model_vs_table2.csv (one row per case).
 *          All 36 rows run (~15 s); S1 rows run under the legacy ORIFICE
 *          law as well.
 *
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_massbalance.h>
#include <openswmm/engine/openswmm_nodes.h>

namespace fs = std::filesystem;

namespace {

const fs::path kRef = fs::path(__FILE__).parent_path() / "../../benchmarks/rubinato2017";
const fs::path kOut = "rubinato2017_model_out";

/**
 * @brief Read simple colon-delimited numeric benchmark rig fields.
 *
 * Skips empty lines and column-zero # comments, uses text before first colon verbatim as key and
 * atof afterward. Does not trim, validate conversion, support YAML nesting/quotes or report
 * malformed numeric values (often converted to zero).
 *
 * @param[in] p Input path for the controlled flat key:value fixture.
 * @return Owned map; duplicate exact keys overwrite and absent/unreadable files produce an empty
 *     map.
 */
std::map<std::string, double> read_yaml(const fs::path& p) {
    std::map<std::string, double> out;
    std::ifstream f(p);
    for (std::string line; std::getline(f, line);) {
        const auto colon = line.find(':');
        if (line.empty() || line[0] == '#' || colon == std::string::npos)
            continue;
        out[line.substr(0, colon)] = std::atof(line.c_str() + colon + 1);
    }
    return out;
}

/**
 * @brief Read the controlled unquoted benchmark table using its first line as keys.
 *
 * No quoted-field/embedded-comma handling, trimming, CR removal or schema/numeric validation.
 * Unreadable input returns no rows; allocation errors propagate.
 *
 * @param[in] p Readable comma-delimited fixture path, unchanged.
 * @return Owned nonempty data-line maps; missing cells remain absent, extra cells are ignored and
 *     duplicate keys overwrite.
 */
std::vector<std::map<std::string, std::string>> read_csv(const fs::path& p) {
    std::ifstream f(p);
    std::string header;
    std::getline(f, header);
    std::vector<std::string> keys;
    std::stringstream hs(header);
    for (std::string k; std::getline(hs, k, ',');)
        keys.push_back(k);
    std::vector<std::map<std::string, std::string>> rows;
    for (std::string line; std::getline(f, line);) {
        if (line.empty())
            continue;
        std::map<std::string, std::string> row;
        std::stringstream ls(line);
        std::string v;
        for (std::size_t i = 0; i < keys.size() && std::getline(ls, v, ','); ++i)
            row[keys[i]] = v;
        rows.push_back(row);
    }
    return rows;
}

/**
 * @brief Build graded grid coordinates outward from a central point to two endpoints.
 *
 * Builds each side independently, reversing the lower side before concatenation. Does not validate
 * geometry/progress or cap count; unsuitable zero/negative/vanishing spacings can stall or
 * overallocate.
 *
 * @param[in] lo Finite lower endpoint in metres, strictly less than c.
 * @param[in] hi Finite upper endpoint in metres, strictly greater than c.
 * @param[in] c Interior center coordinate in metres.
 * @param[in] h0 Positive finite initial spacing in metres.
 * @param[in] hmax Positive finite spacing cap, intended at least h0.
 * @param[in] g Finite spacing growth factor at least one for the intended terminating graded grid.
 * @return Owned increasing coordinate vector from lo through c to hi; endpoint gaps follow the
 *     half-current-spacing stopping rule.
 */
std::vector<double> axis(double lo, double hi, double c, double h0, double hmax, double g) {
    auto side = [&](double end, double dir) {
        std::vector<double> xs;
        double x = c, h = h0;
        while (dir * (end - (x + dir * h)) > 0.5 * h) {
            x += dir * h;
            xs.push_back(x);
            h = std::min(hmax, h * g);
        }
        xs.push_back(end);
        return xs;
    };
    std::vector<double> down = side(lo, -1.0), up = side(hi, 1.0), out(down.rbegin(), down.rend());
    out.push_back(c);
    out.insert(out.end(), up.begin(), up.end());
    return out;
}

struct Case {
    std::string scenario;
    double q_e = 0, h_s = 0, h_p = 0, q1 = 0, q3 = 0; // l/s, m, m, m³/s, m³/s
};

/**
 * @brief Construct the graded flume/manhole benchmark deck with measured case forcing.
 *
 * Uses axis growth1.1, fixed held pipe/outfall forcing and Rubinato2017 manhole coefficients.
 * Requires positive dimensions and derived vertex/cell counts fitting int; no
 * syntax/finite/physical validation or simulation occurs. Rig/Case references are not retained.
 *
 * @param[in] c Scenario inputs: q1/q3 m3/s and downstream head h_p metres; selects sewer feed when
 *     q3>0.
 * @param[in] rig Required named rig dimensions/slopes/roughness/diameters/crest/spacing map;
 *     missing keys throw out_of_range.
 * @param[in] law Raw COUPLING_LAW token inserted verbatim.
 * @param[in] h0 Positive center mesh spacing in metres, not initial water depth.
 * @param[in] extra2d Raw additional 2D_OPTIONS text with required newline delimiters.
 * @param[in] physical_shaft Use a real circular-area manhole storage and matching minimum area when
 *     true; otherwise ordinary junction.
 * @return Owned five-minute CMS/DYNWAVE deck with graded triangular flume, center vertex MH map,
 *     downstream normal flow and optional upstream flow boundary.
 */
std::string deck(const Case& c, const std::map<std::string, double>& rig, const std::string& law,
                 double h0, const std::string& extra2d, bool physical_shaft = false) {
    const double L = rig.at("flume_length_m"), W = rig.at("flume_width_m");
    const double slope = rig.at("flume_slope"), n = rig.at("flume_manning_n");
    const double zc = rig.at("z_crest_m"), xm = 0.5 * L, ym = 0.5 * W;
    const auto xs = axis(0.0, L, xm, h0, rig.at("mesh_size_boundary_m"), 1.1);
    const auto ys = axis(0.0, W, ym, h0, rig.at("mesh_size_boundary_m"), 1.1);
    const int nx = static_cast<int>(xs.size()), ny = static_cast<int>(ys.size());
    auto vid = [&](int i, int j) { return j * nx + i; };
    int centre = -1;

    std::ostringstream m;
    m.precision(10);
    m << "[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
         "END_DATE 01/01/2026\nEND_TIME 00:05:00\nREPORT_STEP 00:01:00\nROUTING_STEP 0.5\n\n";
    if (physical_shaft) {
        const double d = rig.at("manhole_diameter_m"), a = 3.14159265358979323846 * d * d / 4.0;
        // A laboratory shaft has real storage below the rim. The default
        // DW MIN_SURFAREA (about 1.17 m²) is much larger than this 0.045 m²
        // shaft; set both the storage curve and the floor to its actual area.
        m << "MIN_SURFAREA " << a << "\n\n[STORAGE]\nMH 0 " << zc << " 0 FUNCTIONAL 0 0 " << a
          << " 0 0\n\n";
    } else {
        m << "[JUNCTIONS]\nMH 0 " << zc << " 0 0 0\n\n";
    }
    m << "[OUTFALLS]\n";
    const bool sewer = c.q3 > 0; // S2–S4: the inlet tank feeds the pipe
    if (!sewer)
        m << "OUT 0 FREE NO\n\n";
    else
        m << "OUT 0 FIXED " << c.h_p << " NO\n\n";
    // The inlet tank is a storage node so its volume is booked (a dead-end
    // junction integrates over MIN_SURFAREA but books no volume below its
    // rim — a continuity artefact on a network this small). Downstream, a
    // short wide pipe lets the outfall's FIXED stage impose the measured h_p
    // at the manhole; S1 drains through the 75 mm pipe to a free outfall.
    if (sewer)
        m << "[STORAGE]\nIN 0 1.5 0 FUNCTIONAL 0 0 0.05 0 0\n\n";
    m << "[CONDUITS]\n";
    if (sewer)
        m << "C1 IN MH 2.0 0.010 0 0 0\nC2 MH OUT 0.2 0.010 0 0 0\n\n";
    else
        m << "C2 MH OUT 2.0 0.010 0 0 0\n\n";
    m << "[XSECTIONS]\n";
    if (sewer)
        m << "C1 CIRCULAR " << rig.at("pipe_diameter_m") << " 0 0 0 1\nC2 CIRCULAR 0.3 0 0 0 1\n\n";
    else
        m << "C2 CIRCULAR " << rig.at("pipe_diameter_m") << " 0 0 0 1\n\n";
    if (sewer)
        m << "[INFLOWS]\nIN FLOW \"\" FLOW 1.0 1.0 " << c.q3 << "\n\n";
    m << "[2D_OPTIONS]\nMAX_TIMESTEP 1\nDRY_DEPTH 0.0002\nREPORT_2D NO\nCOUPLING_LAW " << law
      << "\nCOUPLING_HEAD STENCIL_WET_MEAN\nCOUPLING_POND_MODE CHAMBER\n"
      << extra2d
      << "\n"
         "[2D_NODE_COUPLING]\nMH MANHOLE D="
      << rig.at("manhole_diameter_m")
      << " COEFFS=RUBINATO2017\n\n"
         "[2D_VERTICES]\n";
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            if (std::fabs(xs[i] - xm) < 1e-12 && std::fabs(ys[j] - ym) < 1e-12)
                centre = vid(i, j);
            m << xs[i] << " " << ys[j] << " " << zc + slope * (xm - xs[i]) << "\n";
        }
    m << "\n[2D_TRIANGLES]\n";
    std::ostringstream bc;
    int tri = 0;
    for (int j = 0; j + 1 < ny; ++j)
        for (int i = 0; i + 1 < nx; ++i) {
            // A = (v00, v10, v11), B = (v00, v11, v01); edge k is opposite vertex k.
            m << vid(i, j) << " " << vid(i + 1, j) << " " << vid(i + 1, j + 1) << " " << n << "\n";
            if (i + 2 == nx)
                bc << tri << " 0 NORMAL_FLOW " << slope << "\n"; // (v10, v11)
            ++tri;
            m << vid(i, j) << " " << vid(i + 1, j + 1) << " " << vid(i, j + 1) << " " << n << "\n";
            if (i == 0 && c.q1 > 0)
                bc << tri << " 1 SPECIFIED_FLOW " << -c.q1 / W << "\n"; // (v01, v00)
            ++tri;
        }
    m << "\n[2D_VERTEX_NODE_MAP]\n" << centre << " MH\n\n[2D_BOUNDARY_CONDITIONS]\n" << bc.str();
    return m.str();
}

struct Result {
    bool ok = false;
    double q_num_lps = 0; ///< steady exchange, + = sewer → surface
    double continuity_pct = 0;
    double q_previous_lps = 0;
    double surface_depth_m = 0, node_head_m = 0;
    double sample_x_m = 0, sample_y_m = 0;
    double duration_s = 0;
    int cells = 0;
};

/**
 * @brief Run a flume/manhole case and measure last-minute exchange and nearby depth/head.
 *
 * Writes/opens/initializes/starts with output disabled, chooses nearest centroid to the upstream
 * sensor target, and measures cumulative exchange windows after 180/240 s. Requires successful
 * step/end, late coverage>=299 s, ordered windows and positive sample time before setting
 * finite-result ok. Many getter/report/close statuses remain unchecked; probe is approximate and no
 * time interpolation/exact experimental-location reconstruction occurs. Destroys engine on returned
 * paths; filesystem/map/allocation exceptions propagate.
 *
 * @param[in] c Measured case forcing forwarded to deck.
 * @param[in] rig Required rig map including flume dimensions and p_surface_offset_m for
 *     nearest-cell probe selection.
 * @param[in] law Raw coupling law forwarded to deck.
 * @param[in] tag Safe artifact basename beneath kOut for .inp/.rpt, overwritten.
 * @param[in] h0 Positive center mesh spacing override in metres; nonpositive selects rig
 *     mesh_size_boundary_m.
 * @param[in] extra2d Raw2D option overrides forwarded unchanged.
 * @param[in] physical_shaft Enable physical manhole storage geometry in deck.
 * @return Owned result with exchange L/s, native continuity converted to percent, sampled mean
 *     surface depth/node head metres, probe coordinates, duration seconds, cell count and validity
 *     flag.
 */
Result run(const Case& c, const std::map<std::string, double>& rig, const std::string& law,
           const std::string& tag, double h0 = 0.0, const std::string& extra2d = {},
           bool physical_shaft = false) {
    Result r;
    fs::create_directories(kOut);
    const fs::path inp = kOut / (tag + ".inp"), rpt = kOut / (tag + ".rpt");
    {
        std::ofstream f(inp);
        f << deck(c, rig, law, h0 > 0 ? h0 : rig.at("mesh_size_boundary_m"), extra2d,
                  physical_shaft);
    }
    SWMM_Engine e = swmm_engine_create();
    if (swmm_engine_open(e, inp.string().c_str(), rpt.string().c_str(), "", nullptr) != SWMM_OK ||
        swmm_engine_initialize(e) != SWMM_OK || swmm_engine_start(e, 0) != SWMM_OK) {
        swmm_engine_destroy(e);
        return r;
    }
    int nt = 0;
    swmm_2d_triangle_count(e, &nt);
    const int manhole = swmm_node_index(e, "MH");
    r.cells = nt;
    // Nearest-cell proxy for P_surface, 0.46 m upstream of the assumed
    // central manhole. Export its coordinates: this is not an exact probe.
    int probe = -1;
    double distance = std::numeric_limits<double>::max();
    for (int i = 0; i < nt; ++i) {
        double x = 0, y = 0, z = 0;
        swmm_2d_triangle_get_centroid(e, i, &x, &y, &z);
        const double d =
            std::hypot(x - (0.5 * rig.at("flume_length_m") - rig.at("p_surface_offset_m")),
                       y - 0.5 * rig.at("flume_width_m"));
        if (d < distance) {
            distance = d;
            probe = i;
            r.sample_x_m = x;
            r.sample_y_m = y;
        }
    }
    std::vector<double> vol(static_cast<std::size_t>(nt));
    auto net = [&] {
        swmm_2d_get_coupling_volume_bulk(e, vol.data());
        double s = 0;
        for (double v : vol)
            s += v;
        return s;
    };
    double elapsed = 0, t0 = -1, v0 = 0, t1 = 0, v1 = 0;
    double tp = -1, vp = 0, hs_time = 0, hp_time = 0, sample_time = 0;
    int status = SWMM_OK;
    while ((status = swmm_engine_step(e, &elapsed)) == SWMM_OK && elapsed > 0.0) {
        const double t = elapsed * 86400.0;
        if (tp < 0 && t >= 180.0) {
            tp = t;
            vp = net();
        }
        if (t0 < 0 && t >= 240.0) {
            t0 = t;
            v0 = net();
        } // last minute = steady window
        if (t0 >= 0 && t1 >= t0 && probe >= 0) {
            double hs = 0, hp = 0;
            swmm_2d_get_depth(e, probe, &hs);
            swmm_node_get_head(e, manhole, &hp);
            hs_time += hs * (t - t1);
            hp_time += hp * (t - t1);
            sample_time += t - t1;
        }
        t1 = t;
    }
    v1 = net();
    const int end_status = swmm_engine_end(e);
    double cont = 0;
    swmm_get_routing_continuity_error(e, &cont);
    swmm_engine_report(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    // A late step failure used to be indistinguishable from normal EOF.
    if (status != SWMM_OK || end_status != SWMM_OK || t1 < 299.0 || tp < 0 || !(t0 > tp) ||
        !(t1 > t0) || !(sample_time > 0))
        return r;
    r.q_num_lps = 1000.0 * (v1 - v0) / (t1 - t0);
    r.continuity_pct = 100.0 * cont;
    r.q_previous_lps = 1000.0 * (v0 - vp) / (t0 - tp);
    r.surface_depth_m = hs_time / sample_time;
    r.node_head_m = hp_time / sample_time;
    r.duration_s = t1;
    r.ok = std::isfinite(r.q_num_lps) && std::isfinite(r.continuity_pct) &&
           std::isfinite(r.surface_depth_m) && std::isfinite(r.node_head_m);
    return r;
}

} // namespace

TEST(RubinatoModel, Table2RowsInTheFlume) {
    const auto rig = read_yaml(kRef / "rig_geometry.yaml");
    const auto rows = read_csv(kRef / "table2_steady_exchange.csv");
    ASSERT_EQ(rows.size(), 36u) << "fixture: Table 2 transcription";
    constexpr double g = 9.81;
    const double W = rig.at("flume_width_m");

    fs::create_directories(kOut);
    std::ofstream csv(kOut / "model_vs_table2.csv");
    csv << std::setprecision(12);
    csv << "case,scenario,law,q_e_lps,q_num_lps,ratio,h_s_m,h_p_m,q1_lps,q3_lps,continuity_pct,"
           "q_previous_lps,surface_depth_m,node_head_m,sample_x_m,sample_y_m,duration_s,cells\n";
    std::map<std::string, int> seen;
    for (const auto& row : rows) {
        Case c;
        c.scenario = row.at("scenario");
        const int k = ++seen[c.scenario]; // row number within the scenario
        c.q_e = std::atof(row.at("q_e_lps").c_str());
        c.h_s = row.at("h_s_m").empty() ? 0.0 : std::atof(row.at("h_s_m").c_str());
        c.h_p = row.at("h_p_m").empty() ? 0.0 : std::atof(row.at("h_p_m").c_str());
        const double fr = std::atof(row.at("fr_s").c_str());
        c.q1 = c.h_s > 0 ? fr * std::sqrt(g * c.h_s) * c.h_s * W : 0.0;
        c.q3 = c.scenario == "S1"
                   ? 0.0
                   : std::fabs(0.1 * c.q_e / std::atof(row.at("surcharge_pct").c_str()));
        const bool to_surface = c.scenario == "S3" || c.scenario == "S4";
        // S1 also runs under the legacy ORIFICE law — the anchor that shows
        // what the pre-plan law predicts for a free-weir drain.
        const std::vector<std::string> laws =
            c.scenario == "S1" ? std::vector<std::string>{"WEIR_ORIFICE", "ORIFICE"}
                               : std::vector<std::string>{"WEIR_ORIFICE"};
        for (const auto& law : laws) {
            const std::string tag = c.scenario + "_" + std::to_string(k) + "_" + law;
            const Result r = run(c, rig, law, tag);
            ASSERT_TRUE(r.ok) << tag << " did not run (see " << (kOut / (tag + ".rpt")) << ")";
            csv << tag << "," << c.scenario << "," << law << "," << c.q_e << "," << r.q_num_lps
                << "," << std::fabs(r.q_num_lps) / c.q_e << "," << c.h_s << "," << c.h_p << ","
                << 1000 * c.q1 << "," << 1000 * c.q3 << "," << r.continuity_pct << ","
                << r.q_previous_lps << "," << r.surface_depth_m << "," << r.node_head_m << ","
                << r.sample_x_m << "," << r.sample_y_m << "," << r.duration_s << "," << r.cells
                << "\n";
            EXPECT_EQ(r.q_num_lps > 0, to_surface)
                << tag << ": exchange direction, Q_num " << r.q_num_lps;
            // Not an accuracy gate — a choke detector: a pond or cap that
            // throttles the exchange shows up as Q_num ≪ Q_e.
            EXPECT_GT(std::fabs(r.q_num_lps), 0.25 * c.q_e) << tag << ": exchange choked";
        }
    }
}

// The paper's own mesh — 0.0125 m cells at the manhole, far smaller than its
// 0.045 m² opening — must not choke the exchange under COUPLING_EQUALIZE
// (on by default with WEIR_ORIFICE). The first form of the cap charged the
// single coupling cell for the whole window and let ~0.1 % of the flow
// through; the cap bounds only the 1D side, held for the window.
TEST(RubinatoModel, EqualisationDoesNotChokeAFineMesh) {
    const auto rig = read_yaml(kRef / "rig_geometry.yaml");
    const auto rows = read_csv(kRef / "table2_steady_exchange.csv");
    const auto& row = rows.at(20); // the first S3 row
    ASSERT_EQ(row.at("scenario"), "S3");
    Case c;
    c.scenario = "S3";
    c.q_e = std::atof(row.at("q_e_lps").c_str());
    c.h_s = std::atof(row.at("h_s_m").c_str());
    c.h_p = std::atof(row.at("h_p_m").c_str());
    c.q1 = std::atof(row.at("fr_s").c_str()) * std::sqrt(9.81 * c.h_s) * c.h_s *
           rig.at("flume_width_m");
    c.q3 = std::fabs(0.1 * c.q_e / std::atof(row.at("surcharge_pct").c_str()));
    const double h0 = rig.at("mesh_size_near_crest_m");
    const Result eq = run(c, rig, "WEIR_ORIFICE", "fine_S3_equalize", h0);
    const Result off =
        run(c, rig, "WEIR_ORIFICE", "fine_S3_no_equalize", h0, "COUPLING_EQUALIZE NO");
    ASSERT_TRUE(eq.ok && off.ok);
    std::ofstream csv(kOut / "fine_mesh_equalisation.csv");
    csv << std::setprecision(12)
        << "equalize,q_num_lps,q_previous_lps,continuity_pct,surface_depth_m,node_head_m,cells\n"
        << "YES," << eq.q_num_lps << "," << eq.q_previous_lps << "," << eq.continuity_pct << ","
        << eq.surface_depth_m << "," << eq.node_head_m << "," << eq.cells << "\n"
        << "NO," << off.q_num_lps << "," << off.q_previous_lps << "," << off.continuity_pct << ","
        << off.surface_depth_m << "," << off.node_head_m << "," << off.cells << "\n";
    ASSERT_GT(off.q_num_lps, 0.25 * c.q_e) << "fixture: the fine mesh exchanges without the cap";
    EXPECT_GT(eq.q_num_lps, 0.5 * off.q_num_lps)
        << "equalisation choked the exchange: " << eq.q_num_lps << " vs " << off.q_num_lps
        << " l/s";
}

// A modelling sensitivity, without retuning any exchange coefficient. This
// separates the previous junction/MIN_SURFAREA surrogate from a shaft with
// the laboratory's actual area and booked below-rim storage. Absolute fit,
// head residuals and continuity remain report-only, including poor results.
TEST(RubinatoModel, PhysicalShaftSensitivity) {
    const auto rig = read_yaml(kRef / "rig_geometry.yaml");
    const auto rows = read_csv(kRef / "table2_steady_exchange.csv");
    ASSERT_EQ(rows.size(), 36u);
    fs::create_directories(kOut);
    std::ofstream csv(kOut / "shaft_vs_table2.csv");
    csv << std::setprecision(12)
        << "case,scenario,law,q_e_lps,q_num_lps,ratio,h_s_m,h_p_m,q1_lps,q3_lps,continuity_pct,"
           "q_previous_lps,surface_depth_m,node_head_m,sample_x_m,sample_y_m,duration_s,cells\n";
    std::map<std::string, int> seen;
    for (const auto& row : rows) {
        Case c;
        c.scenario = row.at("scenario");
        c.q_e = std::atof(row.at("q_e_lps").c_str());
        c.h_s = std::atof(row.at("h_s_m").c_str());
        c.h_p = std::atof(row.at("h_p_m").c_str());
        c.q1 = c.h_s > 0 ? std::atof(row.at("fr_s").c_str()) * std::sqrt(9.81 * c.h_s) * c.h_s *
                               rig.at("flume_width_m")
                         : 0;
        c.q3 = c.scenario == "S1"
                   ? 0
                   : std::fabs(0.1 * c.q_e / std::atof(row.at("surcharge_pct").c_str()));
        const std::string tag = c.scenario + "_" + std::to_string(++seen[c.scenario]) + "_SHAFT";
        const Result r = run(c, rig, "WEIR_ORIFICE", tag, 0, {}, true);
        ASSERT_TRUE(r.ok) << tag;
        csv << tag << "," << c.scenario << ",WEIR_ORIFICE," << c.q_e << "," << r.q_num_lps << ","
            << std::fabs(r.q_num_lps) / c.q_e << "," << c.h_s << "," << c.h_p << "," << 1000 * c.q1
            << "," << 1000 * c.q3 << "," << r.continuity_pct << "," << r.q_previous_lps << ","
            << r.surface_depth_m << "," << r.node_head_m << "," << r.sample_x_m << ","
            << r.sample_y_m << "," << r.duration_s << "," << r.cells << "\n";
        EXPECT_EQ(r.q_num_lps > 0, c.scenario == "S3" || c.scenario == "S4") << tag;
        EXPECT_GT(std::fabs(r.q_num_lps), 0.25 * c.q_e) << tag;
    }
}
