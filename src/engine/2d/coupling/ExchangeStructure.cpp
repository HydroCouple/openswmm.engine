/**
 * @file ExchangeStructure.cpp
 * @brief resolveExchangeStructure: a junction point's WEIR_ORIFICE law,
 *        structure and coefficients from [2D_NODE_COUPLING], presets and
 *        the global options.
 *
 * @details Kept apart from NodeCoupling.cpp, which the standalone Kokkos
 *          alignment test compiles without the engine library: this needs
 *          the node storage curves (node::getSurfArea).
 *
 * @see NodeCoupling.hpp
 * @ingroup engine_2d
 */

#include "NodeCoupling.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../core/UnitConversion.hpp"
#include "../../hydraulics/Node.hpp"

#include <algorithm>
#include <cmath>

namespace openswmm::twoD {

void resolveExchangeStructure(CouplingPoint& cp, const MeshData& mesh,
                              const SimulationContext& ctx,
                              const SolverOptions2D& opts, int row,
                              NodeCoupling2DType type, double project_len_to_m) {
    using Key = NodeCoupling2DKey;
    const auto ni = static_cast<std::size_t>(cp.node_idx);
    const NodeCoupling2DParams* p =
        row >= 0 ? &ctx.node_coupling_2d.params[static_cast<std::size_t>(row)] : nullptr;
    auto authored = [&](Key k, double& out) {
        if (!p) return false;
        const double v = (*p)[static_cast<std::size_t>(k)];
        if (std::isnan(v)) return false;
        out = v;
        return true;
    };

    double v = 0.0;
    const double f = project_len_to_m, f2 = f * f;
    cp.law = authored(Key::LAW, v) ? static_cast<int8_t>(v)
                                   : static_cast<int8_t>(opts.coupling_law);
    const bool weir_orifice = cp.law == static_cast<int8_t>(CouplingLaw2D::WEIR_ORIFICE);
    cp.equalize = opts.coupling_equalize >= 0 ? opts.coupling_equalize != 0 : weir_orifice;
    const bool chamber_node = authored(Key::CHAMBER_AREA, v);
    const double chamber_val = v * f2;
    cp.chamber_area = chamber_node ? chamber_val : cp.area;
    if (!weir_orifice) return;

    // ---- Coefficients ----------------------------------------------------
    const bool coeffs_authored = authored(Key::COEFFS, v);
    const int preset = coeffs_authored ? static_cast<int>(v) : opts.coupling_coeffs;
    exchange::Coefficients c;
    switch (static_cast<NodeCoupling2DCoeffs>(preset)) {
        case NodeCoupling2DCoeffs::RUBINATO2017:       c.c_w = 0.540; c.c_sw = 0.056; c.c_oi = 0.167; c.c_oo = 0.160; break;
        case NodeCoupling2DCoeffs::RUBINATO2017_LOWER: c.c_w = 0.493; c.c_sw = 0.055; c.c_oi = 0.160; c.c_oo = 0.159; break;
        case NodeCoupling2DCoeffs::RUBINATO2017_UPPER: c.c_w = 0.587; c.c_sw = 0.057; c.c_oi = 0.174; c.c_oo = 0.161; break;
        case NodeCoupling2DCoeffs::DEFAULT:            c.c_w = 0.540; c.c_oi = 0.167; c.c_oo = 0.160; break;
    }
    const bool open_pond = type == NodeCoupling2DType::OPEN && !coeffs_authored;
    if (open_pond) c.c_w = c.c_oi = c.c_oo = 1.0;   // open pond: the heads track
    if (authored(Key::CW, v)) c.c_w = v;
    // DEFAULT (and the open pond) keep the two weir forms continuous at
    // h1 = crest (D2): C_sw = ⅔·C_w. A larger C_sw would let the submerged
    // weir exceed the free one, so Q would rise with the sewer head.
    if (static_cast<NodeCoupling2DCoeffs>(preset) == NodeCoupling2DCoeffs::DEFAULT || open_pond)
        c.c_sw = (2.0 / 3.0) * c.c_w;
    if (authored(Key::CSW, v)) c.c_sw = v;
    if (authored(Key::COI, v)) c.c_oi = v;
    if (authored(Key::COO, v)) c.c_oo = v;
    if (cp.cd_authored) c.c_oi = c.c_oo = cp.cd;   // legacy map-row CD

    // ---- Geometry (SI) ---------------------------------------------------
    constexpr double kPi = 3.14159265358979323846;
    // Default manhole: 1.2 m, or 4 ft in US units.
    const double d_default = (f == 1.0) ? 1.2 : 4.0 * f;
    const double d = authored(Key::D, v) ? v * f : d_default;
    double area = kPi * d * d / 4.0;
    double perim = kPi * d;
    const bool area_node  = authored(Key::AREA, v);
    const double area_val = v * f2;
    double perim_val = 0.0;
    const bool perim_node = authored(Key::PERIMETER, perim_val);
    perim_val *= f;
    switch (type) {
        case NodeCoupling2DType::GRATE:
        case NodeCoupling2DType::CURB:
            if (area_node)             area = area_val;
            else if (cp.area_authored) area = cp.area;
            perim = perim_node ? perim_val : 4.0 * std::sqrt(area);
            break;
        case NodeCoupling2DType::OPEN: {
            if (area_node) {
                area = area_val;
            } else {
                const int us = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
                const double a_ft2 = node::getSurfArea(
                    ctx.nodes, cp.node_idx, ctx.nodes.full_depth[ni],
                    const_cast<TableData*>(&ctx.tables), us, &ctx.node_subtypes);
                if (a_ft2 > 0.0) area = a_ft2 * opts.len_1d_to_2d * opts.len_1d_to_2d;
            }
            perim = perim_node ? perim_val : std::sqrt(4.0 * kPi * area);
            break;
        }
        default:   // MANHOLE
            if (area_node)  area = area_val;
            if (perim_node) perim = perim_val;
            break;
    }
    if (cp.area_authored) area = cp.area;          // legacy map-row AREA
    cp.chamber_area = chamber_node ? chamber_val : area;
    const double lid = authored(Key::LID_OPEN_FRACTION, v) ? v : 1.0;
    cp.geo.a_orif = area * lid;
    cp.geo.l_weir = perim * lid;

    // ---- Crest -----------------------------------------------------------
    const double rim = (ctx.nodes.invert_elev[ni] + ctx.nodes.full_depth[ni]) * opts.len_1d_to_2d;
    const double bed = (cp.vertex_idx >= 0) ? mesh.vz[cp.vertex_idx] : mesh.tri_cz[cp.cell_idx];
    const int crest = authored(Key::CREST, v) ? static_cast<int>(v) : 0;
    switch (static_cast<NodeCoupling2DCrest>(crest)) {
        case NodeCoupling2DCrest::MESH_BED: cp.geo.z_crest = bed; break;
        case NodeCoupling2DCrest::MAX:      cp.geo.z_crest = std::max(rim, bed); break;
        case NodeCoupling2DCrest::MIN:      cp.geo.z_crest = std::min(rim, bed); break;
        case NodeCoupling2DCrest::NODE_RIM: cp.geo.z_crest = rim; break;
    }
    cp.coef = c;
}

} // namespace openswmm::twoD
