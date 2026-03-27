"""
Inflow Access
=============

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

The :class:`Inflows` class provides methods for adding external inflows,
dry-weather flows, and RDII to the model.

.. code-block:: python

    from openswmm.engine import Solver, Inflows

    with Solver("model.inp", "model.rpt", "model.out") as s:
        inflows = Inflows(s)
        inflows.add_external(0, "FLOW", ts_name="InflowTS")
        inflows.add_dwf(0, "FLOW", 1.5)
"""

# cython: language_level=3

from ._common cimport *


class Inflows:
    """Add external inflows, dry-weather flows, and RDII to the model.

    :param solver: An active :class:`~openswmm.engine.Solver` instance.
        The solver must remain alive for the lifetime of this object.
    """

    def __init__(self, solver):
        self._solver = solver

    # ------------------------------------------------------------------
    # External inflows
    # ------------------------------------------------------------------

    def add_external(self, int node_idx, str constituent, str ts_name="",
                     str type="FLOW", double m_factor=1.0, double s_factor=1.0,
                     double baseline=0.0, str pattern=""):
        """Add an external inflow to a node.

        :param node_idx: Node index.
        :param constituent: Constituent name (e.g. ``"FLOW"`` or a pollutant ID).
        :param ts_name: Time-series name (empty string if none).
        :param type: Inflow type string (default ``"FLOW"``).
        :param m_factor: Multiplier factor (default 1.0).
        :param s_factor: Scale factor (default 1.0).
        :param baseline: Baseline value (default 0.0).
        :param pattern: Pattern name (empty string if none).
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b_constituent = constituent.encode('utf-8')
        cdef bytes b_ts_name = ts_name.encode('utf-8')
        cdef bytes b_type = type.encode('utf-8')
        cdef bytes b_pattern = pattern.encode('utf-8')
        _check(swmm_ext_inflow_add(h, node_idx, b_constituent, b_ts_name,
                                    b_type, m_factor, s_factor, baseline,
                                    b_pattern))

    def ext_inflow_count(self) -> int:
        """Return the number of external inflows in the model."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_ext_inflow_count(h)

    # ------------------------------------------------------------------
    # Dry-weather flows
    # ------------------------------------------------------------------

    def add_dwf(self, int node_idx, str constituent, double avg_value,
                str pat1="", str pat2="", str pat3="", str pat4=""):
        """Add a dry-weather flow to a node.

        :param node_idx: Node index.
        :param constituent: Constituent name (e.g. ``"FLOW"``).
        :param avg_value: Average DWF value.
        :param pat1: Monthly pattern name (empty string if none).
        :param pat2: Daily pattern name (empty string if none).
        :param pat3: Hourly pattern name (empty string if none).
        :param pat4: Weekend pattern name (empty string if none).
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b_constituent = constituent.encode('utf-8')
        cdef bytes b_pat1 = pat1.encode('utf-8')
        cdef bytes b_pat2 = pat2.encode('utf-8')
        cdef bytes b_pat3 = pat3.encode('utf-8')
        cdef bytes b_pat4 = pat4.encode('utf-8')
        _check(swmm_dwf_add(h, node_idx, b_constituent, avg_value,
                             b_pat1, b_pat2, b_pat3, b_pat4))

    def dwf_count(self) -> int:
        """Return the number of dry-weather flows in the model."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_dwf_count(h)

    # ------------------------------------------------------------------
    # RDII
    # ------------------------------------------------------------------

    def add_rdii(self, int node_idx, str uh_name, double area):
        """Add an RDII inflow to a node.

        :param node_idx: Node index.
        :param uh_name: Unit hydrograph name.
        :param area: Sewershed area contributing RDII.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b_uh_name = uh_name.encode('utf-8')
        _check(swmm_rdii_add(h, node_idx, b_uh_name, area))

    def rdii_count(self) -> int:
        """Return the number of RDII inflows in the model."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_rdii_count(h)
