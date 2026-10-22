/**
 * @file      fv_test_support.hpp
 * @brief     Shared mesh builders for the explicit-FV 1D solver unit tests.
 *
 * @details Builds NetworkMeshData directly, without a SimulationContext, so the
 *          analytic gates in plan §6.1–6.6 exercise the scheme rather than the
 *          engine wiring. Every artefact these tests write goes to a reviewable
 *          location under tests/output (CLAUDE.md §4.1) — nothing lands in a
 *          temporary directory.
 *
 * @ingroup engine_fv
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_TESTS_FV_TEST_SUPPORT_HPP
#define OPENSWMM_TESTS_FV_TEST_SUPPORT_HPP

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "hydraulics/fv/ExplicitFvSolver.hpp"
#include "hydraulics/fv/NetworkMeshBuilder.hpp"
#include "hydraulics/XSectBatch.hpp"
#include "core/Constants.hpp"

namespace fvtest {

using namespace openswmm;
using namespace openswmm::fv;

/// A straight prismatic channel of @p n cells with WALL boundaries at both
/// ends (no node coupling). `bed(x)` gives the bed elevation at distance x from
/// the upstream end, so slope breaks and bumps are expressible.
struct Channel {
    NetworkMeshData mesh;
    NetworkStateData state;

    double dx = 0.0;
    int n = 0;
};

/**
 * @brief Build an open rectangular cross section in internal US length units.
 *
 * Ignores setParams status; caller supplies physically valid dimensions for meaningful fixture
 * geometry.
 *
 * @param[in] width Positive bottom width in feet.
 * @param[in] y_full Positive full depth in feet.
 * @return Owned section parameters initialized with unit conversion factor one.
 */
inline XSectParams rectOpen(double width, double y_full) {
    XSectParams xs;
    xs.type = static_cast<int>(XSectShape::RECT_OPEN);
    double p[4] = {y_full, width, 0.0, 0.0};
    xsect::setParams(xs, xs.type, p, 1.0);
    return xs;
}

/**
 * @brief Build a closed circular cross section in internal US length units.
 *
 * Ignores setParams status; no file or project table is used.
 *
 * @param[in] d Positive diameter in feet.
 * @return Owned circular parameters initialized with unit conversion factor one.
 */
inline XSectParams circular(double d) {
    XSectParams xs;
    xs.type = static_cast<int>(XSectShape::CIRCULAR);
    double p[4] = {d, 0.0, 0.0, 0.0};
    xsect::setParams(xs, xs.type, p, 1.0);
    return xs;
}

/**
 * @brief Build an owned prismatic finite-volume channel with reflecting end walls.
 *
 * Stores bed centers/face elevations and centered cell slopes from bedfn. Initializes dry/zero-flow
 * state, one barrel and no losses/culvert effects; no solver step occurs. Caller supplies valid
 * geometry/counts and side-effect-safe callable; allocation/callable exceptions propagate.
 *
 * @param[in] xs Borrowed initialized section geometry in feet, copied into owned FV geometry.
 * @param[in] n Strictly positive cell count.
 * @param[in] dx Positive uniform cell length in feet.
 * @param[in] bedfn Copyable callable mapping x in feet over [0,n*dx] to bed elevation in feet;
 *     invoked synchronously and not retained.
 * @param[in] manning Nonnegative Manning roughness coefficient; native US grouping uses 1.486.
 * @param[in] slot_celerity Positive closed-section slot wave speed in ft/s.
 * @return Owned zero-species channel with one geometry/conduit/chain, n cells, n+1 faces and no
 *     nodes.
 */
template <class BedFn>
inline Channel makeWalledChannel(const XSectParams& xs, int n, double dx, BedFn bedfn,
                                 double manning, double slot_celerity = 100.0) {
    Channel ch;
    ch.n = n;
    ch.dx = dx;

    ch.mesh.geom.resize(1);
    buildGeometry(xs, xsect::isOpen(xs.type), slot_celerity, ch.mesh.geom[0]);
    ch.mesh.geom[0].barrels = 1;

    ch.mesh.conduit_cell_begin = {0};
    ch.mesh.conduit_cell_count = {n};
    ch.mesh.conduit_link = {0};
    ch.mesh.conduit_section = {0};
    ch.mesh.conduit_roughness = {manning};
    // g·(n/φ)², the same grouping ConduitData::rough_factor carries.
    ch.mesh.conduit_rough_factor = {32.2 * (manning / 1.486) * (manning / 1.486)};
    ch.mesh.conduit_loss_inlet = {0.0};
    ch.mesh.conduit_loss_outlet = {0.0};
    ch.mesh.conduit_slope = {0.0};
    ch.mesh.conduit_culvert_code = {0};
    ch.mesh.conduit_culvert_curve = {hydkernels::CulvertCurve{}};
    ch.mesh.conduit_culvert_mitered = {0};

    for (int i = 0; i < n; ++i) {
        ch.mesh.cell_geom.push_back(0);
        ch.mesh.cell_conduit.push_back(0);
        ch.mesh.cell_dx.push_back(dx);
        const double xc = (static_cast<double>(i) + 0.5) * dx;
        ch.mesh.cell_zb.push_back(bedfn(xc));
        ch.mesh.cell_dzdx.push_back((bedfn(xc + 0.5 * dx) - bedfn(xc - 0.5 * dx)) / dx);
    }

    auto add_face = [&](int cl, int cr, int node, double zb, double fdx) {
        ch.mesh.face_cl.push_back(cl);
        ch.mesh.face_cr.push_back(cr);
        ch.mesh.face_node.push_back(node);
        ch.mesh.face_zb.push_back(zb);
        ch.mesh.face_dx.push_back(fdx);
        ch.mesh.face_dir_l.push_back(1);
        ch.mesh.face_dir_r.push_back(1);
        ch.mesh.face_virtual.push_back(0);
        ch.mesh.face_vj_node.push_back(-1);
    };

    add_face(-1, 0, -1, bedfn(0.0), 0.5 * dx); // upstream wall
    for (int i = 1; i < n; ++i)
        add_face(i - 1, i, -1, bedfn(static_cast<double>(i) * dx), dx);
    add_face(n - 1, -1, -1, bedfn(static_cast<double>(n) * dx), 0.5 * dx); // wall

    // Cell → face back-references (same construction the real builder does).
    ch.mesh.cell_face0.assign(static_cast<std::size_t>(n), -1);
    ch.mesh.cell_face1.assign(static_cast<std::size_t>(n), -1);
    ch.mesh.cell_side0.assign(static_cast<std::size_t>(n), 0);
    ch.mesh.cell_side1.assign(static_cast<std::size_t>(n), 0);
    for (int f = 0; f < ch.mesh.n_faces(); ++f) {
        const auto uf = static_cast<std::size_t>(f);
        auto attach = [&](int cell, int8_t side) {
            const auto uc = static_cast<std::size_t>(cell);
            if (ch.mesh.cell_face0[uc] < 0) {
                ch.mesh.cell_face0[uc] = f;
                ch.mesh.cell_side0[uc] = side;
            } else {
                ch.mesh.cell_face1[uc] = f;
                ch.mesh.cell_side1[uc] = side;
            }
        };
        if (ch.mesh.face_cl[uf] >= 0)
            attach(ch.mesh.face_cl[uf], 0);
        if (ch.mesh.face_cr[uf] >= 0)
            attach(ch.mesh.face_cr[uf], 1);
    }

    // One chain covering the whole channel.
    ch.mesh.chain_ptr = {0, n};
    ch.mesh.cell_chain.assign(static_cast<std::size_t>(n), 0);
    ch.mesh.cell_chain_pos.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        ch.mesh.chain_cells.push_back(i);
        ch.mesh.chain_dir.push_back(1);
        ch.mesh.cell_chain_pos[static_cast<std::size_t>(i)] = i;
    }

    ch.mesh.node_face_ptr.assign(1, 0); // zero nodes
    ch.state.resize(n, 0, 0);
    return ch;
}

/**
 * @brief Build an owned channel coupled to an upstream junction and downstream outfall.
 *
 * Begins from makeWalledChannel, replaces end walls with node coupling, sizes all node
 * geometry/adjacency arrays and uses MIN_SURFAREA for junction mixing/storage. Full depth is 1e6 ft
 * to avoid fixture flooding. Caller supplies lateral inflow/fixed stage through FvStepForcing; does
 * not integrate a simulation.
 *
 * @param[in] xs Borrowed initialized section geometry in feet, copied into owned FV geometry.
 * @param[in] n Strictly positive cell count.
 * @param[in] dx Positive uniform cell length in feet.
 * @param[in] bedfn Copyable callable mapping x in feet over [0,n*dx] to bed elevation in feet;
 *     invoked synchronously and not retained.
 * @param[in] manning Nonnegative Manning roughness coefficient; native US grouping uses 1.486.
 * @param[in] slot_celerity Positive closed-section slot wave speed in ft/s.
 * @return Owned channel with two coupling faces/nodes, dry state and node heads at their invert
 *     elevations.
 */
template <class BedFn>
inline Channel makeNodedChannel(const XSectParams& xs, int n, double dx, BedFn bedfn,
                                double manning, double slot_celerity = 100.0) {
    Channel ch = makeWalledChannel(xs, n, dx, bedfn, manning, slot_celerity);

    // Convert the two wall faces into node-coupling faces.
    const int f_up = 0;
    const int f_dn = ch.mesh.n_faces() - 1;
    ch.mesh.face_node[static_cast<std::size_t>(f_up)] = 0;
    ch.mesh.face_node[static_cast<std::size_t>(f_dn)] = 1;

    ch.mesh.node_invert = {bedfn(0.0), bedfn(static_cast<double>(n) * dx)};
    ch.mesh.node_full_depth = {1.0e6, 1.0e6};
    ch.mesh.node_ponded_area = {0.0, 0.0};
    ch.mesh.node_kind = {kNodeJunction, kNodeOutfall};
    // Storage area of a non-storage node — the engine's own convention, fixed
    // for the run (see NetworkMeshData::node_area). Every array indexed by node
    // has to be sized here; the solver reads node_area every substep.
    ch.mesh.node_area = {openswmm::constants::MIN_SURFAREA, openswmm::constants::MIN_SURFAREA};
    ch.mesh.node_vol_off = {-1, -1};
    ch.mesh.node_vol_dmax = {0.0, 0.0};
    ch.mesh.node_vol_atop = {0.0, 0.0};

    ch.mesh.node_face_ptr = {0, 1, 2};
    ch.mesh.node_face_idx = {f_up, f_dn};
    ch.mesh.node_face_sign = {-1.0, 1.0}; // +1 = flux ENTERS the node
    ch.mesh.node_face_zb = {bedfn(0.0), bedfn(static_cast<double>(n) * dx)};

    ch.state.resize(n, 2, 0);
    ch.state.node_head[0] = ch.mesh.node_invert[0];
    ch.state.node_head[1] = ch.mesh.node_invert[1];
    return ch;
}

/**
 * @brief Set all channel cells to a common free-surface elevation and zero discharge.
 *
 * Computes area from max(0,eta-bed) for each cell. Does not seed node heads, species, old-state
 * arrays or ledgers; caller performs any additional solver initialization.
 *
 * @param[in,out] ch Owned fixture with valid first geometry and n cell arrays.
 * @param[in] eta Water-surface elevation in feet; cells above it are dry.
 */
inline void seedLevel(Channel& ch, double eta) {
    const FvGeometry& g = ch.mesh.geom[0];
    for (int i = 0; i < ch.n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double h = std::max(0.0, eta - ch.mesh.cell_zb[ui]);
        ch.state.cell_a[ui] = kernels::areaOfDepth(g, h);
        ch.state.cell_q[ui] = 0.0;
    }
}

/**
 * @brief Sum water volume stored in channel cells.
 *
 * Does not include exchange queues or mutate state.
 *
 * @param[in] ch Borrowed consistent channel cell-area/length arrays.
 * @return Sum of cell area times cell length in ft3; excludes node storage.
 */
inline double totalVolume(const Channel& ch) {
    double v = 0.0;
    for (int i = 0; i < ch.n; ++i)
        v += ch.state.cell_a[static_cast<std::size_t>(i)] *
             ch.mesh.cell_dx[static_cast<std::size_t>(i)];
    return v;
}

/**
 * @brief Sum the channel discharge-length momentum proxy.
 *
 * Uses ordered double summation without mutating state.
 *
 * @param[in] ch Borrowed consistent channel discharge/length arrays.
 * @return Sum of Q*dx in ft4/s; excludes nodes and boundary fluxes.
 */
inline double totalMomentum(const Channel& ch) {
    double m = 0.0;
    for (int i = 0; i < ch.n; ++i)
        m += ch.state.cell_q[static_cast<std::size_t>(i)] *
             ch.mesh.cell_dx[static_cast<std::size_t>(i)];
    return m;
}

/**
 * @brief Choose default analytic-gate finite-volume options.
 *
 * Does not initialize a solver or modify global settings; individual gates may override the
 * returned value.
 * @return Owned options with HLLC flux, CFL 0.5 and compaction disabled; other fields retain
 *     defaults.
 */
inline FvOptions defaultOptions() {
    FvOptions o;
    o.cfl = 0.5;
    o.riemann = RiemannSolver::HLLC;
    o.compaction = false;
    return o;
}

/// Directory every test artefact is written to — reviewable, not temporary.
inline std::string outputDir() {
    return "tests/output/fv";
}

} // namespace fvtest

#endif // OPENSWMM_TESTS_FV_TEST_SUPPORT_HPP
