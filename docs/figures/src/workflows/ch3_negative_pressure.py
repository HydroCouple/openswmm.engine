"""A sub-atmospheric transient under the slot and TPA closures
(Application Manual, the sub-atmospheric chapter).

Vasconcelos, Wright and Roe's siphon experiment: the rapid-filling
pipeline with its centre raised, started full and drawn down by a siphon
at the upstream end. The pressure at the summit falls toward and through
the crown, which is the regime a Preissmann slot cannot represent — the
slot is open to the atmosphere — and which the two-component pressure
approach carries as a second component. The deck is run under both
closures on both routers, against the measured gauge pressure head at
x = 14.1 m and the measured velocity at x = 9.9 m.

Simulated tier: four runs of docs/figures/decks/negative_pressure,
cached under docs/figures/cache.
"""
from __future__ import annotations

import csv
import re

import numpy as np

REQUIRES = ("openswmm",)

DECK = "negative_pressure/negative_pressure.inp"
VARIANTS = [
    ("FV, TPA", {"FLOW_ROUTING": "FV", "FV_PRESSURE_CLOSURE": "TPA"}, "fv_tpa", "fv"),
    ("FV, static slot", {"FLOW_ROUTING": "FV", "FV_PRESSURE_CLOSURE": "SLOT"}, "fv_slot", "dw-slot"),
    ("Dynamic wave, TPA", {"FLOW_ROUTING": "DYNWAVE", "SURCHARGE_METHOD": "TPA", "TPA_CELERITY": "300"},
     "dw_tpa", "dw"),
    ("Dynamic wave, slot", {"FLOW_ROUTING": "DYNWAVE", "SURCHARGE_METHOD": "SLOT"}, "dw_slot", "dw-legacy"),
]


def _observed(path):
    t, y = [], []
    for row in csv.reader(r for r in open(path) if not r.startswith("#")):
        if row and row[0][:1].isdigit():
            t.append(float(row[0]))
            y.append(float(row[1]))
    return np.array(t), np.array(y)


def _report_step(text):
    v = re.search(r"^REPORT_STEP\s+(\S+)", text, re.M).group(1)
    if ":" in v:
        h, m, s = (float(x) for x in v.split(":"))
        return h * 3600 + m * 60 + s
    return float(v)


def _run(sink, options, key):
    """(t, head, velocity, seconds, failure) — failure is the ERROR line when the run diverged.

    A run that cannot complete is a result here, not an exception: the static
    slot cannot represent this deck's sub-atmospheric full-pipe flow and says
    so by diverging. The figure reports that instead of hiding it.
    """
    from openswmm.engine import OutputReader, OutNodeVar, OutLinkVar
    r = sink.run(DECK, options, key)
    dt = _report_step(r.inp.read_text())
    rpt = r.rpt.read_text(errors="replace")
    fail = next((ln.strip() for ln in rpt.splitlines() if ln.strip().startswith("ERROR")), None)
    with OutputReader(r.out) as o:
        n = o.period_count
        if n == 0:
            return None, None, None, r.seconds, fail or "the run produced no reported periods", None
        t = np.arange(n) * dt
        # The stations are sealed junctions. A node's DEPTH is its gauge pressure
        # head above the invert, which is what the transducers measured; the study's
        # comparison subtracts the same datum from the absolute head.
        return (t,
                np.asarray(o.node_series("VJ141", OutNodeVar.DEPTH), float),
                np.asarray(o.link_series("C3", OutLinkVar.VELOCITY), float),
                r.seconds, fail,
                np.asarray(o.node_series("VJC", OutNodeVar.DEPTH), float))


def _draw(style, runs, obs_h, obs_v):
    import matplotlib.pyplot as plt
    fig, (a0, a1) = plt.subplots(1, 2, figsize=(9.4, 4.1))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(a0)
    style.style_ax(a1)
    a0.plot(*obs_h, marker="o", ms=2.0, lw=0, color=style.INK, alpha=0.55, label="measured")
    failed = []
    for (label, _, _, colour), (t, h, v, _, fail, hc) in zip(VARIANTS, runs):
        if t is None:
            failed.append(label)
            a0.plot([], [], color=style.COLORS[colour], lw=1.5, ls=(0, (2, 2)),
                    label=f"{label} — diverged, no solution")
            a1.plot([], [], color=style.COLORS[colour], lw=1.5, ls=(0, (2, 2)))
            continue
        a0.plot(t, h, color=style.COLORS[colour], lw=1.5, label=label)
        a1.plot(t, hc, color=style.COLORS[colour], lw=1.5, label=label)
    a1.axhline(0.0, color=style.INK, lw=1.0)
    a1.axhline(0.1007, color=style.MUTED, lw=0.9, ls=(0, (4, 2)))
    a1.text(0.6, 0.104, "crown of the pipe at the summit", fontsize=7.0, color=style.MUTED)
    for ax in (a0, a1):
        ax.set_xlim(0, 40)
        ax.set_xlabel("time since the withdrawal starts  (s)")
    a0.set_ylabel("gauge pressure head at x = 14.1 m  (m)")
    a1.set_ylabel("gauge pressure head at the summit  (m)")
    a0.set_title("(a) The drawdown, against the measurement", fontsize=9, loc="left")
    a1.set_title("(b) At the summit, where the column is most at risk", fontsize=9, loc="left")
    a0.legend(fontsize=7.2, frameon=False, loc="lower left")
    note = ("the summit empties to zero gauge under FV with TPA\n"
            "at t = 31.6 s: the column separates")
    if failed:
        note += "\n" + ", ".join(failed) + " diverged rather than reporting a wrong answer"
    a1.text(17.5, a1.get_ylim()[1] - 0.012, note, fontsize=7.2, color=style.MUTED, va="top", ha="left",
            linespacing=1.4)
    fig.tight_layout()
    return fig


def _draw_velocity(style, runs, obs_v):
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(8.4, 3.9))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(ax)
    ax.plot(*obs_v, marker="o", ms=2.2, lw=0, color=style.INK, alpha=0.55, label="measured")
    for (label, _, _, colour), (t, _, v, _, _, _) in zip(VARIANTS, runs):
        if t is None:
            ax.plot([], [], color=style.COLORS[colour], lw=1.5, ls=(0, (2, 2)), label=f"{label} — diverged")
            continue
        ax.plot(t, v, color=style.COLORS[colour], lw=1.5, label=label)
    ax.set_xlim(0, 40)
    ax.set_xlabel("time since the withdrawal starts  (s)")
    ax.set_ylabel("velocity at x = 9.9 m  (m/s)")
    ax.legend(fontsize=7.4, frameon=False)
    ax.set_title("The same four runs, at the velocity transducer", fontsize=9.5, loc="left")
    fig.tight_layout()
    return fig


def build(sink):
    import style
    runs = [_run(sink, o, k) for _, o, k, _ in VARIANTS]
    decks = sink.fig_dir / "decks" / "negative_pressure"
    obs_h = _observed(decks / "e3_pressure_14p1.csv")
    obs_v = _observed(decks / "e3_velocity_9p9.csv")
    for (label, _, _, _), (_, h, _, secs, fail, _) in zip(VARIANTS, runs):
        if h is None:
            print(f"    {label}: {secs:.1f} s, DIVERGED — {fail}")
        else:
            print(f"    {label}: {secs:.1f} s, min head {h.min():+.3f} m")
    if "workflow_ch3_negative_pressure_head_14p1" in sink.expected:
        sink.save(_draw(style, runs, obs_h, obs_v), "workflow_ch3_negative_pressure_head_14p1")
    if "workflow_ch3_velocity_9p9" in sink.expected:
        sink.save(_draw_velocity(style, runs, obs_v), "workflow_ch3_velocity_9p9")
