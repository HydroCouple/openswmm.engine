"""Areal depletion curves through a melt season, and free-water routing through
the pack (hydrology Figures 6-5 and 6-6, Sections 6.4 and 6.5).

hydrology_ch6_adc_melt_progression (Figure 6-5)
    (a) Five areal depletion curves, ASC against AWESI = WSNOW / SI:
        A constant full cover, B the natural-area curve of Figure 6-4
        (ASC > AWESI), C ASC = AWESI, D ASC < AWESI (drifting or plowing),
        E constant 40 % cover.
    (b) The definition sketch: WSNOW * A_T = WS * AS (Equation 6-12), so
        AWESI = (WS / SI) * ASC (Equation 6-13).
    (c)-(g) For each curve, the pack at AWESI = 1.0, 0.8, 0.6 and 0.3: a
        cover of width ASC and actual depth WS = (AWESI / ASC) SI
        (Equation 6-14). The area of every sketch in a column is the same,
        AWESI * SI.
    Every ASC is evaluated with the engine's ten-point interpolation,
    getArealSnowCover in src/engine/hydrology/surface/DegreeDaySnow.cpp:114-123
    (zero at AWESI <= 0, one at AWESI >= 0.9999, ordinates at AWESI =
    0, 0.1, ..., 0.9 and the last interval running to 1.0). Curve B's
    ordinates are those of Figure 6-4 (ch6_adc_curves.py).

hydrology_ch6_snowpack_free_water (Figure 6-6)
    (a) The pack WSNOW and its free-water reservoir of capacity
        FWFRAC * WSNOW: melt SMELT and rain on the covered area enter the
        free water FW, and only the excess over the capacity leaves as
        runoff.
    (b), (c) One pack routed with the engine's own steps, meltElement in
        DegreeDaySnow.cpp:408-443: melt leaves WSNOW and joins FW with the
        rain on the pack, the excess FW - FWFRAC * WSNOW leaves, and a pack
        thinner than 0.001 in releases what is left (:409-419). The melt
        input is prescribed (two melt periods separated by a cold night), so
        the panels isolate the routing; the run has no rain.

Synthetic tier. Replaces two scanned SWMM 4 drawings (hydrology-image41,
hydrology-image43).
"""
from __future__ import annotations

import importlib.util
from pathlib import Path

import numpy as np

REQUIRES = ()

TIMES = (("initial, t₀", 1.0), ("t₁", 0.8), ("t₂", 0.6), ("t₃", 0.3))
CURVES = ("A", "B", "C", "D", "E")
CURVE_NOTE = {"A": "ASC = 1", "B": "ASC > AWESI", "C": "ASC = AWESI", "D": "ASC < AWESI", "E": "ASC = 0.4"}

# routing example (Figure 6-6); inches and hours
WSNOW0, FWFRAC, DT_H = 1.0, 0.10, 0.25
MELT = ((0.0, 8.0, 0.04), (8.0, 14.0, 0.0), (14.0, 40.0, 0.06))   # (from h, to h, in/hr)
T_END = 30.0


def _natural_adc():
    """Curve B: the natural-area ordinates of Figure 6-4, from ch6_adc_curves.py."""
    path = Path(__file__).with_name("ch6_adc_curves.py")
    spec = importlib.util.spec_from_file_location("manual_fig_ch6_adc_curves_ordinates", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    assert np.allclose(mod.AWESI[:10], np.arange(10) / 10.0)
    return [float(v) for v in mod.ASC[:10]]


def adc_tables():
    """Ten ordinates (ASC at AWESI = 0, 0.1, ..., 0.9) for each curve."""
    return {
        "A": [1.0] * 10,
        "B": _natural_adc(),
        "C": [i / 10.0 for i in range(10)],
        "D": [0.0, 0.02, 0.05, 0.10, 0.16, 0.25, 0.36, 0.49, 0.64, 0.81],
        "E": [0.4] * 10,
    }


def areal_snow_cover(adc, awesi):
    """Port of getArealSnowCover, DegreeDaySnow.cpp:114-123."""
    if awesi <= 0.0:
        return 0.0
    if awesi >= 0.9999:
        return 1.0
    m = int(awesi * 10.0 + 0.00001)
    asc1 = adc[m]
    asc2 = 1.0 if m >= 9 else adc[m + 1]
    return asc1 + (asc2 - asc1) / 0.1 * (awesi - 0.1 * float(m))


# ── Figure 6-5 ──────────────────────────────────────────────────────────────

def _curve_style(style):
    return {"A": (style.COLORS["dw-legacy"], "-"), "B": (style.COLORS["fv"], "-"),
            "C": (style.MUTED, (0, (5, 2))), "D": (style.COLORS["dw"], "-"),
            "E": (style.COLORS["dw-slot"], "-")}


def _panel_curves(ax, style, tables):
    style.style_ax(ax)
    cs = _curve_style(style)
    x = np.concatenate([[0.0], np.linspace(1e-4, 1.0, 1001)])
    for k in CURVES:
        col, ls = cs[k]
        y = [areal_snow_cover(tables[k], v) for v in x]
        ax.plot(x, y, color=col, ls=ls, lw=1.9, label=f"{k}   {CURVE_NOTE[k]}")
    for lab, a in TIMES[1:]:
        ax.axvline(a, color=style.MUTED, lw=0.7, ls=":")
        ax.text(a, 1.075, lab, ha="center", va="bottom", fontsize=7.6, color=style.MUTED)
        for k in CURVES:
            ax.plot([a], [areal_snow_cover(tables[k], a)], marker="o", ms=3.2, color=cs[k][0], zorder=5)
    ax.set_xlim(0.0, 1.02)
    ax.set_ylim(0.0, 1.07)
    ax.set_xlabel("AWESI = WSNOW / SI")
    ax.set_ylabel("ASC  (areal snow cover)")
    ax.legend(loc="lower right", fontsize=7.4, handlelength=2.2)
    ax.set_title("(a) Areal depletion curves", loc="left", fontsize=9.5, pad=14)


def _panel_definition(ax, style):
    from matplotlib.patches import FancyArrowPatch, Rectangle
    ax.set_facecolor(style.SURFACE)
    ax.set_xlim(-0.35, 1.25)
    ax.set_ylim(-1.05, 1.65)
    ax.axis("off")
    snow_fc, snow_ec = "#e6eef7", "#5b7fa6"
    ax.plot([0.0, 0.0, 1.0, 1.0], [1.25, 0.0, 0.0, 0.5], color=style.INK, lw=1.2)
    ax.add_patch(Rectangle((0.0, 0.0), 0.55, 0.8, fc=snow_fc, ec=snow_ec, lw=1.2))
    ax.plot([-0.04, 0.0], [1.0, 1.0], color=style.INK, lw=1.0)
    ax.plot([0.0, 1.0], [1.0, 1.0], color=style.MUTED, lw=0.7, ls=":")
    ax.text(-0.06, 1.0, "SI", ha="right", va="center", fontsize=8.0, color=style.INK)
    ax.add_patch(FancyArrowPatch((0.47, 0.0), (0.47, 0.8), arrowstyle="<|-|>", mutation_scale=8,
                                 color=style.INK, lw=0.9, shrinkA=0, shrinkB=0))
    ax.text(0.44, 0.4, "WS", ha="right", va="center", fontsize=8.0, color=style.INK)
    ax.text(0.27, 0.62, "snow pack", ha="center", va="center", fontsize=7.6, color=snow_ec)
    ax.text(0.78, 0.22, "bare\ncatchment", ha="center", va="center", fontsize=7.6, color=style.MUTED,
            linespacing=1.3)
    ax.text(0.55, -0.06, "AS", ha="center", va="top", fontsize=8.0, color=style.INK)
    ax.text(1.0, -0.06, "A_T", ha="center", va="top", fontsize=8.0, color=style.INK)
    ax.add_patch(FancyArrowPatch((-0.2, 0.1), (-0.2, 1.3), arrowstyle="-|>", mutation_scale=9,
                                 color=style.MUTED, lw=0.9))
    ax.text(-0.25, 0.7, "snow depth", ha="right", va="center", fontsize=7.4, color=style.MUTED, rotation=90)
    ax.add_patch(FancyArrowPatch((0.1, -0.3), (0.9, -0.3), arrowstyle="-|>", mutation_scale=9,
                                 color=style.MUTED, lw=0.9))
    ax.text(0.5, -0.36, "area", ha="center", va="top", fontsize=7.4, color=style.MUTED)
    ax.text(-0.3, -0.62, "WSNOW · A_T = WS · AS          (6-12)\n"
            "AWESI = WSNOW / SI = (WS / SI) · ASC          (6-13)", ha="left", va="top", fontsize=8.0,
            color=style.INK, linespacing=1.6)
    ax.set_title("(b) Definition sketch", loc="left", fontsize=9.5, pad=14)


def _panel_progression(ax, style, tables):
    """Rows (c)-(g): curves A-E; columns: the four times. Each row is as tall as its tallest pack."""
    from matplotlib.patches import Rectangle
    snow_fc, snow_ec = "#e6eef7", "#5b7fa6"
    pitch_x = 1.55
    table = {k: [(areal_snow_cover(tables[k], a), 0.0) for _, a in TIMES] for k in CURVES}
    for k in CURVES:
        table[k] = [(asc, a / asc) for (asc, _), (_, a) in zip(table[k], TIMES)]
    heights = [max(1.3, max(ws for _, ws in table[k])) + 0.62 for k in CURVES]
    head = 1.75
    total = head + sum(heights)
    ax.set_facecolor(style.SURFACE)
    ax.set_xlim(-1.45, 4 * pitch_x - 0.35)
    ax.set_ylim(-total, 0.0)
    ax.axis("off")
    ax.text(-1.45, -0.02, "(c)–(g) The pack under each curve as it melts: cover ASC (width) and actual depth "
            "WS (height)", ha="left", va="top", fontsize=9.5, color=style.INK)
    ax.annotate("", xy=(3 * pitch_x + 1.0, -0.62), xytext=(pitch_x, -0.62),
                arrowprops=dict(arrowstyle="-|>", color=style.MUTED, lw=0.9))
    ax.text(2.0 * pitch_x + 0.5, -0.55, "melt progressing", ha="center", va="bottom", fontsize=7.6,
            color=style.MUTED)
    for j, (lab, a) in enumerate(TIMES):
        ax.text(j * pitch_x + 0.5, -0.78, f"{lab}\nAWESI = {a:.1f}", ha="center", va="top", fontsize=7.8,
                color=style.INK, linespacing=1.35)
    top = -head
    out = {}
    for i, k in enumerate(CURVES):
        y0 = top - heights[i] + 0.42
        letter = "cdefg"[i]
        ax.text(-1.4, y0 + 0.95, f"({letter})  {k}", ha="left", va="center", fontsize=9.0, color=style.INK,
                fontweight="bold")
        ax.text(-1.4, y0 + 0.4, CURVE_NOTE[k], ha="left", va="center", fontsize=7.4, color=style.MUTED)
        for j, (_, a) in enumerate(TIMES):
            x0 = j * pitch_x
            asc, ws = table[k][j]
            out[(k, a)] = (asc, ws)
            ax.plot([x0, x0, x0 + 1.0, x0 + 1.0], [y0 + 1.25, y0, y0, y0 + 0.5], color=style.INK, lw=0.9)
            ax.plot([x0 - 0.05, x0], [y0 + 1.0, y0 + 1.0], color=style.INK, lw=0.9)
            if j == 0:
                ax.text(x0 - 0.08, y0 + 1.0, "SI", ha="right", va="center", fontsize=7.2, color=style.INK)
            ax.add_patch(Rectangle((x0, y0), asc, ws, fc=snow_fc, ec=snow_ec, lw=1.0, zorder=2))
            ax.text(x0 + 0.5, y0 - 0.1, f"ASC {asc:.2f},  WS {ws:.2f} SI", ha="center", va="top",
                    fontsize=7.0, color=style.MUTED)
        top -= heights[i]
    return out


def _draw_progression(style):
    tables = adc_tables()
    fig = style.figure(9.6, 9.8)
    gs = fig.add_gridspec(2, 2, height_ratios=[1.0, 1.75], width_ratios=[1.15, 1.0], hspace=0.1,
                          wspace=0.12, left=0.075, right=0.99, top=0.965, bottom=0.005)
    _panel_curves(fig.add_subplot(gs[0, 0]), style, tables)
    _panel_definition(fig.add_subplot(gs[0, 1]), style)
    table = _panel_progression(fig.add_subplot(gs[1, :]), style, tables)
    return fig, table


# ── Figure 6-6 ──────────────────────────────────────────────────────────────

def melt_input(t_h):
    for a, b, r in MELT:
        if a <= t_h < b:
            return r
    return 0.0


def route_pack():
    """meltElement's routing (DegreeDaySnow.cpp:408-443) for a prescribed melt, no rain, ASC = 1."""
    n = int(round(T_END / DT_H))
    wsnow, fw = WSNOW0, 0.0
    rows = []
    for s in range(n):
        t = s * DT_H
        smelt_in = melt_input(t)
        if wsnow <= 0.001:                     # :409-419, the last of the pack leaves at once
            out = (wsnow + fw) / DT_H
            wsnow = fw = smelt_in = 0.0
        else:
            vmelt = min(smelt_in * DT_H, wsnow)
            smelt_in = vmelt / DT_H
            wsnow -= vmelt
            fw += vmelt                        # + rain on the covered area, none here
            excess = max(fw - FWFRAC * wsnow, 0.0)
            fw -= excess
            out = excess / DT_H
        rows.append((t, t + DT_H, smelt_in, out, wsnow, fw))
    return np.array(rows)


def _panel_reservoir(ax, style):
    from matplotlib.patches import FancyArrowPatch, Rectangle
    ax.set_facecolor(style.SURFACE)
    ax.set_xlim(0.0, 10.0)
    ax.set_ylim(0.0, 11.0)
    ax.axis("off")
    snow_fc, snow_ec = "#e6eef7", "#5b7fa6"
    ax.add_patch(Rectangle((1.2, 1.2), 1.6, 7.6, fc=snow_fc, ec=snow_ec, lw=1.3))
    ax.text(2.0, 9.2, "snow pack", ha="center", va="bottom", fontsize=8.0, color=style.INK)
    ax.add_patch(FancyArrowPatch((0.7, 1.2), (0.7, 8.8), arrowstyle="<|-|>", mutation_scale=8,
                                 color=style.INK, lw=0.9, shrinkA=0, shrinkB=0))
    ax.text(0.55, 5.0, "WSNOW", ha="right", va="center", fontsize=8.0, color=style.INK, rotation=90)
    # free-water reservoir
    rx, ry, rw, rh = 5.6, 5.2, 2.0, 2.6
    ax.add_patch(Rectangle((rx, ry), rw, rh * 0.62, fc="#7fb3e6", ec="none", alpha=0.75))
    ax.plot([rx, rx, rx + rw, rx + rw], [ry + rh, ry, ry, ry + rh], color=style.INK, lw=1.2)
    ax.plot([rx, rx + rw], [ry + rh, ry + rh], color=style.INK, lw=0.8, ls=(0, (3, 2)))
    ax.text(rx + rw / 2, ry + rh + 0.15, "free water", ha="center", va="bottom", fontsize=8.0, color=style.INK)
    ax.add_patch(FancyArrowPatch((rx + 0.35, ry), (rx + 0.35, ry + rh * 0.62), arrowstyle="<|-|>",
                                 mutation_scale=7, color=style.INK, lw=0.8, shrinkA=0, shrinkB=0))
    ax.text(rx + 0.5, ry + rh * 0.31, "FW", ha="left", va="center", fontsize=8.0, color=style.INK)
    ax.add_patch(FancyArrowPatch((rx + rw + 0.35, ry), (rx + rw + 0.35, ry + rh), arrowstyle="<|-|>",
                                 mutation_scale=7, color=style.INK, lw=0.8, shrinkA=0, shrinkB=0))
    ax.text(rx + rw + 0.5, ry + rh / 2, "capacity\nFWFRAC · WSNOW", ha="left", va="center", fontsize=7.6,
            color=style.INK, linespacing=1.3)
    # inflows and outflow
    ax.add_patch(FancyArrowPatch((2.9, 3.0), (rx - 0.1, ry + 1.2), arrowstyle="-|>", mutation_scale=11,
                                 color=style.WATER, lw=1.6, connectionstyle="arc3,rad=-0.25"))
    ax.text(3.65, 3.2, "melt SMELT", ha="left", va="top", fontsize=7.8, color=style.WATER)
    ax.add_patch(FancyArrowPatch((rx + 0.6, 10.6), (rx + 0.6, ry + rh * 0.62 + 0.1), arrowstyle="-|>",
                                 mutation_scale=11, color=style.WATER, lw=1.4, ls=(0, (4, 2))))
    ax.text(rx + 0.8, 10.6, "rain on the\ncovered area", ha="left", va="top", fontsize=7.6, color=style.WATER,
            linespacing=1.3)
    ax.add_patch(FancyArrowPatch((rx + rw / 2, ry - 0.05), (rx + rw / 2 + 0.4, 1.4), arrowstyle="-|>",
                                 mutation_scale=11, color=style.COLORS["fv-lts"], lw=1.6,
                                 connectionstyle="arc3,rad=0.2"))
    ax.text(rx + rw / 2 + 0.6, 2.4, "excess over the capacity\n= runoff from the pack", ha="left",
            va="center", fontsize=7.8, color=style.COLORS["fv-lts"], linespacing=1.3)
    ax.text(0.2, 0.25, "Only free water above the capacity leaves. Without melt or rain the\n"
            "capacity holds still, so nothing leaves (hours 8 to 14 in (b)).",
            ha="left", va="bottom", fontsize=7.4, color=style.MUTED, linespacing=1.35)
    ax.set_title("(a) The pack and its free water", loc="left", fontsize=9.5)


def _draw_free_water(style):
    r = route_pack()
    t0, t1, rin, rout, wsnow, fw = r.T
    fig = style.figure(9.6, 5.0)
    gs = fig.add_gridspec(2, 2, width_ratios=[1.0, 1.25], height_ratios=[1.0, 1.0], hspace=0.42,
                          wspace=0.24, left=0.0, right=0.92, top=0.93, bottom=0.11)
    _panel_reservoir(fig.add_subplot(gs[:, 0]), style)
    ax1 = fig.add_subplot(gs[0, 1])
    ax2 = fig.add_subplot(gs[1, 1], sharex=ax1)
    style.style_ax(ax1)
    style.style_ax(ax2)
    edges = np.append(t0, t1[-1])
    ax1.stairs(rin, edges, color=style.WATER, lw=1.6, label="melt entering the free water")
    ax1.stairs(rout, edges, color=style.COLORS["fv-lts"], lw=1.6, label="runoff leaving the pack")
    ax1.set_ylabel("rate (in/hr)")
    ax1.set_ylim(0.0, max(rout.max(), rin.max()) * 1.3)
    ax1.legend(loc="upper left", fontsize=7.4)
    ax1.tick_params(labelbottom=False)
    ax1.set_title("(b) Melt in and runoff out", loc="left", fontsize=9.5)
    cap = FWFRAC * wsnow
    tt = np.concatenate([[0.0], t1])
    ax2.plot(tt, np.concatenate([[0.0], fw]), color=style.COLORS["fv"], lw=1.8, label="free water FW")
    ax2.plot(tt, np.concatenate([[FWFRAC * WSNOW0], cap]), color=style.INK, lw=1.0, ls=(0, (4, 2)),
             label="capacity FWFRAC · WSNOW")
    ax2.set_ylabel("depth (in)")
    ax2.set_ylim(0.0, FWFRAC * WSNOW0 * 1.25)
    ax2.set_xlabel("time (hours)")
    ax2.set_xlim(0.0, T_END)
    ax2.legend(loc="upper right", fontsize=7.4)
    ax2.set_title(f"(c) Free water: WSNOW starts at {WSNOW0:g} in, FWFRAC = {FWFRAC:g}", loc="left", fontsize=9.5)
    for ax in (ax1, ax2):
        ax.axvspan(8.0, 14.0, color=style.GRIDC, alpha=0.45, lw=0)
    ax1.text(11.0, ax1.get_ylim()[1] * 0.04, "no melt", ha="center", va="bottom", fontsize=7.4,
             color=style.MUTED)
    first = float(t1[np.argmax(rout > 0.0)])
    gone = float(t1[np.argmax((wsnow <= 0.0) & (np.arange(len(wsnow)) > 0))])
    return fig, dict(first_runoff_h=first, pack_gone_h=gone, fw_max=float(fw.max()),
                     rout_day1=float(rout[(t0 >= 4.0) & (t0 < 8.0)].mean()),
                     melt_in=float(sum(rin * DT_H)), out=float(sum(rout * DT_H)))


def build(sink):
    import style
    fig, table = _draw_progression(style)
    print("    ASC / WS(SI): " + "; ".join(
        f"{k}@{a:g} {table[(k, a)][0]:.3f}/{table[(k, a)][1]:.2f}" for k in CURVES for _, a in TIMES[1:]))
    sink.save(fig, "hydrology_ch6_adc_melt_progression")
    fig, info = _draw_free_water(style)
    print("    free water: " + ", ".join(f"{k} {v:.4f}" for k, v in info.items()))
    sink.save(fig, "hydrology_ch6_snowpack_free_water")
