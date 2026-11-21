"""What 'spatially explicit' means (hydraulics Chapter 8, §8.3): the legacy
node–link view — one momentum balance per pipe, one depth per manhole — set
against the finite-volume view, in which each conduit is cut into cells, the
water surface is resolved inside the pipe, and the flux leaving one cell is
exactly the flux entering the next. Promoted from the SWMM paper's schematic
(epaswmm5_qa/article/scripts/fig_schematic.py, 2026-09); synthetic geometry.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()


def _draw(style):
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(2, 1, figsize=(8.0, 5.2), sharex=True)
    fig.patch.set_facecolor(style.SURFACE)
    x = np.linspace(0, 100, 200)
    z = 3.0 - 0.02 * x
    crown = z + 1.0
    h = np.where(x < 55, 0.35 + 0.001 * x, 0.85 - 0.001 * (x - 55))   # a jump mid-reach
    eta = z + h
    FV, DW = style.COLORS["fv"], style.COLORS["dw"]
    for ax, title in zip(axes, ("Node–link dynamic wave: one flow per pipe, one depth per manhole",
                                "Finite volume: the pipe itself is discretised — profiles, bores and jumps live inside it")):
        style.style_ax(ax)
        ax.grid(False)
        ax.fill_between(x, 0.4, z, color=style.SOIL, zorder=0)
        ax.fill_between(x, z, crown, color=style.PIPE_FILL, zorder=1)
        ax.plot(x, z, color=style.MUTED, lw=1.2)
        ax.plot(x, crown, color=style.MUTED, lw=1.2, ls=(0, (4, 2)))
        for xm in (0, 100):
            ax.plot([xm, xm], [3.0 - 0.02 * xm, 4.6], color=style.MUTED, lw=6, solid_capstyle="butt", alpha=0.45)
        ax.set_ylim(0.4, 4.9)
        ax.set_yticks([])
        ax.set_title(title, fontsize=9.5, loc="left", color=style.INK)
    A = dict(arrowstyle="->", color=style.MUTED, lw=1.0)
    ax = axes[0]
    ax.plot([0, 100], [z[0] + 0.42, z[-1] + 0.75], color=DW, lw=2.2, ls=(0, (5, 3)))
    for xm, hh in ((0, 0.42), (100, 0.75)):
        ax.plot(xm, 3.0 - 0.02 * xm + hh, "o", color=DW, ms=9, mec=style.SURFACE, mew=1.5)
    ax.annotate("depth is known only here", xy=(0, z[0] + 0.42), xytext=(12, 4.15), fontsize=9, color=style.INK, arrowprops=A)
    ax.annotate("one momentum balance\nfor the whole pipe", xy=(50, 2.6), xytext=(40, 4.0), fontsize=9, color=style.INK, arrowprops=A)
    ax = axes[1]
    edges = np.linspace(0, 100, 11)
    for xe in edges[1:-1]:
        ze = 3.0 - 0.02 * xe
        ax.plot([xe, xe], [ze, ze + 1.0], color=style.MUTED, lw=0.8, alpha=0.6)
    ax.plot(x, eta, color=FV, lw=2.4)
    ax.annotate("hydraulic jump captured\nwhere it happens", xy=(55, z[110] + 0.78), xytext=(60, 4.1), fontsize=9,
                color=style.INK, arrowprops=A)
    ax.annotate("flux out of one cell =\nflux into the next (exactly)", xy=(edges[3], 3.0 - 0.02 * edges[3] + 0.25),
                xytext=(4, 4.1), fontsize=9, color=style.INK, arrowprops=A)
    ax.set_xlabel("distance along the conduit", color=style.MUTED, fontsize=9)
    fig.tight_layout()
    return fig


def build(sink):
    import style
    sink.save(_draw(style), "hydraulics_ch8_cell_layout")
