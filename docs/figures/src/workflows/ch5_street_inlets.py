"""Capture and bypass along a street reach with two inlets
(Application Manual, the street-inlets chapter).

One street draining to a parallel sewer through two inlets attached two
different ways: a conduit-attribute inlet on the first reach, whose
capture is booked at the reach's downstream node, and an inlet junction
between the second and third reaches, which is a node of its own. The
figure follows the water: what stays on the street, what each inlet takes,
and what the sewer receives.

Simulated tier: one run of docs/figures/decks/street_inlets, cached under
docs/figures/cache.
"""
from __future__ import annotations

import re

import numpy as np

REQUIRES = ("openswmm",)

DECK = "street_inlets/street_inlet_junction.inp"
STREET = [("ST_A", "street above the first inlet"), ("ST_B", "street between the inlets"),
          ("ST_C", "street below the inlet junction")]
CAPTURE = [("MH1", "captured by the conduit-attribute inlet on ST_A"),
           ("MH2", "captured by the inlet junction IJ1")]


def _summary_rows(rpt_text):
    """The Street Inlet Flow Summary, as
    {location: (peak approach flow, peak capture %, average capture %, bypass frequency %,
                captured 1000 gal, bypassed 1000 gal)}.

    Note the fourth column of the report: "Bypass Flow Pcnt" is the *frequency* of
    bypass over the capture periods, not a share of the volume. The volume split is
    the last two columns.
    """
    out = {}
    block = rpt_text.split("Street Inlet Flow Summary", 1)
    if len(block) < 2:
        return out
    for ln in block[1].splitlines():
        m = re.match(r"^\s{2}(\S+(?: \(node\))?)\s+(\S+)\s+\S+\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+"
                     r"([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s*$", ln)
        if m:
            out[m.group(1)] = tuple(float(m.group(i)) for i in (4, 5, 6, 7, 11, 12))
    return out


def _draw(style, t, street, capture, rows):
    import matplotlib.pyplot as plt
    fig, (a0, a1) = plt.subplots(1, 2, figsize=(9.4, 3.9), gridspec_kw=dict(width_ratios=[1.35, 1.0]))
    fig.patch.set_facecolor(style.SURFACE)
    style.style_ax(a0)
    style.style_ax(a1)
    cols = [style.COLORS["fv"], style.COLORS["fv-lts"], style.COLORS["dw"]]
    for (name, label), q, c in zip(STREET, street, cols):
        a0.plot(t, q, color=c, lw=1.7, label=f"{name} — {label}")
    for (name, label), q, ls in zip(CAPTURE, capture, ("-", (0, (4, 2)))):
        a0.plot(t, q, color=style.MUTED, lw=1.3, ls=ls, label=label)
    a0.set_yscale("symlog", linthresh=0.05)
    a0.set_xlabel("time  (hours)")
    a0.set_ylabel("flow  (cfs)")
    a0.legend(fontsize=6.9, frameon=False, loc="upper right")
    a0.set_title("(a) What stays on the street, and what each inlet takes", fontsize=9, loc="left")
    a0.text(2.95, 0.35, "symmetric log scale below 0.05 cfs.\nThe street flow falls by three orders\n"
            "of magnitude across the two inlets.", fontsize=7.0, color=style.MUTED, va="center", ha="right",
            linespacing=1.45)
    # capture / bypass split per inlet, from the report's own summary
    labels, cap, byp = [], [], []
    for key, nice in (("ST_A", "ST_A\nconduit attribute"), ("IJ1 (node)", "IJ1\ninlet junction")):
        if key not in rows:
            continue
        labels.append(nice)
        cap.append(rows[key][4])
        byp.append(rows[key][5])
    # a share, not a volume: the two inlets differ by two orders of magnitude in
    # the water they see, and the question here is what each did with it
    x = np.arange(len(labels))
    tot = [c + b for c, b in zip(cap, byp)]
    cap_pct = [100 * c / s for c, s in zip(cap, tot)]
    byp_pct = [100 * b / s for b, s in zip(byp, tot)]
    a1.bar(x, cap_pct, 0.5, color=style.COLORS["fv"], label="captured")
    a1.bar(x, byp_pct, 0.5, bottom=cap_pct, color=style.COLORS["dw-legacy"], label="bypassed")
    for i, (c, b, s) in enumerate(zip(cap, byp, tot)):
        a1.text(i, cap_pct[i] / 2, f"{cap_pct[i]:.1f} %\n{c:,.1f} kgal", ha="center", va="center",
                fontsize=7.6, color="white", linespacing=1.4)
        a1.text(i, 101.5, f"bypass {byp_pct[i]:.1f} %  ({b:,.2f} kgal)", ha="center", fontsize=7.2,
                color=style.INK)
    a1.set_xticks(x)
    a1.set_xticklabels([f"{lab}\n{s:,.1f} kgal reached it" for lab, s in zip(labels, tot)], fontsize=7.6)
    a1.set_ylabel("share of the water reaching the inlet  (%)")
    a1.set_ylim(0, 118)
    a1.set_title("(b) What each inlet did with what reached it", fontsize=9, loc="left")
    fig.tight_layout()
    return fig


def build(sink):
    import style
    from openswmm.engine import OutputReader, OutNodeVar, OutLinkVar
    run = sink.run(DECK, None, "base")
    with OutputReader(run.out) as o:
        n = o.period_count
        t = np.arange(n) / 60.0                       # the deck reports every minute
        street = [np.asarray(o.link_series(k, OutLinkVar.FLOW), float) for k, _ in STREET]
        capture = [np.asarray(o.node_series(k, OutNodeVar.LATERAL_INFLOW), float) for k, _ in CAPTURE]
    rows = _summary_rows(run.rpt.read_text(errors="replace"))
    for k, v in rows.items():
        print(f"    {k}: peak approach {v[0]:.3f} cfs, peak capture {v[1]:.2f} %, avg capture {v[2]:.2f} %, "
              f"bypass frequency {v[3]:.2f} %, {v[4]:,.3f} kgal captured / {v[5]:,.3f} kgal bypassed")
    sink.save(_draw(style, t, street, capture, rows), "workflow_ch5_inlet_capture_vs_bypass")
