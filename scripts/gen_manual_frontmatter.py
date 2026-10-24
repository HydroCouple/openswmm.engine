#!/usr/bin/env python3
"""Generate the reference manuals' List of Figures and List of Tables from
the captions in their chapters.

    scripts/gen_manual_frontmatter.py --write    rewrite the generated blocks
    scripts/gen_manual_frontmatter.py --check    fail (exit 1) if they are stale

The source of truth is the caption line under each figure or table, written
either as `*Figure 8-1 …*` or `<strong>Figure 8-1 …</strong>`. Chapters are
visited in the order the manual's `@subpage` list names them, so the lists
follow the book. The manifest is deliberately NOT the source: legacy figures
carry captions and no manifest figure number, and a table has no manifest row
at all.

Each list lives between markers, which the first `--write` inserts around the
existing list:

    ## List of Figures
    <!-- BEGIN GENERATED: list-of-figures -->
    ...
    <!-- END GENERATED -->

Each manual keeps the entry style it already uses — `- **Figure 1-1** text`,
`Figure 1-1. text` or `Figure 1-1 text` — detected from the entries found
before the first rewrite and then pinned by the marker comment.

Numbers must be unique within a chapter. Gaps are reported but allowed: a
chapter may cite a figure numbered to match its sibling manuals.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"
MANUALS = ["hydraulics", "hydrology", "quality"]

# A caption is a line of its own, either marked up (**…**, *…*, <strong>…</strong>)
# or — for the legacy chapters that never marked theirs — a bare line directly
# under the image it captions.
CAPTION_RE = re.compile(r"^(?:\*\*|\*|<strong>)(Figure|Table)\s+([0-9A-Z]+[-‑][0-9]+)\s+(.+?)"
                        r"(?:\*\*|\*|</strong>)\s*$")
# the pandoc-converted legacy captions: <strong>…</strong> wrapped over several lines,
# sometimes with a non-breaking hyphen in the number
STRONG_RE = re.compile(r"<strong>\s*(Figure|Table)\s+([0-9A-Z]+[-‑][0-9]+)\s+(.+?)</strong>", re.S)
# *…* and **…** captions, which may wrap over lines but never across a blank one
_BODY = r"((?:[^*\n]|\n(?!\n))+?)"
BOLD_RE = re.compile(r"(?m)^\*\*(Figure|Table)\s+([0-9A-Z]+[-‑][0-9]+)\s+" + _BODY + r"\*\*\s*$")
ITALIC_RE = re.compile(r"(?m)^\*(Figure|Table)\s+([0-9A-Z]+[-‑][0-9]+)\s+" + _BODY + r"\*\s*$")
BARE_CAPTION_RE = re.compile(r"^(Figure|Table)\s+([0-9A-Z]+-[0-9]+)[.:]?\s+(.+?)\.?\s*$")
EMBED_RE = re.compile(r"!\[[^\]]*\]\([^)]+\)|<img[^>]+src=")
# prose that happens to open with a figure number, not a caption
PROSE_RE = re.compile(r"\b(shows|depicts|illustrates|gives|plots|summari[sz]es|lists|is |are |presents|compares)\b")
SUBPAGE_RE = re.compile(r"^-\s*@subpage\s+(\S+)")
PAGE_RE = re.compile(r"^@page\s+(\S+)", re.M)
BEGIN = "<!-- BEGIN GENERATED: list-of-{kind}{style} -->"
BEGIN_ANY = re.compile(r"^<!-- BEGIN GENERATED: list-of-(figures|tables)(?:\s+style=(\w+))? -->$")
END = "<!-- END GENERATED -->"
STYLES = {
    "bullet": lambda k, n, c: f"- **{k} {n}** {c}",
    "period": lambda k, n, c: f"{k} {n}. {c}",
    "plain": lambda k, n, c: f"{k} {n} {c}",
}


def chapters_in_order(manual: str):
    """The manual's section files, in @subpage order, then any not listed."""
    front = DOCS / "manuals" / "reference" / f"{manual}.md"
    sections = sorted((DOCS / "manuals" / "reference" / manual / "sections").glob("*.md"))
    by_page = {}
    for p in sections:
        m = PAGE_RE.search(p.read_text(errors="replace"))
        if m:
            by_page[m.group(1)] = p
    order, seen = [], set()
    for ln in front.read_text(errors="replace").split("\n"):
        m = SUBPAGE_RE.match(ln.strip())
        if m and m.group(1) in by_page:
            order.append(by_page[m.group(1)])
            seen.add(by_page[m.group(1)])
    order += [p for p in sections if p not in seen]
    return front, order


def captions(manual: str):
    """{'Figure': [(no, caption, file)], 'Table': [...]} in document order, plus problems."""
    _, chapters = chapters_in_order(manual)
    out = {"Figure": [], "Table": []}
    problems = []
    for p in chapters:
        rel = p.relative_to(DOCS).as_posix()
        text = p.read_text(errors="replace")
        body = text.split("\n")
        found = []                                  # (offset, kind, no, caption, line)
        for rx in (STRONG_RE, BOLD_RE, ITALIC_RE):
            for m in rx.finditer(text):
                found.append((m.start(), m.group(1), m.group(2), m.group(3),
                              text.count("\n", 0, m.start()) + 1))
        off = 0
        for n, ln in enumerate(body, 1):
            m = CAPTION_RE.match(ln.strip())
            if not m:
                m = BARE_CAPTION_RE.match(ln.strip())
                if not m or PROSE_RE.search(m.group(3)):
                    off += len(ln) + 1
                    continue
                # only a bare line sitting directly under its image is a caption
                prev = [q for q in body[max(0, n - 4):n - 1] if q.strip()]
                if not (prev and EMBED_RE.search(prev[-1])):
                    off += len(ln) + 1
                    continue
            found.append((off, m.group(1), m.group(2), m.group(3), n))
            off += len(ln) + 1
        seen = {"Figure": {}, "Table": {}}
        for _, kind, no, cap, n in sorted(found):
            no = no.replace("‑", "-")
            cap = " ".join(re.sub(r"<[^>]+>", "", cap).split())
            if cap.endswith("(continued)"):
                continue            # a table split across pages, not a second table
            if no in seen[kind]:
                if seen[kind][no] != n:
                    problems.append(f"{rel}:{n}: {kind} {no} is captioned twice "
                                    f"(first at line {seen[kind][no]})")
                continue
            seen[kind][no] = n
            out[kind].append((no, cap, rel))
    return out, problems


ENTRY_RE = re.compile(r"^(?:-\s*\*\*)?(Figure|Table)\s+([0-9A-Z]+-[0-9]+)(?:\*\*)?[.:]?\s+(.+?)\.?\s*$")


def parse_entries(lines, kind):
    """{number: text} for the entries already in a list block, in order."""
    out = {}
    for ln in lines:
        m = ENTRY_RE.match(ln.strip())
        if m and m.group(1) == kind:
            out.setdefault(m.group(2), " ".join(m.group(3).split()))
    return out


def sort_key(no):
    ch, _, num = no.partition("-")
    return (0, int(ch)) if ch.isdigit() else (1, 0), ch, int(num)


def merge(existing, sourced):
    """(ordered [(no, text)], unsourced numbers) — captions win, old entries are never dropped."""
    nums = sorted(set(existing) | set(sourced), key=sort_key)
    return ([(n, sourced[n] if n in sourced else existing[n]) for n in nums],
            [n for n in nums if n not in sourced])


def detect_style(block_lines, marker_style):
    if marker_style in STYLES:
        return marker_style
    for ln in block_lines:
        s = ln.strip()
        if s.startswith("- **"):
            return "bullet"
        if re.match(r"^(Figure|Table)\s+[0-9A-Z]+-[0-9]+\.\s", s):
            return "period"
        if re.match(r"^(Figure|Table)\s+[0-9A-Z]+-[0-9]+\s", s):
            return "plain"
    return "plain"


def section_bounds(lines, heading):
    """(start, end) of the list body under `heading`, exclusive of the heading itself."""
    try:
        i = next(k for k, ln in enumerate(lines) if ln.strip() == heading)
    except StopIteration:
        return None
    j = i + 1
    while j < len(lines) and not (lines[j].startswith("## ") or lines[j].strip() == "---"):
        j += 1
    return i + 1, j


def render(entries, style, kind):
    """One line per entry for the bullet style, which markdown renders as a list;
    a blank line between entries otherwise, or the reader's markdown joins them
    into a single paragraph."""
    fn = STYLES[style]
    out, last_chapter = [], None
    for no, cap in entries:
        chapter = no.split("-")[0]
        if last_chapter is not None and (style != "bullet" or chapter != last_chapter):
            out.append("")
        out.append(fn(kind, no, cap))
        last_chapter = chapter
    return out


def rewrite(manual: str, write: bool, verbose: bool):
    front, _ = chapters_in_order(manual)
    caps, problems = captions(manual)
    text = front.read_text()
    lines = text.split("\n")
    changed = []
    for kind, heading in (("Figure", "## List of Figures"), ("Table", "## List of Tables")):
        key = "figures" if kind == "Figure" else "tables"
        bounds = section_bounds(lines, heading)
        if bounds is None:
            problems.append(f"{front.relative_to(DOCS).as_posix()}: no '{heading}' heading")
            continue
        s, e = bounds
        body = lines[s:e]
        marker_style = None
        inner = body
        for k, ln in enumerate(body):
            m = BEGIN_ANY.match(ln.strip())
            if m:
                marker_style = m.group(2)
                try:
                    stop = next(q for q in range(k + 1, len(body)) if body[q].strip() == END)
                except StopIteration:
                    problems.append(f"{front.relative_to(DOCS).as_posix()}: unterminated {key} block")
                    stop = len(body) - 1
                inner = body[k + 1:stop]
                break
        style = detect_style(inner, marker_style)
        entries, unsourced = merge(parse_entries(inner, kind),
                                   {no: cap for no, cap, _ in caps[kind]})
        want = ([""] + [BEGIN.format(kind=key, style=f" style={style}")] + [""]
                + render(entries, style, kind) + ["", END, ""])
        if body != want:
            changed.append(f"{manual}: {heading[3:]} ({len(entries)} entries, {style} style)")
            if write:
                lines[s:e] = want
        if unsourced and verbose:
            print(f"  {manual}: {len(unsourced)} {kind.lower()} entries kept from the old list with no "
                  f"caption found in a chapter: {', '.join(unsourced[:8])}"
                  + (" …" if len(unsourced) > 8 else ""))
    if write and changed:
        front.write_text("\n".join(lines))
    if verbose:
        for kind in ("Figure", "Table"):
            nums = [no for no, _, _ in caps[kind]]
            print(f"  {manual}: {len(nums)} {kind.lower()} captions")
    return changed, problems


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--write", action="store_true", help="rewrite the generated blocks")
    g.add_argument("--check", action="store_true", help="exit 1 if any block is stale")
    ap.add_argument("--manual", choices=MANUALS, help="only this manual")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    manuals = [args.manual] if args.manual else MANUALS
    stale, problems = [], []
    for m in manuals:
        c, p = rewrite(m, args.write, args.verbose)
        stale += c
        problems += p
    for p in problems:
        print(f"ERROR front matter: {p}")
    for c in stale:
        print(("rewrote " if args.write else "ERROR front matter: stale — ") + c)
    if problems:
        return 1
    if args.check and stale:
        print("front matter: run scripts/gen_manual_frontmatter.py --write")
        return 1
    print(f"front matter: {len(manuals)} manuals, "
          f"{'rewrote ' + str(len(stale)) if args.write else str(len(stale)) + ' stale'}, "
          f"{len(problems)} errors")
    return 0


if __name__ == "__main__":
    sys.exit(main())
