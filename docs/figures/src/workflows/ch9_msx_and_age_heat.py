"""Reactions, water age and heat on the transport demonstration deck
(Application Manual, the multi-species-reactions and age-and-heat
chapters).

Three figures from the same five-conduit line:

* what the computed water temperature does to the reaction system's rate,
  through the Arrhenius correction its expression carries — the species
  concentrations themselves are not written to the output file in this
  release, because [REACTION_REPORT] is a planned section;
* water age along the line, against the source ages the component
  declares;
* the water temperature against the air temperature driving it.

Simulated tier: runs of docs/figures/decks/transport_demo, cached under
docs/figures/cache.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ("openswmm",)

DECK = "transport_demo/transport_demo.inp"
FINE = {"REPORT_STEP": "0:01:00"}
LINE = ["J1", "J2", "J3", "J4", "J5"]


def _read(sink, options, key, species, nodes=("J5",), sidecars=None):
    from openswmm.engine import OutputReader, OutNodeVar
    r = sink.run(DECK, options, key, sidecars=sidecars)
    with OutputReader(r.out) as o:
        base = int(OutNodeVar.POLLUT_BASE)
        if species not in o.pollutant_ids:
            raise RuntimeError(f"{species} is not reported by this run: {o.pollutant_ids}")
        i = o.pollutant_ids.index(species)
        n = o.period_count
        out = {nd: np.asarray([o.node_attributes(nd, period=k)[base + i] for k in range(n)], float)
               for nd in nodes}
    return np.arange(n) / 60.0, out, r.seconds


def _draw_arrhenius(style, t_run, temp, kb=0.30, theta=1.07):
    """The Arrhenius correction the .rxn expression carries, and where this run sits on it."""
    import matplotlib.pyplot as plt
    fig, (a0, a1) = plt.subplots(1, 2, figsize=(9.2, 3.9))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(a0)
    style.style_ax(a1)
    T = np.linspace(0.0, 30.0, 400)
    mult = theta ** (T - 20.0)
    a0.plot(T, mult, color=style.COLORS["fv"], lw=2.0)
    a0b = a0.twinx()
    a0b.plot(T, np.log(2.0) / (kb * mult) * 60.0, color=style.COLORS["fv-lts"], lw=1.6, ls=(0, (4, 2)))
    a0b.set_ylabel("half-life of CL2  (minutes)", color=style.COLORS["fv-lts"], fontsize=8.5)
    a0b.tick_params(labelsize=7.5, colors=style.COLORS["fv-lts"])
    for x, lab, col in ((12.0, "initial state, 12 °C", style.INK),
                        (18.0, "inflow, 18 °C", style.COLORS["dw"]),
                        (20.0, "the fallback, 20 °C", style.MUTED)):
        a0.axvline(x, color=col, lw=0.9, ls=(0, (2, 2)))
        a0.text(x, 1.62, lab, rotation=90, fontsize=7.0, color=col, ha="right", va="top")
    a0.set_xlabel("water temperature TEMP  (°C)")
    a0.set_ylabel("rate multiplier  θ^(TEMP − 20)", color=style.COLORS["fv"], fontsize=8.5)
    a0.set_ylim(0, 1.75)
    a0.set_title("(a) The correction the expression carries", fontsize=9, loc="left")
    a0.text(0.4, 0.06, f"kb = 0.30 /hr and θ = 1.07, both from the .rxn file:\n"
            f"the rate changes by {100 * (theta - 1):.0f} % per degree",
            fontsize=7.4, color=style.MUTED, linespacing=1.45)
    # where this run actually sits, over its first half hour
    m = t_run <= 0.5
    cols = [style.COLORS["fv"], style.COLORS["dw"], style.COLORS["dw-slot"], style.COLORS["fv-lts"],
            style.COLORS["dw-legacy"]]
    for nd, c in zip(LINE, cols):
        a1.plot(t_run[m] * 60, temp[nd][m], color=c, lw=1.6, label=nd)
    a1.axhline(18.0, color=style.INK, lw=0.9, ls=(0, (1, 2)))
    a1.set_xlabel("time  (minutes)")
    a1.set_ylabel("water temperature  (°C)")
    a1.set_title("(b) Where this run sits, over its first half hour", fontsize=9, loc="left")
    a1.legend(fontsize=7.2, frameon=False, ncol=5, loc="lower right")
    a1.text(0.97, 0.42, "The line fills from empty, so the first minutes are\n"
            "a cold start: a nearly dry node carries a temperature\n"
            "that is not a result. By ten minutes every node is at\n"
            "the inflow temperature and the multiplier is 0.874.",
            transform=a1.transAxes, fontsize=7.2, color=style.MUTED, va="center", ha="right",
            linespacing=1.5)
    fig.tight_layout()
    return fig


def _draw_age(style, t, age):
    import matplotlib.pyplot as plt
    fig, (a0, a1) = plt.subplots(1, 2, figsize=(9.2, 3.8), gridspec_kw=dict(width_ratios=[1.3, 1.0]))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(a0)
    style.style_ax(a1)
    cols = [style.COLORS["fv"], style.COLORS["dw"], style.COLORS["dw-slot"], style.COLORS["fv-lts"],
            style.COLORS["dw-legacy"]]
    for nd, c in zip(LINE, cols):
        a0.plot(t * 60.0, age[nd], color=c, lw=1.6, label=nd)
    a0.axhline(0.5, color=style.MUTED, lw=0.9, ls=(0, (2, 2)))
    a0.text(1.0, 0.508, "EXTERNAL_INFLOW 0.5 h — the age the arriving water is given",
            fontsize=7.2, color=style.MUTED, va="bottom")
    a0.set_xlim(0, 20)
    a0.set_ylim(0, 0.68)
    a0.set_xlabel("time  (minutes)")
    a0.set_ylabel("water age  (hours)")
    a0.legend(fontsize=7.4, frameon=False, ncol=5, loc="center right")
    a0.set_title("(a) The line fills, and each node settles at its own age", fontsize=9, loc="left")
    a0.text(1.0, 0.015, "INITIAL_STATE 6.0 h never appears: the pipes start empty, so there is\n"
            "no standing water to carry it. A hot-started model would show it.",
            fontsize=7.2, color=style.MUTED, linespacing=1.45)
    # the steady profile at the end of the run
    final = [age[nd][-1] for nd in LINE]
    a1.plot(np.arange(len(LINE)) * 100.0, [f * 60 for f in final], marker="o", ms=5,
            color=style.COLORS["fv"], lw=1.8)
    a1.set_xlabel("distance from the inflow  (m)")
    a1.set_ylabel("water age at the end of the run  (minutes)")
    a1.set_title("(b) What is left is the travel time", fontsize=9, loc="left")
    a1.text(0.04, 0.93, f"{final[0] * 60:.1f} min at J1, the declared source age,\n"
            f"rising to {final[-1] * 60:.1f} min at J5", transform=a1.transAxes, fontsize=7.4,
            color=style.MUTED, va="top", linespacing=1.45)
    fig.tight_layout()
    return fig


def _draw_heat(style, t, temp, air_t, air_c):
    """The first hour, where everything happens, with the six-hour behaviour stated."""
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(8.8, 4.1))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(ax)
    tm = t * 60.0
    m = tm <= 60.0
    air_min = np.linspace(0, 60, 61)
    ax.plot(air_min, np.interp(air_min / 60.0, air_t, air_c), color=style.MUTED, lw=1.5, ls=(0, (5, 2)),
            label="air temperature, from the [TEMPERATURE] series")
    cols = [style.COLORS["fv"], style.COLORS["dw"], style.COLORS["dw-slot"], style.COLORS["fv-lts"],
            style.COLORS["dw-legacy"]]
    for nd, c in zip(LINE, cols):
        ax.plot(tm[m], temp[nd][m], color=c, lw=1.6, label=f"water at {nd}")
    ax.axhline(18.0, color=style.INK, lw=0.9, ls=(0, (1, 2)))
    ax.text(59.5, 18.25, "inflow temperature, 18 °C", fontsize=7.2, color=style.INK, ha="right")
    ax.axhline(12.0, color=style.INK, lw=0.7, ls=(0, (1, 3)))
    ax.text(59.5, 11.3, "initial state, 12 °C", fontsize=7.2, color=style.INK, ha="right")
    ax.set_xlim(0, 60)
    ax.set_xlabel("time  (minutes)")
    ax.set_ylabel("temperature  (°C)")
    ax.legend(fontsize=7.2, frameon=False, ncol=2, loc="lower right")
    ax.set_title("Water temperature along the line, with surface and radiative exchange on",
                 fontsize=9.5, loc="left")
    last = max(float(temp[nd][-1]) for nd in LINE)
    ax.text(30.0, 6.2, "Everything happens in the first ten minutes: the line fills and each node\n"
            f"reaches the inflow temperature. For the remaining five hours the water stays at\n"
            f"{last:.2f} °C while the air rises to {air_c[-1]:.0f} °C — a 35-minute residence in 500 m of\n"
            "pipe is far too short for the surface to move it. Heat here is transport, not exchange.",
            fontsize=7.4, color=style.MUTED, linespacing=1.5, ha="center")
    fig.tight_layout()
    return fig


def build(sink):
    import style
    t_tmp, temp, _ = _read(sink, dict(FINE), "heat_on", "__TEMPERATURE__", tuple(LINE))
    print("    water temperature at the end (degC): " + ", ".join(f"{nd} {temp[nd][-1]:.2f}" for nd in LINE))
    t_age, age, _ = _read(sink, dict(FINE), "heat_on", "__WATER_AGE__", tuple(LINE))
    print("    water age at the end (min): " + ", ".join(f"{nd} {age[nd][-1] * 60:.1f}" for nd in LINE))
    air_t = np.array([0.0, 3.0, 6.0])
    air_c = np.array([12.0, 20.0, 22.0])
    if "workflow_ch9_arrhenius_rate_from_temperature" in sink.expected:
        sink.save(_draw_arrhenius(style, t_tmp, temp), "workflow_ch9_arrhenius_rate_from_temperature")
    if "workflow_ch10_water_age_along_line" in sink.expected:
        sink.save(_draw_age(style, t_age, age), "workflow_ch10_water_age_along_line")
    if "workflow_ch10_heat_diurnal" in sink.expected:
        sink.save(_draw_heat(style, t_tmp, temp, air_t, air_c), "workflow_ch10_heat_diurnal")
