"""
Inflow Access
=============

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
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

    @ivar _solver: The owning solver instance whose engine handle is used
        for every C call.
    @type _solver: L{Solver}

    @param solver: An active L{Solver} instance. The solver must remain
        alive for the lifetime of this object.
    @type solver: L{Solver}
    """

    def __init__(self, solver):
        """Construct an L{Inflows} accessor bound to C{solver}.

        @param solver: An active L{Solver} instance whose engine handle
            will be used for all subsequent inflow operations.
        @type solver: L{Solver}
        """
        self._solver = solver

    # ====================================================================
    # External inflows
    # ====================================================================

    def add_external(self, int node_idx, str constituent, str ts_name="",
                     str type="FLOW", double m_factor=1.0, double s_factor=1.0,
                     double baseline=0.0, str pattern=""):
        """Add an external inflow to a node.

        @param node_idx: Node index.
        @type node_idx: int
        @param constituent: Constituent name (e.g. C{"FLOW"} or a pollutant ID).
        @type constituent: str
        @param ts_name: Time-series name (empty string if none).
        @type ts_name: str
        @param type: Inflow type string (default C{"FLOW"}).
        @type type: str
        @param m_factor: Multiplier factor (default C{1.0}).
        @type m_factor: float
        @param s_factor: Scale factor (default C{1.0}).
        @type s_factor: float
        @param baseline: Baseline value (default C{0.0}).
        @type baseline: float
        @param pattern: Pattern name (empty string if none).
        @type pattern: str
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_ext_inflow_add} call
            returns a non-zero error code.
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
        """Return the number of external inflows in the model.

        @return: External inflow count.
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_ext_inflow_count(h)

    # ====================================================================
    # Dry-weather flow (DWF)
    # ====================================================================

    def add_dwf(self, int node_idx, str constituent, double avg_value,
                str pat1="", str pat2="", str pat3="", str pat4=""):
        """Add a dry-weather flow to a node.

        @param node_idx: Node index.
        @type node_idx: int
        @param constituent: Constituent name (e.g. C{"FLOW"}).
        @type constituent: str
        @param avg_value: Average DWF value.
        @type avg_value: float
        @param pat1: Monthly pattern name (empty string if none).
        @type pat1: str
        @param pat2: Daily pattern name (empty string if none).
        @type pat2: str
        @param pat3: Hourly pattern name (empty string if none).
        @type pat3: str
        @param pat4: Weekend pattern name (empty string if none).
        @type pat4: str
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_dwf_add} call returns
            a non-zero error code.
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
        """Return the number of dry-weather flows in the model.

        @return: DWF count.
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_dwf_count(h)

    # ====================================================================
    # RDII
    # ====================================================================

    def add_rdii(self, int node_idx, str uh_name, double area):
        """Add an RDII inflow to a node.

        @param node_idx: Node index.
        @type node_idx: int
        @param uh_name: Unit hydrograph name.
        @type uh_name: str
        @param area: Sewershed area contributing RDII.
        @type area: float
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_rdii_add} call returns
            a non-zero error code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b_uh_name = uh_name.encode('utf-8')
        _check(swmm_rdii_add(h, node_idx, b_uh_name, area))

    def rdii_count(self) -> int:
        """Return the number of RDII inflows in the model.

        @return: RDII inflow count.
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_rdii_count(h)
