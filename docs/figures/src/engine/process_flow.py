"""Processes modelled by OpenSWMM — the replacement for the EPA "Figure 1-3".

The legacy chart (precipitation → snowmelt → initial abstraction → surface
runoff → LID → washoff → routing → treatment) extended with the distributed
branch (rain on the mesh → 2D overland flow → 1D↔2D exchange), the two
subsurface representations, inlet capture, and the transport chain
(transport engine → reactions → age / heat). Alternatives are status pills
from docs/figures/status.py. Cited by all four Chapter 1 pages.

Layout: four columns feed one full-width routing band, so every arrow into
routing is a vertical drop and nothing crosses a box.
"""
from __future__ import annotations

REQUIRES = ()

CANVAS = (10.0, 7.2)

# id: (title, x, y_top, w, [(pill, feature id | None), ...])
BOXES = {
    # column 1 — lumped hydrology chain
    "precip":   ("Precipitation", 2, 64, 22, [("rain · snow", None), ("scale factors", "climate.precip_scaling")]),
    "snow":     ("Snowmelt", 2, 56.5, 22, [("degree-day · areal depletion", "snowmelt.degree_day")]),
    "abstract": ("Initial abstraction · evaporation", 2, 49, 22, [("depression storage", None)]),
    "runoff":   ("Surface runoff", 2, 41.5, 22, [("nonlinear reservoir", "runoff.subcatchment"),
                                                  ("Horton · Green-Ampt · CN", "infiltration.horton")]),
    "lid":      ("LID controls", 2, 32.5, 22, [("layered units", "lid.layered"), ("storage nodes", "lid.storage_node")]),
    "washoff":  ("Washoff", 2, 25, 22, [("EMC · exponential · rating", "quality.buildup_washoff")]),
    # column 2 — subsurface, and buildup beside washoff
    "gw":       ("Groundwater", 28, 53, 24, [("two-zone aquifer per subcatchment", "groundwater.two_zone"),
                                             ("two-layer aquifer on the mesh", "groundwater.mesh_two_layer"),
                                             ("groundwater transport", "groundwater.transport")]),
    "buildup":  ("Pollutant buildup", 28, 25, 20, [("land uses · sweeping", "quality.buildup_washoff")]),
    # column 3 — distributed hydrology on the 2D mesh
    "rainmesh": ("Rain on the mesh", 56, 64, 22, [("natural neighbour", "rain.mesh_natural_neighbour")]),
    "overland": ("2D overland flow", 56, 56.5, 22, [("local inertial", "overland2d.local_inertial"),
                                                    ("full shallow water", "overland2d.full_swe"),
                                                    ("diffusive wave", "overland2d.diffusive_wave")]),
    "infilcell": ("Per-cell infiltration · evaporation", 56, 47, 22, [("→ lost · subcatchment · mesh aquifer", None)]),
    "surfq2d":  ("2D surface quality", 56, 39, 22, [("coverages · buildup · washoff", "quality.mesh_surface")]),
    # column 4 — external inflows and inlets
    "inflows":  ("External inflows", 81, 64, 17, [("dry weather flow", None), ("RDII: RTK", "rdii.rtk"),
                                                  ("RDII: IA decay", "rdii.ia_decay"), ("user · interface files", None)]),
    "inlets":   ("Street inlets", 86, 52, 12, [("HEC-22 link attribute", "inlets.link_attribute"),
                                               ("inlet junction", "junctions.inlet")]),
    # bands
    "routing":  ("Channel, pipe and storage routing", 2, 17, 96, [("steady", "routing.steady"),
                                                                  ("kinematic wave", "routing.kinwave"),
                                                                  ("dynamic wave", "routing.dynwave"),
                                                                  ("finite volume", "routing.fv"),
                                                                  ("EXTRAN · slot · dynamic slot", "pressurisation.slot"),
                                                                  ("TPA", "pressurisation.tpa"),
                                                                  ("virtual · inlet junctions", "junctions.virtual")]),
    "transport": ("Transport and reactions", 2, 10, 96, [("CSTR", "transport.cstr"), ("ARD", "transport.ard"),
                                                         ("LARD", "transport.lard"),
                                                         ("first-order decay", "reactions.first_order"),
                                                         ("MSX", "reactions.msx"), ("water age", "water_age"),
                                                         ("heat", "heat"), ("sediment", "sediment"),
                                                         ("treatment · diversion", None)]),
}

# vertical chains (src above dst, same column)
DROPS = [("precip", "snow"), ("snow", "abstract"), ("abstract", "runoff"), ("runoff", "lid"), ("lid", "washoff"),
         ("rainmesh", "overland"), ("overland", "infilcell"), ("infilcell", "surfq2d")]
# (src, x, text, kind): vertical drops onto the routing band
TO_ROUTING = [("washoff", 13, None, "mass"), ("gw", 48, "interflow · node–bed exchange", "water"),
              ("surfq2d", 67, "species on the mesh", "mass"), ("inflows", 83, "inflows", "water"),
              ("inlets", 92, "inlet capture", "coupling")]
# (src, dst, text, kind): horizontals between columns (the short ones stay unlabelled)
ACROSS = [("abstract", "gw", None, "water"),
          ("infilcell", "gw", None, "water"),
          ("buildup", "washoff", None, "mass")]


def _draw():
    import primitives as P
    import style

    fig, ax = P.canvas(*CANVAS)
    geom = {}
    for bid, (title, x, ytop, w, alts) in BOXES.items():
        P.label(ax, x + 0.6, ytop - 0.35, title, size=7.8, weight="bold", z=4)
        used = P.pill_row(ax, x + 0.6, ytop - 2.0, w - 1.2, alts, size=6.6, z=4)
        h = 2.0 + used + 0.7
        P.rounded(ax, x, ytop - h, w, h, fc=P.BOX_FILL, ec=P.BOX_EDGE, lw=0.9, r=0.6, z=1)
        geom[bid] = (x, ytop - h, w, h)

    def box(bid):
        return geom[bid]

    for x, text in ((2, "lumped: subcatchments"), (28, "subsurface"), (56, "distributed: 2D mesh"),
                    (81, "inflows · inlets")):
        P.label(ax, x, 66.6, text.upper(), size=6.8, color=style.MUTED, weight="bold")

    for src, dst in DROPS:
        sx, sy, sw, sh = box(src)
        dx, dy, dw, dh = box(dst)
        P.arrow(ax, (sx + sw / 2, sy), (dx + dw / 2, dy + dh), kind="water", lw=1.1)
    rx, ry, rw, rh = box("routing")
    for src, x, text, kind in TO_ROUTING:
        sx, sy, sw, sh = box(src)
        P.arrow(ax, (x, sy), (x, ry + rh), kind=kind, text=text, size=6.3, lw=1.1, rotation=90)
    # 1D <-> 2D exchange: a corridor between the subsurface and mesh columns
    ox, oy, ow, oh = box("overland")
    xc = 54.0
    ax.plot([ox, xc], [oy + oh / 2, oy + oh / 2], color=P.ARROW["coupling"], lw=1.1, zorder=2)
    P.arrow(ax, (xc, oy + oh / 2), (xc, ry + rh), kind="coupling", text="1D ↔ 2D exchange", both=True,
            size=6.3, lw=1.1, rotation=90)
    for src, dst, text, kind in ACROSS:
        sx, sy, sw, sh = box(src)
        dx, dy, dw, dh = box(dst)
        ymid = sy + sh / 2
        if sx < dx:
            P.arrow(ax, (sx + sw, ymid), (dx, ymid), kind=kind, text=text, size=6.3, lw=1.1, text_offset=(0, 1.0))
        else:
            P.arrow(ax, (sx, ymid), (dx + dw, ymid), kind=kind, text=text, size=6.3, lw=1.1, text_offset=(0, 1.0))

    tx, ty, tw, th = box("transport")
    P.arrow(ax, (50, ry), (50, ty + th), kind="water", lw=1.1)
    P.rounded(ax, 2, 1.0, 96, 2.8, fc=P.COMP_FILL["out"], ec=P.COMP_EDGE, lw=0.9, r=0.8, z=1)
    P.arrow(ax, (50, ty), (50, 3.8), kind="water", lw=1.1)
    P.label(ax, 50, 2.4, "OUTFALLS · receiving water", size=7.8, weight="bold", ha="center", va="center", z=4)
    P.legend(ax, 2, 71.4, size=6.4, title="Status of each alternative:")
    P.label(ax, 98, 69.0, "arrows: water (blue) · constituent mass (violet) · 1D–2D exchange (orange)",
            size=6.4, color=style.MUTED, ha="right", va="bottom")
    return fig


def build(sink):
    sink.save(_draw(), "eng_process_flow")
