"""Street cross-sections (hydraulics Figures 5-10 and 7-11, and the Engine
Manual's Figure 1-9).

Figure 5-10  a one-sided street: curb, depressed gutter, road with a cross
             slope up to the crown, and an optional backing (sidewalk or
             shoulder) — the dimensions of the [STREETS] section
Figure 7-11  the same street divided into gutter flow and roadway flow,
             with the spread T and the flow depth at the curb that the
             HEC-22 inlet equations use

Drawn from the [STREETS] parameters; replaces two 500 px sketches.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()

# [STREETS] dimensions (ft): curb height, road width (curb to crown), cross slope,
# gutter width, gutter depression, backing width, backing slope
HC, WR, SX, WG, AG, WB, SB = 0.5, 20.0, 0.02, 2.0, 0.25, 4.0, 0.04   # depression exaggerated: not to scale


def _profile():
    """(x, z) of the one-sided street from the curb face to the crown, curb top at z = 0."""
    x = [0.0, 0.0, WG, WR]
    z_curb_bottom = -HC
    z_gutter_edge = z_curb_bottom + AG + WG * SX          # depressed gutter meets the road cross slope
    z = [0.0, z_curb_bottom, z_gutter_edge, z_gutter_edge + (WR - WG) * SX]
    return np.array(x), np.array(z)


def _base(ax, style, water_depth=None):
    from matplotlib.patches import Polygon
    x, z = _profile()
    # backing behind the curb
    ax.plot([-WB, 0.0], [WB * SB, 0.0], color=style.INK, lw=1.4)
    ax.fill_between([-WB, 0.0], [WB * SB, 0.0], -1.4, color="#e6e6e6", zorder=0)
    # road surface and its fill
    ax.plot(x, z, color=style.INK, lw=1.6)
    ax.fill_between(x[1:], z[1:], -1.4, color="#d5d5d5", zorder=0)
    ax.plot([x[-1], x[-1] + 1.5], [z[-1], z[-1]], color=style.INK, lw=1.6)
    ax.plot([x[-1], x[-1]], [z[-1] - 0.3, z[-1]], color=style.MUTED, lw=0.8, ls=(0, (3, 2)))
    ax.text(x[-1], z[-1] - 0.34, "crown", fontsize=7.5, color=style.MUTED, ha="center", va="top")
    if water_depth is not None:
        zw = -HC + water_depth
        xs = np.linspace(0, WR, 400)
        zs = np.interp(xs, x[1:], z[1:])
        m = zs <= zw
        ax.fill_between(xs[m], zs[m], zw, color=style.WATER, alpha=0.3, zorder=1)
        ax.plot([0, xs[m].max()], [zw, zw], color=style.WATER, lw=1.2)
        return xs[m].max()
    return None


def _fig_5_10(style):
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(8.4, 3.0))
    fig.patch.set_facecolor(style.SURFACE)
    _base(ax, style)
    x, z = _profile()
    D = dict(arrowstyle="<->", color=style.COLORS["fv"], lw=0.9)
    L = dict(fontsize=7.6, color=style.COLORS["fv"])
    ax.annotate("", xy=(-0.5, 0), xytext=(-0.5, -HC), arrowprops=D)
    ax.text(-0.6, -HC / 2, "curb height", ha="right", va="center", **L)
    ax.annotate("", xy=(0, -HC - 0.22), xytext=(WG, -HC - 0.22), arrowprops=D)
    ax.text(WG / 2, -HC - 0.30, "gutter width", ha="center", va="top", **L)
    ax.annotate("", xy=(WG + 0.3, z[2]), xytext=(WG + 0.3, z[2] - AG), arrowprops=D)
    ax.text(WG + 0.5, z[2] - AG / 2, "gutter depression (exaggerated)", va="center", **L)
    ax.annotate("", xy=(0, 0.42), xytext=(WR, 0.42), arrowprops=D)
    ax.text(WR / 2, 0.47, "road width (curb to crown)", ha="center", **L)
    xm = WG + (WR - WG) * 0.55
    zm = np.interp(xm, x[1:], z[1:])
    ax.plot([xm, xm + 4], [zm, zm], color=style.COLORS["fv"], lw=0.8, ls=(0, (3, 2)))
    ax.text(xm + 1.0, zm + 0.10, "cross slope S_x", va="bottom", **L)
    ax.annotate("", xy=(-WB, WB * SB + 0.3), xytext=(0, WB * SB + 0.3), arrowprops=D)
    ax.text(-WB / 2, WB * SB + 0.35, "backing width, slope", ha="center", **L)
    ax.text(-WB, -1.32, "one-sided street, not to scale: a two-sided street repeats the section mirror-wise about the crown",
            fontsize=7.4, color=style.MUTED)
    ax.set_xlim(-WB - 3.5, WR + 2)
    ax.set_ylim(-1.5, 1.0)
    ax.set_aspect(4.0)
    ax.axis("off")
    fig.tight_layout(pad=0.2)
    return fig


def _fig_7_11(style):
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(8.4, 3.0))
    fig.patch.set_facecolor(style.SURFACE)
    depth = 0.46
    spread = _base(ax, style, water_depth=depth)
    zw = -HC + depth
    D = dict(arrowstyle="<->", color=style.COLORS["fv"], lw=0.9)
    L = dict(fontsize=7.6, color=style.COLORS["fv"])
    ax.annotate("", xy=(0, zw + 0.3), xytext=(spread, zw + 0.3), arrowprops=D)
    ax.text(spread / 2, zw + 0.35, "spread T", ha="center", **L)
    ax.annotate("", xy=(-0.5, zw), xytext=(-0.5, -HC), arrowprops=D)
    ax.text(-0.6, (zw - HC) / 2, "depth at curb d", ha="right", va="center", **L)
    ax.plot([WG, WG], [zw - 0.55, zw + 0.05], color=style.MUTED, lw=0.8, ls=(0, (3, 2)))
    ax.text(WG / 2 - 0.2, -HC - 0.30, "gutter flow Q_w", ha="right", va="top", fontsize=7.6, color=style.WATER)
    ax.text(WG + 0.4, -HC - 0.30, "roadway (side) flow Q_s", ha="left", va="top", fontsize=7.6, color=style.WATER)
    ax.text(-WB, -1.32, "HEC-22: E_0 = Q_w / Q is the fraction of the flow in the gutter; capture depends on d, T and E_0",
            fontsize=7.4, color=style.MUTED)
    ax.set_xlim(-WB - 3.5, WR + 2)
    ax.set_ylim(-1.5, 1.0)
    ax.set_aspect(4.0)
    ax.axis("off")
    fig.tight_layout(pad=0.2)
    return fig


def build(sink):
    import style
    if "hydraulics_ch5_street_section" in sink.expected:
        sink.save(_fig_5_10(style), "hydraulics_ch5_street_section")
    if "hydraulics_ch7_street_spread" in sink.expected:
        sink.save(_fig_7_11(style), "hydraulics_ch7_street_spread")
