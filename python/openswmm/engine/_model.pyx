"""
Programmatic Model Building
============================

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
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

    solver = m.to_solver()
    solver.start()
    while solver.step():
        pass
    solver.end()
    solver.destroy()
"""

# cython: language_level=3

from ._common cimport *
from ._solver cimport Solver


cdef class ModelBuilder:
    """Build a SWMM model programmatically (no ``.inp`` file).

    The engine starts in ``BUILDING`` state. Use :meth:`add_node`,
    :meth:`add_link`, etc. to populate the model, then call
    :meth:`finalize` to transition to ``INITIALIZED``.
    """

    cdef SWMM_Engine _handle

    def __init__(self):
        self._handle = swmm_engine_new()
        if self._handle == NULL:
            raise MemoryError("Failed to create engine in BUILDING state")

    def add_node(self, str node_id, int node_type) -> int:
        """Add a node to the model.

        Valid in ``BUILDING`` or ``OPENED`` state.

        :param node_id: Unique node identifier.
        :type node_id: str
        :param node_type: Node type code (0=JUNCTION, 1=OUTFALL, 2=STORAGE, 3=DIVIDER).
        :type node_type: int
        :returns: Error code (0 on success).
        :rtype: int
        """
        cdef bytes b = node_id.encode('utf-8')
        return swmm_node_add(self._handle, b, node_type)

    def pop_last_node(self, str node_id) -> int:
        """Remove the most recently added node (undo-of-add).

        Valid in ``BUILDING`` or ``OPENED`` state. ``node_id`` must
        match the current tail; otherwise ``SWMM_ERR_BADINDEX`` is
        returned. Returns ``SWMM_ERR_BADPARAM`` if any link still
        references the tail node — pop those links first via
        :meth:`pop_last_link`.

        :param node_id: Expected tail node identifier.
        :type node_id: str
        :returns: Error code (0 on success).
        :rtype: int
        """
        cdef bytes b = node_id.encode('utf-8')
        return swmm_node_pop_last(self._handle, b)

    def add_link(self, str link_id, int link_type) -> int:
        """Add a link to the model.

        Valid in ``BUILDING`` or ``OPENED`` state.

        :param link_id: Unique link identifier.
        :type link_id: str
        :param link_type: Link type code (0=CONDUIT, 1=PUMP, 2=ORIFICE, 3=WEIR, 4=OUTLET).
        :type link_type: int
        :returns: Error code.
        :rtype: int
        """
        cdef bytes b = link_id.encode('utf-8')
        return swmm_link_add(self._handle, b, link_type)

    def pop_last_link(self, str link_id) -> int:
        """Remove the most recently added link (undo-of-add).

        Valid in ``BUILDING`` or ``OPENED`` state. ``link_id`` must
        match the current tail; otherwise ``SWMM_ERR_BADINDEX`` is
        returned.

        :param link_id: Expected tail link identifier.
        :type link_id: str
        :returns: Error code (0 on success).
        :rtype: int
        """
        cdef bytes b = link_id.encode('utf-8')
        return swmm_link_pop_last(self._handle, b)

    def add_subcatchment(self, str sc_id) -> int:
        """Add a subcatchment to the model.

        :param sc_id: Unique subcatchment identifier.
        :type sc_id: str
        """
        cdef bytes b = sc_id.encode('utf-8')
        return swmm_subcatch_add(self._handle, b)

    def add_gage(self, str gage_id) -> int:
        """Add a rain gage to the model.

        :param gage_id: Unique gage identifier.
        :type gage_id: str
        """
        cdef bytes b = gage_id.encode('utf-8')
        return swmm_gage_add(self._handle, b)

    def set_node_invert(self, int idx, double elev):
        """Set the invert elevation of a node.

        :param idx: Node index.
        :param elev: Invert elevation (project length units).
        """
        _check(swmm_node_set_invert_elev(self._handle, idx, elev))

    def set_node_max_depth(self, int idx, double depth):
        """Set the maximum depth of a node.

        :param idx: Node index.
        :param depth: Maximum depth (project length units).
        """
        _check(swmm_node_set_max_depth(self._handle, idx, depth))

    def set_link_nodes(self, int idx, int from_node, int to_node):
        """Set the upstream and downstream nodes for a link.

        :param idx: Link index.
        :param from_node: Upstream node index.
        :param to_node: Downstream node index.
        """
        _check(swmm_link_set_nodes(self._handle, idx, from_node, to_node))

    def set_link_length(self, int idx, double length):
        """Set the length of a conduit link.

        :param idx: Link index.
        :param length: Conduit length (project length units).
        """
        _check(swmm_link_set_length(self._handle, idx, length))

    def set_link_roughness(self, int idx, double n):
        """Set Manning's roughness coefficient for a conduit.

        :param idx: Link index.
        :param n: Manning's *n* (dimensionless).
        """
        _check(swmm_link_set_roughness(self._handle, idx, n))

    def set_link_xsect(self, int idx, int shape,
                        double g1, double g2=0, double g3=0, double g4=0):
        """Set the cross-section geometry of a conduit.

        :param idx: Link index.
        :param shape: Cross-section shape code (see :class:`XSectShape`).
        :param g1: Primary geometry parameter (e.g., diameter for circular).
        :param g2: Secondary geometry parameter.
        :param g3: Tertiary geometry parameter.
        :param g4: Quaternary geometry parameter.
        """
        _check(swmm_link_set_xsect(self._handle, idx, shape, g1, g2, g3, g4))

    def validate(self):
        """Validate model topology (no orphaned links, at least one outfall).

        Does not change state. Safe to call multiple times.

        :raises EngineError: If topology validation fails.
        """
        _check(swmm_validate_model(self._handle))

    def finalize(self):
        """Finalize the model — build connectivity, allocate arrays.

        Transitions to ``INITIALIZED`` state.

        :raises EngineError: If finalisation fails.
        """
        _check(swmm_finalize_model(self._handle))

    def write(self, str path):
        """Write the model to a SWMM ``.inp`` file.

        :param path: Output file path.
        :type path: str
        """
        cdef bytes b = path.encode('utf-8')
        _check(swmm_model_write(self._handle, b))

    # --- Title management ---

    def get_title_count(self) -> int:
        """Return the number of title lines in the [TITLE] section.

        :returns: Number of lines.
        :rtype: int
        """
        cdef int count = 0
        _check(swmm_title_get_count(self._handle, &count))
        return count

    def get_title_line(self, int index) -> str:
        """Return a specific title line by zero-based index.

        :param index: Line index.
        :returns: Title line text.
        :rtype: str
        """
        cdef char buf[1024]
        _check(swmm_title_get_line(self._handle, index, buf, 1024))
        return buf.decode('utf-8')

    def add_title_line(self, str line):
        """Append a new line to the [TITLE] section.

        :param line: Text to append.
        :type line: str
        """
        cdef bytes b = line.encode('utf-8')
        _check(swmm_title_add_line(self._handle, b))

    def set_title(self, str text):
        """Replace all title lines with new text.

        Newline characters in *text* are used as line separators.

        :param text: Title text (may contain ``\\n``).
        :type text: str
        """
        cdef bytes b = text.encode('utf-8')
        _check(swmm_title_set(self._handle, b))

    def clear_title(self):
        """Remove all lines from the [TITLE] section."""
        _check(swmm_title_clear(self._handle))

    def to_solver(self) -> Solver:
        """Transfer ownership of the engine handle to a :class:`Solver`.

        After this call, the :class:`ModelBuilder` is invalidated and must
        not be used. The returned :class:`Solver` owns the engine handle.

        :returns: A new :class:`Solver` wrapping this model's engine.
        :rtype: Solver
        """
        cdef Solver s = Solver.__new__(Solver)
        s._handle = self._handle
        s._elapsed = 0.0
        s._inp = ""
        s._rpt = ""
        s._out = ""
        self._handle = NULL
        return s

    # --- Options ---

    def get_option(self, str key) -> str:
        """Return the value of a model option.

        :param key: Option key name.
        :returns: Option value string.
        :rtype: str
        """
        cdef bytes b = key.encode('utf-8')
        cdef char buf[256]
        _check(swmm_options_get(self._handle, b, buf, 256))
        return buf.decode('utf-8')

    def set_option(self, str key, str value):
        """Set a model option.

        :param key: Option key name.
        :param value: Option value string.
        """
        cdef bytes b_key = key.encode('utf-8')
        cdef bytes b_val = value.encode('utf-8')
        _check(swmm_options_set(self._handle, b_key, b_val))

    def get_option_ext(self, str key) -> str:
        """Return the value of an extended model option.

        :param key: Extended option key name.
        :returns: Extended option value string.
        :rtype: str
        """
        cdef bytes b = key.encode('utf-8')
        cdef char buf[256]
        _check(swmm_options_get_ext(self._handle, b, buf, 256))
        return buf.decode('utf-8')

    def set_option_ext(self, str key, str value):
        """Set an extended model option.

        :param key: Extended option key name.
        :param value: Extended option value string.
        """
        cdef bytes b_key = key.encode('utf-8')
        cdef bytes b_val = value.encode('utf-8')
        _check(swmm_options_set_ext(self._handle, b_key, b_val))

    def get_crs(self) -> str:
        """Return the coordinate reference system string.

        :returns: CRS string.
        :rtype: str
        """
        cdef char buf[256]
        _check(swmm_get_crs(self._handle, buf, 256))
        return buf.decode('utf-8')

    # --- User flags ---

    def get_userflag_bool(self, str name) -> bool:
        """Return a boolean user flag value.

        :param name: Flag name.
        :returns: Flag value.
        :rtype: bool
        """
        cdef bytes b = name.encode('utf-8')
        cdef int v = 0
        _check(swmm_userflag_get_bool(self._handle, b, &v))
        return bool(v)

    def get_userflag_int(self, str name) -> int:
        """Return an integer user flag value.

        :param name: Flag name.
        :returns: Flag value.
        :rtype: int
        """
        cdef bytes b = name.encode('utf-8')
        cdef int v = 0
        _check(swmm_userflag_get_int(self._handle, b, &v))
        return v

    def get_userflag_real(self, str name) -> float:
        """Return a real-valued user flag.

        :param name: Flag name.
        :returns: Flag value.
        :rtype: float
        """
        cdef bytes b = name.encode('utf-8')
        cdef double v = 0.0
        _check(swmm_userflag_get_real(self._handle, b, &v))
        return v

    def set_userflag_bool(self, str name, bint value):
        """Set a boolean user flag.

        :param name: Flag name.
        :param value: Flag value.
        """
        cdef bytes b = name.encode('utf-8')
        _check(swmm_userflag_set_bool(self._handle, b, 1 if value else 0))

    def set_userflag_int(self, str name, int value):
        """Set an integer user flag.

        :param name: Flag name.
        :param value: Flag value.
        """
        cdef bytes b = name.encode('utf-8')
        _check(swmm_userflag_set_int(self._handle, b, value))

    def set_userflag_real(self, str name, double value):
        """Set a real-valued user flag.

        :param name: Flag name.
        :param value: Flag value.
        """
        cdef bytes b = name.encode('utf-8')
        _check(swmm_userflag_set_real(self._handle, b, value))
