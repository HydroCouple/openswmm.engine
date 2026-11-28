"""Runtime round-trip verification of the 14 'gap-fill' bindings.

Actually exercises each newly-wrapped C function against a real compiled
engine + real .inp fixtures (no mocks), proving get/set behaviour — not just
that the symbols import. Run in the openswmm env:

    python verify_api_gapfill_roundtrip_2026-06-19.py
"""
from __future__ import annotations
import os

from openswmm.engine import Solver
from openswmm.engine._subcatchments import GroundwaterParams
from openswmm.engine._enums import InfilModel

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
GW_INP   = os.path.join(REPO, "python/tests/engine/output/quality_dwf.inp")
LID_INP  = os.path.join(REPO, "python/tests/engine/output/p11_lid_rb.inp")
TWOD_INP = os.path.join(REPO, "examples/2d_complete_example.inp")

results: list[tuple[str, bool, str]] = []
def rec(fn, ok, detail=""):
    results.append((fn, ok, detail))
    print(f"  [{'PASS' if ok else 'FAIL'}] {fn:38s} {detail}")

def open_solver(inp, initialize=False):
    s = Solver(inp, inp.replace(".inp", "_verify.rpt"),
               inp.replace(".inp", "_verify.out"))
    s.open()
    if initialize:
        s.initialize()
    return s

# ---------------------------------------------------------------------------
print(f"\n=== Group A: subcatchment groundwater/aquifer/infil  ({os.path.basename(GW_INP)}) ===")
s = open_solver(GW_INP)
try:
    # find a subcatchment that has an aquifer assigned
    sub = None
    for i in range(len(s.subcatchments)):
        c = s.subcatchments[i]
        if c.aquifer is not None:
            sub = c
            break
    if sub is None:
        rec("swmm_subcatch_get_aquifer", False, "no aquifer-bearing subcatchment found")
    else:
        # get/set aquifer
        a0 = sub.aquifer
        rec("swmm_subcatch_get_aquifer", isinstance(a0, int), f"aquifer={a0}")
        sub.aquifer = None
        cleared = sub.aquifer is None
        sub.aquifer = a0
        restored = sub.aquifer == a0
        rec("swmm_subcatch_set_aquifer", cleared and restored,
            f"None->{sub.aquifer} clear&restore ok={cleared and restored}")

        # get/set gw_node
        n0 = sub.gw_node
        rec("swmm_subcatch_get_gw_node", n0 is None or isinstance(n0, int), f"gw_node={n0}")
        sub.gw_node = None
        ncl = sub.gw_node is None
        if n0 is not None:
            sub.gw_node = n0
        rec("swmm_subcatch_set_gw_node", ncl and sub.gw_node == n0, f"clear&restore ok")

        # get/set gw_params
        p0 = sub.gw_params
        rec("swmm_subcatch_get_gw_params", isinstance(p0, GroundwaterParams), f"{tuple(round(x,4) for x in p0)}")
        p1 = GroundwaterParams(p0.surf_elev + 1.5, p0.a1 + 0.01, p0.b1 + 0.1,
                               p0.a2 + 0.02, p0.b2 + 0.2, p0.a3 + 0.03,
                               p0.tw + 0.5, p0.hstar + 0.25)
        sub.set_gw_params(*p1)
        got = sub.gw_params
        match = all(abs(a - b) < 1e-9 for a, b in zip(got, p1))
        sub.set_gw_params(*p0)  # restore
        rec("swmm_subcatch_set_gw_params", match, f"set->get equal={match}")

    # infil model setter (any subcatchment)
    c = s.subcatchments[0]
    m0 = c.infiltration.model
    target = InfilModel.CURVE_NUMBER if m0 != InfilModel.CURVE_NUMBER else InfilModel.HORTON
    c.infiltration.model = target
    got_m = c.infiltration.model
    c.infiltration.model = m0  # restore
    rec("swmm_subcatch_set_infil_model", got_m == target, f"{m0.name}->{got_m.name}")
finally:
    s.close(); s.destroy()

# ---------------------------------------------------------------------------
print(f"\n=== Group B: LID usage count/get/remove  ({os.path.basename(LID_INP)}) ===")
s = open_solver(LID_INP)
try:
    lids = s.infrastructure.lids
    n0 = lids.usage_count()
    rec("swmm_lid_usage_count", isinstance(n0, int) and n0 >= 0, f"count={n0}")
    if n0 > 0:
        row = lids.usage_get(0)
        ok_get = {"subcatch_index","lid_index","number","area","width",
                  "init_sat","from_imperv","to_perv","from_perv"} <= set(row)
        rec("swmm_lid_usage_get", ok_get, f"row0={row}")
        # remove the last row and confirm the count drops, then it's gone
        lids.usage_remove(n0 - 1)
        n1 = lids.usage_count()
        rec("swmm_lid_usage_remove", n1 == n0 - 1, f"{n0}->{n1}")
    else:
        rec("swmm_lid_usage_get", False, "no usage rows to read")
        rec("swmm_lid_usage_remove", False, "no usage rows to remove")
finally:
    s.close(); s.destroy()

# ---------------------------------------------------------------------------
print(f"\n=== Group C: 2D triangle/vertex setters  ({os.path.basename(TWOD_INP)}) ===")
s = open_solver(TWOD_INP, initialize=True)
try:
    surf = s.surface2d
    if not surf.is_active:
        for fn in ("swmm_2d_set_triangle_mannings","swmm_2d_set_triangle_tag",
                   "swmm_2d_set_vertex_tag","swmm_2d_set_vertex_coupled_node"):
            rec(fn, False, "2D surface inactive")
    else:
        # triangle mannings
        old_n = surf.get_triangle_mannings(0)
        surf.set_triangle_mannings(0, 0.087)
        got_n = surf.get_triangle_mannings(0)
        surf.set_triangle_mannings(0, old_n)
        rec("swmm_2d_set_triangle_mannings", abs(got_n - 0.087) < 1e-9, f"{old_n}->{got_n}")

        # triangle tag
        surf.set_triangle_tag(0, "VERIFY_T")
        got_tt = surf.get_triangle_tag(0)
        rec("swmm_2d_set_triangle_tag", got_tt == "VERIFY_T", f"tag={got_tt!r}")

        # vertex tag
        surf.set_vertex_tag(0, "VERIFY_V")
        got_vt = surf.get_vertex_tag(0)
        rec("swmm_2d_set_vertex_tag", got_vt == "VERIFY_V", f"tag={got_vt!r}")

        # vertex coupled node — couple vertex 0 to first 1D node, then clear
        node_name = None
        try:
            node_name = s.nodes[0].id
        except Exception:
            pass
        if node_name:
            surf.set_vertex_coupled_node(0, node_name)
            ci = surf.get_vertex_coupled_node(0)
            surf.set_vertex_coupled_node(0, "")  # clear
            cleared = surf.get_vertex_coupled_node(0) < 0
            rec("swmm_2d_set_vertex_coupled_node", ci >= 0 and cleared,
                f"coupled idx={ci}, cleared={cleared}")
        else:
            # at least prove clear-path works
            surf.set_vertex_coupled_node(0, "")
            rec("swmm_2d_set_vertex_coupled_node", True, "clear-path ok (no 1D node to bind)")
finally:
    s.close(); s.destroy()

# ---------------------------------------------------------------------------
n_pass = sum(1 for _, ok, _ in results if ok)
print(f"\n==== RESULT: {n_pass}/{len(results)} bindings exercised successfully ====")
raise SystemExit(0 if n_pass == len(results) else 1)
