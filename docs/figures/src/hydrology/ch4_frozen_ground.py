"""Frozen ground: the enthalpy slab, its infiltration limit and its heat contacts
(hydrology Figure 4-9, Section 4.7).

(a) Slab temperature T and frozen fraction phi against the enthalpy H at the
    default slab, showing the 0 C plateau where the freezable water changes
    phase (Equations 4-52 to 4-55).
(b) The infiltration factor F_o = (1 - phi)^m for m = 2, 11 (the default) and
    25 (Equation 4-57).
(c) The slab and its four linear heat contacts, with each conductance
    evaluated at the defaults (the contact table of Section 4.7.1).

Synthetic tier. Every function is a port of the engine's own code in
src/engine/hydrology/FrozenGround.hpp: the constants Lf, rho_w and c_w
(:67-69), the default slab (:73-84), latentStore/sensibleStore (:93-100),
temperatureOf (:108-113), frozenFraction (:115-120), infiltrationFactor
(:123-126), halfSlabConductance (:129-131), snowContactConductance
(:134-137) and deepConductance (:140-142). The four contacts and their
temperatures are those of advanceSubcatchments in
src/engine/hydrology/FrozenGround.cpp:140-145. The snow water equivalent and
infiltration rate used to evaluate panel (c) are illustrative values, named
in the panel.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()

# FrozenGround.hpp:67-69
LATENT_FUSION = 334000.0      # J/kg
WATER_DENSITY = 1000.0        # kg/m3
WATER_CAPACITY = 4186000.0    # J/(m3 K)

# FrozenGround.hpp:73-84, the Slab defaults (also the table of Section 4.7.4)
SLAB = dict(thickness=0.15, conductivity=1.5, heat_capacity=2.4e6, water_content=0.30,
            deep_temp=8.0, deep_depth=3.0, exponent=11.0, snow_conductivity=0.25,
            snow_density=250.0)

# illustrative states for evaluating the contacts in panel (c)
SNOW_SWE_M = 0.05             # 50 mm of snow water equivalent
INFIL_MM_PER_H = 5.0          # infiltration rate f


def latent_store(s):                      # FrozenGround.hpp:93-95
    return LATENT_FUSION * WATER_DENSITY * s["water_content"] * s["thickness"]


def sensible_store(s):                    # FrozenGround.hpp:98-100
    return s["heat_capacity"] * s["thickness"]


def temperature_of(s, h):                 # FrozenGround.hpp:108-113
    h = np.asarray(h, dtype=float)
    hf, c = latent_store(s), sensible_store(s)
    return np.where(h >= 0.0, h / c, np.where(h > -hf, 0.0, (h + hf) / c))


def frozen_fraction(s, h):                # FrozenGround.hpp:115-120
    h = np.asarray(h, dtype=float)
    hf = latent_store(s)
    return np.where(h >= 0.0, 0.0, np.where(h > -hf, -h / hf, 1.0))


def infiltration_factor(phi, m):          # FrozenGround.hpp:123-126, written in phi
    open_ = 1.0 - np.asarray(phi, dtype=float)
    return np.where(open_ <= 0.0, 0.0, np.where(open_ >= 1.0, 1.0, np.clip(open_, 0.0, 1.0) ** m))


def half_slab_conductance(s):             # FrozenGround.hpp:129-131
    return 2.0 * s["conductivity"] / s["thickness"]


def snow_contact_conductance(s, swe_m):   # FrozenGround.hpp:134-137
    depth = swe_m * WATER_DENSITY / s["snow_density"]
    return 1.0 / (1.0 / half_slab_conductance(s) + depth / s["snow_conductivity"])


def deep_conductance(s):                  # FrozenGround.hpp:140-142
    return s["conductivity"] / (s["deep_depth"] - 0.5 * s["thickness"])


def _panel_a(ax, style):
    s = SLAB
    hf, c = latent_store(s), sensible_store(s)
    h = np.linspace(-hf - 8.0 * c, 12.0 * c, 600)          # -8 C frozen to +12 C unfrozen
    t = temperature_of(s, h)
    phi = frozen_fraction(s, h)
    mj = h / 1e6
    style.style_ax(ax)
    ax.axvspan(-hf / 1e6, 0.0, color="#dbe9f7", alpha=0.7, lw=0, zorder=0)
    ax.plot(mj, t, color=style.COLORS["dw-legacy"], lw=2.0, label="temperature T (°C), left axis")
    ax.axhline(0.0, color=style.GRIDC, lw=0.8)
    ax.set_xlabel("slab enthalpy H (MJ/m²), from unfrozen ground at 0 °C")
    ax.set_ylabel("temperature T (°C)")
    ax.set_xlim(mj[0], mj[-1])
    ax.set_xticks([-15, -10, -5, 0])
    lo, hi = -9.0, 18.0
    ax.set_ylim(lo, hi)
    ax.set_yticks([-8, -4, 0, 4, 8, 12])
    ax2 = ax.twinx()
    ax2.plot(mj, phi, color=style.COLORS["fv"], lw=2.0, ls=(0, (5, 2)), label="frozen fraction φ, right axis")
    # phi = 0 level with T = 0, and phi = 1 level with T = 12 C
    ax2.set_ylim(lo / 12.0, hi / 12.0)
    ax2.set_yticks([0.0, 0.5, 1.0])
    ax2.set_ylabel("frozen fraction φ", color=style.MUTED)
    ax2.tick_params(colors=style.MUTED, labelsize=9)
    for side in ("top", "left", "bottom"):
        ax2.spines[side].set_visible(False)
    ax2.spines["right"].set_color(style.GRIDC)
    y = 16.8
    ax.text(-hf / 2e6, y, f"0 °C plateau: H_f = L_f ρ_w θ D = {hf / 1e6:.1f} MJ/m²", ha="center", va="top",
            fontsize=7.4, color=style.INK)
    ax.text((mj[0] - hf / 1e6) / 2, y, "frozen", ha="center", va="top", fontsize=7.4, color=style.MUTED)
    ax.text(mj[-1] / 2, y, "unfrozen", ha="center", va="top", fontsize=7.4, color=style.MUTED)
    lines = ax.get_lines()[:1] + ax2.get_lines()
    ax.legend(lines, [ln.get_label() for ln in lines], loc="lower right", fontsize=7.4)
    ax.set_title("(a) Temperature and frozen fraction follow from H", loc="left", fontsize=9.5)


def _panel_b(ax, style):
    style.style_ax(ax)
    phi = np.linspace(0.0, 1.0, 401)
    for m, col, ls in ((2.0, style.COLORS["dw"], "-"), (11.0, style.COLORS["fv"], "-"),
                       (25.0, style.COLORS["dw-slot"], (0, (5, 2)))):
        lab = f"m = {m:g}" + ("  (default)" if m == SLAB["exponent"] else "")
        ax.plot(phi, infiltration_factor(phi, m), color=col, lw=2.2 if m == 11 else 1.6, ls=ls, label=lab)
    half = 1.0 - 0.5 ** (1.0 / SLAB["exponent"])
    ax.plot([half, half], [0.0, 0.5], color=style.MUTED, lw=0.8, ls=":")
    ax.plot([0.0, half], [0.5, 0.5], color=style.MUTED, lw=0.8, ls=":")
    ax.annotate(f"with m = 11, infiltration is halved\nonce φ = {half:.3f}", xy=(half, 0.5),
                xytext=(0.40, 0.60), fontsize=7.4, color=style.MUTED, va="center", linespacing=1.4,
                arrowprops=dict(arrowstyle="-", color=style.MUTED, lw=0.7))
    ax.set_xlim(0.0, 1.0)
    ax.set_ylim(0.0, 1.03)
    ax.set_xlabel("frozen fraction φ of the slab's freezable water")
    ax.set_ylabel("infiltration factor F_o = (1 − φ)^m")
    ax.legend(loc="upper right", fontsize=7.6)
    ax.set_title("(b) Share of the pervious surface that still infiltrates", loc="left", fontsize=9.5)


def _panel_c(ax, style):
    from matplotlib.patches import FancyArrowPatch, Rectangle
    s = SLAB
    g_open = half_slab_conductance(s)
    g_snow = snow_contact_conductance(s, SNOW_SWE_M)
    g_deep = deep_conductance(s)
    g_water = WATER_CAPACITY * INFIL_MM_PER_H / 1000.0 / 3600.0
    snow_depth = SNOW_SWE_M * WATER_DENSITY / s["snow_density"]
    ax.set_facecolor(style.SURFACE)
    ax.set_xlim(0.0, 100.0)
    ax.set_ylim(0.0, 38.0)
    ax.axis("off")
    x0, x1, xs = 18.0, 82.0, 54.0             # slab ends; where the snow pack begins
    ys, yt = 21.0, 13.0                       # slab top and bottom
    ax.add_patch(Rectangle((xs, ys), x1 - xs, 4.5, fc="#ffffff", ec=style.MUTED, lw=1.0, hatch="..", zorder=2))
    ax.text(73.0, ys + 2.25, "snow pack (fraction a_s)", ha="center", va="center",
            fontsize=7.4, color=style.INK, zorder=3, bbox=dict(boxstyle="round,pad=0.2", fc="#ffffff", ec="none"))
    ax.plot([x0, xs], [ys, ys], color=style.INK, lw=1.4, zorder=3)
    ax.text(x0 - 1.0, ys + 0.6, "open surface\n(fraction 1 − a_s)", ha="right", va="bottom", fontsize=7.4,
            color=style.MUTED, linespacing=1.35)
    ax.add_patch(Rectangle((x0, yt), x1 - x0, ys - yt, fc="#dbe9f7", ec=style.INK, lw=1.1, zorder=2))
    ax.text(64.5, (ys + yt) / 2, "slab: one enthalpy H\nper unit area", ha="left", va="center",
            fontsize=7.6, color=style.INK, zorder=3, linespacing=1.35)
    ax.text(x1 + 1.0, (ys + yt) / 2, f"D = {s['thickness']:g} m\nk = {s['conductivity']:g} W/m·K\n"
            f"C = {s['heat_capacity'] / 1e6:g} MJ/m³·K", ha="left", va="center", fontsize=7.4,
            color=style.MUTED, linespacing=1.35)
    yd = 4.0
    ax.add_patch(Rectangle((x0, yd), x1 - x0, yt - yd, fc=style.SOIL, ec="none", alpha=0.55, zorder=1))
    ax.plot([x0, x1], [yd, yd], color=style.INK, lw=0.9, ls=(0, (6, 3)), zorder=2)
    ax.text(x1 + 1.0, yd, f"deep ground: z_d = {s['deep_depth']:g} m,\nT_d = {s['deep_temp']:g} °C",
            ha="left", va="center", fontsize=7.4, color=style.INK, linespacing=1.35)

    def arrow(x, ya, yb, col):
        ax.add_patch(FancyArrowPatch((x, ya), (x, yb), arrowstyle="-|>", mutation_scale=11, color=col,
                                     lw=1.6, shrinkA=0, shrinkB=0, zorder=4))

    heat, water = "#d6336c", style.WATER
    ytop, ymid = 37.0, (ys + yt) / 2 + 0.6
    arrow(25.0, 27.5, ymid, heat)
    ax.text(x0 + 1.0, ytop, "1  open surface\nG = (1 − a_s) 2k/D\n"
            f"2k/D = {g_open:.1f} W/m²·K\nat T_ponded, or T_air", ha="left", va="top", fontsize=7.4,
            color=style.INK, linespacing=1.35)
    arrow(44.0, 27.5, ymid, water)
    ax.text(37.0, ytop, "4  infiltrating water\nG = ρ_w c_w f\n"
            f"= {g_water:.2f} W/m²·K at {INFIL_MM_PER_H:g} mm/h\nat max(T_open, 0 °C)", ha="left",
            va="top", fontsize=7.4, color=water, linespacing=1.35)
    arrow(62.0, 27.5, ymid, heat)
    ax.text(xs + 2.0, ytop, "2  snow-covered surface, the pack in series\nG = a_s (D/2k + d_s/k_s)⁻¹\n"
            f"= {g_snow:.2f} W/m²·K × a_s for {SNOW_SWE_M * 1000:.0f} mm SWE (d_s = {snow_depth:.2f} m)\n"
            "at min(T_air, 0 °C)", ha="left", va="top", fontsize=7.4, color=style.INK, linespacing=1.35)
    arrow(32.0, yd, (ys + yt) / 2 - 0.6, heat)
    ax.text(33.5, (yd + yt) / 2, f"3  deep ground   G = k/(z_d − D/2) = {g_deep:.3f} W/m²·K, at T_d",
            ha="left", va="center", fontsize=7.4, color=style.INK)
    ax.text(x0, 0.6, "Each contact adds G (T_contact − T) to dH/dt. Conductances are evaluated at the default slab.",
            ha="left", va="bottom", fontsize=7.4, color=style.MUTED)
    ax.set_title("(c) The slab and its four heat contacts", loc="left", fontsize=9.5)
    return g_open, g_snow, g_deep, g_water


def _draw(style):
    fig = style.figure(9.6, 7.4)
    gs = fig.add_gridspec(2, 2, height_ratios=[1.0, 1.1], hspace=0.38, wspace=0.42,
                          left=0.07, right=0.93, top=0.95, bottom=0.02)
    _panel_a(fig.add_subplot(gs[0, 0]), style)
    _panel_b(fig.add_subplot(gs[0, 1]), style)
    g = _panel_c(fig.add_subplot(gs[1, :]), style)
    return fig, g


def build(sink):
    import style
    fig, (g_open, g_snow, g_deep, g_water) = _draw(style)
    print(f"    H_f = {latent_store(SLAB) / 1e6:.3f} MJ/m2; CD = {sensible_store(SLAB) / 1e6:.3f} MJ/m2/K; "
          f"G open {g_open:.2f}, snow {g_snow:.3f}, deep {g_deep:.4f}, water {g_water:.3f} W/m2/K; "
          f"F_o halves at phi = {1.0 - 0.5 ** (1.0 / SLAB['exponent']):.4f} (m = 11)")
    sink.save(fig, "hydrology_ch4_frozen_ground")
