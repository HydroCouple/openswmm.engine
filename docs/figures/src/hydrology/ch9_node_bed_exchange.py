"""Node-aquifer exchange through a semi-confining bed (hydrology Chapter 9,
§9.6).

A 1D node standing in a mesh cell exchanges with that cell's aquifer by a
MODFLOW-River conductance. The direction follows the heads alone: a water
table above the pipe head drives groundwater infiltration into the
network, a pipe head above the table drives exfiltration into the
aquifer. Two conductances are available — through a declared
semi-confining bed of thickness d_C and conductivity K_c, or, with no bed
declared, directly through half the soil column. Both directions are
capped; the caps are annotated where they bite. Synthetic; no model run.
"""
from __future__ import annotations

REQUIRES = ()

ZS = 4.4         # soil column thickness
HG = 3.0         # saturated thickness (table above the pipe head here)
D_C = 0.42       # semi-confining bed thickness
PIPE_Y = 1.15    # pipe invert
PIPE_H = 0.95    # pipe diameter


def _draw(style):
    import matplotlib.pyplot as plt
    from matplotlib.patches import Rectangle, Circle
    fig, ax = plt.subplots(figsize=(9.2, 4.6))
    fig.patch.set_facecolor(style.SURFACE)
    x0, W = 1.0, 8.0
    INK, WA, DW, OR = style.INK, style.WATER, style.COLORS["dw"], style.COLORS["fv-lts"]
    # cell: unsaturated above, saturated below
    ax.add_patch(Rectangle((x0, HG), W, ZS - HG, fc="#e7d9bf", ec=INK, lw=1.2))
    ax.add_patch(Rectangle((x0, 0), W, HG, fc=WA, alpha=0.34, ec=INK, lw=1.2))
    ax.plot([x0, x0 + W], [HG, HG], color=style.COLORS["fv"], lw=1.8)
    ax.text(x0 + 0.15, ZS + 0.12, "mesh cell — ground surface z_c", fontsize=7.8, color=INK)
    ax.text(x0 + W - 0.15, HG + 0.12, "water table  H_gw = z_bed + h_g", fontsize=7.8, ha="right",
            color=style.COLORS["fv"])
    ax.text(x0 + 0.15, 0.12, "aquifer bottom  z_bed", fontsize=7.6, color=INK)
    # manhole and pipe
    mx = x0 + 3.9
    ax.add_patch(Rectangle((mx - 0.34, PIPE_Y + PIPE_H), 0.68, ZS - PIPE_Y - PIPE_H + 0.35, fc="#d9e6f5",
                           ec=INK, lw=1.1))
    ax.add_patch(Rectangle((mx - 0.9, PIPE_Y), 1.8, PIPE_H, fc="#d9e6f5", ec=INK, lw=1.2))
    ax.add_patch(Circle((mx, ZS + 0.35), 0.2, fc="#bfbfbf", ec=INK, lw=1.0))
    ax.add_patch(Rectangle((mx - 0.9, PIPE_Y), 1.8, 0.52, fc=WA, alpha=0.45, ec="none"))
    ax.add_patch(Rectangle((mx - 0.34, PIPE_Y + PIPE_H), 0.68, 0.0, fc="none", ec="none"))
    ax.text(mx, PIPE_Y + 0.26, "d", fontsize=8, ha="center", va="center", color=INK)
    ax.plot([mx - 2.4, mx + 2.4], [PIPE_Y + 0.52, PIPE_Y + 0.52], color=DW, lw=1.5, ls=(0, (4, 2)))
    ax.text(mx + 2.5, PIPE_Y + 0.62, "pipe head\nH_pipe = z_inv + d", fontsize=7.8, va="bottom", color=DW)
    ax.plot([mx - 1.35, mx - 0.95], [PIPE_Y, PIPE_Y], color=INK, lw=0.9)
    ax.text(mx - 1.0, PIPE_Y - 0.16, "z_inv", fontsize=7.6, ha="right", va="top", color=INK)
    # semi-confining bed around the pipe
    bw = 2.6
    ax.add_patch(Rectangle((mx - bw / 2, PIPE_Y - D_C), bw, D_C, fc="#cbbfa6", ec=INK, lw=1.0, hatch="xx"))
    ax.annotate("", xy=(mx - bw / 2 - 0.12, PIPE_Y - D_C), xytext=(mx - bw / 2 - 0.12, PIPE_Y),
                arrowprops=dict(arrowstyle="<->", color=INK, lw=0.9))
    ax.text(mx - bw / 2 - 0.25, PIPE_Y - D_C / 2, "d_C", fontsize=8, ha="right", va="center", color=INK)
    ax.text(mx + bw / 2 + 0.15, PIPE_Y - D_C / 2, "semi-confining bed:  K_c, contact area A_b",
            fontsize=7.4, va="center", color=INK)
    # the head difference
    ax.annotate("", xy=(mx - 1.95, HG), xytext=(mx - 1.95, PIPE_Y + 0.52),
                arrowprops=dict(arrowstyle="<->", color=style.COLORS["fv"], lw=1.0))
    ax.text(mx - 2.05, (HG + PIPE_Y + 0.52) / 2, "H_gw − H_pipe\ndrives the exchange", fontsize=7.6, ha="right",
            va="center", color=style.COLORS["fv"])
    # the two directions
    A = lambda col: dict(arrowstyle="-|>", color=col, lw=1.8, mutation_scale=13)  # noqa: E731
    ax.annotate("", xy=(mx - 0.95, PIPE_Y + 0.25), xytext=(mx - 1.85, PIPE_Y + 0.25), arrowprops=A(WA))
    ax.text(mx - 1.95, PIPE_Y + 0.25, "Q > 0  infiltration\ninto the pipe", fontsize=7.4, ha="right",
            va="center", color=WA)
    ax.annotate("", xy=(mx + 1.85, PIPE_Y + 0.25), xytext=(mx + 0.95, PIPE_Y + 0.25), arrowprops=A(OR))
    ax.text(mx + 1.95, PIPE_Y + 0.25, "Q < 0  exfiltration\ninto the aquifer", fontsize=7.4, va="center", color=OR)
    # the conductance and the caps
    ax.text(x0, -0.55, "Q_node = C (H_gw − H_pipe),      C = K_c A_b / d_C  with a bed,      C = K_s A_b / (½ z_s)  without one",
            fontsize=8.4, color=INK)
    ax.text(x0, -1.05, "Caps — a drain takes at most half the cell's drainable water per step, ½ h_g S_y A / Δt.  A recharge is refused "
            "when the table is within 10⁻⁴ z_s of the ground,\ntakes at most half the headroom (z_s − h_g) S_y A, and cannot exceed what "
            "the node holds for the batch plus what flows through it.", fontsize=7.4, color=style.MUTED, linespacing=1.5)
    ax.set_xlim(0, 12.2)
    ax.set_ylim(-1.7, ZS + 1.0)
    ax.set_aspect("equal")
    ax.axis("off")
    fig.tight_layout(pad=0.2)
    return fig


def build(sink):
    import style
    sink.save(_draw(style), "hydrology_ch9_node_bed_exchange")
