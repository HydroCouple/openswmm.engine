#!/usr/bin/env python3
"""
Generate the MS-B fixture: a COUPLED MULTISCALE model — the graded fine-patch
watershed mesh of gen_multiscale_mesh.py with a small 1D pipe network buried
under the fine patch, coupled through [2D_VERTEX_NODE_MAP] orifice inlets.

WHY (2D reformulation plan, Phase 0.3):
  MS-A (gen_multiscale_mesh.py) exposes the pure-2D multi-scale stiffness
  (10^4:1 cell-area ratio). Bellinge-class reality adds the OTHER stressor on
  top: stiff 1D<->2D orifice exchange at manholes sitting exactly on the
  smallest cells. This fixture is the Bellinge-shaped synthetic: a fine coupled
  urban core inside a coarse watershed, small enough to run in seconds, with a
  closed 2D boundary so the exchange ledger is exactly checkable
  (rain in == 2D storage + drained-to-1D volume).

  Gates that read this fixture (see the reformulation plan):
  - exchange-ledger closure (repro-style, 0.000%-class 2D continuity);
  - minimum 1D routing step >= 1 s throughout (no exchange-driven collapse);
  - BDF step count vs the uncoupled MS-A run (coupling-stiffness overhead);
  - post-recession "uphill ratchet" volume stranded on coarse cells.

Usage:
  gen_coupled_multiscale.py OUT.inp
      [--dx-coarse 200] [--dx-fine 2] [--n-coarse-side 12] [--n-fine 24]
      [--n-trans 8] [--slope 0.001] [--patch-depth 0.5] [--n-inlets 5]
      [--bury 1.0] [--pipe-diam 0.5] [--coupling-cd 0.65] [--coupling-area 1.0]
      [--routing-step 4] [--hours 3] [--rain-mm-hr 50] [--rain-min 60]
      [--mannings 0.03]

Defaults mirror gen_multiscale_mesh.py (same 8192-triangle grid, 100x edge /
10^4x area ratio) plus a 5-inlet chain draining the depression to one outfall.
"""
import argparse
import math


def graded_spacings(dx_coarse, dx_fine, n_coarse_side, n_fine, n_trans):
    """Same symmetric grading as gen_multiscale_mesh.py (keep in lockstep)."""
    down = []
    for k in range(1, n_trans + 1):
        f = k / (n_trans + 1)
        down.append(dx_coarse * (dx_fine / dx_coarse) ** f)
    up = list(reversed(down))
    return ([dx_coarse] * n_coarse_side + down +
            [dx_fine] * n_fine + up +
            [dx_coarse] * n_coarse_side)


def coords_from_spacings(spac):
    xs = [0.0]
    for s in spac:
        xs.append(xs[-1] + s)
    return xs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--dx-coarse", type=float, default=200.0)
    ap.add_argument("--dx-fine", type=float, default=2.0)
    ap.add_argument("--n-coarse-side", type=int, default=12)
    ap.add_argument("--n-fine", type=int, default=24)
    ap.add_argument("--n-trans", type=int, default=8)
    ap.add_argument("--slope", type=float, default=0.001)
    ap.add_argument("--z0", type=float, default=100.0)
    ap.add_argument("--patch-depth", type=float, default=0.5)
    ap.add_argument("--n-inlets", type=int, default=5,
                    help="coupled junctions spread across the fine patch")
    ap.add_argument("--bury", type=float, default=1.0,
                    help="junction invert depth below the local bed (m)")
    ap.add_argument("--pipe-diam", type=float, default=0.5)
    ap.add_argument("--coupling-cd", type=float, default=0.65)
    ap.add_argument("--coupling-area", type=float, default=1.0)
    ap.add_argument("--routing-step", type=int, default=4)
    ap.add_argument("--hours", type=float, default=3.0)
    ap.add_argument("--rain-mm-hr", type=float, default=50.0)
    ap.add_argument("--rain-min", type=int, default=60)
    ap.add_argument("--mannings", type=float, default=0.03)
    a = ap.parse_args()

    spac = graded_spacings(a.dx_coarse, a.dx_fine, a.n_coarse_side,
                           a.n_fine, a.n_trans)
    xs = coords_from_spacings(spac)
    ys = coords_from_spacings(spac)
    nvx, nvy = len(xs), len(ys)
    nx, ny = nvx - 1, nvy - 1
    n_tri = 2 * nx * ny
    Lx, Ly = xs[-1], ys[-1]

    p_lo = a.n_coarse_side + a.n_trans
    p_hi = p_lo + a.n_fine
    x_patch_lo, x_patch_hi = xs[p_lo], xs[p_hi]
    y_patch_lo, y_patch_hi = ys[p_lo], ys[p_hi]

    def vid(ix, iy):
        return iy * nvx + ix

    def in_patch(coord, lo, hi):
        return lo <= coord <= hi

    def zv(ix, iy):
        x, y = xs[ix], ys[iy]
        z = a.z0 + a.slope * (Lx - x)
        if a.patch_depth > 0 and in_patch(x, x_patch_lo, x_patch_hi) and \
           in_patch(y, y_patch_lo, y_patch_hi):
            fx = (x - x_patch_lo) / max(x_patch_hi - x_patch_lo, 1e-9)
            fy = (y - y_patch_lo) / max(y_patch_hi - y_patch_lo, 1e-9)
            bump = (0.5 - 0.5 * math.cos(2 * math.pi * fx)) * \
                   (0.5 - 0.5 * math.cos(2 * math.pi * fy))
            z -= a.patch_depth * bump
        return z

    # Inlets: spread along the fine patch's center row so the deepest ponding
    # (depression center) and the patch edges are all sampled.
    iy_c = p_lo + a.n_fine // 2
    inlet_ix = [p_lo + max(1, (a.n_fine - 2)) * k // max(a.n_inlets - 1, 1)
                for k in range(a.n_inlets)]
    inlet_ix = sorted({min(max(ix, p_lo + 1), p_hi - 1) for ix in inlet_ix})
    inlets = [(vid(ix, iy_c), ix) for ix in inlet_ix]

    end_h = int(a.hours)
    end_m = int(round((a.hours - end_h) * 60))
    end_time = f"{end_h:02d}:{end_m:02d}:00"

    w = []
    P = w.append
    P("[TITLE]")
    P(f";; MS-B COUPLED MULTISCALE: fine patch {a.dx_fine:g} m in coarse "
      f"{a.dx_coarse:g} m watershed, {len(inlets)} orifice inlets under the patch")
    P(f";; {nx}x{ny} cells = {n_tri} triangles; edge ratio "
      f"{max(spac)/min(spac):.0f}x (area {(max(spac)/min(spac))**2:.0f}x)")
    P("")
    P("[OPTIONS]")
    P("FLOW_UNITS           CMS")
    P("INFILTRATION         HORTON")
    P("FLOW_ROUTING         DYNWAVE")
    P("LINK_OFFSETS         DEPTH")
    P("MIN_SLOPE            0")
    P("ALLOW_PONDING        NO")
    P("SKIP_STEADY_STATE    NO")
    P("START_DATE           01/01/2026")
    P("START_TIME           00:00:00")
    P("REPORT_START_DATE    01/01/2026")
    P("REPORT_START_TIME    00:00:00")
    P("END_DATE             01/01/2026")
    P(f"END_TIME             {end_time}")
    P("REPORT_STEP          0:05:00")
    P("WET_STEP             0:01:00")
    P("DRY_STEP             0:05:00")
    P(f"ROUTING_STEP         {a.routing_step}")
    P("VARIABLE_STEP        0.75")
    P("")
    P("[RAINGAGES]")
    P("GAGE1            INTENSITY 0:15   1.00  TIMESERIES STORM")
    P("")
    P("[TIMESERIES]")
    t = 0
    while t <= a.rain_min:
        P(f"STORM            {t//60}:{t%60:02d}      {a.rain_mm_hr:.3f}")
        t += 15
    P(f"STORM            {(a.rain_min+15)//60}:{(a.rain_min+15)%60:02d}      0.000")
    P("")
    P("[JUNCTIONS]")
    P(";;Name  InvertElev  MaxDepth  InitDepth  SurDepth  Aponded")
    jnames = []
    for k, (v, ix) in enumerate(inlets):
        iy = iy_c
        inv = zv(ix, iy) - a.bury
        name = f"JC{k}"
        jnames.append((name, v, ix, inv))
        # MaxDepth == bury puts the junction RIM exactly at the local bed, so
        # ponded surface water sits on the inlet mouth and free-inlet capture
        # can fire (rim above the bed ⇒ the orifice never sees the pond).
        P(f"{name}   {inv:.3f}   {a.bury:.2f}   0   0   0")
    P("")
    # Outfall well below the last junction so the chain always drains freely.
    out_inv = min(j[3] for j in jnames) - 2.0
    P("[OUTFALLS]")
    P(f"OF1   {out_inv:.3f}   FREE   NO")
    P("")
    P("[CONDUITS]")
    P(";;Name  From  To  Length  N  InOff  OutOff  InitFlow")
    # Virtual lengths floored at 30 m: the physical inlet spacing (a few m in
    # the fine patch) would collapse the 1D Courant step for no physical gain.
    for k in range(len(jnames) - 1):
        P(f"PC{k}   {jnames[k][0]}   {jnames[k+1][0]}   "
          f"{max(abs(xs[jnames[k+1][2]] - xs[jnames[k][2]]), 30.0):.1f}"
          f"   0.013   0   0   0")
    P(f"PC{len(jnames)-1}   {jnames[-1][0]}   OF1   50   0.013   0   0   0")
    P("")
    P("[XSECTIONS]")
    for k in range(len(jnames)):
        P(f"PC{k}   CIRCULAR   {a.pipe_diam:g}   0 0 0 1")
    P("")
    P("[COORDINATES]")
    for name, v, ix, _ in jnames:
        P(f"{name}   {xs[ix]:.2f}   {ys[iy_c]:.2f}")
    P(f"OF1   {xs[jnames[-1][2]] + 60:.2f}   {ys[iy_c]:.2f}")
    P("")
    P("[2D_OPTIONS]")
    P("MAX_TIMESTEP           30")
    P("DRY_DEPTH              0.001")
    P("LIMITER_EPSILON        1e-06")
    P(f"COUPLING_CD            {a.coupling_cd:g}")
    P("RAINFALL_MODE          SYSTEM")
    P("REPORT_2D              YES")
    P("")
    P("[2D_VERTICES]")
    P(";;X  Y  Z")
    rows = []
    for iy in range(nvy):
        for ix in range(nvx):
            rows.append(f"{xs[ix]:.4f} {ys[iy]:.4f} {zv(ix, iy):.4f}")
    P("\n".join(rows))
    P("")
    P("[2D_TRIANGLES]")
    P(";;V1 V2 V3 MANNINGS TAG")
    tris = []
    n = a.mannings
    for iy in range(ny):
        in_y = in_patch(0.5 * (ys[iy] + ys[iy+1]), y_patch_lo, y_patch_hi)
        for ix in range(nx):
            in_x = in_patch(0.5 * (xs[ix] + xs[ix+1]), x_patch_lo, x_patch_hi)
            tag = " urban" if (in_x and in_y) else ""
            sw = vid(ix, iy);     se = vid(ix+1, iy)
            nw = vid(ix, iy+1);   ne = vid(ix+1, iy+1)
            if (ix + iy) % 2 == 0:
                tris.append(f"{sw} {se} {ne} {n:g}{tag}")
                tris.append(f"{sw} {ne} {nw} {n:g}{tag}")
            else:
                tris.append(f"{sw} {se} {nw} {n:g}{tag}")
                tris.append(f"{se} {ne} {nw} {n:g}{tag}")
    P("\n".join(tris))
    P("")
    P("[2D_VERTEX_NODE_MAP]")
    P(";;VertexIdx  Node  Cd  Area")
    for name, v, ix, _ in jnames:
        P(f"{v}   {name}   {a.coupling_cd:g}   {a.coupling_area:g}")
    P("")

    with open(a.out, "w") as f:
        f.write("\n".join(w))

    print(f"wrote {a.out}: {n_tri} triangles, {len(jnames)} coupled inlets "
          f"(vertices {[j[1] for j in jnames]}), edge ratio "
          f"{max(spac)/min(spac):.0f}x, outfall OF1 inv {out_inv:.2f} m")


if __name__ == "__main__":
    main()
