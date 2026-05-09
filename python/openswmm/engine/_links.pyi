"""
Link Access
===========

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Type stubs for :mod:`openswmm.engine._links`.

The :class:`Links` class provides access to link (conduit/pump/orifice/weir/outlet)
properties during a simulation.
"""

from typing import Union

import numpy as np
import numpy.typing as npt

from ._solver import Solver


class Links:
    """Access link properties during a simulation.

    All per-element methods accept either an integer index or a string link
    ID.  When a string is passed it is resolved via :meth:`get_index`.

    Args:
        solver: An active :class:`Solver` instance. The solver must remain
                alive for the lifetime of this object.

    Example::

        from openswmm.engine import Solver, Links

        with Solver("model.inp", "model.rpt", "model.out") as s:
            links = Links(s)
            flow = links.get_flow(0)      # by index
            flow = links.get_flow("C1")   # by name
    """

    def __init__(self, solver: Solver) -> None: ...

    def _resolve(self, idx: Union[int, str]) -> int:
        """Resolve *idx* to an integer index.

        Args:
            idx: Integer index or string link ID.

        Returns:
            Integer index.

        Raises:
            KeyError: If a string ID is not found.
        """
        ...

    def count(self) -> int:
        """Return the number of links in the model.

        Returns:
            Link count.
        """
        ...

    def get_index(self, link_id: str) -> int:
        """Return the integer index of a link by its string ID.

        Args:
            link_id: Link identifier string.

        Returns:
            Link index, or -1 if not found.
        """
        ...

    def get_id(self, idx: int) -> str:
        """Return the string ID of a link by index.

        Args:
            idx: Link index.

        Returns:
            Link ID string.
        """
        ...

    def add(self, link_id: str, link_type: int) -> int:
        """Add a link to the model (OPENED-state editing).

        Wraps ``swmm_link_add``. Valid in ``BUILDING`` or ``OPENED`` state.

        Args:
            link_id: Unique link identifier.
            link_type: Link type code (see :class:`LinkType`).

        Returns:
            Error code (0 on success).
        """
        ...

    def pop_last(self, link_id: str) -> int:
        """Remove the most recently added link (undo-of-add).

        Wraps ``swmm_link_pop_last``. Valid in ``BUILDING`` or ``OPENED``
        state. ``link_id`` must match the current tail; otherwise
        ``SWMM_ERR_BADINDEX`` is returned.

        Args:
            link_id: Expected tail link identifier.

        Returns:
            Error code (0 on success).
        """
        ...

    def get_flow(self, idx: Union[int, str]) -> float:
        """Return the current flow rate in a link.

        Args:
            idx: Link index (int) or link ID (str).

        Returns:
            Flow rate (project flow units). Positive = node1 -> node2.
        """
        ...

    def set_flow(self, idx: Union[int, str], value: float) -> None:
        """Set the flow rate in a link.

        Args:
            idx: Link index (int) or link ID (str).
            value: New flow rate (project flow units).
        """
        ...

    def get_depth(self, idx: Union[int, str]) -> float:
        """Return the current water depth at the midpoint of a link.

        Args:
            idx: Link index (int) or link ID (str).

        Returns:
            Water depth (project length units).
        """
        ...

    def set_control_setting(self, idx: Union[int, str], setting: float) -> None:
        """Override the control/pump setting on a link (RUNNING state).

        Args:
            idx: Link index (int) or link ID (str).
            setting: New setting (0.0=closed to 1.0=fully open for
                     orifices/weirs; 0.0=off to 1.0=full speed for pumps).
        """
        ...

    def get_control_setting(self, idx: Union[int, str]) -> float:
        """Return the current control setting of a link.

        Args:
            idx: Link index (int) or link ID (str).

        Returns:
            Setting value (0.0--1.0).
        """
        ...

    def get_flows_bulk(self) -> npt.NDArray[np.float64]:
        """Return all link flows as a NumPy array.

        Returns:
            Array of shape ``(n_links,)`` with dtype ``float64``.
        """
        ...

    def set_flows_bulk(self, values: npt.NDArray[np.float64]) -> None:
        """Set all link flows from a NumPy array.

        Args:
            values: Array of shape ``(n_links,)`` with dtype ``float64``.
        """
        ...
