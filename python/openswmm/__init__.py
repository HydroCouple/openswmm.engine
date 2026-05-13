"""
openswmm -- Python bindings for the OpenSWMM stormwater modelling engine.

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Subpackages
-----------
legacy.engine
    Legacy EPA SWMM 5.x solver bindings (Cython).
legacy.output
    Legacy EPA SWMM 5.x binary output reader (Cython).
engine
    New data-oriented engine 6.x bindings (Cython).

This top-level package additionally:

  - Configures the platform-specific shared-library search path so the
    bundled C/C++ shared libraries can be located by the dynamic linker.
  - Resolves the package version string into ``__version__``.
  - Re-exports the legacy engine and legacy output public symbols when
    their compiled Cython extensions are available, for backward
    compatibility with pre-6.x callers.
"""

import importlib.metadata
import os
import platform

# ---------------------------------------------------------------------------
# Windows DLL search path
#
# On macOS and Linux the Cython extensions embed @loader_path / $ORIGIN
# RPATH entries pointing at the directory where the bundled dylib/so lives,
# so the dynamic linker finds it automatically — no extra path configuration
# is required.
#
# On Windows, Python 3.8+ requires explicit DLL directories because the
# old LoadLibrary PATH search was disabled.  The bundled .dll files live
# alongside the Cython .pyd extensions under openswmm/engine/ and
# openswmm/legacy/{engine,output}/ — NOT in sys.prefix/bin.  Register each
# subpackage directory so Python can find all bundled DLLs regardless of
# which module is imported first.
# ---------------------------------------------------------------------------
if platform.system() == "Windows" and hasattr(os, "add_dll_directory"):
    _pkg_dir = os.path.dirname(__file__)
    for _dll_subdir in (
        os.path.join(_pkg_dir, "engine"),
        os.path.join(_pkg_dir, "legacy", "engine"),
        os.path.join(_pkg_dir, "legacy", "output"),
    ):
        if os.path.isdir(_dll_subdir):
            os.add_dll_directory(_dll_subdir)

# ---------------------------------------------------------------------------
# Version
# ---------------------------------------------------------------------------
try:
    __version__: str = importlib.metadata.version("openswmm")
except importlib.metadata.PackageNotFoundError:
    __version__ = "0.0.0.dev0"

# ---------------------------------------------------------------------------
# Public re-exports  (legacy subpackages)
#
# These require compiled Cython extensions (.so/.pyd).  When building
# documentation or in a partial install the extensions may not be available,
# so we import them conditionally.
# ---------------------------------------------------------------------------
try:
    from openswmm.legacy.engine import *  # noqa: F401,F403
except ImportError:
    pass

try:
    from openswmm.legacy.output import *  # noqa: F401,F403
except ImportError:
    pass

__all__ = ["__version__"]
