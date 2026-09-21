"""RDII with and without a physics-based recovery (Application Manual,
the long-term-hydrology chapter).

The same two storms, thirty days apart, through one RTK unit hydrograph
group. Without `[RDII_DECAY]` the group answers both storms identically,
because an RTK hydrograph has no memory of what came before. With it,
the antecedent store depletes during the first storm and recovers at a
temperature-dependent rate, so the second storm produces less — and a
colder month produces less still.

Simulated tier: three runs of docs/figures/decks/hydrology_additions,
cached under docs/figures/cache.
"""
from __future__ import annotations

import re

import numpy as np

REQUIRES = ("openswmm",)

DECK = "hydrology_additions/rdii_decay.inp"
COLD = {"01/01/2026  00:00  34.0": "01/01/2026  00:00  24.0",
        "01/10/2026  00:00  38.0": "01/10/2026  00:00  26.0",
        "01/20/2026  00:00  45.0": "01/20/2026  00:00  30.0",
        "01/31/2026  00:00  52.0": "01/31/2026  00:00  34.0"}


def _strip_decay(text):
    """Drop the [RDII_DECAY] section, keeping the marker lines out with it."""
    lines = text.split("\n")
    marks = [i for i, ln in enumerate(lines) if ln.strip() == ";//! [decay]"]
    assert len(marks) == 2, f"expected two decay markers, found {len(marks)}"
    out = "\n".join(lines[:marks[0]] + lines[marks[1] + 1:])
    # the deck's own header mentions the section by name, so test for the header line
    assert not any(ln.strip().startswith("[RDII_DECAY]") for ln in out.split("\n")), \
        "the decay section was not removed"
    return out


def _cold(text):
    for a, b in COLD.items():
        assert a in text, a
        text = text.replace(a, b)
    return text


def _series(sink, key, text=None):
    from openswmm.engine import OutputReader, OutNodeVar
    r = sink.run(DECK, None, key, text=text)
    with OutputReader(r.out) as o:
        q = np.asarray(o.node_series("J_RDII", OutNodeVar.LATERAL_INFLOW), float)
    return np.arange(len(q)) / 24.0, q       # the deck reports hourly


def _storms(t, q):
    a = q[t < 10.0]
    b = q[(t >= 18.0) & (t < 28.0)]
    return float(a.max()), float(b.max()), float(np.trapezoid(a) * 3600), float(np.trapezoid(b) * 3600)


def _draw(style, runs):
    import matplotlib.pyplot as plt
    fig, (a0, a1) = plt.subplots(1, 2, figsize=(9.4, 4.0), gridspec_kw=dict(width_ratios=[1.5, 1.0]))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(a0)
    style.style_ax(a1)
    for (label, colour), (t, q) in runs:
        a0.plot(t, q, color=style.COLORS[colour], lw=1.6, label=label)
    a0.set_xlim(0, 31)
    a0.set_xlabel("day of the month")
    a0.set_ylabel("RDII inflow at the node  (cfs)")
    a0.legend(fontsize=7.4, frameon=False, loc="upper right")
    a0.set_title("(a) Two identical storms, nineteen days apart", fontsize=9, loc="left")
    a0.text(0.16, 0.97, "Above freezing the store recovers between the storms and the second\n"
            "produces about half the first. Below freezing recovery is suppressed\n"
            "entirely, so the ground stays wet and the second storm produces more\n"
            "than the first. The linear model with a zero recovery rate abstracts\n"
            "both storms and reports no RDII at all.",
            transform=a0.transAxes, fontsize=7.2, color=style.MUTED, va="top", linespacing=1.5)
    a0.set_ylim(0, 8.2)
    labels, first, second = [], [], []
    for (label, _), (t, q) in runs:
        p1, p2, v1, v2 = _storms(t, q)
        labels.append(label.split(":")[0].replace("exponential decay, ", "").replace("linear recovery", "linear\nrecovery"))
        first.append(p1)
        second.append(p2)
    x = np.arange(len(labels))
    a1.bar(x - 0.19, first, 0.36, color=style.MUTED, label="first storm")
    a1.bar(x + 0.19, second, 0.36, color=style.COLORS["fv"], label="second storm")
    for i, (p1, p2) in enumerate(zip(first, second)):
        if p1 > 0:
            a1.text(i + 0.19, p2 + 0.12, f"{100 * p2 / p1:.0f} %", ha="center", fontsize=7.6, color=style.INK)
        else:
            a1.text(i, 0.25, "no RDII", ha="center", fontsize=7.4, color=style.MUTED)
    a1.set_xticks(x)
    a1.set_xticklabels(labels, fontsize=7.4)
    a1.set_ylabel("peak RDII inflow  (cfs)")
    a1.set_ylim(0, max(max(first), max(second)) * 1.28)
    a1.legend(fontsize=7.2, frameon=False, loc="upper right")
    a1.set_title("(b) What the second storm produces", fontsize=9, loc="left")
    fig.tight_layout()
    return fig


def build(sink):
    import style
    base_text = (sink.fig_dir / "decks" / DECK).read_text()
    runs = [
        (("linear recovery, IA_r = 0: no RDII at all", "dw-legacy"),
         _series(sink, "no_decay", _strip_decay(base_text))),
        (("exponential decay, month above freezing", "fv"), _series(sink, "decay")),
        (("exponential decay, month below freezing", "dw"), _series(sink, "decay_cold", _cold(base_text))),
    ]
    for (label, _), (t, q) in runs:
        p1, p2, v1, v2 = _storms(t, q)
        share = f"{100 * p2 / p1:.0f} %" if p1 > 0 else "no RDII"
        print(f"    {label}: peaks {p1:.3f} then {p2:.3f} cfs ({share}), "
              f"volumes {v1:,.0f} then {v2:,.0f} cf")
    sink.save(_draw(style, runs), "workflow_ch11_rdii_decay_vs_linear")
