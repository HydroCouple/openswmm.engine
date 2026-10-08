"""The ghost state a coupling face builds from the node head (hydraulics
Figure 8-4, §8.6.1).

The end cell of a conduit meets the node at a coupling face. The face's
outside state is a ghost cell whose depth is the node head above the
conduit invert plus offset, h_g = H − z_f, whose area follows the section
closure A(h_g), and whose velocity is the interior end-cell velocity — so
the Riemann problem at the face sees the node as a reservoir at head H.
Profile view; replaces the watermarked placeholder.
"""
from __future__ import annotations

REQUIRES = ()


def _draw(style):
    import matplotlib.pyplot as plt
    from matplotlib.patches import Rectangle, FancyArrowPatch
    fig, ax = plt.subplots(figsize=(7.6, 4.0))
    fig.patch.set_facecolor(style.SURFACE)
    # ground and manhole shaft
    ax.fill_between([0, 8], -0.2, 0.4, color=style.SOIL, zorder=0)
    ax.add_patch(Rectangle((0.6, 0.4), 1.4, 3.2, fc="#f4f3ef", ec=style.INK, lw=1.8, zorder=1))
    H = 2.6
    ax.fill_between([0.6, 2.0], 0.4, H, color=style.WATER, alpha=0.3, zorder=1.5)
    ax.plot([0.6, 2.0], [H, H], color=style.WATER, lw=2.0, zorder=2)
    ax.text(1.3, H + 0.12, "node head H", ha="center", fontsize=9.5, color=style.WATER)
    ax.text(1.3, 3.72, "manhole", ha="center", fontsize=8.5, color=style.MUTED)
    # conduit: invert from z_f at the face, crown one diameter above
    z_f = 1.0
    ax.fill_between([2.0, 7.6], [z_f - 0.6, 0.2], [z_f, 0.8], color=style.SOIL, zorder=0)
    ax.fill_between([2.0, 7.6], [z_f, 0.8], [z_f + 1.0, 1.8], color=style.PIPE_FILL, zorder=1)
    ax.plot([2.0, 7.6], [z_f, 0.8], color=style.INK, lw=1.8, zorder=2)
    ax.plot([2.0, 7.6], [z_f + 1.0, 1.8], color=style.MUTED, lw=1.2, ls=(0, (4, 2)), zorder=2)
    ax.fill_between([2.0, 7.6], [z_f, 0.8], [z_f + 0.55, 1.15], color=style.WATER, alpha=0.25, zorder=1.5)
    # cells
    for x in (3.4, 4.8, 6.2):
        ax.plot([x, x], [0.8 + (z_f - 0.8) * (7.6 - x) / 5.6, 1.8 + (z_f + 1.0 - 1.8) * (7.6 - x) / 5.6],
                color=style.MUTED, lw=0.7, ls=(0, (2, 2)))
    ax.text(2.7, 0.22, "end cell", fontsize=8.5, color=style.MUTED, ha="center")
    ax.text(4.1, 0.22, "interior cells →", fontsize=8.5, color=style.MUTED)
    # coupling face
    ax.plot([2.0, 2.0], [0.4, 3.6], color=style.INK, lw=1.3, ls=(0, (5, 2, 1, 2)), zorder=3)
    ax.text(2.06, 3.42, "coupling face", fontsize=9, color=style.INK)
    ax.text(2.1, z_f - 0.34, "z_f: invert + offset", fontsize=8.5, color=style.INK, va="top")
    # ghost depth
    ax.annotate("", xy=(1.75, H), xytext=(1.75, z_f), arrowprops=dict(arrowstyle="<->", color=style.INK, lw=1.0))
    ax.text(0.32, (H + z_f) / 2, "h_g = H − z_f", fontsize=9.5, rotation=90, va="center", ha="center", color=style.INK)
    # interior velocity carried onto the ghost
    ax.add_patch(FancyArrowPatch((3.2, 1.28), (4.6, 1.18), arrowstyle="-|>", mutation_scale=16,
                                 color=style.COLORS["fv-lts"], lw=2.0, zorder=4))
    ax.text(3.9, 1.5, "v_int (end cell)", ha="center", fontsize=9, color=style.COLORS["fv-lts"])
    ax.add_patch(FancyArrowPatch((2.9, 2.55), (2.15, 2.15), arrowstyle="-|>", mutation_scale=12,
                                 color=style.COLORS["fv-lts"], lw=1.2, zorder=4))
    ax.text(2.95, 2.62, "ghost: A(h_g),  v_g = v_int", fontsize=9.5, color=style.INK)
    ax.text(0.2, -0.05, "the node is a reservoir at head H: the face flux is the Riemann solution between the ghost and the end cell",
            fontsize=7.8, color=style.MUTED, va="top")
    ax.set_xlim(0, 8)
    ax.set_ylim(-0.45, 4.0)
    ax.set_aspect("equal")
    ax.axis("off")
    ax.set_title("Ghost state at a node coupling face", fontsize=9.5, color=style.INK, loc="left")
    fig.tight_layout()
    return fig


def build(sink):
    import style
    sink.save(_draw(style), "hydraulics_ch8_node_ghost_state")
