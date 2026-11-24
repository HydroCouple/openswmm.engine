"""Staggered 1D/2D delivery: batch booking, the uniform drain of the
delivery queue, and the pending volume that closes the transfer identity.

Synthetic tier. A short replay, in Python, of the engine's own rules for
one coupled junction under a co-advance batch of 20 s (COUPLING_SYNC 20)
and 5 s routing steps:

* At batch close the surface ledger books the batch's signed exchange
  volume, then moves it from ``coupling_volume`` to ``coupling_queue`` and
  sets ``remaining = max(remaining, batch span)``
  (src/engine/2d/SurfaceRouter2D.cpp:3025-3027 and :3079-3095).
* At the start of each routing step, ``assembleLateralInflows`` drains the
  queue at the uniform rate ``queue / remaining`` while ``remaining`` exceeds
  the step, and flushes the remainder as ``queue / dt`` on the step where
  it does not (src/engine/core/SWMMEngine.cpp:12607-12617); ``remaining``
  then ticks down by one step (SWMMEngine.cpp:12717-12719).
* Pending transfer is ``coupling_queue + coupling_volume``, and
  delivered + pending = out - in (docs/manuals/engine/sections/
  Chapter7-TwoDimensionalCpu.md, Section 7.3.1).

The exchange rate itself is an illustrative drain hydrograph followed by a
short spill (1D to 2D, negative); it stands in for what the marcher
evaluates at tier-0 cadence over each batch. Every number in the figure
follows from that rate and the rules above. With the default COUPLING_SYNC 0
(co-advance every routing step) the queue empties in the single following step.
"""
from __future__ import annotations

import numpy as np

REQUIRES = ()

FIG_ID = "eng_ch7_staggered_delivery"
DT = 5.0          # routing step, s
SYNC = 20.0       # COUPLING_SYNC, s
T_END = 140.0
TIER0 = 0.25      # marcher cadence used to integrate the exchange over a batch, s


def exchange_rate(t):
    """Signed 2D -> 1D exchange, m3/s: a drain pulse, then a short spill."""
    t = np.asarray(t, float)
    drain = np.where((t >= 0) & (t <= 60), 0.050 * np.sin(np.pi * t / 60.0) ** 2, 0.0)
    spill = np.where((t >= 62) & (t <= 78), -0.030 * np.sin(np.pi * (t - 62) / 16.0) ** 2, 0.0)
    return drain + spill


def replay():
    """Step the engine's booking and drain rules; returns per-step records."""
    queue, volume, remaining = 0.0, 0.0, 0.0
    out_minus_in, delivered = 0.0, 0.0
    batch_start, span = 0.0, 0.0
    rows = []
    t = 0.0
    while t < T_END - 1e-9:
        # assembleLateralInflows(dt): drain the queue (SWMMEngine.cpp:12607-12617)
        q = 0.0
        if queue != 0.0:
            if remaining > DT:
                q = queue / remaining
                queue -= q * DT
            else:
                q = queue / DT
                queue = 0.0
        if volume != 0.0:                       # decoupled path; unused here, kept for fidelity
            q += volume / DT
            volume = 0.0
        delivered += q * DT
        if remaining > 0.0:                     # one tick per routing step (:12717-12719)
            remaining = max(0.0, remaining - DT)
        # 1D routing over [t, t + DT] happens here, with lateral inflow q
        t += DT
        span += DT
        booked = None
        if span >= SYNC - 1e-9:
            # co-advance: the marcher evaluates the exchange over the batch at tier-0 cadence
            sub = np.arange(batch_start, t, TIER0) + TIER0 / 2
            v = float(np.sum(exchange_rate(sub)) * TIER0)
            out_minus_in += v                   # ledger: coupling_2d_to_1d_out - coupling_1d_to_2d_in
            volume += v                         # accumulateMassBalance reads it, then the queue move
            queue += volume
            volume = 0.0
            remaining = max(remaining, span)    # SurfaceRouter2D.cpp:3093-3094
            booked = (batch_start, t, v)
            batch_start, span = t, 0.0
        pending = queue + volume
        rows.append(dict(t0=t - DT, t1=t, q=q, delivered=delivered, ledger=out_minus_in,
                         pending=pending, booked=booked))
    return rows


def build(sink):
    import matplotlib.pyplot as plt
    import style
    rows = replay()
    resid = max(abs(r["delivered"] + r["pending"] - r["ledger"]) for r in rows)
    batches = [r["booked"] for r in rows if r["booked"]]

    fig = style.figure(9.6, 6.4)
    gs = fig.add_gridspec(2, 1, height_ratios=[1.0, 1.25], hspace=0.38, left=0.09, right=0.98, top=0.93, bottom=0.09)
    a, b = fig.add_subplot(gs[0]), fig.add_subplot(gs[1])
    for ax in (a, b):
        style.style_ax(ax)
        ax.set_xlim(0, T_END)
        for t0, t1, _ in batches:
            ax.axvline(t1, color=style.MUTED, lw=0.7, ls=(0, (3, 2)), zorder=1)
    for i, (t0, t1, v) in enumerate(batches):
        a.text(0.5 * (t0 + t1), 0.063, f"batch {i + 1}", fontsize=7.2, ha="center", color=style.MUTED)

    # (a) rates
    tt = np.linspace(0, T_END, 1401)
    a.fill_between(tt, exchange_rate(tt), 0, color="#eb6834", alpha=0.18, lw=0)
    a.plot(tt, exchange_rate(tt), color="#eb6834", lw=1.3,
           label="2D exchange rate, evaluated over each batch when the batch fires")
    ts = [r["t0"] for r in rows] + [rows[-1]["t1"]]
    qs = [r["q"] for r in rows] + [rows[-1]["q"]]
    a.step(ts, qs, where="post", color=style.WATER, lw=1.6,
           label="1D coupling inflow: the queue drained at queue / remaining")
    a.axhline(0, color=style.INK, lw=0.6)
    a.set_ylim(-0.04, 0.072)
    a.set_ylabel("rate (m³/s)")
    a.legend(loc="lower right", fontsize=7.4)
    a.set_title("(a) Booked at batch close, delivered at a uniform rate over the next batch span",
                loc="left", fontsize=9.5)
    a.text(1.0, -0.031, "positive: 2D to 1D (drain); negative: 1D to 2D (spill)", fontsize=7.2,
           color=style.MUTED)

    # (b) cumulative volumes and the pending band
    t_pts = [0.0] + [r["t1"] for r in rows]
    led = [0.0] + [r["ledger"] for r in rows]
    dlv = [0.0] + [r["delivered"] for r in rows]
    b.step(t_pts, led, where="post", color="#eb6834", lw=1.6, label="out − in: the surface ledger, booked per batch")
    b.plot(t_pts, dlv, color=style.WATER, lw=1.6, marker="o", ms=2.8,
           label="delivered to the 1D node (each marker ends a routing step)")
    # within a step the ledger holds its booked value and delivery ramps: pending is the gap
    for r_prev, r in zip([dict(t1=0.0, delivered=0.0, ledger=0.0)] + rows[:-1], rows):
        x = [r_prev["t1"], r["t1"]]
        b.fill_between(x, [r_prev["delivered"], r["delivered"]], [r_prev["ledger"], r_prev["ledger"]],
                       color="#8a3ffc", alpha=0.16, lw=0)
    b.plot([], [], color="#8a3ffc", alpha=0.35, lw=6, label="pending = coupling_queue + coupling_volume")
    b.axhline(0, color=style.INK, lw=0.6)
    b.set_xlabel("time (s)")
    b.set_ylabel("cumulative volume (m³)")
    b.legend(loc="upper left", fontsize=7.4)
    b.set_title("(b) delivered + pending = out − in at the end of every routing step", loc="left", fontsize=9.5)
    final = rows[-1]
    b.text(T_END - 2, final["ledger"] + 0.12,
           f"queue drained: pending {final['pending']:.2f} m³,\ndelivered = out − in = {final['ledger']:.2f} m³",
           fontsize=7.2, ha="right", va="bottom", color=style.INK, linespacing=1.3)
    b.set_ylim(-0.1, max(led) * 1.32)
    print(f"    batches: " + ", ".join(f"[{t0:.0f},{t1:.0f}] {v:+.3f} m3" for t0, t1, v in batches))
    print(f"    out-in {final['ledger']:.4f} m3, delivered {final['delivered']:.4f} m3, "
          f"max |delivered + pending - (out - in)| = {resid:.2e} m3")
    plt.figure(fig.number)
    sink.save(fig, FIG_ID)
