"""Physical junction chamber storage (hydraulics reference, "Physical junction
chamber storage"; Python guide nodes.rst points here).

(a) A section through a junction with a positive StorageArea A: the chamber
    holds A*y below the rim, stays active above the connected pipe crowns, caps
    its shaft at the rim, ponds above the rim over the separate ponded area, and
    treats SurDepth as a pressure allowance, not shaft height.
(b) The chamber's volume against depth for the reference's worked example
    (MaxDepth 10 ft, StorageArea 100 ft2, 400 ft3 at 4 ft), with and without
    ponding: V = A*clamp(y, 0, MaxDepth) (`src/engine/hydraulics/Node.cpp:112-113`),
    dV/dy = A below the rim and 0 above (`Node.cpp:338-339`), plus
    (y - MaxDepth)*A_ponded when ponding is on
    (`src/engine/hydraulics/DynamicWave.cpp:4215-4216`).
(c) How a conduit's inventory is shared between its two end nodes: each end
    takes A_end/(A_1 + A_2) of the conduit volume, with A_end the section area
    at that end's depth above its offset, zero when dry and A_full above the
    crown under EXTRAN (`DynamicWave.cpp:1815-1856`). With the dynamic-wave
    trapezoidal volume L*(A_1 + A_2)/2 that is A_end*L/2. Section areas come
    from the engine's own geometry (`XSectionGeometry`, circular 2 ft).

Synthetic tier; REQUIRES openswmm for the section areas. US units.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ("openswmm",)

AREA, YMAX, APOND = 100.0, 10.0, 500.0      # ft2, ft, ft2 (the reference's example; A_ponded illustrative)
PIPE_D, PIPE_L, FAR_DEPTH = 2.0, 200.0, 1.0  # circular conduit, ft; far end held half full


def chamber_volume(y, ponding):
    """Node.cpp:112-113 below the rim; DynamicWave.cpp:4215-4216 above it."""
    v = AREA * np.clip(y, 0.0, YMAX)
    if ponding:
        v = v + np.where(y > YMAX, (y - YMAX) * APOND, 0.0)
    return v


def end_shares(geom, y_chamber_end):
    """Chamber-end and far-end shares of the conduit inventory (ft3), DynamicWave.cpp:1830-1856."""
    a1 = np.where(y_chamber_end > 0.0, geom.area(np.clip(y_chamber_end, 0.0, PIPE_D)), 0.0)
    a2 = float(geom.area(FAR_DEPTH))
    volume = PIPE_L * (a1 + a2) / 2.0                  # dynamic-wave trapezoidal pipe volume
    frac = np.where(a1 + a2 > 0.0, a1 / np.maximum(a1 + a2, 1e-300), 0.5)
    return frac * volume, (1.0 - frac) * volume


def _section(ax, style):
    from matplotlib.patches import Polygon, Rectangle
    ink, muted = style.INK, style.MUTED
    water, wall, soil = "#cfe0f5", "#6f6c66", style.SOIL
    ax.set_xlim(-9.5, 9.5)
    ax.set_ylim(-1.2, 14.4)
    ax.set_aspect("equal")
    ax.axis("off")
    half = 1.6                     # drawn half width of the shaft
    y_w = 6.0                      # water depth in the drawing, above both crowns
    # ground and soil around the shaft
    ax.add_patch(Rectangle((-9.5, -1.2), 19.0, 11.2, fc=soil, ec="none", alpha=0.45, zorder=0))
    ax.plot([-9.5, -half], [YMAX, YMAX], color=wall, lw=1.2, zorder=3)
    ax.plot([half, 9.5], [YMAX, YMAX], color=wall, lw=1.2, zorder=3)
    # chamber water and walls
    ax.add_patch(Rectangle((-half, 0.0), 2 * half, y_w, fc=water, ec="none", zorder=1))
    ax.add_patch(Rectangle((-half, 0.0), 2 * half, YMAX, fc="none", ec=wall, lw=1.6, zorder=4))
    # pipes: left D = 2 ft at offset 3 ft, right D = 3 ft at offset 0
    for x0, x1, off, d in ((-9.5, -half, 3.0, 2.0), (half, 9.5, 0.0, 3.0)):
        ax.add_patch(Rectangle((x0, off), x1 - x0, d, fc=water, ec=wall, lw=1.2, zorder=2))
        ax.plot([x0, x1], [off + d, off + d], color=ink, lw=1.2, zorder=3)
    ax.text(-9.3, 5.25, "pipe crown", fontsize=7.6, color=ink, va="bottom")
    ax.text(9.3, 3.25, "pipe crown", fontsize=7.6, color=ink, va="bottom", ha="right")
    # water level and depth
    ax.plot([-half, half], [y_w, y_w], color=style.WATER, lw=1.6, zorder=5)
    ax.annotate("", (0.95, 0.0), (0.95, y_w), arrowprops=dict(arrowstyle="<->", color=ink, lw=0.9), zorder=6)
    ax.text(1.1, 2.0, "y", fontsize=9, color=ink, style="italic", zorder=6)
    ax.text(-0.45, 2.8, "A·y", fontsize=9.5, color=ink, ha="center", zorder=6, fontweight="bold")
    # plan area
    ax.annotate("", (-half, 8.2), (half, 8.2), arrowprops=dict(arrowstyle="<->", color=ink, lw=0.9))
    ax.text(0.0, 8.45, "A", fontsize=9, color=ink, ha="center", style="italic")
    # rim, ponding and the pressure allowance
    ax.text(-9.3, YMAX + 0.2, "rim: MaxDepth", fontsize=7.6, color=ink, va="bottom")
    pond = Polygon([(-6.5, YMAX), (6.5, YMAX), (5.0, YMAX + 0.9), (-5.0, YMAX + 0.9)], closed=True,
                   fc=water, ec=style.WATER, lw=0.9, ls=(0, (3, 2)), alpha=0.8, zorder=2)
    ax.add_patch(pond)
    ax.text(-6.5, YMAX + 1.15, "ponded area A_ponded, used only above the rim", fontsize=7.6,
            color=ink, ha="left", va="bottom")
    ax.annotate("", (7.6, YMAX), (7.6, YMAX + 2.2), arrowprops=dict(arrowstyle="<->", color=muted, lw=0.9))
    ax.text(7.0, YMAX + 2.4, "SurDepth: a pressure\nallowance only", fontsize=7.4,
            color=muted, ha="center", va="bottom", linespacing=1.3)
    # what the shaft does and does not hold
    ax.text(-9.3, -0.9, "the chamber stays active\nabove the pipe crowns", fontsize=7.4,
            color=muted, va="bottom", linespacing=1.3)
    ax.text(9.3, 4.2, "conduit water belongs\nto the conduits, shared\nby the end sections", fontsize=7.4,
            color=muted, va="bottom", ha="right", linespacing=1.3)
    ax.set_title("(a) A junction with StorageArea A > 0", fontsize=9, loc="left")


def _draw(style, geom):
    import matplotlib.pyplot as plt
    fig = style.figure(10.0, 4.9)
    gs = fig.add_gridspec(2, 2, width_ratios=[1.08, 1.0], hspace=0.55, wspace=0.22)
    a0 = fig.add_subplot(gs[:, 0])
    a1 = fig.add_subplot(gs[0, 1])
    a2 = fig.add_subplot(gs[1, 1])
    _section(a0, style)
    a0.set_anchor("N")
    for ax in (a1, a2):
        style.style_ax(ax)

    y = np.linspace(0.0, 12.0, 241)
    a1.plot(y, chamber_volume(y, True), color=style.COLORS["fv"], lw=1.4, ls=(0, (4, 2)),
            label=f"ponding on, A_ponded = {APOND:.0f} ft²")
    a1.plot(y, chamber_volume(y, False), color=style.COLORS["dw"], lw=1.8,
            label=f"shaft only, A = {AREA:.0f} ft²")
    a1.axvline(YMAX, color=style.MUTED, lw=0.8, ls=(0, (2, 2)))
    a1.text(YMAX - 0.15, 1550, "rim", fontsize=7.4, color=style.MUTED, ha="right")
    v4 = float(chamber_volume(np.array(4.0), False))
    a1.plot([4.0], [v4], "o", ms=4.5, mfc=style.COLORS["dw"], mec=style.INK, mew=0.6)
    a1.annotate(f"{v4:.0f} ft³ at 4 ft", (4.0, v4), xytext=(4.6, 230), fontsize=7.4, color=style.INK)
    a1.set_xlim(0, 12)
    a1.set_ylim(0, 2000)
    a1.set_xlabel("junction depth y (ft)")
    a1.set_ylabel("chamber volume (ft³)")
    a1.set_title("(b) Chamber volume, MaxDepth 10 ft", fontsize=9, loc="left")
    a1.legend(loc="upper left", fontsize=7.2)

    yc = np.linspace(-0.5, 3.0, 351)
    own, far = end_shares(geom, yc)
    a2.fill_between(yc, 0, own, color=style.COLORS["dw"], alpha=0.75, lw=0, label="chamber end's share")
    a2.fill_between(yc, own, own + far, color=style.GRIDC, alpha=0.95, lw=0, label="far end's share")
    a2.plot(yc, own + far, color=style.MUTED, lw=0.9)
    a2.axvline(0.0, color=style.MUTED, lw=0.8, ls=(0, (2, 2)))
    a2.axvline(PIPE_D, color=style.MUTED, lw=0.8, ls=(0, (2, 2)))
    a2.text(-0.04, 610, "invert", fontsize=7.2, color=style.MUTED, ha="right")
    a2.text(PIPE_D + 0.06, 610, "crown", fontsize=7.2, color=style.MUTED)
    a2.text(-0.45, 205, "dry end:\nno share", fontsize=7.2, color=style.INK, linespacing=1.25)
    a2.set_xlim(-0.5, 3.0)
    a2.set_ylim(0, 700)
    a2.set_xlabel("depth at the chamber end of the conduit (ft)")
    a2.set_ylabel("conduit volume (ft³)")
    a2.set_title(f"(c) A {PIPE_D:.0f} ft pipe, {PIPE_L:.0f} ft long, far end {FAR_DEPTH:.0f} ft deep",
                 fontsize=9, loc="left")
    a2.legend(loc="upper left", bbox_to_anchor=(0.16, 1.0), fontsize=7.2)
    return fig


def build(sink):
    import openswmm.engine as e
    import style
    geom = e.XSectionGeometry(e.XSectShape.CIRCULAR, PIPE_D, units="US")
    fig = _draw(style, geom)
    own, far = end_shares(geom, np.array([0.0, 1.0, 2.0, 3.0]))
    print(f"    chamber volume at 4 ft: {float(chamber_volume(np.array(4.0), False)):.1f} ft3; "
          f"at 11 ft with ponding: {float(chamber_volume(np.array(11.0), True)):.1f} ft3")
    print("    conduit shares (chamber end, far end) at chamber-end depth 0, 1, 2, 3 ft: "
          + ", ".join(f"({a:.1f}, {b:.1f})" for a, b in zip(own, far)))
    sink.save(fig, "hydraulics_junction_chamber_storage")
