"""How GW_FACE_CAP shares a short donor's water among its faces (hydrology
Chapter 9, §9.5.2).

One donor cell with three aquifer faces and a node bed. The node bed has
already reserved most of the donor's drainable water, so the faces ask for
more than is left. The face pass is computed with the engine's rules:

* Darcy on each face, harmonic K, donor transmissivity, and the per-face
  share cap 0.5 T Sy A / n_f / dt (src/engine/2d/subsurface/
  SubsurfaceSolver.cpp:875-895 for RATIO, :1266-1295 for SEQUENTIAL;
  kFaceShare = 0.5 at :108);
* the unreserved water Sy h_g A minus what is already booked to leave the
  cell (unreservedDrainableWater, :1887-1903);
* RATIO: every donor's unreserved water taken before any booking, its
  faces' demands totaled and scaled by min(1, avail / demand) (:839-930);
* SEQUENTIAL: faces capped in list order by the smaller of the share and
  the water the faces before them left, booking as they go (:1287-1301).

RATIO has been the default since 2026-11-19 (SubsurfaceData.hpp:160-163).
Synthetic; no model run. The cell and its numbers are illustrative.
"""
from __future__ import annotations

REQUIRES = ()

K_FACE_SHARE = 0.5        # SubsurfaceSolver.cpp:108
AREA = 50.0               # m2, donor cell
SY = 0.10                 # donor specific yield
HG = 0.40                 # m, donor saturated thickness (= T on every face it feeds)
KS = 3.0e-4               # m/s on both sides (harmonic mean = KS)
XI, DX = 10.0, 5.0        # m, face length and centroid distance
DT = 3600.0               # s, the face pass's step
DH = (0.45, 0.30, 0.15)   # m, donor head minus each neighbor's head
N_FACES = 3               # the donor's aquifer faces (cell_nface_)
NODE_RESERVED = 1.5       # m3 the node bed booked since the donor last fired (two samples)


def face_pass():
    """Demands and what each rule lets through, m3 per pass, in face order 1-2-3."""
    drainable = SY * HG * AREA
    share = K_FACE_SHARE * HG * SY * AREA / N_FACES              # m3 per pass (cap * dt)
    darcy = [KS * HG * XI / DX * dh * DT for dh in DH]           # m3 per pass, uncapped
    demand = [min(q, share) for q in darcy]                       # pass A
    avail = max(0.0, drainable - NODE_RESERVED)
    ratio = min(1.0, avail / sum(demand))
    out = {"RATIO": [q * ratio for q in demand]}
    for name, order in (("SEQUENTIAL, faces 1-2-3", (0, 1, 2)), ("SEQUENTIAL, faces 3-2-1", (2, 1, 0))):
        reserved = NODE_RESERVED
        got = [0.0, 0.0, 0.0]
        for e in order:
            cap = min(share, max(0.0, drainable - reserved))
            got[e] = min(darcy[e], cap)
            reserved += got[e]                                    # booked into the edge accumulator
        out[name] = got
    return dict(drainable=drainable, share=share, darcy=darcy, demand=demand, avail=avail,
                ratio=ratio, out=out)


def _draw(style, r):
    import matplotlib.pyplot as plt
    from matplotlib.patches import Polygon, Circle, FancyArrowPatch
    INK, MUTED = style.INK, style.MUTED
    fig = style.figure(9.6, 4.0)
    gs = fig.add_gridspec(1, 3, width_ratios=[1.2, 0.5, 1.55], wspace=0.3)
    a0, a1, a2 = (fig.add_subplot(gs[0, i]) for i in range(3))

    # (a) plan view: the donor and its three neighbors
    style.schematic_ax(a0)
    tri = [(0.0, 0.0), (2.0, 0.0), (1.0, 1.73)]
    nbr = [[(0.0, 0.0), (2.0, 0.0), (1.0, -1.73)],
           [(2.0, 0.0), (1.0, 1.73), (3.0, 1.73)],
           [(1.0, 1.73), (0.0, 0.0), (-1.0, 1.73)]]
    for p in nbr:
        a0.add_patch(Polygon(p, closed=True, fc="#eef4fb", ec=MUTED, lw=0.9))
    a0.add_patch(Polygon(tri, closed=True, fc="#d6e6f7", ec=INK, lw=1.3))
    a0.text(1.0, 1.02, "donor", ha="center", va="center", fontsize=8.6, color=INK, weight="bold")
    a0.add_patch(Circle((1.0, 0.56), 0.13, fc="#bfbfbf", ec=INK, lw=0.9))
    a0.text(1.0, 0.36, "node bed", ha="center", va="top", fontsize=7.4, color=INK)
    face_mid = [(1.0, 0.0), (1.5, 0.865), (0.5, 0.865)]
    outward = [(0.0, -1.0), (0.866, 0.5), (-0.866, 0.5)]
    for i, ((mx, my), (nx, ny)) in enumerate(zip(face_mid, outward)):
        a0.add_patch(FancyArrowPatch((mx, my), (mx + 0.5 * nx, my + 0.5 * ny),
                                     arrowstyle="-|>", mutation_scale=10, color=style.WATER, lw=1.6))
        a0.text(mx + 0.78 * nx, my + 0.78 * ny, str(i + 1), ha="center", va="center", fontsize=8.4,
                color=INK, weight="bold")
    a0.text(-1.05, -2.0, "Head drop to the neighbor:\nface 1 {:.2f} m, face 2 {:.2f} m,\nface 3 {:.2f} m. "
            "Every face: K = 3e-4 m/s,\nT = h_g = 0.40 m, ξ = 10 m, d = 5 m,\nΔt = 1 h. Donor: A = 50 m², "
            "Sy = 0.10.".format(*DH), fontsize=7.2, color=MUTED, va="top", linespacing=1.35)
    a0.set_xlim(-1.1, 3.1)
    a0.set_ylim(-3.45, 1.85)
    a0.set_anchor("N")
    a0.set_title("(a) A donor with three faces", fontsize=9, loc="left")

    # (b) the donor's water
    style.style_ax(a1)
    a1.grid(False)
    a1.bar([0], [NODE_RESERVED], width=0.6, color="#bfbfbf", ec=INK, lw=0.8)
    a1.bar([0], [r["avail"]], bottom=[NODE_RESERVED], width=0.6, color="#9cc3ec", ec=INK, lw=0.8)
    a1.text(0, NODE_RESERVED / 2, f"reserved\nby the\nnode bed\n{NODE_RESERVED:.2f}", ha="center",
            va="center", fontsize=7.4, color=INK, linespacing=1.2)
    a1.text(0, NODE_RESERVED + r["avail"] / 2, f"unreserved\n{r['avail']:.2f}", ha="center",
            va="center", fontsize=7.4, color=INK, linespacing=1.2)
    a1.set_xlim(-0.6, 0.6)
    a1.set_ylim(0, r["drainable"] * 1.08)
    a1.set_xticks([])
    a1.set_ylabel(f"m³ (Sy·h_g·A = {r['drainable']:.1f})")
    a1.set_title("(b) The donor", fontsize=9, loc="left")

    # (c) per-face outcomes
    style.style_ax(a2)
    rules = [("demand after the share cap", r["demand"], "#d5d2c8"),
             ("RATIO (default)", r["out"]["RATIO"], style.COLORS["fv"]),
             ("SEQUENTIAL, faces 1-2-3", r["out"]["SEQUENTIAL, faces 1-2-3"], style.COLORS["fv-lts"]),
             ("SEQUENTIAL, faces 3-2-1", r["out"]["SEQUENTIAL, faces 3-2-1"], style.COLORS["dw-slot"])]
    w = 0.2
    for j, (lab, vals, col) in enumerate(rules):
        xs = [i + (j - 1.5) * w for i in range(3)]
        tot = sum(vals)
        a2.bar(xs, vals, width=w * 0.92, color=col, ec=INK if j else MUTED, lw=0.6,
               label=f"{lab}  (total {tot:.2f})")
        for x, v in zip(xs, vals):
            if v == 0.0:
                a2.text(x, 0.006, "0", ha="center", va="bottom", fontsize=7.4, color=col)
    a2.axhline(r["share"], color=MUTED, lw=0.9, ls=(0, (4, 2)))
    a2.text(2.42, r["share"] + 0.008, f"share cap {r['share']:.3f}", fontsize=7.2, color=MUTED,
            ha="right", va="bottom")
    a2.set_xticks(range(3))
    a2.set_xticklabels([f"face {i + 1}" for i in range(3)])
    a2.set_ylabel("m³ through the face in one pass")
    a2.set_ylim(0, 0.52)
    a2.legend(loc="upper right", fontsize=7.4, handlelength=1.2)
    a2.set_title(f"(c) What each rule lets through (ratio {r['ratio']:.3f})", fontsize=9, loc="left")
    return fig


def build(sink):
    import style
    r = face_pass()
    for name, vals in r["out"].items():
        print(f"    {name}: " + ", ".join(f"{v:.4f}" for v in vals) + f"  total {sum(vals):.4f} m3")
    print(f"    drainable {r['drainable']:.3f} m3, unreserved {r['avail']:.3f}, share {r['share']:.4f}, "
          f"demand {sum(r['demand']):.4f}, ratio {r['ratio']:.4f}")
    sink.save(_draw(style, r), "hydrology_ch9_face_cap_sharing")
