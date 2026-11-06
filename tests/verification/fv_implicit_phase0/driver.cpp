// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
// Phase P0: measured component baselines on identical physical FV subgrids.
#include "../../unit/engine/fv_test_support.hpp"
#include "network_cases.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <locale>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace fvtest;
namespace k = openswmm::fv::kernels;

namespace {
struct Config {
    std::string scenario = "pressure_slosh";
    int cells = 16;
    double celerity = 100.0;
    double duration = 30.0;
    double routing_step = 5.0;
    bool implicit_pressure = false;
    fs::path output;
};

std::ofstream output(const fs::path& path) {
    std::ofstream stream(path);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream.imbue(std::locale::classic());
    stream << std::setprecision(17);
    return stream;
}

template<class Range>
void writeArray(std::ostream& out, const char* name, const Range& values) {
    out << name << ',' << values.size();
    for (auto v : values) out << ',' << +v;
    out << '\n';
}

// Canonical text, not raw structs: no pointers, padding or host byte order.
// These fixtures have one analytic section, no structures and no TPA state.
// Keep the schema versioned when expanding the physical fixture family.
void writeMesh(const fs::path& path, const NetworkMeshData& m) {
    auto out = output(path);
    out << "fv_component_mesh_v1,ft,ft2,ft3,s\n";
#define FIELD(name) writeArray(out, #name, m.name)
    FIELD(cell_geom); FIELD(cell_conduit); FIELD(cell_dx); FIELD(cell_zb);
    FIELD(cell_dzdx); FIELD(cell_face0); FIELD(cell_face1);
    FIELD(cell_side0); FIELD(cell_side1); FIELD(face_cl); FIELD(face_cr);
    FIELD(face_node); FIELD(face_geom); FIELD(face_gate); FIELD(face_culvert);
    FIELD(face_zb); FIELD(face_dx); FIELD(face_dir_l); FIELD(face_dir_r);
    FIELD(face_virtual); FIELD(face_vj_node); FIELD(node_vj_face);
    FIELD(conduit_cell_begin); FIELD(conduit_cell_count); FIELD(conduit_link);
    FIELD(conduit_section); FIELD(conduit_roughness); FIELD(conduit_rough_factor);
    FIELD(conduit_loss_inlet); FIELD(conduit_loss_outlet); FIELD(conduit_slope);
    FIELD(conduit_culvert_code); FIELD(conduit_culvert_mitered);
    FIELD(chain_ptr); FIELD(chain_cells); FIELD(chain_dir);
    FIELD(cell_chain); FIELD(cell_chain_pos);
    FIELD(struct_link); FIELD(struct_n1); FIELD(struct_n2); FIELD(struct_is_dummy);
    FIELD(node_dummy_drain); FIELD(node_face_ptr); FIELD(node_face_idx);
    FIELD(node_face_sign); FIELD(node_face_zb); FIELD(node_invert);
    FIELD(node_full_depth); FIELD(node_ponded_area); FIELD(node_sur_depth);
    FIELD(node_can_pond); FIELD(node_kind); FIELD(node_area); FIELD(node_vol_off);
    FIELD(node_vol_dmax); FIELD(node_vol_atop); FIELD(node_vol_tbl);
#undef FIELD
    for (const auto& g : m.geom) {
        out << "geometry," << g.xs.type << ',' << g.y_full << ',' << g.a_full
            << ',' << g.w_max << ',' << g.r_full << ',' << g.barrel_scale
            << ',' << g.y_crown << ',' << g.t_slot << ',' << g.a_crown
            << ',' << g.i1_crown << ',' << +g.is_open << ',' << g.barrels << '\n';
        const auto& c = g.closure_tbl;
        out << "closure," << c.y_full << ',' << c.dh << ',' << c.inv_dh
            << ',' << c.a_crown << ',' << c.t_slot << ',' << c.i1_crown
            << ',' << c.r_full << ',' << c.inv_a_crown_n
            << ',' << c.c2 << ',' << c.c1 << ',' << c.p0 << ',' << c.p1
            << ',' << +c.kind << '\n';
        // Array references preserve table extents without raw struct dumps.
        auto table = [&](const char* name, const auto& values) {
            out << name << ',' << std::size(values);
            for (auto value : values) out << ',' << +value;
            out << '\n';
        };
        table("A", c.A); table("T", c.M); table("I1", c.I1);
        table("R", c.R); table("inverse_panel", c.j_of_a);
    }
}

Channel makeCase(const Config& cfg) {
    if (fvbaseline::networkCase(cfg.scenario))
        return fvbaseline::makeNetwork(cfg.scenario, cfg.cells, cfg.celerity);
    const bool open = cfg.scenario == "open_slosh";
    const auto xs = open ? rectOpen(4.0, 3.0) : circular(3.0);
    const double dx = 400.0 / cfg.cells; // fixed physical length under refinement
    const bool prescribed = cfg.scenario == "filling" || cfg.scenario == "pressure_flow";
    Channel ch = prescribed
        ? makeNodedChannel(xs, cfg.cells, dx, [](double) { return 0.0; },
                           0.013, cfg.celerity)
        : makeWalledChannel(xs, cfg.cells, dx, [](double) { return 0.0; },
                            0.013, cfg.celerity);
    if (prescribed) {
        const double downstream_head = cfg.scenario == "filling" ? 0.3 : 5.5;
        seedLevel(ch, downstream_head);
        ch.mesh.node_kind[0] = kNodeOutfall; // both ends prescribed, no storage
        ch.state.node_head = {6.0, downstream_head};
    } else {
        const bool rest = cfg.scenario == "pressure_rest";
        const double eta = open ? 1.0 : 6.0;
        seedLevel(ch, eta);
        if (!rest) {
            const auto& g = ch.mesh.geom[0];
            for (int i = 0; i < ch.n; ++i) {
                const double shift = (open ? 0.05 : 0.01) *
                    std::cos(2.0 * 3.14159265358979323846 * (i + 0.5) / ch.n);
                ch.state.cell_a[i] = k::areaOfDepth(g, eta + shift);
            }
        }
    }
    return ch;
}

void writeState(std::ostream& out, double time, const Channel& ch) {
    for (int i = 0; i < ch.n; ++i) {
        const auto& g = ch.mesh.geom[ch.mesh.cell_geom[i]];
        out << time << ',' << i << ',' << ch.state.cell_a[i] << ','
            << ch.state.cell_q[i] << ','
            << k::depthOfArea(g, ch.state.cell_a[i]) << '\n';
    }
}

struct NodeInventory { double storage = 0, negative_carry = 0, max_abs_carry = 0; };

NodeInventory nodeInventory(const Channel& ch, const ExplicitFvSolver& solver,
                            const std::vector<double>& fixed) {
    NodeInventory result;
    const auto& carry = solver.node_continuity_carry();
    for (int n = 0; n < ch.mesh.n_nodes(); ++n) {
        if (std::isfinite(fixed[n])) continue; // external reservoir, not inventory
        const double volume = ch.state.node_volume[n];
        if (!std::isfinite(volume) || volume < 0 || !std::isfinite(carry[n]))
            throw std::runtime_error("nonfinite/negative node inventory");
        result.storage += volume;
        // Positive carry is already published as node_volume; count it once.
        result.negative_carry += std::min(0.0, carry[n]);
        result.max_abs_carry = std::max(result.max_abs_carry, std::abs(carry[n]));
    }
    return result;
}

void writeNodes(std::ostream& out, double time, const Channel& ch,
                const ExplicitFvSolver& solver, const std::vector<double>& fixed) {
    const auto& carry = solver.node_continuity_carry();
    const auto& folded = solver.pressure_folded_nodes();
    for (int n = 0; n < ch.mesh.n_nodes(); ++n) {
        if (!std::isfinite(ch.state.node_head[n])) throw std::runtime_error("nonfinite node head");
        out << time << ',' << n << ',' << ch.state.node_head[n] << ','
            << ch.state.node_volume[n] << ',' << carry[n] << ','
            << std::isfinite(fixed[n]) << ',' << (n < static_cast<int>(folded.size()) && folded[n]) << '\n';
    }
}

Config parse(int argc, char** argv) {
    if (argc != 8)
        throw std::runtime_error("usage: fv_implicit_baseline CASE CELLS CELERITY "
                                 "DURATION ROUTING_STEP PRESSURE_IMPLICIT_0_OR_1 OUTPUT_DIR");
    Config c;
    c.scenario = argv[1]; c.cells = std::stoi(argv[2]);
    c.celerity = std::stod(argv[3]); c.duration = std::stod(argv[4]);
    c.routing_step = std::stod(argv[5]); c.output = argv[7];
    const std::string mode = argv[6];
    if (mode != "0" && mode != "1") throw std::runtime_error("pressure flag must be 0 or 1");
    c.implicit_pressure = mode == "1";
    if (c.scenario != "open_slosh" && c.scenario != "pressure_slosh" &&
        c.scenario != "pressure_rest" && c.scenario != "filling" &&
        c.scenario != "pressure_flow" && !fvbaseline::networkCase(c.scenario))
        throw std::runtime_error("unknown baseline case");
    if (c.cells < 4 || c.cells > 100000 || !std::isfinite(c.celerity) ||
        c.celerity <= 0 || !std::isfinite(c.duration) || c.duration <= 0 ||
        !std::isfinite(c.routing_step) || c.routing_step <= 0)
        throw std::runtime_error("invalid baseline dimensions/times");
    if (!fs::create_directory(c.output))
        throw std::runtime_error("output directory must not already exist");
    return c;
}

void run(const Config& cfg) {
    auto ch = makeCase(cfg);
    std::vector<double> fixed(ch.mesh.n_nodes(), std::numeric_limits<double>::quiet_NaN());
    if (cfg.scenario == "filling" || cfg.scenario == "pressure_flow") {
        fixed = {6.0, cfg.scenario == "filling" ? 0.3 : 5.5};
    } else if (fvbaseline::networkCase(cfg.scenario)) {
        for (int n = 0; n < ch.mesh.n_nodes(); ++n)
            if (ch.mesh.node_kind[n] == kNodeOutfall) fixed[n] = 5.5;
        fixed[0] = fvbaseline::restCase(cfg.scenario) ? 5.5 : 6.0;
    }
    FvOptions opts = defaultOptions();
    opts.pressurized_implicit = cfg.implicit_pressure;
    opts.slot_celerity = cfg.celerity;
    opts.lts = false; // isolated global-step comparator; LTS is a later matrix
    opts.backend = Backend::CPU;
    ExplicitFvSolver solver;
    solver.initialize(ch.mesh, ch.state, opts);
    writeMesh(cfg.output / "mesh_before.csv", ch.mesh);
    auto states = output(cfg.output / "states.csv");
    states << "time_s,cell,area_ft2,discharge_cfs,depth_ft\n";
    auto nodes = output(cfg.output / "nodes.csv");
    nodes << "time_s,node,head_ft,volume_ft3,carry_ft3,prescribed_head,pressure_folded\n";
    auto trace = output(cfg.output / "routing_steps.csv");
    trace << "time_s,duration_s,internal_steps,last_internal_step_s,solver_wall_s,"
             "cell_volume_ft3,boundary_out_volume_ft3,balance_defect_ft3,"
             "pressurized_cells,transition_faces,regime_changes_since_sample,"
             "node_storage_ft3,negative_carry_ft3,flood_volume_ft3,max_folded_degree\n";
    writeState(states, 0, ch);
    writeNodes(nodes, 0, ch, solver, fixed);
    const auto initial_nodes = nodeInventory(ch, solver, fixed);
    const double initial_volume = totalVolume(ch) + initial_nodes.storage + initial_nodes.negative_carry;
    double max_defect = 0, net_boundary = 0, flood = 0, time = 0, wall = 0;
    double slot_peak = 0, max_q = 0, max_head_change = 0;
    double max_carry = 0, max_symmetry = 0, max_node_head_change = 0;
    int max_folded_degree = 0;
    long routing_steps = 0, changes = 0;
    std::vector<std::uint8_t> previous(ch.n);
    std::vector<double> initial_head(ch.n);
    const auto initial_node_head = ch.state.node_head;
    for (int i = 0; i < ch.n; ++i) {
        const auto& g = ch.mesh.geom[ch.mesh.cell_geom[i]];
        initial_head[i] = k::depthOfArea(g, ch.state.cell_a[i]);
        previous[i] = !g.is_open && initial_head[i] >= g.y_crown;
    }
    FvStepForcing forcing{};
    if (!fixed.empty()) {
        forcing.node_fixed_head = fixed.data();
        forcing.n_nodes = ch.mesh.n_nodes();
    }
    while (time < cfg.duration) {
        const double target = std::min(time + cfg.routing_step, cfg.duration);
        const double span = target - time;
        const auto start = std::chrono::steady_clock::now();
        const double reached = solver.advance(time, target, forcing);
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        wall += elapsed;
        if (!std::isfinite(reached) || std::abs(reached - target) > 1.e-10)
            throw std::runtime_error("solver did not reach the routing target");
        INetworkSolver::Divergence div;
        if (solver.divergence(div)) throw std::runtime_error("native divergence guard fired");
        time = reached; ++routing_steps;
        for (int n = 0; n < ch.mesh.n_nodes(); ++n) {
            if (std::isfinite(fixed[n])) net_boundary += solver.node_exchange()[n];
            else {
                flood += solver.node_flood_volume()[n];
                max_node_head_change = std::max(max_node_head_change,
                    std::abs(ch.state.node_head[n] - initial_node_head[n]));
            }
        }
        const double volume = totalVolume(ch);
        const auto node_inventory = nodeInventory(ch, solver, fixed);
        const double defect = volume + node_inventory.storage + node_inventory.negative_carry
                            - initial_volume + net_boundary + flood;
        max_defect = std::max(max_defect, std::abs(defect));
        max_carry = std::max(max_carry, node_inventory.max_abs_carry);
        max_symmetry = std::max(max_symmetry, fvbaseline::symmetryDifference(cfg.scenario, ch));
        const auto& folded = solver.pressure_folded_nodes();
        for (int n = 0; n < static_cast<int>(folded.size()); ++n)
            if (folded[n]) max_folded_degree = std::max(max_folded_degree,
                ch.mesh.node_face_ptr[n + 1] - ch.mesh.node_face_ptr[n]);
        long press = 0, transitions = 0, changed = 0;
        double slot = 0;
        std::vector<std::uint8_t> current(ch.n);
        for (int i = 0; i < ch.n; ++i) {
            const auto& g = ch.mesh.geom[ch.mesh.cell_geom[i]];
            const double a = ch.state.cell_a[i], q = ch.state.cell_q[i];
            if (!std::isfinite(a) || !std::isfinite(q) || a < 0)
                throw std::runtime_error("nonfinite or negative cell state");
            const double h = k::depthOfArea(g, a);
            if (!std::isfinite(h)) throw std::runtime_error("nonfinite depth");
            current[i] = !g.is_open && h >= g.y_crown;
            press += current[i]; changed += current[i] != previous[i];
            if (!g.is_open) slot += std::max(0.0, a - g.a_crown) * ch.mesh.cell_dx[i];
            max_q = std::max(max_q, std::abs(q));
            max_head_change = std::max(max_head_change, std::abs(h - initial_head[i]));
        }
        for (std::size_t f = 0; f < ch.mesh.face_cl.size(); ++f) {
            const int l = ch.mesh.face_cl[f], r = ch.mesh.face_cr[f];
            if (l >= 0 && r >= 0) transitions += current[l] != current[r];
        }
        previous = current; changes += changed;
        slot_peak = std::max(slot_peak, slot / std::max(volume, 1.e-30));
        trace << time << ',' << span << ',' << solver.last_num_steps() << ','
            << solver.last_step_size() << ',' << elapsed << ',' << volume << ','
            << net_boundary << ',' << defect << ',' << press << ',' << transitions
            << ',' << changed << ',' << node_inventory.storage << ','
            << node_inventory.negative_carry << ',' << flood << ',' << max_folded_degree << '\n';
        writeState(states, time, ch);
        writeNodes(nodes, time, ch, solver, fixed);
    }
    writeMesh(cfg.output / "mesh_after.csv", ch.mesh);
    const auto stats = solver.run_stats();
    auto summary = output(cfg.output / "summary.json");
    summary << "{\n\"schema_version\":2,\n\"case\":\"" << cfg.scenario
        << "\",\n\"units\":\"ft,cfs,s\",\n\"cells\":" << ch.n
        << ",\n\"cells_per_conduit\":" << cfg.cells
        << ",\n\"conduits\":" << ch.mesh.n_conduits()
        << ",\n\"nodes\":" << ch.mesh.n_nodes()
        << ",\n\"celerity_ft_s\":" << cfg.celerity
        << ",\n\"pressure_implicit\":" << (cfg.implicit_pressure ? "true" : "false")
        << ",\n\"lts\":false,\n\"duration_s\":" << cfg.duration
        << ",\n\"routing_step_s\":" << cfg.routing_step
        << ",\n\"routing_steps\":" << routing_steps
        << ",\n\"internal_steps\":" << stats.nsteps
        << ",\n\"substeps_per_routing_step\":" << double(stats.nsteps) / routing_steps
        << ",\n\"solver_wall_s\":" << wall
        << ",\n\"max_abs_balance_defect_ft3\":" << max_defect
        << ",\n\"max_relative_balance_defect\":" << max_defect / std::max(initial_volume, 1.0)
        << ",\n\"max_abs_discharge_cfs\":" << max_q
        << ",\n\"max_depth_change_ft\":" << max_head_change
        << ",\n\"sampled_peak_slot_share\":" << slot_peak
        << ",\n\"sampled_regime_changes\":" << changes
        << ",\n\"sampled_max_abs_node_carry_ft3\":" << max_carry
        << ",\n\"sampled_max_symmetry_diff_cfs\":" << max_symmetry
        << ",\n\"sampled_max_node_head_change_ft\":" << max_node_head_change
        << ",\n\"sampled_max_folded_degree\":" << max_folded_degree
        << ",\n\"flood_volume_ft3\":" << flood
        << ",\n\"final_conduit_discharge_cfs\":[";
    for (int e = 0; e < ch.mesh.n_conduits(); ++e) {
        if (e) summary << ',';
        const int begin = ch.mesh.conduit_cell_begin[e];
        const int count = ch.mesh.conduit_cell_count[e];
        double sum = 0, length = 0;
        for (int i = begin; i < begin + count; ++i) {
            sum += ch.state.cell_q[i] * ch.mesh.cell_dx[i];
            length += ch.mesh.cell_dx[i];
        }
        summary << sum / length;
    }
    summary << ']'
        << ",\n\"dt_argmin_pressurized\":" << stats.dt_argmin_pressurized
        << ",\n\"dt_argmin_band\":" << stats.dt_argmin_band
        << ",\n\"dt_argmin_free\":" << stats.dt_argmin_free
        << ",\n\"dt_argmin_node\":" << stats.dt_argmin_node << "\n}\n";
}
} // namespace

int main(int argc, char** argv) {
    try { run(parse(argc, argv)); return 0; }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
