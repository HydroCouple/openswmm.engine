# -*- coding: utf-8 -*-
#
# OpenSWMM documentation build configuration file.

import importlib
import importlib.abc
import importlib.machinery
import importlib.util
import sys
import os

# ============================================================================
# .pyi stub-file import hook
# ============================================================================
# Sphinx autodoc works by *importing* modules and inspecting the live objects.
# Our Cython extensions (.pyx) compile to .so/.pyd files that need the C/C++
# engine libraries at load time.  When building docs without a full build, we
# want autodoc to read the .pyi stub files instead — they contain all the
# class definitions, signatures, and docstrings as valid Python (with ``...``
# bodies).
#
# This custom meta-path finder makes Python treat .pyi files as importable
# modules for any subpackage under ``openswmm``.  It is installed *before*
# the normal finders so it takes priority over the missing .so files.
# ============================================================================

_PACKAGE_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))


class _PyiStubFinder(importlib.abc.MetaPathFinder):
    """Meta-path finder that locates .pyi stub files as importable modules."""

    # Subpackages whose private Cython modules have .pyi stubs we can load.
    _STUB_PREFIXES = (
        'openswmm.engine',        # new v6.0 API
        'openswmm.legacy.engine',  # legacy solver
        'openswmm.legacy.output',  # legacy output reader
    )

    def find_spec(self, fullname, path, target=None):
        # Intercept private Cython extension modules (e.g. _solver, _nodes)
        # that have companion .pyi stubs.  Do NOT intercept __init__.py or
        # pure-Python modules like _enums.py.
        if not any(fullname.startswith(p) for p in self._STUB_PREFIXES):
            return None

        parts = fullname.split('.')
        leaf = parts[-1]
        if not leaf.startswith('_') or leaf.startswith('__'):
            return None  # let normal Python handle __init__.py, _enums.py, etc.

        pyi_path = os.path.join(_PACKAGE_ROOT, *parts) + '.pyi'
        if os.path.isfile(pyi_path):
            return importlib.util.spec_from_file_location(
                fullname, pyi_path,
                loader=_PyiStubLoader(pyi_path),
            )
        return None


class _PyiStubLoader(importlib.abc.Loader):
    """Loader that executes a .pyi file as a regular Python module."""

    def __init__(self, pyi_path):
        self.pyi_path = pyi_path

    def create_module(self, spec):
        return None  # use default module creation

    def exec_module(self, module):
        module.__file__ = self.pyi_path
        # Ensure relative imports (from ._solver import Solver) resolve correctly
        if not hasattr(module, '__package__') or module.__package__ is None:
            if hasattr(module, '__path__'):
                module.__package__ = module.__name__
            else:
                module.__package__ = module.__name__.rpartition('.')[0]
        with open(self.pyi_path, 'r', encoding='utf-8') as f:
            code = compile(f.read(), self.pyi_path, 'exec')
        exec(code, module.__dict__)


# Install the hook before everything else
sys.meta_path.insert(0, _PyiStubFinder())

# Add the parent directory to sys.path so autodoc can find the openswmm package
sys.path.insert(0, _PACKAGE_ROOT)

try:
    import openswmm
    version = openswmm.__version__
    release = openswmm.__version__
except Exception:
    version = '6.0.0a1'
    release = version

# -- General configuration ------------------------------------------------

extensions = [
    'sphinx.ext.autodoc',
    'sphinx.ext.autosummary',
    'sphinx.ext.napoleon',
    'sphinx.ext.intersphinx',
    'sphinx.ext.viewcode',
    'sphinx.ext.todo',
    'sphinx.ext.mathjax',
    'myst_parser',
    # sphinx-design: tab-set / grid / grid-item-card directives used in
    # the User Guide and Migration pages.
    'sphinx_design',
]

# MyST (Markdown) settings
myst_enable_extensions = [
    'colon_fence',
    'deflist',
]
myst_heading_anchors = 3
suppress_warnings = ['myst.header', 'ref.python', 'duplicate']
source_suffix = {
    '.rst': 'restructuredtext',
    '.md': 'markdown',
}

master_doc = 'index'

# -- Project information --------------------------------------------------

project = 'OpenSWMM'
copyright = '2026 Caleb Buahin'
author = 'Caleb Buahin'

# -- Napoleon settings (Google/NumPy docstrings) --------------------------

napoleon_google_docstring = True
napoleon_numpy_docstring = True
napoleon_include_init_with_doc = True
napoleon_use_param = True
napoleon_use_rtype = True

# -- Autodoc settings -----------------------------------------------------
# The .pyi import hook above means autodoc can import the stub files
# directly and extract real docstrings and type annotations from them.
# No mocking is needed for the engine subpackage.

autodoc_default_options = {
    'members': True,
    'undoc-members': True,
    'show-inheritance': True,
    'member-order': 'bysource',
}
autodoc_typehints = 'description'
autodoc_typehints_format = 'short'

# Only mock the legacy Cython modules that don't have .pyi stubs
autodoc_mock_imports = [
    'openswmm._openswmm',
    'openswmm.legacy.engine._solver',
    'openswmm.legacy.output._output',
    # Backward-compat shims delegate to legacy.*, so mock the old paths too
    'openswmm.solver._solver',
    'openswmm.output._output',
]

autoclass_content = 'both'

autosummary_generate = True

add_function_parentheses = True
add_module_names = False

numfig = True

# -- Intersphinx ----------------------------------------------------------

intersphinx_mapping = {
    'python': ('https://docs.python.org/3', None),
    'numpy': ('https://numpy.org/doc/stable/', None),
}

# -- Options for HTML output ----------------------------------------------

language = 'en'
pygments_style = 'sphinx'
todo_include_todos = True

exclude_patterns = [
    '_build',
    # Internal planning document — kept in-tree for reference but not
    # surfaced in the published docs.
    'LEGACY_API_EXPANSION_PLAN.md',
]

on_rtd = os.environ.get('READTHEDOCS', None) == 'True'
if not on_rtd:
    html_theme = 'pydata_sphinx_theme'
else:
    html_theme = 'default'

html_theme_options = {
    "logo": {
        "text": "OpenSWMM",
        "image_light": "images/hydrocouple_logo.png",
        "image_dark": "images/hydrocouple_logo.png",
    },
    # Top-nav cross-link back to the Doxygen C/C++ engine docs.
    # Deployment layout (see .github/workflows/documentation.yml):
    #     /         Doxygen docs (root)
    #     /python/  these (Sphinx) docs
    # ``../index.html`` resolves to the Doxygen root when both sites are
    # co-deployed under GitHub Pages.  When viewing the Sphinx docs
    # locally without the Doxygen bundle, this link 404s — that's an
    # accepted local-only limitation.
    "external_links": [
        {"name": "C/C++ Engine Docs", "url": "../index.html"},
    ],
    "icon_links": [
        {
            "name": "GitHub",
            "url": "https://github.com/HydroCouple/openswmm.engine",
            "icon": "fa-brands fa-github",
            "type": "fontawesome",
        },
        {
            "name": "PyPI",
            "url": "https://pypi.org/project/openswmm",
            "icon": "fa-brands fa-python",
            "type": "fontawesome",
        },
    ],
    "use_edit_page_button": False,
    "show_toc_level": 2,
    "navbar_end": ["theme-switcher.html", "navbar-icon-links.html"],
}

html_logo = 'images/hydrocouple_logo.png'
html_title = 'OpenSWMM'
html_favicon = 'images/hydrocouple_logo.png'

html_static_path = ['_static']
html_css_files = ['openswmm.css']

htmlhelp_basename = 'openswmmdoc'

# -- Options for LaTeX output ---------------------------------------------

latex_elements = {
    'papersize': 'letterpaper',
    'pointsize': '10pt',
}

latex_documents = [
    (master_doc, 'openswmm.tex', 'OpenSWMM Documentation', author, 'manual'),
]

# -- Options for manual page output ---------------------------------------

man_pages = [
    (master_doc, 'openswmm', 'OpenSWMM Documentation', [author], 1)
]

# -- Autodoc member filtering ---------------------------------------------

def skip_member(app, what, name, obj, skip, options):
    exclude = [
        'main',
        'configure_subparsers',
        'process_args_decorator',
        'process_args',
    ]
    return True if name in exclude else skip


# ============================================================================
# epytext → reStructuredText docstring translator
# ============================================================================
# The .pyi stub files use epytext-style docstring tags (@param, @type,
# @return, @rtype, @raise, @ivar, @cvar, C{...}, L{...}, B{...}, I{...}).
# Sphinx's docutils parser does not understand these — without this
# translation it emits ~2,870 "Unexpected indentation" / "Block quote ends
# without a blank line" warnings and renders the markup as raw text.
#
# Rather than rewriting every stub, we hook autodoc and translate at
# documentation-build time.  The translation is mechanical:
#
#   @param X: text     →   :param X: text
#   @type X: text      →   :type X: text
#   @return: text      →   :returns: text
#   @rtype: text       →   :rtype: text
#   @raise X: text     →   :raises X: text
#   @ivar X: text      →   :ivar X: text
#   @cvar X: text      →   :cvar X: text
#   @note: text        →   .. note:: text
#   C{anything}        →   ``anything``
#   L{anything}        →   :py:obj:`anything`
#   B{anything}        →   **anything**
#   I{anything}        →   *anything*
#
# Multi-line @-block continuations (subsequent lines indented relative to
# the @-line) are preserved.  A blank line separates each @-block from the
# preceding paragraph so docutils sees a proper field-list, not a block
# quote continuation.
import re

_EPY_TAGS = {
    'param':   ':param',
    'type':    ':type',
    'return':  ':returns:',
    'returns': ':returns:',
    'rtype':   ':rtype:',
    'raise':   ':raises',
    'raises':  ':raises',
    'except':  ':raises',
    'ivar':    ':ivar',
    'cvar':    ':cvar',
    'var':     ':var',
}

_EPY_TAG_RE = re.compile(
    r'^(\s*)@(' + '|'.join(_EPY_TAGS) + r')(?:\s+([^\s:][^:]*?))?\s*:\s*',
    re.MULTILINE,
)
_EPY_NOTE_RE = re.compile(r'^(\s*)@note\s*:\s*', re.MULTILINE)
# Inline tags can be followed by any character.  RST requires a word
# boundary (whitespace or punctuation-followed-by-whitespace) after a
# closing ``...``/`...`/**...**/*...*; if the original text had something
# like C{0.0}=closed, the naïve replacement ``0.0``=closed is invalid.
# We use a regex lookahead and emit a backslash-space when the next char
# is non-whitespace, which RST treats as a zero-width separator.
_EPY_INLINE_RE = re.compile(r'([CLBI])\{([^}]*)\}(?=(.))')


def _inline_replace(m: re.Match) -> str:
    tag, body, nxt = m.group(1), m.group(2), m.group(3)
    if tag == 'C':
        replacement = f'``{body}``'
    elif tag == 'L':
        replacement = f':py:obj:`{body}`'
    elif tag == 'B':
        replacement = f'**{body}**'
    else:  # 'I'
        replacement = f'*{body}*'
    # Word boundary fix: if the next char is not whitespace and not the
    # end of the string, insert a backslash-space so RST recognises the
    # end of the inline-markup span.
    if nxt and not nxt.isspace():
        replacement += '\\ '
    return replacement


def _epytext_to_rst(app, what, name, obj, options, lines):
    """autodoc-process-docstring callback: rewrite epytext to RST in-place."""
    if not lines:
        return

    text = '\n'.join(lines)

    # @param X: ...  →  :param X: ...        (with the value-bearing tags)
    # @return: ...   →  :returns: ...        (no name)
    def _tag_sub(m):
        indent, tag, name_part = m.group(1), m.group(2), m.group(3)
        rst = _EPY_TAGS[tag]
        if name_part:
            # tags that take a name: param, type, raise, ivar, cvar, var
            return f'{indent}{rst} {name_part.strip()}: '
        # nameless: return, returns, rtype
        if rst.endswith(':'):
            return f'{indent}{rst} '
        return f'{indent}{rst}: '

    text = _EPY_TAG_RE.sub(_tag_sub, text)

    # @note: ... → standalone admonition.  Keep it simple — single line.
    text = _EPY_NOTE_RE.sub(r'\1.. note:: ', text)

    # Inline X{...} → RST inline markup.
    text = _EPY_INLINE_RE.sub(_inline_replace, text)

    # Ensure blank lines bracket the field list:
    #   * BEFORE: a paragraph immediately followed by `:param ...` causes
    #     the "Block quote ends without a blank line" warning unless we
    #     drop a blank line in between.
    #   * AFTER: a `:param ...` / `:returns: ...` / `:rtype: ...` block
    #     immediately followed by a continuation paragraph causes the
    #     "Field list ends without a blank line" warning unless we drop
    #     a blank line in between.
    field_re = re.compile(
        r'^\s*:(?:param|type|returns?|rtype|raises?|ivar|cvar|var)\b'
    )

    raw_lines = text.split('\n')
    new_lines: list[str] = []
    prev_is_blank = True
    prev_was_field_block = False  # last non-blank line was field-list

    def _is_field(line: str) -> bool:
        return bool(field_re.match(line))

    def _is_indented_continuation(line: str) -> bool:
        # Continuation of a field's value: indented relative to col 0,
        # non-empty, not itself a field.
        stripped = line.lstrip()
        return (
            bool(stripped)
            and line.startswith(' ')
            and not _is_field(line)
        )

    for ln in raw_lines:
        # Insert blank line BEFORE the start of a field block.
        if (
            _is_field(ln)
            and not prev_is_blank
            and new_lines
            and not _is_field(new_lines[-1])
        ):
            new_lines.append('')

        # Insert blank line AFTER the end of a field block when followed
        # by a non-field, non-continuation line.
        if (
            prev_was_field_block
            and ln.strip()                       # non-blank
            and not _is_field(ln)
            and not _is_indented_continuation(ln)
        ):
            new_lines.append('')

        new_lines.append(ln)

        # Track state for the next iteration.
        if ln.strip() == '':
            prev_is_blank = True
            prev_was_field_block = False
        elif _is_field(ln) or _is_indented_continuation(ln):
            prev_is_blank = False
            prev_was_field_block = True
        else:
            prev_is_blank = False
            prev_was_field_block = False

    lines[:] = new_lines


def setup(app):
    app.connect('autodoc-skip-member', skip_member)
    app.connect('autodoc-process-docstring', _epytext_to_rst)
