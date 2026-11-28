"""LID unit layer stacks: nine table tiles for quality Chapter 6 and the
annotated bio-retention cell for the Engine Manual's Chapter 1.

Each LID type is a vertical stack of the layers the engine models for it
(surface, pavement, soil, storage, drainage mat, underdrain), drawn to the
same conventions so the reader can compare types at a glance. The annotated
figure adds every flux the moisture balance tracks. Replaces the 90–300 px
legacy icons and the 129 × 90 px bio-retention sketch.
"""
from __future__ import annotations

REQUIRES = ()

LAYER_STYLE = {
    "surface":  ("Surface",          "#dbe7c6", "#6f8a4f", "grass"),
    "roof":     ("Roof",             "#e4e4e4", "#7a7a7a", "roof"),
    "pavement": ("Pavement",         "#cfcfcf", "#7a7a7a", "pavement"),
    "pavers":   ("Paver blocks",     "#cfcfcf", "#7a7a7a", "pavers"),
    "soil":     ("Soil",             "#d9c4a3", "#8a6d45", "soil"),
    "sand":     ("Sand bedding",     "#eadfc6", "#a08a5e", "soil"),
    "storage":  ("Storage (gravel)", "#d5d0c4", "#6e6a60", "gravel"),
    "barrel":   ("Barrel",           "#cfe3f7", "#3b6fa0", "water"),
    "mat":      ("Drainage mat",     "#e8e0f2", "#7a5da0", "mat"),
    "native":   ("Native soil",      "#c9b592", "#8a6d45", "soil"),
}

# (fig_id, title, [(layer key, relative thickness), ...], underdrain?)
TILES = [
    ("quality_ch6_lid_bio_cell", "Bio-retention cell", [("surface", 0.7), ("soil", 1.6), ("storage", 1.2)], True),
    ("quality_ch6_lid_rain_garden", "Rain garden", [("surface", 0.7), ("soil", 1.6)], False),
    ("quality_ch6_lid_green_roof", "Green roof", [("surface", 0.6), ("soil", 1.1), ("mat", 0.35), ("roof", 0.35)], False),
    ("quality_ch6_lid_infiltration_trench", "Infiltration trench", [("surface", 0.5), ("storage", 2.2)], True),
    ("quality_ch6_lid_permeable_pavement", "Continuous permeable pavement",
     [("surface", 0.35), ("pavement", 0.8), ("soil", 0.7), ("storage", 1.4)], True),
    ("quality_ch6_lid_block_paver", "Block paver", [("surface", 0.35), ("pavers", 0.7), ("sand", 0.5), ("storage", 1.4)], True),
    ("quality_ch6_lid_rain_barrel", "Rain barrel", [("barrel", 2.4)], True),
    ("quality_ch6_lid_rooftop_disconnection", "Rooftop disconnection", [("roof", 0.5), ("surface", 1.0)], False),
    ("quality_ch6_lid_vegetative_swale", "Vegetative swale", [("surface", 1.3)], False),
]


def _hatch(ax, kind, x0, x1, y0, y1, ec):
    import numpy as np
    w, h = x1 - x0, y1 - y0
    if kind == "grass":
        for x in np.linspace(x0 + 0.06 * w, x1 - 0.06 * w, 9):
            ax.plot([x, x - 0.015 * w, x, x + 0.015 * w], [y1, y1 + 0.35 * h, y1, y1 + 0.32 * h],
                    color=ec, lw=0.9, solid_capstyle="round")
    elif kind == "gravel":
        rng = np.random.default_rng(7)
        for _ in range(int(60 * w)):
            ax.plot([x0 + rng.random() * w], [y0 + rng.random() * h], marker="o", ms=1.6, color=ec, alpha=0.55)
    elif kind == "soil":
        rng = np.random.default_rng(3)
        for _ in range(int(25 * w)):
            ax.plot([x0 + rng.random() * w], [y0 + rng.random() * h], marker=".", ms=1.2, color=ec, alpha=0.4)
    elif kind == "pavement":
        for y in np.linspace(y0, y1, 4)[1:-1]:
            ax.plot([x0, x1], [y, y], color=ec, lw=0.4, alpha=0.6)
    elif kind == "pavers":
        for x in np.linspace(x0, x1, 7)[1:-1]:
            ax.plot([x, x], [y0, y1], color=ec, lw=0.6)
    elif kind == "mat":
        for x in np.linspace(x0, x1, 12)[1:-1]:
            ax.plot([x, x], [y0, y1], color=ec, lw=0.5, alpha=0.7)
    elif kind == "water":
        for y in np.linspace(y0 + 0.1 * h, y1 - 0.4 * h, 3):
            ax.plot([x0 + 0.1 * w, x1 - 0.1 * w], [y, y], color=ec, lw=0.6, alpha=0.5)
    elif kind == "roof":
        for x in np.linspace(x0, x1, 5)[1:-1]:
            ax.plot([x, x + 0.08 * w], [y0, y1], color=ec, lw=0.4, alpha=0.6)


def _stack(ax, layers, underdrain, x0=0.0, x1=1.0, top=1.0, label_size=7.0, style=None, label_side="right"):
    total = sum(t for _, t in layers)
    y = top
    geom = {}
    lx, ha = (x1 + 0.03, "left") if label_side == "right" else (x0 - 0.03, "right")
    for key, thick in layers:
        name, fc, ec, kind = LAYER_STYLE[key]
        h = thick / total * top
        ax.add_patch(__import__("matplotlib.patches", fromlist=["Rectangle"]).Rectangle(
            (x0, y - h), x1 - x0, h, fc=fc, ec=ec, lw=0.9, zorder=1))
        _hatch(ax, kind, x0, x1, y - h, y, ec)
        ax.text(lx, y - h / 2, name, fontsize=label_size, va="center", ha=ha, color=style.INK)
        geom[key] = (y - h, y)
        y -= h
    if underdrain:
        key = layers[-1][0]
        yb, yt = geom[key]
        cy = yb + 0.22 * (yt - yb)
        ax.plot([x0 + 0.5 * (x1 - x0)], [cy], marker="o", ms=7, mfc="white", mec=style.INK, mew=1.0, zorder=3)
        ax.text(lx, cy - 0.09 * top, "Underdrain", fontsize=label_size, va="center", ha=ha, color=style.MUTED)
    return geom


def _tile(fig_id, title, layers, underdrain, style):
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(2.55, 1.75))
    fig.patch.set_facecolor(style.SURFACE)
    _stack(ax, layers, underdrain, x0=0.05, x1=0.55, top=1.0, label_size=6.6, style=style)
    ax.set_xlim(0, 1.3)
    ax.set_ylim(-0.05, 1.42)
    ax.axis("off")
    ax.text(0.05, 1.36, title, fontsize=7.6, weight="bold", va="top", color=style.INK)
    fig.tight_layout(pad=0.2)
    return fig


def _bioretention(style):
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(7.2, 4.2))
    fig.patch.set_facecolor(style.SURFACE)
    layers = [("surface", 0.7), ("soil", 1.6), ("storage", 1.2)]
    geom = _stack(ax, layers, True, x0=0.30, x1=0.85, top=1.0, label_size=8.0, style=style, label_side="left")
    ax.add_patch(__import__("matplotlib.patches", fromlist=["Rectangle"]).Rectangle(
        (0.30, -0.28), 0.55, 0.28, fc=LAYER_STYLE["native"][1], ec=LAYER_STYLE["native"][2], lw=0.9, zorder=1))
    _hatch(ax, "soil", 0.30, 0.85, -0.28, 0.0, LAYER_STYLE["native"][2])
    ax.text(0.27, -0.14, "Native soil", fontsize=8, va="center", ha="right", color=style.INK)
    w = style.WATER
    kw = dict(arrowstyle="-|>", color=w, lw=1.3, mutation_scale=11)
    A = lambda xy, xytext, text, dx=0.0, dy=0.0, ha="center": (  # noqa: E731
        ax.annotate("", xy=xy, xytext=xytext, arrowprops=kw),
        ax.text((xy[0] + xytext[0]) / 2 + dx, (xy[1] + xytext[1]) / 2 + dy, text, fontsize=7.4, color=w,
                ha=ha, va="center", fontstyle="italic",
                bbox=dict(boxstyle="round,pad=0.15", fc=style.SURFACE, ec="none", alpha=0.9)))
    s0, s1 = geom["surface"]
    so0, so1 = geom["soil"]
    st0, st1 = geom["storage"]
    A((0.45, s1 + 0.02), (0.45, s1 + 0.28), "rainfall", dx=-0.02, dy=0.03, ha="right")
    A((0.32, s1 + 0.02), (0.10, s1 + 0.12), "run-on from the drainage area", dx=-0.03, dy=0.06, ha="right")
    A((0.70, s1 + 0.28), (0.70, s1 + 0.02), "evaporation", dx=0.02, dy=0.03, ha="left")
    A((1.10, s1 - 0.5 * (s1 - s0)), (0.86, s1 - 0.5 * (s1 - s0)), "surface outflow", dx=0.0, dy=0.05)
    A((0.58, so1 - 0.08), (0.58, s0 + 0.02), "infiltration into the soil", dx=0.30, dy=0.0)
    A((0.58, st1 - 0.06), (0.58, so0 + 0.02), "percolation", dx=0.22, dy=0.0)
    A((0.58, -0.24), (0.58, st0 + 0.02), "infiltration into native soil (unless lined)", dx=0.03, dy=0.0, ha="left")
    cy = st0 + 0.22 * (st1 - st0)
    A((1.15, cy), (0.60, cy), "underdrain outflow", dx=0.12, dy=0.05)
    ax.text(-0.02, 1.50, "Bio-retention cell: the layers of the moisture balance and its fluxes",
            fontsize=9.2, weight="bold", va="top", color=style.INK)
    ax.set_xlim(-0.02, 1.45)
    ax.set_ylim(-0.34, 1.52)
    ax.axis("off")
    fig.tight_layout(pad=0.3)
    return fig


def build(sink):
    import style
    for fig_id, title, layers, underdrain in TILES:
        if fig_id in sink.expected:
            sink.save(_tile(fig_id, title, layers, underdrain, style), fig_id)
    if "eng_lid_bioretention_layers" in sink.expected:
        sink.save(_bioretention(style), "eng_lid_bioretention_layers")
