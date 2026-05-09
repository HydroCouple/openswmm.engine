"""
Model Editing — Object Deletion and Type Conversion
======================================================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

Type stubs for :mod:`openswmm.engine._edit`.
"""

from __future__ import annotations


class ImpactEntry:
    """One cross-reference that would be affected by a deletion.

    Attributes:
        obj_type:      Integer code for the referencing object type
                       (0=node, 1=link, 2=subcatchment, 3=gage, 4=table,
                       5=transect, 6=inlet_usage).
        obj_type_name: Human-readable name for *obj_type*.
        obj_idx:       Zero-based index of the referencing object.
        field:         Name of the specific cross-reference field
                       (e.g. ``"node1"``, ``"outlet_node"``).
        cascaded:      ``True`` if the referencing object will be deleted;
                       ``False`` if only the reference will be nullified.
    """

    obj_type: int
    obj_type_name: str
    obj_idx: int
    field: str
    cascaded: bool

    def __init__(
        self,
        obj_type: int,
        obj_idx: int,
        field: str,
        cascaded: bool,
    ) -> None: ...

    def __repr__(self) -> str: ...


class ConversionResult:
    """Result of an in-place node or link type conversion.

    Attributes:
        new_type:       The new C-API type enum value.
        cleared_fields: List of field names cleared during conversion.
        warnings:       Non-fatal topology warnings.
    """

    new_type: int
    cleared_fields: list[str]
    warnings: list[str]

    def __init__(
        self,
        new_type: int,
        cleared_fields: list[str],
        warnings: list[str],
    ) -> None: ...

    def __repr__(self) -> str: ...


class ModelEditor:
    """Edit an existing SWMM model — delete objects and convert types.

    Accepts any object that exposes a ``handle`` property returning the raw
    engine pointer as an integer.  Both :class:`ModelBuilder` (``BUILDING``
    state) and :class:`Solver` (``OPENED`` state) satisfy this contract.

    Args:
        engine: A :class:`ModelBuilder` or :class:`Solver` instance.

    Example::

        from openswmm.engine import ModelBuilder, ModelEditor, NodeType, LinkType

        m = ModelBuilder()
        m.add_node("J1", NodeType.JUNCTION)
        m.add_node("J2", NodeType.JUNCTION)
        m.add_node("OUT1", NodeType.OUTFALL)
        m.add_link("C1", LinkType.CONDUIT)
        m.add_link("C2", LinkType.CONDUIT)
        m.set_link_nodes(0, 0, 1)
        m.set_link_nodes(1, 1, 2)

        ed = ModelEditor(m)

        # Non-destructive preview
        impacts = ed.analyze_node_impact("J1")

        # Delete J1 — C1 cascade-deleted automatically
        cascade = ed.delete_node("J1")

        # Convert C2 conduit → weir
        result = ed.convert_link("C2", LinkType.WEIR)
    """

    def __init__(self, engine: object) -> None: ...

    # ---- Impact analysis (non-destructive) ----

    def analyze_node_impact(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Preview which objects reference a node, without deleting it.

        Args:
            id_or_idx: Node name or zero-based index.

        Returns:
            List of :class:`ImpactEntry` describing what would be affected.
        """
        ...

    def analyze_link_impact(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Preview which objects reference a link, without deleting it."""
        ...

    def analyze_subcatch_impact(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Preview which objects reference a subcatchment."""
        ...

    def analyze_gage_impact(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Preview which objects reference a rain gage."""
        ...

    def analyze_table_impact(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Preview which objects reference a time series or curve."""
        ...

    def analyze_transect_impact(self, idx: int) -> list[ImpactEntry]:
        """Preview which links reference a transect."""
        ...

    # ---- Deletion ----

    def delete_node(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Delete a node and cascade-delete or nullify all referencing objects.

        Args:
            id_or_idx: Node name or zero-based index.

        Returns:
            List of :class:`ImpactEntry` describing what was affected.

        Raises:
            RuntimeError: If not in BUILDING or OPENED state.
        """
        ...

    def delete_link(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Delete a link and cascade-delete or nullify referencing objects.

        Raises:
            RuntimeError: If not in BUILDING or OPENED state.
        """
        ...

    def delete_subcatch(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Delete a subcatchment and nullify referencing objects.

        Raises:
            RuntimeError: If not in BUILDING or OPENED state.
        """
        ...

    def delete_gage(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Delete a rain gage and nullify subcatchment gage references.

        Raises:
            RuntimeError: If not in BUILDING or OPENED state.
        """
        ...

    def delete_table(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Delete a time series or curve and nullify all referencing objects.

        Raises:
            RuntimeError: If not in BUILDING or OPENED state.
        """
        ...

    def delete_transect(self, idx: int) -> list[ImpactEntry]:
        """Delete a transect and reset referencing conduit cross-sections.

        Raises:
            RuntimeError: If not in BUILDING or OPENED state.
        """
        ...

    # ---- Type conversion ----

    def convert_node(self, id_or_idx: int | str, new_type: int) -> ConversionResult:
        """Convert a node to a different type in place.

        Args:
            id_or_idx: Node name or zero-based index.
            new_type:  Target node type code (see :class:`NodeType`).

        Returns:
            :class:`ConversionResult` with cleared fields and warnings.

        Raises:
            RuntimeError: If not in BUILDING or OPENED state, type invalid,
                          or new_type equals current type.
        """
        ...

    def convert_link(self, id_or_idx: int | str, new_type: int) -> ConversionResult:
        """Convert a link to a different type in place.

        Args:
            id_or_idx: Link name or zero-based index.
            new_type:  Target link type code (see :class:`LinkType`).

        Returns:
            :class:`ConversionResult` with cleared fields and warnings.

        Raises:
            RuntimeError: If not in BUILDING or OPENED state, type invalid,
                          or new_type equals current type.
        """
        ...

    # ---- Convenience counts ----

    @property
    def node_count(self) -> int:
        """Current number of nodes in the model."""
        ...

    @property
    def link_count(self) -> int:
        """Current number of links in the model."""
        ...

    @property
    def subcatch_count(self) -> int:
        """Current number of subcatchments in the model."""
        ...

    @property
    def gage_count(self) -> int:
        """Current number of rain gages in the model."""
        ...

    @property
    def table_count(self) -> int:
        """Current number of tables (time series + curves) in the model."""
        ...
