#!/usr/bin/env python3
"""
Generate a PURE-2D synthetic surface-routing model for IMEX scaling tests.

Isolates the 2D solver from 1D-coupling throttling so CVODE (BDF) and ARKODE
(ARKStep IMEX) can be compared as the mesh grows. Per
docs/IMEX_LOCAL_INERTIAL_IMPLEMENTATION_PLAN.md §7.

Design (the "large representative cell" hydrological regime the work targets):
  - Flat-ish plane, NX x NY quads, union-jack ("alternating diagonal")
    triangulation -> 2*NX*NY triangles. Large cells (default DX=DY=50 m).
  - Small uniform bed slope toward the +X (downslope) drainage edge so the
    basin drains but stays subcritical / friction-dominated.
  - Rainfall (constant INTENSITY for RAIN_MIN minutes, then dry) auto-broadcast
    to every 2D cell by the engine -- no [SUBCATCHMENTS].
  - NORMAL_FLOW boundary on the downslope (+X) edge so the basin drains.
  - A trivial, DRY, NOT-2D-coupled 1D network (1 junction + 1 outfall + 1
    conduit) only to satisfy the routing parser. With no [2D_VERTEX_NODE_MAP]
    there is zero 1D<->2D coupling, so the 2D advance window is the full
    ROUTING_STEP (default 60 s) -- room for the adaptive integrator to take
    large internal steps. This is where the IMEX split is expected to pay off.

Usage:  gen_scaling_mesh.py NX NY OUT.inp [--dx 50] [--dy 50] [--slope 0.001]
                            [--routing-step 60] [--hours 3] [--rain-mm-hr 50]
                            [--rain-min 60] [--mannings 0.03]
"""
import argparse, sys

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("nx", type=int)
    ap.add_argument("ny", type=int)
    ap.add_argument("out")
    ap.add_argument("--dx", type=float, default=50.0)
    ap.add_argument("--dy", type=float, default=50.0)
    ap.add_argument("--slope", type=float, default=0.001)   # bed slope toward +X
    ap.add_argument("--z0", type=float, default=100.0)      # downslope-edge bed elev
    ap.add_argument("--routing-step", type=int, default=60)
    ap.add_argument("--hours", type=float, default=3.0)
    ap.add_argument("--rain-mm-hr", type=float, default=50.0)
    ap.add_argument("--rain-min", type=int, default=60)
    ap.add_argument("--mannings", type=float, default=0.03)
    ap.add_argument("--max-timestep", type=float, default=60.0)
    ap.add_argument("--threads", type=int, default=1,
                    help="[OPTIONS] THREADS: 1=serial, N=N threads, 0=all cores")
    ap.add_argument("--drain", action="store_true",
                    help="open the downslope (+X) edge with a NORMAL_FLOW outlet so "
                         "water drains out instead of ponding into an ever-deeper "
                         "wedge against a closed wall (keeps ponding shallow and "
                         "scale-invariant — a realistic 24 h drainage runtime test)")
    a = ap.parse_args()

    nx, ny, dx, dy = a.nx, a.ny, a.dx, a.dy
    nvx, nvy = nx + 1, ny + 1
    n_tri = 2 * nx * ny
    Lx = nx * dx

    def vid(ix, iy):           # 0-based vertex id, row-major in x (engine convention)
        return iy * nvx + ix

    # Bed elevation: highest at x=0 (upslope), draining toward x=Lx (+X edge).
    def zv(ix):
        x = ix * dx
        return a.z0 + a.slope * (Lx - x)

    end_h = int(a.hours)
    end_m = int(round((a.hours - end_h) * 60))
    end_time = f"{end_h:02d}:{end_m:02d}:00"

    w = []
    P = w.append
    P("[TITLE]")
    P(f";; Synthetic pure-2D scaling mesh  {nx}x{ny} quads = {n_tri} triangles")
    P(f";; cell {dx}x{dy} m, slope {a.slope}, drain +X NORMAL_FLOW, pure 2D (no coupling)")
    P("")
    P("[OPTIONS]")
    P("FLOW_UNITS           CMS")
    P("INFILTRATION         HORTON")
    P("FLOW_ROUTING         DYNWAVE")
    P("LINK_OFFSETS         DEPTH")
    P("MIN_SLOPE            0")
    P("ALLOW_PONDING        NO")
    P("SKIP_STEADY_STATE    NO")
    P(f"THREADS              {a.threads}")
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
    P(";;Name           Format    Intvl  SCF   Source")
    P("GAGE1            INTENSITY 0:15   1.00  TIMESERIES STORM")
    P("")
    P("[TIMESERIES]")
    P(";;Name           Time      Value")
    # Constant intensity (mm/hr) for rain_min minutes, then dry.
    t = 0
    while t <= a.rain_min:
        P(f"STORM            {t//60}:{t%60:02d}      {a.rain_mm_hr:.3f}")
        t += 15
    P(f"STORM            {(a.rain_min+15)//60}:{(a.rain_min+15)%60:02d}      0.000")
    P("")
    # Trivial DRY uncoupled 1D network (parser needs a routing network).
    P("[JUNCTIONS]")
    P(";;Name  Elev  MaxDepth  InitDepth  SurDepth  Aponded")
    P("J1      0     10        0          0         0")
    P("")
    P("[OUTFALLS]")
    P(";;Name  Elev  Type   Stage  Gated")
    P("O1      0     FREE          NO")
    P("")
    P("[CONDUITS]")
    P(";;Name  From  To  Length  N      Z1  Z2  Q0")
    P("C1      J1    O1  100     0.01   0   0   0")
    P("")
    P("[XSECTIONS]")
    P(";;Link  Shape     Geom1  Geom2  Geom3  Geom4  Barrels")
    P("C1      CIRCULAR  1.0    0      0      0      1")
    P("")
    P("[COORDINATES]")
    P(";;Node  X       Y")
    P(f"J1      {-2*dx:.1f}   {-2*dy:.1f}")
    P(f"O1      {-3*dx:.1f}   {-2*dy:.1f}")
    P("")
    P("[2D_OPTIONS]")
    P(f"MAX_TIMESTEP           {a.max_timestep:g}")
    P("DRY_DEPTH              0.0001")
    P("LIMITER_EPSILON        1e-06")
    P("COUPLING_CD            0.65")
    P("REPORT_2D              NO")
    P("")
    P("[2D_VERTICES]")
    P(";;X  Y  Z  TAG")
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
            if (ix + iy) % 2 == 0:           # SW-NE diagonal
                tris.append(f"{sw} {se} {ne} {n:g}")
                tris.append(f"{sw} {ne} {nw} {n:g}")
            else:                            # NW-SE diagonal
                tris.append(f"{sw} {se} {nw} {n:g}")
                tris.append(f"{se} {ne} {nw} {n:g}")
    P("\n".join(tris))
    P("")
    if a.drain:
        # Open the downslope (+X) edge: the right edge of each rightmost cell is
        # the SE–NE vertex pair. Engine edge convention (MeshBuilder.cpp): edge 0
        # = v1–v2, edge 2 = v0–v1. In the union-jack triangulation cell (ix,iy)
        # emits triA,triB at list indices 2*(iy*nx+ix), +1; for even (ix+iy) the
        # SE–NE pair is triA's v1–v2 (edge 0), for odd it is triB's v0–v1 (edge 2).
        P("[2D_BOUNDARY_CONDITIONS]")
        P(";;TRI  EDGE TYPE          PARAM_1")
        bc = []
        ix = nx - 1
        for iy in range(ny):
            base = 2 * (iy * nx + ix)
            if (ix + iy) % 2 == 0:
                bc.append(f"{base}    0    NORMAL_FLOW   {a.slope:g}")
            else:
                bc.append(f"{base+1}    2    NORMAL_FLOW   {a.slope:g}")
        P("\n".join(bc))
        P("")
    # Closed sloped basin: ALL edges are walls (no [2D_BOUNDARY_CONDITIONS]).
    # Rain falls uniformly, water flows downslope and ponds against the low (+X)
    # edge, then relaxes to a horizontal water surface after the rain stops. The
    # approach to that equilibrium is the stiff parabolic diffusion relaxation —
    # exactly the operator the implicit half integrates. A closed basin is also
    # exactly conservative (rain in == storage), giving a clean continuity gate
    # with no boundary-flux bookkeeping to get wrong.

    with open(a.out, "w") as f:
        f.write("\n".join(w))
    print(f"wrote {a.out}: {n_tri} triangles ({nvx*nvy} vertices), "
          f"domain {Lx:g}x{ny*dy:g} m, slope {a.slope}, routing {a.routing_step}s")

if __name__ == "__main__":
    # argparse stores --routing-step as routing_step
    main()
