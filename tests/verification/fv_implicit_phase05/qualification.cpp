// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
// Native hydraulic evidence driver. No production solver selection changes.
#include "../../unit/engine/fv_test_support.hpp"
#include "openswmm/engine/openswmm_engine.h"
#include "openswmm/engine/openswmm_nodes.h"
#include "openswmm/engine/openswmm_links.h"
#include "openswmm/engine/openswmm_massbalance.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>

using namespace fvtest;
namespace fs = std::filesystem;

namespace {
std::ofstream file(const fs::path& path) {
    std::ofstream out(path);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << std::setprecision(17);
    return out;
}
std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += static_cast<unsigned char>(c) < 32 ? '?' : c;
    }
    return out + '"';
}
void meshEvidence(const fs::path& path, const NetworkMeshData& mesh) {
    auto out = file(path);
    out << "one_section_component_mesh_v1,ft,ft2,ft3,s\n";
    auto array = [&](const char* name, const auto& values) {
        out << name << ',' << values.size();
        for (auto value : values) out << ',' << +value;
        out << '\n';
    };
#define FIELD(name) array(#name, mesh.name)
    FIELD(cell_dx); FIELD(cell_zb); FIELD(cell_geom); FIELD(cell_conduit);
    FIELD(cell_face0); FIELD(cell_face1); FIELD(cell_side0); FIELD(cell_side1);
    FIELD(face_cl); FIELD(face_cr); FIELD(face_node); FIELD(face_zb); FIELD(face_dx);
    FIELD(face_dir_l); FIELD(face_dir_r); FIELD(face_gate); FIELD(face_culvert);
    FIELD(conduit_cell_begin); FIELD(conduit_cell_count); FIELD(conduit_roughness);
    FIELD(node_kind); FIELD(node_invert); FIELD(node_face_ptr); FIELD(node_face_idx);
    FIELD(node_face_sign); FIELD(chain_ptr); FIELD(chain_cells); FIELD(chain_dir);
#undef FIELD
    for (const auto& g : mesh.geom) {
        out << "section," << g.xs.type << ',' << g.y_full << ',' << g.a_crown
            << ',' << g.t_slot << ',' << g.y_crown << ',' << g.barrels << '\n';
        const auto& c = g.closure_tbl;
        out << "closure," << +c.kind << ',' << c.y_full << ',' << c.dh << ','
            << c.a_crown << ',' << c.t_slot << ',' << c.r_full << '\n';
        for (int i = 0; i <= kClosurePanels; ++i)
            out << "panel," << i << ',' << c.A[i] << ',' << c.M[i] << ',' << c.I1[i] << ',' << c.R[i] << '\n';
    }
}

int component(int argc, char** argv) {
    if (argc != 9) throw std::invalid_argument("component CASE MODE CELLS CELERITY DT_CAP END OUTPUT");
    const std::string name = argv[2], mode = argv[3];
    const int cells = std::stoi(argv[4]);
    const double celerity = std::stod(argv[5]), cap = std::stod(argv[6]);
    double finish = std::stod(argv[7]);
    const fs::path out = argv[8]; fs::create_directories(out);
    if (cells < 4 || cells > 8192 || !(celerity > 0 && cap > 0 && finish > 0) ||
        (mode != "explicit" && mode != "legacy" && mode != "candidate"))
        throw std::invalid_argument("invalid component configuration");
    if (name != "wave" && name != "front" && name != "drain" && name != "headloss" && name != "momentum_limit")
        throw std::invalid_argument("unknown hydraulic case");
    constexpr double length = 1200.0, baseline = 6.0, amplitude = .001, sigma = 60.0;
    const double roughness = (name == "wave" || name == "momentum_limit") ? 0.0 : .013;
    auto ch = makeNodedChannel(circular(3.0), cells, length / cells,
                               [](double) { return 0.0; }, roughness, celerity);
    ch.mesh.node_kind.assign(2, kNodeOutfall);
    auto& g = ch.mesh.geom[0];
    const double area0 = kernels::areaOfDepth(g, baseline);
    const double wave_speed = std::sqrt(kernels::kGravity * std::min(area0, g.a_crown) / g.t_slot);
    const double expected_q = (1.486 / .013) * (3.14159265358979323846 * 9.0 / 4.0) *
        std::pow(.75, 2.0 / 3.0) * std::sqrt(.5 / length);
    const double initial = name == "front" ? .3 : baseline;
    seedLevel(ch, initial);
    std::vector<double> fixed = name == "front" ? std::vector<double>{12.0,.3} :
        name == "drain" ? std::vector<double>{.3,.3} :
        name == "headloss" ? std::vector<double>{6.0,5.5} : std::vector<double>{baseline,baseline};
    if (name == "wave") finish = 300.0 / wave_speed;
    for (int i = 0; i < cells; ++i) {
        const double x = (i + .5) * ch.dx;
        if (name == "wave") {
            const double perturbation = amplitude * std::exp(-.5 * std::pow((x - 300.0) / sigma, 2));
            ch.state.cell_a[i] = kernels::areaOfDepth(g, baseline + perturbation);
            ch.state.cell_q[i] = wave_speed * g.t_slot * perturbation;
        } else if (name == "headloss") {
            ch.state.cell_a[i] = kernels::areaOfDepth(g, 6.0 - .5 * x / length);
            ch.state.cell_q[i] = expected_q;
        } else if (name == "momentum_limit") {
            ch.state.cell_q[i] = std::cos(2.0 * 3.14159265358979323846 * i / 12.0);
        }
    }
    ch.state.node_head = fixed;
    auto opts = defaultOptions(); opts.lts = false; opts.pressurized_implicit = mode != "explicit";
    ExplicitFvSolver solver; solver.initialize(ch.mesh, ch.state, opts);
    const auto initial_q = ch.state.cell_q;
    if (mode == "candidate") solver.enablePressureStorageCandidate();
    FvStepForcing forcing; forcing.node_fixed_head = fixed.data(); forcing.n_nodes = 2;
    meshEvidence(out / "mesh_before.csv", ch.mesh);
    auto states = file(out / "states.csv"), trace = file(out / "trace.csv");
    states << "time_s,cell,x_ft,head_ft,discharge_cfs\n";
    trace << "time_s,watch_head_ft,inventory_ft3,boundary_out_ft3,balance_ft3,internal_steps,rejections\n";
    const double start_volume = totalVolume(ch);
    double t = 0.0, boundary = 0.0, maximum_balance = 0.0, arrival = -1.0;
    long steps = 0, rejections = 0;
    const double sample = name == "wave" ? finish / 20.0 : .5;
    long sample_number = 1;
    auto watch = [&] {
        const double coordinate = .6 * cells - .5;
        const int left = std::clamp(static_cast<int>(coordinate), 0, cells - 2);
        const double fraction = coordinate - left;
        return ch.state.cell_h[left] * (1.0 - fraction) + ch.state.cell_h[left + 1] * fraction;
    };
    auto write = [&] {
        for (int i = 0; i < cells; ++i)
            states << t << ',' << i << ',' << (i + .5) * ch.dx << ','
                   << ch.state.cell_h[i] << ',' << ch.state.cell_q[i] << '\n';
        trace << t << ',' << watch() << ',' << totalVolume(ch) << ',' << boundary << ','
              << totalVolume(ch) - start_volume + boundary << ',' << steps << ',' << rejections << '\n';
    };
    write();
    std::string error;
    try {
        while (t < finish) {
            const double target = std::min({finish, t + cap, sample_number * sample});
            if (target <= t) { ++sample_number; continue; }
            const double old_head = watch(), old_t = t;
            const double reached = solver.advance(t, target, forcing);
            if (reached != target) throw std::runtime_error("incomplete native advance");
            t = reached;
            INetworkSolver::Divergence divergence;
            if (solver.divergence(divergence)) throw std::runtime_error("native divergence guard fired");
            for (double receipt : solver.node_exchange()) boundary += receipt;
            steps += solver.last_num_steps(); rejections += solver.pressureCandidateRejections();
            for (double area : ch.state.cell_a)
                if (!std::isfinite(area) || area < 0) throw std::runtime_error("inadmissible cell area");
            maximum_balance = std::max(maximum_balance, std::fabs(totalVolume(ch) - start_volume + boundary));
            if (name == "front" && arrival < 0 && old_head < 3.0 && watch() >= 3.0)
                arrival = old_t + (t - old_t) * (3.0 - old_head) / (watch() - old_head);
            if (t >= sample_number * sample || t >= finish) { write(); ++sample_number; }
        }
    } catch (const std::exception& e) { error = e.what(); write(); }
    meshEvidence(out / "mesh_after.csv", ch.mesh);
    double square_error = 0.0, square_signal = 0.0, peak = 0.0, centroid = 0.0, weight = 0.0;
    for (int i = 0; i < cells; ++i) {
        const double x = (i + .5) * ch.dx;
        const double signal = amplitude * std::exp(-.5 * std::pow((x - 300.0 - wave_speed * t) / sigma, 2));
        square_error += std::pow(ch.state.cell_h[i] - baseline - signal, 2);
        square_signal += signal * signal;
        const double h = std::max(0.0, ch.state.cell_h[i] - baseline);
        peak = std::max(peak, h); centroid += x * h; weight += h;
    }
    const double mean_q = std::accumulate(ch.state.cell_q.begin(), ch.state.cell_q.end(), 0.0) / cells;
    auto summary = file(out / "summary.json");
    summary << "{\"case\":" << quoted(name) << ",\"mode\":" << quoted(mode)
        << ",\"cells\":" << cells << ",\"celerity_ft_s\":" << celerity << ",\"effective_wave_speed_ft_s\":" << wave_speed
        << ",\"dt_cap_s\":" << cap << ",\"requested_end_s\":" << finish << ",\"reached_s\":" << t
        << ",\"completed\":" << (error.empty() ? "true" : "false") << ",\"error\":" << quoted(error)
        << ",\"internal_steps\":" << steps << ",\"rejections\":" << rejections
        << ",\"max_relative_balance\":" << maximum_balance / start_volume << ",\"arrival_s\":" << arrival
        << ",\"mean_q_cfs\":" << mean_q << ",\"manning_q_cfs\":" << expected_q
        << ",\"headloss_relative_q_error\":" << std::fabs(mean_q - expected_q) / expected_q;
    if (name == "wave") summary << ",\"wave_relative_rms\":" << std::sqrt(square_error / square_signal)
        << ",\"wave_peak_ratio\":" << peak / amplitude << ",\"wave_speed_relative_error\":"
        << (weight > 0 && t > 0 ? std::fabs((centroid / weight - 300.0) / t - wave_speed) / wave_speed : 1.0);
    if (name == "momentum_limit") {
        double change = 0.0, prediction_error = 0.0, norm = 0.0;
        for (int i = cells / 4; i < 3 * cells / 4; ++i) {
            const double filtered = .25 * initial_q[i-1] + .5 * initial_q[i] + .25 * initial_q[i+1];
            change += std::pow(ch.state.cell_q[i] - initial_q[i], 2);
            prediction_error += std::pow(ch.state.cell_q[i] - filtered, 2);
            norm += initial_q[i] * initial_q[i];
        }
        summary << ",\"interior_relative_q_change\":" << std::sqrt(change / norm)
                << ",\"filter_prediction_relative_error\":" << std::sqrt(prediction_error / norm);
    }
    summary << "}\n";
    return error.empty() ? 0 : 2;
}

int engine(int argc, char** argv) {
    if (argc != 5) throw std::invalid_argument("engine INPUT END OUTPUT");
    const fs::path input = argv[2], out = argv[4];
    const double finish = std::stod(argv[3]); fs::create_directories(out);
    SWMM_Engine e = swmm_engine_create();
    auto check = [&](int code) { if (code != 0) throw std::runtime_error(swmm_get_last_error_msg(e)); };
    std::string error; double t = 0.0, balance = 0.0;
    auto trace = file(out / "engine_trace.csv");
    trace << "time_s,head_9p9_m,velocity_C2_m_s,tank_level_m\n";
    try {
        check(swmm_engine_open(e, input.string().c_str(), (out / "model.rpt").string().c_str(), nullptr, nullptr));
        check(swmm_engine_initialize(e)); check(swmm_engine_start(e, 0));
        const int gauge = swmm_node_index(e, "VJ99"), tank = swmm_node_index(e, "ST"), link = swmm_link_index(e, "C2");
        if (gauge < 0 || tank < 0 || link < 0) throw std::runtime_error("E2 probe mapping missing");
        auto sample = [&] {
            double head = 0, velocity = 0, depth = 0;
            check(swmm_node_get_head(e, gauge, &head)); check(swmm_node_get_depth(e, tank, &depth));
            check(swmm_link_get_velocity(e, link, &velocity));
            trace << t << ',' << head << ',' << velocity << ',' << depth << '\n';
        };
        sample();
        while (t < finish - 1.e-8) {
            double elapsed = 0; check(swmm_engine_step(e, &elapsed));
            check(swmm_engine_get_elapsed_seconds(e, &t)); sample();
            if (elapsed == 0.0) break;
        }
        check(swmm_engine_end(e)); check(swmm_get_routing_continuity_error(e, &balance));
    } catch (const std::exception& failure) { error = failure.what(); }
    swmm_engine_destroy(e);
    auto summary = file(out / "summary.json");
    summary << "{\"completed\":" << (error.empty() && t >= finish - 1.e-8 ? "true" : "false")
            << ",\"reached_s\":" << t << ",\"routing_continuity_fraction\":" << balance
            << ",\"error\":" << quoted(error) << "}\n";
    return error.empty() ? 0 : 2;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string(argv[1]) == "component") return component(argc, argv);
        if (argc > 1 && std::string(argv[1]) == "engine") return engine(argc, argv);
        throw std::invalid_argument("expected component or engine subcommand");
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
