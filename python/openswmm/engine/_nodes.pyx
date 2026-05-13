"""
Node Access
===========

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

The :class:`Nodes` class provides access to node properties during a
simulation. Create an instance by passing an active :class:`Solver`:

.. code-block:: python

    from openswmm.engine import Solver, Nodes

    with Solver("model.inp", "model.rpt", "model.out") as s:
        nodes = Nodes(s)
        print(f"Node count: {nodes.count()}")
        while s.state == EngineState.RUNNING:
            if s.step() != 0:
                break
            depth = nodes.get_depth("J1")       # by name
            nodes.set_lateral_inflow(0, 0.5)     # or by index

Bulk array access (numpy)
-------------------------

.. code-block:: python

    depths = nodes.get_depths_bulk()   # np.ndarray, shape (n_nodes,)
    depths *= 1.1                      # modify in-place
    nodes.set_depths_bulk(depths)      # write back
"""

# cython: language_level=3

import numpy as np
cimport numpy as np

from ._common cimport *


class Nodes:
    """Access node properties during a simulation.

    All per-element methods accept either an integer index or a string node
    ID.  When a string is passed it is resolved via L{get_index}.

    @param solver: An active L{openswmm.engine.Solver} instance. The
        solver must remain alive for the lifetime of this object.
    @type solver: L{openswmm.engine.Solver}

    Example::

        nodes = Nodes(solver)
        depth = nodes.get_depth(0)      # by index
        depth = nodes.get_depth("J1")   # by name
    """

    def __init__(self, solver):
        self._solver = solver

    def _resolve(self, idx) -> int:
        """Resolve C{idx} to an integer index.

        @param idx: Integer index or string node ID.
        @type idx: Union[int, str]
        @return: Integer index.
        @rtype: int
        @raise KeyError: If a string ID is not found.
        """
        cdef int i
        if isinstance(idx, str):
            i = self.get_index(idx)
            if i < 0:
                raise KeyError(f"Node '{idx}' not found")
            return i
        return idx

    # ====================================================================
    # Identification & lookup
    # ====================================================================

    def count(self) -> int:
        """Return the number of nodes in the model.

        @return: Node count.
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_node_count(h)

    def get_index(self, str node_id) -> int:
        """Return the integer index of a node by its string ID.

        @param node_id: Node identifier string.
        @type node_id: str
        @return: Node index, or C{-1} if not found.
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = node_id.encode('utf-8')
        return swmm_node_index(h, b)

    def get_id(self, int idx) -> str:
        """Return the string ID of a node by index.

        @param idx: Node index.
        @type idx: int
        @return: Node ID string.
        @rtype: str
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef const char* raw = swmm_node_id(h, idx)
        return raw.decode('utf-8') if raw != NULL else ""

    # ====================================================================
    # Construction (add/pop_last)
    # ====================================================================

    def add(self, str node_id, int node_type) -> int:
        """Add a node to the model (OPENED-state editing).

        Wraps C{swmm_node_add}. Valid in C{BUILDING} or C{OPENED} state.
        Use this on a L{Solver} that has been opened for interactive
        editing of an existing model. For from-scratch construction without
        an .inp file, use L{ModelBuilder.add_node} instead.

        @param node_id: Unique node identifier.
        @type node_id: str
        @param node_type: Node type code (see L{NodeType}).
        @type node_type: int
        @return: Error code (C{0} on success, C{SWMM_ERR_LIFECYCLE} if the
            engine is not in an editable state).
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = node_id.encode('utf-8')
        return swmm_node_add(h, b, node_type)

    def pop_last(self, str node_id) -> int:
        """Remove the most recently added node (undo-of-add).

        Wraps C{swmm_node_pop_last}. Valid in C{BUILDING} or C{OPENED}
        state. C{node_id} must match the current tail; otherwise
        C{SWMM_ERR_BADINDEX} is returned. Returns C{SWMM_ERR_BADPARAM}
        if any link still references the tail node — pop those links
        first via L{Links.pop_last}.

        @param node_id: Expected tail node identifier.
        @type node_id: str
        @return: Error code (C{0} on success).
        @rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = node_id.encode('utf-8')
        return swmm_node_pop_last(h, b)

    # ====================================================================
    # Geometry getters (single element)
    # ====================================================================

    def get_type(self, idx) -> int:
        """Return the node type code.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Node type code.
        @rtype: int
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_node_get_type(h, i, &v))
        return v

    def get_invert_elev(self, idx) -> float:
        """Return the invert elevation of a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Invert elevation (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_invert_elev(h, i, &v))
        return v

    def get_max_depth(self, idx) -> float:
        """Return the maximum depth of a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Maximum depth (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_max_depth(h, i, &v))
        return v

    def get_surcharge_depth(self, idx) -> float:
        """Return the surcharge depth of a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Surcharge depth (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_surcharge_depth(h, i, &v))
        return v

    def get_ponded_area(self, idx) -> float:
        """Return the ponded area of a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Ponded area (project area units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_ponded_area(h, i, &v))
        return v

    def get_initial_depth(self, idx) -> float:
        """Return the initial depth of a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Initial depth (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_initial_depth(h, i, &v))
        return v

    def get_crown_elev(self, idx) -> float:
        """Return the crown elevation of a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Crown elevation (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_crown_elev(h, i, &v))
        return v

    def get_full_volume(self, idx) -> float:
        """Return the full volume of a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Full volume (project volume units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_full_volume(h, i, &v))
        return v

    def get_losses(self, idx) -> float:
        """Return the losses at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Losses (project flow units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_losses(h, i, &v))
        return v

    def get_outflow(self, idx) -> float:
        """Return the outflow from a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Outflow (project flow units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_outflow(h, i, &v))
        return v

    def get_degree(self, idx) -> int:
        """Return the number of links connected to a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Node degree (number of connected links).
        @rtype: int
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_node_get_degree(h, i, &v))
        return v

    # ====================================================================
    # Per-element hydraulic state (depth/head/volume/inflow)
    # ====================================================================

    def get_depth(self, idx) -> float:
        """Return the current water depth at a node.

        @param idx: Node index (int) or node ID (str). Strings are
            resolved via L{get_index}.
        @type idx: Union[int, str]
        @return: Water depth in project length units (feet for US, metres for SI).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_depth(h, i, &v))
        return v

    def get_head(self, idx) -> float:
        """Return the hydraulic head at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Hydraulic head (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_head(h, i, &v))
        return v

    def get_volume(self, idx) -> float:
        """Return the volume stored at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Volume (project volume units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_volume(h, i, &v))
        return v

    def get_lateral_inflow(self, idx) -> float:
        """Return the lateral inflow at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Lateral inflow (project flow units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_lateral_inflow(h, i, &v))
        return v

    def get_overflow(self, idx) -> float:
        """Return the overflow rate at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Overflow rate (project flow units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_overflow(h, i, &v))
        return v

    def get_inflow(self, idx) -> float:
        """Return the total inflow at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Total inflow (project flow units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_inflow(h, i, &v))
        return v

    # ====================================================================
    # Per-element setters (geometry & runtime forcing)
    # ====================================================================

    def set_depth(self, idx, double value):
        """Set the water depth at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param value: New depth (project length units).
        @type value: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_depth(h, i, value))

    def set_lateral_inflow(self, idx, double flow):
        """Prescribe a lateral inflow at a node (RUNNING state only).

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param flow: Lateral inflow rate (project flow units).
        @type flow: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_lateral_inflow(h, i, flow))

    def set_head_boundary(self, idx, double head):
        """Set a fixed head boundary condition at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param head: Boundary head value (project length units).
        @type head: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_head_boundary(h, i, head))

    def set_invert_elev(self, idx, double elev):
        """Set the invert elevation of a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param elev: Invert elevation (project length units).
        @type elev: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_invert_elev(h, i, elev))

    def set_max_depth(self, idx, double depth):
        """Set the maximum depth of a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param depth: Maximum depth (project length units).
        @type depth: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_max_depth(h, i, depth))

    def set_surcharge_depth(self, idx, double depth):
        """Set the surcharge depth of a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param depth: Surcharge depth (project length units).
        @type depth: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_surcharge_depth(h, i, depth))

    def set_pond_area(self, idx, double area):
        """Set the ponded area of a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param area: Ponded area (project area units).
        @type area: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_pond_area(h, i, area))

    def set_initial_depth(self, idx, double depth):
        """Set the initial depth of a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param depth: Initial depth (project length units).
        @type depth: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_initial_depth(h, i, depth))

    # ====================================================================
    # Pollutant/quality
    # ====================================================================

    def get_quality(self, idx, int pollutant_idx) -> float:
        """Return the concentration of a pollutant at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param pollutant_idx: Pollutant index.
        @type pollutant_idx: int
        @return: Pollutant concentration.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_quality(h, i, pollutant_idx, &v))
        return v

    def set_quality_mass_flux(self, idx, int pollutant_idx, double mass_rate):
        """Inject a pollutant mass flux at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param pollutant_idx: Pollutant index.
        @type pollutant_idx: int
        @param mass_rate: Mass injection rate (mass/time in model units).
        @type mass_rate: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_quality_mass_flux(h, i, pollutant_idx, mass_rate))

    # ====================================================================
    # Storage node methods
    # ====================================================================

    def set_storage_curve(self, idx, int curve_idx):
        """Assign a storage curve to a storage node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param curve_idx: Curve index.
        @type curve_idx: int
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_storage_curve(h, i, curve_idx))

    def get_storage_curve(self, idx) -> int:
        """Return the storage curve index for a storage node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Curve index.
        @rtype: int
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_node_get_storage_curve(h, i, &v))
        return v

    def set_storage_functional(self, idx, double a, double b, double c):
        """Set the functional storage parameters (A, B, C) for a storage node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param a: Coefficient A.
        @type a: float
        @param b: Coefficient B.
        @type b: float
        @param c: Coefficient C.
        @type c: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_storage_functional(h, i, a, b, c))

    def get_storage_functional(self, idx) -> tuple:
        """Return the functional storage parameters (A, B, C) for a storage node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Tuple of C{(a, b, c)} coefficients.
        @rtype: tuple
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double a = 0.0
        cdef double b = 0.0
        cdef double c = 0.0
        _check(swmm_node_get_storage_functional(h, i, &a, &b, &c))
        return (a, b, c)

    def set_storage_seep_rate(self, idx, double rate):
        """Set the seepage rate for a storage node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param rate: Seepage rate.
        @type rate: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_storage_seep_rate(h, i, rate))

    def get_storage_seep_rate(self, idx) -> float:
        """Return the seepage rate for a storage node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Seepage rate.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_storage_seep_rate(h, i, &v))
        return v

    def set_exfil_params(self, idx, double suction, double ksat, double imd):
        """Set exfiltration parameters for a storage node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param suction: Suction head.
        @type suction: float
        @param ksat: Saturated hydraulic conductivity.
        @type ksat: float
        @param imd: Initial moisture deficit.
        @type imd: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_exfil_params(h, i, suction, ksat, imd))

    def get_exfil_params(self, idx) -> tuple:
        """Return exfiltration parameters for a storage node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Tuple of C{(suction, ksat, imd)}.
        @rtype: tuple
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double suction = 0.0
        cdef double ksat = 0.0
        cdef double imd = 0.0
        _check(swmm_node_get_exfil_params(h, i, &suction, &ksat, &imd))
        return (suction, ksat, imd)

    # ====================================================================
    # Outfall node methods
    # ====================================================================

    def set_outfall_type(self, idx, int type):
        """Set the outfall type for an outfall node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param type: Outfall type code.
        @type type: int
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_outfall_type(h, i, type))

    def get_outfall_type(self, idx) -> int:
        """Return the outfall type code for an outfall node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Outfall type code.
        @rtype: int
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_node_get_outfall_type(h, i, &v))
        return v

    def set_outfall_stage(self, idx, double stage):
        """Set a fixed stage for an outfall node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param stage: Fixed stage value (project length units).
        @type stage: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_outfall_stage(h, i, stage))

    def set_outfall_tidal(self, idx, int curve_idx):
        """Assign a tidal curve to an outfall node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param curve_idx: Tidal curve index.
        @type curve_idx: int
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_outfall_tidal(h, i, curve_idx))

    def set_outfall_timeseries(self, idx, int ts_idx):
        """Assign a time series to an outfall node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param ts_idx: Time series index.
        @type ts_idx: int
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_outfall_timeseries(h, i, ts_idx))

    def get_outfall_param(self, idx) -> float:
        """Return the outfall parameter value for an outfall node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Outfall parameter value.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_outfall_param(h, i, &v))
        return v

    def set_outfall_flap_gate(self, idx, bint has_gate):
        """Set whether an outfall node has a flap gate.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param has_gate: C{True} if the outfall has a flap gate.
        @type has_gate: bool
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_outfall_flap_gate(h, i, has_gate))

    def get_outfall_flap_gate(self, idx) -> bool:
        """Return whether an outfall node has a flap gate.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: C{True} if the outfall has a flap gate.
        @rtype: bool
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_node_get_outfall_flap_gate(h, i, &v))
        return bool(v)

    def set_outfall_route_to(self, idx, int subcatch_idx):
        """Set the subcatchment to route outfall discharge to.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param subcatch_idx: Subcatchment index (C{-1} = no routing).
        @type subcatch_idx: int
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_outfall_route_to(h, i, subcatch_idx))

    def get_outfall_route_to(self, idx) -> int:
        """Get the subcatchment index outfall discharge is routed to.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Subcatchment index (C{-1} = no routing).
        @rtype: int
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = -1
        _check(swmm_node_get_outfall_route_to(h, i, &v))
        return v

    # ====================================================================
    # Statistics
    # ====================================================================

    def get_stat_max_depth(self, idx) -> float:
        """Return the maximum depth recorded at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Maximum depth (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_stat_max_depth(h, i, &v))
        return v

    def get_stat_max_overflow(self, idx) -> float:
        """Return the maximum overflow rate recorded at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Maximum overflow rate (project flow units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_stat_max_overflow(h, i, &v))
        return v

    def get_stat_vol_flooded(self, idx) -> float:
        """Return the total volume flooded at a node.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Volume flooded (project volume units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_stat_vol_flooded(h, i, &v))
        return v

    def get_stat_time_flooded(self, idx) -> float:
        """Return the total time a node was flooded.

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @return: Time flooded (hours).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_stat_time_flooded(h, i, &v))
        return v

    # ====================================================================
    # Depth from volume (inverse)
    # ====================================================================

    def get_depth_from_volume(self, idx, double volume) -> float:
        """Compute depth from volume (inverse of volume-depth relationship).

        @param idx: Node index (int) or node ID (str).
        @type idx: Union[int, str]
        @param volume: Volume (project volume units).
        @type volume: float
        @return: Depth (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the node ID is not found.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_depth_from_volume(h, i, volume, &v))
        return v

    # ====================================================================
    # Bulk array access (numpy)
    # ====================================================================

    def get_depths_bulk(self):
        """Return all node depths as a NumPy array.

        Uses the bulk C API for a single C{memcpy} -- much faster than
        calling L{get_depth} in a loop.

        @return: Array of shape C{(n_nodes,)} with dtype C{float64}.
        @rtype: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        _check(swmm_node_get_depths_bulk(h, &buf[0], n))
        return buf

    def get_heads_bulk(self):
        """Return all node heads as a NumPy array.

        @return: Array of shape C{(n_nodes,)} with dtype C{float64}.
        @rtype: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        _check(swmm_node_get_heads_bulk(h, &buf[0], n))
        return buf

    def set_depths_bulk(self, np.ndarray[double, ndim=1] values):
        """Set all node depths from a NumPy array.

        @param values: Array of shape C{(n_nodes,)} with dtype C{float64}.
        @type values: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        _check(swmm_node_set_depths_bulk(h, &values[0], n))

    def get_inflows_bulk(self):
        """Return all node total inflows as a NumPy array.

        @return: Array of shape C{(n_nodes,)} with dtype C{float64}.
        @rtype: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        _check(swmm_node_get_inflows_bulk(h, &buf[0], n))
        return buf

    def get_overflows_bulk(self):
        """Return all node overflow rates as a NumPy array.

        @return: Array of shape C{(n_nodes,)} with dtype C{float64}.
        @rtype: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        _check(swmm_node_get_overflows_bulk(h, &buf[0], n))
        return buf

    def set_lat_inflows_bulk(self, np.ndarray[double, ndim=1] values):
        """Set all node lateral inflows from a NumPy array.

        @param values: Array of shape C{(n_nodes,)} with dtype C{float64}.
        @type values: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        _check(swmm_node_set_lat_inflows_bulk(h, &values[0], n))

    def get_quality_bulk(self, int pollutant_idx):
        """Return all node concentrations for a pollutant as a NumPy array.

        @param pollutant_idx: Pollutant index.
        @type pollutant_idx: int
        @return: Array of shape C{(n_nodes,)} with dtype C{float64}.
        @rtype: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        _check(swmm_node_get_quality_bulk(h, pollutant_idx, &buf[0], n))
        return buf
