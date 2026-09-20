"""Drawing primitives shared by the diagram generators.

Only what two or more generators use: rounded boxes, status pills, labelled
arrows, a legend of the four statuses, and a text-width estimate for laying
pills out without a renderer. Axes are in "tenths of an inch" data units
(see ``canvas``), so estimates stay honest at any figure size.
"""
from __future__ import annotations

import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

import status as st
import style

U = 10.0                       # data units per inch
EM = 0.56                      # average glyph width, DejaVu Sans, in em

COMP_FILL = {"atm": "#eef4fb", "land": "#f2f6ec", "sub": "#f7f1e6", "conv": "#eeeeee", "out": "#e6eff7"}
COMP_EDGE = "#b9b6ad"
BOX_FILL, BOX_EDGE = "#ffffff", "#d5d2c8"
ARROW = {"water": style.WATER, "mass": "#8a3ffc", "coupling": "#eb6834", "energy": "#d6336c"}


def canvas(width_in: float, height_in: float):
    """A figure + axis whose data units are tenths of an inch."""
    fig = style.figure(width_in, height_in)
    ax = fig.add_axes([0, 0, 1, 1])
    ax.set_xlim(0, width_in * U)
    ax.set_ylim(0, height_in * U)
    style.schematic_ax(ax)
    return fig, ax


def est_width(text: str, size_pt: float) -> float:
    """Approximate rendered width in data units."""
    return len(text) * size_pt * EM / 72.0 * U


def rounded(ax, x, y, w, h, *, fc=BOX_FILL, ec=BOX_EDGE, lw=0.9, ls="-", alpha=1.0, r=0.5, z=1):
    p = FancyBboxPatch((x, y), w, h, boxstyle=f"round,pad=0,rounding_size={r}",
                       fc=fc, ec=ec, lw=lw, ls=ls, alpha=alpha, zorder=z)
    ax.add_patch(p)
    return p


def label(ax, x, y, s, *, size=9, weight="normal", color=style.INK, ha="left", va="top",
          alpha=1.0, z=3, style_=None, rotation=0):
    return ax.text(x, y, s, fontsize=size, fontweight=weight, color=color, ha=ha, va=va,
                   alpha=alpha, zorder=z, fontstyle=style_ or "normal", rotation=rotation)


def pill(ax, x, y, text, status_key, *, size=7.0, alpha=1.0, z=4):
    """One alternative as a pill; returns (width, height) in data units.

    Style is the single status vocabulary: Implemented = thin green outline,
    Experimental = amber outline and tint, Planned = grey dashed outline and
    grey text, Retired = pink outline and struck-through text.
    """
    lab, colour = st.STATUS[status_key]
    w = est_width(text, size) + 0.9
    h = size / 72.0 * U * 1.75
    ls, fc, tc, lw = "-", "#ffffff", style.INK, 0.8
    if status_key == "experimental":
        fc, lw = colour + "26", 1.1
    elif status_key == "planned":
        ls, tc = (0, (2.2, 1.6)), colour
    elif status_key == "retired":
        tc = colour
    rounded(ax, x, y, w, h, fc=fc, ec=colour, lw=lw, ls=ls, alpha=alpha, r=h / 2, z=z)
    t = ax.text(x + w / 2, y + h / 2, text, fontsize=size, color=tc, ha="center", va="center",
                alpha=alpha, zorder=z + 1)
    if status_key == "retired":
        t.set_path_effects([])
        ax.plot([x + 0.45, x + w - 0.45], [y + h / 2, y + h / 2], color=colour, lw=0.9,
                alpha=alpha, zorder=z + 2)
    return w, h


def pill_row(ax, x, y, width, alts, *, size=7.0, gap=0.45, alpha=1.0, z=4, out=None):
    """Lay pills out left-to-right, wrapping within `width`; y is the TOP.

    `alts` is a list of (text, feature_id); a feature_id of None draws a
    neutral pill (a value, not an alternative). Returns the height consumed.
    When `out` is a list, every pill's (text, feature_id, x, y, w, h) box in
    data units is appended to it, so a generator can register hotspots.
    """
    cx, cy = x, y
    row_h = size / 72.0 * U * 1.75
    used = row_h
    for text, fid in alts:
        key = st.FEATURES[fid] if fid else "implemented"
        w = est_width(text, size) + 0.9
        if cx > x and cx + w > x + width:
            cx, cy = x, cy - row_h - gap
            used += row_h + gap
        if out is not None:
            out.append((text, fid, cx, cy - row_h, w, row_h))
        if fid is None:
            rounded(ax, cx, cy - row_h, w, row_h, fc="#f4f3ef", ec="#e1e0d9", lw=0.7,
                    alpha=alpha, r=row_h / 2, z=z)
            ax.text(cx + w / 2, cy - row_h / 2, text, fontsize=size, color=style.MUTED,
                    ha="center", va="center", alpha=alpha, zorder=z + 1)
        else:
            pill(ax, cx, cy - row_h, text, key, size=size, alpha=alpha, z=z)
        cx += w + gap
    return used


def arrow(ax, p0, p1, *, kind="water", text=None, size=6.8, both=False, alpha=1.0, z=2,
          rad=0.0, text_offset=(0.0, 0.0), rotation=0, lw=1.2):
    colour = ARROW[kind]
    a = FancyArrowPatch(p0, p1, arrowstyle="<|-|>" if both else "-|>", mutation_scale=9,
                        color=colour, lw=lw, alpha=alpha, zorder=z,
                        connectionstyle=f"arc3,rad={rad}", shrinkA=0, shrinkB=0)
    ax.add_patch(a)
    if text:
        mx, my = (p0[0] + p1[0]) / 2 + text_offset[0], (p0[1] + p1[1]) / 2 + text_offset[1]
        ax.text(mx, my, text, fontsize=size, color=colour, ha="center", va="center", alpha=alpha,
                zorder=z + 1, fontstyle="italic", rotation=rotation,
                bbox=dict(boxstyle="round,pad=0.15", fc=style.SURFACE, ec="none", alpha=0.9 * alpha))
    return a


def legend(ax, x, y, *, size=7.0, title="Status of each alternative"):
    """The four statuses as pills, left-to-right, starting at (x, y-top)."""
    label(ax, x, y, title, size=size, color=style.MUTED)
    cx = x
    row_h = size / 72.0 * U * 1.75
    top = y - size / 72.0 * U * 1.6
    for key, (lab, _) in st.STATUS.items():
        w, h = pill(ax, cx, top - row_h, lab, key, size=size)
        cx += w + 0.5
    return cx - x


def close_all():
    plt.close("all")
