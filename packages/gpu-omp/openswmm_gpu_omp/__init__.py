"""openswmm-gpu-omp -- OpenMP (Kokkos) acceleration plugin for OpenSWMM 2D.

This companion distribution ships a single native plugin
(``libopenswmm_gpu_omp``) beside this file and carries no public Python API:
*installing it is sufficient*.

At import, the base :mod:`openswmm` package detects this package and prepends
this directory to the engine's plugin search path
(``OPENSWMM_GPU_PLUGIN_PATH``). The C++ core then dlopen()s the plugin and, with
``OPENSWMM_2D_BACKEND`` left at ``auto`` (the default), runs the 2D surface
solver on the Kokkos-OpenMP backend automatically. Uninstalling this package
returns 2D runs to the serial CPU solver.

:author: Caleb Buahin
:license: MIT
"""

import importlib.metadata
import os

#: Absolute directory holding the bundled plugin shared library. Exposed so the
#: base package (and callers) can locate it without re-deriving the path.
plugin_dir: str = os.path.dirname(os.path.abspath(__file__))

try:
    __version__: str = importlib.metadata.version("openswmm-gpu-omp")
except importlib.metadata.PackageNotFoundError:
    __version__ = "0.0.0.dev0"

__all__ = ["__version__", "plugin_dir"]
