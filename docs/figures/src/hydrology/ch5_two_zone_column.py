"""The per-subcatchment two-zone groundwater model (hydrology Chapter 5).

Figure 5-1  definitional sketch: upper unsaturated zone, lower saturated
            zone, and the six fluxes of the moisture balance (also the
            Engine Manual's Figure 1-14)
Figure 5-2  the heights that enter the lateral groundwater flow equation:
            the water table H_GW, the receiving node's water surface H_SW,
            the threshold H* (default: the node invert) and the aquifer bottom

Synthetic geometry; replaces two 470–740 px EPA drawings.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()

SOIL_UNSAT = "#e7d9bf"
SOIL_SAT = "#b9cfe6"
BEDROCK = "#cfc9bd"


def _flux(ax, x, y0, y1, text, style, dx=0.02, ha="left", both=False):
    ax.annotate("", xy=(x, y1), xytext=(x, y0),
                arrowprops=dict(arrowstyle="<|-|>" if both else "-|>", color=style.WATER, lw=1.4, mutation_scale=11))
    ax.text(x + dx, (y0 + y1) / 2, text, fontsize=7.6, color=style.WATER, va="center", ha=ha, fontstyle="italic",
            bbox=dict(boxstyle="round,pad=0.15", fc=style.SURFACE, ec="none", alpha=0.9))


def _fig_5_1(style):
    import matplotlib.pyplot as plt
    from matplotlib.patches import Rectangle
    fig, ax = plt.subplots(figsize=(7.4, 3.9))
    fig.patch.set_facecolor(style.SURFACE)
    x0, x1 = 0.6, 5.4
    zb, zwt, zs = 0.0, 1.5, 3.3                       # bottom, water table, ground surface
    ax.add_patch(Rectangle((x0, zwt), x1 - x0, zs - zwt, fc=SOIL_UNSAT, ec=style.INK, lw=1.0, zorder=1))
    ax.add_patch(Rectangle((x0, zb), x1 - x0, zwt - zb, fc=SOIL_SAT, ec=style.INK, lw=1.0, zorder=1))
    ax.add_patch(Rectangle((x0, zb - 0.35), x1 - x0, 0.35, fc=BEDROCK, ec=style.INK, lw=1.0, hatch="//", zorder=1))
    rng = np.random.default_rng(5)
    ax.scatter(x0 + rng.random(70) * (x1 - x0), zwt + rng.random(70) * (zs - zwt), s=2, color="#a08a5e", alpha=0.5, zorder=2)
    ax.plot([x0, x1], [zwt, zwt], color=style.WATER, lw=1.2, ls=(0, (5, 2)), zorder=3)
    ax.text(x0 + 0.08, zwt + 0.06, "water table", fontsize=7.5, color=style.WATER, ha="left")
    # grass and the ground line
    for x in np.linspace(x0 + 0.1, x1 - 0.1, 22):
        ax.plot([x, x - 0.03, x, x + 0.03], [zs, zs + 0.14, zs, zs + 0.12], color="#6f8a4f", lw=0.9)
    ax.text(x0 - 0.08, (zwt + zs) / 2, "upper zone\nunsaturated\nmoisture content θ", fontsize=8, ha="right",
            va="center", color=style.INK)
    ax.text(x0 - 0.08, (zb + zwt) / 2, "lower zone\nsaturated\nθ = porosity φ", fontsize=8, ha="right",
            va="center", color=style.INK)
    ax.text(x0 - 0.08, zb - 0.17, "impermeable bottom", fontsize=7.5, ha="right", va="center", color=style.MUTED)
    # depths
    ax.annotate("", xy=(x1 + 0.25, zs), xytext=(x1 + 0.25, zwt), arrowprops=dict(arrowstyle="<->", color=style.MUTED, lw=0.8))
    ax.text(x1 + 0.32, (zs + zwt) / 2, "d_U", fontsize=8, color=style.MUTED, va="center")
    ax.annotate("", xy=(x1 + 0.25, zwt), xytext=(x1 + 0.25, zb), arrowprops=dict(arrowstyle="<->", color=style.MUTED, lw=0.8))
    ax.text(x1 + 0.32, (zwt + zb) / 2, "d_L", fontsize=8, color=style.MUTED, va="center")
    # fluxes
    W = dict(arrowstyle="-|>", color=style.WATER, lw=1.4, mutation_scale=11)
    T = dict(fontsize=7.6, color=style.WATER, fontstyle="italic", va="center", ha="left",
             bbox=dict(boxstyle="round,pad=0.15", fc=style.SURFACE, ec="none", alpha=0.9))
    ax.annotate("", xy=(1.0, zs + 0.02), xytext=(1.0, zs + 0.95), arrowprops=W)
    ax.text(1.08, zs + 0.92, "f_I  infiltration from the surface", **T)
    ax.annotate("", xy=(2.9, zs + 0.95), xytext=(2.9, zs + 0.02), arrowprops=W)
    ax.text(2.98, zs + 0.62, "f_EU  upper-zone ET", **T)
    ax.annotate("", xy=(4.4, zs + 0.95), xytext=(4.4, zs + 0.02), arrowprops=W)
    ax.text(4.48, zs + 0.30, "f_EL  lower-zone ET", **T)
    _flux(ax, 1.9, zwt + 0.55, zwt + 0.03, "f_U  percolation, upper to lower zone", style)
    _flux(ax, 3.9, zb + 0.4, zb - 0.02, "f_L  deep percolation (seepage) loss", style)
    ax.annotate("", xy=(x1 + 0.9, zwt / 2 + 0.2), xytext=(x1 - 0.02, zwt / 2 + 0.2),
                arrowprops=dict(arrowstyle="-|>", color=style.WATER, lw=1.4, mutation_scale=11))
    ax.text(x1 + 0.95, zwt / 2 + 0.2, "f_G  lateral groundwater\ninterflow to the\ndrainage system", fontsize=7.6,
            color=style.WATER, va="center", fontstyle="italic")
    ax.text(x0, zs + 1.05, "Two-zone groundwater model of a subcatchment  (fluxes per unit area)", fontsize=9.2,
            weight="bold", color=style.INK)
    ax.set_xlim(-1.35, 7.4)
    ax.set_ylim(zb - 0.5, zs + 1.2)
    ax.set_aspect("equal")
    ax.axis("off")
    fig.tight_layout(pad=0.2)
    return fig


def _fig_5_2(style):
    import matplotlib.pyplot as plt
    from matplotlib.patches import Rectangle, Polygon
    fig, ax = plt.subplots(figsize=(7.4, 3.7))
    fig.patch.set_facecolor(style.SURFACE)
    xa0, xa1 = 0.5, 4.2          # aquifer column
    zb, hgw, zs = 0.0, 1.9, 3.2  # bottom, water table, surface
    ax.add_patch(Rectangle((xa0, hgw), xa1 - xa0, zs - hgw, fc=SOIL_UNSAT, ec=style.INK, lw=1.0))
    ax.add_patch(Rectangle((xa0, zb), xa1 - xa0, hgw - zb, fc=SOIL_SAT, ec=style.INK, lw=1.0))
    ax.add_patch(Rectangle((xa0, zb - 0.3), 6.2, 0.3, fc=BEDROCK, ec=style.INK, lw=1.0, hatch="//"))
    # receiving channel / node to the right
    ch = Polygon([(4.2, zs), (4.9, 0.9), (6.1, 0.9), (6.7, zs)], closed=True, fc="#f4f3ef", ec=style.INK, lw=1.0)
    ax.add_patch(ch)
    hsw = 1.55
    ax.add_patch(Polygon([(4.9 - (hsw - 0.9) * 0.3, hsw), (4.9, 0.9), (6.1, 0.9), (6.1 + (hsw - 0.9) * 0.26, hsw)],
                         closed=True, fc=style.WATER, alpha=0.25, ec="none"))
    ax.plot([4.9 - (hsw - 0.9) * 0.3, 6.1 + (hsw - 0.9) * 0.26], [hsw, hsw], color=style.WATER, lw=1.2)
    ax.plot([xa0, xa1], [hgw, hgw], color=style.WATER, lw=1.2, ls=(0, (5, 2)))
    ax.text(xa1 - 0.05, hgw + 0.06, "water table", fontsize=7.5, color=style.WATER, ha="right")
    ax.text(5.45, zs + 0.12, "receiving node or channel", fontsize=7.8, ha="center", va="bottom", color=style.INK)
    # flow arrow
    ax.annotate("", xy=(5.05, 1.25), xytext=(xa1 - 0.3, 1.25),
                arrowprops=dict(arrowstyle="-|>", color=style.WATER, lw=1.6, mutation_scale=12))
    ax.text(4.35, 1.36, "Q_GW", fontsize=8.5, color=style.WATER, fontstyle="italic")
    # heights measured from the aquifer bottom
    def height(x, z, text, color):
        ax.annotate("", xy=(x, z), xytext=(x, zb), arrowprops=dict(arrowstyle="<->", color=color, lw=0.9))
        ax.text(x + 0.06, z / 2, text, fontsize=8, color=color, va="center")
    height(2.0, hgw, "H_GW  groundwater head", style.INK)
    height(7.05, hsw, "H_SW  surface water height", style.INK)
    ax.plot([4.5, 7.3], [0.9, 0.9], color=style.MUTED, lw=0.9, ls=(0, (3, 2)))
    ax.text(7.35, 1.22, "H*  threshold (dashed; default: the node invert)", fontsize=7.4, color=style.MUTED, va="bottom")
    ax.plot([0.2, 7.3], [zb, zb], color=style.MUTED, lw=0.6)
    ax.text(7.35, zb + 0.02, "aquifer bottom: the datum of every height", fontsize=7.4, color=style.MUTED, va="bottom")
    ax.text(xa0, zs + 0.95, "Heights in the lateral groundwater flow equation", fontsize=9.2, weight="bold", color=style.INK)
    ax.text(xa0, zs + 0.55, "Q_GW = A1 (H_GW − H*)^B1 − A2 (H_SW − H*)^B2 + A3 · H_GW · H_SW", fontsize=8.4, color=style.INK)
    ax.set_xlim(0.1, 10.4)
    ax.set_ylim(zb - 0.45, zs + 1.15)
    ax.set_aspect("equal")
    ax.axis("off")
    fig.tight_layout(pad=0.2)
    return fig


def build(sink):
    import style
    if "hydrology_ch5_two_zone" in sink.expected:
        sink.save(_fig_5_1(style), "hydrology_ch5_two_zone")
    if "hydrology_ch5_lateral_flow_heights" in sink.expected:
        sink.save(_fig_5_2(style), "hydrology_ch5_lateral_flow_heights")
