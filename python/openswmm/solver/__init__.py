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
Backward-compatibility shim that re-exports the full public API of
``openswmm.legacy.engine`` under the shorter ``openswmm.solver`` namespace.

.. deprecated::
    Import directly from ``openswmm.legacy.engine`` instead::

        from openswmm.legacy.engine import Solver, run_solver

@note: Every name listed in the explicit re-export block below is guaranteed
    to remain importable from this module for backward-compatibility purposes,
    but new code should use the ``openswmm.legacy.engine`` path directly.

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: Apache-2.0
"""
# Backward-compatibility shim: openswmm.solver -> openswmm.legacy.engine
# Deprecated: use ``from openswmm.legacy.engine import ...`` instead.
from openswmm.legacy.engine import *  # noqa: F401,F403
from openswmm.legacy.engine import (  # noqa: F401  explicit re-exports
    SWMMObjects,
    SWMMNodeTypes,
    SWMMLinkTypes,
    SWMMRainGageProperties,
    SWMMSubcatchmentProperties,
    SWMMNodeProperties,
    SWMMLinkProperties,
    SWMMSystemProperties,
    SWMMFlowUnits,
    SWMMAPIErrors,
    run_solver,
    decode_swmm_datetime,
    encode_swmm_datetime,
    version,
    get_error_message,
    SolverState,
    CallbackType,
    SWMMSolverException,
    Solver,
)
