"""The static slot closure of the finite-volume solver (hydraulics Chapter
8, §8.4.1–8.4.2), drawn from the engine's own circular-pipe geometry.

Left: the top width T(h) of a 3 ft circular pipe. Below the crown cutoff
y_c it is the section's own width; across [y_c, y_full] the slot opens
through the C1 ramp φ(s) = s²(3 − 2s); above y_full it is the slot width
T_slot = g A_full / c_slot², capped at 5 % of the section's maximum width.
Right: the resulting celerity c = √(g A / T). Without the taper the
celerity would blow up at the crown of a closed section; with it the
celerity rises smoothly to the requested slot celerity. Requests below the
cap-implied celerity are inert: the 30 ft/s curve is byte-identical to the
cap. REQUIRES openswmm (XSectionGeometry); no model run.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ("openswmm",)

G_FT = 32.174
D = 3.0                       # ft, circular
CROWN_CUTOFF = 0.985257       # y_c / y_full, Chapter 3
CELERITIES = (30.0, 50.0, 100.0, 200.0)   # ft/s; 30 falls under the 5 % cap


def _closure(width_fn, yfull, afull, wmax, c_slot, h):
    """(T, A) along h for the tapered static slot at celerity c_slot."""
    t_slot = min(G_FT * afull / c_slot ** 2, 0.05 * wmax)
    yc = CROWN_CUTOFF * yfull
    T = np.empty_like(h)
    below = h <= yfull
    T[below] = width_fn(h[below])
    s = np.clip((h - yc) / (yfull - yc), 0.0, 1.0)
    T += t_slot * s ** 2 * (3.0 - 2.0 * s)
    T[~below] = t_slot
    A = np.concatenate([[0.0], np.cumsum(0.5 * (T[1:] + T[:-1]) * np.diff(h))])
    return T, A, t_slot


def _draw(style, geom):
    import matplotlib.pyplot as plt
    yfull, afull = geom.full_depth, geom.full_area
    wmax = float(np.max(geom.width(np.linspace(0.0, yfull, 801))))
    h = np.linspace(1e-4, 1.35 * yfull, 4000)
    fig, (a0, a1) = plt.subplots(1, 2, figsize=(9.2, 3.8))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(a0)
    style.style_ax(a1)
    shades = [style.COLORS["dw-legacy"], style.COLORS["fv-lts"], style.COLORS["fv"], style.COLORS["dw"]]
    cap_c = np.sqrt(G_FT * afull / (0.05 * wmax))
    curves = []
    for c, col in zip(CELERITIES, shades):
        T, A, t_slot = _closure(geom.width, yfull, afull, wmax, c, h)
        lab = f"c_slot = {c:.0f} ft/s"
        if c < cap_c:
            lab += f"  (below the cap {cap_c:.0f} ft/s: inert)"
        curves.append((T, np.sqrt(G_FT * A / T), col, lab))
        a0.plot(T, h / yfull, color=col, lw=1.6, label=lab)
        a1.plot(curves[-1][1], h / yfull, color=col, lw=1.6)
    # the section without a slot: the width collapses at the crown and the celerity diverges as
    # 1/sqrt(y_full - h), so sample the last decades densely and use the engine's own area
    hb = np.concatenate([np.linspace(1e-4, 0.97 * yfull, 800), yfull * (1.0 - np.logspace(-1.5, -7, 300))])
    T0 = geom.width(hb)
    c0 = np.sqrt(G_FT * geom.area(hb) / np.maximum(T0, 1e-12))
    a0.plot(T0, hb / yfull, color=style.MUTED, lw=1.0, ls=(0, (3, 2)), label="section alone (no slot)")
    a1.plot(c0, hb / yfull, color=style.MUTED, lw=1.2, ls=(0, (3, 2)), zorder=5)
    for ax in (a0, a1):
        ax.axhline(1.0, color=style.INK, lw=0.7, ls=(0, (2, 2)))
        ax.axhline(CROWN_CUTOFF, color=style.INK, lw=0.5, ls=(0, (1, 2)))
        ax.set_ylim(0, 1.35)
    a0.text(wmax * 0.98, 1.012, "y_full (crown)", fontsize=7.5, ha="right", color=style.INK)
    a0.text(wmax * 0.98, CROWN_CUTOFF - 0.05, "y_c = 0.985 y_full", fontsize=7.5, ha="right", color=style.INK)
    a0.set_xlabel("top width T  (ft)")
    a0.set_ylabel("depth  h / y_full")
    a0.set_title("(a) Tapered slot mouth: T(h) for a 3 ft circular pipe", fontsize=9, loc="left")
    a0.legend(fontsize=7, loc="center left", bbox_to_anchor=(0.02, 0.57), frameon=False)
    # inset (a): the mouth
    ins = a0.inset_axes([0.12, 0.06, 0.30, 0.26])
    m = (h / yfull > 0.975) & (h / yfull < 1.06)
    for T, _, col, _ in curves:
        ins.plot(T[m], h[m] / yfull, color=col, lw=1.4)
    ins.set_xlim(0, 0.18)
    ins.set_ylim(0.975, 1.06)
    ins.set_title("the mouth, zoomed", fontsize=7, color=style.MUTED)
    # inset (b): the crown, where the untapered celerity diverges
    ins2 = a1.inset_axes([0.52, 0.10, 0.44, 0.40])
    m2 = (h / yfull > 0.96) & (h / yfull < 1.04)
    for _, cel, col, _ in curves:
        ins2.plot(cel[m2], h[m2] / yfull, color=col, lw=1.4)
    mb = (hb / yfull > 0.96)
    ins2.plot(c0[mb], hb[mb] / yfull, color=style.MUTED, lw=1.2, ls=(0, (3, 2)), zorder=5)
    ins2.set_xscale("log")
    ins2.set_xlim(10, 600)
    ins2.set_ylim(0.96, 1.04)
    ins2.set_title("the crown, zoomed", fontsize=7, color=style.MUTED)
    for i in (ins, ins2):
        i.axhline(1.0, color=style.INK, lw=0.6, ls=(0, (2, 2)))
        i.axhline(CROWN_CUTOFF, color=style.INK, lw=0.5, ls=(0, (1, 2)))
        i.tick_params(labelsize=6.5)
        i.set_facecolor(style.SURFACE)
    a1.set_xscale("log")
    a1.set_xlim(3, 600)
    a1.set_xlabel("celerity c = √(g A / T)  (ft/s)")
    a1.set_title("(b) Celerity rises smoothly to c_slot instead of diverging at the crown", fontsize=9, loc="left")
    a1.text(3.4, 1.31, "T_slot = g A_full / c_slot²\ncapped at 0.05 W_max\ndashed: the section alone,\nno slot", fontsize=7.4,
            color=style.MUTED, va="top", linespacing=1.25)
    fig.tight_layout()
    return fig


def build(sink):
    import style
    from openswmm import engine as e
    geom = e.XSectionGeometry(e.XSectShape.CIRCULAR, D, units="US")
    sink.save(_draw(style, geom), "hydraulics_ch8_slot_closure")
