"""One column of the mesh aquifer and the two ways its unsaturated zone is
represented (hydrology Chapter 9, §9.2 and §9.4).

Left: closure A (CLOSED_FORM and ENSLAVED) — the unsaturated column of
thickness L = z_s − h_g is one bulk store h_u, exchanging with the
saturated zone through the quasi-steady recharge q_0. Right: closure B
(SIGMA) — the same column as m layers on sigma = z/L that stretch and
compress with the table, integrating the real Richards flux. Every flux
the ledger of §9.10 books is drawn: infiltration in from the surface,
evapotranspiration out, recharge across the table, lateral Darcy through
the sides, deep loss at the bottom, node exchange with a pipe and Dunne
return flow back to the surface. Synthetic; no model run.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()

W = 3.6          # cell width, drawing units
ZS = 5.0         # soil column thickness
HG = 1.7         # saturated thickness
M_LAYERS = 8


def _column(ax, style, x0, sigma, title):
    from matplotlib.patches import Rectangle
    P = dict(lw=1.2, ec=style.INK)
    ground, bed, table = ZS, 0.0, HG
    # saturated zone
    ax.add_patch(Rectangle((x0, bed), W, table, fc=style.WATER, alpha=0.38, **P))
    # unsaturated zone
    if not sigma:
        ax.add_patch(Rectangle((x0, table), W, ground - table, fc="#e7d9bf", **P))
        ax.add_patch(Rectangle((x0 + 0.25, table + 0.2), W - 0.5, 1.15, fc=style.WATER, alpha=0.42,
                               ec=style.WATER, lw=1.0))
        ax.text(x0 + W / 2, table + 0.78, "bulk store  h_u", fontsize=8, ha="center", va="center", color=style.INK)
        ax.text(x0 + W / 2, table + 2.35, "one state for the\nwhole column", fontsize=7.4, ha="center",
                va="center", color=style.MUTED, style="italic")
    else:
        L = ground - table
        dz = L / M_LAYERS
        for j in range(M_LAYERS):
            y = table + j * dz
            theta = 0.40 - 0.30 * (j / (M_LAYERS - 1)) ** 0.7     # wetter near the table
            ax.add_patch(Rectangle((x0, y), W, dz, fc="#e7d9bf", ec=style.MUTED, lw=0.5))
            ax.add_patch(Rectangle((x0, y), W * theta / 0.42, dz, fc=style.WATER, alpha=0.40, ec="none"))
        ax.add_patch(Rectangle((x0, table), W, L, fc="none", **P))
        ax.annotate("", xy=(x0 + W + 0.28, ground), xytext=(x0 + W + 0.28, table),
                    arrowprops=dict(arrowstyle="<->", color=style.MUTED, lw=0.8))
        ax.text(x0 + W + 0.38, (ground + table) / 2, f"m = {M_LAYERS} layers\nΔσ = 1/m, σ = z/L\nθ_j per layer",
                fontsize=7.2, va="center", color=style.MUTED)
    ax.plot([x0, x0 + W], [table, table], color=style.COLORS["fv"], lw=1.8)
    ax.plot([x0 - 0.35, x0], [ground, ground], color=style.INK, lw=1.4)
    ax.text(x0 + W / 2, ground + 1.62, title, fontsize=8.8, ha="center", weight="bold", color=style.INK)


def _draw(style):
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(8.8, 4.9))
    fig.patch.set_facecolor(style.SURFACE)
    xa, xb = 1.9, 7.6
    _column(ax, style, xa, False, "closure A — CLOSED_FORM / ENSLAVED")
    _column(ax, style, xb, True, "closure B — SIGMA")
    A = lambda col, lw=1.5: dict(arrowstyle="-|>", color=col, lw=lw, mutation_scale=12)  # noqa: E731
    WA, OR, DW = style.WATER, style.COLORS["fv-lts"], style.COLORS["dw"]
    for x0 in (xa, xb):
        # infiltration in (left), ET out (middle), Dunne return up (right)
        ax.annotate("", xy=(x0 + 0.55, ZS - 0.05), xytext=(x0 + 0.55, ZS + 0.95), arrowprops=A(WA))
        ax.annotate("", xy=(x0 + 1.8, ZS + 0.95), xytext=(x0 + 1.8, ZS - 0.05), arrowprops=A(OR))
        ax.annotate("", xy=(x0 + 3.05, ZS + 0.95), xytext=(x0 + 3.05, ZS - 0.05), arrowprops=A(DW))
        # recharge across the table
        ax.annotate("", xy=(x0 + W / 2, HG - 0.45), xytext=(x0 + W / 2, HG + 0.45), arrowprops=A(style.COLORS["fv"]))
        ax.text(x0 + W / 2 + 0.12, HG + 0.12, "q_0", fontsize=8, color=style.COLORS["fv"])
        # deep loss
        ax.annotate("", xy=(x0 + W / 2, -0.75), xytext=(x0 + W / 2, -0.05), arrowprops=A(WA, 1.3))
        # lateral Darcy
        ax.annotate("", xy=(x0 - 0.8, HG / 2), xytext=(x0 - 0.05, HG / 2), arrowprops=A(WA, 1.3))
        ax.annotate("", xy=(x0 + W + 0.8, HG / 2), xytext=(x0 + W + 0.05, HG / 2), arrowprops=A(WA, 1.3))
        for x, col, lab in ((0.55, WA, "q⁺"), (1.8, OR, "q_ET"), (3.05, DW, "Dunne")):
            ax.text(x0 + x, ZS + 1.05, lab, fontsize=7.6, ha="center", color=col)
    ax.text(xa + W / 2, -0.95, "deep loss  c_loss h_g / z_s", fontsize=7.4, ha="center", va="top", color=WA)
    ax.text(xb + W / 2, -0.95, "deep loss", fontsize=7.4, ha="center", va="top", color=WA)
    ax.text(xa - 0.9, HG / 2, "lateral Darcy\n(harmonic K)", fontsize=7.2, ha="right", va="center", color=WA)
    ax.text(-1.6, ZS + 2.45, "q⁺  infiltration delivered by the surface (Chapter 8)        q_ET  root uptake and boundary ET\n"
            "Dunne  return flow when the table reaches the ground        q_0  recharge across the table,  H = z_bed + h_g",
            fontsize=7.4, color=style.MUTED, va="top", linespacing=1.5)
    # geometry labels on the left column
    D = dict(arrowstyle="<->", color=style.INK, lw=0.9)
    ax.annotate("", xy=(xa - 2.75, 0), xytext=(xa - 2.75, ZS), arrowprops=D)
    ax.text(xa - 2.85, ZS / 2, "z_s", fontsize=9, ha="right", va="center", color=style.INK)
    ax.annotate("", xy=(xa + W + 0.3, 0), xytext=(xa + W + 0.3, HG), arrowprops=D)
    ax.text(xa + W + 0.4, HG / 2, "h_g", fontsize=9, va="center", color=style.INK)
    ax.text(xa + W + 0.4, HG + 0.7, "L = z_s − h_g", fontsize=8, va="center", color=style.INK)
    ax.text(xa - 0.45, ZS, "ground  z_c", fontsize=7.6, ha="right", va="center", color=style.INK)
    ax.text(xa - 0.45, 0.0, "bottom  z_bed", fontsize=7.6, ha="right", va="center", color=style.INK)
    ax.text(xa - 0.45, HG + 0.42, "water table  H", fontsize=7.6, ha="right", va="center",
            color=style.COLORS["fv"])
    # a pipe standing in the right column's saturated zone: the exchange of Figure 9-3
    from matplotlib.patches import Rectangle, Circle
    px = xb + W - 1.15
    ax.add_patch(Rectangle((px, 0.5), 0.8, 0.75, fc="#d9e6f5", ec=style.INK, lw=1.0))
    ax.add_patch(Rectangle((px + 0.25, 1.25), 0.3, ZS - 1.25, fc="#d9e6f5", ec=style.INK, lw=0.9))
    ax.add_patch(Circle((px + 0.4, ZS), 0.16, fc="#bfbfbf", ec=style.INK, lw=0.9))
    ax.annotate("", xy=(px - 0.05, 0.88), xytext=(px - 0.75, 0.88), arrowprops=A(DW, 1.4))
    ax.text(px + 1.0, 0.88, "node exchange\n(Figure 9-3)", fontsize=7.2, ha="left", va="center", color=DW)
    ax.text(-1.6, -1.65, "Both closures carry the same saturated state h_g and the same ledger;\nthey differ only in how the "
            "unsaturated column above it is held. AUTO picks\nbetween them from αL, the column thickness in capillary "
            "lengths (§9.4.1).", fontsize=7.6, color=style.MUTED, va="top", linespacing=1.5)
    ax.set_xlim(-1.9, 13.4)
    ax.set_ylim(-2.6, ZS + 2.6)
    ax.set_aspect("equal")
    ax.axis("off")
    fig.tight_layout(pad=0.2)
    return fig


def build(sink):
    import style
    sink.save(_draw(style), "hydrology_ch9_two_layer_column")
