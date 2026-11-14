# SPDX-License-Identifier: Apache-2.0
#
# Copyright 2026 Caleb Buahin
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
Model Editing — Object Deletion and Type Conversion
======================================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: Apache-2.0

Type stubs for :mod:`openswmm.engine._edit`.
"""

from __future__ import annotations
from typing import Iterable

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
# DeleteRecord
# =============================================================================

class DeleteRecord:
    """How to undo one recorded delete exactly.

    Returned by the C{*_recorded} delete methods of L{ModelEditor}. It holds
    every row the delete removed, with its original index, and every
    reference the delete cleared or renumbered. L{restore} puts all of it
    back, at most once, on the engine that made the delete.

    The native record is released when this object is garbage-collected, or
    earlier by L{close}. It cannot be constructed directly.
    """

    @property
    def restorable(self) -> bool:
        """C{True} until L{restore} succeeds or L{close} is called.

        @rtype: bool
        """
        ...

    def restore(self) -> None:
        """Undo the delete: reinsert every removed row at its original index
        and restore every reference the delete cleared or renumbered.

        The engine refuses, changing nothing, when the model no longer matches
        the state the delete left (object counts, the name being free,
        cleared references still clear); the record then stays restorable.
        A successful restore spends the record and invalidates existing
        element wrappers, as a delete does.

        @raise ValueError: If the record was already restored or closed.
        @raise BadHandleError: If the engine that made the record has been
            destroyed.
        @raise BadParamError: If the model no longer matches the state the
            delete left.
        @raise EngineError: On another C API failure, such as an engine that
            is not in C{BUILDING} or C{OPENED} state.
        """
        ...

    def close(self) -> None:
        """Release the native record; it can no longer be restored.

        Safe to call more than once, and after the engine is destroyed.
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
    def delete_nodes(self, keys: Iterable[int | str]) -> list[ImpactEntry]: ...
    def delete_links(self, keys: Iterable[int | str]) -> list[ImpactEntry]: ...
    def delete_subcatchments(self, keys: Iterable[int | str]) -> list[ImpactEntry]: ...
    def delete_gages(self, keys: Iterable[int | str]) -> list[ImpactEntry]: ...

    # Recorded (undoable) deletion: each returns (impacts, DeleteRecord).
    def delete_node_recorded(self, id_or_idx: int | str) -> tuple[list[ImpactEntry], DeleteRecord]:
        """Delete a node as L{delete_node} does, and record how to undo it.

        The record covers the node's row, subtype and 2D coupling rows, name
        and coordinates, every link it cascade-deletes, its inlet usages,
        inflow, DWF and RDII rows, treatment, reaction rows, initial quality,
        user flags and heat overrides, and every reference the delete cleared
        or renumbered.

        @param id_or_idx: Node name (C{str}) or zero-based index (C{int}).
        @return: C{(impacts, record)}: the cascade report, as from
            L{delete_node}, and the L{DeleteRecord} that undoes the delete.
        @raise KeyError: If C{id_or_idx} is a name and the node is not found.
        @raise EngineError: On C API failure, such as an engine that is not
            in C{BUILDING} or C{OPENED} state.
        """
        ...
    def delete_link_recorded(self, id_or_idx: int | str) -> tuple[list[ImpactEntry], DeleteRecord]:
        """Delete a link as L{delete_link} does, and record how to undo it.

        @param id_or_idx: Link name (C{str}) or zero-based index (C{int}).
        @return: C{(impacts, record)}, as for L{delete_node_recorded}.
        """
        ...
    def delete_subcatch_recorded(
            self, id_or_idx: int | str) -> tuple[list[ImpactEntry], DeleteRecord]:
        """Delete a subcatchment as L{delete_subcatch} does, and record how to
        undo it.

        @param id_or_idx: Subcatchment name (C{str}) or zero-based index (C{int}).
        @return: C{(impacts, record)}, as for L{delete_node_recorded}.
        """
        ...
    def delete_gage_recorded(self, id_or_idx: int | str) -> tuple[list[ImpactEntry], DeleteRecord]:
        """Delete a rain gage as L{delete_gage} does, and record how to undo it,
        including the subcatchment references the delete cleared.

        @param id_or_idx: Gage name (C{str}) or zero-based index (C{int}).
        @return: C{(impacts, record)}, as for L{delete_node_recorded}.
        """
        ...
    def delete_nodes_recorded(
            self, keys: Iterable[int | str]) -> tuple[list[ImpactEntry], DeleteRecord]:
        """Delete a batch as L{delete_nodes} does, and record how to undo the
        whole batch: one L{DeleteRecord} restores every node at its original
        index. An empty batch returns a record whose restore does nothing.

        @param keys: Pre-batch node names or zero-based indices.
        @return: C{(impacts, record)}: the aggregate cascade report and the
            record.
        """
        ...
    def delete_links_recorded(
            self, keys: Iterable[int | str]) -> tuple[list[ImpactEntry], DeleteRecord]:
        """Delete a batch as L{delete_links} does, and record how to undo the
        whole batch. See L{delete_nodes_recorded}."""
        ...
    def delete_subcatchments_recorded(
            self, keys: Iterable[int | str]) -> tuple[list[ImpactEntry], DeleteRecord]:
        """Delete a batch as L{delete_subcatchments} does, and record how to
        undo the whole batch. See L{delete_nodes_recorded}."""
        ...
    def delete_gages_recorded(
            self, keys: Iterable[int | str]) -> tuple[list[ImpactEntry], DeleteRecord]:
        """Delete a batch as L{delete_gages} does, and record how to undo the
        whole batch. See L{delete_nodes_recorded}."""
        ...


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

    def analyze_pollutant_impact(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Preview which objects reference a pollutant."""
        ...

    def analyze_pattern_impact(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Preview which objects reference a time pattern."""
        ...

    def analyze_aquifer_impact(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Preview which subcatchments reference an aquifer."""
        ...

    def analyze_snowpack_impact(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Preview which subcatchments reference a snowpack."""
        ...

    def analyze_lid_impact(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Preview which LID-usage rows reference a LID control."""
        ...

    def analyze_street_impact(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Preview which inlet-usage rows reference a street."""
        ...

    def analyze_inlet_impact(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Preview which inlet-usage rows reference an inlet design."""
        ...

    def analyze_landuse_impact(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Preview which objects reference a land use."""
        ...

    def analyze_hydrograph_impact(self, uh_name: str) -> list[ImpactEntry]:
        """Preview which objects reference a unit-hydrograph group by name."""
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

    def delete_pollutant(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Delete a pollutant and re-pack every per-pollutant matrix."""
        ...

    def delete_pattern(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Delete a time pattern and clear all name-based references."""
        ...

    def delete_aquifer(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Delete an aquifer; referencing subcatchments lose groundwater."""
        ...

    def delete_snowpack(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Delete a snowpack; clear referencing subcatchments and renumber."""
        ...

    def delete_lid(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Delete a LID control; cascade-delete referencing LID-usage rows."""
        ...

    def delete_street(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Delete a street; cascade-delete referencing inlet-usage rows."""
        ...

    def delete_inlet(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Delete an inlet design; cascade-delete referencing inlet-usage rows."""
        ...

    def delete_landuse(self, id_or_idx: int | str) -> list[ImpactEntry]:
        """Delete a land use; re-pack buildup/washoff and coverage columns."""
        ...

    def delete_hydrograph(self, uh_name: str) -> list[ImpactEntry]:
        """Delete a unit-hydrograph group by name."""
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
    # Virtual junctions — split / fuse / flag (refactored engine only)
    # =========================================================================

    def set_node_virtual(self, id_or_idx: int | str, make_virtual: bool = True) -> None:
        """Set or clear a node's virtual-junction flag (validated).

        @param id_or_idx: Node name or zero-based index.
        @type id_or_idx: int or str
        @param make_virtual: C{True} to set, C{False} to clear.
        @type make_virtual: bool
        @raise KeyError: If C{id_or_idx} is a name and the node is not found.
        @raise EngineError: On a violated usage rule or C API failure.
        """
        ...

    def split_conduit(self, id_or_idx: int | str, t: float, new_node_name: str,
                      new_link_name: str, make_virtual: bool = False) -> tuple[int, int]:
        """Split a conduit at normalized position C{t}, inserting a new node.

        @param id_or_idx: Conduit name or zero-based index.
        @type id_or_idx: int or str
        @param t: Normalized split position, exclusive (0, 1).
        @type t: float
        @param new_node_name: Unique name for the inserted node.
        @type new_node_name: str
        @param new_link_name: Unique name for the new downstream conduit.
        @type new_link_name: str
        @param make_virtual: Flag the inserted node as a virtual junction.
        @type make_virtual: bool
        @return: C{(new_node_index, new_link_index)}.
        @rtype: tuple[int, int]
        @raise KeyError: If C{id_or_idx} is a name and the link is not found.
        @raise EngineError: On invalid parameters or a rule failure.
        """
        ...

    def fuse_virtual_junction(self, id_or_idx: int | str) -> int:
        """Re-fuse the two conduits of a virtual junction into one.

        @param id_or_idx: Node name or zero-based index.
        @type id_or_idx: int or str
        @return: Index of the surviving conduit AFTER deletions renumber.
        @rtype: int
        @raise KeyError: If C{id_or_idx} is a name and the node is not found.
        @raise EngineError: If the node is not a two-conduit through virtual
            junction, or on C API failure.
        """
        ...

    # =========================================================================
    # Inlet junctions — split / fuse / flag (refactored engine only)
    # =========================================================================

    def set_node_inlet(self, id_or_idx: int | str, make_inlet: bool = True) -> None:
        """Promote a node to an inlet junction, or demote it back.

        @param id_or_idx: Node name or zero-based index.
        @type id_or_idx: int or str
        @param make_inlet: C{True} to promote, C{False} to demote.
        @type make_inlet: bool
        @raise KeyError: If C{id_or_idx} is a name and the node is not found.
        @raise EngineError: On a violated usage rule or C API failure.
        """
        ...

    def split_conduit_inlet(self, id_or_idx: int | str, t: float,
                            new_node_name: str, new_link_name: str,
                            inlet_id: str, capture_node: str) -> tuple[int, int]:
        """Split a street conduit, inserting an inlet junction.

        @param id_or_idx: Conduit name or zero-based index.
        @type id_or_idx: int or str
        @param t: Normalized split position, exclusive (0, 1).
        @type t: float
        @param new_node_name: Unique name for the inserted inlet junction.
        @type new_node_name: str
        @param new_link_name: Unique name for the new downstream conduit.
        @type new_link_name: str
        @param inlet_id: Name of an existing C{[INLETS]} design.
        @type inlet_id: str
        @param capture_node: Name of the receiving (underdrain) node.
        @type capture_node: str
        @return: C{(new_node_index, new_link_index)}.
        @rtype: tuple[int, int]
        @raise KeyError: If C{id_or_idx} is a name and the link is not found.
        @raise EngineError: On invalid parameters or a rule failure.
        """
        ...

    def fuse_inlet_junction(self, id_or_idx: int | str) -> int:
        """Re-fuse the two conduits of an inlet junction into one.

        @param id_or_idx: Node name or zero-based index.
        @type id_or_idx: int or str
        @return: Index of the surviving conduit AFTER deletions renumber.
        @rtype: int
        @raise KeyError: If C{id_or_idx} is a name and the node is not found.
        @raise EngineError: If the node is not a two-conduit through junction,
            or on C API failure.
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
