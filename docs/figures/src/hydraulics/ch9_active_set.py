"""The flux-active set at a wetting front (hydraulics Chapter 9, §9.5.7).

A cell is flux-active only above `H_MOVE`, with a hysteresis band, and a
face flows only when both of its cells are active. A halo of inactive
cells is therefore kept around the active set so an advancing front always
has an active cell to flow into: one ring at the default cadence, and
`FRONT_REBUILD` widens it to five rings and marks the frontier ring, so
the rebuild can be deferred while the front crosses the halo. A cell of
the frontier whose depth crosses the activation threshold is a breach, and
forces the rebuild early. Inactive cells are not skipped but integrated
lazily: rainfall and held coupling accumulate as pure storage over the
whole interval, in one pass. Synthetic; no model run.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()

NX, NY = 22, 11
FRONT_X = 7.4       # the wet front's position, in cell widths
BREACH = (13, 4)    # a frontier cell that crosses h_on


def _classify():
    """state per cell: wet, halo1, halo5, frontier, dry — plus the breach."""
    st = np.full((NY, NX), "dry", dtype=object)
    for j in range(NY):
        # a curved front: wetter in the middle
        edge = FRONT_X + 2.6 * np.exp(-((j - (NY - 1) / 2) ** 2) / 9.0)
        for i in range(NX):
            if i < edge - 0.5:
                st[j, i] = "wet"
            elif i < edge + 0.5:
                st[j, i] = "halo1"
            elif i < edge + 4.5:
                st[j, i] = "halo5"
            elif i < edge + 5.5:
                st[j, i] = "frontier"
    st[BREACH[1], BREACH[0]] = "breach"
    return st


def _draw(style):
    import matplotlib.pyplot as plt
    from matplotlib.patches import Rectangle
    fig, (a0, a1) = plt.subplots(1, 2, figsize=(9.4, 4.0), gridspec_kw=dict(width_ratios=[1.62, 1.0]))
    fig.patch.set_facecolor(style.SURFACE)
    st = _classify()
    COL = {"wet": ("#2a78d6", 0.55, "flux-active  (h > H_MOVE)"),
           "halo1": ("#7fb0e8", 0.55, "one-ring halo — the default cadence"),
           "halo5": ("#cfe0f5", 0.85, "rings 2–5 — kept under FRONT_REBUILD"),
           "frontier": ("#eda100", 0.35, "the marked frontier ring"),
           "dry": ("#efeee9", 1.0, "dry, integrated lazily"),
           "breach": ("#e87ba4", 0.95, "a breach: this cell crossed h_on")}
    for j in range(NY):
        for i in range(NX):
            c, a, _ = COL[st[j, i]]
            a0.add_patch(Rectangle((i, j), 1, 1, fc=c, alpha=a, ec=style.SURFACE, lw=0.8))
    a0.annotate("", xy=(FRONT_X + 3.4, NY + 0.75), xytext=(FRONT_X - 0.6, NY + 0.75),
                arrowprops=dict(arrowstyle="-|>", color=style.INK, lw=1.4, mutation_scale=12))
    a0.text(FRONT_X + 1.4, NY + 0.95, "the front advances", fontsize=7.8, ha="center", color=style.INK)
    a0.plot([BREACH[0] + 0.5], [BREACH[1] + 0.5], marker="*", ms=12, mfc="white", mec=style.INK, mew=1.0, zorder=5)
    a0.annotate("breach — rebuild now", xy=(BREACH[0] + 0.5, BREACH[1] + 0.5), xytext=(BREACH[0] + 2.6, BREACH[1] - 2.6),
                fontsize=7.6, color=style.INK, arrowprops=dict(arrowstyle="->", color=style.INK, lw=0.9))
    a0.set_xlim(-0.4, NX + 0.4)
    a0.set_ylim(-0.6, NY + 1.5)
    a0.set_aspect("equal", adjustable="datalim")
    a0.axis("off")
    a0.set_title("(a) The active set and its halo, in plan", fontsize=9, loc="left")

    # legend and the hysteresis band
    y = 9.0
    for key in ("wet", "halo1", "halo5", "frontier", "breach", "dry"):
        c, a, lab = COL[key]
        a1.add_patch(Rectangle((0.1, y), 0.62, 0.42, fc=c, alpha=a, ec=style.SURFACE, lw=0.8))
        a1.text(0.88, y + 0.21, lab, fontsize=7.4, va="center", color=style.INK)
        y -= 0.72
    a1.text(0.1, 4.25, "A face flows only when both of its cells are active.", fontsize=7.6, color=style.INK)
    a1.text(0.1, 3.9, "A one-sided face exports volume into a cell whose update\n"
            "never runs — measured as an 18 % basin loss when it was allowed.",
            fontsize=7.2, color=style.MUTED, va="top", linespacing=1.45)
    # the hysteresis band
    h = np.linspace(0, 2.2, 400)
    hm, d = 1.0, 0.18
    a1.plot(h, np.where(h > hm + d, 1.0, 0.0), color=style.COLORS["fv"], lw=1.6)
    a1.plot(h, np.where(h > hm - d, 1.0, 0.0) * 0.0 + np.where(h > hm - d, 1.0, 0.0), color="none")
    a1.set_xlim(0, 9.4)
    a1.set_ylim(-0.6, 9.9)
    a1.axis("off")
    a1.set_title("(b) What the colours mean", fontsize=9, loc="left")
    # hysteresis inset
    ins = a1.inset_axes([0.08, 0.06, 0.86, 0.26])
    ins.plot([0, hm + d, hm + d, 2.2], [0, 0, 1, 1], color=style.COLORS["fv"], lw=1.6, label="entering: h_move + δ")
    ins.plot([2.2, hm - d, hm - d, 0], [1, 1, 0, 0], color=style.COLORS["fv-lts"], lw=1.6, ls=(0, (4, 2)),
             label="leaving: h_move − δ")
    ins.axvline(hm, color=style.MUTED, lw=0.8, ls=(0, (1, 2)))
    ins.set_xticks([hm])
    ins.set_xticklabels(["H_MOVE"], fontsize=6.6)
    ins.set_yticks([])
    ins.set_ylim(-0.15, 1.35)
    ins.legend(fontsize=6.2, frameon=False, loc="upper left")
    ins.set_title("δ = min(1 mm, H_MOVE / 2): the band scales with the threshold", fontsize=6.8, color=style.MUTED)
    ins.set_facecolor(style.SURFACE)
    fig.tight_layout(pad=0.35)
    return fig


def build(sink):
    import style
    sink.save(_draw(style), "hydraulics_ch9_active_set")
