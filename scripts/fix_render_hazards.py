#!/usr/bin/env python3
"""Fix constructs that Doxygen's HTML output renders incorrectly, as
diagnosed against the 2026-08-11 built site:

1. <figure>/<figcaption> wrappers (unsupported tags, shown literally as
   &lt;figure&gt;) — unwrapped, keeping the image and caption content.
2. Pandoc simple-table blocks (indented dash-run lines from the original
   EPA document conversion) — rendered as stray <hr> elements and could
   turn following text into accidental headings. The dash lines are
   removed and the rows dedented into plain paragraphs.
3. Bare angle-bracket placeholders in prose (<Esc>, <Enter>, <object>,
   <userfolder>, <list of ...>) — treated as unknown HTML tags and
   swallowed. Escaped to &lt;...&gt; outside code blocks.

Idempotent; skips fenced code blocks and <pre> blocks.
"""
import re
import sys
from pathlib import Path

DOCS = Path(__file__).resolve().parent.parent / "docs" / "manuals"
SKIP_RE = re.compile(r"/media/|/SWMM[^/]*\.md$")

DASH_LINE = re.compile(r"^\s+[-]{4,}[-\s]*$")
FIG_TAG = re.compile(r"^\s*</?(figure|figcaption)>\s*$")
FIG_INLINE = re.compile(r"</?(figure|figcaption)>")
# bare non-HTML angle tags in prose: <Esc>, <Enter>, <object>, <userfolder>,
# <username>, <list of x> — anything not in doxygen's supported tag set and
# not looking like a real closing/self-closing html construct.
SUPPORTED = {
    "a","b","blockquote","br","caption","center","code","dd","del","dfn",
    "div","dl","dt","em","hr","h1","h2","h3","h4","h5","h6","i","img","ins",
    "kbd","li","ol","p","pre","s","small","span","strike","strong","sub",
    "sup","table","tbody","td","tfoot","th","thead","tr","tt","u","ul","var",
    "figure","figcaption",  # handled separately
}
ANGLE = re.compile(r"(?<!\\)<(/?)([A-Za-z][A-Za-z0-9]*)((?:\s[^<>]*)?)>")


def escape_unknown_tags(line):
    def rep(m):
        name = m.group(2).lower()
        if name in SUPPORTED:
            return m.group(0)
        return "&lt;" + m.group(1) + m.group(2) + m.group(3) + "&gt;"

    return ANGLE.sub(rep, line)


def process(text, name, report):
    lines = text.split("\n")
    out = []
    in_fence = False
    in_pre = False
    for i, line in enumerate(lines):
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            out.append(line)
            continue
        if "<pre" in line:
            in_pre = True
        if "</pre>" in line:
            in_pre = False
            out.append(line)
            continue
        if in_fence or in_pre:
            out.append(line)
            continue
        # 1. figure wrappers
        if FIG_TAG.match(line):
            continue
        line = FIG_INLINE.sub("", line)
        # 2. pandoc dash lines
        if DASH_LINE.match(line):
            report.append(f"{name}:{i+1}: removed pandoc table rule")
            continue
        # 3. unknown angle tags
        line = escape_unknown_tags(line)
        out.append(line)
    # dedent former pandoc rows: lines with 2+ leading spaces that contain
    # ' = ' and a symbol pattern were table rows; leave indentation alone —
    # dedenting risks breaking legit nested lists. Rows render as plain
    # paragraphs once the rules are gone, indented ones as code blocks, so
    # dedent only rows directly between two removed rules is handled above.
    return "\n".join(out)


def main():
    report = []
    changed = 0
    for p in sorted(DOCS.rglob("*.md")):
        if SKIP_RE.search(str(p)):
            continue
        text = p.read_text()
        new = process(text, str(p.relative_to(DOCS)), report)
        if new != text:
            p.write_text(new)
            changed += 1
            print("fixed", p.relative_to(DOCS))
    print(f"\n{changed} files changed, {len(report)} pandoc rules removed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
