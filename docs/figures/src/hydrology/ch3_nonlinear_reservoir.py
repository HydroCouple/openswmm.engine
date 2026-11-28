"""The subcatchment as a nonlinear reservoir (hydrology Figure 3-2 and the
Engine Manual's Figure 1-13).

A subcatchment surface holds a depth d of water; rainfall (and snowmelt
and run-on) enter, evaporation and infiltration leave, and once d exceeds
the depression storage d_s the excess discharges as overland flow q by
Manning's equation. Synthetic geometry; replaces a 470 × 224 px sketch.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()


def _draw(style):
    import matplotlib.pyplot as plt
    from matplotlib.patches import Polygon, Rectangle
    fig, ax = plt.subplots(figsize=(7.4, 3.6))
    fig.patch.set_facecolor(style.SURFACE)
    # a sloping subcatchment slab
    x0, x1 = 0.8, 6.4
    slope = 0.09
    zs0, zs1 = 1.8, 1.8 - slope * (x1 - x0)
    slab = Polygon([(x0, zs0), (x1, zs1), (x1, zs1 - 0.55), (x0, zs0 - 0.55)], closed=True,
                   fc="#e7d9bf", ec=style.INK, lw=1.0, zorder=1)
    ax.add_patch(slab)
    rng = np.random.default_rng(2)
    xs = x0 + rng.random(60) * (x1 - x0)
    ax.scatter(xs, zs0 - slope * (xs - x0) - 0.1 - rng.random(60) * 0.4, s=2, color="#a08a5e", alpha=0.5, zorder=2)
    # ponded water: depth d above the surface, depression storage d_s marked
    d, ds = 0.42, 0.22
    water = Polygon([(x0, zs0), (x1, zs1), (x1, zs1 + d), (x0, zs0 + d)], closed=True,
                    fc=style.WATER, alpha=0.25, ec=style.WATER, lw=1.0, zorder=1.5)
    ax.add_patch(water)
    ax.plot([x0, x1], [zs0 + ds, zs1 + ds], color=style.WATER, lw=0.9, ls=(0, (4, 2)), zorder=2)
    ax.text(x1 + 0.12, zs1 - 0.02, "depression storage d_s (dashed)", fontsize=7.6, color=style.WATER, va="top")
    ax.annotate("", xy=(x0 - 0.15, zs0 + d), xytext=(x0 - 0.15, zs0), arrowprops=dict(arrowstyle="<->", color=style.INK, lw=0.9))
    ax.text(x0 - 0.22, zs0 + d / 2, "d", fontsize=9, color=style.INK, ha="right", va="center")
    W = dict(arrowstyle="-|>", color=style.WATER, lw=1.5, mutation_scale=12)
    for x in (1.6, 2.4, 3.2, 4.0, 4.8):
        z = zs0 - slope * (x - x0) + d
        ax.annotate("", xy=(x, z + 0.03), xytext=(x, z + 0.75), arrowprops=W)
    ax.text(3.2, zs0 + d + 0.85, "rainfall i  (+ snowmelt, run-on from upstream subcatchments)", fontsize=8,
            color=style.WATER, ha="center", fontstyle="italic")
    ax.annotate("", xy=(5.7, zs1 + 0.12 + d + 0.75), xytext=(5.7, zs1 + 0.12 + d + 0.03), arrowprops=W)
    ax.text(5.7, zs1 + 0.12 + d + 0.85, "evaporation e", fontsize=8, color=style.WATER, ha="center", fontstyle="italic")
    for x in (2.0, 3.6):
        z = zs0 - slope * (x - x0)
        ax.annotate("", xy=(x, z - 0.5), xytext=(x, z - 0.02), arrowprops=W)
    ax.text(4.5, zs0 - 0.62, "infiltration f", fontsize=8, color=style.WATER, ha="left", va="center", fontstyle="italic")
    # outflow over the "weir" at the downstream end
    ax.annotate("", xy=(x1 + 0.9, zs1 + d * 0.55), xytext=(x1 + 0.02, zs1 + d * 0.55),
                arrowprops=dict(arrowstyle="-|>", color=style.WATER, lw=2.0, mutation_scale=13))
    ax.text(x1 + 0.12, zs1 + d + 0.1, "runoff q = (W/n) (d − d_s)^(5/3) S^(1/2)", fontsize=8, color=style.WATER,
            fontstyle="italic")
    ax.text(x1 + 0.12, zs1 - 0.5, "outlet: a node or\nanother subcatchment", fontsize=7.4, color=style.MUTED, va="top")
    ax.annotate("", xy=(x1 + 0.2, zs1 - 0.75), xytext=(x0 - 0.2, zs0 - 0.75),
                arrowprops=dict(arrowstyle="<->", color=style.MUTED, lw=0.8))
    ax.text((x0 + x1) / 2, zs0 - 0.75 - 0.32, "flow length = area / width W;  slope S", fontsize=7.6, color=style.MUTED,
            ha="center", va="top")
    ax.text(x0 - 0.3, zs0 + d + 1.35, "The subcatchment as a nonlinear reservoir:  dd/dt = i − e − f − q",
            fontsize=9.2, weight="bold", color=style.INK)
    ax.set_xlim(0.2, 9.0)
    ax.set_ylim(zs1 - 1.3, zs0 + d + 1.55)
    ax.set_aspect("equal")
    ax.axis("off")
    fig.tight_layout(pad=0.2)
    return fig


def build(sink):
    import style
    sink.save(_draw(style), "hydrology_ch3_nonlinear_reservoir")
