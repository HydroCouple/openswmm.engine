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

from ._solver import Solver


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

    def rdii_count(self) -> int:
        """Return the number of RDII inflows in the model.

        @return: RDII inflow count.
        @rtype: int
        """
        ...
