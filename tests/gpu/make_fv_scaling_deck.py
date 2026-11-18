"""Write a synthetic sewer network for scaling runs of the 1D finite volume solver.

The network is a comb: BRANCHES sewers of LENGTH conduits each (circular pipes) drain into the
junctions of a trunk (an open rectangular channel sized for the total flow) that ends in a free
outfall. Every branch head receives the same inflow hydrograph, so all of the network is wet
and active for most of the run. The mesh has no pumps, weirs or orifices.

Conduit count is BRANCHES * (LENGTH + 1); the solver meshes each conduit into FV_MIN_CELLS cells
(4 unless --min-cells says otherwise), so cells = conduits * FV_MIN_CELLS.

With --diameter below about 1.2 ft the branch pipes surcharge at the hydrograph peak, which puts
part of the mesh on the Preissmann slot and shortens the stable step.
"""

import argparse
from pathlib import Path


def build(branches, length, diameter, min_cells, hours, routing_step, peak):
    pipe_len = 100.0   # ft
    trunk_len = 200.0  # ft
    branch_slope = 0.005
    trunk_slope = 0.001
    drop = 1.0         # ft, branch invert above the trunk invert at the confluence
    depth = 12.0       # ft, junction rim above invert
    trunk_width = max(4.0, 0.12 * branches * peak / 3.0)

    out = []
    add = out.append
    add("[TITLE]")
    add(f";; FV scaling comb: {branches} branches x {length} conduits, D = {diameter} ft")
    add("")
    add("[OPTIONS]")
    add("FLOW_UNITS            CFS")
    add("INFILTRATION          HORTON")
    add("FLOW_ROUTING          FV")
    add("START_DATE            01/01/2026")
    add("START_TIME            00:00:00")
    add("REPORT_START_DATE     01/01/2026")
    add("REPORT_START_TIME     00:00:00")
    add("END_DATE              01/01/2026")
    hh = int(hours)
    mm = int(round((hours - hh) * 60))
    add(f"END_TIME              {hh:02d}:{mm:02d}:00")
    add("REPORT_STEP           00:05:00")
    add("WET_STEP              00:05:00")
    add("DRY_STEP              01:00:00")
    add(f"ROUTING_STEP          {routing_step}")
    add("ALLOW_PONDING         NO")
    add("THREADS               1")
    add(f"FV_MIN_CELLS          {min_cells}")
    add("FV_LTS                NO")
    add("FV_COMPACTION         NO")
    add("FV_STRUCTURE_COUPLING ROUTING_STEP")
    add("")

    add("[JUNCTIONS]")
    add(";;Name           Elev      MaxDepth  InitDepth SurDepth  Aponded")
    trunk_elev = [(branches - b) * trunk_len * trunk_slope for b in range(branches)]
    for b in range(branches):
        add(f"T{b:<14d} {trunk_elev[b]:<9.4f} {depth:<9.1f} 0         0         0")
        for k in range(length):
            elev = trunk_elev[b] + drop + (length - k) * pipe_len * branch_slope
            add(f"J{b}_{k:<10d} {elev:<9.4f} {depth:<9.1f} 0         0         0")
    add("")

    add("[OUTFALLS]")
    add("OUT             0.0       FREE      NO")
    add("")

    add("[CONDUITS]")
    add(";;Name           From            To              Length    N         InOff OutOff")
    for b in range(branches):
        for k in range(length):
            to = f"J{b}_{k + 1}" if k + 1 < length else f"T{b}"
            add(f"C{b}_{k:<10d} J{b}_{k:<12d} {to:<15s} {pipe_len:<9.1f} 0.013     0     0")
        to = f"T{b + 1}" if b + 1 < branches else "OUT"
        add(f"TC{b:<13d} T{b:<14d} {to:<15s} {trunk_len:<9.1f} 0.013     0     0")
    add("")

    add("[XSECTIONS]")
    for b in range(branches):
        for k in range(length):
            add(f"C{b}_{k:<10d} CIRCULAR   {diameter:<8.3f} 0 0 0 1")
        add(f"TC{b:<13d} RECT_OPEN  {depth:<8.1f} {trunk_width:.2f} 0 0 1")
    add("")

    add("[INFLOWS]")
    for b in range(branches):
        add(f"J{b}_0{'':<9s} FLOW  HYDRO  FLOW  1.0  1.0")
    add("")

    add("[TIMESERIES]")
    total = hours * 60.0
    for minute, factor in ((0.0, 0.1), (0.15 * total, 1.0), (0.45 * total, 1.0),
                           (0.70 * total, 0.2), (total, 0.2)):
        h, m = divmod(int(round(minute)), 60)
        add(f"HYDRO  {h}:{m:02d}  {peak * factor:.4f}")
    add("")

    add("[REPORT]")
    add("INPUT     NO")
    add("CONTROLS  NO")
    add("NODES     NONE")
    add("LINKS     NONE")
    add("")
    return "\n".join(out) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--branches", type=int, required=True)
    parser.add_argument("--length", type=int, required=True, help="conduits per branch")
    parser.add_argument("--diameter", type=float, default=2.0, help="branch pipe diameter, ft")
    parser.add_argument("--min-cells", type=int, default=4)
    parser.add_argument("--hours", type=float, default=1.0)
    parser.add_argument("--routing-step", default="0:00:10")
    parser.add_argument("--peak", type=float, default=3.0, help="peak inflow per branch, cfs")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    text = build(args.branches, args.length, args.diameter, args.min_cells, args.hours,
                 args.routing_step, args.peak)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text)
    conduits = args.branches * (args.length + 1)
    print(f"{args.output}: {conduits} conduits, {conduits * args.min_cells} cells")


if __name__ == "__main__":
    main()
