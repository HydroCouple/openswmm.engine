"""The water balance of one mesh cell (hydrology Chapter 8, §8.1 and
§8.3.4).

Every term of the cell volume update (8-6) drawn on one cell in section:
interpolated rainfall in, evaporation and infiltration out through their
shared dry-depth ramp, the held coupling flux with the 1D network, the
aquifer's return from Chapter 9, and the face fluxes exchanged with the
neighbouring cells. Inset: the cubic Hermite ramp both sinks pass through
below DRY_DEPTH, which is what lets a drying cell shut them off together.
Synthetic; no model run.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()


def _draw(style):
    import matplotlib.pyplot as plt
    from matplotlib.patches import Polygon, Rectangle, Circle
    fig, ax = plt.subplots(figsize=(8.8, 4.6))
    fig.patch.set_facecolor(style.SURFACE)
    INK, WA, OR, DW, FV = style.INK, style.WATER, style.COLORS["fv-lts"], style.COLORS["dw"], style.COLORS["fv"]
    # terrain: the cell in the middle, neighbours either side
    xs = [0.0, 3.0, 7.0, 10.0]
    zb = [2.25, 1.85, 1.55, 1.2]
    ax.add_patch(Polygon([(xs[0], zb[0]), (xs[3], zb[3]), (xs[3], -0.9), (xs[0], -0.9)], closed=True,
                         fc=style.SOIL, ec="none", zorder=0))
    ax.plot(xs, zb, color=INK, lw=1.4, zorder=3)
    # water in the three cells
    h = [0.55, 0.85, 0.5]
    for k in range(3):
        x0, x1 = xs[k], xs[k + 1]
        z0, z1 = zb[k], zb[k + 1]
        top = max(z0, z1) + h[k]
        ax.add_patch(Polygon([(x0, z0), (x1, z1), (x1, top), (x0, top)], closed=True,
                             fc=WA, alpha=0.32 if k == 1 else 0.20, ec=WA, lw=1.1, zorder=2))
        ax.plot([x0, x0], [z0 - 0.5, top + 0.15], color=style.MUTED, lw=0.9, ls=(0, (3, 2)), zorder=1)
    ax.plot([xs[3], xs[3]], [zb[3] - 0.5, zb[3] + h[2] + 0.15], color=style.MUTED, lw=0.9, ls=(0, (3, 2)), zorder=1)
    ax.text(5.0, 2.05, "cell i", fontsize=9.5, ha="center", weight="bold", color=INK, zorder=5)
    ax.text(5.0, 1.82, "depth h_i, area A_i, volume V_i", fontsize=7.4, ha="center", color=INK, zorder=5)
    ax.text(1.5, 1.6, "neighbour", fontsize=7.6, ha="center", color=style.MUTED)
    ax.text(8.9, 1.05, "neighbour", fontsize=7.6, ha="center", color=style.MUTED)
    A = lambda col, lw=1.7: dict(arrowstyle="-|>", color=col, lw=lw, mutation_scale=13)  # noqa: E731
    # rainfall in
    for x in (4.2, 5.0, 5.8):
        ax.annotate("", xy=(x, 2.62), xytext=(x, 3.55), arrowprops=A(WA, 1.4))
    ax.text(3.95, 3.66, "r_i   interpolated rainfall (§8.2)", fontsize=7.8, color=WA)
    # evaporation out
    ax.annotate("", xy=(6.6, 3.55), xytext=(6.6, 2.55), arrowprops=A(OR))
    ax.text(6.72, 3.2, "e*_i  evaporation,\nramped (§8.5)", fontsize=7.8, color=OR)
    # infiltration out
    ax.annotate("", xy=(4.6, 0.55), xytext=(4.6, 1.62), arrowprops=A(OR))
    ax.text(4.45, 0.42, "f*_i  infiltration, ramped (§8.3)\n→ lost · subcatchment aquifer\n     · mesh aquifer (§8.4)",
            fontsize=7.8, ha="left", va="top", color=OR)
    # aquifer return up
    ax.annotate("", xy=(3.55, 1.72), xytext=(3.55, 0.75), arrowprops=A(DW, 1.5))
    ax.text(3.45, 0.62, "g_i  aquifer return\n(Chapter 9)", fontsize=7.6, ha="right", va="top", color=DW)
    # coupling with a node
    ax.add_patch(Rectangle((6.35, -0.62), 0.9, 0.62, fc="#d9e6f5", ec=INK, lw=1.0, zorder=2))
    ax.add_patch(Rectangle((6.65, 0.0), 0.3, 1.45, fc="#d9e6f5", ec=INK, lw=0.9, zorder=2))
    ax.add_patch(Circle((6.8, 1.45), 0.13, fc="#bfbfbf", ec=INK, lw=0.9, zorder=4))
    ax.annotate("", xy=(6.8, 1.62), xytext=(6.8, 0.95),
                arrowprops=dict(arrowstyle="<|-|>", color=DW, lw=1.6, mutation_scale=11))
    ax.text(7.35, 0.75, "s_i  held coupling flux\nwith the 1D network", fontsize=7.6, color=DW)
    # face fluxes
    ax.annotate("", xy=(3.3, 2.05), xytext=(2.6, 2.05), arrowprops=A(FV, 1.6))
    ax.annotate("", xy=(7.4, 1.75), xytext=(6.7, 1.75), arrowprops=A(FV, 1.6))
    ax.text(2.55, 2.22, "ΣΔV  face fluxes to and from the neighbours (Hydraulics Ch. 9)", fontsize=7.6, color=FV)
    # the update equation
    ax.text(-0.15, -1.35, "V_i ← max( 0,  V_i + Δt A_i ( r_i + s_i + g_i − e*_i − f*_i ) + Σ_faces ΔV )",
            fontsize=9, color=INK)
    ax.text(-0.15, -1.75, "the same terms the 2D Surface Routing Continuity block of the report prints",
            fontsize=7.4, color=style.MUTED)
    # inset: the shared ramp
    ins = ax.inset_axes([0.775, 0.07, 0.205, 0.26])
    t = np.linspace(0, 1.6, 300)
    ramp = np.where(t >= 1.0, 1.0, t ** 2 * (3 - 2 * t))
    ins.plot(t, ramp, color=OR, lw=1.6)
    ins.axvline(1.0, color=style.MUTED, lw=0.8, ls=(0, (2, 2)))
    ins.set_xticks([0, 1.0])
    ins.set_xticklabels(["0", "DRY_DEPTH"], fontsize=6.4)
    ins.set_yticks([0, 1])
    ins.tick_params(labelsize=6.4)
    ins.set_xlabel("cell depth h_i", fontsize=6.6, labelpad=1)
    ins.set_title("both sinks share this ramp", fontsize=6.8, color=style.MUTED)
    ins.set_facecolor(style.SURFACE)
    ax.set_xlim(-0.4, 10.6)
    ax.set_ylim(-2.0, 4.1)
    ax.axis("off")
    fig.tight_layout(pad=0.2)
    return fig


def build(sink):
    import style
    sink.save(_draw(style), "hydrology_ch8_cell_budget")
