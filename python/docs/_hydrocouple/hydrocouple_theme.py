"""HydroCouple documentation theme for Sphinx.

Templates and styles layered on pydata-sphinx-theme: the manual keeps PyData's sidebars,
search, page outline and color-mode switcher, and gains the hydrocouple.org top bar,
cross-manual menu, landing hero, footer, fonts and palette.

Use from a project's ``conf.py``::

    sys.path.insert(0, os.path.abspath("_hydrocouple"))
    import hydrocouple_theme
    hydrocouple_theme.configure(
        globals(),
        manual_id="openswmm.engine.python",
        project_label="OpenSWMM Engine · Python",
        repository="https://github.com/HydroCouple/openswmm.engine",
        hero={...},          # optional landing hero for the root page
    )

``configure`` only adds to the existing settings; options a project already sets
(``html_theme_options``, ``html_context``, ``html_css_files``...) are kept.

The module is also a Sphinx extension that provides the ``mermaid`` directive (and MyST
``mermaid`` fences through ``myst_fence_as_directive``). It replaces sphinxcontrib-mermaid:
the directive writes the diagram source into ``<pre class="mermaid">`` and the shared
``hc-diagrams.js`` renders it in the theme's colors, in both color modes.
"""
import html
import json
import os

from docutils import nodes
from docutils.parsers.rst import directives
from sphinx import addnodes
from sphinx.util.docutils import SphinxDirective

HERE = os.path.dirname(os.path.abspath(__file__))
STYLESHEETS = ["hc/hc-fonts.css", "hc/hc-tokens.css", "hc/hc-diagrams.css", "hc/hydrocouple-sphinx.css"]
SCRIPTS = ["hc/hc-diagrams.js"]


class MermaidDirective(SphinxDirective):
    """``.. mermaid:: [file]`` with inline content; ``:caption:`` and ``:name:`` optional."""

    has_content = True
    optional_arguments = 1
    option_spec = {"caption": directives.unchanged, "name": directives.unchanged,
                   "alt": directives.unchanged, "align": directives.unchanged,
                   "zoom": directives.flag, "config": directives.unchanged}

    def run(self):
        if self.arguments:
            rel, path = self.env.relfn2path(self.arguments[0])
            self.env.note_dependency(rel)
            with open(path, encoding="utf-8") as handle:
                source = handle.read()
        else:
            source = "\n".join(self.content)
        label = html.escape(self.options.get("alt", "Diagram"), quote=True)
        raw = nodes.raw("", f'<pre class="mermaid" aria-label="{label}">{html.escape(source)}</pre>',
                        format="html")
        other = addnodes.only(expr="not html")
        other += nodes.literal_block(source, source)
        content = [raw, other]
        if "caption" not in self.options:
            return content
        figure = nodes.figure("", *content, classes=["hc-mermaid-figure"])
        parsed, _ = self.state.inline_text(self.options["caption"], self.lineno)
        figure += nodes.caption(self.options["caption"], "", *parsed)
        if "name" in self.options:
            self.add_name(figure)
        return [figure]


def setup(app):
    app.add_directive("mermaid", MermaidDirective, override=True)
    return {"parallel_read_safe": True, "parallel_write_safe": True}


def load_manuals(current_id):
    """Return the registry with unpublished manuals removed and the current one marked."""
    with open(os.path.join(HERE, "manuals.json"), encoding="utf-8") as handle:
        registry = json.load(handle)
    groups = []
    for group in registry["groups"]:
        manuals = [dict(m, current=(m["id"] == current_id))
                   for m in group["manuals"] if m.get("published") or m["id"] == current_id]
        if manuals:
            groups.append({"title": group["title"], "manuals": manuals})
    registry["groups"] = groups
    return registry


def configure(ns, *, manual_id, project_label, repository, hero=None):
    """Apply the HydroCouple theme to a conf.py namespace (pass ``globals()``)."""
    extensions = [e for e in ns.setdefault("extensions", []) if e != "sphinxcontrib.mermaid"]
    if "hydrocouple_theme" not in extensions:
        extensions.append("hydrocouple_theme")
    ns["extensions"] = extensions
    ns["html_theme"] = "pydata_sphinx_theme"
    ns["templates_path"] = [os.path.join(HERE, "templates")] + [
        p for p in ns.get("templates_path", []) if p != os.path.join(HERE, "templates")]
    ns["html_static_path"] = list(ns.get("html_static_path", [])) + [os.path.join(HERE, "static")]

    css = list(ns.get("html_css_files", []))
    ns["html_css_files"] = STYLESHEETS + [c for c in css if c not in STYLESHEETS]
    js = [s for s in ns.get("html_js_files", []) if s not in SCRIPTS]
    ns["html_js_files"] = js + SCRIPTS

    registry = load_manuals(manual_id)
    options = dict(ns.get("html_theme_options") or {})
    options.setdefault("navbar_start", ["hc-brand.html"])
    options.setdefault("navbar_center", ["navbar-nav.html"])
    options.setdefault("navbar_align", "left")
    options.setdefault("header_links_before_dropdown", 4)
    options["navbar_end"] = ["hc-manuals.html"] + [
        item for item in options.get("navbar_end", ["theme-switcher.html", "navbar-icon-links.html"])
        if item != "hc-manuals.html"]
    options.setdefault("footer_start", ["hc-footer.html"])
    options.setdefault("footer_end", [])
    options.setdefault("pygments_light_style", "a11y-high-contrast-light")
    options.setdefault("pygments_dark_style", "a11y-high-contrast-dark")
    if not any(link.get("url") == repository for link in options.get("icon_links", [])):
        options.setdefault("icon_links", []).insert(0, {
            "name": "GitHub", "url": repository, "icon": "fa-brands fa-github", "type": "fontawesome"})
    ns["html_theme_options"] = options

    context = dict(ns.get("html_context") or {})
    context.update({
        "hc_manuals": registry,
        "hc_manual_id": manual_id,
        "hc_project_label": project_label,
        "hc_repository": repository,
        "hc_hero": hero,
    })
    ns["html_context"] = context
