"""Per-cell infiltration: parameter resolution and the held rate
(hydrology Chapter 8, §8.3.3 and §8.3.4).

(a) A strip of cells carrying three soil tags. Each cell takes its method
and parameters by the resolution order — a cell override beats a TAG row,
which beats the mesh-wide `*` row — and a row whose method is NONE clears
the default for its cells rather than inheriting it. (b) The kernel is
called on the INFIL_STEP cadence and the rate it returns is held between
calls, so the sink the marcher consumes is a step function whatever the
substep. Synthetic; the Horton curve is its own analytic decay, no model
run.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()

# (tag, cells, method, note) in strip order
TAGS = [("SILT", 3, "GREEN_AMPT", "TAG row"),
        ("SAND", 3, "HORTON", "TAG row"),
        ("CLAY", 3, "NONE", "TAG row clears the default")]
OVERRIDE_CELL = 5          # 1-based, inside the SAND run
OVERRIDE_METHOD = "CURVE_NUMBER"

F0, FC, K = 3.0, 0.5, 4.5   # in/hr, in/hr, 1/hr — a Horton decay for panel (b)
INFIL_STEP = 0.25           # hr


def _strip(ax, style):
    from matplotlib.patches import Rectangle
    fills = {"SILT": "#e2d6bd", "SAND": "#efe3c3", "CLAY": "#cfc4ae"}
    x = 0.0
    idx = 0
    for tag, n, method, note in TAGS:
        for _ in range(n):
            idx += 1
            over = idx == OVERRIDE_CELL
            m = OVERRIDE_METHOD if over else method
            ax.add_patch(Rectangle((x, 0), 1.0, 1.0, fc=fills[tag], ec=style.INK, lw=1.0))
            if over:
                ax.add_patch(Rectangle((x, 0), 1.0, 1.0, fc="none", ec=style.COLORS["fv-lts"], lw=2.2))
            ax.text(x + 0.5, 0.62, f"cell {idx}", fontsize=6.8, ha="center", color=style.MUTED)
            ax.text(x + 0.5, 0.34, m.replace("_", "\n"), fontsize=6.6, ha="center", va="center",
                    color=style.COLORS["fv-lts"] if over else style.INK)
            x += 1.0
        x0 = x - n
        ax.plot([x0, x], [-0.18, -0.18], color=style.INK, lw=1.0)
        ax.text((x0 + x) / 2, -0.28, f"TAG {tag}", fontsize=7.4, ha="center", va="top", color=style.INK)
        ax.text((x0 + x) / 2, -0.58, note, fontsize=6.8, ha="center", va="top", color=style.MUTED)
    ax.annotate("", xy=(OVERRIDE_CELL - 0.5, 1.08), xytext=(OVERRIDE_CELL - 0.5, 1.62),
                arrowprops=dict(arrowstyle="-|>", color=style.COLORS["fv-lts"], lw=1.3, mutation_scale=11))
    ax.text(OVERRIDE_CELL - 0.5, 1.70, "[2D_INFILTRATION] CELL row — a per-cell override",
            fontsize=7.4, ha="center", color=style.COLORS["fv-lts"])
    ax.text(0.0, 2.35, "resolution order:   cell override  >  TAG row  >  `*` row  >  none",
            fontsize=8.2, color=style.INK)
    ax.text(0.0, 2.02, "the `*` row of [2D_INFILTRATION_DEFAULTS] is the mesh-wide fallback; "
            "a NONE row deliberately clears it", fontsize=7.2, color=style.MUTED)
    ax.set_xlim(-0.3, 9.3)
    ax.set_ylim(-1.15, 2.6)
    ax.set_aspect("equal")
    ax.axis("off")


def _held(ax, style):
    t = np.linspace(0, 2.0, 800)
    cont = FC + (F0 - FC) * np.exp(-K * t)
    ax.plot(t, cont, color=style.MUTED, lw=1.1, ls=(0, (4, 2)), label="the kernel's continuous capacity")
    edges = np.arange(0, 2.0 + 1e-9, INFIL_STEP)
    for k in range(len(edges) - 1):
        f = FC + (F0 - FC) * np.exp(-K * edges[k])
        ax.plot([edges[k], edges[k + 1]], [f, f], color=style.COLORS["fv"], lw=2.0,
                label="the held rate f_i the marcher consumes" if k == 0 else None)
        if k:
            fp = FC + (F0 - FC) * np.exp(-K * edges[k - 1])
            ax.plot([edges[k], edges[k]], [fp, f], color=style.COLORS["fv"], lw=2.0)
        ax.plot([edges[k]], [f], marker="o", ms=3.4, color=style.COLORS["fv"])
    for e in edges:
        ax.axvline(e, color=style.GRIDC, lw=0.6, zorder=0)
    ax.annotate("", xy=(edges[1], 0.15), xytext=(edges[2], 0.15),
                arrowprops=dict(arrowstyle="<->", color=style.INK, lw=0.9))
    ax.text(edges[1] + INFIL_STEP / 2, 0.24, "INFIL_STEP", fontsize=7.4, ha="center", color=style.INK)
    ax.set_xlabel("time  (hr)")
    ax.set_ylabel("infiltration rate  (in/hr)")
    ax.set_ylim(0, F0 * 1.12)
    ax.set_xlim(0, 2.0)
    ax.legend(fontsize=7.2, frameon=False, loc="upper right")
    ax.set_title("(b) The rate is refreshed on INFIL_STEP and held between calls", fontsize=9, loc="left")
    ax.text(0.78, 2.35, "the marcher's substeps are far shorter than INFIL_STEP;\n"
            "each consumes the held rate through the dry-depth ramp of (8-5)", fontsize=7.2, color=style.MUTED,
            linespacing=1.4)


def _draw(style):
    import matplotlib.pyplot as plt
    fig, (a0, a1) = plt.subplots(2, 1, figsize=(8.6, 5.4), gridspec_kw=dict(height_ratios=[1.0, 1.15]))
    fig.patch.set_facecolor(style.SURFACE)
    a0.set_title("(a) How each cell resolves its method and parameters", fontsize=9, loc="left")
    _strip(a0, style)
    style.style_ax(a1)
    _held(a1, style)
    fig.tight_layout(pad=0.4)
    return fig


def build(sink):
    import style
    sink.save(_draw(style), "hydrology_ch8_infiltration_strip")
