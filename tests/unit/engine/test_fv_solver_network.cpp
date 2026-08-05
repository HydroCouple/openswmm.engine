/**
 * @file test_fv_solver_network.cpp
 * @brief Network-level gates for the explicit FV 1D solver (plan §6.3–6.11).
 *
 * @details
 *   §6.3   Steady-state gates: Manning normal depth on a uniform slope, and
 *          subcritical flow over a bump. The full SWASHES matrix lives in the
 *          separate epaswmm5_qa suite (its reserved `1d-fv` column); these are
 *          the in-repo gates that catch a broken bed-slope source or friction
 *          closure before that campaign runs.
 *   §6.5   Virtual-junction equivalence — including the REVERSED-orientation
 *          splice, which the [VIRTUAL_JUNCTIONS] rules permit and which is the
 *          only case where the face direction bookkeeping can be wrong.
 *   §6.6   Mixed-flow transitions: filling bore, and pressurize/depressurize
 *          cycling for hysteresis-free behaviour.
 *   §6.10  Compaction results-transparency — bit-identical, not merely close.
 *   §6.11  Scalar transport: uniform-field preservation, discrete maximum
 *          principle, solute mass conservation, the four-way front-sharpness
 *          comparison that decides D-FV2 on measurement, and the implicit
 *          dispersion gate.
 *
 * @ingroup engine_fv
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

#include "fv_test_support.hpp"

using namespace fvtest;
namespace k = openswmm::fv::kernels;

namespace {

constexpr double kG = 32.2;

/// Drive a channel with a fixed lateral inflow at node 0 and a fixed stage at
/// node 1, in `step`-second routing steps.
void runNoded(Channel& ch, const FvOptions& opts, double t_end, double step,
              double q_in, double stage_dn) {
    ExplicitFvSolver s;
    s.initialize(ch.mesh, ch.state, opts);

    std::vector<double> lateral = {q_in, 0.0};
    std::vector<double> fixed   = {std::numeric_limits<double>::quiet_NaN(),
                                   stage_dn};
    FvStepForcing f;
    f.node_lateral    = lateral.data();
    f.node_fixed_head = fixed.data();
    f.n_nodes = 2;

    for (double t = 0.0; t < t_end - 1.0e-9; t += step)
        s.advance(t, std::min(t + step, t_end), f);
}

void dump(const Channel& ch, const std::string& name) {
    std::error_code ec;
    std::filesystem::create_directories(outputDir(), ec);
    std::ofstream os(outputDir() + "/" + name + ".csv");
    if (!os) return;
    os << "x,zb,depth,area,q,eta\n";
    for (int i = 0; i < ch.n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        os << (static_cast<double>(i) + 0.5) * ch.dx << ','
           << ch.mesh.cell_zb[ui] << ',' << ch.state.cell_h[ui] << ','
           << ch.state.cell_a[ui] << ',' << ch.state.cell_q[ui] << ','
           << (ch.mesh.cell_zb[ui] + ch.state.cell_h[ui]) << '\n';
    }
}

} // namespace

// ===========================================================================
// §6.3 — Steady state
// ===========================================================================

TEST(FvNetwork, ReachesManningNormalDepthOnAUniformSlope) {
    // Wide rectangle: normal depth for discharge q per unit width is
    //   h_n = (n·q / (φ·√S))^{3/5}, using R ≈ h.
    // This is the gate that ties the bed-slope source and the friction closure
    // together — either one wrong on its own moves the equilibrium depth.
    const double width = 40.0, slope = 0.002, manning = 0.030;
    const double Q = 300.0;
    const int n = 240;
    const double dx = 25.0;

    auto bed = [&](double x) { return 50.0 - slope * x; };
    Channel ch = makeNodedChannel(rectOpen(width, 25.0), n, dx, bed, manning);

    const double q_unit = Q / width;
    const double hn = std::pow(manning * q_unit / (1.486 * std::sqrt(slope)), 0.6);
    for (int i = 0; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        ch.state.cell_a[ui] = width * hn;
        ch.state.cell_q[ui] = Q;
    }
    ch.state.node_head[0] = ch.mesh.node_invert[0] + hn;
    ch.state.node_head[1] = ch.mesh.node_invert[1] + hn;

    FvOptions o = defaultOptions();
    runNoded(ch, o, 3000.0, 60.0, Q, ch.mesh.node_invert[1] + hn);
    dump(ch, "normal_depth_uniform_slope");

    for (int i = n / 4; i < 3 * n / 4; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        EXPECT_NEAR(ch.state.cell_h[ui], hn, 0.06 * hn)
            << "depth off normal at cell " << i;
        EXPECT_NEAR(ch.state.cell_q[ui], Q, 0.06 * Q)
            << "discharge off target at cell " << i;
    }
}

TEST(FvNetwork, SubcriticalFlowOverABumpConservesEnergy) {
    // The SWASHES bump-subcritical configuration. On a frictionless bed the
    // energy head E = h + z + u²/2g is constant; a scheme that is not
    // well-balanced through the bump loses that outright.
    const double width = 10.0, Q = 45.0, L = 25.0;
    const int n = 250;
    const double dx = L / n;
    auto bed = [](double x) {
        return (x > 8.0 && x < 12.0) ? 0.2 - 0.05 * (x - 10.0) * (x - 10.0) : 0.0;
    };
    Channel ch = makeNodedChannel(rectOpen(width, 6.0), n, dx, bed, 0.0);

    const double h_out = 2.0;
    for (int i = 0; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        ch.state.cell_a[ui] = width * std::max(0.1, h_out - ch.mesh.cell_zb[ui]);
        ch.state.cell_q[ui] = Q;
    }
    ch.state.node_head[0] = h_out;
    ch.state.node_head[1] = h_out;

    FvOptions o = defaultOptions();
    runNoded(ch, o, 600.0, 5.0, Q, h_out);
    dump(ch, "bump_subcritical");

    double emin = 1.0e30, emax = -1.0e30;
    for (int i = 5; i < n - 5; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double h = ch.state.cell_h[ui];
        if (h < 1.0e-3) continue;
        const double u = ch.state.cell_q[ui] / (width * h);
        const double e = h + ch.mesh.cell_zb[ui] + u * u / (2.0 * kG);
        emin = std::min(emin, e);
        emax = std::max(emax, e);
    }
    EXPECT_LT(emax - emin, 0.08 * h_out)
        << "energy head varied by " << (emax - emin) << " ft across the bump";
}

// ===========================================================================
// §6.5 — Virtual-junction equivalence
// ===========================================================================

namespace {

/// Split a walled channel of 2·half cells into two chains joined by a virtual
/// junction. @p reversed makes the SECOND chain point back at the junction —
/// the A→VJ←B orientation the rules permit, where the face's dir bookkeeping is
/// the only thing standing between "equivalent" and "wrong sign".
Channel makeSplitChannel(const XSectParams& xs, int half, double dx,
                         double manning, bool reversed) {
    Channel ch;
    ch.n = 2 * half;
    ch.dx = dx;

    ch.mesh.geom.resize(1);
    buildGeometry(xs, xsect::isOpen(xs.type), 100.0, ch.mesh.geom[0]);
    ch.mesh.geom[0].roughness = manning;
    ch.mesh.geom[0].rough_factor = 32.2 * (manning / 1.486) * (manning / 1.486);
    ch.mesh.geom[0].barrels = 1;

    auto bed = [&](double x) { return 10.0 - 0.001 * x; };

    for (int i = 0; i < 2 * half; ++i) {
        double x;
        if (i < half || !reversed) {
            x = (static_cast<double>(i) + 0.5) * dx;
        } else {
            const int j = i - half;                 // reversed: far end first
            x = (static_cast<double>(2 * half - j) - 0.5) * dx;
        }
        ch.mesh.cell_geom.push_back(0);
        ch.mesh.cell_conduit.push_back(i < half ? 0 : 1);
        ch.mesh.cell_dx.push_back(dx);
        ch.mesh.cell_zb.push_back(bed(x));
    }

    auto add_face = [&](int cl, int cr, double zb, double fdx,
                        int8_t dl, int8_t dr, bool vj) {
        ch.mesh.face_cl.push_back(cl);
        ch.mesh.face_cr.push_back(cr);
        ch.mesh.face_node.push_back(-1);
        ch.mesh.face_zb.push_back(zb);
        ch.mesh.face_dx.push_back(fdx);
        ch.mesh.face_dir_l.push_back(dl);
        ch.mesh.face_dir_r.push_back(dr);
        ch.mesh.face_virtual.push_back(vj ? uint8_t{1} : uint8_t{0});
    };

    add_face(-1, 0, bed(0.0), 0.5 * dx, 1, 1, false);                 // wall
    for (int i = 1; i < half; ++i)
        add_face(i - 1, i, bed(i * dx), dx, 1, 1, false);

    // The virtual junction. Chain A's downstream end is always on the left.
    // When B is reversed its downstream end also meets the junction, so B's
    // axis opposes the face normal: dir_r = −1.
    const int b_first = reversed ? (2 * half - 1) : half;
    add_face(half - 1, b_first, bed(half * dx), dx, 1,
             reversed ? int8_t{-1} : int8_t{1}, true);

    if (reversed) {
        // Walking downstream means walking DOWN the index range; each such face
        // sits at both incident cells' upstream ends in their own axes.
        for (int j = 2 * half - 1; j > half; --j)
            add_face(j, j - 1, bed((2 * half - (j - half)) * dx), dx, -1, -1,
                     false);
        add_face(half, -1, bed(2 * half * dx), 0.5 * dx, -1, 1, false);
    } else {
        for (int i = half + 1; i < 2 * half; ++i)
            add_face(i - 1, i, bed(i * dx), dx, 1, 1, false);
        add_face(2 * half - 1, -1, bed(2 * half * dx), 0.5 * dx, 1, 1, false);
    }

    const int nc = 2 * half;
    ch.mesh.cell_face0.assign(static_cast<std::size_t>(nc), -1);
    ch.mesh.cell_face1.assign(static_cast<std::size_t>(nc), -1);
    ch.mesh.cell_side0.assign(static_cast<std::size_t>(nc), 0);
    ch.mesh.cell_side1.assign(static_cast<std::size_t>(nc), 0);
    for (int f = 0; f < ch.mesh.n_faces(); ++f) {
        const auto uf = static_cast<std::size_t>(f);
        auto attach = [&](int cell, int8_t side) {
            const auto uc = static_cast<std::size_t>(cell);
            if (ch.mesh.cell_face0[uc] < 0) {
                ch.mesh.cell_face0[uc] = f; ch.mesh.cell_side0[uc] = side;
            } else {
                ch.mesh.cell_face1[uc] = f; ch.mesh.cell_side1[uc] = side;
            }
        };
        if (ch.mesh.face_cl[uf] >= 0) attach(ch.mesh.face_cl[uf], 0);
        if (ch.mesh.face_cr[uf] >= 0) attach(ch.mesh.face_cr[uf], 1);
    }

    ch.mesh.chain_ptr = {0, nc};
    ch.mesh.cell_chain.assign(static_cast<std::size_t>(nc), 0);
    ch.mesh.cell_chain_pos.resize(static_cast<std::size_t>(nc));
    for (int i = 0; i < nc; ++i) {
        ch.mesh.chain_cells.push_back(i);
        ch.mesh.chain_dir.push_back(1);
        ch.mesh.cell_chain_pos[static_cast<std::size_t>(i)] = i;
    }
    ch.mesh.node_face_ptr.assign(1, 0);
    ch.state.resize(nc, 0, 0);
    return ch;
}

/// Index j of the result is the cell occupying the j-th position from upstream.
std::vector<int> physicalOrder(int half, bool reversed) {
    std::vector<int> order;
    for (int i = 0; i < half; ++i) order.push_back(i);
    if (reversed) for (int j = 2 * half - 1; j >= half; --j) order.push_back(j);
    else          for (int j = half; j < 2 * half; ++j)      order.push_back(j);
    return order;
}

} // namespace

TEST(FvNetwork, VirtualJunctionIsIndistinguishableFromAnInteriorCut) {
    const int half = 60;
    const double dx = 8.0;
    const XSectParams xs = rectOpen(15.0, 12.0);
    auto bed = [](double x) { return 10.0 - 0.001 * x; };

    auto seed = [&](Channel& c, const std::vector<int>& order) {
        for (std::size_t j = 0; j < order.size(); ++j) {
            const auto uc = static_cast<std::size_t>(order[j]);
            const double x = (static_cast<double>(j) + 0.5) * dx;
            const double eta = (x < 400.0) ? 9.0 : 6.5;     // a step to propagate
            c.state.cell_a[uc] = k::areaOfDepth(
                c.mesh.geom[0], std::max(0.0, eta - c.mesh.cell_zb[uc]));
            c.state.cell_q[uc] = 0.0;
        }
    };

    std::vector<int> identity(static_cast<std::size_t>(2 * half));
    for (int i = 0; i < 2 * half; ++i)
        identity[static_cast<std::size_t>(i)] = i;

    for (bool reversed : {false, true}) {
        Channel plain = makeWalledChannel(xs, 2 * half, dx, bed, 0.02);
        seed(plain, identity);
        Channel split = makeSplitChannel(xs, half, dx, 0.02, reversed);
        const std::vector<int> order = physicalOrder(half, reversed);
        seed(split, order);

        FvOptions o = defaultOptions();
        ExplicitFvSolver s1, s2;
        s1.initialize(plain.mesh, plain.state, o);
        s2.initialize(split.mesh, split.state, o);
        FvStepForcing f;
        for (double t = 0.0; t < 120.0; t += 10.0) {
            s1.advance(t, t + 10.0, f);
            s2.advance(t, t + 10.0, f);
        }

        // Cell-level agreement — the strongest form of the equivalence test in
        // the virtual-junction plan §9.2.
        for (std::size_t j = 0; j < order.size(); ++j) {
            const auto up = static_cast<std::size_t>(j);
            const auto us = static_cast<std::size_t>(order[j]);
            const double sgn = (reversed && order[j] >= half) ? -1.0 : 1.0;
            EXPECT_NEAR(split.state.cell_a[us], plain.state.cell_a[up], 1.0e-10)
                << (reversed ? "reversed" : "forward") << " area at position " << j;
            EXPECT_NEAR(sgn * split.state.cell_q[us], plain.state.cell_q[up], 1.0e-8)
                << (reversed ? "reversed" : "forward")
                << " discharge at position " << j;
        }
    }
}

// ===========================================================================
// §6.6 — Mixed-flow transitions
// ===========================================================================

TEST(FvNetwork, FillingBorePropagatesAndPressurizesWithoutRinging) {
    // A horizontal closed pipe, initially part-full, driven by a node head well
    // above the crown: a filling bore runs down the pipe. The gates are that it
    // ARRIVES and pressurizes, and that the crown crossing produces no
    // oscillation — which is what the tapered slot mouth exists to prevent.
    const double D = 3.0;
    const int n = 200;
    Channel ch = makeNodedChannel(circular(D), n, 10.0,
                                  [](double) { return 0.0; }, 0.013);
    for (int i = 0; i < n; ++i)
        ch.state.cell_a[static_cast<std::size_t>(i)] =
            k::areaOfDepth(ch.mesh.geom[0], 0.6);
    ch.state.node_head[0] = 12.0;      // 9 ft above the crown
    ch.state.node_head[1] = 0.6;

    FvOptions o = defaultOptions();
    ExplicitFvSolver s;
    s.initialize(ch.mesh, ch.state, o);

    std::vector<double> lateral = {0.0, 0.0};
    std::vector<double> fixed   = {12.0, 0.6};
    FvStepForcing f;
    f.node_lateral    = lateral.data();
    f.node_fixed_head = fixed.data();
    f.n_nodes = 2;

    for (double t = 0.0; t < 120.0; t += 1.0) s.advance(t, t + 1.0, f);
    dump(ch, "filling_bore");

    int pressurized = 0;
    for (int i = 0; i < n / 3; ++i)
        if (ch.state.cell_h[static_cast<std::size_t>(i)] > D) ++pressurized;
    EXPECT_GT(pressurized, n / 8) << "bore never pressurized the upstream reach";

    // Monotone depth profile: ringing at the crown crossing shows up here.
    for (int i = 1; i < n; ++i) {
        const double a = ch.state.cell_h[static_cast<std::size_t>(i - 1)];
        const double b = ch.state.cell_h[static_cast<std::size_t>(i)];
        EXPECT_LE(b, a + 1.0e-4)
            << "non-monotone depth (ringing) between cells " << i - 1 << " and " << i;
    }
}

TEST(FvNetwork, PressurizeDepressurizeCyclingIsHysteresisFree) {
    // Drive the pipe over the crown and back down twice. The static taper
    // carries no state, so the second cycle must land exactly on the first — a
    // relaxing (dynamic) slot would not, which is precisely why plan §3.3.6
    // rejects DPS for this scheme.
    const double D = 3.0;
    const int n = 60;
    Channel ch = makeNodedChannel(circular(D), n, 10.0,
                                  [](double) { return 0.0; }, 0.013);
    for (int i = 0; i < n; ++i)
        ch.state.cell_a[static_cast<std::size_t>(i)] =
            k::areaOfDepth(ch.mesh.geom[0], 1.0);
    ch.state.node_head[0] = 1.0;
    ch.state.node_head[1] = 1.0;

    FvOptions o = defaultOptions();
    ExplicitFvSolver s;
    s.initialize(ch.mesh, ch.state, o);

    std::vector<double> lateral = {0.0, 0.0};
    std::vector<double> fixed(2, 1.0);
    FvStepForcing f;
    f.node_lateral    = lateral.data();
    f.node_fixed_head = fixed.data();
    f.n_nodes = 2;

    // Hold each state long enough to SETTLE. Comparing two unsettled transients
    // would only measure how far the run got, not whether the closure carries
    // memory — the holds have to reach the rest state for the comparison to
    // mean anything.
    double t = 0.0;
    auto hold = [&](double head, double seconds) {
        fixed[0] = head; fixed[1] = head;
        for (double e = 0.0; e < seconds; e += 5.0) { s.advance(t, t + 5.0, f); t += 5.0; }
    };
    auto restError = [&](double head) {
        double worst = 0.0;
        for (int i = 0; i < n; ++i) {
            const auto ui = static_cast<std::size_t>(i);
            worst = std::max(worst, std::fabs(ch.state.cell_a[ui] -
                                              k::areaOfDepth(ch.mesh.geom[0], head)));
        }
        return worst;
    };

    hold(8.0, 1500.0);
    const std::vector<double> pressurized_1 = ch.state.cell_a;
    const double rest_hi = restError(8.0);
    dump(ch, "cycle_pressurized_1");
    hold(1.0, 1500.0);
    const std::vector<double> open_1 = ch.state.cell_a;

    hold(8.0, 1500.0);
    const std::vector<double> pressurized_2 = ch.state.cell_a;
    hold(1.0, 1500.0);
    const std::vector<double> open_2 = ch.state.cell_a;

    // Each hold reaches the analytic rest state for its head. The residual is
    // undamped sloshing, not closure error — friction is weak once the pipe
    // pressurizes because the hydraulic radius is frozen at r_full — so the
    // tolerances below bound the transient, deliberately, rather than pretending
    // a 1500 s hold converges to the last bit.
    const double a_rest = k::areaOfDepth(ch.mesh.geom[0], 8.0);
    EXPECT_LT(rest_hi, 1.0e-5 * a_rest) << "pressurized hold never settled";

    // The load-bearing assertion: the SECOND visit lands on the first. A
    // relaxing (dynamic) slot carries state across the crown crossing and would
    // differ by of order the slot storage — percent-level, four orders above
    // this bound. A static taper is memoryless and lands back on itself.
    for (int i = 0; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        EXPECT_NEAR(pressurized_2[ui], pressurized_1[ui], 1.0e-6 * a_rest)
            << "pressurized state drifted between cycles at cell " << i;
        EXPECT_NEAR(open_2[ui], open_1[ui], 1.0e-6 * a_rest)
            << "open-channel state drifted between cycles at cell " << i;
    }
}

// ===========================================================================
// §6.10 — Compaction transparency
// ===========================================================================

TEST(FvNetwork, CompactionIsBitIdenticalToTheFullSweep) {
    // A wetting front advancing into a long dry reach — the case work-list
    // compaction is designed for, and the case a too-tight active set would
    // silently truncate.
    const int n = 600;
    const double dx = 5.0;
    auto bed = [](double x) { return 20.0 - 0.004 * x; };

    auto build = [&]() {
        Channel c = makeWalledChannel(rectOpen(10.0, 15.0), n, dx, bed, 0.02);
        for (int i = 0; i < 40; ++i)
            c.state.cell_a[static_cast<std::size_t>(i)] = 10.0 * 6.0;
        return c;
    };

    Channel plain = build();
    Channel packed = build();

    FvOptions o1 = defaultOptions();  o1.compaction = false;
    FvOptions o2 = defaultOptions();  o2.compaction = true;

    ExplicitFvSolver s1, s2;
    s1.initialize(plain.mesh, plain.state, o1);
    s2.initialize(packed.mesh, packed.state, o2);

    FvStepForcing f;
    for (double t = 0.0; t < 300.0; t += 5.0) {
        s1.advance(t, t + 5.0, f);
        s2.advance(t, t + 5.0, f);
        for (int i = 0; i < n; ++i) {
            const auto ui = static_cast<std::size_t>(i);
            ASSERT_EQ(packed.state.cell_a[ui], plain.state.cell_a[ui])
                << "compaction diverged (area) at cell " << i << ", t=" << t;
            ASSERT_EQ(packed.state.cell_q[ui], plain.state.cell_q[ui])
                << "compaction diverged (flow) at cell " << i << ", t=" << t;
        }
    }
    // Confirm the skip actually engaged — otherwise the check is vacuous.
    const auto st = s2.run_stats();
    ASSERT_GE(st.active_frac_min, 0.0);
    EXPECT_LT(st.active_frac_min, 0.9)
        << "compaction never skipped anything; transparency check is vacuous";
}

// ===========================================================================
// §6.11 — Scalar transport
// ===========================================================================

namespace {

Channel makeTransportChannel(int n, double dx, int nspecies) {
    Channel ch = makeWalledChannel(rectOpen(10.0, 15.0), n, dx,
                                   [](double) { return 0.0; }, 0.0);
    ch.state.resize(n, 0, nspecies);
    return ch;
}

double soluteMass(const Channel& ch, int s) {
    const auto base = static_cast<std::size_t>(s) * static_cast<std::size_t>(ch.n);
    double m = 0.0;
    for (int i = 0; i < ch.n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        m += ch.state.cell_phi[base + ui] * ch.state.cell_a[ui] *
             ch.mesh.cell_dx[ui];
    }
    return m;
}

} // namespace

TEST(FvNetwork, UniformConcentrationStaysUniformUnderArbitraryFlow) {
    // The sharpest flux-consistency gate (§6.11a). If the species flux were
    // built from a separately-evaluated velocity rather than the SAME mass flux
    // the water used, this fails on the first substep.
    const int n = 200;
    Channel ch = makeTransportChannel(n, 5.0, 1);
    for (int i = 0; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double x = (static_cast<double>(i) + 0.5) * 5.0;
        ch.state.cell_a[ui] = 10.0 * (4.0 + 3.0 * std::sin(x / 80.0));
        ch.state.cell_q[ui] = 60.0 * std::cos(x / 60.0);
        ch.state.cell_phi[ui] = 7.25;
    }

    FvOptions o = defaultOptions();
    ExplicitFvSolver s;
    s.initialize(ch.mesh, ch.state, o);
    FvStepForcing f;
    for (double t = 0.0; t < 600.0; t += 10.0) {
        s.advance(t, t + 10.0, f);
        for (int i = 0; i < n; ++i) {
            const auto ui = static_cast<std::size_t>(i);
            if (ch.state.cell_h[ui] <= k::kDryDepth) continue;
            ASSERT_NEAR(ch.state.cell_phi[ui], 7.25, 1.0e-10)
                << "uniform field broke at cell " << i << ", t=" << t;
        }
    }
}

TEST(FvNetwork, StepConcentrationObeysTheDiscreteMaximumPrinciple) {
    const int n = 300;
    Channel base = makeTransportChannel(n, 4.0, 1);
    for (int i = 0; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        base.state.cell_a[ui] = 10.0 * 5.0;
        base.state.cell_q[ui] = 200.0;
        base.state.cell_phi[ui] = (i < n / 3) ? 100.0 : 0.0;
    }

    for (ScalarScheme sch : {ScalarScheme::UPWIND, ScalarScheme::MUSCL,
                             ScalarScheme::QUICKEST_ULTIMATE}) {
        Channel c = base;
        FvOptions o = defaultOptions();
        o.scalar_scheme = sch;
        ExplicitFvSolver s;
        s.initialize(c.mesh, c.state, o);
        FvStepForcing f;
        for (double t = 0.0; t < 60.0; t += 5.0) {
            s.advance(t, t + 5.0, f);
            for (int i = 0; i < n; ++i) {
                const double p = c.state.cell_phi[static_cast<std::size_t>(i)];
                ASSERT_GE(p, -1.0e-9)
                    << "negative concentration, scheme " << static_cast<int>(sch);
                ASSERT_LE(p, 100.0 + 1.0e-9)
                    << "new maximum, scheme " << static_cast<int>(sch);
            }
        }
    }
}

TEST(FvNetwork, SoluteMassIsConservedInAClosedReach) {
    const int n = 200;
    Channel ch = makeTransportChannel(n, 5.0, 2);
    for (int i = 0; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double x = (static_cast<double>(i) + 0.5) * 5.0;
        ch.state.cell_a[ui] = 10.0 * (5.0 + 1.5 * std::sin(x / 70.0));
        ch.state.cell_phi[ui] = (i > 60 && i < 100) ? 50.0 : 1.0;
        ch.state.cell_phi[static_cast<std::size_t>(n) + ui] =
            3.0 + 0.01 * static_cast<double>(i);
    }

    FvOptions o = defaultOptions();
    ExplicitFvSolver s;
    s.initialize(ch.mesh, ch.state, o);
    const double m0 = soluteMass(ch, 0), m1 = soluteMass(ch, 1);

    FvStepForcing f;
    for (double t = 0.0; t < 400.0; t += 10.0) s.advance(t, t + 10.0, f);

    EXPECT_NEAR(soluteMass(ch, 0), m0, 1.0e-9 * m0);
    EXPECT_NEAR(soluteMass(ch, 1), m1, 1.0e-9 * m1);
}

TEST(FvNetwork, FrontSharpnessSeparatesRiemannFromReconstruction) {
    // §6.11(d): the four-way comparison that separates the Riemann solver's
    // contribution (the contact wave) from the reconstruction's (order of
    // accuracy). Both layers are needed — HLLC alone is still first-order
    // upwind on the scalar, and no reconstruction can recover a wave HLL
    // averaged away. The measured widths are written out so D-FV2 is decided
    // on numbers rather than convention.
    const int n = 400;
    const double dx = 4.0;

    auto measureWidth = [&](RiemannSolver rs, ScalarScheme sch) {
        Channel c = makeTransportChannel(n, dx, 1);
        for (int i = 0; i < n; ++i) {
            const auto ui = static_cast<std::size_t>(i);
            c.state.cell_a[ui] = 10.0 * 5.0;
            c.state.cell_q[ui] = 250.0;
            c.state.cell_phi[ui] = (i < 80) ? 1.0 : 0.0;
        }
        FvOptions o = defaultOptions();
        o.riemann = rs;
        o.scalar_scheme = sch;
        ExplicitFvSolver s;
        s.initialize(c.mesh, c.state, o);
        FvStepForcing f;
        for (double t = 0.0; t < 200.0; t += 10.0) s.advance(t, t + 10.0, f);
        int w = 0;
        for (int i = 0; i < n; ++i) {
            const double p = c.state.cell_phi[static_cast<std::size_t>(i)];
            if (p > 0.05 && p < 0.95) ++w;
        }
        return w;
    };

    const int hll_upwind  = measureWidth(RiemannSolver::HLL,  ScalarScheme::UPWIND);
    const int hllc_upwind = measureWidth(RiemannSolver::HLLC, ScalarScheme::UPWIND);
    const int hllc_muscl  = measureWidth(RiemannSolver::HLLC, ScalarScheme::MUSCL);
    const int hllc_quick  = measureWidth(RiemannSolver::HLLC,
                                         ScalarScheme::QUICKEST_ULTIMATE);

    std::error_code ec;
    std::filesystem::create_directories(outputDir(), ec);
    std::ofstream os(outputDir() + "/front_sharpness.csv");
    os << "riemann,scalar_scheme,front_width_cells\n"
       << "HLL,UPWIND," << hll_upwind << "\n"
       << "HLLC,UPWIND," << hllc_upwind << "\n"
       << "HLLC,MUSCL," << hllc_muscl << "\n"
       << "HLLC,QUICKEST_ULTIMATE," << hllc_quick << "\n";

    EXPECT_LT(hllc_muscl, hllc_upwind) << "MUSCL no sharper than upwind";
    EXPECT_LT(hllc_quick, hllc_upwind) << "QUICKEST no sharper than upwind";
    EXPECT_LE(hllc_upwind, hll_upwind) << "HLLC smeared more than HLL";
}

TEST(FvNetwork, ImplicitDispersionRemovesTheDeltaXSquaredStepRestriction) {
    // §6.11(f): with D_L large enough that an EXPLICIT parabolic term would
    // demand dt ≤ Δx²/(2·D_L) ≈ 0.02 s — far below the CFL step — the implicit
    // solve must stay stable, monotone and conservative at the CFL step.
    const int n = 200;
    const double dx = 4.0;
    Channel ch = makeTransportChannel(n, dx, 1);
    for (int i = 0; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        ch.state.cell_a[ui] = 10.0 * 5.0;
        ch.state.cell_phi[ui] = (i == n / 2) ? 100.0 : 0.0;
    }

    FvOptions o = defaultOptions();
    o.dispersion = 400.0;
    ExplicitFvSolver s;
    s.initialize(ch.mesh, ch.state, o);
    const double m0 = soluteMass(ch, 0);

    FvStepForcing f;
    for (double t = 0.0; t < 200.0; t += 10.0) s.advance(t, t + 10.0, f);

    for (int i = 0; i < n; ++i) {
        const double p = ch.state.cell_phi[static_cast<std::size_t>(i)];
        EXPECT_TRUE(std::isfinite(p));
        EXPECT_GE(p, -1.0e-9);
        EXPECT_LE(p, 100.0 + 1.0e-9);
    }
    EXPECT_NEAR(soluteMass(ch, 0), m0, 1.0e-6 * m0);
    // And it actually spread — otherwise the gate is vacuous.
    EXPECT_LT(ch.state.cell_phi[static_cast<std::size_t>(n / 2)], 50.0);
}
