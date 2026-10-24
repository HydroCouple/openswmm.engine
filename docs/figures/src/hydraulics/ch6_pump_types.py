"""The five pump curve types of hydraulics Table 6-1, one tile each.

Type 1  flow in steps of inlet-node volume        Type 2  flow in steps of inlet depth
Type 3  head–flow characteristic (centrifugal)     Type 4  flow varying continuously with depth
Type 5  a Type 3 curve shifted by speed setting
Synthetic curves in the engine's conventions; replace 190 px icons.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()

TILES = [("hydraulics_ch6_pump_type1", "Type 1: flow versus inlet volume", "inlet node volume", "pump flow", "step_v"),
         ("hydraulics_ch6_pump_type2", "Type 2: flow versus inlet depth", "inlet node depth", "pump flow", "step_d"),
         ("hydraulics_ch6_pump_type3", "Type 3: head versus flow", "pump flow", "head difference", "hq"),
         ("hydraulics_ch6_pump_type4", "Type 4: flow versus inlet depth", "inlet node depth", "pump flow", "cont"),
         ("hydraulics_ch6_pump_type5", "Type 5: head versus flow, by speed", "pump flow", "head difference", "hq_speed")]


def _tile(fig_id, title, xl, yl, kind, style):
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(3.0, 2.2))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(ax)
    c = style.COLORS["fv"]
    if kind in ("step_v", "step_d"):
        x = [0, 1, 1, 2, 2, 3, 3, 4]
        y = [0, 0, 1.5, 1.5, 2.6, 2.6, 3.4, 3.4]
        ax.plot(x, y, color=c, lw=2.0)
        for xi, yi in zip([1, 2, 3], [1.5, 2.6, 3.4]):
            ax.plot([xi], [yi], marker="o", ms=3.5, color=c)
    elif kind == "hq":
        q = np.linspace(0, 4, 100)
        ax.plot(q, 4 - 0.18 * q ** 2, color=c, lw=2.0)
        ax.plot([0, 1.4, 2.6, 3.6], [4, 3.65, 2.8, 1.65], marker="o", ms=3.5, color=c, lw=0)
    elif kind == "cont":
        d = np.linspace(0, 4, 100)
        ax.plot(d, 3.6 * (1 - np.exp(-1.1 * d)), color=c, lw=2.0)
        ax.plot([0.5, 1.2, 2.2, 3.5], 3.6 * (1 - np.exp(-1.1 * np.array([0.5, 1.2, 2.2, 3.5]))), marker="o", ms=3.5, color=c, lw=0)
    else:
        q = np.linspace(0, 4, 100)
        for s, cc in ((1.0, c), (0.8, style.COLORS["dw"]), (0.6, style.COLORS["dw-slot"])):
            ax.plot(q * s, s * s * (4 - 0.18 * (q) ** 2), color=cc, lw=1.8, label=f"speed {s:g}")
        ax.legend(fontsize=6, loc="upper right")
    ax.set_xlabel(xl, fontsize=7)
    ax.set_ylabel(yl, fontsize=7)
    ax.set_xticks([])
    ax.set_yticks([])
    ax.set_xlim(0, 4.2)
    ax.set_ylim(0, 4.4)
    ax.set_title(title, fontsize=7.6, loc="left", color=style.INK)
    fig.tight_layout(pad=0.3)
    return fig


def build(sink):
    import style
    for fig_id, title, xl, yl, kind in TILES:
        if fig_id in sink.expected:
            sink.save(_tile(fig_id, title, xl, yl, kind, style), fig_id)
