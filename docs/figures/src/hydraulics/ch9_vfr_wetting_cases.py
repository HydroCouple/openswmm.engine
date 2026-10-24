"""Wetting cases of a planar-bed triangular cell and the wetted-edge face
gate (hydraulics Figure 9-3, §9.4 and §9.5.9).

The VFR closure treats each cell as a planar bed through its three vertex
elevations z_1 ≤ z_2 ≤ z_3. A free surface η wets a triangle (η below
z_2), a quadrilateral (z_2 < η < z_3) or the whole cell. The fourth panel
shows the face gate: a shared edge sloping from z_lo to z_hi is blocked,
partially wet or submerged depending on η. Replaces the watermarked
placeholder (which was also over the 1600 px width bound).
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()


def _draw(style):
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(1, 4, figsize=(9.6, 3.0), width_ratios=[1, 1, 1, 1.25])
    fig.patch.set_facecolor(style.SURFACE)
    tri = np.array([[0.5, 0.5], [3.5, 1.0], [1.8, 3.4], [0.5, 0.5]])
    verts = [(0.5, 0.5, "z_1"), (3.5, 1.0, "z_2"), (1.8, 3.4, "z_3")]
    cases = [
        ("η below z_2: a wet triangle", np.array([[0.5, 0.5], [1.9, 0.75], [1.05, 1.75], [0.5, 0.5]])),
        ("z_2 < η < z_3: a wet quadrilateral", np.array([[0.5, 0.5], [3.5, 1.0], [2.6, 2.3], [1.15, 2.1], [0.5, 0.5]])),
        ("η above z_3: fully wet", tri),
    ]
    for ax, (title, poly) in zip(axes[:3], cases):
        ax.fill(tri[:, 0], tri[:, 1], color=style.SOIL, alpha=0.6, zorder=0)
        ax.plot(tri[:, 0], tri[:, 1], color=style.INK, lw=1.6)
        ax.fill(poly[:, 0], poly[:, 1], color=style.WATER, alpha=0.4)
        for x, y, lab in verts:
            ax.plot(x, y, marker="o", ms=4, color=style.INK)
            ax.text(x + 0.1, y + 0.08, lab, fontsize=9, color=style.INK)
        ax.set_xlim(0, 4.1)
        ax.set_ylim(0, 3.9)
        ax.set_title(title, fontsize=8.4, color=style.INK)
        ax.set_aspect("equal")
        ax.axis("off")
    ax = axes[3]
    z_lo, z_hi = 0.8, 2.2
    ax.fill_between([0, 4], -0.2, [z_lo, z_hi], color=style.SOIL, zorder=0)
    ax.plot([0, 4], [z_lo, z_hi], color=style.INK, lw=1.6)
    ax.text(0.05, z_lo + 0.1, "z_lo", fontsize=9, color=style.INK, va="bottom")
    ax.text(3.55, z_hi + 0.1, "z_hi", fontsize=9, color=style.INK)
    for eta, lab in ((0.55, "blocked"), (1.5, "partially wet"), (2.6, "submerged")):
        ax.plot([0, 4], [eta, eta], color=style.WATER, lw=1.2, ls=(0, (4, 2)))
        ax.text(4.08, eta, lab, fontsize=8, va="center", color=style.WATER)
    xw = np.linspace(0, 2.0, 50)
    ax.fill_between(xw, z_lo + (z_hi - z_lo) * xw / 4, 1.5, color=style.WATER, alpha=0.3)
    ax.set_xlim(0, 5.6)
    ax.set_ylim(-0.2, 3.2)
    ax.set_title("the wetted-edge face gate", fontsize=8.4, color=style.INK)
    ax.axis("off")
    fig.tight_layout()
    return fig


def build(sink):
    import style
    sink.save(_draw(style), "hydraulics_ch9_vfr_wetting_cases")
