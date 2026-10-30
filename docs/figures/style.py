"""House style for every generated manual figure.

Vendored 2026-09-19 from two validated sources so the manuals, the benchmark
reports and the SWMM paper share one visual language:

* openswmm.engine.benchmarks/suites/analytical/transitions/translib/report.py
  (COLORS, INK/MUTED/GRIDC/SURFACE/SOIL/PIPE_FILL, _style)
* epaswmm5_qa/article/scripts/_style.py (DPI 160, save())

The categorical solver palette is a fixed identity: a solver keeps its colour
in every figure and is never re-ranked. Status chips take their colours from
docs/figures/status.py, not from here.

Only generators import this module; the manifest audit (check) stays stdlib.
"""
from __future__ import annotations

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt  # noqa: E402

# categorical solver identity (never re-ranked)
COLORS = {
    "fv": "#2a78d6",
    "fv-lts": "#eb6834",
    "dw": "#1baf7a",
    "dw-slot": "#eda100",
    "dw-legacy": "#e87ba4",
}
LABELS = {
    "fv": "Finite volume",
    "fv-lts": "Finite volume + LTS",
    "dw": "Dynamic wave",
    "dw-slot": "Dynamic wave, slot",
    "dw-legacy": "Legacy SWMM 5",
}
ORDER = ["fv", "fv-lts", "dw", "dw-slot", "dw-legacy"]   # legend order
ZBOT = list(reversed(ORDER))                             # draw fv last (top)

# chrome
INK, MUTED, GRIDC = "#0b0b0b", "#898781", "#e1e0d9"
SURFACE, SOIL, PIPE_FILL = "#fcfcfb", "#e2d7bd", "#efede6"
WATER = "#2a78d6"

DPI = 160            # article export; width <= 10 in keeps PNGs under 1600 px
MAX_WIDTH_IN = 10.0


def style_ax(ax):
    """Spines, grid and tick colours of the house style."""
    ax.set_facecolor(SURFACE)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRIDC)
    ax.tick_params(colors=MUTED, labelsize=9)
    ax.xaxis.label.set_color(MUTED)
    ax.yaxis.label.set_color(MUTED)
    ax.title.set_color(INK)
    ax.grid(True, color=GRIDC, linewidth=0.6, alpha=0.7)
    ax.set_axisbelow(True)


def schematic_ax(ax):
    """A diagram axis: no ticks, no spines, equal aspect."""
    ax.set_facecolor(SURFACE)
    ax.set_aspect("equal")
    ax.axis("off")


def figure(width_in=8.0, height_in=4.5, **kw):
    """A figure whose width respects the 1600 px embed bound at DPI."""
    width_in = min(width_in, MAX_WIDTH_IN)
    fig = plt.figure(figsize=(width_in, height_in), **kw)
    fig.patch.set_facecolor(SURFACE)
    return fig


def apply_rc():
    """Deterministic text and export settings; called once by the build script."""
    plt.rcParams.update({
        "font.family": "DejaVu Sans",      # bundled with matplotlib
        "font.size": 9,
        "axes.titlesize": 10,
        "axes.labelsize": 9,
        "legend.fontsize": 8,
        "legend.frameon": False,
        "savefig.facecolor": SURFACE,
        "svg.fonttype": "none",            # text stays text in the SVG twin
        "path.simplify": True,
    })
