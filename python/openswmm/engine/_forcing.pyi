"""
Runtime Forcing
===============

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Type stubs for :mod:`openswmm.engine._forcing`.

The :class:`Forcing` class provides advanced runtime forcing of node, link,
subcatchment, and gage values with support for mode (replace or add) and
persistence (single-step vs. held until cleared).
"""

from ._solver import Solver


class Forcing:
    """Advanced runtime forcing for nodes, links, subcatchments, and gages.

    All per-element methods accept an integer index. Use the corresponding
    domain accessor (e.g. C{solver.nodes.get_index("J1")}) to resolve
    string IDs to indices.

    @ivar _solver: The owning solver instance whose engine handle is used
        for every C call.
    @type _solver: L{Solver}

    Example::

        from openswmm.engine import Solver, Forcing, ForcingMode

        with Solver("model.inp", "model.rpt", "model.out") as s:
            forcing = Forcing(s)
            j1 = s.nodes.get_index("J1")
            forcing.node_lat_inflow(j1, 1.5, ForcingMode.REPLACE, persist=True)
            while s.state == EngineState.RUNNING:
                if s.step() != 0:
                    break
                pass
            forcing.clear_all()

    @param solver: An active L{Solver} instance. The solver must remain
        alive for the lifetime of this object.
    @type solver: L{Solver}
    """

    def __init__(self, solver: Solver) -> None:
        """Construct a L{Forcing} accessor bound to C{solver}.

        @param solver: An active L{Solver} instance whose engine handle
            will be used for all subsequent forcing operations.
        @type solver: L{Solver}
        """
        ...

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

    def node_lat_inflow(
        self,
        idx: int,
        value: float,
        mode: int = 0,
        persist: bool = False,
    ) -> None:
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
        ...

    def node_head_boundary(
        self,
        idx: int,
        value: float,
        mode: int = 0,
        persist: bool = False,
    ) -> None:
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
        ...

    def node_quality(
        self,
        node_idx: int,
        pollutant_idx: int,
        mass_rate: float,
        mode: int = 0,
        persist: bool = False,
    ) -> None:
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
        ...

    # ====================================================================
    # Link forcing
    # ====================================================================

    def link_flow(
        self,
        idx: int,
        value: float,
        mode: int = 0,
        persist: bool = False,
    ) -> None:
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
        ...

    def link_setting(
        self,
        idx: int,
        value: float,
        mode: int = 0,
        persist: bool = False,
    ) -> None:
        """Force a control setting on a link (pump speed, gate opening, ...).

        @param idx: Link index.
        @type idx: int
        @param value: Setting value (C{0.0}-C{1.0} for gates; speed
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
        ...

    # ====================================================================
    # Subcatchment forcing
    # ====================================================================

    def subcatch_rainfall(
        self,
        idx: int,
        value: float,
        mode: int = 0,
        persist: bool = False,
    ) -> None:
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
        ...

    def subcatch_evap(
        self,
        idx: int,
        value: float,
        mode: int = 0,
        persist: bool = False,
    ) -> None:
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
        ...

    # ====================================================================
    # Gage forcing
    # ====================================================================

    def gage_rainfall(
        self,
        idx: int,
        value: float,
        mode: int = 0,
        persist: bool = False,
    ) -> None:
        """Force a rainfall rate at a rain gage.

        Affects all subcatchments assigned to the gage.

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
        ...

    # ====================================================================
    # Clear/reset
    # ====================================================================

    def clear(self, target_type: int, idx: int) -> None:
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
        ...

    def clear_all(self) -> None:
        """Remove all active forcing across all object types.

        @return: C{None}.
        @rtype: None
        @raise EngineError: If the underlying C{swmm_forcing_clear_all}
            call returns a non-zero error code.
        """
        ...
