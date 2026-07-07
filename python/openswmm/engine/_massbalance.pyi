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

    @property
    def runoff_continuity_error(self) -> float:
        """Runoff continuity (mass-balance) error, as a percentage."""
        ...
    @property
    def routing_continuity_error(self) -> float:
        """Flow-routing continuity (mass-balance) error, as a percentage."""
        ...
    @property
    def routing_diagnostics(self) -> RoutingDiagnostics:
        """Routing-solver time-step diagnostics (step sizes, Courant number,
        convergence counts) as a :class:`RoutingDiagnostics` record."""
        ...
    @property
    def max_courant(self) -> float:
        """Maximum Courant number reached over the routing run."""
        ...

    def quality_continuity_error(self, pollutant: _Key) -> float: ...
    def runoff_total(self, component: RunoffTotal) -> float: ...
    def routing_total(self, component: RoutingTotal) -> float: ...
    def quality_seep_loss(self, pollutant: _Key) -> float: ...
    def quality_evap_loss(self, pollutant: _Key) -> float: ...

    def __repr__(self) -> str: ...
