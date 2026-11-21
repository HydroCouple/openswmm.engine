# HydroCouple documentation theme

The shared look of the HydroCouple manuals: the hydrocouple.org top bar and cross-manual
menu, the landing hero, fonts, color tokens for light and dark modes, and diagram
rendering. One set of tokens and assets serves both generators: Sphinx (layered on
pydata-sphinx-theme) and Doxygen (layered on doxygen-awesome).

This directory is the canonical copy. Manuals do not read it at build time; each one
carries a vendored copy written by `sync_docs_theme.py`, so it builds on its own.

## Layout

| Path | Contents |
| --- | --- |
| `hc-tokens.css` | Design tokens (colors, surfaces, status colors) for both modes |
| `hc-fonts.css`, `fonts/` | Inter, Sora and JetBrains Mono (`*.woff2`) with their OFL licenses |
| `hc-diagrams.css`, `js/hc-diagrams.js` | Mermaid rendering in the theme colors, with the diagram viewer |
| `img/hydrocouple_logo.png` | Brand mark for the top bar and footer |
| `manuals.json` | Registry of every manual; drives the cross-manual menu |
| `sphinx/` | `hydrocouple_theme.py` (configuration helper and `mermaid` directive), templates, stylesheet |
| `doxygen/` | `hydrocouple-doxygen.css` and `hc-doxygen.js` (manuals menu, landing hero, color-mode button) |
| `sync_docs_theme.py` | Writes a vendored copy and checks it for drift |
| `contrast.py` | WCAG contrast check of the tokens in both modes |

## Vendoring the theme into a manual

From the repository root:

```sh
# Sphinx: python/docs/conf.py then calls hydrocouple_theme.configure(...)
python3 docs/theme/sync_docs_theme.py sphinx python/docs/_hydrocouple

# Doxygen: also fills the top bar between <!-- hc:topbar:begin --> and
# <!-- hc:topbar:end --> in the header
python3 docs/theme/sync_docs_theme.py doxygen docs/custom/hydrocouple \
  --header docs/custom/html/header.html \
  --repository https://github.com/HydroCouple/openswmm.engine \
  --manual-id openswmm.engine
```

Add `--check` to either command to report drift without writing anything; it exits 1
when the vendored copy or the header's top bar differs from this directory. Run the sync
again after changing anything here, and commit the vendored copies with the change.

The Doxygen copy is flat because Doxygen copies `HTML_EXTRA_FILES` and
`HTML_EXTRA_STYLESHEET` into the root of the HTML output by base name, so the fonts sit
next to the stylesheets (the sync rewrites the font URLs in `hc-fonts.css` to match).
Doxygen does not expand wildcards there either: every vendored file is listed by name in
`docs/Doxyfile`.

## Changing the tokens

Every token change must keep `python3 docs/theme/contrast.py` passing. It checks each text
pair at 4.5:1 or better, and large text and meaningful graphics at 3:1, in both modes.
