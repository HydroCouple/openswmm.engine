"""Where each formulation is documented — the click targets of the
conceptual map, the process-flow chart and the object sketch.

`FEATURE_REFS` maps a feature id of status.py to the Doxygen page (or
anchor) that documents it; `TITLE_REFS` maps a process-box title or a
neutral pill's text to a page. The generators register one hotspot per
pill and per title through `sink.hotspot()`, and the build writes them
into the citing pages as `@ref` links, so Doxygen and the lint validate
every target. A feature that has no entry here is drawn but not linked.

Targets are page ids (`@page …`) or explicit anchors (`{#…}`); the
`engine_manual_sect_<SECTION>` anchors are the [SECTION] grammar entries
of the Engine Manual's Chapter 2.
"""
from __future__ import annotations

H = "hydraulics_ref_"
Y = "hydrology_ref_"
Q = "quality_ref_"
S = "engine_manual_sect_"

# feature id -> page id or anchor
FEATURE_REFS = {
    "runoff.subcatchment": Y + "ch3_surface_runoff",
    "runoff.mesh_2d": H + "ch9_two_dimensional",
    "infiltration.horton": Y + "ch4_infiltration",
    "infiltration.mod_horton": Y + "ch4_infiltration",
    "infiltration.green_ampt": Y + "ch4_infiltration",
    "infiltration.mod_green_ampt": Y + "ch4_infiltration",
    "infiltration.curve_number": Y + "ch4_infiltration",
    "infiltration.mesh_per_cell": H + "ch9_two_dimensional",     # -> hydrology_ref_ch8_mesh_surface when it lands
    "infiltration.mesh_constant": H + "ch9_two_dimensional",
    "snowmelt.degree_day": Y + "ch6_snowmelt",
    "rain.mesh_natural_neighbour": H + "ch9_two_dimensional",   # -> hydrology_ref_ch8_mesh_surface
    "evaporation.mesh": H + "ch9_two_dimensional",
    "lid.layered": Q + "ch6_lid_controls",
    "lid.storage_node": Q + "ch6_lid_controls",                 # -> hydrology_ref_ch10_planned
    "lid.detailed_output": Q + "ch6_lid_controls",
    "quality.buildup_washoff": Q + "ch3_pollutant_buildup",
    "quality.mesh_surface": Q + "ch4_surface_washoff",          # -> quality_ref_ch10_mesh_quality
    "climate.humidity_dewpoint": Y + "ch2_meteorology",
    "climate.precip_scaling": Y + "ch2_meteorology",
    "climate.pet_forcing": Y + "ch2_meteorology",
    "rdii.rtk": Y + "ch7_rdii",
    "rdii.ia_decay": Y + "ch7_rdii",
    "groundwater.two_zone": Y + "ch5_groundwater",
    "groundwater.mesh_two_layer": Y + "ch5_groundwater",        # -> hydrology_ref_ch9_mesh_groundwater
    "groundwater.mesh_per_subcatch": Y + "ch5_groundwater",
    "groundwater.soil.gardner": Y + "ch5_groundwater",
    "groundwater.soil.russo": Y + "ch5_groundwater",
    "groundwater.soil.brooks_corey": Y + "ch5_groundwater",
    "groundwater.soil.van_genuchten": Y + "ch5_groundwater",
    "groundwater.transport": Y + "ch5_groundwater",             # -> hydrology_ref_ch10_planned
    "routing.steady": H + "ch2_hydraulic_model",
    "routing.kinwave": H + "ch4_kinematic_wave",
    "routing.dynwave": H + "ch3_dynamic_wave",
    "routing.fv": H + "ch8_finite_volume",
    "routing.node_continuity.explicit": H + "ch3_dynamic_wave",
    "routing.node_continuity.semi_implicit": H + "ch3_dynamic_wave",
    "routing.anderson": H + "ch3_dynamic_wave",
    "pressurisation.extran": H + "ch3_dynamic_wave",
    "pressurisation.slot": H + "ch3_dynamic_wave",
    "pressurisation.dynamic_slot": H + "ch3_dynamic_wave",
    "pressurisation.tpa": H + "ch3_dynamic_wave",
    "fv.closure.slot": H + "ch8_finite_volume",
    "fv.closure.tpa": H + "ch8_finite_volume",
    "fv.pressurized_implicit": H + "ch8_finite_volume",
    "fv.lts": H + "ch8_finite_volume",
    "fv.node_coupling_keys": H + "ch8_finite_volume",
    "fv.implicit_hypre": H + "ch8_finite_volume",               # -> hydraulics_ref_ch10_planned
    "fv.bed_step_junction_faces": H + "ch8_finite_volume",
    "fv.tpa_high_celerity_fix": H + "ch8_finite_volume",
    "friction.unsteady_vitkovsky": H + "ch3_dynamic_wave",
    "backend.1d.cpu": H + "ch8_finite_volume",
    "backend.1d.gpu": H + "ch8_finite_volume",                  # -> hydraulics_ref_ch10_planned
    "junctions.virtual": H + "ch3_dynamic_wave",
    "junctions.virtual_momentum_full": H + "ch3_dynamic_wave",
    "junctions.inlet": H + "ch7_advanced_features",
    "inlets.link_attribute": H + "ch7_advanced_features",
    "xsect.tabulated": H + "ch5_cross_section",
    "xsect.street": H + "ch5_cross_section",
    "xsect.dummy": H + "ch5_cross_section",
    "xsect.chebyshev_irregular": H + "ch5_cross_section",       # -> hydraulics_ref_ch10_planned
    "storage.shapes": H + "ch5_cross_section",
    "overland2d.local_inertial": H + "ch9_two_dimensional",
    "overland2d.full_swe": H + "ch9_two_dimensional",
    "overland2d.diffusive_wave": H + "ch9_two_dimensional",
    "overland2d.integrator.explicit": H + "ch9_two_dimensional",
    "overland2d.integrator.cvode": H + "ch9_two_dimensional",
    "overland2d.imex": H + "ch9_two_dimensional",
    "overland2d.advection_key": H + "ch9_two_dimensional",
    "overland2d.cell_closure.flat": H + "ch9_two_dimensional",
    "overland2d.cell_closure.vfr": H + "ch9_two_dimensional",
    "overland2d.mesh.triangles": H + "ch9_two_dimensional",
    "overland2d.mesh.quads": H + "ch9_two_dimensional",
    "coupling.1d2d.vertex": H + "ch9_two_dimensional",
    "coupling.1d2d.cell": H + "ch9_two_dimensional",
    "backend.2d.cpu": H + "ch9_two_dimensional",
    "backend.2d.omp": H + "ch9_two_dimensional",
    "backend.2d.cuda": H + "ch9_two_dimensional",
    "backend.2d.hip": H + "ch9_two_dimensional",
    "backend.2d.sycl": H + "ch9_two_dimensional",
    "backend.2d.metal": H + "ch9_two_dimensional",              # -> hydraulics_ref_ch10_planned
    "transport.cstr": Q + "ch5_transport_treatment",
    "transport.ard": Q + "ch7_ard_transport",
    "transport.lard": Q + "ch7_ard_transport",
    "transport.rwpt": Q + "ch7_ard_transport",
    "transport.mesh_2d": Q + "ch7_ard_transport",               # -> quality_ref_ch10_mesh_quality
    "transport.fv_mesh_pollutants": Q + "ch7_ard_transport",    # -> quality_ref_ch11_planned
    "reactions.first_order": Q + "ch5_transport_treatment",
    "reactions.msx": Q + "ch8_msx_reactions",
    "reactions.bw_msx": Q + "ch8_msx_reactions",
    "water_age": Q + "ch9_age_heat",
    "heat": Q + "ch9_age_heat",
    "heat.per_element_attributes": Q + "ch9_age_heat",
    "sediment": Q + "ch5_transport_treatment",                  # -> quality_ref_ch11_planned
    "outfall.backflow_quality": Q + "ch7_ard_transport",
}

# process-box title or neutral-pill text -> page id or anchor
TITLE_REFS = {
    # conceptual map processes
    "Precipitation": Y + "ch2_meteorology",
    "Climate": Y + "ch2_meteorology",
    "Snowmelt": Y + "ch6_snowmelt",
    "Runoff": Y + "ch3_surface_runoff",
    "Infiltration": Y + "ch4_infiltration",
    "LID controls": Q + "ch6_lid_controls",
    "Surface quality": Q + "ch3_pollutant_buildup",
    "Rain on the mesh": H + "ch9_two_dimensional",
    "Per-cell infiltration": H + "ch9_two_dimensional",
    "Overland flow": H + "ch9_two_dimensional",
    "Mesh and closure": H + "ch9_two_dimensional",
    "Surface quality · transport": Q + "ch4_surface_washoff",
    "Backends": H + "ch9_two_dimensional",
    "Two-zone aquifer": Y + "ch5_groundwater",
    "Groundwater transport": Y + "ch5_groundwater",
    "Two-layer aquifer": Y + "ch5_groundwater",
    "Nodes": S + "JUNCTIONS",
    "Links": S + "CONDUITS",
    "Flow routing": H + "ch2_hydraulic_model",
    "Pressurisation": H + "ch3_dynamic_wave",
    "Node solution": H + "ch3_dynamic_wave",
    "Transport": Q + "ch5_transport_treatment",
    "Inflows": S + "INFLOWS",
    # process-flow boxes
    "Initial abstraction · evaporation": Y + "ch3_surface_runoff",
    "Surface runoff": Y + "ch3_surface_runoff",
    "Washoff": Q + "ch4_surface_washoff",
    "Groundwater": Y + "ch5_groundwater",
    "Pollutant buildup": Q + "ch3_pollutant_buildup",
    "2D overland flow": H + "ch9_two_dimensional",
    "Per-cell infiltration · evaporation": H + "ch9_two_dimensional",
    "2D surface quality": Q + "ch4_surface_washoff",
    "External inflows": S + "INFLOWS",
    "Street inlets": H + "ch7_advanced_features",
    "Channel, pipe and storage routing": H + "ch2_hydraulic_model",
    "Transport and reactions": Q + "ch5_transport_treatment",
    "OUTFALLS · receiving water": S + "OUTFALLS",
    # neutral pills (values, not alternatives)
    "rain gages": S + "RAINGAGES",
    "rain · snow": S + "RAINGAGES",
    "temperature": S + "TEMPERATURE",
    "wind": S + "TEMPERATURE",
    "evaporation: constant · monthly · series · Hargreaves · file": S + "EVAPORATION",
    "depression storage": S + "SUBAREAS",
    "routing to another subcatchment": S + "SUBAREAS",
    "street sweeping": S + "LANDUSES",
    "system": S + "2D_OPTIONS",
    "→ lost · subcatchment aquifer · mesh aquifer": S + "2D_INFILTRATION_OPTIONS",
    "→ lost · subcatchment · mesh aquifer": S + "2D_INFILTRATION_OPTIONS",
    "percolation · deep percolation · ET": S + "AQUIFERS",
    "lateral interflow, user [GWF]": S + "GWF",
    "Dunne return flow": S + "2D_AQUIFER_OPTIONS",
    "node–bed exchange": S + "2D_AQUIFER_NODE",
    "capillary rise · boundary ET": S + "2D_AQUIFER_OPTIONS",
    "junction · storage · divider · outfall": S + "JUNCTIONS",
    "pumps · orifices · weirs · outlets · culverts": S + "PUMPS",
    "treatment functions": S + "TREATMENT",
    "treatment · diversion": S + "TREATMENT",
    "dry weather flow": S + "DWF",
    "user · interface files": S + "INFLOWS",
    "control rules": S + "CONTROLS",
}


def ref_for(feature_id, text, title=None):
    """The @ref target for a pill: by feature id, else by its text, else by its box title."""
    if feature_id and feature_id in FEATURE_REFS:
        return FEATURE_REFS[feature_id]
    if text in TITLE_REFS:
        return TITLE_REFS[text]
    if title in TITLE_REFS:
        return TITLE_REFS[title]
    return None
