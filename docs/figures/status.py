"""Single source of truth for the status of every formulation the manuals name.

Consumed by three things that must never disagree:

* the figure generators under docs/figures/src/ — every alternative drawn on
  the conceptual map or a process diagram carries a chip coloured from STATUS;
* scripts/lint_manual_docs.py check_status_badges() — the only words a
  \\status{...} alias may carry are the labels below, and the CSS that styles
  them must carry the same hex values;
* the "Planned formulations" chapters and the ROADMAP refresh, which are
  written against FEATURES rather than from memory.

Vocabulary (defined once in the Engine Manual, Chapter 1):

  Implemented   shipped, selectable, exercised by gates or parity suites
  Experimental  shipped and selectable, default-off, with a validation caveat
                recorded in source or in plans/
  Planned       design recorded in plans/ or ROADMAP.md, no kernel — includes
                sections that parse but are inert at run time
  Retired       parsed for compatibility, warned or ignored, no effect

Stdlib only: the lint imports this file from a checkout that has no matplotlib.
"""

STATUS = {
    # key            (label,          hex)   — hex values are the benchmarks
    #                                          translib categorical palette
    "implemented":  ("Implemented",  "#1baf7a"),
    "experimental": ("Experimental", "#eda100"),
    "planned":      ("Planned",      "#898781"),
    "retired":      ("Retired",      "#e87ba4"),
}

# feature id -> status key. Ids are dotted, lower case, grouped by process.
# Sources for each ruling are cited in plans/DOCUMENTATION_FORMULATIONS_PLAN_2026-09-19.md.
FEATURES = {
    # land surface
    "runoff.subcatchment": "implemented",
    "runoff.mesh_2d": "implemented",
    "infiltration.horton": "implemented",
    "infiltration.mod_horton": "implemented",
    "infiltration.green_ampt": "implemented",
    "infiltration.mod_green_ampt": "implemented",
    "infiltration.curve_number": "implemented",
    "infiltration.mesh_per_cell": "implemented",
    "infiltration.mesh_constant": "implemented",
    "snowmelt.degree_day": "implemented",
    "rain.mesh_natural_neighbour": "implemented",
    "evaporation.mesh": "implemented",
    "lid.layered": "implemented",
    "lid.storage_node": "planned",
    "lid.detailed_output": "planned",
    "quality.buildup_washoff": "implemented",
    "quality.mesh_surface": "implemented",
    "climate.humidity_dewpoint": "implemented",
    "climate.precip_scaling": "implemented",
    "climate.pet_forcing": "implemented",
    "rdii.rtk": "implemented",
    "rdii.ia_decay": "implemented",
    # subsurface
    "groundwater.two_zone": "implemented",
    "groundwater.mesh_two_layer": "implemented",
    "groundwater.mesh_per_subcatch": "implemented",
    "groundwater.soil.gardner": "implemented",
    "groundwater.soil.russo": "experimental",
    "groundwater.soil.brooks_corey": "experimental",
    "groundwater.soil.van_genuchten": "experimental",
    "groundwater.transport": "planned",          # [GW_*] sections authored but inert
    # conveyance, 1D
    "routing.steady": "implemented",
    "routing.kinwave": "implemented",
    "routing.dynwave": "implemented",
    "routing.fv": "implemented",
    "routing.node_continuity.explicit": "implemented",
    "routing.node_continuity.semi_implicit": "implemented",
    "routing.anderson": "implemented",
    "pressurisation.extran": "implemented",
    "pressurisation.slot": "implemented",
    "pressurisation.dynamic_slot": "implemented",
    "pressurisation.tpa": "experimental",
    "fv.closure.slot": "implemented",
    "fv.closure.tpa": "experimental",
    "fv.pressurized_implicit": "experimental",
    "fv.lts": "implemented",
    "fv.node_coupling_keys": "retired",           # FV_NODE_COUPLING, FV_NODE_DT, ...
    "fv.implicit_hypre": "planned",
    "friction.unsteady_vitkovsky": "implemented",
    "backend.1d.cpu": "implemented",
    "backend.1d.gpu": "planned",                 # loader hooks only, no plugin
    "junctions.virtual": "implemented",
    "junctions.virtual_momentum_full": "retired",
    "junctions.inlet": "implemented",
    "inlets.link_attribute": "implemented",
    "xsect.tabulated": "implemented",
    "xsect.street": "implemented",
    "xsect.dummy": "implemented",
    "xsect.chebyshev_irregular": "planned",
    "storage.shapes": "implemented",
    "fv.bed_step_junction_faces": "planned",
    "fv.tpa_high_celerity_fix": "planned",
    # conveyance, 2D
    "overland2d.local_inertial": "implemented",
    "overland2d.full_swe": "implemented",
    "overland2d.diffusive_wave": "implemented",
    "overland2d.integrator.explicit": "implemented",
    "overland2d.integrator.cvode": "retired",
    "overland2d.imex": "retired",
    "overland2d.advection_key": "retired",
    "overland2d.cell_closure.flat": "implemented",
    "overland2d.cell_closure.vfr": "implemented",
    "overland2d.mesh.triangles": "implemented",
    "overland2d.mesh.quads": "implemented",
    "coupling.1d2d.vertex": "implemented",
    "coupling.1d2d.cell": "implemented",
    "backend.2d.cpu": "implemented",
    "backend.2d.omp": "implemented",
    "backend.2d.cuda": "implemented",
    "backend.2d.hip": "implemented",
    "backend.2d.sycl": "implemented",
    "backend.2d.metal": "planned",
    # transport and quality
    "transport.cstr": "implemented",
    "transport.ard": "implemented",
    "transport.lard": "implemented",
    "transport.rwpt": "implemented",
    "transport.mesh_2d": "implemented",
    "transport.fv_mesh_pollutants": "planned",   # FV_DISPERSION path not wired to [POLLUTANTS]
    "reactions.first_order": "implemented",
    "reactions.msx": "implemented",
    "reactions.bw_msx": "implemented",
    "water_age": "implemented",
    "heat": "implemented",
    "heat.per_element_attributes": "planned",
    "sediment": "planned",
    "outfall.backflow_quality": "implemented",
}


def label(feature_id):
    return STATUS[FEATURES[feature_id]][0]


def colour(feature_id):
    return STATUS[FEATURES[feature_id]][1]


def labels():
    """The four words a \\status{} badge may carry."""
    return {lab for lab, _ in STATUS.values()}


def self_check():
    """Every feature maps to a known status; every status has a distinct label and hex."""
    errors = []
    for fid, key in FEATURES.items():
        if key not in STATUS:
            errors.append(f"status.py: feature {fid!r} has unknown status {key!r}")
    labs = [lab for lab, _ in STATUS.values()]
    hexes = [h for _, h in STATUS.values()]
    if len(set(labs)) != len(labs) or len(set(hexes)) != len(hexes):
        errors.append("status.py: STATUS labels/colours are not distinct")
    return errors


if __name__ == "__main__":
    import sys
    errs = self_check()
    for e in errs:
        print("ERROR", e)
    print(f"{len(FEATURES)} features, {len(STATUS)} statuses, {len(errs)} errors")
    sys.exit(1 if errs else 0)
