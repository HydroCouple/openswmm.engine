"""The four standard storage-unit solids of hydraulics Table 5-14, whose
depth–area relation the engine evaluates as A(Y) = a0 + a1 Y + a2 Y²:
elliptical cylinder, elliptical paraboloid, elliptical cone and rectangular
pyramid. Drawn as shaded surfaces with the dimensions the table names
(major and minor axis or length and width at the top, depth). One tile per
manifest row so each sits in its table cell; replaces 67–93 px icons.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()

SHAPES = ["hydraulics_ch5_storage_cylinder", "hydraulics_ch5_storage_paraboloid",
          "hydraulics_ch5_storage_cone", "hydraulics_ch5_storage_pyramid"]


def _surface(kind, n=40):
    """(X, Y, Z) of the solid's side wall, depth Z from 0 (bottom) to 1 (top)."""
    z = np.linspace(0.0, 1.0, n)
    if kind == "pyramid":
        # rectangular pyramid: half-widths grow linearly from a small base to the top
        u = np.linspace(0, 4, 4 * n + 1)               # perimeter parameter, 4 sides
        side = np.floor(u).astype(int) % 4
        f = u - np.floor(u)
        px = np.select([side == 0, side == 1, side == 2, side == 3], [f * 2 - 1, 1.0, 1 - f * 2, -1.0])
        py = np.select([side == 0, side == 1, side == 2, side == 3], [-1.0, f * 2 - 1, 1.0, 1 - f * 2])
        Z, U = np.meshgrid(z, u, indexing="ij")
        r = 0.15 + 0.85 * Z
        X = r * px[np.newaxis, :] * 1.3
        Y = r * py[np.newaxis, :] * 0.9
        return X, Y, Z
    t = np.linspace(0, 2 * np.pi, 2 * n)
    Z, T = np.meshgrid(z, t, indexing="ij")
    if kind == "cylinder":
        r = np.ones_like(Z)
    elif kind == "paraboloid":
        r = np.sqrt(Z)                                  # A ∝ Y
    else:                                               # cone: half-widths grow linearly
        r = 0.1 + 0.9 * Z
    return r * np.cos(T) * 1.3, r * np.sin(T) * 0.9, Z


def _tile(kind, title, style):
    import matplotlib.pyplot as plt
    from matplotlib.colors import LightSource
    fig = plt.figure(figsize=(2.9, 2.2))
    fig.patch.set_facecolor(style.SURFACE)
    ax = fig.add_subplot(111, projection="3d")
    X, Y, Z = _surface(kind)
    ls = LightSource(azdeg=225, altdeg=40)
    rgb = ls.shade(Z, cmap=plt.cm.Blues, vert_exag=0.3, blend_mode="soft")
    ax.plot_surface(X, Y, Z, facecolors=rgb, rstride=1, cstride=1, linewidth=0, antialiased=True, alpha=0.95)
    # the water-surface outline at the top
    ax.plot(X[-1], Y[-1], Z[-1], color=style.INK, lw=0.9)
    ax.set_box_aspect((1.3, 0.9, 0.85))
    ax.view_init(elev=22, azim=-58)
    ax.set_axis_off()
    ax.set_facecolor(style.SURFACE)
    fig.text(0.02, 0.96, title, fontsize=7.8, weight="bold", va="top", color=style.INK)
    fig.subplots_adjust(left=0, right=1, bottom=0, top=0.92)
    return fig


def build(sink):
    import style
    for fig_id, kind, title in zip(SHAPES, ("cylinder", "paraboloid", "cone", "pyramid"),
                                   ("Elliptical cylinder", "Elliptical paraboloid", "Elliptical cone", "Rectangular pyramid")):
        if fig_id in sink.expected:
            sink.save(_tile(kind, title, style), fig_id)
