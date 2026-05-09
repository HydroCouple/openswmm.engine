"""
Model Editing — Object Deletion and Type Conversion
======================================================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
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

from ._common cimport *

# ---------------------------------------------------------------------------
# Python-level result classes
# ---------------------------------------------------------------------------

_REF_TYPE_NAMES = {
    0: "node",
    1: "link",
    2: "subcatchment",
    3: "gage",
    4: "table",
    5: "transect",
    6: "inlet_usage",
}


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

    __slots__ = ("obj_type", "obj_type_name", "obj_idx", "field", "cascaded")

    def __init__(self, int obj_type, int obj_idx, str field, bint cascaded):
        self.obj_type      = obj_type
        self.obj_type_name = _REF_TYPE_NAMES.get(obj_type, f"type_{obj_type}")
        self.obj_idx       = obj_idx
        self.field         = field
        self.cascaded      = bool(cascaded)

    def __repr__(self):
        action = "deleted" if self.cascaded else "nullified"
        return (f"ImpactEntry({self.obj_type_name}[{self.obj_idx}].{self.field}"
                f" → {action})")


class ConversionResult:
    """Result of an in-place node or link type conversion.

    Attributes:
        new_type:       The new C-API type enum value.
        cleared_fields: List of field names that were cleared (reset to
                        defaults) during the conversion.
        warnings:       Non-fatal topology warnings (e.g. "model has no
                        outfall after this conversion").  Conversion still
                        proceeds even when warnings are present.
    """

    __slots__ = ("new_type", "cleared_fields", "warnings")

    def __init__(self, int new_type, list cleared_fields, list warnings):
        self.new_type       = new_type
        self.cleared_fields = cleared_fields
        self.warnings       = warnings

    def __repr__(self):
        return (f"ConversionResult(new_type={self.new_type}, "
                f"cleared={self.cleared_fields}, warnings={self.warnings})")


# ---------------------------------------------------------------------------
# Internal C-to-Python converters
# ---------------------------------------------------------------------------

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


# ---------------------------------------------------------------------------
# ModelEditor
# ---------------------------------------------------------------------------

cdef class ModelEditor:
    """Edit an existing SWMM model — delete objects and convert types.

    Accepts any object that exposes a ``handle`` property returning the raw
    engine pointer as an integer.  Both :class:`ModelBuilder` (``BUILDING``
    state) and :class:`Solver` (``OPENED`` state) satisfy this contract.

    All mutating methods require the engine to be in ``BUILDING`` or ``OPENED``
    state; a :exc:`RuntimeError` is raised for any other state.

    Args:
        engine: A :class:`ModelBuilder` or :class:`Solver` instance.
    """

    cdef SWMM_Engine _handle

    def __init__(self, engine):
        self._handle = <SWMM_Engine><size_t>engine.handle

    # -----------------------------------------------------------------------
    # Internal index resolution helpers
    # -----------------------------------------------------------------------

    cdef int _node_idx(self, object id_or_idx) except -1:
        cdef bytes nb
        cdef int ni
        if isinstance(id_or_idx, str):
            nb = id_or_idx.encode('utf-8')
            ni = swmm_node_index(self._handle, nb)
            if ni < 0:
                raise KeyError(f"Node '{id_or_idx}' not found")
            return ni
        return int(id_or_idx)

    cdef int _link_idx(self, object id_or_idx) except -1:
        cdef bytes lb
        cdef int li
        if isinstance(id_or_idx, str):
            lb = id_or_idx.encode('utf-8')
            li = swmm_link_index(self._handle, lb)
            if li < 0:
                raise KeyError(f"Link '{id_or_idx}' not found")
            return li
        return int(id_or_idx)

    cdef int _subcatch_idx(self, object id_or_idx) except -1:
        cdef bytes sb
        cdef int si
        if isinstance(id_or_idx, str):
            sb = id_or_idx.encode('utf-8')
            si = swmm_subcatch_index(self._handle, sb)
            if si < 0:
                raise KeyError(f"Subcatchment '{id_or_idx}' not found")
            return si
        return int(id_or_idx)

    cdef int _gage_idx(self, object id_or_idx) except -1:
        cdef bytes gb
        cdef int gi
        if isinstance(id_or_idx, str):
            gb = id_or_idx.encode('utf-8')
            gi = swmm_gage_index(self._handle, gb)
            if gi < 0:
                raise KeyError(f"Gage '{id_or_idx}' not found")
            return gi
        return int(id_or_idx)

    cdef int _table_idx(self, object id_or_idx) except -1:
        cdef bytes tb
        cdef int ti
        if isinstance(id_or_idx, str):
            tb = id_or_idx.encode('utf-8')
            ti = swmm_table_index(self._handle, tb)
            if ti < 0:
                raise KeyError(f"Table '{id_or_idx}' not found")
            return ti
        return int(id_or_idx)

    # -----------------------------------------------------------------------
    # Impact analysis (non-destructive)
    # -----------------------------------------------------------------------

    def analyze_node_impact(self, id_or_idx) -> list:
        """Preview which objects reference a node, without deleting it.

        Args:
            id_or_idx: Node name (str) or zero-based index (int).

        Returns:
            List of :class:`ImpactEntry` objects describing what would be
            affected.
        """
        cdef int idx = self._node_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_node_analyze_impact(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def analyze_link_impact(self, id_or_idx) -> list:
        """Preview which objects reference a link, without deleting it.

        Args:
            id_or_idx: Link name (str) or zero-based index (int).

        Returns:
            List of :class:`ImpactEntry` objects.
        """
        cdef int idx = self._link_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_link_analyze_impact(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def analyze_subcatch_impact(self, id_or_idx) -> list:
        """Preview which objects reference a subcatchment.

        Args:
            id_or_idx: Subcatchment name (str) or zero-based index (int).

        Returns:
            List of :class:`ImpactEntry` objects.
        """
        cdef int idx = self._subcatch_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_subcatch_analyze_impact(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def analyze_gage_impact(self, id_or_idx) -> list:
        """Preview which objects reference a rain gage.

        Args:
            id_or_idx: Gage name (str) or zero-based index (int).

        Returns:
            List of :class:`ImpactEntry` objects.
        """
        cdef int idx = self._gage_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_gage_analyze_impact(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def analyze_table_impact(self, id_or_idx) -> list:
        """Preview which objects reference a time series or curve.

        Args:
            id_or_idx: Table name (str) or zero-based index (int).

        Returns:
            List of :class:`ImpactEntry` objects.
        """
        cdef int idx = self._table_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_table_analyze_impact(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def analyze_transect_impact(self, int idx) -> list:
        """Preview which links reference a transect.

        Args:
            idx: Zero-based transect index.

        Returns:
            List of :class:`ImpactEntry` objects.
        """
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_transect_analyze_impact(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    # -----------------------------------------------------------------------
    # Deletion
    # -----------------------------------------------------------------------

    def delete_node(self, id_or_idx) -> list:
        """Delete a node and cascade-delete or nullify all referencing objects.

        Links that use the node as an endpoint are deleted.  Subcatchment
        ``outlet_node`` references and inlet-usage ``node_index`` references
        are nullified (set to −1).  All integer cross-references whose value
        was greater than the deleted index are decremented by one.

        Args:
            id_or_idx: Node name (str) or zero-based index (int).

        Returns:
            List of :class:`ImpactEntry` objects describing what was deleted
            or nullified.

        Raises:
            RuntimeError: If the engine is not in BUILDING or OPENED state.
        """
        cdef int idx = self._node_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_node_delete(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def delete_link(self, id_or_idx) -> list:
        """Delete a link and cascade-delete or nullify referencing objects.

        Divider-node ``divider_link`` fields are nullified. Inlet-usage entries
        referencing the link are deleted entirely.

        Args:
            id_or_idx: Link name (str) or zero-based index (int).

        Returns:
            List of :class:`ImpactEntry` objects.

        Raises:
            RuntimeError: If the engine is not in BUILDING or OPENED state.
        """
        cdef int idx = self._link_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_link_delete(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def delete_subcatch(self, id_or_idx) -> list:
        """Delete a subcatchment and nullify referencing objects.

        Other subcatchments' ``outlet_subcatch`` fields and outfall nodes'
        ``outfall_route_to`` fields are nullified.

        Args:
            id_or_idx: Subcatchment name (str) or zero-based index (int).

        Returns:
            List of :class:`ImpactEntry` objects.

        Raises:
            RuntimeError: If the engine is not in BUILDING or OPENED state.
        """
        cdef int idx = self._subcatch_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_subcatch_delete(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def delete_gage(self, id_or_idx) -> list:
        """Delete a rain gage and nullify subcatchment gage references.

        Args:
            id_or_idx: Gage name (str) or zero-based index (int).

        Returns:
            List of :class:`ImpactEntry` objects.

        Raises:
            RuntimeError: If the engine is not in BUILDING or OPENED state.
        """
        cdef int idx = self._gage_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_gage_delete(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def delete_table(self, id_or_idx) -> list:
        """Delete a time series or curve and nullify all referencing objects.

        Affected references include: gage ``ts_index``, node
        ``storage_curve`` / ``outfall_param`` / ``divider_curve``, link
        ``pump_curve`` / ``xsect_curve``.

        Args:
            id_or_idx: Table name (str) or zero-based index (int).

        Returns:
            List of :class:`ImpactEntry` objects.

        Raises:
            RuntimeError: If the engine is not in BUILDING or OPENED state.
        """
        cdef int idx = self._table_idx(id_or_idx)
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_table_delete(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    def delete_transect(self, int idx) -> list:
        """Delete a transect and reset referencing conduit cross-sections.

        Conduit links with an IRREGULAR cross-section shape referencing this
        transect have their ``xsect_curve`` reset to −1 and their shape reset
        to CIRCULAR.

        Args:
            idx: Zero-based transect index.

        Returns:
            List of :class:`ImpactEntry` objects.

        Raises:
            RuntimeError: If the engine is not in BUILDING or OPENED state.
        """
        cdef SWMM_ImpactReport report
        report.entries = NULL
        report.n_entries = 0
        _check(swmm_transect_delete(self._handle, idx, &report))
        return _impact_report_to_list(&report)

    # -----------------------------------------------------------------------
    # Type conversion
    # -----------------------------------------------------------------------

    def convert_node(self, id_or_idx, int new_type) -> ConversionResult:
        """Convert a node to a different type in place.

        Common properties (invert elevation, max depth, coordinates) are
        preserved.  Type-specific properties for the old type are cleared;
        sensible defaults for the new type are applied.  Topology warnings
        are generated (e.g. "model will have no outfall") but do not prevent
        the conversion.

        Args:
            id_or_idx: Node name (str) or zero-based index (int).
            new_type:  Target node type code.  See :class:`NodeType`.
                       Must differ from the current type.

        Returns:
            :class:`ConversionResult` with the new type, cleared fields, and
            any topology warnings.

        Raises:
            RuntimeError: If the engine is not in BUILDING or OPENED state,
                          if *new_type* is invalid, or if *new_type* equals
                          the current type.
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
        preserved.  Type-specific properties are cleared and new-type
        defaults applied.

        Args:
            id_or_idx: Link name (str) or zero-based index (int).
            new_type:  Target link type code.  See :class:`LinkType`.
                       Must differ from the current type.

        Returns:
            :class:`ConversionResult` with the new type, cleared fields, and
            any topology warnings.

        Raises:
            RuntimeError: If the engine is not in BUILDING or OPENED state,
                          if *new_type* is invalid, or if *new_type* equals
                          the current type.
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

    # -----------------------------------------------------------------------
    # Convenience: node / link counts (useful after deletions)
    # -----------------------------------------------------------------------

    @property
    def node_count(self) -> int:
        """Current number of nodes in the model."""
        return swmm_node_count(self._handle)

    @property
    def link_count(self) -> int:
        """Current number of links in the model."""
        return swmm_link_count(self._handle)

    @property
    def subcatch_count(self) -> int:
        """Current number of subcatchments in the model."""
        return swmm_subcatch_count(self._handle)

    @property
    def gage_count(self) -> int:
        """Current number of rain gages in the model."""
        return swmm_gage_count(self._handle)

    @property
    def table_count(self) -> int:
        """Current number of tables (time series + curves) in the model."""
        return swmm_table_count(self._handle)
