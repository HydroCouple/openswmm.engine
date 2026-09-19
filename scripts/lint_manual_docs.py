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
import xml.etree.ElementTree as ET
from collections import defaultdict
from pathlib import Path

DOCS = Path(__file__).resolve().parent.parent / "docs"
MANUAL_DIRS = [DOCS / "manuals", DOCS / "authors.md"]
IMAGE_PATHS = [
    DOCS / "images",
    DOCS / "manuals" / "engine" / "figures",
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


def external_pages():
    """Page ids imported from the Doxyfile's TAGFILES.

    `@ref manual_*` into the SWMMVis manual resolves through
    docs/tags/swmmvis-manual.tag at build time, so those targets are valid even
    though no local file declares them. Without this the lint reports every
    cross-repo reference as unresolved.
    """
    ids = set()
    for tag in sorted((DOCS / "tags").glob("*.tag")):
        try:
            root = ET.parse(tag).getroot()
        except ET.ParseError as exc:
            print(f"ERROR {tag.name}: not parseable as a Doxygen tagfile ({exc})")
            continue
        ids.update(c.findtext("name") for c in root if c.get("kind") == "page")
    return {i for i in ids if i}


# Delphi-interface vocabulary. The engine manual documents an engine; prose
# that tells the reader to click something is a sign that text was carried over
# from the retired user manual without being rewritten. Case-sensitive on
# purpose: "Study Area Map" is the old UI's proper noun, "study area map" in a
# figure caption is just English.
GUI_PROSE = re.compile(
    r"Main Menu|Main Toolbar|Project Browser|Property Editor|Map Browser"
    r"|Windows Clipboard|Study Area Map|epaswmm5\.exe"
    r"|click the .*button|right-click")


def check_gui_prose():
    engine_dir = DOCS / "manuals" / "engine"
    if not engine_dir.is_dir():
        return 0
    errors = 0
    for f in sorted(engine_dir.rglob("*.md")):
        for n, line in enumerate(f.read_text(errors="replace").splitlines(), 1):
            # A table row that names a retired chapter and points at what
            # replaced it is a record of the migration, not prose left behind.
            if line.lstrip().startswith("|") and ("@ref manual_" in line
                                                  or "@ref tutorial_" in line
                                                  or "SWMMVis manual" in line):
                continue
            if GUI_PROSE.search(line):
                print(f"ERROR {f.relative_to(DOCS)}:{n}: retired-interface prose "
                      f"in the engine manual")
                errors += 1
    return errors


def check_message_catalogue():
    """Appendix A must document every code the engine can emit.

    Source of truth is src/engine/core/ErrorCodes.hpp. The appendix splits
    WARNING 10 into 10a/10b (two distinct conditions share one code), so the
    warning numbers are matched on their leading digits.
    """
    hdr = DOCS.parent / "src" / "engine" / "core" / "ErrorCodes.hpp"
    apx = DOCS / "manuals" / "engine" / "sections" / "AppendixA-Messages.md"
    if not hdr.exists() or not apx.exists():
        return 0
    src, text = hdr.read_text(errors="replace"), apx.read_text(errors="replace")

    def enum(name):
        m = re.search(rf"enum\s+{name}\s*:\s*int\s*\{{(.*?)^\}};", src, re.S | re.M)
        if not m:
            return None
        return {int(v) for v in re.findall(r"^\s*[A-Z][A-Z0-9_]*\s*=\s*(\d+)", m.group(1), re.M)} - {0}

    errors = 0
    for enum_name, prefix in (("ErrorCode", "ERROR"), ("WarnCode", "WARNING")):
        want = enum(enum_name)
        if want is None:
            print(f"ERROR messages: could not find enum {enum_name}")
            return 1
        have = {int(x) for x in re.findall(rf"^{prefix} 0*(\d+)[a-z]?:", text, re.M)}
        for c in sorted(want - have):
            print(f"ERROR messages: {prefix} {c} is in ErrorCodes.hpp but not in Appendix A")
            errors += 1
        for c in sorted(have - want):
            print(f"ERROR messages: Appendix A documents {prefix} {c}, "
                  f"which ErrorCodes.hpp does not define")
            errors += 1
    return errors


def check_figure_manifest():
    """Every figure is listed, present, and actually cited by its page.

    The retired manual accumulated 307 images that no page referenced. The
    third assertion is what stops that recurring: a figure nobody wires in
    fails here instead of sitting unnoticed.
    """
    figs = DOCS / "manuals" / "engine" / "figures"
    manifest = figs / "MANIFEST.tsv"
    if not figs.is_dir():
        return 0
    if not manifest.exists():
        print("ERROR figures: MANIFEST.tsv is missing")
        return 1

    rows = []
    for ln in manifest.read_text(encoding="utf-8").splitlines():
        if not ln.strip() or ln.startswith("#"):
            continue
        f = ln.split("\t")
        if len(f) < 5:
            print(f"ERROR figures: malformed manifest row: {ln[:60]}")
            return 1
        rows.append({"new": f[1].strip(), "page": f[2].strip()})

    pages = {}
    for md in (DOCS / "manuals" / "engine").rglob("*.md"):
        for line in md.read_text(errors="replace").splitlines():
            m = re.match(r"^@page\s+(\S+)", line)
            if m:
                pages[m.group(1)] = md

    errors = 0
    listed = {r["new"] for r in rows}
    on_disk = {q.name for q in figs.iterdir() if q.suffix.lower() in (".png", ".jpg", ".gif")}
    for extra in sorted(on_disk - listed):
        print(f"ERROR figures: {extra} is on disk but not in MANIFEST.tsv")
        errors += 1
    for r in rows:
        if r["new"] not in on_disk:
            print(f"ERROR figures: {r['new']} is in MANIFEST.tsv but not on disk")
            errors += 1
            continue
        page = pages.get(r["page"])
        if page is None:
            print(f"ERROR figures: {r['new']} names page {r['page']}, which does not exist")
            errors += 1
            continue
        if r["new"] not in page.read_text(errors="replace"):
            print(f"ERROR figures: {r['new']} is never cited by {r['page']}")
            errors += 1
    return errors


def check_section_coverage():
    """Chapter 2 must document every [SECTION] the parser registers.

    The allowlist (KNOWN_SECTION_GAPS.txt) holds the 2D_*/GW_* backlog, so this
    fails when someone registers a NEW section without documenting it — the
    regression worth catching — rather than demanding the backlog be written
    before anything else can land. Shrink the allowlist, never grow it.
    """
    src = DOCS.parent / "src" / "engine"
    chapter = DOCS / "manuals" / "engine" / "sections" / "Chapter2-InputFileReference.md"
    gaps_file = chapter.parent / "KNOWN_SECTION_GAPS.txt"
    if not src.is_dir() or not chapter.exists():
        return 0

    registered = set()
    for f in src.rglob("*.cpp"):
        text = f.read_text(errors="replace")
        registered |= set(re.findall(r'register_builtin\(\s*"([A-Z0-9_]+)"', text))
        registered |= set(re.findall(r'register_custom\(\s*"([A-Z0-9_]+)"', text))
    if not registered:
        print("ERROR section coverage: parsed no registered sections — "
              "the registration call's shape changed")
        return 1

    documented = set(re.findall(r"^### Section: \[([A-Z0-9_]+)\]",
                                chapter.read_text(errors="replace"), re.M))
    allowed = set()
    if gaps_file.exists():
        allowed = {ln.strip() for ln in gaps_file.read_text().splitlines()
                   if ln.strip() and not ln.startswith("#")}

    errors = 0
    for s in sorted(documented - registered):
        print(f"ERROR section coverage: Chapter 2 documents [{s}], "
              f"which no handler registers")
        errors += 1
    for s in sorted(registered - documented - allowed):
        print(f"ERROR section coverage: [{s}] is registered but undocumented "
              f"and not in KNOWN_SECTION_GAPS.txt")
        errors += 1
    for s in sorted(allowed & documented):
        print(f"ERROR section coverage: [{s}] is documented — remove it from "
              f"KNOWN_SECTION_GAPS.txt")
        errors += 1
    return errors


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

    known = set(pages) | anchors | external_pages()
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

    errors += check_section_coverage()
    errors += check_gui_prose()
    errors += check_figure_manifest()
    errors += check_message_catalogue()

    print(f"\n{len(files)} files, {len(pages)} pages, {len(anchors)} anchors, {errors} errors")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
