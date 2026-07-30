#!/usr/bin/env python3
"""Summarize an OPENSWMM_DT_TRACE=1 stderr capture (.cverr).

Usage: dt_trace_summary.py TRACE_FILE INP_FILE [--top N]

Reads `[DT] t=<s> dt=<s> gov=<link|node|floor|fixed> name=<id>` lines and the
model's [CONDUITS] + [2D_VERTEX_NODE_MAP]/[2D_CELL_NODE_MAP] sections, then
reports the dt distribution, the governing-constraint breakdown, the top
governing elements, and how many link-governed steps sit adjacent to a 2D
coupled node (Task-2 step-shrink investigation, 2026-07-29).
"""
import argparse
import re
import sys
from collections import Counter

ap = argparse.ArgumentParser()
ap.add_argument("trace")
ap.add_argument("inp", nargs="+",
                help=".inp (and optionally the .2dm sidecar that carries the "
                     "[2D_TRIANGLE_NODE_MAP] coupling rows)")
ap.add_argument("--top", type=int, default=15)
args = ap.parse_args()

# --- model topology -----------------------------------------------------------
link_nodes = {}   # link name -> (node1, node2)
coupled = set()   # node names with a 2D coupling row
COUPLING_SECTIONS = ("[2D_VERTEX_NODE_MAP]", "[2D_CELL_NODE_MAP]",
                     "[2D_TRIANGLE_NODE_MAP]")
for path in args.inp:
    section = None
    for raw in open(path, errors="replace"):
        s = raw.strip()
        if not s or s.startswith(";"):
            continue
        if s.startswith("["):
            section = s.upper()
            continue
        tok = s.split()
        if section == "[CONDUITS]" and len(tok) >= 3:
            link_nodes[tok[0]] = (tok[1], tok[2])
        elif section in COUPLING_SECTIONS and len(tok) >= 2:
            coupled.add(tok[1])

# --- trace --------------------------------------------------------------------
pat = re.compile(r"\[DT\] t=([\d.]+) dt=([\d.]+) gov=(\w+) name=(\S+)")
dts, gov_count, elem_count = [], Counter(), Counter()
link_steps = coupled_adj_steps = 0
for line in open(args.trace, errors="replace"):
    m = pat.search(line)
    if not m:
        continue
    dt, gov, name = float(m.group(2)), m.group(3), m.group(4)
    dts.append(dt)
    gov_count[gov] += 1
    if gov in ("link", "node"):
        elem_count[(gov, name)] += 1
    if gov == "link":
        link_steps += 1
        n1, n2 = link_nodes.get(name, ("", ""))
        if n1 in coupled or n2 in coupled:
            coupled_adj_steps += 1

if not dts:
    sys.exit(f"no [DT] lines found in {args.trace}")

dts.sort()
n = len(dts)
pct = lambda q: dts[min(n - 1, int(q * n))]
print(f"steps                {n}")
print(f"dt mean/median (s)   {sum(dts)/n:.3f} / {pct(0.5):.3f}")
print(f"dt p10/p90 (s)       {pct(0.10):.3f} / {pct(0.90):.3f}")
print(f"dt min/max (s)       {dts[0]:.3f} / {dts[-1]:.3f}")
print("\ngoverning constraint:")
for gov, c in gov_count.most_common():
    print(f"  {gov:<6} {c:>8}  ({100.0*c/n:.1f}%)")
if link_steps:
    print(f"\nlink-governed steps adjacent to a coupled node: "
          f"{coupled_adj_steps}/{link_steps} ({100.0*coupled_adj_steps/link_steps:.1f}%)"
          f"   [{len(coupled)} coupled nodes in model]")
print(f"\ntop {args.top} governing elements:")
for (gov, name), c in elem_count.most_common(args.top):
    tag = ""
    if gov == "link":
        n1, n2 = link_nodes.get(name, ("", ""))
        tag = "  <— coupled-adjacent" if (n1 in coupled or n2 in coupled) else ""
    elif gov == "node":
        tag = "  <— coupled" if name in coupled else ""
    print(f"  {gov:<5} {name:<20} {c:>8}  ({100.0*c/n:.1f}%){tag}")
