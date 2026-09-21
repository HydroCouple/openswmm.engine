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
  * raw $ math, unbalanced \\f[ \\f] and odd \\f$ counts
  * <figure>/<figcaption>, unknown HTML tags, indented dash rules
  * image references whose basename exists in no IMAGE_PATH directory
  * [SECTION] coverage of Chapter 2 against the parser's registrations
  * the error/warning catalogue against ErrorCodes.hpp
  * the engine figure manifest (listed, present, cited)
  * retired-interface prose in the engine manual
  * \\status{...} badges: alias present, vocabulary from docs/figures/status.py,
    every Status-column table cell is exactly one badge, CSS colours in sync
  * Mermaid blocks: labelled `<!-- workflow: id -->`, closed, unique ids, no
    piped node labels and no Doxygen command characters
  * Application Manual deck citations and \\snippet markers resolve

Exit code 1 if any errors. Run from anywhere. `--docs-root` points the lint
at a copy of docs/ (the negative-test runner mutates copies, never the tree);
`--src-root` stays on the real source tree unless overridden.
"""
import argparse
import importlib.util
import re
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"
SRC = ROOT / "src"
MANUAL_DIRS = []
IMAGE_PATHS = []
EXCLUDE_RE = re.compile(r"/SWMM[^/]*\.md$|/media/")

CODE_REF = re.compile(r"::|\.h$|^[a-z]+_[a-zA-Z]+$")  # C API / namespaced / header files


def configure(docs_root, src_root):
    """Bind the module paths. Called once from main(); tests call it directly."""
    global DOCS, SRC, MANUAL_DIRS, IMAGE_PATHS
    DOCS = Path(docs_root).resolve()
    SRC = Path(src_root).resolve()
    MANUAL_DIRS = [DOCS / "manuals", DOCS / "authors.md"]
    IMAGE_PATHS = [
        DOCS / "images",
        DOCS / "figures" / "png",
        DOCS / "manuals" / "engine" / "figures",
        DOCS / "manuals" / "reference" / "hydrology" / "media" / "media",
        DOCS / "manuals" / "reference" / "hydraulics" / "media" / "media",
        DOCS / "manuals" / "reference" / "quality" / "media" / "media",
    ]


configure(DOCS, SRC)


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
    hdr = SRC / "engine" / "core" / "ErrorCodes.hpp"
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
    """Every image any manual embeds has a manifest row, and vice versa.

    The audit itself lives in scripts/build_manual_figures.py (one
    implementation for the lint and for the figure pipeline's `check`); it
    covers the retired manual's failure mode — 307 images no page referenced —
    and the reference manuals' basename collisions that let Doxygen serve one
    manual's figure on another manual's page.
    """
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from build_manual_figures import check_manifest
    return check_manifest(DOCS)


def check_front_matter():
    """Each reference manual's List of Figures and List of Tables matches its captions.

    The lists are generated from the caption lines by
    scripts/gen_manual_frontmatter.py; this runs its --check. The manuals'
    invented entries (the quality manual listed "SWMM's Object Model" for a
    figure captioned "Elements of a typical urban drainage system") are what
    the generator exists to prevent.
    """
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import io
    import contextlib
    import gen_manual_frontmatter as gm
    if gm.DOCS != DOCS:                      # --docs-root: point the generator at the copy
        gm.DOCS = DOCS
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = gm.main(["--check"])
    out = buf.getvalue().strip().split("\n")
    if rc == 0:
        print(out[-1])
        return 0
    for ln in out:
        if ln.startswith("ERROR") or ln.startswith("front matter:"):
            print(ln)
    return sum(1 for ln in out if ln.startswith("ERROR"))


def check_section_coverage():
    """Chapter 2 must document every [SECTION] the parser registers.

    The allowlist (KNOWN_SECTION_GAPS.txt) holds the 2D_*/GW_* backlog, so this
    fails when someone registers a NEW section without documenting it — the
    regression worth catching — rather than demanding the backlog be written
    before anything else can land. Shrink the allowlist, never grow it.
    """
    src = SRC / "engine"
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


def _function_body(text, start_re):
    """Source text from the first match of start_re to the next lone `}` at column 0."""
    m = re.search(start_re, text, re.M)
    if not m:
        return None
    end = re.search(r"^}\s*$", text[m.end():], re.M)
    return text[m.start(): m.end() + (end.end() if end else 0)]


def _documented_keys(chapter_text, section):
    """Keywords a `### Section: [X]` entry documents: first-column backticked
    keys of its tables plus the leading token of each line in its fenced
    Format blocks."""
    m = re.search(r"^### Section: \[" + re.escape(section.strip("[]")) + r"\].*?(?=^### Section: |\Z)",
                  chapter_text, re.M | re.S)
    if not m:
        return None
    region = m.group(0)
    keys = set(re.findall(r"^\|\s*`?([A-Z][A-Z0-9_]*)`?\s*\|", region, re.M))
    # Format blocks: fenced, or the legacy plain column-0 `KEYWORD   value` layout
    for block in re.findall(r"```.*?```", region, re.S):
        keys |= set(re.findall(r"^([A-Z][A-Z0-9_]{2,})\s", block, re.M))
    keys |= set(re.findall(r"^([A-Z][A-Z0-9_]{2,})\s{2,}\S", region, re.M))
    return keys


# (source under src/, regex locating the function, regex for the key literal inside it, section)
OPTIONS_COVERAGE = [
    ("engine/input/handlers/OptionsHandler.cpp", r"^void handle_options\(",
     r'key\s*==\s*"([A-Z][A-Z0-9_]*)"', "[OPTIONS]"),
    ("engine/2d/input/SectionHandlers2D.cpp", r"^bool is2DOptionKey\(",
     r'"([A-Z][A-Z0-9_]*)"', "[2D_OPTIONS]"),
    ("engine/2d/input/SectionHandlers2D.cpp", r"^bool is2DRetiredOptionKey\(",
     r'"([A-Z][A-Z0-9_]*)"', "[2D_OPTIONS]"),
    # the file parser itself: it accepts DISPERSION, which the API allow-list omits
    ("engine/2d/input/SectionHandlers2D.cpp", r"parse2DOptionsLine\(",
     r'iequals\(key,\s*"([A-Z][A-Z0-9_]*)"\)', "[2D_OPTIONS]"),
]


def check_options_coverage():
    """The [OPTIONS] and [2D_OPTIONS] entries document every keyword the parser accepts.

    Parsed keys are the string literals compared inside handle_options() and
    the two 2D allow-lists; documented keys are the backticked first column
    of the entry's tables and the leading token of its Format block. Fails in
    both directions, so an invented keyword and a forgotten one are the same
    error. Retired keys are parsed too and belong in the entry's Retired table.
    """
    chapter = DOCS / "manuals" / "engine" / "sections" / "Chapter2-InputFileReference.md"
    if not chapter.exists() or not SRC.is_dir():
        return 0
    text = chapter.read_text(errors="replace")
    parsed = defaultdict(set)
    for rel, start_re, key_re, section in OPTIONS_COVERAGE:
        src = SRC / rel
        if not src.exists():
            print(f"ERROR options coverage: {rel} not found")
            return 1
        body = _function_body(src.read_text(errors="replace"), start_re)
        if body is None:
            print(f"ERROR options coverage: {start_re!r} not found in {rel}")
            return 1
        parsed[section] |= set(re.findall(key_re, body))
    errors = 0
    for section, keys in parsed.items():
        if not keys:
            print(f"ERROR options coverage: parsed no keys for {section} — the parser's shape changed")
            errors += 1
            continue
        documented = _documented_keys(text, section)
        if documented is None:
            print(f"ERROR options coverage: Chapter 2 has no `### Section: {section}` entry")
            errors += 1
            continue
        for k in sorted(keys - documented):
            print(f"ERROR options coverage: {section} keyword {k} is parsed but not documented")
            errors += 1
        for k in sorted(documented - keys):
            print(f"ERROR options coverage: {section} documents {k}, which the parser does not accept")
            errors += 1
    return errors


# (source under src/, regex locating the function or None for whole file, regex for the section literal)
COMPONENT_SOURCES = [
    ("engine/transport/components/ReactionModule/ReactionsComponent.cpp",
     r"reactionSectionTags\(\)\s*\{", r'"(REACTION_[A-Z_]+)"'),
    ("engine/transport/components/HeatModule/HeatComponent.cpp", None, r'sec\.first\s*[!=]=\s*"([A-Z_]+)"'),
    ("engine/transport/components/WaterAgeModule/WaterAgeComponent.cpp", None, r'sec\.first\s*[!=]=\s*"([A-Z_]+)"'),
    ("engine/transport/components/EulerianArdComponent/ArdConfig.cpp", None, r'tag\s*==\s*"([A-Z_]+)"'),
]
COMPONENT_MENTION = re.compile(r"\[((?:REACTION|HEAT|TRANSPORT|WATER_AGE)_[A-Z_]+"
                               r"|RADIATIVE_FLUXES|SOLAR_RADIATION|CLOUD_COVER|SEDIMENT_EXCHANGE)\]")


def check_component_sections():
    """Chapter 2 §2.5 documents every configuration section a process component parses.

    These sections never pass through the SectionRegistry (they come from
    the file named in [PROCESS_COMPONENTS]), so check_section_coverage()
    cannot see them. Parsed names are the literals each component compares
    against; documented names use the distinct heading
    `### Component section: [X]`. A second assertion: any bracketed
    component-section name mentioned anywhere in the manuals must exist —
    which is how a stale `[REACTION_INITIAL]` gets caught.
    """
    chapter = DOCS / "manuals" / "engine" / "sections" / "Chapter2-InputFileReference.md"
    if not chapter.exists() or not SRC.is_dir():
        return 0
    parsed = set()
    for rel, start_re, lit_re in COMPONENT_SOURCES:
        src = SRC / rel
        if not src.exists():
            print(f"ERROR component sections: {rel} not found")
            return 1
        text = src.read_text(errors="replace")
        body = _function_body(text, start_re) if start_re else text
        if body is None:
            print(f"ERROR component sections: {start_re!r} not found in {rel}")
            return 1
        parsed |= set(re.findall(lit_re, body))
    if not parsed:
        print("ERROR component sections: parsed no section literals — the components' shape changed")
        return 1
    documented = set(re.findall(r"^### Component section: \[([A-Z0-9_]+)\]",
                                chapter.read_text(errors="replace"), re.M))
    errors = 0
    for s in sorted(parsed - documented):
        print(f"ERROR component sections: [{s}] is parsed by a component but has no "
              f"`### Component section:` entry in Chapter 2")
        errors += 1
    for s in sorted(documented - parsed):
        print(f"ERROR component sections: Chapter 2 documents [{s}], which no component parses")
        errors += 1
    for f in md_files():
        for n, ln in enumerate(f.read_text(errors="replace").split("\n"), 1):
            for m in COMPONENT_MENTION.finditer(ln):
                if m.group(1) not in parsed:
                    print(f"ERROR {f.relative_to(DOCS)}:{n}: [{m.group(1)}] is not a section any "
                          f"component parses")
                    errors += 1
    return errors


def load_status_module():
    """docs/figures/status.py — the badge vocabulary and colours. None if absent."""
    path = DOCS / "figures" / "status.py"
    if not path.exists():
        return None
    spec = importlib.util.spec_from_file_location("manual_status", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


BADGE_RE = re.compile(r"[\\@]status\{([^}]*)\}")


def table_rows(lines, i):
    """If lines[i] starts a markdown table, return (header_cells, [(lineno, cells)...])."""
    if not (re.match(r"^\s*\|.*\|\s*$", lines[i]) and i + 1 < len(lines)
            and re.match(r"^\s*\|[\s:|-]+\|\s*$", lines[i + 1])):
        return None
    header = [c.strip() for c in lines[i].strip().strip("|").split("|")]
    body = []
    j = i + 2
    while j < len(lines) and re.match(r"^\s*\|.*\|\s*$", lines[j]):
        body.append((j + 1, [c.strip() for c in lines[j].strip().strip("|").split("|")]))
        j += 1
    return header, body


def check_status_badges(status_report=False):
    """\\status{...} is the one way a manual states a formulation's status.

    Three things must agree: the Doxyfile alias (else the badge renders as
    literal text), the vocabulary in docs/figures/status.py (the same dict the
    figure generators colour their chips from), and the CSS colours. Any table
    whose header has a cell reading exactly `Status` is an alternatives table:
    every body row must carry exactly one badge in that column.
    """
    errors = 0
    doxyfile = DOCS / "Doxyfile"
    if not doxyfile.exists() or '"status{1}=' not in doxyfile.read_text(errors="replace"):
        print("ERROR status: docs/Doxyfile has no status{1} alias — badges would render as text")
        errors += 1
    mod = load_status_module()
    if mod is None:
        print("ERROR status: docs/figures/status.py is missing")
        return errors + 1
    for e in mod.self_check():
        print(f"ERROR {e}")
        errors += 1
    vocab = mod.labels()
    css = DOCS / "custom" / "css" / "manual.css"
    css_text = css.read_text(errors="replace") if css.exists() else ""
    for lab, hexval in mod.STATUS.values():
        if hexval not in css_text:
            print(f"ERROR status: {css.name} lacks the {lab} colour {hexval} from status.py")
            errors += 1

    badge_cell = re.compile(r"^[\\@]status\{(" + "|".join(sorted(vocab)) + r")\}$")
    report = []
    for f in md_files():
        rel = f.relative_to(DOCS)
        lines = f.read_text(errors="replace").split("\n")
        for n, ln in enumerate(lines, 1):
            for m in BADGE_RE.finditer(ln):
                if m.group(1) not in vocab:
                    print(f"ERROR {rel}:{n}: \\status{{{m.group(1)}}} is not one of "
                          f"{', '.join(sorted(vocab))}")
                    errors += 1
                else:
                    report.append((str(rel), n, m.group(1)))
        i = 0
        while i < len(lines):
            t = table_rows(lines, i)
            if t is None:
                i += 1
                continue
            header, body = t
            if "Status" in header:
                col = header.index("Status")
                for n, cells in body:
                    cell = cells[col] if col < len(cells) else ""
                    if not badge_cell.match(cell):
                        print(f"ERROR {rel}:{n}: Status column must hold exactly one "
                              f"\\status{{...}} badge, found {cell[:40]!r}")
                        errors += 1
            i += 2 + len(body)
    if status_report:
        print("\nSTATUS REPORT (file:line badge)")
        for rel, n, lab in report:
            print(f"  {rel}:{n} {lab}")
        print(f"  {len(report)} badges")
    return errors


MERMAID_ID_RE = re.compile(r"^<!-- workflow: ([a-z0-9_]+) -->$")
MERMAID_LABEL_RE = re.compile(r"\[([^\]]*)\]|\{([^}]*)\}")
WORKFLOW_LINKS_RE = re.compile(r'^<div class="workflow-links" data-workflow="([a-z0-9_]+)">\s*$')
WORKFLOW_NODE_RE = re.compile(r'^<span data-node="([A-Za-z0-9_]+)">@ref [A-Za-z0-9_]+ "[^"]+"</span>\s*$')


def _check_workflow_links(rel, lines, k, wid, body, consumed):
    """A `<div class="workflow-links">` block right after a mermaid block: well formed, same id,
    every node a node of that flowchart. Returns the error count."""
    errors = 0
    consumed.add((rel, k))
    m = WORKFLOW_LINKS_RE.match(lines[k])
    if not m:
        print(f"ERROR {rel}:{k+1}: malformed workflow-links opener (expected "
              f'<div class="workflow-links" data-workflow="id">)')
        return 1
    if wid and m.group(1) != wid:
        print(f"ERROR {rel}:{k+1}: workflow-links names workflow {m.group(1)!r} but follows the block {wid!r}")
        errors += 1
    first = next((ln.strip() for ln in body.split("\n") if ln.strip()), "")
    if not (first.startswith("graph") or first.startswith("flowchart")):
        print(f"ERROR {rel}:{k+1}: workflow-links on a non-flowchart diagram ({first[:20]!r}); "
              f"Mermaid supports click on flowcharts only")
        errors += 1
    e, n_nodes = k + 1, 0
    while e < len(lines) and lines[e].strip() != "</div>":
        nm = WORKFLOW_NODE_RE.match(lines[e])
        if not nm:
            print(f"ERROR {rel}:{e+1}: workflow-links line is not "
                  f'<span data-node="X">@ref target "label"</span>')
            errors += 1
        else:
            node = nm.group(1)
            if not re.search(rf"(?<![A-Za-z0-9_]){re.escape(node)}(?![A-Za-z0-9_])", body):
                print(f"ERROR {rel}:{e+1}: workflow-links node {node!r} is not a node of workflow {wid}")
                errors += 1
            n_nodes += 1
        e += 1
    if e >= len(lines):
        print(f"ERROR {rel}:{k+1}: workflow-links block is not closed by </div>")
        errors += 1
    elif n_nodes == 0:
        print(f"ERROR {rel}:{k+1}: workflow-links block holds no link")
        errors += 1
    return errors


def check_mermaid_workflows():
    """Every `<pre class="mermaid">` block is labelled, closed and safe.

    Doxygen expands `@` and `\\` commands inside the block; a pipe inside a
    node label is Mermaid's edge-label syntax and breaks the diagram. The
    `<!-- workflow: id -->` line before the block names the diagram so a
    figure manifest and the prose can refer to it.
    """
    errors = 0
    seen = {}
    consumed = set()
    link_divs = []
    for f in md_files():
        rel = f.relative_to(DOCS)
        lines = f.read_text(errors="replace").split("\n")
        for i, ln in enumerate(lines):
            if ln.startswith('<div class="workflow-links"'):
                link_divs.append((rel, i))
            if ln.strip() != '<pre class="mermaid">':
                continue
            k = i - 1
            while k >= 0 and not lines[k].strip():
                k -= 1
            m = MERMAID_ID_RE.match(lines[k].strip()) if k >= 0 else None
            if not m:
                print(f"ERROR {rel}:{i+1}: mermaid block lacks a `<!-- workflow: id -->` "
                      f"line immediately above it")
                errors += 1
            else:
                wid = m.group(1)
                if wid in seen:
                    print(f"ERROR {rel}:{i+1}: workflow id {wid!r} already used in {seen[wid]}")
                    errors += 1
                seen[wid] = f"{rel}:{i+1}"
            j = i + 1
            while j < len(lines) and lines[j].strip() != "</pre>":
                if lines[j].strip().startswith("<pre"):
                    break
                j += 1
            if j >= len(lines) or lines[j].strip() != "</pre>":
                print(f"ERROR {rel}:{i+1}: mermaid block is not closed by </pre>")
                errors += 1
                continue
            for n in range(i + 1, j):
                body = lines[n]
                if "@" in body or "\\" in body:
                    print(f"ERROR {rel}:{n+1}: mermaid line contains @ or \\ "
                          f"(Doxygen command characters)")
                    errors += 1
                for lm in MERMAID_LABEL_RE.finditer(body):
                    label = lm.group(1) if lm.group(1) is not None else lm.group(2)
                    if "|" in label:
                        print(f"ERROR {rel}:{n+1}: mermaid node label contains '|' "
                              f"(edge-label syntax): {label[:40]!r}")
                        errors += 1
            # an optional click-target block must follow the diagram directly
            k = j + 1
            while k < len(lines) and not lines[k].strip():
                k += 1
            if k < len(lines) and lines[k].startswith('<div class="workflow-links"'):
                errors += _check_workflow_links(rel, lines, k, m.group(1) if m else None,
                                                "\n".join(lines[i + 1:j]), consumed)
    for rel, k in link_divs:
        if (rel, k) not in consumed:
            print(f"ERROR {rel}:{k+1}: workflow-links block does not directly follow a mermaid block")
            errors += 1
    return errors


DECK_RE = re.compile(r"docs/figures/decks/([A-Za-z0-9_./-]+\.(?:inp|rxn|ard|age|heat|py|csv))")
SNIPPET_RE = re.compile(r"[\\@]snippet\s+(\S+)\s+(\S+)")


def check_application_decks():
    """Every deck the Application Manual cites exists, and every \\snippet resolves.

    Decks live under docs/figures/decks/ (the Doxyfile's EXAMPLE_PATH). A
    \\snippet marker must appear exactly twice in its file — Doxygen reports a
    missing pair only as a warning, which nothing fails on.
    """
    app = DOCS / "manuals" / "application"
    decks = DOCS / "figures" / "decks"
    if not app.is_dir():
        return 0
    errors = 0
    for f in sorted(app.rglob("*.md")):
        rel = f.relative_to(DOCS)
        text = f.read_text(errors="replace")
        for n, ln in enumerate(text.split("\n"), 1):
            for m in DECK_RE.finditer(ln):
                if not (decks / m.group(1)).exists():
                    print(f"ERROR {rel}:{n}: cites docs/figures/decks/{m.group(1)}, "
                          f"which does not exist")
                    errors += 1
            for m in SNIPPET_RE.finditer(ln):
                path, marker = decks / m.group(1), m.group(2)
                if not path.exists():
                    print(f"ERROR {rel}:{n}: \\snippet {m.group(1)} is not under docs/figures/decks/")
                    errors += 1
                    continue
                hits = len(re.findall(r"^\s*;?//!\s*\[" + re.escape(marker) + r"\]\s*$",
                                      path.read_text(errors="replace"), re.M))
                if hits != 2:
                    print(f"ERROR {rel}:{n}: \\snippet marker [{marker}] appears {hits} "
                          f"times in {m.group(1)} (must be exactly 2)")
                    errors += 1
    return errors


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--docs-root", default=str(DOCS), help="docs/ directory to lint")
    ap.add_argument("--src-root", default=str(SRC), help="src/ directory for coverage checks")
    ap.add_argument("--status-report", action="store_true",
                    help="also print every \\status badge in the manuals")
    args = ap.parse_args(argv)
    configure(args.docs_root, args.src_root)

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
    errors += check_options_coverage()
    errors += check_component_sections()
    errors += check_gui_prose()
    errors += check_figure_manifest()
    errors += check_front_matter()
    errors += check_message_catalogue()
    errors += check_status_badges(status_report=args.status_report)
    errors += check_mermaid_workflows()
    errors += check_application_decks()

    print(f"\n{len(files)} files, {len(pages)} pages, {len(anchors)} anchors, {errors} errors")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
