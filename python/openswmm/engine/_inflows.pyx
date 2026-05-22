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

    def get_rdii(self, int entry_idx):
        """Read back an RDII assignment by entry index.

        @param entry_idx: Zero-based index into the assignment list.
        @type entry_idx: int
        @return: C{(node_idx, uh_name, area)}.
        @rtype: tuple[int, str, float]
        @raise EngineError: If the underlying C call returns a non-zero code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int    node_idx = 0
        cdef double area     = 0.0
        cdef char   buf[256]
        _check(swmm_rdii_get(h, entry_idx, &node_idx, buf, 256, &area))
        return node_idx, buf.decode('utf-8'), area

    def rdii_count(self) -> int:
        """Return the number of RDII inflows in the model.

        @return: RDII inflow count.
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_rdii_count(h)

    # ====================================================================
    # Unit hydrographs ([HYDROGRAPHS])
    # ====================================================================

    def add_hydrograph(self, str uh_name, int month, int response,
                       double r, double t, double k,
                       double dmax=0.0, double drecov=0.0, double dinit=0.0):
        """Add a unit hydrograph parameter line.

        @param uh_name: Unit hydrograph group name.
        @type uh_name: str
        @param month: 0..11 = JAN..DEC, or C{-1} for ALL.
        @type month: int
        @param response: 0=SHORT, 1=MEDIUM, 2=LONG.
        @type response: int
        @param r: Fraction of rainfall that becomes RDII.
        @type r: float
        @param t: Time to peak in hours.
        @type t: float
        @param k: Ratio of base time to peak time (>= 1).
        @type k: float
        @param dmax: Maximum initial-abstraction depth.
        @type dmax: float
        @param drecov: Linear-model IA recovery rate. Ignored when an
            exponential decay row exists for this (UH, response) pair.
        @type drecov: float
        @param dinit: Initial IA already used.
        @type dinit: float
        @raise EngineError: If the underlying C call returns a non-zero code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b_uh = uh_name.encode('utf-8')
        _check(swmm_hydrograph_add(h, b_uh, month, response,
                                    r, t, k, dmax, drecov, dinit))

    def get_hydrograph(self, int entry_idx):
        """Read back a hydrograph parameter entry by index.

        @param entry_idx: Zero-based index into the parameter list.
        @type entry_idx: int
        @return: A dict with keys
            C{uh_name}, C{month}, C{response}, C{r}, C{t}, C{k},
            C{dmax}, C{drecov}, C{dinit}.
        @rtype: dict
        @raise EngineError: If the underlying C call returns a non-zero code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef char buf[256]
        cdef int  month    = 0
        cdef int  response = 0
        cdef double r=0.0, t=0.0, k=0.0
        cdef double dmax=0.0, drecov=0.0, dinit=0.0
        _check(swmm_hydrograph_get(h, entry_idx, buf, 256,
                                    &month, &response, &r, &t, &k,
                                    &dmax, &drecov, &dinit))
        return {
            'uh_name':  buf.decode('utf-8'),
            'month':    month,
            'response': response,
            'r':        r,
            't':        t,
            'k':        k,
            'dmax':     dmax,
            'drecov':   drecov,
            'dinit':    dinit,
        }

    def hydrograph_count(self) -> int:
        """Return the number of hydrograph parameter entries.

        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_hydrograph_count(h)

    def add_hydrograph_gage(self, str uh_name, str gage_name):
        """Assign a rain gage to a unit hydrograph group.

        @param uh_name: Unit hydrograph group name.
        @type uh_name: str
        @param gage_name: Rain gage name.
        @type gage_name: str
        @raise EngineError: If the underlying C call returns a non-zero code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b_uh = uh_name.encode('utf-8')
        cdef bytes b_g  = gage_name.encode('utf-8')
        _check(swmm_hydrograph_add_gage(h, b_uh, b_g))

    def get_hydrograph_gage(self, int entry_idx):
        """Read back a UH-to-gage assignment by index.

        @param entry_idx: Zero-based index.
        @type entry_idx: int
        @return: C{(uh_name, gage_name)}.
        @rtype: tuple[str, str]
        @raise EngineError: If the underlying C call returns a non-zero code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef char uh_buf[256]
        cdef char g_buf[256]
        _check(swmm_hydrograph_get_gage(h, entry_idx,
                                         uh_buf, 256, g_buf, 256))
        return uh_buf.decode('utf-8'), g_buf.decode('utf-8')

    def hydrograph_gage_count(self) -> int:
        """Return the number of UH-to-gage assignments.

        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_hydrograph_gage_count(h)

    # ====================================================================
    # Exponential IA decay ([RDII_DECAY])
    # ====================================================================

    def add_rdii_decay(self, str uh_name, int response,
                       double k_dep, double k_0, double k_T,
                       double T_ref=10.0, double theta_rec=0.0,
                       double T_freeze=0.0):
        """Add an exponential-decay row for a (UH, response) pair.

        Physically-based replacement for the legacy linear IA recovery.
        Depletion during storms follows
        S{exp}(-C{k_dep} * rainfall); recovery during dry periods uses an
        additive base+thermal rate
        C{k_rec(T) = k_0 + k_T * exp(theta_rec * (T - T_ref))} with
        suppression when C{T <= T_freeze}.

        @param uh_name: UH group name (must match a hydrograph entry).
        @type uh_name: str
        @param response: 0=SHORT, 1=MEDIUM, 2=LONG.
        @type response: int
        @param k_dep: Depletion rate (1/project-depth-unit).
        @type k_dep: float
        @param k_0: Base recovery rate (1/hr).
        @type k_0: float
        @param k_T: Thermal recovery rate at C{T_ref} (1/hr).
        @type k_T: float
        @param T_ref: Reference temperature (deg C). Default 10.
        @type T_ref: float
        @param theta_rec: Temperature sensitivity (1/deg C).
        @type theta_rec: float
        @param T_freeze: Frozen-ground threshold (deg C). Default 0.
        @type T_freeze: float
        @raise EngineError: If the underlying C call returns a non-zero code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b_uh = uh_name.encode('utf-8')
        _check(swmm_rdii_decay_add(h, b_uh, response,
                                    k_dep, k_0, k_T,
                                    T_ref, theta_rec, T_freeze))

    def get_rdii_decay(self, int entry_idx):
        """Read back an exponential-decay row by index.

        @param entry_idx: Zero-based index into the decay-row list.
        @type entry_idx: int
        @return: A dict with keys
            C{uh_name}, C{response}, C{k_dep}, C{k_0}, C{k_T},
            C{T_ref}, C{theta_rec}, C{T_freeze}.
        @rtype: dict
        @raise EngineError: If the underlying C call returns a non-zero code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef char buf[256]
        cdef int  response = 0
        cdef double k_dep=0.0, k_0=0.0, k_T=0.0
        cdef double T_ref=0.0, theta_rec=0.0, T_freeze=0.0
        _check(swmm_rdii_decay_get(h, entry_idx, buf, 256,
                                    &response, &k_dep, &k_0, &k_T,
                                    &T_ref, &theta_rec, &T_freeze))
        return {
            'uh_name':   buf.decode('utf-8'),
            'response':  response,
            'k_dep':     k_dep,
            'k_0':       k_0,
            'k_T':       k_T,
            'T_ref':     T_ref,
            'theta_rec': theta_rec,
            'T_freeze':  T_freeze,
        }

    def rdii_decay_count(self) -> int:
        """Return the number of exponential-decay rows.

        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_rdii_decay_count(h)
