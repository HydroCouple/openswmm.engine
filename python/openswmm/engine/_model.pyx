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
