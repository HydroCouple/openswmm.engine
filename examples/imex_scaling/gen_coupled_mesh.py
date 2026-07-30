#!/usr/bin/env python3
"""
Generate a COUPLING-HEAVY 2D model: a uniform surface mesh draining ONLY through
a dense grid of 1D<->2D inlet couplings into a pipe network, scalable in cell
count and coupling-point count together.

WHY (verification experiment #2, IMEX_SCALING_VERIFICATION_NOTE.md §6):
  The original bottleneck that motivated the whole IMEX effort
  (2D_SOLVER_STEPPING_PERFORMANCE_PLAN.md) was the STIFF 1D<->2D orifice coupling
  collapsing CVODE's step. But every scaling mesh used so far is rainfall-driven,
  walls-only, with NO coupling — so "CVODE-DW does 1M in 12.5 s" was established
  only for the EASY (uncoupled) regime. This mesh puts the coupling back: the 2D
  surface is closed (walls), so every drop of rainfall must leave through an
  orifice coupling into the pipe network. Coupling points scale with the mesh.

  Run CVODE-DW on this at increasing size, with and without the LIVE
  (state-dependent, stiff) coupling path (OPENSWMM_2D_LIVE_COUPLING), and watch
  the step count / mean step / wall time. This measures whether CVODE-DW still
  scales when the coupling stiffness — the actual original problem — is present.

NETWORK DESIGN (kept parser-simple and guaranteed-draining):
  - Inlets placed on a sub-grid of 2D vertices, every STRIDE vertices in x and y.
  - Each inlet vertex couples to a buried junction (elev = bed - BURY) via the
    orifice equation in [2D_VERTEX_NODE_MAP].
  - Junctions in each inlet-ROW are chained downslope (+X) in series and discharge
    to a FREE outfall at the row's downslope end. Conduits are generously sized so
    the network never becomes the throttle (we are stressing the COUPLING, not the
    pipes).

Usage:
  gen_coupled_mesh.py NX NY OUT.inp
      [--dx 10] [--dy 10] [--stride 4] [--bury 1.0] [--slope 0.002]
      [--pipe-diam 0.5] [--routing-step 30] [--hours 2] [--rain-mm-hr 40]
      [--rain-min 60] [--mannings 0.03] [--coupling-cd 0.65] [--coupling-area 1.0]

Example: gen_coupled_mesh.py 100 100 coupled_20k.inp  (20k tris, ~676 couplings).
"""
import argparse


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("nx", type=int)
    ap.add_argument("ny", type=int)
    ap.add_argument("out")
    ap.add_argument("--dx", type=float, default=10.0)
    ap.add_argument("--dy", type=float, default=10.0)
    ap.add_argument("--stride", type=int, default=4,
                    help="couple every Nth vertex in x and y")
    ap.add_argument("--bury", type=float, default=1.0,
                    help="junction invert depth below the local 2D bed (m)")
    ap.add_argument("--slope", type=float, default=0.002)
    ap.add_argument("--z0", type=float, default=100.0)
    ap.add_argument("--pipe-diam", type=float, default=0.5)
    ap.add_argument("--routing-step", type=int, default=30)
    ap.add_argument("--hours", type=float, default=2.0)
    ap.add_argument("--rain-mm-hr", type=float, default=40.0)
    ap.add_argument("--rain-min", type=int, default=60)
    ap.add_argument("--mannings", type=float, default=0.03)
    ap.add_argument("--coupling-cd", type=float, default=0.65)
    ap.add_argument("--coupling-area", type=float, default=1.0)
    a = ap.parse_args()

    nx, ny, dx, dy = a.nx, a.ny, a.dx, a.dy
    nvx, nvy = nx + 1, ny + 1
    n_tri = 2 * nx * ny
    Lx = nx * dx

    def vid(ix, iy):
        return iy * nvx + ix

    def zv(ix):                       # slope toward +X
        return a.z0 + a.slope * (Lx - ix * dx)

    # Inlet vertices: every STRIDE in x and y (skip the very downslope column so
    # each row has a clear downstream run to its outfall).
    inlet_ix = list(range(0, nvx, a.stride))
    inlet_iy = list(range(0, nvy, a.stride))
    couplings = []        # (vertex_id, junction_name, x, y, bed_z)
    for iy in inlet_iy:
        for ix in inlet_ix:
            couplings.append((vid(ix, iy), f"J_{ix}_{iy}", ix*dx, iy*dy, zv(ix)))

    end_h = int(a.hours)
    end_m = int(round((a.hours - end_h) * 60))
    end_time = f"{end_h:02d}:{end_m:02d}:00"

    w = []
    P = w.append
    P("[TITLE]")
    P(f";; COUPLING-HEAVY 2D mesh {nx}x{ny} = {n_tri} triangles; "
      f"{len(couplings)} 1D<->2D orifice couplings (stride {a.stride})")
    P(";; closed 2D (walls) -> all rainfall exits via orifice coupling into pipes")
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
    P("REPORT_STEP          0:15:00")
    P("WET_STEP             0:01:00")
    P("DRY_STEP             0:05:00")
    P(f"ROUTING_STEP         0:00:{a.routing_step:02d}" if a.routing_step < 60
      else f"ROUTING_STEP         0:0{a.routing_step//60}:{a.routing_step%60:02d}")
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

    # ---- 1D network: buried junctions, chained per inlet-row to a row outfall.
    P("[JUNCTIONS]")
    P(";;Name      Elev   MaxDepth InitDepth SurDepth Aponded")
    for (vid_, jn, x, y, bed) in couplings:
        inv = bed - a.bury
        P(f"{jn:12s} {inv:.3f}  {a.bury+5:.2f}     0         0        0")
    P("")
    P("[OUTFALLS]")
    P(";;Name      Elev   Type  Stage Gated")
    outfalls = {}
    for iy in inlet_iy:
        bed_out = zv(inlet_ix[-1])
        on = f"OUT_{iy}"
        outfalls[iy] = (on, bed_out - a.bury - 1.0)
        P(f"{on:12s} {outfalls[iy][1]:.3f}  FREE        NO")
    P("")
    P("[CONDUITS]")
    P(";;Name      From          To            Length N      Z1 Z2 Q0")
    clen = a.stride * dx
    for iy in inlet_iy:
        row = [ix for ix in inlet_ix]
        for j in range(len(row) - 1):
            frm = f"J_{row[j]}_{iy}"
            to  = f"J_{row[j+1]}_{iy}"
            P(f"C_{row[j]}_{iy}_x   {frm:12s} {to:12s} {clen:.1f}  0.012  0  0  0")
        # last junction in the row -> row outfall
        frm = f"J_{row[-1]}_{iy}"
        on  = outfalls[iy][0]
        P(f"C_{row[-1]}_{iy}_o  {frm:12s} {on:12s} {clen:.1f}  0.012  0  0  0")
    P("")
    P("[XSECTIONS]")
    P(";;Link  Shape  Geom1  Geom2 Geom3 Geom4 Barrels")
    for iy in inlet_iy:
        row = [ix for ix in inlet_ix]
        for j in range(len(row) - 1):
            P(f"C_{row[j]}_{iy}_x   CIRCULAR  {a.pipe_diam:g}  0 0 0 1")
        P(f"C_{row[-1]}_{iy}_o  CIRCULAR  {a.pipe_diam:g}  0 0 0 1")
    P("")
    P("[COORDINATES]")
    P(";;Node  X  Y")
    for (vid_, jn, x, y, bed) in couplings:
        P(f"{jn:12s} {x:.1f}  {y:.1f}")
    for iy in inlet_iy:
        on = outfalls[iy][0]
        P(f"{on:12s} {Lx + 2*dx:.1f}  {iy*dy:.1f}")
    P("")

    P("[2D_OPTIONS]")
    P("MAX_TIMESTEP           60")
    P("DRY_DEPTH              0.0001")
    P("LIMITER_EPSILON        1e-06")
    P(f"COUPLING_CD            {a.coupling_cd:g}")
    P("REPORT_2D              NO")
    P("")
    P("[2D_VERTICES]")
    P(";;X  Y  Z")
    rows = []
    for iy in range(nvy):
        for ix in range(nvx):
            rows.append(f"{ix*dx:g} {iy*dy:g} {zv(ix):.4f}")
    P("\n".join(rows))
    P("")
    P("[2D_TRIANGLES]")
    P(";;V1 V2 V3 MANNINGS")
    tris = []
    n = a.mannings
    for iy in range(ny):
        for ix in range(nx):
            sw = vid(ix, iy);     se = vid(ix+1, iy)
            nw = vid(ix, iy+1);   ne = vid(ix+1, iy+1)
            if (ix + iy) % 2 == 0:
                tris.append(f"{sw} {se} {ne} {n:g}")
                tris.append(f"{sw} {ne} {nw} {n:g}")
            else:
                tris.append(f"{sw} {se} {nw} {n:g}")
                tris.append(f"{se} {ne} {nw} {n:g}")
    P("\n".join(tris))
    P("")
    P("[2D_VERTEX_NODE_MAP]")
    P(";;VERTEX  SWMM_NODE   CD      AREA")
    for (vid_, jn, x, y, bed) in couplings:
        P(f"{vid_:8d}  {jn:12s} {a.coupling_cd:g}  {a.coupling_area:g}")
    P("")
    # Closed 2D basin: walls everywhere (no [2D_BOUNDARY_CONDITIONS]), so the
    # ONLY surface outlet is the orifice coupling -> maximal coupling stress.

    with open(a.out, "w") as f:
        f.write("\n".join(w))
    print(f"wrote {a.out}: {n_tri} triangles, {len(couplings)} couplings "
          f"({len(inlet_ix)}x{len(inlet_iy)} inlet grid, stride {a.stride}), "
          f"{len(inlet_iy)} outfalls, domain {Lx:g}x{ny*dy:g} m")
    print("  run CVODE-DW with and without OPENSWMM_2D_LIVE_COUPLING=1 to test "
          "the stiff-coupling regime.")


if __name__ == "__main__":
    main()
