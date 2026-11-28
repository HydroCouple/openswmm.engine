"""The output cell velocity of the local-inertial and diffusive-wave closures
(hydraulics Chapter 9, §9.9 Reporting).

Each wet cell's reported vector u minimizes sum_e A_e (n_e . u - Q_e / A_e)^2
over its edges, where Q_e is the edge's volume flux and A_e its wet cross
section (face flow depth x edge length x interior transmissivity). The
engine accumulates sum A n n^T and sum n Q and solves the 2 x 2 system, so
no tiny section is ever divided by; a cell whose wet normals cannot fix two
components (det <= 1e-10 trace^2), or whose accepted flow crosses an edge
that is dry now, reports a pair of NaNs, and a dry cell reports zero.

(a) A triangle, all edges wet at one depth: the three edge-normal
    velocities Q/A and the fitted vector. One edge carries 30 % more than a
    uniform flow would, so the fit cannot honor all three; the residual
    n . u - Q/A is printed at each edge.
(b) A quadrilateral with one shallow edge (a thin film): the same misfit on
    that edge barely moves the vector, because its weight A_e is small.
(c) The same quadrilateral wet across two opposite edges only, the other two
    dry with no flow: the normals are parallel, the determinant is zero, and
    the cell reports NaN (unavailable), not zero velocity.

The edge depths and flows are illustrative inputs; the fit is the engine's
own, ported from computeFaceVelocity in
src/engine/2d/solver/SurfaceFluxCalculator.cpp:390-479 (the accumulation at
:460-464, the rank test at :466-471, the solve at :472-473; the dry-edge
rule at :449-457). Vectors are drawn in the physical outward-normal sign
convention the API and the results file use.

Synthetic tier: numpy only, no model run.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()

DRY_DEPTH = 0.001        # an illustrative DRY_DEPTH (m) for the edge wet test of :449
U_TRUE = np.array([0.60, 0.25])

TRI = np.array([[0.0, 0.0], [3.0, 0.3], [1.2, 2.7]])
QUAD = np.array([[0.0, 0.0], [3.0, 0.0], [3.0, 2.2], [0.0, 2.2]])


def edges(poly):
    """(midpoint, outward unit normal, length) per edge, counterclockwise polygon."""
    out = []
    n = len(poly)
    for k in range(n):
        a, b = poly[k], poly[(k + 1) % n]
        t = b - a
        length = float(np.hypot(*t))
        nrm = np.array([t[1], -t[0]]) / length
        out.append(((a + b) / 2.0, nrm, length))
    return out


def fit(normals, areas, flows):
    """SurfaceFluxCalculator.cpp:449-473: weighted normal-equation solve with the rank test."""
    a00 = a01 = a11 = b0 = b1 = 0.0
    for (nx, ny), area, q in zip(normals, areas, flows):
        if area <= 0.0:                       # a dry section
            if q != 0.0:
                return np.array([np.nan, np.nan]), 0.0, 0.0
            continue
        a00 += area * nx * nx
        a01 += area * nx * ny
        a11 += area * ny * ny
        b0 += nx * q
        b1 += ny * q
    det = a00 * a11 - a01 * a01
    trace = a00 + a11
    if not det > 1e-10 * trace * trace:
        return np.array([np.nan, np.nan]), det, trace
    return np.array([(a11 * b0 - a01 * b1) / det, (a00 * b1 - a01 * b0) / det]), det, trace


def _case(poly, depths, bump):
    """Edge sections, normals and flows: a uniform flow U_TRUE, one edge scaled by `bump`."""
    eds = edges(poly)
    areas, normals, flows = [], [], []
    for k, (m, nrm, length) in enumerate(eds):
        h = depths[k]
        area = h * length if h > DRY_DEPTH else 0.0
        q = area * float(nrm @ U_TRUE) * bump.get(k, 1.0)
        areas.append(area)
        normals.append(nrm)
        flows.append(q)
    return eds, np.array(areas), np.array(normals), np.array(flows)


def _draw(ax, style, poly, eds, areas, flows, u, title, depths, note, hfrac=None):
    from matplotlib.patches import FancyArrowPatch, Polygon
    ax.add_patch(Polygon(poly, closed=True, fc="#e8f0fa", ec=style.INK, lw=1.3, zorder=1))
    c = poly.mean(axis=0)
    scale = 1.5                                  # drawing length per m/s
    for k, ((m, nrm, length), area, q, h) in enumerate(zip(eds, areas, flows, depths)):
        wet = area > 0.0
        a, b = poly[k], poly[(k + 1) % len(poly)]
        f = 0.2 if hfrac is None else hfrac[k]
        inside = a + f * (b - a) - nrm * (0.2 + 0.32 * abs(nrm[0]))     # clear of its arrow
        if not wet:
            ax.plot([a[0], b[0]], [a[1], b[1]], color="#b9b6ad", lw=3.2, zorder=2, solid_capstyle="butt")
            ax.text(m[0] + nrm[0] * 0.3, m[1] + nrm[1] * 0.3, "dry, Q = 0", fontsize=7.0,
                    ha="center", va="center", color=style.MUTED)
            continue
        ax.text(inside[0], inside[1], f"h {h:.2f}", fontsize=6.8, ha="center", va="center",
                color=style.MUTED, rotation=0)
        vn = q / area
        p1 = m + nrm * vn * scale
        ax.add_patch(FancyArrowPatch(m, p1, arrowstyle="-|>", mutation_scale=9, lw=1.4,
                                     color=style.COLORS["dw"], zorder=4, shrinkA=0, shrinkB=0))
        if np.all(np.isfinite(u)):
            res = round(float(nrm @ u) - vn, 2) + 0.0
            lab = f"{vn:+.2f}\nres. {res:+.2f}"
        else:
            lab = f"{vn:+.2f}"
        gap = 0.32 + 0.42 * abs(nrm[0])          # text is wider than it is tall
        off = nrm * (max(vn, 0.0) * scale + gap)
        ax.text(m[0] + off[0], m[1] + off[1], lab, fontsize=7.0, ha="center", va="center",
                color=style.INK, linespacing=1.2,
                bbox=dict(boxstyle="round,pad=0.15", fc=style.SURFACE, ec="none", alpha=0.85))
    ax.plot(*c, marker="o", ms=4.5, color=style.INK, zorder=5)
    if np.all(np.isfinite(u)):
        ax.add_patch(FancyArrowPatch(c, c + u * scale, arrowstyle="-|>", mutation_scale=12, lw=2.4,
                                     color=style.COLORS["fv"], zorder=6, shrinkA=0, shrinkB=0))
        tip = c + u * scale
        ax.text(tip[0] + 0.12, tip[1] + 0.08, "u", fontsize=8.5, ha="left", va="bottom",
                color=style.COLORS["fv"], weight="bold")
    else:
        ax.text(c[0], c[1] - 0.3, "u = (NaN, NaN):\nunavailable,\nnot zero", fontsize=7.4,
                ha="center", va="top", color=style.COLORS["fv-lts"], weight="bold", linespacing=1.25)
    ax.text(0.03, 0.02, note, transform=ax.transAxes, fontsize=7.0, color=style.MUTED, va="top",
            linespacing=1.35)
    ax.set_title(title, loc="left", fontsize=9)
    style.schematic_ax(ax)


def build(sink):
    import matplotlib.pyplot as plt
    import style
    fig = style.figure(10.0, 3.45)
    axes = [fig.add_axes([0.005 + 0.333 * k, 0.255, 0.32, 0.61]) for k in range(3)]

    tri_depth = [0.20, 0.20, 0.20]
    eds, areas, normals, flows = _case(TRI, tri_depth, {1: 1.3})
    u_tri, det_t, tr_t = fit(normals, areas, flows)
    _draw(axes[0], style, TRI, eds, areas, flows, u_tri, "(a) Triangle, every edge wet",
          tri_depth, f"u = ({u_tri[0]:.2f}, {u_tri[1]:.2f}) m/s. The flow is uniform at\n"
                     "(0.60, 0.25) m/s, but one edge carries 30 % more:\n"
                     "the fit cannot honor all three edges at once.")

    quad_depth = [0.30, 0.30, 0.02, 0.30]
    eds_q, areas_q, normals_q, flows_q = _case(QUAD, quad_depth, {2: 1.3})
    u_quad, det_q, tr_q = fit(normals_q, areas_q, flows_q)
    _draw(axes[1], style, QUAD, eds_q, areas_q, flows_q, u_quad, "(b) Quad with a shallow edge",
          quad_depth, f"u = ({u_quad[0]:.2f}, {u_quad[1]:.2f}) m/s. The same 30 % misfit on a\n"
                      "0.02 m edge barely moves the vector: that\nedge's weight A_e is small.")

    deg_depth = [0.0, 0.30, 0.0, 0.30]
    eds_d, areas_d, normals_d, flows_d = _case(QUAD, deg_depth, {})
    u_deg, det_d, tr_d = fit(normals_d, areas_d, flows_d)
    _draw(axes[2], style, QUAD, eds_d, areas_d, flows_d, u_deg, "(c) Two parallel wet edges",
          deg_depth, f"The wet normals are parallel: det = {det_d:.1g} ≤ 1e-10 trace²,\n"
                     "so two components cannot be fixed. A dry edge\nwith nonzero flow would "
                     "also give NaN.", hfrac=[0.2, 0.8, 0.2, 0.2])

    for ax, poly in zip(axes, (TRI, QUAD, QUAD)):        # one span for all three panels
        cx = 0.5 * (poly[:, 0].min() + poly[:, 0].max())
        cy = 0.5 * (poly[:, 1].min() + poly[:, 1].max())
        ax.set_xlim(cx - 3.3, cx + 3.3)
        ax.set_ylim(cy - 2.15, cy + 2.15)
    fig.text(0.01, 0.015, "Green arrows and numbers: edge-normal velocity Q/A (m/s, outward positive); "
             "res. is n·u − Q/A at that edge; h is the edge's flow depth (m).\n"
             "Blue arrow: the fitted cell vector u (m/s), minimizing Σ A_e (n_e·u − Q_e/A_e)² "
             "with A_e = h × edge length.", fontsize=7.4, color=style.INK, linespacing=1.4)
    print(f"    triangle u = ({u_tri[0]:.4f}, {u_tri[1]:.4f}) m/s, det/trace^2 = {det_t / tr_t ** 2:.3f}")
    print(f"    quad u = ({u_quad[0]:.4f}, {u_quad[1]:.4f}) m/s, det/trace^2 = {det_q / tr_q ** 2:.3f}")
    print(f"    parallel wet edges: det = {det_d:.3g}, trace = {tr_d:.3g}, u = {u_deg}")
    sink.save(fig, "hydraulics_ch9_lsq_cell_velocity")
