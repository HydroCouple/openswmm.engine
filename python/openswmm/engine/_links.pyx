"""
Link Access
===========

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

The :class:`Links` class provides access to link (conduit/pump/orifice/weir/outlet)
properties during a simulation.

.. code-block:: python

    from openswmm.engine import Solver, Links

    with Solver("model.inp", "model.rpt", "model.out") as s:
        links = Links(s)
        while s.step():
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

    :param solver: An active :class:`~openswmm.engine.Solver` instance.
        The solver must remain alive for the lifetime of this object.

    All per-element methods accept either an integer index or a string link
    ID.  When a string is passed it is resolved via :meth:`get_index`.

    .. code-block:: python

        links = Links(solver)
        flow = links.get_flow(0)      # by index
        flow = links.get_flow("C1")   # by name
    """

    def __init__(self, solver):
        self._solver = solver

    def _resolve(self, idx) -> int:
        """Resolve *idx* to an integer index.

        :param idx: Integer index or string link ID.
        :returns: Integer index.
        :raises KeyError: If a string ID is not found.
        """
        cdef int i
        if isinstance(idx, str):
            i = self.get_index(idx)
            if i < 0:
                raise KeyError(f"Link '{idx}' not found")
            return i
        return idx

    def count(self) -> int:
        """Return the number of links in the model.

        :returns: Link count.
        :rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        return swmm_link_count(h)

    def get_index(self, str link_id) -> int:
        """Return the integer index of a link by its string ID.

        :param link_id: Link identifier string.
        :returns: Link index, or -1 if not found.
        :rtype: int
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bytes b = link_id.encode('utf-8')
        return swmm_link_index(h, b)

    def get_id(self, int idx) -> str:
        """Return the string ID of a link by index.

        :param idx: Link index.
        :returns: Link ID string.
        :rtype: str
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef const char* raw = swmm_link_id(h, idx)
        return raw.decode('utf-8') if raw != NULL else ""

    def get_flow(self, idx) -> float:
        """Return the current flow rate in a link.

        :param idx: Link index (int) or link ID (str).
        :returns: Flow rate (project flow units). Positive = node1 -> node2.
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_flow(h, i, &v))
        return v

    def set_flow(self, idx, double value):
        """Set the flow rate in a link.

        :param idx: Link index (int) or link ID (str).
        :param value: New flow rate (project flow units).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_flow(h, i, value))

    def get_depth(self, idx) -> float:
        """Return the current water depth at the midpoint of a link.

        :param idx: Link index (int) or link ID (str).
        :returns: Water depth (project length units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_depth(h, i, &v))
        return v

    def set_control_setting(self, idx, double setting):
        """Override the control/pump setting on a link (RUNNING state).

        :param idx: Link index (int) or link ID (str).
        :param setting: New setting (0.0=closed to 1.0=fully open for
                        orifices/weirs; 0.0=off to 1.0=full speed for pumps).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_control_setting(h, i, setting))

    def get_control_setting(self, idx) -> float:
        """Return the current control setting of a link.

        :param idx: Link index (int) or link ID (str).
        :returns: Setting value (0.0--1.0).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_control_setting(h, i, &v))
        return v

    def get_flows_bulk(self):
        """Return all link flows as a NumPy array.

        :returns: Array of shape ``(n_links,)`` with dtype ``float64``.
        :rtype: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        _check(swmm_link_get_flows_bulk(h, &buf[0], n))
        return buf

    def set_flows_bulk(self, np.ndarray[double, ndim=1] values):
        """Set all link flows from a NumPy array.

        :param values: Array of shape ``(n_links,)`` with dtype ``float64``.
        :type values: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        _check(swmm_link_set_flows_bulk(h, &values[0], n))

    # ------------------------------------------------------------------
    # Connectivity
    # ------------------------------------------------------------------

    def get_from_node(self, idx) -> int:
        """Return the upstream (from) node index of a link.

        :param idx: Link index (int) or link ID (str).
        :returns: Node index of the upstream node.
        :rtype: int
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_link_get_from_node(h, i, &v))
        return v

    def get_to_node(self, idx) -> int:
        """Return the downstream (to) node index of a link.

        :param idx: Link index (int) or link ID (str).
        :returns: Node index of the downstream node.
        :rtype: int
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_link_get_to_node(h, i, &v))
        return v

    # ------------------------------------------------------------------
    # Geometry getters
    # ------------------------------------------------------------------

    def get_type(self, idx) -> int:
        """Return the type code of a link.

        :param idx: Link index (int) or link ID (str).
        :returns: Link type code (e.g. conduit, pump, orifice, weir, outlet).
        :rtype: int
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_link_get_type(h, i, &v))
        return v

    def get_length(self, idx) -> float:
        """Return the length of a link.

        :param idx: Link index (int) or link ID (str).
        :returns: Length (project length units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_length(h, i, &v))
        return v

    def get_roughness(self, idx) -> float:
        """Return the roughness coefficient of a link.

        :param idx: Link index (int) or link ID (str).
        :returns: Manning's roughness coefficient.
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_roughness(h, i, &v))
        return v

    def get_slope(self, idx) -> float:
        """Return the slope of a link.

        :param idx: Link index (int) or link ID (str).
        :returns: Slope (dimensionless).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_slope(h, i, &v))
        return v

    def get_offset_up(self, idx) -> float:
        """Return the upstream invert offset of a link.

        :param idx: Link index (int) or link ID (str).
        :returns: Upstream offset (project length units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_offset_up(h, i, &v))
        return v

    def get_offset_dn(self, idx) -> float:
        """Return the downstream invert offset of a link.

        :param idx: Link index (int) or link ID (str).
        :returns: Downstream offset (project length units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_offset_dn(h, i, &v))
        return v

    def get_xsect(self, idx) -> tuple:
        """Return the cross-section geometry of a link.

        :param idx: Link index (int) or link ID (str).
        :returns: Tuple of (shape, geom1, geom2, geom3, geom4).
        :rtype: tuple
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int shape = 0
        cdef double geom1 = 0.0, geom2 = 0.0, geom3 = 0.0, geom4 = 0.0
        _check(swmm_link_get_xsect(h, i, &shape, &geom1, &geom2, &geom3, &geom4))
        return (shape, geom1, geom2, geom3, geom4)

    # ------------------------------------------------------------------
    # Hydraulic state
    # ------------------------------------------------------------------

    def get_velocity(self, idx) -> float:
        """Return the current flow velocity in a link.

        :param idx: Link index (int) or link ID (str).
        :returns: Velocity (project length / time units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_velocity(h, i, &v))
        return v

    def get_capacity(self, idx) -> float:
        """Return the fraction of full capacity used in a link.

        :param idx: Link index (int) or link ID (str).
        :returns: Capacity fraction (0.0--1.0).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_capacity(h, i, &v))
        return v

    def get_volume(self, idx) -> float:
        """Return the current volume stored in a link.

        :param idx: Link index (int) or link ID (str).
        :returns: Volume (project volume units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_volume(h, i, &v))
        return v

    # ------------------------------------------------------------------
    # Runtime forcing
    # ------------------------------------------------------------------

    def set_target_setting(self, idx, double setting):
        """Set the target control setting for a link.

        :param idx: Link index (int) or link ID (str).
        :param setting: Target setting value.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_target_setting(h, i, setting))

    def get_target_setting(self, idx) -> float:
        """Return the target control setting of a link.

        :param idx: Link index (int) or link ID (str).
        :returns: Target setting value.
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_target_setting(h, i, &v))
        return v

    def set_closed(self, idx, bint closed):
        """Set whether a link is forced closed.

        :param idx: Link index (int) or link ID (str).
        :param closed: True to force the link closed.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_closed(h, i, closed))

    def get_closed(self, idx) -> bool:
        """Return whether a link is forced closed.

        :param idx: Link index (int) or link ID (str).
        :returns: True if the link is forced closed.
        :rtype: bool
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bint v = 0
        _check(swmm_link_get_closed(h, i, &v))
        return bool(v)

    # ------------------------------------------------------------------
    # Geometry setters
    # ------------------------------------------------------------------

    def set_offset_up(self, idx, double offset):
        """Set the upstream invert offset of a link.

        :param idx: Link index (int) or link ID (str).
        :param offset: Upstream offset (project length units).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_offset_up(h, i, offset))

    def set_offset_dn(self, idx, double offset):
        """Set the downstream invert offset of a link.

        :param idx: Link index (int) or link ID (str).
        :param offset: Downstream offset (project length units).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_offset_dn(h, i, offset))

    def set_initial_flow(self, idx, double flow):
        """Set the initial flow in a link.

        :param idx: Link index (int) or link ID (str).
        :param flow: Initial flow rate (project flow units).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_initial_flow(h, i, flow))

    def set_max_flow(self, idx, double flow):
        """Set the maximum flow capacity of a link.

        :param idx: Link index (int) or link ID (str).
        :param flow: Maximum flow rate (project flow units).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_max_flow(h, i, flow))

    # ------------------------------------------------------------------
    # Pump
    # ------------------------------------------------------------------

    def set_pump_curve(self, idx, int curve_idx):
        """Set the pump curve index for a pump link.

        :param idx: Link index (int) or link ID (str).
        :param curve_idx: Index of the pump curve.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_pump_curve(h, i, curve_idx))

    def get_pump_curve(self, idx) -> int:
        """Return the pump curve index for a pump link.

        :param idx: Link index (int) or link ID (str).
        :returns: Pump curve index.
        :rtype: int
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_link_get_pump_curve(h, i, &v))
        return v

    def set_pump_init_state(self, idx, bint on):
        """Set the initial on/off state of a pump link.

        :param idx: Link index (int) or link ID (str).
        :param on: True to set the pump initially on.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_pump_init_state(h, i, on))

    def get_pump_init_state(self, idx) -> bool:
        """Return the initial on/off state of a pump link.

        :param idx: Link index (int) or link ID (str).
        :returns: True if the pump is initially on.
        :rtype: bool
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bint v = 0
        _check(swmm_link_get_pump_init_state(h, i, &v))
        return bool(v)

    # ------------------------------------------------------------------
    # Weir
    # ------------------------------------------------------------------

    def set_crest_height(self, idx, double h):
        """Set the crest height of a weir link.

        :param idx: Link index (int) or link ID (str).
        :param h: Crest height (project length units).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine handle = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_crest_height(handle, i, h))

    def get_crest_height(self, idx) -> float:
        """Return the crest height of a weir link.

        :param idx: Link index (int) or link ID (str).
        :returns: Crest height (project length units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_crest_height(h, i, &v))
        return v

    def set_discharge_coeff(self, idx, double cd):
        """Set the discharge coefficient of a weir link.

        :param idx: Link index (int) or link ID (str).
        :param cd: Discharge coefficient.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_discharge_coeff(h, i, cd))

    def get_discharge_coeff(self, idx) -> float:
        """Return the discharge coefficient of a weir link.

        :param idx: Link index (int) or link ID (str).
        :returns: Discharge coefficient.
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_discharge_coeff(h, i, &v))
        return v

    def set_end_contractions(self, idx, double n):
        """Set the number of end contractions for a weir link.

        :param idx: Link index (int) or link ID (str).
        :param n: Number of end contractions.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_end_contractions(h, i, n))

    def get_end_contractions(self, idx) -> float:
        """Return the number of end contractions for a weir link.

        :param idx: Link index (int) or link ID (str).
        :returns: Number of end contractions.
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_end_contractions(h, i, &v))
        return v

    # ------------------------------------------------------------------
    # Loss / misc
    # ------------------------------------------------------------------

    def set_loss_coeff(self, idx, double inlet, double outlet, double avg):
        """Set the loss coefficients for a link.

        :param idx: Link index (int) or link ID (str).
        :param inlet: Inlet loss coefficient.
        :param outlet: Outlet loss coefficient.
        :param avg: Average loss coefficient.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_loss_coeff(h, i, inlet, outlet, avg))

    def get_loss_coeff(self, idx) -> tuple:
        """Return the loss coefficients for a link.

        :param idx: Link index (int) or link ID (str).
        :returns: Tuple of (inlet, outlet, avg) loss coefficients.
        :rtype: tuple
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double inlet = 0.0, outlet = 0.0, avg = 0.0
        _check(swmm_link_get_loss_coeff(h, i, &inlet, &outlet, &avg))
        return (inlet, outlet, avg)

    def set_flap_gate(self, idx, bint has_gate):
        """Set whether a link has a flap gate.

        :param idx: Link index (int) or link ID (str).
        :param has_gate: True if the link has a flap gate.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_flap_gate(h, i, has_gate))

    def get_flap_gate(self, idx) -> bool:
        """Return whether a link has a flap gate.

        :param idx: Link index (int) or link ID (str).
        :returns: True if the link has a flap gate.
        :rtype: bool
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef bint v = 0
        _check(swmm_link_get_flap_gate(h, i, &v))
        return bool(v)

    def set_seep_rate(self, idx, double rate):
        """Set the seepage rate for a link.

        :param idx: Link index (int) or link ID (str).
        :param rate: Seepage rate (project flow units per unit length).
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_seep_rate(h, i, rate))

    def get_seep_rate(self, idx) -> float:
        """Return the seepage rate for a link.

        :param idx: Link index (int) or link ID (str).
        :returns: Seepage rate.
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_seep_rate(h, i, &v))
        return v

    def set_culvert_code(self, idx, int code):
        """Set the culvert inlet geometry code for a link.

        :param idx: Link index (int) or link ID (str).
        :param code: Culvert code number.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_culvert_code(h, i, code))

    def get_culvert_code(self, idx) -> int:
        """Return the culvert inlet geometry code for a link.

        :param idx: Link index (int) or link ID (str).
        :returns: Culvert code number.
        :rtype: int
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_link_get_culvert_code(h, i, &v))
        return v

    def set_barrels(self, idx, int n):
        """Set the number of barrels for a link.

        :param idx: Link index (int) or link ID (str).
        :param n: Number of barrels.
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_link_set_barrels(h, i, n))

    def get_barrels(self, idx) -> int:
        """Return the number of barrels for a link.

        :param idx: Link index (int) or link ID (str).
        :returns: Number of barrels.
        :rtype: int
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int v = 0
        _check(swmm_link_get_barrels(h, i, &v))
        return v

    # ------------------------------------------------------------------
    # Quality
    # ------------------------------------------------------------------

    def get_quality(self, idx, int pollutant_idx) -> float:
        """Return the concentration of a pollutant in a link.

        :param idx: Link index (int) or link ID (str).
        :param pollutant_idx: Pollutant index.
        :returns: Pollutant concentration.
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_quality(h, i, pollutant_idx, &v))
        return v

    # ------------------------------------------------------------------
    # Statistics
    # ------------------------------------------------------------------

    def get_stat_max_flow(self, idx) -> float:
        """Return the maximum flow recorded for a link.

        :param idx: Link index (int) or link ID (str).
        :returns: Maximum flow (project flow units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_stat_max_flow(h, i, &v))
        return v

    def get_stat_max_velocity(self, idx) -> float:
        """Return the maximum velocity recorded for a link.

        :param idx: Link index (int) or link ID (str).
        :returns: Maximum velocity (project length / time units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_stat_max_velocity(h, i, &v))
        return v

    def get_stat_max_filling(self, idx) -> float:
        """Return the maximum filling fraction recorded for a link.

        :param idx: Link index (int) or link ID (str).
        :returns: Maximum filling fraction (0.0--1.0).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_stat_max_filling(h, i, &v))
        return v

    def get_stat_vol_flow(self, idx) -> float:
        """Return the total volume of flow through a link.

        :param idx: Link index (int) or link ID (str).
        :returns: Total volume of flow (project volume units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_stat_vol_flow(h, i, &v))
        return v

    def get_stat_surcharge_time(self, idx) -> float:
        """Return the total time a link was surcharged.

        :param idx: Link index (int) or link ID (str).
        :returns: Surcharge duration (simulation time units).
        :rtype: float
        """
        cdef int i = self._resolve(idx)
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef double v = 0.0
        _check(swmm_link_get_stat_surcharge_time(h, i, &v))
        return v

    # ------------------------------------------------------------------
    # Bulk
    # ------------------------------------------------------------------

    def get_depths_bulk(self):
        """Return all link depths as a NumPy array.

        :returns: Array of shape ``(n_links,)`` with dtype ``float64``.
        :rtype: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        _check(swmm_link_get_depths_bulk(h, &buf[0], n))
        return buf

    def get_quality_bulk(self, int pollutant_idx):
        """Return all link pollutant concentrations as a NumPy array.

        :param pollutant_idx: Pollutant index.
        :returns: Array of shape ``(n_links,)`` with dtype ``float64``.
        :rtype: numpy.ndarray
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        cdef int n = swmm_link_count(h)
        cdef np.ndarray[double, ndim=1] buf = np.empty(n, dtype=np.float64)
        _check(swmm_link_get_quality_bulk(h, pollutant_idx, &buf[0], n))
        return buf
