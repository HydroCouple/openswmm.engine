"""RDII unit hydrographs (hydrology Figure 7-4 and the Engine Manual's
Figure 1-7).

A single RTK unit hydrograph is a triangle with peak at time T, base
T(1 + K) and area R (the fraction of rainfall that becomes RDII). The
engine sums three of them — short, medium and long response — for each
unit of rainfall. Both figures are the triangles drawn from (R, T, K);
they replace two low-resolution EPA bitmaps.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()

# (R, T hours, K) — a typical short / medium / long set
UH = [(0.030, 1.0, 2.0), (0.040, 4.0, 3.0), (0.030, 12.0, 4.0)]


def _triangle(t, R, T, K):
    """Ordinates of the unit hydrograph (per unit rainfall depth, per hour) with area R."""
    base = T * (1 + K)
    peak = 2 * R / base
    return np.where(t <= T, peak * t / T, np.where(t <= base, peak * (base - t) / (base - T), 0.0))


def _single(style):
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(6.2, 3.4))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(ax)
    R, T, K = 0.05, 3.0, 2.0
    t = np.linspace(0, T * (1 + K) + 1, 400)
    q = _triangle(t, R, T, K)
    ax.fill_between(t, 0, q, color=style.COLORS["fv"], alpha=0.18)
    ax.plot(t, q, color=style.COLORS["fv"], lw=2.2)
    ax.axvline(T, color=style.MUTED, lw=0.8, ls=(0, (3, 2)))
    ax.axvline(T * (1 + K), color=style.MUTED, lw=0.8, ls=(0, (3, 2)))
    ymax = q.max()
    ax.annotate("", xy=(T, -0.06 * ymax), xytext=(0, -0.06 * ymax), arrowprops=dict(arrowstyle="<->", color=style.INK, lw=0.9))
    ax.text(T / 2, -0.09 * ymax, "T  (time to peak)", fontsize=8, ha="center", va="top", color=style.INK)
    ax.annotate("", xy=(T * (1 + K), -0.06 * ymax), xytext=(T, -0.06 * ymax), arrowprops=dict(arrowstyle="<->", color=style.INK, lw=0.9))
    ax.text(T + T * K / 2, -0.09 * ymax, "K · T  (recession)", fontsize=8, ha="center", va="top", color=style.INK)
    ax.text(T * 0.55, ymax * 0.45, "area = R\n(fraction of rainfall\nbecoming RDII)", fontsize=8, ha="center",
            color=style.COLORS["fv"])
    ax.set_xlabel("time since the unit of rainfall (h)")
    ax.set_ylabel("RDII flow per unit rainfall")
    ax.set_xlim(0, T * (1 + K) + 1)
    ax.set_ylim(-0.22 * ymax, 1.15 * ymax)
    ax.set_yticks([])
    fig.tight_layout()
    return fig


def _three(style):
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(7.2, 3.8))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(ax)
    t = np.linspace(0, 65, 800)
    total = np.zeros_like(t)
    colours = [style.COLORS["fv"], style.COLORS["dw"], style.COLORS["fv-lts"]]
    names = ["short-term response  (R1, T1, K1)", "intermediate response  (R2, T2, K2)", "long-term response  (R3, T3, K3)"]
    for (R, T, K), c, name in zip(UH, colours, names):
        q = _triangle(t, R, T, K)
        total += q
        ax.plot(t, q, color=c, lw=1.6, label=name)
        ax.fill_between(t, 0, q, color=c, alpha=0.10)
    ax.plot(t, total, color=style.INK, lw=2.2, label="total RDII unit hydrograph (sum)")
    ax.set_xlabel("time since the unit of rainfall (h)")
    ax.set_ylabel("RDII flow per unit rainfall per unit area")
    ax.set_xlim(0, 65)
    ax.set_yticks([])
    ax.legend(loc="upper right", fontsize=7.5)
    fig.tight_layout()
    return fig


def build(sink):
    import style
    if "eng_rdii_unit_hydrograph" in sink.expected:
        sink.save(_single(style), "eng_rdii_unit_hydrograph")
    if "hydrology_ch7_rtk_unit_hydrographs" in sink.expected:
        sink.save(_three(style), "hydrology_ch7_rtk_unit_hydrographs")
