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
# Optional GPU/OpenMP acceleration companion
#
# The base `openswmm` wheel is CPU-only and portable (Kokkos-free). OpenMP
# acceleration of the 2D surface solver is shipped separately as the
# `openswmm-gpu-omp` companion distribution, which installs a sibling import
# package (`openswmm_gpu_omp`) containing the `libopenswmm_gpu_omp` plugin.
#
# The C++ engine discovers the plugin at runtime via the OPENSWMM_GPU_PLUGIN_PATH
# search path (SurfaceSolverFactory::search_dirs). When the companion is
# installed, surface its directory to the engine by PREPENDING it to that env
# var — without clobbering a value the user set deliberately. With no companion
# installed this is a silent no-op and 2D runs on the serial CPU solver.
#
# This must happen before any 2D model is initialised; import time (here) is
# comfortably early enough, since the factory reads the env var lazily when it
# builds a solver.
# ---------------------------------------------------------------------------
def _register_gpu_companion() -> None:
    try:
        import openswmm_gpu_omp
    except ImportError:
        return  # companion not installed → CPU-only, nothing to do
    plugin_dir = os.path.dirname(os.path.abspath(openswmm_gpu_omp.__file__))
    if not os.path.isdir(plugin_dir):
        return
    sep = os.pathsep  # ';' on Windows, ':' elsewhere — matches search_dirs()
    existing = os.environ.get("OPENSWMM_GPU_PLUGIN_PATH", "")
    parts = existing.split(sep) if existing else []
    if plugin_dir not in parts:
        os.environ["OPENSWMM_GPU_PLUGIN_PATH"] = (
            plugin_dir + (sep + existing if existing else "")
        )
    # On Windows the plugin DLL's vendored dependencies (e.g. libomp) sit beside
    # it; register the dir so LoadLibrary can resolve them.
    if platform.system() == "Windows" and hasattr(os, "add_dll_directory"):
        os.add_dll_directory(plugin_dir)


_register_gpu_companion()

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
