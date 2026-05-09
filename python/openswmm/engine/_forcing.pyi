"""
Runtime Forcing
===============

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Type stubs for :mod:`openswmm.engine._forcing`.

The :class:`Forcing` class provides advanced runtime forcing of node, link,
subcatchment, and gage values with support for mode (replace or add) and
persistence (single-step vs. held until cleared).
"""

from ._solver import Solver


class Forcing:
    """Advanced runtime forcing for nodes, links, subcatchments, and gages.

    All per-element methods accept an integer index. Use the corresponding
    domain accessor (e.g. ``solver.nodes.get_index("J1")``) to resolve
    string IDs to indices.

    Args:
        solver: An active :class:`Solver` instance. The solver must remain
                alive for the lifetime of this object.

    Example::

        from openswmm.engine import Solver, Forcing, ForcingMode

        with Solver("model.inp", "model.rpt", "model.out") as s:
            forcing = Forcing(s)
            j1 = s.nodes.get_index("J1")
            forcing.node_lat_inflow(j1, 1.5, ForcingMode.REPLACE, persist=True)
            while s.step():
                pass
            forcing.clear_all()
    """

    def __init__(self, solver: Solver) -> None: ...

    # ------------------------------------------------------------------
    # Node forcing
    # ------------------------------------------------------------------

    def node_lat_inflow(
        self,
        idx: int,
        value: float,
        mode: int = 0,
        persist: bool = False,
    ) -> None:
        """Force a lateral inflow at a node.

        Args:
            idx: Node index.
            value: Lateral inflow rate (project flow units).
            mode: :class:`~openswmm.engine.ForcingMode` — ``REPLACE`` (0)
                or ``ADD`` (1).
            persist: If ``True``, hold the value every step until cleared.
        """
        ...

    def node_head_boundary(
        self,
        idx: int,
        value: float,
        mode: int = 0,
        persist: bool = False,
    ) -> None:
        """Force a head boundary condition at a node.

        Args:
            idx: Node index.
            value: Boundary head (project length units).
            mode: :class:`~openswmm.engine.ForcingMode`.
            persist: If ``True``, hold the value every step until cleared.
        """
        ...

    def node_quality(
        self,
        node_idx: int,
        pollutant_idx: int,
        mass_rate: float,
        mode: int = 0,
        persist: bool = False,
    ) -> None:
        """Force a pollutant mass-rate injection at a node.

        Args:
            node_idx: Node index.
            pollutant_idx: Pollutant index.
            mass_rate: Mass rate (mass / time).
            mode: :class:`~openswmm.engine.ForcingMode`.
            persist: If ``True``, hold the value every step until cleared.
        """
        ...

    # ------------------------------------------------------------------
    # Link forcing
    # ------------------------------------------------------------------

    def link_flow(
        self,
        idx: int,
        value: float,
        mode: int = 0,
        persist: bool = False,
    ) -> None:
        """Force a flow rate in a link.

        Args:
            idx: Link index.
            value: Flow rate (project flow units).
            mode: :class:`~openswmm.engine.ForcingMode`.
            persist: If ``True``, hold the value every step until cleared.
        """
        ...

    def link_setting(
        self,
        idx: int,
        value: float,
        mode: int = 0,
        persist: bool = False,
    ) -> None:
        """Force a control setting on a link.

        Args:
            idx: Link index.
            value: Setting value (0.0–1.0 for gates; speed multiplier
                for pumps).
            mode: :class:`~openswmm.engine.ForcingMode`.
            persist: If ``True``, hold the value every step until cleared.
        """
        ...

    # ------------------------------------------------------------------
    # Subcatchment forcing
    # ------------------------------------------------------------------

    def subcatch_rainfall(
        self,
        idx: int,
        value: float,
        mode: int = 0,
        persist: bool = False,
    ) -> None:
        """Force a rainfall rate on a subcatchment.

        Args:
            idx: Subcatchment index.
            value: Rainfall rate (project rainfall units).
            mode: :class:`~openswmm.engine.ForcingMode`.
            persist: If ``True``, hold the value every step until cleared.
        """
        ...

    def subcatch_evap(
        self,
        idx: int,
        value: float,
        mode: int = 0,
        persist: bool = False,
    ) -> None:
        """Force an evaporation rate on a subcatchment.

        Args:
            idx: Subcatchment index.
            value: Evaporation rate (project length / time units).
            mode: :class:`~openswmm.engine.ForcingMode`.
            persist: If ``True``, hold the value every step until cleared.
        """
        ...

    # ------------------------------------------------------------------
    # Gage forcing
    # ------------------------------------------------------------------

    def gage_rainfall(
        self,
        idx: int,
        value: float,
        mode: int = 0,
        persist: bool = False,
    ) -> None:
        """Force a rainfall rate at a rain gage.

        Affects all subcatchments assigned to the gage.

        Args:
            idx: Gage index.
            value: Rainfall rate (project rainfall units).
            mode: :class:`~openswmm.engine.ForcingMode`.
            persist: If ``True``, hold the value every step until cleared.
        """
        ...

    # ------------------------------------------------------------------
    # Clear
    # ------------------------------------------------------------------

    def clear(self, target_type: int, idx: int) -> None:
        """Remove all active forcing on a specific object.

        Args:
            target_type: :class:`~openswmm.engine.ForcingTarget` code
                (``NODE=0``, ``LINK=1``, ``SUBCATCH=2``, ``GAGE=3``).
            idx: Object index within its type.
        """
        ...

    def clear_all(self) -> None:
        """Remove all active forcing across all object types."""
        ...
