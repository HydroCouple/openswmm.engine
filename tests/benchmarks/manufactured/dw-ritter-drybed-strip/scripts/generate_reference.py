#!/usr/bin/env python3
"""
Generate reference CSV for the dw-ritter-drybed-strip benchmark.

Ritter (1892) exact solution for a 1D frictionless dry-bed dam break
on a horizontal rectangular channel.  US customary units throughout to
match the DW solver's hardcoded GRAVITY = 32.2 ft/s².

Strip: L = 250 ft, dam at x_d = 125 ft, h0 = 1.0 ft, g = 32.2 ft/s².
Discretised into 51 nodes spaced 5 ft apart (nodes 0–50).
Reference is sampled at t ∈ {2, 4, 6, 8} s — all before t_max ≈ 11.0 s
when the wet front reaches the downstream boundary.

Output: ../reference.csv
Columns: t_s, xi_ft, h_ft, u_fps
  t_s    — elapsed time (s)
  xi_ft  — local coordinate xi = x_abs - x_d (ft)
  h_ft   — water depth (ft)
  u_fps  — depth-averaged velocity, positive downstream (ft/s)
"""

import csv
import math
import pathlib

# ── parameters ──────────────────────────────────────────────────────────────
G    = 32.2      # ft/s²  (matches openswmm::constants::GRAVITY)
H0   = 1.0       # ft
L    = 250.0     # ft (strip length)
X_D  = 125.0     # ft (dam location from upstream end)
N    = 51        # number of nodes (spacing DX = L/(N-1) = 5 ft)
DX   = L / (N - 1)
TIMES = [2.0, 4.0, 6.0, 8.0]  # s

C0 = math.sqrt(G * H0)          # wave celerity = 5.6745… ft/s
T_MAX = (L - X_D) / (2.0 * C0)  # ≈ 11.01 s — solution valid up to here

# ── Ritter solution ──────────────────────────────────────────────────────────

def ritter(xi: float, t: float) -> tuple[float, float]:
    """
    Return (h [ft], u [ft/s]) for local coordinate xi = x - x_d, time t > 0.
    Three-region structure:
      undisturbed reservoir : xi <= -c0*t
      rarefaction fan       : -c0*t < xi < 2*c0*t
      dry bed               : xi >= 2*c0*t
    """
    xi_back  = -C0 * t
    xi_front =  2.0 * C0 * t
    if xi <= xi_back:
        return H0, 0.0
    if xi >= xi_front:
        return 0.0, 0.0
    # rarefaction fan
    term = 2.0 * C0 - xi / t
    h = term * term / (9.0 * G)
    u = (2.0 / 3.0) * (C0 + xi / t)
    return h, u

# ── generate rows ────────────────────────────────────────────────────────────
rows = []
for t in TIMES:
    assert t < T_MAX, f"t={t} s exceeds t_max={T_MAX:.3f} s"
    for i in range(N):
        x   = DX * i
        xi  = x - X_D
        h, u = ritter(xi, t)
        rows.append((t, xi, h, u))

# ── write CSV ────────────────────────────────────────────────────────────────
out = pathlib.Path(__file__).parent.parent / "reference.csv"
with open(out, "w", newline="") as f:
    f.write("# Ritter (1892) exact solution — dry-bed dam break\n")
    f.write("# US customary: h0=1.0 ft, g=32.2 ft/s^2, L=250 ft, x_d=125 ft\n")
    f.write("# 51 nodes (xi = 5*i - 125 ft, i=0..50), t in {2,4,6,8} s\n")
    f.write("# t_max = (L-x_d)/(2*c0) = 125/(2*sqrt(32.2)) ≈ 11.01 s\n")
    f.write("# Columns: t_s [s], xi_ft [ft], h_ft [ft], u_fps [ft/s]\n")
    writer = csv.writer(f)
    writer.writerow(["t_s", "xi_ft", "h_ft", "u_fps"])
    for (t, xi, h, u) in rows:
        writer.writerow([
            f"{t:.1f}",
            f"{xi:.3f}",
            f"{h:.15g}",
            f"{u:.15g}",
        ])

print(f"Wrote {len(rows)} rows to {out}")
print(f"c0 = {C0:.6f} ft/s,  t_max = {T_MAX:.4f} s")
print(f"Dam-station check: 4*h0/9 = {4*H0/9:.6f} ft")
