"""Rain and infiltration on a mesh (Application Manual, the
mesh-hydrology chapter).

Two gages of different intensity stand west and east of a 20 x 20 m
patch, so every cell takes its own interpolated rainfall; three soil tags
give the cells three infiltration methods. The figure is the two fields
the run produces, read from the 2D results file: what fell on each cell,
and what went into the ground.

Simulated tier: one run of docs/figures/decks/mesh_hydrology, cached
under docs/figures/cache. Needs h5py as well as the openswmm package.
"""
from __future__ import annotations

import re

import numpy as np

REQUIRES = ("openswmm", "h5py")

DECK = "mesh_hydrology/mesh_hydrology.inp"
METHOD_OF_TAG = {"LAWN": "HORTON", "PAVED": "CONSTANT", "WOODS": "GREEN_AMPT"}


def _tags(deck_text):
    m = re.search(r"^\[2D_TRIANGLES\]\s*$(.*?)(?=^\[|\Z)", deck_text, re.M | re.S)
    out = []
    for ln in m.group(1).splitlines():
        s = ln.split(";")[0].split()
        if len(s) >= 6:
            out.append(s[5])
    return out


def _gages(deck_text):
    m = re.search(r"^\[SYMBOLS\]\s*$(.*?)(?=^\[|\Z)", deck_text, re.M | re.S)
    out = []
    for ln in m.group(1).splitlines():
        s = ln.split(";")[0].split()
        if len(s) >= 3:
            out.append((s[0], float(s[1]), float(s[2])))
    return out


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
    tags = _tags(deck_text)
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
        ax.set_ylim(-4.0, 24.0)
        ax.set_aspect("equal", adjustable="box")
        ax.axis("off")
        ax.set_title(title, fontsize=9, loc="left")
    # the gages, on panel (a)
    for name, gx, gy in _gages(deck_text):
        a0.plot(gx, gy, marker="*", ms=15, mfc="#ffd23f", mec=style.INK, mew=0.9, zorder=5)
        a0.text(gx, gy - 1.8, name, fontsize=7.4, ha="center", color=style.INK)
    a0.text(0.0, -2.6, "mm/hr in each cell while it rains; the gages read 20 and 5 mm/hr",
            fontsize=7.2, color=style.MUTED)
    a0.text(0.0, 22.3, "Laplace natural-neighbour weights: every cell takes its own value,\n"
            "and no cell leaves the range of the gage readings.", fontsize=7.2, color=style.MUTED,
            linespacing=1.45)
    # the tags and their methods, on panel (b)
    for i, (px, py) in enumerate(zip(cx, cy)):
        a1.text(px, py - 1.0, f"{tags[i]}\n{METHOD_OF_TAG[tags[i]]}", fontsize=6.2, ha="center", va="center",
                color=style.MUTED, linespacing=1.3)
    a1.text(0.0, -2.6, "mm infiltrated over the storm, per cell", fontsize=7.2, color=style.MUTED)
    dry = int(np.argmin(depth))
    a1.text(0.0, 22.3, f"Three tags, three methods. Cell {dry} infiltrated {infil[dry]:.2f} mm: it never\n"
            f"held more than {depth[dry]:.2f} mm of water, and the sink is ramped off below DRY_DEPTH.",
            fontsize=7.2, color=style.MUTED, linespacing=1.45)
    fig.tight_layout()
    return fig


def build(sink):
    import h5py
    import style
    deck_text = (sink.fig_dir / "decks" / DECK).read_text()
    run = sink.run(DECK, None, "base")
    with h5py.File(run.h5, "r") as h5:
        fig = _draw(style, h5, deck_text)
        rain = np.asarray(h5["Mesh2_face_rainfall"])[10] * 1000.0 * 3600.0
        infil = np.asarray(h5["Mesh2_face_infil_cum"])[-1] * 1000.0
    print(f"    rainfall per cell {rain.min():.1f} to {rain.max():.1f} mm/hr; "
          f"infiltration {infil.min():.2f} to {infil.max():.2f} mm")
    sink.save(fig, "workflow_ch7_rain_and_infiltration")
