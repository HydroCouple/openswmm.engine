// SPDX-License-Identifier: Apache-2.0

/**
 * @file      CommonSourceStage.cpp
 * @brief     Shared source intervals and stage bookkeeping for coupled water owners.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "core/ProcessPerf.hpp"
#include "CommonSourceStage.hpp"
#include "SubsurfaceSolver.hpp"
#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../solver/SurfaceFluxCalculator.hpp"
#include "../solver/InertialKernels.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../core/CompletedSourceArchive.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace openswmm::twoD {
namespace {
constexpr double ft2 = .3048 * .3048, ft3 = ft2 * .3048;
/**
 * @brief Compare nonnegative physical areas with the reviewed-footprint closure tolerance.
 * @param[in] a First area m2, finite and nonnegative by caller validation.
 * @param[in] b Second area m2 under the same assumptions.
 * @return abs(a-b) <= 1e-8*max(1,a,b). No allocation, mutation or independent finite validation.
 */
bool closeArea(double a, double b) {
    return std::abs(a - b) <= 1e-8 * std::max(1.0, std::max(a, b));
}
/**
 * @brief Validate finite transport amounts with one optional signed temperature row.
 * @param[in] mass Native transport inventory vector; row-count checking belongs to the caller.
 * @param[in] temp_row Signed temperature row index, or -1 if absent.
 * @return True for finite amounts and nonnegative non-temperature rows. No zero-water pairing,
 * layout checking or state mutation occurs.
 */
bool validTransportMass(const std::vector<double>& mass, int temp_row) {
    for (std::size_t s = 0; s < mass.size(); ++s)
        if (!std::isfinite(mass[s]) || (int(s) != temp_row && mass[s] < 0))
            return false;
    return true;
}
using Key = std::tuple<SurfaceDonorKind, std::string, int>;
/**
 * @brief Copy the stable donor identity fields used for plan/receipt matching.
 * @param[in] r Stable intake request.
 * @return Owned tuple (kind,source,unit), excluding receiving cell and geometric/volume fields.
 * Source-string copying may allocate; inputs are not normalized or validated here.
 */
Key key(const SurfaceIntakeRequest& r) {
    return {r.kind, r.source, r.unit};
}
/**
 * @brief Sum cumulative spatial native-LID bottom infiltration for one private source.
 * @param[in] s Stable prepared source driver with consistent usage-order/group arrays.
 * @param[in] sc Source/subcatchment index to match; invalid/nonmatching indices yield zero.
 * @return SI volume m3, summing each matched wb_spatial_infil depth ft times area ft2 and the
 * ft3 conversion. Does not consume receipts, include non-LID infiltration or mutate source
 * state.
 */
double bottomTotal(const runoff::SourceWaterDriver& s, int sc) {
    double v = 0;
    for (const auto& [t, u] : s.lids().usageOrder()) {
        const auto& g = s.lids().group(t);
        if (g.subcatch_idx[u] == sc)
            v += g.wb_spatial_infil[u] * g.area[u] * ft3;
    }
    return v;
}
/**
 * @brief Check the finite local-bed/transport array layout for surface-only checkpoint
 * ownership.
 * @param[in] mesh Stable built mesh supplying cell count.
 * @param[in] surface Prepared heat/transport owners with corresponding dimensions/row metadata.
 * @return True for enabled finite bed without ground column, matching bed/cell/species arrays,
 * pollutant plus optional age/temperature layout, no MSX, and consistent special row identities.
 * False for layout mismatch. No physical values/finiteness, clocks or external owner lifetime
 * are validated; no mutation/allocation occurs.
 */
bool standaloneLocalBedLayout(const MeshData& mesh, const SurfaceStateData& surface) {
    const auto& h = surface.heat;
    const auto& t = surface.transport;
    const auto n = static_cast<std::size_t>(mesh.n_cells());
    const auto ns = static_cast<std::size_t>(t.n_species);
    const bool rows_supported =
        t.n_species >= 0 && t.n_pollut >= 0 && t.n_msx == 0 &&
        t.n_species == t.n_pollut + (t.age_row >= 0 ? 1 : 0) + (t.temp_row >= 0 ? 1 : 0) &&
        t.row_names.size() == ns &&
        (t.age_row < 0 || (t.age_row >= t.n_pollut && t.age_row < t.n_species &&
                           t.row_names[static_cast<std::size_t>(t.age_row)] == "__WATER_AGE__")) &&
        (t.temp_row < 0 ||
         (t.temp_row >= t.n_pollut && t.temp_row < t.n_species &&
          t.row_names[static_cast<std::size_t>(t.temp_row)] == "__TEMPERATURE__")) &&
        (t.age_row < 0 || t.temp_row < 0 || t.age_row != t.temp_row);
    return h.enabled && h.bed && !h.ground_column && h.bed_capacity.size() == n &&
           h.bed_energy.size() == n && h.initial_bed_energy.size() == n &&
           h.reported_temperature.size() == n && h.bed_pore_volume.size() == n &&
           h.bed_species == t.n_species && h.bed_temp_row == t.temp_row &&
           h.bed_age_row == t.age_row && h.bed_carried_mass.size() == ns * n &&
           h.bed_initial_mass.size() == ns * n && h.bed_exchange_mass.size() == ns * n &&
           (h.bed_generated_age.empty() || h.bed_generated_age.size() == n) &&
           (!t.active() || t.n_cells == mesh.n_cells()) && rows_supported;
}
/**
 * @brief Check canonical matched mesh/groundwater transport ownership for reviewed quality
 * stages.
 * @param[in] ctx Stable model context with network/subcatchment counts and compiled reaction
 * metadata.
 * @param[in] names Enabled pollutant identifiers in canonical order.
 * @param[in] surface Prepared mesh transport/heat state.
 * @param[in] gw Prepared active groundwater transport/heat owner.
 * @return True for matching row counts/names/cells/special rows/ledgers in a qualified shared
 * porous thermal profile or network-free aqueous profile. Complete dissolved BULK MSX may
 * participate only with compiled matching all-BULK layout and no nodes/links/subcatchments.
 * False otherwise.
 * @note Checks metadata/extents and owner activation, not finite inventories or every
 * concentration. No simulation state changes; stable consistent context/reaction arrays are
 * required.
 */
bool matchingGroundQualityProfile(const SimulationContext& ctx,
                                  const std::vector<std::string>& names,
                                  const SurfaceStateData& surface, const SubsurfaceSolver& gw) {
    const auto& a = surface.transport;
    const auto& b = gw.transport();
    const auto np = names.size();
    const bool rows_supported =
        a.n_species ==
            a.n_pollut + a.n_msx + (a.age_row >= 0 ? 1 : 0) + (a.temp_row >= 0 ? 1 : 0) &&
        a.row_names.size() == std::size_t(a.n_species) &&
        (a.age_row < 0 || (a.age_row >= a.n_pollut + a.n_msx && a.age_row < a.n_species &&
                           a.row_names[std::size_t(a.age_row)] == "__WATER_AGE__")) &&
        (a.temp_row < 0 || (a.temp_row >= a.n_pollut + a.n_msx && a.temp_row < a.n_species &&
                            a.row_names[std::size_t(a.temp_row)] == "__TEMPERATURE__")) &&
        (a.age_row < 0 || a.temp_row < 0 || a.age_row != a.temp_row);
    if (np == 0 && a.n_msx == 0 &&
        (ctx.n_nodes() != 0 || ctx.n_links() != 0 || ctx.n_subcatches() != 0))
        return false;
    const bool porous = gw.heat().active() && surface.heat.enabled && surface.heat.bed &&
                        surface.heat.ground_column;
    const bool aqueous = ctx.n_nodes() == 0 && ctx.n_links() == 0 && ctx.n_subcatches() == 0 &&
                         !ctx.options.heat_transport && !gw.heat().active() &&
                         !surface.heat.enabled && a.temp_row < 0 && b.temp_row < 0;
    if (!gw.active() || (!porous && !aqueous) || a.n_species <= 0 || a.n_species != b.n_species ||
        a.n_pollut != int(np) || b.n_pollut != int(np) || a.n_msx < 0 || a.n_msx != b.n_msx ||
        !rows_supported || a.row_names != b.row_names || a.n_cells != gw.state().n_cells ||
        b.n_cells != gw.state().n_cells ||
        a.cell_mass.size() != std::size_t(a.n_species) * std::size_t(a.n_cells) ||
        a.age_row != b.age_row || a.temp_row != b.temp_row ||
        (a.age_row >= 0 &&
         (a.age_row >= a.n_species || a.row_names[std::size_t(a.age_row)] != "__WATER_AGE__")) ||
        (a.temp_row >= 0 && (a.temp_row >= a.n_species ||
                             a.row_names[std::size_t(a.temp_row)] != "__TEMPERATURE__")) ||
        (!a.rain_conc.empty() && a.rain_conc.size() != std::size_t(a.n_species)))
        return false;
    const bool only_temperature = a.n_species == 1 && a.temp_row == 0;
    if (porous && (a.gained_bed.size() != std::size_t(a.n_species) ||
                   b.generated_age.size() != b.gained_contact.size() ||
                   ((!only_temperature || !b.generated_age.empty()) &&
                    b.generated_age.size() != std::size_t(b.n_species))))
        return false;
    if (aqueous &&
        (!a.gained_bed.empty() ||
         (!b.generated_age.empty() && b.generated_age.size() != std::size_t(b.n_species)) ||
         b.generated_age.size() != b.gained_contact.size()))
        return false;
    for (std::size_t p = 0; p < np; ++p)
        if (a.row_names[p] != names[p])
            return false;
    if (a.n_msx > 0) {
        // This adds no private network/catchment reaction owner. Both mesh
        // inventories use the same complete BULK layout and native units.
        const auto& rx = ctx.reactions;
        if (ctx.n_nodes() != 0 || ctx.n_links() != 0 || ctx.n_subcatches() != 0 || !rx.compiled ||
            a.n_msx != rx.n_species() ||
            std::any_of(rx.species_is_wall.begin(), rx.species_is_wall.end(),
                        [](auto v) { return v != 0; }))
            return false;
        for (int row = 0; row < a.n_msx; ++row)
            if (a.row_names[np + row] != rx.species_name[row])
                return false;
    }
    return true;
}
/**
 * @brief Qualify matched mesh/groundwater quality only when no private source groups exist.
 * @param[in] sources Stable source owner supplying context, pollutant IDs and group presence.
 * @param[in] surface Prepared mesh transport/heat state.
 * @param[in] gw Active prepared groundwater transport/heat owner.
 * @return Empty private groups and matchingGroundQualityProfile() both true. No inventory or
 * clock is changed and no private catchment reaction owner is created.
 */
bool sharedGroundQualityProfile(const runoff::SourceWaterDriver& sources,
                                const SurfaceStateData& surface, const SubsurfaceSolver& gw) {
    return sources.clocks().groups().empty() &&
           matchingGroundQualityProfile(sources.context(), sources.pollutantNames(), surface, gw);
}
/**
 * @brief Validate the common stage's supported water/pollutant/mesh-only quality layout.
 * @param[in] sources Stable prepared private source owner and canonical pollutant IDs.
 * @param[in] surface Stable mesh transport/heat owner.
 * @param[in] gw Stable prepared groundwater transport/heat owner.
 * @param[in] standalone_local_bed True selects only the enabled local-bed/non-ground-column flag
 * check; full standaloneLocalBedLayout() validation belongs to attachment/boundary
 * qualification.
 * @return Empty for qualified shared mesh-ground layout, water-only, exact heat-only mesh
 * profile, or conventional identical pollutant-only source/mesh/GW layout with valid rainfall
 * concentrations; otherwise diagnostic. Conventional private-source layouts exclude age, heat
 * and MSX.
 * @note Read-only. This validates selected metadata/extents/signs, not complete runtime
 * inventories or physical receipt balance. Temporary comparison containers and diagnostics can
 * allocate.
 */
std::string qualityProfile(const runoff::SourceWaterDriver& sources,
                           const SurfaceStateData& surface, const SubsurfaceSolver& gw,
                           bool standalone_local_bed = false) {
    if (standalone_local_bed)
        return surface.heat.bed && surface.heat.enabled && !surface.heat.ground_column
                   ? std::string{}
                   : "Standalone local-bed checkpoint transport layout differs.";
    const auto& names = sources.pollutantNames();
    const int np = int(names.size());
    const auto& a = surface.transport;
    const auto& b = gw.transport();
    if (sharedGroundQualityProfile(sources, surface, gw))
        return {};
    if (np == 0) {
        if (a.n_species == 0 && !b.active())
            return {};
        const bool heat_only =
            sources.clocks().groups().empty() && surface.heat.enabled && gw.heat().active() &&
            a.n_pollut == 0 && a.n_msx == 0 && a.age_row < 0 && a.temp_row == 0 &&
            a.n_species == 1 && a.row_names == std::vector<std::string>{"__TEMPERATURE__"} &&
            b.n_pollut == 0 && b.n_msx == 0 && b.age_row < 0 && b.temp_row == 0 &&
            b.n_species == 1 && b.row_names == std::vector<std::string>{"__TEMPERATURE__"};
        return heat_only ? std::string{}
                         : "Common source stage quality/age/heat/MSX is not qualified.";
    }
    if (a.n_species != np || a.n_pollut != np || a.n_msx != 0 || a.age_row >= 0 ||
        a.temp_row >= 0 || a.row_names != names || b.n_species != np || b.n_pollut != np ||
        b.n_msx != 0 || b.age_row >= 0 || b.temp_row >= 0 || b.row_names != names ||
        a.n_cells != gw.state().n_cells || b.n_cells != gw.state().n_cells ||
        a.cell_mass.size() != std::size_t(np * a.n_cells) ||
        a.gained_rainfall.size() != std::size_t(np) ||
        a.lost_infiltration.size() != std::size_t(np) ||
        a.gained_exfiltration.size() != std::size_t(np) ||
        (!a.rain_conc.empty() && a.rain_conc.size() != std::size_t(np)) ||
        b.xacc_from_surface.size() != std::size_t(np * b.n_cells) ||
        b.xacc_to_surface.size() != std::size_t(np * b.n_cells))
        return "Common source pollutant layout must match the source pollutant IDs; age/heat/MSX are not qualified.";
    for (double concentration : a.rain_conc)
        if (!std::isfinite(concentration) || concentration < 0)
            return "Common source mesh rainfall concentration is invalid.";
    return {};
}
}

/**
 * @brief Check matched mesh/ground quality against the context's enabled pollutant order.
 * @param[in] ctx Stable model context; ignore_quality yields an empty pollutant list.
 * @param[in] surface Prepared mesh transport/heat owner.
 * @param[in] gw Prepared groundwater transport/heat owner.
 * @return matchingGroundQualityProfile() for canonical enabled pollutant names. No ownership is
 * bound or state mutated. Name copying may allocate/throw; runtime inventory validation is
 * separate.
 */
bool CommonSourceStage::supportsMeshGroundQuality(const SimulationContext& ctx,
                                                  const SurfaceStateData& surface,
                                                  const SubsurfaceSolver& gw) {
    std::vector<std::string> names;
    if (!ctx.options.ignore_quality)
        for (int row = 0; row < ctx.n_pollutants(); ++row)
            names.push_back(ctx.pollutant_names.name_of(row));
    return matchingGroundQualityProfile(ctx, names, surface, gw);
}

/**
 * @brief Bind reviewed uniform source/mesh weather contacts to a prepared initial receiver
 * stage.
 * @param[in] mesh Stable built SI mixed mesh; referenced arrays must be internally sized/valid.
 * @param[in,out] surface Prepared cell water/transport/heat/snow state. Successful attachment
 * can reinitialize mesh snow with remaining mesh weather areas while retaining its
 * ground-contact mask.
 * @param[in] options Stable prepared solver options, borrowed for the lifetime of the stage.
 * @param[in,out] gw Prepared receiver with initial empty exchange queues and unbound receipt
 * ownership; inactive GW is allowed only for the qualified mesh-only standalone LOCAL_STORAGE
 * bed profile.
 * @param[in,out] sources Prepared private source driver at zero completed group clocks with no
 * pending group interval; spatial coverage is configured from reviewed inside/outside fractions.
 * @param[in] preview Complete valid noncancelled ownership preview matching cell weather
 * extents; reviewed component/source/mesh areas must close and not overbook physical contacts.
 * @param[in] mesh_only True assigns full mesh weather without private source groups/preview
 * objects.
 * @param[in] runtime_water_identity Optional bounded nonempty explicit water-provider replay
 * identity; only reviewed water-only mesh/private-catchment profiles accept it, with no active
 * rows at attachment.
 * @param[in] runtime_boundaries Enables qualified copied hydraulic boundaries; requires the
 * runtime water identity and a live authored boundary owner whose descriptors are copied for
 * matching.
 * @param[in] runtime_catchment_forcing Enables reviewed private-catchment forcing; requires
 * groups, identity, non-mesh-only mode and compatible snow forcing phase.
 * @return Empty after binding pointers/contacts/profiles and freezing checkpoint model identity;
 * diagnostic on existing attachment, unsupported profile, source
 * clocks/coverage/contact/receiver or participant refusal. Allocation/configuration exceptions
 * may propagate.
 * @note Mesh/options/source/GW/surface and their contexts remain borrowed and must outlive this
 * stage. Most checks/local preparation precede binding, but coverage/receipt configuration and
 * final copied state/identity creation can mutate owners before a later exception; no
 * whole-attachment rollback is provided. Use the enclosing engine transaction when atomic model
 * adoption is required.
 */
std::string CommonSourceStage::initialize(const MeshData& mesh, SurfaceStateData& surface,
                                          const SolverOptions2D& options, SubsurfaceSolver& gw,
                                          runoff::SourceWaterDriver& sources,
                                          const SurfaceOwnershipPreview& preview, bool mesh_only,
                                          const std::string& runtime_water_identity,
                                          bool runtime_boundaries, bool runtime_catchment_forcing) {
    if (mesh_)
        return "Common source stage already initialized.";
    if (runtime_catchment_forcing && sources.snow().active())
        return "Runtime catchment providers require phase-aware snow forcing receipts.";
    if (!preview.valid() || preview.cancelled ||
        preview.mesh_weather_area.size() != mesh.tri_area.size())
        return "Common source stage requires a valid reviewed mesh/receiver snapshot.";
    const bool standalone_local_bed = !gw.active();
    if (standalone_local_bed &&
        (!mesh_only || !sources.clocks().groups().empty() || !preview.objects.empty() ||
         !preview.shares.empty() || !runtime_water_identity.empty() || runtime_boundaries ||
         runtime_catchment_forcing || !standaloneLocalBedLayout(mesh, surface)))
        return "Inactive groundwater supports checkpoint attachment only for a standalone LOCAL_STORAGE bed with no reviewed source ownership.";
    const auto quality_error = qualityProfile(sources, surface, gw, standalone_local_bed);
    if (!quality_error.empty())
        return quality_error;
    const bool mesh_only_ground_quality =
        mesh_only && sharedGroundQualityProfile(sources, surface, gw);
    if (runtime_catchment_forcing &&
        (mesh_only || runtime_water_identity.empty() || sources.clocks().groups().empty()))
        return "Catchment provider forcing requires its explicit reviewed private water profile.";
    if (!runtime_water_identity.empty() &&
        ((!mesh_only && !runtime_catchment_forcing) || !sources.pollutantNames().empty() ||
         runtime_water_identity.size() >= 64 * 1024 * 1024))
        return "Runtime water source stage requires an explicit water-only footprint and bounded replay identity.";
    if (runtime_boundaries && (runtime_water_identity.empty() || !surface.boundary))
        return "Runtime boundaries require the initial water profile and authored boundary model.";
    if (surface.runtime_sources.active() || surface.runtime_forcings.active() ||
        gw.runtimeSources().active())
        return "Common source stage runtime coupling providers are not qualified.";
    if (surface.volume.size() != mesh.tri_area.size() ||
        (!standalone_local_bed && gw.state().n_cells != mesh.n_cells()))
        return "Common source stage mesh/receiver sizes differ.";
    for (const auto& g : sources.clocks().groups())
        if (g.completed_end != 0 || !g.pending.empty())
            return "Common source stage must attach at the initial source clock.";
    const auto& c = sources.context();
    const auto& r = sources.runoff().soa();
    if (mesh_only &&
        (!sources.clocks().groups().empty() || !preview.objects.empty() || !preview.shares.empty()))
        return "Mesh-only source stage cannot own private subcatchment sources.";
    std::vector<Donor> donors;
    std::vector<bool> owned(mesh.tri_area.size(), mesh_only && !standalone_local_bed);
    std::vector<double> coverage(mesh.tri_area.size(), 0);
    std::vector<bool> resolved(c.n_subcatches(), false);
    std::vector<double> fractions(c.n_subcatches(), 0);
    std::vector<std::vector<std::pair<int, double>>> forcing_contacts(c.n_subcatches());
    for (const auto& o : preview.objects) {
        const int sc = o.subcatch;
        if (sc < 0 || sc >= c.n_subcatches())
            return "Invalid reviewed common source.";
        if (sources.clocks().groupForSubcatch(sc) < 0) {
            if (o.reviewed)
                return "Reviewed common source is outside its clock groups.";
            continue;
        }
        if (o.lumped)
            return "Common source stage lumped groundwater is not qualified.";
        if (resolved[sc])
            return "Duplicate common source footprint.";
        resolved[sc] = true;
        const double full = (r.area[sc] + c.subcatches.total_lid_area_ft2[sc]) * ft2;
        if (!std::isfinite(full) || full <= 0)
            return "Common source has no physical footprint.";
        Donor d;
        d.source = sc;
        double weather = 0, pervious = 0, impervious = 0, lid_area = 0;
        for (const auto& share : preview.shares)
            if (share.subcatch == sc) {
                if (share.cell < 0 || share.cell >= mesh.n_cells())
                    return "Invalid common source contact.";
                for (double area : {share.weather_area, share.pervious_area, share.impervious_area,
                                    share.lid_area})
                    if (!std::isfinite(area) || area < 0)
                        return "Invalid common source contact area.";
                if (!closeArea(share.weather_area,
                               share.pervious_area + share.impervious_area + share.lid_area))
                    return "Reviewed component areas do not close their weather footprint.";
                if (share.weather_area > 0) {
                    owned[share.cell] = true;
                    coverage[share.cell] += share.weather_area;
                }
                if (share.weather_area > 0) {
                    auto& contacts = forcing_contacts[sc];
                    const auto found =
                        std::find_if(contacts.begin(), contacts.end(),
                                     [&](const auto& x) { return x.first == share.cell; });
                    if (found == contacts.end())
                        contacts.emplace_back(share.cell, share.weather_area);
                    else
                        found->second += share.weather_area;
                }
                weather += share.weather_area;
                pervious += share.pervious_area;
                impervious += share.impervious_area;
                lid_area += share.lid_area;
                if (share.pervious_area > 0)
                    d.contacts.emplace_back(share.cell, share.pervious_area);
            }
        if (weather > 0 && !o.reviewed)
            return "Inside source shares require reviewed uniform ownership.";
        if (!std::isfinite(o.inside_area) || o.inside_area < 0 ||
            !closeArea(weather, o.inside_area) || !std::isfinite(o.outside_area) ||
            o.outside_area < 0 || !closeArea(weather + o.outside_area, full) ||
            (o.declared_area > 0 && !closeArea(o.declared_area, full)))
            return "Reviewed inside/outside shares do not match private physical water areas.";
        fractions[sc] = o.outside_area == 0 ? 1.0
                        : weather == 0      ? 0.0
                                            : std::clamp(weather / full, 0.0, 1.0);
        if (!closeArea(pervious, r.area[sc] * (1 - r.imperv_pct[sc]) * ft2 * fractions[sc]) ||
            !closeArea(impervious, r.area[sc] * r.imperv_pct[sc] * ft2 * fractions[sc]) ||
            !closeArea(lid_area, c.subcatches.total_lid_area_ft2[sc] * ft2 * fractions[sc]))
            return "Reviewed components do not match uniform inside/outside footprints.";
        d.area = pervious;
        if (d.area > 0)
            donors.push_back(d);
        for (std::size_t id = 0; id < sources.lids().usageOrder().size(); ++id) {
            const auto [t, u] = sources.lids().usageOrder()[id];
            const auto& g = sources.lids().group(t);
            if (g.subcatch_idx[u] != sc ||
                !lid::hasNativeBottom(g.type, g.stor_thick[u], g.stor_ksat[u]) ||
                fractions[sc] == 0)
                continue;
            Donor l;
            l.source = sc;
            l.type = t;
            l.unit = u;
            l.identity = int(id);
            l.area = g.area[u] * ft2 * fractions[sc];
            for (const auto& share : preview.shares)
                if (share.subcatch == sc && share.lid_area > 0)
                    l.contacts.emplace_back(share.cell, share.lid_area * g.area[u] /
                                                            c.subcatches.total_lid_area_ft2[sc]);
            double total = 0;
            for (const auto& x : l.contacts)
                total += x.second;
            if (!closeArea(total, l.area))
                return "Reviewed LID bottom shares do not match its inside footprint.";
            if (l.area > 0)
                donors.push_back(std::move(l));
        }
    }
    for (const auto& g : sources.clocks().groups())
        for (int sc : g.subcatches)
            if (!resolved[sc])
                return "Every member of a common source clock group needs a resolved inside/outside footprint.";
    std::vector<std::vector<std::pair<int, double>>> et_contacts;
    std::vector<double> atmospheric_area(mesh.tri_area.size(), 0);
    for (const auto& b : sources.atmosphere()) {
        std::vector<std::pair<int, double>> contacts;
        double total = 0;
        for (const auto& sh : preview.shares)
            if (sh.subcatch == b.source) {
                double area = b.kind == runoff::SourceEtKind::PERVIOUS ? sh.pervious_area
                                                                       : sh.impervious_area;
                if (b.kind == runoff::SourceEtKind::LID)
                    area = sh.lid_area * b.area / c.subcatches.total_lid_area_ft2[b.source];
                if (!std::isfinite(area) || area < 0)
                    return "Invalid common atmospheric contact area.";
                if (area > 0) {
                    contacts.emplace_back(sh.cell, area);
                    total += area;
                    atmospheric_area[sh.cell] += area;
                }
            }
        if (!closeArea(total, b.area * ft2 * fractions[b.source]))
            return "Reviewed atmospheric shares do not match private component footprints.";
        et_contacts.push_back(std::move(contacts));
    }
    std::vector<int> cells;
    for (int i = 0; i < mesh.n_cells(); ++i) {
        if (!owned[i] && !standalone_local_bed)
            continue;
        const double w = preview.mesh_weather_area[i];
        if (!std::isfinite(w) || w < 0 || !closeArea(coverage[i] + w, mesh.tri_area[i]))
            return "Reviewed mesh/source weather areas do not close.";
        if (!closeArea(atmospheric_area[i], coverage[i]))
            return "Reviewed atmospheric components do not close the cell's owned weather area.";
        double contact = w;
        for (const auto& d : donors)
            for (const auto& x : d.contacts)
                if (x.first == i)
                    contact += x.second;
        if (contact > mesh.tri_area[i] * (1 + 1e-8))
            return "Common source contact areas overbook the cell.";
        if (owned[i] || standalone_local_bed)
            cells.push_back(i);
    }
    if (cells.empty())
        return "Common source stage has no reviewed receiving cells.";
    if (!standalone_local_bed) {
        if (gw.sourceReceiptOwnershipBound())
            return "Aquifer source receipt ownership already bound.";
        if (!gw.lidReceiptsEnabled() &&
            std::any_of(gw.richards_bed_owned_.begin(), gw.richards_bed_owned_.end(),
                        [](bool x) { return x; }))
            return "Common source ownership must bind before Richards owner firings.";
        // Both bindings are initial-state operations. Validate receiver queues
        // before changing the source partition so an invalid attachment is retryable.
        for (int i : cells) {
            if (gw.state().xacc_from_surface[i] != 0 || gw.state().xacc_to_surface[i] != 0)
                return "Common source ownership must bind before pending aquifer exchange.";
            for (int p = 0; p < gw.transport().n_species; ++p)
                if (gw.transport().xacc_from_surface[gw.transport().idx(p, i)] != 0 ||
                    gw.transport().xacc_to_surface[gw.transport().idx(p, i)] != 0)
                    return "Common source ownership must bind before pending aquifer mass exchange.";
        }
    }
    if (runtime_boundaries) {
        RuntimeWaterCheckpointBinding binding;
        binding.identity = runtime_water_identity;
        binding.authored_boundary = surface.boundary;
        binding.boundary_tables = &sources.context().tables;
        for (double zs : gw.state().zs)
            binding.groundwater_cells.push_back(zs > 0);
        binding.boundary_slots.assign(mesh.n_cells() * kMaxCellVerts, false);
        for (int c = 0; c < mesh.n_cells(); ++c)
            for (int edge = 0; edge < mesh.cell_vertex_count(c); ++edge)
                if (mesh.cell_neighbour(c, edge) < 0)
                    binding.boundary_slots[MeshData::slot(c, edge)] = true;
        const auto invalid =
            RuntimeWaterCheckpoint::validate(binding, surface.runtime_sources, gw.runtimeSources(),
                                             surface.runtime_forcings, surface.boundary);
        if (!invalid.empty())
            return invalid;
    }
    auto mesh_snow = surface.snow;
    if (mesh_snow.active()) {
        if (!surface.snow_context)
            return "Mesh snow has no weather owner.";
        const auto ground_contact = mesh_snow.groundContactMask();
        auto geometry = mesh_snow.geometry();
        for (std::size_t i = 0; i < geometry.size(); ++i)
            geometry[i].area = preview.mesh_weather_area[i];
        if (auto error = mesh_snow.initialize(*surface.snow_context, geometry); !error.empty())
            return error;
        if (ground_contact.size() == geometry.size())
            if (auto error = mesh_snow.setGroundContactMask(ground_contact); !error.empty())
                return error;
    }
    if (!standalone_local_bed) {
        const auto partition_error = sources.configureSpatialCoverage(fractions);
        if (!partition_error.empty())
            return partition_error;
        const auto receipt_error = gw.configureSourceReceipts(cells);
        if (!receipt_error.empty())
            return receipt_error;
    }
    surface.snow = std::move(mesh_snow);
    mesh_ = &mesh;
    surface_ = &surface;
    options_ = &options;
    gw_ = &gw;
    sources_ = &sources;
    donors_ = std::move(donors);
    cells_ = std::move(cells);
    owned_ = std::move(owned);
    weather_ = preview.mesh_weather_area;
    et_contacts_ = std::move(et_contacts);
    runtime_water_identity_ = runtime_water_identity;
    runtime_boundaries_ = runtime_boundaries;
    standalone_local_bed_ = standalone_local_bed;
    mesh_only_ground_quality_ = mesh_only_ground_quality;
    runtime_catchment_forcing_ = runtime_catchment_forcing;
    forcing_contacts_ = std::move(forcing_contacts);
    if (runtime_boundaries)
        authored_boundary_ = *surface.boundary;
    checkpoint_model_ = checkpointModel();
    return {};
}

/*
 * @brief Synchronize standalone-local-bed cumulative water diagnostics from accepted mesh
 * ledgers.
 * - ctx (in): Stable simulation context supplying SI mass_balance_2d rainfall_in, evap_out and
 * aquifer_in volumes m3.
 * @note Copies these three totals only for standalone_local_bed_; otherwise does nothing. Does
 * not change physical water, source intervals, receipt queues or hydraulic reporting clocks.
 * Caller orders this after the appropriate accepted surface ledger publication.
 */
void CommonSourceStage::synchronizeSurfaceLedgers(const SimulationContext& ctx) {
    if (!standalone_local_bed_)
        return;
    mesh_rain_ = ctx.mass_balance_2d.rainfall_in;
    mesh_evap_ = ctx.mass_balance_2d.evap_out;
    mesh_return_ = ctx.mass_balance_2d.aquifer_in;
}

/**
 * @brief Build copied-profile checkpoint identity and receiver/exterior-edge eligibility masks.
 * @param[in] time Completed-boundary elapsed seconds to bind; finiteness is checked downstream.
 * @return Owned mask/identity value borrowing this stage's authored boundary and source table
 * pointers when runtime boundaries are enabled. Ground coverage is zs>0 and exterior slots use
 * the mesh's actual local-edge count within four-slot storage.
 * @note Requires live attached GW/mesh/source owners; no null/extent validation occurs. Returned
 * borrowed pointers cannot outlive those owners/stage. Allocates masks/identity and can throw;
 * physical state/prescriptions remain unchanged.
 */
RuntimeWaterCheckpointBinding CommonSourceStage::runtimeWaterBinding(double time) const {
    RuntimeWaterCheckpointBinding b;
    b.identity = checkpoint_model_;
    b.completed_time = time;
    for (double zs : gw_->state().zs)
        b.groundwater_cells.push_back(zs > 0);
    if (runtime_boundaries_) {
        b.authored_boundary = &authored_boundary_;
        b.boundary_tables = &sources_->context().tables;
        b.boundary_slots.assign(mesh_->n_cells() * kMaxCellVerts, false);
        for (int c = 0; c < mesh_->n_cells(); ++c)
            for (int e = 0; e < mesh_->cell_vertex_count(c); ++e)
                if (mesh_->cell_neighbour(c, e) < 0)
                    b.boundary_slots[MeshData::slot(c, e)] = true;
    }
    return b;
}
/**
 * @brief Validate copied source/forcing prescriptions against this stage's completed-profile
 * binding.
 * @param[in] surface Stable surface provider rows/receipts.
 * @param[in] groundwater Stable groundwater provider rows/receipts.
 * @param[in] forcing Stable weather/paired-infiltration provider rows/receipts.
 * @param[in] time Completed elapsed seconds bound to the validation snapshot.
 * @param[in] boundary Optional stable live boundary override for enabled runtime-boundary
 * profile; null then uses the attached surface boundary. Ignored when that profile is disabled.
 * @return Disabled-profile diagnostic or RuntimeWaterCheckpoint::validate() result, empty on
 * success.
 * @note Requires attached stable owners; binding/mask construction can allocate/throw. No
 * provider expiry, physics mutation, file restore or ownership transfer occurs.
 */
std::string CommonSourceStage::validateRuntimeWater(const RuntimeSources& surface,
                                                    const RuntimeSources& groundwater,
                                                    const RuntimeForcings& forcing, double time,
                                                    const BoundaryData* boundary) const {
    if (!runtimeWaterEnabled())
        return "Common source runtime providers require their initial water profile.";
    return RuntimeWaterCheckpoint::validate(
        runtimeWaterBinding(time), surface, groundwater, forcing,
        runtime_boundaries_ ? (boundary ? boundary : surface_->boundary) : nullptr);
}

/**
 * @brief Qualify the attached accepted component boundary before save/inspection/restore.
 * @return Empty if all reviewed source/GW/surface/profile conditions agree; otherwise the first
 * qualification or caught participant round-trip diagnostic. No simulation clock or water state
 * is advanced. Temporary copy/archive allocation can propagate outside the final participant
 * catch.
 * @note Requires attached mesh/source/GW/surface, no pending exchange plan, matching snow/source
 * clocks with no pending source group interval, qualified transport/thermal ownership, no
 * runtime boundary quality, supported provider profile and no receiver firing/intake beyond
 * completedEnd(). Expired runtime prescriptions are checked on copies. For boundary providers,
 * gross GW receipts must fit completed physical source ledgers. Source/GW checkpoint round trips
 * on private state validate receipt pairing and balances; standalone local beds omit inactive
 * groundwater runtime. Requires stable borrowed owners and serial access; supplies no
 * whole-engine checkpoint guarantee.
 */
std::string CommonSourceStage::checkpointBoundary() const {
    if (!mesh_ || !sources_ || !gw_ || !surface_ || exchange_.planned_)
        return "Checkpoint requires an attached accepted source boundary.";
    if (surface_->snow.active() && surface_->snow.completedEnd() != completedEnd())
        return "Checkpoint mesh snow clock does not match the accepted source boundary.";
    for (const auto& group : sources_->clocks().groups())
        if (!group.pending.empty() || group.completed_end != completedEnd())
            return "Checkpoint source clocks do not match the accepted boundary.";
    if (standalone_local_bed_ && !standaloneLocalBedLayout(*mesh_, *surface_))
        return "Standalone local-bed checkpoint transport layout differs.";
    const auto error = qualityProfile(*sources_, *surface_, *gw_, standalone_local_bed_);
    if (!error.empty())
        return error;
    if (!surface_->transport.runtime_bc_conc.empty())
        return "Checkpoint runtime boundary quality is not qualified.";
    if (runtimeWaterEnabled()) {
        auto surface = surface_->runtime_sources, groundwater = gw_->runtimeSources();
        auto forcing = surface_->runtime_forcings;
        surface.expire(completedEnd());
        groundwater.expire(completedEnd());
        forcing.expire(completedEnd());
        const auto invalid = validateRuntimeWater(surface, groundwater, forcing, completedEnd());
        if (!invalid.empty())
            return invalid;
        if (runtime_boundaries_) {
            double incoming = 0, outgoing = 0;
            for (const auto& r : groundwater.rows) {
                incoming += r.receipt.water_in;
                outgoing += r.receipt.water_out;
            }
            const auto& state = gw_->state();
            if (!std::isfinite(state.led_source_in) || !std::isfinite(state.led_source_out) ||
                incoming > state.led_source_in + 1e-10 * std::max(1., incoming) ||
                outgoing > state.led_source_out + 1e-10 * std::max(1., outgoing))
                return "Groundwater provider receipts exceed their completed physical source ledgers.";
        }
    } else if (!surface_->runtime_sources.rows.empty() ||
               !surface_->runtime_forcings.rows.empty() || !gw_->runtimeSources().rows.empty())
        return "Checkpoint runtime provider history is not qualified.";
    if (!standalone_local_bed_)
        for (int cell : cells_) {
            if (gw_->sourceCompletedTime(cell) > completedEnd())
                return "Checkpoint aquifer firing exceeds the accepted source horizon.";
            for (const auto& receipt : gw_->pendingSourceReceipts(cell))
                if (receipt.end > completedEnd())
                    return "Checkpoint intake exceeds the accepted source horizon.";
        }
    // Check required pairing and private balances on copies, on save as well as load.
    try {
        detail::CompletedSourceArchive a;
        sources_->checkpoint(a, false);
        auto source = sources_->waterTrial();
        detail::CompletedSourceArchive b(a.bytes());
        source.checkpoint(b, false);
        b.finish();
        if (!standalone_local_bed_) {
            auto gw = *gw_;
            detail::CompletedSourceArchive c;
            gw.checkpoint(c, false);
            detail::CompletedSourceArchive d(c.bytes());
            gw.checkpoint(d, false);
            d.finish();
        }
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

/**
 * @brief Settle one contiguous accepted source interval after surface face transfers have
 * landed.
 * @param[in] start Elapsed interval start s, exactly completedEnd(); each private source group
 * must already be complete at this same time with no staged pending interval.
 * @param[in] end Finite elapsed end s, strictly later than start. Enabled copied provider
 * prescriptions must remain valid through the interval without crossing their next expiry.
 * @return Empty after successful settlement and incrementing the accepted interval count;
 * otherwise a diagnostic for attachment/clocks/profile/forcing/ownership, participant or paired
 * water/species/ ET budget refusal. Allocation or other participant exceptions can propagate
 * without translation.
 * @details Uses remaining mesh weather footprints and private source trials, consumes only
 * returns available by start, attributes physical emergence to local owners and numerical
 * refunds to their original donors, and plans shared finite aquifer intake. A second bounded
 * private-source trial withdraws actual water/species against awards; denied capacity is not
 * moved to another cell. Receiver booking precedes donor adoption. Successful settlement updates
 * mesh water/head/depth, source state, infiltration/coupling/weather/species/provider/ET
 * ledgers, last receipts and receiver source horizon; it queues aquifer intake rather than
 * advancing the groundwater marcher itself.
 * @note Requires exclusive serial access to all attached owners, stable
 * geometry/options/context, and prepared consistent arrays. Normal refusal paths after planning
 * cancel that plan, but an exception can leave a pending plan, and receiver provenance
 * allocation during commit can interrupt after partial booking. No enclosing whole-engine
 * rollback is provided here; the caller supplies its accepted-step transaction when atomic
 * physical adoption is required. Previously accepted face transfers remain the caller's
 * responsibility on refusal. A standalone local bed instead advances its copied mesh snow and
 * synchronization horizon only; ordinary mesh source kernels retain rain/ evaporation/physical
 * water ownership. Source driver ft/ft3 quantities are converted at the stage boundary; stage
 * water receipts are m3, times s and pollutant inventory native_concentration*m3.
 */
std::string CommonSourceStage::advance(double start, double end) {
    perf::ProcessTimer process_timer(perf::ProcessPhase::common_source);
    if (!mesh_ || !std::isfinite(end) || end <= start || start != completedEnd())
        return "Invalid common source interval.";
    const double dt = end - start;
    if (standalone_local_bed_) {
        perf::count(perf::ProcessCounter::mesh_snow_copies);
        auto snow = surface_->snow;
        if (snow.active()) {
            if (!surface_->snow_context)
                return "Mesh snow has no weather owner.";
            const auto error = snow.advanceTrial(*surface_->snow_context, start, end,
                                                 surface_->rainfall, surface_->snow_catch,
                                                 &surface_->snow);
            if (!error.empty())
                return error;
        }
        surface_->snow = std::move(snow);
        exchange_.completed_end_ = end;
        ++intervals_;
        return {};
    }
    for (const auto& g : sources_->clocks().groups())
        if (g.completed_end != start || !g.pending.empty())
            return "Common source clocks do not match the accepted mesh source interval.";
    const auto quality_error = qualityProfile(*sources_, *surface_, *gw_);
    if (!quality_error.empty())
        return quality_error;
    const int np = int(sources_->pollutantNames().size());
    const int ntransport = surface_->transport.n_species;
    const int temp_row = surface_->transport.temp_row;
    if (runtimeWaterEnabled()) {
        // Boundary face receipts have just advanced, while the router books
        // cumulative edge flux after this source interval. Check descriptors
        // here; accepted checkpoint validation pairs the completed ledgers.
        const auto invalid = RuntimeWaterCheckpoint::validate(
            runtimeWaterBinding(start), surface_->runtime_sources, gw_->runtimeSources(),
            surface_->runtime_forcings, runtime_boundaries_ ? surface_->boundary : nullptr, false);
        if (!invalid.empty())
            return invalid;
        if (end > std::min({surface_->runtime_sources.nextTime(start),
                            gw_->runtimeSources().nextTime(start),
                            surface_->runtime_forcings.nextTime(start)}))
            return "Common source interval crosses provider expiry.";
    } else if (surface_->runtime_sources.active() || surface_->runtime_forcings.active() ||
               gw_->runtimeSources().active())
        return "Common source runtime provider profile changed to an unsupported configuration.";
    perf::count(perf::ProcessCounter::mesh_snow_copies);
    auto snow = surface_->snow;
    if (snow.active()) {
        if (!surface_->snow_context)
            return "Mesh snow has no weather owner.";
        auto rain = surface_->rainfall;
        for (int i = 0; i < int(rain.size()); ++i)
            rain[i] = surface_->runtime_forcings.rate(1, i, rain[i]);
        const auto error =
            snow.advanceTrial(*surface_->snow_context, start, end, rain, surface_->snow_catch,
                              &surface_->snow);
        if (!error.empty())
            return error;
    }
    std::vector<SurfaceReturnReceipt> ready_returns;
    std::vector<runoff::SourceSurfaceReturn> source_returns;
    std::vector<double> mesh_return(mesh_->n_cells(), 0);
    std::vector<std::vector<double>> mesh_return_mass(mesh_->n_cells(),
                                                      std::vector<double>(ntransport, 0));
    const auto credit = [&](const SurfaceReturnReceipt& r, int source, int type, int unit, int pool,
                            double fraction) {
        if (fraction <= 0)
            return;
        if (source < 0) {
            const int cell = r.intake.request.cell;
            mesh_return[cell] += r.volume * fraction;
            for (int p = 0; p < ntransport; ++p)
                mesh_return_mass[cell][p] += r.mass[p] * fraction;
        } else {
            runoff::SourceSurfaceReturn q;
            q.source = source;
            q.type = type;
            q.unit = unit;
            q.pool = pool;
            q.volume = r.volume * fraction / ft3;
            q.mass.resize(np);
            for (int p = 0; p < np; ++p)
                q.mass[p] = r.mass[p] * fraction;
            source_returns.push_back(std::move(q));
        }
    };
    for (const auto& r : gw_->sourceReturns())
        if (r.available_at <= start) {
            const int cell = r.intake.request.cell;
            if (!ownsCell(cell) || !std::isfinite(r.volume) || r.volume < 0 ||
                int(r.mass.size()) != ntransport || !validTransportMass(r.mass, temp_row))
                return "Invalid paired common source return.";
            for (double mass : r.mass)
                if (r.volume == 0 && mass != 0)
                    return "Invalid common source return mass.";
            if (r.kind == SurfaceReturnKind::NUMERICAL_REFUND) {
                const auto& original = r.intake.request;
                if (original.kind == SurfaceDonorKind::MESH)
                    credit(r, -1, -1, -1, 2, 1);
                else {
                    auto donor = std::find_if(donors_.begin(), donors_.end(), [&](const auto& d) {
                        return sources_->context().subcatch_names.name_of(d.source) ==
                                   original.source &&
                               d.identity == original.unit &&
                               (d.type < 0) == (original.kind == SurfaceDonorKind::NON_LID);
                    });
                    if (donor == donors_.end())
                        return "Source refund has no originating donor.";
                    credit(r, donor->source, donor->type, donor->unit, 2, 1);
                }
            } else {
                struct Owner {
                    int source, type, unit, pool;
                    double area;
                };
                std::vector<Owner> owners;
                if (weather_[cell] > 0)
                    owners.push_back({-1, -1, -1, 2, weather_[cell]});
                const auto& atmosphere = sources_->atmosphere();
                const auto& runoff = sources_->runoff().soa();
                for (std::size_t j = 0; j < atmosphere.size(); ++j)
                    for (const auto& [i, area] : et_contacts_[j])
                        if (i == cell) {
                            const auto& b = atmosphere[j];
                            if (b.kind == runoff::SourceEtKind::IMPERVIOUS) {
                                const double impervious = runoff.imperv_pct[b.source];
                                for (int pool = 0; pool < 2; ++pool) {
                                    const double fraction = pool == 0
                                                                ? runoff.frac_imperv0[b.source]
                                                                : runoff.frac_imperv1[b.source];
                                    if (fraction > 0)
                                        owners.push_back(
                                            {b.source, -1, -1, pool, area * fraction / impervious});
                                }
                            } else
                                owners.push_back({b.source, b.type, b.unit, 2, area});
                        }
                double total = 0;
                for (const auto& o : owners)
                    total += o.area;
                if (!closeArea(total, mesh_->tri_area[cell]))
                    return "Surface emergence has incomplete local ownership.";
                double remaining = 1;
                for (std::size_t j = 0; j < owners.size(); ++j) {
                    const auto& o = owners[j];
                    const double fraction =
                        j + 1 == owners.size() ? remaining : std::min(remaining, o.area / total);
                    remaining -= fraction;
                    credit(r, o.source, o.type, o.unit, o.pool, fraction);
                }
            }
            ready_returns.push_back(r);
        }
    struct MeshTrial {
        int cell;
        double rain, evap, out, incoming, available, requested_infil, et_rate;
        std::vector<double> mass, rain_mass;
    };
    std::vector<MeshTrial> mesh_trials;
    std::vector<int> mesh_index(mesh_->n_cells(), -1);
    std::vector<SurfaceIntakeRequest> requests;
    std::map<Key, int> donor_index;
    // Mesh requests are water-limited after its competing evaporation/outflow.
    for (int i : cells_) {
        if (!std::isfinite(surface_->volume[i]) || surface_->volume[i] < 0 ||
            !std::isfinite(surface_->rainfall[i]) || !std::isfinite(surface_->evap_rate[i]) ||
            !std::isfinite(surface_->coupling_flux[i]) || !std::isfinite(surface_->depth[i]))
            return "Common source mesh water/forcing is invalid.";
        if (gw_->state().xacc_to_surface[i] != 0)
            return "Unpaired generic aquifer return in common source cell.";
        if (ntransport > 0) {
            if (surface_->coupling_flux[i] != 0)
                return "Common LID pollutant pairing has no qualified mesh coupling donor.";
            for (int j = 0; j < ntransport; ++j)
                if (gw_->transport().xacc_to_surface[gw_->transport().idx(j, i)] != 0)
                    return "Common LID pollutant pairing needs qualified paired source returns.";
        }
        const double A = mesh_->tri_area[i], w = weather_[i];
        const double rain =
            std::max(0.0, snow.owns(i)
                              ? snow.liquidRates()[i]
                              : surface_->runtime_forcings.rate(1, i, surface_->rainfall[i])) *
            w * dt;
        const double coupling = surface_->coupling_flux[i] * A * dt;
        const double incoming = rain + std::max(coupling, 0.0) + mesh_return[i];
        const double available = std::max(0.0, surface_->volume[i] + incoming);
        const double et_rate = surface_->runtime_forcings.rate(2, i, surface_->evap_rate[i]) *
                               (snow.owns(i) ? snow.exposedFractions()[i] : 1.);
        const double evap = evapSink(et_rate, surface_->depth[i], options_->dry_depth) * w * dt;
        const double out = std::max(-coupling, 0.0);
        const auto* prescribed = surface_->runtime_forcings.get(3, i);
        const double cap =
            (prescribed
                 ? std::max(0.0,
                            infilSink(prescribed->rate, surface_->depth[i], options_->dry_depth))
                 : gw_->sourceInfiltrationCapacity(i, std::max(0.0, surface_->depth[i]), dt)) *
            w * dt;
        const double sum = cap + evap + out,
                     scale = sum > available && sum > 0 ? available / sum : 1;
        MeshTrial trial{i,
                        rain,
                        evap * scale,
                        out * scale,
                        incoming,
                        available,
                        cap,
                        et_rate,
                        std::vector<double>(ntransport, 0),
                        std::vector<double>(ntransport, 0)};
        const auto& tr = surface_->transport;
        for (int p = 0; p < ntransport; ++p) {
            const double opening = tr.cell_mass[tr.idx(p, i)];
            trial.rain_mass[p] = rain * (tr.rain_conc.empty() ? 0 : tr.rain_conc[p]);
            trial.mass[p] = opening + trial.rain_mass[p] + mesh_return_mass[i][p];
            if (!std::isfinite(opening) || (p != temp_row && opening < 0) ||
                !std::isfinite(trial.mass[p]))
                return "Common source mesh transport mixture is invalid.";
        }
        mesh_index[i] = int(mesh_trials.size());
        mesh_trials.push_back(std::move(trial));
        if (w > 0)
            requests.push_back({SurfaceDonorKind::MESH, "cell:" + std::to_string(i), -1, i, w,
                                std::max(0.0, surface_->depth[i]), cap * scale, cap * scale});
    }
    auto credited = sources_->waterTrial();
    const auto return_error = credited.creditSurfaceReturns(start, source_returns);
    if (!return_error.empty())
        return return_error;
    auto proposal = credited.waterTrial();
    // The reviewed uniform source still has one physical store. Only inside
    // shares inherit cell prescriptions; outside shares keep resolved native weather.
    std::vector<std::map<int, double>> pet_span(
        runtime_catchment_forcing_ ? sources_->context().n_subcatches() : 0);
    std::vector<double> outside_pet_span(pet_span.size(), 0);
    const auto transform = [&](runoff::SourceForcingInterval& f, bool collect) {
        for (auto& w : f.sources) {
            const int sc = w.subcatch;
            const double outside = 1 - sources_->spatialFractions()[sc];
            const double full = (sources_->runoff().soa().area[sc] +
                                 sources_->context().subcatches.total_lid_area_ft2[sc]) *
                                ft2;
            double rain = w.rain * .3048 * full * outside, pet = w.pet * .3048 * full * outside;
            bool rain_changed = false, pet_changed = false;
            if (collect)
                outside_pet_span[sc] += w.pet * .3048 * (f.end - f.start);
            for (const auto& [cell, area] : forcing_contacts_[sc]) {
                const double r = surface_->runtime_forcings.rate(1, cell, w.rain * .3048);
                const double p = surface_->runtime_forcings.rate(2, cell, w.pet * .3048);
                rain_changed |= surface_->runtime_forcings.get(1, cell) != nullptr;
                pet_changed |= surface_->runtime_forcings.get(2, cell) != nullptr;
                rain += area * r;
                pet += area * p;
                if (collect)
                    pet_span[sc][cell] += p * (f.end - f.start);
            }
            if (rain_changed)
                w.rain = rain / full / .3048;
            if (pet_changed)
                w.pet = pet / full / .3048;
        }
    };
    runoff::SourceWaterDriver::ForcingTransform proposal_weather = [&](auto& f) {
        transform(f, false);
    };
    runoff::SourceWaterDriver::ForcingTransform bounded_weather = [&](auto& f) {
        transform(f, true);
    };
    const auto findDonor = [&](int sc, int t = -1, int u = -1) -> int {
        for (std::size_t j = 0; j < donors_.size(); ++j)
            if (donors_[j].source == sc && donors_[j].type == t && donors_[j].unit == u)
                return int(j);
        return -1;
    };
    std::vector<double> pond(donors_.size(), 1e30);
    const auto intakeRate = [&](int cell, double p, double span) {
        if (runtime_catchment_forcing_)
            if (const auto* row = surface_->runtime_forcings.get(3, cell))
                return std::max(0.0, infilSink(row->rate, p, options_->dry_depth));
        return gw_->sourceInfiltrationCapacity(cell, p, span);
    };
    const auto capacity = [&](int j, double p) {
        pond[j] = std::min(pond[j], p);
        double total = 0;
        for (const auto& [cell, area] : donors_[j].contacts)
            total += intakeRate(cell, p, dt) * area;
        return total / donors_[j].area / .3048;
    };
    runoff::RunoffSolver::InfiltrationBoundary candidate = [&](int sc, double p, double,
                                                               double& rate) {
        const int j = findDonor(sc);
        if (j < 0)
            return false;
        rate = capacity(j, p * .3048);
        return true;
    };
    runoff::SourceWaterDriver::BottomCeiling bottom = [&](int t, int u, double, double) {
        const auto& g = proposal.lids(true).group(t);
        const int j = findDonor(g.subcatch_idx[u], t, u);
        return j < 0 ? 1e10
                     : capacity(j,
                                std::max(0.0, g.type == lid::LIDType::VEG_SWALE ? g.surf_depth[u]
                                                                                : g.stor_depth[u]) *
                                    .3048);
    };
    for (std::size_t group = 0; group < proposal.clocks().groups().size(); ++group) {
        auto error = proposal.stage(int(group), end, end, &candidate, &bottom,
                                    runtime_catchment_forcing_ ? &proposal_weather : nullptr);
        if (!error.empty())
            return error;
        error = proposal.commit();
        if (!error.empty())
            return error;
    }
    const auto donorVolume = [&](const runoff::SourceWaterDriver& s, int j) {
        const auto& d = donors_[j];
        if (d.type >= 0) {
            const auto& a = s.lids().group(d.type);
            const auto& b = sources_->lids().group(d.type);
            return (a.wb_spatial_infil[d.unit] - b.wb_spatial_infil[d.unit]) * a.area[d.unit] * ft3;
        }
        return (s.ledgers()[d.source].spatial_infiltration -
                sources_->ledgers()[d.source].spatial_infiltration) *
                   ft3 -
               (bottomTotal(s, d.source) - bottomTotal(*sources_, d.source));
    };
    const auto donorMass = [&](const runoff::SourceWaterDriver& s, int j, int pollutant) {
        const auto& d = donors_[j];
        double mass = 0;
        for (const auto& b : s.pollutants())
            if (b.source == d.source && b.type == d.type && b.unit == d.unit)
                mass += b.spatial_infiltration[pollutant];
        for (const auto& b : sources_->pollutants())
            if (b.source == d.source && b.type == d.type && b.unit == d.unit)
                mass -= b.spatial_infiltration[pollutant];
        return mass;
    };
    for (std::size_t j = 0; j < donors_.size(); ++j) {
        const auto& d = donors_[j];
        const double volume = std::max(0.0, donorVolume(proposal, int(j)));
        const double p = pond[j] == 1e30 ? 0 : pond[j];
        double total = 0;
        for (const auto& [cell, area] : d.contacts)
            total += intakeRate(cell, p, dt) * area;
        const auto kind = d.type < 0 ? SurfaceDonorKind::NON_LID : SurfaceDonorKind::LID_BOTTOM;
        const auto name = sources_->context().subcatch_names.name_of(d.source);
        donor_index[{kind, name, d.identity}] = int(j);
        for (const auto& [cell, area] : d.contacts) {
            const double weight = total > 0 ? intakeRate(cell, p, dt) * area / total : 0;
            requests.push_back({kind, name, d.identity, cell, area, p, volume, volume * weight});
        }
    }
    SurfaceExchange::Capacity capacity_override = [&](const SurfaceIntakeRequest& r, double span) {
        if (r.kind == SurfaceDonorKind::MESH)
            if (const auto* row = surface_->runtime_forcings.get(3, r.cell))
                return std::max(0.0, infilSink(row->rate, surface_->depth[r.cell],
                                               options_->dry_depth)) *
                       r.area * span;
        return intakeRate(r.cell, r.pond, span) * r.area * span;
    };
    const auto* prescribed_capacity = runtimeWaterEnabled() ? &capacity_override : nullptr;
    auto error = exchange_.plan(*gw_, start, end, end, requests, prescribed_capacity);
    if (!error.empty())
        return error;
    std::vector<double> awarded(donors_.size(), 0);
    std::map<int, double> mesh_awarded;
    for (const auto& a : exchange_.awards()) {
        if (a.request.kind == SurfaceDonorKind::MESH)
            mesh_awarded[a.request.cell] += a.maximum;
        else
            awarded[donor_index.at(key(a.request))] += a.maximum;
    }
    // The credited snapshot has no remaining readers; transfer its owned state.
    auto bounded = std::move(credited);
    const auto rate = [&](int j) {
        return std::nextafter(awarded[j] / donors_[j].area / .3048 / dt, 0.0);
    };
    runoff::RunoffSolver::InfiltrationBoundary limit = [&](int sc, double, double, double& q) {
        const int j = findDonor(sc);
        if (j < 0)
            return false;
        q = rate(j);
        return true;
    };
    runoff::SourceWaterDriver::BottomCeiling bottom_limit = [&](int t, int u, double, double) {
        const int j = findDonor(bounded.lids(true).group(t).subcatch_idx[u], t, u);
        return j < 0 ? 1e10 : rate(j);
    };
    for (std::size_t group = 0; group < bounded.clocks().groups().size(); ++group) {
        error = bounded.stage(int(group), end, end, &limit, &bottom_limit,
                              runtime_catchment_forcing_ ? &bounded_weather : nullptr);
        if (error.empty())
            error = bounded.commit();
        if (!error.empty()) {
            exchange_.cancel();
            return error;
        }
    }
    std::vector<SurfaceIntakeActual> actual;
    std::vector<double> consumed(donors_.size(), 0), processed(donors_.size(), 0);
    std::vector<std::vector<double>> consumed_mass(donors_.size(), std::vector<double>(np, 0));
    std::vector<std::vector<double>> mesh_mass(mesh_trials.size(),
                                               std::vector<double>(ntransport, 0));
    for (const auto& a : exchange_.awards()) {
        double volume = a.maximum;
        std::vector<double> mass(ntransport, 0);
        if (a.request.kind == SurfaceDonorKind::MESH) {
            const int j = mesh_index[a.request.cell];
            const auto& m = mesh_trials[j];
            // Match ordinary mesh source transport: advective sinks sample the
            // post-arrival mixture before evaporation; evaporation retains solute.
            for (int p = 0; p < ntransport; ++p) {
                mass[p] = m.available > 0 ? m.mass[p] * (volume / m.available) : 0;
                mesh_mass[j][p] += mass[p];
            }
        } else {
            const int j = donor_index.at(key(a.request));
            const double taken = std::max(0.0, donorVolume(bounded, j));
            std::vector<double> mass(np, 0);
            if (taken > awarded[j] + 1e-12) {
                exchange_.cancel();
                return "Bounded source exceeded its shared award.";
            }
            // Spread actual withdrawal over its original awards. Denied cell
            // capacity never migrates; only FP residue lands on the last share.
            processed[j] += a.maximum;
            volume = awarded[j] > 0
                         ? std::min({a.maximum, std::max(0.0, taken - consumed[j]),
                                     processed[j] >= awarded[j] ? std::max(0.0, taken - consumed[j])
                                                                : taken * a.maximum / awarded[j]})
                         : 0;
            consumed[j] += volume;
            for (int p = 0; p < np; ++p) {
                const double transferred = donorMass(bounded, j, p);
                mass[p] = taken > 0 ? transferred * (volume / taken) : 0;
                consumed_mass[j][p] += mass[p];
            }
            actual.push_back({volume, std::move(mass)});
            continue;
        }
        actual.push_back({volume, std::move(mass)});
    }
    for (std::size_t j = 0; j < donors_.size(); ++j)
        for (int p = 0; p < np; ++p) {
            const double mass = donorMass(bounded, int(j), p);
            if (!std::isfinite(mass) || mass < 0 ||
                std::abs(consumed_mass[j][p] - mass) > 1e-12 * std::max(1.0, mass)) {
                exchange_.cancel();
                return "Common source pollutant receipts do not match the actual donor-pool withdrawal.";
            }
        }
    for (std::size_t j = 0; j < donors_.size(); ++j)
        if (std::abs(consumed[j] - donorVolume(bounded, int(j))) > 1e-12) {
            exchange_.cancel();
            return "Common source actual receipts do not match donor withdrawal.";
        }
    std::vector<SurfaceEtReceipt> et_receipts;
    struct EtBudget {
        double potential = 0, evaporation = 0, soil = 0;
    };
    std::vector<EtBudget> et_budget(mesh_->n_cells());
    const auto addEt = [&](SurfaceEtReceipt r, bool eligible) {
        const double remainder = std::max(0.0, r.potential - r.evaporation);
        r.soil_demand = eligible ? remainder : 0;
        r.unused = eligible ? 0 : remainder;
        if (r.cell >= 0) {
            auto& b = et_budget[r.cell];
            b.potential += r.potential;
            b.evaporation += r.evaporation;
            b.soil += r.soil_demand;
        }
        et_receipts.push_back(r);
    };
    for (const auto& m : mesh_trials)
        addEt({SurfaceEtOwner::MESH, -1, -1, -1, m.cell, start, end, weather_[m.cell],
               std::max(0.0, m.et_rate) * weather_[m.cell] * dt, m.evap},
              true);
    for (std::size_t j = 0; j < bounded.atmosphere().size(); ++j) {
        const auto& a = bounded.atmosphere()[j];
        const auto& b = sources_->atmosphere()[j];
        const auto owner = a.kind == runoff::SourceEtKind::PERVIOUS     ? SurfaceEtOwner::PERVIOUS
                           : a.kind == runoff::SourceEtKind::IMPERVIOUS ? SurfaceEtOwner::IMPERVIOUS
                                                                        : SurfaceEtOwner::LID;
        for (const auto& [cell, area] : et_contacts_[j]) {
            const double area_fraction = area / (a.area * ft2);
            const double potential = runtime_catchment_forcing_
                                         ? pet_span[a.source][cell] * area
                                         : (a.potential - b.potential) * ft3 * area_fraction;
            const double fraction =
                runtime_catchment_forcing_
                    ? (a.potential > b.potential ? potential / ((a.potential - b.potential) * ft3)
                                                 : 0)
                    : area_fraction;
            addEt({owner, a.source, a.type, a.unit, cell, start, end, area, potential,
                   (a.evaporation - b.evaporation) * ft3 * fraction},
                  a.soil_eligible);
        }
        const double outside_area = a.area * ft2 * (1 - bounded.spatialFractions()[a.source]);
        if (outside_area > 0) {
            const double area_fraction = outside_area / (a.area * ft2);
            const double potential = runtime_catchment_forcing_
                                         ? outside_pet_span[a.source] * outside_area
                                         : (a.potential - b.potential) * ft3 * area_fraction;
            const double fraction =
                runtime_catchment_forcing_
                    ? (a.potential > b.potential ? potential / ((a.potential - b.potential) * ft3)
                                                 : 0)
                    : area_fraction;
            addEt({owner, a.source, a.type, a.unit, -1, start, end, outside_area, potential,
                   (a.evaporation - b.evaporation) * ft3 * fraction},
                  false);
        }
    }
    // Validate all atmospheric volumes before either receiver or owner changes.
    for (int i : cells_) {
        const auto& b = et_budget[i];
        if (!std::isfinite(b.potential) || !std::isfinite(b.evaporation) ||
            !std::isfinite(b.soil) || b.potential < 0 || b.evaporation < 0 || b.soil < 0 ||
            b.evaporation + b.soil > b.potential + 1e-12 * std::max(1.0, b.potential)) {
            exchange_.cancel();
            return "Common source atmospheric area budget does not close.";
        }
    }
    // Prepare provider receipts before receiver booking, including zero-intake
    // cells, so this additional accounting cannot allocate after the transfer.
    std::vector<std::pair<double, double>> provider_intake(
        runtime_catchment_forcing_ ? mesh_->n_cells() : 0);
    for (std::size_t j = 0; j < exchange_.awards().size(); ++j) {
        const auto& q = exchange_.awards()[j].request;
        if (runtime_catchment_forcing_ && q.kind != SurfaceDonorKind::MESH &&
            surface_->runtime_forcings.get(3, q.cell)) {
            auto& r = provider_intake[q.cell];
            r.first += q.candidate;
            r.second += actual[j].volume;
        }
    }
    // Revalidation and receiver booking happen before any managed source debit.
    error = exchange_.commit(*gw_, actual, prescribed_capacity);
    if (!error.empty()) {
        exchange_.cancel();
        return error;
    }
    for (const auto& m : mesh_trials) {
        const double infil = mesh_awarded[m.cell], A = mesh_->tri_area[m.cell];
        if (auto* row = const_cast<RuntimeForcing*>(surface_->runtime_forcings.get(3, m.cell))) {
            const auto extra =
                runtime_catchment_forcing_ ? provider_intake[m.cell] : std::pair<double, double>{};
            const double total = infil + extra.second;
            auto& r = row->receipt;
            r.requested += m.requested_infil + extra.first;
            r.applied += total;
            r.water_out += total;
            r.last_flow = total / dt;
            r.last_dt = dt;
        }
        surface_->volume[m.cell] =
            std::max(0.0, surface_->volume[m.cell] + m.incoming - m.evap - m.out - infil);
        surface_->infil_applied[m.cell] += infil / A;
        surface_->coupling_applied[m.cell] += m.incoming - m.rain - m.out;
        mesh_return_ += mesh_return[m.cell];
        surface_->evap_loss_total += m.evap;
        const auto& b = et_budget[m.cell];
        gw_->bookAreaEt(m.cell, dt, b.potential, b.evaporation, b.soil);
        inertial::cellEtaDepth(*mesh_, *options_, m.cell, surface_->volume[m.cell],
                               surface_->head[m.cell], surface_->depth[m.cell]);
        mesh_rain_ += m.rain;
        mesh_evap_ += m.evap;
        auto& tr = surface_->transport;
        for (int p = 0; p < ntransport; ++p) {
            const double taken = mesh_mass[mesh_index[m.cell]][p];
            tr.cell_mass[tr.idx(p, m.cell)] = m.mass[p] - taken;
            tr.gained_rainfall[p] += m.rain_mass[p];
            tr.lost_infiltration[p] += taken;
            tr.gained_exfiltration[p] += mesh_return_mass[m.cell][p];
        }
    }
    surface_->snow = std::move(snow);
    *sources_ = std::move(bounded);
    et_receipts_ = std::move(et_receipts);
    returns_ = std::move(ready_returns);
    gw_->acknowledgeSourceReturns(start);
    gw_->source_completed_end_ = end;
    ++intervals_;
    return {};
}
}
