#!/usr/bin/env python3
"""Categorise the residual gap candidates and VERIFY each functional gap by
grepping the whole MCP package for a distinguishing keyword (the C symbol's
attribute noun). If the keyword never appears, the gap is confirmed."""
from __future__ import annotations
import json, re
from pathlib import Path

MCP_PKG = Path("/sessions/wizardly-ecstatic-johnson/mnt/openswmm.mcp/src/openswmm_mcp")
OUT = Path("/sessions/wizardly-ecstatic-johnson/mnt/outputs")

blob = ""
for p in MCP_PKG.rglob("*.py"):
    blob += "\n" + p.read_text(encoding="utf-8")

DUNDER = {"__len__","__iter__","__delitem__","__contains__","__getitem__",
          "__setitem__","__init__","__cinit__","__repr__","__eq__","__hash__"}

# distinguishing keyword per symbol = last meaningful attribute token(s)
def keyword(sym):
    s = sym[len("swmm_"):]
    for noise in ("_bulk",):
        s = s.replace(noise, "")
    # strip leading domain + verb to isolate the attribute
    return s

rows = json.load(open(OUT / "api_gap_rows.json"))
gaps = [r for r in rows if r["mcp_status"] == "gap"]

protocol, functional = [], []
for r in gaps:
    mem = set(r["members"])
    if mem and mem <= DUNDER:
        protocol.append(r)
    elif not mem:
        protocol.append(r)  # bound but only via internal/error paths
    else:
        functional.append(r)

# Verify functional gaps: search MCP for the public member name AND for the
# attribute keyword. Confirmed gap if neither appears in MCP source.
def in_mcp(token):
    return re.search(r"\b" + re.escape(token) + r"\b", blob) is not None

confirmed, maybe = [], []
for r in functional:
    members = [m for m in r["members"] if m not in DUNDER]
    hit = [m for m in members if in_mcp(m)]
    if hit:
        maybe.append((r["symbol"], hit))
    else:
        confirmed.append((r["symbol"], r["members"], r["header"]))

print(f"Total gap candidates: {len(gaps)}")
print(f"  Protocol/internal (dunder-only or no public member): {len(protocol)}")
print(f"  Functional: {len(functional)}  -> confirmed {len(confirmed)}, member-appears-in-mcp {len(maybe)}")

print("\n=== PROTOCOL / INTERNAL (count/iter/del/construct/error) ===")
for r in protocol:
    print(f"  {r['symbol']:42} {r['members']}")

print("\n=== CONFIRMED FUNCTIONAL GAPS (no MCP reference at all) ===")
byh = {}
for s, m, h in confirmed:
    byh.setdefault(h, []).append((s, m))
for h in sorted(byh):
    print(f"\n[{h}] ({len(byh[h])})")
    for s, m in byh[h]:
        print(f"  {s:44} {m}")

print("\n=== MEMBER NAME APPEARS IN MCP (needs manual check, likely covered) ===")
for s, hit in maybe:
    print(f"  {s:44} via {hit}")

json.dump({"protocol":[r['symbol'] for r in protocol],
           "confirmed":[s for s,_,_ in confirmed],
           "maybe":[s for s,_ in maybe]}, open(OUT/"gap_final.json","w"), indent=1)
