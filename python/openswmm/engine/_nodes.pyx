"""
Node Access
===========

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

The :class:`Nodes` class provides access to node properties during a
simulation. Create an instance by passing an active :class:`Solver`:

.. code-block:: python

    from openswmm.engine import Solver, Nodes

    with Solver("model.inp", "model.rpt", "model.out") as s:
        nodes = Nodes(s)
        print(f"Node count: {nodes.count()}")
        while s.step():
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

    :param solver: An active :class:`~openswmm.engine.Solver` instance.
        The solver must remain alive for the lifetime of this object.

    All per-element methods accept either an integer index or a string node
    ID.  When a string is passed it is resolved via :meth:`get_index`.

    .. code-block:: python

        nodes = Nodes(solver)
        depth = nodes.get_depth(0)      # by index
        depth = nodes.get_depth("J1")   # by name
    """

    def __init__(self, solver):
        self._solver = solver

    def _resolve(self, idx) -> int:
        """Resolve *idx* to an integer index.

        :param idx: Integer index or string node ID.
        :returns: Integer index.
        :raises KeyError: If a string ID is not found.
        """
        cdef int i
        if isinstance(idx, str):
            i = self.get_index(idx)
            if i < 0:
                raise KeyError(f"Node '{idx}' not found")
            return i
        return idx

    def count(self) -> int:
        """Return the number of nodes in the model.

        :returns: Node count.
        :rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_node_count(h)

    def get_index(self, str node_id) -> int:
        """Return the integer index of a node by its string ID.

        :param node_id: Node identifier string.
        :returns: Node index, or -1 if not found.
        :rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = node_id.encode('utf-8')
        return swmm_node_index(h, b)

    def get_id(self, int idx) -> str:
        """Return the string ID of a node by index.

        :param idx: Node index.
        :returns: Node ID string.
        :rtype: str
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef const char* raw = swmm_node_id(h, idx)
        return raw.decode('utf-8') if raw != NULL else ""

    def get_depth(self, idx) -> float:
        """Return the current water depth at a node.

        :param idx: Node index (int) or node ID (str).
        :returns: Water depth (project length units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_depth(h, i, &v))
        return v

    def set_depth(self, idx, double value):
        """Set the water depth at a node.

        :param idx: Node index (int) or node ID (str).
        :param value: New depth (project length units).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_depth(h, i, value))

    def get_head(self, idx) -> float:
        """Return the hydraulic head at a node.

        :param idx: Node index (int) or node ID (str).
        :returns: Hydraulic head (project length units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_head(h, i, &v))
        return v

    def get_volume(self, idx) -> float:
        """Return the volume stored at a node.

        :param idx: Node index (int) or node ID (str).
        :returns: Volume (project volume units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_volume(h, i, &v))
        return v

    def set_lateral_inflow(self, idx, double flow):
        """Prescribe a lateral inflow at a node (RUNNING state only).

        :param idx: Node index (int) or node ID (str).
        :param flow: Lateral inflow rate (project flow units).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_lateral_inflow(h, i, flow))

    def get_depths_bulk(self):
        """Return all node depths as a NumPy array.

        Uses the bulk C API for a single ``memcpy`` -- much faster than
        calling :meth:`get_depth` in a loop.

        :returns: Array of shape ``(n_nodes,)`` with dtype ``float64``.
        :rtype: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        _check(swmm_node_get_depths_bulk(h, &buf[0], n))
        return buf

    def get_heads_bulk(self):
        """Return all node heads as a NumPy array.

        :returns: Array of shape ``(n_nodes,)`` with dtype ``float64``.
        :rtype: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        _check(swmm_node_get_heads_bulk(h, &buf[0], n))
        return buf

    def set_depths_bulk(self, np.ndarray[double, ndim=1] values):
        """Set all node depths from a NumPy array.

        :param values: Array of shape ``(n_nodes,)`` with dtype ``float64``.
        :type values: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        _check(swmm_node_set_depths_bulk(h, &values[0], n))

    # ------------------------------------------------------------------
    # Geometry getters (single element)
    # ------------------------------------------------------------------

    def get_type(self, idx) -> int:
        """Return the node type code.

        :param idx: Node index (int) or node ID (str).
        :returns: Node type code.
        :rtype: int
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_node_get_type(h, i, &v))
        return v

    def get_invert_elev(self, idx) -> float:
        """Return the invert elevation of a node.

        :param idx: Node index (int) or node ID (str).
        :returns: Invert elevation (project length units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_invert_elev(h, i, &v))
        return v

    def get_max_depth(self, idx) -> float:
        """Return the maximum depth of a node.

        :param idx: Node index (int) or node ID (str).
        :returns: Maximum depth (project length units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_max_depth(h, i, &v))
        return v

    def get_surcharge_depth(self, idx) -> float:
        """Return the surcharge depth of a node.

        :param idx: Node index (int) or node ID (str).
        :returns: Surcharge depth (project length units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_surcharge_depth(h, i, &v))
        return v

    def get_ponded_area(self, idx) -> float:
        """Return the ponded area of a node.

        :param idx: Node index (int) or node ID (str).
        :returns: Ponded area (project area units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_ponded_area(h, i, &v))
        return v

    def get_initial_depth(self, idx) -> float:
        """Return the initial depth of a node.

        :param idx: Node index (int) or node ID (str).
        :returns: Initial depth (project length units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_initial_depth(h, i, &v))
        return v

    def get_crown_elev(self, idx) -> float:
        """Return the crown elevation of a node.

        :param idx: Node index (int) or node ID (str).
        :returns: Crown elevation (project length units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_crown_elev(h, i, &v))
        return v

    def get_full_volume(self, idx) -> float:
        """Return the full volume of a node.

        :param idx: Node index (int) or node ID (str).
        :returns: Full volume (project volume units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_full_volume(h, i, &v))
        return v

    def get_losses(self, idx) -> float:
        """Return the losses at a node.

        :param idx: Node index (int) or node ID (str).
        :returns: Losses (project flow units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_losses(h, i, &v))
        return v

    def get_outflow(self, idx) -> float:
        """Return the outflow from a node.

        :param idx: Node index (int) or node ID (str).
        :returns: Outflow (project flow units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_outflow(h, i, &v))
        return v

    def get_degree(self, idx) -> int:
        """Return the number of links connected to a node.

        :param idx: Node index (int) or node ID (str).
        :returns: Node degree (number of connected links).
        :rtype: int
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_node_get_degree(h, i, &v))
        return v

    # ------------------------------------------------------------------
    # Hydraulic state getters
    # ------------------------------------------------------------------

    def get_lateral_inflow(self, idx) -> float:
        """Return the lateral inflow at a node.

        :param idx: Node index (int) or node ID (str).
        :returns: Lateral inflow (project flow units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_lateral_inflow(h, i, &v))
        return v

    def get_overflow(self, idx) -> float:
        """Return the overflow rate at a node.

        :param idx: Node index (int) or node ID (str).
        :returns: Overflow rate (project flow units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_overflow(h, i, &v))
        return v

    def get_inflow(self, idx) -> float:
        """Return the total inflow at a node.

        :param idx: Node index (int) or node ID (str).
        :returns: Total inflow (project flow units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_inflow(h, i, &v))
        return v

    # ------------------------------------------------------------------
    # Runtime forcing
    # ------------------------------------------------------------------

    def set_head_boundary(self, idx, double head):
        """Set a fixed head boundary condition at a node.

        :param idx: Node index (int) or node ID (str).
        :param head: Boundary head value (project length units).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_head_boundary(h, i, head))

    # ------------------------------------------------------------------
    # Geometry setters
    # ------------------------------------------------------------------

    def set_invert_elev(self, idx, double elev):
        """Set the invert elevation of a node.

        :param idx: Node index (int) or node ID (str).
        :param elev: Invert elevation (project length units).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_invert_elev(h, i, elev))

    def set_max_depth(self, idx, double depth):
        """Set the maximum depth of a node.

        :param idx: Node index (int) or node ID (str).
        :param depth: Maximum depth (project length units).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_max_depth(h, i, depth))

    def set_surcharge_depth(self, idx, double depth):
        """Set the surcharge depth of a node.

        :param idx: Node index (int) or node ID (str).
        :param depth: Surcharge depth (project length units).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_surcharge_depth(h, i, depth))

    def set_pond_area(self, idx, double area):
        """Set the ponded area of a node.

        :param idx: Node index (int) or node ID (str).
        :param area: Ponded area (project area units).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_pond_area(h, i, area))

    def set_initial_depth(self, idx, double depth):
        """Set the initial depth of a node.

        :param idx: Node index (int) or node ID (str).
        :param depth: Initial depth (project length units).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_initial_depth(h, i, depth))

    # ------------------------------------------------------------------
    # Quality
    # ------------------------------------------------------------------

    def get_quality(self, idx, int pollutant_idx) -> float:
        """Return the concentration of a pollutant at a node.

        :param idx: Node index (int) or node ID (str).
        :param pollutant_idx: Pollutant index.
        :returns: Pollutant concentration.
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_quality(h, i, pollutant_idx, &v))
        return v

    # ------------------------------------------------------------------
    # Storage node methods
    # ------------------------------------------------------------------

    def set_storage_curve(self, idx, int curve_idx):
        """Assign a storage curve to a storage node.

        :param idx: Node index (int) or node ID (str).
        :param curve_idx: Curve index.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_storage_curve(h, i, curve_idx))

    def get_storage_curve(self, idx) -> int:
        """Return the storage curve index for a storage node.

        :param idx: Node index (int) or node ID (str).
        :returns: Curve index.
        :rtype: int
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_node_get_storage_curve(h, i, &v))
        return v

    def set_storage_functional(self, idx, double a, double b, double c):
        """Set the functional storage parameters (A, B, C) for a storage node.

        :param idx: Node index (int) or node ID (str).
        :param a: Coefficient A.
        :param b: Coefficient B.
        :param c: Coefficient C.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_storage_functional(h, i, a, b, c))

    def get_storage_functional(self, idx) -> tuple:
        """Return the functional storage parameters (A, B, C) for a storage node.

        :param idx: Node index (int) or node ID (str).
        :returns: Tuple of (a, b, c) coefficients.
        :rtype: tuple
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

        :param idx: Node index (int) or node ID (str).
        :param rate: Seepage rate.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_storage_seep_rate(h, i, rate))

    def get_storage_seep_rate(self, idx) -> float:
        """Return the seepage rate for a storage node.

        :param idx: Node index (int) or node ID (str).
        :returns: Seepage rate.
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_storage_seep_rate(h, i, &v))
        return v

    def set_exfil_params(self, idx, double suction, double ksat, double imd):
        """Set exfiltration parameters for a storage node.

        :param idx: Node index (int) or node ID (str).
        :param suction: Suction head.
        :param ksat: Saturated hydraulic conductivity.
        :param imd: Initial moisture deficit.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_exfil_params(h, i, suction, ksat, imd))

    def get_exfil_params(self, idx) -> tuple:
        """Return exfiltration parameters for a storage node.

        :param idx: Node index (int) or node ID (str).
        :returns: Tuple of (suction, ksat, imd).
        :rtype: tuple
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double suction = 0.0
        cdef double ksat = 0.0
        cdef double imd = 0.0
        _check(swmm_node_get_exfil_params(h, i, &suction, &ksat, &imd))
        return (suction, ksat, imd)

    # ------------------------------------------------------------------
    # Outfall node methods
    # ------------------------------------------------------------------

    def set_outfall_type(self, idx, int type):
        """Set the outfall type for an outfall node.

        :param idx: Node index (int) or node ID (str).
        :param type: Outfall type code.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_outfall_type(h, i, type))

    def get_outfall_type(self, idx) -> int:
        """Return the outfall type code for an outfall node.

        :param idx: Node index (int) or node ID (str).
        :returns: Outfall type code.
        :rtype: int
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_node_get_outfall_type(h, i, &v))
        return v

    def set_outfall_stage(self, idx, double stage):
        """Set a fixed stage for an outfall node.

        :param idx: Node index (int) or node ID (str).
        :param stage: Fixed stage value (project length units).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_outfall_stage(h, i, stage))

    def set_outfall_tidal(self, idx, int curve_idx):
        """Assign a tidal curve to an outfall node.

        :param idx: Node index (int) or node ID (str).
        :param curve_idx: Tidal curve index.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_outfall_tidal(h, i, curve_idx))

    def set_outfall_timeseries(self, idx, int ts_idx):
        """Assign a time series to an outfall node.

        :param idx: Node index (int) or node ID (str).
        :param ts_idx: Time series index.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_outfall_timeseries(h, i, ts_idx))

    def get_outfall_param(self, idx) -> float:
        """Return the outfall parameter value for an outfall node.

        :param idx: Node index (int) or node ID (str).
        :returns: Outfall parameter value.
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_outfall_param(h, i, &v))
        return v

    def set_outfall_flap_gate(self, idx, bint has_gate):
        """Set whether an outfall node has a flap gate.

        :param idx: Node index (int) or node ID (str).
        :param has_gate: True if the outfall has a flap gate.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_outfall_flap_gate(h, i, has_gate))

    def get_outfall_flap_gate(self, idx) -> bool:
        """Return whether an outfall node has a flap gate.

        :param idx: Node index (int) or node ID (str).
        :returns: True if the outfall has a flap gate.
        :rtype: bool
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_node_get_outfall_flap_gate(h, i, &v))
        return bool(v)

    # ------------------------------------------------------------------
    # Statistics
    # ------------------------------------------------------------------

    def get_stat_max_depth(self, idx) -> float:
        """Return the maximum depth recorded at a node.

        :param idx: Node index (int) or node ID (str).
        :returns: Maximum depth (project length units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_stat_max_depth(h, i, &v))
        return v

    def get_stat_max_overflow(self, idx) -> float:
        """Return the maximum overflow rate recorded at a node.

        :param idx: Node index (int) or node ID (str).
        :returns: Maximum overflow rate (project flow units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_stat_max_overflow(h, i, &v))
        return v

    def get_stat_vol_flooded(self, idx) -> float:
        """Return the total volume flooded at a node.

        :param idx: Node index (int) or node ID (str).
        :returns: Volume flooded (project volume units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_stat_vol_flooded(h, i, &v))
        return v

    def get_stat_time_flooded(self, idx) -> float:
        """Return the total time a node was flooded.

        :param idx: Node index (int) or node ID (str).
        :returns: Time flooded (hours).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_stat_time_flooded(h, i, &v))
        return v

    # ------------------------------------------------------------------
    # Additional bulk methods
    # ------------------------------------------------------------------

    def get_inflows_bulk(self):
        """Return all node total inflows as a NumPy array.

        :returns: Array of shape ``(n_nodes,)`` with dtype ``float64``.
        :rtype: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        _check(swmm_node_get_inflows_bulk(h, &buf[0], n))
        return buf

    def get_overflows_bulk(self):
        """Return all node overflow rates as a NumPy array.

        :returns: Array of shape ``(n_nodes,)`` with dtype ``float64``.
        :rtype: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        _check(swmm_node_get_overflows_bulk(h, &buf[0], n))
        return buf

    def set_lat_inflows_bulk(self, np.ndarray[double, ndim=1] values):
        """Set all node lateral inflows from a NumPy array.

        :param values: Array of shape ``(n_nodes,)`` with dtype ``float64``.
        :type values: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        _check(swmm_node_set_lat_inflows_bulk(h, &values[0], n))

    def get_quality_bulk(self, int pollutant_idx):
        """Return all node concentrations for a pollutant as a NumPy array.

        :param pollutant_idx: Pollutant index.
        :returns: Array of shape ``(n_nodes,)`` with dtype ``float64``.
        :rtype: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_node_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        _check(swmm_node_get_quality_bulk(h, pollutant_idx, &buf[0], n))
        return buf

    # ------------------------------------------------------------------
    # Outfall route-to
    # ------------------------------------------------------------------

    def set_outfall_route_to(self, idx, int subcatch_idx):
        """Set the subcatchment to route outfall discharge to.

        :param idx: Node index (int) or node ID (str).
        :param subcatch_idx: Subcatchment index (-1 = no routing).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_node_set_outfall_route_to(h, i, subcatch_idx))

    def get_outfall_route_to(self, idx) -> int:
        """Get the subcatchment index outfall discharge is routed to.

        :param idx: Node index (int) or node ID (str).
        :returns: Subcatchment index (-1 = no routing).
        :rtype: int
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = -1
        _check(swmm_node_get_outfall_route_to(h, i, &v))
        return v

    # ------------------------------------------------------------------
    # Depth from volume (inverse)
    # ------------------------------------------------------------------

    def get_depth_from_volume(self, idx, double volume) -> float:
        """Compute depth from volume (inverse of volume-depth relationship).

        :param idx: Node index (int) or node ID (str).
        :param volume: Volume (project volume units).
        :returns: Depth (project length units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_node_get_depth_from_volume(h, i, volume, &v))
        return v
