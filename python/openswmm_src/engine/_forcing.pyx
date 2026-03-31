"""
Runtime Forcing
===============

:author: Caleb Buahin
:copyright: Copyright (c) HydroCouple 2026
:license: MIT

The :class:`Forcing` class provides advanced runtime forcing of node, link,
subcatchment, and gage values during a simulation.  Unlike the simple
``set_*`` methods on the domain classes, forcing supports **mode** (replace
or add) and **persistence** (single-step vs. held until cleared).

The forcing API is accessed via :attr:`Solver.forcing`:

.. code-block:: python

    from openswmm.engine import Solver, ForcingMode

    with Solver("model.inp", "model.rpt", "model.out") as s:
        # Apply a persistent lateral inflow of 1.5 at node "J1"
        j1 = s.nodes.get_index("J1")
        s.forcing.node_lat_inflow(j1, 1.5, ForcingMode.REPLACE, persist=True)

        while s.step():
            pass  # inflow stays applied every step

        # Clear all forcing before ending
        s.forcing.clear_all()

Forcing modes
-------------

=================  ===========================================================
Mode               Behaviour
=================  ===========================================================
``REPLACE`` (0)    The forced value **replaces** the model-computed value.
``ADD`` (1)        The forced value is **added** to the model-computed value.
=================  ===========================================================

Persistence
-----------

When ``persist=False`` (default), the forcing is applied for a single
timestep and automatically removed.  When ``persist=True`` the value is
held every step until explicitly removed with :meth:`clear` or
:meth:`clear_all`.
"""

# cython: language_level=3

from ._common cimport *


class Forcing:
    """Advanced runtime forcing for nodes, links, subcatchments, and gages.

    :param solver: An active :class:`~openswmm.engine.Solver` instance.
        The solver must remain alive for the lifetime of this object.

    All per-element methods accept an integer index.  Use the corresponding
    domain accessor (e.g. ``solver.nodes.get_index("J1")``) to resolve
    string IDs to indices.
    """

    def __init__(self, solver):
        self._solver = solver

    # ------------------------------------------------------------------
    # Node forcing
    # ------------------------------------------------------------------

    def node_lat_inflow(self, int idx, double value, int mode=0,
                        bint persist=False):
        """Force a lateral inflow at a node.

        :param idx: Node index.
        :param value: Lateral inflow rate (project flow units).
        :param mode: :class:`~openswmm.engine.ForcingMode` — ``REPLACE`` (0)
            or ``ADD`` (1).
        :param persist: If ``True``, hold the value every step until cleared.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_node_lat_inflow(h, idx, value, mode,
                                             1 if persist else 0))

    def node_head_boundary(self, int idx, double value, int mode=0,
                           bint persist=False):
        """Force a head boundary condition at a node.

        :param idx: Node index.
        :param value: Boundary head (project length units).
        :param mode: :class:`~openswmm.engine.ForcingMode`.
        :param persist: Hold until cleared.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_node_head_boundary(h, idx, value, mode,
                                                1 if persist else 0))

    def node_quality(self, int node_idx, int pollutant_idx,
                     double mass_rate, int mode=0, bint persist=False):
        """Force a pollutant mass-rate injection at a node.

        :param node_idx: Node index.
        :param pollutant_idx: Pollutant index.
        :param mass_rate: Mass rate (mass / time).
        :param mode: :class:`~openswmm.engine.ForcingMode`.
        :param persist: Hold until cleared.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_node_quality(h, node_idx, pollutant_idx,
                                          mass_rate, mode,
                                          1 if persist else 0))

    # ------------------------------------------------------------------
    # Link forcing
    # ------------------------------------------------------------------

    def link_flow(self, int idx, double value, int mode=0,
                  bint persist=False):
        """Force a flow rate in a link.

        :param idx: Link index.
        :param value: Flow rate (project flow units).
        :param mode: :class:`~openswmm.engine.ForcingMode`.
        :param persist: Hold until cleared.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_link_flow(h, idx, value, mode,
                                       1 if persist else 0))

    def link_setting(self, int idx, double value, int mode=0,
                     bint persist=False):
        """Force a control setting on a link (pump speed, gate opening, etc.).

        :param idx: Link index.
        :param value: Setting value (0.0--1.0 for gates; speed multiplier
            for pumps).
        :param mode: :class:`~openswmm.engine.ForcingMode`.
        :param persist: Hold until cleared.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_link_setting(h, idx, value, mode,
                                          1 if persist else 0))

    # ------------------------------------------------------------------
    # Subcatchment forcing
    # ------------------------------------------------------------------

    def subcatch_rainfall(self, int idx, double value, int mode=0,
                          bint persist=False):
        """Force a rainfall rate on a subcatchment.

        :param idx: Subcatchment index.
        :param value: Rainfall rate (project rainfall units).
        :param mode: :class:`~openswmm.engine.ForcingMode`.
        :param persist: Hold until cleared.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_subcatch_rainfall(h, idx, value, mode,
                                               1 if persist else 0))

    def subcatch_evap(self, int idx, double value, int mode=0,
                      bint persist=False):
        """Force an evaporation rate on a subcatchment.

        :param idx: Subcatchment index.
        :param value: Evaporation rate (project length / time units).
        :param mode: :class:`~openswmm.engine.ForcingMode`.
        :param persist: Hold until cleared.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_subcatch_evap(h, idx, value, mode,
                                           1 if persist else 0))

    # ------------------------------------------------------------------
    # Gage forcing
    # ------------------------------------------------------------------

    def gage_rainfall(self, int idx, double value, int mode=0,
                      bint persist=False):
        """Force a rainfall rate at a rain gage.

        This affects all subcatchments assigned to the gage.

        :param idx: Gage index.
        :param value: Rainfall rate (project rainfall units).
        :param mode: :class:`~openswmm.engine.ForcingMode`.
        :param persist: Hold until cleared.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_gage_rainfall(h, idx, value, mode,
                                           1 if persist else 0))

    # ------------------------------------------------------------------
    # Clear
    # ------------------------------------------------------------------

    def clear(self, int target_type, int idx):
        """Remove all active forcing on a specific object.

        :param target_type: :class:`~openswmm.engine.ForcingTarget` code
            (``NODE=0``, ``LINK=1``, ``SUBCATCH=2``, ``GAGE=3``).
        :param idx: Object index within its type.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_clear(h, target_type, idx))

    def clear_all(self):
        """Remove **all** active forcing across all object types."""
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_clear_all(h))
