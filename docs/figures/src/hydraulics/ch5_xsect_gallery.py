"""Cross-section shape galleries for hydraulics Chapter 5, drawn from the
engine's own geometry.

Every outline is the engine's width-at-depth function — `XSectionGeometry`
from the openswmm package, the same kernel the solvers call — sampled from
the invert to the crown and mirrored about the centreline, so the drawing
cannot disagree with the tables. Each tile carries an inset of the
area-versus-depth relation A(y)/A_full. Replaces the low-resolution EPA
bitmaps for Figures 5-1, 5-4, 5-5 and 5-6 and the θ definition sketch.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ("openswmm",)

# (label, shape name, geom1..geom4) — Geom1 is the full height; the rest
# follow the [XSECTIONS] conventions of the Engine Manual, Chapter 2.
ELLIPTICAL_ARCH = [
    ("Horizontal ellipse", "HORIZ_ELLIPSE", (2.0, 3.0)),
    ("Vertical ellipse", "VERT_ELLIPSE", (3.0, 2.0)),
    ("Arch", "ARCH", (2.0, 3.2)),
]
MASONRY = [
    ("Egg-shaped", "EGGSHAPED", (3.0,)),
    ("Horseshoe", "HORSESHOE", (3.0,)),
    ("Gothic", "GOTHIC", (3.0,)),
    ("Catenary", "CATENARY", (3.0,)),
    ("Semi-elliptical", "SEMIELLIPTICAL", (3.0,)),
    ("Basket-handle", "BASKETHANDLE", (3.0,)),
    ("Semi-circular", "SEMICIRCULAR", (3.0,)),
]
COMPOSITE = [
    ("Rectangular-triangular", "RECT_TRIANG", (3.0, 3.0, 1.0)),
    ("Rectangular-round", "RECT_ROUND", (3.0, 3.0, 1.5)),
    ("Modified basket-handle", "MODBASKETHANDLE", (3.0, 3.0, 1.5)),
    ("Filled circular", "FILLED_CIRCULAR", (3.0, 0.6)),
]
POWER_EXPONENTS = [0.5, 1.0, 2.0, 4.0]


def _outline(G, S, shape, geom, n=161):
    """(x_left, x_right, y) of the engine's section, plus whether it is open."""
    g = G(getattr(S, shape), *geom, units="US")
    yfull = g.full_depth
    y = np.linspace(0.0, yfull, n)
    w = g.width(y)
    return -w / 2.0, w / 2.0, y, g.is_open, g.area(y) / g.full_area


def _tile(ax, G, S, label, shape, geom, P, style, inset=True):
    xl, xr, y, is_open, a_rel = _outline(G, S, shape, geom)
    ax.fill_betweenx(y, xl, xr, color=style.PIPE_FILL, zorder=1)
    ax.plot(xl, y, color=style.INK, lw=1.3, zorder=2)
    ax.plot(xr, y, color=style.INK, lw=1.3, zorder=2)
    if not is_open and (xr[-1] - xl[-1]) > 1e-9:
        ax.plot([xl[-1], xr[-1]], [y[-1], y[-1]], color=style.INK, lw=1.3, zorder=2)
    if is_open:
        ax.plot([xl[-1] - 0.25, xl[-1]], [y[-1], y[-1]], color=style.MUTED, lw=0.8, ls=(0, (2, 2)))
        ax.plot([xr[-1], xr[-1] + 0.25], [y[-1], y[-1]], color=style.MUTED, lw=0.8, ls=(0, (2, 2)))
    # a water level at 40 % depth
    k = int(0.4 * (len(y) - 1))
    ax.fill_betweenx(y[: k + 1], xl[: k + 1], xr[: k + 1], color=style.WATER, alpha=0.22, zorder=1.5)
    ax.plot([xl[k], xr[k]], [y[k], y[k]], color=style.WATER, lw=1.0, zorder=2)
    span = max(xr.max() - xl.min(), y.max())
    # the shape occupies the left 60 % of the tile; the inset gets the right column
    ax.set_xlim(xl.min() - 0.12 * span, xl.min() + (1.9 if inset else 1.24) * span)
    ax.set_ylim(-0.08 * span, y.max() + 0.16 * span)
    ax.set_aspect("equal")
    ax.axis("off")
    ax.set_title(label, fontsize=8.5, color=style.INK, pad=8, loc="left")
    if inset:
        ia = ax.inset_axes([0.66, 0.16, 0.32, 0.46])
        ia.plot(a_rel, y / y.max(), color=style.COLORS["fv"], lw=1.1)
        ia.set_xlim(0, 1)
        ia.set_ylim(0, 1)
        ia.set_xticks([0, 1])
        ia.set_yticks([0, 1])
        ia.tick_params(labelsize=5, colors=style.MUTED, length=2, pad=1)
        ia.set_xlabel("A/A_full", fontsize=5.5, color=style.MUTED, labelpad=1)
        ia.set_ylabel("y/Y_full", fontsize=5.5, color=style.MUTED, labelpad=1)
        ia.set_facecolor(style.SURFACE)
        for s in ("top", "right"):
            ia.spines[s].set_visible(False)
        for s in ("left", "bottom"):
            ia.spines[s].set_color(style.GRIDC)


def _gallery(specs, ncol, title, G, S, P, style, width_in=9.0):
    import matplotlib.pyplot as plt
    nrow = -(-len(specs) // ncol)
    fig, axes = plt.subplots(nrow, ncol, figsize=(width_in, 1.85 * nrow + 0.6))
    fig.patch.set_facecolor(style.SURFACE)
    axes = np.atleast_1d(axes).ravel()
    for ax, (label, shape, geom) in zip(axes, specs):
        _tile(ax, G, S, label, shape, geom, P, style)
    for ax in axes[len(specs):]:
        ax.axis("off")
    fig.suptitle(title, fontsize=9.5, color=style.INK, x=0.01, ha="left", y=0.995)
    fig.text(0.99, 0.005, "outlines from the engine's width(depth); inset: area fraction versus depth fraction; "
                          "shaded: water at 40 % of full depth",
             fontsize=6.3, color=style.MUTED, ha="right", va="bottom")
    fig.tight_layout(rect=(0, 0.02, 1, 0.965))
    return fig


def _power_law(G, S, P, style):
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(1, len(POWER_EXPONENTS), figsize=(9.0, 2.9))
    fig.patch.set_facecolor(style.SURFACE)
    for ax, ex in zip(axes, POWER_EXPONENTS):
        _tile(ax, G, S, f"1/γ = {ex:g}" + ("  (parabola)" if ex == 2.0 else ""), "POWER", (3.0, 4.0, ex),
              P, style, inset=False)
        xl, xr, y, _, _ = _outline(G, S, "POWER", (3.0, 4.0, ex))
        ax.annotate("", xy=(xr[-1], y[-1] + 0.22), xytext=(xl[-1], y[-1] + 0.22),
                    arrowprops=dict(arrowstyle="<->", color=style.MUTED, lw=0.8))
        ax.text(xl[-1] + 0.3, y[-1] + 0.3, "b", fontsize=7.5, color=style.MUTED, ha="left", va="bottom")
        ax.annotate("", xy=(xr.max() + 0.35, y[-1]), xytext=(xr.max() + 0.35, 0),
                    arrowprops=dict(arrowstyle="<->", color=style.MUTED, lw=0.8))
        ax.text(xr.max() + 0.45, y[-1] / 2, "Y_full", fontsize=7.5, color=style.MUTED, va="center")
        ax.set_xlim(xl.min() - 0.5, xr.max() + 1.3)
        ax.set_ylim(-0.3, y[-1] + 1.1)
        ax.set_title(ax.get_title(), fontsize=8.5, color=style.INK, pad=2, loc="center")
    fig.suptitle("Power-law cross section  y = α x^(1/γ):  the same full depth and top width, four exponents",
                 fontsize=9.5, color=style.INK, x=0.01, ha="left")
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    return fig


def _theta(G, S, P, style):
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(4.2, 3.6))
    fig.patch.set_facecolor(style.SURFACE)
    t = np.linspace(0, 2 * np.pi, 361)
    ax.plot(np.cos(t), np.sin(t), color=style.INK, lw=1.4)
    y = -0.35                                            # water surface at 0.325 D of a unit-radius pipe
    xw = np.sqrt(1 - y * y)
    theta_half = np.arccos(-y)                           # half central angle to the free surface
    arc = np.linspace(-np.pi / 2 - theta_half, -np.pi / 2 + theta_half, 200)
    ax.fill(np.concatenate([[0], np.cos(arc), [0]]), np.concatenate([[0], np.sin(arc), [0]]),
            color=style.WATER, alpha=0.18, zorder=0)
    ax.fill_between(np.cos(arc), np.sin(arc), y, color=style.WATER, alpha=0.22, zorder=0)
    ax.plot([-xw, xw], [y, y], color=style.WATER, lw=1.2)
    ax.plot([0, np.cos(arc[0])], [0, np.sin(arc[0])], color=style.INK, lw=0.9)
    ax.plot([0, np.cos(arc[-1])], [0, np.sin(arc[-1])], color=style.INK, lw=0.9)
    ax.plot([0], [0], marker="o", ms=3, color=style.INK)
    small = np.linspace(arc[0], arc[-1], 60)
    ax.plot(0.28 * np.cos(small), 0.28 * np.sin(small), color=style.INK, lw=0.8)
    ax.text(0, -0.42, "θ", fontsize=11, ha="center", va="top", color=style.INK)
    ax.annotate("", xy=(1.18, y), xytext=(1.18, -1.0), arrowprops=dict(arrowstyle="<->", color=style.MUTED, lw=0.8))
    ax.text(1.24, (y - 1.0) / 2, "y", fontsize=8, color=style.MUTED, va="center")
    ax.annotate("", xy=(-1.18, 1.0), xytext=(-1.18, -1.0), arrowprops=dict(arrowstyle="<->", color=style.MUTED, lw=0.8))
    ax.text(-1.24, 0, "D", fontsize=8, color=style.MUTED, va="center", ha="right")
    ax.text(0, 1.12, "A = (θ − sin θ) D² / 8,   P = θ D / 2,   T = D sin(θ/2)", fontsize=7.2,
            ha="center", va="bottom", color=style.INK)
    ax.set_xlim(-1.7, 1.7)
    ax.set_ylim(-1.2, 1.35)
    ax.set_aspect("equal")
    ax.axis("off")
    fig.tight_layout()
    return fig


def build(sink):
    import primitives as P
    import style
    from openswmm import engine as e
    G, S = e.XSectionGeometry, e.XSectShape
    sink.save(_gallery(ELLIPTICAL_ARCH, 3, "Elliptical and arch pipe sections", G, S, P, style),
              "hydraulics_ch5_shapes_elliptical_arch")
    sink.save(_gallery(MASONRY, 4, "Older masonry sewer sections", G, S, P, style),
              "hydraulics_ch5_shapes_masonry")
    sink.save(_gallery(COMPOSITE, 4, "Composite sections: rectangular, triangular and circular parts", G, S, P, style),
              "hydraulics_ch5_shapes_composite")
    sink.save(_power_law(G, S, P, style), "hydraulics_ch5_power_law")
    sink.save(_theta(G, S, P, style), "hydraulics_ch5_circular_theta")
