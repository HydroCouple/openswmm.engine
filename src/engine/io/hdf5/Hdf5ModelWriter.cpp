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
 * @file Hdf5ModelWriter.cpp
 * @brief Columnar HDF5 model serialisation. See STRATEGY.md.
 * @ingroup engine_hdf5
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "Hdf5ModelWriter.hpp"
#include "Hdf5Util.hpp"

#include "core/SimulationContext.hpp"
#include "core/UnitConversion.hpp"
#include "data/StorageGeometry.hpp"   // storage_shape_keyword
#include "input/PostParseResolver.hpp"

#include <ctime>
#include <string>
#include <vector>

namespace openswmm::h5io {
namespace {

// ---------------------------------------------------------------------------
// Shape keywords. Written as the LEGACY KEYWORD STRING, never an enum ordinal.
//
// Two enums in this codebase (LinkData::XsectShape and the kernel XSectShape)
// disagree on ordering, and an `enum + 1` translation between them was once
// the single largest parity defect in the project. A format that stores the
// keyword cannot reproduce that class of error, and stays readable besides.
// ---------------------------------------------------------------------------
const char* shape_keyword(int s) {
    static const char* n[] = {
        "CIRCULAR","FILLED_CIRCULAR","RECT_CLOSED","RECT_OPEN",
        "TRAPEZOIDAL","TRIANGULAR","PARABOLIC","POWER","MODBASKETHANDLE",
        "EGG","HORSESHOE","GOTHIC","CATENARY","SEMIELLIPTICAL",
        "BASKETHANDLE","SEMICIRCULAR","RECT_TRIANGULAR","RECT_ROUND",
        "HORIZ_ELLIPSE","VERT_ELLIPSE","ARCH",
        "IRREGULAR","CUSTOM","FORCE_MAIN","STREET","DUMMY"
    };
    return (s >= 0 && s <= 25) ? n[s] : "CIRCULAR";
}

/// Curve type as the legacy [CURVES] keyword. Same reasoning as the shape
/// keywords: a string cannot be silently misread the way an ordinal can.
const char* curve_type_keyword(TableType t) {
    switch (t) {
        case TableType::CURVE_DIVERSION: return "DIVERSION";
        case TableType::CURVE_RATING:    return "RATING";
        case TableType::CURVE_SHAPE:     return "SHAPE";
        case TableType::CURVE_CONTROL:   return "CONTROL";
        case TableType::CURVE_TIDAL:     return "TIDAL";
        case TableType::CURVE_WEIR:      return "WEIR";
        case TableType::CURVE_PUMP1:     return "PUMP1";
        case TableType::CURVE_PUMP2:     return "PUMP2";
        case TableType::CURVE_PUMP3:     return "PUMP3";
        case TableType::CURVE_PUMP4:     return "PUMP4";
        case TableType::CURVE_PUMP5:     return "PUMP5";
        default:                         return "STORAGE";
    }
}

const char* node_type_keyword(NodeType t) {
    switch (t) {
        case NodeType::OUTFALL: return "OUTFALL";
        case NodeType::DIVIDER: return "DIVIDER";
        case NodeType::STORAGE: return "STORAGE";
        default:                return "JUNCTION";
    }
}

const char* link_type_keyword(LinkType t) {
    switch (t) {
        case LinkType::PUMP:    return "PUMP";
        case LinkType::ORIFICE: return "ORIFICE";
        case LinkType::WEIR:    return "WEIR";
        case LinkType::OUTLET:  return "OUTLET";
        default:                return "CONDUIT";
    }
}

/// Safe element access — a SoA array may legitimately be shorter than the
/// object count when a model was built through the API rather than parsed.
template <typename V>
auto at(const V& v, std::size_t i) -> typename V::value_type {
    return i < v.size() ? v[i] : typename V::value_type{};
}

/// Node / link name by index, or "" for an unresolved (-1) reference.
std::string node_name(const SimulationContext& c, int idx) {
    return (idx >= 0 && idx < c.n_nodes()) ? c.node_names.name_of(idx) : std::string();
}
std::string link_name(const SimulationContext& c, int idx) {
    return (idx >= 0 && idx < c.n_links()) ? c.link_names.name_of(idx) : std::string();
}

/// Unit labels resolved against the file's unit system, so every column can
/// say what it actually holds rather than "model units".
struct UnitWords {
    const char* length;
    const char* area;
    const char* flow;
    const char* depth_rate;   // [LOSSES] seepage, infiltration rates
};

UnitWords unit_words(int unit_system) {
    return unit_system == 0 ? UnitWords{"ft", "ac", "cfs", "in/hr"}
                            : UnitWords{"m",  "ha", "cms", "mm/hr"};
}

const char* flow_units_keyword(int fu) {
    static const char* n[] = {"CFS", "GPM", "MGD", "CMS", "LPS", "MLD"};
    return (fu >= 0 && fu < 6) ? n[fu] : "CFS";
}

// ---------------------------------------------------------------------------
// Sections
// ---------------------------------------------------------------------------

void write_root_attrs(hid_t file, const SimulationContext& ctx, int us) {
    write_attr(file, "schema_version", std::string(kSchemaVersion));
    write_attr(file, "format", std::string("openswmm-model-hdf5"));
    write_attr(file, "flow_units",
               std::string(flow_units_keyword(static_cast<int>(ctx.options.flow_units))));
    write_attr(file, "unit_system", std::string(us == 0 ? "US" : "SI"));
    // Units are AUTHORED, not the engine's internal feet/cfs — the one thing a
    // consumer most needs to know and the decision STRATEGY.md §4 turns on.
    write_attr(file, "units_convention", std::string("authored"));

    std::time_t now = std::time(nullptr);
    char stamp[32] = {0};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
    write_attr(file, "created_utc", std::string(stamp));

    std::string title;
    for (const auto& line : ctx.title_notes) {
        if (!title.empty()) title += "\n";
        title += line;
    }
    write_attr(file, "title", title);
}

void write_nodes(hid_t file, const SimulationContext& ctx, const UnitWords& u) {
    const int n = ctx.n_nodes();
    std::vector<std::string> name(static_cast<std::size_t>(n)), type, tag, comment;
    std::vector<double> invert, full, init, sur, ponded;
    type.reserve(name.size()); tag.reserve(name.size()); comment.reserve(name.size());
    invert.reserve(name.size()); full.reserve(name.size()); init.reserve(name.size());
    sur.reserve(name.size()); ponded.reserve(name.size());

    for (int j = 0; j < n; ++j) {
        const auto i = static_cast<std::size_t>(j);
        name[i] = ctx.node_names.name_of(j);
        type.push_back(node_type_keyword(at(ctx.nodes.type, i)));
        tag.push_back(at(ctx.nodes.tags, i));
        comment.push_back(at(ctx.nodes.comments, i));
        invert.push_back(at(ctx.nodes.invert_elev, i));
        full.push_back(at(ctx.nodes.full_depth, i));
        init.push_back(at(ctx.nodes.init_depth, i));
        sur.push_back(at(ctx.nodes.sur_depth, i));
        ponded.push_back(at(ctx.nodes.ponded_area, i));
    }

    Handle g = make_group(file, "nodes");
    if (!g) return;
    write_column(g.get(), "name", name, "node id");
    write_column(g.get(), "type", type, "JUNCTION | OUTFALL | DIVIDER | STORAGE");
    write_column(g.get(), "invert_elev", invert, u.length, "invert elevation");
    write_column(g.get(), "full_depth", full, u.length, "depth from invert to ground");
    write_column(g.get(), "init_depth", init, u.length, "initial water depth");
    write_column(g.get(), "sur_depth", sur, u.length, "surcharge depth above ground");
    write_column(g.get(), "ponded_area", ponded,
                 std::string(u.length) + "^2", "ponded surface area when flooded");
    write_column(g.get(), "tag", tag, "user tag");
    write_column(g.get(), "comment", comment, "authored comment");
}

void write_node_subtypes(hid_t file, const SimulationContext& ctx, const UnitWords& u) {
    const int n = ctx.n_nodes();

    // --- outfalls -----------------------------------------------------------
    {
        std::vector<std::string> name, bc, stage_curve;
        std::vector<double> stage;
        std::vector<int> gated;
        std::vector<std::string> route_to;
        for (int j = 0; j < n; ++j) {
            const auto i = static_cast<std::size_t>(j);
            if (at(ctx.nodes.type, i) != NodeType::OUTFALL) continue;
            const int r = ctx.node_subtypes.outfall_row(j);
            const auto& O = ctx.node_subtypes.outfalls;
            name.push_back(ctx.node_names.name_of(j));
            const auto bt = (r >= 0) ? at(O.bc_type, static_cast<std::size_t>(r))
                                     : OutfallType::FREE;
            switch (bt) {
                case OutfallType::NORMAL:     bc.push_back("NORMAL"); break;
                case OutfallType::FIXED:      bc.push_back("FIXED"); break;
                case OutfallType::TIDAL:      bc.push_back("TIDAL"); break;
                case OutfallType::TIMESERIES: bc.push_back("TIMESERIES"); break;
                default:                      bc.push_back("FREE"); break;
            }
            const double p = (r >= 0) ? at(O.param, static_cast<std::size_t>(r)) : 0.0;
            // FIXED parks a stage in `param`; TIDAL/TIMESERIES park a table
            // index there instead, which is written as the table's NAME.
            if (bt == OutfallType::FIXED) { stage.push_back(p); stage_curve.push_back(""); }
            else {
                stage.push_back(0.0);
                const int t = static_cast<int>(p);
                stage_curve.push_back((bt == OutfallType::TIDAL || bt == OutfallType::TIMESERIES)
                                      && t >= 0 && t < ctx.n_tables()
                                      ? ctx.tables[t].id : std::string());
            }
            gated.push_back((r >= 0) ? at(O.has_flap_gate, static_cast<std::size_t>(r)) : 0);
            const int rt = (r >= 0) ? at(O.route_to, static_cast<std::size_t>(r)) : -1;
            route_to.push_back(rt >= 0 && rt < ctx.n_subcatches()
                               ? ctx.subcatch_names.name_of(rt) : std::string());
        }
        Handle g = make_group(file, "nodes/outfalls");
        if (g) {
            write_column(g.get(), "name", name, "node id");
            write_column(g.get(), "bc_type", bc, "FREE | NORMAL | FIXED | TIDAL | TIMESERIES");
            write_column(g.get(), "stage", stage, u.length, "fixed stage elevation");
            write_column(g.get(), "stage_series", stage_curve, "tidal curve or stage time series");
            write_column(g.get(), "has_flap_gate", gated, "boolean", "flap gate present");
            write_column(g.get(), "route_to", route_to, "subcatchment receiving the outflow");
        }
    }

    // --- dividers -----------------------------------------------------------
    {
        std::vector<std::string> name, div_link, method, curve;
        std::vector<double> cutoff, qmin, dh_max, cd;
        for (int j = 0; j < n; ++j) {
            const auto i = static_cast<std::size_t>(j);
            if (at(ctx.nodes.type, i) != NodeType::DIVIDER) continue;
            const int r = ctx.node_subtypes.divider_row(j);
            const auto& D = ctx.node_subtypes.dividers;
            const auto ur = static_cast<std::size_t>(r);
            name.push_back(ctx.node_names.name_of(j));
            const int dl = (r >= 0) ? at(D.link, ur) : -1;
            std::string dln = link_name(ctx, dl);
            if (dln.empty() && r >= 0) dln = at(D.link_name, ur);
            div_link.push_back(dln);
            const auto m = (r >= 0) ? at(D.method, ur) : DividerType::CUTOFF;
            switch (m) {
                case DividerType::OVERFLOW_DIV: method.push_back("OVERFLOW"); break;
                case DividerType::TABULAR:      method.push_back("TABULAR"); break;
                case DividerType::WEIR:         method.push_back("WEIR"); break;
                default:                        method.push_back("CUTOFF"); break;
            }
            const int ci = (r >= 0) ? at(D.curve, ur) : -1;
            std::string cn = (ci >= 0 && ci < ctx.n_tables()) ? ctx.tables[ci].id : std::string();
            if (cn.empty() && r >= 0) cn = at(D.curve_name, ur);
            curve.push_back(cn);
            cutoff.push_back((r >= 0) ? at(D.cutoff, ur) : 0.0);
            qmin.push_back((r >= 0) ? at(D.cutoff, ur) : 0.0);
            dh_max.push_back((r >= 0) ? at(D.max_depth, ur) : 0.0);
            cd.push_back((r >= 0) ? at(D.cd, ur) : 0.0);
        }
        Handle g = make_group(file, "nodes/dividers");
        if (g) {
            write_column(g.get(), "name", name, "node id");
            write_column(g.get(), "diverted_link", div_link, "link receiving the diverted flow");
            write_column(g.get(), "method", method, "CUTOFF | OVERFLOW | TABULAR | WEIR");
            write_column(g.get(), "curve", curve, "diversion curve (TABULAR)");
            write_column(g.get(), "cutoff_flow", cutoff, u.flow, "CUTOFF/WEIR minimum flow");
            write_column(g.get(), "weir_dh_max", dh_max, u.length, "WEIR head range");
            write_column(g.get(), "weir_cd", cd, "dimensionless", "WEIR discharge coefficient");
        }
    }

    // --- storage ------------------------------------------------------------
    {
        std::vector<std::string> name, shape, curve;
        std::vector<double> a0, a1, a2, p1, p2, p3, fevap, ksat, suction, imd;
        for (int j = 0; j < n; ++j) {
            const auto i = static_cast<std::size_t>(j);
            if (at(ctx.nodes.type, i) != NodeType::STORAGE) continue;
            const int r = ctx.node_subtypes.storage_row(j);
            const auto& S = ctx.node_subtypes.storages;
            const auto ur = static_cast<std::size_t>(r);
            name.push_back(ctx.node_names.name_of(j));
            const int ci = (r >= 0) ? at(S.curve, ur) : -1;
            shape.push_back(ci >= 0 ? "TABULAR"
                                    : storage_shape_keyword((r >= 0) ? at(S.shape, ur)
                                                                     : StorageShape::FUNCTIONAL));
            curve.push_back((ci >= 0 && ci < ctx.n_tables()) ? ctx.tables[ci].id : std::string());
            // [STORAGE] FUNCTIONAL columns are A1 A2 A0 in that order, so the
            // store's a/b/c are the COEFFICIENT, the EXPONENT and the
            // CONSTANT — not a0/a1/a2 as the letters suggest.
            a1.push_back((r >= 0) ? at(S.a, ur) : 0.0);   // A1, coefficient
            a2.push_back((r >= 0) ? at(S.b, ur) : 0.0);   // A2, exponent
            a0.push_back((r >= 0) ? at(S.c, ur) : 0.0);   // A0, constant
            p1.push_back((r >= 0) ? at(S.p1, ur) : 0.0);
            p2.push_back((r >= 0) ? at(S.p2, ur) : 0.0);
            p3.push_back((r >= 0) ? at(S.p3, ur) : 0.0);
            fevap.push_back((r >= 0) ? at(S.evap_frac, ur) : 0.0);
            ksat.push_back((r >= 0) ? at(S.exfil_ksat, ur) : 0.0);
            suction.push_back((r >= 0) ? at(S.exfil_suction, ur) : 0.0);
            imd.push_back((r >= 0) ? at(S.exfil_imd, ur) : 0.0);
        }
        Handle g = make_group(file, "nodes/storage");
        if (g) {
            write_column(g.get(), "name", name, "node id");
            write_column(g.get(), "shape", shape, "TABULAR | FUNCTIONAL | CYLINDRICAL | ...");
            write_column(g.get(), "curve", curve, "storage curve (TABULAR)");
            // FUNCTIONAL A(d) = a0 + a1*d^a2. These stay in USER units
            // end-to-end by design (legacy converts per call in node.c), so
            // they are already authored here.
            write_column(g.get(), "a1", a1, "model",
                         "FUNCTIONAL coefficient A1 in A(d) = A0 + A1*d^A2");
            write_column(g.get(), "a2", a2, "dimensionless", "FUNCTIONAL exponent A2");
            write_column(g.get(), "a0", a0, "model", "FUNCTIONAL constant A0");
            write_column(g.get(), "p1", p1, u.length, "geometric shape parameter 1 (L)");
            write_column(g.get(), "p2", p2, u.length, "geometric shape parameter 2 (W)");
            write_column(g.get(), "p3", p3, u.length, "geometric shape parameter 3 (Z/H)");
            write_column(g.get(), "evap_frac", fevap, "fraction", "evaporation factor");
            write_column(g.get(), "exfil_suction", suction, u.length, "exfiltration suction head");
            write_column(g.get(), "exfil_ksat", ksat, u.depth_rate, "exfiltration conductivity");
            write_column(g.get(), "exfil_imd", imd, "fraction", "initial moisture deficit");
        }
    }
}

void write_links(hid_t file, const SimulationContext& ctx, const UnitWords& u) {
    const int n = ctx.n_links();
    std::vector<std::string> name, type, n1, n2, tag, comment;
    std::vector<double> off1, off2, q0, qlim;
    std::vector<int> flap;

    for (int j = 0; j < n; ++j) {
        const auto i = static_cast<std::size_t>(j);
        name.push_back(ctx.link_names.name_of(j));
        type.push_back(link_type_keyword(at(ctx.links.type, i)));
        n1.push_back(node_name(ctx, at(ctx.links.node1, i)));
        n2.push_back(node_name(ctx, at(ctx.links.node2, i)));
        off1.push_back(at(ctx.links.offset1, i));
        off2.push_back(at(ctx.links.offset2, i));
        q0.push_back(at(ctx.links.q0, i));
        qlim.push_back(at(ctx.links.q_limit, i));
        flap.push_back(at(ctx.links.has_flap_gate, i));
        tag.push_back(at(ctx.links.tags, i));
        comment.push_back(at(ctx.links.comments, i));
    }

    Handle g = make_group(file, "links");
    if (!g) return;
    write_column(g.get(), "name", name, "link id");
    write_column(g.get(), "type", type, "CONDUIT | PUMP | ORIFICE | WEIR | OUTLET");
    write_column(g.get(), "node1", n1, "upstream node id");
    write_column(g.get(), "node2", n2, "downstream node id");
    write_column(g.get(), "offset1", off1, u.length, "upstream offset (depth convention)");
    write_column(g.get(), "offset2", off2, u.length, "downstream offset (depth convention)");
    write_column(g.get(), "q0", q0, u.flow, "initial flow");
    write_column(g.get(), "q_limit", qlim, u.flow, "maximum flow, 0 = no limit");
    write_column(g.get(), "has_flap_gate", flap, "boolean", "flap gate present");
    write_column(g.get(), "tag", tag, "user tag");
    write_column(g.get(), "comment", comment, "authored comment");
}

void write_xsections(hid_t file, const SimulationContext& ctx, const UnitWords& u) {
    const int n = ctx.n_links();
    std::vector<std::string> link, shape, ref;
    std::vector<double> g1, g2, g3, g4;
    std::vector<int> barrels, culvert;

    for (int j = 0; j < n; ++j) {
        const auto i = static_cast<std::size_t>(j);
        const auto lt = at(ctx.links.type, i);
        // Cross sections belong to conduits, orifices and weirs. A pump or
        // outlet has none, and emitting a default one makes legacy reject the
        // deck (ERROR 211).
        if (lt == LinkType::PUMP || lt == LinkType::OUTLET) continue;

        const auto sh = at(ctx.links.xsect_shape, i);
        link.push_back(ctx.link_names.name_of(j));
        shape.push_back(shape_keyword(static_cast<int>(sh)));

        // IRREGULAR names a transect, STREET names a street, CUSTOM names a
        // shape curve — a NAME, not a dimension. Keeping it in its own column
        // removes the .inp's overloading of the Geom1/Geom2 slots entirely.
        std::string r;
        if (sh == XsectShape::IRREGULAR || sh == XsectShape::STREET_XSECT ||
            sh == XsectShape::CUSTOM) {
            r = at(ctx.links.pump_curve_name, i);
            if (r.empty()) {
                const int xc = at(ctx.links.xsect_curve, i);
                if (sh == XsectShape::IRREGULAR && xc >= 0 && xc < ctx.transects.count())
                    r = ctx.transects.names[static_cast<std::size_t>(xc)];
            }
        }
        ref.push_back(r);

        // RAW authored geoms — never the resolver's derived y_full/w_max.
        // Their meaning is shape-dependent (a length, a dimensionless side
        // slope, a C-factor, or a lookup-table index for the ellipse/arch
        // standard sizes), and only the raw values reconstruct the section.
        g1.push_back(at(ctx.links.xsect_geom1, i));
        g2.push_back(at(ctx.links.xsect_geom2, i));
        g3.push_back(at(ctx.links.xsect_geom3, i));
        g4.push_back(at(ctx.links.xsect_geom4, i));

        const int cr = ctx.link_subtypes.conduit_row(j);
        barrels.push_back(cr >= 0
            ? at(ctx.link_subtypes.conduits.barrels, static_cast<std::size_t>(cr)) : 1);
        culvert.push_back(cr >= 0
            ? at(ctx.link_subtypes.conduits.culvert_code, static_cast<std::size_t>(cr)) : 0);
    }

    Handle g = make_group(file, "links/xsections");
    if (!g) return;
    write_column(g.get(), "link", link, "link id");
    write_column(g.get(), "shape", shape, "legacy shape keyword");
    write_column(g.get(), "reference", ref,
                 "transect / street / shape-curve name for IRREGULAR, STREET, CUSTOM");
    const char* geom_note =
        "raw authored Geom value; meaning is shape-dependent — see STRATEGY.md and "
        "the cross-section matrix in plans/FILE_IO_PARITY_AUDIT_2026-09-22.md";
    write_column(g.get(), "geom1", g1, "shape-dependent", geom_note);
    write_column(g.get(), "geom2", g2, "shape-dependent", geom_note);
    write_column(g.get(), "geom3", g3, "shape-dependent", geom_note);
    write_column(g.get(), "geom4", g4, "shape-dependent", geom_note);
    write_column(g.get(), "barrels", barrels, "count", "identical parallel barrels");
    write_column(g.get(), "culvert_code", culvert, "code",
                 "FHWA culvert inlet-control code, 0 = not a culvert");
    write_attr(g.get(), "note",
               std::string("For HORIZ_ELLIPSE, VERT_ELLIPSE and ARCH a nonzero geom3 "
                           "(or geom1 with geom2 == 0) is a STANDARD SIZE CODE, not a "
                           "dimension. Preserved verbatim."));
    (void)u;
}

void write_link_subtypes(hid_t file, const SimulationContext& ctx, const UnitWords& u) {
    const int n = ctx.n_links();

    // --- conduits -----------------------------------------------------------
    {
        std::vector<std::string> name;
        std::vector<double> length, rough, k_in, k_out, k_avg, seep;
        for (int j = 0; j < n; ++j) {
            if (at(ctx.links.type, static_cast<std::size_t>(j)) != LinkType::CONDUIT) continue;
            const int r = ctx.link_subtypes.conduit_row(j);
            const auto& C = ctx.link_subtypes.conduits;
            const auto ur = static_cast<std::size_t>(r);
            name.push_back(ctx.link_names.name_of(j));
            length.push_back((r >= 0) ? at(C.length, ur) : 0.0);
            rough.push_back((r >= 0) ? at(C.roughness, ur) : 0.01);
            k_in.push_back((r >= 0) ? at(C.loss_inlet, ur) : 0.0);
            k_out.push_back((r >= 0) ? at(C.loss_outlet, ur) : 0.0);
            k_avg.push_back((r >= 0) ? at(C.loss_avg, ur) : 0.0);
            seep.push_back((r >= 0) ? at(C.seep_rate, ur) : 0.0);
        }
        Handle g = make_group(file, "links/conduits");
        if (g) {
            write_column(g.get(), "name", name, "link id");
            write_column(g.get(), "length", length, u.length, "conduit length");
            write_column(g.get(), "roughness", rough, "dimensionless", "Manning's n");
            write_column(g.get(), "loss_inlet", k_in, "dimensionless", "entry loss coefficient");
            write_column(g.get(), "loss_outlet", k_out, "dimensionless", "exit loss coefficient");
            write_column(g.get(), "loss_avg", k_avg, "dimensionless", "average loss coefficient");
            // Seepage arrives and leaves in in/hr (mm/hr) even though the
            // solver consumes ft/s — the conversion that a LENGTH-only writer
            // gate used to skip on US decks.
            write_column(g.get(), "seep_rate", seep, u.depth_rate, "seepage loss rate");
        }
    }

    // --- pumps --------------------------------------------------------------
    {
        std::vector<std::string> name, curve, status;
        std::vector<double> startup, shutoff;
        for (int j = 0; j < n; ++j) {
            if (at(ctx.links.type, static_cast<std::size_t>(j)) != LinkType::PUMP) continue;
            const int r = ctx.link_subtypes.pump_row(j);
            const auto& P = ctx.link_subtypes.pumps;
            const auto ur = static_cast<std::size_t>(r);
            name.push_back(ctx.link_names.name_of(j));
            curve.push_back(at(ctx.links.pump_curve_name, static_cast<std::size_t>(j)));
            status.push_back((r >= 0 && at(P.init_state, ur) != 0) ? "ON" : "OFF");
            startup.push_back((r >= 0) ? at(P.startup, ur) : 0.0);
            shutoff.push_back((r >= 0) ? at(P.shutoff, ur) : 0.0);
        }
        Handle g = make_group(file, "links/pumps");
        if (g) {
            write_column(g.get(), "name", name, "link id");
            write_column(g.get(), "curve", curve, "pump curve name, '*' for an ideal pump");
            write_column(g.get(), "init_status", status, "ON | OFF");
            write_column(g.get(), "startup_depth", startup, u.length, "wet-well start depth");
            write_column(g.get(), "shutoff_depth", shutoff, u.length, "wet-well stop depth");
        }
    }

    // --- orifices -----------------------------------------------------------
    {
        std::vector<std::string> name, orient;
        std::vector<double> cd, crest, orate;
        for (int j = 0; j < n; ++j) {
            if (at(ctx.links.type, static_cast<std::size_t>(j)) != LinkType::ORIFICE) continue;
            const int r = ctx.link_subtypes.orifice_row(j);
            const auto& O = ctx.link_subtypes.orifices;
            const auto ur = static_cast<std::size_t>(r);
            name.push_back(ctx.link_names.name_of(j));
            orient.push_back((r >= 0 && at(O.orifice_type, ur) != 0.0) ? "SIDE" : "BOTTOM");
            cd.push_back((r >= 0) ? at(O.cd, ur) : 0.0);
            crest.push_back(at(ctx.links.offset1, static_cast<std::size_t>(j)));
            orate.push_back((r >= 0) ? at(O.orate, ur) : 0.0);
        }
        Handle g = make_group(file, "links/orifices");
        if (g) {
            write_column(g.get(), "name", name, "link id");
            write_column(g.get(), "orientation", orient, "SIDE | BOTTOM");
            write_column(g.get(), "crest_height", crest, u.length, "offset above the node invert");
            write_column(g.get(), "discharge_coeff", cd, "dimensionless", "Cd");
            write_column(g.get(), "open_close_time", orate, "hours", "time to open or close fully");
        }
    }

    // --- weirs --------------------------------------------------------------
    {
        std::vector<std::string> name, wtype, cd_curve;
        std::vector<double> crest, cd, cd2, end_con, road_width;
        std::vector<int> surcharge;
        for (int j = 0; j < n; ++j) {
            if (at(ctx.links.type, static_cast<std::size_t>(j)) != LinkType::WEIR) continue;
            const int r = ctx.link_subtypes.weir_row(j);
            const auto& W = ctx.link_subtypes.weirs;
            const auto ur = static_cast<std::size_t>(r);
            static const char* kinds[] = {"TRANSVERSE","SIDEFLOW","V-NOTCH","TRAPEZOIDAL","ROADWAY"};
            const int wt = (r >= 0) ? at(W.weir_type, ur) : 0;
            name.push_back(ctx.link_names.name_of(j));
            wtype.push_back((wt >= 0 && wt < 5) ? kinds[wt] : "TRANSVERSE");
            crest.push_back((r >= 0) ? at(W.crest_height, ur) : 0.0);
            cd.push_back((r >= 0) ? at(W.cd, ur) : 0.0);
            cd2.push_back((r >= 0) ? at(W.cd2, ur) : 0.0);
            end_con.push_back((r >= 0) ? at(W.end_contractions, ur) : 0.0);
            road_width.push_back((r >= 0) ? at(W.road_width, ur) : 0.0);
            surcharge.push_back((r >= 0) ? at(W.can_surcharge, ur) : 1);
            cd_curve.push_back("");
        }
        Handle g = make_group(file, "links/weirs");
        if (g) {
            write_column(g.get(), "name", name, "link id");
            write_column(g.get(), "weir_type", wtype,
                         "TRANSVERSE | SIDEFLOW | V-NOTCH | TRAPEZOIDAL | ROADWAY");
            write_column(g.get(), "crest_height", crest, u.length, "crest above the node invert");
            write_column(g.get(), "discharge_coeff", cd, "dimensionless", "Cd");
            write_column(g.get(), "discharge_coeff2", cd2, "dimensionless",
                         "second Cd (TRAPEZOIDAL sides)");
            write_column(g.get(), "end_contractions", end_con, "count", "end contractions");
            write_column(g.get(), "road_width", road_width, u.length, "ROADWAY width");
            write_column(g.get(), "can_surcharge", surcharge, "boolean", "may surcharge");
        }
    }

    // --- outlets ------------------------------------------------------------
    {
        std::vector<std::string> name, rating, curve;
        std::vector<double> crest, coeff, expon;
        for (int j = 0; j < n; ++j) {
            if (at(ctx.links.type, static_cast<std::size_t>(j)) != LinkType::OUTLET) continue;
            const int r = ctx.link_subtypes.outlet_row(j);
            const auto& O = ctx.link_subtypes.outlets;
            const auto ur = static_cast<std::size_t>(r);
            static const char* kinds[] = {"FUNCTIONAL/HEAD","FUNCTIONAL/DEPTH",
                                          "TABULAR/HEAD","TABULAR/DEPTH"};
            const int ot = (r >= 0) ? at(O.outlet_type, ur) : 0;
            name.push_back(ctx.link_names.name_of(j));
            rating.push_back((ot >= 0 && ot < 4) ? kinds[ot] : kinds[0]);
            curve.push_back(at(ctx.links.pump_curve_name, static_cast<std::size_t>(j)));
            crest.push_back((r >= 0) ? at(O.crest_height, ur)
                                     : at(ctx.links.offset1, static_cast<std::size_t>(j)));
            coeff.push_back((r >= 0) ? at(O.coeff, ur) : 0.0);
            expon.push_back((r >= 0) ? at(O.expon, ur) : 0.0);
        }
        Handle g = make_group(file, "links/outlets");
        if (g) {
            write_column(g.get(), "name", name, "link id");
            write_column(g.get(), "rating_type", rating,
                         "FUNCTIONAL|TABULAR crossed with HEAD|DEPTH");
            write_column(g.get(), "crest_height", crest, u.length, "offset above the node invert");
            write_column(g.get(), "rating_curve", curve, "rating curve name (TABULAR)");
            write_column(g.get(), "coefficient", coeff, "model", "FUNCTIONAL coefficient");
            write_column(g.get(), "exponent", expon, "dimensionless", "FUNCTIONAL exponent");
        }
    }
}

void write_subcatchments(hid_t file, const SimulationContext& ctx, const UnitWords& u) {
    const int n = ctx.n_subcatches();
    std::vector<std::string> name, gage, outlet, route_to, tag, comment;
    std::vector<double> area, pct_imp, width, slope, curb;
    std::vector<double> n_imp, n_perv, s_imp, s_perv, pct_zero, pct_routed;

    for (int j = 0; j < n; ++j) {
        const auto i = static_cast<std::size_t>(j);
        name.push_back(ctx.subcatch_names.name_of(j));
        const int gi = at(ctx.subcatches.gage, i);
        gage.push_back(gi >= 0 && gi < ctx.gage_names.size()
                       ? ctx.gage_names.name_of(gi) : std::string());
        // An outlet is either a node or another subcatchment.
        const int on = at(ctx.subcatches.outlet_node, i);
        const int os = at(ctx.subcatches.outlet_subcatch, i);
        outlet.push_back(on >= 0 ? node_name(ctx, on)
                                 : (os >= 0 && os < n ? ctx.subcatch_names.name_of(os)
                                                      : std::string()));
        area.push_back(at(ctx.subcatches.area, i));
        pct_imp.push_back(at(ctx.subcatches.frac_imperv, i) * 100.0);
        width.push_back(at(ctx.subcatches.width, i));
        slope.push_back(at(ctx.subcatches.slope, i) * 100.0);
        curb.push_back(at(ctx.subcatches.curb_length, i));
        n_imp.push_back(at(ctx.subcatches.n_imperv, i));
        n_perv.push_back(at(ctx.subcatches.n_perv, i));
        s_imp.push_back(at(ctx.subcatches.ds_imperv, i));
        s_perv.push_back(at(ctx.subcatches.ds_perv, i));
        pct_zero.push_back(at(ctx.subcatches.frac_imperv_no_store, i) * 100.0);
        static const char* rt[] = {"OUTLET", "IMPERVIOUS", "PERVIOUS"};
        const int m = at(ctx.subcatches.subarea_routing, i);
        route_to.push_back((m >= 0 && m < 3) ? rt[m] : rt[0]);
        pct_routed.push_back(at(ctx.subcatches.pct_routed, i) * 100.0);
        tag.push_back(at(ctx.subcatches.tags, i));
        comment.push_back(at(ctx.subcatches.comments, i));
    }

    Handle g = make_group(file, "subcatchments");
    if (!g) return;
    write_column(g.get(), "name", name, "subcatchment id");
    write_column(g.get(), "rain_gage", gage, "rain gage id");
    write_column(g.get(), "outlet", outlet, "receiving node or subcatchment id");
    write_column(g.get(), "area", area, u.area, "surface area");
    write_column(g.get(), "pct_imperv", pct_imp, "percent", "impervious fraction");
    write_column(g.get(), "width", width, u.length, "characteristic overland-flow width");
    write_column(g.get(), "slope", slope, "percent", "average surface slope");
    write_column(g.get(), "curb_length", curb, u.length, "curb length for pollutant buildup");
    write_column(g.get(), "n_imperv", n_imp, "dimensionless", "Manning's n, impervious");
    write_column(g.get(), "n_perv", n_perv, "dimensionless", "Manning's n, pervious");
    write_column(g.get(), "s_imperv", s_imp, "depth", "depression storage, impervious");
    write_column(g.get(), "s_perv", s_perv, "depth", "depression storage, pervious");
    write_column(g.get(), "pct_zero_imperv", pct_zero, "percent",
                 "impervious area with no depression storage");
    write_column(g.get(), "route_to", route_to, "OUTLET | IMPERVIOUS | PERVIOUS");
    write_column(g.get(), "pct_routed", pct_routed, "percent", "runoff routed between subareas");
    write_column(g.get(), "tag", tag, "user tag");
    write_column(g.get(), "comment", comment, "authored comment");
}

/// Curves, time series and patterns share the ragged-table problem: each
/// object owns a variable number of ordinates. The CSR idiom keeps everything
/// contiguous — one flat value array plus an offsets array of length n+1 —
/// which needs no variable-length HDF5 types and reads cleanly from h5py.
void write_tables(hid_t file, const SimulationContext& ctx) {
    std::vector<std::string> cname, ctype;
    std::vector<int> coffs{0};
    std::vector<double> cx, cy;

    std::vector<std::string> tname;
    std::vector<int> toffs{0};
    std::vector<double> tx, ty;

    for (int t = 0; t < ctx.n_tables(); ++t) {
        const auto& tb = ctx.tables[t];
        const bool is_series = (tb.type == TableType::TIMESERIES);
        auto& nm  = is_series ? tname : cname;
        auto& off = is_series ? toffs : coffs;
        auto& xs  = is_series ? tx : cx;
        auto& ys  = is_series ? ty : cy;
        nm.push_back(tb.id);
        if (!is_series) ctype.push_back(curve_type_keyword(tb.type));
        xs.insert(xs.end(), tb.x.begin(), tb.x.end());
        ys.insert(ys.end(), tb.y.begin(), tb.y.end());
        off.push_back(static_cast<int>(xs.size()));
    }

    {
        Handle g = make_group(file, "tables/curves");
        if (g) {
            write_column(g.get(), "name", cname, "curve id");
            write_column(g.get(), "type", ctype, "STORAGE | RATING | PUMP1..5 | SHAPE | ...");
            write_column(g.get(), "offsets", coffs, "index",
                         "CSR row starts into x/y; row i spans [offsets[i], offsets[i+1])");
            write_column(g.get(), "x", cx, "model", "abscissa values, all curves concatenated");
            write_column(g.get(), "y", cy, "model", "ordinate values, all curves concatenated");
        }
    }
    {
        Handle g = make_group(file, "tables/timeseries");
        if (g) {
            write_column(g.get(), "name", tname, "time series id");
            write_column(g.get(), "offsets", toffs, "index", "CSR row starts into time/value");
            // Times are stored as the engine holds them: absolute OADate when
            // the series was authored with dates, else elapsed hours. The
            // `absolute` flag says which, so a relative series is never
            // silently anchored to a start date it did not have.
            write_column(g.get(), "time", tx, "OADate or elapsed hours", "abscissa");
            write_column(g.get(), "value", ty, "model", "ordinate");
        }
    }
}

void write_geometry(hid_t file, const SimulationContext& ctx, const UnitWords& u) {
    // --- node coordinates ---------------------------------------------------
    {
        std::vector<std::string> name;
        std::vector<double> x, y;
        for (int j = 0; j < ctx.n_nodes(); ++j) {
            const auto i = static_cast<std::size_t>(j);
            if (i >= ctx.spatial.node_x.size()) break;
            name.push_back(ctx.node_names.name_of(j));
            x.push_back(ctx.spatial.node_x[i]);
            y.push_back(ctx.spatial.node_y[i]);
        }
        Handle g = make_group(file, "geometry/coordinates");
        if (g) {
            write_column(g.get(), "node", name, "node id");
            write_column(g.get(), "x", x, "map units", "easting");
            write_column(g.get(), "y", y, "map units", "northing");
        }
    }
    // --- link vertices (CSR) ------------------------------------------------
    {
        std::vector<std::string> name;
        std::vector<int> offs{0};
        std::vector<double> x, y;
        for (int j = 0; j < ctx.n_links(); ++j) {
            const auto i = static_cast<std::size_t>(j);
            if (i >= ctx.spatial.link_vertices_x.size()) break;
            name.push_back(ctx.link_names.name_of(j));
            const auto& vx = ctx.spatial.link_vertices_x[i];
            const auto& vy = ctx.spatial.link_vertices_y[i];
            x.insert(x.end(), vx.begin(), vx.end());
            y.insert(y.end(), vy.begin(), vy.end());
            offs.push_back(static_cast<int>(x.size()));
        }
        Handle g = make_group(file, "geometry/vertices");
        if (g) {
            write_column(g.get(), "link", name, "link id");
            write_column(g.get(), "offsets", offs, "index", "CSR row starts into x/y");
            write_column(g.get(), "x", x, "map units", "easting");
            write_column(g.get(), "y", y, "map units", "northing");
        }
    }
    (void)u;
}

void write_transects_and_streets(hid_t file, const SimulationContext& ctx,
                                 const UnitWords& u) {
    // --- transects (CSR over the station/elevation pairs) -------------------
    {
        const int n = ctx.transects.count();
        std::vector<std::string> name;
        std::vector<double> nl, nr, nc, xl, xr, lf, xf, yf, station, elevation;
        std::vector<int> offs{0};
        for (int t = 0; t < n; ++t) {
            const auto i = static_cast<std::size_t>(t);
            name.push_back(ctx.transects.names[i]);
            nl.push_back(ctx.transects.n_left[i]);
            nr.push_back(ctx.transects.n_right[i]);
            nc.push_back(ctx.transects.n_channel[i]);
            xl.push_back(ctx.transects.x_left_bank[i]);
            xr.push_back(ctx.transects.x_right_bank[i]);
            lf.push_back(ctx.transects.length_factor[i]);
            xf.push_back(ctx.transects.x_factor[i]);
            yf.push_back(ctx.transects.y_factor[i]);
            station.insert(station.end(), ctx.transects.stations[i].begin(),
                           ctx.transects.stations[i].end());
            elevation.insert(elevation.end(), ctx.transects.elevations[i].begin(),
                             ctx.transects.elevations[i].end());
            offs.push_back(static_cast<int>(station.size()));
        }
        Handle g = make_group(file, "geometry/transects");
        if (g) {
            write_column(g.get(), "name", name, "transect id");
            write_column(g.get(), "n_left", nl, "dimensionless", "Manning's n, left overbank");
            write_column(g.get(), "n_right", nr, "dimensionless", "Manning's n, right overbank");
            write_column(g.get(), "n_channel", nc, "dimensionless", "Manning's n, main channel");
            write_column(g.get(), "x_left_bank", xl, u.length, "left bank station");
            write_column(g.get(), "x_right_bank", xr, u.length, "right bank station");
            write_column(g.get(), "length_factor", lf, "dimensionless", "meander modifier");
            write_column(g.get(), "x_factor", xf, "dimensionless", "station multiplier");
            write_column(g.get(), "y_factor", yf, u.length, "elevation offset");
            write_column(g.get(), "offsets", offs, "index", "CSR row starts into station/elevation");
            write_column(g.get(), "station", station, u.length, "horizontal station");
            write_column(g.get(), "elevation", elevation, u.length, "ground elevation");
        }
    }
    // --- streets ------------------------------------------------------------
    {
        const int n = ctx.streets.count();
        std::vector<std::string> name;
        std::vector<double> tcrown, hcurb, sx, nroad, a, wdep, tback, sback, nback;
        std::vector<int> sides;
        for (int j = 0; j < n; ++j) {
            const auto i = static_cast<std::size_t>(j);
            name.push_back(ctx.streets.names[i]);
            tcrown.push_back(ctx.streets.t_crown[i]);
            hcurb.push_back(ctx.streets.h_curb[i]);
            sx.push_back(ctx.streets.sx[i]);
            nroad.push_back(ctx.streets.n_road[i]);
            a.push_back(ctx.streets.gutter_depres[i]);
            wdep.push_back(ctx.streets.gutter_width[i]);
            sides.push_back(ctx.streets.sides[i]);
            tback.push_back(ctx.streets.back_width[i]);
            sback.push_back(ctx.streets.back_slope[i]);
            nback.push_back(ctx.streets.back_n[i]);
        }
        Handle g = make_group(file, "geometry/streets");
        if (g) {
            write_column(g.get(), "name", name, "street id");
            write_column(g.get(), "crown_width", tcrown, u.length, "half-width to the crown");
            write_column(g.get(), "curb_height", hcurb, u.length, "curb height");
            write_column(g.get(), "cross_slope", sx, "percent", "road cross slope");
            write_column(g.get(), "n_road", nroad, "dimensionless", "road Manning's n");
            write_column(g.get(), "gutter_depression", a, u.length, "gutter depression");
            write_column(g.get(), "gutter_width", wdep, u.length, "gutter width");
            write_column(g.get(), "sides", sides, "count", "1 or 2 gutters");
            write_column(g.get(), "backing_width", tback, u.length, "backing width");
            write_column(g.get(), "backing_slope", sback, "percent", "backing slope");
            write_column(g.get(), "n_backing", nback, "dimensionless", "backing Manning's n");
        }
    }
}

}  // namespace

int write_model(const std::string& path, const SimulationContext& ctx_internal,
                std::vector<std::string>* warnings) {
    // Authored units, exactly like the .inp writer (STRATEGY.md §4): the same
    // display + authored passes on a COPY, so the live model is never mutated
    // and the numbers in the file are the numbers the modeller typed.
    const int us = ucf::getUnitSystem(static_cast<int>(ctx_internal.options.flow_units));
    const auto usz = static_cast<std::size_t>(us);
    const bool needs_display =
        ucf::Ucf[ucf::LENGTH][usz]   != 1.0 ||
        ucf::Ucf[ucf::RAINFALL][usz] != 1.0 ||
        ucf::Qcf[static_cast<std::size_t>(ctx_internal.options.flow_units)] != 1.0;
    const bool needs_authored = input::needs_authored_conversion(ctx_internal);

    SimulationContext copy;
    if (needs_display || needs_authored) {
        copy = ctx_internal;
        if (needs_display)  input::convert_internal_to_display(copy);
        if (needs_authored) input::convert_internal_to_authored(copy);
    }
    const SimulationContext& ctx = (needs_display || needs_authored) ? copy : ctx_internal;

    Handle file(H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT),
                Handle::Kind::File);
    if (!file) {
        if (warnings) warnings->push_back("HDF5: could not create '" + path + "'");
        return 1;
    }

    const UnitWords u = unit_words(us);
    write_root_attrs(file.get(), ctx, us);
    write_nodes(file.get(), ctx, u);
    write_node_subtypes(file.get(), ctx, u);
    write_links(file.get(), ctx, u);
    write_link_subtypes(file.get(), ctx, u);
    write_xsections(file.get(), ctx, u);
    write_subcatchments(file.get(), ctx, u);
    write_tables(file.get(), ctx);
    write_transects_and_streets(file.get(), ctx, u);
    write_geometry(file.get(), ctx, u);

    // Sections this slice does not yet carry. Named explicitly rather than
    // dropped in silence — the mistake the GeoPackage schema made and the
    // file-IO audit had to reconstruct by reading the DDL.
    if (warnings) {
        warnings->push_back(
            "HDF5 model schema " + std::string(kSchemaVersion) + " does not yet carry: "
            "[CONTROLS], [LID_CONTROLS], [LID_USAGE], [AQUIFERS], [GROUNDWATER], [GWF], "
            "[SNOWPACKS], [POLLUTANTS], [LANDUSES], [BUILDUP], [WASHOFF], [COVERAGES], "
            "[LOADINGS], [TREATMENT], [INFLOWS], [DWF], [RDII], [HYDROGRAPHS], "
            "[PATTERNS], [INLETS], [INLET_USAGE], [OPTIONS], [REPORT] and the 2D "
            "sections. Use the .inp or .gpkg writer for a complete model.");
    }
    return 0;
}

}  // namespace openswmm::h5io
