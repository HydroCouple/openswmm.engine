# SPDX-License-Identifier: Apache-2.0
#
# Copyright 2026 Caleb Buahin
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
openswmm -- Python bindings for the OpenSWMM stormwater modelling engine.

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: Apache-2.0

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
# GPU/OpenMP acceleration of the 2D surface solver
#
# The plugin can be present twice:
#
#   1. IN-WHEEL — this wheel builds it and installs it into openswmm/engine/,
#      beside openswmm_engine and the single bundled libomp.
#   2. COMPANION — the `openswmm-gpu-omp` distribution installs a sibling
#      import package (`openswmm_gpu_omp`) holding the same plugin, so it can
#      be added or refreshed against an already-installed base.
#
# The engine discovers the plugin at runtime through the
# OPENSWMM_GPU_PLUGIN_PATH search path (SurfaceSolverFactory::search_dirs), and
# loads the FIRST match. Only one may ever be dlopen()ed: each copy is linked
# against the libomp bundled next to it, and loading two OpenMP runtimes into
# one process aborts every 2D run with "OMP: Error #15".
#
# So the in-wheel copy wins when it exists. It is the one guaranteed to match
# both the engine binary it ships with and that engine's libomp; a companion
# installed from a different build could satisfy neither. The companion is
# registered only as a fallback, which is exactly the situation it was built
# for — no in-wheel plugin, so nothing to conflict with.
#
# An OPENSWMM_GPU_PLUGIN_PATH the caller set is preserved, not clobbered — but
# it is appended after ours, so it does not override the chosen copy. That is
# the pre-existing behaviour of this shim, kept deliberately: prepending is what
# makes the packaged plugin the default without the user opting in. A caller who
# needs their own build to win should point the env var at it and uninstall the
# packaged copy, or set it after import.
#
# This must happen before any 2D model is initialised; import time is early
# enough, since the factory reads the env var lazily when it builds a solver.
# ---------------------------------------------------------------------------
def _prepend_plugin_dir(plugin_dir: str) -> None:
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


def _in_wheel_plugin_dir() -> "str | None":
    """openswmm/engine/ if it holds a built GPU plugin, else None."""
    engine_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "engine")
    if not os.path.isdir(engine_dir):
        return None
    for name in os.listdir(engine_dir):
        # libopenswmm_gpu_omp.{so,dylib} / openswmm_gpu_omp.dll, and the same
        # for any other backend the wheel was built with (cuda, hip, sycl).
        if "openswmm_gpu_" in name and name.endswith((".so", ".dylib", ".dll")):
            return engine_dir
    return None


def _register_gpu_plugin() -> None:
    in_wheel = _in_wheel_plugin_dir()
    if in_wheel is not None:
        _prepend_plugin_dir(in_wheel)
        return  # do NOT also register the companion — see the note above
    try:
        import openswmm_gpu_omp
    except ImportError:
        return  # no plugin anywhere → 2D runs on the serial CPU solver
    plugin_dir = os.path.dirname(os.path.abspath(openswmm_gpu_omp.__file__))
    if os.path.isdir(plugin_dir):
        _prepend_plugin_dir(plugin_dir)


_register_gpu_plugin()

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
