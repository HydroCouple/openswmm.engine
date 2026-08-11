#!/usr/bin/env python3
"""Lint the Doxygen manual markdown sources for referential integrity and
rendering hazards. Doxygen itself is the final arbiter; this catches the
common failures without a doxygen install.

Checks:
  * duplicate @page ids
  * @subpage / @ref targets that resolve to no known @page, {#anchor}, or
    \\anchor in the manual tree (code-entity refs are skipped: they contain
    '::' or match a known source symbol pattern)
  * markdown tables whose rows have inconsistent column counts
  * unbalanced $$ display-math delimiters
  * image references whose basename exists in no IMAGE_PATH directory

Exit code 1 if any errors. Run from anywhere.
"""
import re
import sys
from collections import defaultdict
from pathlib import Path

DOCS = Path(__file__).resolve().parent.parent / "docs"
MANUAL_DIRS = [DOCS / "manuals", DOCS / "authors.md", DOCS / "Updates.md"]
IMAGE_PATHS = [
    DOCS / "images",
    DOCS / "manuals" / "user" / "figures",
    DOCS / "manuals" / "reference" / "hydrology" / "media" / "media",
    DOCS / "manuals" / "reference" / "hydraulics" / "media" / "media",
    DOCS / "manuals" / "reference" / "quality" / "media" / "media",
]
EXCLUDE_RE = re.compile(r"/SWMM[^/]*\.md$|/media/")

CODE_REF = re.compile(r"::|\.h$|^[a-z]+_[a-zA-Z]+$")  # C API / namespaced / header files


def md_files():
    out = []
    for root in MANUAL_DIRS:
        if root.is_file():
            out.append(root)
        else:
            out += [p for p in root.rglob("*.md") if not EXCLUDE_RE.search(str(p))]
    return sorted(out)


def main():
    files = md_files()
    pages, anchors = {}, set()
    texts = {}
    for f in files:
        t = f.read_text(errors="replace")
        texts[f] = t
        for m in re.finditer(r"^@page +(\S+)", t, re.M):
            if m.group(1) in pages:
                print(f"ERROR duplicate @page {m.group(1)}: {f.name} and {pages[m.group(1)].name}")
            pages[m.group(1)] = f
        anchors.update(re.findall(r"\{#([A-Za-z_0-9]+)\}", t))
        anchors.update(re.findall(r"[\\@]anchor +(\S+)", t))

    known = set(pages) | anchors
    errors = 0
    image_names = set()
    for d in IMAGE_PATHS:
        if d.is_dir():
            image_names.update(p.name for p in d.iterdir())

    for f in files:
        t = texts[f]
        rel = f.relative_to(DOCS)
        # refs
        for m in re.finditer(r"[\\@](?:subpage|ref) +([A-Za-z_0-9:~.]+)", t):
            tgt = m.group(1).rstrip(".,")
            if tgt in known or CODE_REF.search(tgt):
                continue
            print(f"ERROR {rel}: unresolved @ref/@subpage '{tgt}'")
            errors += 1
        # tables
        lines = t.split("\n")
        i = 0
        while i < len(lines):
            if re.match(r"^\s*\|.*\|\s*$", lines[i]) and i + 1 < len(lines) and re.match(
                r"^\s*\|[\s:|-]+\|\s*$", lines[i + 1]
            ):
                ncols = lines[i].strip().strip("|").count("|") + 1
                j = i + 2
                while j < len(lines) and re.match(r"^\s*\|.*\|\s*$", lines[j]):
                    nc = lines[j].strip().strip("|").count("|") + 1
                    if nc != ncols:
                        print(
                            f"ERROR {rel}:{j+1}: table row has {nc} cols, header has {ncols}"
                        )
                        errors += 1
                    j += 1
                i = j
            else:
                i += 1
        # math delimiters: everything must be \f-delimited (raw $ math is
        # mangled by doxygen's markdown pass — see convert_math_delimiters.py)
        stripped = re.sub(r"```.*?```", "", t, flags=re.S)
        stripped = re.sub(r"<pre.*?</pre>", "", stripped, flags=re.S)
        code_free = re.sub(r"`[^`\n]*`", "", stripped)
        residue = code_free.replace("\\f$", "").replace("\\f[", "").replace("\\f]", "")
        if "$" in residue:
            for j, ln in enumerate(t.split("\n"), 1):
                if "$" in re.sub(r"`[^`\n]*`", "", ln).replace("\\f$", "").replace("\\f[", "").replace("\\f]", ""):
                    print(f"ERROR {rel}:{j}: raw $ math (use \\f$ / \\f[ delimiters)")
                    errors += 1
                    break
        if code_free.count("\\f[") != code_free.count("\\f]"):
            print(f"ERROR {rel}: unbalanced \\f[ / \\f] delimiters")
            errors += 1
        if code_free.count("\\f$") % 2:
            print(f"ERROR {rel}: odd number of \\f$ delimiters")
            errors += 1
        # constructs doxygen renders literally or mangles
        if re.search(r"</?fig(ure|caption)>", code_free):
            print(f"ERROR {rel}: <figure>/<figcaption> tags (unsupported by doxygen)")
            errors += 1
        for j, ln in enumerate(t.split("\n"), 1):
            if re.match(r"^\s+[-]{4,}[-\s]*$", ln):
                print(f"ERROR {rel}:{j}: indented dash rule (pandoc table artifact)")
                errors += 1
        supported = {
            "a","b","blockquote","br","caption","center","code","dd","del",
            "dfn","div","dl","dt","em","hr","h1","h2","h3","h4","h5","h6","i",
            "img","ins","kbd","li","ol","p","pre","s","small","span","strike",
            "strong","sub","sup","table","tbody","td","tfoot","th","thead",
            "tr","tt","u","ul","var",
        }
        for m in re.finditer(r"(?<!\\)<(/?)([A-Za-z][A-Za-z0-9]*)(?:\s[^<>]*)?>", code_free):
            if m.group(2).lower() not in supported:
                print(f"ERROR {rel}: unknown HTML tag <{m.group(1)}{m.group(2)}> (escape as &lt;...&gt;)")
                errors += 1
        # images
        for m in re.finditer(r"!\[[^\]]*\]\(([^) ]+)", t):
            name = Path(m.group(1)).name
            if name not in image_names:
                print(f"ERROR {rel}: image not found in IMAGE_PATH: {m.group(1)}")
                errors += 1
        for m in re.finditer(r'<img[^>]+src="([^"]+)"', t):
            name = Path(m.group(1)).name
            if name not in image_names:
                print(f"ERROR {rel}: <img> not found in IMAGE_PATH: {m.group(1)}")
                errors += 1

    print(f"\n{len(files)} files, {len(pages)} pages, {len(anchors)} anchors, {errors} errors")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
