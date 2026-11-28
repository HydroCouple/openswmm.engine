"""Rain and infiltration on a mesh (Application Manual, the
mesh-hydrology chapter).

Two gages of different intensity stand west and east of a 20 x 20 m
patch, so every cell takes its own rainfall. With two gages there is no
triangle of gages to interpolate inside, so the engine weights them by
inverse distance (power 2) in every cell and says so in the report
(src/engine/2d/mesh/RainfallInterpolator.hpp:57-58;
src/engine/2d/SurfaceRouter2D.cpp:509-514). A two-layer aquifer lies
beneath every cell, so every cell is aquifer-owned: the tag rows of
`[2D_INFILTRATION_DEFAULTS]` are skipped (src/engine/2d/infil/Infil2D.cpp:306-320)
and the aquifer publishes each cell's receiving capacity. The intake applies
in full only above DRY_DEPTH and ramps off below it
(src/engine/2d/solver/SurfaceFluxCalculator.hpp:133-140), and the drain
vertex exchanges with its lowest-bed incident cell
(src/engine/2d/SurfaceRouter2D.cpp:993-999).

The figure is the two fields the run produces, read from the 2D results
file: what fell on each cell, and what went into the ground. The build
refuses a run whose report does not confirm the inverse-distance weights
and the aquifer ownership the annotations state.

Simulated tier: one run of docs/figures/decks/mesh_hydrology as committed,
cached under docs/figures/cache. Needs h5py as well as the openswmm package.
"""
from __future__ import annotations

import re

import numpy as np

REQUIRES = ("openswmm", "h5py")

DECK = "mesh_hydrology/mesh_hydrology.inp"


def _section(text, name):
    m = re.search(r"^\[" + name + r"\]\s*$(.*?)(?=^\[|\Z)", text, re.M | re.S)
    if not m:
        return []
    return [ln.split(";")[0].split() for ln in m.group(1).splitlines() if ln.split(";")[0].strip()]


def _gages(deck_text):
    return [(s[0], float(s[1]), float(s[2])) for s in _section(deck_text, "SYMBOLS") if len(s) >= 3]


def _drain_cell(h5, deck_text):
    """The drain vertex, its node, and the lowest-bed cell around it (the exchange cell)."""
    vertex, node = _section(deck_text, "2D_VERTEX_NODE_MAP")[0][:2]
    vertex = int(vertex)
    tri = np.asarray(h5["Mesh2_face_nodes"])
    z = np.asarray(h5["Mesh2_face_z"])
    around = [i for i, t in enumerate(tri) if vertex in t]
    return vertex, node, min(around, key=lambda i: z[i])


def _draw(style, h5, deck_text):
    import matplotlib.pyplot as plt
    from matplotlib.patches import Polygon
    x = np.asarray(h5["Mesh2_node_x"])
    y = np.asarray(h5["Mesh2_node_y"])
    tri = np.asarray(h5["Mesh2_face_nodes"])
    cx = np.asarray(h5["Mesh2_face_x"])
    cy = np.asarray(h5["Mesh2_face_y"])
    rain = np.asarray(h5["Mesh2_face_rainfall"])[10] * 1000.0 * 3600.0     # mm/hr while it rains
    infil = np.asarray(h5["Mesh2_face_infil_cum"])[-1] * 1000.0           # mm over the storm
    depth = np.asarray(h5["Mesh2_face_max_depth"]) * 1000.0               # mm
    vertex, node, drain = _drain_cell(h5, deck_text)
    fig, (a0, a1) = plt.subplots(1, 2, figsize=(9.4, 4.5))
    fig.patch.set_facecolor(style.SURFACE)
    for ax, vals, cmap, title, fmt in (
            (a0, rain, "Blues", "(a) Rainfall on each cell, interpolated from two gages", "{:.1f}"),
            (a1, infil, "YlOrBr", "(b) Water infiltrated over the storm", "{:.2f}")):
        cm = plt.get_cmap(cmap)
        lo, hi = float(vals.min()), float(vals.max())
        rng = (hi - lo) or 1.0
        for t, v in zip(tri, vals):
            ax.add_patch(Polygon(np.column_stack([x[t], y[t]]), closed=True,
                                 fc=cm(0.18 + 0.68 * (v - lo) / rng), ec=style.MUTED, lw=0.9))
        for i, (px, py, v) in enumerate(zip(cx, cy, vals)):
            ax.text(px, py + 0.7, fmt.format(v), fontsize=7.4, ha="center", va="center", color=style.INK)
        ax.set_xlim(-9.0, 29.0)
        ax.set_ylim(-4.0, 26.5)
        ax.set_aspect("equal", adjustable="box")
        ax.axis("off")
        ax.set_title(title, fontsize=9, loc="left")
    # the gages, on panel (a)
    for name, gx, gy in _gages(deck_text):
        a0.plot(gx, gy, marker="*", ms=15, mfc="#ffd23f", mec=style.INK, mew=0.9, zorder=5)
        a0.text(gx, gy - 1.8, name, fontsize=7.4, ha="center", color=style.INK)
    a0.text(0.0, -2.6, "mm/hr in each cell while it rains; the gages read 20 and 5 mm/hr",
            fontsize=7.2, color=style.MUTED)
    a0.text(0.0, 26.2, "Two gages: inverse-distance weights in every cell.\n"
            "Each cell takes its own value, and none leaves the\n"
            "range of the gage readings.",
            fontsize=7.2, color=style.MUTED, linespacing=1.45, va="top")
    # the drain, on panel (b)
    a1.plot(x[vertex], y[vertex], marker="o", ms=7, mfc=style.SURFACE, mec=style.INK, mew=1.2, zorder=5)
    a1.text(x[vertex] + 0.5, y[vertex] - 0.4, f"drain to {node}", fontsize=7.2, color=style.INK,
            ha="left", va="top", zorder=5)
    a1.text(cx[drain], cy[drain] - 1.0, f"cell {drain}", fontsize=7.0, ha="center", va="center",
            color=style.MUTED)
    a1.text(0.0, -2.6, "mm infiltrated over the storm, per cell", fontsize=7.2, color=style.MUTED)
    a1.text(0.0, 26.2, "The aquifer owns every cell, and each takes in the water\n"
            f"that reaches it. Cell {drain}, where the drain draws, never held\n"
            f"more than {depth[drain]:.4f} mm; the intake ramps off below DRY_DEPTH.",
            fontsize=7.2, color=style.MUTED, linespacing=1.45, va="top")
    fig.tight_layout()
    return fig, dict(drain=drain, rain=rain, infil=infil, depth=depth)


def build(sink):
    import h5py
    import style
    deck_text = (sink.fig_dir / "decks" / DECK).read_text()
    run = sink.run(DECK, None, "base")
    rpt = run.rpt.read_text(errors="replace")
    n_cells = len(_section(deck_text, "2D_TRIANGLES"))
    for claim in (f"inverse-distance fallback used for {n_cells} cells",
                  f"2D infiltration default '*': 0 applied, {n_cells} skipped (aquifer-owned)"):
        if claim not in rpt:
            raise RuntimeError(f"the annotations say so, but the report lacks: {claim!r}")
    with h5py.File(run.h5, "r") as h5:
        fig, k = _draw(style, h5, deck_text)
    print(f"    rainfall per cell {k['rain'].min():.1f} to {k['rain'].max():.1f} mm/hr; "
          f"infiltration {k['infil'].min():.2f} to {k['infil'].max():.2f} mm; "
          f"drain cell {k['drain']} held at most {k['depth'][k['drain']]:.4f} mm")
    sink.save(fig, "workflow_ch7_rain_and_infiltration")
