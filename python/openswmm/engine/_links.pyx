"""
Link Access
===========

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

The :class:`Links` class provides access to link (conduit/pump/orifice/weir/outlet)
properties during a simulation.

.. code-block:: python

    from openswmm.engine import Solver, Links

    with Solver("model.inp", "model.rpt", "model.out") as s:
        links = Links(s)
        while s.state == EngineState.RUNNING:
            if s.step() != 0:
                break
            flow = links.get_flow("C1")             # by name
            links.set_control_setting(0, 0.5)        # or by index

Bulk array access (numpy)
-------------------------

.. code-block:: python

    flows = links.get_flows_bulk()  # np.ndarray
"""

# cython: language_level=3

import numpy as np
cimport numpy as np

from ._common cimport *


class Links:
    """Access link properties during a simulation.

    All per-element methods accept either an integer index or a string link
    ID.  When a string is passed it is resolved via L{get_index}.

    @param solver: An active L{openswmm.engine.Solver} instance. The
        solver must remain alive for the lifetime of this object.
    @type solver: L{openswmm.engine.Solver}

    Example::

        links = Links(solver)
        flow = links.get_flow(0)      # by index
        flow = links.get_flow("C1")   # by name
    """

    def __init__(self, solver):
        self._solver = solver

    def _resolve(self, idx) -> int:
        """Resolve C{idx} to an integer index.

        @param idx: Integer index or string link ID.
        @type idx: Union[int, str]
        @return: Integer index.
        @rtype: int
        @raise KeyError: If a string ID is not found.
        """
        cdef int i
        if isinstance(idx, str):
            i = self.get_index(idx)
            if i < 0:
                raise KeyError(f"Link '{idx}' not found")
            return i
        return idx

    # ====================================================================
    # Identification & lookup
    # ====================================================================

    def count(self) -> int:
        """Return the number of links in the model.

        @return: Link count.
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_link_count(h)

    def get_index(self, str link_id) -> int:
        """Return the integer index of a link by its string ID.

        @param link_id: Link identifier string.
        @type link_id: str
        @return: Link index, or C{-1} if not found.
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = link_id.encode('utf-8')
        return swmm_link_index(h, b)

    def get_id(self, int idx) -> str:
        """Return the string ID of a link by index.

        @param idx: Link index.
        @type idx: int
        @return: Link ID string.
        @rtype: str
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef const char* raw = swmm_link_id(h, idx)
        return raw.decode('utf-8') if raw != NULL else ""

    # ====================================================================
    # Construction (add/pop_last)
    # ====================================================================

    def add(self, str link_id, int link_type) -> int:
        """Add a link to the model (OPENED-state editing).

        Wraps C{swmm_link_add}. Valid in C{BUILDING} or C{OPENED} state.
        Use on a L{Solver} that has been opened for interactive
        editing of an existing model. For from-scratch construction
        without an .inp file, use L{ModelBuilder.add_link}.

        @param link_id: Unique link identifier.
        @type link_id: str
        @param link_type: Link type code (see L{LinkType}).
        @type link_type: int
        @return: Error code (C{0} on success).
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = link_id.encode('utf-8')
        return swmm_link_add(h, b, link_type)

    def pop_last(self, str link_id) -> int:
        """Remove the most recently added link (undo-of-add).

        Wraps C{swmm_link_pop_last}. Valid in C{BUILDING} or C{OPENED}
        state. C{link_id} must match the current tail; otherwise
        C{SWMM_ERR_BADINDEX} is returned.

        @param link_id: Expected tail link identifier.
        @type link_id: str
        @return: Error code (C{0} on success).
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = link_id.encode('utf-8')
        return swmm_link_pop_last(h, b)

    # ====================================================================
    # Connectivity
    # ====================================================================

    def set_nodes(self, idx, int from_node, int to_node):
        """Set the upstream and downstream nodes of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param from_node: Upstream node index.
        @type from_node: int
        @param to_node: Downstream node index.
        @type to_node: int
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        @raise EngineError: If the C API rejects the assignment.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_nodes(h, i, from_node, to_node))

    def get_from_node(self, idx) -> int:
        """Return the upstream (from) node index of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Node index of the upstream node.
        @rtype: int
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_link_get_from_node(h, i, &v))
        return v

    def get_to_node(self, idx) -> int:
        """Return the downstream (to) node index of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Node index of the downstream node.
        @rtype: int
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_link_get_to_node(h, i, &v))
        return v

    # ====================================================================
    # Geometry/cross-section
    # ====================================================================

    def set_length(self, idx, double length):
        """Set the length of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param length: Link length in project units.
        @type length: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        @raise EngineError: If the C API rejects the assignment.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_length(h, i, length))

    def set_roughness(self, idx, double n):
        """Set the Manning's roughness of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param n: Manning's M{n} roughness coefficient.
        @type n: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        @raise EngineError: If the C API rejects the assignment.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_roughness(h, i, n))

    def get_type(self, idx) -> int:
        """Return the type code of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Link type code (e.g. conduit, pump, orifice, weir, outlet).
        @rtype: int
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_link_get_type(h, i, &v))
        return v

    def get_length(self, idx) -> float:
        """Return the length of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Length (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_length(h, i, &v))
        return v

    def get_roughness(self, idx) -> float:
        """Return the roughness coefficient of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Manning's roughness coefficient.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_roughness(h, i, &v))
        return v

    def get_slope(self, idx) -> float:
        """Return the slope of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Slope (dimensionless).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_slope(h, i, &v))
        return v

    def get_offset_up(self, idx) -> float:
        """Return the upstream invert offset of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Upstream offset (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_offset_up(h, i, &v))
        return v

    def get_offset_dn(self, idx) -> float:
        """Return the downstream invert offset of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Downstream offset (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_offset_dn(h, i, &v))
        return v

    def get_xsect(self, idx) -> tuple:
        """Return the cross-section geometry of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Tuple of C{(shape, geom1, geom2, geom3, geom4)}.
        @rtype: tuple
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int shape = 0
        cdef double geom1 = 0.0, geom2 = 0.0, geom3 = 0.0, geom4 = 0.0
        _check(swmm_link_get_xsect(h, i, &shape, &geom1, &geom2, &geom3, &geom4))
        return (shape, geom1, geom2, geom3, geom4)

    def set_xsect(self, idx, int shape,
                  double geom1, double geom2=0.0,
                  double geom3=0.0, double geom4=0.0):
        """Set the cross-section geometry of a link.

        Geometry parameter semantics depend on C{shape} (see
        L{CrossSection.geom_labels}). For example, a circular conduit takes
        C{geom1 = diameter}; a rectangular shape takes C{geom1 = depth} and
        C{geom2 = width}.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param shape: Cross-section shape code (see L{XSectShape} enum).
        @type shape: int
        @param geom1: First geometry parameter (units depend on C{shape}).
        @param geom2: Second geometry parameter.
        @param geom3: Third geometry parameter.
        @param geom4: Fourth geometry parameter.
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        @raise EngineError: On invalid shape, negative geometry, or other
            C API failure.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_xsect(h, i, shape, geom1, geom2, geom3, geom4))

    def get_xsect_info(self, idx):
        """Return cross-section geometry as a L{CrossSection} object.

        Wraps L{get_xsect} and resolves the shape code to a human-readable
        name.  The returned object exposes a C{geom_labels} property that maps
        each geometry parameter to its semantic name for the given shape
        (e.g. C{{"diameter": 1.2}} for a circular pipe).

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Structured cross-section geometry.
        @rtype: CrossSection
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        from ._geometry import CrossSection, _XSECT_SHAPE_NAMES
        shape, g1, g2, g3, g4 = self.get_xsect(idx)
        return CrossSection(
            shape=shape,
            shape_name=_XSECT_SHAPE_NAMES.get(shape, f"SHAPE_{shape}"),
            geom1=g1,
            geom2=g2,
            geom3=g3,
            geom4=g4,
        )

    def set_offset_up(self, idx, double offset):
        """Set the upstream invert offset of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param offset: Upstream offset (project length units).
        @type offset: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_offset_up(h, i, offset))

    def set_offset_dn(self, idx, double offset):
        """Set the downstream invert offset of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param offset: Downstream offset (project length units).
        @type offset: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_offset_dn(h, i, offset))

    def set_initial_flow(self, idx, double flow):
        """Set the initial flow in a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param flow: Initial flow rate (project flow units).
        @type flow: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_initial_flow(h, i, flow))

    def set_max_flow(self, idx, double flow):
        """Set the maximum flow capacity of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param flow: Maximum flow rate (project flow units).
        @type flow: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_max_flow(h, i, flow))

    def get_initial_flow(self, idx) -> float:
        """Return the initial flow assigned to a link.

        Symmetric reader for L{set_initial_flow} — engine gap BN-LINK-01a
        (added 2026-05-25).

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_initial_flow(h, i, &v))
        return v

    def get_max_flow(self, idx) -> float:
        """Return the maximum flow limit assigned to a link (0 = no limit).

        Symmetric reader for L{set_max_flow} — engine gap BN-LINK-01b
        (added 2026-05-25).

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_max_flow(h, i, &v))
        return v

    def get_orifice_type(self, idx) -> int:
        """Return the orifice flow-attack classification (0=SIDE, 1=BOTTOM).

        Engine gap BN-LINK-02 (added 2026-05-25). Returns SWMM_ERR_BADPARAM
        when C{idx} names a non-orifice link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int t = 0
        _check(swmm_link_get_orifice_type(h, i, &t))
        return t

    def set_orifice_type(self, idx, int type_):
        """Set the orifice flow-attack classification.

        Engine gap BN-LINK-02 (added 2026-05-25).

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param type_: 0 for SIDE, 1 for BOTTOM.
        @type type_: int
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_orifice_type(h, i, type_))

    def get_weir_type(self, idx) -> int:
        """Return the weir flow classification.

        Engine gap BN-LINK-03 (added 2026-05-25). Returns one of
        0=TRANSVERSE, 1=SIDEFLOW, 2=VNOTCH, 3=TRAPEZOIDAL, 4=ROADWAY.
        Errors if C{idx} names a non-weir link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int t = 0
        _check(swmm_link_get_weir_type(h, i, &t))
        return t

    def set_weir_type(self, idx, int type_):
        """Set the weir flow classification.

        Engine gap BN-LINK-03 (added 2026-05-25). Accepts 0..4.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param type_: 0=TRANSVERSE, 1=SIDEFLOW, 2=VNOTCH, 3=TRAPEZOIDAL, 4=ROADWAY.
        @type type_: int
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_weir_type(h, i, type_))

    def get_outlet_rating_type(self, idx) -> int:
        """Return the outlet rating-curve classification (0..3).

        Engine gap BN-LINK-04 (added 2026-05-25).
        0=FUNCTIONAL_HEAD, 1=FUNCTIONAL_DEPTH, 2=TABULAR_HEAD, 3=TABULAR_DEPTH.
        Errors for non-outlet links.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int t = 0
        _check(swmm_link_get_outlet_rating_type(h, i, &t))
        return t

    def set_outlet_rating_type(self, idx, int type_):
        """Set the outlet rating-curve classification (0..3).

        Engine gap BN-LINK-04 (added 2026-05-25). Errors for non-outlet
        links or out-of-range values.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_outlet_rating_type(h, i, type_))

    def get_outlet_expon(self, idx) -> float:
        """Return the outlet functional-form exponent.

        Engine gap BN-LINK-04 (added 2026-05-25). Meaningful only for
        FUNCTIONAL_* rating types; engine ignores the value when the
        type is TABULAR_*. Errors for non-outlet links.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_outlet_expon(h, i, &v))
        return v

    def set_outlet_expon(self, idx, double expon):
        """Set the outlet functional-form exponent.

        Engine gap BN-LINK-04 (added 2026-05-25). Errors for non-outlet
        links.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_outlet_expon(h, i, expon))

    def get_pump_startup_depth(self, idx) -> float:
        """Return the pump startup depth (project length units).

        Engine gap BN-LINK-05 (added 2026-05-25). Errors for non-pump links.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_pump_startup_depth(h, i, &v))
        return v

    def set_pump_startup_depth(self, idx, double depth):
        """Set the pump startup depth.

        Engine gap BN-LINK-05 (added 2026-05-25). Errors for non-pump links.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_pump_startup_depth(h, i, depth))

    def get_pump_shutoff_depth(self, idx) -> float:
        """Return the pump shutoff depth (project length units).

        Engine gap BN-LINK-05 (added 2026-05-25). Errors for non-pump links.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_pump_shutoff_depth(h, i, &v))
        return v

    def set_pump_shutoff_depth(self, idx, double depth):
        """Set the pump shutoff depth.

        Engine gap BN-LINK-05 (added 2026-05-25). Errors for non-pump links.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_pump_shutoff_depth(h, i, depth))

    def get_orifice_open_close_rate(self, idx) -> float:
        """Return the orifice open/close rate (fraction per second).

        Engine gap BN-LINK-06 (added 2026-05-25). 0 = instantaneous.
        Errors for non-orifice links.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_orifice_open_close_rate(h, i, &v))
        return v

    def set_orifice_open_close_rate(self, idx, double rate):
        """Set the orifice open/close rate (fraction per second).

        Engine gap BN-LINK-06 (added 2026-05-25). 0 = instantaneous.
        Errors for non-orifice links.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_orifice_open_close_rate(h, i, rate))

    # ====================================================================
    # Per-element flow/depth state
    # ====================================================================

    def get_flow(self, idx) -> float:
        """Return the current flow rate in a link.

        @param idx: Link index (int) or link ID (str). Strings are
            resolved via L{get_index}.
        @type idx: Union[int, str]
        @return: Flow rate (project flow units). Positive = node1 -> node2.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_flow(h, i, &v))
        return v

    def set_flow(self, idx, double value):
        """Set the flow rate in a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param value: New flow rate (project flow units).
        @type value: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_flow(h, i, value))

    def get_depth(self, idx) -> float:
        """Return the current water depth at the midpoint of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Water depth (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_depth(h, i, &v))
        return v

    def get_velocity(self, idx) -> float:
        """Return the current flow velocity in a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Velocity (project length / time units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_velocity(h, i, &v))
        return v

    def get_capacity(self, idx) -> float:
        """Return the fraction of full capacity used in a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Capacity fraction (C{0.0}-C{1.0}).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_capacity(h, i, &v))
        return v

    def get_volume(self, idx) -> float:
        """Return the current volume stored in a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Volume (project volume units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_volume(h, i, &v))
        return v

    # ====================================================================
    # Setters & control settings
    # ====================================================================

    def set_control_setting(self, idx, double setting):
        """Override the control/pump setting on a link (RUNNING state).

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param setting: New setting (C{0.0}=closed to C{1.0}=fully open for
            orifices/weirs; C{0.0}=off to C{1.0}=full speed for pumps).
        @type setting: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_control_setting(h, i, setting))

    def get_control_setting(self, idx) -> float:
        """Return the current control setting of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Setting value (C{0.0}-C{1.0}).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_control_setting(h, i, &v))
        return v

    def set_target_setting(self, idx, double setting):
        """Set the target control setting for a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param setting: Target setting value.
        @type setting: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_target_setting(h, i, setting))

    def get_target_setting(self, idx) -> float:
        """Return the target control setting of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Target setting value.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_target_setting(h, i, &v))
        return v

    def set_closed(self, idx, bint closed):
        """Set whether a link is forced closed.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param closed: C{True} to force the link closed.
        @type closed: bool
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_closed(h, i, closed))

    def get_closed(self, idx) -> bool:
        """Return whether a link is forced closed.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: C{True} if the link is forced closed.
        @rtype: bool
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_link_get_closed(h, i, &v))
        return bool(v)

    # ====================================================================
    # Pump
    # ====================================================================

    def set_pump_curve(self, idx, int curve_idx):
        """Set the pump curve index for a pump link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param curve_idx: Index of the pump curve.
        @type curve_idx: int
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_pump_curve(h, i, curve_idx))

    def get_pump_curve(self, idx) -> int:
        """Return the pump curve index for a pump link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Pump curve index.
        @rtype: int
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_link_get_pump_curve(h, i, &v))
        return v

    def set_pump_init_state(self, idx, bint on):
        """Set the initial on/off state of a pump link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param on: C{True} to set the pump initially on.
        @type on: bool
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_pump_init_state(h, i, on))

    def get_pump_init_state(self, idx) -> bool:
        """Return the initial on/off state of a pump link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: C{True} if the pump is initially on.
        @rtype: bool
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_link_get_pump_init_state(h, i, &v))
        return bool(v)

    # ====================================================================
    # Weir
    # ====================================================================

    def set_crest_height(self, idx, double h):
        """Set the crest height of a weir link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param h: Crest height (project length units).
        @type h: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine handle = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_crest_height(handle, i, h))

    def get_crest_height(self, idx) -> float:
        """Return the crest height of a weir link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Crest height (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_crest_height(h, i, &v))
        return v

    def set_discharge_coeff(self, idx, double cd):
        """Set the discharge coefficient of a weir link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param cd: Discharge coefficient.
        @type cd: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_discharge_coeff(h, i, cd))

    def get_discharge_coeff(self, idx) -> float:
        """Return the discharge coefficient of a weir link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Discharge coefficient.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_discharge_coeff(h, i, &v))
        return v

    def set_end_contractions(self, idx, double n):
        """Set the number of end contractions for a weir link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param n: Number of end contractions.
        @type n: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_end_contractions(h, i, n))

    def get_end_contractions(self, idx) -> float:
        """Return the number of end contractions for a weir link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Number of end contractions.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_end_contractions(h, i, &v))
        return v

    # ====================================================================
    # Loss / misc
    # ====================================================================

    def set_loss_coeff(self, idx, double inlet, double outlet, double avg):
        """Set the loss coefficients for a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param inlet: Inlet loss coefficient.
        @type inlet: float
        @param outlet: Outlet loss coefficient.
        @type outlet: float
        @param avg: Average loss coefficient.
        @type avg: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_loss_coeff(h, i, inlet, outlet, avg))

    def get_loss_coeff(self, idx) -> tuple:
        """Return the loss coefficients for a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Tuple of C{(inlet, outlet, avg)} loss coefficients.
        @rtype: tuple
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double inlet = 0.0, outlet = 0.0, avg = 0.0
        _check(swmm_link_get_loss_coeff(h, i, &inlet, &outlet, &avg))
        return (inlet, outlet, avg)

    def set_flap_gate(self, idx, bint has_gate):
        """Set whether a link has a flap gate.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param has_gate: C{True} if the link has a flap gate.
        @type has_gate: bool
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_flap_gate(h, i, has_gate))

    def get_flap_gate(self, idx) -> bool:
        """Return whether a link has a flap gate.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: C{True} if the link has a flap gate.
        @rtype: bool
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_link_get_flap_gate(h, i, &v))
        return bool(v)

    def set_seep_rate(self, idx, double rate):
        """Set the seepage rate for a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param rate: Seepage rate (project flow units per unit length).
        @type rate: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_seep_rate(h, i, rate))

    def get_seep_rate(self, idx) -> float:
        """Return the seepage rate for a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Seepage rate.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_seep_rate(h, i, &v))
        return v

    def set_culvert_code(self, idx, int code):
        """Set the culvert inlet geometry code for a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param code: Culvert code number.
        @type code: int
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_culvert_code(h, i, code))

    def get_culvert_code(self, idx) -> int:
        """Return the culvert inlet geometry code for a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Culvert code number.
        @rtype: int
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_link_get_culvert_code(h, i, &v))
        return v

    def set_barrels(self, idx, int n):
        """Set the number of barrels for a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param n: Number of barrels.
        @type n: int
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_barrels(h, i, n))

    def get_barrels(self, idx) -> int:
        """Return the number of barrels for a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Number of barrels.
        @rtype: int
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_link_get_barrels(h, i, &v))
        return v

    # ====================================================================
    # Pollutant/quality
    # ====================================================================

    def get_quality(self, idx, int pollutant_idx) -> float:
        """Return the concentration of a pollutant in a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param pollutant_idx: Pollutant index.
        @type pollutant_idx: int
        @return: Pollutant concentration.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_quality(h, i, pollutant_idx, &v))
        return v

    # ====================================================================
    # Statistics
    # ====================================================================

    def get_stat_max_flow(self, idx) -> float:
        """Return the maximum flow recorded for a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Maximum flow (project flow units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_stat_max_flow(h, i, &v))
        return v

    def get_stat_max_velocity(self, idx) -> float:
        """Return the maximum velocity recorded for a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Maximum velocity (project length / time units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_stat_max_velocity(h, i, &v))
        return v

    def get_stat_max_filling(self, idx) -> float:
        """Return the maximum filling fraction recorded for a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Maximum filling fraction (C{0.0}-C{1.0}).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_stat_max_filling(h, i, &v))
        return v

    def get_stat_vol_flow(self, idx) -> float:
        """Return the total volume of flow through a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Total volume of flow (project volume units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_stat_vol_flow(h, i, &v))
        return v

    def get_stat_surcharge_time(self, idx) -> float:
        """Return the total time a link was surcharged.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Surcharge duration (simulation time units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_stat_surcharge_time(h, i, &v))
        return v

    def get_stat_pump_cycles(self, idx) -> int:
        """Return pump on/off cycle count.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Number of pump on->off and off->on transitions.
        @rtype: int
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_link_get_stat_pump_cycles(h, i, &v))
        return v

    def get_stat_pump_on_time(self, idx) -> float:
        """Return total pump on-time (seconds).

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Total time pump was running (seconds).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_stat_pump_on_time(h, i, &v))
        return v

    def get_stat_pump_volume(self, idx) -> float:
        """Return total volume pumped.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Total volume pumped (project volume units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_stat_pump_volume(h, i, &v))
        return v

    def get_hyd_power(self, idx) -> float:
        """Return hydraulic power dissipated in a link (ft-lb/s).

        C{P = gamma * |Q| * |hL|} where C{gamma} = specific weight of water.
        Divide by C{550} to convert to horsepower.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Hydraulic power (ft-lb/s).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_hyd_power(h, i, &v))
        return v

    def get_pump_stats_bulk(self):
        """Return pump utilization statistics for **all** links in one call.

        This is the bulk equivalent of calling :py:meth:`get_stat_pump_cycles`,
        :py:meth:`get_stat_pump_on_time`, and :py:meth:`get_stat_pump_volume`
        for every link. It performs a single C ABI crossing instead of
        C{3 * n_links}, which makes it the preferred accessor when assembling
        a network-wide pump summary.

        Non-pump links are tagged with a sentinel: ``cycles[i] == -1`` and
        ``on_time[i] == volume[i] == 0.0``. Use the cycles sentinel (not the
        zero values, which are also valid for an inactive pump) to filter:

        .. code-block:: python

            stats = links.get_pump_stats_bulk()
            mask = stats["cycles"] >= 0          # pumps only
            pump_indices = np.where(mask)[0]
            total_volume = stats["volume"][mask].sum()

        The GIL is released for the duration of the C call.

        :returns: A dict with three NumPy arrays of length ``n_links``:

                  * ``cycles`` — ``int32`` array of pump on/off cycle counts;
                    ``-1`` where the link is not a pump.
                  * ``on_time`` — ``float64`` array of total pump on-time in
                    seconds; ``0.0`` for non-pumps.
                  * ``volume`` — ``float64`` array of total volume pumped in
                    ft³; ``0.0`` for non-pumps.

        :rtype: dict[str, numpy.ndarray]

        .. versionadded:: 6.0.0

        .. seealso::

           :py:meth:`get_stat_pump_cycles`,
           :py:meth:`get_stat_pump_on_time`,
           :py:meth:`get_stat_pump_volume` — equivalent per-link scalar
           accessors.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        # Allocate NumPy buffers up-front; the C call only sees raw pointers
        # so the GIL can be safely released during the iteration.
        cdef np.ndarray[int, ndim=1] cycles = np.empty(n, dtype=np.intc)
        cdef np.ndarray[double, ndim=1] on_time = np.empty(n, dtype=np.float64)
        cdef np.ndarray[double, ndim=1] volume = np.empty(n, dtype=np.float64)
        cdef int err
        cdef int* p_cycles = <int*>cycles.data
        cdef double* p_on = <double*>on_time.data
        cdef double* p_vol = <double*>volume.data
        with nogil:
            err = swmm_link_get_pump_stats_bulk(h, p_cycles, p_on, p_vol, n)
        _check(err)
        return {"cycles": cycles, "on_time": on_time, "volume": volume}

    # ====================================================================
    # Bulk array access (numpy)
    # ====================================================================

    # Each bulk accessor below releases the GIL for its single C call —
    # see the analogous comment block in `_nodes.pyx` for the rationale
    # and the pre-/post-call pattern.

    def get_flows_bulk(self):
        """Return all link flows as a NumPy array. GIL is released during
        the C call.

        @return: Array of shape C{(n_links,)} with dtype C{float64}.
        @rtype: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef double* p = <double*>buf.data
        cdef int err
        with nogil:
            err = swmm_link_get_flows_bulk(h, p, n)
        _check(err)
        return buf

    def set_flows_bulk(self, np.ndarray[double, ndim=1] values):
        """Set all link flows from a NumPy array. GIL is released during
        the C call.

        @param values: Array of shape C{(n_links,)} with dtype C{float64}.
        @type values: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef const double* p = <const double*>values.data
        cdef int err
        with nogil:
            err = swmm_link_set_flows_bulk(h, p, n)
        _check(err)

    def get_depths_bulk(self):
        """Return all link depths as a NumPy array. GIL is released during
        the C call.

        @return: Array of shape C{(n_links,)} with dtype C{float64}.
        @rtype: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef double* p = <double*>buf.data
        cdef int err
        with nogil:
            err = swmm_link_get_depths_bulk(h, p, n)
        _check(err)
        return buf

    def get_quality_bulk(self, int pollutant_idx):
        """Return all link pollutant concentrations as a NumPy array. GIL
        is released during the C call.

        @param pollutant_idx: Pollutant index.
        @type pollutant_idx: int
        @return: Array of shape C{(n_links,)} with dtype C{float64}.
        @rtype: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef double* p = <double*>buf.data
        cdef int err
        with nogil:
            err = swmm_link_get_quality_bulk(h, pollutant_idx, p, n)
        _check(err)
        return buf

    # ------------------------------------------------------------------
    # Phase 3 bulk getters — velocities / capacities / volumes /
    # control_settings / target_settings / hyd_powers / ids.
    #
    # Velocities, capacities, and hydraulic powers are derived quantities
    # (per-link calculation in C, not a memcpy from a SoA column). The
    # bulk forms still save the N-call ABI crossing cost. GIL is released
    # for each C call.
    # ------------------------------------------------------------------

    def get_velocities_bulk(self):
        """Return cross-sectional velocities for all links as a NumPy
        array. Computed per link in C as ``q / area`` (area approximated
        from ``d / y_full * a_full``). GIL is released during the C call.

        :returns: Array of shape ``(n_links,)``, dtype ``float64``, in
                  project length/time units.
        :rtype: numpy.ndarray

        .. versionadded:: 6.0.0
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef double* p = <double*>buf.data
        cdef int err
        with nogil:
            err = swmm_link_get_velocities_bulk(h, p, n)
        _check(err)
        return buf

    def get_capacities_bulk(self):
        """Return capacity ratios (``q / q_full``) for all links as a
        NumPy array. GIL is released during the C call.

        :returns: Array of shape ``(n_links,)``, dtype ``float64``,
                  dimensionless ratio (>= 1 indicates surcharge).
        :rtype: numpy.ndarray

        .. versionadded:: 6.0.0
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef double* p = <double*>buf.data
        cdef int err
        with nogil:
            err = swmm_link_get_capacities_bulk(h, p, n)
        _check(err)
        return buf

    def get_volumes_bulk(self):
        """Return stored volumes for all links as a NumPy array. GIL is
        released during the C call.

        :returns: Array of shape ``(n_links,)``, dtype ``float64``, in
                  project volume units.
        :rtype: numpy.ndarray

        .. versionadded:: 6.0.0
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef double* p = <double*>buf.data
        cdef int err
        with nogil:
            err = swmm_link_get_volumes_bulk(h, p, n)
        _check(err)
        return buf

    def get_control_settings_bulk(self):
        """Return active control settings (0..1) for all links as a NumPy
        array. GIL is released during the C call.

        :returns: Array of shape ``(n_links,)``, dtype ``float64``.
                  ``0.0`` = closed, ``1.0`` = fully open.
        :rtype: numpy.ndarray

        .. versionadded:: 6.0.0
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef double* p = <double*>buf.data
        cdef int err
        with nogil:
            err = swmm_link_get_control_settings_bulk(h, p, n)
        _check(err)
        return buf

    def get_target_settings_bulk(self):
        """Return target control settings for all links as a NumPy array.
        GIL is released during the C call.

        :returns: Array of shape ``(n_links,)``, dtype ``float64``.
        :rtype: numpy.ndarray

        .. versionadded:: 6.0.0
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef double* p = <double*>buf.data
        cdef int err
        with nogil:
            err = swmm_link_get_target_settings_bulk(h, p, n)
        _check(err)
        return buf

    def get_hyd_powers_bulk(self):
        """Return hydraulic power dissipated in every link as a NumPy
        array. ``P = gamma * |Q| * |h_up - h_dn|`` (ft-lb/s). GIL is
        released during the C call.

        :returns: Array of shape ``(n_links,)``, dtype ``float64``,
                  in ft-lb/s. Divide by 550 for horsepower.
        :rtype: numpy.ndarray

        .. seealso::

           :py:meth:`get_pump_stats_bulk` — pair this with ``cycles >= 0``
           to filter to pumps when assembling a pump-energy summary.

        .. versionadded:: 6.0.0
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        cdef double* p = <double*>buf.data
        cdef int err
        with nogil:
            err = swmm_link_get_hyd_powers_bulk(h, p, n)
        _check(err)
        return buf

    def get_ids_bulk(self, int stride=64):
        """Return all link IDs as a Python list of strings in a single C
        call (stride-packed UTF-8). GIL is released during the C copy;
        per-slot decoding runs afterwards.

        :param stride: Per-ID slot size in bytes (default 64). IDs
                       longer than ``stride - 1`` are truncated.
        :type stride: int
        :returns: List of ``n_links`` Python strings.
        :rtype: list[str]

        .. versionadded:: 6.0.0
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[char, ndim=1, mode="c"] buf = np.zeros(
            n * stride, dtype=np.int8)
        cdef char* p = <char*>buf.data
        cdef int err
        with nogil:
            err = swmm_link_get_ids_bulk(h, p, stride, n)
        _check(err)
        raw = bytes(buf)
        ids = []
        for i in range(n):
            slot = raw[i * stride:(i + 1) * stride]
            nul = slot.find(b"\x00")
            if nul >= 0:
                slot = slot[:nul]
            ids.append(slot.decode("utf-8"))
        return ids

    # ====================================================================
    # Rename
    # ====================================================================

    def rename(self, idx, str new_id):
        """Rename a link.

        @param idx: Link index (int) or current link ID (str).
        @type idx: Union[int, str]
        @param new_id: New identifier string.
        @type new_id: str
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        @raise EngineError: If the C API rejects the rename.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = new_id.encode('utf-8')
        _check(swmm_link_rename(h, i, b))

