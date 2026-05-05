"""
Simulation Statistics
=====================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Type stubs for :mod:`openswmm.engine._statistics`.

The :class:`Statistics` class provides access to post-simulation summary
statistics for nodes, links, and subcatchments.
"""

import numpy as np
import numpy.typing as npt

from ._solver import Solver


class Statistics:
    """Access post-simulation summary statistics.

    Query after calling :meth:`~openswmm.engine.Solver.end` to obtain
    cumulative extremes and totals computed over the full simulation.

    Args:
        solver: An active :class:`Solver` instance. The solver must remain
                alive for the lifetime of this object.

    Example::

        from openswmm.engine import Solver
        from openswmm.engine._statistics import Statistics

        with Solver("model.inp", "model.rpt", "model.out") as s:
            while s.step():
                pass
        stats = Statistics(s)
        print(f"Max depth at node 0: {stats.node_max_depth(0)}")
    """

    def __init__(self, solver: Solver) -> None: ...

    # ------------------------------------------------------------------
    # Node statistics
    # ------------------------------------------------------------------

    def node_max_depth(self, idx: int) -> float:
        """Return the maximum depth recorded at a node.

        Args:
            idx: Node index.

        Returns:
            Maximum depth (project length units).
        """
        ...

    def node_max_overflow(self, idx: int) -> float:
        """Return the maximum overflow rate recorded at a node.

        Args:
            idx: Node index.

        Returns:
            Maximum overflow rate (project flow units).
        """
        ...

    def node_vol_flooded(self, idx: int) -> float:
        """Return the total volume flooded at a node.

        Args:
            idx: Node index.

        Returns:
            Volume flooded (project volume units).
        """
        ...

    def node_time_flooded(self, idx: int) -> float:
        """Return the total time a node was flooded.

        Args:
            idx: Node index.

        Returns:
            Time flooded (hours).
        """
        ...

    # ------------------------------------------------------------------
    # Link statistics
    # ------------------------------------------------------------------

    def link_max_flow(self, idx: int) -> float:
        """Return the maximum flow recorded in a link.

        Args:
            idx: Link index.

        Returns:
            Maximum flow (project flow units).
        """
        ...

    def link_max_velocity(self, idx: int) -> float:
        """Return the maximum velocity recorded in a link.

        Args:
            idx: Link index.

        Returns:
            Maximum velocity (project length / time units).
        """
        ...

    def link_max_filling(self, idx: int) -> float:
        """Return the maximum filling fraction recorded in a link.

        Args:
            idx: Link index.

        Returns:
            Maximum filling fraction (0–1).
        """
        ...

    def link_vol_flow(self, idx: int) -> float:
        """Return the total volume of flow through a link.

        Args:
            idx: Link index.

        Returns:
            Total flow volume (project volume units).
        """
        ...

    def link_surcharge_time(self, idx: int) -> float:
        """Return the total surcharge time for a link.

        Args:
            idx: Link index.

        Returns:
            Surcharge time (hours).
        """
        ...

    # ------------------------------------------------------------------
    # Subcatchment statistics
    # ------------------------------------------------------------------

    def subcatch_precip(self, idx: int) -> float:
        """Return the total precipitation on a subcatchment.

        Args:
            idx: Subcatchment index.

        Returns:
            Total precipitation volume (project depth × area units).
        """
        ...

    def subcatch_runoff_vol(self, idx: int) -> float:
        """Return the total runoff volume from a subcatchment.

        Args:
            idx: Subcatchment index.

        Returns:
            Total runoff volume (project volume units).
        """
        ...

    def subcatch_max_runoff(self, idx: int) -> float:
        """Return the maximum runoff rate from a subcatchment.

        Args:
            idx: Subcatchment index.

        Returns:
            Maximum runoff rate (project flow units).
        """
        ...

    # ------------------------------------------------------------------
    # Bulk statistics (numpy)
    # ------------------------------------------------------------------

    def node_max_depth_bulk(self) -> npt.NDArray[np.float64]:
        """Return maximum depths for all nodes as a NumPy array.

        Returns:
            Array of shape ``(n_nodes,)`` with dtype ``float64``.
        """
        ...

    def link_max_flow_bulk(self) -> npt.NDArray[np.float64]:
        """Return maximum flows for all links as a NumPy array.

        Returns:
            Array of shape ``(n_links,)`` with dtype ``float64``.
        """
        ...

    def subcatch_runoff_vol_bulk(self) -> npt.NDArray[np.float64]:
        """Return total runoff volumes for all subcatchments as a NumPy array.

        Returns:
            Array of shape ``(n_subcatchments,)`` with dtype ``float64``.
        """
        ...
