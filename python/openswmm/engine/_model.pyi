"""
Programmatic Model Building
============================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Type stubs for :mod:`openswmm.engine._model`.

The :class:`ModelBuilder` class creates a SWMM model entirely through the API
without requiring a ``.inp`` file.
"""

from ._solver import Solver


class ModelBuilder:
    """Build a SWMM model programmatically (no ``.inp`` file).

    The engine starts in ``BUILDING`` state. Use :meth:`add_node`,
    :meth:`add_link`, etc. to populate the model, then call
    :meth:`finalize` to transition to ``INITIALIZED``.

    Example::

        m = ModelBuilder()
        m.add_node("J1", 0)       # JUNCTION
        m.add_node("OUT1", 1)     # OUTFALL
        m.add_link("C1", 0)       # CONDUIT
        m.set_link_nodes(0, 0, 1)
        m.set_link_length(0, 300.0)
        m.set_link_roughness(0, 0.013)
        m.validate()
        m.finalize()
        solver = m.to_solver()
    """

    def __init__(self) -> None: ...

    def add_node(self, node_id: str, node_type: int) -> int:
        """Add a node to the model.

        Valid in ``BUILDING`` or ``OPENED`` state.

        Args:
            node_id: Unique node identifier.
            node_type: Node type code (0=JUNCTION, 1=OUTFALL, 2=STORAGE,
                       3=DIVIDER). See :class:`~openswmm.engine.NodeType`.

        Returns:
            Error code (0 on success).
        """
        ...

    def pop_last_node(self, node_id: str) -> int:
        """Remove the most recently added node (undo-of-add).

        Valid in ``BUILDING`` or ``OPENED`` state. The ``node_id`` must
        match the current tail; otherwise ``SWMM_ERR_BADINDEX`` is
        returned. Returns ``SWMM_ERR_BADPARAM`` if any link still
        references the tail node — pop those links first via
        :meth:`pop_last_link`.

        Args:
            node_id: Expected tail node identifier.

        Returns:
            Error code (0 on success).
        """
        ...

    def add_link(self, link_id: str, link_type: int) -> int:
        """Add a link to the model.

        Valid in ``BUILDING`` or ``OPENED`` state.

        Args:
            link_id: Unique link identifier.
            link_type: Link type code (0=CONDUIT, 1=PUMP, 2=ORIFICE, 3=WEIR,
                       4=OUTLET). See :class:`~openswmm.engine.LinkType`.

        Returns:
            Error code (0 on success).
        """
        ...

    def pop_last_link(self, link_id: str) -> int:
        """Remove the most recently added link (undo-of-add).

        Valid in ``BUILDING`` or ``OPENED`` state. The ``link_id`` must
        match the current tail; otherwise ``SWMM_ERR_BADINDEX`` is
        returned.

        Args:
            link_id: Expected tail link identifier.

        Returns:
            Error code (0 on success).
        """
        ...

    def add_subcatchment(self, sc_id: str) -> int:
        """Add a subcatchment to the model.

        Args:
            sc_id: Unique subcatchment identifier.

        Returns:
            Error code (0 on success).
        """
        ...

    def add_gage(self, gage_id: str) -> int:
        """Add a rain gage to the model.

        Args:
            gage_id: Unique gage identifier.

        Returns:
            Error code (0 on success).
        """
        ...

    def set_node_invert(self, idx: int, elev: float) -> None:
        """Set the invert elevation of a node.

        Args:
            idx: Node index.
            elev: Invert elevation (project length units).
        """
        ...

    def set_node_max_depth(self, idx: int, depth: float) -> None:
        """Set the maximum depth of a node.

        Args:
            idx: Node index.
            depth: Maximum depth (project length units).
        """
        ...

    def set_link_nodes(self, idx: int, from_node: int, to_node: int) -> None:
        """Set the upstream and downstream nodes for a link.

        Args:
            idx: Link index.
            from_node: Upstream node index.
            to_node: Downstream node index.
        """
        ...

    def set_link_length(self, idx: int, length: float) -> None:
        """Set the length of a conduit link.

        Args:
            idx: Link index.
            length: Conduit length (project length units).
        """
        ...

    def set_link_roughness(self, idx: int, n: float) -> None:
        """Set Manning's roughness coefficient for a conduit.

        Args:
            idx: Link index.
            n: Manning's *n* (dimensionless).
        """
        ...

    def set_link_xsect(
        self,
        idx: int,
        shape: int,
        g1: float,
        g2: float = 0,
        g3: float = 0,
        g4: float = 0,
    ) -> None:
        """Set the cross-section geometry of a conduit.

        Args:
            idx: Link index.
            shape: Cross-section shape code.
                   See :class:`~openswmm.engine.XSectShape`.
            g1: Primary geometry parameter (e.g., diameter for circular).
            g2: Secondary geometry parameter.
            g3: Tertiary geometry parameter.
            g4: Quaternary geometry parameter.
        """
        ...

    def validate(self) -> None:
        """Validate model topology (no orphaned links, at least one outfall).

        Does not change state. Safe to call multiple times.

        Raises:
            EngineError: If topology validation fails.
        """
        ...

    def finalize(self) -> None:
        """Finalize the model -- build connectivity, allocate arrays.

        Transitions to ``INITIALIZED`` state.

        Raises:
            EngineError: If finalisation fails.
        """
        ...

    def write(self, path: str) -> None:
        """Write the model to a SWMM ``.inp`` file.

        Args:
            path: Output file path.
        """
        ...

    def to_solver(self) -> Solver:
        """Transfer ownership of the engine handle to a :class:`Solver`.

        After this call, the :class:`ModelBuilder` is invalidated and must
        not be used. The returned :class:`Solver` owns the engine handle.

        Returns:
            A new :class:`Solver` wrapping this model's engine.
        """
        ...
