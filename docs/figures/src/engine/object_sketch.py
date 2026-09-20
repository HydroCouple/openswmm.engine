"""OpenSWMM's object model of a drainage system — the plan-view sketch.

Replaces the 338 × 257 px legacy "conceptual model of a stormwater drainage
system" bitmap that all four Chapter 1 pages embedded. Same idea (every
object type placed once in a small network) with the objects the legacy
sketch predates: a virtual junction splicing a surveyed reach, an inlet
junction on a street conduit, and a 2D mesh patch coupled to the network.
Synthetic geometry; no model is run.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()

CANVAS = (10.0, 5.6)


def _subcatchment(ax, pts, label, P, style):
    poly = np.array(pts)
    ax.fill(poly[:, 0], poly[:, 1], fc="#e9efe0", ec="#6f8a4f", lw=1.0, hatch="//", zorder=1, alpha=0.9)
    cx, cy = poly[:, 0].mean(), poly[:, 1].mean()
    ax.plot([cx], [cy], marker="s", ms=4, color="#6f8a4f", zorder=3)
    P.label(ax, cx, cy - 1.2, label, size=7, color="#4f6338", ha="center", va="top", z=4)
    return cx, cy


def _node(ax, x, y, kind, P, style):
    ink = style.INK
    if kind == "junction":
        ax.plot([x], [y], marker="o", ms=6, mfc=style.WATER, mec=ink, mew=0.8, zorder=5)
    elif kind == "virtual":
        ax.plot([x], [y], marker="o", ms=5, mfc="white", mec=style.WATER, mew=1.2, zorder=5)
    elif kind == "inlet":
        ax.plot([x], [y], marker="s", ms=7, mfc="#ffd8a8", mec=ink, mew=0.8, zorder=5)
        ax.plot([x - 0.5, x + 0.5], [y, y], color=ink, lw=0.7, zorder=6)
        ax.plot([x, x], [y - 0.5, y + 0.5], color=ink, lw=0.7, zorder=6)
    elif kind == "divider":
        ax.plot([x], [y], marker="D", ms=6, mfc="#cbd7f2", mec=ink, mew=0.8, zorder=5)
    elif kind == "storage":
        P.rounded(ax, x - 1.6, y - 1.0, 3.2, 2.0, fc="#cfe3f7", ec=ink, lw=0.9, r=0.4, z=5)
    elif kind == "outfall":
        ax.plot([x], [y], marker="v", ms=8, mfc="#9fb9d8", mec=ink, mew=0.8, zorder=5)


def _link(ax, p0, p1, kind, style, lw=2.0):
    x = [p0[0], p1[0]]
    y = [p0[1], p1[1]]
    if kind == "conduit":
        ax.plot(x, y, color="#5aa552", lw=lw, zorder=2, solid_capstyle="round")
    elif kind == "street":
        ax.plot(x, y, color="#8f8f8f", lw=lw + 1.6, zorder=2, solid_capstyle="round")
        ax.plot(x, y, color="#e8e8e8", lw=lw - 1.0, zorder=2.1, ls=(0, (3, 2)))
    elif kind == "pump":
        ax.plot(x, y, color="#5aa552", lw=lw, zorder=2)
        mx, my = (x[0] + x[1]) / 2, (y[0] + y[1]) / 2
        ax.plot([mx], [my], marker="^", ms=8, mfc="#ffffff", mec=style.INK, mew=0.9, zorder=4)
    elif kind == "weir":
        ax.plot(x, y, color="#5aa552", lw=lw, zorder=2)
        mx, my = (x[0] + x[1]) / 2, (y[0] + y[1]) / 2
        ax.plot([mx - 0.6, mx + 0.6], [my + 0.45, my + 0.45], color=style.INK, lw=1.6, zorder=4)
        ax.plot([mx - 0.6, mx + 0.6], [my - 0.45, my - 0.45], color=style.INK, lw=1.6, zorder=4)
    elif kind == "orifice":
        ax.plot(x, y, color="#5aa552", lw=lw, zorder=2)
        mx, my = (x[0] + x[1]) / 2, (y[0] + y[1]) / 2
        ax.plot([mx], [my], marker="o", ms=8, mfc="#ffffff", mec=style.INK, mew=0.9, zorder=4)
        ax.plot([mx], [my], marker="o", ms=3, mfc=style.INK, mec=style.INK, zorder=4.1)


def _mesh(ax, x0, y0, w, h, nx, ny, P, style):
    xs = np.linspace(x0, x0 + w, nx + 1)
    ys = np.linspace(y0, y0 + h, ny + 1)
    for j in range(ny):
        for i in range(nx):
            a, b = (xs[i], ys[j]), (xs[i + 1], ys[j])
            c, d = (xs[i + 1], ys[j + 1]), (xs[i], ys[j + 1])
            if (i + j) % 2:
                tris = [(a, b, c), (a, c, d)]
            else:
                tris = [(a, b, d), (b, c, d)]
            for t in tris:
                pts = np.array(t)
                depth = 0.18 + 0.5 * np.exp(-((pts[:, 0].mean() - (x0 + w * 0.35)) ** 2 + (pts[:, 1].mean() - (y0 + h * 0.5)) ** 2) / 14)
                ax.fill(pts[:, 0], pts[:, 1], fc=(0.16, 0.47, 0.84, depth), ec="#7a9bc4", lw=0.5, zorder=1.5)


def _draw():
    import primitives as P
    import style

    fig, ax = P.canvas(*CANVAS)

    # subcatchments with a rain gage
    s1 = _subcatchment(ax, [(4, 42), (18, 46), (24, 38), (16, 30), (6, 33)], "Subcatchment S1", P, style)
    s2 = _subcatchment(ax, [(24, 50), (40, 52), (44, 44), (34, 38), (26, 41)], "Subcatchment S2", P, style)
    ax.plot([12], [50], marker="*", ms=12, mfc="#ffd23f", mec=style.INK, mew=0.7, zorder=5)
    P.label(ax, 14, 50, "Rain gage", size=7, va="center", z=4)

    # network
    J1, J2, J3, J4 = (24, 30), (36, 26), (52, 24), (64, 22)
    VJ = (44, 25)
    D = (76, 24)
    ST = (80, 12)
    OUT = (94, 20)
    IJ = (58, 34)
    _link(ax, s1[0:2] if False else (16, 30), J1, "conduit", style, lw=1.2)   # subcatchment outlet
    _link(ax, J1, J2, "conduit", style)
    _link(ax, J2, VJ, "conduit", style)
    _link(ax, VJ, J3, "conduit", style)
    _link(ax, J3, J4, "conduit", style)
    _link(ax, J4, D, "conduit", style)
    _link(ax, D, OUT, "weir", style)
    _link(ax, D, ST, "orifice", style)
    _link(ax, ST, (94, 12), "pump", style)
    _link(ax, (94, 12), OUT, "conduit", style)
    _link(ax, (62, 40), IJ, "street", style)
    _link(ax, IJ, J4, "conduit", style, lw=1.2)
    ax.plot([34, 36], [38, 26], color="#6f8a4f", lw=1.0, ls=(0, (2, 2)), zorder=2)   # S2 outlet
    ax.plot([16, 24], [30, 30], color="#6f8a4f", lw=1.0, ls=(0, (2, 2)), zorder=2)   # S1 outlet

    for pt, kind in ((J1, "junction"), (J2, "junction"), (J3, "junction"), (J4, "junction"),
                     (VJ, "virtual"), (D, "divider"), (ST, "storage"), (OUT, "outfall"), (IJ, "inlet")):
        _node(ax, *pt, kind, P, style)

    # 2D mesh patch coupled at J3 (vertex coupling)
    _mesh(ax, 46, 6, 20, 12, 5, 3, P, style)
    ax.plot([J3[0], 54], [J3[1], 18], color="#eb6834", lw=1.4, ls=(0, (3, 1.5)), zorder=3)
    ax.plot([54], [18], marker="o", ms=4, mfc="#eb6834", mec="#eb6834", zorder=4)

    # labels
    lab = dict(size=7, z=6)
    P.label(ax, J1[0] - 1.2, J1[1] + 1.0, "Junction", ha="right", va="bottom", **lab)
    P.label(ax, J2[0], J2[1] - 1.4, "Conduit", ha="center", va="top", **lab)
    P.label(ax, VJ[0], VJ[1] + 1.3, "Virtual junction", ha="center", va="bottom", **lab)
    P.label(ax, IJ[0] + 1.4, IJ[1], "Inlet junction on a street", va="center", **lab)
    P.label(ax, 62, 41.2, "Street conduit", ha="center", va="bottom", **lab)
    P.label(ax, D[0], D[1] + 1.4, "Divider", ha="center", va="bottom", **lab)
    P.label(ax, 85, 25.2, "Weir", ha="center", va="bottom", **lab)
    P.label(ax, 76.6, 18.0, "Orifice", ha="right", va="center", **lab)
    P.label(ax, ST[0], ST[1] - 1.6, "Storage unit", ha="center", va="top", **lab)
    P.label(ax, 87, 10.6, "Pump", ha="center", va="top", **lab)
    P.label(ax, OUT[0] + 1.4, OUT[1], "Outfall", va="center", **lab)
    P.label(ax, 56, 4.6, "2D mesh (triangles · quads) coupled to junction J3", ha="center", va="top", **lab)
    P.label(ax, 50.5, 20.2, "1D ↔ 2D exchange", size=6.5, color="#eb6834", ha="right", va="bottom", z=6,
            style_="italic")

    P.label(ax, 2, 55.0, "Objects of an OpenSWMM model", size=9.6, weight="bold")
    P.label(ax, 2, 52.6, "hydrology: rain gages and subcatchments, with aquifers and snow packs beneath them · "
                         "hydraulics: nodes, links and a 2D mesh · quality: pollutants and land uses on the subcatchments",
            size=6.4, color=style.MUTED, va="top")
    return fig


def build(sink):
    sink.save(_draw(), "eng_object_sketch")
