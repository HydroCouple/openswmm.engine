"""
Programmatic Model Building
============================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

The :class:`ModelBuilder` class creates a SWMM model entirely through the API
without requiring a ``.inp`` file. Objects are added and configured while the
engine is in ``BUILDING`` state, then :meth:`finalize` transitions to
``INITIALIZED`` for simulation.

.. code-block:: python

    from openswmm.engine import ModelBuilder

    m = ModelBuilder()
    m.add_node("J1", 0)       # JUNCTION
    m.add_node("OUT1", 1)     # OUTFALL
    m.add_link("C1", 0)       # CONDUIT
    m.set_link_nodes(0, 0, 1)
    m.set_link_length(0, 300.0)
    m.set_link_roughness(0, 0.013)
    m.validate()
    m.finalize()

    from openswmm.engine import EngineState

    solver = m.to_solver()
    solver.start()
    while solver.state == EngineState.RUNNING:
        if solver.step() != 0:
            break
    solver.end()
    solver.destroy()
"""

# cython: language_level=3

from datetime import datetime

from ._common cimport *
from ._solver cimport Solver
from ._dates import datetime_to_oadate, oadate_to_datetime


cdef class ModelBuilder:
    """Build a SWMM model programmatically (no C{.inp} file).

    The engine starts in C{BUILDING} state. Use L{add_node}, L{add_link}, etc.
    to populate the model, then call L{finalize} to transition to
    C{INITIALIZED}.

    @note: The L{ModelBuilder} owns its engine handle until L{to_solver} is
        called; the resulting L{Solver} then takes ownership.

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

    cdef SWMM_Engine _handle

    def __init__(self):
        self._handle = swmm_engine_new()
        if self._handle == NULL:
            raise MemoryError("Failed to create engine in BUILDING state")

    # =========================================================================
    # Nodes
    # =========================================================================

    def add_node(self, str node_id, int node_type) -> int:
        """Add a node to the model.

        Valid in C{BUILDING} or C{OPENED} state.

        @param node_id: Unique node identifier.
        @type node_id: str
        @param node_type: Node type code (0=JUNCTION, 1=OUTFALL, 2=STORAGE,
            3=DIVIDER).
        @type node_type: int
        @return: Error code (C{0} on success).
        @rtype: int
        @see: L{openswmm.engine.NodeType}
        """
        cdef bytes b = node_id.encode('utf-8')
        return swmm_node_add(self._handle, b, node_type)

    def pop_last_node(self, str node_id) -> int:
        """Remove the most recently added node (undo-of-add).

        Valid in C{BUILDING} or C{OPENED} state. C{node_id} must match the
        current tail; otherwise C{SWMM_ERR_BADINDEX} is returned. Returns
        C{SWMM_ERR_BADPARAM} if any link still references the tail node --
        pop those links first via L{pop_last_link}.

        @param node_id: Expected tail node identifier.
        @type node_id: str
        @return: Error code (C{0} on success).
        @rtype: int
        """
        cdef bytes b = node_id.encode('utf-8')
        return swmm_node_pop_last(self._handle, b)

    # =========================================================================
    # Links
    # =========================================================================

    def add_link(self, str link_id, int link_type) -> int:
        """Add a link to the model.

        Valid in C{BUILDING} or C{OPENED} state.

        @param link_id: Unique link identifier.
        @type link_id: str
        @param link_type: Link type code (0=CONDUIT, 1=PUMP, 2=ORIFICE,
            3=WEIR, 4=OUTLET).
        @type link_type: int
        @return: Error code (C{0} on success).
        @rtype: int
        @see: L{openswmm.engine.LinkType}
        """
        cdef bytes b = link_id.encode('utf-8')
        return swmm_link_add(self._handle, b, link_type)

    def pop_last_link(self, str link_id) -> int:
        """Remove the most recently added link (undo-of-add).

        Valid in C{BUILDING} or C{OPENED} state. C{link_id} must match the
        current tail; otherwise C{SWMM_ERR_BADINDEX} is returned.

        @param link_id: Expected tail link identifier.
        @type link_id: str
        @return: Error code (C{0} on success).
        @rtype: int
        """
        cdef bytes b = link_id.encode('utf-8')
        return swmm_link_pop_last(self._handle, b)

    # =========================================================================
    # Subcatchments and gages
    # =========================================================================

    def add_subcatchment(self, str sc_id) -> int:
        """Add a subcatchment to the model.

        @param sc_id: Unique subcatchment identifier.
        @type sc_id: str
        @return: Error code (C{0} on success).
        @rtype: int
        """
        cdef bytes b = sc_id.encode('utf-8')
        return swmm_subcatch_add(self._handle, b)

    def add_subcatch(self, str sc_id) -> int:
        """Backward-compatible alias for L{add_subcatchment}.

        @param sc_id: Unique subcatchment identifier.
        @type sc_id: str
        @return: Error code (C{0} on success).
        @rtype: int
        """
        return self.add_subcatchment(sc_id)

    def add_gage(self, str gage_id) -> int:
        """Add a rain gage to the model.

        @param gage_id: Unique gage identifier.
        @type gage_id: str
        @return: Error code (C{0} on success).
        @rtype: int
        """
        cdef bytes b = gage_id.encode('utf-8')
        return swmm_gage_add(self._handle, b)

    # =========================================================================
    # Node properties
    # =========================================================================

    def set_node_invert(self, int idx, double elev):
        """Set the invert elevation of a node.

        @param idx: Node index.
        @type idx: int
        @param elev: Invert elevation (project length units).
        @type elev: float
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        _check(swmm_node_set_invert_elev(self._handle, idx, elev))

    def set_node_max_depth(self, int idx, double depth):
        """Set the maximum depth of a node.

        @param idx: Node index.
        @type idx: int
        @param depth: Maximum depth (project length units).
        @type depth: float
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        _check(swmm_node_set_max_depth(self._handle, idx, depth))

    # =========================================================================
    # Link properties
    # =========================================================================

    def set_link_nodes(self, int idx, int from_node, int to_node):
        """Set the upstream and downstream nodes for a link.

        @param idx: Link index.
        @type idx: int
        @param from_node: Upstream node index.
        @type from_node: int
        @param to_node: Downstream node index.
        @type to_node: int
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        _check(swmm_link_set_nodes(self._handle, idx, from_node, to_node))

    def set_link_length(self, int idx, double length):
        """Set the length of a conduit link.

        @param idx: Link index.
        @type idx: int
        @param length: Conduit length (project length units).
        @type length: float
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        _check(swmm_link_set_length(self._handle, idx, length))

    def set_link_roughness(self, int idx, double n):
        """Set Manning's roughness coefficient for a conduit.

        @param idx: Link index.
        @type idx: int
        @param n: Manning's I{n} (dimensionless).
        @type n: float
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        _check(swmm_link_set_roughness(self._handle, idx, n))

    # =========================================================================
    # Cross-sections
    # =========================================================================

    def set_link_xsect(self, int idx, int shape,
                        double g1, double g2=0, double g3=0, double g4=0):
        """Set the cross-section geometry of a conduit.

        @param idx: Link index.
        @type idx: int
        @param shape: Cross-section shape code.
        @type shape: int
        @param g1: Primary geometry parameter (e.g. diameter for circular).
        @type g1: float
        @param g2: Secondary geometry parameter.
        @type g2: float
        @param g3: Tertiary geometry parameter.
        @type g3: float
        @param g4: Quaternary geometry parameter.
        @type g4: float
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        @see: L{openswmm.engine.XSectShape}
        """
        _check(swmm_link_set_xsect(self._handle, idx, shape, g1, g2, g3, g4))

    # =========================================================================
    # Validation / finalization
    # =========================================================================

    def validate(self):
        """Validate model topology.

        Checks for orphaned links and ensures at least one outfall is present.
        Does not change state. Safe to call multiple times.

        @return: None
        @rtype: None
        @raise EngineError: If topology validation fails.
        """
        _check(swmm_validate_model(self._handle))

    def finalize(self):
        """Finalize the model -- build connectivity and allocate arrays.

        Transitions to C{INITIALIZED} state.

        @return: None
        @rtype: None
        @raise EngineError: If finalization fails.
        """
        _check(swmm_finalize_model(self._handle))

    def write(self, str path):
        """Write the model to a SWMM C{.inp} file.

        @param path: Output file path.
        @type path: str
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        cdef bytes b = path.encode('utf-8')
        _check(swmm_model_write(self._handle, b))

    # =========================================================================
    # Title management
    # =========================================================================

    def get_title_count(self) -> int:
        """Return the number of title lines in the C{[TITLE]} section.

        @return: Number of title lines.
        @rtype: int
        @raise EngineError: On C API failure.
        """
        cdef int count = 0
        _check(swmm_title_get_count(self._handle, &count))
        return count

    def get_title_line(self, int index) -> str:
        """Return a specific title line by zero-based index.

        @param index: Zero-based title line index.
        @type index: int
        @return: Title line text.
        @rtype: str
        @raise EngineError: On C API failure.
        """
        cdef char buf[1024]
        _check(swmm_title_get_line(self._handle, index, buf, 1024))
        return buf.decode('utf-8')

    def add_title_line(self, str line):
        """Append a new line to the C{[TITLE]} section.

        @param line: Text to append.
        @type line: str
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        cdef bytes b = line.encode('utf-8')
        _check(swmm_title_add_line(self._handle, b))

    def set_title(self, str text):
        """Replace all title lines with new text.

        Newline characters in C{text} are used as line separators.

        @param text: Title text (may contain C{\\n}).
        @type text: str
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        cdef bytes b = text.encode('utf-8')
        _check(swmm_title_set(self._handle, b))

    def clear_title(self):
        """Remove all lines from the C{[TITLE]} section.

        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        _check(swmm_title_clear(self._handle))

    # =========================================================================
    # Handle
    # =========================================================================

    @property
    def handle(self) -> int:
        """Raw engine handle as an integer (for use by L{ModelEditor}).

        @return: The underlying C engine pointer cast to an integer.
        @rtype: int
        """
        return <size_t>self._handle

    # =========================================================================
    # Conversion to Solver
    # =========================================================================

    def to_solver(self) -> Solver:
        """Transfer ownership of the engine handle to a L{Solver}.

        After this call, the L{ModelBuilder} is invalidated and must not be
        used. The returned L{Solver} owns the engine handle.

        @return: A new L{Solver} wrapping this model's engine.
        @rtype: L{Solver}
        """
        cdef Solver s = Solver.__new__(Solver)
        s._handle = self._handle
        s._elapsed = 0.0
        s._inp = ""
        s._rpt = ""
        s._out = ""
        self._handle = NULL
        return s

    # =========================================================================
    # Options
    # =========================================================================

    def get_option(self, str key) -> str:
        """Return the value of a model option.

        @param key: Option key name.
        @type key: str
        @return: Option value as a string.
        @rtype: str
        @raise EngineError: On C API failure.
        """
        cdef bytes b = key.encode('utf-8')
        cdef char buf[256]
        _check(swmm_options_get(self._handle, b, buf, 256))
        return buf.decode('utf-8')

    def set_option(self, str key, str value):
        """Set a model option.

        @param key: Option key name.
        @type key: str
        @param value: Option value string.
        @type value: str
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        cdef bytes b_key = key.encode('utf-8')
        cdef bytes b_val = value.encode('utf-8')
        _check(swmm_options_set(self._handle, b_key, b_val))

    def get_file_path(self, int role, str owner="") -> tuple:
        """Read an external-file slot's resolved and original paths.

        @param role: Which slot to read; a L{FilePathRole} value.
        @type role: int
        @param owner: Owner key for vector slots (decimal index for
            hot-start saves, gage id for rain-gage data, series id for
            time-series data). Ignored for scalar slots.
        @type owner: str
        @return: C{(absolute, original)} — the resolved absolute path and
            the original token as authored. Either may be empty.
        @rtype: tuple[str, str]
        @raise EngineError: On C API failure (e.g. unknown role/owner).
        """
        cdef bytes b_owner = owner.encode('utf-8')
        cdef char abs_buf[512]
        cdef char orig_buf[512]
        _check(swmm_file_path_get(self._handle, <SWMM_FilePathRole>role, b_owner,
                                  abs_buf, 512, orig_buf, 512))
        return (abs_buf.decode('utf-8'), orig_buf.decode('utf-8'))

    def set_file_path(self, int role, str new_path, str owner="") -> None:
        """Set the original token for an external-file slot.

        Clears the cached absolute resolution. For vector slots the
        ``owner`` must already exist in the model. Pass an empty
        ``new_path`` to clear the slot.

        @param role: Which slot to set; a L{FilePathRole} value.
        @type role: int
        @param new_path: New path token; empty string clears the slot.
        @type new_path: str
        @param owner: Owner key for vector slots; ignored for scalar slots.
        @type owner: str
        @raise EngineError: On C API failure.
        """
        cdef bytes b_owner = owner.encode('utf-8')
        cdef bytes b_path = new_path.encode('utf-8')
        _check(swmm_file_path_set(self._handle, <SWMM_FilePathRole>role, b_owner, b_path))

    def get_option_ext(self, str key) -> str:
        """Return the value of an extended model option.

        @param key: Extended option key name.
        @type key: str
        @return: Extended option value as a string.
        @rtype: str
        @raise EngineError: On C API failure.
        """
        cdef bytes b = key.encode('utf-8')
        cdef char buf[256]
        _check(swmm_options_get_ext(self._handle, b, buf, 256))
        return buf.decode('utf-8')

    def set_option_ext(self, str key, str value):
        """Set an extended model option.

        @param key: Extended option key name.
        @type key: str
        @param value: Extended option value string.
        @type value: str
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        cdef bytes b_key = key.encode('utf-8')
        cdef bytes b_val = value.encode('utf-8')
        _check(swmm_options_set_ext(self._handle, b_key, b_val))

    def get_crs(self) -> str:
        """Return the coordinate reference system string.

        @return: CRS string (e.g. EPSG identifier or WKT) for the current
            model.
        @rtype: str
        @raise EngineError: On C API failure.
        """
        cdef char buf[256]
        _check(swmm_get_crs(self._handle, buf, 256))
        return buf.decode('utf-8')

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

    # =========================================================================
    # User flags
    # =========================================================================

    def get_userflag_bool(self, str name) -> bool:
        """Return a boolean user flag value.

        @param name: Flag name.
        @type name: str
        @return: Flag value.
        @rtype: bool
        @raise EngineError: On C API failure.
        """
        cdef bytes b = name.encode('utf-8')
        cdef int v = 0
        _check(swmm_userflag_get_bool(self._handle, b, &v))
        return bool(v)

    def get_userflag_int(self, str name) -> int:
        """Return an integer user flag value.

        @param name: Flag name.
        @type name: str
        @return: Flag value.
        @rtype: int
        @raise EngineError: On C API failure.
        """
        cdef bytes b = name.encode('utf-8')
        cdef int v = 0
        _check(swmm_userflag_get_int(self._handle, b, &v))
        return v

    def get_userflag_real(self, str name) -> float:
        """Return a real-valued user flag.

        @param name: Flag name.
        @type name: str
        @return: Flag value.
        @rtype: float
        @raise EngineError: On C API failure.
        """
        cdef bytes b = name.encode('utf-8')
        cdef double v = 0.0
        _check(swmm_userflag_get_real(self._handle, b, &v))
        return v

    def set_userflag_bool(self, str name, bint value):
        """Set a boolean user flag.

        @param name: Flag name.
        @type name: str
        @param value: Flag value.
        @type value: bool
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        cdef bytes b = name.encode('utf-8')
        _check(swmm_userflag_set_bool(self._handle, b, 1 if value else 0))

    def set_userflag_int(self, str name, int value):
        """Set an integer user flag.

        @param name: Flag name.
        @type name: str
        @param value: Flag value.
        @type value: int
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        cdef bytes b = name.encode('utf-8')
        _check(swmm_userflag_set_int(self._handle, b, value))

    def set_userflag_real(self, str name, double value):
        """Set a real-valued user flag.

        @param name: Flag name.
        @type name: str
        @param value: Flag value.
        @type value: float
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        cdef bytes b = name.encode('utf-8')
        _check(swmm_userflag_set_real(self._handle, b, value))

    def define_userflag(self, str name, int type, str description=""):
        """Define (or redefine) a user-flag schema entry (C{[USER_FLAGS]}).

        Redefining an existing name overwrites its definition; previously
        assigned per-object values are kept as-is.

        @param name: Flag name (stored uppercase).
        @type name: str
        @param type: Flag type: 0=BOOLEAN, 1=INTEGER, 2=REAL, 3=STRING
            (see L{UserFlagType}).
        @type type: int
        @param description: Optional description.
        @type description: str
        @return: None
        @rtype: None
        @raise EngineError: On C API failure (empty name or invalid type).
        """
        cdef bytes b_name = name.encode('utf-8')
        cdef bytes b_desc = description.encode('utf-8')
        _check(swmm_userflag_define(self._handle, b_name, type, b_desc))

    def undefine_userflag(self, str name):
        """Remove a user-flag definition and all its per-object values.

        @param name: Flag name (case-insensitive).
        @type name: str
        @return: None
        @rtype: None
        @raise EngineError: If the flag is not defined.
        """
        cdef bytes b = name.encode('utf-8')
        _check(swmm_userflag_undefine(self._handle, b))

    def userflag_def_count(self) -> int:
        """Return the number of user-flag schema definitions.

        @return: Definition count.
        @rtype: int
        @raise EngineError: On C API failure.
        """
        cdef int v = 0
        _check(swmm_userflag_def_count(self._handle, &v))
        return v

    def get_userflag_def(self, int index) -> tuple:
        """Return a user-flag schema definition by index (insertion order).

        @param index: Zero-based definition index.
        @type index: int
        @return: C{(name, type, description)} where C{type} is 0=BOOLEAN,
            1=INTEGER, 2=REAL, 3=STRING.
        @rtype: tuple[str, int, str]
        @raise EngineError: If C{index} is out of range.
        """
        cdef char name_buf[128]
        cdef char desc_buf[512]
        cdef int t = 0
        _check(swmm_userflag_def_get(self._handle, index, name_buf, 128,
                                     &t, desc_buf, 512))
        return (name_buf.decode('utf-8'), t, desc_buf.decode('utf-8'))

    def get_userflag_value(self, str obj_type, str obj_name, str flag_name):
        """Return the flag value assigned to a specific object, as a string.

        String form is symmetric with the INP encoding: BOOLEAN as
        C{YES}/C{NO}, INTEGER as a decimal, REAL as C{%g}, STRING verbatim.

        @param obj_type: Object type token (e.g. C{"NODE"}, C{"LINK"},
            C{"SUBCATCHMENT"}); case-insensitive.
        @type obj_type: str
        @param obj_name: Object identifier (case-preserved).
        @type obj_name: str
        @param flag_name: Flag name (case-insensitive).
        @type flag_name: str
        @return: The value string, or C{None} when no value is assigned.
        @rtype: str or None
        @raise EngineError: On C API failure.
        """
        cdef bytes b_type = obj_type.encode('utf-8')
        cdef bytes b_name = obj_name.encode('utf-8')
        cdef bytes b_flag = flag_name.encode('utf-8')
        cdef char buf[512]
        cdef int found = 0
        _check(swmm_userflag_value_get(self._handle, b_type, b_name, b_flag,
                                       buf, 512, &found))
        if not found:
            return None
        return buf.decode('utf-8')

    def set_userflag_value(self, str obj_type, str obj_name, str flag_name,
                           str value):
        """Assign a flag value to a specific object from a string.

        The flag must already be defined (its declared type drives parsing).
        BOOLEAN accepts C{YES}/C{NO}/C{TRUE}/C{FALSE}/C{1}/C{0}; INTEGER a
        decimal integer; REAL a decimal number; STRING is stored verbatim.

        @param obj_type: Object type token; case-insensitive.
        @type obj_type: str
        @param obj_name: Object identifier (case-preserved).
        @type obj_name: str
        @param flag_name: Flag name (case-insensitive); must be defined.
        @type flag_name: str
        @param value: Value string parsed per the flag's declared type.
        @type value: str
        @return: None
        @rtype: None
        @raise EngineError: On undefined flag or a value that does not parse
            as the declared type.
        """
        cdef bytes b_type = obj_type.encode('utf-8')
        cdef bytes b_name = obj_name.encode('utf-8')
        cdef bytes b_flag = flag_name.encode('utf-8')
        cdef bytes b_val = value.encode('utf-8')
        _check(swmm_userflag_value_set(self._handle, b_type, b_name, b_flag,
                                       b_val))

    def clear_userflag_value(self, str obj_type, str obj_name, str flag_name):
        """Remove the flag value assigned to a specific object (mark unset).

        Clearing an unassigned value succeeds (idempotent).

        @param obj_type: Object type token; case-insensitive.
        @type obj_type: str
        @param obj_name: Object identifier (case-preserved).
        @type obj_name: str
        @param flag_name: Flag name (case-insensitive).
        @type flag_name: str
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        cdef bytes b_type = obj_type.encode('utf-8')
        cdef bytes b_name = obj_name.encode('utf-8')
        cdef bytes b_flag = flag_name.encode('utf-8')
        _check(swmm_userflag_value_clear(self._handle, b_type, b_name, b_flag))

    # =========================================================================
    # Plugins
    # =========================================================================

    def plugins_count(self) -> int:
        """Return the number of [PLUGINS] entries on the engine.

        @return: Plugin count.
        @rtype: int
        @raise EngineError: On C API failure.
        """
        cdef int count = 0
        _check(swmm_plugins_count(self._handle, &count))
        return count

    def plugin_get(self, int idx) -> tuple:
        """Read one [PLUGINS] row by index.

        @param idx: Index in C{[0, plugins_count())}.
        @type idx: int
        @return: Tuple C{(path, args)} where C{args} is a space-joined string.
        @rtype: tuple
        @raise EngineError: On C API failure.
        """
        cdef char path_buf[4096]
        cdef char args_buf[4096]
        _check(swmm_plugin_get(self._handle, idx, path_buf, 4096, args_buf, 4096))
        return path_buf.decode('utf-8'), args_buf.decode('utf-8')

    def plugin_set(self, str path_or_id, str args=""):
        """Add or replace a [PLUGINS] row keyed by path/id.

        If a row with the same C{path_or_id} exists its args are replaced;
        otherwise a new row is appended.

        @param path_or_id: Library path, plugin id, or C{id:version} string.
        @type path_or_id: str
        @param args: Space-separated argument tokens; C{""} for no arguments.
        @type args: str
        @raise EngineError: On C API failure.
        """
        cdef bytes b_path = path_or_id.encode('utf-8')
        cdef bytes b_args = args.encode('utf-8')
        _check(swmm_plugin_set(self._handle, b_path, b_args))

    def plugin_remove(self, str path_or_id):
        """Remove the [PLUGINS] row matching C{path_or_id}.

        Idempotent: returns without error when no row matches.

        @param path_or_id: Library path, plugin id, or C{id:version} string.
        @type path_or_id: str
        @raise EngineError: On C API failure.
        """
        cdef bytes b = path_or_id.encode('utf-8')
        _check(swmm_plugin_remove(self._handle, b))

    # =========================================================================
    # [FILES] section
    # =========================================================================

    def files_get(self, str key) -> str:
        """Read one [FILES] field by key.

        Recognised keys include: C{"RAINFALL_PATH"}, C{"RAINFALL_MODE"},
        C{"RUNOFF_PATH"}, C{"RUNOFF_MODE"}, C{"RDII_PATH"}, C{"RDII_MODE"},
        C{"INFLOWS_PATH"}, C{"OUTFLOWS_PATH"}, C{"HOTSTART_USE_PATH"},
        C{"HOTSTART_SAVE_PATH"}, C{"HOTSTART_SAVE_DATETIME"}.

        @param key: Field key (case-insensitive).
        @type key: str
        @return: Field value string.
        @rtype: str
        @raise EngineError: On C API failure.
        """
        cdef bytes b = key.encode('utf-8')
        cdef char buf[4096]
        _check(swmm_files_get(self._handle, b, buf, 4096))
        return buf.decode('utf-8')

    def files_set(self, str key, str value):
        """Write one [FILES] field by key.

        Pass an empty C{value} to clear a path slot or mode.

        @param key: Field key (see L{files_get} for recognised keys).
        @type key: str
        @param value: New field value; C{""} to clear.
        @type value: str
        @raise EngineError: On C API failure.
        """
        cdef bytes b_key = key.encode('utf-8')
        cdef bytes b_val = value.encode('utf-8')
        _check(swmm_files_set(self._handle, b_key, b_val))

    # =========================================================================
    # Write with plugin
    # =========================================================================

    def write_with_plugin(self, str path, str output_plugin_id=""):
        """Write the current model to disk using an output plugin.

        Pass an empty string (the default) to use the built-in `.inp` writer.

        @param path: Destination file path.
        @type path: str
        @param output_plugin_id: Plugin id or empty string for built-in writer.
        @type output_plugin_id: str
        @raise EngineError: On C API failure.
        """
        cdef bytes b_path = path.encode('utf-8')
        cdef bytes b_plugin = output_plugin_id.encode('utf-8')
        _check(swmm_model_write_with_plugin(self._handle, b_path, b_plugin))

