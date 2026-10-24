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

"""What the installed engine build can do, as JSON-ready data."""

from __future__ import annotations

from typing import Any

# SWMM_CHECKPOINT_* feature bits of openswmm_checkpoint.h (text-tested against the header).
CHECKPOINT_FEATURES = {
    "inspection": 1 << 0,
    "restore": 1 << 1,
    "save_new": 1 << 2,
    "binary_profile": 1 << 3,
    "surface_profile": 1 << 4,
    "source_stream": 1 << 5,
    "file_profile": 1 << 6,
    "bundle_export": 1 << 7,
    "session_builder": 1 << 8,
}


def capabilities() -> dict[str, Any]:
    """Describe this engine build: version, catalog revision, optional features, threads,
    checkpoint support and process components.

    The values describe the build, not a particular model, and are all JSON
    types, so a server or job system can publish them unchanged.

    .. code-block:: python

        from openswmm.engine import capabilities
        capabilities()["features"]["2d"]        # True
    """
    import openswmm

    from . import HAS_2D, HAS_GEOPACKAGE, catalog
    from ._output2d import Output2DReader
    from ._process_components import ProcessComponents
    from ._solver import Solver
    from ._transport import thread_info

    solver = Solver()
    solver.create()
    try:
        flags = solver.checkpoints.capabilities
    finally:
        solver.destroy()
    return {
        "version": openswmm.__version__,
        "catalog_revision": catalog.revision(),
        "c_exports": catalog.load()["c_exports"],
        "features": {
            "2d": HAS_2D,
            "geopackage": HAS_GEOPACKAGE,
            "hdf5_2d_output": bool(Output2DReader.available()),
        },
        "threads": thread_info()._asdict(),
        "checkpoints": [name for name, bit in CHECKPOINT_FEATURES.items() if flags & bit],
        "process_components": [c.id for c in ProcessComponents.known() if c.implemented],
        "component_libraries": [
            {"id": lib.id, "kind": lib.kind, "version": lib.version, "loaded": not lib.load_error}
            for lib in ProcessComponents.libraries()
        ],
    }
