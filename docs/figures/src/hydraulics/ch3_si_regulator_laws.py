"""The three regulator laws that semi-implicit node continuity changes
(Hydraulics Reference, Chapter 3, Section 3.5).

Each column is one row of the chapter's table, as the engine computes it
under NODE_CONTINUITY EXPLICIT (the SWMM 5 law, kept bit for bit) and under
SEMI_IMPLICIT. The top row is the flow, the bottom row the dQ/dH the link
hands to the node head update. All quantities are in ft and cfs, the
engine's internal units; the linearization head delta is SI_SMOOTH_HEAD =
0.05 ft (src/engine/hydraulics/HydStructures.cpp:87).

(a) A transverse weir, free flow Q = Cd L H^1.5 (HydStructures.cpp:1126-1129),
    submerged by Villemonte's factor (1 - (H2/H1)^1.5)^0.385, Equation 6-26
    (HydStructures.cpp:105-120, :1324-1356). Its dQ/dH under EXPLICIT is the
    free-flow 1.5 Q/H1 taken before the factor (:1291-1306); the dotted
    line is the law's true sensitivity to the downstream head, Q |dF/dH2|,
    which grows without bound as H2 -> H1. Under SEMI_IMPLICIT the factor
    is evaluated at a head difference of at least delta, the flow runs
    linearly to zero below it, and dQ/dH is the larger of the factored
    free-flow slope and Q |dF/dH2| (or the slope of the line below delta).
(b) A circular bottom orifice whose downstream head stands above the
    opening, so its head is the head difference (Equation 6-6;
    HydStructures.cpp:774-781). Below H* = (Cd/0.414)(D/4) (Equations
    6-10 to 6-12; :749) it runs as a weir, above it as an orifice
    (:848-858). EXPLICIT applies Equation 6-13 on top of the head
    difference (:894-898), so the flow jumps by 1/f_S at H*; SEMI_IMPLICIT
    does not apply the factor to this orifice (:899).
(c) An outlet whose rating starts at a nonzero flow (a flow regulator
    rated 1 cfs at every head) and one with a power below one
    (Q = 2 He^0.5). EXPLICIT rates any head above 0.0001 ft (:1469) and
    hands on dQ/dH = 0; SEMI_IMPLICIT runs the flow linearly from zero to
    the rated value at delta, with that line's slope as dQ/dH
    (:1501-1536).

The structure sizes are illustrative (a 2 ft weir under 0.5 ft of head, a
1 ft orifice with Cd 0.65 under 0.5 ft of tailwater submergence); the laws
and constants are the engine's. Synthetic tier: numpy only.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()

DELTA = 0.05                 # SI_SMOOTH_HEAD, ft
G = 32.2                     # src/engine/core/Constants.hpp:52
EXPLICIT, SEMI = "#e87ba4", "#2a78d6"


def _villemonte(r, p):
    return np.power(1.0 - np.power(r, p), 0.385)


def _villemonte_slope(r, p, h1):
    r = np.asarray(r, float)
    out = np.zeros_like(r)
    ok = (r > 0.0) & (r < 1.0)
    out[ok] = 0.385 * np.power(1.0 - np.power(r[ok], p), -0.615) * p * np.power(r[ok], p - 1.0) / h1
    return out


def weir(dh, *, cd=3.33, length=2.0, h1=0.5, p=1.5):
    """Transverse weir with H1 fixed over the crest and H2 = H1 - dh (both above the crest)."""
    qf = cd * length * h1 ** 1.5
    dq_free = 1.5 * qf / h1
    r = (h1 - dh) / h1
    q_x = qf * _villemonte(r, p)
    slope_x = qf * _villemonte_slope(r, p, h1)
    d = min(DELTA, h1)
    r_s = np.where(dh >= d, r, (h1 - d) / h1)
    f_s = _villemonte(r_s, p)
    dq_s = np.maximum(dq_free * f_s, qf * _villemonte_slope(r_s, p, h1))
    q_s = qf * f_s
    below = dh < d
    dq_s = np.where(below, q_s / d, dq_s)
    q_s = np.where(below, q_s * dh / d, q_s)
    return q_x, np.full_like(dh, dq_free), slope_x, q_s, dq_s


def bottom_orifice(dh, *, cd=0.65, diam=1.0, sub=0.5):
    """Circular bottom orifice, fully open, tailwater `sub` ft above the opening; head = dh."""
    area = np.pi * diam * diam / 4.0
    f_area = area * np.sqrt(2.0 * G)
    c_orif = cd * f_area
    h_crit = max((cd / 0.414) * (diam / 4.0), 1.0e-4)
    c_weir = cd * np.sqrt(h_crit) * f_area
    f = np.minimum(dh / h_crit, 1.0)
    weir_regime = f < 1.0
    q = np.where(weir_regime, c_weir * np.power(f, 1.5), c_orif * np.sqrt(np.maximum(dh, 0.0)))
    with np.errstate(divide="ignore", invalid="ignore"):
        dq = np.where(weir_regime, 1.5 * q / (f * h_crit), q / (2.0 * dh))
    ratio = sub / (sub + dh)                     # (H2 - Z_O) / (H1 - Z_O)
    q_x = np.where(weir_regime, q * _villemonte(ratio, 1.5), q)
    return q_x, dq, q, dq, h_crit


def outlet(he, rating):
    """Outlet flow and dQ/dH at effective head `he` for a rating function, setting 1."""
    q_x = np.where(he > 1.0e-4, rating(he), 0.0)
    q_d = rating(np.array([DELTA]))[0]
    below = he < DELTA
    q_s = np.where(below, q_d * he / DELTA, rating(he))
    q_s = np.where(he > 1.0e-4, q_s, 0.0)
    dq_s = np.where(below & (he > 1.0e-4), q_d / DELTA, 0.0)
    return q_x, np.zeros_like(he), q_s, dq_s


def _draw(style):
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(2, 3, figsize=(10.0, 6.2), sharex="col",
                             gridspec_kw={"height_ratios": [1.0, 0.85]})
    fig.patch.set_facecolor(style.SURFACE)
    for ax in axes.flat:
        style.style_ax(ax)
    # (a) the submerged weir
    dh = np.linspace(0.0, 0.25, 1201)
    q_x, dq_x, slope_x, q_s, dq_s = weir(dh)
    a, b = axes[0, 0], axes[1, 0]
    a.plot(dh, q_x, color=EXPLICIT, lw=1.7)
    a.plot(dh, q_s, color=SEMI, lw=1.7, ls=(0, (5, 2)))
    b.plot(dh, dq_x, color=EXPLICIT, lw=1.7)
    b.plot(dh[1:], slope_x[1:], color=EXPLICIT, lw=1.2, ls=(0, (1, 1.5)))
    b.plot(dh, dq_s, color=SEMI, lw=1.7, ls=(0, (5, 2)))
    b.set_ylim(0, 60)
    b.text(0.012, 56, "the law's true slope\nQ |dF/dH$_2$|, unbounded", fontsize=7.2, color=EXPLICIT,
           va="top", linespacing=1.3)
    b.text(0.24, 13.5, "EXPLICIT reports the free-flow\n1.5 Q/H$_1$, taken before the factor",
           fontsize=7.2, color=EXPLICIT, ha="right", va="bottom", linespacing=1.3)
    a.set_title("(a) Submerged weir, Villemonte factor\n2 ft transverse weir, H$_1$ = 0.5 ft over the crest",
                fontsize=8.6, loc="left")
    b.set_xlabel("head difference H$_1$ − H$_2$  (ft)")
    # (b) the bottom orifice in its weir regime
    dh = np.linspace(1.0e-4, 0.8, 1601)
    q_x, dq_x, q_s, dq_s, h_crit = bottom_orifice(dh)
    a, b = axes[0, 1], axes[1, 1]
    jump = dh < h_crit
    for mask in (jump, ~jump):
        a.plot(dh[mask], q_x[mask], color=EXPLICIT, lw=1.7)
    a.plot(dh, q_s, color=SEMI, lw=1.7, ls=(0, (5, 2)))
    k = int(np.argmax(~jump))
    a.annotate("", xy=(h_crit, q_x[k]), xytext=(h_crit, q_x[k - 1]),
               arrowprops=dict(arrowstyle="->", color=EXPLICIT, lw=1.0))
    f_s = float(_villemonte(np.array(0.5 / (0.5 + h_crit)), 1.5))   # Equation 6-13 at H*
    a.text(h_crit + 0.02, (q_x[k] + q_x[k - 1]) / 2, f"jump × 1/f$_S$ = {1.0 / f_s:.2f}",
           fontsize=7.2, color=EXPLICIT, va="center")
    for mask in (jump, ~jump):
        b.plot(dh[mask], dq_x[mask], color=EXPLICIT, lw=1.7)
        b.plot(dh[mask], dq_s[mask], color=SEMI, lw=1.7, ls=(0, (5, 2)))
    for ax in (a, b):
        ax.axvline(h_crit, color=style.MUTED, lw=0.8, ls=(0, (2, 2)))
    a.text(h_crit - 0.015, 0.25, f"H* = {h_crit:.3f} ft", fontsize=7.2, color=style.MUTED, ha="right")
    b.text(0.78, 13.0, "weir regime below H*, orifice above;\nthe same dQ/dH under both settings",
           fontsize=7.2, color=style.MUTED, ha="right", va="top", linespacing=1.3)
    b.set_ylim(0, 14)
    a.set_title("(b) Bottom orifice, tailwater above the opening\n1 ft circular, Cd 0.65, tailwater 0.5 ft "
                "over it", fontsize=8.6, loc="left")
    b.set_xlabel("head difference H$_1$ − H$_2$  (ft)")
    # (c) outlets whose rating starts above zero or rises as a power below one
    he = np.linspace(0.0, 0.15, 1501)
    a, b = axes[0, 2], axes[1, 2]
    ratings = [("flow regulator, 1 cfs at any head", lambda h: np.full_like(np.asarray(h, float), 1.0), "-"),
               ("Q = 2 H$_e^{0.5}$", lambda h: 2.0 * np.sqrt(np.maximum(np.asarray(h, float), 0.0)),
                (0, (1, 1.2)))]
    for name, fn, ls in ratings:
        q_x, dq_x, q_s, dq_s = outlet(he, fn)
        a.plot(he, q_x, color=EXPLICIT, lw=1.7 if ls == "-" else 1.4, ls=ls)
        a.plot(he, q_s, color=SEMI, lw=1.7 if ls == "-" else 1.4, ls=ls if ls != "-" else (0, (5, 2)))
        b.plot(he, dq_x, color=EXPLICIT, lw=1.7)
        b.plot(he, dq_s, color=SEMI, lw=1.7 if ls == "-" else 1.4, ls=ls if ls != "-" else (0, (5, 2)))
    a.text(0.148, 1.08, "flow regulator, 1 cfs", fontsize=7.2, color=style.MUTED, ha="right", va="bottom")
    a.text(0.148, 0.50, "Q = 2 H$_e^{0.5}$  (dotted)", fontsize=7.2, color=style.MUTED, ha="right",
           va="top")
    b.text(0.148, 1.5, "EXPLICIT: dQ/dH = 0 at every head", fontsize=7.2, color=EXPLICIT, ha="right")
    a.set_ylim(0, 1.3)
    b.set_ylim(-0.8, 22)
    a.set_title("(c) Outlet rating at small heads\nrating curve from zero head", fontsize=8.6, loc="left")
    b.set_xlabel("effective head H$_e$  (ft)")
    for ax in axes.flat:
        ax.axvline(DELTA, color=SEMI, lw=0.8, ls=(0, (2, 2)), alpha=0.8)
    for ax in axes[0]:
        lo, hi = ax.get_ylim()
        ax.text(DELTA + 0.004 * (ax.get_xlim()[1] / 0.25), lo + 0.04 * (hi - lo), "δ = 0.05 ft",
                fontsize=7.2, color=SEMI)
    axes[0, 0].set_ylabel("flow Q  (cfs)")
    axes[1, 0].set_ylabel("dQ/dH handed to the\nnode update  (cfs/ft)")
    handles = [plt.Line2D([], [], color=EXPLICIT, lw=1.7),
               plt.Line2D([], [], color=SEMI, lw=1.7, ls=(0, (5, 2)))]
    fig.legend(handles, ["NODE_CONTINUITY EXPLICIT: the SWMM 5 law", "NODE_CONTINUITY SEMI_IMPLICIT"],
               loc="upper center", ncol=2, fontsize=8, frameon=False, bbox_to_anchor=(0.5, 1.0))
    fig.tight_layout(rect=(0, 0, 1, 0.955), h_pad=1.2, w_pad=1.6)
    return fig


def build(sink):
    import style
    dh = np.array([1.0e-3, 0.01, DELTA, 0.1])
    q_x, dq_x, slope_x, q_s, dq_s = weir(dh)
    print("    weir H1-H2 (ft): " + "; ".join(
        f"{d:g}: Q {a:.3f}/{c:.3f} cfs, dQ/dH {b:.2f}/{e:.2f}, true slope {s:.1f}"
        for d, a, b, c, e, s in zip(dh, q_x, dq_x, q_s, dq_s, slope_x)) + "  (EXPLICIT/SEMI)")
    h_crit = (0.65 / 0.414) * 0.25
    q_x, _, q_s, _, _ = bottom_orifice(np.array([h_crit * (1 - 1e-9), h_crit]))
    print(f"    bottom orifice: H* {h_crit:.4f} ft; EXPLICIT {q_x[0]:.3f} -> {q_x[1]:.3f} cfs "
          f"(x{q_x[1] / q_x[0]:.3f}); SEMI {q_s[0]:.3f} -> {q_s[1]:.3f} cfs")
    q_x, _, q_s, dq_s = outlet(np.array([0.001, 0.02]), lambda h: np.full_like(np.asarray(h, float), 1.0))
    print(f"    flow regulator at He 0.001/0.02 ft: EXPLICIT {q_x[0]:.2f}/{q_x[1]:.2f} cfs, "
          f"SEMI {q_s[0]:.3f}/{q_s[1]:.3f} cfs with dQ/dH {dq_s[0]:.1f}")
    sink.save(_draw(style), "hydraulics_ch3_si_regulator_laws")
