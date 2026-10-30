"""A regular junction and a virtual junction at a grade break (hydraulics
Figure 3-10).

Left: the node-link scheme places a manhole with MIN_SURFAREA storage at
every conduit end, so a surveyed reach chained from short segments carries
a stagnation volume and a head update at each splice. Right: a virtual
junction is an interior point — zero storage, continuous invert — and
momentum is carried through it. Profile view, synthetic geometry; replaces
the watermarked placeholder.
"""
from __future__ import annotations

REQUIRES = ()


def _panel(ax, style, virtual):
    from matplotlib.patches import Rectangle, FancyArrowPatch
    xs, zin = [0, 4, 8], [2.0, 1.0, 0.7]         # invert with a grade break at x = 4
    crown = [z + 1.0 for z in zin]
    ax.fill_between(xs, [z - 0.9 for z in zin], zin, color=style.SOIL, zorder=0)
    ax.fill_between(xs, zin, crown, color=style.PIPE_FILL, zorder=1)
    ax.plot(xs, zin, color=style.INK, lw=1.8, zorder=2)
    ax.plot(xs, crown, color=style.MUTED, lw=1.2, ls=(0, (4, 2)), zorder=2)
    # water
    wl = [2.45, 1.55, 1.25]
    ax.fill_between(xs, zin, wl, color=style.WATER, alpha=0.25, zorder=1.5)
    ax.plot(xs, wl, color=style.WATER, lw=1.2, zorder=2)
    if not virtual:
        ax.add_patch(Rectangle((3.7, 1.0), 0.6, 2.3, fc="#e9e9e9", ec=style.INK, lw=1.3, zorder=3))
        ax.fill_between([3.7, 4.3], 1.0, 1.55, color=style.WATER, alpha=0.35, zorder=3.5)
        ax.text(4.0, 3.42, "manhole: MIN_SURFAREA storage,\nits own head update every trial", ha="center",
                va="bottom", fontsize=8, color=style.INK)
        ax.annotate("", xy=(4.0, 1.0), xytext=(4.0, 0.25), arrowprops=dict(arrowstyle="-|>", color=style.MUTED, lw=1.0))
        ax.text(4.0, 0.12, "stagnation volume\nbelow the outlet invert", ha="center", va="top", fontsize=7.6,
                color=style.MUTED)
        ax.text(1.0, 3.2, "conduit 1", fontsize=8, color=style.MUTED)
        ax.text(6.2, 2.0, "conduit 2", fontsize=8, color=style.MUTED)
    else:
        ax.plot([4.0], [1.0], marker="o", ms=7, mfc="white", mec=style.WATER, mew=1.6, zorder=4)
        ax.text(4.0, 3.42, "virtual junction: zero storage,\ncontinuous invert, no head unknown", ha="center",
                va="bottom", fontsize=8, color=style.INK)
        ax.add_patch(FancyArrowPatch((1.8, 1.95), (6.2, 1.05), arrowstyle="-|>", mutation_scale=16,
                                     color=style.WATER, lw=2.2, zorder=5))
        ax.text(4.0, 2.05, "momentum flux carried through", ha="center", fontsize=8.5, color=style.WATER, zorder=6,
                bbox=dict(boxstyle="round,pad=0.15", fc=style.SURFACE, ec="none", alpha=0.9))
        ax.text(1.0, 3.2, "one surveyed reach", fontsize=8, color=style.MUTED)
        ax.text(4.9, 0.45, "the solver sees one conduit;\nthe file keeps two", fontsize=7.6, color=style.MUTED)
    ax.set_xlim(-0.2, 8.4)
    ax.set_ylim(-0.6, 4.4)
    ax.axis("off")


def _draw(style):
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(1, 2, figsize=(9.0, 3.9), sharey=True)
    fig.patch.set_facecolor(style.SURFACE)
    for ax, virtual, title in zip(axes, (False, True), ("Regular junction at the grade break", "Virtual junction at the grade break")):
        _panel(ax, style, virtual)
        ax.set_title(title, fontsize=9.5, color=style.INK, loc="left")
    fig.tight_layout()
    return fig


def build(sink):
    import style
    sink.save(_draw(style), "hydraulics_ch3_virtual_junction")
