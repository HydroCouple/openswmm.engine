"""The three transport engines on one tracer front (Application Manual,
the transport-engines chapter).

A step of conservative tracer enters a five-conduit line at t = 0 and
travels 500 m to the outfall. The three engines selected by
`[OPTIONS] QUALITY_SOLVER` are run on the same deck, with the reporting
step shortened so the front is resolved rather than stepped over, and the
Eulerian engine is then run again at three cell sizes to show what
`TARGET_DX` buys and costs.

Simulated tier: six runs of docs/figures/decks/transport_demo, cached
under docs/figures/cache.
"""
from __future__ import annotations

import re

import numpy as np

REQUIRES = ("openswmm",)

DECK = "transport_demo/transport_demo.inp"
# 10 s reporting: the front crosses the line in about five minutes, so the
# deck's own 5-minute reporting steps straight over it
FINE = {"REPORT_STEP": "0:00:10"}
ENGINES = [
    ("EULERIAN_ARD", "Eulerian, MUSCL with the van Leer limiter", "fv"),
    ("LAGRANGIAN", "Lagrangian parcels", "dw"),
    ("LEGACY", "legacy CSTR routing", "dw-legacy"),
]
DXS = [(50, "dw-slot"), (25, "fv"), (10, "fv-lts")]
ARD_TEMPLATE = """;; Eulerian ARD transport options — generated for the Application Manual's
;; TARGET_DX comparison; the committed sidecar carries TARGET_DX 25.

[TRANSPORT_OPTIONS]
DISPERSION      FISCHER
SCALAR_SCHEME   MUSCL
LIMITER         VANLEER
TARGET_DX       {dx}
"""


def _series(sink, options, key, sidecars=None):
    from openswmm.engine import OutputReader, OutNodeVar
    r = sink.run(DECK, options, key, sidecars=sidecars)
    dt = 10.0
    with OutputReader(r.out) as o:
        base = int(OutNodeVar.POLLUT_BASE)
        i = o.pollutant_ids.index("TRACER")
        c = np.asarray([o.node_attributes("J5", period=k)[base + i] for k in range(o.period_count)], float)
    return np.arange(len(c)) * dt / 60.0, c, r.seconds


def _travel(t, c, level=5.0):
    """Time at which the front reaches half its plateau, by linear interpolation."""
    k = int(np.argmax(c >= level))
    if c[k] < level or k == 0:
        return float("nan")
    t0, t1, c0, c1 = t[k - 1], t[k], c[k - 1], c[k]
    return float(t0 + (level - c0) * (t1 - t0) / (c1 - c0))


def _draw_engines(style, runs):
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(8.6, 4.0))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(ax)
    for (name, label, colour), (t, c, secs) in zip(ENGINES, runs):
        ax.plot(t, c, color=style.COLORS[colour], lw=1.8, label=f"QUALITY_SOLVER {name} — {label}")
        t50 = _travel(t, c)
        if np.isfinite(t50):
            ax.plot([t50], [5.0], marker="o", ms=5, color=style.COLORS[colour], mec="white", mew=1.0, zorder=5)
    ax.axhline(10.0, color=style.MUTED, lw=0.9, ls=(0, (4, 2)))
    ax.text(0.15, 10.15, "inflow concentration, 10 mg/L", fontsize=7.2, color=style.MUTED)
    ax.axhline(5.0, color=style.GRIDC, lw=0.8)
    ax.text(19.85, 5.25, "half of it: the marker on each curve is the half-rise", fontsize=7.2,
            color=style.MUTED, ha="right")
    ax.set_xlim(0, 20)
    ax.set_ylim(0, 11.4)
    ax.set_xlabel("time  (minutes)")
    ax.set_ylabel("tracer at J5, 400 m downstream  (mg/L)")
    ax.legend(fontsize=7.4, frameon=False, loc="lower right")
    ax.set_title("A step of conservative tracer, reported every 10 s", fontsize=9.5, loc="left")
    return fig


def _draw_dx(style, runs):
    import matplotlib.pyplot as plt
    fig, (a0, a1) = plt.subplots(1, 2, figsize=(9.2, 3.8), gridspec_kw=dict(width_ratios=[1.45, 1.0]))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(a0)
    style.style_ax(a1)
    times, secs = [], []
    for (dx, colour), (t, c, s) in zip(DXS, runs):
        per = -(-100 // dx)          # the deck is five 100 m conduits
        a0.plot(t, c, color=style.COLORS[colour], lw=1.8,
                label=f"TARGET_DX {dx} m  ({per} cells per conduit, {5 * per} in all)")
        times.append(_travel(t, c))
        secs.append(s)
    a0.axhline(10.0, color=style.MUTED, lw=0.9, ls=(0, (4, 2)))
    a0.set_xlim(2, 12)
    a0.set_ylim(0, 11.4)
    a0.set_xlabel("time  (minutes)")
    a0.set_ylabel("tracer at J5  (mg/L)")
    a0.legend(fontsize=7.4, frameon=False, loc="lower right")
    a0.set_title("(a) The same front at three cell sizes", fontsize=9, loc="left")
    x = np.arange(len(DXS))
    a1.bar(x, secs, 0.5, color=[style.COLORS[c] for _, c in DXS])
    for i, s in enumerate(secs):
        a1.text(i, s + max(secs) * 0.03, f"{s:.2f} s", ha="center", fontsize=7.6, color=style.INK)
    a1.set_xticks(x)
    a1.set_xticklabels([f"{dx} m" for dx, _ in DXS], fontsize=8)
    a1.set_ylim(0, max(secs) * 1.25)
    a1.set_xlabel("TARGET_DX")
    a1.set_ylabel("run time  (s)")
    a1.set_title("(b) What the refinement costs", fontsize=9, loc="left")
    fig.tight_layout()
    return fig


def build(sink):
    import style
    engines = [_series(sink, dict(FINE, QUALITY_SOLVER=name), f"engine_{name.lower()}")
               for name, _, _ in ENGINES]
    for (name, _, _), (t, c, secs) in zip(ENGINES, engines):
        print(f"    {name}: {secs:.2f} s, half-rise at {_travel(t, c):.2f} min, plateau {c[-1]:.3f} mg/L")
    dx_runs = [_series(sink, dict(FINE, QUALITY_SOLVER="EULERIAN_ARD"), f"dx_{dx}",
                       sidecars={"transport_demo.ard": ARD_TEMPLATE.format(dx=dx)})
               for dx, _ in DXS]
    for (dx, _), (t, c, secs) in zip(DXS, dx_runs):
        print(f"    TARGET_DX {dx}: {secs:.2f} s, half-rise at {_travel(t, c):.2f} min")
    if "workflow_ch8_tracer_breakthrough_three_engines" in sink.expected:
        sink.save(_draw_engines(style, engines), "workflow_ch8_tracer_breakthrough_three_engines")
    if "workflow_ch8_ard_dx_sensitivity" in sink.expected:
        sink.save(_draw_dx(style, dx_runs), "workflow_ch8_ard_dx_sensitivity")
