"""
Runtime Forcing
===============

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
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

        while s.state == EngineState.RUNNING:
            if s.step() != 0:
                break
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

    All per-element methods accept an integer index.  Use the corresponding
    domain accessor (e.g. C{solver.nodes.get_index("J1")}) to resolve
    string IDs to indices.

    @ivar _solver: The owning solver instance whose engine handle is used
        for every C call.
    @type _solver: L{Solver}

    @param solver: An active L{Solver} instance. The solver must remain
        alive for the lifetime of this object.
    @type solver: L{Solver}
    """

    def __init__(self, solver):
        """Construct a L{Forcing} accessor bound to C{solver}.

        @param solver: An active L{Solver} instance whose engine handle
            will be used for all subsequent forcing operations.
        @type solver: L{Solver}
        """
        self._solver = solver

    # ====================================================================
    # Mode/persistence configuration -- shared semantics
    # ====================================================================
    #
    # All forcing methods below take two common parameters:
    #
    #   - mode: integer code referencing L{ForcingMode}
    #         REPLACE (0)  -- the forced value REPLACES the model-computed
    #                         value at every step.
    #         ADD (1)      -- the forced value is ADDED to the model-computed
    #                         value at every step.
    #
    #   - persist: boolean flag controlling lifetime of the forcing
    #         False        -- applied for one timestep then automatically
    #                         removed.
    #         True         -- held every step until explicitly removed via
    #                         L{clear} or L{clear_all}.
    # ====================================================================

    # ====================================================================
    # Node forcing
    # ====================================================================

    def node_lat_inflow(self, int idx, double value, int mode=0,
                        bint persist=False):
        """Force a lateral inflow at a node.

        @param idx: Node index.
        @type idx: int
        @param value: Lateral inflow rate (project flow units).
        @type value: float
        @param mode: Forcing mode -- see L{ForcingMode}: C{REPLACE} (0) or
            C{ADD} (1).
        @type mode: int
        @param persist: If C{True}, hold the value every step until cleared;
            otherwise apply only for the next timestep.
        @type persist: bool
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying
            C{swmm_forcing_node_lat_inflow} call returns a non-zero error
            code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_node_lat_inflow(h, idx, value, mode,
                                             1 if persist else 0))

    def node_head_boundary(self, int idx, double value, int mode=0,
                           bint persist=False):
        """Force a head boundary condition at a node.

        @param idx: Node index.
        @type idx: int
        @param value: Boundary head (project length units).
        @type value: float
        @param mode: Forcing mode -- see L{ForcingMode}.
        @type mode: int
        @param persist: If C{True}, hold the value every step until cleared.
        @type persist: bool
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying
            C{swmm_forcing_node_head_boundary} call returns a non-zero
            error code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_node_head_boundary(h, idx, value, mode,
                                                1 if persist else 0))

    def node_quality(self, int node_idx, int pollutant_idx,
                     double mass_rate, int mode=0, bint persist=False):
        """Force a pollutant mass-rate injection at a node.

        @param node_idx: Node index.
        @type node_idx: int
        @param pollutant_idx: Pollutant index.
        @type pollutant_idx: int
        @param mass_rate: Mass rate (mass / time, in project units).
        @type mass_rate: float
        @param mode: Forcing mode -- see L{ForcingMode}.
        @type mode: int
        @param persist: If C{True}, hold the value every step until cleared.
        @type persist: bool
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_forcing_node_quality}
            call returns a non-zero error code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_node_quality(h, node_idx, pollutant_idx,
                                          mass_rate, mode,
                                          1 if persist else 0))

    # ====================================================================
    # Link forcing
    # ====================================================================

    def link_flow(self, int idx, double value, int mode=0,
                  bint persist=False):
        """Force a flow rate in a link.

        @param idx: Link index.
        @type idx: int
        @param value: Flow rate (project flow units).
        @type value: float
        @param mode: Forcing mode -- see L{ForcingMode}.
        @type mode: int
        @param persist: If C{True}, hold the value every step until cleared.
        @type persist: bool
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_forcing_link_flow}
            call returns a non-zero error code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_link_flow(h, idx, value, mode,
                                       1 if persist else 0))

    def link_setting(self, int idx, double value, int mode=0,
                     bint persist=False):
        """Force a control setting on a link (pump speed, gate opening, ...).

        @param idx: Link index.
        @type idx: int
        @param value: Setting value (C{0.0}--C{1.0} for gates; speed
            multiplier for pumps).
        @type value: float
        @param mode: Forcing mode -- see L{ForcingMode}.
        @type mode: int
        @param persist: If C{True}, hold the value every step until cleared.
        @type persist: bool
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_forcing_link_setting}
            call returns a non-zero error code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_link_setting(h, idx, value, mode,
                                          1 if persist else 0))

    # ====================================================================
    # Subcatchment forcing
    # ====================================================================

    def subcatch_rainfall(self, int idx, double value, int mode=0,
                          bint persist=False):
        """Force a rainfall rate on a subcatchment.

        @param idx: Subcatchment index.
        @type idx: int
        @param value: Rainfall rate (project rainfall units).
        @type value: float
        @param mode: Forcing mode -- see L{ForcingMode}.
        @type mode: int
        @param persist: If C{True}, hold the value every step until cleared.
        @type persist: bool
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying
            C{swmm_forcing_subcatch_rainfall} call returns a non-zero error
            code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_subcatch_rainfall(h, idx, value, mode,
                                               1 if persist else 0))

    def subcatch_evap(self, int idx, double value, int mode=0,
                      bint persist=False):
        """Force an evaporation rate on a subcatchment.

        @param idx: Subcatchment index.
        @type idx: int
        @param value: Evaporation rate (project length / time units).
        @type value: float
        @param mode: Forcing mode -- see L{ForcingMode}.
        @type mode: int
        @param persist: If C{True}, hold the value every step until cleared.
        @type persist: bool
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_forcing_subcatch_evap}
            call returns a non-zero error code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_subcatch_evap(h, idx, value, mode,
                                           1 if persist else 0))

    # ====================================================================
    # Gage forcing
    # ====================================================================

    def gage_rainfall(self, int idx, double value, int mode=0,
                      bint persist=False):
        """Force a rainfall rate at a rain gage.

        This affects all subcatchments assigned to the gage.

        @param idx: Gage index.
        @type idx: int
        @param value: Rainfall rate (project rainfall units).
        @type value: float
        @param mode: Forcing mode -- see L{ForcingMode}.
        @type mode: int
        @param persist: If C{True}, hold the value every step until cleared.
        @type persist: bool
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_forcing_gage_rainfall}
            call returns a non-zero error code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_gage_rainfall(h, idx, value, mode,
                                           1 if persist else 0))

    # ====================================================================
    # Clear/reset
    # ====================================================================

    def clear(self, int target_type, int idx):
        """Remove all active forcing on a specific object.

        @param target_type: Target type code -- see L{ForcingTarget}:
            C{NODE} (0), C{LINK} (1), C{SUBCATCH} (2), C{GAGE} (3).
        @type target_type: int
        @param idx: Object index within its type.
        @type idx: int
        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_forcing_clear} call
            returns a non-zero error code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_clear(h, target_type, idx))

    def clear_all(self):
        """Remove B{all} active forcing across all object types.

        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_forcing_clear_all}
            call returns a non-zero error code.
        """
        cdef SWMM_Engine h = <SWMM_Engine><size_t>self._solver.handle
        _check(swmm_forcing_clear_all(h))
