"""
Inflow Access
=============

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Type stubs for :mod:`openswmm.engine._inflows`.

The :class:`Inflows` class provides methods for adding external inflows,
dry-weather flows, and RDII to model nodes.
"""

from ._solver import Solver


class Inflows:
    """Add external inflows, dry-weather flows, and RDII to the model.

    Args:
        solver: An active :class:`Solver` instance. The solver must remain
                alive for the lifetime of this object.

    Example::

        from openswmm.engine import Solver, Inflows

        with Solver("model.inp", "model.rpt", "model.out") as s:
            inflows = Inflows(s)
            inflows.add_external(0, "FLOW", ts_name="InflowTS")
            inflows.add_dwf(0, "FLOW", 1.5)
    """

    def __init__(self, solver: Solver) -> None: ...

    # ------------------------------------------------------------------
    # External inflows
    # ------------------------------------------------------------------

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

        Args:
            node_idx: Node index.
            constituent: Constituent name (e.g. ``"FLOW"`` or a pollutant ID).
            ts_name: Time-series name; empty string if none.
            type: Inflow type string (default ``"FLOW"``).
            m_factor: Multiplier factor (default 1.0).
            s_factor: Scale factor (default 1.0).
            baseline: Baseline value (default 0.0).
            pattern: Pattern name; empty string if none.
        """
        ...

    def ext_inflow_count(self) -> int:
        """Return the number of external inflows in the model.

        Returns:
            External inflow count.
        """
        ...

    # ------------------------------------------------------------------
    # Dry-weather flows
    # ------------------------------------------------------------------

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

        Args:
            node_idx: Node index.
            constituent: Constituent name (e.g. ``"FLOW"``).
            avg_value: Average DWF value.
            pat1: Monthly pattern name; empty string if none.
            pat2: Daily pattern name; empty string if none.
            pat3: Hourly pattern name; empty string if none.
            pat4: Weekend pattern name; empty string if none.
        """
        ...

    def dwf_count(self) -> int:
        """Return the number of dry-weather flows in the model.

        Returns:
            DWF count.
        """
        ...

    # ------------------------------------------------------------------
    # RDII
    # ------------------------------------------------------------------

    def add_rdii(self, node_idx: int, uh_name: str, area: float) -> None:
        """Add an RDII inflow to a node.

        Args:
            node_idx: Node index.
            uh_name: Unit hydrograph name.
            area: Sewershed area contributing RDII.
        """
        ...

    def rdii_count(self) -> int:
        """Return the number of RDII inflows in the model.

        Returns:
            RDII inflow count.
        """
        ...
