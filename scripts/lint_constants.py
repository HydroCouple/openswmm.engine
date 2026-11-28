#!/usr/bin/env python3
"""Fail when a physical constant or unit factor is written as a literal outside
its one definition.

Constants.hpp and UnitConversion.hpp define every physical constant and fixed
unit factor the engine uses, in two families: the legacy-faithful literals
legacy SWMM computes with (PI 3.141592654, GRAVITY 32.2, FT3_TO_L 28.317, ...)
and the exact values for code with no legacy counterpart (PI_EXACT,
GRAVITY_SI, FT3_TO_M3_EXACT, ...). A literal copy anywhere else is a second
definition, and second definitions drift: before the 2026-11-22
consolidation the engine carried a mistyped sqrt(32.2) and a report volume
factor of 28.3168 beside legacy's 28.317. This check keeps every count at one.

It scans the engine's C and C++ sources -- not the tests, whose analytic
oracles may spell numbers out -- on code lines only: block and line comments
and string literals are blanked first. Every rule names what to use instead.
Fahrenheit's 32 and 1.8 are too common as plain numbers to guard; the parity
test pins those constants instead.

    scripts/lint_constants.py             scan src/engine, src/couplers, include
    scripts/lint_constants.py --root DIR  scan the same subdirectories under DIR

Exit status 1 on any hit, 0 when the tree is clean.
"""
import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SCAN_DIRS = ("src/engine", "src/couplers", "include")
EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp"}

# A number is a hit only as a whole token: not preceded by a digit or a point,
# not followed by a digit.
_B = r"(?<![\d.])"
_E = r"(?![\d])"

# (pattern, what to use instead)
RULES = [
    (_B + r"3\.14159\d*", "constants::PI (legacy paths) or constants::PI_EXACT"),
    (_B + r"32\.2" + _E, "constants::GRAVITY"),
    (_B + r"32\.174\d*", "constants::GRAVITY"),
    (_B + r"9\.80665" + _E, "constants::GRAVITY_SI"),
    (_B + r"9\.81" + _E, "constants::GRAVITY_SI"),
    (_B + r"1\.486" + _E, "constants::PHI"),
    (_B + r"62\.4" + _E, "constants::GAMMA_WATER"),
    (_B + r"273\.15" + _E, "constants::KELVIN_OFFSET"),
    (_B + r"86400(000)?(\.0*)?" + _E, "constants::SEC_PER_DAY, SEC_PER_DAY_INT or MSEC_PER_DAY"),
    (_B + r"8\.64e7" + _E, "constants::MSEC_PER_DAY"),
    (_B + r"0\.3048" + _E, "ucf::FT_TO_M"),
    (_B + r"3\.2808\d*", "1.0 / ucf::FT_TO_M"),
    (_B + r"28\.31[67]\d*", "ucf::FT3_TO_L (legacy) or ucf::FT3_TO_L_EXACT"),
    (_B + r"0\.0283(1\d*|2\d+)", "ucf::FT3_TO_M3 (legacy) or ucf::FT3_TO_M3_EXACT"),
    (_B + r"0\.02832" + _E, "ucf::Qcf[CMS] or ucf::Ucf[VOLUME][SI]"),
    (_B + r"0\.09290304" + _E, "ucf::FT2_TO_M2_EXACT"),
    (_B + r"25\.4" + _E, "ucf::IN_TO_MM"),
    (_B + r"7\.48(052)?" + _E, "ucf::FT3_TO_GAL"),
    (_B + r"43560(\.0*)?" + _E, "ucf::ACRES_TO_FT2"),
    (_B + r"448\.831" + _E, "ucf::Qcf[GPM]"),
    (_B + r"0\.64632" + _E, "ucf::Qcf[MGD]"),
    (_B + r"2\.4466" + _E, "ucf::Qcf[MLD]"),
]
COMPILED = [(re.compile(p), use) for p, use in RULES]

# Files allowed to carry literals: None exempts the whole file, a list exempts
# only the rules whose pattern contains one of the given fragments.
ALLOW = {
    # The definitions themselves.
    "src/engine/core/Constants.hpp": None,
    "src/engine/core/UnitConversion.hpp": None,
    # Fortran port of Tarboton's UEB: its literals are the reference model's.
    "src/engine/hydrology/surface/UebReference.cpp": None,
    # ueb_reference::kGravity, the UEB model's own g, defined once for the
    # port and the native forcing.
    "src/engine/hydrology/surface/UebReference.hpp": ["9\\.81"],
}

_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
_STRING = re.compile(r'"(?:\\.|[^"\\])*"')
_CHAR = re.compile(r"'(?:\\.|[^'\\])'")


def code_lines(text):
    """The file's lines with comments and string literals blanked, line numbers kept."""
    text = _BLOCK_COMMENT.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)
    lines = []
    for line in text.split("\n"):
        line = _STRING.sub('""', line)
        line = _CHAR.sub("''", line)
        lines.append(line.split("//", 1)[0])
    return lines


def allowed(rel, pattern):
    rule = ALLOW.get(rel, False)
    if rule is None:
        return True
    if rule is False:
        return False
    return any(fragment in pattern for fragment in rule)


def scan(root):
    hits = []
    files = 0
    for sub in SCAN_DIRS:
        base = root / sub
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in EXTENSIONS or not path.is_file():
                continue
            files += 1
            rel = path.relative_to(root).as_posix()
            if ALLOW.get(rel, False) is None:
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError as exc:
                hits.append((rel, 0, f"unreadable: {exc}", ""))
                continue
            for number, line in enumerate(code_lines(text), start=1):
                for regex, use in COMPILED:
                    if allowed(rel, regex.pattern):
                        continue
                    for m in regex.finditer(line):
                        hits.append((rel, number, m.group(0), use))
    return files, hits


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=str(REPO), help="repository root to scan (default: this checkout)")
    args = ap.parse_args()
    root = Path(args.root).resolve()
    files, hits = scan(root)
    for rel, number, literal, use in hits:
        print(f"{rel}:{number}: literal {literal} -> use {use}")
    print(f"constants: {files} files scanned, {len(hits)} literal copies")
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main())
