"""
Model Editing — Object Deletion and Type Conversion
======================================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Type stubs for :mod:`openswmm.engine._edit`.
"""

from __future__ import annotations

from datetime import datetime


# =============================================================================
# Result types
# =============================================================================

class ImpactEntry:
    """One cross-reference that would be affected by a deletion.

    @ivar obj_type: Integer code for the referencing object type
        (0=node, 1=link, 2=subcatchment, 3=gage, 4=table, 5=transect,
        6=inlet_usage).
    @ivar obj_type_name: Human-readable name for C{obj_type}.
    @ivar obj_idx: Zero-based index of the referencing object.
    @ivar field: Name of the specific cross-reference field
        (e.g. C{"node1"}, C{"outlet_node"}).
    @ivar cascaded: C{True} if the referencing object will be deleted;
        C{False} if only the reference will be nullified.

    @param obj_type: Integer code for the referencing object type.
    @type obj_type: int
    @param obj_idx: Zero-based index of the referencing object.
    @type obj_idx: int
    @param field: Name of the specific cross-reference field.
    @type field: str
    @param cascaded: C{True} for cascade-delete, C{False} for nullify.
    @type cascaded: bool
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

    def __repr__(self) -> str:
        """Return a developer-readable representation.

        @return: String describing this entry, including the action that will
            be taken (C{deleted} vs. C{nullified}).
        @rtype: str
        """
        ...


class ConversionResult:
    """Result of an in-place node or link type conversion.

    @ivar new_type: The new C-API type enum value.
    @ivar cleared_fields: List of field names cleared during conversion.
    @ivar warnings: Non-fatal topology warnings.

    @param new_type: The new C-API type enum value.
    @type new_type: int
    @param cleared_fields: List of field names that were cleared (reset to
        defaults) during the conversion.
    @type cleared_fields: list[str]
    @param warnings: Non-fatal topology warnings (e.g. "model has no outfall
        after this conversion"). Conversion still proceeds even when
        warnings are present.
    @type warnings: list[str]
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

    def __repr__(self) -> str:
        """Return a developer-readable representation.

        @return: String describing the new type, cleared fields, and warnings.
        @rtype: str
        """
        ...


# =============================================================================
# ModelEditor
# =============================================================================

class ModelEditor:
    """Edit an existing SWMM model -- delete objects and convert types.

    Accepts any object that exposes a C{handle} property returning the raw
    engine pointer as an integer. Both L{ModelBuilder} (C{BUILDING} state)
    and L{Solver} (C{OPENED} state) satisfy this contract.

    @param engine: A L{ModelBuilder} or L{Solver} instance.
    @type engine: object

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

        # Delete J1 -- C1 cascade-deleted automatically
        cascade = ed.delete_node("J1")

        # Convert C2 conduit -> weir
        result = ed.convert_link("C2", LinkType.WEIR)
    """

    def __init__(self, engine: object) -> None: ...

    # =========================================================================
    # Impact analysis (non-destructive)
    # =========================================================================

    def analyze_node_impact(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Preview which objects reference a node, without deleting it.

        @param id_or_idx: Node name or zero-based index.
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} describing what would be affected.
        @rtype: list[L{ImpactEntry}]
        @raise KeyError: If C{id_or_idx} is a name and the node is not found.
        @raise EngineError: On C API failure.
        """
        ...

    def analyze_link_impact(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Preview which objects reference a link, without deleting it.

        @param id_or_idx: Link name or zero-based index.
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} describing what would be affected.
        @rtype: list[L{ImpactEntry}]
        @raise KeyError: If C{id_or_idx} is a name and the link is not found.
        @raise EngineError: On C API failure.
        """
        ...

    def analyze_subcatch_impact(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Preview which objects reference a subcatchment.

        @param id_or_idx: Subcatchment name or zero-based index.
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} describing what would be affected.
        @rtype: list[L{ImpactEntry}]
        @raise KeyError: If C{id_or_idx} is a name and the subcatchment is
            not found.
        @raise EngineError: On C API failure.
        """
        ...

    def analyze_gage_impact(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Preview which objects reference a rain gage.

        @param id_or_idx: Gage name or zero-based index.
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} describing what would be affected.
        @rtype: list[L{ImpactEntry}]
        @raise KeyError: If C{id_or_idx} is a name and the gage is not found.
        @raise EngineError: On C API failure.
        """
        ...

    def analyze_table_impact(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Preview which objects reference a time series or curve.

        @param id_or_idx: Table name or zero-based index.
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} describing what would be affected.
        @rtype: list[L{ImpactEntry}]
        @raise KeyError: If C{id_or_idx} is a name and the table is not found.
        @raise EngineError: On C API failure.
        """
        ...

    def analyze_transect_impact(self, idx: int) -> list[ImpactEntry]:
        """Preview which links reference a transect.

        @param idx: Zero-based transect index.
        @type idx: int
        @return: List of L{ImpactEntry} describing what would be affected.
        @rtype: list[L{ImpactEntry}]
        @raise EngineError: On C API failure.
        """
        ...

    # =========================================================================
    # Deletion
    # =========================================================================

    def delete_node(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Delete a node and cascade-delete or nullify all referencing objects.

        @param id_or_idx: Node name or zero-based index.
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} describing what was affected.
        @rtype: list[L{ImpactEntry}]
        @raise RuntimeError: If not in C{BUILDING} or C{OPENED} state.
        @raise KeyError: If C{id_or_idx} is a name and the node is not found.
        @raise EngineError: On C API failure.
        """
        ...

    def delete_link(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Delete a link and cascade-delete or nullify referencing objects.

        @param id_or_idx: Link name or zero-based index.
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} describing what was affected.
        @rtype: list[L{ImpactEntry}]
        @raise RuntimeError: If not in C{BUILDING} or C{OPENED} state.
        @raise KeyError: If C{id_or_idx} is a name and the link is not found.
        @raise EngineError: On C API failure.
        """
        ...

    def delete_subcatch(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Delete a subcatchment and nullify referencing objects.

        @param id_or_idx: Subcatchment name or zero-based index.
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} describing what was affected.
        @rtype: list[L{ImpactEntry}]
        @raise RuntimeError: If not in C{BUILDING} or C{OPENED} state.
        @raise KeyError: If C{id_or_idx} is a name and the subcatchment is
            not found.
        @raise EngineError: On C API failure.
        """
        ...

    def delete_gage(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Delete a rain gage and nullify subcatchment gage references.

        @param id_or_idx: Gage name or zero-based index.
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} describing what was affected.
        @rtype: list[L{ImpactEntry}]
        @raise RuntimeError: If not in C{BUILDING} or C{OPENED} state.
        @raise KeyError: If C{id_or_idx} is a name and the gage is not found.
        @raise EngineError: On C API failure.
        """
        ...

    def delete_table(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Delete a time series or curve and nullify all referencing objects.

        @param id_or_idx: Table name or zero-based index.
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} describing what was affected.
        @rtype: list[L{ImpactEntry}]
        @raise RuntimeError: If not in C{BUILDING} or C{OPENED} state.
        @raise KeyError: If C{id_or_idx} is a name and the table is not found.
        @raise EngineError: On C API failure.
        """
        ...

    def delete_transect(self, idx: int) -> list[ImpactEntry]:
        """Delete a transect and reset referencing conduit cross-sections.

        @param idx: Zero-based transect index.
        @type idx: int
        @return: List of L{ImpactEntry} describing what was affected.
        @rtype: list[L{ImpactEntry}]
        @raise RuntimeError: If not in C{BUILDING} or C{OPENED} state.
        @raise EngineError: On C API failure.
        """
        ...

    # =========================================================================
    # Type conversion
    # =========================================================================

    def convert_node(self, id_or_idx: int | str, new_type: int) -> ConversionResult:
        """Convert a node to a different type in place.

        @param id_or_idx: Node name or zero-based index.
        @type id_or_idx: int or str
        @param new_type: Target node type code.
        @type new_type: int
        @return: L{ConversionResult} with cleared fields and warnings.
        @rtype: L{ConversionResult}
        @raise RuntimeError: If not in C{BUILDING} or C{OPENED} state, the
            type is invalid, or C{new_type} equals the current type.
        @raise KeyError: If C{id_or_idx} is a name and the node is not found.
        @raise EngineError: On C API failure.
        @see: L{openswmm.engine.NodeType}
        """
        ...

    def convert_link(self, id_or_idx: int | str, new_type: int) -> ConversionResult:
        """Convert a link to a different type in place.

        @param id_or_idx: Link name or zero-based index.
        @type id_or_idx: int or str
        @param new_type: Target link type code.
        @type new_type: int
        @return: L{ConversionResult} with cleared fields and warnings.
        @rtype: L{ConversionResult}
        @raise RuntimeError: If not in C{BUILDING} or C{OPENED} state, the
            type is invalid, or C{new_type} equals the current type.
        @raise KeyError: If C{id_or_idx} is a name and the link is not found.
        @raise EngineError: On C API failure.
        @see: L{openswmm.engine.LinkType}
        """
        ...

    # =========================================================================
    # Convenience counts
    # =========================================================================

    @property
    def node_count(self) -> int:
        """Current number of nodes in the model.

        @return: Node count.
        @rtype: int
        """
        ...

    @property
    def link_count(self) -> int:
        """Current number of links in the model.

        @return: Link count.
        @rtype: int
        """
        ...

    @property
    def subcatch_count(self) -> int:
        """Current number of subcatchments in the model.

        @return: Subcatchment count.
        @rtype: int
        """
        ...

    @property
    def gage_count(self) -> int:
        """Current number of rain gages in the model.

        @return: Gage count.
        @rtype: int
        """
        ...

    @property
    def table_count(self) -> int:
        """Current number of tables (time series + curves) in the model.

        @return: Table count.
        @rtype: int
        """
        ...

    # =========================================================================
    # Typed time-control properties (datetime)
    # =========================================================================

    start_datetime: datetime
    """Simulation start date/time.

    @raise EngineError: On C API failure.
    """

    end_datetime: datetime
    """Simulation end date/time.

    @raise EngineError: On C API failure.
    """

    report_start_datetime: datetime
    """Report start date/time.

    @raise EngineError: On C API failure.
    """
