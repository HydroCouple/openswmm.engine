#!/usr/bin/env python3
"""Convert raw $ / $$ math in the manual markdown to Doxygen-native
\\f$ ... \\f$ (inline) and \\f[ ... \\f] (display) delimiters.

Rationale: Doxygen's markdown pass interprets backslash sequences
(\\{, \\}, unknown commands) and emphasis characters inside raw $-math,
which mangles equations and — as observed in the 2026-08-11 build —
can silently truncate an entire page. Content inside \\f...\\f is passed
to MathJax verbatim and is immune to all of that.

Skips fenced code blocks, inline code spans, <pre> blocks, and existing
\\f-delimited math. Reports any unpaired delimiters it cannot convert.
"""
import re
import sys
from pathlib import Path

DOCS = Path(__file__).resolve().parent.parent / "docs" / "manuals"
SKIP_RE = re.compile(r"/media/|/SWMM[^/]*\.md$")


def convert_text(text, report, name):
    # multi-line display blocks first: $$ ... $$ spanning lines
    def ml(m):
        return "\\f[" + m.group(1) + "\\f]"

    text = re.sub(r"\$\$((?:[^$]*\n)+[^$]*?)\$\$", ml, text)
    out_lines = []
    in_fence = False
    in_pre = False
    for lineno, line in enumerate(text.split("\n"), 1):
        stripped = line.lstrip()
        if stripped.startswith("```"):
            in_fence = not in_fence
            out_lines.append(line)
            continue
        if "<pre" in line:
            in_pre = True
        if "</pre>" in line:
            in_pre = False
            out_lines.append(line)
            continue
        if in_fence or in_pre:
            out_lines.append(line)
            continue

        # protect segments that must not be touched: inline code and
        # existing \f formulas
        protected = []

        def stash(m):
            protected.append(m.group(0))
            return f"\x00{len(protected)-1}\x00"

        work = re.sub(r"`[^`]*`", stash, line)
        work = re.sub(r"\\f\$.*?\\f\$", stash, work)
        work = re.sub(r"\\f\[.*?\\f\]", stash, work)

        # display math first: $$...$$ (single line)
        work, n_disp = re.subn(r"\$\$(.+?)\$\$", r"\\f[\1\\f]", work)
        # inline math: $...$ with non-space content boundaries
        work, n_inl = re.subn(r"\$(?=\S)((?:[^$])+?)(?<=\S)\$", r"\\f$\1\\f$", work)


        def unstash(m):
            return protected[int(m.group(1))]

        work = re.sub(r"\x00(\d+)\x00", unstash, work)
        out_lines.append(work)
    text = "\n".join(out_lines)

    # final pass: inline $...$ pairs broken across a line wrap. Protect
    # everything already converted, code, and <pre> blocks, then pair the
    # remaining $ across newlines (unwrapping the break inside the math).
    protected = []

    def stash(m):
        protected.append(m.group(0))
        return f"\x00{len(protected)-1}\x00"

    work = re.sub(r"```.*?```", stash, text, flags=re.S)
    work = re.sub(r"<pre.*?</pre>", stash, work, flags=re.S)
    work = re.sub(r"`[^`\n]*`", stash, work)
    work = re.sub(r"\\f\$.*?\\f\$", stash, work, flags=re.S)
    work = re.sub(r"\\f\[.*?\\f\]", stash, work, flags=re.S)

    def xline(m):
        return "\\f$" + " ".join(m.group(1).split()) + "\\f$"

    work = re.sub(r"\$(?=\S)([^$]{1,400}?)(?<=\S)\$", xline, work, flags=re.S)
    return re.sub(r"\x00(\d+)\x00", lambda m: protected[int(m.group(1))], work)


def main():
    report = []
    changed = 0
    for p in sorted(DOCS.rglob("*.md")):
        if SKIP_RE.search(str(p)):
            continue
        text = p.read_text()
        if "$" not in text:
            continue
        new = convert_text(text, report, str(p.relative_to(DOCS)))
        residue = new.replace("\\f$", "").replace("\\f[", "").replace("\\f]", "")
        residue = re.sub(r"`[^`\n]*`", "", residue)
        for i, ln in enumerate(residue.split("\n"), 1):
            if "$" in ln:
                report.append(f"{p.relative_to(DOCS)}:{i}: unconverted $ remains: {ln.strip()[:100]}")
        if new != text:
            p.write_text(new)
            changed += 1
            n = new.count("\\f$") // 2 + new.count("\\f[")
            print(f"converted {p.relative_to(DOCS)}  ({n} formulas now \\f-delimited)")
    print(f"\n{changed} files changed")
    if report:
        print("\nNEEDS MANUAL REVIEW (unpaired $):")
        for r in report:
            print(" ", r)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
