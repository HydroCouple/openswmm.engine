"""
Link Access
===========

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: MIT

Type stubs for :mod:`openswmm.engine._links`.

The :class:`Links` class provides access to link (conduit/pump/orifice/weir/outlet)
properties during a simulation.
"""

from typing import Tuple, Union

import numpy as np
import numpy.typing as npt

from ._solver import Solver


class Links:
    """Access link properties during a simulation.

    All per-element methods accept either an integer index or a string link
    ID.  When a string is passed it is resolved via L{get_index}.

    @param solver: An active L{Solver} instance. The solver must remain
        alive for the lifetime of this object.
    @type solver: L{Solver}

    Example::

        from openswmm.engine import Solver, Links

        with Solver("model.inp", "model.rpt", "model.out") as s:
            links = Links(s)
            flow = links.get_flow(0)      # by index
            flow = links.get_flow("C1")   # by name
    """

    def __init__(self, solver: Solver) -> None: ...

    def _resolve(self, idx: Union[int, str]) -> int:
        """Resolve C{idx} to an integer index.

        @param idx: Integer index or string link ID.
        @type idx: Union[int, str]
        @return: Integer index.
        @rtype: int
        @raise KeyError: If a string ID is not found.
        """
        ...

    # ====================================================================
    # Identification & lookup
    # ====================================================================

    def count(self) -> int:
        """Return the number of links in the model.

        @return: Link count.
        @rtype: int
        """
        ...

    def get_index(self, link_id: str) -> int:
        """Return the integer index of a link by its string ID.

        @param link_id: Link identifier string.
        @type link_id: str
        @return: Link index, or C{-1} if not found.
        @rtype: int
        """
        ...

    def get_id(self, idx: int) -> str:
        """Return the string ID of a link by index.

        @param idx: Link index.
        @type idx: int
        @return: Link ID string.
        @rtype: str
        """
        ...

    # ====================================================================
    # Construction (add/pop_last)
    # ====================================================================

    def add(self, link_id: str, link_type: int) -> int:
        """Add a link to the model (OPENED-state editing).

        Wraps C{swmm_link_add}. Valid in C{BUILDING} or C{OPENED} state.

        @param link_id: Unique link identifier.
        @type link_id: str
        @param link_type: Link type code (see L{LinkType}).
        @type link_type: int
        @return: Error code (C{0} on success).
        @rtype: int
        """
        ...

    def pop_last(self, link_id: str) -> int:
        """Remove the most recently added link (undo-of-add).

        Wraps C{swmm_link_pop_last}. Valid in C{BUILDING} or C{OPENED}
        state. C{link_id} must match the current tail; otherwise
        C{SWMM_ERR_BADINDEX} is returned.

        @param link_id: Expected tail link identifier.
        @type link_id: str
        @return: Error code (C{0} on success).
        @rtype: int
        """
        ...

    # ====================================================================
    # Connectivity
    # ====================================================================

    def get_from_node(self, idx: Union[int, str]) -> int:
        """Return the upstream (from) node index of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Node index of the upstream node.
        @rtype: int
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_to_node(self, idx: Union[int, str]) -> int:
        """Return the downstream (to) node index of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Node index of the downstream node.
        @rtype: int
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    # ====================================================================
    # Geometry/cross-section
    # ====================================================================

    def get_type(self, idx: Union[int, str]) -> int:
        """Return the type code of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Link type code (e.g. conduit, pump, orifice, weir, outlet).
        @rtype: int
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_length(self, idx: Union[int, str]) -> float:
        """Return the length of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Length (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_roughness(self, idx: Union[int, str]) -> float:
        """Return the roughness coefficient of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Manning's roughness coefficient.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_slope(self, idx: Union[int, str]) -> float:
        """Return the slope of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Slope (dimensionless).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_offset_up(self, idx: Union[int, str]) -> float:
        """Return the upstream invert offset of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Upstream offset (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_offset_dn(self, idx: Union[int, str]) -> float:
        """Return the downstream invert offset of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Downstream offset (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_xsect(self, idx: Union[int, str]) -> Tuple[int, float, float, float, float]:
        """Return the cross-section geometry of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Tuple of C{(shape, geom1, geom2, geom3, geom4)}.
        @rtype: Tuple[int, float, float, float, float]
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def set_offset_up(self, idx: Union[int, str], offset: float) -> None:
        """Set the upstream invert offset of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param offset: Upstream offset (project length units).
        @type offset: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def set_offset_dn(self, idx: Union[int, str], offset: float) -> None:
        """Set the downstream invert offset of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param offset: Downstream offset (project length units).
        @type offset: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def set_initial_flow(self, idx: Union[int, str], flow: float) -> None:
        """Set the initial flow in a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param flow: Initial flow rate (project flow units).
        @type flow: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def set_max_flow(self, idx: Union[int, str], flow: float) -> None:
        """Set the maximum flow capacity of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param flow: Maximum flow rate (project flow units).
        @type flow: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    # ====================================================================
    # Per-element flow/depth state
    # ====================================================================

    def get_flow(self, idx: Union[int, str]) -> float:
        """Return the current flow rate in a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Flow rate (project flow units). Positive = node1 -> node2.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def set_flow(self, idx: Union[int, str], value: float) -> None:
        """Set the flow rate in a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param value: New flow rate (project flow units).
        @type value: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_depth(self, idx: Union[int, str]) -> float:
        """Return the current water depth at the midpoint of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Water depth (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_velocity(self, idx: Union[int, str]) -> float:
        """Return the current flow velocity in a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Velocity (project length / time units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_capacity(self, idx: Union[int, str]) -> float:
        """Return the fraction of full capacity used in a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Capacity fraction (C{0.0}-C{1.0}).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_volume(self, idx: Union[int, str]) -> float:
        """Return the current volume stored in a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Volume (project volume units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    # ====================================================================
    # Setters & control settings
    # ====================================================================

    def set_control_setting(self, idx: Union[int, str], setting: float) -> None:
        """Override the control/pump setting on a link (RUNNING state).

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param setting: New setting (C{0.0}=closed to C{1.0}=fully open for
            orifices/weirs; C{0.0}=off to C{1.0}=full speed for pumps).
        @type setting: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_control_setting(self, idx: Union[int, str]) -> float:
        """Return the current control setting of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Setting value (C{0.0}-C{1.0}).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def set_target_setting(self, idx: Union[int, str], setting: float) -> None:
        """Set the target control setting for a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param setting: Target setting value.
        @type setting: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_target_setting(self, idx: Union[int, str]) -> float:
        """Return the target control setting of a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Target setting value.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def set_closed(self, idx: Union[int, str], closed: bool) -> None:
        """Set whether a link is forced closed.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param closed: C{True} to force the link closed.
        @type closed: bool
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_closed(self, idx: Union[int, str]) -> bool:
        """Return whether a link is forced closed.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: C{True} if the link is forced closed.
        @rtype: bool
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    # ====================================================================
    # Pump
    # ====================================================================

    def set_pump_curve(self, idx: Union[int, str], curve_idx: int) -> None:
        """Set the pump curve index for a pump link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param curve_idx: Index of the pump curve.
        @type curve_idx: int
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_pump_curve(self, idx: Union[int, str]) -> int:
        """Return the pump curve index for a pump link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Pump curve index.
        @rtype: int
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def set_pump_init_state(self, idx: Union[int, str], on: bool) -> None:
        """Set the initial on/off state of a pump link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param on: C{True} to set the pump initially on.
        @type on: bool
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_pump_init_state(self, idx: Union[int, str]) -> bool:
        """Return the initial on/off state of a pump link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: C{True} if the pump is initially on.
        @rtype: bool
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    # ====================================================================
    # Weir
    # ====================================================================

    def set_crest_height(self, idx: Union[int, str], h: float) -> None:
        """Set the crest height of a weir link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param h: Crest height (project length units).
        @type h: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_crest_height(self, idx: Union[int, str]) -> float:
        """Return the crest height of a weir link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Crest height (project length units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def set_discharge_coeff(self, idx: Union[int, str], cd: float) -> None:
        """Set the discharge coefficient of a weir link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param cd: Discharge coefficient.
        @type cd: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_discharge_coeff(self, idx: Union[int, str]) -> float:
        """Return the discharge coefficient of a weir link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Discharge coefficient.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def set_end_contractions(self, idx: Union[int, str], n: float) -> None:
        """Set the number of end contractions for a weir link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param n: Number of end contractions.
        @type n: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_end_contractions(self, idx: Union[int, str]) -> float:
        """Return the number of end contractions for a weir link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Number of end contractions.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    # ====================================================================
    # Loss / misc
    # ====================================================================

    def set_loss_coeff(
        self, idx: Union[int, str], inlet: float, outlet: float, avg: float
    ) -> None:
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
        ...

    def get_loss_coeff(self, idx: Union[int, str]) -> Tuple[float, float, float]:
        """Return the loss coefficients for a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Tuple of C{(inlet, outlet, avg)} loss coefficients.
        @rtype: Tuple[float, float, float]
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def set_flap_gate(self, idx: Union[int, str], has_gate: bool) -> None:
        """Set whether a link has a flap gate.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param has_gate: C{True} if the link has a flap gate.
        @type has_gate: bool
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_flap_gate(self, idx: Union[int, str]) -> bool:
        """Return whether a link has a flap gate.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: C{True} if the link has a flap gate.
        @rtype: bool
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def set_seep_rate(self, idx: Union[int, str], rate: float) -> None:
        """Set the seepage rate for a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param rate: Seepage rate (project flow units per unit length).
        @type rate: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_seep_rate(self, idx: Union[int, str]) -> float:
        """Return the seepage rate for a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Seepage rate.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def set_culvert_code(self, idx: Union[int, str], code: int) -> None:
        """Set the culvert inlet geometry code for a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param code: Culvert code number.
        @type code: int
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_culvert_code(self, idx: Union[int, str]) -> int:
        """Return the culvert inlet geometry code for a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Culvert code number.
        @rtype: int
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def set_barrels(self, idx: Union[int, str], n: int) -> None:
        """Set the number of barrels for a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param n: Number of barrels.
        @type n: int
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_barrels(self, idx: Union[int, str]) -> int:
        """Return the number of barrels for a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Number of barrels.
        @rtype: int
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    # ====================================================================
    # Pollutant/quality
    # ====================================================================

    def get_quality(self, idx: Union[int, str], pollutant_idx: int) -> float:
        """Return the concentration of a pollutant in a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @param pollutant_idx: Pollutant index.
        @type pollutant_idx: int
        @return: Pollutant concentration.
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    # ====================================================================
    # Statistics
    # ====================================================================

    def get_stat_max_flow(self, idx: Union[int, str]) -> float:
        """Return the maximum flow recorded for a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Maximum flow (project flow units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_stat_max_velocity(self, idx: Union[int, str]) -> float:
        """Return the maximum velocity recorded for a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Maximum velocity (project length / time units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_stat_max_filling(self, idx: Union[int, str]) -> float:
        """Return the maximum filling fraction recorded for a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Maximum filling fraction (C{0.0}-C{1.0}).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_stat_vol_flow(self, idx: Union[int, str]) -> float:
        """Return the total volume of flow through a link.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Total volume of flow (project volume units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_stat_surcharge_time(self, idx: Union[int, str]) -> float:
        """Return the total time a link was surcharged.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Surcharge duration (simulation time units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_stat_pump_cycles(self, idx: Union[int, str]) -> int:
        """Return pump on/off cycle count.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Number of pump on->off and off->on transitions.
        @rtype: int
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_stat_pump_on_time(self, idx: Union[int, str]) -> float:
        """Return total pump on-time (seconds).

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Total time pump was running (seconds).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_stat_pump_volume(self, idx: Union[int, str]) -> float:
        """Return total volume pumped.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Total volume pumped (project volume units).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    def get_hyd_power(self, idx: Union[int, str]) -> float:
        """Return hydraulic power dissipated in a link (ft-lb/s).

        C{P = gamma * |Q| * |hL|} where C{gamma} = specific weight of water.
        Divide by C{550} to convert to horsepower.

        @param idx: Link index (int) or link ID (str).
        @type idx: Union[int, str]
        @return: Hydraulic power (ft-lb/s).
        @rtype: float
        @raise KeyError: If C{idx} is a string and the link ID is not found.
        """
        ...

    # ====================================================================
    # Bulk array access (numpy)
    # ====================================================================

    def get_flows_bulk(self) -> npt.NDArray[np.float64]:
        """Return all link flows as a NumPy array.

        @return: Array of shape C{(n_links,)} with dtype C{float64}.
        @rtype: numpy.typing.NDArray[numpy.float64]
        """
        ...

    def set_flows_bulk(self, values: npt.NDArray[np.float64]) -> None:
        """Set all link flows from a NumPy array.

        @param values: Array of shape C{(n_links,)} with dtype C{float64}.
        @type values: numpy.typing.NDArray[numpy.float64]
        """
        ...

    def get_depths_bulk(self) -> npt.NDArray[np.float64]:
        """Return all link depths as a NumPy array.

        @return: Array of shape C{(n_links,)} with dtype C{float64}.
        @rtype: numpy.typing.NDArray[numpy.float64]
        """
        ...

    def get_quality_bulk(self, pollutant_idx: int) -> npt.NDArray[np.float64]:
        """Return all link pollutant concentrations as a NumPy array.

        @param pollutant_idx: Pollutant index.
        @type pollutant_idx: int
        @return: Array of shape C{(n_links,)} with dtype C{float64}.
        @rtype: numpy.typing.NDArray[numpy.float64]
        """
        ...
