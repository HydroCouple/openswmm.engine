"""
openswmm -- Python bindings for the OpenSWMM stormwater modelling engine.

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Subpackages
-----------
legacy.engine
    Legacy EPA SWMM 5.x solver bindings (Cython).
legacy.output
    Legacy EPA SWMM 5.x binary output reader (Cython).
engine
    New data-oriented engine 6.x bindings (Cython).
"""

import importlib.metadata
import os
import platform
import sys

# ---------------------------------------------------------------------------
# Platform-specific shared-library search path configuration
#
# The Cython extensions link against shared C/C++ libraries that are
# installed alongside the Python package.  We need to tell the dynamic
# linker where to find them.
# ---------------------------------------------------------------------------
_LIB_DIR = os.path.join(sys.prefix, "lib")
_BIN_DIR = os.path.join(sys.prefix, "bin")

if platform.system() == "Windows":
    if hasattr(os, "add_dll_directory"):
        if os.path.exists(os.path.join(sys.prefix, "conda-meta")):
            os.environ["CONDA_DLL_SEARCH_MODIFICATION_ENABLE"] = "1"
        os.add_dll_directory(_BIN_DIR)
    else:
        os.environ["PATH"] = _BIN_DIR + ";" + os.environ.get("PATH", "")

elif platform.system() == "Linux":
    sys.path.append(_LIB_DIR)
    os.environ["LD_LIBRARY_PATH"] = (
        _LIB_DIR + ":" + os.environ.get("LD_LIBRARY_PATH", "")
    )

elif platform.system() == "Darwin":
    sys.path.append(_LIB_DIR)
    os.environ["DYLD_LIBRARY_PATH"] = (
        _LIB_DIR + ":" + _BIN_DIR + ":" + os.environ.get("DYLD_LIBRARY_PATH", "")
    )

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
