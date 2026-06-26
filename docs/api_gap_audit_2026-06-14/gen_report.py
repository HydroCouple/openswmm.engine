#!/usr/bin/env python3
"""Generate the per-domain functional-gap tables and the full per-function
appendix from api_gap_rows.json. Emits markdown fragments to stdout."""
from __future__ import annotations
import json
from pathlib import Path

OUT = Path("/sessions/wizardly-ecstatic-johnson/mnt/outputs")
rows = json.load(open(OUT / "api_gap_rows.json"))

DUNDER = {"__len__","__iter__","__delitem__","__contains__","__getitem__",
          "__setitem__","__init__","__cinit__","__repr__","__eq__","__hash__"}

# Functions wired through the session/lifecycle/error layer (intentionally not
# standalone MCP tools) — confirmed by inspecting session.py / backends/.
INTERNAL = {
    "swmm_engine_create","swmm_engine_destroy","swmm_engine_new","swmm_engine_report",
    "swmm_engine_run","swmm_engine_run_with_callback",
    "swmm_error_message","swmm_get_last_error","swmm_get_last_error_msg",
    "swmm_runoff_iface_read_step","swmm_runoff_iface_save_step",
    "swmm_runoff_iface_close",
    "swmm_conversion_result_free","swmm_impact_report_free",
    "swmm_gpkg_open","swmm_output_open",
}

HEADER_DOMAIN = {
    "openswmm_2d.h":"2D surface","openswmm_controls.h":"Controls",
    "openswmm_datetime.h":"Date/time utils","openswmm_edit.h":"Editing",
    "openswmm_engine.h":"Engine/lifecycle","openswmm_forcing.h":"Forcing",
    "openswmm_gages.h":"Rain gages","openswmm_geopackage.h":"GeoPackage",
    "openswmm_hotstart.h":"Hotstart","openswmm_inflows.h":"Inflows",
    "openswmm_infrastructure.h":"Infrastructure (LID/inlet/street/transect)",
    "openswmm_links.h":"Links","openswmm_massbalance.h":"Mass balance",
    "openswmm_model.h":"Model/options","openswmm_nodes.h":"Nodes",
    "openswmm_output.h":"Output (.out reader)","openswmm_pollutants.h":"Pollutants",
    "openswmm_quality.h":"Water quality","openswmm_spatial.h":"Spatial/geometry",
    "openswmm_statistics.h":"Statistics","openswmm_subcatchments.h":"Subcatchments",
    "openswmm_tables.h":"Tables/curves/patterns/timeseries",
}

# Verified-by-inspection: bound via a private member, but the data IS reachable
# through an aggregate/bulk MCP tool (e.g. tables_get_point[_count] read the
# whole `.points` array; tables_count exists).
CAPABILITY_OVERRIDE = {
    "swmm_table_count", "swmm_table_get_point", "swmm_table_get_point_count",
}

def classify(r):
    if r["mcp_status"] == "direct":
        return "direct"
    if r["mcp_status"] == "capability" or r["symbol"] in CAPABILITY_OVERRIDE:
        return "capability"
    # gap candidates -> refine
    if r["symbol"] in INTERNAL:
        return "internal"
    mem = set(r["members"])
    if not mem or mem <= DUNDER:
        return "protocol"
    return "gap"

for r in rows:
    r["cls"] = classify(r)

# ---- summary table ----
order = list(HEADER_DOMAIN.keys())
print("## Coverage summary by domain\n")
print("| Domain (header) | C fns | ✅ MCP direct | ◑ via sibling | ⚙ protocol | 🔧 internal | ❌ functional gap |")
print("|---|--:|--:|--:|--:|--:|--:|")
tot = {k:0 for k in ["n","direct","capability","protocol","internal","gap"]}
for h in order:
    sub = [r for r in rows if r["header"] == h]
    if not sub: continue
    c = {k: sum(1 for r in sub if r["cls"]==k) for k in ["direct","capability","protocol","internal","gap"]}
    tot["n"]+=len(sub)
    for k in c: tot[k]+=c[k]
    print(f"| {HEADER_DOMAIN[h]} (`{h}`) | {len(sub)} | {c['direct']} | {c['capability']} | {c['protocol']} | {c['internal']} | **{c['gap']}** |")
print(f"| **TOTAL** | **{tot['n']}** | **{tot['direct']}** | **{tot['capability']}** | **{tot['protocol']}** | **{tot['internal']}** | **{tot['gap']}** |")

# ---- functional gap tables by domain ----
print("\n## Functional MCP gaps by domain\n")
gaps = [r for r in rows if r["cls"]=="gap"]
for h in order:
    sub = [r for r in gaps if r["header"]==h]
    if not sub: continue
    print(f"\n### {HEADER_DOMAIN[h]} (`{h}`) — {len(sub)} gap(s)\n")
    print("| C symbol | Python binding member | Signature |")
    print("|---|---|---|")
    for r in sorted(sub, key=lambda x:x["symbol"]):
        mem = ", ".join(m for m in r["members"] if m not in DUNDER) or "—"
        sig = r["sig"].replace("|","\\|")
        if len(sig) > 130: sig = sig[:127]+"..."
        print(f"| `{r['symbol']}` | `{mem}` | `{sig}` |")

# ---- protocol + internal appendix lists ----
print("\n## Protocol / Pythonic-only (not individually tool-wrapped)\n")
for r in sorted([r for r in rows if r["cls"]=="protocol"], key=lambda x:x["symbol"]):
    print(f"- `{r['symbol']}` — bound via `{', '.join(r['members']) or 'internal'}`")
print("\n## Lifecycle / error / memory (intentionally session-internal)\n")
for r in sorted([r for r in rows if r["cls"]=="internal"], key=lambda x:x["symbol"]):
    print(f"- `{r['symbol']}`")

# ---- full appendix ----
print("\n## Appendix — full per-function inventory\n")
ICON = {"direct":"✅","capability":"◑","protocol":"⚙","internal":"🔧","gap":"❌"}
for h in order:
    sub = [r for r in rows if r["header"]==h]
    if not sub: continue
    ng = sum(1 for r in sub if r["cls"]=="gap")
    print(f"\n<details>\n<summary><b>{HEADER_DOMAIN[h]}</b> — <code>{h}</code> ({len(sub)} fns, {ng} gap)</summary>\n")
    print("| C symbol | Cython | MCP | Binding member |")
    print("|---|:--:|:--:|---|")
    for r in sorted(sub, key=lambda x:x["symbol"]):
        cy = "✅" if r["bound"] else "❌"
        mem = ", ".join(r["members"]) or "—"
        print(f"| `{r['symbol']}` | {cy} | {ICON[r['cls']]} | {mem} |")
    print("\n</details>")
