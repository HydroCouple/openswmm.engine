"""
Model Editing — Object Deletion and Type Conversion
======================================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

The :class:`ModelEditor` class exposes the two editing capabilities added in
engine 6.0.0:

**Deletion** — remove any node, link, subcatchment, rain gage, table/curve, or
transect from a model that is in ``BUILDING`` or ``OPENED`` state.  A
non-destructive impact-analysis method lets you preview what would be affected
before committing.

**Type conversion** — convert a node between JUNCTION / OUTFALL / STORAGE /
DIVIDER, or a link between CONDUIT / PUMP / ORIFICE / WEIR / OUTLET, in place.
Common properties are preserved; type-specific properties are cleared and
sensible defaults are applied.

The editor works with both :class:`ModelBuilder` (BUILDING state) and
:class:`Solver` (OPENED state after parsing a ``.inp`` file).

.. code-block:: python

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

    # Preview what deleting J1 would affect (non-destructive)
    impacts = ed.analyze_node_impact(0)
    for e in impacts:
        print(e.obj_type_name, e.obj_idx, e.field, "cascaded=" + str(e.cascaded))

    # Delete J1; C1 is cascade-deleted automatically
    cascade = ed.delete_node(0)

    # Convert C2 from CONDUIT to WEIR
    result = ed.convert_link(0, LinkType.WEIR)
    print("Cleared fields:", result.cleared_fields)
    print("Warnings:", result.warnings)
"""

# cython: language_level=3

from ._exceptions import ElementNotFoundError
from datetime import datetime

from ._common cimport *
from ._dates import datetime_to_oadate, oadate_to_datetime

# =============================================================================
# Python-level result classes
# =============================================================================

_REF_TYPE_NAMES = {
    0: "node",
    1: "link",
    2: "subcatchment",
    3: "gage",
    4: "table",
    5: "transect",
    6: "inlet_usage",
    7: "ext_inflow",
    8: "dwf_inflow",
    9: "rdii_assign",
    10: "treatment",
    11: "lid_usage",
    12: "snowpack",
    13: "hydrograph",
    14: "pollutant",
    15: "pattern",
    16: "aquifer",
    17: "lid_control",
    18: "street",
    19: "inlet_design",
    20: "landuse",
    21: "control_rule",
}


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

    __slots__ = ("obj_type", "obj_type_name", "obj_idx", "field", "cascaded")

    def __init__(self, int obj_type, int obj_idx, str field, bint cascaded):
        self.obj_type      = obj_type
        self.obj_type_name = _REF_TYPE_NAMES.get(obj_type, f"type_{obj_type}")
        self.obj_idx       = obj_idx
        self.field         = field
        self.cascaded      = bool(cascaded)

    def __repr__(self):
        """Return a developer-readable representation.

        @return: String describing this entry, including the action that will
            be taken (C{deleted} vs. C{nullified}).
        @rtype: str
        """
        action = "deleted" if self.cascaded else "nullified"
        return (f"ImpactEntry({self.obj_type_name}[{self.obj_idx}].{self.field}"
                f" → {action})")


class ConversionResult:
    """Result of an in-place node or link type conversion.

    @ivar new_type: The new C-API type enum value.
    @ivar cleared_fields: List of field names that were cleared (reset to
        defaults) during the conversion.
    @ivar warnings: Non-fatal topology warnings (e.g. "model has no outfall
        after this conversion"). Conversion still proceeds even when
        warnings are present.

    @param new_type: The new C-API type enum value.
    @type new_type: int
    @param cleared_fields: List of field names that were cleared.
    @type cleared_fields: list[str]
    @param warnings: Non-fatal topology warnings.
    @type warnings: list[str]
    """

    __slots__ = ("new_type", "cleared_fields", "warnings")

    def __init__(self, int new_type, list cleared_fields, list warnings):
        self.new_type       = new_type
        self.cleared_fields = cleared_fields
        self.warnings       = warnings

    def __repr__(self):
        """Return a developer-readable representation.

        @return: String describing the new type, cleared fields, and warnings.
        @rtype: str
        """
        return (f"ConversionResult(new_type={self.new_type}, "
                f"cleared={self.cleared_fields}, warnings={self.warnings})")


# =============================================================================
# Internal C-to-Python converters
# =============================================================================

cdef list _impact_report_to_list(SWMM_ImpactReport* report):
    """Convert a SWMM_ImpactReport to a Python list and free the C memory."""
    cdef list result = []
    cdef int i
    if report.n_entries > 0 and report.entries != NULL:
        for i in range(report.n_entries):
            result.append(ImpactEntry(
                report.entries[i].obj_type,
                report.entries[i].obj_idx,
                report.entries[i].field.decode('utf-8') if report.entries[i].field != NULL else "",
                report.entries[i].cascaded != 0,
            ))
    swmm_impact_report_free(report)
    return result


cdef object _conversion_result_to_py(SWMM_ConversionResult* res):
    """Convert a SWMM_ConversionResult to a Python object and free C memory."""
    cdef list cleared = []
    cdef list warnings = []
    cdef int i
    cdef int new_type
    for i in range(res.n_cleared):
        if res.cleared_fields[i] != NULL:
            cleared.append(res.cleared_fields[i].decode('utf-8'))
    for i in range(res.n_warnings):
        if res.warnings[i] != NULL:
            warnings.append(res.warnings[i].decode('utf-8'))
    new_type = res.new_type
    swmm_conversion_result_free(res)
    return ConversionResult(new_type, cleared, warnings)


# =============================================================================
# ModelEditor
# =============================================================================

cdef class ModelEditor:
    """Edit an existing SWMM model -- delete objects and convert types.

    Accepts any object that exposes a C{handle} property returning the raw
    engine pointer as an integer. Both L{ModelBuilder} (C{BUILDING} state)
    and L{Solver} (C{OPENED} state) satisfy this contract.

    All mutating methods require the engine to be in C{BUILDING} or
    C{OPENED} state; a L{RuntimeError} is raised for any other state.

    @param engine: A L{ModelBuilder} or L{Solver} instance exposing a
        C{handle} property.
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

    cdef SWMM_Engine _handle

    def __init__(self, engine):
        self._handle = <SWMM_Engine><size_t>engine.handle

    # =========================================================================
    # Internal index resolution helpers
    # =========================================================================

    cdef int _node_idx(self, object id_or_idx) except -1:
        cdef bytes nb
        cdef int ni
        if isinstance(id_or_idx, str):
            nb = id_or_idx.encode('utf-8')
            ni = swmm_node_index(self._handle, nb)
            if ni < 0:
                raise ElementNotFoundError(id_or_idx)
            return ni
        return int(id_or_idx)

    cdef int _link_idx(self, object id_or_idx) except -1:
        cdef bytes lb
        cdef int li
        if isinstance(id_or_idx, str):
            lb = id_or_idx.encode('utf-8')
            li = swmm_link_index(self._handle, lb)
            if li < 0:
                raise ElementNotFoundError(id_or_idx)
            return li
        return int(id_or_idx)

    cdef int _subcatch_idx(self, object id_or_idx) except -1:
        cdef bytes sb
        cdef int si
        if isinstance(id_or_idx, str):
            sb = id_or_idx.encode('utf-8')
            si = swmm_subcatch_index(self._handle, sb)
            if si < 0:
                raise ElementNotFoundError(id_or_idx)
            return si
        return int(id_or_idx)

    cdef int _gage_idx(self, object id_or_idx) except -1:
        cdef bytes gb
        cdef int gi
        if isinstance(id_or_idx, str):
            gb = id_or_idx.encode('utf-8')
            gi = swmm_gage_index(self._handle, gb)
            if gi < 0:
                raise ElementNotFoundError(id_or_idx)
            return gi
        return int(id_or_idx)

    cdef int _table_idx(self, object id_or_idx) except -1:
        cdef bytes tb
        cdef int ti
        if isinstance(id_or_idx, str):
            tb = id_or_idx.encode('utf-8')
            ti = swmm_table_index(self._handle, tb)
            if ti < 0:
                raise ElementNotFoundError(id_or_idx)
            return ti
        return int(id_or_idx)

    cdef int _pollutant_idx(self, object id_or_idx) except -1:
        cdef bytes b
        cdef int i
        if isinstance(id_or_idx, str):
            b = id_or_idx.encode('utf-8')
            i = swmm_pollutant_index(self._handle, b)
            if i < 0:
                raise ElementNotFoundError(id_or_idx)
            return i
        return int(id_or_idx)

    cdef int _pattern_idx(self, object id_or_idx) except -1:
        cdef bytes b
        cdef int i
        if isinstance(id_or_idx, str):
            b = id_or_idx.encode('utf-8')
            i = swmm_pattern_index(self._handle, b)
            if i < 0:
                raise ElementNotFoundError(id_or_idx)
            return i
        return int(id_or_idx)

    cdef int _aquifer_idx(self, object id_or_idx) except -1:
        cdef bytes b
        cdef int i
        if isinstance(id_or_idx, str):
            b = id_or_idx.encode('utf-8')
            i = swmm_aquifer_index(self._handle, b)
            if i < 0:
                raise ElementNotFoundError(id_or_idx)
            return i
        return int(id_or_idx)

    cdef int _snowpack_idx(self, object id_or_idx) except -1:
        cdef bytes b
        cdef int i
        if isinstance(id_or_idx, str):
            b = id_or_idx.encode('utf-8')
            i = swmm_snowpack_index(self._handle, b)
            if i < 0:
                raise ElementNotFoundError(id_or_idx)
            return i
        return int(id_or_idx)

    cdef int _lid_idx(self, object id_or_idx) except -1:
        cdef bytes b
        cdef int i
        if isinstance(id_or_idx, str):
            b = id_or_idx.encode('utf-8')
            i = swmm_lid_index(self._handle, b)
            if i < 0:
                raise ElementNotFoundError(id_or_idx)
            return i
        return int(id_or_idx)

    cdef int _street_idx(self, object id_or_idx) except -1:
        cdef bytes b
        cdef int i
        if isinstance(id_or_idx, str):
            b = id_or_idx.encode('utf-8')
            i = swmm_street_index(self._handle, b)
            if i < 0:
                raise ElementNotFoundError(id_or_idx)
            return i
        return int(id_or_idx)

    cdef int _inlet_idx(self, object id_or_idx) except -1:
        cdef bytes b
        cdef int i
        if isinstance(id_or_idx, str):
            b = id_or_idx.encode('utf-8')
            i = swmm_inlet_index(self._handle, b)
            if i < 0:
                raise ElementNotFoundError(id_or_idx)
            return i
        return int(id_or_idx)

    cdef int _landuse_idx(self, object id_or_idx) except -1:
        cdef bytes b
        cdef int i
        if isinstance(id_or_idx, str):
            b = id_or_idx.encode('utf-8')
            i = swmm_landuse_index(self._handle, b)
            if i < 0:
                raise ElementNotFoundError(id_or_idx)
            return i
        return int(id_or_idx)

    # =========================================================================
    # Impact analysis (non-destructive)
    # =========================================================================

    def analyze_node_impact(self, id_or_idx) -> list:
        """Preview which objects reference a node, without deleting it.

        @param id_or_idx: Node name (C{str}) or zero-based index (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects describing what would be
            affected.
        @rtype: list[L{ImpactEntry}]
        @raise KeyError: If C{id_or_idx} is a name and the node is not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._node_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_node_analyze_impact(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def analyze_link_impact(self, id_or_idx) -> list:
        """Preview which objects reference a link, without deleting it.

        @param id_or_idx: Link name (C{str}) or zero-based index (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects describing what would be
            affected.
        @rtype: list[L{ImpactEntry}]
        @raise KeyError: If C{id_or_idx} is a name and the link is not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._link_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_link_analyze_impact(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def analyze_subcatch_impact(self, id_or_idx) -> list:
        """Preview which objects reference a subcatchment.

        @param id_or_idx: Subcatchment name (C{str}) or zero-based index
            (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects describing what would be
            affected.
        @rtype: list[L{ImpactEntry}]
        @raise KeyError: If C{id_or_idx} is a name and the subcatchment is
            not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._subcatch_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_subcatch_analyze_impact(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def analyze_gage_impact(self, id_or_idx) -> list:
        """Preview which objects reference a rain gage.

        @param id_or_idx: Gage name (C{str}) or zero-based index (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects describing what would be
            affected.
        @rtype: list[L{ImpactEntry}]
        @raise KeyError: If C{id_or_idx} is a name and the gage is not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._gage_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_gage_analyze_impact(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def analyze_table_impact(self, id_or_idx) -> list:
        """Preview which objects reference a time series or curve.

        @param id_or_idx: Table name (C{str}) or zero-based index (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects describing what would be
            affected.
        @rtype: list[L{ImpactEntry}]
        @raise KeyError: If C{id_or_idx} is a name and the table is not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._table_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_table_analyze_impact(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def analyze_transect_impact(self, int idx) -> list:
        """Preview which links reference a transect.

        @param idx: Zero-based transect index.
        @type idx: int
        @return: List of L{ImpactEntry} objects describing what would be
            affected.
        @rtype: list[L{ImpactEntry}]
        @raise EngineError: On C API failure.
        """
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_transect_analyze_impact(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def analyze_pollutant_impact(self, id_or_idx) -> list:
        """Preview which objects reference a pollutant, without deleting it.

        @param id_or_idx: Pollutant name (C{str}) or zero-based index (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects describing what would be
            affected.
        @rtype: list[L{ImpactEntry}]
        @raise KeyError: If C{id_or_idx} is a name and it is not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._pollutant_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_pollutant_analyze_impact(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def analyze_pattern_impact(self, id_or_idx) -> list:
        """Preview which objects reference a time pattern, without deleting it.

        @param id_or_idx: Pattern name (C{str}) or zero-based index (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects describing what would be
            affected.
        @rtype: list[L{ImpactEntry}]
        @raise KeyError: If C{id_or_idx} is a name and it is not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._pattern_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_pattern_analyze_impact(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def analyze_aquifer_impact(self, id_or_idx) -> list:
        """Preview which subcatchments reference an aquifer.

        @param id_or_idx: Aquifer name (C{str}) or zero-based index (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects.
        @rtype: list[L{ImpactEntry}]
        @raise KeyError: If C{id_or_idx} is a name and it is not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._aquifer_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_aquifer_analyze_impact(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def analyze_snowpack_impact(self, id_or_idx) -> list:
        """Preview which subcatchments reference a snowpack.

        @param id_or_idx: Snowpack name (C{str}) or zero-based index (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects.
        @rtype: list[L{ImpactEntry}]
        @raise KeyError: If C{id_or_idx} is a name and it is not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._snowpack_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_snowpack_analyze_impact(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def analyze_lid_impact(self, id_or_idx) -> list:
        """Preview which LID-usage rows reference a LID control.

        @param id_or_idx: LID control name (C{str}) or zero-based index
            (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects.
        @rtype: list[L{ImpactEntry}]
        @raise KeyError: If C{id_or_idx} is a name and it is not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._lid_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_lid_analyze_impact(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def analyze_street_impact(self, id_or_idx) -> list:
        """Preview which inlet-usage rows reference a street.

        @param id_or_idx: Street name (C{str}) or zero-based index (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects.
        @rtype: list[L{ImpactEntry}]
        @raise KeyError: If C{id_or_idx} is a name and it is not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._street_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_street_analyze_impact(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def analyze_inlet_impact(self, id_or_idx) -> list:
        """Preview which inlet-usage rows reference an inlet design.

        @param id_or_idx: Inlet design name (C{str}) or zero-based index
            (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects.
        @rtype: list[L{ImpactEntry}]
        @raise KeyError: If C{id_or_idx} is a name and it is not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._inlet_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_inlet_analyze_impact(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def analyze_landuse_impact(self, id_or_idx) -> list:
        """Preview which objects reference a land use.

        @param id_or_idx: Land-use name (C{str}) or zero-based index (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects.
        @rtype: list[L{ImpactEntry}]
        @raise KeyError: If C{id_or_idx} is a name and it is not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._landuse_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_landuse_analyze_impact(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def analyze_hydrograph_impact(self, str uh_name) -> list:
        """Preview which objects reference a unit-hydrograph group by name.

        Unit-hydrograph groups are keyed by name (they have no stable index —
        a group is the set of C{[HYDROGRAPHS]} lines sharing one name).

        @param uh_name: Unit-hydrograph group name.
        @type uh_name: str
        @return: List of L{ImpactEntry} objects.
        @rtype: list[L{ImpactEntry}]
        @raise EngineError: On C API failure (including an unknown name).
        """
        cdef bytes nb = uh_name.encode('utf-8')
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_hydrograph_analyze_impact(self._handle, nb, &report))
        return _impact_report_to_list(&report)

    # =========================================================================
    # Deletion
    # =========================================================================

    def delete_node(self, id_or_idx) -> list:
        """Delete a node and cascade-delete or nullify all referencing objects.

        Links that use the node as an endpoint are deleted. Subcatchment
        C{outlet_node} references and inlet-usage C{node_index} references
        are nullified (set to C{-1}). All integer cross-references whose
        value was greater than the deleted index are decremented by one.

        @param id_or_idx: Node name (C{str}) or zero-based index (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects describing what was deleted
            or nullified.
        @rtype: list[L{ImpactEntry}]
        @raise RuntimeError: If the engine is not in C{BUILDING} or
            C{OPENED} state.
        @raise KeyError: If C{id_or_idx} is a name and the node is not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._node_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_node_delete(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def delete_link(self, id_or_idx) -> list:
        """Delete a link and cascade-delete or nullify referencing objects.

        Divider-node C{divider_link} fields are nullified. Inlet-usage
        entries referencing the link are deleted entirely.

        @param id_or_idx: Link name (C{str}) or zero-based index (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects describing what was deleted
            or nullified.
        @rtype: list[L{ImpactEntry}]
        @raise RuntimeError: If the engine is not in C{BUILDING} or
            C{OPENED} state.
        @raise KeyError: If C{id_or_idx} is a name and the link is not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._link_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_link_delete(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def delete_subcatch(self, id_or_idx) -> list:
        """Delete a subcatchment and nullify referencing objects.

        Other subcatchments' C{outlet_subcatch} fields and outfall nodes'
        C{outfall_route_to} fields are nullified.

        @param id_or_idx: Subcatchment name (C{str}) or zero-based index
            (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects describing what was
            nullified.
        @rtype: list[L{ImpactEntry}]
        @raise RuntimeError: If the engine is not in C{BUILDING} or
            C{OPENED} state.
        @raise KeyError: If C{id_or_idx} is a name and the subcatchment is
            not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._subcatch_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_subcatch_delete(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def delete_gage(self, id_or_idx) -> list:
        """Delete a rain gage and nullify subcatchment gage references.

        @param id_or_idx: Gage name (C{str}) or zero-based index (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects describing what was
            nullified.
        @rtype: list[L{ImpactEntry}]
        @raise RuntimeError: If the engine is not in C{BUILDING} or
            C{OPENED} state.
        @raise KeyError: If C{id_or_idx} is a name and the gage is not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._gage_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_gage_delete(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def delete_table(self, id_or_idx) -> list:
        """Delete a time series or curve and nullify all referencing objects.

        Affected references include: gage C{ts_index}, node
        C{storage_curve} / C{outfall_param} / C{divider_curve}, link
        C{pump_curve} / C{xsect_curve}.

        @param id_or_idx: Table name (C{str}) or zero-based index (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects describing what was
            nullified.
        @rtype: list[L{ImpactEntry}]
        @raise RuntimeError: If the engine is not in C{BUILDING} or
            C{OPENED} state.
        @raise KeyError: If C{id_or_idx} is a name and the table is not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._table_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_table_delete(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def delete_transect(self, int idx) -> list:
        """Delete a transect and reset referencing conduit cross-sections.

        Conduit links with an C{IRREGULAR} cross-section shape referencing
        this transect have their C{xsect_curve} reset to C{-1} and their
        shape reset to C{CIRCULAR}.

        @param idx: Zero-based transect index.
        @type idx: int
        @return: List of L{ImpactEntry} objects describing what was reset.
        @rtype: list[L{ImpactEntry}]
        @raise RuntimeError: If the engine is not in C{BUILDING} or
            C{OPENED} state.
        @raise EngineError: On C API failure.
        """
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_transect_delete(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def delete_pollutant(self, id_or_idx) -> list:
        """Delete a pollutant and re-pack every per-pollutant matrix.

        Cascades ext-inflow / DWF rows that name the pollutant; re-packs
        buildup / washoff columns, treatment expressions, and per-object
        concentration state; nullifies co-pollutant references and LID removal
        pairs; renumbers surviving co-pollutant indices.

        @param id_or_idx: Pollutant name (C{str}) or zero-based index (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects describing what changed.
        @rtype: list[L{ImpactEntry}]
        @raise RuntimeError: If the engine is not in C{BUILDING} or C{OPENED}
            state.
        @raise KeyError: If C{id_or_idx} is a name and it is not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._pollutant_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_pollutant_delete(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def delete_pattern(self, id_or_idx) -> list:
        """Delete a time pattern and clear all name-based references.

        Clears matching DWF C{pat1..4}, ext-inflow baseline patterns, aquifer
        ET patterns, and the global evaporation-recovery pattern. Unlike the
        report-less ``patterns`` collection removal, this reports what was
        cleared and rejects out-of-range indices.

        @param id_or_idx: Pattern name (C{str}) or zero-based index (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects describing what was cleared.
        @rtype: list[L{ImpactEntry}]
        @raise RuntimeError: If the engine is not in C{BUILDING} or C{OPENED}
            state.
        @raise KeyError: If C{id_or_idx} is a name and it is not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._pattern_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_pattern_delete(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def delete_aquifer(self, id_or_idx) -> list:
        """Delete an aquifer; referencing subcatchments lose their groundwater.

        Subcatchment C{gw_aquifer} references are set to C{-1} and surviving
        aquifer indices are renumbered.

        @param id_or_idx: Aquifer name (C{str}) or zero-based index (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects.
        @rtype: list[L{ImpactEntry}]
        @raise RuntimeError: If the engine is not in C{BUILDING} or C{OPENED}
            state.
        @raise KeyError: If C{id_or_idx} is a name and it is not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._aquifer_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_aquifer_delete(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def delete_snowpack(self, id_or_idx) -> list:
        """Delete a snowpack; clear referencing subcatchments and renumber.

        Referencing subcatchments' snowpack index and raw-name mirror are
        cleared, and surviving snowpack indices are renumbered.

        @param id_or_idx: Snowpack name (C{str}) or zero-based index (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects.
        @rtype: list[L{ImpactEntry}]
        @raise RuntimeError: If the engine is not in C{BUILDING} or C{OPENED}
            state.
        @raise KeyError: If C{id_or_idx} is a name and it is not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._snowpack_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_snowpack_delete(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def delete_lid(self, id_or_idx) -> list:
        """Delete a LID control; cascade-delete referencing LID-usage rows.

        LID-usage rows referencing the control are cascade-deleted and
        surviving C{lid_index} values are renumbered.

        @param id_or_idx: LID control name (C{str}) or zero-based index
            (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects.
        @rtype: list[L{ImpactEntry}]
        @raise RuntimeError: If the engine is not in C{BUILDING} or C{OPENED}
            state.
        @raise KeyError: If C{id_or_idx} is a name and it is not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._lid_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_lid_delete(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def delete_street(self, id_or_idx) -> list:
        """Delete a street; cascade-delete referencing inlet-usage rows.

        Inlet-usage rows referencing the street are cascade-deleted
        (consistent with link deletion) and C{street_index} values are
        renumbered.

        @param id_or_idx: Street name (C{str}) or zero-based index (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects.
        @rtype: list[L{ImpactEntry}]
        @raise RuntimeError: If the engine is not in C{BUILDING} or C{OPENED}
            state.
        @raise KeyError: If C{id_or_idx} is a name and it is not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._street_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_street_delete(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def delete_inlet(self, id_or_idx) -> list:
        """Delete an inlet design; cascade-delete referencing inlet-usage rows.

        Inlet-usage rows referencing the design are cascade-deleted and
        C{design_index} values are renumbered.

        @param id_or_idx: Inlet design name (C{str}) or zero-based index
            (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects.
        @rtype: list[L{ImpactEntry}]
        @raise RuntimeError: If the engine is not in C{BUILDING} or C{OPENED}
            state.
        @raise KeyError: If C{id_or_idx} is a name and it is not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._inlet_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_inlet_delete(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def delete_landuse(self, id_or_idx) -> list:
        """Delete a land use; re-pack buildup/washoff and coverage columns.

        Buildup / washoff rows and subcatchment coverage columns for the land
        use are re-packed.

        @param id_or_idx: Land-use name (C{str}) or zero-based index (C{int}).
        @type id_or_idx: int or str
        @return: List of L{ImpactEntry} objects.
        @rtype: list[L{ImpactEntry}]
        @raise RuntimeError: If the engine is not in C{BUILDING} or C{OPENED}
            state.
        @raise KeyError: If C{id_or_idx} is a name and it is not found.
        @raise EngineError: On C API failure.
        """
        cdef int idx = self._landuse_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_landuse_delete(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def delete_hydrograph(self, str uh_name) -> list:
        """Delete a unit-hydrograph group by name.

        Cascade-deletes RDII assignments using the group, then removes the
        group's parameter lines, gage assignment, and RDII-decay rows.

        @param uh_name: Unit-hydrograph group name.
        @type uh_name: str
        @return: List of L{ImpactEntry} objects describing what was deleted.
        @rtype: list[L{ImpactEntry}]
        @raise RuntimeError: If the engine is not in C{BUILDING} or C{OPENED}
            state.
        @raise EngineError: On C API failure (including an unknown name).
        """
        cdef bytes nb = uh_name.encode('utf-8')
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_hydrograph_delete(self._handle, nb, &report))
        return _impact_report_to_list(&report)

    # =========================================================================
    # Type conversion
    # =========================================================================

    def convert_node(self, id_or_idx, int new_type) -> ConversionResult:
        """Convert a node to a different type in place.

        Common properties (invert elevation, max depth, coordinates) are
        preserved. Type-specific properties for the old type are cleared;
        sensible defaults for the new type are applied. Topology warnings
        are generated (e.g. "model will have no outfall") but do not
        prevent the conversion.

        @param id_or_idx: Node name (C{str}) or zero-based index (C{int}).
        @type id_or_idx: int or str
        @param new_type: Target node type code. Must differ from the
            current type.
        @type new_type: int
        @return: L{ConversionResult} with the new type, cleared fields,
            and any topology warnings.
        @rtype: L{ConversionResult}
        @raise RuntimeError: If the engine is not in C{BUILDING} or
            C{OPENED} state, if C{new_type} is invalid, or if C{new_type}
            equals the current type.
        @raise KeyError: If C{id_or_idx} is a name and the node is not found.
        @raise EngineError: On C API failure.
        @see: L{openswmm.engine.NodeType}
        """
        cdef int idx = self._node_idx(id_or_idx)
        cdef SWMM_ConversionResult res
        res.new_type = 0
        res.cleared_fields = NULL
        res.n_cleared = 0
        res.warnings = NULL
        res.n_warnings = 0
        _check(swmm_node_convert(self._handle, idx, new_type, &res))
        return _conversion_result_to_py(&res)

    def convert_link(self, id_or_idx, int new_type) -> ConversionResult:
        """Convert a link to a different type in place.

        Common properties (endpoint nodes, offsets, initial flow) are
        preserved. Type-specific properties are cleared and new-type
        defaults applied.

        @param id_or_idx: Link name (C{str}) or zero-based index (C{int}).
        @type id_or_idx: int or str
        @param new_type: Target link type code. Must differ from the
            current type.
        @type new_type: int
        @return: L{ConversionResult} with the new type, cleared fields,
            and any topology warnings.
        @rtype: L{ConversionResult}
        @raise RuntimeError: If the engine is not in C{BUILDING} or
            C{OPENED} state, if C{new_type} is invalid, or if C{new_type}
            equals the current type.
        @raise KeyError: If C{id_or_idx} is a name and the link is not found.
        @raise EngineError: On C API failure.
        @see: L{openswmm.engine.LinkType}
        """
        cdef int idx = self._link_idx(id_or_idx)
        cdef SWMM_ConversionResult res
        res.new_type = 0
        res.cleared_fields = NULL
        res.n_cleared = 0
        res.warnings = NULL
        res.n_warnings = 0
        _check(swmm_link_convert(self._handle, idx, new_type, &res))
        return _conversion_result_to_py(&res)

    # =========================================================================
    # Virtual junctions — split / fuse / flag (refactored engine only)
    # =========================================================================

    def set_node_virtual(self, id_or_idx, bint make_virtual=True) -> None:
        """Set or clear a node's virtual-junction flag.

        A virtual junction is a zero-storage, momentum-transmitting
        JUNCTION-typed node connecting exactly two conduits of identical
        cross-section. Setting runs full validation of the usage rules and
        applies the derived-geometry contract (max depth = pipe crown, zero
        surcharge depth, zero ponded area); a violated rule raises with the
        specific rule message. Clearing always succeeds for a virtual node.

        @param id_or_idx: Node name (C{str}) or zero-based index (C{int}).
        @type id_or_idx: int or str
        @param make_virtual: C{True} to set, C{False} to clear.
        @type make_virtual: bool
        @raise KeyError: If C{id_or_idx} is a name and the node is not found.
        @raise EngineError: On a violated usage rule or C API failure.
        """
        cdef int idx = self._node_idx(id_or_idx)
        _check(swmm_node_set_virtual(self._handle, idx, 1 if make_virtual else 0))

    def split_conduit(self, id_or_idx, double t, str new_node_name,
                      str new_link_name, bint make_virtual=False) -> tuple:
        """Split a conduit at normalized position C{t}, inserting a new node.

        The split point lies at fraction C{t} of the conduit's vertex-aware
        polyline length (0 < t < 1). The original conduit keeps its name and
        upstream end and is shortened to t·L; the new conduit carries the
        remaining length, copies the cross-section, roughness and barrels,
        and takes over the downstream offset. The break-point invert is
        interpolated along the conduit gradient and interior vertices are
        partitioned. With C{make_virtual=True} the inserted node becomes a
        virtual junction (validation runs; on a rule failure the split still
        stands as a regular junction and the rule error is raised).

        @param id_or_idx: Conduit name (C{str}) or zero-based index (C{int}).
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
        @raise EngineError: On invalid C{t}, duplicate names, a non-conduit
            link, or a virtual-junction rule failure.
        """
        cdef int idx = self._link_idx(id_or_idx)
        cdef bytes node_b = new_node_name.encode('utf-8')
        cdef bytes link_b = new_link_name.encode('utf-8')
        cdef int new_node = -1
        cdef int new_link = -1
        _check(swmm_conduit_split(self._handle, idx, t, node_b, link_b,
                                  1 if make_virtual else 0,
                                  &new_node, &new_link))
        return (new_node, new_link)

    def fuse_virtual_junction(self, id_or_idx) -> int:
        """Re-fuse the two conduits of a virtual junction into one.

        Inverse of a virtual split: the upstream conduit's name survives,
        lengths sum, the node coordinate becomes an interior vertex of the
        merged conduit (map alignment preserved), and the downstream conduit
        and the node are deleted. Requires a through orientation (one conduit
        flowing in, one flowing out).

        @param id_or_idx: Node name (C{str}) or zero-based index (C{int}) of
            the virtual junction.
        @type id_or_idx: int or str
        @return: Index of the surviving conduit AFTER deletions renumber.
        @rtype: int
        @raise KeyError: If C{id_or_idx} is a name and the node is not found.
        @raise EngineError: If the node is not a two-conduit through virtual
            junction, or on C API failure.
        """
        cdef int idx = self._node_idx(id_or_idx)
        cdef int surviving = -1
        _check(swmm_virtual_junction_fuse(self._handle, idx, &surviving))
        return surviving

    # =========================================================================
    # Convenience: node / link counts (useful after deletions)
    # =========================================================================

    @property
    def node_count(self) -> int:
        """Current number of nodes in the model.

        @return: Node count.
        @rtype: int
        """
        return swmm_node_count(self._handle)

    @property
    def link_count(self) -> int:
        """Current number of links in the model.

        @return: Link count.
        @rtype: int
        """
        return swmm_link_count(self._handle)

    @property
    def subcatch_count(self) -> int:
        """Current number of subcatchments in the model.

        @return: Subcatchment count.
        @rtype: int
        """
        return swmm_subcatch_count(self._handle)

    @property
    def gage_count(self) -> int:
        """Current number of rain gages in the model.

        @return: Gage count.
        @rtype: int
        """
        return swmm_gage_count(self._handle)

    @property
    def table_count(self) -> int:
        """Current number of tables (time series + curves) in the model.

        @return: Table count.
        @rtype: int
        """
        return swmm_table_count(self._handle)

    # =========================================================================
    # Typed time-control properties (datetime)
    # =========================================================================

    @property
    def start_datetime(self) -> datetime:
        """Simulation start date/time.

        @rtype: datetime.datetime
        @raise EngineError: On C API failure.
        """
        cdef double v = 0.0
        _check(swmm_options_get_start_date(self._handle, &v))
        return oadate_to_datetime(v)

    @start_datetime.setter
    def start_datetime(self, value: datetime) -> None:
        cdef double v = datetime_to_oadate(value)
        _check(swmm_options_set_start_date(self._handle, v))

    @property
    def end_datetime(self) -> datetime:
        """Simulation end date/time.

        @rtype: datetime.datetime
        @raise EngineError: On C API failure.
        """
        cdef double v = 0.0
        _check(swmm_options_get_end_date(self._handle, &v))
        return oadate_to_datetime(v)

    @end_datetime.setter
    def end_datetime(self, value: datetime) -> None:
        cdef double v = datetime_to_oadate(value)
        _check(swmm_options_set_end_date(self._handle, v))

    @property
    def report_start_datetime(self) -> datetime:
        """Report start date/time.

        @rtype: datetime.datetime
        @raise EngineError: On C API failure.
        """
        cdef double v = 0.0
        _check(swmm_options_get_report_start(self._handle, &v))
        return oadate_to_datetime(v)

    @report_start_datetime.setter
    def report_start_datetime(self, value: datetime) -> None:
        cdef double v = datetime_to_oadate(value)
        _check(swmm_options_set_report_start(self._handle, v))
