"""Pythonic access to SWMM system-level properties and mass balance totals."""

from typing import TYPE_CHECKING, Dict, Tuple

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

    def __init__(self, solver: "Solver") -> None:
        self._solver = solver

    def _get(self, prop: SWMMSystemProperties) -> float:
        return self._solver.get_value(SWMMObjects.SYSTEM, prop, 0)

    def _set(self, prop: SWMMSystemProperties, value: float) -> None:
        self._solver.set_value(SWMMObjects.SYSTEM, prop, 0, value)

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

    # --- climate ---
    def get_evap_rate(self) -> float:
        """Return the current climate-derived evaporation rate.

        The rate the engine would apply in the absence of any PET
        prescription, including monthly adjustments (read-only). Intended
        for caller-side composition with
        L{LegacySubcatchment.set_api_pet}: read this rate, apply your own
        adjustment logic, and prescribe the result.

        @return: Evaporation rate in user units (in/day or mm/day).
        @rtype: float
        """
        return self._get(SWMMSystemProperties.EVAP_RATE)

    def get_temperature(self) -> float:
        """Return the current air temperature.

        @return: Air temperature in user units (deg F or deg C).
        @rtype: float
        """
        return self._get(SWMMSystemProperties.TEMPERATURE)

    def set_api_temperature(self, value: float) -> None:
        """Prescribe the air temperature used for snowmelt and
        temperature-derived evaporation.

        Overrides the climate data-source value (bypassing monthly
        adjustments) and keeps derived quantities (saturation vapor
        pressure) consistent. Persists until cleared with
        L{clear_api_temperature}.

        @param value: Air temperature in user units (deg F or deg C).
        @type value: float
        @return: None
        @rtype: None
        """
        self._set(SWMMSystemProperties.API_TEMPERATURE, value)

    def get_api_temperature(self) -> float:
        """Return the prescribed air temperature.

        @return: Prescribed temperature in user units, or -999 when no
            prescription is active.
        @rtype: float
        """
        return self._get(SWMMSystemProperties.API_TEMPERATURE)

    def clear_api_temperature(self) -> None:
        """Clear any prescribed air temperature, reverting to climate data.

        @return: None
        @rtype: None
        """
        self._set(SWMMSystemProperties.API_TEMPERATURE, -1000.0)

    def get_wind_speed(self) -> float:
        """Return the current wind speed.

        @return: Wind speed in user units (mph or km/hr).
        @rtype: float
        """
        return self._get(SWMMSystemProperties.WIND_SPEED)

    def set_api_wind_speed(self, value: float) -> None:
        """Prescribe the wind speed used in rain-on-snow melt.

        Overrides the monthly/climate-file value. Persists until cleared
        with L{clear_api_wind_speed}.

        @param value: Wind speed in user units (mph or km/hr), >= 0.
        @type value: float
        @return: None
        @rtype: None
        """
        self._set(SWMMSystemProperties.API_WIND_SPEED, value)

    def get_api_wind_speed(self) -> float:
        """Return the prescribed wind speed.

        @return: Prescribed wind speed in user units, or a negative value
            when no prescription is active.
        @rtype: float
        """
        return self._get(SWMMSystemProperties.API_WIND_SPEED)

    def clear_api_wind_speed(self) -> None:
        """Clear any prescribed wind speed, reverting to climate data.

        @return: None
        @rtype: None
        """
        self._set(SWMMSystemProperties.API_WIND_SPEED, -1.0)

    def set_api_evap_rate(self, value: float) -> None:
        """Prescribe the system-wide evaporation rate.

        Replaces the climate-derived rate (after monthly adjustments) for
        every consumer — subcatchments, LID units, groundwater, conduits
        and storage nodes. Per-subcatchment
        L{LegacySubcatchment.set_api_pet} still takes precedence. Persists
        until cleared with L{clear_api_evap_rate}.

        @param value: Evaporation rate in user units (in/day or mm/day), >= 0.
        @type value: float
        @return: None
        @rtype: None
        """
        self._set(SWMMSystemProperties.API_EVAP, value)

    def get_api_evap_rate(self) -> float:
        """Return the prescribed system-wide evaporation rate.

        @return: Prescribed rate in user units, or a negative value when
            no prescription is active.
        @rtype: float
        """
        return self._get(SWMMSystemProperties.API_EVAP)

    def clear_api_evap_rate(self) -> None:
        """Clear any prescribed evaporation rate, reverting to climate data.

        @return: None
        @rtype: None
        """
        self._set(SWMMSystemProperties.API_EVAP, -1.0)

    def set_evap_dry_only(self, flag: bool) -> None:
        """Set the evaporation DRY_ONLY option at runtime.

        @param flag: C{True} suppresses evaporation during rainfall.
        @type flag: bool
        @return: None
        @rtype: None
        """
        self._set(SWMMSystemProperties.EVAP_DRY_ONLY, 1.0 if flag else 0.0)

    def get_evap_dry_only(self) -> bool:
        """Return the evaporation DRY_ONLY option.

        @return: C{True} if evaporation is suppressed during rainfall.
        @rtype: bool
        """
        return bool(self._get(SWMMSystemProperties.EVAP_DRY_ONLY))

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
    def routing_totals(self) -> Dict[str, float]:
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
    def runoff_totals(self) -> Dict[str, float]:
        """System-level surface runoff mass balance totals.

        Returns a dict with keys: rainfall, evap, infil, runoff, drains,
        runon, init_storage, final_storage, init_snow_cover,
        final_snow_cover, snow_removed, pct_error.
        """
        return self._solver.get_runoff_totals()

    @property
    def mass_balance_error(self) -> Tuple[float, float, float]:
        """Mass balance errors as (runoff_err%, flow_err%, quality_err%)."""
        return self._solver.get_mass_balance_error()

    def __repr__(self) -> str:
        return "LegacySystem()"
