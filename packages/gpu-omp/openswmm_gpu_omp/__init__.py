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
:license: Apache-2.0
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
