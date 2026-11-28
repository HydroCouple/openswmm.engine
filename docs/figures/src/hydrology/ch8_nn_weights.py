"""Rainfall weights on the mesh: Laplace natural-neighbour inside the gage
hull, inverse distance outside it (hydrology Chapter 8, §8.2.2).

Left: six gages and their Delaunay triangulation. Inserting a cell
centroid as a new site carves the cavity of triangles whose circumcircles
contain it and fans that cavity; the fan circumcentres are the vertices of
the centroid's new Voronoi cell, and the facet it shares with each natural
neighbour, over the distance to that gage, is the Laplace weight. Gages
outside the cavity get no weight at all. Right: a centroid beyond the hull
falls back to power-2 inverse distance over every located gage.

The triangulation, the insertion and the weights are computed here by the
same construction the engine uses, so the numbers on the drawing are the
numbers the interpolator produces. Synthetic; no model run.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()

# (name, x, y, mm/hr)
GAGES = [("A", 0.6, 1.2, 10.0), ("B", 4.3, 0.5, 22.0), ("C", 8.0, 1.9, 30.0),
         ("D", 7.1, 6.2, 26.0), ("E", 3.2, 7.0, 16.0), ("F", 0.9, 4.6, 12.0)]
P_IN = np.array([5.5, 2.5])
P_OUT = np.array([10.2, 5.4])
XY = np.array([[g[1], g[2]] for g in GAGES])
NAMES = [g[0] for g in GAGES]
RATES = np.array([g[3] for g in GAGES])


def circumcentre(a, b, c):
    d = 2.0 * (a[0] * (b[1] - c[1]) + b[0] * (c[1] - a[1]) + c[0] * (a[1] - b[1]))
    if abs(d) < 1e-12:
        return None
    sa, sb, sc = a @ a, b @ b, c @ c
    return np.array([(sa * (b[1] - c[1]) + sb * (c[1] - a[1]) + sc * (a[1] - b[1])) / d,
                     (sa * (c[0] - b[0]) + sb * (a[0] - c[0]) + sc * (b[0] - a[0])) / d])


def in_circumcircle(p, a, b, c):
    o = circumcentre(a, b, c)
    return o is not None and np.linalg.norm(p - o) < np.linalg.norm(a - o) - 1e-12


def delaunay(pts):
    """Bowyer-Watson over a super-triangle; returns index triples into pts."""
    lo, hi = pts.min(axis=0), pts.max(axis=0)
    c, r = (lo + hi) / 2, np.linalg.norm(hi - lo) + 1.0
    sup = np.array([c + [0, 3 * r], c + [-3 * r, -2 * r], c + [3 * r, -2 * r]])
    work = np.vstack([pts, sup])
    n = len(pts)
    tris = [(n, n + 1, n + 2)]
    for i in range(n):
        p = work[i]
        bad = [t for t in tris if in_circumcircle(p, work[t[0]], work[t[1]], work[t[2]])]
        edges = {}
        for t in bad:
            for e in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
                k = tuple(sorted(e))
                edges[k] = edges.get(k, 0) + 1
        tris = [t for t in tris if t not in bad]
        tris += [(i, a, b) for (a, b), cnt in edges.items() if cnt == 1]
    return [t for t in tris if max(t) < n]


def insert_point(p, pts, tris):
    """(ordered Voronoi polygon of p, {site index: (facet endpoints, length)})."""
    bad = [t for t in tris if in_circumcircle(p, pts[t[0]], pts[t[1]], pts[t[2]])]
    if not bad:
        return None, {}
    edges = {}
    for t in bad:
        for e in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
            k = tuple(sorted(e))
            edges[k] = edges.get(k, 0) + 1
    hull = [e for e, cnt in edges.items() if cnt == 1]
    fan = [(a, b) for a, b in hull]
    cc = {}
    for a, b in fan:
        o = circumcentre(p, pts[a], pts[b])
        if o is None:
            return None, {}
        cc[tuple(sorted((a, b)))] = o
    by_site = {}
    for a, b in fan:
        for s in (a, b):
            by_site.setdefault(s, []).append(tuple(sorted((a, b))))
    facets = {}
    for s, keys in by_site.items():
        if len(keys) != 2:
            continue
        e0, e1 = cc[keys[0]], cc[keys[1]]
        facets[s] = ((e0, e1), float(np.linalg.norm(e0 - e1)))
    poly = np.array(list(cc.values()))
    ang = np.arctan2(poly[:, 1] - p[1], poly[:, 0] - p[0])
    return poly[np.argsort(ang)], facets


def _sites(ax, style, weights=None):
    for i, (name, x, y, r) in enumerate(GAGES):
        on = weights is None or weights.get(i, 0) > 0
        ax.plot(x, y, marker="*", ms=15, mfc="#ffd23f" if on else "#e4e4e4", mec=style.INK, mew=0.9, zorder=6)
        ax.text(x, y + 0.36, f"{name} · {r:.0f}", fontsize=7.4, ha="center",
                color=style.INK if on else style.MUTED, zorder=6)


def _panel_inside(ax, style):
    from matplotlib.patches import Polygon
    tris = delaunay(XY)
    poly, facets = insert_point(P_IN, XY, tris)
    raw = {s: L / np.linalg.norm(P_IN - XY[s]) for s, (_, L) in facets.items()}
    tot = sum(raw.values())
    w = {s: v / tot for s, v in raw.items()}
    for t in tris:
        ax.add_patch(Polygon(XY[list(t)], closed=True, fc="none", ec=style.MUTED, lw=0.8, alpha=0.7, zorder=1))
    ax.add_patch(Polygon(poly, closed=True, fc=style.COLORS["fv"], alpha=0.10, ec="none", zorder=2))
    for s, ((e0, e1), L) in facets.items():
        ax.plot([e0[0], e1[0]], [e0[1], e1[1]], color=style.COLORS["fv-lts"], lw=2.6, zorder=4, solid_capstyle="round")
        ax.plot([P_IN[0], XY[s][0]], [P_IN[1], XY[s][1]], color=style.COLORS["fv"], lw=0.9, ls=(0, (3, 2)),
                alpha=0.8, zorder=3)
        mid = (e0 + e1) / 2
        out = mid - P_IN
        out = out / max(np.linalg.norm(out), 1e-9) * 0.42
        ax.text(mid[0] + out[0], mid[1] + out[1], f"ℓ={L:.2f}", fontsize=6.8, color=style.COLORS["fv-lts"],
                zorder=5, ha="center", va="center")
    ax.plot(*P_IN, marker="o", ms=7, mfc=style.COLORS["fv"], mec="white", mew=1.2, zorder=7)
    ax.text(P_IN[0] + 0.15, P_IN[1] - 0.45, "cell centroid p", fontsize=7.6, color=style.COLORS["fv"], zorder=7)
    _sites(ax, style, w)
    rate = sum(w[s] * RATES[s] for s in w)
    txt = "   ".join(f"w_{NAMES[s]} = {w[s]:.3f}" for s in sorted(w))
    ax.text(-0.9, 9.35, "Laplace weights  w = ℓ / distance, normalised", fontsize=7.8, color=style.INK, va="top")
    ax.text(-0.9, 8.85, txt, fontsize=7.6, color=style.INK, va="top")
    away = [NAMES[s] for s in range(len(GAGES)) if s not in w]
    tail = (f"        {' and '.join(away)} {'is' if len(away) == 1 else 'are'} not a natural neighbour: weight 0"
            if away else "")
    ax.text(-0.9, 8.35, f"intensity at p = {rate:.1f} mm/hr{tail}", fontsize=7.6, color=style.INK, va="top")
    ax.text(-0.9, -1.35, "grey: the Delaunay triangulation of the gages        blue fill: the centroid's new Voronoi cell\n"
            "orange: the facet it shares with each natural neighbour", fontsize=7.2, color=style.MUTED, va="top",
            linespacing=1.5)
    ax.set_title("(a) Inside the hull — Laplace natural-neighbour weights", fontsize=9, loc="left")


def _panel_outside(ax, style):
    from matplotlib.patches import Polygon
    tris = delaunay(XY)
    d = np.linalg.norm(XY - P_OUT, axis=1)
    w = (1.0 / d ** 2) / np.sum(1.0 / d ** 2)
    for t in tris:
        ax.add_patch(Polygon(XY[list(t)], closed=True, fc="none", ec=style.MUTED, lw=0.8, alpha=0.45, zorder=1))
    for i, (name, x, y, _) in enumerate(GAGES):
        ax.plot([P_OUT[0], x], [P_OUT[1], y], color=style.COLORS["dw"], lw=0.8 + 2.2 * w[i], ls=(0, (3, 2)), zorder=2)
    ax.plot(*P_OUT, marker="o", ms=7, mfc=style.COLORS["dw"], mec="white", mew=1.2, zorder=7)
    ax.text(P_OUT[0], P_OUT[1] + 0.4, "cell centroid q,\noutside every Delaunay triangle", fontsize=7.6,
            ha="right", color=style.COLORS["dw"], zorder=7)
    _sites(ax, style)
    rate = float(w @ RATES)
    txt = "   ".join(f"w_{NAMES[i]} = {w[i]:.3f}" for i in range(len(GAGES)))
    ax.text(-0.9, 9.35, "Inverse-distance weights  w = d⁻² normalised, over every located gage", fontsize=7.8,
            color=style.INK, va="top")
    ax.text(-0.9, 8.85, txt, fontsize=7.2, color=style.INK, va="top")
    ax.text(-0.9, 8.35, f"intensity at q = {rate:.1f} mm/hr", fontsize=7.6, color=style.INK, va="top")
    ax.text(-0.9, -1.35, "line weight is the gage's share. IDW never leaves the range of the readings: it tends to the\n"
            "nearest gage close in, and to the plain mean far out.", fontsize=7.2, color=style.MUTED, va="top",
            linespacing=1.5)
    ax.set_title("(b) Outside the hull — inverse-distance weights", fontsize=9, loc="left")


def _draw(style):
    import matplotlib.pyplot as plt
    fig, (a0, a1) = plt.subplots(1, 2, figsize=(9.2, 4.9))
    fig.patch.set_facecolor(style.SURFACE)
    for ax in (a0, a1):
        ax.set_xlim(-1.0, 12.0)
        ax.set_ylim(-1.8, 9.6)
        ax.set_aspect("equal", adjustable="datalim")
        ax.axis("off")
    _panel_inside(a0, style)
    _panel_outside(a1, style)
    fig.subplots_adjust(left=0.01, right=0.99, top=0.94, bottom=0.02, wspace=0.05)
    return fig


def build(sink):
    import style
    sink.save(_draw(style), "hydrology_ch8_nn_weights")
