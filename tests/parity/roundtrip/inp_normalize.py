"""Section-aware .inp normaliser for the writer round-trip audit.

The audit compares a deck against its own generation-1 rewrite. A raw textual
diff is useless for that: the writer re-flows every column, drops comments,
reorders sections and prints 1.5 as "1.5" where the original said "1.500000".
This module reduces both files to a comparable shape — section -> rows ->
tokens, with numbers compared as numbers — so that what survives the diff is
a genuine difference in *content*.

What it deliberately does NOT do is decide whether a difference matters. A
value legacy itself normalises (a negative conduit offset clamped to 0, see
link.c:1061-1069) shows up here as a difference and is benign. Triage against
the legacy-agrees gate, never against this module alone.
"""

from __future__ import annotations

import math
import re

# Sections whose row order carries no meaning, so rows are compared as a
# multiset keyed on the first token (the object name).
_UNORDERED = {
    "JUNCTIONS", "OUTFALLS", "DIVIDERS", "STORAGE", "CONDUITS", "PUMPS",
    "ORIFICES", "WEIRS", "OUTLETS", "XSECTIONS", "LOSSES", "SUBCATCHMENTS",
    "SUBAREAS", "INFILTRATION", "RAINGAGES", "AQUIFERS", "GROUNDWATER",
    "COORDINATES", "SYMBOLS", "TAGS", "INFLOWS", "DWF", "RDII", "TREATMENT",
    "COVERAGES", "LOADINGS", "BUILDUP", "WASHOFF", "LANDUSES", "POLLUTANTS",
    "INLET_USAGE", "STREETS", "SNOWPACKS", "LID_USAGE",
    # Key/value sections: the writer emits its own canonical order, so a
    # positional compare reports every line after the first insertion as
    # changed. Keyed on the option/flag name they compare correctly.
    "OPTIONS", "REPORT", "EVAPORATION", "TEMPERATURE", "ADJUSTMENTS", "FILES",
}

# Multi-line object sections: several rows share one name and their order
# WITHIN that name matters (curve ordinates, series points, LID layers), but
# the objects themselves may be emitted in any order. The keyed comparison
# below groups on the name and then compares positionally inside the group,
# which is exactly right for these — comparing them positionally across the
# whole section reports a cascade of mismatches as soon as one object moves.
_UNORDERED |= {
    "CURVES", "TIMESERIES", "PATTERNS", "HYDROGRAPHS", "LID_CONTROLS",
    "INLETS", "VERTICES", "POLYGONS",
}

# Genuinely positional: the first token is a KEYWORD that repeats (NC/X1/GR,
# RULE/IF/THEN), so keying on it would collapse unrelated rows together.
_ORDERED = {"CONTROLS", "TRANSECTS", "TITLE"}

_NUM_RE = re.compile(r"^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$")
_CLOCK_RE = re.compile(r"^(\d+):([0-5]?\d)(?::([0-5]?\d))?$")


def _as_hours(tok: str) -> float | None:
    """Decimal hours for a time token, or None if it is not one.

    Legacy accepts either form in [TIMESERIES] and several [OPTIONS] keys
    (table.c tries getDouble first, then HH:MM[:SS]), and our writer converts
    decimal hours to H:MM on save. Comparing the two forms as text reports
    every row of every series as changed and buries the real findings.
    """
    m = _CLOCK_RE.match(tok)
    if not m:
        return None
    h, mi, s = m.group(1), m.group(2), m.group(3)
    return int(h) + int(mi) / 60.0 + (int(s) / 3600.0 if s else 0.0)


def _strip_comment(line: str) -> str:
    """Legacy truncates a line at the first ';' anywhere (input.c:919-920)."""
    i = line.find(";")
    return line if i < 0 else line[:i]


def parse_sections(text: str) -> dict[str, list[list[str]]]:
    """Split an .inp into {SECTION_NAME: [[token, ...], ...]}.

    Section names are upper-cased and stripped of the brackets. Comment-only
    and blank lines are dropped. A duplicated section header appends to the
    section already collected, which is what every SWMM parser does.
    """
    out: dict[str, list[list[str]]] = {}
    cur: str | None = None
    for raw in text.splitlines():
        line = _strip_comment(raw).strip()
        if not line:
            continue
        if line.startswith("["):
            name = line[1:].split("]")[0].strip().upper()
            cur = name
            out.setdefault(cur, [])
            continue
        if cur is None:
            continue
        toks = line.split()
        if toks:
            out[cur].append(toks)
    return out


def _tok_equal(a: str, b: str, rtol: float, atol: float) -> bool:
    """Compare two tokens, numerically when both look like numbers."""
    if a == b:
        return True
    # A clock token against a decimal-hours token: compare as hours. The
    # tolerance is absolute here — H:MM quantises to a whole minute, so a
    # genuine sub-minute loss still shows up as a difference.
    ha, hb = _as_hours(a), _as_hours(b)
    if (ha is not None) != (hb is not None):
        other = b if ha is not None else a
        if _NUM_RE.match(other):
            clock = ha if ha is not None else hb
            return abs(clock - float(other)) <= 1e-9
    elif ha is not None and hb is not None:
        return abs(ha - hb) <= 1e-9
    if _NUM_RE.match(a) and _NUM_RE.match(b):
        try:
            fa, fb = float(a), float(b)
        except ValueError:
            return False
        if math.isnan(fa) and math.isnan(fb):
            return True
        return abs(fa - fb) <= max(atol, rtol * max(abs(fa), abs(fb)))
    return a.upper() == b.upper()


def _rows_equal(ra: list[str], rb: list[str], rtol: float, atol: float) -> bool:
    if len(ra) != len(rb):
        return False
    return all(_tok_equal(x, y, rtol, atol) for x, y in zip(ra, rb))


def _key(row: list[str]) -> str:
    return row[0].upper() if row else ""


def diff(text_a: str, text_b: str, *, rtol: float = 1e-9,
         atol: float = 1e-12, ignore: set[str] | None = None) -> list[dict]:
    """Diff two .inp texts. Returns one record per difference found.

    Each record is {"section", "kind", "detail"} where kind is one of:
      section-missing  — the section exists in A but not in B (or vice versa)
      row-missing      — an object row present in A has no counterpart in B
      row-extra        — a row in B with no counterpart in A
      row-changed      — same object, different token count or values
      order-changed    — an ordered section's rows differ in sequence only
    """
    ignore = ignore or set()
    A, B = parse_sections(text_a), parse_sections(text_b)
    findings: list[dict] = []

    for sect in sorted(set(A) | set(B)):
        if sect in ignore:
            continue
        ra, rb = A.get(sect), B.get(sect)
        # A section present but empty is not a difference from an absent one:
        # the writer omits empty sections and that is the legacy convention.
        if not ra and not rb:
            continue
        if ra and not rb:
            findings.append({"section": sect, "kind": "section-missing",
                             "detail": f"{len(ra)} row(s) dropped"})
            continue
        if rb and not ra:
            findings.append({"section": sect, "kind": "section-missing",
                             "detail": f"{len(rb)} row(s) added"})
            continue

        if sect in _ORDERED or sect not in _UNORDERED:
            # Positional comparison. Unknown sections default to ordered,
            # which is the conservative choice (it reports more, not less).
            n = max(len(ra), len(rb))
            for i in range(n):
                if i >= len(ra):
                    findings.append({"section": sect, "kind": "row-extra",
                                     "detail": " ".join(rb[i])[:200]})
                elif i >= len(rb):
                    findings.append({"section": sect, "kind": "row-missing",
                                     "detail": " ".join(ra[i])[:200]})
                elif not _rows_equal(ra[i], rb[i], rtol, atol):
                    findings.append({
                        "section": sect, "kind": "row-changed",
                        "detail": f"[{i}] {' '.join(ra[i])[:100]}"
                                  f"  ->  {' '.join(rb[i])[:100]}"})
        else:
            # Keyed comparison on the object name.
            ba: dict[str, list[list[str]]] = {}
            bb: dict[str, list[list[str]]] = {}
            for r in ra:
                ba.setdefault(_key(r), []).append(r)
            for r in rb:
                bb.setdefault(_key(r), []).append(r)
            for k in sorted(set(ba) | set(bb)):
                la, lb = ba.get(k, []), bb.get(k, [])
                for i in range(max(len(la), len(lb))):
                    if i >= len(la):
                        findings.append({"section": sect, "kind": "row-extra",
                                         "detail": " ".join(lb[i])[:200]})
                    elif i >= len(lb):
                        findings.append({"section": sect, "kind": "row-missing",
                                         "detail": " ".join(la[i])[:200]})
                    elif not _rows_equal(la[i], lb[i], rtol, atol):
                        findings.append({
                            "section": sect, "kind": "row-changed",
                            "detail": f"{' '.join(la[i])[:100]}"
                                      f"  ->  {' '.join(lb[i])[:100]}"})
    return findings
