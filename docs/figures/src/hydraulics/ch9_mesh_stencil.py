"""The mixed triangle-quadrilateral stencil (hydraulics Chapter 9, §9.3
and §9.3.1).

Left: a quadrilateral and a triangle sharing one face. Local edge k of a
cell runs between vertices (k+1) mod nv and (k+2) mod nv, so the same rule
numbers both shapes and local edge k is opposite vertex k on a triangle.
The centroid-to-edge normal distance d_e of (9-33) is drawn on both sides
of the shared face; the face's own separation is the sum. Right: the
padded slot layout. Every cell owns four slots whatever its vertex count,
so slot 4t + k addresses cell t's edge k with no indirection, and a
triangle's fourth slot is simply never visited. Synthetic; no model run.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()

QUAD = np.array([[0.0, 0.0], [2.9, -0.35], [3.25, 2.5], [0.3, 2.85]])
TRI = np.array([[2.9, -0.35], [5.9, 1.05], [3.25, 2.5]])


def _centroid(p):
    return p.mean(axis=0)


def _edges(p):
    nv = len(p)
    return [(p[(k + 1) % nv], p[(k + 2) % nv]) for k in range(nv)]


def _cell(ax, style, poly, name, tag, fc, label_colour):
    from matplotlib.patches import Polygon
    ax.add_patch(Polygon(poly, closed=True, fc=fc, ec=style.INK, lw=1.4, zorder=2, alpha=0.5))
    c = _centroid(poly)
    ax.plot(*c, marker="o", ms=5, color=style.INK, zorder=5)
    ax.text(c[0], c[1] + 0.3, name, fontsize=8.6, ha="center", weight="bold", color=style.INK, zorder=6)
    for k, (a, b) in enumerate(_edges(poly)):
        m = (a + b) / 2
        out = m - c
        out /= max(np.linalg.norm(out), 1e-9)
        d = 0.75 if np.allclose(m, (QUAD[1] + QUAD[2]) / 2) else 0.32
        ax.text(m[0] + out[0] * d, m[1] + out[1] * d, f"k={k}", fontsize=7.4, ha="center", va="center",
                color=label_colour, zorder=6)
    for i, v in enumerate(poly):
        ax.plot(*v, marker="o", ms=4.2, mfc="white", mec=style.INK, mew=1.0, zorder=6)
        vout = v - c
        vout /= max(np.linalg.norm(vout), 1e-9)
        ax.text(v[0] + vout[0] * 0.38, v[1] + vout[1] * 0.38, f"{tag}:v{i}", fontsize=6.8, ha="center",
                va="center", color=label_colour, zorder=6)
    return c


def _geometry(ax, style):
    FV, OR = style.COLORS["fv"], style.COLORS["fv-lts"]
    cq = _cell(ax, style, QUAD, "quad  L", "L", "#cfe0f5", FV)
    ct = _cell(ax, style, TRI, "triangle  R", "R", "#d8e9d4", "#4a7a42")
    # the shared face: quad edge k=1 (v2,v3)? compute the common pair
    a, b = QUAD[1], QUAD[2]
    ax.plot([a[0], b[0]], [a[1], b[1]], color=OR, lw=3.4, zorder=4, solid_capstyle="round")
    m = (a + b) / 2
    e = b - a
    n = np.array([e[1], -e[0]])
    n /= np.linalg.norm(n)
    for c, col, dy in ((cq, FV, -0.3), (ct, "#4a7a42", 0.3)):
        ax.annotate("", xy=(m[0], m[1]), xytext=(c[0], c[1]),
                    arrowprops=dict(arrowstyle="-|>", color=col, lw=1.2, mutation_scale=10, ls=(0, (3, 2))))
        mid = (c + m) / 2
        ax.text(mid[0], mid[1] + dy, "d_e", fontsize=8, color=col, ha="center", va="center", zorder=7)
    ax.annotate("", xy=(m[0] + n[0] * 0.95, m[1] + n[1] * 0.95 - 0.55), xytext=(m[0], m[1] - 0.55),
                arrowprops=dict(arrowstyle="-|>", color=OR, lw=1.6, mutation_scale=12))
    ax.text(m[0] + n[0] * 1.12, m[1] + n[1] * 1.12 - 0.55, "n̂", fontsize=9, color=OR, ha="center", va="center")
    ax.text(-1.5, -0.25, "shared face: length ξ, outward normal n̂ of L,\nseparation d_n = d_e(L) + d_e(R)",
            fontsize=7.4, ha="left", va="top", color=OR, linespacing=1.5)
    ax.text(-1.5, 3.95, "local edge k runs between vertices (k+1) mod nv and (k+2) mod nv — one rule for both\n"
            "shapes, so on a triangle edge k is opposite vertex k. Each cell numbers its own vertices.",
            fontsize=7.6, color=style.INK, linespacing=1.45, va="top")
    ax.set_xlim(-1.6, 6.9)
    ax.set_ylim(-1.3, 4.1)
    ax.set_aspect("equal", adjustable="datalim")
    ax.axis("off")
    ax.set_title("(a) A quadrilateral and a triangle sharing a face", fontsize=9, loc="left")


def _slots(ax, style):
    from matplotlib.patches import Rectangle
    FV = style.COLORS["fv"]
    cells = [("cell 0\nquad", 4), ("cell 1\ntriangle", 3), ("cell 2\ntriangle", 3), ("cell 3\nquad", 4)]
    w, h = 1.15, 0.62
    for t, (name, nv) in enumerate(cells):
        x = t * 4 * w
        ax.text(x + 2 * w, 1.35, name, fontsize=7.4, ha="center", va="center", color=style.INK, linespacing=1.3)
        for k in range(4):
            used = k < nv
            ax.add_patch(Rectangle((x + k * w, 0), w - 0.08, h, fc=FV if used else "#e4e3de",
                                   ec=style.SURFACE, lw=1.4, alpha=0.85 if used else 1.0))
            ax.text(x + k * w + (w - 0.08) / 2, h / 2, f"{4 * t + k}", fontsize=7.6, ha="center", va="center",
                    color="#ffffff" if used else style.MUTED)
            ax.text(x + k * w + (w - 0.08) / 2, -0.22, f"k={k}", fontsize=6.4, ha="center", va="top",
                    color=style.MUTED if used else "#d5d4cf")
        ax.plot([x - 0.04, x + 4 * w - 0.08], [-0.52, -0.52], color=style.MUTED, lw=0.8)
    ax.text(0, 2.35, "slot  4t + k  addresses cell t's edge k directly: one stride, no per-cell offset table",
            fontsize=7.6, color=style.INK)
    ax.text(0, 2.0, "a triangle's fourth slot is allocated and never visited — the padding a fixed stride costs",
            fontsize=7.2, color=style.MUTED)
    ax.text(0, -0.95, "edge arrays (neighbour, length, normal, conveyance, momentum) all share this layout, "
            "so one index reaches every one of them", fontsize=7.2, color=style.MUTED)
    ax.set_xlim(-0.3, 4 * 4 * w + 0.3)
    ax.set_ylim(-1.35, 2.6)
    ax.axis("off")
    ax.set_title("(b) The padded slot layout", fontsize=9, loc="left")


def _draw(style):
    import matplotlib.pyplot as plt
    fig, (a0, a1) = plt.subplots(2, 1, figsize=(8.4, 5.4), gridspec_kw=dict(height_ratios=[1.2, 1.0]))
    fig.patch.set_facecolor(style.SURFACE)
    _geometry(a0, style)
    _slots(a1, style)
    fig.tight_layout(pad=0.35)
    return fig


def build(sink):
    import style
    sink.save(_draw(style), "hydraulics_ch9_mesh_stencil")
