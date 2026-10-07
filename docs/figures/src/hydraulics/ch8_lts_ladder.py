"""The local-time-stepping tier ladder (hydraulics Chapter 8, §8.5.6).

Every control volume takes a power-of-two multiple of the base step
Δt_0 from its own Courant limit; tiers are graded so that no face joins
volumes more than one tier apart, and a face fires at the finer side's
cadence, booking its flux into both accumulators. A macro cycle is one step
of the coarsest tier, at whose close every volume drains its accumulator.
Synthetic; no run.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()


def _draw(style):
    import matplotlib.pyplot as plt
    from matplotlib.patches import Rectangle
    fig, ax = plt.subplots(figsize=(8.6, 3.9))
    fig.patch.set_facecolor(style.SURFACE)
    K = 3
    macro = 2 ** K
    colours = [style.COLORS["fv"], style.COLORS["dw"], style.COLORS["dw-slot"], style.COLORS["fv-lts"]]
    for k in range(K + 1):
        y = K - k
        step = 2 ** k
        ax.text(-0.4, y, f"tier {k}\nΔt = {step} Δt_0" if k else "tier 0\nΔt = Δt_0", fontsize=8.2, ha="right", va="center",
                color=style.INK)
        for j in range(0, macro, step):
            ax.add_patch(Rectangle((j, y - 0.3), step, 0.6, fc=colours[k], ec=style.SURFACE, lw=1.2, alpha=0.75))
            ax.plot([j + step], [y], marker="|", ms=12, color=style.INK, mew=1.4)
    for j in range(macro + 1):
        ax.plot([j, j], [-0.65, K + 0.45], color=style.GRIDC, lw=0.6, zorder=0)
    ax.text(macro / 2, K + 0.62, f"one macro cycle = one step of the coarsest tier = 2^{K} Δt_0 for {K + 1} tiers",
            fontsize=8.6, ha="center", color=style.INK)
    ax.annotate("", xy=(macro, K + 0.5), xytext=(0, K + 0.5), arrowprops=dict(arrowstyle="<->", color=style.MUTED, lw=0.9))
    # a face between tier 1 and tier 2 fires at tier 1's cadence
    for j in range(0, macro, 2):
        ax.plot([j + 2], [K - 1.5], marker="v", ms=6, color=style.INK, zorder=5)
    ax.text(macro + 0.15, K - 1.5, "a face between tier 1 and tier 2\nfires at the finer cadence and\nbooks ±F Δt into both accumulators", fontsize=7.8,
            va="center", color=style.INK)
    ax.text(macro + 0.15, K - 3.0, "a volume drains its accumulator\nwhen its own window closes:\nexact conservation across tiers", fontsize=7.8,
            va="center", color=style.INK)
    ax.plot([macro], [-0.55], marker="^", ms=7, color=style.INK)
    ax.text(macro, -0.85, "macro-cycle close: re-tiering allowed", fontsize=8, ha="center", va="top", color=style.INK)
    ax.text(0, -0.85, "t", fontsize=8, ha="center", va="top", color=style.MUTED)
    ax.text(0, K + 1.0, "Tiers are graded: no face joins volumes more than one tier apart; FV_LTS_MAX_TIERS caps the spread",
            fontsize=8, color=style.MUTED)
    ax.set_xlim(-2.2, macro + 3.6)
    ax.set_ylim(-1.2, K + 1.3)
    ax.axis("off")
    fig.tight_layout()
    return fig


def build(sink):
    import style
    sink.save(_draw(style), "hydraulics_ch8_lts_ladder")
