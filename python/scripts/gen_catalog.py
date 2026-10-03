#!/usr/bin/env python3
"""Generate ``openswmm/engine/catalog.json``, a machine-readable map of the
Python engine API.

The catalog lists every target reachable from a :class:`Solver` (and a few
standalone roots) with its properties and methods. Each entry carries its type,
access, units, lifecycle states, applicable subtypes, the C symbols it wraps,
and a one-paragraph description. Downstream consumers use it to reach the whole
API without per-function code: the MCP server's generic ``get``/``set``/``call``
tools and gymnasium's field-path observations.

It is generated from source text, so no compiled engine is needed:

* ``.pyi`` stubs give the typed class graph, signatures and return types.
* ``.pyx`` sources say which properties have setters and hold runtime docstrings.
* The ``.pyx`` call graph (shared with ``plans/parity``) gives C symbols per member.
* Public headers give ``@brief`` text, units and lifecycle states per C symbol.
* ``python/scripts/catalog_overrides.json`` names element kinds, subtypes and
  roots, and lists deliberate exclusions.

Usage::

    python python/scripts/gen_catalog.py          # rewrite catalog.json
    python python/scripts/gen_catalog.py --check  # exit 1 if stale or incomplete
"""
from __future__ import annotations

import argparse
import ast
import importlib.util
import json
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ENGINE_DIR = REPO / "python" / "openswmm" / "engine"
HEADER_DIR = REPO / "include" / "openswmm" / "engine"
OUT_JSON = ENGINE_DIR / "catalog.json"
OVERRIDES = Path(__file__).with_name("catalog_overrides.json")
SCHEMA = 1


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


_provenance = _load("_provenance", REPO / "plans/parity/tools/build_matrix_provenance.py")

# ---------------------------------------------------------------------------
# Stubs
# ---------------------------------------------------------------------------
RECORD_BASES = {"NamedTuple"}
MAPPING_BASES = ("MutableMapping", "Mapping")
SEQUENCE_BASES = ("MutableSequence", "Sequence")


@dataclass
class Member:
    name: str
    form: str                      # "property" | "method"
    type: str = ""                 # property type / method return type
    params: list[dict] = field(default_factory=list)
    doc: str = ""
    stub_setter: bool = False
    static: bool = False


@dataclass
class Cls:
    name: str
    module: str
    bases: list[str]
    doc: str = ""
    record: bool = False
    members: dict[str, Member] = field(default_factory=dict)
    getitem: tuple[str, str] | None = None     # (key type, value type)
    setitem: bool = False
    delitem: bool = False
    iter_type: str = ""


def _first_paragraph(doc: str | None) -> str:
    if not doc:
        return ""
    text = doc.strip().split("\n\n", 1)[0]
    text = re.sub(r"\s+", " ", text)
    # Epytext / reST markup to plain text.
    text = re.sub(r"[LCIBM]\{([^}]*)\}", r"\1", text)
    text = re.sub(r":(?:class|meth|func|attr|data|mod):`~?([^`]*)`", r"\1", text)
    return text.replace("``", "").strip()


def _unparse(node: ast.AST | None) -> str:
    return "" if node is None else ast.unparse(node).strip("'\"")


def parse_stubs() -> tuple[dict[str, Cls], dict[str, dict], dict[str, str]]:
    """Return (classes, module functions, type aliases) from every ``_*.pyi``."""
    classes: dict[str, Cls] = {}
    functions: dict[str, dict] = {}
    aliases: dict[str, str] = {}
    for pyi in sorted(ENGINE_DIR.glob("_*.pyi")):
        module = pyi.stem
        tree = ast.parse(pyi.read_text(encoding="utf-8"))
        for node in tree.body:
            if isinstance(node, ast.Assign) and len(node.targets) == 1 \
                    and isinstance(node.targets[0], ast.Name) and node.targets[0].id.startswith("_"):
                aliases[node.targets[0].id] = _unparse(node.value)
            elif isinstance(node, ast.FunctionDef) and not node.name.startswith("_"):
                functions.setdefault(module, {})[node.name] = _method(node).__dict__
            elif isinstance(node, ast.ClassDef):
                classes[node.name] = _parse_class(node, module)
    return classes, functions, aliases


def _method(node: ast.FunctionDef) -> Member:
    params = []
    args = node.args
    positional = args.posonlyargs + args.args
    defaults = [None] * (len(positional) - len(args.defaults)) + list(args.defaults)
    for a, d in zip(positional, defaults):
        if a.arg in ("self", "cls"):
            continue
        params.append({"name": a.arg, "type": _unparse(a.annotation), "required": d is None})
    for a, d in zip(args.kwonlyargs, args.kw_defaults):
        params.append({"name": a.arg, "type": _unparse(a.annotation), "required": d is None,
                       "keyword_only": True})
    static = any(_unparse(d) in ("staticmethod", "classmethod") for d in node.decorator_list)
    return Member(node.name, "method", _unparse(node.returns), params,
                  _first_paragraph(ast.get_docstring(node)), static=static)


def _parse_class(node: ast.ClassDef, module: str) -> Cls:
    bases = [_unparse(b) for b in node.bases]
    decorators = [_unparse(d) for d in node.decorator_list]
    record = any(b.split("[")[0] in RECORD_BASES for b in bases) or \
        any(d.startswith("dataclass") for d in decorators)
    cls = Cls(node.name, module, bases, _first_paragraph(ast.get_docstring(node)), record)
    for item in node.body:
        if isinstance(item, ast.AnnAssign) and isinstance(item.target, ast.Name):
            name = item.target.id
            if not name.startswith("_"):
                cls.members[name] = Member(name, "property", _unparse(item.annotation))
        elif isinstance(item, ast.FunctionDef):
            decos = [_unparse(d) for d in item.decorator_list]
            if item.name == "__getitem__":
                key = item.args.args[1].annotation if len(item.args.args) > 1 else None
                cls.getitem = (_unparse(key), _unparse(item.returns))
            elif item.name == "__setitem__":
                cls.setitem = True
            elif item.name == "__delitem__":
                cls.delitem = True
            elif item.name == "__iter__":
                cls.iter_type = _unparse(item.returns)
            elif item.name.startswith("_"):
                continue
            elif "property" in decos:
                m = _method(item)
                cls.members[item.name] = Member(item.name, "property", m.type, doc=m.doc)
            elif any(d.endswith(".setter") for d in decos):
                if item.name in cls.members:
                    cls.members[item.name].stub_setter = True
            else:
                cls.members[item.name] = _method(item)
    return cls


def parse_enums() -> dict[str, dict[str, int]]:
    """``{EnumName: {MEMBER: value}}`` from the runtime ``_enums.py``."""
    tree = ast.parse((ENGINE_DIR / "_enums.py").read_text(encoding="utf-8"))
    enums: dict[str, dict[str, int]] = {}
    for node in tree.body:
        if not isinstance(node, ast.ClassDef):
            continue
        if not any(_unparse(b) in ("IntEnum", "IntFlag", "Enum") for b in node.bases):
            continue
        values: dict[str, int] = {}
        for item in node.body:
            if isinstance(item, ast.Assign) and isinstance(item.targets[0], ast.Name):
                name = item.targets[0].id
                if name.startswith("_"):
                    continue
                try:
                    values[name] = ast.literal_eval(item.value)
                except ValueError:
                    ref = _unparse(item.value)
                    if ref in values:
                        values[name] = values[ref]
        enums[node.name] = values
    return enums


# ---------------------------------------------------------------------------
# .pyx: setters and runtime docstrings
# ---------------------------------------------------------------------------
_CLASS = re.compile(r"^(?:cdef\s+)?class\s+(\w+)")
_DEF = re.compile(r"^    (?:cp?def\s+(?:[\w\[\], ]+\s+)?|def\s+)(\w+)\s*\(")
_SETTER = re.compile(r"^    @(\w+)\.setter")


def parse_pyx() -> tuple[set[tuple[str, str]], dict[tuple[str, str], str]]:
    setters: set[tuple[str, str]] = set()
    docs: dict[tuple[str, str], str] = {}
    for pyx in sorted(ENGINE_DIR.glob("_*.pyx")):
        lines = pyx.read_text(encoding="utf-8").splitlines()
        cls = ""
        for i, line in enumerate(lines):
            m = _CLASS.match(line)
            if m:
                cls = m.group(1)
                continue
            if line and not line[0].isspace():
                cls = ""
            if not cls:
                continue
            m = _SETTER.match(line)
            if m:
                setters.add((cls, m.group(1)))
                continue
            m = _DEF.match(line)
            if m and (cls, m.group(1)) not in docs:
                doc = _docstring_after(lines, i)
                if doc:
                    docs[(cls, m.group(1))] = doc
    return setters, docs


def _docstring_after(lines: list[str], i: int) -> str:
    j = i
    while j < len(lines) and not lines[j].rstrip().endswith(":"):
        j += 1
    j += 1
    if j >= len(lines):
        return ""
    first = lines[j].strip()
    m = re.match(r'^[rRuU]?("""|\'\'\')', first)
    if not m:
        return ""
    quote = m.group(1)
    body = first[m.end():]
    if quote in body:
        return _first_paragraph(body.split(quote)[0])
    chunk = [body]
    for k in range(j + 1, min(j + 200, len(lines))):
        if quote in lines[k]:
            chunk.append(lines[k].split(quote)[0])
            break
        chunk.append(lines[k])
    return _first_paragraph("\n".join(chunk))


# ---------------------------------------------------------------------------
# Headers: brief, units, lifecycle states per C symbol
# ---------------------------------------------------------------------------
_DOC_DECL = re.compile(r"/\*\*((?:(?!\*/).)*)\*/\s*SWMM_ENGINE_API[^;{]*?\b(swmm_\w+)\s*\(", re.S)

# First match wins; applied to the documentation of the value a getter returns.
UNIT_PATTERNS: list[tuple[str, str]] = [
    (r"in/hr for us, mm/hr for si|in/hr or mm/hr|rainfall intensity", "rain_rate"),
    (r"in/day (?:for )?us, mm/day (?:for )?si", "evap_rate"),
    (r"mph us, km/hr si", "wind_speed"),
    (r"deg f us, deg c si", "user_temperature"),
    (r"cfs for us, cms for si|project flow units|\bflow units\b", "flow"),
    (r"project length/time units|project velocity units|velocity units|ft/s or m/s", "velocity"),
    (r"project volume units|volume units|ft3 or m3|ft³ or m³", "volume"),
    (r"project area units|\barea units\b|ft2 or m2", "area"),
    (r"project length units|\blength units\b|\bft or m\b|feet or meters|user depth units", "length"),
    (r"project rate units", "rain_rate"),
    (r"pollutant units|concentration units|pollutant's concentration", "concentration"),
    (r"\bcrs units\b", "crs"),
    (r"\(m3/s\)|\(m³/s\)", "m3/s"),
    (r"\(m2/s\)", "m2/s"),
    (r"\(m/s", "m/s"),
    (r"\(m³\)|\(m3\)", "m3"),
    (r"\(m2\)|\(m²\)", "m2"),
    (r"\(m\)", "m"),
    (r"\(mm\)|millimetres|millimeters", "mm"),
    (r"\(in or mm\)|inches or mm", "rain_depth"),
    (r"\(°c\)|\(deg ?c\)|\bdegc\b|degrees c(?:elsius)?\b|°c", "degC"),
    (r"\bin seconds\b|\(seconds\)|\bseconds\b", "s"),
    (r"\bin hours\b|\(hours\)", "h"),
    (r"\bin days\b|\(days\b", "d"),
    (r"\bin percent\b|\bpercentage\b", "percent"),
    (r"fraction per second", "1/s"),
    (r"\bfraction\b|\(0\.\.1\)|\bratio\b", "fraction"),
    (r"\bdimensionless\b", "dimensionless"),
    (r"\(1/day\)", "1/day"),
    (r"\bw/m2\b|w/m²", "W/m2"),
    (r"project temperature units", "temperature"),
    (r"project length squared per second", "length2/s"),
    (r"ft-lb/s", "ft-lb/s"),
    (r"\(ft3\)", "volume"),
    (r"\(degrees\b", "deg"),
    (r"\(minutes\b", "min"),
]
_STATE_RE = re.compile(r"@param\s+engine\b[^\n]*?\(([A-Z_]+(?:\s*(?:,|or|/)\s*[A-Z_]+)*)\s+state", re.S)


_CALL = re.compile(r"(?:\b(self|cls)\s*\.\s*)?\b([A-Za-z_]\w*)\s*\(")
# Error reporting runs on every failing call; it is not functionality.
_NOT_HELPERS = {"_check"}


_CLASS_BASES = re.compile(r"^\s*(?:cdef\s+)?class\s+(\w+)\s*(?:\(([^)]*)\))?")


def c_symbols_by_def() -> dict[tuple[str, str], set[str]]:
    """``{(class, def): C symbols}``, following calls into helpers.

    A public method often reaches the C API through a private helper
    (``self._add``, ``_call_scalar``) or a module-level function, possibly in
    another module (``Solver.thread_info`` -> ``_transport.thread_info``).
    Those symbols belong to the public member too. ``self._helper`` calls
    dispatch on the concrete class, so a method inherited from a Cython base
    (``Aquifers.rename`` from ``_NamedObjects``) gets the subclass's symbols.
    Module-level defs use the class ``""``.
    """
    direct: dict[tuple[str, str], set[str]] = defaultdict(set)
    for sym, sites in _provenance.pyx_callgraph().items():
        for _module, cls, name in sites:
            direct[(cls, name)].add(sym)
    calls: dict[tuple[str, str], set[tuple[str, str]]] = defaultdict(set)
    defined: set[tuple[str, str]] = set()
    module_defs: dict[str, int] = defaultdict(int)          # name -> modules defining it
    bases: dict[str, list[str]] = {}
    for pyx in sorted(ENGINE_DIR.glob("*.pyx")):
        scopes: list[tuple[int, str, str]] = []
        in_extern, extern_indent = False, 0
        for raw in _provenance._audit.executable_cython(pyx.read_text(encoding="utf-8")).splitlines():
            code = raw.split("#", 1)[0]
            if not code.strip():
                continue
            indent = len(code) - len(code.lstrip())
            if _provenance._CDEF_EXTERN.match(code.strip()):
                in_extern, extern_indent = True, indent
                continue
            if in_extern:
                if indent <= extern_indent:
                    in_extern = False
                else:
                    continue
            if (cm := _provenance._CLASS_RE.match(code)):
                while scopes and scopes[-1][0] >= len(cm.group("i")):
                    scopes.pop()
                scopes.append((len(cm.group("i")), "class", cm.group("name")))
                bm = _CLASS_BASES.match(code)
                bases.setdefault(cm.group("name"), [b.strip() for b in (bm.group(2) or "").split(",")
                                                     if b.strip()])
                continue
            if (dm := _provenance._DEF_RE.match(code)):
                while scopes and scopes[-1][0] >= len(dm.group("i")):
                    scopes.pop()
                if not any(s[1] == "def" for s in scopes):
                    cls = next((s[2] for s in reversed(scopes) if s[1] == "class"), "")
                    defined.add((cls, dm.group("name")))
                    if not cls:
                        module_defs[dm.group("name")] += 1
                scopes.append((len(dm.group("i")), "def", dm.group("name")))
                continue
            cur_def = next((s for s in reversed(scopes) if s[1] == "def"), None)
            if cur_def is None:
                continue
            cls = next((s[2] for s in reversed(scopes) if s[1] == "class"), "")
            for qual, name in _CALL.findall(code):
                calls[(cls, cur_def[2])].add((qual, name))

    def mro(cls: str) -> list[str]:
        out, todo = [], [cls]
        while todo:
            c = todo.pop(0)
            if c not in out:
                out.append(c)
                todo.extend(bases.get(c, []))
        return out

    def owner(cls: str, name: str) -> str | None:
        return next((c for c in mro(cls) if (c, name) in defined), None)

    memo: dict[tuple[str, str], set[str]] = {}

    def symbols(cls: str, name: str, seen: frozenset) -> set[str]:
        """Symbols reached by ``cls.name`` (``cls`` is the concrete class)."""
        key = (cls, name)
        if key in memo:
            return memo[key]
        home = owner(cls, name) if cls else ("" if ("", name) in defined else None)
        if home is None:
            return set()
        out = set(direct.get((home, name), ()))
        for qual, callee in calls.get((home, name), ()):
            if callee in _NOT_HELPERS:
                continue
            if qual and cls:
                target = (cls, callee) if callee.startswith("_") and not callee.startswith("__") \
                    else None
            else:
                target = ("", callee) if module_defs.get(callee) == 1 or ("", callee) in defined \
                    else None
            if target and target not in seen and target != key:
                out |= symbols(target[0], target[1], seen | {key})
        memo[key] = out
        return out

    result: dict[tuple[str, str], set[str]] = {}
    for cls in {c for c, _ in defined} | set(bases):
        for c in mro(cls):
            for d_cls, name in defined:
                if d_cls == c and (cls, name) not in result:
                    result[(cls, name)] = symbols(cls, name, frozenset())
    return result


def parse_headers() -> dict[str, dict]:
    info: dict[str, dict] = {}
    for header in sorted(HEADER_DIR.glob("openswmm_*.h")):
        text = header.read_text(encoding="utf-8")
        for m in _DOC_DECL.finditer(text):
            doc = re.sub(r"^\s*\*", "", m.group(1), flags=re.M)
            info[m.group(2)] = _header_entry(doc)
    return info


def _header_entry(doc: str) -> dict:
    brief = re.search(r"@brief\s+(.*?)(?:\n\s*\n|\n\s*@|$)", doc, re.S)
    outs = re.findall(r"@param\[out\][^\n]*(?:\n(?!\s*@)[^\n]*)*", doc)
    # With several out-parameters a unit phrase cannot be tied to one of them.
    out_lines = outs[0] if len(outs) == 1 else ""
    returns = " ".join(re.findall(r"@returns?\b[^\n]*", doc))
    states = _STATE_RE.search(doc)
    entry = {
        "brief": re.sub(r"\s+", " ", brief.group(1)).strip() if brief else "",
        "units": _units(out_lines) or _units(brief.group(1) if brief else "")
                 or _units(returns),
        "phases": sorted(set(re.findall(r"[A-Z_]{4,}", states.group(1)))) if states else [],
    }
    return entry


def _rank_symbols(member: str, symbols: list[str]) -> list[str]:
    """Order a member's C symbols so the one documenting it comes first.

    Getters whose name contains the member name win; counts and setters
    (called only for sizing or writing) come last.
    """
    stem = member.rstrip("s")

    def key(sym: str) -> tuple[int, int, str]:
        return (0 if stem and stem in sym else 1,
                1 if sym.endswith("_count") or "_set_" in sym else 0, sym)

    return sorted(symbols, key=key)


def _units(text: str) -> str | None:
    low = text.lower()
    for pattern, unit in UNIT_PATTERNS:
        if re.search(pattern, low):
            return unit
    return None


# ---------------------------------------------------------------------------
# Catalog assembly
# ---------------------------------------------------------------------------
def _type_name(t: str) -> str:
    """Strip quotes/Optional from a stub annotation to find a class name."""
    t = t.strip().strip("'\"")
    m = re.fullmatch(r"(?:Optional\[)?\s*['\"]?(\w+)['\"]?\s*\]?", t)
    return m.group(1) if m else ""


def _expand(t: str, aliases: dict[str, str]) -> str:
    return re.sub(r"\b(_[A-Z]\w*)\b", lambda m: aliases.get(m.group(1), m.group(1)), t)


class Builder:
    def __init__(self) -> None:
        self.classes, self.functions, self.aliases = parse_stubs()
        self.enums = parse_enums()
        self.setters, self.pyx_docs = parse_pyx()
        self.headers = parse_headers()
        self.overrides = json.loads(OVERRIDES.read_text(encoding="utf-8"))
        self.c_by_member = c_symbols_by_def()
        self.targets: dict[str, dict] = {}
        self.members: list[dict] = []
        self.class_target: dict[str, str] = {}

    # -- walk ---------------------------------------------------------------
    def is_target_class(self, name: str) -> bool:
        cls = self.classes.get(name)
        return bool(cls) and not cls.record and name not in self.enums \
            and name not in self.overrides["record_classes"]

    def walk(self, target: str, cls_name: str, path: str, **extra) -> None:
        """Breadth-first walk of the typed property/element graph from one root.

        Breadth-first so a class is named by its shortest path: ``gages[]`` is
        the ``gage`` element before ``subcatchments[].gage`` can claim it.
        """
        ov = self.overrides
        queue = [(target, cls_name, path, extra)]
        while queue:
            target, cls_name, path, extra = queue.pop(0)
            if target in self.targets or cls_name in self.class_target:
                continue
            cls = self.classes[cls_name]
            entry = {"class": cls_name, "module": cls.module, "path": path,
                     "doc": cls.doc or self.pyx_docs.get((cls_name, "__init__"), "")}
            entry.update({k: v for k, v in extra.items() if v is not None})
            if target in ov["subtypes"]:
                entry["subtypes"] = ov["subtypes"][target]
            self.targets[target] = entry
            self.class_target[cls_name] = target
            prefix = "" if target == "solver" else target + "."
            for name, m in cls.members.items():
                if f"{cls_name}.{name}" in ov["exclude_members"]:
                    continue
                child = _type_name(m.type) if m.form == "property" else ""
                if child and self.is_target_class(child):
                    queue.append((prefix + name, child, f"{path}.{name}".lstrip("."),
                                  {"parent": target}))
            if cls.getitem:
                value = _type_name(cls.getitem[1])
                if value and self.is_target_class(value) and value not in self.class_target:
                    elem = ov["element_names"].get(target, f"{target}[]")
                    entry["element"] = elem
                    queue.append((elem, value, f"{path}[]",
                                  {"collection": target, "key": _expand(cls.getitem[0], self.aliases),
                                   "subtype_field": ov["subtype_fields"].get(value)}))

    def build(self) -> dict:
        self.walk("solver", "Solver", "")
        for root, spec in self.overrides["roots"].items():
            self.walk(root, spec["class"], "", construct=spec["construct"])
            ctor = set().union(*(self.c_by_member.get((spec["class"], d), set())
                                 for d in ("__cinit__", "__init__", "__dealloc__")))
            if ctor:
                self.targets[root]["c"] = sorted(ctor)
        for target, entry in self.targets.items():
            self.add_members(target, entry)
        for target, variants in self.overrides["variants"].items():
            for cls_name in variants:
                self.class_target.setdefault(cls_name, target)
                for m in self.classes[cls_name].members.values():
                    self.members.append(dict(self.member_entry(target, cls_name, m), variant=cls_name))
        bulk = self.bulk_links()
        by_path = {m["path"]: m for m in self.members}
        for m in self.members:
            if m["form"] == "property" and m["path"] in bulk:
                m["bulk"] = bulk[m["path"]]
                array = by_path[bulk[m["path"]]]
                if "units" in m and "units" not in array:
                    array["units"] = m["units"]
        records = {name: {"fields": {k: _expand(v.type, self.aliases)
                                     for k, v in c.members.items() if v.form == "property"},
                          "doc": c.doc}
                   for name, c in sorted(self.classes.items())
                   if c.record or name in self.overrides["record_classes"]}
        return {
            "schema": SCHEMA,
            "note": "Generated by python/scripts/gen_catalog.py -- do not edit by hand.",
            "c_exports": len(_provenance.c_symbols()),
            "targets": dict(sorted(self.targets.items())),
            "members": sorted(self.members, key=lambda m: m["path"]),
            "functions": {mod: {name: dict(fn, c=sorted(self.c_by_member.get(("", name), ())))
                                for name, fn in sorted(f.items())}
                          for mod, f in sorted(self.functions.items())
                          if mod.lstrip("_") in self.overrides["function_modules"]},
            "records": records,
            "enums": dict(sorted(self.enums.items())),
        }

    def add_members(self, target: str, entry: dict) -> None:
        cls_name = entry["class"]
        cls = self.classes[cls_name]
        ov = self.overrides
        for name, m in cls.members.items():
            if f"{cls_name}.{name}" in ov["exclude_members"]:
                continue
            child = _type_name(m.type) if m.form == "property" else ""
            if child and self.is_target_class(child):
                ref = self.class_target.get(child, "")
                if ref and self.targets[ref].get("collection") and ref != target:
                    # A reference to another element (e.g. a subcatchment's gage):
                    # expose it as a value -- the referenced element's id.
                    self.members.append(dict(self.member_entry(target, cls_name, m),
                                             type=f"ref:{ref}"))
                continue            # otherwise a navigable sub-target, not a value
            self.members.append(self.member_entry(target, cls_name, m))
        if cls.getitem and not entry.get("element"):
            self.members.extend(self.item_members(target, cls))
        elif any(b.startswith(MAPPING_BASES + SEQUENCE_BASES) for b in cls.bases):
            self.members.extend(self.item_members(target, cls))
        elif cls.iter_type and not entry.get("element"):
            # Iterable but not indexable (Inlets, Aquifers): list it.
            syms = set().union(*(self.c_by_member.get((cls_name, d), set())
                                 for d in ("__iter__", "__len__", "__contains__")))
            self.members.append({"path": f"{target}.items", "target": target, "name": "items",
                                 "form": "item", "python": f"list({cls_name})", "doc": cls.doc,
                                 "returns": f"list[tuple[int, {_expand(cls.iter_type, self.aliases)}]]",
                                 "params": [], "c": sorted(syms)})
        if entry.get("element"):
            # Element collections are enumerated through len()/iteration by every tool.
            syms = set().union(*(self.c_by_member.get((cls_name, d), set())
                                 for d in ("__len__", "__iter__", "__getitem__", "__contains__")))
            if syms:
                entry["c"] = sorted(set(entry.get("c", [])) | syms)

    def member_entry(self, target: str, cls_name: str, m: Member) -> dict:
        prefix = "" if target == "solver" else target + "."
        c_syms = sorted(self.c_by_member.get((cls_name, m.name), ()))
        header = next((self.headers[s] for s in _rank_symbols(m.name, c_syms)
                       if s in self.headers), {})
        doc = self.pyx_docs.get((cls_name, m.name)) or m.doc or header.get("brief", "")
        out = {"path": prefix + m.name, "target": target, "name": m.name, "form": m.form,
               "python": f"{cls_name}.{m.name}", "doc": doc, "c": c_syms}
        if m.form == "property":
            out["type"] = _expand(m.type, self.aliases)
            out["access"] = "rw" if (cls_name, m.name) in self.setters or m.stub_setter else "r"
        else:
            out["returns"] = _expand(m.type, self.aliases)
            out["params"] = [dict(p, type=_expand(p["type"], self.aliases)) for p in m.params]
            if m.static:
                out["static"] = True
        out["c"] = sorted(set(out["c"]) | self.returned_symbols(m.type))
        units = ov_units = self.overrides["units"].get(prefix + m.name)
        # Tuples mix quantities and ints are usually codes, so only plain
        # floats and float arrays carry a single unit.
        numeric = m.type == "float" or m.type.startswith("NDArray")
        if ov_units is None and m.form == "property" and numeric:
            units = header.get("units") or (_units(doc) if m.type == "float" else None)
        if units:
            out["units"] = units
        phases = sorted({p for s in c_syms if s in self.headers for p in self.headers[s]["phases"]})
        if phases:
            out["phases"] = phases
        return out

    def returned_symbols(self, type_str: str) -> set[str]:
        """C symbols of an uncatalogued object a member hands back.

        ``OutputReader.node_stats`` returns an ``_OutputNodeStats`` whose
        properties call the C API; serializing the result reaches them.
        """
        out: set[str] = set()
        for name in re.findall(r"\b(_?[A-Z]\w*)\b", type_str or ""):
            obj = self.classes.get(name)
            if obj is None or name in self.class_target or name in self.enums:
                continue
            for d in [p for p, pm in obj.members.items() if pm.form == "property"] + \
                    ["__len__", "__iter__", "__getitem__"]:
                out |= self.c_by_member.get((name, d), set())
        return out

    def item_members(self, target: str, cls: Cls) -> list[dict]:
        """Synthesize ``items``/``get_item``/``set_item``/``delete_item`` for mapping-like targets."""
        key_t, val_t = cls.getitem or ("", "")
        for b in cls.bases:
            m = re.match(r"Mutable(?:Mapping|Sequence)\[(.*)\]", b)
            if m:
                parts = [p.strip() for p in m.group(1).split(",", 1)]
                key_t, val_t = (parts[0], parts[1]) if len(parts) == 2 else ("int", parts[0])
        mutable = cls.setitem or any(b.startswith("Mutable") for b in cls.bases)
        key_t, val_t = _expand(key_t, self.aliases), _expand(val_t, self.aliases)
        def syms(*defs: str) -> list[str]:
            return sorted(set().union(*(self.c_by_member.get((cls.name, d), set()) for d in defs)))

        base = {"target": target, "form": "item", "python": f"{cls.name}[]", "doc": cls.doc}
        values = sorted(self.returned_symbols(val_t))
        out = [dict(base, path=f"{target}.items", name="items", returns=f"list[tuple[{key_t}, {val_t}]]",
                    params=[], c=sorted(set(syms("__iter__", "__len__", "__getitem__", "keys",
                                                 "items", "values")) | set(values))),
               dict(base, path=f"{target}.get_item", name="get_item", returns=val_t,
                    params=[{"name": "key", "type": key_t, "required": True}],
                    c=sorted(set(syms("__getitem__")) | set(values)))]
        if mutable:
            out.append(dict(base, path=f"{target}.set_item", name="set_item", returns="None",
                            params=[{"name": "key", "type": key_t, "required": True},
                                    {"name": "value", "type": val_t, "required": True}],
                            c=syms("__setitem__")))
        if cls.delitem or any(b.startswith("Mutable") for b in cls.bases):
            out.append(dict(base, path=f"{target}.delete_item", name="delete_item", returns="None",
                            params=[{"name": "key", "type": key_t, "required": True}],
                            c=syms("__delitem__")))
        return out

    def bulk_links(self) -> dict[str, str]:
        """Map an element property path to the collection array that holds it for every element."""
        links: dict[str, str] = {}
        for target, entry in self.targets.items():
            elem = entry.get("element")
            if not elem:
                continue
            coll = self.classes[entry["class"]]
            elem_props = {m["name"] for m in self.members
                          if m["target"] == elem and m["form"] == "property"}
            for name, m in coll.members.items():
                if m.form != "property" or "NDArray" not in m.type:
                    continue
                singular = name[:-3] + "y" if name.endswith("ies") else name
                for cand in (name, singular, name[:-1], name[:-2]):
                    if cand in elem_props and cand != "ids":
                        links[f"{elem}.{cand}"] = f"{target}.{name}"
                        break
        return links

    # -- completeness -------------------------------------------------------
    def uncovered(self) -> list[str]:
        """Public classes/members neither in the catalog nor excluded."""
        covered_cls = {e["class"] for e in self.targets.values()}
        missing = []
        for name, cls in sorted(self.classes.items()):
            if name.startswith("_") and name not in covered_cls:
                continue
            if cls.record or name in self.enums or name in self.overrides["exclude_classes"] \
                    or name in self.overrides["record_classes"] or name in self.class_target:
                continue
            if name not in covered_cls:
                missing.append(name)
        return missing


def unitless_floats(catalog: dict) -> list[str]:
    """Float properties without a unit kind (``dimensionless`` counts as one)."""
    return [m["path"] for m in catalog["members"]
            if m["form"] == "property" and m.get("type") == "float" and "units" not in m]


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true",
                    help="fail if catalog.json is stale, a class is uncovered, or a float field has no unit kind")
    args = ap.parse_args(argv)
    b = Builder()
    catalog = b.build()
    text = json.dumps(catalog, indent=1, ensure_ascii=False) + "\n"
    missing = b.uncovered()
    unitless = unitless_floats(catalog)
    print(f"targets={len(catalog['targets'])} members={len(catalog['members'])} "
          f"float fields without a unit kind={len(unitless)}")
    problems = []
    if missing:
        problems.append("classes neither reachable nor excluded: " + ", ".join(missing))
    if unitless:
        problems.append("float fields without a unit kind (add one, or 'dimensionless', to "
                        "catalog_overrides.json): " + ", ".join(unitless))
    if args.check:
        current = OUT_JSON.read_text(encoding="utf-8") if OUT_JSON.exists() else ""
        if current != text:
            problems.append(f"{OUT_JSON.relative_to(REPO)} is stale; run python/scripts/gen_catalog.py")
    else:
        OUT_JSON.write_text(text, encoding="utf-8")
        print(f"wrote {OUT_JSON.relative_to(REPO)} ({len(text) // 1024} KiB)")
    for p in problems:
        print("ERROR:", p, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
