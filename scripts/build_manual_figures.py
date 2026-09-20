#!/usr/bin/env python3
"""Manual figure pipeline: one manifest, reproducible generators, a stdlib audit.

    docs/figures/MANIFEST.tsv   every image any manual embeds, one row each
    docs/figures/src/<manual>/  one generator module per figure family
    docs/figures/png|svg/       committed outputs of the generators
    docs/figures/decks/         small in-tree models the simulated tier runs
    docs/figures/cache/         git-ignored engine runs

Subcommands
  list        tabulate manifest rows            [--tier T] [--manual M] [--page P]
  check       audit the manifest (stdlib only; scripts/lint_manual_docs.py calls it)
  build       run generators                    [--tier synthetic|simulated|all]
                                                [--only GLOB] [--out DIR]
                                                [--skip-missing-deps] [--cli PATH]
  run-decks   run every simulated-tier deck through the engine CLI and record
              exit / seconds / continuity       [--cli PATH] [--out DIR]
                                                [--max-seconds N] [--deck PATH]
  clean-cache remove docs/figures/cache

Manifest columns (tab-separated, `#` comments allowed):
  fig_id      ^(eng|hydrology|hydraulics|quality|workflow)_[a-z0-9_]+$ ; unique
  tier        synthetic  numpy/matplotlib only (may REQUIRE the openswmm package)
              simulated  runs an in-tree deck through the engine first
              external   committed image whose generator or data cannot live in-tree
              legacy     EPA / hand-drawn figure, no generator
  generator   path under docs/figures/ of a module defining build(sink), or -
  inputs      ;-joined paths under docs/figures/ (decks, data), or -
  file        path under docs/ of the embedded image; generated rows must use
              figures/png/<fig_id>.png (an SVG twin is required beside it)
  page_id     ;-joined @page ids that embed the file, or - (Doxyfile-only assets)
  figure_no   n-m as printed in the caption, or -
  caption     free text
  provenance  where a legacy/external image came from; - for generated rows

What `check` asserts (every one has a recorded negative test):
  C1  every image a page embeds resolves to a row that lists that page
  C2  every row's file exists; generated rows also have their SVG twin
  C3  every row is embedded by every page it lists
  C4  no image on disk lacks a row; no emf/wmf/bmp anywhere (browsers cannot render them)
  C5  generated rows name a generator that exists and defines build(
  C6  PNG width <= 1600 px and every image <= 400 KB (Doxygen embeds at 100 %)
  C7  fig_id/tier/page_id/figure_no are well formed and unique
  C8  simulated rows' inputs exist
  C9  when a row carries a figure number, "Figure n-m" follows the embed within 5 lines
  C11 docs/figures/status.py is self-consistent
  C12 no two image directories hold the same basename — Doxygen copies images by
      basename into one output directory, so a collision silently serves one
      manual's figure on another manual's page
  C13 every `<div class="fig-hotspots" data-fig="X">` block names a figure the
      page embeds, is closed, and holds only well-formed
      `<span class="hs" data-box="x0,y0,x1,y1">@ref target "label"</span>` lines
      (fractions of the image, top-left origin, x0 < x1, y0 < y1)
  C14 the interactive-diagram assets are wired: docs/custom/js/manual-interactive.js
      exists, the Doxyfile ships it (HTML_EXTRA_FILES) and header.html loads it

Hotspots: a generator calls sink.hotspot(fig_id, ax, x0, y0, x1, y1, ref, label)
in data coordinates; save() converts the boxes to fractions of the saved PNG and
`build` rewrites the page's fig-hotspots block (inserted after the caption) so
the clickable regions never drift from the drawing. Under --out the blocks are
reported but no page is touched.
"""
from __future__ import annotations

import argparse
import fnmatch
import importlib
import importlib.util
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"

COLUMNS = ["fig_id", "tier", "generator", "inputs", "file", "page_id",
           "figure_no", "caption", "provenance"]
TIERS = {"synthetic", "simulated", "external", "legacy"}
GENERATED = {"synthetic", "simulated"}
FIG_ID_RE = re.compile(r"^(eng|hydrology|hydraulics|quality|workflow)_[a-z0-9_]+$")
FIGURE_NO_RE = re.compile(r"^[0-9A-Z]+-[0-9]+$")
IMAGE_EXT = {".png", ".jpg", ".jpeg", ".gif"}
UNRENDERABLE = {".emf", ".wmf", ".bmp", ".tif", ".tiff"}
MAX_WIDTH = 1600
MAX_BYTES = 400 * 1024
EXCLUDE_RE = re.compile(r"/SWMM[^/]*\.md$|/media/")
EMBED_RE = re.compile(r"!\[[^\]]*\]\(([^) ]+)|<img[^>]+src=\"([^\"]+)\"")
DPI = 160
HOTSPOT_DIV_RE = re.compile(r'^<div class="fig-hotspots" data-fig="([A-Za-z0-9_]+)">\s*$')
HOTSPOT_SPAN_RE = re.compile(
    r'^<span class="hs" data-box="(\d\.\d{4}),(\d\.\d{4}),(\d\.\d{4}),(\d\.\d{4})">'
    r'@ref [A-Za-z0-9_]+ "[^"]+"</span>\s*$')
CAPTION_RE = re.compile(r"^(\*|<strong>|<em>)?Figure\s+[0-9A-Z]+-[0-9]+")


# ── manifest ────────────────────────────────────────────────────────────────

class Row(dict):
    __getattr__ = dict.__getitem__

    @property
    def pages(self):
        return [] if self["page_id"] == "-" else self["page_id"].split(";")

    @property
    def input_list(self):
        return [] if self["inputs"] == "-" else self["inputs"].split(";")


def load_manifest(path: Path):
    rows, errors = [], []
    for n, ln in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not ln.strip() or ln.startswith("#"):
            continue
        f = ln.split("\t")
        if len(f) != len(COLUMNS):
            errors.append(f"MANIFEST.tsv:{n}: {len(f)} columns, expected {len(COLUMNS)}")
            continue
        rows.append(Row(zip(COLUMNS, (x.strip() for x in f))))
    return rows, errors


def write_manifest(path: Path, rows):
    lines = ["# " + "\t".join(COLUMNS),
             "# One row per image any manual embeds. Audited by scripts/build_manual_figures.py check;",
             "# the column contract is in that script's docstring. Keep rows sorted by fig_id."]
    for r in sorted(rows, key=lambda r: r["fig_id"]):
        lines.append("\t".join(r[c] for c in COLUMNS))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


# ── docs tree helpers (stdlib) ──────────────────────────────────────────────

def doxyfile_image_paths(docs: Path):
    """IMAGE_PATH entries from docs/Doxyfile, resolved against docs/."""
    text = (docs / "Doxyfile").read_text(errors="replace")
    m = re.search(r"^IMAGE_PATH\s*=\s*(.*?)(?=^\S|\Z)", text, re.M | re.S)
    if not m:
        return []
    value = m.group(1).replace("\\\n", " ")
    return [(docs / p).resolve() for p in value.split() if p]


def manual_md_files(docs: Path):
    out = [p for p in (docs / "manuals").rglob("*.md") if not EXCLUDE_RE.search(str(p))]
    if (docs / "authors.md").exists():
        out.append(docs / "authors.md")
    return sorted(out)


def page_id_of(md: Path):
    m = re.search(r"^@page +(\S+)", md.read_text(errors="replace"), re.M)
    return m.group(1) if m else md.stem


def page_index(docs: Path):
    return {page_id_of(md): md for md in manual_md_files(docs)}


def embeds(md: Path):
    """(lineno, raw path) for every markdown image or <img> in the page."""
    out = []
    for n, ln in enumerate(md.read_text(errors="replace").splitlines(), 1):
        for m in EMBED_RE.finditer(ln):
            out.append((n, m.group(1) or m.group(2)))
    return out


def resolve_embed(docs: Path, md: Path, raw: str, image_dirs):
    """The on-disk file an embed refers to, relative to docs/ — or an error.

    Doxygen resolves by basename over IMAGE_PATH; the manuals write paths
    relative to their own root (reference: `hydraulics/media/media/x.png`,
    engine: `figures/x.png`). Try the citing manual's root, then docs/, then
    the basename across the image directories (which must be unambiguous).
    """
    try:
        parts = md.relative_to(docs / "manuals").parts
        manual_root = docs / "manuals" / parts[0]
    except ValueError:
        manual_root = docs
    for base in (manual_root, docs):
        cand = base / raw
        if cand.is_file():
            return cand.resolve().relative_to(docs.resolve()).as_posix(), None
    name = Path(raw).name
    hits = [d / name for d in image_dirs if (d / name).is_file()]
    if len(hits) == 1:
        return hits[0].resolve().relative_to(docs.resolve()).as_posix(), None
    if not hits:
        return None, f"image {raw} not found under the manual root, docs/, or any IMAGE_PATH"
    return None, f"image {raw} is ambiguous: {', '.join(h.relative_to(docs).as_posix() for h in hits)}"


def png_size(path: Path):
    with path.open("rb") as fh:
        head = fh.read(24)
    if len(head) < 24 or head[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    return struct.unpack(">II", head[16:24])


def normalise_dashes(s: str) -> str:
    return s.replace("‑", "-").replace("–", "-").replace("‐", "-")


def load_status(docs: Path):
    path = docs / "figures" / "status.py"
    if not path.exists():
        return None
    spec = importlib.util.spec_from_file_location("manual_status", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ── check ───────────────────────────────────────────────────────────────────

def check_manifest(docs=DOCS, verbose=False) -> int:
    """Audit MANIFEST.tsv against the manuals and the image directories.

    Prints one `ERROR figures: ...` line per problem and returns the count.
    """
    docs = Path(docs).resolve()
    fig = docs / "figures"
    manifest = fig / "MANIFEST.tsv"
    errors = []

    def err(msg):
        errors.append(msg)
        print(f"ERROR figures: {msg}")

    if not manifest.exists():
        err("docs/figures/MANIFEST.tsv is missing")
        return 1
    rows, load_errs = load_manifest(manifest)
    for e in load_errs:
        err(e)
    image_dirs = doxyfile_image_paths(docs)
    if not image_dirs:
        err("docs/Doxyfile has no IMAGE_PATH")
    pages = page_index(docs)

    # C7 / C2 / C5 / C8 — row validity
    by_file, seen_ids = {}, set()
    for r in rows:
        if not FIG_ID_RE.match(r.fig_id):
            err(f"{r.fig_id}: fig_id must match {FIG_ID_RE.pattern}")
        if r.fig_id in seen_ids:
            err(f"{r.fig_id}: duplicate fig_id")
        seen_ids.add(r.fig_id)
        if r.tier not in TIERS:
            err(f"{r.fig_id}: tier {r.tier!r} is not one of {sorted(TIERS)}")
        if r.figure_no != "-" and not FIGURE_NO_RE.match(r.figure_no):
            err(f"{r.fig_id}: figure_no {r.figure_no!r} must look like 3-8 or C-1, or -")
        for p in r.pages:
            if p not in pages:
                err(f"{r.fig_id}: page_id {p} is not a known @page")
        if r.file in by_file:
            err(f"{r.fig_id}: file {r.file} already listed by {by_file[r.file].fig_id}")
        by_file[r.file] = r
        path = docs / r.file
        if not path.is_file():
            err(f"{r.fig_id}: file {r.file} does not exist")
        if r.tier in GENERATED:
            want = f"figures/png/{r.fig_id}"
            if not (r.file == want + ".png" or r.file == want + ".gif"):
                err(f"{r.fig_id}: generated rows must embed {want}.png (got {r.file})")
            if not (fig / "svg" / f"{r.fig_id}.svg").is_file():
                err(f"{r.fig_id}: SVG twin figures/svg/{r.fig_id}.svg is missing")
            gen = fig / r.generator
            if r.generator == "-" or not gen.is_file():
                err(f"{r.fig_id}: generator {r.generator} does not exist under docs/figures/")
            elif not re.search(r"^def build\(", gen.read_text(errors="replace"), re.M):
                err(f"{r.fig_id}: generator {r.generator} defines no build(sink)")
            if r.provenance != "-":
                err(f"{r.fig_id}: generated rows carry no provenance (got {r.provenance!r})")
        else:
            if r.generator != "-":
                err(f"{r.fig_id}: {r.tier} rows must have generator -, got {r.generator}")
            if r.tier == "external" and r.provenance == "-":
                err(f"{r.fig_id}: external rows must state their provenance")
        for inp in r.input_list:
            if not (fig / inp).exists():
                err(f"{r.fig_id}: input {inp} does not exist under docs/figures/")
        if r.tier == "simulated" and not r.input_list:
            err(f"{r.fig_id}: simulated rows must name the deck they run in inputs")

    # C1 / C3 / C9 — embeds vs rows
    cited = defaultdict(set)
    embed_lines = defaultdict(list)
    embedded_ids = defaultdict(set)
    for md in manual_md_files(docs):
        pid = page_id_of(md)
        rel = md.relative_to(docs).as_posix()
        lines = md.read_text(errors="replace").splitlines()
        for n, raw in embeds(md):
            resolved, e = resolve_embed(docs, md, raw, image_dirs)
            if e:
                err(f"{rel}:{n}: {e}")
                continue
            row = by_file.get(resolved)
            if row is None:
                err(f"{rel}:{n}: {resolved} is embedded but has no MANIFEST.tsv row")
                continue
            if pid not in row.pages:
                err(f"{rel}:{n}: {row.fig_id} is embedded by {pid}, which is not in its page_id")
            cited[resolved].add(pid)
            embed_lines[(resolved, pid)].append((rel, n, lines[n:n + 5]))
            embedded_ids[rel].add(row.fig_id)
        # C13 — hotspot blocks
        i = 0
        while i < len(lines):
            m = HOTSPOT_DIV_RE.match(lines[i])
            if lines[i].startswith('<div class="fig-hotspots"') and not m:
                err(f"{rel}:{i + 1}: malformed fig-hotspots block opener")
            if not m:
                i += 1
                continue
            fid = m.group(1)
            if fid not in embedded_ids[rel]:
                err(f"{rel}:{i + 1}: fig-hotspots block for {fid}, but the page embeds no such figure")
            j, n_spans = i + 1, 0
            while j < len(lines) and lines[j].strip() != "</div>":
                sm = HOTSPOT_SPAN_RE.match(lines[j])
                if not sm:
                    err(f"{rel}:{j + 1}: fig-hotspots line is not a well-formed hotspot span")
                else:
                    x0, y0, x1, y1 = (float(v) for v in sm.groups())
                    if not (0 <= x0 < x1 <= 1 and 0 <= y0 < y1 <= 1):
                        err(f"{rel}:{j + 1}: hotspot box {x0},{y0},{x1},{y1} is not inside the image")
                    n_spans += 1
                j += 1
            if j >= len(lines):
                err(f"{rel}:{i + 1}: fig-hotspots block for {fid} is not closed by </div>")
            elif n_spans == 0:
                err(f"{rel}:{i + 1}: fig-hotspots block for {fid} holds no hotspot")
            i = j + 1
    for r in rows:
        for p in r.pages:
            if p not in cited.get(r.file, ()):
                err(f"{r.fig_id}: never embedded by {p}")
    # C9: a row that claims a figure number must be captioned with it at least
    # once. "At least once", not at every embed: the EPA manuals reuse a
    # figure inline (an icon in a table, a sketch repeated in a later chapter)
    # without repeating its caption, and that is not drift.
    locs_by_file = defaultdict(list)
    for (file, pid), locs in embed_lines.items():
        locs_by_file[file] += locs
    for file, locs in locs_by_file.items():
        r = by_file[file]
        if r.figure_no == "-":
            continue
        pat = re.compile(r"Figure\s+" + re.escape(r.figure_no) + r"(?![0-9])")
        # embed lines are excluded from the window: a following image's
        # `![](x.png "Figure 3-2")` title attribute is not this figure's caption
        if not any(pat.search(normalise_dashes(" ".join(
                ln for ln in window if not EMBED_RE.search(ln)))) for _, _, window in locs):
            rel, n, _ = locs[0]
            err(f"{rel}:{n}: no 'Figure {r.figure_no}' caption within 5 lines of any embed of {r.fig_id}")

    # C4 / C12 — the image directories
    basenames = defaultdict(list)
    for d in image_dirs:
        if not d.is_dir():
            err(f"IMAGE_PATH directory {d.relative_to(docs) if d.is_relative_to(docs) else d} does not exist")
            continue
        for q in sorted(d.iterdir()):
            if not q.is_file() or q.name.startswith("."):
                continue
            ext = q.suffix.lower()
            rel = q.resolve().relative_to(docs).as_posix()
            if ext in UNRENDERABLE:
                err(f"{rel}: {ext} is not renderable by browsers — convert or delete")
            if ext not in IMAGE_EXT:
                continue
            basenames[q.name].append(rel)
            if rel not in by_file:
                err(f"{rel} is on disk but has no MANIFEST.tsv row (orphan)")
    for name, paths in sorted(basenames.items()):
        if len(paths) > 1:
            err(f"basename {name} exists in {len(paths)} IMAGE_PATH dirs ({'; '.join(paths)}) — "
                f"Doxygen serves one of them on every page")

    # C6 — bounds
    for r in rows:
        path = docs / r.file
        if not path.is_file():
            continue
        size = path.stat().st_size
        if size > MAX_BYTES:
            err(f"{r.fig_id}: {r.file} is {size // 1024} KB (max {MAX_BYTES // 1024} KB)")
        if path.suffix.lower() == ".png":
            dims = png_size(path)
            if dims is None:
                err(f"{r.fig_id}: {r.file} is not a PNG")
            elif dims[0] > MAX_WIDTH:
                err(f"{r.fig_id}: {r.file} is {dims[0]} px wide (max {MAX_WIDTH})")

    # C11 — status vocabulary
    st = load_status(docs)
    if st is None:
        err("docs/figures/status.py is missing")
    else:
        for e in st.self_check():
            err(e)

    # C14 — the interactive-diagram assets are wired
    js = docs / "custom" / "js" / "manual-interactive.js"
    if not js.is_file():
        err("docs/custom/js/manual-interactive.js is missing")
    doxy = (docs / "Doxyfile").read_text(errors="replace") if (docs / "Doxyfile").exists() else ""
    if "custom/js/manual-interactive.js" not in doxy:
        err("docs/Doxyfile HTML_EXTRA_FILES does not ship custom/js/manual-interactive.js")
    header = docs / "custom" / "html" / "header.html"
    if not (header.is_file() and "manual-interactive.js" in header.read_text(errors="replace")):
        err("docs/custom/html/header.html does not load manual-interactive.js")
    css = docs / "custom" / "css" / "manual.css"
    if not (css.is_file() and "fig-hotspots" in css.read_text(errors="replace")):
        err("docs/custom/css/manual.css lacks the fig-hotspots rules")

    if verbose or not errors:
        n_gen = sum(1 for r in rows if r.tier in GENERATED)
        print(f"figures: {len(rows)} rows ({n_gen} generated, "
              f"{sum(1 for r in rows if r.tier == 'legacy')} legacy, "
              f"{sum(1 for r in rows if r.tier == 'external')} external), "
              f"{len(cited)} embedded files, {len(errors)} errors")
    return len(errors)


# ── build ───────────────────────────────────────────────────────────────────

def find_cli(explicit=None):
    """The openswmm CLI: --cli, $OPENSWMM_BIN, $OPENSWMM_BUILD_DIR, or a build/install tree."""
    cands = []
    if explicit:
        cands.append(Path(explicit))
    if os.environ.get("OPENSWMM_BIN"):
        cands.append(Path(os.environ["OPENSWMM_BIN"]))
    bd = os.environ.get("OPENSWMM_BUILD_DIR")
    if bd:
        for sub in ("bin/Release", "bin/Debug", "bin", "."):
            cands.append(Path(bd) / sub / "openswmm")
    cands += sorted(ROOT.glob("build*/bin/*/openswmm")) + sorted(ROOT.glob("build*/bin/openswmm"))
    cands += sorted(ROOT.glob("install/*/bin/openswmm"))
    for c in cands:
        if c.is_file() and os.access(c, os.X_OK):
            return c
    return None


def upsert_options(text: str, options: dict) -> str:
    """Rewrite [OPTIONS] keys in an .inp (add the key if absent)."""
    lines = text.splitlines()
    out, in_opts, done = [], False, set()
    for ln in lines:
        s = ln.strip()
        if s.startswith("["):
            if in_opts:
                for k, v in options.items():
                    if k not in done:
                        out.append(f"{k:<24}{v}")
                out.append("")
            in_opts = s.upper().startswith("[OPTIONS]")
        elif in_opts and s and not s.startswith(";"):
            key = s.split()[0].upper()
            if key in options:
                out.append(f"{key:<24}{options[key]}")
                done.add(key)
                continue
        out.append(ln)
    if in_opts:
        for k, v in options.items():
            if k not in done:
                out.append(f"{k:<24}{v}")
    return "\n".join(out) + "\n"


class RunResult:
    def __init__(self, inp, rpt, out, h5, seconds):
        self.inp, self.rpt, self.out, self.h5, self.seconds = inp, rpt, out, h5, seconds


class FigureSink:
    """What a generator's build(sink) receives."""

    def __init__(self, fig_dir: Path, expected_ids, out_dir: Path | None, cli):
        self.fig_dir = fig_dir
        self.expected = set(expected_ids)
        self.png_dir = (out_dir or fig_dir) / "png" if out_dir else fig_dir / "png"
        self.svg_dir = (out_dir or fig_dir) / "svg" if out_dir else fig_dir / "svg"
        self.cli = cli
        self.produced = set()
        self.status = load_status(fig_dir.parent)
        self._pending_hotspots = defaultdict(list)
        self.hotspots = {}

    def hotspot(self, fig_id: str, ax, x0, y0, x1, y1, ref: str, label: str):
        """Register a clickable region of `fig_id`, in `ax` data coordinates.

        `ref` is a Doxygen page id or anchor; `label` the link text. The
        region is converted to fractions of the saved PNG by save().
        """
        if not ref or not label:
            return
        self._pending_hotspots[fig_id].append((ax, float(x0), float(y0), float(x1), float(y1), ref, label))

    def _resolve_hotspots(self, fig, fig_id):
        pending = self._pending_hotspots.pop(fig_id, [])
        if not pending:
            return
        fig.canvas.draw()
        renderer = fig.canvas.get_renderer()
        tb = fig.get_tightbbox(renderer)            # inches, the same box bbox_inches="tight" saves
        dpi = fig.dpi
        boxes = []
        for ax, x0, y0, x1, y1, ref, label in pending:
            (px0, py0), (px1, py1) = ax.transData.transform([(x0, y0), (x1, y1)])
            fx0 = (min(px0, px1) / dpi - tb.x0) / tb.width
            fx1 = (max(px0, px1) / dpi - tb.x0) / tb.width
            fy0 = (tb.y1 - max(py0, py1) / dpi) / tb.height
            fy1 = (tb.y1 - min(py0, py1) / dpi) / tb.height
            clip = lambda v: min(1.0, max(0.0, v))  # noqa: E731
            fx0, fx1, fy0, fy1 = clip(fx0), clip(fx1), clip(fy0), clip(fy1)
            if fx1 - fx0 < 1e-3 or fy1 - fy0 < 1e-3:
                continue
            boxes.append((round(fx0, 4), round(fy0, 4), round(fx1, 4), round(fy1, 4), ref, label))
        self.hotspots[fig_id] = boxes

    def save(self, fig, fig_id: str):
        if fig_id not in self.expected:
            raise RuntimeError(f"generator produced {fig_id}, which its manifest rows do not list")
        import matplotlib.pyplot as plt
        self.png_dir.mkdir(parents=True, exist_ok=True)
        self.svg_dir.mkdir(parents=True, exist_ok=True)
        png, svg = self.png_dir / f"{fig_id}.png", self.svg_dir / f"{fig_id}.svg"
        self._resolve_hotspots(fig, fig_id)
        # pad_inches=0: the canvases carry their own margins, and the default
        # 0.1 in pad would push a 10 in figure past the 1600 px bound
        fig.savefig(png, dpi=DPI, bbox_inches="tight", pad_inches=0, metadata={"Software": None})
        self._shrink_png(png)
        with_salt = {"svg.hashsalt": fig_id}
        import matplotlib
        with matplotlib.rc_context(with_salt):
            fig.savefig(svg, format="svg", bbox_inches="tight", pad_inches=0,
                        metadata={"Date": None, "Creator": None})
        plt.close(fig)
        self.produced.add(fig_id)
        print(f"  wrote {png.name} ({png.stat().st_size // 1024} KB) + {svg.name}")

    @staticmethod
    def _shrink_png(png: Path):
        """Lossless re-encode; if still over the byte bound, quantise to a 256-colour palette.

        Diagrams are flat colour and text, so a palette costs nothing visible
        and halves the file. A figure that stays over the bound after that is
        drawn too large: the build's bounds check reports it.
        """
        from PIL import Image
        with Image.open(png) as im:
            im.load()
            im.save(png, optimize=True)
            if png.stat().st_size > MAX_BYTES:
                im.convert("RGB").quantize(colors=256, method=Image.Quantize.MEDIANCUT).save(png, optimize=True)
                print(f"  note: {png.name} quantised to 256 colours to stay under {MAX_BYTES // 1024} KB")

    def run(self, deck: str, options: dict | None = None, key: str | None = None) -> RunResult:
        """Run docs/figures/decks/<deck> through the engine, cached under docs/figures/cache."""
        src = self.fig_dir / "decks" / deck
        if not src.is_file():
            raise FileNotFoundError(f"deck {deck} is not under docs/figures/decks/")
        key = key or (re.sub(r"[^A-Za-z0-9]+", "_", "_".join(f"{k}{v}" for k, v in sorted((options or {}).items()))) or "base")
        work = self.fig_dir / "cache" / Path(deck).stem / key
        work.mkdir(parents=True, exist_ok=True)
        inp, rpt, out = work / "model.inp", work / "model.rpt", work / "model.out"
        text = src.read_text(errors="replace")
        if options:
            text = upsert_options(text, {k.upper(): v for k, v in options.items()})
        for sidecar in src.parent.iterdir():
            if sidecar.is_file() and sidecar != src and not (work / sidecar.name).exists():
                shutil.copy2(sidecar, work / sidecar.name)
        if inp.exists() and inp.read_text(errors="replace") == text and rpt.exists() and out.exists():
            return RunResult(inp, rpt, out, next(work.glob("*.h5"), None), 0.0)
        inp.write_text(text)
        cli = find_cli(self.cli)
        if cli is None:
            raise RuntimeError("no openswmm CLI: pass --cli, set OPENSWMM_BIN or OPENSWMM_BUILD_DIR")
        t0 = time.time()
        p = subprocess.run([str(cli), inp.name, rpt.name, out.name], cwd=work,
                           capture_output=True, text=True)
        secs = time.time() - t0
        if p.returncode != 0:
            raise RuntimeError(f"{cli.name} failed on {deck} [{key}] (exit {p.returncode}):\n{p.stdout[-800:]}{p.stderr[-800:]}")
        return RunResult(inp, rpt, out, next(work.glob("*.h5"), None), secs)


def load_generator(path: Path):
    spec = importlib.util.spec_from_file_location(f"manual_fig_{path.stem}", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def hotspot_block(fig_id: str, boxes) -> list:
    lines = [f'<div class="fig-hotspots" data-fig="{fig_id}">']
    for x0, y0, x1, y1, ref, label in boxes:
        label = label.replace('"', "'")
        lines.append(f'<span class="hs" data-box="{x0:.4f},{y0:.4f},{x1:.4f},{y1:.4f}">@ref {ref} "{label}"</span>')
    lines.append("</div>")
    return lines


def upsert_hotspot_blocks(docs: Path, row, boxes) -> list:
    """Write (or rewrite) the fig-hotspots block after the figure's caption on every page the row lists.

    Returns the pages changed. A page that embeds the figure without a caption
    line within five lines gets the block right after the embed.
    """
    pages = page_index(docs)
    changed = []
    block = hotspot_block(row.fig_id, boxes)
    for pid in row.pages:
        md = pages.get(pid)
        if md is None:
            continue
        text = md.read_text(errors="replace")
        lines = text.split("\n")
        out, i, done = [], 0, False
        while i < len(lines):
            ln = lines[i]
            out.append(ln)
            m = EMBED_RE.search(ln)
            if not done and m and Path(m.group(1) or m.group(2)).name == f"{row.fig_id}.png":
                # find the caption line within the next five lines
                j = i + 1
                cap = None
                for k in range(i + 1, min(i + 6, len(lines))):
                    if CAPTION_RE.match(lines[k].strip()):
                        cap = k
                        break
                end = cap if cap is not None else i
                out.extend(lines[i + 1:end + 1])
                j = end + 1
                # an existing block (after optional blank lines) is replaced
                k = j
                while k < len(lines) and not lines[k].strip():
                    k += 1
                if k < len(lines) and HOTSPOT_DIV_RE.match(lines[k]) and HOTSPOT_DIV_RE.match(lines[k]).group(1) == row.fig_id:
                    e = k
                    while e < len(lines) and lines[e].strip() != "</div>":
                        e += 1
                    out.extend(lines[j:k])
                    out.extend(block)
                    i = e + 1
                else:
                    out.append("")
                    out.extend(block)
                    i = j
                done = True
                continue
            i += 1
        new = "\n".join(out)
        if done and new != text:
            md.write_text(new)
            changed.append(md.relative_to(docs).as_posix())
    return changed


def cmd_build(args) -> int:
    docs = DOCS
    fig = docs / "figures"
    rows, errs = load_manifest(fig / "MANIFEST.tsv")
    if errs:
        for e in errs:
            print("ERROR", e)
        return 1
    tiers = GENERATED if args.tier == "all" else {args.tier}
    by_gen = defaultdict(list)
    for r in rows:
        if r.tier in tiers and (not args.only or fnmatch.fnmatch(r.fig_id, args.only)):
            by_gen[r.generator].append(r)
    if not by_gen:
        print(f"no {args.tier} rows match" + (f" {args.only}" if args.only else ""))
        return 0
    sys.path.insert(0, str(fig))
    try:
        import style  # noqa: F401  (vendored house style; sets the Agg backend)
        style.apply_rc()
    except ImportError as exc:
        print(f"ERROR figures: cannot import docs/figures/style.py ({exc}); install matplotlib")
        return 1
    out_dir = Path(args.out).resolve() if args.out else None
    ran, skipped, failures = 0, [], 0
    for gen, grp in sorted(by_gen.items()):
        path = fig / gen
        print(f"== {gen}  ({', '.join(r.fig_id for r in grp)})")
        try:
            mod = load_generator(path)
        except ImportError as exc:
            if args.skip_missing_deps:
                skipped.append(f"{gen}: {exc}")
                continue
            print(f"ERROR figures: {gen}: {exc}")
            failures += 1
            continue
        missing = [m for m in getattr(mod, "REQUIRES", ()) if importlib.util.find_spec(m) is None]
        if missing:
            if args.skip_missing_deps:
                skipped.append(f"{gen}: requires {', '.join(missing)}")
                continue
            print(f"ERROR figures: {gen} requires {', '.join(missing)}")
            failures += 1
            continue
        sink = FigureSink(fig, [r.fig_id for r in grp], out_dir, args.cli)
        try:
            mod.build(sink)
        except Exception as exc:  # a generator that throws is a failed gate, not a crash
            print(f"ERROR figures: {gen} raised {type(exc).__name__}: {exc}")
            failures += 1
            continue
        want = {r.fig_id for r in grp}
        if sink.produced != want:
            print(f"ERROR figures: {gen} produced {sorted(sink.produced)} but the manifest lists {sorted(want)}")
            failures += 1
        for fid, boxes in sorted(sink.hotspots.items()):
            row = next(r for r in grp if r.fig_id == fid)
            if out_dir is not None:
                print(f"  {fid}: {len(boxes)} hotspots (not written under --out)")
                continue
            changed = upsert_hotspot_blocks(docs, row, boxes)
            print(f"  {fid}: {len(boxes)} hotspots" + (f" -> {', '.join(changed)}" if changed else " (pages unchanged)"))
        for fid in sink.produced:
            png = sink.png_dir / f"{fid}.png"
            dims = png_size(png)
            if dims and dims[0] > MAX_WIDTH:
                print(f"ERROR figures: {fid}.png is {dims[0]} px wide (max {MAX_WIDTH})")
                failures += 1
            if png.stat().st_size > MAX_BYTES:
                print(f"ERROR figures: {fid}.png is {png.stat().st_size // 1024} KB (max {MAX_BYTES // 1024})")
                failures += 1
        ran += 1
    for s in skipped:
        print(f"skipped {s}")
    if ran == 0 and not failures:
        print("ERROR figures: no generator ran (all skipped)")
        return 1
    print(f"\n{ran} generators ran, {len(skipped)} skipped, {failures} failures")
    return 1 if failures else 0


# ── run-decks ───────────────────────────────────────────────────────────────

CONTINUITY_RE = re.compile(r"Continuity Error \(%\)\s*\.+\s*([-+0-9.]+)")


def run_one_deck(cli: Path, deck: Path, work: Path, max_seconds: float):
    work.mkdir(parents=True, exist_ok=True)
    for sidecar in deck.parent.iterdir():
        if sidecar.is_file():
            shutil.copy2(sidecar, work / sidecar.name)
    inp = work / deck.name
    rpt, out = work / "model.rpt", work / "model.out"
    t0 = time.time()
    try:
        p = subprocess.run([str(cli), inp.name, rpt.name, out.name], cwd=work,
                           capture_output=True, text=True, timeout=max_seconds)
        code, timed_out = p.returncode, False
        (work / "stdout.txt").write_text(p.stdout + p.stderr)
    except subprocess.TimeoutExpired:
        code, timed_out = 124, True
    secs = time.time() - t0
    rpt_text = rpt.read_text(errors="replace") if rpt.exists() else ""
    rpt_errors = [ln.strip() for ln in rpt_text.splitlines() if ln.strip().startswith("ERROR")]
    cont = CONTINUITY_RE.findall(rpt_text)
    ok = code == 0 and not rpt_errors and not timed_out
    return ok, code, secs, rpt_errors, cont, timed_out


def cmd_run_decks(args) -> int:
    fig = DOCS / "figures"
    cli = find_cli(args.cli)
    if cli is None:
        print("ERROR decks: no openswmm CLI: pass --cli, set OPENSWMM_BIN or OPENSWMM_BUILD_DIR")
        return 1
    if args.deck:
        decks = [Path(args.deck).resolve()]
    else:
        rows, errs = load_manifest(fig / "MANIFEST.tsv")
        decks = sorted({(fig / i).resolve() for r in rows if r.tier == "simulated"
                        for i in r.input_list if i.endswith(".inp")})
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    if not decks:
        print("run-decks: no simulated-tier decks in the manifest — nothing to run")
        (out / "summary.tsv").write_text("deck\texit\tseconds\tcontinuity\tnote\n")
        return 0
    lines = ["deck\texit\tseconds\tcontinuity\tnote"]
    failures = 0
    for deck in decks:
        work = out / deck.parent.name / deck.stem
        ok, code, secs, rpt_errors, cont, timed_out = run_one_deck(cli, deck, work, args.max_seconds)
        note = "timeout" if timed_out else ("; ".join(rpt_errors)[:120] if rpt_errors else "ok")
        lines.append(f"{deck.relative_to(fig) if deck.is_relative_to(fig) else deck}\t{code}\t{secs:.1f}\t{','.join(cont)}\t{note}")
        print(f"{'PASS' if ok else 'FAIL'}  {deck.name}  exit={code}  {secs:.1f}s  {note}")
        failures += 0 if ok else 1
    (out / "summary.tsv").write_text("\n".join(lines) + "\n")
    print(f"\n{len(decks)} decks, {failures} failures — {out / 'summary.tsv'}")
    return 1 if failures else 0


# ── list / clean ────────────────────────────────────────────────────────────

def cmd_list(args) -> int:
    rows, errs = load_manifest(DOCS / "figures" / "MANIFEST.tsv")
    for e in errs:
        print("ERROR", e)
    for r in sorted(rows, key=lambda r: r["fig_id"]):
        if args.tier and r.tier != args.tier:
            continue
        if args.manual and not r.fig_id.startswith(args.manual + "_"):
            continue
        if args.page and args.page not in r.pages:
            continue
        print(f"{r.fig_id:48} {r.tier:10} {r.figure_no:6} {r.file}")
    return 1 if errs else 0


def cmd_clean_cache(args) -> int:
    cache = DOCS / "figures" / "cache"
    if cache.exists():
        shutil.rmtree(cache)
        print(f"removed {cache}")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Manual figure pipeline",
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list")
    p.add_argument("--tier")
    p.add_argument("--manual")
    p.add_argument("--page")
    p = sub.add_parser("check")
    p.add_argument("--docs-root", default=str(DOCS))
    p.add_argument("-v", "--verbose", action="store_true")
    p = sub.add_parser("build")
    p.add_argument("--tier", default="synthetic", choices=["synthetic", "simulated", "all"])
    p.add_argument("--only", help="glob on fig_id")
    p.add_argument("--out", help="write png/ and svg/ under this directory instead of docs/figures/")
    p.add_argument("--skip-missing-deps", action="store_true")
    p.add_argument("--cli", help="openswmm executable for the simulated tier")
    p = sub.add_parser("run-decks")
    p.add_argument("--cli")
    p.add_argument("--out", default=str(ROOT / "tests" / "output" / "manual_decks"))
    p.add_argument("--max-seconds", type=float, default=600)
    p.add_argument("--deck", help="run this one deck instead of the manifest's simulated decks")
    sub.add_parser("clean-cache")
    args = ap.parse_args(argv)
    if args.cmd == "list":
        return cmd_list(args)
    if args.cmd == "check":
        return 1 if check_manifest(Path(args.docs_root), verbose=args.verbose) else 0
    if args.cmd == "build":
        return cmd_build(args)
    if args.cmd == "run-decks":
        return cmd_run_decks(args)
    if args.cmd == "clean-cache":
        return cmd_clean_cache(args)
    return 2


if __name__ == "__main__":
    sys.exit(main())
