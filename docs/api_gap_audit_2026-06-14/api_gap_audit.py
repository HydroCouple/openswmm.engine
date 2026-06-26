#!/usr/bin/env python3
"""Three-layer API gap audit: C-API -> Cython bindings -> MCP tools.

Pure text analysis. Outputs JSON + CSV for the review markdown.
"""
from __future__ import annotations
import json, re, csv
from pathlib import Path

ENGINE = Path("/sessions/wizardly-ecstatic-johnson/mnt/openswmm.engine")
MCP = Path("/sessions/wizardly-ecstatic-johnson/mnt/openswmm.mcp")
OUT = Path("/sessions/wizardly-ecstatic-johnson/mnt/outputs")

HEADERS = ENGINE / "include" / "openswmm" / "engine"
CYTHON = ENGINE / "python" / "openswmm" / "engine"
MCP_TOOLS = MCP / "src" / "openswmm_mcp" / "tools"

# ---------------------------------------------------------------------------
# Layer 0: C-API symbols + signatures, per header
# ---------------------------------------------------------------------------
# Capture full declaration up to closing paren (may span lines).
DECL = re.compile(
    r"SWMM_ENGINE_API\s+(.*?\b(swmm_[a-z0-9_]+)\s*\(.*?\))\s*;",
    re.DOTALL,
)

c_syms = {}          # symbol -> {header, sig}
header_syms = {}     # header -> [symbols]
for h in sorted(HEADERS.glob("*.h")):
    src = h.read_text(encoding="utf-8")
    header_syms[h.name] = []
    for m in DECL.finditer(src):
        sig = re.sub(r"\s+", " ", m.group(1)).strip()
        name = m.group(2)
        if name in c_syms:
            continue
        c_syms[name] = {"header": h.name, "sig": sig}
        header_syms[h.name].append(name)

# ---------------------------------------------------------------------------
# Layer 1: Cython binding references + symbol -> enclosing member map
# ---------------------------------------------------------------------------
REF = re.compile(r"\b(swmm_[a-z0-9_]+)\b")
cython_refs = set()
for p in list(CYTHON.glob("*.pxd")) + list(CYTHON.glob("*.pyx")):
    for m in REF.finditer(p.read_text(encoding="utf-8")):
        cython_refs.add(m.group(1))

# symbol -> set of member where it's called inside a Python-visible
# (def / cpdef / property) body.  Only def/cpdef/property are reachable from
# MCP; `cdef` C-level helpers and `cdef <type> var = ...` declarations are
# deliberately ignored (the latter previously mis-parsed as definitions).
DEF = re.compile(
    r"^(\s*)(?:async\s+)?(?:cpdef|def)\s+(?:[\w\[\],.*]+\s+)?([a-zA-Z_][a-zA-Z0-9_]*)\s*\(")
sym_members = {}     # symbol -> set("member")
member_module = {}   # member -> set(module)
for pyx in sorted(CYTHON.glob("*.pyx")):
    mod = pyx.stem  # e.g. _links
    lines = pyx.read_text(encoding="utf-8").splitlines()
    stack = []  # (indent, member)
    for ln in lines:
        dm = DEF.match(ln)
        if dm:
            indent = len(dm.group(1))
            member = dm.group(2)
            while stack and stack[-1][0] >= indent:
                stack.pop()
            stack.append((indent, member))
            member_module.setdefault(member, set()).add(mod)
        for sm in REF.finditer(ln):
            s = sm.group(1)
            if stack:
                sym_members.setdefault(s, set()).add(stack[-1][1])

# ---------------------------------------------------------------------------
# Layer 1b: .pyi stub member names (for stub-coverage signal)
# ---------------------------------------------------------------------------
pyi_members = set()
PYIDEF = re.compile(r"^\s*def\s+([a-zA-Z_][a-zA-Z0-9_]*)")
for pyi in CYTHON.glob("*.pyi"):
    for ln in pyi.read_text(encoding="utf-8").splitlines():
        m = PYIDEF.match(ln)
        if m:
            pyi_members.add(m.group(1))

# ---------------------------------------------------------------------------
# Layer 2: MCP tool surface
# ---------------------------------------------------------------------------
# (a) dotted identifiers reached anywhere in tool files (method/attr names)
# (b) registered tool names: @<ns>_mcp.tool() async def <name>
DOT = re.compile(r"\.([a-zA-Z_][a-zA-Z0-9_]*)")
STR = re.compile(r"""['"]([a-zA-Z_][a-zA-Z0-9_]*)['"]""")
TOOLNAME = re.compile(r"@\w+_mcp\.tool[^\n]*\)\s*\n\s*async\s+def\s+([a-zA-Z_][a-zA-Z0-9_]*)", re.DOTALL)
mcp_dotted = set()
mcp_strings = set()   # quoted tokens — catches getattr / string-dispatch wiring
mcp_tools = []
mcp_blob = ""
# Scan the WHOLE MCP package: lifecycle / error / callback bindings are wired
# through session.py, backends/, dependencies.py — not the tools/ files.
MCP_PKG = MCP / "src" / "openswmm_mcp"
for tf in sorted(MCP_PKG.rglob("*.py")):
    src = tf.read_text(encoding="utf-8")
    mcp_blob += "\n" + src
    for m in DOT.finditer(src):
        mcp_dotted.add(m.group(1))
    for m in STR.finditer(src):
        mcp_strings.add(m.group(1))
    if tf.parent.name == "tools":
        for m in TOOLNAME.finditer(src):
            mcp_tools.append(m.group(1))

# Only string tokens that are actually binding member names count, so random
# string literals can't manufacture coverage.
all_binding_members = set(member_module.keys())
mcp_reached_members = mcp_dotted | (mcp_strings & all_binding_members)

# A C symbol is "MCP-reachable" if any of its enclosing binding members is
# referenced as a dotted identifier in the MCP tool layer.
def mcp_reachable(sym):
    members = sym_members.get(sym, set())
    hit = members & mcp_reached_members
    return (bool(hit), sorted(hit))

# Capability key: collapse parallel C surfaces so a "covered" capability that
# is reachable through *some* sibling isn't reported as a hard gap.
#   - strip trailing _bulk (bulk vs scalar variant)
#   - canonicalise the two stat naming schemes:
#       swmm_stat_node_max_depth  == swmm_node_get_stat_max_depth
def cap_key(sym):
    s = sym
    if s.endswith("_bulk"):
        s = s[:-5]
    m = re.match(r"swmm_stat_(node|link|subcatch)_(.+)", s)
    if m:
        s = f"swmm_{m.group(1)}_stat_{m.group(2)}"
    s = s.replace("_get_stat_", "_stat_")
    return s

# ---------------------------------------------------------------------------
# Assemble per-symbol rows
# ---------------------------------------------------------------------------
# First pass: which capability keys are directly reachable via any sibling.
direct_caps = set()
for name in c_syms:
    if mcp_reachable(name)[0]:
        direct_caps.add(cap_key(name))

rows = []
for name, info in sorted(c_syms.items()):
    bound = name in cython_refs
    members = sorted(sym_members.get(name, set()))
    stubbed = any(m in pyi_members for m in members)
    mr, mhit = mcp_reachable(name)
    if mr:
        status = "direct"
    elif cap_key(name) in direct_caps:
        status = "capability"   # covered via a sibling (bulk/scalar/stat alias)
    else:
        status = "gap"
    rows.append({
        "symbol": name,
        "header": info["header"],
        "sig": info["sig"],
        "bound": bound,
        "members": members,
        "stubbed": stubbed,
        "mcp_reachable": mr,
        "mcp_members": mhit,
        "mcp_status": status,
    })

# ---------------------------------------------------------------------------
# Summaries
# ---------------------------------------------------------------------------
total = len(rows)
unbound = [r for r in rows if not r["bound"]]
bound_no_stub = [r for r in rows if r["bound"] and not r["stubbed"]]
bound_no_mcp = [r for r in rows if r["bound"] and not r["mcp_reachable"]]

true_gap = [r for r in rows if r["mcp_status"] == "gap"]
cap_only = [r for r in rows if r["mcp_status"] == "capability"]

by_header = {}
for r in rows:
    h = r["header"]
    d = by_header.setdefault(h, {"total":0,"unbound":0,"direct":0,"cap":0,"gap":0})
    d["total"] += 1
    if not r["bound"]:
        d["unbound"] += 1
    d[{"direct":"direct","capability":"cap","gap":"gap"}[r["mcp_status"]]] += 1

(OUT / "api_gap_rows.json").write_text(json.dumps(rows, indent=1))
with (OUT / "api_gap_rows.csv").open("w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["symbol","header","bound","stubbed","mcp_reachable","members","mcp_members","sig"])
    for r in rows:
        w.writerow([r["symbol"],r["header"],r["bound"],r["stubbed"],r["mcp_reachable"],
                    ";".join(r["members"]),";".join(r["mcp_members"]),r["sig"]])

print(f"C-API symbols (with signatures): {total}")
print(f"Cython refs (any swmm_*):        {len(cython_refs)}")
print(f"MCP tools registered:            {len(mcp_tools)}")
print(f"UNBOUND (no Cython):             {len(unbound)}")
print(f"MCP direct:                      {sum(1 for r in rows if r['mcp_status']=='direct')}")
print(f"MCP capability-covered (sibling):{len(cap_only)}")
print(f"MCP TRUE GAP (no path):          {len(true_gap)}")
print()
print(f"{'header':32} tot  unb  dir  cap  GAP")
for h in sorted(by_header):
    d = by_header[h]
    print(f"  {h:30} {d['total']:>3} {d['unbound']:>4} {d['direct']:>4} {d['cap']:>4} {d['gap']:>4}")
print()
print("TRUE GAP symbols by header:")
gh = {}
for r in true_gap:
    gh.setdefault(r["header"], []).append(r["symbol"])
for h in sorted(gh):
    print(f"\n[{h}]  ({len(gh[h])})")
    for s in gh[h]:
        print(f"  - {s}")
