"""A surveyed trunk chained by virtual junctions (Application Manual, the
virtual-junctions chapter).

Cedar Glen to FRS: a synthetic raw-water conveyance built the way a
GIS-derived network is — a long trunk of short survey segments joined by
virtual junctions rather than manholes, with a double-pocket reach that
pressurises at each surge peak. Three decks differ only in their routing
options. The figures are the trunk's hydraulic grade line at the worst
instant and the head history of the node where the solvers disagree
most.

Simulated tier: three runs of docs/figures/decks/cedar_glen, cached
under docs/figures/cache. The finite-volume run takes about two minutes.
"""
from __future__ import annotations

import json

import numpy as np

REQUIRES = ("openswmm",)

DECKS = [("cedar_glen_fv.inp", "FV", "finite volume", "fv"),
         ("cedar_glen_dw.inp", "DW slot", "dynamic wave, Preissmann slot", "dw-slot"),
         ("cedar_glen_dw_extran.inp", "DW EXTRAN", "dynamic wave, EXTRAN surcharge", "dw-legacy")]


def _meta(sink):
    return json.loads((sink.fig_dir / "decks" / "cedar_glen" / "model_meta.json").read_text())


def _heads(run, names):
    from openswmm.engine import OutputReader, OutNodeVar
    with OutputReader(run.out) as o:
        have = set(o.node_ids)
        idx = [n for n in names if n in have]
        H = np.column_stack([np.asarray(o.node_series(n, OutNodeVar.HEAD), float) for n in idx])
        t = np.arange(o.period_count) / 60.0      # the decks report every minute
    return t, idx, H


def _continuity(run):
    """The Flow Routing Continuity block's error, not the last line that mentions one."""
    lines = run.rpt.read_text(errors="replace").splitlines()
    start = next(i for i, ln in enumerate(lines) if "Flow Routing Continuity" in ln)
    for ln in lines[start:start + 20]:
        if "Continuity Error" in ln:
            return float(ln.split()[-1])
    raise RuntimeError("no continuity error in the Flow Routing Continuity block")


def _draw_profile(style, meta, runs, worst_k):
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(9.2, 4.2))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(ax)
    s = np.array([p["s"] for p in meta["trunk"]]) / 1000.0
    z = np.array([p["z"] for p in meta["trunk"]])
    crown = z + np.array([p["diam"] for p in meta["trunk"]]) / 12.0
    ax.fill_between(s, z - 6.0, z, color=style.SOIL, zorder=0)
    ax.plot(s, z, color=style.INK, lw=1.2, label="invert")
    ax.plot(s, crown, color=style.MUTED, lw=1.0, ls=(0, (4, 2)), label="crown")
    for (_, short, label, colour), (t, idx, H, _) in zip(DECKS, runs):
        pos = {n: i for i, n in enumerate(idx)}
        sel = [(p["s"] / 1000.0, H[worst_k, pos[p["name"]]]) for p in meta["trunk"] if p["name"] in pos]
        xs = np.array([a for a, _ in sel])
        ys = np.array([b for _, b in sel])
        ax.plot(xs, ys, color=style.COLORS[colour], lw=1.6, label=f"{short}: {label}")
    ax.set_xlabel("chainage along the trunk  (1000 ft)")
    ax.set_ylabel("head  (ft)")
    ax.legend(fontsize=7.4, frameon=False, loc="upper right", ncol=2)
    ax.set_title("The hydraulic grade line along the trunk, at the instant the solvers disagree most",
                 fontsize=9.5, loc="left")
    ax.text(0.02, 0.06, "370 virtual junctions chain 387 conduits into one continuous trunk: no manhole,\n"
            "no storage, no head loss at a splice — the grade line runs through them.",
            transform=ax.transAxes, fontsize=7.4, color=style.MUTED, va="bottom", linespacing=1.45)
    fig.tight_layout()
    return fig


def _draw_worst(style, runs, worst_node):
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(8.8, 4.0))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(ax)
    for (_, short, label, colour), (t, idx, H, secs) in zip(DECKS, runs):
        pos = {n: i for i, n in enumerate(idx)}
        ax.plot(t, H[:, pos[worst_node]], color=style.COLORS[colour], lw=1.6,
                label=f"{short} — {label}")
    ax.set_xlabel("time  (hours)")
    ax.set_ylabel(f"head at {worst_node}  (ft)")
    ax.legend(fontsize=7.4, frameon=False, loc="lower right")
    ax.text(0.02, 0.94, "The slot run oscillates between the pocket floor and the crest for hours; the other two\n"
            "hold a smooth recession. Its flow-routing continuity error is -8.3 %.",
            transform=ax.transAxes, fontsize=7.4, color=style.MUTED, va="top", linespacing=1.45)
    ax.set_title("Head at the node where the three routings disagree most", fontsize=9.5, loc="left")
    fig.tight_layout()
    return fig


def build(sink):
    import style
    meta = _meta(sink)
    names = [p["name"] for p in meta["trunk"]]
    runs, conts = [], []
    for deck, short, _, _ in DECKS:
        run = sink.run(f"cedar_glen/{deck}", None, deck.replace(".inp", ""))
        t, idx, H = _heads(run, names)
        runs.append((t, idx, H, run.seconds))
        conts.append(_continuity(run))
        print(f"    {short}: {run.seconds:.0f} s, continuity {conts[-1]:+.3f} %, "
              f"{H.shape[1]} trunk nodes reported")
    # the instant and the node of greatest disagreement between FV and DW-slot
    a, b = runs[0][2], runs[1][2]
    n = min(a.shape[0], b.shape[0])
    diff = np.abs(a[:n] - b[:n])
    worst_k, worst_i = np.unravel_index(int(np.argmax(diff)), diff.shape)
    worst_node = runs[0][1][worst_i]
    print(f"    worst disagreement {diff.max():.2f} ft at {worst_node}, t = {runs[0][0][worst_k]:.2f} h")
    if "workflow_ch4_cedar_glen_profile_hgl" in sink.expected:
        sink.save(_draw_profile(style, meta, runs, worst_k), "workflow_ch4_cedar_glen_profile_hgl")
    if "workflow_ch4_cedar_glen_worst_node_head" in sink.expected:
        sink.save(_draw_worst(style, runs, worst_node), "workflow_ch4_cedar_glen_worst_node_head")
