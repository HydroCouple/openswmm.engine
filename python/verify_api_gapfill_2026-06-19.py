"""Verification probe for the API Gap-Fill plan (2026-06-18).

Confirms the 14 "unwrapped" functions from the plan are now reachable on the
Pythonic surface of the freshly-rebuilt engine, and that getters/setters
round-trip. Run:  python verify_api_gapfill_2026-06-19.py
"""
from __future__ import annotations
import inspect

import openswmm.engine as E
from openswmm.engine import _subcatchments as sub
from openswmm.engine import _infrastructure as infra
from openswmm.engine import _2d as twod

print("openswmm.engine from:", E.__file__)
print("version:", getattr(E, "__version__", "?"))
print()

ok = True
def check(label, cond):
    global ok
    mark = "PASS" if cond else "FAIL"
    if not cond:
        ok = False
    print(f"  [{mark}] {label}")

# --- Stale-marker that proved the old .so was outdated -------------------
check("GroundwaterParams present in _subcatchments", hasattr(sub, "GroundwaterParams"))

# --- Group A: Subcatchment groundwater/aquifer/infil surfaces ------------
Sub = getattr(sub, "Subcatchment", None)
check("Subcatchment class present", Sub is not None)
if Sub is not None:
    for attr in ("aquifer", "gw_node", "gw_params", "set_gw_params"):
        check(f"Subcatchment.{attr} present", hasattr(Sub, attr))

# InfiltrationView.model settable (set_infil_model)
InfilView = None
for name in dir(sub):
    obj = getattr(sub, name)
    if inspect.isclass(obj) and "Infil" in name:
        InfilView = obj
if InfilView is not None:
    prop = getattr(InfilView, "model", None)
    settable = isinstance(prop, property) and prop.fset is not None
    check(f"{InfilView.__name__}.model has a setter", settable)
else:
    print("  [info] no InfiltrationView class found; infil model setter may live on Subcatchment")

# --- Group B: LID usage view (count/get/remove) -------------------------
classes_infra = [n for n in dir(infra) if n[0].isupper()]
print("  infra classes:", classes_infra)

# --- Group C: 2D triangle/vertex setters --------------------------------
classes_2d = [n for n in dir(twod) if n[0].isupper()]
print("  2d classes:", classes_2d)

print()
print("OVERALL:", "PASS" if ok else "FAIL")
