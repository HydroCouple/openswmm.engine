#!/usr/bin/env python3
"""
Generate reference CSV for the exfil-storage-geometry-greenampt benchmark.

Fixed-stage storage node (depth = 2.0 ft) with analytical area A(d) = 50 + 100 d.
At depth 2.0 ft the bottom seepage area is 50 ft^2 (c1_bottom = 0.5 ft) and the
bank seepage area is A(2) - 50 = 200 ft^2 (c1_bank = 0.3 ft; SWMM 5
exfil.c: bank area = MIN(area, bankMaxArea) - btmArea). Each component follows the
exact saturated Green-Ampt relation

    t(F) = [F - c1*ln(1 + F/c1)] / Ks

inverted for cumulative depth F at each sample time. The cumulative exfiltration
volume is

    E(t) = 50*F_bottom(t) + 200*F_bank(t)        [ft^3]

and the per-interval average rate is (E[i] - E[i-1]) / dt [cfs]. Values are
written at full (round-trippable) double precision per shared/conventions.md.

Output: ../reference.csv
Columns: t_s, exfil_rate_cfs, exfil_cumul_ft3
"""

import csv
import math
import pathlib

# ── parameters (US customary) ────────────────────────────────────────────────
KS         = 1.0e-4    # ft/s (4.32 in/hr)
C1_BOTTOM  = 0.5       # ft  (suction 0.5 ft, bottom)
C1_BANK    = 0.3       # ft  (bank)
A_BOTTOM   = 50.0      # ft^2
A_BANK     = 200.0     # ft^2  (A(2) - A_bottom)
DT         = 60.0      # s
N_STEPS    = 10        # -> t = 0,60,...,600 s


def F_of_t(t: float, c1: float) -> float:
    """Invert t(F) = [F - c1*ln(1+F/c1)]/Ks for F given t (Newton + bisection)."""
    if t <= 0.0:
        return 0.0
    target = KS * t                      # solve F - c1*ln(1+F/c1) = target
    # g(F) = F - c1*ln(1+F/c1) - target ;  g'(F) = F/(F+c1)  (>0, increasing)
    F = target + c1                      # robust initial guess (F >= target)
    for _ in range(200):
        g  = F - c1 * math.log(1.0 + F / c1) - target
        gp = F / (F + c1)
        step = g / gp
        F -= step
        if F <= 0.0:
            F = target * 0.5 + 1e-9      # keep iterate positive
        if abs(step) < 1e-15:
            break
    return F


# ── generate + self-check ──────────────────────────────────────────────────────
times = [DT * i for i in range(N_STEPS + 1)]
cumul = []
for t in times:
    Fb = F_of_t(t, C1_BOTTOM)
    Fk = F_of_t(t, C1_BANK)
    # residual self-check: the inverted F must satisfy the relation to ~machine eps
    for (F, c1) in ((Fb, C1_BOTTOM), (Fk, C1_BANK)):
        if F > 0.0:
            resid = (F - c1 * math.log(1.0 + F / c1)) - KS * t
            assert abs(resid) < 1e-11, f"GA inversion residual {resid} at t={t}"
    cumul.append(A_BOTTOM * Fb + A_BANK * Fk)

rows = []
for i, t in enumerate(times):
    rate = 0.0 if i == 0 else (cumul[i] - cumul[i - 1]) / DT
    rows.append((t, rate, cumul[i]))

for (_, r0, E0), (_, r1, E1) in zip(rows[1:], rows[2:]):
    assert E1 > E0 and r1 <= r0, "cumulative must grow, rate must relax"

# ── write CSV ────────────────────────────────────────────────────────────────
out = pathlib.Path(__file__).parent.parent / "reference.csv"
with open(out, "w", newline="") as fh:
    fh.write("# Fixed-stage storage exfiltration, analytic geometry + saturated Green-Ampt\n")
    fh.write("# A(d) = 50 + 100 d; at d=2 ft: bottom 50 ft^2 (c1=0.5), bank A(2)-50 = 200 ft^2 (c1=0.3)\n")
    fh.write("# Ks = 1e-4 ft/s.  t(F) = [F - c1 ln(1 + F/c1)] / Ks inverted per component\n")
    fh.write("# E(t) = 50 F_bottom(t) + 200 F_bank(t) [ft^3]; rate = dE/dt averaged over interval\n")
    fh.write("# Full double precision.\n")
    writer = csv.writer(fh)
    writer.writerow(["t_s", "exfil_rate_cfs", "exfil_cumul_ft3"])
    for (t, rate, E) in rows:
        writer.writerow([int(t), repr(rate), repr(E)])

print(f"Wrote {len(rows)} rows to {out}")
print(f"E(600) = {cumul[-1]!r} ft^3")
