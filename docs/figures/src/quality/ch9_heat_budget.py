"""The heat budget of a water body in the heat-transport component
(quality Chapter 9, §9.3.3–9.3.6 and §9.3.11): the six surface terms of
J_net grouped by the [HEAT_FLUXES] module that enables them, plus the bed
zone's conduction and hyporheic exchange. Sign convention of the text:
positive flux is heat leaving the water. Synthetic; no run.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()


def _draw(style):
    import matplotlib.pyplot as plt
    from matplotlib.patches import Polygon, Rectangle, Circle
    fig, ax = plt.subplots(figsize=(9.2, 4.9))
    fig.patch.set_facecolor(style.SURFACE)
    WARM, COOL = style.COLORS["fv-lts"], style.COLORS["fv"]
    SUN = "#e6b422"
    # ground, channel and water
    ax.add_patch(Polygon([(0, 0), (12, 0), (12, 2.0), (9.2, 2.0), (8.2, 0.7), (3.8, 0.7), (2.8, 2.0), (0, 2.0)], closed=True,
                         fc=style.SOIL, ec="none", zorder=0))
    ax.add_patch(Polygon([(3.1, 1.6), (8.9, 1.6), (8.2, 0.7), (3.8, 0.7)], closed=True, fc=style.WATER, alpha=0.35, ec=style.WATER, lw=1.2, zorder=1))
    ax.add_patch(Rectangle((3.8, 0.0), 4.4, 0.7, fc="#bda98a", alpha=0.55, ec="none", zorder=0.5))
    ax.text(6.0, 1.15, "water  T_w", fontsize=9, ha="center", color=style.INK)
    ax.text(1.9, 4.55, "air  T_a,  RH,  wind w,  pressure p", fontsize=8, color=style.MUTED)
    # land cover on the right bank
    ax.add_patch(Rectangle((10.9, 2.0), 0.18, 1.0, fc="#7a5a3a", ec="none"))
    ax.add_patch(Circle((10.99, 3.35), 0.55, fc="#6aa15c", ec="none"))
    ax.text(11.0, 1.78, "land cover\n(1 − f_sky)", fontsize=7, ha="center", va="top", color=style.INK)
    # sun
    ax.add_patch(Circle((1.3, 4.6), 0.32, fc=SUN, ec="none"))
    A = lambda col, lw=1.8: dict(arrowstyle="-|>", color=col, lw=lw, mutation_scale=13)
    # incoming (warming): J_sn, J_an, J_lc
    ax.annotate("", xy=(4.1, 1.62), xytext=(1.6, 4.35), arrowprops=A(SUN, 2.2))
    ax.text(0.2, 2.75, "J_sn  net shortwave\n(1 − R_s) J_in max(0, 1 − f_s)", fontsize=7.4, color="#a07a10", ha="left", va="top")
    ax.annotate("", xy=(5.2, 1.62), xytext=(5.2, 4.4), arrowprops=A(WARM))
    ax.text(5.1, 3.95, "J_an  atmospheric longwave\nσ T_a⁴ ε_a (1 − R_L) f_sky", fontsize=7.4, color=WARM, ha="right", va="top")
    ax.annotate("", xy=(8.6, 1.62), xytext=(10.55, 3.0), arrowprops=A(WARM))
    ax.text(9.25, 2.1, "J_lc  ε_lc σ (1 − f_sky) T_a⁴", fontsize=7.0, color=WARM, ha="left", va="top")
    # outgoing (cooling): J_br, J_e, J_h
    ax.annotate("", xy=(6.35, 4.4), xytext=(6.35, 1.62), arrowprops=A(COOL))
    ax.text(6.45, 4.35, "J_br  back radiation\nε_w σ T_w⁴", fontsize=7.4, color=COOL, ha="left", va="top")
    ax.annotate("", xy=(7.3, 3.6), xytext=(7.3, 1.62), arrowprops=A(COOL))
    ax.text(7.4, 3.6, "J_e  latent = ρ_w L_v E\nE = (a + b w)(e_w − e_a)\nsigned: condensation warms", fontsize=7.4, color=COOL, ha="left", va="top")
    ax.annotate("", xy=(8.0, 2.55), xytext=(8.0, 1.62), arrowprops=A(COOL))
    ax.text(8.1, 2.62, "J_h = B_r J_e  (Bowen)", fontsize=7.4, color=COOL, ha="left", va="top")
    # bed zone: conduction and hyporheic exchange
    ax.annotate("", xy=(4.6, 0.1), xytext=(4.6, 0.65), arrowprops=A(style.INK, 1.4))
    ax.annotate("", xy=(4.95, 0.65), xytext=(4.95, 0.1), arrowprops=A(style.INK, 1.4))
    ax.text(3.9, -0.08, "conduction  G_cond (T_w − T_b)", fontsize=7.2, color=style.INK, va="top")
    ax.annotate("", xy=(7.4, 0.62), xytext=(7.4, 0.12), arrowprops=A(style.WATER, 1.4))
    ax.annotate("", xy=(7.75, 0.12), xytext=(7.75, 0.62), arrowprops=A(style.WATER, 1.4))
    ax.text(8.2, -0.30, "hyporheic advection  G_adv (T_w − T_b)", fontsize=7.2, color=style.WATER, va="top", ha="right")
    ax.text(6.15, 0.44, "bed zone  T_b", fontsize=7.4, ha="center", color=style.INK)
    ax.text(6.15, 0.12, "to deep ground:  G_bg (T_b − T_gr)", fontsize=6.8, ha="center", color=style.INK)
    # sign convention and module grouping
    ax.text(0.2, -0.80,
            "Positive flux is heat leaving the water:   J_net = (J_e + J_h)  +  (J_br − J_sn − J_an − J_lc)",
            fontsize=8.2, color=style.INK)
    ax.text(0.2, -1.10, "[HEAT_FLUXES]  SURFACE_EXCHANGE → J_e, J_h      RADIATIVE_EXCHANGE → J_sn, J_an, J_lc, J_br",
            fontsize=7.2, color=style.MUTED)
    ax.text(0.2, -1.36, "SEDIMENT_EXCHANGE → bed conduction, hyporheic exchange      LAYER_CONDUCTION → LID layer conduction",
            fontsize=7.2, color=style.MUTED)
    ax.plot([0.2, 0.7], [-1.64, -1.64], color=WARM, lw=2.2)
    ax.text(0.8, -1.64, "warms the water", fontsize=7.4, va="center", color=style.INK)
    ax.plot([2.6, 3.1], [-1.64, -1.64], color=COOL, lw=2.2)
    ax.text(3.2, -1.64, "cools the water (when positive)", fontsize=7.4, va="center", color=style.INK)
    ax.set_xlim(0, 12)
    ax.set_ylim(-1.82, 5.1)
    ax.set_aspect("equal")
    ax.axis("off")
    fig.tight_layout(pad=0.2)
    return fig


def build(sink):
    import style
    sink.save(_draw(style), "quality_ch9_heat_budget")
