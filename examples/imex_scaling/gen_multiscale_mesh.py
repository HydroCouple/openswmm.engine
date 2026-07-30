#!/usr/bin/env python3
"""
Generate a MULTISCALE pure-2D surface-routing model: a fine "urban" patch
(small cells) embedded in a coarse "watershed" (large cells), as a single
conforming mesh.

WHY (verification experiment #1, IMEX_SCALING_VERIFICATION_NOTE.md §6):
  The existing scaling meshes (gen_scaling_mesh.py) are UNIFORM, so they never
  exercise the regime the whole design targets — a mesh whose smallest cells are
  meters (urban streets) while most cells are hundreds of meters (watershed).
  On a uniform mesh the explicit gravity-wave CFL  Δt ≤ Δx/√(g·h)  is benign, so
  explicit-inertial looks merely "constant-factor slower." On a MULTISCALE mesh
  the few small cells set a tiny GLOBAL explicit step, which should make
  explicit-inertial collapse while implicit CVODE-DW (no CFL) is unaffected.

  This generator builds exactly that mesh so the claim can be MEASURED, not
  inferred: run CVODE-DW and explicit-inertial on it and compare wall time +
  mean internal step (OPENSWMM_2D_DIAG_CSV). Expected: inertial's mean step is
  pinned near Δx_min/√(g·h) and its wall time blows up vs CVODE-DW.

HOW (conforming, no hanging nodes):
  A logically-rectangular (structured) grid with NON-UNIFORM coordinate arrays.
  x and y spacings are coarse in the outer watershed, geometrically graded down
  to a fine patch in the middle, then graded back to coarse. Tensor product +
  union-jack triangulation stays conforming (it is still a structured grid; only
  the vertex coordinates are stretched), so cell SIZES vary ~100x with no
  T-junctions. The fine patch sits in a shallow depression so water collects
  there → deepest water on the smallest cells = worst-case CFL, sharpening the
  contrast. Closed basin (all walls) ⇒ rain in == storage ⇒ clean continuity gate.

Usage:
  gen_multiscale_mesh.py OUT.inp
      [--dx-coarse 200] [--dx-fine 2] [--n-coarse-side 12] [--n-fine 24]
      [--n-trans 8] [--ratio auto] [--slope 0.001] [--patch-depth 0.5]
      [--routing-step 60] [--hours 3] [--rain-mm-hr 50] [--rain-min 60]
      [--mannings 0.03] [--max-timestep 60]

The default builds a ~(2*12+8+24+8 = 76)^2-vertex grid ⇒ ~11k triangles with
Δx_min=2 m, Δx_max=200 m (ratio 100x) — small enough to run quickly, multiscale
enough to expose the CFL. Scale up via --n-coarse-side / --n-fine for bigger runs.
"""
import argparse


def graded_spacings(dx_coarse, dx_fine, n_coarse_side, n_fine, n_trans):
    """Symmetric 1-D spacing list: coarse | grade↓ | fine | grade↑ | coarse.

    The transition cells interpolate geometrically between dx_coarse and dx_fine
    so neighbouring cells never jump by more than the per-step ratio (smooth
    grading keeps the FV gradient reconstruction well-behaved across the seam)."""
    down = []
    for k in range(1, n_trans + 1):
        f = k / (n_trans + 1)                      # 0<f<1
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
    ap.add_argument("--n-coarse-side", type=int, default=12,
                    help="coarse cells on EACH side of the patch, per axis")
    ap.add_argument("--n-fine", type=int, default=24,
                    help="fine cells across the patch, per axis")
    ap.add_argument("--n-trans", type=int, default=8,
                    help="geometrically graded transition cells each side")
    ap.add_argument("--slope", type=float, default=0.001)
    ap.add_argument("--z0", type=float, default=100.0)
    ap.add_argument("--patch-depth", type=float, default=0.5,
                    help="bed depression (m) over the fine patch so water ponds "
                         "on the smallest cells (worst-case CFL). 0 = flat.")
    ap.add_argument("--routing-step", type=int, default=60)
    ap.add_argument("--hours", type=float, default=3.0)
    ap.add_argument("--rain-mm-hr", type=float, default=50.0)
    ap.add_argument("--rain-min", type=int, default=60)
    ap.add_argument("--mannings", type=float, default=0.03)
    ap.add_argument("--max-timestep", type=float, default=60.0)
    a = ap.parse_args()

    spac = graded_spacings(a.dx_coarse, a.dx_fine, a.n_coarse_side,
                           a.n_fine, a.n_trans)
    xs = coords_from_spacings(spac)
    ys = coords_from_spacings(spac)        # square, symmetric grid
    nvx, nvy = len(xs), len(ys)
    nx, ny = nvx - 1, nvy - 1
    n_tri = 2 * nx * ny
    Lx, Ly = xs[-1], ys[-1]

    # Index range of the fine patch (for the bed depression + a tag).
    p_lo = a.n_coarse_side + a.n_trans            # first fine cell index
    p_hi = p_lo + a.n_fine                        # one past last fine cell index
    x_patch_lo, x_patch_hi = xs[p_lo], xs[p_hi]
    y_patch_lo, y_patch_hi = ys[p_lo], ys[p_hi]

    def vid(ix, iy):
        return iy * nvx + ix

    def in_patch(coord, lo, hi):
        return lo <= coord <= hi

    # Bed: gentle slope toward +X, minus a smooth depression over the fine patch
    # so ponded depth is greatest exactly where the cells are smallest.
    def zv(ix, iy):
        x, y = xs[ix], ys[iy]
        z = a.z0 + a.slope * (Lx - x)
        if a.patch_depth > 0 and in_patch(x, x_patch_lo, x_patch_hi) and \
           in_patch(y, y_patch_lo, y_patch_hi):
            # Cosine bump so the depression edges are C1 (no spurious gradient).
            import math
            fx = (x - x_patch_lo) / max(x_patch_hi - x_patch_lo, 1e-9)
            fy = (y - y_patch_lo) / max(y_patch_hi - y_patch_lo, 1e-9)
            bump = (0.5 - 0.5 * math.cos(2 * math.pi * fx)) * \
                   (0.5 - 0.5 * math.cos(2 * math.pi * fy))
            z -= a.patch_depth * bump
        return z

    # Smallest / largest cell edge for the CFL note.
    dmin = min(min(s for s in spac), a.dx_fine)
    dmax = max(spac)

    end_h = int(a.hours)
    end_m = int(round((a.hours - end_h) * 60))
    end_time = f"{end_h:02d}:{end_m:02d}:00"

    w = []
    P = w.append
    P("[TITLE]")
    P(f";; MULTISCALE pure-2D mesh: fine patch {a.dx_fine:g} m in coarse "
      f"{a.dx_coarse:g} m watershed")
    P(f";; {nx}x{ny} cells = {n_tri} triangles; Dx_min={dmin:g} m  Dx_max={dmax:g} m "
      f"(ratio {dmax/dmin:.0f}x)")
    P(";; closed basin (walls), pure 2D (no 1D coupling) — CVODE-DW vs explicit-inertial")
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
    P(f"ROUTING_STEP         0:0{a.routing_step//60}:{a.routing_step%60:02d}"
      if a.routing_step >= 60 else f"ROUTING_STEP         0:00:{a.routing_step:02d}")
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
    # Trivial DRY uncoupled 1D network (parser needs a routing network).
    P("[JUNCTIONS]")
    P("J1      0     10        0          0         0")
    P("")
    P("[OUTFALLS]")
    P("O1      0     FREE          NO")
    P("")
    P("[CONDUITS]")
    P("C1      J1    O1  100     0.01   0   0   0")
    P("")
    P("[XSECTIONS]")
    P("C1      CIRCULAR  1.0    0      0      0      1")
    P("")
    P("[COORDINATES]")
    P(f"J1      {-2*a.dx_coarse:.1f}   {-2*a.dx_coarse:.1f}")
    P(f"O1      {-3*a.dx_coarse:.1f}   {-2*a.dx_coarse:.1f}")
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
    # Closed basin: all boundary edges are walls (no [2D_BOUNDARY_CONDITIONS]).

    with open(a.out, "w") as f:
        f.write("\n".join(w))

    # CFL estimate at a nominal ponded depth, to print the expected throttle.
    import math
    h_nom = max(a.patch_depth, 0.05)
    dt_fine = dmin / math.sqrt(9.80665 * h_nom)
    dt_coarse = dmax / math.sqrt(9.80665 * 0.05)
    print(f"wrote {a.out}: {n_tri} triangles ({nvx*nvy} vertices), "
          f"domain {Lx:.0f}x{Ly:.0f} m")
    print(f"  Dx_min={dmin:g} m  Dx_max={dmax:g} m  (ratio {dmax/dmin:.0f}x); "
          f"fine patch {a.n_fine}x{a.n_fine} cells")
    print(f"  explicit gravity-wave CFL ~ Dx/sqrt(g*h): "
          f"fine~{dt_fine:.2f}s (h={h_nom:g}) vs coarse~{dt_coarse:.0f}s "
          f"=> explicit-inertial step pinned ~{dt_coarse/dt_fine:.0f}x smaller "
          f"than coarse cells allow")


if __name__ == "__main__":
    main()
