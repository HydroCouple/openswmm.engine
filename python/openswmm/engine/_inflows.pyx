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

    # =========================================================================
    # RDII inflows
    # =========================================================================

    def add_rdii(self, node, str uh_name, double area) -> None:
        cdef int n = _resolve_node(self._solver, node)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = uh_name.encode('utf-8')
        _check(swmm_rdii_add(h, n, b, area))

    def get_rdii(self, int idx) -> RDIIEntry:
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

    # =========================================================================
    # Unit hydrographs ([HYDROGRAPHS])
    # =========================================================================

    def add_hydrograph(self, str uh_name, int month, int response,
                       double r, double t, double k,
                       *,
                       double dmax=0.0, double drecov=0.0, double dinit=0.0) -> None:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = uh_name.encode('utf-8')
        _check(swmm_hydrograph_add(
            h, b, month, response, r, t, k, dmax, drecov, dinit))

    def get_hydrograph(self, int idx) -> HydrographEntry:
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

    def add_hydrograph_gage(self, str uh_name, str gage_name) -> None:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b_uh = uh_name.encode('utf-8')
        cdef bytes b_gage = gage_name.encode('utf-8')
        _check(swmm_hydrograph_add_gage(h, b_uh, b_gage))

    def get_hydrograph_gage(self, int idx) -> HydrographGageEntry:
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
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef char buf[128]
        _check(swmm_hydrograph_group_id(h, idx, buf, 128))
        return buf.decode('utf-8')

    # =========================================================================
    # RDII decay
    # =========================================================================

    def add_rdii_decay(self, str uh_name, int response,
                       double k_dep, double k_0, double k_T,
                       double T_ref, double theta_rec, double T_freeze) -> None:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = uh_name.encode('utf-8')
        _check(swmm_rdii_decay_add(
            h, b, response, k_dep, k_0, k_T, T_ref, theta_rec, T_freeze))

    def get_rdii_decay(self, int idx) -> RDIIDecayEntry:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef char buf[128]
        cdef int response = 0
        cdef double k_dep = 0, k_0 = 0, k_T = 0
        cdef double T_ref = 0, theta_rec = 0, T_freeze = 0
        _check(swmm_rdii_decay_get(
            h, idx, buf, 128, &response,
            &k_dep, &k_0, &k_T, &T_ref, &theta_rec, &T_freeze))
        return RDIIDecayEntry(
            uh_name=buf.decode('utf-8'), response=response,
            k_dep=k_dep, k_0=k_0, k_T=k_T,
            T_ref=T_ref, theta_rec=theta_rec, T_freeze=T_freeze)

    @property
    def rdii_decay_count(self) -> int:
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_rdii_decay_count(h)

    # ---- Repr -----------------------------------------------------

    def __repr__(self) -> str:
        try:
            return (f"<Inflows ext={self.external_count} dwf={self.dwf_count} "
                    f"rdii={self.rdii_count}>")
        except Exception:
            return "<Inflows (engine closed)>"
