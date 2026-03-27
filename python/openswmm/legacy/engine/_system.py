"""Pythonic access to SWMM system-level properties and mass balance totals."""

from typing import TYPE_CHECKING

from ._solver import (
    SWMMObjects,
    SWMMSystemProperties,
    SWMMFlowUnits,
)

if TYPE_CHECKING:
    from ._solver import Solver


class LegacySystem:
    """System-level properties, simulation settings, and mass balance totals.

    Wraps a :class:`Solver` instance and provides convenient property-based
    access to global simulation parameters and post-simulation mass balance
    breakdowns.
    """

    __slots__ = ("_solver",)

    def __init__(self, solver: "Solver"):
        self._solver = solver

    def _get(self, prop: SWMMSystemProperties) -> float:
        return self._solver.get_value(SWMMObjects.SYSTEM, prop, 0)

    # --- simulation settings (read-only after start) ---
    @property
    def flow_units(self) -> SWMMFlowUnits:
        """Flow units used in this simulation."""
        return SWMMFlowUnits(int(self._get(SWMMSystemProperties.FLOW_UNITS)))

    @property
    def unit_system(self) -> int:
        """Unit system (0=US, 1=SI)."""
        return int(self._get(SWMMSystemProperties.UNIT_SYSTEM))

    @property
    def routing_step(self) -> float:
        """Routing time step (seconds)."""
        return self._get(SWMMSystemProperties.ROUTING_STEP)

    @property
    def report_step(self) -> float:
        """Reporting time step (seconds)."""
        return self._get(SWMMSystemProperties.REPORT_STEP)

    @property
    def total_steps(self) -> int:
        """Total number of routing steps."""
        return int(self._get(SWMMSystemProperties.TOTAL_STEPS))

    @property
    def num_threads(self) -> int:
        """Number of threads for parallel computation."""
        return int(self._get(SWMMSystemProperties.NUM_THREADS))

    @property
    def allow_ponding(self) -> bool:
        """Whether ponding is allowed at nodes."""
        return bool(int(self._get(SWMMSystemProperties.ALLOW_PONDING)))

    # --- error/tolerance settings ---
    @property
    def head_tolerance(self) -> float:
        """Head convergence tolerance (length units)."""
        return self._get(SWMMSystemProperties.HEAD_TOL)

    @property
    def sys_flow_tolerance(self) -> float:
        """System flow convergence tolerance."""
        return self._get(SWMMSystemProperties.SYS_FLOW_TOL)

    @property
    def lat_flow_tolerance(self) -> float:
        """Lateral flow convergence tolerance."""
        return self._get(SWMMSystemProperties.LAT_FLOW_TOL)

    # --- continuity errors ---
    @property
    def runoff_error(self) -> float:
        """Runoff continuity error (%)."""
        return self._get(SWMMSystemProperties.RUNOFF_ERROR)

    @property
    def flow_error(self) -> float:
        """Flow routing continuity error (%)."""
        return self._get(SWMMSystemProperties.FLOW_ERROR)

    @property
    def quality_error(self) -> float:
        """Quality routing continuity error (%)."""
        return self._get(SWMMSystemProperties.QUAL_ERROR)

    # --- mass balance totals (call after solver.end()) ---
    @property
    def routing_totals(self) -> dict:
        """System-level flow routing mass balance totals.

        Returns a dict with keys: dw_inflow, ww_inflow, gw_inflow,
        ii_inflow, ex_inflow, flooding, outflow, evap_loss, seep_loss,
        reacted, init_storage, final_storage, pct_error.

        Mass balance equation::

            total_inflow = dw + ww + gw + ii + ex
            total_outflow = flooding + outflow + evap_loss + seep_loss
            storage_change = final_storage - init_storage
            balance = total_inflow - total_outflow - storage_change
        """
        return self._solver.get_routing_totals()

    @property
    def runoff_totals(self) -> dict:
        """System-level surface runoff mass balance totals.

        Returns a dict with keys: rainfall, evap, infil, runoff, drains,
        runon, init_storage, final_storage, init_snow_cover,
        final_snow_cover, snow_removed, pct_error.
        """
        return self._solver.get_runoff_totals()

    @property
    def mass_balance_error(self):
        """Mass balance errors as (runoff_err%, flow_err%, quality_err%)."""
        return self._solver.get_mass_balance_error()

    def __repr__(self) -> str:
        return "LegacySystem()"
