"""Hydrostatic (Audusse) reconstruction at a wet/dry face (hydraulics
Figure 8-3, §8.5.1–8.5.2).

At a face between cells with beds z_L < z_R the reconstruction uses the
higher bed z* = max(z_L, z_R) and the face depths h*_L = max(0, η_L − z*),
h*_R = max(0, η_R − z*). Left: the left water surface stands above z*, so
the face is wet on the left and the front advances onto the dry cell.
Right: the left surface is below z*, both face depths are zero, and the
bank acts as a wall with exactly zero flux — the still-water property.
Replaces the watermarked placeholder.
"""
from __future__ import annotations

REQUIRES = ()


def _panel(ax, style, eta_l, title):
    from matplotlib.patches import FancyArrowPatch
    z_l, z_r = 0.5, 1.4
    ax.fill_between([0, 4], -0.2, z_l, color=style.SOIL, zorder=0)
    ax.fill_between([4, 8], -0.2, z_r, color=style.SOIL, zorder=0)
    ax.plot([0, 4], [z_l, z_l], color=style.INK, lw=1.8)
    ax.plot([4, 8], [z_r, z_r], color=style.INK, lw=1.8)
    ax.plot([4, 4], [z_l, z_r], color=style.INK, lw=1.8)
    ax.plot([2.6, 5.4], [z_r, z_r], color=style.MUTED, lw=1.0, ls=(0, (4, 2)))
    ax.text(5.5, z_r - 0.06, "z* = max(z_L, z_R)", fontsize=8.5, va="top", color=style.MUTED)
    ax.fill_between([0, 4], z_l, eta_l, color=style.WATER, alpha=0.28)
    ax.plot([0, 4], [eta_l, eta_l], color=style.WATER, lw=2.0)
    ax.text(0.25, eta_l + 0.07, "η_L", fontsize=10, color=style.WATER)
    ax.text(0.25, z_l - 0.2, "z_L", fontsize=9, color=style.INK, va="top")
    ax.text(7.4, z_r - 0.2, "z_R", fontsize=9, color=style.INK, va="top")
    h_star = max(0.0, eta_l - z_r)
    if h_star > 0:
        ax.annotate("", xy=(4.35, z_r + h_star), xytext=(4.35, z_r),
                    arrowprops=dict(arrowstyle="<->", color=style.WATER, lw=1.0))
        ax.text(4.5, z_r + h_star / 2, "h*_L = η_L − z*", fontsize=9, color=style.WATER, va="center")
        ax.add_patch(FancyArrowPatch((3.2, eta_l + 0.18), (5.2, eta_l + 0.18), arrowstyle="-|>",
                                     mutation_scale=14, color=style.WATER, lw=1.6))
        ax.text(4.2, eta_l + 0.32, "front advances onto the dry cell", ha="center", fontsize=8.5, color=style.WATER)
        ax.text(6.4, z_r + 0.12, "h*_R = 0 (dry)", fontsize=9, color=style.INK)
    else:
        ax.text(4.0, z_r + 0.45, "h*_L = h*_R = 0\nzero flux: the bank is a wall", ha="center", fontsize=9,
                color=style.INK)
        ax.text(6.4, z_r + 0.12, "dry", fontsize=9, color=style.INK)
    ax.text(2.0, -0.05, "cell L", ha="center", fontsize=8.5, color=style.MUTED, va="top")
    ax.text(6.0, -0.05, "cell R", ha="center", fontsize=8.5, color=style.MUTED, va="top")
    ax.set_xlim(0, 8)
    ax.set_ylim(-0.45, 2.7)
    ax.set_title(title, fontsize=9.5, color=style.INK, loc="left")
    ax.axis("off")


def _draw(style):
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(1, 2, figsize=(9.0, 3.8), sharey=True)
    fig.patch.set_facecolor(style.SURFACE)
    _panel(axes[0], style, 1.9, "Wetting front: η_L above z*")
    _panel(axes[1], style, 1.15, "Emerged bank: η_L below z*")
    fig.tight_layout()
    return fig


def build(sink):
    import style
    sink.save(_draw(style), "hydraulics_ch8_hydrostatic_reconstruction")
