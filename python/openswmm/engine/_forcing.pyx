"""
Runtime forcing (Pythonic v1 surface)
=====================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

The :class:`Forcing` view, reached via ``solver.forcing``, applies
runtime overrides to node/link/subcatchment/gage state.

.. code-block:: python

    from openswmm.engine import Solver, ForcingMode, ForcingTarget

    with Solver("model.inp") as s:
        # One-shot: replaces the engine-computed value for the next step.
        s.forcing.node_lat_inflow("J1", 0.5)

        # Sticky: persists every step until cleared.
        s.forcing.node_lat_inflow(
            "J1", 0.5, mode=ForcingMode.REPLACE, persist=True)

        # Run.
        for _ in s.steps():
            pass

        # Clear.
        s.forcing.clear(ForcingTarget.NODE, "J1")
        s.forcing.clear_all()
"""

# cython: language_level=3

from ._common cimport *
from ._enums import ForcingMode, ForcingTarget, ForcingType


cdef inline SWMM_Engine _h(solver):
    return <SWMM_Engine><size_t>solver.handle


cdef inline int _resolve_node(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_node_index, swmm_node_count, "Node")


cdef inline int _resolve_link(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_link_index, swmm_link_count, "Link")


cdef inline int _resolve_subcatch(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_subcatch_index, swmm_subcatch_count,
        "Subcatchment")


cdef inline int _resolve_gage(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_gage_index, swmm_gage_count, "Gage")


cdef inline int _resolve_pollutant(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_pollutant_index, swmm_pollutant_count, "Pollutant")


class Forcing:
    """Runtime forcing accessor exposed as ``solver.forcing``.

    Every entry point takes:

    * The object as ``int | str`` (or a wrapper — its index is used).
    * ``mode`` defaulting to :attr:`ForcingMode.REPLACE`; can be
      :attr:`ForcingMode.ADD` for additive forcing.
    * ``persist`` defaulting to ``False`` (one-shot for the next step).

    All methods raise :class:`EngineError` on C API failure.
    """

    def __init__(self, solver):
        self._solver = solver

    # ---- Node forcing ---------------------------------------------

    def node_lat_inflow(self, node, double value, *,
                        mode=ForcingMode.REPLACE, bint persist=False) -> None:
        """Force a lateral inflow (flow units) at *node* for the current step; *mode* selects replace/add/scale and *persist* keeps it across steps."""
        cdef int i = _resolve_node(self._solver, node)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_node_lat_inflow(
            h, i, value, int(mode), 1 if persist else 0))

    def node_head_boundary(self, node, double value, *,
                           mode=ForcingMode.REPLACE, bint persist=False) -> None:
        """Force a head/stage boundary at *node* for the current step (see *mode* / *persist*)."""
        cdef int i = _resolve_node(self._solver, node)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_node_head_boundary(
            h, i, value, int(mode), 1 if persist else 0))

    def node_quality(self, node, pollutant, double mass_rate, *,
                     mode=ForcingMode.REPLACE, bint persist=False) -> None:
        """Force a pollutant mass-rate inflow of *pollutant* at *node* (see *mode* / *persist*)."""
        cdef int ni = _resolve_node(self._solver, node)
        cdef int pi = _resolve_pollutant(self._solver, pollutant)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_node_quality(
            h, ni, pi, mass_rate, int(mode), 1 if persist else 0))

    # ---- Link forcing ---------------------------------------------

    def link_flow(self, link, double value, *,
                  mode=ForcingMode.REPLACE, bint persist=False) -> None:
        """Force the flow through *link* for the current step (see *mode* / *persist*)."""
        cdef int i = _resolve_link(self._solver, link)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_link_flow(
            h, i, value, int(mode), 1 if persist else 0))

    def link_setting(self, link, double value, *,
                     mode=ForcingMode.REPLACE, bint persist=False) -> None:
        """Force the control setting (0-1) of *link* for the current step (see *mode* / *persist*)."""
        cdef int i = _resolve_link(self._solver, link)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_link_setting(
            h, i, value, int(mode), 1 if persist else 0))

    # ---- Subcatchment forcing -------------------------------------

    def subcatchment_rainfall(self, sub, double value, *,
                              mode=ForcingMode.REPLACE, bint persist=False) -> None:
        """Force the rainfall rate on subcatchment *sub* for the current step (see *mode* / *persist*)."""
        cdef int i = _resolve_subcatch(self._solver, sub)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_subcatch_rainfall(
            h, i, value, int(mode), 1 if persist else 0))

    def subcatchment_evap(self, sub, double value, *,
                          mode=ForcingMode.REPLACE, bint persist=False) -> None:
        """Prescribe a potential evapotranspiration (PET) rate on a subcatchment.

        The prescribed rate replaces (REPLACE) or augments (ADD) the
        climate-derived evaporation rate for the subcatchment's surface,
        LID, and groundwater upper-zone evaporation. A REPLACE rate is
        applied as-is, bypassing the DRY_ONLY option and monthly
        adjustments. Actual evaporation remains capped by available
        water, so losses are tracked through the normal runoff
        continuity totals.

        @param sub: Subcatchment id or index.
        @type sub: int or str
        @param value: PET rate in user units (in/day for US, mm/day for SI).
        @type value: float
        @param mode: L{ForcingMode.REPLACE} or L{ForcingMode.ADD}.
        @type mode: ForcingMode
        @param persist: C{False} (one-shot for the next step) or C{True}
            (persists every step until cleared).
        @type persist: bool
        @return: None
        @rtype: None
        """
        cdef int i = _resolve_subcatch(self._solver, sub)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_subcatch_evap(
            h, i, value, int(mode), 1 if persist else 0))

    def climate_evap_rate(self) -> float:
        """Return the current climate-derived evaporation rate.

        The broadcast rate the engine would apply in the absence of any
        PET forcing, including monthly adjustments (read-only). Intended
        for caller-side composition: read this rate, apply your own
        adjustment logic, and prescribe the result via
        L{subcatchment_evap}.

        @return: Evaporation rate in user units (in/day US, mm/day SI).
        @rtype: float
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_climate_get_evap_rate(h, &v))
        return v

    def subcatchment_snowfall(self, sub, double value, *,
                              mode=ForcingMode.REPLACE, bint persist=False) -> None:
        """Prescribe snowfall on a subcatchment.

        The prescribed rate replaces (REPLACE) or augments (ADD) the
        gage-derived, temperature-split snowfall used for snow pack
        accumulation, plowing, and melt computation. Only meaningful for
        subcatchments with an assigned snow pack.

        @param sub: Subcatchment id or index.
        @type sub: int or str
        @param value: Snowfall rate in user units (in/hr US, mm/hr SI), as
            snow water equivalent; must be >= 0 for REPLACE.
        @type value: float
        @param mode: L{ForcingMode.REPLACE} or L{ForcingMode.ADD}.
        @type mode: ForcingMode
        @param persist: C{False} (one-shot for the next step) or C{True}
            (persists every step until cleared).
        @type persist: bool
        @return: None
        @rtype: None
        """
        cdef int i = _resolve_subcatch(self._solver, sub)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_subcatch_snowfall(
            h, i, value, int(mode), 1 if persist else 0))

    # ---- Climate forcing (system-wide) ----------------------------

    def climate_temperature(self, double value, *,
                            mode=ForcingMode.REPLACE, bint persist=False) -> None:
        """Prescribe the air temperature used for snowmelt and
        temperature-derived evaporation.

        Applied before derived climate quantities (saturation vapor
        pressure, psychrometric constant, Hargreaves moving average) are
        computed, so all temperature consumers stay consistent. A REPLACE
        prescription replaces the climate data-source value and bypasses
        monthly adjustments; ADD augments it (for ADD, ``value`` is a
        temperature *delta*).

        @param value: Air temperature in user units (deg F US, deg C SI).
        @type value: float
        @param mode: L{ForcingMode.REPLACE} or L{ForcingMode.ADD}.
        @type mode: ForcingMode
        @param persist: C{False} (one-shot for the next step) or C{True}
            (persists every step until cleared).
        @type persist: bool
        @return: None
        @rtype: None
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_climate_temperature(
            h, value, int(mode), 1 if persist else 0))

    def get_climate_temperature(self) -> float:
        """Return the current air temperature (read-only).

        @return: Air temperature in user units (deg F US, deg C SI).
        @rtype: float
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_climate_get_temperature(h, &v))
        return v

    def climate_wind(self, double value, *,
                     mode=ForcingMode.REPLACE, bint persist=False) -> None:
        """Prescribe the wind speed used in rain-on-snow melt.

        A REPLACE prescription replaces the monthly/climate-file value;
        ADD augments it.

        @param value: Wind speed in user units (mph US, km/hr SI); must be
            >= 0 for REPLACE.
        @type value: float
        @param mode: L{ForcingMode.REPLACE} or L{ForcingMode.ADD}.
        @type mode: ForcingMode
        @param persist: C{False} (one-shot for the next step) or C{True}
            (persists every step until cleared).
        @type persist: bool
        @return: None
        @rtype: None
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_climate_wind(
            h, value, int(mode), 1 if persist else 0))

    def get_climate_wind_speed(self) -> float:
        """Return the current wind speed (read-only).

        @return: Wind speed in user units (mph US, km/hr SI).
        @rtype: float
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_climate_get_wind_speed(h, &v))
        return v

    def climate_evap(self, double value, *,
                     mode=ForcingMode.REPLACE, bint persist=False) -> None:
        """Prescribe the system-wide evaporation rate.

        Replaces (REPLACE) or augments (ADD) the climate-derived rate
        after all sources and monthly adjustments, so it reaches every
        consumer. Per-subcatchment L{subcatchment_evap} still takes
        precedence on its subcatchment.

        @param value: Evaporation rate in user units (in/day US,
            mm/day SI); must be >= 0 for REPLACE.
        @type value: float
        @param mode: L{ForcingMode.REPLACE} or L{ForcingMode.ADD}.
        @type mode: ForcingMode
        @param persist: C{False} (one-shot) or C{True} (sticky).
        @type persist: bool
        @return: None
        @rtype: None
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_climate_evap(
            h, value, int(mode), 1 if persist else 0))

    def climate_dry_only(self, bint flag) -> None:
        """Set the evaporation DRY_ONLY option at runtime.

        @param flag: C{True} suppresses evaporation during rainfall.
        @type flag: bool
        @return: None
        @rtype: None
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_climate_set_dry_only(h, 1 if flag else 0))

    def get_climate_dry_only(self) -> bool:
        """Return the evaporation DRY_ONLY option.

        @return: C{True} if evaporation is suppressed during rainfall.
        @rtype: bool
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_climate_get_dry_only(h, &v))
        return bool(v)

    def link_quality(self, link, pollutant, double value, *,
                     mode=ForcingMode.REPLACE, bint persist=False) -> None:
        """Force pollutant quality on a link.

        Same semantics as L{node_quality}: REPLACE sets the link
        concentration directly (not mass-balanced); ADD injects a mass
        rate (mass/sec) tracked in the quality mass-balance forcing
        bucket.

        @param link: Link id or index.
        @type link: int or str
        @param pollutant: Pollutant id or index.
        @type pollutant: int or str
        @param value: Concentration (REPLACE) or mass rate (ADD).
        @type value: float
        @param mode: L{ForcingMode.REPLACE} or L{ForcingMode.ADD}.
        @type mode: ForcingMode
        @param persist: C{False} (one-shot) or C{True} (sticky).
        @type persist: bool
        @return: None
        @rtype: None
        """
        cdef int li = _resolve_link(self._solver, link)
        cdef int pi = _resolve_pollutant(self._solver, pollutant)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_link_quality(
            h, li, pi, value, int(mode), 1 if persist else 0))

    # ---- Gage forcing ---------------------------------------------

    def gage_rainfall(self, gage, double value, *,
                      mode=ForcingMode.REPLACE, bint persist=False) -> None:
        """Force the rainfall rate reported by rain *gage* for the current step (see *mode* / *persist*)."""
        cdef int i = _resolve_gage(self._solver, gage)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_gage_rainfall(
            h, i, value, int(mode), 1 if persist else 0))

    # ---- Clearing -------------------------------------------------

    def clear(self, target, key) -> None:
        """Clear all forcing channels on one object.

        Maps the object-kind L{ForcingTarget} to every C-level
        C{SWMM_ForcingType} channel that applies to that kind and clears
        each one. (C{swmm_forcing_clear} takes a channel code, not an
        object-kind code — passing the target directly cleared the wrong
        channel.)

        @param target: L{ForcingTarget} (NODE/LINK/SUBCATCH/GAGE).
        @type target: ForcingTarget
        @param key: Object id or index, resolved against the matching domain.
        @type key: int or str
        @return: None
        @rtype: None
        """
        cdef int t = int(target)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int i
        if t == int(ForcingTarget.NODE):
            i = _resolve_node(self._solver, key)
            channels = (ForcingType.NODE_LAT_INFLOW,
                        ForcingType.NODE_HEAD_BOUNDARY,
                        ForcingType.NODE_QUALITY)
        elif t == int(ForcingTarget.LINK):
            i = _resolve_link(self._solver, key)
            channels = (ForcingType.LINK_FLOW, ForcingType.LINK_SETTING,
                        ForcingType.LINK_QUALITY)
        elif t == int(ForcingTarget.SUBCATCH):
            i = _resolve_subcatch(self._solver, key)
            channels = (ForcingType.SUBCATCH_RAINFALL,
                        ForcingType.SUBCATCH_EVAP,
                        ForcingType.SUBCATCH_SNOWFALL)
        elif t == int(ForcingTarget.GAGE):
            i = _resolve_gage(self._solver, key)
            channels = (ForcingType.GAGE_RAINFALL,)
        elif t == int(ForcingTarget.CLIMATE):
            i = 0  # system-wide; key is ignored
            channels = (ForcingType.CLIMATE_TEMPERATURE,
                        ForcingType.CLIMATE_WIND,
                        ForcingType.CLIMATE_EVAP)
        else:
            raise ValueError(f"unknown ForcingTarget {target!r}")
        for channel in channels:
            _check(swmm_forcing_clear(h, int(channel), i))

    def clear_all(self) -> None:
        """Clear all active runtime forcing overrides."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_clear_all(h))

    def __repr__(self) -> str:
        return f"<Forcing for {self._solver!r}>"
