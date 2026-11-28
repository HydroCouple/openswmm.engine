"""Mixing drained 2D pollutant with net or gross water (quality Figure 10-2, §10.4).

One 60 s routing step at one coupling point holds six 10 s exchange windows
that alternate between draining the mesh into the junction and spilling the
junction onto the mesh. The hydraulics take the step's net volume. The figure
replays the two ways the 1D side has mixed the drained mass:

* net (before 2026-11-20): each window queues its drained mass less the part
  its own spill pairs against at the node's published concentration
  (`src/engine/2d/SurfaceRouter2D.cpp:2971`, `:2988`, floored at zero at
  `:3012`), and the node mixes that mass with the NET coupling water, which
  is no carrier at all when the step spills on net;
* gross (now): each window queues its gross drain water and gross spill
  (`SurfaceRouter2D.cpp:2953-2956`), the mass queues whole, and the node mixes
  it with the gross drain water (`src/engine/quality/QualityRouting.cpp:655`).

The node is a zero-volume junction, so its concentration is the inflow mass
over the inflow volume, provided the inflow rate exceeds 1e-10 cfs
(`QualityRouting.cpp:1651-1670`, `QualityRouting.hpp:64`). The queues are
delivered within the step (`src/engine/core/SWMMEngine.cpp:12639-12665`).

Synthetic tier: numpy and matplotlib only; the scenario is invented, the rules
are the engine's. US units (ft3, cfs, mg/L), as the 1D side books them.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()

DT = 60.0                       # routing step, s
WINDOW = 10.0                   # exchange window, s
DRAIN = np.array([18.0, 0.0, 14.0, 0.0, 16.0, 0.0])   # gross drain per window, ft3
SPILL = np.array([0.0, 15.0, 4.0, 17.0, 0.0, 14.0])   # gross spill per window, ft3
C_CELL = 6.0                    # drained cell water, mg/L
C_NODE = 4.0                    # node's published concentration for the batch, mg/L
C_OTHER = 4.0                   # the junction's other inflow, mg/L
Q_OTHER = 0.05                  # the junction's other inflow, cfs
LEGACY_ZERO = 1.0e-10           # QualityRouting.hpp:64


def queue(gross):
    """Queues one step's windows: (net water, gross drain water, mass) in ft3 and mg/L*ft3."""
    net_q = drain_q = mass_q = 0.0
    for d, s in zip(DRAIN, SPILL):
        exch = d - s                                   # the window's net exchange
        net_q += exch
        if gross:
            drain = max(0.0, exch + s)                 # SurfaceRouter2D.cpp:2953
            drain_q += drain
        inc = C_CELL * d                               # the marcher's gross drained mass
        paired = min(s, d if d > 0.0 else 0.0)         # SurfaceRouter2D.cpp:2971
        if not gross and paired > 0.0:
            inc -= paired * C_NODE                     # SurfaceRouter2D.cpp:2988
        mass_q = max(0.0, mass_q + inc)                # SurfaceRouter2D.cpp:3012
    return net_q, drain_q, mass_q


def node_conc(gross, q_other):
    """Zero-volume junction concentration after the step (mg/L), and its carrier water (ft3)."""
    net_q, drain_q, mass_q = queue(gross)
    carrier_rate = (drain_q if gross else net_q) / DT  # QualityRouting.cpp:655
    v_in = q_other * DT + (carrier_rate * DT if carrier_rate > 0.0 else 0.0)
    mass_in = mass_q + q_other * C_OTHER * DT
    if v_in / DT > LEGACY_ZERO:                        # QualityRouting.cpp:1651
        return mass_in / v_in, v_in                    # QualityRouting.cpp:1670
    return float("nan"), v_in


def _draw(style):
    import matplotlib.pyplot as plt
    blue, orange = style.WATER, style.COLORS["fv-lts"]
    net_col, gross_col = style.COLORS["dw-legacy"], style.COLORS["dw"]
    fig, (a0, a1) = plt.subplots(1, 2, figsize=(9.6, 4.3), gridspec_kw={"width_ratios": [1.0, 1.15]})
    fig.patch.set_facecolor(style.SURFACE)
    for ax in (a0, a1):
        style.style_ax(ax)

    # (a) the windows of one routing step
    t0 = np.arange(len(DRAIN)) * WINDOW
    a0.bar(t0 + WINDOW / 2, DRAIN / WINDOW, width=WINDOW * 0.86, color=blue,
           label="drain, mesh to junction")
    a0.bar(t0 + WINDOW / 2, -SPILL / WINDOW, width=WINDOW * 0.86, color=orange,
           label="spill, junction to mesh")
    a0.axhline(0.0, color=style.MUTED, lw=0.8)
    a0.set_xlim(0, DT)
    a0.set_ylim(-2.6, 2.6)
    a0.set_xticks(np.arange(0, DT + 1, WINDOW))
    a0.set_xlabel("time within one 60 s routing step (s)")
    a0.set_ylabel("exchange flow at the coupling point (cfs)")
    a0.set_title("(a) Six 10 s exchange windows at one coupling point", fontsize=9, loc="left")
    d, s = DRAIN.sum(), SPILL.sum()
    a0.text(1.0, -1.95, f"gross drain {d:.0f} ft³ at {C_CELL:.0f} mg/L\n"
            f"gross spill {s:.0f} ft³\n"
            f"net {d - s:+.0f} ft³: the hydraulics take this", fontsize=7.6, color=style.INK,
            linespacing=1.4, va="center")
    a0.legend(loc="upper right", fontsize=7.6)

    # (b) what the junction reads
    q = np.logspace(-3, 1, 200)
    net = np.array([node_conc(False, x)[0] for x in q])
    gross = np.array([node_conc(True, x)[0] for x in q])
    a1.axhspan(min(C_CELL, C_OTHER), max(C_CELL, C_OTHER), color=style.GRIDC, alpha=0.9, lw=0)
    a1.text(1.2e-3, 6.6, "between the inflows' concentrations", fontsize=7.4, color=style.MUTED)
    a1.loglog(q, net, color=net_col, lw=1.8, label="mixed with the net water (before 2026-11-20)")
    a1.loglog(q, gross, color=gross_col, lw=1.8, label="mixed with the gross drain water (now)")
    cn, vn = node_conc(False, Q_OTHER)
    cg, vg = node_conc(True, Q_OTHER)
    for c, col in ((cn, net_col), (cg, gross_col)):
        a1.plot(Q_OTHER, c, "o", ms=5.5, mfc=col, mec=style.INK, mew=0.6, zorder=5)
    a1.annotate(f"{cn:.0f} mg/L: carrier {vn:.0f} ft³,\nthe other inflow only", (Q_OTHER, cn),
                xytext=(0.18, 260), fontsize=7.4, color=style.INK,
                arrowprops=dict(arrowstyle="-", color=style.MUTED, lw=0.7))
    a1.annotate(f"{cg:.2f} mg/L: carrier {vg:.0f} ft³", (Q_OTHER, cg),
                xytext=(0.12, 1.6), fontsize=7.4, color=style.INK,
                arrowprops=dict(arrowstyle="-", color=style.MUTED, lw=0.7))
    a1.set_xlim(1e-3, 10)
    a1.set_ylim(1.0, 2.0e4)
    a1.set_xlabel(f"the junction's other inflow, at {C_OTHER:.0f} mg/L (cfs)")
    a1.set_ylabel("junction concentration after the step (mg/L)")
    a1.set_title("(b) What a zero-volume junction reads", fontsize=9, loc="left")
    a1.legend(loc="upper right", fontsize=7.4)
    fig.tight_layout()
    return fig


def build(sink):
    import style
    fig = _draw(style)
    net_q, _, mass_net = queue(False)
    _, drain_q, mass_gross = queue(True)
    cn, vn = node_conc(False, Q_OTHER)
    cg, vg = node_conc(True, Q_OTHER)
    print(f"    net {net_q:+.1f} ft3, gross drain {drain_q:.1f} ft3, gross spill {SPILL.sum():.1f} ft3; "
          f"queued mass net-rule {mass_net:.1f}, gross-rule {mass_gross:.1f} mg/L*ft3")
    print(f"    junction at {Q_OTHER} cfs other inflow: net mix {cn:.1f} mg/L (carrier {vn:.1f} ft3), "
          f"gross mix {cg:.3f} mg/L (carrier {vg:.1f} ft3)")
    sink.save(fig, "quality_ch10_gross_volume_mixing")
