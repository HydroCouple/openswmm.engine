"""A filling pipe under the dynamic wave and finite-volume routers
(Application Manual, the filling-pipe chapter).

The deck is Vasconcelos, Wright and Roe's 2006 rapid-filling experiment,
run three ways with nothing changed but the routing and surcharge keys:
the finite-volume router with its static slot, the dynamic wave router
with EXTRAN surcharge, and the dynamic wave router with the Preissmann
slot. The measured pressure head, velocity and surge-tank level of the
laboratory pipe are plotted under all three.

Simulated tier: each variant is one run of docs/figures/decks/rapid_fill,
cached under docs/figures/cache.
"""
from __future__ import annotations

import csv
import re

import numpy as np

REQUIRES = ("openswmm",)

DECK = "rapid_fill/rapid_fill.inp"
VARIANTS = [
    ("FV, static slot", {"FLOW_ROUTING": "FV"}, "fv_slot", "fv"),
    ("Dynamic wave, EXTRAN", {"FLOW_ROUTING": "DYNWAVE", "SURCHARGE_METHOD": "EXTRAN"}, "dw_extran", "dw-legacy"),
    ("Dynamic wave, slot", {"FLOW_ROUTING": "DYNWAVE", "SURCHARGE_METHOD": "SLOT"}, "dw_slot", "dw-slot"),
]


def _observed(path):
    t, y = [], []
    with open(path, newline="") as fh:
        for row in csv.reader(r for r in fh if not r.startswith("#")):
            if not row or not row[0].replace(".", "", 1).replace("-", "", 1).isdigit():
                continue
            t.append(float(row[0]))
            y.append(float(row[1]))
    return np.array(t), np.array(y)


def _report_step(inp_text):
    m = re.search(r"^REPORT_STEP\s+(\S+)", inp_text, re.M)
    v = m.group(1)
    if ":" in v:                                     # hh:mm:ss
        h, mi, s = (float(x) for x in v.split(":"))
        return h * 3600 + mi * 60 + s
    return float(v)


def _series(sink, options, key):
    """(t, head at 9.9 m, velocity at 9.9 m, surge-tank depth) for one variant."""
    from openswmm.engine import OutputReader, OutNodeVar, OutLinkVar
    run = sink.run(DECK, options, key)
    dt = _report_step(run.inp.read_text())
    with OutputReader(run.out) as r:
        n = r.period_count
        t = np.arange(n) * dt
        return (t,
                np.asarray(r.node_series("VJ99", OutNodeVar.HEAD), dtype=float),
                np.asarray(r.link_series("C2", OutLinkVar.VELOCITY), dtype=float),
                np.asarray(r.node_series("ST", OutNodeVar.DEPTH), dtype=float),
                run.seconds)


def _draw_pressure(style, runs, obs_t, obs_h):
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(8.4, 4.2))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(ax)
    ax.plot(obs_t, obs_h, marker="o", ms=2.2, lw=0, color=style.INK, alpha=0.55,
            label="measured, Vasconcelos et al. 2006")
    for (label, _, _, colour), (t, h, _, _, _) in zip(VARIANTS, runs):
        ax.plot(t, h, color=style.COLORS[colour], lw=1.6, label=label)
    ax.set_xlim(0, 15)
    ax.set_ylim(0.0, 0.50)                 # the measured range; see the note on the EXTRAN excursion
    ax.set_xlabel("time since the inflow starts  (s)")
    ax.set_ylabel("pressure head at x = 9.9 m  (m)")
    ax.legend(fontsize=7.6, frameon=False, loc="upper left")
    ax.set_title("The bore arrives and the pipe fills: pressure head at the transducer",
                 fontsize=9.5, loc="left")
    peak = max(float(h.max()) for _, h, _, _, _ in runs)
    ax.text(0.15, 0.004,
            "The arrival time is the bore speed and the rise is the closure's stiffness. Outside this window\n"
            f"the EXTRAN run reaches {peak:.1f} m at t = 30.7 s — a spurious surcharge excursion forty times the\n"
            "physical head, absent from both slot closures.",
            fontsize=7.4, color=style.MUTED, va="bottom", ha="left", linespacing=1.45)
    fig.tight_layout()
    return fig


def _draw_front(style, runs, obs_v, obs_st):
    import matplotlib.pyplot as plt
    fig, (a0, a1) = plt.subplots(1, 2, figsize=(9.2, 3.8))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(a0)
    style.style_ax(a1)
    a0.plot(obs_v[0], obs_v[1], marker="o", ms=2.0, lw=0, color=style.INK, alpha=0.5, label="measured")
    a1.plot(obs_st[0], obs_st[1], marker="o", ms=2.0, lw=0, color=style.INK, alpha=0.5, label="measured")
    for (label, _, _, colour), (t, _, v, st, _) in zip(VARIANTS, runs):
        a0.plot(t, v, color=style.COLORS[colour], lw=1.5, label=label)
        a1.plot(t, st, color=style.COLORS[colour], lw=1.5, label=label)
    a0.set_xlim(0, 40)
    a1.set_xlim(0, 40)
    a0.set_xlabel("time  (s)")
    a1.set_xlabel("time  (s)")
    a0.set_ylabel("velocity at x = 9.9 m  (m/s)")
    a1.set_ylabel("surge-tank level  (m)")
    a0.set_title("(a) Velocity: the front passing the transducer", fontsize=9, loc="left")
    a1.set_title("(b) Surge tank: where the filled pipe puts the water", fontsize=9, loc="left")
    a0.legend(fontsize=7.2, frameon=False)
    fig.tight_layout()
    return fig


def build(sink):
    import style
    runs = [_series(sink, opts, key) for _, opts, key, _ in VARIANTS]
    decks = sink.fig_dir / "decks" / "rapid_fill"
    obs_h = _observed(decks / "e2_pressure_9p9.csv")
    obs_v = _observed(decks / "e2_velocity_9p9.csv")
    obs_st = _observed(decks / "e2_surgetank_level.csv")
    for label, (_, _, _, _, secs) in zip((v[0] for v in VARIANTS), runs):
        print(f"    {label}: {secs:.1f} s")
    if "workflow_ch2_rapid_fill_pressure_9p9" in sink.expected:
        sink.save(_draw_pressure(style, runs, *obs_h), "workflow_ch2_rapid_fill_pressure_9p9")
    if "workflow_ch2_rapid_fill_front" in sink.expected:
        sink.save(_draw_front(style, runs, obs_v, obs_st), "workflow_ch2_rapid_fill_front")
