"""
Inflows (Pythonic v1 surface)
=============================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

The :class:`Inflows` view exposes external inflows, dry-weather flows,
RDII, unit hydrographs, gage assignments, and IA-decay entries on the
engine. Reach via ``solver.inflows``.

The C API supports ``add`` plus ``count`` plus a per-row ``get`` for
RDII / hydrograph / gage / decay rows; it does **not** support per-row
``delete`` / ``set`` for external inflows or DWF. The Pythonic surface
mirrors that constraint: ``add_*`` and ``*_count`` are flat methods,
plus ``get_*`` where supported. Object selectors accept ``int | str``.

.. code-block:: python

    from openswmm.engine import Solver

    with Solver("model.inp") as s:
        s.inflows.add_external("J1", "FLOW", ts_name="ts_in")
        s.inflows.add_dwf("J1", "FLOW", avg_value=0.5, hourly_pattern="dly1")
        s.inflows.add_rdii("J1", uh_name="UH1", area=2.5)
"""

# cython: language_level=3

from typing import NamedTuple, Optional

from ._common cimport *


cdef inline SWMM_Engine _h(solver):
    return <SWMM_Engine><size_t>solver.handle


cdef inline int _resolve_node(solver, key) except -1:
    return _resolve_index(
        _h(solver), key, swmm_node_index, swmm_node_count, "Node")


class RDIIEntry(NamedTuple):
    node_index: int
    uh_name: str
    area: float


class HydrographEntry(NamedTuple):
    uh_name: str
    month: int
    response: int
    r: float
    t: float
    k: float
    dmax: float
    drecov: float
    dinit: float


class HydrographGageEntry(NamedTuple):
    uh_name: str
    gage_name: str


class RDIIDecayEntry(NamedTuple):
    uh_name: str
    response: int
    k_dep: float
    k_0: float
    k_T: float
    T_ref: float
    theta_rec: float
    T_freeze: float
    snow_on: bool = False
    snow_T: float = 1.0
    snow_ddf: float = 0.0


class Inflows:
    """``solver.inflows`` — entry point for inflow editing & inspection."""

    def __init__(self, solver):
        self._solver = solver

    # =========================================================================
    # External inflows ([INFLOWS])
    # =========================================================================

    def add_external(self, node, str constituent, *,
                     str ts_name="", str type="FLOW",
                     double m_factor=1.0, double s_factor=1.0,
                     double baseline=0.0, str pattern="") -> None:
        """Add an external inflow.

        :param node: ``int | str`` node selector.
        :param constituent: ``"FLOW"`` or a pollutant id.
        :param ts_name: Time series id (empty for none).
        :param type: Inflow type tag (default ``"FLOW"``).
        :param m_factor: Multiplier.
        :param s_factor: Scale factor.
        :param baseline: Baseline value.
        :param pattern: Pattern id (empty for none).
        """
        cdef int n = _resolve_node(self._solver, node)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b_constituent = constituent.encode('utf-8')
        cdef bytes b_ts_name = ts_name.encode('utf-8')
        cdef bytes b_type = type.encode('utf-8')
        cdef bytes b_pattern = pattern.encode('utf-8')
        _check(swmm_ext_inflow_add(
            h, n, b_constituent, b_ts_name, b_type,
            m_factor, s_factor, baseline, b_pattern))

    @property
    def external_count(self) -> int:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_ext_inflow_count(h)

    def get_external(self, int entry_idx):
        """Read one external-inflow row by entry index.

        @param entry_idx: Zero-based row index (0..L{external_count}-1).
        @return: C{(node_index, constituent, ts_name, type, m_factor,
            s_factor, baseline, pattern)}.
        @rtype: tuple
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int node_idx = -1
        cdef char c_buf[128]
        cdef char ts_buf[128]
        cdef char ty_buf[128]
        cdef char pat_buf[128]
        cdef double m_factor = 0.0, s_factor = 0.0, baseline = 0.0
        _check(swmm_ext_inflow_get(
            h, entry_idx, &node_idx, c_buf, 128, ts_buf, 128, ty_buf, 128,
            &m_factor, &s_factor, &baseline, pat_buf, 128))
        return (node_idx, c_buf.decode('utf-8'), ts_buf.decode('utf-8'),
                ty_buf.decode('utf-8'), m_factor, s_factor, baseline,
                pat_buf.decode('utf-8'))

    def remove_external(self, int entry_idx) -> None:
        """Remove the external-inflow row at C{entry_idx}.

        Indices shift after removal — re-query by row rather than caching.

        @param entry_idx: Zero-based row index.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_ext_inflow_remove(h, entry_idx))
        self._solver._bump_generation()

    def set_external_scale(self, int entry_idx, double scale) -> None:
        """Set an external-inflow row's timeseries scale factor at runtime.

        Takes effect on the next step (the inflow solver's per-step cache is
        refreshed) — a lighter mid-run edit than remove + re-add.

        @param entry_idx: Zero-based row index.
        @param scale: New timeseries scale factor.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_ext_inflow_set_scale(h, entry_idx, scale))

    def set_external_baseline(self, int entry_idx, double baseline) -> None:
        """Set an external-inflow row's constant baseline at runtime.

        @param entry_idx: Zero-based row index.
        @param baseline: New baseline value (inflow display units).
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_ext_inflow_set_baseline(h, entry_idx, baseline))

    # =========================================================================
    # Dry-weather flow ([DWF])
    # =========================================================================

    def add_dwf(self, node, str constituent, *,
                double avg_value=0.0,
                str monthly_pattern="", str daily_pattern="",
                str hourly_pattern="", str weekend_pattern="") -> None:
        """Add a dry-weather flow.

        :param node: ``int | str`` node selector.
        :param constituent: ``"FLOW"`` or a pollutant id.
        :param avg_value: Average value.
        :param monthly_pattern / daily_pattern / hourly_pattern / weekend_pattern: Pattern ids.
        """
        cdef int n = _resolve_node(self._solver, node)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b_const = constituent.encode('utf-8')
        cdef bytes b_p1 = monthly_pattern.encode('utf-8')
        cdef bytes b_p2 = daily_pattern.encode('utf-8')
        cdef bytes b_p3 = hourly_pattern.encode('utf-8')
        cdef bytes b_p4 = weekend_pattern.encode('utf-8')
        _check(swmm_dwf_add(h, n, b_const, avg_value,
                            b_p1, b_p2, b_p3, b_p4))

    @property
    def dwf_count(self) -> int:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_dwf_count(h)

    def get_dwf(self, int entry_idx):
        """Read one dry-weather-flow row by entry index.

        @param entry_idx: Zero-based row index (0..L{dwf_count}-1).
        @return: C{(node_index, constituent, avg_value, monthly, daily,
            hourly, weekend)} where the four pattern entries are pattern-id
            strings (empty when unset).
        @rtype: tuple
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int node_idx = -1
        cdef char c_buf[128]
        cdef char p1[128]
        cdef char p2[128]
        cdef char p3[128]
        cdef char p4[128]
        cdef double avg_value = 0.0
        _check(swmm_dwf_get(
            h, entry_idx, &node_idx, c_buf, 128, &avg_value,
            p1, 128, p2, 128, p3, 128, p4, 128))
        return (node_idx, c_buf.decode('utf-8'), avg_value,
                p1.decode('utf-8'), p2.decode('utf-8'),
                p3.decode('utf-8'), p4.decode('utf-8'))

    def remove_dwf(self, int entry_idx) -> None:
        """Remove the dry-weather-flow row at C{entry_idx}.

        @param entry_idx: Zero-based row index.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_dwf_remove(h, entry_idx))
        self._solver._bump_generation()

    def set_dwf_baseline(self, int entry_idx, double avg_value) -> None:
        """Set a DWF row's average (baseline) value at runtime.

        Takes effect on the next step (the inflow solver's per-step cache is
        refreshed).

        @param entry_idx: Zero-based row index.
        @param avg_value: New average value (flow units for FLOW).
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_dwf_set_baseline(h, entry_idx, avg_value))

    # =========================================================================
    # RDII inflows
    # =========================================================================

    def add_rdii(self, node, str uh_name, double area) -> None:
        """Add an RDII inflow at *node* driven by unit-hydrograph group *uh_name* over sewered *area*."""
        cdef int n = _resolve_node(self._solver, node)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = uh_name.encode('utf-8')
        _check(swmm_rdii_add(h, n, b, area))

    def get_rdii(self, int idx) -> RDIIEntry:
        """Return the :class:`RDIIEntry` at *idx*."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int node_idx = -1
        cdef char buf[128]
        cdef double area = 0.0
        _check(swmm_rdii_get(h, idx, &node_idx, buf, 128, &area))
        return RDIIEntry(
            node_index=node_idx, uh_name=buf.decode('utf-8'), area=area)

    @property
    def rdii_count(self) -> int:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_rdii_count(h)

    def remove_rdii(self, int entry_idx) -> None:
        """Remove the RDII assignment row at C{entry_idx}.

        @param entry_idx: Zero-based row index (0..L{rdii_count}-1).
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_rdii_remove(h, entry_idx))
        self._solver._bump_generation()

    # =========================================================================
    # Unit hydrographs ([HYDROGRAPHS])
    # =========================================================================

    def add_hydrograph(self, str uh_name, int month, int response,
                       double r, double t, double k,
                       *,
                       double dmax=0.0, double drecov=0.0, double dinit=0.0) -> None:
        """Add an RTK unit-hydrograph (response R/T/K) to group *uh_name* for *month*."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = uh_name.encode('utf-8')
        _check(swmm_hydrograph_add(
            h, b, month, response, r, t, k, dmax, drecov, dinit))

    def get_hydrograph(self, int idx) -> HydrographEntry:
        """Return the :class:`HydrographEntry` at *idx*."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef char buf[128]
        cdef int month = 0, response = 0
        cdef double r = 0.0, t = 0.0, k = 0.0
        cdef double dmax = 0.0, drecov = 0.0, dinit = 0.0
        _check(swmm_hydrograph_get(
            h, idx, buf, 128, &month, &response,
            &r, &t, &k, &dmax, &drecov, &dinit))
        return HydrographEntry(
            uh_name=buf.decode('utf-8'), month=month, response=response,
            r=r, t=t, k=k, dmax=dmax, drecov=drecov, dinit=dinit)

    @property
    def hydrograph_count(self) -> int:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_hydrograph_count(h)

    def set_hydrograph_rtk(self, str uh_name, int month, int response,
                           double r, double t, double k) -> None:
        """Upsert the C{(R, T, K)} triplet for one (group, month, response).

        @param uh_name: Unit-hydrograph group name.
        @param month: C{0..11} (JAN..DEC) or C{-1} for ALL.
        @param response: C{0}=short, C{1}=medium, C{2}=long.
        @param r: Rainfall fraction that becomes RDII.
        @param t: Time to peak (hours).
        @param k: Recession-to-peak time ratio (base time = C{t*(1+k)}).
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = uh_name.encode('utf-8')
        _check(swmm_hydrograph_set_rtk(h, b, month, response, r, t, k))

    def set_hydrograph_ia(self, str uh_name, int month, int response,
                          double dmax, double drecov, double dinit) -> None:
        """Upsert linear initial-abstraction params for one (group, month, response).

        @param uh_name: Unit-hydrograph group name.
        @param month: C{0..11} (JAN..DEC) or C{-1} for ALL.
        @param response: C{0}=short, C{1}=medium, C{2}=long.
        @param dmax: Maximum initial-abstraction depth.
        @param drecov: Linear IA recovery rate.
        @param dinit: Initial IA already used.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = uh_name.encode('utf-8')
        _check(swmm_hydrograph_set_ia(h, b, month, response, dmax, drecov, dinit))

    def remove_hydrograph_entry(self, str uh_name, int month, int response) -> None:
        """Remove one (group, month, response) entry. Idempotent.

        @param uh_name: Unit-hydrograph group name.
        @param month: C{0..11} or C{-1}.
        @param response: C{0}=short, C{1}=medium, C{2}=long.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = uh_name.encode('utf-8')
        _check(swmm_hydrograph_remove_entry(h, b, month, response))
        self._solver._bump_generation()

    def remove_hydrograph_group(self, str uh_name) -> None:
        """Remove an entire unit-hydrograph group.

        @param uh_name: Unit-hydrograph group name.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = uh_name.encode('utf-8')
        _check(swmm_hydrograph_remove_group(h, b))
        self._solver._bump_generation()

    def clear_hydrograph_group_months(self, str uh_name) -> None:
        """Clear all monthly entries from a unit-hydrograph group.

        @param uh_name: Unit-hydrograph group name.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = uh_name.encode('utf-8')
        _check(swmm_hydrograph_clear_group_months(h, b))
        self._solver._bump_generation()

    def rename_hydrograph_group(self, int idx, str new_id) -> None:
        """Rename the unit-hydrograph group at C{idx}.

        @param idx: Zero-based group index (0..L{hydrograph_group_count}-1).
        @param new_id: New group identifier.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = new_id.encode('utf-8')
        _check(swmm_hydrograph_group_rename(h, idx, b))
        self._solver._bump_generation()

    def set_hydrograph_gage(self, str uh_name, str gage_name) -> None:
        """Assign the rain gage that drives a unit-hydrograph group.

        @param uh_name: Unit-hydrograph group name.
        @param gage_name: Rain gage identifier.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b_uh = uh_name.encode('utf-8')
        cdef bytes b_gage = gage_name.encode('utf-8')
        _check(swmm_hydrograph_set_gage(h, b_uh, b_gage))

    def add_hydrograph_gage(self, str uh_name, str gage_name) -> None:
        """Associate rain *gage_name* with unit-hydrograph group *uh_name*."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b_uh = uh_name.encode('utf-8')
        cdef bytes b_gage = gage_name.encode('utf-8')
        _check(swmm_hydrograph_add_gage(h, b_uh, b_gage))

    def get_hydrograph_gage(self, int idx) -> HydrographGageEntry:
        """Return the :class:`HydrographGageEntry` at *idx*."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef char uh_buf[128]
        cdef char g_buf[128]
        _check(swmm_hydrograph_get_gage(h, idx, uh_buf, 128, g_buf, 128))
        return HydrographGageEntry(
            uh_name=uh_buf.decode('utf-8'), gage_name=g_buf.decode('utf-8'))

    @property
    def hydrograph_gage_count(self) -> int:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_hydrograph_gage_count(h)

    @property
    def hydrograph_group_count(self) -> int:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_hydrograph_group_count(h)

    def get_hydrograph_group_id(self, int idx) -> str:
        """Return the ID of the unit-hydrograph group at *idx*."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef char buf[128]
        _check(swmm_hydrograph_group_id(h, idx, buf, 128))
        return buf.decode('utf-8')

    # =========================================================================
    # RDII decay
    # =========================================================================

    def add_rdii_decay(self, str uh_name, int response,
                       double k_dep, double k_0, double k_T,
                       double T_ref, double theta_rec, double T_freeze,
                       bint snow_on=False, double snow_T=1.0,
                       double snow_ddf=0.0) -> None:
        """Add an exponential-decay RDII entry to unit-hydrograph *uh_name*.

        Optional degree-day snow model: with C{snow_on}, precipitation at
        temperatures at or below C{snow_T} (deg C) accumulates as SWE, and
        above it melts at C{snow_ddf} (project rain-depth unit/degC/day).
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = uh_name.encode('utf-8')
        _check(swmm_rdii_decay_add(
            h, b, response, k_dep, k_0, k_T, T_ref, theta_rec, T_freeze,
            1 if snow_on else 0, snow_T, snow_ddf))

    def get_rdii_decay(self, int idx) -> RDIIDecayEntry:
        """Return the :class:`RDIIDecayEntry` at *idx*."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef char buf[128]
        cdef int response = 0
        cdef double k_dep = 0, k_0 = 0, k_T = 0
        cdef double T_ref = 0, theta_rec = 0, T_freeze = 0
        cdef int snow_on = 0
        cdef double snow_T = 0, snow_ddf = 0
        _check(swmm_rdii_decay_get(
            h, idx, buf, 128, &response,
            &k_dep, &k_0, &k_T, &T_ref, &theta_rec, &T_freeze,
            &snow_on, &snow_T, &snow_ddf))
        return RDIIDecayEntry(
            uh_name=buf.decode('utf-8'), response=response,
            k_dep=k_dep, k_0=k_0, k_T=k_T,
            T_ref=T_ref, theta_rec=theta_rec, T_freeze=T_freeze,
            snow_on=bool(snow_on), snow_T=snow_T, snow_ddf=snow_ddf)

    @property
    def rdii_decay_count(self) -> int:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_rdii_decay_count(h)

    def set_rdii_decay(self, str uh_name, int response,
                       double k_dep, double k_0, double k_T,
                       double T_ref, double theta_rec, double T_freeze,
                       bint snow_on=False, double snow_T=1.0,
                       double snow_ddf=0.0) -> None:
        """Upsert the IA-decay row for one (group, response).

        Like L{add_rdii_decay} but overwrites an existing row instead of
        erroring when one already exists.

        @param uh_name: Unit-hydrograph group name.
        @param response: C{0}=short, C{1}=medium, C{2}=long.
        @param k_dep: Depletion rate (1/project rain-depth unit: 1/in for
            US-unit projects, 1/mm for SI).
        @param k_0: Additive base recovery rate (1/hr).
        @param k_T: Thermal recovery coefficient (1/hr).
        @param T_ref: Reference temperature (deg C).
        @param theta_rec: Recovery temperature sensitivity (1/deg C).
        @param T_freeze: Recovery suppressed below this temperature (deg C).
        @param snow_on: Enable the degree-day snow model for this row.
        @param snow_T: Rain/snow partition threshold & melt base (deg C).
        @param snow_ddf: Degree-day melt factor (project rain-depth
            unit/degC/day); must be >= 0.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = uh_name.encode('utf-8')
        _check(swmm_rdii_decay_set(
            h, b, response, k_dep, k_0, k_T, T_ref, theta_rec, T_freeze,
            1 if snow_on else 0, snow_T, snow_ddf))

    def remove_rdii_decay(self, str uh_name, int response) -> None:
        """Remove the IA-decay row for one (group, response). Idempotent.

        @param uh_name: Unit-hydrograph group name.
        @param response: C{0}=short, C{1}=medium, C{2}=long.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = uh_name.encode('utf-8')
        _check(swmm_rdii_decay_remove(h, b, response))
        self._solver._bump_generation()

    # ---- Repr -----------------------------------------------------

    def __repr__(self) -> str:
        try:
            return (f"<Inflows ext={self.external_count} dwf={self.dwf_count} "
                    f"rdii={self.rdii_count}>")
        except Exception:
            return "<Inflows (engine closed)>"
