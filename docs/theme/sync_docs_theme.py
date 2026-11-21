#!/usr/bin/env python3
"""Vendor the HydroCouple documentation theme into a project's documentation sources.

The canonical theme lives in one place (this tool's directory). Each project carries a copy
so that it builds on its own; this tool writes that copy and checks it for drift.

    sync_docs_theme.py sphinx  <dest-dir>            # e.g. python/docs/_hydrocouple
    sync_docs_theme.py doxygen <dest-dir> [--header <header.html>] [--repository <url>]
                                         [--manual-id <id>]
    add --check to report differences without writing (exit 1 on drift)

Sphinx layout written to <dest-dir>:
    hydrocouple_theme.py, manuals.json, templates/*.html,
    static/hc/{hc-fonts.css,hc-tokens.css,hc-diagrams.css,hydrocouple-sphinx.css,
               hc-diagrams.js,hydrocouple_logo.png,fonts/*}

Doxygen layout written to <dest-dir> (flat, because Doxygen copies HTML_EXTRA_FILES and
HTML_EXTRA_STYLESHEET into the output root by base name):
    hc-fonts.css (font URLs without the fonts/ prefix), hc-tokens.css, hc-diagrams.css,
    hydrocouple-doxygen.css, hc-doxygen.js, hc-diagrams.js, hydrocouple_logo.png, *.woff2, *-OFL.txt
With --header, the region between <!-- hc:topbar:begin --> and <!-- hc:topbar:end --> in
that header is replaced by the top bar rendered from manuals.json.
"""
import argparse
import html
import json
import re
import sys
from pathlib import Path

THEME = Path(__file__).resolve().parent
BEGIN, END = "<!-- hc:topbar:begin -->", "<!-- hc:topbar:end -->"


def sphinx_files():
    out = {
        "hydrocouple_theme.py": (THEME / "sphinx/hydrocouple_theme.py").read_bytes(),
        "manuals.json": (THEME / "manuals.json").read_bytes(),
    }
    for template in sorted((THEME / "sphinx/templates").iterdir()):
        out[f"templates/{template.name}"] = template.read_bytes()
    static = "static/hc/"
    out[static + "hc-fonts.css"] = (THEME / "hc-fonts.css").read_bytes()
    out[static + "hc-tokens.css"] = (THEME / "hc-tokens.css").read_bytes()
    out[static + "hc-diagrams.css"] = (THEME / "hc-diagrams.css").read_bytes()
    out[static + "hydrocouple-sphinx.css"] = (THEME / "sphinx/hydrocouple-sphinx.css").read_bytes()
    out[static + "hc-diagrams.js"] = (THEME / "js/hc-diagrams.js").read_bytes()
    out[static + "hydrocouple_logo.png"] = (THEME / "img/hydrocouple_logo.png").read_bytes()
    for font in sorted((THEME / "fonts").iterdir()):
        out[static + "fonts/" + font.name] = font.read_bytes()
    return out


def doxygen_files():
    fonts_css = (THEME / "hc-fonts.css").read_text().replace("url(fonts/", "url(")
    out = {
        "hc-fonts.css": fonts_css.encode(),
        "hc-tokens.css": (THEME / "hc-tokens.css").read_bytes(),
        "hc-diagrams.css": (THEME / "hc-diagrams.css").read_bytes(),
        "hydrocouple-doxygen.css": (THEME / "doxygen/hydrocouple-doxygen.css").read_bytes(),
        "hc-doxygen.js": (THEME / "doxygen/hc-doxygen.js").read_bytes(),
        "hc-diagrams.js": (THEME / "js/hc-diagrams.js").read_bytes(),
        "hydrocouple_logo.png": (THEME / "img/hydrocouple_logo.png").read_bytes(),
    }
    for font in sorted((THEME / "fonts").iterdir()):
        out[font.name] = font.read_bytes()
    return out


def render_topbar(manual_id, repository):
    registry = json.loads((THEME / "manuals.json").read_text())
    e = html.escape
    groups = []
    for group in registry["groups"]:
        items = []
        for m in group["manuals"]:
            if not (m.get("published") or m["id"] == manual_id):
                continue
            current = m["id"] == manual_id
            items.append(
                f'<li><a class="hc-manuals__item{" active" if current else ""}" href="{e(m["url"])}"'
                f'{" aria-current=\"page\"" if current else ""}><span class="hc-manuals__title">{e(m["title"])}</span>'
                f'<span class="hc-manuals__kind">{e(m["kind"])}</span></a></li>')
        if items:
            groups.append(f'<div class="hc-manuals__group"><p class="hc-manuals__heading">{e(group["title"])}</p>'
                          f'<ul>{"".join(items)}</ul></div>')
    links = "".join(f'<a href="{e(l["url"])}">{e(l["title"])}</a>' for l in registry["links"])
    return f"""{BEGIN}
<header class="hc-topbar" role="banner">
  <div class="hc-topbar__inner">
    <a class="hc-brand__home" href="{e(registry["home"])}" title="HydroCouple home"><img class="hc-brand__mark" src="$relpath^hydrocouple_logo.png" alt="" width="30" height="30"/><span class="hc-brand__name">HydroCouple</span></a>
    <span class="hc-brand__sep" aria-hidden="true"></span>
    <a class="hc-brand__project" href="$relpath^index.html">$projectname</a>
    <nav class="hc-topbar__nav" aria-label="HydroCouple documentation">
      <div class="hc-manuals">
        <button class="hc-manuals__button" type="button" aria-expanded="false" aria-haspopup="true">Documentation</button>
        <div class="hc-manuals__menu" hidden="hidden">
          <div class="hc-manuals__groups">{"".join(groups)}</div>
          <div class="hc-manuals__footer">{links}</div>
        </div>
      </div>
      <a class="hc-topbar__icon" href="{e(repository)}" title="Source on GitHub" aria-label="Source on GitHub"><svg viewBox="0 0 16 16" width="20" height="20" aria-hidden="true" focusable="false"><path fill="currentColor" d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0016 8c0-4.42-3.58-8-8-8z"/></svg></a>
      <button class="hc-topbar__icon hc-mode-toggle" type="button" title="Color mode" aria-label="Color mode"><span class="hc-mode-toggle__light" aria-hidden="true">&#9728;</span><span class="hc-mode-toggle__dark" aria-hidden="true">&#9790;</span></button>
    </nav>
  </div>
</header>
{END}"""


def apply(dest, files, check):
    drift = []
    for rel, data in files.items():
        path = dest / rel
        if not path.exists() or path.read_bytes() != data:
            drift.append(rel)
            if not check:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
    return drift


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("kind", choices=["sphinx", "doxygen"])
    parser.add_argument("dest", type=Path)
    parser.add_argument("--header", type=Path)
    parser.add_argument("--repository", default="https://github.com/HydroCouple")
    parser.add_argument("--manual-id", default="")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    drift = apply(args.dest, sphinx_files() if args.kind == "sphinx" else doxygen_files(), args.check)
    if args.header:
        text = args.header.read_text()
        if BEGIN not in text or END not in text:
            sys.exit(f"{args.header}: missing {BEGIN} ... {END} markers")
        new = re.sub(re.escape(BEGIN) + ".*?" + re.escape(END),
                     lambda _: render_topbar(args.manual_id, args.repository), text, flags=re.S)
        if new != text:
            drift.append(str(args.header))
            if not args.check:
                args.header.write_text(new)
    for rel in drift:
        print(("drift: " if args.check else "wrote: ") + rel)
    return 1 if (args.check and drift) else 0


if __name__ == "__main__":
    sys.exit(main())
