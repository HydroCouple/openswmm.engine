"""The four surcharge treatments of the dynamic wave router on one overloaded
pipe chain (Hydraulics Reference, Chapter 3, Figure 3-8).

The deck is the engine's Preissmann-slot regression model: three 400 ft,
1 ft circular pipes from J1 to a free outfall, fed 12 cfs at J1 (a ramp over
the first 10 minutes, held to 2:30, off by 3:00) against a full-pipe
capacity near 1 cfs, so the chain surcharges for most of the run and J1
floods at its 30 ft rim. The same deck is run four times, changing only
SURCHARGE_METHOD (EXTRAN, SLOT, DYNAMIC_SLOT, TPA), and the figure shows:

* (a)-(c) the hydraulic grade line through the four nodes at two instants
  while the chain fills and one while it drains, read from the binary
  output (node HEAD); the node-link scheme defines the grade line at the
  nodes only, so the lines between them are straight;
* (d) the head at J3 through the filling, the node where the methods
  differ most;
* (e) the provisional Preissmann number of the dynamic slot after a pipe
  first pressurizes. P is not written to any output, so this panel is
  Equation 3-40 evaluated with the engine's own constants for this deck's
  pipe: P_hat_0 = max(c_pT / (alpha c_g), 1) with c_g = sqrt(g A_full /
  W_max) (src/engine/hydraulics/DynamicWave.cpp:796-804), c_pT =
  DPS_CELERITY x 3.28084 ft/s, alpha = max(DPS_ALPHA, 2), r =
  DPS_DECAY_TIME (DynamicWave.cpp:775-777; defaults 25 m/s, 3 and 0.5 s at
  src/engine/core/SimulationOptions.hpp:458-472), g = 32.2 ft/s^2
  (src/engine/core/Constants.hpp:52). The engine updates P_hat once per
  routing step, after the iterations (DynamicWave.cpp:1612-1615 and
  :441-444), so the markers are the values it takes at the dynamic-slot
  run's minimum and average routing steps, read from that run's report.

Simulated tier: four runs of docs/figures/decks/slot_surcharge, cached
under docs/figures/cache. Needs the openswmm package for OutputReader.
"""
from __future__ import annotations

import math
import re

import numpy as np

REQUIRES = ("openswmm",)

DECK = "slot_surcharge/slot_surcharge.inp"
# (SURCHARGE_METHOD, legend label, color, line style); EXTRAN, the static
# slot and DW TPA keep the colors the Application Manual figures give them
METHODS = [
    ("EXTRAN", "EXTRAN (the default)", "#e87ba4", "-"),
    ("SLOT", "SLOT, static slot", "#eda100", "-"),
    ("DYNAMIC_SLOT", "DYNAMIC_SLOT", "#8a3ffc", (0, (4, 2))),
    ("TPA", "TPA (experimental)", "#1baf7a", "-"),
]
NODES = ["J1", "J2", "J3", "OUT"]
INSTANTS = [(4, "(a) Filling, 4 min"), (9, "(b) Filling, 9 min"), (175, "(c) Draining, 175 min")]


def _section(text, name):
    m = re.search(r"^\[" + name + r"\]\s*$(.*?)(?=^\[|\Z)", text, re.M | re.S)
    rows = []
    for ln in m.group(1).splitlines():
        s = ln.split(";")[0].split()
        if s:
            rows.append(s)
    return rows


def _geometry(text):
    """Node inverts and rims, and the distance of each node along the chain."""
    invert, rim = {}, {}
    for s in _section(text, "JUNCTIONS"):
        invert[s[0]] = float(s[1])
        rim[s[0]] = float(s[1]) + float(s[2])
    for s in _section(text, "OUTFALLS"):
        invert[s[0]] = float(s[1])
    x, pos = {"J1": 0.0}, 0.0
    for s in _section(text, "CONDUITS"):
        pos += float(s[3])
        x[s[2]] = pos
    diam = float(_section(text, "XSECTIONS")[0][2])
    return invert, rim, x, diam


def _routing_steps(rpt):
    """(minimum, average) routing step in seconds from the report's summary."""
    lo = float(re.search(r"Minimum Time Step\s*:\s*([0-9.]+)", rpt).group(1))
    avg = float(re.search(r"Average Time Step\s*:\s*([0-9.]+)", rpt).group(1))
    return lo, avg


def _continuity(rpt):
    vals = re.findall(r"Continuity Error \(%\)\s*\.+\s*([-+0-9.]+)", rpt)
    return float(vals[-1]) if vals else float("nan")


def _run(sink, method):
    from openswmm.engine import OutputReader, OutNodeVar
    r = sink.run(DECK, {"SURCHARGE_METHOD": method}, f"hyd36_{method.lower()}")
    with OutputReader(r.out) as o:
        t = (o.period_times - np.datetime64(o.start_datetime)) / np.timedelta64(1, "s") / 60.0
        heads = np.array([np.asarray(o.node_series(n, OutNodeVar.HEAD), float) for n in NODES])
    rpt = r.rpt.read_text(errors="replace")
    return {"t": np.asarray(t, float), "h": heads, "rpt": rpt}


def _p_hat(text):
    """P_hat_0 and the decay of Equation 3-40 for the deck's pipe, with the engine's constants."""
    def opt(key, default):
        m = re.search(r"^" + key + r"\s+(\S+)", text, re.M)
        return float(m.group(1)) if m else default
    g = 32.2
    c_pt = opt("DPS_CELERITY", 25.0) * 3.28084
    alpha = max(opt("DPS_ALPHA", 3.0), 2.0)
    r = max(opt("DPS_DECAY_TIME", 0.5), 0.001)
    d = float(_section(text, "XSECTIONS")[0][2])
    a_full, w_max = math.pi * d * d / 4.0, d
    c_g = math.sqrt(g * a_full / w_max)
    p0 = max(c_pt / (alpha * c_g), 1.0)
    t_s1 = g * a_full / (c_pt * c_pt)           # slot top width at P = 1 (Equation 3-37)
    return p0, r, c_pt, c_g, alpha, t_s1


def _draw(style, runs, text, steps):
    import matplotlib.pyplot as plt
    from matplotlib.gridspec import GridSpec
    invert, rim, x, diam = _geometry(text)
    xs = np.array([x[n] for n in NODES])
    inv = np.array([invert[n] for n in NODES])
    fig = style.figure(10.0, 7.0)
    gs = GridSpec(2, 6, figure=fig, height_ratios=[1.0, 0.92], hspace=0.38, wspace=0.95,
                  left=0.07, right=0.99, top=0.885, bottom=0.085)
    axes = [fig.add_subplot(gs[0, 2 * i:2 * i + 2]) for i in range(3)]
    for ax, (minute, title) in zip(axes, INSTANTS):
        style.style_ax(ax)
        ax.fill_between(xs, inv, inv + diam, color=style.PIPE_FILL, ec=style.MUTED, lw=0.6, zorder=1)
        ax.plot(xs[:-1], [rim[n] for n in NODES[:-1]], color=style.MUTED, lw=0.9, ls=(0, (5, 3)), zorder=2)
        for method, label, line_color, ls in METHODS:
            run = runs[method]
            k = int(np.argmin(np.abs(run["t"] - minute)))
            ax.plot(xs, run["h"][:, k], color=line_color, ls=ls, lw=1.6, marker="o", ms=3.2, zorder=4)
        ax.set_xlim(-60, 1260)
        ax.set_ylim(96, 133)
        ax.set_xticks(xs)
        ax.set_xticklabels(NODES)
        ax.set_title(title, fontsize=9, loc="left")
    axes[0].set_ylabel("head  (ft)")
    axes[2].text(xs[2] - 20, rim["J3"] - 0.8, "junction rims, 30 ft above\nthe invert; J1 floods at 130 ft",
                 fontsize=7.2, color=style.MUTED, va="top", ha="right", linespacing=1.3)
    axes[1].annotate("the pipes, 1 ft\ncircular", xy=(150, inv[0] + 0.3), xytext=(40, 106.0),
                     fontsize=7.2, color=style.MUTED, ha="left", linespacing=1.3,
                     arrowprops=dict(arrowstyle="-", color=style.MUTED, lw=0.7))
    # (d) the head at J3 through the filling
    ad = fig.add_subplot(gs[1, 0:3])
    style.style_ax(ad)
    j3 = NODES.index("J3")
    for method, label, line_color, ls in METHODS:
        run = runs[method]
        ad.plot(run["t"], run["h"][j3], color=line_color, ls=ls, lw=1.6, label=label)
    for minute, _ in INSTANTS[:2]:
        ad.axvline(minute, color=style.MUTED, lw=0.8, ls=(0, (2, 2)))
    ad.axhline(invert["J3"] + diam, color=style.MUTED, lw=0.8)
    ad.text(34.5, invert["J3"] + diam + 0.4, "crown of the pipes at J3", fontsize=7.2, color=style.MUTED,
            ha="right")
    ad.set_xlim(0, 35)
    ad.set_ylim(98, 116)
    ad.set_yticks([100, 105, 110, 115])
    ad.set_xlabel("time  (min)")
    ad.set_ylabel("head at J3  (ft)")
    ad.set_title("(d) Head at J3 while the chain fills", fontsize=9, loc="left")
    # (e) the provisional Preissmann number of the dynamic slot
    ae = fig.add_subplot(gs[1, 3:6])
    style.style_ax(ae)
    p0, r, c_pt, c_g, alpha, t_s1 = _p_hat(text)
    tau = np.linspace(0.0, 4.0, 400)
    ae.plot(tau, 1.0 + (p0 - 1.0) * np.exp(-10.0 * tau / r), color="#8a3ffc", lw=1.6)
    ae.axhline(1.0, color=style.MUTED, lw=0.8)
    lo, avg = steps
    ae.plot([0.0], [p0], marker="o", ms=5, color="#8a3ffc", zorder=5)
    for dt, name in ((lo, "minimum"), (avg, "average")):
        p = 1.0 + (p0 - 1.0) * math.exp(-10.0 * dt / r)
        ae.plot([dt], [p], marker="o", ms=5, mfc=style.SURFACE, mec="#8a3ffc", mew=1.3, zorder=5)
        ae.axvline(dt, color=style.MUTED, lw=0.8, ls=(0, (2, 2)))
        ae.text(dt - 0.06, 3.5, f"{name} routing\nstep, {dt:.2f} s", fontsize=7.2, color=style.MUTED,
                va="center", ha="right", linespacing=1.3)
    ae.text(0.12, p0, f"$\\hat{{P}}_0$ = {p0:.2f}: slot width {t_s1 * p0 * p0:.3f} ft,\n"
            f"pressure celerity {c_pt / p0:.1f} ft/s = {alpha:.0f} $c_g$",
            fontsize=7.2, color=style.INK, va="top", linespacing=1.35)
    ae.text(0.85, 1.25, f"P = 1: slot width {t_s1:.4f} ft, celerity {c_pt:.0f} ft/s",
            fontsize=7.2, color=style.INK, va="bottom")
    ae.set_xlim(0, 4.0)
    ae.set_ylim(0.5, p0 + 0.6)
    ae.set_xlabel("time since the pipe pressurized  (s)")
    ae.set_ylabel("provisional Preissmann number")
    ae.set_title(f"(e) Dynamic slot: Equation 3-40, DPS_DECAY_TIME {r:g} s", fontsize=9, loc="left")
    handles = [plt.Line2D([], [], color=c, ls=ls, lw=1.6, marker="o", ms=3.2) for _, _, c, ls in METHODS]
    fig.legend(handles, [m[1] for m in METHODS], loc="upper center", ncol=4, fontsize=8, frameon=False,
               bbox_to_anchor=(0.53, 0.985), title="SURCHARGE_METHOD, same deck otherwise",
               title_fontsize=8)
    return fig


def build(sink):
    import style
    text = (sink.fig_dir / "decks" / DECK).read_text()
    runs = {m: _run(sink, m) for m, _, _, _ in METHODS}
    steps = _routing_steps(runs["DYNAMIC_SLOT"]["rpt"])
    p0, r, c_pt, c_g, alpha, t_s1 = _p_hat(text)
    for method, _, _, _ in METHODS:
        run = runs[method]
        lo, avg = _routing_steps(run["rpt"])
        cells = []
        for minute, _ in INSTANTS:
            k = int(np.argmin(np.abs(run["t"] - minute)))
            cells.append(f"{minute} min " + "/".join(f"{v:.1f}" for v in run["h"][:3, k]))
        j3 = run["h"][NODES.index("J3")]
        steady = j3[int(np.argmin(np.abs(run["t"] - 60)))]
        reach = run["t"][int(np.argmax(np.abs(j3 - steady) < 0.1))]
        print(f"    {method:12s} heads J1/J2/J3: {'; '.join(cells)}; J3 within 0.1 ft of its "
              f"1:00 value {steady:.2f} ft by {reach:.0f} min; steps {lo:.2f}/{avg:.2f} s; "
              f"continuity {_continuity(run['rpt']):+.3f} %")
    print(f"    DPS: P_hat_0 {p0:.3f} (c_pT {c_pt:.2f} ft/s, c_g {c_g:.3f} ft/s, alpha {alpha:g}); "
          f"P_hat - 1 after the minimum step {steps[0]:.2f} s: {(p0 - 1) * math.exp(-10 * steps[0] / r):.2e}")
    sink.save(_draw(style, runs, text, steps), "hydraulics_ch3_surcharge_methods")
