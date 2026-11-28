"""The independent groundwater clock against the surface's tier ladder
(hydrology Chapter 9, §9.9.1).

Three aquifer cells are scheduled by a replica of the engine's clock and
compared with where the shared tier ladder would put them:

* rung selection from the stability bound, the accuracy limit and the
  accuracy floor, with the surface step as the allowance
  (GwClock::select, src/engine/2d/subsurface/GwClock.cpp:39-72);
* absolute deadlines on a 2^k x GW_BASE_STEP grid from the epoch
  (GwClock::next, :74-94; GwClock::period, :31-33);
* after a firing, the rung re-selected from the new limits, loosened only
  when the deadline is shared with the slower rung (GwClock::event,
  :186-213);
* coarse cells collected at the producer check that reaches their
  deadline, stiff cells (negative rungs) subcycled at their own deadlines
  (GwClock::check, :216-270);
* a budget-triggered early update when the receipts waiting for a cell
  reach budget_fraction x min(head budget, room), checked when a producer
  check crosses the cell's accuracy-floor period (IndependentOwner::early,
  src/engine/2d/subsurface/SubsurfaceClock.cpp:160-213; clockBudgets,
  :339-389), and the receipt-rate accuracy limit set after a firing
  (IndependentOwner::cells, :131-146; accuracy, :155-159).

Producer checks are the ends of the surface's base substeps
(src/engine/2d/solver/ExplicitInertialSolver.cpp:4274-4292, :4357-4358).
Under SHARED the base step is the smaller of the surface's and the
aquifer's finest bound and a cell's rung is capped by the ladder height
LTS_TIERS (ExplicitInertialSolver.cpp:1575-1580; SubsurfaceSolver.cpp
assignTiers). Defaults: GW_BASE_STEP AUTO = 1 s without node beds,
GW_MAX_STEP 3600 s, GW_BUDGET_FRACTION 0.5, GW_HEAD_TOLERANCE 0.05
(SubsurfaceData.hpp:142-153). The cells' bounds and the receipt burst are
illustrative. Synthetic; no model run.
"""
from __future__ import annotations

import math

REQUIRES = ()

BASE = 1.0              # GW_BASE_STEP AUTO without node beds (SubsurfaceData.hpp:151)
MAX_STEP = 3600.0       # GW_MAX_STEP default
MARGIN = 1.0            # the surface's MAX_TIMESTEP: the allowance a coarse cell waits for a check
DT0 = 0.5               # surface base substep under INDEPENDENT (GW_BINDS_SURFACE_DT NO)
LTS_TIERS = 4           # surface ladder height (default)
T_END = 64.0
BUDGET_FRACTION, HEAD_TOL = 0.5, 0.05
SY, AREA, HG = 0.10, 100.0, 2.0
ROOM_IN = BUDGET_FRACTION * HEAD_TOL * SY * AREA * max(0.01, HG)   # m3; the room itself is larger
BURST = (10.0, 40.0, 0.04)                                          # receipts into cell C: start, end, m3/s
CELLS = {"A": 0.4, "B": 9.0, "C": 40.0}                             # stability bounds, s


def ilogb(x):
    return math.frexp(x)[1] - 1


def period(k):
    return math.ldexp(BASE, k)


def quantize(limit):
    k = ilogb(limit) - ilogb(BASE)
    if period(k) > limit:
        k -= 1
    return k


def select(bound, accuracy=1e30, floor=0):
    """GwClock::select."""
    cap = MAX_STEP
    cap_margin = min(MARGIN, cap / 4)
    fine_cap = cap - cap_margin < BASE
    k = quantize(cap if fine_cap else cap - cap_margin)
    if fine_cap:
        k = min(k, -1)
    if bound < cap and bound - MARGIN < BASE:
        k = min(k, min(quantize(bound), -1))
    elif bound < cap and period(k) > bound - MARGIN:
        k = quantize(bound - MARGIN)
    limit_accuracy = max(period(floor), accuracy - MARGIN)
    if accuracy < cap and period(k) > limit_accuracy:
        k = min(k, max(floor, quantize(limit_accuracy)))
    return k


def next_due(after, k):
    """GwClock::next on an absolute grid from epoch 0."""
    p = period(k)
    due = math.floor(after / p) * p
    return due if due > after else due + p


def received(t0, t1):
    """Receipts booked into cell C over [t0, t1], m3."""
    a, b, rate = BURST
    return rate * max(0.0, min(t1, b) - max(t0, a))


def simulate():
    """Firing times per cell, early updates of C, and C's rung history."""
    st = {c: {"last": 0.0, "rung": select(b), "acc": 1e30} for c, b in CELLS.items()}
    for c in st:
        st[c]["due"] = next_due(0.0, st[c]["rung"])
    fired = {c: [] for c in CELLS}
    early, rungs = [], [(0.0, st["C"]["rung"])]

    def fire(c, t):
        s = st[c]
        if c == "C":
            got = received(s["last"], t)
            rate = got / (t - s["last"])
            s["acc"] = ROOM_IN / rate if rate > 0 else 1e30
        s["last"] = t
        fired[c].append(t)
        k = select(CELLS[c], max(BASE, s["acc"]))
        if k > s["rung"]:
            grid = round(s["due"] / period(k)) * period(k)
            if s["due"] > t or grid != s["due"]:
                k = s["rung"]
        s["rung"] = k
        s["due"] = next_due(t, k)
        if c == "C":
            rungs.append((t, k))

    prev, n = 0.0, 0
    while True:
        n += 1
        end = n * DT0
        if end > T_END + 1e-9:
            break
        # stiff cells subcycle at their own deadlines inside the check
        for c in st:
            while st[c]["rung"] < 0 and st[c]["due"] < end - 1e-12:
                fire(c, st[c]["due"])
        landing = [c for c in st if st[c]["due"] <= end + 1e-12]
        # budget wake: only when the check crosses the floor period (1 s here)
        if math.floor(end / BASE) > math.floor(prev / BASE) and "C" not in landing:
            if received(st["C"]["last"], end) >= ROOM_IN:
                landing.append("C")
                early.append(end)
        for c in landing:
            fire(c, end)
        prev = end
    return fired, early, rungs


def shared_periods():
    """Where the shared ladder puts the same cells: dt0 folds in the finest aquifer bound."""
    dt0 = min(DT0, min(CELLS.values()))
    out = {}
    for c, b in CELLS.items():
        r = max(0, ilogb(b / dt0)) if b >= dt0 else 0
        out[c] = (dt0, min(r, LTS_TIERS - 1))
    return out


def _draw(style, fired, early, rungs):
    import matplotlib.pyplot as plt
    import numpy as np
    INK, MUTED = style.INK, style.MUTED
    fig = style.figure(9.6, 6.2)
    gs = fig.add_gridspec(2, 1, height_ratios=[3.1, 1.25], hspace=0.34, left=0.255, right=0.875,
                          top=0.95, bottom=0.08)
    ax, bx = fig.add_subplot(gs[0]), fig.add_subplot(gs[1], sharex=None)
    style.style_ax(ax)
    ax.grid(False)
    rows = []
    surf_col, gw_col, sh_col = "#9a978f", style.COLORS["fv"], style.COLORS["fv-lts"]
    for k in range(LTS_TIERS):
        p = DT0 * 2 ** k
        lab = f"surface tier {k}, {p:g} s" + (": producer checks" if k == 0 else "")
        rows.append((lab, np.arange(p, T_END + 1e-9, p), surf_col, 0.9))
    nice = {"A": "A, stiff", "B": "B", "C": "C, burst"}
    for c in ("A", "B", "C"):
        k0 = select(CELLS[c])
        rows.append((f"{nice[c]}: bound {CELLS[c]:g} s → {period(k0):g} s, k = {k0}".replace("-", "−"),
                     np.array(fired[c]), gw_col, 1.2))
    sh = shared_periods()
    for c in ("A", "C"):
        dt0, r = sh[c]
        p = dt0 * 2 ** r
        rows.append((f"{c} under SHARED: rung {r}, {p:g} s", np.arange(p, T_END + 1e-9, p), sh_col, 1.1))
    n = len(rows)
    for i, (lab, ts, col, lw) in enumerate(rows):
        y = n - i
        ax.vlines(ts, y - 0.32, y + 0.32, color=col, lw=lw)
        ax.text(-0.8, y, lab, ha="right", va="center", fontsize=7.6, color=INK)
    # group labels and separators
    for y in (n - LTS_TIERS + 0.5, n - LTS_TIERS - 3 + 0.5):
        ax.axhline(y, color=style.GRIDC, lw=0.9)
    ax.text(T_END + 1.2, n - 1.5, "surface\nmarcher", fontsize=7.6, color=surf_col, va="center", linespacing=1.3)
    ax.text(T_END + 1.2, n - LTS_TIERS - 1, "GW_CLOCK\nINDEPENDENT", fontsize=7.6, color=gw_col, va="center",
            linespacing=1.3)
    ax.text(T_END + 1.2, 1.5, "GW_CLOCK\nSHARED", fontsize=7.6, color=sh_col, va="center", linespacing=1.3)
    # the burst and the early update on cell C's row
    yc = n - LTS_TIERS - 2
    a, b, _ = BURST
    from matplotlib.patches import Rectangle
    ax.add_patch(Rectangle((a, yc - 0.4), b - a, 0.8, fc=gw_col, alpha=0.12, lw=0))
    ax.text(a + 0.4, yc - 0.36, "receipts arriving", fontsize=7.0, color=gw_col, ha="left", va="bottom")
    for t in early:
        ax.plot([t], [yc + 0.45], marker="v", ms=7, color=style.COLORS["dw-slot"], mec=INK, mew=0.6, zorder=5)
        ax.text(t + 0.7, yc + 0.02, f"early update\nat {t:g} s", fontsize=7.0, color=INK, ha="left",
                va="center", linespacing=1.2)
    ax.set_ylim(0.4, n + 1.0)
    ax.set_xlim(0, T_END)
    ax.set_yticks([])
    ax.spines["left"].set_visible(False)
    ax.set_xlabel("time, s")
    ax.set_title("(a) When each cell fires: surface tiers, the independent clock, and the shared ladder",
                 fontsize=9, loc="left")

    # (b) receipts waiting at cell C against its budget
    style.style_ax(bx)
    t = np.linspace(0, T_END, 2561)
    last = np.zeros_like(t)
    fc = np.array(fired["C"])
    for i, ti in enumerate(t):
        prior = fc[fc < ti - 1e-12]
        last[i] = prior.max() if prior.size else 0.0
    pending = np.array([received(l, ti) for l, ti in zip(last, t)])
    bx.plot(t, pending, color=gw_col, lw=1.4, label="receipts waiting at cell C")
    bx.axhline(ROOM_IN, color=style.COLORS["dw-slot"], lw=1.0, ls=(0, (4, 2)),
               label=f"budget: fraction × head term = {ROOM_IN:g} m³")
    for ti in fc:
        bx.axvline(ti, color=MUTED, lw=0.6, ls=":")
    bx.axvspan(a, b, color=gw_col, alpha=0.08, lw=0)
    bx.set_xlim(0, T_END)
    bx.set_ylim(0, 0.8)
    bx.set_xlabel("time, s")
    bx.set_ylabel("m³")
    bx.legend(loc="upper right", fontsize=7.4, ncol=2)
    bx.set_title(f"(b) Cell C: receipts of {BURST[2]:g} m³/s from {a:g} to {b:g} s, gathered at each firing "
                 "(dotted)", fontsize=9, loc="left")
    return fig


def build(sink):
    import style
    fired, early, rungs = simulate()
    print(f"    cell A fires {len(fired['A'])} times, B at {fired['B']}, C at {fired['C']}; early {early}; "
          f"C rungs {rungs}")
    print(f"    shared: {shared_periods()}")
    sink.save(_draw(style, fired, early, rungs), "hydrology_ch9_gw_clock_timeline")
