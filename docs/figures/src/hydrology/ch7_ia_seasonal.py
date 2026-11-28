"""Seasonal behaviour of the exponential-decay initial-abstraction model
(hydrology Figure 7-11, §7.6).

A one-year synthetic forcing — random storms and a sinusoidal temperature —
drives the available initial abstraction IA_avail: each storm depletes it
in proportion to rainfall, dry weather restores it at a temperature-
dependent rate, and recovery is suspended while the ground is frozen. The
RDII response is the rainfall in excess of the abstraction. Schematic:
deterministic seed, engine-free, so the seasonal pattern the section
describes is visible without a project.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()


def _draw(style):
    import matplotlib.pyplot as plt
    rng = np.random.default_rng(7)
    days = np.arange(365)
    temp = 10 + 15 * np.sin(2 * np.pi * (days - 100) / 365)
    rain = np.where(rng.random(365) < 0.25, rng.gamma(2.0, 6.0, 365), 0.0)
    ia_max, ia = 25.0, 12.0
    k_dep, r_rec, ddf, t_freeze = 0.08, 0.6, 0.12, 0.0
    ia_series, rdii = [], []
    for d in days:
        ia = max(ia - k_dep * rain[d] * ia / ia_max * 3, 0.0)
        rec = 0.0 if temp[d] < t_freeze else r_rec * (1 + ddf * max(temp[d], 0)) * 0.15
        ia = min(ia + rec, ia_max)
        ia_series.append(ia)
        rdii.append(max(rain[d] - (ia / ia_max) * rain[d], 0.0) * 0.12)
    fig, axes = plt.subplots(3, 1, figsize=(8.6, 6.0), sharex=True)
    fig.patch.set_facecolor(style.SURFACE)
    for ax in axes:
        style.style_ax(ax)
    axes[0].bar(days, rain, color=style.WATER, width=1.0, label="rainfall")
    ax0b = axes[0].twinx()
    ax0b.plot(days, temp, color=style.COLORS["fv-lts"], lw=1.3, label="air temperature")
    ax0b.axhline(t_freeze, color=style.COLORS["fv-lts"], ls=(0, (2, 2)), lw=0.8)
    ax0b.set_ylabel("temperature (°C)", color=style.COLORS["fv-lts"])
    ax0b.tick_params(colors=style.MUTED, labelsize=9)
    for s in ("top",):
        ax0b.spines[s].set_visible(False)
    axes[0].set_ylabel("rainfall (mm/d)")
    frozen = temp < t_freeze
    axes[1].fill_between(days, 0, ia_max * 1.1, where=frozen, color=style.GRIDC, alpha=0.7,
                         label="frozen ground: recovery suspended")
    axes[1].plot(days, ia_series, color=style.COLORS["dw"], lw=1.6, label="available initial abstraction IA_avail")
    axes[1].axhline(ia_max, color=style.MUTED, ls=(0, (3, 2)), lw=0.8)
    axes[1].text(3, ia_max + 0.6, "IA_max", fontsize=8, color=style.MUTED)
    axes[1].set_ylabel("IA_avail (mm)")
    axes[1].set_ylim(0, ia_max * 1.15)
    axes[1].legend(loc="upper center", fontsize=7.5, ncol=2)
    axes[2].plot(days, rdii, color=style.COLORS["fv"], lw=1.3)
    axes[2].fill_between(days, 0, rdii, color=style.COLORS["fv"], alpha=0.15)
    axes[2].set_ylabel("RDII response\n(schematic)")
    axes[2].set_xlabel("day of year")
    axes[2].set_xlim(0, 364)
    for ax, t in zip(axes, ("forcing: storms deplete the abstraction, warmth restores it",
                            "the abstraction store: depletion by rain, temperature-dependent recovery, frozen-ground hold",
                            "RDII: the same storm yields more inflow when the store is depleted or frozen")):
        ax.set_title(t, fontsize=8.6, loc="left", color=style.INK)
    fig.tight_layout()
    return fig


def build(sink):
    import style
    sink.save(_draw(style), "hydrology_ch7_ia_seasonal")
