"""
Node Access
===========

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Type stubs for :mod:`openswmm.engine._nodes`.

The :class:`Nodes` class provides access to node properties during a
simulation.
"""

from typing import Union

import numpy as np
import numpy.typing as npt

from ._solver import Solver


class Nodes:
    """Access node properties during a simulation.

    All per-element methods accept either an integer index or a string node
    ID.  When a string is passed it is resolved via :meth:`get_index`.

    Args:
        solver: An active :class:`Solver` instance. The solver must remain
                alive for the lifetime of this object.

    Example::

        from openswmm.engine import Solver, Nodes

        with Solver("model.inp", "model.rpt", "model.out") as s:
            nodes = Nodes(s)
            depth = nodes.get_depth(0)      # by index
            depth = nodes.get_depth("J1")   # by name
    """

    def __init__(self, solver: Solver) -> None: ...

    def _resolve(self, idx: Union[int, str]) -> int:
        """Resolve *idx* to an integer index.

        Args:
            idx: Integer index or string node ID.

        Returns:
            Integer index.

        Raises:
            KeyError: If a string ID is not found.
        """
        ...

    def count(self) -> int:
        """Return the number of nodes in the model.

        Returns:
            Node count.
        """
        ...

    def get_index(self, node_id: str) -> int:
        """Return the integer index of a node by its string ID.

        Args:
            node_id: Node identifier string.

        Returns:
            Node index, or -1 if not found.
        """
        ...

    def get_id(self, idx: int) -> str:
        """Return the string ID of a node by index.

        Args:
            idx: Node index.

        Returns:
            Node ID string.
        """
        ...

    def get_depth(self, idx: Union[int, str]) -> float:
        """Return the current water depth at a node.

        Args:
            idx: Node index (int) or node ID (str).

        Returns:
            Water depth (project length units).
        """
        ...

    def set_depth(self, idx: Union[int, str], value: float) -> None:
        """Set the water depth at a node.

        Args:
            idx: Node index (int) or node ID (str).
            value: New depth (project length units).
        """
        ...

    def get_head(self, idx: Union[int, str]) -> float:
        """Return the hydraulic head at a node.

        Args:
            idx: Node index (int) or node ID (str).

        Returns:
            Hydraulic head (project length units).
        """
        ...

    def get_volume(self, idx: Union[int, str]) -> float:
        """Return the volume stored at a node.

        Args:
            idx: Node index (int) or node ID (str).

        Returns:
            Volume (project volume units).
        """
        ...

    def set_lateral_inflow(self, idx: Union[int, str], flow: float) -> None:
        """Prescribe a lateral inflow at a node (RUNNING state only).

        Args:
            idx: Node index (int) or node ID (str).
            flow: Lateral inflow rate (project flow units).
        """
        ...

    def get_depths_bulk(self) -> npt.NDArray[np.float64]:
        """Return all node depths as a NumPy array.

        Uses the bulk C API for a single ``memcpy`` -- much faster than
        calling :meth:`get_depth` in a loop.

        Returns:
            Array of shape ``(n_nodes,)`` with dtype ``float64``.
        """
        ...

    def get_heads_bulk(self) -> npt.NDArray[np.float64]:
        """Return all node heads as a NumPy array.

        Returns:
            Array of shape ``(n_nodes,)`` with dtype ``float64``.
        """
        ...

    def set_depths_bulk(self, values: npt.NDArray[np.float64]) -> None:
        """Set all node depths from a NumPy array.

        Args:
            values: Array of shape ``(n_nodes,)`` with dtype ``float64``.
        """
        ...
