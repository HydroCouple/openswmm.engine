"""Two-zone representation of the Green-Ampt infiltration model (hydrology
Figure 4-5, Section 4.4.1).

(a) The soil column: ponded depth d over a wetted zone at saturation theta_s
    whose depth L_s grows as water infiltrates, the wetting front, and the
    unwetted zone at the initial moisture content theta_i below it. The
    infiltration capacity is Darcy's law across the wetted zone (Equation
    4-26).
(b) The moisture-content profile at one instant. The wetted zone holds the
    deficit theta_d = theta_s - theta_i over the depth L_s, so the shaded area
    is the cumulative infiltration F = L_s theta_d, which is how Equation 4-27
    follows from Equation 4-26.

Synthetic tier. The numbers are the chapter's own example of Figure 4-6
(K_s = 0.25 in/hr, psi_s = 6.5 in, theta_d = 0.20) at F = 1.0 in, and the
capacity printed in panel (b) is evaluated the way the engine evaluates it:
c1 = (S + depth) * IMD in src/engine/hydrology/Infiltration.cpp:310 (also
:414), and the explicit form f2 = f1 + ks (1 + c1/f1) ts at :272, i.e.
f_p = K_s (1 + (psi_s + d) theta_d / F). Replaces the 536 x 286 px redrawing
after Nicklow et al. (2006).
"""
from __future__ import annotations

REQUIRES = ()

KS, PSI, THETA_S, THETA_I = 0.25, 6.5, 0.45, 0.25    # in/hr, in, -, -  (theta_d = 0.20, Figure 4-6)
F_CUM, PONDED = 1.0, 0.0                              # in of cumulative infiltration; ponded depth d


def capacity(F, d=PONDED):
    """f_p = K_s (1 + c1 / F) with c1 = (psi_s + d) theta_d (Infiltration.cpp:272, :310)."""
    c1 = (PSI + d) * (THETA_S - THETA_I)
    return KS * (1.0 + c1 / F)


def _panel_a(ax, style):
    from matplotlib.patches import FancyArrowPatch, Rectangle
    ax.set_facecolor(style.SURFACE)
    ax.set_xlim(-8.0, 58.0)
    ax.set_ylim(-34.0, 9.0)
    ax.axis("off")
    x0, x1 = 6.0, 34.0
    ls = 13.0                                   # drawn depth of the wetted zone
    pond = 2.2
    ax.add_patch(Rectangle((x0, 0.0), x1 - x0, pond, fc="#cfe3f6", ec="none", zorder=1))
    ax.plot([x0, x1], [pond, pond], color=style.WATER, lw=1.0, zorder=2)
    ax.add_patch(Rectangle((x0, -ls), x1 - x0, ls, fc="#7fb3e6", ec="none", alpha=0.75, zorder=1))
    ax.add_patch(Rectangle((x0, -31.0), x1 - x0, 31.0 - ls, fc=style.SOIL, ec="none", alpha=0.7, zorder=1))
    ax.plot([x0, x0], [pond + 2.0, -31.0], color=style.INK, lw=1.0)
    ax.plot([x1, x1], [pond + 2.0, -31.0], color=style.INK, lw=1.0)
    ax.plot([x0 - 4.0, x1 + 4.0], [0.0, 0.0], color=style.INK, lw=1.4, zorder=3)
    ax.plot([x0, x1], [-ls, -ls], color=style.INK, lw=1.2, ls=(0, (5, 2)), zorder=3)
    ax.text(x0 - 4.5, 0.0, "ground\nsurface", ha="right", va="center", fontsize=7.6, color=style.INK,
            linespacing=1.3)
    ax.text(x0 - 4.5, -ls, "wetting\nfront", ha="right", va="center", fontsize=7.6, color=style.INK,
            linespacing=1.3)
    ax.text((x0 + x1) / 2, -ls / 2 + 1.6, "wetted zone", ha="center", va="center", fontsize=8.2,
            color=style.INK)
    ax.text((x0 + x1) / 2, -ls / 2 - 1.6, "θ = θ_s", ha="center", va="center", fontsize=8.2, color=style.INK)
    ax.text((x0 + x1) / 2, -22.0, "unwetted zone", ha="center", va="center", fontsize=8.2, color=style.INK)
    ax.text((x0 + x1) / 2, -25.2, "θ = θ_i", ha="center", va="center", fontsize=8.2, color=style.INK)
    ax.add_patch(FancyArrowPatch(((x0 + x1) / 2 - 9.0, -2.0), ((x0 + x1) / 2 - 9.0, -ls - 4.5),
                                 arrowstyle="-|>", mutation_scale=11, color=style.WATER, lw=1.6, zorder=4))
    ax.text((x0 + x1) / 2 - 8.0, -ls - 4.5, "front advances\nas F grows", ha="left", va="center",
            fontsize=7.2, color=style.WATER, linespacing=1.3)

    def dim(x, ya, yb, text):
        ax.add_patch(FancyArrowPatch((x, ya), (x, yb), arrowstyle="<|-|>", mutation_scale=8,
                                     color=style.INK, lw=0.9, shrinkA=0, shrinkB=0))
        ax.text(x + 1.2, (ya + yb) / 2, text, ha="left", va="center", fontsize=8.0, color=style.INK)

    dim(x1 + 3.0, 0.0, pond, "d")
    dim(x1 + 3.0, 0.0, -ls, "L_s")
    dim(x1 + 9.0, -ls, -ls - 9.0, "ψ_s")
    ax.plot([x1, x1 + 10.5], [-ls, -ls], color=style.MUTED, lw=0.6, ls=":")
    ax.text(x1 + 10.0, -ls - 10.4, "suction head\nat the front", ha="left", va="top", fontsize=7.2,
            color=style.MUTED, linespacing=1.3)
    ax.text(x0, 6.8, "f_p = K_s (d + L_s + ψ_s) / L_s     (4-26)", ha="left", va="center", fontsize=8.2,
            color=style.INK)
    ax.set_title("(a) The two zones", loc="left", fontsize=9.5)


def _panel_b(ax, style):
    style.style_ax(ax)
    td = THETA_S - THETA_I
    ls_in = F_CUM / td
    zmax = 2.0 * ls_in
    ax.fill_betweenx([0.0, ls_in], THETA_I, THETA_S, color="#7fb3e6", alpha=0.6, lw=0)
    ax.plot([THETA_S, THETA_S, THETA_I, THETA_I], [0.0, ls_in, ls_in, zmax], color=style.INK, lw=1.8)
    ax.axvline(THETA_I, color=style.MUTED, lw=0.7, ls=":")
    ax.set_xlim(0.0, 0.6)
    ax.set_ylim(zmax, 0.0)
    ax.set_xlabel("moisture content θ")
    ax.set_ylabel("depth below the surface (in)")
    ax.set_xticks([0.0, THETA_I, THETA_S])
    ax.set_xticklabels(["0", f"θ_i = {THETA_I:.2f}", f"θ_s = {THETA_S:.2f}"])
    ax.text((THETA_I + THETA_S) / 2, ls_in * 0.42, f"F = L_s θ_d\n= {F_CUM:.1f} in", ha="center",
            va="center", fontsize=8.0, color=style.INK, linespacing=1.35)
    ax.annotate("", xy=(THETA_I, ls_in * 0.80), xytext=(THETA_S, ls_in * 0.80),
                arrowprops=dict(arrowstyle="<|-|>", color=style.INK, lw=0.8, mutation_scale=8))
    ax.text((THETA_I + THETA_S) / 2, ls_in * 0.80 + 0.25, f"θ_d = {td:.2f}", ha="center", va="top",
            fontsize=7.6, color=style.INK)
    ax.plot([0.0, THETA_I], [ls_in, ls_in], color=style.MUTED, lw=0.6, ls=":")
    ax.plot([THETA_S, THETA_S + 0.015], [ls_in, ls_in], color=style.MUTED, lw=0.6, ls=":")
    ax.text(THETA_S + 0.02, ls_in, f"wetting front\nL_s = F / θ_d\n= {ls_in:.1f} in", ha="left", va="center",
            fontsize=7.4, color=style.INK, linespacing=1.35)
    fp = capacity(F_CUM)
    ax.text(THETA_I + 0.03, zmax - 0.35, f"with K_s = {KS:g} in/hr,\nψ_s = {PSI:g} in and d = 0:\n"
            f"f_p = K_s (1 + ψ_s θ_d / F)\n= {fp:.3f} in/hr", ha="left", va="bottom", fontsize=7.4,
            color=style.INK, linespacing=1.4)
    ax.set_title("(b) Moisture profile: F fills the deficit", loc="left", fontsize=9.5)
    return ls_in, fp


def _draw(style):
    fig = style.figure(9.0, 4.2)
    gs = fig.add_gridspec(1, 2, width_ratios=[1.25, 1.0], wspace=0.18, left=0.0, right=0.98,
                          top=0.92, bottom=0.12)
    _panel_a(fig.add_subplot(gs[0, 0]), style)
    out = _panel_b(fig.add_subplot(gs[0, 1]), style)
    return fig, out


def build(sink):
    import style
    fig, (ls_in, fp) = _draw(style)
    print(f"    F = {F_CUM} in, theta_d = {THETA_S - THETA_I:.2f}: L_s = {ls_in:.2f} in, f_p = {fp:.4f} in/hr")
    sink.save(fig, "hydrology_ch4_green_ampt_zones")
