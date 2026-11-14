"""The OpenSWMM process-and-formulation map — the conceptual diagram.

One data model, five outputs. Compartments follow Rossman's four (atmosphere,
land surface, subsurface, conveyance) with the representations the modeller
now chooses between drawn side by side: lumped subcatchments next to the 2D
mesh, the per-subcatchment two-zone aquifer next to the two-layer aquifer on
the mesh. Every process box lists its alternatives as status pills coloured
from docs/figures/status.py, so the diagram cannot disagree with the badges
in the text. Fluxes are labelled arrows.

Variants hide nothing — a compartment outside the variant's scope is drawn
ghosted so the reader sees where the zoom sits.
"""
from __future__ import annotations

REQUIRES = ()

# ── data model ──────────────────────────────────────────────────────────────

# id: (title, x, y_top, w, h, fill-key)  in tenths of an inch on a 100 x 94 canvas
CANVAS = (10.0, 9.4)
COMPARTMENTS = {
    "atm":      ("ATMOSPHERE",                                   2, 89.5, 87, 9.8, "atm"),
    "land_sub": ("LAND SURFACE · subcatchments (lumped)",        2, 76.7, 46, 24, "land"),
    "land_mesh": ("LAND SURFACE · 2D mesh (distributed)",        52, 76.7, 37, 24, "land"),
    "sub_2z":   ("SUBSURFACE · two-zone aquifer per subcatchment", 2, 49.7, 46, 13.5, "sub"),
    "sub_mesh": ("SUBSURFACE · two-layer aquifer on the mesh",   52, 49.7, 37, 13.5, "sub"),
    "conv":     ("CONVEYANCE · 1D node–link network",            2, 33.2, 87, 25, "conv"),
    "out":      ("OUTFALLS · receiving water — free · normal · fixed · tidal · time-series stage",
                 2, 5.2, 87, 3.0, "out"),
}

# (compartment, title, [(pill text, feature id | None), ...])
PROCESSES = [
    ("atm", "Precipitation", [("rain gages", None), ("rain · snow split (SCF)", "snowmelt.degree_day"),
                              ("scale factors", "climate.precip_scaling")]),
    ("atm", "Climate", [("temperature", None), ("evaporation: constant · monthly · series · Hargreaves · file", None),
                        ("wind", None), ("humidity · dew point", "climate.humidity_dewpoint"),
                        ("prescribed PET (API)", "climate.pet_forcing")]),

    ("land_sub", "Snowmelt", [("degree-day · heat budget · areal depletion", "snowmelt.degree_day")]),
    ("land_sub", "Runoff", [("nonlinear reservoir", "runoff.subcatchment"), ("depression storage", None),
                            ("routing to another subcatchment", None)]),
    ("land_sub", "Infiltration", [("Horton", "infiltration.horton"), ("modified Horton", "infiltration.mod_horton"),
                                  ("Green-Ampt", "infiltration.green_ampt"),
                                  ("modified Green-Ampt", "infiltration.mod_green_ampt"),
                                  ("curve number", "infiltration.curve_number")]),
    ("land_sub", "LID controls", [("layered units (8 types)", "lid.layered"),
                                  ("LID as storage nodes", "lid.storage_node"),
                                  ("detailed output", "lid.detailed_output")]),
    ("land_sub", "Surface quality", [("buildup · washoff by land use", "quality.buildup_washoff"),
                                     ("street sweeping", None), ("MSX species", "reactions.bw_msx")]),

    ("land_mesh", "Rain on the mesh", [("natural neighbour", "rain.mesh_natural_neighbour"), ("system", None)]),
    ("land_mesh", "Per-cell infiltration", [("Horton · Green-Ampt · CN · constant", "infiltration.mesh_per_cell"),
                                            ("→ lost · subcatchment aquifer · mesh aquifer", None)]),
    ("land_mesh", "Overland flow", [("local inertial", "overland2d.local_inertial"),
                                    ("full shallow water", "overland2d.full_swe"),
                                    ("diffusive wave", "overland2d.diffusive_wave"),
                                    ("IMEX / CVODE", "overland2d.imex")]),
    ("land_mesh", "Mesh and closure", [("triangles", "overland2d.mesh.triangles"), ("quads", "overland2d.mesh.quads"),
                                       ("flat cells", "overland2d.cell_closure.flat"),
                                       ("VFR closure", "overland2d.cell_closure.vfr")]),
    ("land_mesh", "Surface quality · transport", [("coverages · buildup · washoff", "quality.mesh_surface"),
                                                  ("species on the mesh", "transport.mesh_2d")]),
    ("land_mesh", "Backends", [("CPU", "backend.2d.cpu"), ("OpenMP", "backend.2d.omp"),
                               ("CUDA · HIP · SYCL", "backend.2d.cuda"), ("Metal", "backend.2d.metal")]),

    ("sub_2z", "Two-zone aquifer", [("unsaturated + saturated zone", "groundwater.two_zone"),
                                    ("percolation · deep percolation · ET", None),
                                    ("lateral interflow, user [GWF]", None)]),
    ("sub_2z", "Groundwater transport", [("advection–dispersion · heat", "groundwater.transport")]),

    ("sub_mesh", "Two-layer aquifer", [("Gardner", "groundwater.soil.gardner"), ("Russo", "groundwater.soil.russo"),
                                       ("Brooks–Corey", "groundwater.soil.brooks_corey"),
                                       ("van Genuchten", "groundwater.soil.van_genuchten"),
                                       ("closed form · enslaved · σ column", "groundwater.mesh_two_layer"),
                                       ("Dunne return flow", None), ("node–bed exchange", None),
                                       ("capillary rise · boundary ET", None),
                                       ("per-subcatchment mode", "groundwater.mesh_per_subcatch")]),

    ("conv", "Nodes", [("junction · storage · divider · outfall", None), ("virtual junction", "junctions.virtual"),
                       ("inlet junction", "junctions.inlet")]),
    ("conv", "Links", [("conduits, 26 sections", "xsect.tabulated"), ("street · dummy", "xsect.street"),
                       ("Chebyshev irregular", "xsect.chebyshev_irregular"),
                       ("pumps · orifices · weirs · outlets · culverts", None),
                       ("storage shapes", "storage.shapes"), ("HEC-22 inlets", "inlets.link_attribute")]),
    ("conv", "Flow routing", [("steady", "routing.steady"), ("kinematic wave", "routing.kinwave"),
                              ("dynamic wave", "routing.dynwave"), ("finite volume", "routing.fv"),
                              ("local time stepping", "fv.lts"), ("1D GPU backend", "backend.1d.gpu")]),
    ("conv", "Pressurisation", [("EXTRAN", "pressurisation.extran"), ("slot", "pressurisation.slot"),
                                ("dynamic slot", "pressurisation.dynamic_slot"), ("TPA", "pressurisation.tpa"),
                                ("FV: slot", "fv.closure.slot"), ("FV: TPA", "fv.closure.tpa"),
                                ("FV: implicit head", "fv.pressurized_implicit"),
                                ("unsteady friction", "friction.unsteady_vitkovsky")]),
    ("conv", "Node solution", [("explicit continuity", "routing.node_continuity.explicit"),
                               ("semi-implicit continuity", "routing.node_continuity.semi_implicit"),
                               ("Anderson acceleration", "routing.anderson"),
                               ("FV_NODE_* keys", "fv.node_coupling_keys")]),
    ("conv", "Transport", [("CSTR", "transport.cstr"), ("Eulerian ARD", "transport.ard"),
                           ("Lagrangian LARD", "transport.lard"), ("random-walk dispersion", "transport.rwpt"),
                           ("first-order decay", "reactions.first_order"), ("MSX reactions", "reactions.msx"),
                           ("water age", "water_age"), ("heat", "heat"), ("sediment", "sediment"),
                           ("treatment functions", None)]),
    ("conv", "Inflows", [("dry weather flow", None), ("RDII: RTK", "rdii.rtk"), ("RDII: IA decay", "rdii.ia_decay"),
                         ("user · interface files", None), ("control rules", None)]),
]

# (src compartment, x, dst compartment, x, text, kind, both) — vertical arrows between
# the facing edges of two compartments, at absolute x positions
VERTICAL_FLUXES = [
    ("atm", 14, "land_sub", 14, "precipitation · snowmelt", "water", False),
    ("land_sub", 34, "atm", 34, "evaporation · ET", "water", False),
    ("atm", 62, "land_mesh", 62, "rain on cells", "water", False),
    ("land_mesh", 80, "atm", 80, "evaporation on cells", "water", False),
    ("land_sub", 16, "sub_2z", 16, "infiltration", "water", False),
    ("land_mesh", 62, "sub_mesh", 62, "per-cell infiltration", "water", False),
    ("sub_mesh", 80, "land_mesh", 80, "Dunne return flow · capillary rise", "water", False),
    ("sub_2z", 16, "conv", 16, "GW interflow (user [GWF])", "water", False),
    ("sub_mesh", 72, "conv", 72, "node–bed exchange", "water", True),
    ("conv", 45.5, "out", 45.5, None, "water", False),
]
# corridors: runoff to nodes (centre gap) and the two 1D↔2D pathways (right margin)
CORRIDOR_FLUXES = [
    ("centre", 50.0, "land_sub", "conv", "runoff → outlet node", "water", False),
    ("right", 92.6, "land_mesh", "conv", "1D ↔ 2D exchange at vertices / cells", "coupling", True),
    ("right", 96.8, "land_mesh", "conv", "street inlet capture", "coupling", False),
]
HORIZONTAL_FLUXES = [
    ("land_sub", "land_mesh", 0.86, "runoff → cell", "water"),
]

VARIANTS = {
    "eng_conceptual_map":        {"keep": set(COMPARTMENTS), "title": "OpenSWMM: compartments, processes and the formulations that represent them"},
    "hydrology_conceptual_map":  {"keep": {"atm", "land_sub", "land_mesh", "sub_2z", "sub_mesh"},
                                  "title": "Hydrology: atmosphere, land surface and subsurface representations"},
    "hydraulics_conceptual_map": {"keep": {"land_mesh", "conv", "out"},
                                  "title": "Hydraulics: the 1D network, the 2D surface and their coupling"},
    "quality_conceptual_map":    {"keep": {"land_sub", "land_mesh", "conv", "out"},
                                  "title": "Water quality: buildup, washoff, transport and reactions across the compartments"},
    "hydraulics_conceptual_1d2d": {"keep": {"land_mesh", "conv"},
                                   "title": "One-dimensional network and two-dimensional surface: the exchange pathways"},
}
GHOST = 0.22


def _comp_box(cid):
    _, x, ytop, w, h, _ = COMPARTMENTS[cid]
    return x, ytop - h, w, h


def _edge_point(cid, rel_x, side):
    x, y, w, h = _comp_box(cid)
    return (x + rel_x * w, y + h if side == "top" else y)


def _draw(variant: str):
    """Returns (fig, ax, hotspots); hotspots are (x0, y0, x1, y1, ref, label) in data units."""
    import primitives as P
    import refs
    import style
    keep = VARIANTS[variant]["keep"]
    fig, ax = P.canvas(*CANVAS)
    alpha_of = lambda cid: 1.0 if cid in keep else GHOST  # noqa: E731
    hotspots = []

    # compartments
    for cid, (title, x, ytop, w, h, fill) in COMPARTMENTS.items():
        a = alpha_of(cid)
        P.rounded(ax, x, ytop - h, w, h, fc=P.COMP_FILL[fill], ec=P.COMP_EDGE, lw=1.0, r=0.8, alpha=a, z=0)
        P.label(ax, x + 1.0, ytop - (0.55 if cid != "out" else 0.75), title, size=8.2, weight="bold",
                color=style.INK, alpha=a, z=3)

    # processes: stacked inside each compartment, two columns for the wide ones
    by_comp = {}
    for cid, title, alts in PROCESSES:
        by_comp.setdefault(cid, []).append((title, alts))
    for cid, procs in by_comp.items():
        a = alpha_of(cid)
        x, y, w, h = _comp_box(cid)
        ncol = 3 if cid == "conv" else (2 if cid in ("atm", "land_sub", "land_mesh", "sub_mesh") else 2)
        if cid in ("sub_2z",):
            ncol = 2
        gap = 0.9
        col_w = (w - 2.0 - gap * (ncol - 1)) / ncol
        cols_y = [y + h - 3.0] * ncol
        for i, (title, alts) in enumerate(procs):
            c = i % ncol if cid != "sub_mesh" else 0
            if cid == "sub_mesh":
                col_w = w - 2.0
            cx = x + 1.0 + c * (col_w + gap)
            top = cols_y[c]
            title_h = 2.0
            P.label(ax, cx + 0.6, top - 0.35, title, size=7.6, weight="bold", alpha=a, z=4)
            pills = []
            used = P.pill_row(ax, cx + 0.6, top - title_h, col_w - 1.2, alts, size=6.6, alpha=a, z=4, out=pills)
            box_h = title_h + used + 0.7
            P.rounded(ax, cx, top - box_h, col_w, box_h, fc=P.BOX_FILL, ec=P.BOX_EDGE, lw=0.8, r=0.5,
                      alpha=a, z=1)
            if title in refs.TITLE_REFS:
                hotspots.append((cx, top - title_h, cx + col_w, top, refs.TITLE_REFS[title], title))
            for text, fid, px, py, pw, ph in pills:
                ref = refs.ref_for(fid, text, title)
                if ref:
                    hotspots.append((px, py, px + pw, py + ph, ref, text))
            cols_y[c] = top - box_h - 0.8

    # fluxes
    for src, x0, dst, x1, text, kind, both in VERTICAL_FLUXES:
        a = min(alpha_of(src), alpha_of(dst))
        sx, sy, sw, sh = _comp_box(src)
        dx, dy, dw, dh = _comp_box(dst)
        downward = sy > dy
        p0 = (x0, sy if downward else sy + sh)
        p1 = (x1, dy + dh if downward else dy)
        P.arrow(ax, p0, p1, kind=kind, text=text, both=both, alpha=a, z=2)
    for corridor, xc, src, dst, text, kind, both in CORRIDOR_FLUXES:
        a = min(alpha_of(src), alpha_of(dst))
        sx, sy, sw, sh = _comp_box(src)
        dx, dy, dw, dh = _comp_box(dst)
        if corridor == "centre":
            ax.plot([sx + sw, xc], [sy + 3.0, sy + 3.0], color=P.ARROW[kind], lw=1.2, alpha=a, zorder=2)
            P.arrow(ax, (xc, sy + 3.0), (xc, dy + dh), kind=kind, text=text, alpha=a, z=2, rotation=90)
        else:
            P.arrow(ax, (xc, sy), (xc, dy + dh), kind=kind, text=text, both=both, alpha=a, z=2,
                    rotation=90, size=6.4)
            ax.plot([sx + sw, xc], [sy, sy], color=P.ARROW[kind], lw=1.2, alpha=a, zorder=2)
            ax.plot([dx + dw, xc], [dy + dh, dy + dh], color=P.ARROW[kind], lw=1.2, alpha=a, zorder=2)
    for src, dst, ry, text, kind in HORIZONTAL_FLUXES:
        a = min(alpha_of(src), alpha_of(dst))
        sx, sy, sw, sh = _comp_box(src)
        dx, dy, dw, dh = _comp_box(dst)
        P.arrow(ax, (sx + sw, sy + ry * sh), (dx, dy + ry * dh), kind=kind, text=text or None, alpha=a, z=2,
                text_offset=(0, 1.1), size=6.4)

    # title band: title left, status legend and arrow key right
    P.label(ax, 2, 93.6, VARIANTS[variant]["title"], size=9.6, weight="bold")
    P.legend(ax, 64, 93.9, size=6.6, title="Status of each alternative:")
    P.label(ax, 89, 0.6, "arrows: water (blue) · 1D–2D exchange (orange)", size=6.4, color=style.MUTED,
            ha="right", va="bottom")
    return fig, ax, hotspots


def build(sink):
    for variant in VARIANTS:
        if variant in sink.expected:
            fig, ax, hotspots = _draw(variant)
            for x0, y0, x1, y1, ref, label in hotspots:
                sink.hotspot(variant, ax, x0, y0, x1, y1, ref, label)
            sink.save(fig, variant)
