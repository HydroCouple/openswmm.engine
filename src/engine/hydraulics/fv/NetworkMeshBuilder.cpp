/**
 * @file NetworkMeshBuilder.cpp
 * @brief Implementation of the FV network mesh construction.
 *
 * @see NetworkMeshBuilder.hpp
 * @ingroup engine_fv
 */

#include "NetworkMeshBuilder.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

#include "FvKernels.hpp"
#include "../Culvert.hpp"
#include "../Link.hpp"
#include "../Node.hpp"
#include "../../core/Constants.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../core/UnitConversion.hpp"

namespace openswmm::fv {

namespace {

/// One end of one conduit attached to a node.
struct Attachment {
    int conduit = -1;   ///< ConduitData row
    int end     = 0;    ///< 0 = conduit's upstream (node1) end, 1 = downstream
};

/// Composite integration of A(h) over [0, y_full] onto the kI1Samples grid.
/// Each table interval is integrated with kSub Simpson panels of the exact
/// closure, so the tabulated I₁ is far more accurate than a trapezoid over the
/// coarse grid would be — it is the pressure term of every momentum flux.
void buildI1Table(FvGeometry& g) {
    constexpr int kSub = 8;                      // Simpson panels per interval
    const int n = kI1Samples;
    for (int q = 0; q < 2 * n; ++q) g.i1_tbl[q] = 0.0;
    if (g.y_full <= 0.0) return;

    const double dh = g.y_full / static_cast<double>(n - 1);
    const double hs = dh / static_cast<double>(2 * kSub);   // Simpson half-panel

    double acc = 0.0;
    g.i1_tbl[0] = 0.0;
    g.i1_tbl[static_cast<std::size_t>(n)] = 0.0;            // A(0) = 0
    for (int i = 1; i < n; ++i) {
        const double h0 = static_cast<double>(i - 1) * dh;
        // Composite Simpson over [h0, h0 + dh].
        double s = kernels::areaOfDepth(g, h0) +
                   kernels::areaOfDepth(g, h0 + dh);
        for (int k = 1; k < 2 * kSub; ++k) {
            const double hk = h0 + static_cast<double>(k) * hs;
            s += ((k & 1) ? 4.0 : 2.0) * kernels::areaOfDepth(g, hk);
        }
        acc += s * hs / 3.0;
        const double h_i = static_cast<double>(i) * dh;
        g.i1_tbl[static_cast<std::size_t>(i)] = acc;
        g.i1_tbl[static_cast<std::size_t>(n + i)] = kernels::areaOfDepth(g, h_i);
    }
    g.i1_crown = acc;
}

/// The area-uniform inverse table, built from the bracketed inverse so the
/// fast path and the slow one converge to the same root by construction. Must
/// run AFTER buildI1Table and after a_crown is set — it inverts what they
/// produced.
void buildDepthTable(FvGeometry& g) {
    const int n = kI1Samples;
    for (int j = 0; j < n; ++j) g.h_tbl[j] = 0.0;
    if (!(g.a_crown > 0.0) || g.y_full <= 0.0) return;

    const double da = g.a_crown / static_cast<double>(n - 1);
    g.h_tbl[0] = 0.0;
    for (int j = 1; j < n - 1; ++j)
        g.h_tbl[j] = kernels::depthOfAreaBracketed(g, static_cast<double>(j) * da);
    g.h_tbl[n - 1] = g.y_full;              // A(y_full) == a_crown by definition
}

} // namespace

// ===========================================================================
// buildGeometry
// ===========================================================================

void buildGeometry(const XSectParams& xs, bool is_open, double slot_celerity,
                   FvGeometry& g, int barrels) {
    g.xs      = xs;
    g.barrels = std::max(1, barrels);
    g.barrel_scale = static_cast<double>(g.barrels);
    // Host binding by default. A device backend rebinds this to its own
    // evaluator over device copies of the same tables (plan §5.1); nothing else
    // about the geometry changes.
    g.eval    = &xsect::hostEval();
    // Area and width are aggregate over the barrels; depth and hydraulic radius
    // are per barrel and unscaled.
    g.y_full  = xs.y_full;
    g.a_full  = g.barrel_scale * xs.a_full;
    g.w_max   = g.barrel_scale * xs.w_max;
    g.r_full  = xs.r_full;
    g.is_open = is_open ? uint8_t{1} : uint8_t{0};

    if (is_open) {
        // No crown: the section simply continues with vertical walls above
        // y_full. One code path with the closed case — the "slot" width is just
        // the section's own top width there, so celerity stays physical.
        g.y_crown = xs.y_full;
        g.t_slot  = (g.w_max > 0.0) ? g.w_max : 1.0;
    } else {
        // T_slot from the DESIGN celerity: c = √(g·A_full/T_slot). Making the
        // celerity the user-facing knob (FV_SLOT_CELERITY) rather than the
        // width keeps the accuracy/cost trade explicit — physical acoustic
        // speeds (~1000+ ft/s) would crush the global CFL step.
        g.y_crown = constants::SLOT_CROWN_CUTOFF * xs.y_full;
        const double c = (slot_celerity > 1.0) ? slot_celerity : 1.0;
        g.t_slot = kernels::kGravity * g.a_full / (c * c);
        // Never let the slot be wider than the section it pressurizes — that
        // would make the "slot" the dominant storage and understate surge.
        const double cap = 0.05 * ((g.w_max > 0.0) ? g.w_max : 1.0);
        if (g.t_slot > cap) g.t_slot = cap;
        if (g.t_slot <= 0.0) g.t_slot = 1.0e-6;
    }

    const double band = g.y_full - g.y_crown;
    g.a_crown = g.a_full + g.t_slot * band * 0.5;   // ∫₀¹ ramp = ½

    buildI1Table(g);
    buildDepthTable(g);
}

// ===========================================================================
// buildNetworkMesh
// ===========================================================================

MeshBuildReport buildNetworkMesh(SimulationContext& ctx,
                                 const FvOptions& opts,
                                 NetworkMeshData& mesh) {
    MeshBuildReport rep;
    mesh.clear();

    const int n_links = ctx.n_links();
    const int n_nodes = ctx.n_nodes();
    const auto& CD    = ctx.link_subtypes.conduits;
    const int n_cond  = CD.count();

    // -----------------------------------------------------------------------
    // Nodes
    // -----------------------------------------------------------------------
    mesh.node_invert.resize(static_cast<std::size_t>(n_nodes));
    mesh.node_full_depth.resize(static_cast<std::size_t>(n_nodes));
    mesh.node_ponded_area.resize(static_cast<std::size_t>(n_nodes));
    mesh.node_sur_depth.resize(static_cast<std::size_t>(n_nodes));
    mesh.node_can_pond.resize(static_cast<std::size_t>(n_nodes));
    mesh.node_kind.resize(static_cast<std::size_t>(n_nodes));
    mesh.node_area.assign(static_cast<std::size_t>(n_nodes),
                          constants::MIN_SURFAREA);
    for (int i = 0; i < n_nodes; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        mesh.node_invert[ui]      = ctx.nodes.invert_elev[ui];
        mesh.node_full_depth[ui]  = ctx.nodes.full_depth[ui];
        mesh.node_ponded_area[ui] = ctx.nodes.ponded_area[ui];
        mesh.node_sur_depth[ui]   = ctx.nodes.sur_depth[ui];
        // Same eligibility rule as DW (DynamicWave.cpp getFloodedDepth): the
        // project option gates ponding, and a 2D-coupled node ponds regardless
        // because its rim is the surface exchange.
        const bool is_coupled =
            (ui < ctx.coupled_node.size() && ctx.coupled_node[ui]);
        mesh.node_can_pond[ui] = static_cast<uint8_t>(
            (ctx.options.allow_ponding || is_coupled) &&
            ctx.nodes.ponded_area[ui] > 0.0 &&
            ctx.nodes.type[ui] != NodeType::OUTFALL);
        uint8_t kind = kNodeJunction;
        if (ui < ctx.nodes.is_virtual.size() && ctx.nodes.is_virtual[ui])
            kind = kNodeVirtual;
        else if (ctx.nodes.type[ui] == NodeType::STORAGE) kind = kNodeStorage;
        else if (ctx.nodes.type[ui] == NodeType::OUTFALL) kind = kNodeOutfall;
        mesh.node_kind[ui] = kind;

        // Match node::getVolume's JUNCTION branch exactly: V = full_volume *
        // depth / full_depth, with full_volume defaulting to MIN_SURFAREA *
        // full_depth. Storage nodes use their real curve instead (below).
        const double fd = ctx.nodes.full_depth[ui];
        const double fvol = ctx.nodes.full_volume[ui];
        mesh.node_area[ui] =
            (fd > 0.0 && fvol > 0.0) ? fvol / fd : constants::MIN_SURFAREA;
    }

    // Flatten STORAGE geometry into a monotone depth→volume table so the solver
    // can invert volume → depth without reaching into SimulationContext (the
    // GPU plugin cannot). The engine's own node::getVolume remains the
    // authority for reported volumes — the Router glue recomputes them from the
    // published depth — so this table only has to be accurate enough to march.
    mesh.node_vol_off.assign(static_cast<std::size_t>(n_nodes), -1);
    mesh.node_vol_dmax.assign(static_cast<std::size_t>(n_nodes), 0.0);
    mesh.node_vol_atop.assign(static_cast<std::size_t>(n_nodes), 0.0);
    {
        const int us = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
        for (int i = 0; i < n_nodes; ++i) {
            const auto ui = static_cast<std::size_t>(i);
            if (mesh.node_kind[ui] != kNodeStorage) continue;
            double dmax = ctx.nodes.full_depth[ui] + ctx.nodes.sur_depth[ui];
            if (!(dmax > 0.0)) dmax = std::max(ctx.nodes.full_depth[ui], 1.0);
            mesh.node_vol_off[ui]  = static_cast<int>(mesh.node_vol_tbl.size());
            mesh.node_vol_dmax[ui] = dmax;
            const double dd = dmax / static_cast<double>(kNodeVolSamples - 1);
            for (int k = 0; k < kNodeVolSamples; ++k) {
                const double d = static_cast<double>(k) * dd;
                mesh.node_vol_tbl.push_back(
                    node::getVolume(ctx.nodes, i, d, &ctx.tables, us,
                                    &ctx.node_subtypes));
            }
            mesh.node_vol_atop[ui] = std::max(
                node::getSurfArea(ctx.nodes, i, dmax, &ctx.tables, us,
                                  &ctx.node_subtypes),
                constants::MIN_SURFAREA);
        }
    }

    // -----------------------------------------------------------------------
    // Per-conduit geometry + cell allocation
    // -----------------------------------------------------------------------
    mesh.geom.resize(static_cast<std::size_t>(n_cond));
    mesh.conduit_cell_begin.assign(static_cast<std::size_t>(n_cond), -1);
    mesh.conduit_cell_count.assign(static_cast<std::size_t>(n_cond), 0);
    mesh.conduit_link.assign(static_cast<std::size_t>(n_cond), -1);

    rep.min_dx = 1.0e30;

    // Tabulation memo. buildGeometry() runs ~2,200 areaOfDepth evaluations plus
    // 127 bracketed root solves per call, and its result depends only on
    // (cross-section, open/closed, slot celerity, barrels) — slot celerity
    // being constant across this loop. Real models have tens of distinct
    // sections and can have hundreds of thousands of conduits, so without a
    // memo this dominates initialize().
    //
    // The key is an explicit tuple of every XSectParams field rather than the
    // struct's bytes: the struct has padding, whose contents are unspecified,
    // so memcmp would report spurious mismatches. The three table pointers are
    // part of the key — same pointer means the same transect table, and two
    // identical tables at different addresses merely miss the memo, which is
    // safe. (After the STREET/CUSTOM memoization in the resolver, links sharing
    // a street already share one table address, so they hit.)
    using GeomKey = std::tuple<int, int, int,
                               double, double, double, double, double, double,
                               double, double, double, double, double,
                               const double*, const double*, const double*,
                               int, int, bool>;
    const auto geom_key = [](const XSectParams& x, int barrels, bool is_open) {
        return GeomKey{x.type, x.culvert_code, x.transect,
                       x.y_full, x.w_max, x.yw_max, x.a_full, x.r_full,
                       x.s_full, x.s_max, x.y_bot, x.a_bot, x.s_bot, x.r_bot,
                       x.area_tbl, x.hrad_tbl, x.width_tbl,
                       x.transect_tbl_size, barrels, is_open};
    };
    std::map<GeomKey, int> geom_memo;   // key -> conduit row already tabulated

    for (int r = 0; r < n_cond; ++r) {
        const auto ur = static_cast<std::size_t>(r);
        const int j = CD.link_idx[ur];
        if (j < 0 || j >= n_links) continue;
        const auto uj = static_cast<std::size_t>(j);
        mesh.conduit_link[ur] = j;

        XSectParams xs = link::buildXSectParams(ctx.links, uj, &ctx.transect_tables);
        if (xs.y_full <= 0.0 || xs.a_full <= 0.0) {
            // A control volume needs a real section. DUMMY-shape conduits and
            // links whose geometry never resolved cannot be marched.
            rep.errors.push_back(
                "FV routing: conduit '" + ctx.link_names.name_of(j) +
                "' has no usable cross-section (full depth/area is zero). "
                "FLOW_ROUTING FV requires every conduit to carry real geometry.");
            continue;
        }

        auto& g = mesh.geom[ur];
        const bool is_open = xsect::isOpen(xs.type);
        if (const auto hit = geom_memo.find(geom_key(xs, CD.barrels[ur], is_open));
            hit != geom_memo.end()) {
            // Copy the already-tabulated geometry wholesale. The per-conduit
            // scalars below are assigned after this point in both branches, so
            // the result is identical to re-running buildGeometry.
            g = mesh.geom[static_cast<std::size_t>(hit->second)];
        } else {
            buildGeometry(xs, is_open, opts.slot_celerity, g, CD.barrels[ur]);
            geom_memo.emplace(geom_key(xs, CD.barrels[ur], is_open), r);
        }
        g.roughness    = CD.roughness[ur];
        g.rough_factor = CD.rough_factor[ur];
        g.loss_inlet   = CD.loss_inlet[ur];
        g.loss_outlet  = CD.loss_outlet[ur];
        g.culvert_code = CD.culvert_code[ur];
        g.slope        = CD.slope[ur];
        if (g.culvert_code > 0) {
            const culvert::CulvertCoeffs cc = culvert::getCoeffs(g.culvert_code);
            g.culvert_curve = {cc.K, cc.M, cc.C, cc.Y};
            g.culvert_mitered = static_cast<uint8_t>(
                g.culvert_code == 5 || g.culvert_code == 37 ||
                g.culvert_code == 46);
        }

        // Mesh length: the Courant-lengthened mod_length is reused as the Δx
        // floor in BOTH modes (plan §3.2). Router::init has already adjusted
        // slope and roughness for the same factor, so bed slope (drop/mod_length)
        // and rough_factor stay mutually consistent.
        const double len  = CD.length[ur];
        const double mlen = CD.mod_length[ur];
        const double L    = std::max({len, mlen, constants::FUDGE});

        // FV_MIN_CELLS is a FLOOR on the discretization, so it applies whether
        // or not FV_CELL_LENGTH asked for a target Δx. Applying it only inside
        // the FINE branch made it inert on its own — another parsed knob that
        // did nothing unless a second one was also set.
        int ncell = 1;                                   // COARSE default
        if (opts.cell_length > 0.0)                      // FINE
            ncell = static_cast<int>(std::ceil(L / opts.cell_length));
        ncell = std::max({ncell, opts.min_cells, 1});
        mesh.conduit_cell_begin[ur] = mesh.n_cells();
        mesh.conduit_cell_count[ur] = ncell;

        const double dx = L / static_cast<double>(ncell);
        rep.min_dx = std::min(rep.min_dx, dx);
        rep.max_dx = std::max(rep.max_dx, dx);

        const int n1 = ctx.links.node1[uj];
        const int n2 = ctx.links.node2[uj];
        const double z1 = ctx.nodes.invert_elev[static_cast<std::size_t>(n1)] +
                          ctx.links.offset1[uj];
        const double z2 = ctx.nodes.invert_elev[static_cast<std::size_t>(n2)] +
                          ctx.links.offset2[uj];

        for (int i = 0; i < ncell; ++i) {
            const double t = (static_cast<double>(i) + 0.5) /
                             static_cast<double>(ncell);
            mesh.cell_geom.push_back(r);
            mesh.cell_conduit.push_back(r);
            mesh.cell_dx.push_back(dx);
            mesh.cell_zb.push_back(z1 + (z2 - z1) * t);
            mesh.cell_dzdx.push_back((z2 - z1) / L);
        }
    }
    if (!rep.errors.empty()) return rep;
    if (rep.min_dx > 1.0e29) rep.min_dx = 0.0;

    // -----------------------------------------------------------------------
    // Interior faces inside each conduit chain
    // -----------------------------------------------------------------------
    auto add_face = [&](int cl, int cr, int node, double zb, double dx,
                        int8_t dl, int8_t dr, bool is_vj) {
        mesh.face_cl.push_back(cl);
        mesh.face_cr.push_back(cr);
        mesh.face_node.push_back(node);
        mesh.face_zb.push_back(zb);
        mesh.face_dx.push_back(dx);
        mesh.face_dir_l.push_back(dl);
        mesh.face_dir_r.push_back(dr);
        mesh.face_virtual.push_back(is_vj ? uint8_t{1} : uint8_t{0});
        mesh.face_gate.push_back(0);
        mesh.face_culvert.push_back(-1);
    };

    for (int r = 0; r < n_cond; ++r) {
        const auto ur = static_cast<std::size_t>(r);
        const int begin = mesh.conduit_cell_begin[ur];
        const int count = mesh.conduit_cell_count[ur];
        if (begin < 0 || count < 2) continue;
        const int j = mesh.conduit_link[ur];
        const auto uj = static_cast<std::size_t>(j);
        const int n1 = ctx.links.node1[uj];
        const int n2 = ctx.links.node2[uj];
        const double z1 = ctx.nodes.invert_elev[static_cast<std::size_t>(n1)] +
                          ctx.links.offset1[uj];
        const double z2 = ctx.nodes.invert_elev[static_cast<std::size_t>(n2)] +
                          ctx.links.offset2[uj];
        const double dx = mesh.cell_dx[static_cast<std::size_t>(begin)];
        for (int i = 1; i < count; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(count);
            add_face(begin + i - 1, begin + i, -1, z1 + (z2 - z1) * t, dx,
                     1, 1, false);
        }
    }

    // -----------------------------------------------------------------------
    // Node attachments — boundary faces, and virtual-junction splices
    // -----------------------------------------------------------------------
    std::vector<std::vector<Attachment>> attach(static_cast<std::size_t>(n_nodes));
    for (int r = 0; r < n_cond; ++r) {
        const auto ur = static_cast<std::size_t>(r);
        if (mesh.conduit_cell_begin[ur] < 0) continue;
        const int j = mesh.conduit_link[ur];
        const auto uj = static_cast<std::size_t>(j);
        const int n1 = ctx.links.node1[uj];
        const int n2 = ctx.links.node2[uj];
        if (n1 >= 0 && n1 < n_nodes) attach[static_cast<std::size_t>(n1)].push_back({r, 0});
        if (n2 >= 0 && n2 < n_nodes) attach[static_cast<std::size_t>(n2)].push_back({r, 1});
    }

    auto chain_end_cell = [&](const Attachment& a) {
        const auto ur = static_cast<std::size_t>(a.conduit);
        const int begin = mesh.conduit_cell_begin[ur];
        const int count = mesh.conduit_cell_count[ur];
        return (a.end == 0) ? begin : begin + count - 1;
    };
    auto invert_at = [&](const Attachment& a) {
        const int j = mesh.conduit_link[static_cast<std::size_t>(a.conduit)];
        const auto uj = static_cast<std::size_t>(j);
        const int n = (a.end == 0) ? ctx.links.node1[uj] : ctx.links.node2[uj];
        const double off = (a.end == 0) ? ctx.links.offset1[uj] : ctx.links.offset2[uj];
        return ctx.nodes.invert_elev[static_cast<std::size_t>(n)] + off;
    };

    // -----------------------------------------------------------------------
    // Flap gates, per conduit
    // -----------------------------------------------------------------------
    // DW applies two checks to a conduit's flow (DynamicWave.cpp
    // getConduitFlow): the link's own [LOSSES] gate zeroes q whenever it
    // opposes links.direction, and a gated OUTFALL at either end zeroes any q
    // that would leave that outfall. Both act on the SINGLE link flow, so each
    // seals the whole conduit, not one end of it — an outfall gate blocking
    // only its own face let the pipe drain backwards out its other end.
    //
    // Both reduce to a blocked sign along the chain, and a chain-positive flow
    // is a positive face flux at BOTH boundary faces (upstream the node is on
    // the face's left, downstream it is on the right), so one mask per conduit
    // applies unchanged to both. Bit 0 blocks positive, bit 1 blocks negative.
    std::vector<uint8_t> conduit_gate(static_cast<std::size_t>(n_cond), 0);
    for (int r = 0; r < n_cond; ++r) {
        const auto ur = static_cast<std::size_t>(r);
        if (mesh.conduit_cell_begin[ur] < 0) continue;
        const auto uj = static_cast<std::size_t>(mesh.conduit_link[ur]);
        uint8_t gate = 0;
        auto block = [&](int s) { gate |= (s > 0) ? uint8_t{1} : uint8_t{2}; };

        if (ctx.links.has_flap_gate[uj]) block(-ctx.links.direction[uj]);

        // A gated outfall upstream blocks chain-POSITIVE flow (out of node1
        // into the pipe); downstream it blocks chain-negative.
        const int ends[2] = {ctx.links.node1[uj], ctx.links.node2[uj]};
        for (int e = 0; e < 2; ++e) {
            const int nd = ends[e];
            if (nd < 0 || ctx.nodes.type[static_cast<std::size_t>(nd)] != NodeType::OUTFALL)
                continue;
            const int ofr = ctx.node_subtypes.outfall_row(nd);
            if (ofr >= 0 &&
                ctx.node_subtypes.outfalls.has_flap_gate[static_cast<std::size_t>(ofr)])
                block((e == 0) ? 1 : -1);
        }
        conduit_gate[ur] = gate;
    }

    // Per-node face lists, gathered before flattening to CSR so the ordering is
    // deterministic (node-major, then attachment order) on every backend.
    std::vector<std::vector<int>>    nf_idx(static_cast<std::size_t>(n_nodes));
    std::vector<std::vector<double>> nf_sign(static_cast<std::size_t>(n_nodes));
    std::vector<std::vector<double>> nf_zb(static_cast<std::size_t>(n_nodes));

    for (int i = 0; i < n_nodes; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        auto& at = attach[ui];

        if (mesh.node_kind[ui] == kNodeVirtual) {
            if (at.size() != 2) {
                rep.errors.push_back(
                    "FV routing: virtual junction '" + ctx.node_names.name_of(i) +
                    "' must join exactly two conduits (found " +
                    std::to_string(at.size()) + ").");
                continue;
            }
            // Splice the two chains into ONE interior face. Nothing else to do:
            // conservation across a virtual junction is then a property of the
            // scheme, not a special treatment (plan §3.4).
            const Attachment& a0 = at[0];
            const Attachment& a1 = at[1];
            const int cl = chain_end_cell(a0);
            const int cr = chain_end_cell(a1);
            const int8_t dl = (a0.end == 1) ? int8_t{1} : int8_t{-1};
            const int8_t dr = (a1.end == 0) ? int8_t{1} : int8_t{-1};
            const double dxl = mesh.cell_dx[static_cast<std::size_t>(cl)];
            const double dxr = mesh.cell_dx[static_cast<std::size_t>(cr)];
            add_face(cl, cr, -1, mesh.node_invert[ui], 0.5 * (dxl + dxr),
                     dl, dr, true);
            ++rep.n_virtual;
            continue;
        }

        for (const Attachment& a : at) {
            const int cell = chain_end_cell(a);
            const double dx = mesh.cell_dx[static_cast<std::size_t>(cell)];
            const double zb = invert_at(a);
            const int fidx = mesh.n_faces();
            if (a.end == 0) {
                // Conduit's upstream end: the node sits on the face's LEFT, so
                // a positive L→R flux LEAVES the node.
                add_face(-1, cell, i, zb, 0.5 * dx, 1, 1, false);
                nf_sign[ui].push_back(-1.0);
            } else {
                add_face(cell, -1, i, zb, 0.5 * dx, 1, 1, false);
                nf_sign[ui].push_back(1.0);
            }
            nf_idx[ui].push_back(fidx);
            nf_zb[ui].push_back(zb);
            mesh.face_gate[static_cast<std::size_t>(fidx)] =
                conduit_gate[static_cast<std::size_t>(a.conduit)];

            // HEC-5 inlet control acts at the culvert's UPSTREAM face only.
            if (a.end == 0 &&
                mesh.geom[static_cast<std::size_t>(a.conduit)].culvert_code > 0)
                mesh.face_culvert[static_cast<std::size_t>(fidx)] = a.conduit;
        }
    }
    if (!rep.errors.empty()) return rep;

    // Flatten to CSR.
    mesh.node_face_ptr.assign(static_cast<std::size_t>(n_nodes) + 1, 0);
    for (int i = 0; i < n_nodes; ++i)
        mesh.node_face_ptr[static_cast<std::size_t>(i) + 1] =
            mesh.node_face_ptr[static_cast<std::size_t>(i)] +
            static_cast<int>(nf_idx[static_cast<std::size_t>(i)].size());
    const int total = mesh.node_face_ptr[static_cast<std::size_t>(n_nodes)];
    mesh.node_face_idx.reserve(static_cast<std::size_t>(total));
    mesh.node_face_sign.reserve(static_cast<std::size_t>(total));
    mesh.node_face_zb.reserve(static_cast<std::size_t>(total));
    for (int i = 0; i < n_nodes; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        for (std::size_t k = 0; k < nf_idx[ui].size(); ++k) {
            mesh.node_face_idx.push_back(nf_idx[ui][k]);
            mesh.node_face_sign.push_back(nf_sign[ui][k]);
            mesh.node_face_zb.push_back(nf_zb[ui][k]);
        }
    }

    // -----------------------------------------------------------------------
    // Cell → face back-references. Every cell is bounded by exactly two faces
    // (interior, spliced, or node-coupling), so this is a fixed-width map and
    // the cell update needs no scatter.
    // -----------------------------------------------------------------------
    const int nc = mesh.n_cells();
    mesh.cell_face0.assign(static_cast<std::size_t>(nc), -1);
    mesh.cell_face1.assign(static_cast<std::size_t>(nc), -1);
    mesh.cell_side0.assign(static_cast<std::size_t>(nc), 0);
    mesh.cell_side1.assign(static_cast<std::size_t>(nc), 0);
    auto attach_cell_face = [&](int cell, int face, int8_t side) {
        const auto uc = static_cast<std::size_t>(cell);
        if (mesh.cell_face0[uc] < 0) { mesh.cell_face0[uc] = face; mesh.cell_side0[uc] = side; }
        else                         { mesh.cell_face1[uc] = face; mesh.cell_side1[uc] = side; }
    };
    for (int f = 0; f < mesh.n_faces(); ++f) {
        const auto uf = static_cast<std::size_t>(f);
        if (mesh.face_cl[uf] >= 0) attach_cell_face(mesh.face_cl[uf], f, 0);
        if (mesh.face_cr[uf] >= 0) attach_cell_face(mesh.face_cr[uf], f, 1);
    }
    for (int c = 0; c < nc; ++c) {
        const auto uc = static_cast<std::size_t>(c);
        if (mesh.cell_face0[uc] < 0 || mesh.cell_face1[uc] < 0) {
            rep.errors.push_back(
                "FV routing: internal error — cell " + std::to_string(c) +
                " is not bounded by two faces (dangling conduit end).");
            break;
        }
    }
    if (!rep.errors.empty()) return rep;

    // -----------------------------------------------------------------------
    // Cell chains. Every cell has exactly two faces, so the interior-face graph
    // is a disjoint union of simple paths: walk from each cell that has a
    // boundary face. A chain crosses virtual junctions transparently, which is
    // what makes a spliced pair indistinguishable from an interior cut.
    // -----------------------------------------------------------------------
    mesh.cell_chain.assign(static_cast<std::size_t>(nc), -1);
    mesh.cell_chain_pos.assign(static_cast<std::size_t>(nc), -1);
    mesh.chain_ptr.push_back(0);

    // Neighbour across a face, plus the direction the neighbour's axis takes
    // relative to this cell's axis.
    auto step = [&](int cell, int face, int8_t side, int& next, int8_t& flip) {
        const auto uf = static_cast<std::size_t>(face);
        if (side == 0) {                      // this cell is L
            next = mesh.face_cr[uf];
            flip = static_cast<int8_t>(mesh.face_dir_l[uf] * mesh.face_dir_r[uf]);
        } else {
            next = mesh.face_cl[uf];
            flip = static_cast<int8_t>(mesh.face_dir_l[uf] * mesh.face_dir_r[uf]);
        }
        (void)cell;
    };

    for (int c0 = 0; c0 < nc; ++c0) {
        const auto u0 = static_cast<std::size_t>(c0);
        if (mesh.cell_chain[u0] >= 0) continue;
        // Only start a walk at a chain END — a cell with at least one face that
        // has no neighbour cell.
        const int fa = mesh.cell_face0[u0], fb = mesh.cell_face1[u0];
        const bool end_a = (mesh.face_cl[static_cast<std::size_t>(fa)] < 0 ||
                            mesh.face_cr[static_cast<std::size_t>(fa)] < 0);
        const bool end_b = (mesh.face_cl[static_cast<std::size_t>(fb)] < 0 ||
                            mesh.face_cr[static_cast<std::size_t>(fb)] < 0);
        if (!end_a && !end_b) continue;

        const int chain = mesh.n_chains();
        int cur = c0;
        int8_t dir = 1;
        // Leave through the face that is NOT the terminating one.
        int in_face = end_a ? fa : fb;
        int pos = 0;
        while (cur >= 0) {
            const auto uc = static_cast<std::size_t>(cur);
            mesh.cell_chain[uc]     = chain;
            mesh.cell_chain_pos[uc] = pos++;
            mesh.chain_cells.push_back(cur);
            mesh.chain_dir.push_back(dir);

            const int out_face = (mesh.cell_face0[uc] == in_face)
                                     ? mesh.cell_face1[uc] : mesh.cell_face0[uc];
            const int8_t out_side = (mesh.cell_face0[uc] == in_face)
                                        ? mesh.cell_side1[uc] : mesh.cell_side0[uc];
            int next = -1;
            int8_t flip = 1;
            step(cur, out_face, out_side, next, flip);
            if (next < 0 || mesh.cell_chain[static_cast<std::size_t>(next)] >= 0)
                break;
            dir = static_cast<int8_t>(dir * flip);
            in_face = out_face;
            cur = next;
        }
        mesh.chain_ptr.push_back(static_cast<int>(mesh.chain_cells.size()));
    }
    for (int c = 0; c < nc; ++c) {
        if (mesh.cell_chain[static_cast<std::size_t>(c)] < 0) {
            rep.errors.push_back(
                "FV routing: internal error — cell " + std::to_string(c) +
                " is not reachable from any chain end (closed conduit loop). "
                "A cycle of conduits joined only by virtual junctions has no "
                "boundary and cannot be meshed.");
            break;
        }
    }
    if (!rep.errors.empty()) return rep;

    // -----------------------------------------------------------------------
    // Non-conduit links — applied as node source/sink pairs (plan §3.4).
    // -----------------------------------------------------------------------
    for (int j = 0; j < n_links; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        if (ctx.links.type[uj] == LinkType::CONDUIT) continue;
        const int a = ctx.links.node1[uj];
        const int b = ctx.links.node2[uj];
        if (a < 0 || b < 0) continue;
        mesh.struct_link.push_back(j);
        mesh.struct_n1.push_back(a);
        mesh.struct_n2.push_back(b);
    }

    rep.n_cells    = nc;
    rep.n_faces    = mesh.n_faces();
    rep.n_conduits = n_cond;
    return rep;
}

} // namespace openswmm::fv
