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
copyright = '2026 HydroCouple'
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

exclude_patterns = ['_build']

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
    "icon_links": [
        {
            "name": "GitHub",
            "url": "https://github.com/HydroCouple/OpenSWMMCore",
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


def setup(app):
    app.connect('autodoc-skip-member', skip_member)
