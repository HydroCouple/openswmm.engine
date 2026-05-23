"""
Inflow Access
=============

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Type stubs for :mod:`openswmm.engine._inflows`.

The :class:`Inflows` class provides methods for adding external inflows,
dry-weather flows, and RDII to model nodes.
"""

from typing import Tuple, TypedDict

from ._solver import Solver


class HydrographEntry(TypedDict):
    """Parameters of a single C{[HYDROGRAPHS]} parameter row."""

    uh_name:  str
    month:    int    # -1 = ALL, otherwise 0..11 = JAN..DEC
    response: int    # 0 = SHORT, 1 = MEDIUM, 2 = LONG
    r:        float
    t:        float
    k:        float
    dmax:     float
    drecov:   float
    dinit:    float


class RDIIDecayEntry(TypedDict):
    """Parameters of a single C{[RDII_DECAY]} row (exponential IA model)."""

    uh_name:   str
    response:  int    # 0 = SHORT, 1 = MEDIUM, 2 = LONG
    k_dep:     float
    k_0:       float
    k_T:       float
    T_ref:     float
    theta_rec: float
    T_freeze:  float


class Inflows:
    """Add external inflows, dry-weather flows, and RDII to the model.

    @ivar _solver: The owning solver instance whose engine handle is used
        for every C call.
    @type _solver: L{Solver}

    Example::

        from openswmm.engine import Solver, Inflows

        with Solver("model.inp", "model.rpt", "model.out") as s:
            inflows = Inflows(s)
            inflows.add_external(0, "FLOW", ts_name="InflowTS")
            inflows.add_dwf(0, "FLOW", 1.5)

    @param solver: An active L{Solver} instance. The solver must remain
        alive for the lifetime of this object.
    @type solver: L{Solver}
    """

    def __init__(self, solver: Solver) -> None:
        """Construct an L{Inflows} accessor bound to C{solver}.

        @param solver: An active L{Solver} instance whose engine handle
            will be used for all subsequent inflow operations.
        @type solver: L{Solver}
        """
        ...

    # ====================================================================
    # External inflows
    # ====================================================================

    def add_external(
        self,
        node_idx: int,
        constituent: str,
        ts_name: str = "",
        type: str = "FLOW",
        m_factor: float = 1.0,
        s_factor: float = 1.0,
        baseline: float = 0.0,
        pattern: str = "",
    ) -> None:
        """Add an external inflow to a node.

        @param node_idx: Node index.
        @type node_idx: int
        @param constituent: Constituent name (e.g. C{"FLOW"} or a pollutant ID).
        @type constituent: str
        @param ts_name: Time-series name; empty string if none.
        @type ts_name: str
        @param type: Inflow type string (default C{"FLOW"}).
        @type type: str
        @param m_factor: Multiplier factor (default C{1.0}).
        @type m_factor: float
        @param s_factor: Scale factor (default C{1.0}).
        @type s_factor: float
        @param baseline: Baseline value (default C{0.0}).
        @type baseline: float
        @param pattern: Pattern name; empty string if none.
        @type pattern: str
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_ext_inflow_add} call
            returns a non-zero error code.
        """
        ...

    def ext_inflow_count(self) -> int:
        """Return the number of external inflows in the model.

        @return: External inflow count.
        @rtype: int
        """
        ...

    # ====================================================================
    # Dry-weather flow (DWF)
    # ====================================================================

    def add_dwf(
        self,
        node_idx: int,
        constituent: str,
        avg_value: float,
        pat1: str = "",
        pat2: str = "",
        pat3: str = "",
        pat4: str = "",
    ) -> None:
        """Add a dry-weather flow to a node.

        @param node_idx: Node index.
        @type node_idx: int
        @param constituent: Constituent name (e.g. C{"FLOW"}).
        @type constituent: str
        @param avg_value: Average DWF value.
        @type avg_value: float
        @param pat1: Monthly pattern name; empty string if none.
        @type pat1: str
        @param pat2: Daily pattern name; empty string if none.
        @type pat2: str
        @param pat3: Hourly pattern name; empty string if none.
        @type pat3: str
        @param pat4: Weekend pattern name; empty string if none.
        @type pat4: str
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_dwf_add} call returns
            a non-zero error code.
        """
        ...

    def dwf_count(self) -> int:
        """Return the number of dry-weather flows in the model.

        @return: DWF count.
        @rtype: int
        """
        ...

    # ====================================================================
    # RDII
    # ====================================================================

    def add_rdii(self, node_idx: int, uh_name: str, area: float) -> None:
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
        ...

    def get_rdii(self, entry_idx: int) -> Tuple[int, str, float]:
        """Read back an RDII assignment by entry index.

        @param entry_idx: Zero-based index.
        @type entry_idx: int
        @return: C{(node_idx, uh_name, area)}.
        @rtype: tuple[int, str, float]
        """
        ...

    def rdii_count(self) -> int:
        """Return the number of RDII inflows in the model.

        @return: RDII inflow count.
        @rtype: int
        """
        ...

    # ====================================================================
    # Unit hydrographs ([HYDROGRAPHS])
    # ====================================================================

    def add_hydrograph(
        self,
        uh_name: str,
        month: int,
        response: int,
        r: float,
        t: float,
        k: float,
        dmax: float = 0.0,
        drecov: float = 0.0,
        dinit: float = 0.0,
    ) -> None:
        """Add a unit hydrograph parameter line.

        @param uh_name: Unit hydrograph group name.
        @type uh_name: str
        @param month: C{-1} = ALL, or C{0..11} = JAN..DEC.
        @type month: int
        @param response: C{0} = SHORT, C{1} = MEDIUM, C{2} = LONG.
        @type response: int
        @param r: Fraction of rainfall that becomes RDII.
        @param t: Time to peak in hours.
        @param k: Ratio of base time to peak time (>= 1).
        @param dmax: Maximum initial-abstraction depth.
        @param drecov: Linear-model IA recovery rate. Ignored when an
            exponential decay row exists for this C{(uh_name, response)} pair.
        @param dinit: Initial IA already used.
        """
        ...

    def get_hydrograph(self, entry_idx: int) -> HydrographEntry:
        """Read back a hydrograph parameter entry by index.

        @rtype: L{HydrographEntry}
        """
        ...

    def hydrograph_count(self) -> int:
        """Return the number of hydrograph parameter entries.

        @rtype: int
        """
        ...

    def add_hydrograph_gage(self, uh_name: str, gage_name: str) -> None:
        """Assign a rain gage to a unit hydrograph group."""
        ...

    def get_hydrograph_gage(self, entry_idx: int) -> Tuple[str, str]:
        """Read back a UH-to-gage assignment.

        @return: C{(uh_name, gage_name)}.
        @rtype: tuple[str, str]
        """
        ...

    def hydrograph_gage_count(self) -> int:
        """Return the number of UH-to-gage assignments.

        @rtype: int
        """
        ...

    def hydrograph_group_count(self) -> int:
        """Return the number of unique unit-hydrograph group names defined.

        @rtype: int
        """
        ...

    def get_hydrograph_group_id(self, idx: int) -> str:
        """Read back the name of a unit-hydrograph group by index.

        @param idx: Zero-based group index
            (C{0..hydrograph_group_count()-1}).
        @type idx: int
        @return: The group name.
        @rtype: str
        """
        ...

    # ====================================================================
    # Exponential IA decay ([RDII_DECAY])
    # ====================================================================

    def add_rdii_decay(
        self,
        uh_name: str,
        response: int,
        k_dep: float,
        k_0: float,
        k_T: float,
        T_ref: float = 10.0,
        theta_rec: float = 0.0,
        T_freeze: float = 0.0,
    ) -> None:
        """Add an exponential-decay row for a C{(uh_name, response)} pair.

        Physically-based replacement for the legacy linear IA recovery.
        Depletion during storms follows S{exp}(-C{k_dep} * rainfall);
        recovery during dry periods uses an additive base+thermal rate
        C{k_rec(T) = k_0 + k_T * exp(theta_rec * (T - T_ref))} with
        suppression when C{T <= T_freeze}.

        @param uh_name: UH group name (must match a hydrograph entry).
        @param response: C{0} = SHORT, C{1} = MEDIUM, C{2} = LONG.
        @param k_dep: Depletion rate (1/project-depth-unit).
        @param k_0: Base recovery rate (1/hr).
        @param k_T: Thermal recovery rate at C{T_ref} (1/hr).
        @param T_ref: Reference temperature (deg C). Default 10.
        @param theta_rec: Temperature sensitivity (1/deg C).
        @param T_freeze: Frozen-ground threshold (deg C). Default 0.
        """
        ...

    def get_rdii_decay(self, entry_idx: int) -> RDIIDecayEntry:
        """Read back an exponential-decay row by index.

        @rtype: L{RDIIDecayEntry}
        """
        ...

    def rdii_decay_count(self) -> int:
        """Return the number of exponential-decay rows.

        @rtype: int
        """
        ...
