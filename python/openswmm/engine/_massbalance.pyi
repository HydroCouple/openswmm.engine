"""
Mass balance & continuity (Pythonic v1 surface)
===============================================

Type stubs for :mod:`openswmm.engine._massbalance`.
"""

from typing import Union

from ._enums import RoutingTotal, RunoffTotal
from ._report import RoutingDiagnostics
from ._solver import Solver


_Key = Union[int, str]


class MassBalance:
    def __init__(self, solver: Solver) -> None: ...

    runoff_continuity_error: float
    routing_continuity_error: float
    routing_diagnostics: RoutingDiagnostics
    max_courant: float

    def quality_continuity_error(self, pollutant: _Key) -> float: ...
    def runoff_total(self, component: RunoffTotal) -> float: ...
    def routing_total(self, component: RoutingTotal) -> float: ...
    def quality_seep_loss(self, pollutant: _Key) -> float: ...
    def quality_evap_loss(self, pollutant: _Key) -> float: ...

    def __repr__(self) -> str: ...
