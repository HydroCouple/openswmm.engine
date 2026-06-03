"""
Programmatic Model Building
============================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Type stubs for :mod:`openswmm.engine._model`.

The :class:`ModelBuilder` class creates a SWMM model entirely through the API
without requiring a ``.inp`` file.
"""

from datetime import datetime

from ._solver import Solver


class ModelBuilder:
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

    def __init__(self) -> None: ...

    # =========================================================================
    # Nodes
    # =========================================================================

    def add_node(self, node_id: str, node_type: int) -> int:
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
        ...

    def pop_last_node(self, node_id: str) -> int:
        """Remove the most recently added node (undo-of-add).

        Valid in C{BUILDING} or C{OPENED} state. The C{node_id} must match
        the current tail; otherwise C{SWMM_ERR_BADINDEX} is returned.
        Returns C{SWMM_ERR_BADPARAM} if any link still references the tail
        node -- pop those links first via L{pop_last_link}.

        @param node_id: Expected tail node identifier.
        @type node_id: str
        @return: Error code (C{0} on success).
        @rtype: int
        """
        ...

    # =========================================================================
    # Links
    # =========================================================================

    def add_link(self, link_id: str, link_type: int) -> int:
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
        ...

    def pop_last_link(self, link_id: str) -> int:
        """Remove the most recently added link (undo-of-add).

        Valid in C{BUILDING} or C{OPENED} state. The C{link_id} must match
        the current tail; otherwise C{SWMM_ERR_BADINDEX} is returned.

        @param link_id: Expected tail link identifier.
        @type link_id: str
        @return: Error code (C{0} on success).
        @rtype: int
        """
        ...

    # =========================================================================
    # Subcatchments and gages
    # =========================================================================

    def add_subcatchment(self, sc_id: str) -> int:
        """Add a subcatchment to the model.

        @param sc_id: Unique subcatchment identifier.
        @type sc_id: str
        @return: Error code (C{0} on success).
        @rtype: int
        """
        ...

    def add_subcatch(self, sc_id: str) -> int:
        """Backward-compatible alias for L{add_subcatchment}.

        @param sc_id: Unique subcatchment identifier.
        @type sc_id: str
        @return: Error code (C{0} on success).
        @rtype: int
        """
        ...

    def add_gage(self, gage_id: str) -> int:
        """Add a rain gage to the model.

        @param gage_id: Unique gage identifier.
        @type gage_id: str
        @return: Error code (C{0} on success).
        @rtype: int
        """
        ...

    # =========================================================================
    # Node properties
    # =========================================================================

    def set_node_invert(self, idx: int, elev: float) -> None:
        """Set the invert elevation of a node.

        @param idx: Node index.
        @type idx: int
        @param elev: Invert elevation (project length units).
        @type elev: float
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        ...

    def set_node_max_depth(self, idx: int, depth: float) -> None:
        """Set the maximum depth of a node.

        @param idx: Node index.
        @type idx: int
        @param depth: Maximum depth (project length units).
        @type depth: float
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        ...

    # =========================================================================
    # Link properties
    # =========================================================================

    def set_link_nodes(self, idx: int, from_node: int, to_node: int) -> None:
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
        ...

    def set_link_length(self, idx: int, length: float) -> None:
        """Set the length of a conduit link.

        @param idx: Link index.
        @type idx: int
        @param length: Conduit length (project length units).
        @type length: float
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        ...

    def set_link_roughness(self, idx: int, n: float) -> None:
        """Set Manning's roughness coefficient for a conduit.

        @param idx: Link index.
        @type idx: int
        @param n: Manning's I{n} (dimensionless).
        @type n: float
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        ...

    # =========================================================================
    # Cross-sections
    # =========================================================================

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
        ...

    # =========================================================================
    # Validation / finalization
    # =========================================================================

    def validate(self) -> None:
        """Validate model topology.

        Checks for orphaned links and ensures at least one outfall is present.
        Does not change state. Safe to call multiple times.

        @return: None
        @rtype: None
        @raise EngineError: If topology validation fails.
        """
        ...

    def finalize(self) -> None:
        """Finalize the model -- build connectivity and allocate arrays.

        Transitions to C{INITIALIZED} state.

        @return: None
        @rtype: None
        @raise EngineError: If finalization fails.
        """
        ...

    def write(self, path: str) -> None:
        """Write the model to a SWMM C{.inp} file.

        @param path: Output file path.
        @type path: str
        @return: None
        @rtype: None
        @raise EngineError: On C API failure.
        """
        ...

    # =========================================================================
    # [TITLE] section
    # =========================================================================

    def get_title_count(self) -> int:
        """Number of lines in the C{[TITLE]} section.

        @raise EngineError: On C API failure.
        """
        ...

    def get_title_line(self, index: int) -> str:
        """Return a title line by zero-based index.

        @raise EngineError: On bad index.
        """
        ...

    def add_title_line(self, line: str) -> None:
        """Append a new line to the C{[TITLE]} section.

        @raise EngineError: On C API failure.
        """
        ...

    def set_title(self, text: str) -> None:
        """Replace all title lines with new text. Newlines split lines.

        @raise EngineError: On C API failure.
        """
        ...

    def clear_title(self) -> None:
        """Remove all lines from the C{[TITLE]} section.

        @raise EngineError: On C API failure.
        """
        ...

    # =========================================================================
    # Options ([OPTIONS] section)
    # =========================================================================

    def get_option(self, key: str) -> str:
        """Return a SWMM option value as a string.

        @raise EngineError: On unknown key.
        """
        ...

    def set_option(self, key: str, value: str) -> None:
        """Set a SWMM option.

        @raise EngineError: On unknown key or invalid value.
        """
        ...

    def get_file_path(self, role: int, owner: str = ...) -> tuple[str, str]:
        """Read an external-file slot's ``(absolute, original)`` paths.

        @param role: A L{FilePathRole} value.
        @param owner: Owner key for vector slots; ignored for scalar slots.
        @rtype: tuple[str, str]
        """
        ...
    def set_file_path(self, role: int, new_path: str, owner: str = ...) -> None:
        """Set an external-file slot's path token (empty clears it).

        @param role: A L{FilePathRole} value.
        @param owner: Owner key for vector slots; must already exist.
        """
        ...

    def get_option_ext(self, key: str) -> str:
        """Return the value of an extension option (unknown to base SWMM).

        @raise EngineError: On unknown key.
        """
        ...

    def set_option_ext(self, key: str, value: str) -> None:
        """Set the value of an extension option.

        @raise EngineError: On C API failure.
        """
        ...

    def get_crs(self) -> str:
        """Return the model's coordinate reference system string.

        @return: CRS string (EPSG identifier, PROJ string, or WKT).
        @raise EngineError: On C API failure.
        """
        ...

    # =========================================================================
    # User flags
    # =========================================================================

    def get_userflag_bool(self, name: str) -> bool:
        """Return a boolean user flag value.

        @raise EngineError: On unknown flag.
        """
        ...

    def get_userflag_int(self, name: str) -> int:
        """Return an integer user flag value.

        @raise EngineError: On unknown flag.
        """
        ...

    def get_userflag_real(self, name: str) -> float:
        """Return a real-valued user flag.

        @raise EngineError: On unknown flag.
        """
        ...

    def set_userflag_bool(self, name: str, value: bool) -> None:
        """Set a boolean user flag.

        @raise EngineError: On C API failure.
        """
        ...

    def set_userflag_int(self, name: str, value: int) -> None:
        """Set an integer user flag.

        @raise EngineError: On C API failure.
        """
        ...

    def set_userflag_real(self, name: str, value: float) -> None:
        """Set a real-valued user flag.

        @raise EngineError: On C API failure.
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

    # =========================================================================
    # Handle
    # =========================================================================

    @property
    def handle(self) -> int:
        """Raw engine handle as an integer (for use by L{ModelEditor}).

        @return: The underlying C engine pointer cast to an integer.
        @rtype: int
        """
        ...

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
        ...

    # =========================================================================
    # Plugins
    # =========================================================================

    def plugins_count(self) -> int:
        """Return the number of [PLUGINS] entries on the engine.

        @return: Plugin count.
        """
        ...

    def plugin_get(self, idx: int) -> tuple[str, str]:
        """Read one [PLUGINS] row by index.

        @param idx: Plugin index in C{[0, plugins_count())}.
        @return: Tuple C{(path, args)}.
        """
        ...

    def plugin_set(self, path_or_id: str, args: str = "") -> None:
        """Add or replace a [PLUGINS] row keyed by path/id.

        @param path_or_id: Library path, plugin id, or C{id:version} string.
        @param args: Space-separated argument tokens.
        """
        ...

    def plugin_remove(self, path_or_id: str) -> None:
        """Remove the [PLUGINS] row matching C{path_or_id}.

        @param path_or_id: Library path, plugin id, or C{id:version} string.
        """
        ...

    # =========================================================================
    # [FILES] section
    # =========================================================================

    def files_get(self, key: str) -> str:
        """Read one [FILES] field by key.

        @param key: Field key (e.g. C{"RAINFALL_PATH"}, C{"HOTSTART_USE_PATH"}).
        @return: Field value string.
        """
        ...

    def files_set(self, key: str, value: str) -> None:
        """Write one [FILES] field by key.

        @param key: Field key.
        @param value: New value; C{""} to clear.
        """
        ...

    # =========================================================================
    # Write with plugin
    # =========================================================================

    def write_with_plugin(self, path: str, output_plugin_id: str = "") -> None:
        """Write the current model to disk using an output plugin.

        @param path: Destination file path.
        @param output_plugin_id: Plugin id, or C{""} for the built-in writer.
        """
        ...

